#pragma once

#include "protocol.hpp"

#include <string>

namespace lc {

inline constexpr wchar_t kPipeName[] = L"\\\\.\\pipe\\eu4-lab-control";

using LogFn = void (*)(int level, const std::string& message);  // level: AnvilLogLevel

// Starts a background thread serving kPipeName: one client at a time, local
// clients of the current Windows user only. Returns false when the pipe could
// not be created (for example a second game already owns it).
bool StartPipeServer(const Handlers& handlers, LogFn log);

}  // namespace lc
