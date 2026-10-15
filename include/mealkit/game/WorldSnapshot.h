#pragma once

#include "mealkit/core/Reflect.h"
#include "mealkit/physics/PhysicsWorld.h"

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
