#pragma once

#include "mealkit/core/Reflect.h"
#include "mealkit/physics/PhysicsWorld.h"

#include <cstring>
#include <vector>

// World 전체 스냅샷: 선언한 상태 필드(reflect) + 물리. 같은 프로세스 안에서 되감기와 그림자 시뮬레이션에 쓴다.
// World 약속: state(MEALKIT_STRUCT, state.tick 필드), physics(physics::PhysicsWorld), hash().
// 이 셋 밖의 값(매치 시작 때 정해지는 spawns, tuning 등)은 스냅샷에 들어가지 않는다 (같은 매치 안에서는 바뀌지 않는다).
namespace mealkit::game {

struct WorldSnapshot {
    u32 tick = 0;
    u64 hash = 0; // 저장할 때의 World::hash() (복원 확인용)
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

// 다른 프로세스로 보낼 바이트 (중간 참가·재접속, 같은 빌드끼리): [틱 u32][해시 u64][상태 크기 u32][상태][물리]
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

// 복원한 뒤의 hash()가 저장할 때와 다르면 false (선언하지 않은 상태가 있다는 뜻)
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
