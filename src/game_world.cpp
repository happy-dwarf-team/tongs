#include "game_world.hpp"

#include "game_memory.hpp"

namespace tongs {
namespace {

constexpr std::size_t kMaxCores = 1024;       // a longer core vector is taken as garbage
constexpr std::int32_t kMaxBucketTags = 4096;  // likewise a bucket

WorldFailure Failed(std::string detail) { return {"read_failed", std::move(detail)}; }

std::uint8_t ByteOf(std::uint64_t tag, std::uint32_t index) { return static_cast<std::uint8_t>(tag >> (8 * index)); }

// The tag's characters, or empty when its valid byte says no country.
std::string TagText(std::uint64_t tag, const WorldLayout& l) {
    if (ByteOf(tag, l.tag_valid) == 0) return {};
    std::string text;
    for (std::uint32_t i = 0; i < 3 && ByteOf(tag, i) != 0; ++i) text += static_cast<char>(ByteOf(tag, i));
    return text;
}

char Upper(char c) { return c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c; }

// The first `count` characters of the stored tag against `text`, ignoring case, as _strnicmp.
bool SameTag(std::uint64_t tag, std::string_view text, std::uint32_t count) {
    for (std::uint32_t i = 0; i < count; ++i) {
        const char stored = static_cast<char>(ByteOf(tag, i));
        const char wanted = i < text.size() ? text[i] : '\0';
        if (Upper(stored) != Upper(wanted)) return false;
        if (stored == '\0') return true;
    }
    return true;
}

bool ReadRoot(std::uintptr_t base, const WorldLayout& l, std::uintptr_t& root) {
    return Read(base + l.root_rva, root) && root != 0;
}

// Entries in the root's province vector (4942 in vanilla, the dummy entry 0 included).
bool ProvinceCount(std::uintptr_t root, const WorldLayout& l, std::int64_t& count) {
    std::uintptr_t begin = 0, end = 0;
    if (!Read(root + l.provinces, begin) || !Read(root + l.provinces_end, end) || end < begin) return false;
    count = static_cast<std::int64_t>((end - begin) >> l.pointer_shift);
    return true;
}

}  // namespace

WorldFailure ReadProvince(std::uintptr_t base, const WorldLayout& l, std::int32_t id, ProvinceRaw& out) {
    out = {};
    std::uintptr_t root = 0;
    if (!ReadRoot(base, l, root)) return {"not_in_game", "no game loaded (root object is null)"};
    std::int64_t count = 0;
    if (!ProvinceCount(root, l, count)) return Failed("the province vector is not readable");
    if (id < 1 || id >= count) {
        return {"unknown_province",
                "no province " + std::to_string(id) + " (valid IDs 1.." + std::to_string(count - 1) + ")"};
    }
    std::uintptr_t array = 0;
    if (!Read(root + l.province_array, array) || array == 0) return Failed("the province array is not readable");
    const std::uintptr_t p = array + static_cast<std::uintptr_t>(id) * l.province_size;
    std::int32_t own_id = 0;
    if (!Read(p + l.province_id, own_id)) return Failed("province " + std::to_string(id) + " is not readable");
    if (own_id != id) {
        return Failed("the entry of province " + std::to_string(id) + " holds ID " + std::to_string(own_id));
    }
    out.id = id;
    std::uint64_t owner = 0, controller = 0;
    std::uintptr_t cores = 0, cores_end = 0, map_part = 0;
    if (!Read(p + l.owner, owner) || !Read(p + l.controller, controller) || !Read(p + l.cores, cores) ||
        !Read(p + l.cores + l.cores_end, cores_end) || !Read(p + l.base_tax, out.base_tax) ||
        !Read(p + l.base_production, out.base_production) || !Read(p + l.base_manpower, out.base_manpower) ||
        !Read(p + l.static_part, map_part) || map_part == 0) {
        return Failed("province " + std::to_string(id) + " is not readable");
    }
    out.owner = TagText(owner, l);
    out.controller = TagText(controller, l);
    if (cores_end < cores || (cores_end - cores) % 8 != 0 || (cores_end - cores) / 8 > kMaxCores) {
        return Failed("the cores of province " + std::to_string(id) + " are not a vector of tags");
    }
    for (std::uintptr_t at = cores; at < cores_end; at += 8) {
        std::uint64_t core = 0;
        if (!Read(at, core)) return Failed("the cores of province " + std::to_string(id) + " are not readable");
        out.cores.push_back(TagText(core, l));
    }
    std::uint8_t land = 0, wasteland = 0;
    std::string name;
    if (!Read(map_part + l.land_flags, land) || !Read(map_part + l.wasteland_flags, wasteland) ||
        !ReadString(map_part + l.name, name)) {
        return Failed("the map part of province " + std::to_string(id) + " is not readable");
    }
    out.land = ((land >> l.land_bit) & 1) != 0;
    out.wasteland = ((wasteland >> l.wasteland_bit) & 1) != 0;
    out.name = GameTextToUtf8(name);
    return {};
}

WorldFailure ReadCountry(std::uintptr_t base, const WorldLayout& l, std::string_view text, CountryRaw& out) {
    out = {};
    std::uintptr_t root = 0, db = 0;
    if (!ReadRoot(base, l, root)) return {"not_in_game", "no game loaded (root object is null)"};
    if (!Read(base + l.countries_db_rva, db) || db == 0) return Failed("the country database is not set up");
    // The game's own lookup (0x286120), walked here instead of called.
    const std::uintptr_t bucket = db + static_cast<std::uintptr_t>(TagBucket(text, static_cast<int>(l.bucket_mask))) * l.bucket_stride;
    std::uintptr_t tags = 0;
    std::int32_t n = 0;
    if (!Read(bucket + l.buckets, tags) || !Read(bucket + l.bucket_count, n) || n < 0 || n > kMaxBucketTags) {
        return Failed("the tag buckets are not readable");
    }
    std::uint64_t tag = 0;
    bool found = false;
    for (std::int32_t i = 0; i < n && !found; ++i) {
        if (!Read(tags + static_cast<std::uintptr_t>(i) * 8, tag)) return Failed("the tag buckets are not readable");
        found = SameTag(tag, text, l.compared);
    }
    if (!found) return {"unknown_country", "the game knows no country " + std::string(text)};
    const auto index = static_cast<std::int16_t>(tag >> l.country_index_shift);
    std::uintptr_t countries = 0, c = 0;
    if (index < 0 || !Read(db + l.countries, countries) || countries == 0 ||
        !Read(countries + static_cast<std::uintptr_t>(index) * 8, c) || c == 0) {
        return Failed("country " + std::string(text) + " (index " + std::to_string(index) + ") is not readable");
    }
    std::uint64_t own = 0;
    if (!Read(c + l.country_tag, own)) return Failed("country " + std::string(text) + " is not readable");
    if (!SameTag(own, text, 3)) return Failed("country " + std::string(text) + " holds the tag " + TagText(own, l));
    std::int64_t count = 0;
    std::uintptr_t owned = 0;
    if (!ProvinceCount(root, l, count) || !Read(c + l.owned_count, out.province_count) || !Read(c + l.owned, owned) ||
        !Read(c + l.capital, out.capital)) {
        return Failed("country " + std::string(text) + " is not readable");
    }
    if (out.province_count < 0 || out.province_count > count || (out.province_count > 0 && owned == 0)) {
        return Failed("country " + std::string(text) + " has " + std::to_string(out.province_count) + " provinces");
    }
    out.provinces.resize(static_cast<std::size_t>(out.province_count));
    if (out.province_count > 0 && !ReadRaw(owned, out.provinces.data(), out.provinces.size() * sizeof(std::int32_t))) {
        return Failed("the provinces of " + std::string(text) + " are not readable");
    }
    out.tag = TagText(own, l);
    out.observer = ByteOf(own, l.tag_kind) == l.observer_kind;
    out.capital_valid = out.capital >= 1 && out.capital < count;
    return {};
}

WorldFailure ReadPlayer(std::uintptr_t base, const WorldLayout& l, PlayerRaw& out) {
    out = {};
    std::uintptr_t root = 0;
    if (!ReadRoot(base, l, root)) return {"not_in_game", "no game loaded (root object is null)"};
    std::uint64_t player = 0, observed = 0;
    if (!Read(root + l.player, player) || !Read(root + l.observed, observed)) return Failed("the player's tags are not readable");
    out.player_tag = TagText(player, l);
    out.observed_tag = TagText(observed, l);
    out.observer = ByteOf(player, l.tag_kind) == l.observer_kind;
    // The rule of every reader in the game (player_country).
    out.tag = out.observer && ByteOf(observed, l.tag_valid) != 0 ? out.observed_tag : out.player_tag;
    return {};
}

WorldFailure ReadSelection(std::uintptr_t base, const WorldLayout& l, SelectionRaw& out) {
    out = {};
    std::uintptr_t root = 0, session = 0, holder = 0;
    if (!ReadRoot(base, l, root)) return {"not_in_game", "no game loaded (root object is null)"};
    if (!Read(root + l.session, session) || session == 0 || !Read(session + l.holder, holder)) {
        return Failed("the session is not readable");
    }
    if (holder == 0) return {};
    std::uintptr_t selected = 0, view = 0, shown = 0, window = 0;
    if (!Read(holder + l.selected, selected) || !Read(holder + l.view, view)) return Failed("the selection is not readable");
    if (selected != 0 && !Read(selected + l.province_id, out.last_selected)) {
        return Failed("the selected province is not readable");
    }
    if (view != 0) {
        if (!Read(view + l.view_province, shown) || !Read(view + l.view_window, window)) {
            return Failed("the province view is not readable");
        }
        if (shown != 0 && !Read(shown + l.province_id, out.view_province)) {
            return Failed("the province the view shows is not readable");
        }
        if (window != 0) out.view_widget = window + l.window_part;
    }
    return {};
}

}  // namespace tongs
