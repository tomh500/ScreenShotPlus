#include "app.h"
#include "capture.h"
#include "storage.h"
#include "config.h"
#include "dragdrop.h"
#include "preview.h"
#include "settings.h"
#include "ui.h"
#include "hotkeys.h"
#include "startup.h"
#include "editor.h"
#include <nlohmann/json.hpp>
#include <atomic>
#include <iostream>
#include <set>
#include <thread>
#include <future>
#include <aclapi.h>
using namespace std;
using namespace Gdiplus;
namespace {
void Check(bool condition, const char* message) { if (!condition) throw runtime_error(message); }
void Pump(unsigned milliseconds) {
    const auto end = GetTickCount64() + milliseconds;
    while (GetTickCount64() < end) {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
        MsgWaitForMultipleObjectsEx(0, nullptr, 20, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }
}
void Click(HWND window, int x, int y) {
    const float scale = ui::Scale(window);
    SendMessageTimeoutW(window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(static_cast<int>(x * scale), static_cast<int>(y * scale)),
        SMTO_ABORTIFHUNG, 1000, nullptr);
    SendMessageTimeoutW(window, WM_LBUTTONUP, 0, MAKELPARAM(static_cast<int>(x * scale), static_cast<int>(y * scale)),
        SMTO_ABORTIFHUNG, 1000, nullptr);
}
void Snapshot(HWND hwnd, const filesystem::path& folder) {
    UpdateWindow(hwnd);
    RECT rect{}; GetClientRect(hwnd, &rect);
    HDC dc = GetDC(hwnd), memory = CreateCompatibleDC(dc);
    HBITMAP bitmap = CreateCompatibleBitmap(dc, rect.right, rect.bottom);
    HGDIOBJ old = SelectObject(memory, bitmap);
    const bool copied = BitBlt(memory, 0, 0, rect.right, rect.bottom, dc, 0, 0, SRCCOPY) != FALSE;
    SelectObject(memory, old); DeleteDC(memory); ReleaseDC(hwnd, dc);
    bool saved = false;
    if (copied) {
        unique_ptr<Bitmap> image(Bitmap::FromHBITMAP(bitmap, nullptr));
        filesystem::path file; wstring error;
        saved = image && SavePng(*image, folder, file, error);
    }
    DeleteObject(bitmap);
    Check(saved, "UI snapshot failed");
}
void Geometry() {
    RECT r = SelectionRect({90, 75}, {-20, 10}, 100, 80);
    Check(r.left == 0 && r.top == 10 && r.right == 90 && r.bottom == 75, "reverse / clipped selection");
    r = SelectionRect({0, 0}, {300, 200}, 100, 80);
    Check(r.right == 100 && r.bottom == 80, "selection bounds");
    r = SelectionRect({20, 20}, {20, 20}, 100, 80);
    Check(r.left == r.right && r.top == r.bottom, "empty selection");
    // Physical points on a virtual desktop whose origin is (-1920, -1080).
    r = SelectionRect({1700, 900}, {2300, 1300}, 3840, 2160);
    Check(r.left - 1920 == -220 && r.right - 1920 == 380 && r.bottom - r.top == 400, "cross-origin geometry");
}
void Configuration(const filesystem::path& root) {
    const auto file = root / L"设置" / L"config.json";
    Config c = DefaultConfig(); c.folder = root / L"截图 中文 空格";
    c.modifiers = MOD_CONTROL | MOD_ALT; c.key = VK_F9; c.printScreen = true; c.previewSeconds = 15;
    wstring error;
    Check(SaveConfig(file, c, error), "save config");
    wstring warning;
    auto loaded = LoadConfig(file, warning);
    Check(warning.empty() && loaded.folder == c.folder && loaded.key == VK_F9 && loaded.previewSeconds == 15 && loaded.printScreen, "Unicode config round trip");
    c.previewSeconds = 0; Check(SaveConfig(file, c, error), "replace existing config atomically");
    Check(LoadConfig(file, warning).previewSeconds == 0, "updated config persisted");
    nlohmann::json data;
    { ifstream in(file); in >> data; }
    for (const auto& bad : {nlohmann::json(-1), nlohmann::json(4000), nlohmann::json(2.5), nlohmann::json("bad")}) {
        auto invalid = data; invalid["previewSeconds"] = bad;
        { ofstream out(file); out << invalid; }
        warning.clear(); loaded = LoadConfig(file, warning);
        Check(!warning.empty() && loaded.modifiers == (MOD_CONTROL | MOD_SHIFT) && loaded.key == 'S', "invalid config falls back");
    }
    { ofstream out(file); out << "{broken"; }
    warning.clear(); LoadConfig(file, warning); Check(!warning.empty(), "broken JSON warning");
    { ofstream out(root / L"blocked"); out << "original"; }
    Check(!SaveConfig(root / L"blocked" / L"config.json", c, error), "config write failure");
    c.modifiers = 0; Check(!ValidConfig(c), "bare key rejected");
}
namespace {
wstring ReadRunValue() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, startup::kRunKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) return {};
    DWORD type = 0, bytes = 0;
    wstring value;
    if (RegQueryValueExW(key, startup::kValueName, nullptr, &type, nullptr, &bytes) == ERROR_SUCCESS &&
        type == REG_SZ && bytes) {
        vector<wchar_t> buffer(bytes / sizeof(wchar_t) + 1, L'\0');
        if (RegQueryValueExW(key, startup::kValueName, nullptr, &type, reinterpret_cast<BYTE*>(buffer.data()), &bytes) == ERROR_SUCCESS)
            value.assign(buffer.data());
    }
    RegCloseKey(key);
    return value;
}
void WriteRunValue(const wstring& value) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, startup::kRunKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        throw runtime_error("cannot open run key");
    if (value.empty()) RegDeleteValueW(key, startup::kValueName);
    else RegSetValueExW(key, startup::kValueName, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
        static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
}
// 探测当前进程能否写入 HKCU：受限沙箱环境会整体拒绝注册表写入，此时跳过启动项实测。
bool RegistryWritable() {
    constexpr wchar_t probe[] = L"Software\\ScreenShotPlus.RegistryProbe";
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, probe, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return false;
    RegCloseKey(key);
    RegDeleteKeyW(HKEY_CURRENT_USER, probe);
    return true;
}
// 测试会短暂改写当前账户的启动项，结束后按进入时的原值恢复。
void StartupRegistration(const filesystem::path& root) {
    wstring error;
    // 配置层：runAtStartup 必须能独立完成 JSON 往返，与注册表可用性无关。
    const auto file = root / L"启动项" / L"config.json";
    Config config = DefaultConfig();
    config.folder = root / L"启动项";
    config.runAtStartup = true;
    Check(SaveConfig(file, config, error) && LoadConfig(file, error).runAtStartup, "startup flag persisted in config");
    config.runAtStartup = false;
    Check(SaveConfig(file, config, error) && !LoadConfig(file, error).runAtStartup, "disabled startup flag persisted");
    config.runAtStartup = true;
    // 旧配置缺少该字段时必须回退为关闭，不能因为少一个键就判定配置损坏。
    {
        Config legacy = DefaultConfig();
        legacy.folder = root / L"启动项";
        legacy.runAtStartup = true;
        wstring note;
        Check(SaveConfig(file, legacy, note), "write config before legacy downgrade");
        nlohmann::json data;
        { ifstream in(file); in >> data; }
        data.erase("runAtStartup");
        { ofstream out(file); out << data.dump(2); }
        wstring warning;
        const auto loaded = LoadConfig(file, warning);
        Check(warning.empty() && !loaded.runAtStartup, "legacy config without startup flag still loads");
    }
    if (!RegistryWritable()) {
        cout << "SKIP startup registry / settings transaction: HKCU writes denied in this sandbox\n";
        return;
    }

    const wstring original = ReadRunValue();
    try {
        Check(startup::Apply(false, error), "disable startup");
        Check(!startup::Enabled(), "startup disabled removes the run value");
        Check(startup::Apply(true, error), "enable startup");
        Check(startup::Enabled(), "startup enabled writes the run value");
        const auto executable = startup::Executable();
        Check(!executable.empty() && executable.is_absolute(), "executable path resolved");
        Check(startup::Matches(executable), "run value points at this executable");
        // Run 项必须包含程序路径与 --startup，登录时才能静默驻留托盘。
        const wstring value = ReadRunValue();
        Check(value.find(executable.wstring()) != wstring::npos, "run value contains the executable path");
        Check(value.find(startup::kStartupArgument) != wstring::npos, "run value requests tray-only startup");
        Check(value.size() > executable.wstring().size(), "run value is a quoted command line");
        // 指向其它路径的启动项必须被识别为不一致，以便设置面板提示并修正。
        WriteRunValue(L"\"C:\\other\\ScreenShotPlus.exe\" --startup");
        Check(startup::Enabled() && !startup::Matches(executable), "stale run value detected as mismatched");

        // 通过真实设置事务验证联动、回滚与配置持久化。
        HWND window = CreateWindowExW(0, L"STATIC", L"startup host", WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, nullptr, nullptr);
        Check(window != nullptr, "startup test host window");
        Hotkeys state;
        wstring warning = RegisterConfiguredHotkeys(window, state, config);
        Check(warning.empty(), "startup test hotkey registration");
        Config next = config; next.runAtStartup = true;
        Check(ApplyConfiguredSettings(window, state, config, next, file, error), "settings enable startup");
        Check(startup::Matches(startup::Executable()), "settings wrote the run value");
        Check(config.runAtStartup && LoadConfig(file, warning).runAtStartup, "startup flag persisted by settings");
        Config off = config; off.runAtStartup = false;
        Check(ApplyConfiguredSettings(window, state, config, off, file, error), "settings disable startup");
        Check(!startup::Enabled(), "settings removed the run value");
        Check(!LoadConfig(file, warning).runAtStartup, "disabled startup flag persisted by settings");

        // 热键冲突必须回滚刚写入的启动项，不能留下半套设置。
        Config blocked = config; blocked.runAtStartup = true;
        blocked.modifiers = MOD_CONTROL | MOD_ALT; blocked.key = VK_F8;
        Check(RegisterHotKey(window, 9, blocked.modifiers | MOD_NOREPEAT, blocked.key), "occupy hotkey for rollback test");
        const bool applied = ApplyConfiguredSettings(window, state, config, blocked, file, error);
        Check(!applied && !error.empty(), "hotkey conflict rejects settings");
        Check(!startup::Enabled(), "hotkey conflict rolled back the run value");
        UnregisterHotKey(window, 9);
        ReleaseHotkeys(window, state);
        DestroyWindow(window);
    } catch (...) { WriteRunValue(original); throw; }
    WriteRunValue(original);
}
}
namespace {
Bitmap* MakeBitmap(int width, int height, Color color) {
    Bitmap* image = new Bitmap(width, height, PixelFormat32bppRGB);
    Graphics g(image);
    g.Clear(color);
    return image;
}
// 在半径为 radius 的邻域里找一个与期望颜色相符的像素，用于容忍抗锯齿边缘。
bool AnyPixelNear(Bitmap& image, int x, int y, int radius, Color expected) {
    for (int dy = -radius; dy <= radius; ++dy)
        for (int dx = -radius; dx <= radius; ++dx) {
            const int px = x + dx, py = y + dy;
            if (px < 0 || py < 0 || px >= static_cast<int>(image.GetWidth()) || py >= static_cast<int>(image.GetHeight()))
                continue;
            Color actual;
            if (image.GetPixel(px, py, &actual) == Ok && actual.GetR() == expected.GetR() &&
                actual.GetG() == expected.GetG() && actual.GetB() == expected.GetB()) return true;
        }
    return false;
}
bool SamePixel(Bitmap& image, int x, int y, Color expected) {
    Color actual;
    return image.GetPixel(x, y, &actual) == Ok && actual.GetR() == expected.GetR() &&
        actual.GetG() == expected.GetG() && actual.GetB() == expected.GetB();
}
// 标注必须只影响被绘制的像素，且落点与画笔颜色在导出后的 PNG 中保持一致。
void EditorAnnotation(const filesystem::path& root) {
    const Color background(255, 240, 240, 240);
    unique_ptr<Bitmap> base(MakeBitmap(120, 80, background));
    Check(base && base->GetLastStatus() == Ok, "editor base image");
    // 预设颜色：赤橙黄绿青蓝紫齐全，且每个都不相同。
    set<ARGB> presets;
    for (int i = 0; i < editor::kPresetCount; ++i) presets.insert(editor::PresetColor(i).GetValue());
    Check(static_cast<int>(presets.size()) == editor::kPresetCount, "preset colors are distinct");
    Check(editor::PresetColor(0).GetR() > 200 && editor::PresetColor(0).GetG() < 100, "first preset is red");
    Check(editor::PresetColor(2).GetR() > 200 && editor::PresetColor(2).GetG() > 150 &&
        editor::PresetColor(2).GetB() < 120, "third preset is yellow");
    Check(editor::PresetColor(6).GetB() > 130 && editor::PresetColor(6).GetR() > 90, "seventh preset is purple");
    // 画笔：一段折线，颜色与线宽都由标注自身决定。
    editor::Shape stroke;
    stroke.tool = editor::Tool::Brush;
    stroke.color = editor::PresetColor(5);
    stroke.stroke = 8;
    for (int x = 10; x <= 90; x += 4) stroke.points.push_back(PointF(static_cast<REAL>(x), 40.0f));
    // 文字：字号与颜色独立于画笔。
    editor::Shape label;
    label.tool = editor::Tool::Text;
    label.color = editor::PresetColor(0);
    label.size = 24;
    label.at = PointF(10.0f, 8.0f);
    label.text = L"标注";
    Check(editor::DefaultStrokeWidth(*base) >= 3, "default stroke width scales with the image");
    Check(editor::DefaultFontSize(*base) >= 20, "default font size scales with the image");
    Check(editor::NextStrokeWidth(3) > 3 && editor::NextStrokeWidth(18) == 3, "stroke width cycles");
    Check(editor::NextFontSize(30, 1) > 30 && editor::NextFontSize(30, -1) < 30, "font size steps up and down");
    Check(editor::NextFontSize(16, -1) == 16 && editor::NextFontSize(128, 1) == 128, "font size stays in range");

    unique_ptr<Bitmap> flattened(MakeBitmap(120, 80, Color(255, 0, 0, 0)));
    Check(editor::Flatten(*base, {stroke, label}, *flattened), "flatten annotations");
    Color middle;
    Check(AnyPixelNear(*flattened, 50, 40, 6, editor::PresetColor(5)), "brush stroke keeps the selected color");
    Check(SamePixel(*flattened, 5, 70, background), "untouched pixels keep the original image");
    Check(AnyPixelNear(*flattened, 18, 18, 10, editor::PresetColor(0)), "text is drawn in the text color");
    // 擦除笔用背景色覆盖，等价于把画笔换成不绘制。
    editor::Shape erase = stroke;
    erase.color = Color(255, 240, 240, 240);
    unique_ptr<Bitmap> erased(MakeBitmap(120, 80, Color(255, 0, 0, 0)));
    Check(editor::Flatten(*base, {erase}, *erased) && AnyPixelNear(*erased, 50, 40, 6, background), "eraser restores the background");
    // 空标注列表必须原样复制。
    unique_ptr<Bitmap> plain(MakeBitmap(120, 80, Color(255, 0, 0, 0)));
    Check(editor::Flatten(*base, {}, *plain) && SamePixel(*plain, 5, 70, background) &&
        SamePixel(*plain, 50, 50, background), "empty annotation list copies the source");
    // 尺寸不一致的目标位图必须被拒绝，避免越界绘制。
    unique_ptr<Bitmap> wrong(MakeBitmap(60, 40, background));
    Check(!editor::Flatten(*base, {stroke}, *wrong), "mismatched target size rejected");

    // 写回原路径：内容替换，且不留下临时文件。
    const auto folder = root / L"标注 中文" / L"editor";
    filesystem::create_directories(folder);
    filesystem::path file;
    wstring error;
    Check(SavePng(*base, folder, file, error), "save annotation source");
    unique_ptr<Bitmap> baked(MakeBitmap(120, 80, Color(255, 0, 0, 0)));
    Check(editor::Flatten(*base, {stroke}, *baked) && ReplacePng(*baked, file, error), "write annotations back");
    {
        Bitmap reloaded(file.c_str());
        Check(reloaded.GetLastStatus() == Ok && AnyPixelNear(reloaded, 50, 40, 6, editor::PresetColor(5)),
            "annotated PNG persisted in place");
    }
    size_t temporary = 0;
    for (const auto& item : filesystem::directory_iterator(folder)) if (item.path().extension() == L".tmp") ++temporary;
    Check(temporary == 0, "no temporary file is left behind");
    // 空路径必须被拒绝，而不是写到当前目录。
    Check(!ReplacePng(*baked, {}, error) && !error.empty(), "empty annotation target rejected");
}
// 编辑窗口的交互：画笔落点必须映射回图片像素，保存后原文件被替换。
void EditorInteraction(HINSTANCE instance, const filesystem::path& root) {
    const Color background(255, 240, 240, 240);
    unique_ptr<Bitmap> base(MakeBitmap(80, 60, background));
    const auto folder = root / L"编辑交互";
    filesystem::create_directories(folder);
    filesystem::path file;
    wstring error;
    Check(SavePng(*base, folder, file, error), "editor interaction source");
    Bitmap* finished = nullptr;
    Check(editor::InitEditor(instance, nullptr, [&](Bitmap* done, const filesystem::path&) {
        finished = done;
    }), "editor initialization");
    editor::OpenEditor(*base, file);
    HWND window = FindWindowW(L"ScreenShotPlus.Editor", nullptr);
    Check(window && editor::EditorVisible(), "editor window opens");
    Bitmap* canvas = editor::Canvas();
    Check(canvas && canvas->GetWidth() == 80 && canvas->GetHeight() == 60, "editor loads the screenshot copy");
    UpdateWindow(window); Pump(120);
    // 画布在窗口中的位置由编辑器自己报告，测试据此把图片像素换算成客户区坐标。
    const RectF canvasRect = editor::CanvasRect();
    const float scale = ui::Scale(window);
    const float imageScale = canvasRect.Width / 80.0f;
    Check(canvasRect.Width > 0 && imageScale > 0, "editor reports a usable canvas rectangle");
    const auto toClient = [&](float imageX, float imageY) {
        return POINT{static_cast<LONG>((canvasRect.X + imageX * imageScale) * scale),
            static_cast<LONG>((canvasRect.Y + imageY * imageScale) * scale)};
    };
    // 在图片坐标 (20,30) 附近画一笔，落点必须精确回到同一像素。
    const POINT from = toClient(20, 30), to = toClient(60, 30);
    SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(from.x, from.y));
    SendMessageW(window, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM((from.x + to.x) / 2, from.y));
    SendMessageW(window, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(to.x, to.y));
    SendMessageW(window, WM_LBUTTONUP, 0, MAKELPARAM(to.x, to.y));
    // 工具栏命中：默认选中第一个预设颜色，点击第二个色块后画笔应换成橙色。
    const RectF swatch = editor::SwatchRect(1);
    const POINT swatchPoint{static_cast<LONG>((swatch.X + swatch.Width / 2) * scale),
        static_cast<LONG>((swatch.Y + swatch.Height / 2) * scale)};
    SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(swatchPoint.x, swatchPoint.y));
    SendMessageW(window, WM_LBUTTONUP, 0, MAKELPARAM(swatchPoint.x, swatchPoint.y));
    Check(editor::SelectedColorIndex() == 1, "preset swatch selects the brush color");
    Check(editor::ShapeCount() == 1, "brush stroke recorded once");
    // 自定义 RGB 调色板：滑条换算、预览色，以及确定后成为当前画笔颜色。
    editor::SetColorPickerValues(0, 0, 0);
    editor::OpenColorPickerForTest();
    Check(editor::ColorPickerVisible(), "RGB palette opens");
    HWND palette = FindWindowW(L"ScreenShotPlus.ColorPicker", nullptr);
    Check(palette != nullptr, "RGB palette window exists");
    const RectF track = editor::ColorPickerTrackRect(0);
    const POINT knob{static_cast<LONG>((track.X + track.Width * 0.5f) * scale),
        static_cast<LONG>((track.Y + track.Height / 2) * scale)};
    SendMessageW(palette, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(knob.x, knob.y));
    SendMessageW(palette, WM_LBUTTONUP, 0, MAKELPARAM(knob.x, knob.y));
    const Color midway = editor::ColorPickerPreview();
    Check(midway.GetR() >= 120 && midway.GetR() <= 136, "slider position maps to the channel value");
    editor::SetColorPickerValues(12, 200, 90);
    editor::ConfirmColorPicker();
    Check(!editor::ColorPickerVisible(), "RGB palette closes after confirming");
    Check(editor::SelectedColorIndex() == editor::kPresetCount, "custom RGB becomes the active brush color");
    // 用自定义色再画一笔，导出后必须出现该颜色。
    const POINT from2 = toClient(20, 45), to2 = toClient(60, 45);
    SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(from2.x, from2.y));
    SendMessageW(window, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(to2.x, to2.y));
    SendMessageW(window, WM_LBUTTONUP, 0, MAKELPARAM(to2.x, to2.y));
    const Color custom(255, 12, 200, 90);
    unique_ptr<Bitmap> withCustom(MakeBitmap(80, 60, background));
    Check(editor::Flatten(*editor::Canvas(), editor::Shapes(), *withCustom) &&
        AnyPixelNear(*withCustom, 40, 45, 5, custom), "custom RGB stroke is flattened with the chosen color");
    // 取消不应改变当前颜色。
    editor::OpenColorPickerForTest();
    editor::SetColorPickerValues(1, 2, 3);
    editor::CancelColorPicker();
    Check(editor::SelectedColorIndex() == editor::kPresetCount, "cancelling the RGB palette keeps the color");
    editor::UndoLastShape();
    Check(editor::ShapeCount() == 1, "undo removes the custom RGB annotation");
    editor::RedoLastShape();
    Check(editor::ShapeCount() == 2, "redo restores the custom RGB annotation");
    editor::UndoLastShape();
    Check(editor::ShapeCount() == 1, "undo is repeatable");
    editor::RedoLastShape();
    Check(editor::ShapeCount() == 2, "redo is repeatable");
    // 保存：写回原文件，并把成品交回调用方。
    editor::RequestSave();
    Check(!editor::EditorVisible(), "editor closes after saving");
    Check(finished != nullptr, "editor hands the flattened image back");
    {
        Bitmap reloaded(file.c_str());
        const Color expected = editor::PresetColor(0);
        Check(reloaded.GetLastStatus() == Ok && AnyPixelNear(reloaded, 40, 30, 4, expected),
            "saved file contains the annotation at the expected pixel");
    }
    unique_ptr<Bitmap> handed(finished);
    Check(handed && handed->GetWidth() == 80, "flattened image keeps the source size");
    editor::DestroyEditor();
    Check(!editor::EditorVisible(), "editor destroyed");
    // 取消路径：不写文件，并把空结果交回调用方。
    finished = nullptr;
    Check(editor::InitEditor(instance, nullptr, [&](Bitmap* done, const filesystem::path&) { finished = done; }),
        "editor reinitialization");
    editor::OpenEditor(*base, file);
    window = FindWindowW(L"ScreenShotPlus.Editor", nullptr);
    UpdateWindow(window); Pump(120);
    SendMessageW(window, WM_KEYDOWN, VK_ESCAPE, 0);
    Check(!editor::EditorVisible() && finished == nullptr, "Escape cancels without writing the file");
    editor::DestroyEditor();
}
// 预览图片的单击必须进入标注编辑，拖动则仍然是文件分享。
void EditorEntry(HINSTANCE instance, const filesystem::path& root) {
    unique_ptr<Bitmap> base(MakeBitmap(60, 40, Color(255, 30, 136, 229)));
    const auto folder = root / L"标注入口";
    filesystem::create_directories(folder);
    filesystem::path file;
    wstring error;
    Check(SavePng(*base, folder, file, error), "editor entry source");
    bool edited = false;
    Check(editor::InitEditor(instance, nullptr, [](Bitmap* image, const filesystem::path&) { delete image; }),
        "editor entry initialization");
    Check(InitPreview(instance, [] {}, [&] { edited = true; }), "preview with edit hook");
    ShowPreview(base.get(), file, POINT{400, 300}, 0);
    HWND preview = FindWindowW(L"ScreenShotPlus.Preview", nullptr);
    Check(preview != nullptr && PreviewVisible(), "preview shown for the edit hook");
    const float scale = ui::Scale(preview);
    // 单击图片区域（未越过拖拽阈值）应触发编辑回调。
    SendMessageW(preview, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(static_cast<int>(100 * scale), static_cast<int>(100 * scale)));
    SendMessageW(preview, WM_LBUTTONUP, 0, MAKELPARAM(static_cast<int>(100 * scale), static_cast<int>(100 * scale)));
    Check(edited, "single click on the preview opens the annotation editor");
    edited = false;
    // 按下并越过阈值是拖动，不应进入编辑。
    SendMessageW(preview, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(static_cast<int>(100 * scale), static_cast<int>(100 * scale)));
    SendMessageW(preview, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(static_cast<int>(160 * scale), static_cast<int>(100 * scale)));
    SendMessageW(preview, WM_LBUTTONUP, 0, MAKELPARAM(static_cast<int>(160 * scale), static_cast<int>(100 * scale)));
    Check(!edited, "drag on the preview does not open the editor");
    DestroyPreview(); editor::DestroyEditor();
}
}
filesystem::path PngStorage(const filesystem::path& root) {
    Bitmap image(17, 23, PixelFormat32bppRGB);
    { Graphics g(&image); g.Clear(Color(255, 13, 101, 203)); }
    set<filesystem::path> paths;
    filesystem::path latest;
    for (int i = 0; i < 12; ++i) {
        wstring error; Check(SavePng(image, root / L"图片 中文", latest, error), "save PNG");
        Check(paths.insert(latest).second, "unique filenames / no overwrite");
    }
    for (const auto& file : paths) {
        Bitmap decoded(file.c_str()); Color pixel;
        Check(decoded.GetLastStatus() == Ok && decoded.GetWidth() == 17 && decoded.GetHeight() == 23, "PNG dimensions");
        Check(decoded.GetPixel(3, 4, &pixel) == Ok && pixel.GetR() == 13 && pixel.GetG() == 101 && pixel.GetB() == 203 && pixel.GetA() == 255, "PNG pixels / opaque alpha");
    }
    wstring error;
    Check(!SavePng(image, root / L"blocked", latest, error) && !error.empty(), "PNG save failure");
    SYSTEMTIME time{}; time.wYear = 2026; time.wMonth = 10; time.wDay = 9; time.wMilliseconds = 7;
    Check(ScreenshotStem(time) == L"Screenshot_20261009_000000_007", "filename timestamp");
    return *paths.begin();
}
void FileTransfer(const filesystem::path& file) {
    IDataObject* data = nullptr;
    Check(SUCCEEDED(CreateFileDataObject(file, &data)), "create file transfer");
    FORMATETC format{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM medium{};
    Check(data->GetData(&format, &medium) == S_OK, "CF_HDROP payload");
    const UINT size = DragQueryFileW(static_cast<HDROP>(medium.hGlobal), 0, nullptr, 0);
    vector<wchar_t> path(size + 1);
    DragQueryFileW(static_cast<HDROP>(medium.hGlobal), 0, path.data(), static_cast<UINT>(path.size()));
    Check(DragQueryFileW(static_cast<HDROP>(medium.hGlobal), 0xffffffff, nullptr, 0) == 1 && filesystem::path(path.data()) == file, "one real Unicode file");
    STGMEDIUM second{}; Check(data->GetData(&format, &second) == S_OK && second.hGlobal != medium.hGlobal, "independent transfer allocations");
    ReleaseStgMedium(&second); ReleaseStgMedium(&medium);
    format.cfFormat = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT));
    Check(data->GetData(&format, &medium) == S_OK, "preferred copy format");
    const auto* effect = static_cast<DWORD*>(GlobalLock(medium.hGlobal));
    Check(effect && *effect == DROPEFFECT_COPY, "copy-only effect");
    GlobalUnlock(medium.hGlobal); ReleaseStgMedium(&medium);
    IEnumFORMATETC* enumerator = nullptr;
    Check(data->EnumFormatEtc(DATADIR_GET, &enumerator) == S_OK, "format enumeration");
    FORMATETC formats[2]{}; ULONG count = 0;
    Check(enumerator->Next(2, formats, &count) == S_OK && count == 2, "two transfer formats");
    enumerator->Release();
    format.tymed = TYMED_ISTREAM;
    Check(data->QueryGetData(&format) == DV_E_TYMED, "unsupported transfer medium");
    data->Release();
    DWORD copy = 123;
    Check(FAILED(DragFile(file.parent_path() / L"missing.png", copy)) && copy == DROPEFFECT_NONE, "missing source handled");
    Check(filesystem::exists(file), "source preserved");
}
void WritePermissions(const filesystem::path& root) {
    const auto folder = root / (L"permission-test-" + to_wstring(GetTickCount64()));
    Check(filesystem::create_directory(folder), "fresh permission test directory");
    Config config = DefaultConfig(); config.folder = folder;
    const auto file = folder / L"config.json"; wstring error, warning;
    Check(SaveConfig(file, config, error), "original configuration before access denial");
    struct RestoreDacl {
        wstring name;
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        PACL original = nullptr, denied = nullptr;
        DWORD flags = DACL_SECURITY_INFORMATION;
        bool changed = false;
        ~RestoreDacl() {
            if (changed) SetNamedSecurityInfoW(name.data(), SE_FILE_OBJECT, flags, nullptr, nullptr, original, nullptr);
            if (denied) LocalFree(denied);
            if (descriptor) LocalFree(descriptor);
        }
    } restore;
    restore.name = folder.wstring();
    Check(GetNamedSecurityInfoW(restore.name.data(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
        nullptr, nullptr, &restore.original, nullptr, &restore.descriptor) == ERROR_SUCCESS, "read fixture DACL");
    SECURITY_DESCRIPTOR_CONTROL control{}; DWORD revision = 0;
    Check(GetSecurityDescriptorControl(restore.descriptor, &control, &revision) != FALSE, "read DACL inheritance flags");
    restore.flags |= control & SE_DACL_PROTECTED ? PROTECTED_DACL_SECURITY_INFORMATION : UNPROTECTED_DACL_SECURITY_INFORMATION;
    HANDLE token = nullptr; Check(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) != FALSE, "query fixture user");
    DWORD size = 0; GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    vector<char> identity(size);
    const bool queried = GetTokenInformation(token, TokenUser, identity.data(), size, &size) != FALSE;
    CloseHandle(token); Check(queried, "read fixture user SID");
    EXPLICIT_ACCESSW access{};
    access.grfAccessPermissions = FILE_ADD_FILE | FILE_ADD_SUBDIRECTORY;
    access.grfAccessMode = DENY_ACCESS; access.grfInheritance = NO_INHERITANCE;
    BuildTrusteeWithSidW(&access.Trustee, reinterpret_cast<TOKEN_USER*>(identity.data())->User.Sid);
    Check(SetEntriesInAclW(1, &access, restore.original, &restore.denied) == ERROR_SUCCESS, "construct fixture deny ACL");
    // 受限沙箱会拒绝改写工作区内目录的 DACL；此时跳过实测，不把环境限制当成产品缺陷。
    if (SetNamedSecurityInfoW(restore.name.data(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
        nullptr, nullptr, restore.denied, nullptr) != ERROR_SUCCESS) {
        cout << "SKIP real ACL write denial: DACL changes to the workspace are denied in this sandbox\n";
        return;
    }
    restore.changed = true;
    Bitmap image(8, 8, PixelFormat32bppRGB); filesystem::path png;
    Check(!SavePng(image, folder, png, error), "PNG denied by real filesystem permissions");
    config.previewSeconds = 10;
    Check(!SaveConfig(file, config, error), "configuration denied by real filesystem permissions");
    Check(LoadConfig(file, warning).previewSeconds == 0, "write denial preserves previous configuration");
    Check(SetNamedSecurityInfoW(restore.name.data(), SE_FILE_OBJECT, restore.flags, nullptr, nullptr,
        restore.original, nullptr) == ERROR_SUCCESS, "restore fixture permissions");
    restore.changed = false;
    Check(SavePng(image, folder, png, error), "PNG succeeds after permission restoration");
}
LRESULT CALLBACK PatternProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps{}; HDC dc = BeginPaint(hwnd, &ps);
        { Graphics g(dc); g.Clear(Color(255, 29, 137, 211)); }
        EndPaint(hwnd, &ps); return 0;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}
