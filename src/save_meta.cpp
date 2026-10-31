#include "save_meta.hpp"

#include "game_date.hpp"

#include <charconv>
#include <cstring>

namespace tongs {
namespace {

constexpr std::size_t kMaxMeta = 16u << 20;  // a zip member `meta` larger than this is taken as garbage

char Lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

bool EndsWithEu4(std::string_view s) {
    if (s.size() < 4) return false;
    const std::string_view tail = s.substr(s.size() - 4);
    return tail[0] == '.' && Lower(tail[1]) == 'e' && Lower(tail[2]) == 'u' && tail[3] == '4';
}

// Every code point printable ASCII or U+00A0..U+00FF (UTF-8 from the pipe).
bool PrintableLatin1(std::string_view s) {
    for (std::size_t i = 0; i < s.size(); ++i) {
        const auto c = static_cast<unsigned char>(s[i]);
        if (c >= 0x20 && c < 0x7F) continue;
        if ((c == 0xC2 || c == 0xC3) && i + 1 < s.size()) {
            const auto next = static_cast<unsigned char>(s[i + 1]);
            if ((next & 0xC0) != 0x80 || (c == 0xC2 && next < 0xA0)) return false;
            ++i;
            continue;
        }
        return false;
    }
    return true;
}

std::uint16_t U16(std::string_view s, std::size_t at) {
    return static_cast<std::uint16_t>(static_cast<unsigned char>(s[at]) | static_cast<unsigned char>(s[at + 1]) << 8);
}

std::uint32_t U32(std::string_view s, std::size_t at) {
    return static_cast<std::uint32_t>(U16(s, at)) | static_cast<std::uint32_t>(U16(s, at + 2)) << 16;
}

// --- DEFLATE -----------------------------------------------------------------

struct Bits {
    std::string_view in;
    std::size_t pos = 0;
    std::uint32_t buffer = 0;
    int count = 0;
    bool error = false;

    int Need(int n) {
        while (count < n) {
            if (pos >= in.size()) {
                error = true;
                return 0;
            }
            buffer |= static_cast<std::uint32_t>(static_cast<unsigned char>(in[pos++])) << count;
            count += 8;
        }
        const int value = static_cast<int>(buffer & ((1u << n) - 1));
        buffer >>= n;
        count -= n;
        return value;
    }
};

// A canonical Huffman code: counts per length and the symbols in code order.
struct Huffman {
    short count[16] = {};
    short symbol[288] = {};

    bool Build(const short* lengths, int n) {
        std::memset(count, 0, sizeof count);
        for (int i = 0; i < n; ++i) ++count[lengths[i]];
        if (count[0] == n) return true;  // no codes: only an error if used
        int left = 1;
        for (int len = 1; len < 16; ++len) {
            left <<= 1;
            left -= count[len];
            if (left < 0) return false;
        }
        short offs[16] = {};
        for (int len = 1; len < 15; ++len) offs[len + 1] = static_cast<short>(offs[len] + count[len]);
        for (int i = 0; i < n; ++i) {
            if (lengths[i] != 0) symbol[offs[lengths[i]]++] = static_cast<short>(i);
        }
        return true;
    }

