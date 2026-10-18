#include "mealkit/game/Headless.h"

#include "mealkit/game/GameData.h"
#include "mealkit/net/Socket.h"
#include "mealkit/platform/Paths.h"
#include "mealkit/platform/System.h"

#include <cstdlib>

namespace mealkit::game {

void printHeadlessUsage() {
    std::printf("MealKit headless (no window, renderer or audio; bot input)\n"
                "  --exit-tick <t>        run until tick t, print the state hash and exit (required unless replaying)\n"
                "  --host [port]          host a match (default: the game's project settings)\n"
                "  --join <addr[:port]>   join a host\n"
                "  --local                host on 127.0.0.1 only\n"
                "  --players <n>          max players when hosting\n"
                "  --auto-start <n>       host: start when n players are in the lobby (default: --players)\n"
                "  --match-data <n>       game-defined match value sent by the host (sandbox: physics boxes)\n"
                "  --delay <ticks>        input delay (default 3)\n"
                "  --seed <n>             world seed\n"
                "  --threads <n>          physics worker threads (default: auto)\n"
                "  --scene <file>         scene to play (default: scene in mealkit.project)\n"
                "  --tuning <file>        tuning values (default assets/game.tuning)\n"
                "  --shadow               run a shadow simulation and compare state hashes every tick\n"
                "  --hash-log <file>      write \"<tick> <hash>\" for every tick\n"
                "  --record-inputs <file> save the confirmed inputs and hashes of every tick\n"
                "  --replay-inputs <file> replay a recorded match and compare every tick hash (no network)\n"
                "  --any-build            host: accept peers running a different build\n"
                "Window options (--bot, --vsync, --fps, --size, --debug) are accepted and ignored.\n");
}

bool parseHeadlessArgs(int argc, char** argv, HeadlessOptions& o) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](const char*& out) {
            if (i + 1 >= argc) return false;
            out = argv[++i];
            return true;
        };
        auto hasValue = [&] { return i + 1 < argc && argv[i + 1][0] != '-'; };
        const char* v = nullptr;
        if (a == "--headless" || a == "--bot" || a == "--debug" || a == "--mute") continue;
        if (a == "--host") {
            o.mode = HeadlessOptions::Mode::Host;
            if (hasValue()) o.port = static_cast<u16>(std::atoi(argv[++i]));
        } else if (a == "--join" && next(v)) {
            o.mode = HeadlessOptions::Mode::Join;
            o.joinAddress = v;
        } else if (a == "--local") o.loopbackOnly = true;
        else if (a == "--players" && next(v)) o.maxPlayers = static_cast<u32>(std::atoi(v));
        else if (a == "--auto-start" && next(v)) o.autoStart = static_cast<u32>(std::atoi(v));
        else if (a == "--match-data" && next(v)) o.matchData = static_cast<u32>(std::atoi(v));
        else if (a == "--delay" && next(v)) o.inputDelay = static_cast<u32>(std::atoi(v));
        else if (a == "--seed" && next(v)) o.seed = std::strtoull(v, nullptr, 10);
        else if (a == "--threads" && next(v)) o.physicsThreads = static_cast<u32>(std::atoi(v));
        else if (a == "--exit-tick" && next(v)) o.exitTick = static_cast<u32>(std::atoi(v));
        else if (a == "--scene" && next(v)) o.scenePath = v;
        else if (a == "--tuning" && next(v)) o.tuningPath = v;
        else if (a == "--shadow") o.shadow = true;
        else if (a == "--hash-log" && next(v)) o.hashLogPath = v;
        else if (a == "--record-inputs" && next(v)) o.recordInputsPath = v;
        else if (a == "--replay-inputs" && next(v)) o.replayInputsPath = v;
        else if (a == "--any-build") o.anyBuild = true;
        else if ((a == "--vsync" || a == "--fps") && next(v)) continue;
        else if (a == "--size" && i + 2 < argc) i += 2;
        else if (a == "--help" || a == "-h") return false;
        else {
            std::printf("Unknown option: %s\n", a.c_str());
            return false;
        }
    }
    return true;
}

bool loadHeadlessData(const HeadlessOptions& options, const reflect::TypeInfo& tuningType, void* tuning,
                      const reflect::TypeInfo* gameProjectType, void* gameProject, HeadlessData& out) {
    loadGameProject(out.project, gameProjectType, gameProject);
    MEALKIT_INFO("CPU: %s", cpuName().c_str()); // 다른 PC 결정론 기록용
    const std::string scenePath =
        options.scenePath.empty() ? findAsset(startSceneAsset(out.project.settings)) : options.scenePath;
    if (!scenePath.empty()) {
        std::string error;
        if (!scene::load(out.scene, scenePath, &error)) {
            MEALKIT_ERROR("Cannot load scene %s: %s", scenePath.c_str(), error.c_str());
            return false;
        }
        MEALKIT_INFO("Scene: %s (%zu entities)", scenePath.c_str(), out.scene.entities.size());
    } else {
        out.scene = scene::makeDefault();
        MEALKIT_INFO("Scene file not found - using the built-in arena");
    }
    out.flatScene = scene::flattened(out.scene);
    loadTuning(tuningType, tuning, options.tuningPath);
    // 출시 exe(main.cpp)와 같은 해시라야 창 있는 피어와도 접속되고, 입력 기록을 서로 재생할 수 있다
    out.contentHash = contentHash(out.scene, out.models, &reflect::moduleObjectTypes(), &tuningType, tuning,
                                  {&reflect::typeOf<ProjectSettings>(), &out.project.settings},
                                  {gameProjectType, gameProject}, &reflect::moduleTables());
    return true;
}

bool startHeadlessSession(net::LockstepSession& session, const HeadlessOptions& o, u64 seed) {
    switch (o.mode) {
    case HeadlessOptions::Mode::Offline: return session.startOffline(seed, o.matchData);
    case HeadlessOptions::Mode::Host:
        if (!session.host(o.port, o.maxPlayers, o.inputDelay, o.loopbackOnly)) {
            MEALKIT_ERROR("Could not host on port %u", o.port);
            return false;
        }
        return true;
    case HeadlessOptions::Mode::Join: {
        net::Address address;
        const bool resolved = net::startup() && net::resolveAddress(o.joinAddress.c_str(), o.port, address);
        net::cleanup();
        if (!resolved) {
            MEALKIT_ERROR("Could not resolve '%s'", o.joinAddress.c_str());
            return false;
        }
        if (!session.join(address)) {
            MEALKIT_ERROR("Could not open a UDP socket");
            return false;
        }
        return true;
    }
    }
    return false;
}

void printFinal(u32 tick, u64 hash, bool desynced) {
    std::printf("FINAL tick=%u hash=%016llx desync=%d\n", tick, static_cast<unsigned long long>(hash), desynced ? 1 : 0);
    std::fflush(stdout);
}

} // namespace mealkit::game