void CaptureAndPreview(HINSTANCE instance, const filesystem::path& root) {
    WNDCLASSW cls{}; cls.lpfnWndProc = PatternProc; cls.hInstance = instance; cls.lpszClassName = L"ScreenShotPlus.TestPattern";
    Check(RegisterClassW(&cls) != 0, "test pattern class");
    HWND pattern = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, cls.lpszClassName, L"Screenshot test pattern", WS_POPUP,
        100, 100, 400, 300, nullptr, nullptr, instance, nullptr);
    Check(pattern != nullptr, "test pattern window");
    ShowWindow(pattern, SW_SHOWNOACTIVATE); UpdateWindow(pattern); DwmFlush();
    unique_ptr<Bitmap> result;
    POINT finished{}; wstring error; bool called = false;
    Check(BeginSelection(instance, [&](unique_ptr<Bitmap> image, POINT point, wstring failure) {
        result = move(image); finished = point; error = move(failure); called = true;
    }, error), "desktop freeze");
    HWND overlay = FindWindowW(L"ScreenShotPlus.Selection", nullptr);
    Check(overlay != nullptr, "selection overlay");
    const int x = 150 - GetSystemMetrics(SM_XVIRTUALSCREEN), y = 150 - GetSystemMetrics(SM_YVIRTUALSCREEN);
    SendMessageW(overlay, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x + 80, y + 60));
    SendMessageW(overlay, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(x, y));
    SendMessageW(overlay, WM_LBUTTONUP, 0, MAKELPARAM(x, y));
    Check(called && result && error.empty() && result->GetWidth() == 80 && result->GetHeight() == 60, "live reverse selection crop");
    Color pixel; result->GetPixel(20, 20, &pixel);
    Check(pixel.GetR() == 29 && pixel.GetG() == 137 && pixel.GetB() == 211 && pixel.GetA() == 255, "frozen desktop pixel / alpha");
    Check(finished.x == 150 && finished.y == 150, "selection end physical point");
    const DWORD resources = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    for (int i = 0; i < 3; ++i) {
        called = false;
        Check(BeginSelection(instance, [&](unique_ptr<Bitmap> image, POINT, wstring) { called = !image; }, error), "repeat freeze");
        CancelSelection(); Check(called, "cancel selection without image");
    }
    Check(GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS) <= resources + 2, "capture GDI handles released");
    DestroyWindow(pattern);
    filesystem::path file; Check(SavePng(*result, root, file, error), "save actual cropped image");
    bool dragged = false;
    Check(InitPreview(instance, [&] {
        dragged = true; Pump(1300); Check(PreviewVisible(), "timer paused during drag callback");
    }, [] {}), "preview initialization");
    const HWND foreground = GetForegroundWindow();
    ShowPreview(result.get(), file, finished, 0);
    Check(PreviewVisible() && GetForegroundWindow() == foreground, "preview does not activate");
    Snapshot(FindWindowW(L"ScreenShotPlus.Preview", nullptr), root / L"preview-ui");
    Pump(150); Check(PreviewVisible(), "persistent preview"); HidePreview();
    Check(!PreviewVisible() && filesystem::exists(file), "closing preview preserves PNG");
    ShowPreview(result.get(), file, finished, 1);
    HWND preview = FindWindowW(L"ScreenShotPlus.Preview", nullptr);
    Snapshot(preview, root / L"preview-ui");
    // 计时逻辑依据真实光标位置判定悬停，因此必须固定光标，否则结果取决于鼠标当前停在哪里。
    // 窗口尺寸也必须在这一步之后读取，因为 ShowPreview 会先按显示器工作区摆放窗口。
    const float scale = ui::Scale(preview);
    RECT clip{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    const bool clipped = ClipCursor(&clip) != FALSE;
    POINT pinned{clip.right / 2, clip.bottom / 2};
    if (clipped) SetCursorPos(pinned.x, pinned.y);
    if (clipped) GetCursorPos(&pinned);
    // 位置一律显式指定，并读回实际矩形，避免依赖窗口创建位置或系统对越界坐标的修正。
    const auto place = [&](int x, int y) {
        SetWindowPos(preview, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOZORDER);
        RECT applied{}; GetWindowRect(preview, &applied);
        return applied;
    };
    const auto coversCursor = [&](const RECT& r) {
        return pinned.x >= r.left && pinned.x < r.right && pinned.y >= r.top && pinned.y < r.bottom;
    };
    // 每个场景都显式重新显示窗口，不能沿用上一场景残留的可见状态。
    ShowPreview(result.get(), file, finished, 1);
    Check(IsWindowVisible(preview), "preview shown for the countdown scenarios");
    // 情况一：窗口远离光标，1 秒计时必须让它收起。
    const RECT farRect = place(clip.left, clip.top);
    Check(!coversCursor(farRect), "preview moved away from the pinned cursor");
    Pump(1300);
    Check(!PreviewVisible(), "preview timer expires");
    // 情况二：窗口覆盖光标，倒计时暂停，悬停期间不得收起。
    ShowPreview(result.get(), file, finished, 1);
    Check(IsWindowVisible(preview), "preview shown again for the hover case");
    const int width = farRect.right - farRect.left, height = farRect.bottom - farRect.top;
    const RECT hoverRect = place(clamp(pinned.x - width / 2, clip.left, max(clip.left, clip.right - width)),
        clamp(pinned.y - height / 2, clip.top, max(clip.top, clip.bottom - height)));
    Check(coversCursor(hoverRect), "preview covers the pinned cursor");
    Pump(1300);
    Check(PreviewVisible(), "timer paused while hovered");
    // 情况三：窗口移开光标，按住图片并越过拖拽阈值；拖拽回调期间计时同样必须暂停。
    const RECT movedRect = place(clip.left, clip.top);
    Check(!coversCursor(movedRect), "preview moved away again for the drag case");
    SendMessageW(preview, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(static_cast<int>(100 * scale), static_cast<int>(100 * scale)));
    SendMessageW(preview, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(static_cast<int>(140 * scale), static_cast<int>(100 * scale)));
    Check(dragged && PreviewVisible(), "drag threshold and paused timer");
    Pump(1300);
    Check(!PreviewVisible(), "preview timer expires after the drag callback"); DestroyPreview();
    if (clipped) ClipCursor(nullptr);
}
bool ProbeHotkey(UINT modifiers, UINT key) {
    return async(launch::async, [=] {
        const bool registered = RegisterHotKey(nullptr, 98, modifiers | MOD_NOREPEAT, key) != FALSE;
        if (registered) UnregisterHotKey(nullptr, 98);
        return registered;
    }).get();
}
void HotkeyTransactions(HINSTANCE instance, const filesystem::path& root) {
    WNDCLASSEXW cls{sizeof(cls)}, existing{sizeof(existing)};
    cls.lpfnWndProc = PatternProc; cls.hInstance = instance; cls.lpszClassName = L"ScreenShotPlus.TestPattern";
    if (!GetClassInfoExW(instance, cls.lpszClassName, &existing)) Check(RegisterClassExW(&cls) != 0, "hotkey test class");
    HWND window = CreateWindowExW(0, L"ScreenShotPlus.TestPattern", L"Hotkey tests", 0,
        0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, nullptr);
    Check(window != nullptr, "hotkey test window");
    Config current = DefaultConfig(); current.folder = root / L"hotkey-images";
    current.modifiers = MOD_CONTROL | MOD_ALT | MOD_SHIFT; current.key = VK_F24;
    Hotkeys state; wstring error, warning;
    const auto file = root / L"hotkeys" / L"config.json";
    Check(ApplyConfiguredSettings(window, state, current, current, file, error), "initial hotkey setup");
    Check(!ProbeHotkey(current.modifiers, current.key), "active primary key is reserved");
    Config next = current; next.key = VK_F23;
    promise<bool> ready; auto registered = ready.get_future();
    jthread blocker([&](stop_token stop) {
        const bool ok = RegisterHotKey(nullptr, 99, next.modifiers | MOD_NOREPEAT, next.key) != FALSE;
        ready.set_value(ok);
        while (!stop.stop_requested()) this_thread::sleep_for(chrono::milliseconds(10));
        if (ok) UnregisterHotKey(nullptr, 99);
    });
    Check(registered.get(), "conflicting key registered on another thread");
    Check(!ApplyConfiguredSettings(window, state, current, next, file, error) && current.key == VK_F24,
        "hotkey conflict preserves current setting");
    Check(LoadConfig(file, warning).key == VK_F24 && !ProbeHotkey(current.modifiers, current.key),
        "conflict preserves persisted config and old registration");
    blocker.request_stop(); blocker.join();
    Check(ApplyConfiguredSettings(window, state, current, next, file, error) && current.key == VK_F23,
        "hotkey change succeeds once conflict removed");
    Check(ProbeHotkey(current.modifiers, VK_F24) && !ProbeHotkey(current.modifiers, VK_F23), "old registration released after change");
    next.key = VK_F22;
    Check(!ApplyConfiguredSettings(window, state, current, next, root / L"blocked" / L"config.json", error),
        "config failure rolls back candidate key");
    Check(current.key == VK_F23 && !ProbeHotkey(current.modifiers, VK_F23) && ProbeHotkey(current.modifiers, VK_F22),
        "config write failure keeps old key and releases candidate");
    ReleaseHotkeys(window, state); DestroyWindow(window);
    Check(ProbeHotkey(current.modifiers, VK_F23), "shutdown releases hotkey");
}
bool AppSmoke(HINSTANCE instance, const filesystem::path& artifacts) {
    HANDLE mutex = CreateMutexW(nullptr, FALSE, L"Local\\ScreenShotPlus.Singleton");
    const DWORD created = GetLastError();
    Check(mutex != nullptr, "singleton mutex available");
    if (created == ERROR_ALREADY_EXISTS) { CloseHandle(mutex); return false; }
    const auto file = artifacts / L"app-config" / L"config.json";
    Config initial = DefaultConfig(); initial.folder = artifacts / L"app-screens";
    wstring error; Check(SaveConfig(file, initial, error), "isolated application config");
    const DWORD uiThread = GetCurrentThreadId(); atomic<bool> ok = false;
    jthread observer([&] {
        HWND panel = nullptr;
        for (int i = 0; i < 50 && !panel; ++i) {
            HWND candidate = FindWindowW(PANEL_CLASS, nullptr);
            DWORD pid = 0; if (candidate) GetWindowThreadProcessId(candidate, &pid);
            if (pid == GetCurrentProcessId() && IsWindowVisible(candidate)) panel = candidate;
            else this_thread::sleep_for(chrono::milliseconds(50));
        }
        try {
            Check(panel != nullptr, "app panel available");
            const auto executable = filesystem::current_path() / L"x64" / L"Release" / L"ScreenShotPlus.exe";
            STARTUPINFOW startup{sizeof(startup)}; startup.dwFlags = STARTF_USESHOWWINDOW; startup.wShowWindow = SW_HIDE;
            PROCESS_INFORMATION child{};
            Check(CreateProcessW(executable.c_str(), nullptr, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &child), "launch second instance");
            const DWORD waited = WaitForSingleObject(child.hProcess, 5000);
            if (waited != WAIT_OBJECT_0) TerminateProcess(child.hProcess, 1);
            DWORD exitCode = 1; GetExitCodeProcess(child.hProcess, &exitCode);
            CloseHandle(child.hThread); CloseHandle(child.hProcess);
            Check(waited == WAIT_OBJECT_0 && exitCode == 0, "second instance exits successfully");
            Snapshot(panel, artifacts / L"panel-ui"); Click(panel, 100, 370);
            HWND settings = FindWindowW(L"ScreenShotPlus.Settings", nullptr);
            Check(settings && IsWindowVisible(settings), "settings opens");
            Snapshot(settings, artifacts / L"settings-ui");
            // 按钮位置由设置模块导出，测试只负责点击逻辑中心，不再自行换算缩放。
            const auto center = [](SettingsButton button) {
                const RectF rect = SettingsButtonRect(button);
                return POINT{static_cast<LONG>(rect.X + rect.Width / 2), static_cast<LONG>(rect.Y + rect.Height / 2)};
            };
            const POINT seconds = center(SettingsButton::Seconds), save = center(SettingsButton::Save);
            Click(settings, seconds.x, seconds.y);
            Click(settings, save.x, save.y);
            wstring warning;
            Config saved;
            const auto deadline = GetTickCount64() + 3000;
            for (;;) {
                saved = LoadConfig(file, warning);
                if (!IsWindowVisible(settings) && saved.previewSeconds == 5) break;
                if (GetTickCount64() > deadline) {
                    Check(false, "settings save did not take effect within 3s");
                }
                this_thread::sleep_for(chrono::milliseconds(50));
            }
            Check(!IsWindowVisible(settings) && saved.previewSeconds == 5, "UI settings persisted");
            HWND pattern = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
                L"ScreenShotPlus.TestPattern", L"Application capture test", WS_POPUP,
                100, 100, 400, 300, nullptr, nullptr, instance, nullptr);
            ShowWindow(pattern, SW_SHOWNOACTIVATE); UpdateWindow(pattern); DwmFlush();
            Click(panel, 100, 198);
            HWND overlay = FindWindowW(L"ScreenShotPlus.Selection", nullptr);
            Check(overlay != nullptr, "application starts region capture");
            const int x = 150 - GetSystemMetrics(SM_XVIRTUALSCREEN), y = 150 - GetSystemMetrics(SM_YVIRTUALSCREEN);
            SendMessageW(overlay, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x, y));
            SendMessageW(overlay, WM_LBUTTONUP, 0, MAKELPARAM(x + 80, y + 60));
            DestroyWindow(pattern);
            Check(PreviewVisible() && !IsWindowVisible(panel), "application displays saved preview");
            vector<filesystem::path> screenshots;
            for (const auto& item : filesystem::directory_iterator(initial.folder)) if (item.path().extension() == L".png") screenshots.push_back(item.path());
            Check(screenshots.size() == 1, "exactly one screenshot auto saved");
            { Bitmap image(screenshots[0].c_str()); Check(image.GetWidth() == 80 && image.GetHeight() == 60, "application PNG dimensions"); }
            SendMessageW(panel, WM_HOTKEY, 1, 0);
            overlay = FindWindowW(L"ScreenShotPlus.Selection", nullptr);
            Check(overlay != nullptr && !PreviewVisible(), "next capture hides old preview");
            SendMessageW(panel, WM_HOTKEY, 1, 0); SendMessageW(panel, WM_SHOW_PANEL, 0, 0);
            Check(FindWindowW(L"ScreenShotPlus.Selection", nullptr) == overlay && !IsWindowVisible(panel), "capture state prevents duplicate capture and panel");
            SendMessageW(overlay, WM_KEYDOWN, VK_ESCAPE, 0);
            Check(PreviewVisible() && filesystem::exists(screenshots[0]), "cancel restores previous saved preview");
            SendMessageW(panel, WM_SHOW_PANEL, 0, 0);
            Click(panel, 100, 425); ok = true;
        } catch (const exception& e) { cerr << "App smoke: " << e.what() << '\n'; PostThreadMessageW(uiThread, WM_QUIT, 1, 0); }
    });
    const int result = RunApp(instance, false, file);
    CloseHandle(mutex);
    Check(result == 0, "app message loop / shutdown");
    observer.join(); Check(ok, "app startup / settings / exit");
    return true;
}
}
int wmain(int argc, wchar_t** argv) {
    bool liveUi = !(argc > 1 && wstring_view(argv[1]) == L"--no-ui");
    HANDLE running = OpenMutexW(SYNCHRONIZE, FALSE, L"Local\\ScreenShotPlus.Singleton");
    if (running) { CloseHandle(running); liveUi = false; }
    const HRESULT ole = OleInitialize(nullptr);
    if (FAILED(ole)) return 2;
    GdiplusStartupInput input; ULONG_PTR token = 0;
    if (GdiplusStartup(&token, &input, nullptr) != Ok) { OleUninitialize(); return 3; }
    const auto root = filesystem::current_path() / L"build" / L"test-artifacts" / to_wstring(GetCurrentProcessId());
    int code = 0;
    try {
        filesystem::create_directories(root);
        Geometry(); cout << "PASS selection geometry\n";
        Configuration(root); cout << "PASS config round trip / corrupt and unwritable paths\n";
        StartupRegistration(root); cout << "PASS startup registration / config sync / hotkey rollback / restore\n";
        const auto file = PngStorage(root); cout << "PASS PNG pixels / Unicode paths / unique saves\n";
        EditorAnnotation(root); cout << "PASS annotation flatten / colors / text / in-place write back\n";
        EditorInteraction(GetModuleHandleW(nullptr), root); cout << "PASS editor canvas mapping / swatch / undo / save / cancel\n";
        EditorEntry(GetModuleHandleW(nullptr), root); cout << "PASS preview click opens editor / drag still drags\n";
        FileTransfer(file); cout << "PASS Unicode file transfer / copy-only format\n";
        WritePermissions(root); cout << "PASS real ACL write denial / original config preserved / permissions restored\n";
        if (liveUi) {
            CaptureAndPreview(GetModuleHandleW(nullptr), root); cout << "PASS live capture / cancel / preview / timeout\n";
        } else cout << "SKIP live capture and preview UI: --no-ui or existing application instance\n";
        HotkeyTransactions(GetModuleHandleW(nullptr), root); cout << "PASS real hotkey conflict / change / write-failure rollback\n";
        if (liveUi && AppSmoke(GetModuleHandleW(nullptr), root))
            cout << "PASS application capture / persistence / state / single instance / shutdown\n";
        else cout << "SKIP application UI smoke: --no-ui or existing application instance\n";
        cout << "Monitors actually present: " << GetSystemMetrics(SM_CMONITORS) << '\n';
        wcout << L"Test artifacts: " << root.wstring() << L'\n';
    } catch (const exception& e) { cerr << "FAIL: " << e.what() << '\n'; code = 1; }
    CancelSelection(); DestroySettings(); DestroyPreview();
    GdiplusShutdown(token); OleUninitialize(); return code;
}
