// The loopback socket Tongs serves under Wine (src/socket_server.cpp), run as a real server in this process and driven
// by plain Winsock clients: the state file, the handshake and its refusals, a round trip of commands, a subscription
// that ends when its client leaves, the five second handshake timeout and the limit of eight clients. No game is
// needed, and the same test runs on Windows, where only the pipe is used in the game.
#include <winsock2.h>
#include <ws2tcpip.h>

#include "notifications.hpp"
#include "serve_client.hpp"
#include "socket_server.hpp"

#include <windows.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace fs = std::filesystem;

int g_checks = 0;
int g_failures = 0;
#define CHECK(condition)                                                           \
    do {                                                                           \
        ++g_checks;                                                                \
        if (!(condition)) {                                                        \
            ++g_failures;                                                          \
            std::printf("  FAILED %s:%d: %s\n", __FILE__, __LINE__, #condition);   \
        }                                                                          \
    } while (0)

void Run(const char* name, void (*body)()) {
    const int before = g_failures;
    body();
    std::printf("%s %s\n", g_failures == before ? "ok  " : "FAIL", name);
    std::fflush(stdout);
}

struct State {
    int port = 0;
    std::string token;
    unsigned long pid = 0;
};

State g_state;
fs::path g_folder;
tongs::NotificationHub* g_hub = nullptr;

bool ReadState(State& state) {
    std::ifstream in(g_folder / L"bridge.json", std::ios::binary);
    std::stringstream text;
    text << in.rdbuf();
    const std::string json = text.str();
    const auto number = [&](const char* key) -> long {
        const auto at = json.find(std::string("\"") + key + "\":");
        return at == std::string::npos ? -1 : std::strtol(json.c_str() + at + std::strlen(key) + 3, nullptr, 10);
    };
    const auto token_at = json.find("\"token\": \"");
    if (token_at == std::string::npos) return false;
    const auto start = token_at + 10;
    const auto end = json.find('"', start);
    if (end == std::string::npos) return false;
    state.token = json.substr(start, end - start);
    state.port = static_cast<int>(number("port"));
    state.pid = static_cast<unsigned long>(number("pid"));
    return state.port > 0;
}

SOCKET Connect() {
    SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<u_short>(g_state.port));
    inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    if (connect(s, reinterpret_cast<sockaddr*>(&address), sizeof address) != 0) {
        closesocket(s);
        return INVALID_SOCKET;
    }
    const DWORD timeout_ms = 15000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout_ms), sizeof timeout_ms);
    return s;
}

std::string ReadLine(SOCKET s) {
    std::string line;
    for (char c; recv(s, &c, 1, 0) == 1;) {
        if (c == '\n') break;
        line.push_back(c);
    }
    return line;
}

void SendText(SOCKET s, const std::string& text) { send(s, text.data(), static_cast<int>(text.size()), 0); }

std::string Ask(const std::string& request) {
    const SOCKET s = Connect();
    if (s == INVALID_SOCKET) return "(cannot connect)";
    SendText(s, request);
    std::string answer = ReadLine(s);
    closesocket(s);
    return answer;
}

// A connection that has passed the handshake.
SOCKET Login() {
    const SOCKET s = Connect();
    if (s == INVALID_SOCKET) return s;
    SendText(s, "RELAY " + g_state.token + " tongs\n");
    if (ReadLine(s) != "OK") {
        closesocket(s);
        return INVALID_SOCKET;
    }
    return s;
}

bool Contains(const std::string& text, const char* part) { return text.find(part) != std::string::npos; }

void LogToConsole(int level, const std::string& message) { std::printf("  [tongs %d] %s\n", level, message.c_str()); }

void TestStateFile() {
    CHECK(g_state.port > 0 && g_state.port < 65536);
    CHECK(g_state.token.size() == 32);
    CHECK(g_state.token.find_first_not_of("0123456789abcdef") == std::string::npos);
    CHECK(g_state.pid == GetCurrentProcessId());
}

void TestRefusals() {
    CHECK(Ask("hello\n") == "ERR bad request");
    CHECK(Ask("RELAY 00 tongs\n") == "ERR bad token");
    CHECK(Ask("RELAY " + g_state.token + " evil\n") == "ERR pipe not served");
    CHECK(Ask("RELAY " + g_state.token + " etch-host-0123456789abcdef0123456789abcdef\n") == "ERR pipe not served");
    CHECK(Ask("RELAY " + g_state.token + " ..\\\\tongs\n") == "ERR pipe not served");
}

