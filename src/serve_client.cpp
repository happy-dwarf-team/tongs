#include "serve_client.hpp"

#include "journal.hpp"

#include <windows.h>

#include <algorithm>
#include <memory>
#include <string>

namespace tongs {
namespace {

constexpr std::size_t kMaxLine = 4096;
constexpr int kStreamWaitMs = 20;  // a subscribed connection looks for requests this often

std::uint64_t UnixMs() {
    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);
    const std::uint64_t ticks = (static_cast<std::uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    return ticks / 10000 - 11644473600000ull;  // 100 ns since 1601 -> ms since 1970
}

}  // namespace

void ServeClient(const Handlers& handlers, Channel& channel, int number) {
    std::string pending;
    std::shared_ptr<Subscription> subscription;
    NotificationHub* hub = handlers.notifications;
    const auto end = [&subscription, hub] {
        if (subscription && hub) hub->Unsubscribe(subscription);
    };
    char chunk[512];
    for (;;) {
        for (std::size_t eol; (eol = pending.find('\n')) != std::string::npos;) {
            const std::string_view line = std::string_view(pending).substr(0, eol);
            std::string reply;
            const std::uint64_t asked_unix_ms = UnixMs();
            const ULONGLONG asked = GetTickCount64();
            if (subscription) {
                reply = SubscribedRequest(line, handlers, subscription);
            } else if (IsSubscribe(line)) {
                reply = Subscribe(line, handlers, subscription);
            } else {
                // A wait this request starts ends when the client leaves (Available() is -1 once it is gone).
                const ClientScope scope([&channel] { return channel.Available() < 0; });
                reply = HandleRequest(line, handlers, number);
            }
            if (handlers.journal != nullptr) {
                handlers.journal->Record({asked_unix_ms, number, line, reply, GetTickCount64() - asked});
            }
            pending.erase(0, eol + 1);
            if (!channel.Write(reply + "\n")) return end();
        }
        if (pending.size() > kMaxLine) {
            channel.Write("{\"ok\":false,\"error\":\"line too long\"}\n");
            return end();
        }
        std::size_t got = 0;
        if (!subscription) {
            if (!channel.Read(chunk, sizeof chunk, got) || got == 0) return;
            pending.append(chunk, got);
            continue;
        }
        const long available = channel.Available();
        if (available < 0) return end();  // the client left
        if (available > 0) {
            if (!channel.Read(chunk, std::min<std::size_t>(static_cast<std::size_t>(available), sizeof chunk), got) ||
                got == 0) {
                return end();
            }
            pending.append(chunk, got);
            continue;
        }
        const std::string lines = subscription->Take(kStreamWaitMs);
        if (!lines.empty() && !channel.Write(lines)) return end();
    }
}

}  // namespace tongs
