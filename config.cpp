#include "config.h"
#include "ui.h"
#include <nlohmann/json.hpp>
#include <cerrno>
using namespace std;
using nlohmann::json;
Config DefaultConfig() {
    Config config;
    config.folder = ui::KnownFolder(FOLDERID_Pictures) / L"ScreenShotPlus";
    return config;
}
filesystem::path PortableConfigFile() {
    wstring executable(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    if (!length || length >= executable.size()) return {};
    executable.resize(length);
    return filesystem::path(executable).parent_path() / L"ScreenShotPlus-data" / L"config.json";
}
filesystem::path ConfigFile() {
    const auto portable = PortableConfigFile();
    error_code ec;
    if (!portable.empty() && filesystem::exists(portable, ec)) return portable;
    const auto local = ui::KnownFolder(FOLDERID_LocalAppData);
    return local.empty() ? portable : local / L"ScreenShotPlus" / L"config.json";
}
bool ValidConfig(const Config& c) {
    const bool key = (c.key >= 'A' && c.key <= 'Z') || (c.key >= '0' && c.key <= '9') ||
        (c.key >= VK_F1 && c.key <= VK_F24);
    const auto path = c.folder.native();
    return c.folder.is_absolute() && !path.empty() && path.find(L'\0') == wstring::npos && key &&
        c.modifiers && !(c.modifiers & ~(MOD_CONTROL | MOD_SHIFT | MOD_ALT)) && c.previewSeconds <= 3600 &&
        c.animationLevel <= 4 && c.animationSpeed <= 2 && c.accentColor <= 4 &&
        c.previewCorner <= 3 && c.previewSize <= 2 && c.captureShade >= 25 && c.captureShade <= 80;
}
Config LoadConfig(const filesystem::path& file, wstring& warning) {
    Config result = DefaultConfig();
    error_code ec;
    if (!filesystem::exists(file, ec)) {
        if (ec) warning = L"无法读取配置目录，已使用默认设置。";
        return result;
    }
    try {
        if (filesystem::file_size(file) > 65536) throw runtime_error("config too large");
        ifstream input(file, ios::binary);
        const auto data = json::parse(input);
        Config parsed;
        const string path = data.at("folder").get<string>();
        parsed.folder = filesystem::path(u8string(reinterpret_cast<const char8_t*>(path.data()), path.size()));
        const auto integer = [&](const char* name, unsigned fallback, unsigned low, unsigned high, bool required = false) {
            const auto it = data.find(name);
            if (it == data.end()) {
                if (required) throw runtime_error("missing config");
                return fallback;
            }
            if (!it->is_number_integer()) throw runtime_error("non-integer config");
            // JSON can contain integers larger than INT64_MAX. Check unsigned
            // values before converting so oversized inputs can never wrap.
            uint64_t value = 0;
            if (it->is_number_unsigned()) value = it->get<uint64_t>();
            else {
                const auto signedValue = it->get<int64_t>();
                if (signedValue < 0) throw runtime_error("negative config");
                value = static_cast<uint64_t>(signedValue);
            }
            if (value < low || value > high) throw runtime_error("invalid config range");
            return static_cast<unsigned>(value);
        };
        parsed.modifiers = integer("modifiers", parsed.modifiers, 1, 7, true);
        parsed.key = integer("key", parsed.key, 0, 255, true);
        parsed.previewSeconds = integer("previewSeconds", parsed.previewSeconds, 0, 3600, true);
        parsed.animationLevel = integer("animationLevel", parsed.animationLevel, 0, 4);
        parsed.animationSpeed = integer("animationSpeed", parsed.animationSpeed, 0, 2);
        parsed.accentColor = integer("accentColor", parsed.accentColor, 0, 4);
        parsed.previewCorner = integer("previewCorner", parsed.previewCorner, 0, 3);
        parsed.previewSize = integer("previewSize", parsed.previewSize, 0, 2);
        parsed.captureShade = integer("captureShade", parsed.captureShade, 25, 80);
        parsed.printScreen = data.at("printScreen").get<bool>();
        parsed.startup = data.value("startup", false);
        parsed.captureAnimation = data.value("captureAnimation", true);
        parsed.dragAnimation = data.value("dragAnimation", true);
        parsed.startupAnimation = data.value("startupAnimation", true);
        parsed.captureGuides = data.value("captureGuides", true);
        parsed.rightClickLocation = data.value("rightClickLocation", true);
        parsed.previewFilename = data.value("previewFilename", true);
        parsed.previewHints = data.value("previewHints", true);
        parsed.hideAfterDrag = data.value("hideAfterDrag", false);
        if (!ValidConfig(parsed)) throw runtime_error("invalid config");
        result = move(parsed);
    } catch (const exception&) { warning = L"配置损坏或无效，已使用默认设置；原文件保留到下次保存设置。"; }
    return result;
}
bool SaveConfig(const filesystem::path& file, const Config& c, wstring& error) {
    if (!ValidConfig(c)) { error = L"设置无效：请检查完整目录、组合快捷键和自定义选项范围。"; return false; }
    filesystem::path temporary = file; temporary += L"." + to_wstring(GetCurrentProcessId()) + L".tmp";
    bool created = false;
    const auto failed = [&](const wstring& stage, const error_code& code) {
        error_code cleanup;
        if (created) filesystem::remove(temporary, cleanup);
        error = L"无法保存设置（" + stage + L"，错误 " + to_wstring(code.value()) + L"），原设置仍然有效。\n配置：" + file.wstring();
        return false;
    };
    if (!file.is_absolute()) return failed(L"配置路径无效", make_error_code(errc::invalid_argument));
    try {
        error_code ec;
        filesystem::create_directories(file.parent_path(), ec);
        if (ec) return failed(L"创建配置目录", ec);
        const auto path = c.folder.u8string();
        const json data{{"folder", string(reinterpret_cast<const char*>(path.data()), path.size())},
            {"modifiers", c.modifiers}, {"key", c.key}, {"printScreen", c.printScreen},
            {"previewSeconds", c.previewSeconds}, {"startup", c.startup},
            {"animationLevel", c.animationLevel}, {"animationSpeed", c.animationSpeed}, {"accentColor", c.accentColor},
            {"previewCorner", c.previewCorner}, {"previewSize", c.previewSize}, {"captureShade", c.captureShade},
            {"captureAnimation", c.captureAnimation}, {"dragAnimation", c.dragAnimation},
            {"startupAnimation", c.startupAnimation}, {"captureGuides", c.captureGuides},
            {"rightClickLocation", c.rightClickLocation}, {"previewFilename", c.previewFilename},
            {"previewHints", c.previewHints}, {"hideAfterDrag", c.hideAfterDrag}};
        ofstream output(temporary, ios::binary | ios::trunc);
        if (!output) return failed(L"创建临时配置", error_code(errno ? errno : EIO, generic_category()));
        created = true;
        output << data.dump(2) << '\n'; output.close();
        if (!output) return failed(L"写入配置", error_code(errno ? errno : EIO, generic_category()));
        filesystem::rename(temporary, file, ec);
        if (ec) return failed(L"替换配置文件", ec);
        return true;
    } catch (const exception&) { return failed(L"生成配置", make_error_code(errc::io_error)); }
}
bool SaveConfigWithFallback(const filesystem::path& file, const filesystem::path& fallback,
    const Config& config, filesystem::path& savedFile, wstring& error) {
    if (SaveConfig(file, config, error)) { savedFile = file; error.clear(); return true; }
    if (!ValidConfig(config) || fallback.empty() || fallback == file) return false;
    const auto originalError = error;
    if (SaveConfig(fallback, config, error)) { savedFile = fallback; error.clear(); return true; }
    error = originalError + L"\n备用配置也无法保存：" + fallback.wstring();
    return false;
}
wstring HotkeyText(const Config& c) {
    wstring text;
    if (c.modifiers & MOD_CONTROL) text += L"Ctrl + ";
    if (c.modifiers & MOD_ALT) text += L"Alt + ";
    if (c.modifiers & MOD_SHIFT) text += L"Shift + ";
    if (c.key >= VK_F1 && c.key <= VK_F24) text += L"F" + to_wstring(c.key - VK_F1 + 1);
    else text += static_cast<wchar_t>(c.key);
    return text;
}
