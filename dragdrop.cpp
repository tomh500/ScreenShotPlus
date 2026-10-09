#include "dragdrop.h"
using namespace std;
namespace {
struct FileData final : IDataObject {
    LONG refs = 1;
    wstring file;
    const CLIPFORMAT preferred = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT));
    explicit FileData(wstring path) : file(move(path)) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (id != IID_IUnknown && id != IID_IDataObject) return E_NOINTERFACE;
        *out = static_cast<IDataObject*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&refs)); }
    ULONG STDMETHODCALLTYPE Release() override {
        const LONG count = InterlockedDecrement(&refs);
        if (!count) delete this;
        return static_cast<ULONG>(count);
    }
    HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* format) override {
        if (!format) return E_POINTER;
        if (format->dwAspect != DVASPECT_CONTENT) return DV_E_DVASPECT;
        if (format->lindex != -1) return DV_E_LINDEX;
        if (!(format->tymed & TYMED_HGLOBAL)) return DV_E_TYMED;
        return format->cfFormat == CF_HDROP || format->cfFormat == preferred ? S_OK : DV_E_FORMATETC;
    }
    HRESULT STDMETHODCALLTYPE GetData(FORMATETC* format, STGMEDIUM* medium) override {
        if (!medium) return E_POINTER;
        *medium = {};
        const HRESULT check = QueryGetData(format);
        if (FAILED(check)) return check;
        const bool drop = format->cfFormat == CF_HDROP;
        const SIZE_T bytes = drop ? sizeof(DROPFILES) + (file.size() + 2) * sizeof(wchar_t) : sizeof(DWORD);
        HGLOBAL memory = GlobalAlloc(GHND, bytes);
        if (!memory) return E_OUTOFMEMORY;
        void* data = GlobalLock(memory);
        if (!data) { GlobalFree(memory); return E_OUTOFMEMORY; }
        if (drop) {
            auto* header = static_cast<DROPFILES*>(data);
            header->pFiles = sizeof(DROPFILES); header->fWide = TRUE;
            memcpy(static_cast<char*>(data) + sizeof(DROPFILES), file.c_str(), (file.size() + 1) * sizeof(wchar_t));
        } else *static_cast<DWORD*>(data) = DROPEFFECT_COPY;
        GlobalUnlock(memory);
        medium->tymed = TYMED_HGLOBAL; medium->hGlobal = memory; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC*, STGMEDIUM*) override { return DATA_E_FORMATETC; }
    HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC*, FORMATETC* out) override {
        if (!out) return E_POINTER;
        out->ptd = nullptr; return DATA_S_SAMEFORMATETC;
    }
    HRESULT STDMETHODCALLTYPE SetData(FORMATETC* format, STGMEDIUM* medium, BOOL release) override {
        if (!format || !medium) return E_POINTER;
        const auto performed = RegisterClipboardFormatW(CFSTR_PERFORMEDDROPEFFECT);
        const auto pasted = RegisterClipboardFormatW(CFSTR_PASTESUCCEEDED);
        if (format->cfFormat != performed && format->cfFormat != pasted) return E_NOTIMPL;
        if (release) ReleaseStgMedium(medium);
        return S_OK; // Transfers are copy-only; acknowledgments never delete the source.
    }
    HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD direction, IEnumFORMATETC** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (direction != DATADIR_GET) return E_NOTIMPL;
        FORMATETC formats[]{{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL},
            {preferred, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL}};
        return SHCreateStdEnumFmtEtc(2, formats, out);
    }
    HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA** out) override {
        if (out) *out = nullptr;
        return OLE_E_ADVISENOTSUPPORTED;
    }
};
struct DropSource final : IDropSource {
    LONG refs = 1;
    function<void(DWORD)> feedback;
    explicit DropSource(function<void(DWORD)> update) : feedback(move(update)) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (id != IID_IUnknown && id != IID_IDropSource) return E_NOINTERFACE;
        *out = static_cast<IDropSource*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&refs)); }
    ULONG STDMETHODCALLTYPE Release() override {
        const LONG count = InterlockedDecrement(&refs);
        if (!count) delete this;
        return static_cast<ULONG>(count);
    }
    HRESULT STDMETHODCALLTYPE QueryContinueDrag(BOOL escape, DWORD keys) override {
        if (escape) return DRAGDROP_S_CANCEL;
        return keys & MK_LBUTTON ? S_OK : DRAGDROP_S_DROP;
    }
    HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD effect) override {
        if (feedback) feedback(effect);
        return DRAGDROP_S_USEDEFAULTCURSORS;
    }
};
}
HRESULT CreateFileDataObject(const filesystem::path& file, IDataObject** result) {
    if (!result) return E_POINTER;
    *result = nullptr;
    if (!file.is_absolute() || file.native().find(L'\0') != wstring::npos) return E_INVALIDARG;
    auto* data = new (nothrow) FileData(file.wstring());
    if (!data) return E_OUTOFMEMORY;
    *result = data; return S_OK;
}
HRESULT DragFile(const filesystem::path& file, DWORD& effect, function<void(DWORD)> feedback) {
    effect = DROPEFFECT_NONE;
    error_code ec;
    if (!filesystem::is_regular_file(file, ec)) return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    IDataObject* data = nullptr;
    const HRESULT created = CreateFileDataObject(file, &data);
    if (FAILED(created)) return created;
    auto* source = new (nothrow) DropSource(move(feedback));
    if (!source) { data->Release(); return E_OUTOFMEMORY; }
    const HRESULT result = DoDragDrop(data, source, DROPEFFECT_COPY, &effect);
    source->Release(); data->Release(); return result;
}
