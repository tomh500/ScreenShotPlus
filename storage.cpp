#include "storage.h"
using namespace std;
using namespace Gdiplus;
namespace {
const CLSID kPngEncoder{0x557cf406, 0x1a04, 0x11d3, {0x9a, 0x73, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e}};
// 把位图编码成 PNG 字节。调用方负责写入磁盘，因此编码失败不会产生半截文件。
bool EncodePng(Bitmap& image, vector<char>& bytes, wstring& error) {
    bytes.clear();
    IStream* stream = nullptr;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream))) { error = L"无法分配 PNG 编码缓冲区。"; return false; }
    if (image.Save(stream, &kPngEncoder, nullptr) != Ok) {
        stream->Release(); error = L"PNG 编码失败，请重试。"; return false;
    }
    LARGE_INTEGER zero{};
    if (FAILED(stream->Seek(zero, STREAM_SEEK_SET, nullptr))) {
        stream->Release(); error = L"无法读取 PNG 编码结果。"; return false;
    }
    array<char, 65536> buffer{};
    for (;;) {
        ULONG read = 0;
        if (FAILED(stream->Read(buffer.data(), static_cast<ULONG>(buffer.size()), &read)) || !read) break;
        bytes.insert(bytes.end(), buffer.data(), buffer.data() + read);
    }
    stream->Release();
    if (bytes.empty()) { error = L"PNG 编码结果为空。"; return false; }
    return true;
}
// 独占创建输出流，保证不会覆盖已存在的文件。
bool OpenUnique(const filesystem::path& candidate, ofstream& output) {
    output.open(candidate, ios::binary | ios::out | ios::noreplace);
    return output.is_open();
}
}
wstring ScreenshotStem(const SYSTEMTIME& t) {
    wchar_t name[96]{};
    swprintf_s(name, L"Screenshot_%04u%02u%02u_%02u%02u%02u_%03u",
        t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    return name;
}
bool SavePng(Bitmap& image, const filesystem::path& folder, filesystem::path& file, wstring& error) {
    error_code ec; filesystem::create_directories(folder, ec);
    if (ec) { error = L"无法创建保存目录：" + folder.wstring(); return false; }
    vector<char> bytes;
    if (!EncodePng(image, bytes, error)) return false;
    SYSTEMTIME time{}; GetLocalTime(&time);
    const auto stem = ScreenshotStem(time);
    ofstream output;
    filesystem::path candidate;
    for (unsigned suffix = 0; suffix < 10000; ++suffix) {
        candidate = folder / (stem + (suffix ? L"_" + to_wstring(suffix) : L"") + L".png");
        if (OpenUnique(candidate, output)) break;
        if (!filesystem::exists(candidate, ec) || ec) break;
    }
    if (!output.is_open()) { error = L"无法写入保存目录；请在设置中更换目录后重试。"; return false; }
    output.write(bytes.data(), static_cast<streamsize>(bytes.size()));
    output.close();
    if (output.fail()) {
        filesystem::remove(candidate, ec); error = L"PNG 写入失败；图像已保留，可更换目录重试。"; return false;
    }
    file = move(candidate); return true;
}
bool ReplacePng(Bitmap& image, const filesystem::path& file, wstring& error) {
    if (!file.is_absolute()) { error = L"截图路径无效，无法保存标注。"; return false; }
    vector<char> bytes;
    if (!EncodePng(image, bytes, error)) return false;
    // 临时文件与原文件同目录，保证替换是同卷操作。
    error_code ec;
    SYSTEMTIME time{}; GetLocalTime(&time);
    filesystem::path temporary;
    ofstream output;
    for (unsigned suffix = 0; suffix < 1000 && !output.is_open(); ++suffix) {
        temporary = file.parent_path() /
            (ScreenshotStem(time) + L"_edit" + (suffix ? L"_" + to_wstring(suffix) : L"") + L".png.tmp");
        OpenUnique(temporary, output);
    }
    if (!output.is_open()) { error = L"无法在截图目录创建临时文件；请检查目录权限。"; return false; }
    output.write(bytes.data(), static_cast<streamsize>(bytes.size()));
    output.close();
    if (output.fail()) {
        filesystem::remove(temporary, ec);
        error = L"写入编辑结果失败；原有截图保持不变。"; return false;
    }
    if (!MoveFileExW(temporary.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD code = GetLastError();
        filesystem::remove(temporary, ec);
        error = code == ERROR_SHARING_VIOLATION || code == ERROR_ACCESS_DENIED ?
            L"截图文件正被其他程序占用，无法覆盖；标注仍保留在编辑窗口中。" :
            L"无法替换原有截图；原有截图保持不变。";
        return false;
    }
    return true;
}
