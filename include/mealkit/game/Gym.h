#pragma once

#include "mealkit/core/Log.h"
#include "mealkit/core/Types.h"
#include "mealkit/game/InputRecord.h"
#include "mealkit/game/GameOptions.h"
#include "mealkit/session/Session.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

// Learning-agent environment (--gym on a headless exe): a gym-style reset/step loop over stdin/stdout, one JSON object per line.
//   The trainer (any language) starts `<game>_headless --gym [--players N] [--seed S] [--exit-tick T]` and talks to it:
//     {"cmd":"reset","seed":7}                  -> {"ok":true,"tick":0,"hash":"..","obs":[[..]],"reward":[..],"done":false}
//     {"cmd":"step","actions":[[..],[..]]}      -> same shape, one tick per step ("repeat":k runs k ticks with the same actions)
//     {"cmd":"step","action":[..]}              -> actions of player 0 only (other players send zero input)
//     {"cmd":"close"}                           -> {"ok":true}, then the program exits
//   An error is {"ok":false,"error":".."}. Output lines of the protocol start with '{'; engine log lines (start with '[') may be mixed in, skip them.
//   Everything runs on the same World the game plays with (same startWorld, same step), outside the simulation: determinism is unchanged,
//   and the same seed and actions give the same hashes (the "hash" field) on every machine.
//
// The game declares what an agent sees and does in its Headless class. All members are optional:
//   static session::PlayerInput gymAction(u8 player, const std::vector<f32>& action);   // default: values are the input bytes (0..255, negative = i8)
//   static void gymObserve(const World&, u8 player, std::vector<f32>& out);              // default: no observation (the hash only)
//   static f32 gymReward(const World&, u8 player);                                        // default: 0
//   static bool gymDone(const World&);                                                    // default: false (--exit-tick ends the episode too)
namespace mealkit::game::gym {

struct Request {
    std::string cmd;
    bool hasSeed = false;
    u64 seed = 0;
    u32 repeat = 1;
    std::vector<std::vector<f32>> actions; // per player
};

// Minimal reader for the request lines (keys: cmd, seed, repeat, action, actions). Returns false with a reason on malformed input.

// Quantizes one action value to an input byte: 0..255 as is, negatives wrap as i8 (-1 = 255)


// Builds a response line. obs/reward are per player.

namespace {

struct Reader {
    const std::string& s;
    size_t i = 0;
    void skip() {
        while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    }
    bool eat(char c) {
        skip();
        if (i < s.size() && s[i] == c) return ++i, true;
        return false;
    }
    bool string(std::string& out) {
        skip();
        if (i >= s.size() || s[i] != '"') return false;
        ++i;
        out.clear();
        while (i < s.size() && s[i] != '"') {
            if (s[i] == '\\' && i + 1 < s.size()) ++i;
            out += s[i++];
        }
        if (i >= s.size()) return false;
        ++i;
        return true;
    }
    bool number(f64& out) {
        skip();
        const char* begin = s.c_str() + i;
        char* end = nullptr;
        out = std::strtod(begin, &end);
        if (end == begin) return false;
        i += static_cast<size_t>(end - begin);
        return true;
    }
    bool numbers(std::vector<f32>& out) { // [1,2,3]
        out.clear();
        if (!eat('[')) return false;
        if (eat(']')) return true;
        for (;;) {
            f64 v;
            if (!number(v)) return false;
            out.push_back(static_cast<f32>(v));
            if (eat(']')) return true;
            if (!eat(',')) return false;
        }
    }
    bool matrix(std::vector<std::vector<f32>>& out) { // [[1,2],[3]]
        out.clear();
        if (!eat('[')) return false;
        if (eat(']')) return true;
        for (;;) {
            out.emplace_back();
            if (!numbers(out.back())) return false;
            if (eat(']')) return true;
            if (!eat(',')) return false;
        }
    }
};

} // namespace

inline bool parseRequest(const std::string& line, Request& out, std::string& error) {
    Reader r{line};
    if (!r.eat('{')) {
        error = "request must be a JSON object";
        return false;
    }
    if (r.eat('}')) {
        error = "missing cmd";
        return false;
    }
    for (;;) {
        std::string key;
        if (!r.string(key) || !r.eat(':')) {
            error = "malformed key";
            return false;
        }
        bool ok = true;
        f64 n = 0;
        if (key == "cmd") ok = r.string(out.cmd);
        else if (key == "seed") ok = r.number(n), out.seed = static_cast<u64>(n), out.hasSeed = ok;
        else if (key == "repeat") ok = r.number(n), out.repeat = n < 1 ? 1u : static_cast<u32>(n);
        else if (key == "action") {
            out.actions.assign(1, {});
            ok = r.numbers(out.actions[0]);
        } else if (key == "actions") ok = r.matrix(out.actions);
        else ok = false; // unknown keys are errors: a typo must not be silently ignored
        if (!ok) {
            error = "bad value for '" + key + "'";
            return false;
        }
        if (r.eat('}')) break;
        if (!r.eat(',')) {
            error = "expected ',' or '}'";
            return false;
        }
    }
    if (out.cmd.empty()) {
        error = "missing cmd";
        return false;
    }
    return true;
}

inline u8 actionByte(f32 value) {
    if (!(value == value)) return 0; // NaN
    const f32 clamped = std::clamp(value, -128.0f, 255.0f);
    const int v = static_cast<int>(std::lround(clamped));
    return static_cast<u8>(v & 0xFF);
}

inline std::string hashText(u64 hash) {
    char buf[20];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(hash));
    return buf;
}

