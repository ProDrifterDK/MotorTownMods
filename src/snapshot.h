#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <variant>
#include <vector>

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
        static auto take(uint64_t request_id) -> std::optional<Result>;
        static auto cancel(uint64_t request_id) -> bool;
        static auto cancel_all() -> void;
        static auto set_active_game_state(RC::Unreal::UObject* game_state, RC::Unreal::UObject* world) -> void;
        static auto clear_active_game_state() -> void;
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
    };
}
