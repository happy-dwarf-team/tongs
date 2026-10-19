#include "game_files.hpp"

#include <windows.h>
#include <shlobj.h>

#include <string>

namespace tongs {
namespace {

using GetWriteDirFn = const char* (*)();

std::wstring Wide(const std::string& utf8) {
    if (utf8.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), n);
    return out;
}

std::string Utf8(const wchar_t* text) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string out(static_cast<std::size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), n, nullptr, nullptr);
    return out;
}

// PHYSFS_getWriteDir takes PhysFS's state lock for a moment and returns the
// directory's name; copied at once, under SEH like every read of game memory.
bool CopyWriteDir(GetWriteDirFn get, char* out, std::size_t size) {
    __try {
        const char* dir = get();
        if (dir == nullptr) return false;
        std::size_t i = 0;
        for (; i + 1 < size && dir[i] != '\0'; ++i) out[i] = dir[i];
        out[i] = '\0';
        return i > 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

std::string WithLogFile(std::string dir) {
    while (!dir.empty() && (dir.back() == '\\' || dir.back() == '/')) dir.pop_back();
    return dir + "\\logs\\error.log";
}

}  // namespace

ErrorLogLocation LocateErrorLog(std::uintptr_t image_base) {
    ErrorLogLocation out;
    const auto get = reinterpret_cast<GetWriteDirFn>(
        GetProcAddress(reinterpret_cast<HMODULE>(image_base), "PHYSFS_getWriteDir"));
    if (get != nullptr) {
        char dir[MAX_PATH * 3] = {};
        if (!CopyWriteDir(get, dir, sizeof dir)) {
            out.error = "the game has not set its PhysFS write directory yet";
            return out;
        }
        out.path = WithLogFile(dir);
        out.source = "physfs_write_dir";
        return out;
    }
    PWSTR documents = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &documents))) {
        CoTaskMemFree(documents);
        out.error = "no PHYSFS_getWriteDir export and no Documents folder";
        return out;
    }
    out.path = WithLogFile(Utf8(documents) + "\\Paradox Interactive\\Europa Universalis IV");
    out.source = "documents";
    CoTaskMemFree(documents);
    return out;
}

std::string ReadLogChunk(const std::string& path, std::uint64_t offset, std::size_t max, LineTail::Chunk& chunk) {
    chunk = {};
    HANDLE file = CreateFileW(Wide(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        chunk.missing = error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
        return "cannot open error.log (error " + std::to_string(error) + ")";
    }
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(file, &info)) {
        const DWORD error = GetLastError();
        CloseHandle(file);
        return "cannot read error.log's size (error " + std::to_string(error) + ")";
    }
    chunk.size = (static_cast<std::uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    chunk.file_id = (static_cast<std::uint64_t>(info.dwVolumeSerialNumber) << 48) ^
                    ((static_cast<std::uint64_t>(info.nFileIndexHigh) << 32) | info.nFileIndexLow);
    std::string error;
    if (chunk.size > offset && max > 0) {
        const std::uint64_t want = chunk.size - offset < max ? chunk.size - offset : max;
        chunk.data.resize(static_cast<std::size_t>(want));
        LARGE_INTEGER at;
        at.QuadPart = static_cast<LONGLONG>(offset);
        DWORD read = 0;
        if (!SetFilePointerEx(file, at, nullptr, FILE_BEGIN) ||
            !ReadFile(file, chunk.data.data(), static_cast<DWORD>(want), &read, nullptr)) {
            error = "cannot read error.log (error " + std::to_string(GetLastError()) + ")";
            read = 0;
        }
        chunk.data.resize(read);
    }
    CloseHandle(file);
    return error;
}

}  // namespace tongs