    int Decode(Bits& b) const {
        int code = 0, first = 0, index = 0;
        for (int len = 1; len < 16; ++len) {
            code |= b.Need(1);
            if (b.error) return -1;
            const int c = count[len];
            if (code - c < first) return symbol[index + (code - first)];
            index += c;
            first += c;
            first <<= 1;
            code <<= 1;
        }
        return -1;
    }
};

constexpr short kLengthBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                   35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr short kLengthExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr short kDistBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
                                 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
constexpr short kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

bool Codes(Bits& b, std::string& out, std::size_t max, const Huffman& lit, const Huffman& dist) {
    for (;;) {
        int sym = lit.Decode(b);
        if (sym < 0) return false;
        if (sym < 256) {
            if (out.size() >= max) return false;
            out.push_back(static_cast<char>(sym));
        } else if (sym == 256) {
            return true;
        } else {
            sym -= 257;
            if (sym >= 29) return false;
            const int len = kLengthBase[sym] + b.Need(kLengthExtra[sym]);
            const int d = dist.Decode(b);
            if (d < 0 || d >= 30 || b.error) return false;
            const std::size_t back = static_cast<std::size_t>(kDistBase[d] + b.Need(kDistExtra[d]));
            if (b.error || back > out.size() || out.size() + static_cast<std::size_t>(len) > max) return false;
            for (int i = 0; i < len; ++i) out.push_back(out[out.size() - back]);
        }
    }
}

bool Fixed(Bits& b, std::string& out, std::size_t max) {
    static Huffman lit, dist;
    static bool built = false;
    if (!built) {
        short lengths[288];
        int i = 0;
        for (; i < 144; ++i) lengths[i] = 8;
        for (; i < 256; ++i) lengths[i] = 9;
        for (; i < 280; ++i) lengths[i] = 7;
        for (; i < 288; ++i) lengths[i] = 8;
        lit.Build(lengths, 288);
        for (i = 0; i < 30; ++i) lengths[i] = 5;
        dist.Build(lengths, 30);
        built = true;
    }
    return Codes(b, out, max, lit, dist);
}

bool Dynamic(Bits& b, std::string& out, std::size_t max) {
    static constexpr short kOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    const int nlen = b.Need(5) + 257;
    const int ndist = b.Need(5) + 1;
    const int ncode = b.Need(4) + 4;
    if (b.error || nlen > 286 || ndist > 30) return false;
    short lengths[320] = {};
    for (int i = 0; i < ncode; ++i) lengths[kOrder[i]] = static_cast<short>(b.Need(3));
    Huffman code;
    if (b.error || !code.Build(lengths, 19)) return false;
    int index = 0;
    while (index < nlen + ndist) {
        int sym = code.Decode(b);
        if (sym < 0) return false;
        if (sym < 16) {
            lengths[index++] = static_cast<short>(sym);
            continue;
        }
        short len = 0;
        int repeat = 0;
        if (sym == 16) {
            if (index == 0) return false;
            len = lengths[index - 1];
            repeat = 3 + b.Need(2);
        } else if (sym == 17) {
            repeat = 3 + b.Need(3);
        } else {
            repeat = 11 + b.Need(7);
        }
        if (b.error || index + repeat > nlen + ndist) return false;
        while (repeat--) lengths[index++] = len;
    }
    if (lengths[256] == 0) return false;
    Huffman lit, dist;
    if (!lit.Build(lengths, nlen) || !dist.Build(lengths + nlen, ndist)) return false;
    return Codes(b, out, max, lit, dist);
}

// --- zip ---------------------------------------------------------------------

// The bytes of the member `name`, inflated; an error text, or empty.
std::string ZipMember(std::string_view zip, std::string_view name, std::string& out) {
    constexpr std::size_t kEnd = 22;
    if (zip.size() < kEnd) return "the zip is too short";
    std::size_t end = std::string_view::npos;
    const std::size_t lowest = zip.size() > kEnd + 0xFFFF ? zip.size() - kEnd - 0xFFFF : 0;
    for (std::size_t at = zip.size() - kEnd + 1; at-- > lowest;) {
        if (U32(zip, at) == 0x06054B50) {
            end = at;
            break;
        }
    }
    if (end == std::string_view::npos) return "no zip end record";
    const std::uint16_t entries = U16(zip, end + 10);
    std::size_t at = U32(zip, end + 16);
    for (std::uint16_t i = 0; i < entries; ++i) {
        if (at + 46 > zip.size() || U32(zip, at) != 0x02014B50) return "a bad zip directory";
        const std::uint16_t method = U16(zip, at + 10);
        const std::uint32_t packed = U32(zip, at + 20);
        const std::uint32_t size = U32(zip, at + 24);
        const std::uint16_t name_len = U16(zip, at + 28);
        const std::uint16_t extra_len = U16(zip, at + 30);
        const std::uint16_t comment_len = U16(zip, at + 32);
        const std::uint32_t local = U32(zip, at + 42);
        if (at + 46 + name_len > zip.size()) return "a bad zip directory";
        if (zip.substr(at + 46, name_len) == name) {
            if (local + 30 > zip.size() || U32(zip, local) != 0x04034B50) return "a bad zip entry";
            const std::size_t data = local + 30 + U16(zip, local + 26) + U16(zip, local + 28);
            if (data + packed > zip.size() || size > kMaxMeta) return "a bad zip entry";
            const std::string_view raw = zip.substr(data, packed);
            if (method == 0) {
                out.assign(raw);
                return {};
            }
            if (method != 8) return "an unknown zip compression method " + std::to_string(method);
            out.reserve(size);
            if (!Inflate(raw, out, size)) return "the zip member " + std::string(name) + " does not inflate";
            return {};
        }
        at += 46 + name_len + extra_len + comment_len;
    }
    return "the zip has no " + std::string(name);
}

// --- text metadata -----------------------------------------------------------

std::string_view Unquote(std::string_view v) {
    if (v.size() >= 2 && v.front() == '"' && v.back() == '"') return v.substr(1, v.size() - 2);
    return v;
}

std::string_view TrimSpace(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

std::string ReadText(std::string_view text, SaveMeta& out) {
    bool date = false, player = false, versions = false;
    int depth = 0;
    std::size_t pos = 0;
    while (pos < text.size()) {
        std::size_t eol = text.find('\n', pos);
        if (eol == std::string_view::npos) eol = text.size();
        const std::string_view line = TrimSpace(text.substr(pos, eol - pos));
        pos = eol + 1;
        if (depth == 0) {
            const std::size_t eq = line.find('=');
            const std::string_view key = eq == std::string_view::npos ? std::string_view{} : TrimSpace(line.substr(0, eq));
            const std::string_view value = eq == std::string_view::npos ? std::string_view{} : TrimSpace(line.substr(eq + 1));
            if (key == "date" && !date) {
                const auto raw = ParseGameDate(value);
                if (!raw) return "a bad date in the metadata: " + std::string(value.substr(0, 32));
                out.date_raw = *raw;
                date = true;
            } else if (key == "player" && !player) {
                out.player = std::string(Unquote(value));
                player = true;
            } else if (key == "ironman") {
                out.ironman = value == "yes";
            } else if (key == "savegame_versions" && !versions) {
                // savegame_versions={ then "1.37.5.0" lines until }
                versions = true;
                std::size_t close = text.find('}', pos);
                if (close == std::string_view::npos) close = text.size();
                std::string_view body = text.substr(pos, close - pos);
                for (std::size_t q = body.find('"'); q != std::string_view::npos; q = body.find('"', q + 1)) {
                    const std::size_t r = body.find('"', q + 1);
                    if (r == std::string_view::npos) break;
                    out.versions.emplace_back(body.substr(q + 1, r - q - 1));
                    q = r;
                }
                pos = close + 1;
                continue;
            }
        }
        for (const char c : line) {
            if (c == '{') ++depth;
            if (c == '}' && depth > 0) --depth;
        }
    }
    if (!date) return "the metadata has no date";
    if (!player) return "the metadata has no player";
    return {};
}

// --- binary metadata ---------------------------------------------------------

constexpr std::uint16_t kEquals = 0x0001;
constexpr std::uint16_t kOpen = 0x0003;
constexpr std::uint16_t kClose = 0x0004;
constexpr std::uint16_t kInt = 0x000C;
constexpr std::uint16_t kFloat = 0x000D;
constexpr std::uint16_t kBool = 0x000E;
constexpr std::uint16_t kString = 0x000F;
constexpr std::uint16_t kUint = 0x0014;
constexpr std::uint16_t kUnquoted = 0x0017;
constexpr std::uint16_t kFixed = 0x0167;
constexpr std::uint16_t kFixed64 = 0x0190;
constexpr std::uint16_t kUint64 = 0x029C;

// Bytes of payload after a value token; -1 for a string (length prefixed), 0 for a key.
int Payload(std::uint16_t token) {
    switch (token) {
        case kInt: case kFloat: case kUint: case kFixed: return 4;
        case kBool: return 1;
        case kFixed64: case kUint64: return 8;
        case kString: case kUnquoted: return -1;
        default: return 0;
    }
}

std::string ReadBinary(std::string_view b, const SaveTokens& t, SaveMeta& out) {
    bool date = false, player = false;
    int depth = 0;
    std::size_t pos = 0;
    // One token with its payload; false at the end or on a short read.
    const auto next = [&b, &pos](std::uint16_t& token, std::string_view& payload) {
        if (pos + 2 > b.size()) return false;
        token = U16(b, pos);
        pos += 2;
        const int n = Payload(token);
        std::size_t size = n > 0 ? static_cast<std::size_t>(n) : 0;
        if (n < 0) {
            if (pos + 2 > b.size()) return false;
            size = U16(b, pos);
            pos += 2;
        }
        if (pos + size > b.size()) return false;
        payload = b.substr(pos, size);
        pos += size;
        return true;
    };
    std::uint16_t token = 0;
    std::string_view payload;
    while (next(token, payload)) {
        if (token == kOpen) {
            ++depth;
            continue;
        }
        if (token == kClose) {
            if (depth > 0) --depth;
            continue;
        }
        if (depth != 0 || Payload(token) != 0 || token == kEquals) continue;
        const std::uint16_t key = token;
        if (key != t.date && key != t.player && key != t.ironman && key != t.ironman_alt && key != t.versions) continue;
        if (!next(token, payload) || token != kEquals || !next(token, payload)) break;
        if (key == t.date && token == kInt && !date) {
            out.date_raw = static_cast<std::int32_t>(U32(payload, 0));
            date = true;
        } else if (key == t.player && (token == kString || token == kUnquoted) && !player) {
            out.player = std::string(payload);
            player = true;
        } else if ((key == t.ironman || key == t.ironman_alt) && token == kBool) {
            out.ironman = out.ironman || payload[0] != 0;
        } else if (key == t.versions && token == kOpen) {
            while (next(token, payload) && token != kClose) {
                if (token == kString || token == kUnquoted) out.versions.emplace_back(payload);
            }
        } else if (token == kOpen) {
            ++depth;
        }
    }
    if (!date) return "the metadata has no date";
    if (!player) return "the metadata has no player";
    return {};
}

std::string ReadMetaBytes(std::string_view bytes, const SaveTokens& tokens, SaveMeta& out, const char* prefix) {
    if (bytes.substr(0, 6) == "EU4txt") {
        out.format = std::string(prefix) + "text";
        return ReadText(bytes.substr(6), out);
    }
    if (bytes.substr(0, 6) == "EU4bin") {
        out.format = std::string(prefix) + "binary";
        return ReadBinary(bytes.substr(6), tokens, out);
    }
    return "not an EU4 save (no EU4txt or EU4bin header)";
}

}  // namespace

std::string ParseSaveName(std::string_view argument, std::string& name, std::string_view& rest) {
    name.clear();
    rest = {};
    while (!argument.empty() && (argument.front() == ' ' || argument.front() == '\t')) argument.remove_prefix(1);
    std::string_view raw;
    if (!argument.empty() && argument.front() == '"') {
        const std::size_t close = argument.find('"', 1);
        if (close == std::string_view::npos) return "the name has no closing quote";
        raw = argument.substr(1, close - 1);
        rest = argument.substr(close + 1);
    } else {
        const std::size_t end = argument.find_first_of(" \t");
        raw = argument.substr(0, end);
        rest = end == std::string_view::npos ? std::string_view{} : argument.substr(end);
    }
    if (EndsWithEu4(raw)) raw.remove_suffix(4);
    if (raw.empty()) return "an empty name";
    if (raw.find_first_of("/\\") != std::string_view::npos) return "a name with a folder (only top-level saves)";
    if (raw.find("..") != std::string_view::npos) return "a name with ..";
    if (!PrintableLatin1(raw)) return "a name with characters outside printable ASCII and Latin-1";
    name = std::string(raw);
    return {};
}

bool Inflate(std::string_view in, std::string& out, std::size_t max) {
    Bits b{in};
    for (;;) {
        const int last = b.Need(1);
        const int type = b.Need(2);
        if (b.error) return false;
        bool ok = false;
        if (type == 0) {
            b.buffer = 0;
            b.count = 0;
            if (b.pos + 4 > in.size()) return false;
            const std::uint16_t len = U16(in, b.pos);
            const std::uint16_t nlen = U16(in, b.pos + 2);
            b.pos += 4;
            if (static_cast<std::uint16_t>(~nlen) != len || b.pos + len > in.size() || out.size() + len > max) return false;
            out.append(in.substr(b.pos, len));
            b.pos += len;
            ok = true;
        } else if (type == 1) {
            ok = Fixed(b, out, max);
        } else if (type == 2) {
            ok = Dynamic(b, out, max);
        }
        if (!ok) return false;
        if (last) return true;
    }
}

std::string ReadSaveMeta(std::string_view file, const SaveTokens& tokens, SaveMeta& out) {
    out = {};
    if (file.substr(0, 2) == "PK") {
        std::string meta;
        if (std::string error = ZipMember(file, "meta", meta); !error.empty()) return error;
        return ReadMetaBytes(meta, tokens, out, "compressed_");
    }
    return ReadMetaBytes(file, tokens, out, "");
}

bool VersionListed(const std::vector<std::string>& versions, const std::array<int, 4>& running) {
    for (const std::string& v : versions) {
        std::array<int, 4> parts{};
        std::string_view rest = v;
        bool ok = true;
        for (int i = 0; i < 4 && ok; ++i) {
            const std::size_t dot = rest.find('.');
            const std::string_view part = rest.substr(0, dot);
            const auto [ptr, ec] = std::from_chars(part.data(), part.data() + part.size(), parts[static_cast<std::size_t>(i)]);
            ok = ec == std::errc{} && ptr == part.data() + part.size() && (i == 3 ? dot == std::string_view::npos : dot != std::string_view::npos);
            rest = dot == std::string_view::npos ? std::string_view{} : rest.substr(dot + 1);
        }
        if (ok && parts == running) return true;
    }
    return false;
}

}  // namespace tongs
