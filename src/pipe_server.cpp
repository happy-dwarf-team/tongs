#include "pipe_server.hpp"

#include <anvil/anvil_plugin.h>

#include <windows.h>
#include <sddl.h>

#include <memory>

namespace lc {
namespace {

constexpr DWORD kBufferSize = 4096;
constexpr std::size_t kMaxLine = 4096;

struct Server {
    Handlers handlers;
    LogFn log;
    SECURITY_ATTRIBUTES security{};
    PSECURITY_DESCRIPTOR descriptor = nullptr;
};

// DACL granting access only to the user the game runs as (and SYSTEM).
bool BuildSecurity(Server& server) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    auto buffer = std::make_unique<unsigned char[]>(size);
    const bool got = GetTokenInformation(token, TokenUser, buffer.get(), size, &size) != 0;
    CloseHandle(token);
    if (!got) return false;
    LPWSTR sid = nullptr;
    if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer.get())->User.Sid, &sid)) return false;
    const std::wstring sddl = std::wstring(L"D:P(A;;GA;;;") + sid + L")(A;;GA;;;SY)";
    LocalFree(sid);
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &server.descriptor,
                                                              nullptr)) {
        return false;
    }
    server.security.nLength = sizeof(server.security);
    server.security.lpSecurityDescriptor = server.descriptor;
    server.security.bInheritHandle = FALSE;
    return true;
}

HANDLE CreatePipeInstance(Server& server, bool first) {
    return CreateNamedPipeW(kPipeName, PIPE_ACCESS_DUPLEX | (first ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0),
                            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1,
                            kBufferSize, kBufferSize, 0, &server.security);
}

bool WriteAll(HANDLE pipe, const std::string& text) {
    DWORD written = 0;
    return WriteFile(pipe, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) &&
           written == text.size();
}

// Serves one connected client until it disconnects.
void ServeClient(Server& server, HANDLE pipe) {
    std::string pending;
    char chunk[512];
    for (;;) {
        DWORD read = 0;
        if (!ReadFile(pipe, chunk, sizeof chunk, &read, nullptr) || read == 0) return;
        pending.append(chunk, read);
        for (std::size_t eol; (eol = pending.find('\n')) != std::string::npos;) {
            const std::string reply = HandleRequest(std::string_view(pending).substr(0, eol), server.handlers) + "\n";
            pending.erase(0, eol + 1);
            if (!WriteAll(pipe, reply)) return;
        }
        if (pending.size() > kMaxLine) {
            WriteAll(pipe, "{\"ok\":false,\"error\":\"line too long\"}\n");
            return;
        }
    }
}

DWORD WINAPI ServerThread(void* param) {
    auto& server = *static_cast<Server*>(param);
    HANDLE pipe = CreatePipeInstance(server, true);
    if (pipe == INVALID_HANDLE_VALUE) {
        server.log(ANVIL_LOG_ERROR, "cannot create the pipe (error " + std::to_string(GetLastError()) +
                                        "); is another game instance serving it?");
        return 1;
    }
    server.log(ANVIL_LOG_INFO, "serving \\\\.\\pipe\\eu4-lab-control");
    for (;;) {
        const bool connected = ConnectNamedPipe(pipe, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED;
        if (connected) ServeClient(server, pipe);
        FlushFileBuffers(pipe);
        DisconnectNamedPipe(pipe);
    }
}

}  // namespace

bool StartPipeServer(const Handlers& handlers, LogFn log) {
    // Lives for the rest of the process, like the thread that uses it.
    auto* server = new Server{handlers, log};
    if (!BuildSecurity(*server)) {
        log(ANVIL_LOG_ERROR, "cannot build the pipe security descriptor (error " + std::to_string(GetLastError()) + ")");
        delete server;
        return false;
    }
    HANDLE thread = CreateThread(nullptr, 0, ServerThread, server, 0, nullptr);
    if (thread == nullptr) {
        log(ANVIL_LOG_ERROR, "cannot start the pipe thread (error " + std::to_string(GetLastError()) + ")");
        LocalFree(server->descriptor);
        delete server;
        return false;
    }
    SetThreadDescription(thread, L"eu4-lab-control pipe");
    CloseHandle(thread);
    return true;
}

}  // namespace lc
