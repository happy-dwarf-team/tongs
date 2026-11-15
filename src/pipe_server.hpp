#pragma once

#include "protocol.hpp"

#include <string>

namespace tongs {

inline constexpr int kMaxClients = 8;  // pipe instances, each served by its own thread; a subscription holds one

using LogFn = void (*)(int level, const std::string& message);  // level: AnvilLogLevel

// Starts background threads serving \\.\pipe\<name> (`name` from PipeName):
// up to kMaxClients clients at once (so `state` answers while another client
// waits in run_until), local clients of the current Windows user only. Returns
// false when the pipe could not be created (for example a second game already
// owns it).
bool StartPipeServer(const Handlers& handlers, LogFn log, const std::string& name);

}  // namespace tongs
