#include "storage.h"
using namespace std;
using namespace Gdiplus;
wstring ScreenshotStem(const SYSTEMTIME& t) {
    wchar_t name[96]{};
    swprintf_s(name, L"Screenshot_%04u%02u%02u_%02u%02u%02u_%03u",
        t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    return name;
}
bool SavePng(Bitmap& image, const filesystem::path& folder, filesystem::path& file, wstring& error) {
    error_code ec; filesystem::create_directories(folder, ec);
    if (ec) { error = L"无法创建保存目录：" + folder.wstring(); return false; }
    IStream* stream = nullptr;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream))) { error = L"无法分配 PNG 编码缓冲区。"; return false; }
    const CLSID png{0x557cf406, 0x1a04, 0x11d3, {0x9a, 0x73, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e}};
    if (image.Save(stream, &png, nullptr) != Ok) {
        stream->Release(); error = L"PNG 编码失败，请重试。"; return false;
    }
    LARGE_INTEGER zero{};
    if (FAILED(stream->Seek(zero, STREAM_SEEK_SET, nullptr))) {
        stream->Release(); error = L"无法读取 PNG 编码结果。"; return false;
    }
    SYSTEMTIME time{}; GetLocalTime(&time);
    const auto stem = ScreenshotStem(time);
    ofstream output;
    filesystem::path candidate;
    for (unsigned suffix = 0; suffix < 10000; ++suffix) {
        candidate = folder / (stem + (suffix ? L"_" + to_wstring(suffix) : L"") + L".png");
        output.open(candidate, ios::binary | ios::out | ios::noreplace);
        if (output) break;
        output.clear();
        if (!filesystem::exists(candidate, ec) || ec) break;
    }
    if (!output.is_open()) {
        stream->Release(); error = L"无法写入保存目录；请在设置中更换目录后重试。"; return false;
    }
    array<char, 65536> buffer{};
    bool ok = true;
    for (;;) {
        ULONG read = 0;
        if (FAILED(stream->Read(buffer.data(), static_cast<ULONG>(buffer.size()), &read))) { ok = false; break; }
        if (!read) break;
        output.write(buffer.data(), read);
        if (!output) { ok = false; break; }
    }
    output.close(); ok = ok && !output.fail(); stream->Release();
    if (!ok) {
        filesystem::remove(candidate, ec); error = L"PNG 写入失败；图像已保留，可更换目录重试。"; return false;
    }
    file = move(candidate); return true;
}
