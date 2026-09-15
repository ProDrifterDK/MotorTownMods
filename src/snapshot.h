#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "active_snapshot_root.h"
#include <LuaMadeSimple/LuaMadeSimple.hpp>
#include <Unreal/FWeakObjectPtr.hpp>

namespace RC::Unreal
{
    class UObject;
    class FProperty;
}

namespace MotorTown::Snapshot
{
    struct Value
    {
        using Array = std::vector<Value>;
        using Object = std::map<std::string, Value>;
        std::variant<std::monostate, bool, int64_t, double, std::string, Array, Object> data{};

        Value() = default;
        template <typename T>
        Value(T value) : data(std::move(value)) {}
    };

    enum class State
    {
        Queued,
        Capturing,
        Ready,
        Error,
    };

    struct Limits
    {
        size_t max_nodes{20000};
        size_t max_elements{10000};
        size_t max_slots{50000};
        size_t max_bytes{4 * 1024 * 1024};
        int32_t max_depth{8};
    };

    struct Query
    {
        enum class Kind { Vehicles, Players } kind{};
        std::optional<int64_t> vehicle_id{};
        std::string player_id{};
        std::vector<std::string> fields{};
        size_t limit{100};
        bool controlled_only{};
        Limits limits{};
        std::chrono::steady_clock::time_point deadline{};
    };

    struct Result
    {
        State state{State::Queued};
        Value value{};
        std::string error{};
    };

    class Store
    {
      public:
        static constexpr size_t MaxPending = 64;
        static constexpr size_t MaxFields = 32;
        static constexpr size_t MaxFieldLength = 128;
        static constexpr size_t MaxIdLength = 128;

        static auto begin(Query query) -> uint64_t;
        static auto capture(uint64_t request_id) -> void;
        static auto fail(uint64_t request_id, std::string error) -> bool;
        static auto take(uint64_t request_id) -> std::optional<Result>;
        static auto cancel(uint64_t request_id) -> bool;
        static auto cancel_all() -> void;
        static auto set_active_game_state(RC::Unreal::UObject* game_state, RC::Unreal::UObject* world) -> void;
        static auto clear_active_game_state() -> void;
        // LoadMap pre notification (delivered-only boundary, see dllmain.cpp):
        // the outgoing world's cached root pair AND the current-world anchor
        // are invalidated together under one lock, the travel generation
        // advances (invalidating every admission sampled before it), and the
        // open-travel count rises. Until the matching post notification is
        // DELIVERED, every serve and recovery refuses fail-closed; there is no
        // delivery guarantee to fall back on.
        static auto invalidate_travel_state() -> void;
        // Engine-established current-world identity, captured inside the
        // pinned overlay's LoadMap post callback (see dllmain.cpp). A null or
        // unresolvable anchor refuses every serve and recovery attempt. The
        // refresh advances the generation and closes exactly one open travel
        // interval, so a nested inner LoadMap's post cannot restore serve
        // authority while an enclosing travel is still open.
        static auto set_current_world(RC::Unreal::UObject* world) -> void;
        static auto resolve_active_game_state() -> RC::Unreal::UObject*;
        static auto push_value(const RC::LuaMadeSimple::Lua& lua, const Value& value) -> void;

      private:
        struct Entry
        {
            uint64_t generation{};
            Query query{};
            Result result{};
        };

        static std::mutex s_mutex;
        static std::map<uint64_t, Entry> s_entries;
        static uint64_t s_next_id;
        static RC::Unreal::FWeakObjectPtr s_active_game_state;
        static RC::Unreal::FWeakObjectPtr s_active_world;
        static RC::Unreal::FWeakObjectPtr s_current_world;
        // Monotonic travel bookkeeping (see active_snapshot_root.h). The
        // generation advances on every travel invalidation and anchor refresh;
        // open_travels counts delivered pre notifications whose matching post
        // has not been delivered. Both are written only under s_mutex.
        static uint64_t s_travel_generation;
        static uint64_t s_open_travels;

        // Caller must hold s_mutex. Resolves the anchor weak pointer and
        // snapshots the travel bookkeeping into the pure lifecycle sample the
        // admission seams (active_snapshot_root.h) consume. A weak pointer
        // that no longer resolves samples as a null anchor: unavailable
        // authority, never a servable one.
        static auto sample_lifecycle_state() -> SnapshotLifecycleState;
        // Final decision point for a recovered pair: the validity check and
        // the store run in ONE critical section, so a concurrent travel
        // invalidation/refresh cannot land between AdmissionStillValid and the
        // store. A pair admitted against generation N is never stored once the
        // generation has advanced; returns false (storing nothing) otherwise.
        static auto store_active_game_state_if_current(const SnapshotLifecycleState& sampled, RC::Unreal::UObject* game_state, RC::Unreal::UObject* world) -> bool;
    };
}
