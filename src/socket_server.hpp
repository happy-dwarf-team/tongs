#pragma once

#include "pipe_server.hpp"

#include <filesystem>
#include <string>
#include <string_view>

namespace tongs {

// Wine and Proton keep named pipes inside the wineserver, so a native Linux client cannot open \\.\pipe\tongs. Under
// Wine, Tongs can serve the same commands on a loopback socket, which a native client reaches directly (docs/design.md,
// "The socket under Wine"). It is off unless the `bridge` setting is on, and it is never started on Windows.

// True when this process runs under Wine or Proton (ntdll exports wine_get_version only there).
bool RunningUnderWine();

// The folder bridge.json is written to: `configured` when it is not empty (a Unix path such as /home/me/tongs is
// reached through Wine's Z: drive), else tongs/ in the user's XDG state folder ($XDG_STATE_HOME, else ~/.local/state).
// The host's home comes from WINEHOMEDIR, which Wine and Proton set; the runtime directory ($XDG_RUNTIME_DIR) is no
// choice, because Steam's container gives a game its own private one. Empty when the home cannot be found.
std::filesystem::path BridgeFolder(const std::string& configured);

// The owner of the TCP connection between `server_port` and `client_port` on 127.0.0.1, read from the text of
// /proc/net/tcp: the uid of the client's own socket and the uid of the server's end. Both are in the table while the
// client is connected. Nothing when either line is missing. Exposed for the tests.
struct PeerUids {
    long client = -1;
    long server = -1;
};
PeerUids ProcNetTcpUids(std::string_view table, int server_port, int client_port);

// Listens on 127.0.0.1 on a free port and writes {"port": N, "token": "...", "pid": N} to <folder>/bridge.json. A
// client sends one line, `RELAY <token> tongs` (the first line of Anvil's pipe relay, so one client speaks to both),
// and is answered `OK` or `ERR <reason>`; from then on it speaks the pipe's protocol. Up to kMaxClients at once; a
// client that does not send its line within five seconds is cut off. Returns false when it could not start.
bool StartSocketServer(const Handlers& handlers, LogFn log, const std::filesystem::path& folder);

}  // namespace tongs
