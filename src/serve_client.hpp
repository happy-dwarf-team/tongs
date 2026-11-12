#pragma once

#include "protocol.hpp"

#include <cstddef>
#include <string_view>

namespace tongs {

// One connected client, whatever carries its bytes: the named pipe, or a loopback socket under Wine and Proton
// (socket_server.hpp). The commands and their answers are the same on both.
class Channel {
public:
    virtual ~Channel() = default;
    // Blocks until bytes arrive. False when the client is gone.
    virtual bool Read(char* buffer, std::size_t size, std::size_t& got) = 0;
    // Bytes that can be read without blocking: 0 for none, -1 when the client left.
    virtual long Available() = 0;
    virtual bool Write(std::string_view data) = 0;
};

// Serves one client until it disconnects: one answer line per command line, in order. After `subscribe` the thread
// alternates between writing notifications and looking for a request without blocking, so the channel itself stays
// synchronous. A write to a client that does not read blocks only this thread; its queue drops the oldest
// notifications meanwhile. `number` names the client in the journal.
void ServeClient(const Handlers& handlers, Channel& channel, int number);

}  // namespace tongs