inline std::string errorLine(const std::string& message) {
    std::string out = "{\"ok\":false,\"error\":\"";
    for (char c : message) {
        if (c == '"' || c == '\\') out += '\\';
        if (static_cast<unsigned char>(c) >= 0x20) out += c;
    }
    return out + "\"}";
}

inline std::string okLine() { return "{\"ok\":true}"; }

inline void appendNumber(std::string& out, f32 v) {
    if (!std::isfinite(v)) v = 0.0f; // JSON has no NaN/inf
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.9g", static_cast<double>(v));
    out += buf;
}

inline std::string stateLine(u32 tick, u64 hash, const std::vector<std::vector<f32>>& obs, const std::vector<f32>& reward, bool done) {
    std::string out = "{\"ok\":true,\"tick\":" + std::to_string(tick) + ",\"hash\":\"" + hashText(hash) + "\",\"obs\":[";
    for (size_t p = 0; p < obs.size(); ++p) {
        out += p ? ",[" : "[";
        for (size_t i = 0; i < obs[p].size(); ++i) {
            if (i) out += ',';
            appendNumber(out, obs[p][i]);
        }
        out += ']';
    }
    out += "],\"reward\":[";
    for (size_t p = 0; p < reward.size(); ++p) {
        if (p) out += ',';
        appendNumber(out, reward[p]);
    }
    return out + "],\"done\":" + (done ? "true}" : "false}");
}

template <class Game, class World>
session::PlayerInput actionToInput(u8 player, const std::vector<f32>& action) {
    if constexpr (requires { Game::gymAction(player, action); }) {
        return Game::gymAction(player, action);
    } else {
        session::PlayerInput pi;
        for (u32 i = 0; i < session::kInputBytes && i < action.size(); ++i) pi.bytes[i] = actionByte(action[i]);
        return pi;
    }
}

// Runs the protocol until "close" or end of input. start(world, match, physicsThreads) is the game's match start.
template <class Game, class World, class Start>
int run(const MatchOptions& options, Start&& start) {
    const u32 players = std::clamp(options.maxPlayers ? options.maxPlayers : 1u, 1u, static_cast<u32>(session::kMaxPlayers));
    std::unique_ptr<World> world;
    u32 tick = 0; // ticks run since reset
    bool done = false;

    auto observe = [&](u8 p) {
        std::vector<f32> out;
        if constexpr (requires { Game::gymObserve(*world, p, out); }) Game::gymObserve(*world, p, out);
        return out;
    };
    auto rewardOf = [&](u8 p) {
        if constexpr (requires { Game::gymReward(*world, p); }) return static_cast<f32>(Game::gymReward(*world, p));
        else return 0.0f;
    };
    auto respond = [&] {
        std::vector<std::vector<f32>> obs;
        std::vector<f32> reward;
        for (u32 p = 0; p < players; ++p) {
            obs.push_back(observe(static_cast<u8>(p)));
            reward.push_back(rewardOf(static_cast<u8>(p)));
        }
        std::puts(stateLine(tick, world->hash(), obs, reward, done).c_str());
        std::fflush(stdout);
    };
    auto fail = [&](const std::string& message) {
        std::puts(errorLine(message).c_str());
        std::fflush(stdout);
    };

    MEALKIT_INFO("Gym: %u player(s), waiting for requests on stdin", players);
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;
        Request req;
        std::string error;
        if (!parseRequest(line, req, error)) {
            fail(error);
            continue;
        }
        if (req.cmd == "close") {
            std::puts(okLine().c_str());
            std::fflush(stdout);
            return 0;
        }
        if (req.cmd == "reset") {
            MatchInfo match;
            match.seed = req.hasSeed ? req.seed : (options.seed ? options.seed : 1);
            match.playerCount = players;
            match.matchData = options.matchData;
            world = std::make_unique<World>();
            start(*world, match, options.physicsThreads);
            tick = 0;
            done = false;
            respond();
        } else if (req.cmd == "step") {
            if (!world) {
                fail("step before reset");
                continue;
            }
            if (done) {
                fail("episode is done, send reset");
                continue;
            }
            session::TickInputs inputs;
            for (u32 p = 0; p < players && p < req.actions.size(); ++p)
                inputs.players[p] = actionToInput<Game, World>(static_cast<u8>(p), req.actions[p]);
            const u32 repeat = std::max(req.repeat, 1u);
            for (u32 i = 0; i < repeat && !done; ++i) {
                world->step(inputs);
                ++tick;
                if constexpr (requires { Game::gymDone(*world); }) done = done || Game::gymDone(*world);
                if (options.exitTick && tick >= options.exitTick) done = true;
            }
            respond();
        } else {
            fail("unknown cmd '" + req.cmd + "'");
        }
    }
    return 0; // input closed
}

} // namespace mealkit::game::gym
