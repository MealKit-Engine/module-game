#include "mealkit/game/InputRecord.h"

#include "mealkit/core/Log.h"
#include "mealkit/platform/Paths.h"

#include <cstring>

namespace mealkit::game {
namespace {

constexpr char kMagic[8] = {'M', 'K', 'I', 'N', 'P', 'U', 'T', '1'};
constexpr u32 kVersion = 2; // 2: 틱마다 확정 명령
constexpr size_t kHeaderBytes = 8 + 4 + 4 + 8 + 4 + 4 + 8;

// 어느 CPU에서 써도 같은 바이트가 되도록 직접 리틀 엔디언으로 쓴다
void put(std::vector<u8>& out, u64 v, int bytes) {
    for (int i = 0; i < bytes; ++i) out.push_back(static_cast<u8>(v >> (8 * i)));
}
u64 get(const u8*& p, int bytes) {
    u64 v = 0;
    for (int i = 0; i < bytes; ++i) v |= static_cast<u64>(p[i]) << (8 * i);
    p += bytes;
    return v;
}

} // namespace

bool InputRecordWriter::open(const std::string& path, const MatchInfo& match, u64 contentHash) {
    close();
    if (match.playerCount == 0 || match.playerCount > net::kMaxPlayers) return false;
    m_file = openFile(path, "wb");
    if (!m_file) {
        MEALKIT_ERROR("Cannot write the input record %s", path.c_str());
        return false;
    }
    m_players = match.playerCount;
    std::vector<u8> header(kMagic, kMagic + 8);
    put(header, kVersion, 4);
    put(header, match.playerCount, 4);
    put(header, match.seed, 8);
    put(header, match.matchData, 4);
    put(header, 0, 4);
    put(header, contentHash, 8);
    std::fwrite(header.data(), 1, header.size(), m_file);
    return true;
}

void InputRecordWriter::write(const net::TickInputs& inputs, u64 hash) {
    if (!m_file) return;
    std::vector<u8> tick;
    tick.reserve(m_players * net::kInputBytes + 2 + inputs.commandBytes + 8);
    for (u32 p = 0; p < m_players; ++p)
        tick.insert(tick.end(), inputs.players[p].bytes, inputs.players[p].bytes + net::kInputBytes);
    put(tick, inputs.commandBytes, 2);
    tick.insert(tick.end(), inputs.commands, inputs.commands + inputs.commandBytes);
    put(tick, hash, 8);
    std::fwrite(tick.data(), 1, tick.size(), m_file);
}

void InputRecordWriter::close() {
    if (m_file) std::fclose(m_file);
    m_file = nullptr;
}

bool loadInputRecording(const std::string& path, InputRecording& out, std::string& error) {
    out = {};
    std::FILE* f = openFile(path, "rb");
    if (!f) {
        error = "cannot open " + path;
        return false;
    }
    std::vector<u8> data;
    u8 buffer[65536];
    for (size_t n; (n = std::fread(buffer, 1, sizeof(buffer), f)) > 0;) data.insert(data.end(), buffer, buffer + n);
    std::fclose(f);

    if (data.size() < kHeaderBytes || std::memcmp(data.data(), kMagic, 8) != 0) {
        error = "not a MealKit input record";
        return false;
    }
    const u8* p = data.data() + 8;
    const u32 version = static_cast<u32>(get(p, 4));
    if (version != 1 && version != kVersion) {
        error = "unsupported input record version " + std::to_string(version);
        return false;
    }
    out.match.playerCount = static_cast<u32>(get(p, 4));
    out.match.seed = get(p, 8);
    out.match.matchData = static_cast<u32>(get(p, 4));
    get(p, 4);
    out.contentHash = get(p, 8);
    if (out.match.playerCount == 0 || out.match.playerCount > net::kMaxPlayers) {
        error = "bad player count";
        return false;
    }
    const u8* end = data.data() + data.size();
    const size_t inputBytes = out.match.playerCount * net::kInputBytes;
    for (;;) {
        if (static_cast<size_t>(end - p) < inputBytes + (version >= 2 ? 2u : 0u) + 8u) break;
        net::TickInputs in{};
        for (u32 pl = 0; pl < out.match.playerCount; ++pl) {
            std::memcpy(in.players[pl].bytes, p, net::kInputBytes);
            p += net::kInputBytes;
        }
        if (version >= 2) {
            const u32 commandBytes = static_cast<u32>(get(p, 2));
            if (commandBytes > net::kCommandBytesPerTick || static_cast<size_t>(end - p) < commandBytes + 8u) break;
            in.commandBytes = static_cast<u16>(commandBytes);
            std::memcpy(in.commands, p, commandBytes);
            p += commandBytes;
        }
        out.inputs.push_back(in);
        out.hashes.push_back(get(p, 8));
    }
    return true;
}

bool HashLog::open(const std::string& path) {
    close();
    m_file = openFile(path, "wb");
    if (!m_file) MEALKIT_ERROR("Cannot write the hash log %s", path.c_str());
    return m_file != nullptr;
}

void HashLog::write(u32 tick, u64 hash) {
    if (m_file) std::fprintf(m_file, "%u %016llx\n", tick, static_cast<unsigned long long>(hash));
}

void HashLog::close() {
    if (m_file) std::fclose(m_file);
    m_file = nullptr;
}

} // namespace mealkit::game
