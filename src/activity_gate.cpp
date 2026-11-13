#include "activity_gate.hpp"

#include "game_date.hpp"

#include <cassert>
#include <utility>

namespace tongs {
namespace {

constexpr char kInputDisabled[] = "input commands are disabled (set input = true in anvil/data/tongs/settings.toml)";
constexpr char kGameCallsDisabled[] = "game calls are disabled (set game_calls = true in anvil/data/tongs/settings.toml)";
constexpr char kSuspended[] =
    "Anvil suspended Tongs in this ironman or multiplayer game; the player can allow it with allow_ironman or "
    "allow_multiplayer = [\"tongs\"] in anvil/anvil.toml";

const char* Bool(bool value) { return value ? "true" : "false"; }

bool SameSave(std::uint64_t load, std::uintptr_t session, std::int32_t date_raw, const StateSnapshot& s) {
    return s.load == load && s.session == session && s.date_raw >= date_raw;
}

// "<command> <subject> (pipe N, <date>): <what>": the Anvil log line of an activity. The date
// is the one of the save the call belongs to when known, else the one read now.
void LogLine(const Request& request, const std::string& subject, const StateSnapshot& saw, int level,
             const std::string& what) {
    const Handlers& h = request.handlers;
    if (!h.log) return;
    std::string date = "no game";
    const StateSnapshot s = saw.in_game ? saw : (h.read_state ? h.read_state() : saw);
    if (s.in_game) date = FormatGameDate(DecodeGameDate(s.date_raw));
    h.log(level, std::string(request.command.name) + (subject.empty() ? "" : " " + subject) + " (pipe " +
                     std::to_string(request.client) + ", " + date + "): " + what);
}

// Game calls log every refusal (ADR 0001), client calls their refusals (ADR 0003), inputs none.
int RefusalLevel(CommandType type, bool promoted) {
    if (type == CommandType::GameCall || promoted) return kLogWarning;
    return type == CommandType::ClientCall ? kLogInfo : 0;
}

std::string RefusalText(const Refusal& r) { return "refused: " + r.error + " (" + r.detail + ")"; }

}  // namespace

std::shared_ptr<ActivityGate> MakeActivityGate() { return std::make_shared<ActivityGate>(); }

std::string ModeMembers(const GameCallPolicy& policy, const ConsoleRaw& raw) {
    if ((!policy.ironman && !policy.multiplayer) || raw.session == 0) return {};
    return std::string(",\"mode\":{\"ironman\":") + Bool(raw.ironman) + ",\"multiplayer\":" + Bool(raw.multiplayer) +
           ",\"game_calls_ironman\":" + Bool(policy.ironman) + ",\"game_calls_multiplayer\":" + Bool(policy.multiplayer) + "}";
}

Refusal ActivityGate::Switch(const Handlers& h, CommandType type) {
    switch (type) {
        case CommandType::Read: return {};
        case CommandType::Input:
        case CommandType::ClientCall:
            if (h.send_input && h.read_state && h.sleep_ms && h.now_ms) return {};
            return {"input_disabled", kInputDisabled};
        case CommandType::GameCall:
            if (h.game_calls.enabled) return {};
            return {"game_calls_disabled", kGameCallsDisabled};
    }
    return {};
}

Refusal ActivityGate::Suspension(const Handlers& h) {
    if (h.suspended && h.suspended()) return {"suspended", kSuspended};
    return {};
}

Refusal ActivityGate::TrippedRefusal(Breaker breaker) {
    switch (breaker) {
        case Breaker::GameCalls: return {"game_calls_tripped", "game calls are off until the game exits after a game call faulted"};
        case Breaker::SpeedButtons:
            return {"speed_buttons_tripped", "the speed and pause buttons are off until the game exits after a press faulted"};
        case Breaker::Navigation: return {"navigation_tripped", "navigation is off until the game exits after a call faulted"};
        case Breaker::MapMode: return {"map_mode_tripped", "map_mode is off until the game exits after a call faulted"};
    }
    return {};
}

bool ActivityGate::Tripped(Breaker breaker) const { return tripped_[static_cast<std::size_t>(breaker)].load(); }

void ActivityGate::Trip(Breaker breaker) { tripped_[static_cast<std::size_t>(breaker)].store(true); }

ActivityGate::Counts ActivityGate::counts() const { return {inputs_.load(), client_calls_.load(), game_calls_.load()}; }

std::uint64_t ActivityGate::Total(CommandType type, const StateSnapshot& s) const {
    std::lock_guard lock(totals_mutex_);
    const Total_& total = type == CommandType::GameCall ? game_total_ : client_total_;
    if (type != CommandType::GameCall && type != CommandType::ClientCall) return 0;
    return SameSave(total.load, total.session, total.date_raw, s) ? total.count : 0;
}

std::uint64_t ActivityGate::Count(CommandType type) {
    switch (type) {
        case CommandType::Input: return inputs_.fetch_add(1) + 1;
        case CommandType::ClientCall: return client_calls_.fetch_add(1) + 1;
        case CommandType::GameCall: return game_calls_.fetch_add(1) + 1;
        case CommandType::Read: break;
    }
    return 0;
}

// A loaded save is told by the session the state reads saw (LoadWatch: a moment without a
// game in between), its session object, or a date earlier than the last call's.
std::uint64_t ActivityGate::AddTotal(CommandType type, const StateSnapshot& s) {
    std::lock_guard lock(totals_mutex_);
    Total_& total = type == CommandType::GameCall ? game_total_ : client_total_;
    if (!SameSave(total.load, total.session, total.date_raw, s)) total = {s.load, s.session, s.date_raw, 0};
    total.date_raw = s.date_raw;
    return ++total.count;
}

void ActivityGate::Publish(const Handlers& h, CommandType type, std::string_view command, int client,
                           std::string_view members, std::uint64_t count) const {
    if (!h.notifications || !h.notifications->Wants(NotificationKind::Activity)) return;
    std::optional<std::int32_t> date;
    if (h.read_state) {
        const StateSnapshot s = h.read_state();
        if (s.in_game) date = s.date_raw;
    }
    const bool input = type == CommandType::Input;
    const bool client_call = type == CommandType::ClientCall;
    h.notifications->Publish(NotificationKind::Activity, date,
                             std::string(",\"what\":\"") + CommandTypeName(type) + "\",\"command\":\"" + JsonEscape(command) +
                                 "\"" + std::string(members) + ",\"" +
                                 (input ? "inputs" : client_call ? "client_calls" : "game_calls") +
                                 "\":" + std::to_string(count) + ",\"client\":" + std::to_string(client));
}

void ActivityGate::Counted(const Handlers& h, CommandType type, std::string_view command, int client,
                           const StateSnapshot& save, std::string_view members) {
    const std::uint64_t count = Count(type);
    AddTotal(type, save);
    Publish(h, type, command, client, members, count);
}

std::string ActivityGate::Run(const Request& request, const Plan& plan) {
    const Handlers& h = request.handlers;
    const CommandType type = request.command.type;
    assert(type != CommandType::Read && plan.refuse && plan.act);
    // A read is no activity: a table entry of type read that reached the gate would run unchecked.
    if (type == CommandType::Read) return plan.refuse({"not_an_activity", "a read runs without the activity gate"});
    std::optional<Breaker> breaker;
    if (type == CommandType::GameCall) breaker = Breaker::GameCalls;
    if (type == CommandType::ClientCall) breaker = plan.breaker;
    const auto refuse = [&](const Refusal& r) {
        if (const int level = RefusalLevel(type, false); level != 0) LogLine(request, plan.subject, {}, level, RefusalText(r));
        return plan.refuse(r);
    };
    if (plan.first) {
        if (const Refusal r = plan.first(); r.refused()) return refuse(r);
    }
    if (const Refusal r = Switch(h, type); r.refused()) return refuse(r);
    if (const Refusal r = Suspension(h); r.refused()) return refuse(r);
    if (plan.check) {
        if (const Refusal r = plan.check(); r.refused()) return refuse(r);
    }
    if (breaker && Tripped(*breaker)) return refuse(TrippedRefusal(*breaker));
    Activity activity(*this, request, plan, breaker);
    return plan.act(activity);
}

ActivityGate::Part::Part(ActivityGate& gate, const Handlers& h, std::string command, int client)
    : gate_(gate), handlers_(h), command_(std::move(command)), client_(client) {
    if (h.send_input) {
        handlers_.send_input = [this, send = h.send_input, window = h.read_window](const std::vector<InputEvent>& events) {
            if (window) {
                const WindowSnapshot w = window();
                if (w.found && w.minimized) return std::string(kWindowMinimized);
            }
            std::string error = send(events);
            if (error.empty()) sent_ = true;
            return error;
        };
    }
    if (h.press_speed_button) {
        handlers_.press_speed_button = [this, press = h.press_speed_button](SpeedButton button) {
            SpeedButtonPress p = press(button);
            if (p.pressed) sent_ = true;
            return p;
        };
    }
    // A unit command the service posted counts as the button press it stands for.
    if (h.post_units) {
        handlers_.post_units = [this, post = h.post_units](const UnitRequest& request) {
            UnitPost p = post(request);
            if (!p.failure.failed() && p.posted != 0) sent_ = true;
            return p;
        };
    }
    // A choice the service made counts as the click it stands for.
    if (h.choose_option) {
        handlers_.choose_option = [this, choose = h.choose_option](std::int32_t pending, std::int32_t option) {
            ServiceChoice c = choose(pending, option);
            if (c.posted != 0) sent_ = true;
            return c;
        };
    }
}

ActivityGate::Part::~Part() {
    if (!sent_) return;
    gate_.Publish(handlers_, CommandType::Input, command_, client_, {}, gate_.Count(CommandType::Input));
}

ActivityGate::Activity::Activity(ActivityGate& gate, const Request& request, const Plan& plan,
                                 std::optional<Breaker> breaker)
    : gate_(gate), request_(request), plan_(plan), breaker_(breaker), turn_(gate.turn_) {
    if (request.command.type == CommandType::Input) {
        input_.emplace(gate, request.handlers, std::string(request.command.name), request.client);
    }
}

const Handlers& ActivityGate::Activity::handlers() const { return input_ ? input_->handlers() : request_.handlers; }

ActivityGate::Part ActivityGate::Activity::CountAs(std::string command) const {
    return Part(gate_, request_.handlers, std::move(command), request_.client);
}

void ActivityGate::Activity::Release() { turn_.unlock(); }

void ActivityGate::Activity::Take() { turn_.lock(); }

bool ActivityGate::Activity::Tripped() const { return breaker_ && gate_.Tripped(*breaker_); }

void ActivityGate::Activity::Trip() {
    if (breaker_) gate_.Trip(*breaker_);
}

Refusal ActivityGate::Activity::Promote(std::string detail, std::string why) {
    promoted_ = true;
    why_ = std::move(why);
    const Handlers& h = request_.handlers;
    if (!h.game_calls.enabled) return {"needs_game_calls", std::move(detail)};
    if (gate_.Tripped(Breaker::GameCalls)) return TrippedRefusal(Breaker::GameCalls);
    if (h.read_console) {
        ConsoleRaw raw;
        if (std::string error = h.read_console(raw); !error.empty()) return {"console_unavailable", error};
        mode_ = ModeMembers(h.game_calls, raw);
        if (ConsoleRefusal r = CheckConsoleMode(h.game_calls, raw); r.refused()) return {r.error, r.detail};
    }
    return {};
}

void ActivityGate::Activity::Called(std::string_view members) {
    assert(request_.command.type != CommandType::Input);
    const CommandType type = promoted_ ? CommandType::GameCall : request_.command.type;
    gate_.Publish(request_.handlers, type, request_.command.name, request_.client, members, gate_.Count(type));
    if (promoted_) Log(kLogWarning, "made as a game call: " + why_);
}

std::uint64_t ActivityGate::Activity::Total(const StateSnapshot& save) {
    if (totalled_) return total_;
    totalled_ = true;
    total_ = gate_.AddTotal(promoted_ ? CommandType::GameCall : request_.command.type, save);
    if (save.in_game) saw_ = save;
    return total_;
}

void ActivityGate::Activity::Refused(const Refusal& refusal) const {
    if (const int level = RefusalLevel(request_.command.type, promoted_); level != 0) Log(level, RefusalText(refusal));
}

void ActivityGate::Activity::Log(int level, const std::string& what) const {
    LogLine(request_, plan_.subject, saw_, level, what);
}

}  // namespace tongs
