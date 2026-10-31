// Synthetic integration fixture, compiled in the guest. Never copies arbitrary
// clipboard data: every top-level name must have the reserved fixture prefix.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <shldisp.h>
#include <filesystem>
#include <fstream>
#include <cstring>
#include <cstdint>
#include <vector>
#include <string>
namespace fs = std::filesystem;
const std::wstring prefix = L"StreamingClipboardFixture-";
bool allowed(const std::wstring &name) {
  return name.rfind(prefix, 0) == 0 && name.find(L"..") == std::wstring::npos && name.find(L':') == std::wstring::npos && name.find(L'/') == std::wstring::npos;
}
void pump() { MSG m; while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); } }
int wmain(int argc, wchar_t **argv) {
  if (argc != 4) return 2;
  std::wstring command = argv[1]; fs::path root = argv[2], receipt = argv[3];
  if (!root.is_absolute() || !receipt.is_absolute()) return 2;
  HRESULT result = OleInitialize(nullptr); if (FAILED(result)) return 3;
  HWND clipboard_window = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
  if (!clipboard_window) { OleUninitialize(); return 3; }
  unsigned entries = 0; bool copied = false; std::uint64_t read_bytes = 0;
  if (command == L"copy") {
    std::vector<std::wstring> paths;
    for (const auto &item : fs::directory_iterator(root)) {
      auto name = item.path().filename().wstring(); if (!allowed(name) || name.find(L'\\') != std::wstring::npos) { OleUninitialize(); return 4; }
      paths.push_back(fs::absolute(item.path()).wstring());
    }
    if (paths.empty() || paths.size() > 16) { OleUninitialize(); return 4; }
    std::size_t characters = 1; for (const auto &path : paths) characters += path.size() + 1;
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sizeof(DROPFILES) + characters * sizeof(wchar_t));
    auto *drop = static_cast<DROPFILES *>(GlobalLock(memory)); drop->pFiles = sizeof(DROPFILES); drop->fWide = TRUE;
    auto *position = reinterpret_cast<wchar_t *>(reinterpret_cast<unsigned char *>(drop) + sizeof(DROPFILES));
    for (const auto &path : paths) { std::memcpy(position, path.c_str(), (path.size() + 1) * sizeof(wchar_t)); position += path.size() + 1; }
    GlobalUnlock(memory);
    result = E_ACCESSDENIED;
    for (unsigned retry = 0; retry < 20; ++retry) {
      if (OpenClipboard(clipboard_window)) {
        EmptyClipboard();
        if (SetClipboardData(CF_HDROP, memory)) { memory = nullptr; result = S_OK; entries = static_cast<unsigned>(paths.size()); }
        CloseClipboard(); break;
      }
      Sleep(100);
    }
    if (memory) GlobalFree(memory);
  } else if (command == L"paste" || command == L"read" || command == L"revoked" || command == L"large") {
    IDataObject *object = nullptr;
    result = OleGetClipboard(&object);
    FORMATETC descriptor {static_cast<CLIPFORMAT>(RegisterClipboardFormatW(CFSTR_FILEDESCRIPTORW)), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM data {};
    if (SUCCEEDED(result)) result = object->GetData(&descriptor, &data);
    unsigned first_file = 0; bool have_file = false;
    if (SUCCEEDED(result)) {
      auto *group = static_cast<FILEGROUPDESCRIPTORW *>(GlobalLock(data.hGlobal));
      bool valid = group && group->cItems > 0 && group->cItems <= 20000 &&
          GlobalSize(data.hGlobal) >= sizeof(UINT) + std::size_t(group->cItems) * sizeof(FILEDESCRIPTORW);
      if (valid) for (UINT i = 0; i < group->cItems; ++i) {
        auto name = std::wstring(group->fgd[i].cFileName);
        if (!allowed(name)) valid = false;
        if (!have_file && !(group->fgd[i].dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) { have_file = true; first_file = i; }
        if (command == L"large" && name == L"StreamingClipboardFixture-large.bin") { have_file = true; first_file = i; }
      }
      if (valid) entries = group->cItems;
      if (group) GlobalUnlock(data.hGlobal);
      ReleaseStgMedium(&data);
      if (!valid) result = E_ACCESSDENIED;
    }
    if (SUCCEEDED(result) && command == L"paste") {
      fs::create_directories(root); PIDLIST_ABSOLUTE pidl = nullptr; IShellFolder *folder = nullptr; IDropTarget *target = nullptr;
      result = SHParseDisplayName(root.c_str(), nullptr, &pidl, 0, nullptr);
      if (SUCCEEDED(result)) result = SHBindToObject(nullptr, pidl, nullptr, IID_PPV_ARGS(&folder));
      if (SUCCEEDED(result)) result = folder->CreateViewObject(nullptr, IID_PPV_ARGS(&target));
      DWORD effect = DROPEFFECT_COPY; POINTL point {};
      if (SUCCEEDED(result)) result = target->DragEnter(object, MK_LBUTTON | MK_CONTROL, point, &effect);
      if (SUCCEEDED(result)) { effect = DROPEFFECT_COPY; result = target->Drop(object, MK_CONTROL, point, &effect); }
      if (target) target->Release(); if (folder) folder->Release(); if (pidl) CoTaskMemFree(pidl);
      const auto deadline = GetTickCount64() + 45000;
      // The PowerShell controller verifies the complete tree and hashes after
      // this shell operation; this loop also serves async virtual-file reads.
      unsigned stable = 0; std::uintmax_t last = 0;
      IDataObjectAsyncCapability *asynchronous = nullptr;
      object->QueryInterface(IID_PPV_ARGS(&asynchronous));
      while (SUCCEEDED(result) && GetTickCount64() < deadline) {
        pump(); std::uintmax_t size = 0; bool any = false;
        for (const auto &file : fs::recursive_directory_iterator(root)) if (file.is_regular_file()) { size += file.file_size(); any = true; }
        if (any && size == last) ++stable; else stable = 0;
        BOOL busy = FALSE;
        if (asynchronous) asynchronous->InOperation(&busy);
        last = size; if (stable >= 30 && !busy) { copied = true; break; } Sleep(100);
      }
      if (asynchronous) asynchronous->Release();
    } else if (SUCCEEDED(result) && have_file) {
      FORMATETC content {static_cast<CLIPFORMAT>(RegisterClipboardFormatW(CFSTR_FILECONTENTS)), nullptr, DVASPECT_CONTENT, static_cast<LONG>(first_file), TYMED_ISTREAM};
      result = object->GetData(&content, &data);
      if (SUCCEEDED(result)) {
        if (command == L"revoked") { if (OpenClipboard(clipboard_window)) { EmptyClipboard(); CloseClipboard(); } Sleep(200); pump(); }
        if (command == L"large") {
          LARGE_INTEGER position; position.QuadPart = (std::uint64_t(1) << 32) + 8;
          result = data.pstm->Seek(position, STREAM_SEEK_SET, nullptr);
        }
        unsigned char buffer[4096]; ULONG got = 0;
        if (SUCCEEDED(result)) result = data.pstm->Read(buffer, command == L"large" ? 16 : sizeof(buffer), &got);
        read_bytes = got;
        if (command == L"large" && SUCCEEDED(result)) {
          if (got != 16) result = STG_E_READFAULT;
          for (unsigned i = 0; i < got; ++i) if (buffer[i] != i + 8) result = STG_E_READFAULT;
        }
        ReleaseStgMedium(&data);
      }
    }
    if (object) object->Release();
  } else { OleUninitialize(); return 2; }
  std::ofstream report(receipt, std::ios::binary);
  report << "{\"HRESULT\":" << static_cast<std::uint32_t>(result) << ",\"Entries\":" << entries
         << ",\"ShellCopied\":" << (copied ? "true" : "false") << ",\"ReadBytes\":" << read_bytes << "}";
  report.close(); DestroyWindow(clipboard_window); OleUninitialize(); return FAILED(result) ? 1 : 0;
}
