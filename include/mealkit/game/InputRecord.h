#pragma once

#include "mealkit/core/Types.h"
#include "mealkit/session/Session.h"

#include <cstdio>
#include <string>
#include <vector>

// Input recording and per-tick hash log (headless/CI determinism comparison, the first form of stage 3 replays).
//   Input recording (--record-inputs): confirmed tick inputs + that tick's state hash. --replay-inputs reruns the same inputs and compares hashes.
//     Cross-build hash comparison works by feeding the same file instead of running bots separately per build/OS/CPU.
//   Hash log (--hash-log): one "<tick> <hash hex>" line per tick. If two runs' files are identical, the state is the same from start to finish.
// File format (little endian): "MKINPUT1", u32 version, u32 player count, u64 seed, u32 match data, u32 0, u64 content hash,
// then per tick (player count × 8-byte inputs, u16 command byte count + command bytes, u64 hash). The number of ticks read to the end is the tick count (if it ends midway, the earlier part is still usable).
// Version 3 has a u8 loadout size + bytes per player after the header. Versions 1 (no commands, fixed tick size) and 2 (no loadouts) are also read.
namespace mealkit::game {

// Values needed to recreate the match exactly (decided by the lockstep session and sent to everyone)
struct MatchInfo {
    u64 seed = 0;
    u32 playerCount = 1;
    u32 matchData = 0;
    std::vector<std::vector<u8>> loadouts; // per-player match start data (mealkit/game/Loadout.h, empty if none)
};

// Match values confirmed by the session (after Running)
MatchInfo matchInfo(const session::Session& session);

struct InputRecording {
    MatchInfo match;
    u64 contentHash = 0;
    std::vector<session::TickInputs> inputs; // from tick 0
    std::vector<u64> hashes;             // state hash after running inputs[t]
};

class InputRecordWriter {
public:
    InputRecordWriter() = default;
    ~InputRecordWriter() { close(); }
    InputRecordWriter(const InputRecordWriter&) = delete;
    InputRecordWriter& operator=(const InputRecordWriter&) = delete;

    bool open(const std::string& path, const MatchInfo& match, u64 contentHash);
    bool isOpen() const { return m_file != nullptr; }
    void write(const session::TickInputs& inputs, u64 hash);
    void close();

private:
    std::FILE* m_file = nullptr;
    u32 m_players = 0;
};

bool loadInputRecording(const std::string& path, InputRecording& out, std::string& error);

// Replays (match recordings): the game exe and editor play write the input recording to <user folder>/replays/current.rec every match,
// and when the match ends keepReplay keeps it under a date/time name (keepReplays from project settings, oldest deleted first).
// A current.rec left over from a crash is also kept the same way on the next run (replays as many ticks as were read to the end).
// Watching: game exe --replay <file>, editor [File] > [Open Replay] (game::MatchService::playReplay)
inline constexpr const char* kReplayFolder = "replays";
std::string replayRecordingPath(const std::string& userFolder); // empty string if userFolder is empty (no recording)
// Moves current.rec to replays/<YYYY-MM-DD_HH-MM>.rec and keeps only keep of them. Returns the kept path (empty string if no recording or no ticks)
std::string keepReplay(const std::string& userFolder, u32 keep);
std::vector<std::string> listReplays(const std::string& userFolder); // full paths of kept replays, newest first

class HashLog {
public:
    HashLog() = default;
    ~HashLog() { close(); }
    HashLog(const HashLog&) = delete;
    HashLog& operator=(const HashLog&) = delete;

    bool open(const std::string& path);
    bool isOpen() const { return m_file != nullptr; }
    void write(u32 tick, u64 hash);
    void close();

private:
    std::FILE* m_file = nullptr;
};

} // namespace mealkit::game
