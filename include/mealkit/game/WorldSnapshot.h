#pragma once

#include "mealkit/core/Reflect.h"
#include "mealkit/physics/PhysicsWorld.h"

#include <cstring>
#include <vector>

// Full World snapshot: declared state fields (reflect) + physics. Used for rewind and shadow simulation within the same process.
// World contract: state (MEALKIT_STRUCT, state.tick field), physics (physics::PhysicsWorld), hash().
// Values outside these three (spawns, tuning etc. decided at match start) aren't in the snapshot (they don't change within the same match).
namespace mealkit::game {

struct WorldSnapshot {
    u32 tick = 0;
    u64 hash = 0; // World::hash() at save time (to verify restore)
    std::vector<u8> state;
    physics::PhysicsSnapshot physics;

    size_t byteSize() const { return state.size() + physics.byteSize(); }
};

template <class World>
void saveSnapshot(const World& world, WorldSnapshot& out) {
    reflect::ByteWriter w;
    reflect::saveStruct(reflect::typeOf<std::remove_cvref_t<decltype(world.state)>>(), &world.state, w);
    out.state = std::move(w.data());
    world.physics.save(out.physics);
    out.tick = world.state.tick;
    out.hash = world.hash();
}

// Bytes to send to another process (late join/reconnect, between identical builds): [tick u32][hash u64][state size u32][state][physics]
inline void writeSnapshotBytes(const WorldSnapshot& s, std::vector<u8>& out) {
    std::vector<u8> physics;
    s.physics.write(physics);
    const u32 stateSize = static_cast<u32>(s.state.size());
    out.resize(16 + s.state.size() + physics.size());
    std::memcpy(out.data(), &s.tick, 4);
    std::memcpy(out.data() + 4, &s.hash, 8);
    std::memcpy(out.data() + 12, &stateSize, 4);
    if (stateSize) std::memcpy(out.data() + 16, s.state.data(), stateSize);
    if (!physics.empty()) std::memcpy(out.data() + 16 + stateSize, physics.data(), physics.size());
}

inline bool readSnapshotBytes(const u8* data, size_t size, WorldSnapshot& out) {
    u32 stateSize = 0;
    if (size < 16) return false;
    std::memcpy(&out.tick, data, 4);
    std::memcpy(&out.hash, data + 4, 8);
    std::memcpy(&stateSize, data + 12, 4);
    if (16 + static_cast<size_t>(stateSize) > size) return false;
    out.state.assign(data + 16, data + 16 + stateSize);
    return out.physics.read(data + 16 + stateSize, size - 16 - stateSize);
}

// false if hash() after restore differs from save time (meaning there's undeclared state)
template <class World>
bool loadSnapshot(World& world, const WorldSnapshot& snapshot) {
    reflect::ByteReader r(snapshot.state.data(), snapshot.state.size());
    if (!reflect::loadStruct(reflect::typeOf<std::remove_cvref_t<decltype(world.state)>>(), &world.state, r) ||
        !r.atEnd())
        return false;
    if (!world.physics.restore(snapshot.physics)) return false;
    return world.hash() == snapshot.hash;
}

} // namespace mealkit::game