void TestRoundTrip() {
    const SOCKET s = Login();
    CHECK(s != INVALID_SOCKET);
    if (s == INVALID_SOCKET) return;
    for (int i = 0; i < 3; ++i) {  // several commands on one connection, answered in order
        SendText(s, "ping\n");
        const std::string answer = ReadLine(s);
        CHECK(Contains(answer, "\"ok\":true") && Contains(answer, "\"pid\":4242"));
    }
    SendText(s, "nonsense\n");
    CHECK(Contains(ReadLine(s), "\"ok\":false"));
    closesocket(s);
}

void TestSubscriptionEndsWhenTheClientLeaves() {
    const SOCKET s = Login();
    CHECK(s != INVALID_SOCKET);
    if (s == INVALID_SOCKET) return;
    SendText(s, "subscribe date\n");
    CHECK(Contains(ReadLine(s), "\"ok\":true"));
    CHECK(g_hub->subscriptions() == 1);
    SendText(s, "ping\n");  // answered while subscribed
    CHECK(Contains(ReadLine(s), "\"pong\":true"));
    closesocket(s);
    for (int i = 0; i < 100 && g_hub->subscriptions() != 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK(g_hub->subscriptions() == 0);  // the server saw the socket close without a request
}

void TestSilentClientIsCutOff() {
    const SOCKET silent = Connect();
    CHECK(silent != INVALID_SOCKET);
    if (silent == INVALID_SOCKET) return;
    const auto begin = std::chrono::steady_clock::now();
    CHECK(ReadLine(silent) == "ERR bad request");  // after its handshake timeout, as for any request without a line
    const auto waited = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - begin);
    CHECK(waited.count() >= 3 && waited.count() <= 12);
    closesocket(silent);
    CHECK(Ask("RELAY 00 tongs\n") == "ERR bad token");  // and the server still answers others
}

