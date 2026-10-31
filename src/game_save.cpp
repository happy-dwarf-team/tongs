#include "game_save.hpp"

#include "game_memory.hpp"

#include <windows.h>

#include <algorithm>
#include <cstddef>
#include <cstring>

namespace tongs {
namespace {

using GetWriteDirFn = sym::Types::physfs_get_write_dir::Pointer;
using LoadFn = int (*)(void* request);
using HelperFn = void* (*)();
using SessionFn = void (*)(void* session);
using AssignFn = void* (*)(void* text, const char* chars, std::size_t size);
using FreeFn = void (*)(void* text);

constexpr std::size_t kHeadBytes = 1u << 20;   // a text or binary save's metadata is at its start
constexpr std::uint64_t kMaxZip = 1ull << 30;  // a zip larger than this is not read

std::wstring Wide(const std::string& utf8) {
    if (utf8.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), n);
    return out;
}

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

// The request as the loader reads it (0x50 bytes; std::string and std::vector as MSVC lays them out).
struct alignas(8) Request {
    unsigned char name[0x20];
    void* subfolders[3];
    void* session;
    unsigned char cloud;
    unsigned char pad[7];
    void* helper;
};
static_assert(sizeof(Request) == 0x50 && offsetof(Request, session) == 0x38 && offsetof(Request, cloud) == 0x40 &&
              offsetof(Request, helper) == 0x48);

// No C++ objects with destructors here: the body runs under __try.
bool Load(const SaveLayout& l, std::uintptr_t base, std::uintptr_t session, const char* name, std::size_t size,
          LoadCall* out) {
    Request request;
    std::memset(&request, 0, sizeof request);
    const std::uint64_t empty_capacity = 15;  // an empty std::string: size 0, the inline buffer
    std::memcpy(request.name + 0x18, &empty_capacity, 8);
    request.session = reinterpret_cast<void*>(session);
    request.cloud = 0;
    __try {
        request.helper = reinterpret_cast<HelperFn>(base + l.helper_rva)();
        reinterpret_cast<AssignFn>(base + l.assign_rva)(request.name, name, size);
        out->result = reinterpret_cast<LoadFn>(base + l.load_rva)(&request);
        reinterpret_cast<FreeFn>(base + l.string_free_rva)(request.name);
        return true;
    } __except (out->fault_code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        out->faulted = true;
        return false;
    }
}

bool Close(const SaveLayout& l, std::uintptr_t base, std::uintptr_t session, unsigned long* code) {
    __try {
        reinterpret_cast<SessionFn>(base + l.close_rva)(reinterpret_cast<void*>(session));
        return true;
    } __except (*code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool Session(std::uintptr_t base, const SaveLayout& l, std::uintptr_t& session) {
    std::uintptr_t root = 0;
    return Read(base + l.root_rva, root) && root != 0 && Read(root + l.session, session) && session != 0;
}

}  // namespace

std::string LocateSave(const std::string& name, std::string& path, bool& exists) {
    exists = false;
    char dir[MAX_PATH * 3] = {};
    if (!CopyWriteDir(anvil::Host{}.function(sym::physfs_get_write_dir), dir, sizeof dir)) {
        return "the game has not set its PhysFS write directory yet";
    }
    std::string base = dir;
    while (!base.empty() && (base.back() == '\\' || base.back() == '/')) base.pop_back();
    path = base + "\\save games\\" + name + ".eu4";
    const DWORD attributes = GetFileAttributesW(Wide(path).c_str());
    exists = attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
    return {};
}

std::string ReadSaveHead(const std::string& path, std::string& bytes) {
    bytes.clear();
    HANDLE file = CreateFileW(Wide(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return "cannot open the save (error " + std::to_string(GetLastError()) + ")";
    LARGE_INTEGER size{};
    char magic[2] = {};
    DWORD got = 0;
    std::string error;
    if (!GetFileSizeEx(file, &size) || !ReadFile(file, magic, 2, &got, nullptr) || got != 2) {
        error = "cannot read the save";
    } else {
        const bool zip = magic[0] == 'P' && magic[1] == 'K';
        const std::uint64_t want = zip ? static_cast<std::uint64_t>(size.QuadPart) : std::min<std::uint64_t>(size.QuadPart, kHeadBytes);
        if (want > kMaxZip) {
            error = "the save is larger than 1 GiB";
        } else {
            bytes.resize(static_cast<std::size_t>(want));
            std::memcpy(bytes.data(), magic, 2);
            std::size_t at = 2;
            while (at < bytes.size() && error.empty()) {
                const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(bytes.size() - at, 1u << 24));
                if (!ReadFile(file, bytes.data() + at, chunk, &got, nullptr) || got == 0) error = "cannot read the save";
                at += got;
            }
        }
    }
    CloseHandle(file);
    return error;
}

LoadCall CallLoad(std::uintptr_t base, const SaveLayout& l, const std::string& name) {
    LoadCall out;
    std::uintptr_t session = 0;
    if (!Session(base, l, session)) {
        out.faulted = true;
        out.fault_code = 0;
        return out;
    }
    Load(l, base, session, name.c_str(), name.size(), &out);
    return out;
}

bool CloseWindows(std::uintptr_t base, const SaveLayout& l, unsigned long& fault_code) {
    std::uintptr_t session = 0;
    if (!Session(base, l, session)) return false;
    return Close(l, base, session, &fault_code);
}

bool ReadLoading(std::uintptr_t base, const SaveLayout& l, bool& loading) {
    std::uintptr_t session = 0;
    std::uint8_t flag = 0;
    if (!Session(base, l, session) || !Read(session + l.loading, flag)) return false;
    loading = flag != 0;
    return true;
}

bool ReadRunningVersion(std::uintptr_t base, const SaveLayout& l, std::array<int, 4>& out) {
    std::uintptr_t app = 0;
    if (!Read(base + l.app_rva, app) || app == 0) return false;
    for (std::size_t i = 0; i < 4; ++i) {
        if (!Read(app + l.version[i], out[i])) return false;
    }
    return true;
}

}  // namespace tongs
