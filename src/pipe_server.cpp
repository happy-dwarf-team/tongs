#include "pipe_server.hpp"

#include "serve_client.hpp"

#include <anvil/anvil_plugin.h>

#include <windows.h>
#include <sddl.h>

#include <memory>

namespace tongs {
namespace {

constexpr DWORD kBufferSize = 4096;

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
                            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                            kMaxClients, kBufferSize, kBufferSize, 0, &server.security);
}

// The named pipe as a Channel: byte mode, synchronous, with PeekNamedPipe to look for a request without blocking.
class PipeChannel final : public Channel {
public:
    explicit PipeChannel(HANDLE pipe) : pipe_(pipe) {}
    bool Read(char* buffer, std::size_t size, std::size_t& got) override {
        DWORD read = 0;
        const bool ok = ReadFile(pipe_, buffer, static_cast<DWORD>(size), &read, nullptr) != 0;
        got = read;
        return ok;
    }
    long Available() override {
        DWORD available = 0;
        if (!PeekNamedPipe(pipe_, nullptr, 0, nullptr, &available, nullptr)) return -1;  // the client left
        return static_cast<long>(available);
    }
    bool Write(std::string_view data) override {
        DWORD written = 0;
        return WriteFile(pipe_, data.data(), static_cast<DWORD>(data.size()), &written, nullptr) &&
               written == data.size();
    }

private:
    HANDLE pipe_;
};

struct Instance {
    Server* server;
    HANDLE pipe;
    int number;  // from 1, for the log
};

// Serves one pipe instance for the rest of the process, one client at a time.
DWORD WINAPI InstanceThread(void* param) {
    const Instance instance = *static_cast<Instance*>(param);
    delete static_cast<Instance*>(param);
    for (;;) {
        const bool connected = ConnectNamedPipe(instance.pipe, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED;
        if (connected) {
            PipeChannel channel(instance.pipe);
            ServeClient(instance.server->handlers, channel, instance.number);
        }
        FlushFileBuffers(instance.pipe);
        DisconnectNamedPipe(instance.pipe);
    }
}

bool StartInstance(Server& server, HANDLE pipe, int number) {
    auto* instance = new Instance{&server, pipe, number};
    HANDLE thread = CreateThread(nullptr, 0, InstanceThread, instance, 0, nullptr);
    if (thread == nullptr) {
        delete instance;
        return false;
    }
    SetThreadDescription(thread, L"tongs pipe");
    CloseHandle(thread);
    return true;
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
    // The first instance claims the name, so a second game cannot serve it.
    HANDLE first = CreatePipeInstance(*server, true);
    if (first == INVALID_HANDLE_VALUE) {
        log(ANVIL_LOG_ERROR, "cannot create the pipe (error " + std::to_string(GetLastError()) +
                                 "); is another game instance serving it?");
        LocalFree(server->descriptor);
        delete server;
        return false;
    }
    if (!StartInstance(*server, first, 1)) {
        log(ANVIL_LOG_ERROR, "cannot start the pipe thread (error " + std::to_string(GetLastError()) + ")");
        CloseHandle(first);
        LocalFree(server->descriptor);
        delete server;
        return false;
    }
    int instances = 1;
    while (instances < kMaxClients) {
        HANDLE pipe = CreatePipeInstance(*server, false);
        if (pipe == INVALID_HANDLE_VALUE) break;
        if (!StartInstance(*server, pipe, instances + 1)) {
            CloseHandle(pipe);
            break;
        }
        ++instances;
    }
    log(ANVIL_LOG_INFO, "serving \\\\.\\pipe\\tongs, " + std::to_string(instances) + " clients at a time");
    return true;
}

}  // namespace tongs