void TestEightClientsAtOnce() {
    std::vector<SOCKET> held;
    for (int i = 0; i < tongs::kMaxClients; ++i) {
        held.push_back(Login());
        CHECK(held.back() != INVALID_SOCKET);
    }
    CHECK(Ask("RELAY " + g_state.token + " tongs\n") == "ERR busy");
    for (const SOCKET s : held) {
        if (s != INVALID_SOCKET) closesocket(s);
    }
    SOCKET again = INVALID_SOCKET;
    for (int i = 0; i < 100 && again == INVALID_SOCKET; ++i) {  // the freed threads need a moment
        again = Login();
        if (again == INVALID_SOCKET) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    CHECK(again != INVALID_SOCKET);
    if (again != INVALID_SOCKET) closesocket(again);
}

void ClearFolderEnvironment() {
    for (const wchar_t* name : {L"XDG_STATE_HOME", L"WINE_HOST_XDG_STATE_HOME", L"WINEHOMEDIR", L"HOME", L"WINE_HOST_HOME"}) {
        SetEnvironmentVariableW(name, nullptr);
    }
}

// The real table, where there is one (Wine on Linux): it lists this very connection, with the same owner at both ends.
void TestTheRealProcNetTcp() {
    std::ifstream in(L"Z:\\proc\\net\\tcp", std::ios::binary);
    if (!in) {
        std::printf("  skipped: no /proc/net/tcp here (not Wine on Linux)\n");
        return;
    }
    std::stringstream table;
    table << in.rdbuf();
    const SOCKET s = Login();
    CHECK(s != INVALID_SOCKET);
    if (s == INVALID_SOCKET) return;
    sockaddr_in local{};
    int size = sizeof local;
    CHECK(getsockname(s, reinterpret_cast<sockaddr*>(&local), &size) == 0);
    in.close();
    std::ifstream again(L"Z:\\proc\\net\\tcp", std::ios::binary);  // the connection exists now
    std::stringstream current;
    current << again.rdbuf();
    const tongs::PeerUids uids = tongs::ProcNetTcpUids(current.str(), g_state.port, ntohs(local.sin_port));
    CHECK(uids.client >= 0 && uids.server >= 0 && uids.client == uids.server);
    closesocket(s);
}

void TestFolder() {
    ClearFolderEnvironment();
    CHECK(tongs::BridgeFolder("").empty());  // no idea where the host's home is
    SetEnvironmentVariableW(L"WINEHOMEDIR", L"\\??\\Z:\\home\\me");  // Wine and Proton: the host's home as an NT path
    CHECK(tongs::BridgeFolder("") == fs::path(L"Z:\\home\\me\\.local\\state\\tongs"));
    SetEnvironmentVariableW(L"XDG_STATE_HOME", L"/data/state");
    CHECK(tongs::BridgeFolder("") == fs::path(L"Z:\\data\\state\\tongs"));
    SetEnvironmentVariableW(L"XDG_STATE_HOME", nullptr);
    SetEnvironmentVariableW(L"WINE_HOST_XDG_STATE_HOME", L"/data/renamed");  // how newer Wine passes it
    CHECK(tongs::BridgeFolder("") == fs::path(L"Z:\\data\\renamed\\tongs"));
    ClearFolderEnvironment();
    SetEnvironmentVariableW(L"WINE_HOST_HOME", L"/home/you");  // no WINEHOMEDIR: the host's HOME, however it arrives
    CHECK(tongs::BridgeFolder("") == fs::path(L"Z:\\home\\you\\.local\\state\\tongs"));
    CHECK(tongs::BridgeFolder("/tmp/x") == fs::path(L"Z:\\tmp\\x"));  // a configured Unix path is reached through Z:
    CHECK(tongs::BridgeFolder("D:\\bridge") == fs::path(L"D:\\bridge"));
    ClearFolderEnvironment();
}

void TestProcNetTcp() {
    const char* table =
        "  sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode\n"
        "   0: 00000000:0016 00000000:0000 0A 00000000:00000000 00:00000000 00000000     0        0 11 1 0 100 0 0 10 0\n"
        "   1: 0100007F:A7C1 0100007F:C350 01 00000000:00000000 00:00000000 00000000  1001        0 12 1 0 20 4 28 10 -1\n"
        "   2: 0100007F:C350 0100007F:A7C1 01 00000000:00000000 00:00000000 00000000  1000        0 13 1 0 20 4 29 10 -1\n"
        "   3: 0A00020F:A7C2 0A00020F:C350 01 00000000:00000000 00:00000000 00000000  7777        0 14 1 0 20 4 29 10 -1\n";
    const tongs::PeerUids other_user = tongs::ProcNetTcpUids(table, 0xC350, 0xA7C1);  // the client's socket is another user's
    CHECK(other_user.client == 1001 && other_user.server == 1000);
    const tongs::PeerUids missing = tongs::ProcNetTcpUids(table, 0xC350, 0x1234);
    CHECK(missing.client < 0 && missing.server < 0);
    const tongs::PeerUids not_loopback = tongs::ProcNetTcpUids(table, 0xC350, 0xA7C2);  // only 127.0.0.1 counts
    CHECK(not_loopback.client < 0 && not_loopback.server < 0);
    CHECK(tongs::ProcNetTcpUids("", 1, 2).client < 0);
}

// test_bridge --serve <folder|default> <seconds>: only serves, so a client outside this process (a native Linux client
// under Wine, tests/wine in the lab) can be tried against the real server. `default` takes BridgeFolder("").
int Serve(const char* folder, int seconds) {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 2;
    tongs::NotificationHub hub([] { return std::uint64_t{0}; });
    tongs::Handlers handlers;
    handlers.process_id = 4242;
    handlers.process_start_ms = 7;
    handlers.notifications = &hub;
    const fs::path target = std::strcmp(folder, "default") == 0 ? tongs::BridgeFolder("") : fs::path(folder);
    if (target.empty()) {
        std::printf("no folder for bridge.json (the host's home is unknown)\n");
        return 2;
    }
    if (!tongs::StartSocketServer(handlers, LogToConsole, target)) return 2;
    std::fflush(stdout);
    std::this_thread::sleep_for(std::chrono::seconds(seconds));
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 4 && std::strcmp(argv[1], "--serve") == 0) return Serve(argv[2], std::atoi(argv[3]));
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 2;
    g_folder = fs::temp_directory_path() / ("tongs-bridge-test-" + std::to_string(GetCurrentProcessId()));
    fs::remove_all(g_folder);

    tongs::NotificationHub hub([] { return std::uint64_t{0}; });
    g_hub = &hub;
    tongs::Handlers handlers;
    handlers.process_id = 4242;
    handlers.process_start_ms = 7;
    handlers.notifications = &hub;
    if (!tongs::StartSocketServer(handlers, LogToConsole, g_folder) || !ReadState(g_state)) {
        std::printf("cannot start the socket server\n");
        return 2;
    }

    Run("the state file", TestStateFile);
    Run("refusals", TestRefusals);
    Run("commands round-trip on one connection", TestRoundTrip);
    Run("a subscription ends when its client leaves", TestSubscriptionEndsWhenTheClientLeaves);
    Run("a client that sends nothing is cut off", TestSilentClientIsCutOff);
    Run("eight clients at once, a ninth is turned away", TestEightClientsAtOnce);
    Run("the folder of bridge.json", TestFolder);
    Run("the owners of a connection in /proc/net/tcp", TestProcNetTcp);
    Run("the real /proc/net/tcp lists our own connection", TestTheRealProcNetTcp);

    std::error_code ignored;
    fs::remove_all(g_folder, ignored);
    std::printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
