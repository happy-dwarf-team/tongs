// anvil-game.commands as far as Tongs uses it: version 2's event option and
// version 1's unit commands, and anvil-game.world's units
// (include/anvil/anvil_game.h of Anvil 1.17; Tongs builds against an SDK that
// predates them, so the structs are copied here). Optional: without the service,
// run_until events=first clicks and the unit commands answer unavailable.
#pragma once

#include "protocol.hpp"
#include "units.hpp"

#include <cstdint>

struct AnvilPlugin;

namespace tongs {

struct GameUnitRef {  // AnvilGameUnitRef: no size, never grows within version 1
    std::uint64_t id;
    char country[4];
    std::uint32_t kind;
};
static_assert(sizeof(GameUnitRef) == 16);

struct GameCommand {
    std::uint32_t size;
    std::uint32_t type;
    std::uint32_t flags;
    std::uint32_t count;
    const GameUnitRef* units;
    std::int32_t pending_event_id;
    std::int32_t option_index;
};

struct GameCommandResult {
    std::uint32_t size;
    std::uint32_t posted;
    std::uint32_t skipped;
    std::uint32_t refused;
};

struct GameCommandsService {
    std::uint32_t size;
    int (*post)(AnvilPlugin* self, const GameCommand* command, GameCommandResult* out);
};

constexpr const char* kGameCommandsService = "anvil-game.commands";
constexpr std::uint32_t kGameCommandsVersion = 2;  // the first with the event option
constexpr std::uint32_t kGameCommandsVersionUnits = 1;  // the unit commands (Anvil 1.17)
constexpr std::uint32_t kEventOptionCommand = 4;  // ANVIL_GAME_COMMAND_EVENT_OPTION
constexpr std::uint32_t kDrillCommand = 1;        // ANVIL_GAME_COMMAND_DRILL
constexpr std::uint32_t kMergeCommand = 2;        // ANVIL_GAME_COMMAND_MERGE
constexpr std::uint32_t kSplitCommand = 3;        // ANVIL_GAME_COMMAND_SPLIT
constexpr std::uint32_t kCommandStart = 1;        // ANVIL_GAME_COMMAND_START

struct GameTag {
    char text[4];
};

struct GameUnit {  // AnvilGameUnit
    std::uint32_t size;
    std::uint32_t kind;
    std::uint64_t id;
    GameTag country;
    std::int32_t location;
    std::int32_t mission;
    std::uint32_t subunits;
    std::int32_t strength;
    std::uint32_t reserved;
    std::uint64_t leader;
};

struct WorldDate {  // AnvilGameDate
    std::uint32_t size;
    std::int32_t year;
    std::uint32_t month;
    std::uint32_t day;
    std::uint64_t session;
};

struct GameFlag {  // AnvilGameFlag (anvil-game 1.20)
    std::int32_t id;
    std::uint32_t reserved;
    WorldDate date;
};
static_assert(sizeof(GameFlag) == 32);

struct GamePendingEvent {  // AnvilGamePendingEvent (anvil-game 1.21)
    std::uint32_t size;
    std::int32_t pending_id;
    GameTag country;
    std::int32_t province;
    std::uint32_t options;
    std::uint32_t flags;  // 0x1: hidden
    WorldDate auto_select;
};
static_assert(sizeof(GamePendingEvent) == 48);

struct GameRelation {  // AnvilGameRelation (anvil-game 1.22)
    GameTag first;
    GameTag second;
    WorldDate start;  // size 0: none
    WorldDate end;
};
static_assert(sizeof(GameRelation) == 56);

struct GameDiplomacyStatus {  // AnvilGameDiplomacyStatus (anvil-game 1.22)
    std::uint32_t size;
    std::int32_t opinion;
    std::int32_t timed_sum;
    std::uint32_t trust;
    std::int32_t favors;  // x 1000
    std::uint32_t flags;  // 0x1: truce
};
static_assert(sizeof(GameDiplomacyStatus) == 24);

// AnvilGameWorldService up to country_subjects (anvil-game 1.22); the
// members Tongs does not call are kept as plain pointers so the offsets match.
// Older tables are shorter: HasUnits, HasFlags / HasVariables, HasPending and HasDiplomacy check `size`.
struct GameWorldService {
    std::uint32_t size;
    int (*player)(GameTag* out);
    void* province_limit;
    void* province;
    void* province_name;
    void* province_cores;
    void* country;
    void* country_provinces;
    void* war_ids;
    void* war;
    void* war_name;
    void* war_side;
    void* modifier_id;
    void* country_modifier;
    void* province_modifier;
    int (*country_units)(const char* tag, std::uint64_t* ids, std::uint32_t capacity, std::uint32_t* count);
    int (*unit)(const char* tag, std::uint64_t id, GameUnit* out);
    int (*unit_name)(const char* tag, std::uint64_t id, char* utf8, std::uint32_t capacity, std::uint32_t* length);
    void* unit_leader_name;
    int (*selected_units)(GameUnitRef* out, std::uint32_t capacity, std::uint32_t* count);
    // anvil-game 1.20
    void* monarch;
    void* monarch_name;
    void* monarch_dynasty;
    void* flag_id;
    int (*flag_name)(std::int32_t id, char* utf8, std::uint32_t capacity, std::uint32_t* length);
    int (*country_flags)(const char* tag, GameFlag* out, std::uint32_t capacity, std::uint32_t* count);
    int (*province_flags)(std::int32_t province, GameFlag* out, std::uint32_t capacity, std::uint32_t* count);
    void* monarch_flags;
    void* global_flags;
    int (*country_variable)(const char* tag, const char* name, std::int32_t* value);
    int (*province_variable)(std::int32_t province, const char* name, std::int32_t* value);
    int (*country_variable_names)(const char* tag, char* names, std::uint32_t capacity, std::uint32_t* length);
    int (*province_variable_names)(std::int32_t province, char* names, std::uint32_t capacity, std::uint32_t* length);
    // anvil-game 1.21
    int (*pending_events)(const char* tag, std::int32_t* ids, std::uint32_t capacity, std::uint32_t* count);
    int (*pending_event)(std::int32_t pending_id, GamePendingEvent* out);
    int (*pending_event_key)(std::int32_t pending_id, char* utf8, std::uint32_t capacity, std::uint32_t* length);
    int (*pending_event_title)(std::int32_t pending_id, char* utf8, std::uint32_t capacity, std::uint32_t* length);
    void* country_statistics;
    void* country_timed_modifiers;
    void* country_timed_modifier_keys;
    void* province_timed_modifiers;
    void* province_timed_modifier_keys;
    void* province_view;
    // anvil-game 1.22
    int (*country_relations)(const char* tag, std::uint32_t kind, GameRelation* out, std::uint32_t capacity,
                             std::uint32_t* count);
    int (*subject_type)(const char* overlord, const char* subject, char* utf8, std::uint32_t capacity, std::uint32_t* length);
    int (*diplomacy_status)(const char* from, const char* to, GameDiplomacyStatus* out);
    int (*country_rivals)(const char* tag, GameTag* out, std::uint32_t capacity, std::uint32_t* count);
    int (*country_overlord)(const char* tag, GameTag* out);
    int (*country_subjects)(const char* tag, GameTag* out, std::uint32_t capacity, std::uint32_t* count);
};

constexpr const char* kGameWorldService = "anvil-game.world";
constexpr std::uint32_t kGameWorldVersion = 1;

// True when the table has every member Tongs calls (anvil-game 1.15 and newer).
bool HasUnits(const GameWorldService* world);

// The armies and navies of `tag` (empty: the player's). Main thread, after an
// outer frame, in a session.
WorldFailure ReadUnits(const GameWorldService* world, std::string_view tag, UnitsRaw& out);

// Posts `request` through the commands service: the units are the player's, by id, or
// the player's selection. Main thread, after an outer frame, as the service requires.
UnitPost PostUnits(const GameCommandsService* commands, const GameWorldService* world, AnvilPlugin* self,
                   const UnitRequest& request);

// The service's answer for `option` of the pending event `pending`. Main thread,
// after an outer frame, as the service requires.
ServiceChoice ChooseEventOption(const GameCommandsService* service, AnvilPlugin* self, std::int32_t pending,
                                std::int32_t option);

}  // namespace tongs
