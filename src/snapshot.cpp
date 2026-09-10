#include "snapshot.h"
#include "active_snapshot_root.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <unordered_set>

#include <Unreal/UObject.hpp>
#include <Unreal/UClass.hpp>
#include <Unreal/UnrealFlags.hpp>
#include <Unreal/UScriptStruct.hpp>
#include <Unreal/Property/FArrayProperty.hpp>
#include <Unreal/Property/FBoolProperty.hpp>
#include <Unreal/Property/FEnumProperty.hpp>
#include <Unreal/Property/FMapProperty.hpp>
#include <Unreal/Property/FNameProperty.hpp>
#include <Unreal/Property/FObjectProperty.hpp>
#include <Unreal/Property/FSetProperty.hpp>
#include <Unreal/Property/FStrProperty.hpp>
#include <Unreal/Property/FStructProperty.hpp>
#include <Unreal/Property/FTextProperty.hpp>
#include <Unreal/Property/NumericPropertyTypes.hpp>

using namespace RC;
using namespace RC::Unreal;

namespace MotorTown::Snapshot
{
    std::mutex Store::s_mutex{};
    std::map<uint64_t, Store::Entry> Store::s_entries{};
    uint64_t Store::s_next_id{1};
    FWeakObjectPtr Store::s_active_game_state{};
    FWeakObjectPtr Store::s_active_world{};

    namespace
    {
        struct Budget
        {
            Limits limits;
            std::chrono::steady_clock::time_point deadline;
            size_t nodes{};
            size_t slots{};
            size_t elements{};
            size_t bytes{};

            auto node(size_t byte_count = 0) -> void
            {
                if (std::chrono::steady_clock::now() >= deadline)
                    throw std::runtime_error{"snapshot deadline exceeded during capture"};
                if (++nodes > limits.max_nodes || byte_count > limits.max_bytes - bytes)
                {
                    throw std::runtime_error{"snapshot budget exceeded"};
                }
                bytes += byte_count;
            }

            auto slot() -> void
            {
                if (++slots > limits.max_slots)
                    throw std::runtime_error{"snapshot slot budget exceeded"};
                node();
            }

            auto container(size_t count) -> void
            {
                if (count > limits.max_elements - elements)
                {
                    throw std::runtime_error{"snapshot element budget exceeded"};
                }
                elements += count;
                node();
            }
        };

        auto property_storage(FProperty* property, void* data, bool value_address) -> void*
        {
            return value_address ? data : property->ContainerPtrToValuePtr<void>(data);
        }

        auto is_unsigned_numeric(FNumericProperty* property) -> bool
        {
            return property->IsA<FByteProperty>() || property->IsA<FUInt16Property>() ||
                   property->IsA<FUInt32Property>() || property->IsA<FUInt64Property>();
        }

        auto read_numeric(FNumericProperty* property, void* storage) -> Value
        {
            if (!is_unsigned_numeric(property)) return property->GetSignedIntPropertyValue(storage);
            const uint64 value = property->GetUnsignedIntPropertyValue(storage);
            if (value <= static_cast<uint64>(std::numeric_limits<int64_t>::max()))
            {
                return static_cast<int64_t>(value);
            }
            return std::to_string(value);
        }

        auto object_is_readable(UObject* object) -> bool
        {
            return object && !object->IsUnreachable() &&
                   !object->HasAnyFlags(static_cast<EObjectFlags>(RF_BeginDestroyed | RF_FinishDestroyed));
        }

        auto object_reference(UObject* object, Budget& budget) -> Value
        {
            if (!object_is_readable(object))
            {
                budget.node();
                return {};
            }

            Value::Object ref;
            const auto class_name = to_string(object->GetClassPrivate()->GetName());
            budget.node(class_name.size());
            ref.emplace("type", class_name);

            if (auto id_property = object->GetPropertyByNameInChain(STR("Net_VehicleId")); id_property && id_property->IsA<FNumericProperty>())
            {
                auto numeric = static_cast<FNumericProperty*>(id_property);
                auto value = id_property->ContainerPtrToValuePtr<void>(object);
                ref.emplace("id", read_numeric(numeric, value));
            }
            else if (auto id_property = object->GetPropertyByNameInChain(STR("UniqueID")); id_property)
            {
                FString text;
                auto value = id_property->ContainerPtrToValuePtr<void>(object);
                id_property->ExportTextItem(text, value, nullptr, object, 0);
                const auto id = to_string(*text);
                budget.node(id.size());
                ref.emplace("id", id);
            }
            return ref;
        }

        auto read_value(FProperty* property, void* data, Budget& budget, int32 depth, bool value_address = false) -> Value;

        auto read_struct(FStructProperty* property, void* value, Budget& budget, int32 depth, bool movement_projection) -> Value
        {
            if (depth < 0) throw std::runtime_error{"snapshot depth budget exceeded"};
            Value::Object result;
            static const std::unordered_set<std::string> movement_fields{
                "Location", "Velocity", "Throttle", "Brake", "HandBrake",
                "Quat", "AngularVelocityInRadian", "Rotation", "LinearVelocity", "AngularVelocity",
                "VehicleId", "ControllerId", "bSimulatedPhysicSleep", "bRepPhysics"
            };

            for (FProperty* inner = property->GetStruct()->GetPropertyLink(); inner; inner = inner->GetPropertyLinkNext())
            {
                const auto name = to_string(inner->GetName());
                if (movement_projection && !movement_fields.contains(name)) continue;
                budget.node(name.size());
                result.emplace(name, read_value(inner, value, budget, depth - 1));
            }
            budget.container(result.size());
            return result;
        }

        auto scalar_key(const Value& value) -> std::string
        {
            if (auto string_value = std::get_if<std::string>(&value.data)) return *string_value;
            if (auto integer_value = std::get_if<int64_t>(&value.data)) return std::to_string(*integer_value);
            if (auto double_value = std::get_if<double>(&value.data)) return std::to_string(*double_value);
            if (auto bool_value = std::get_if<bool>(&value.data)) return *bool_value ? "true" : "false";
            throw std::runtime_error{"unsupported snapshot map key"};
        }

        auto read_value(FProperty* property, void* data, Budget& budget, int32 depth, bool value_address) -> Value
        {
            if (!property || !data) return {};
            if (depth < 0) throw std::runtime_error{"snapshot depth budget exceeded"};
            void* storage = property_storage(property, data, value_address);
            if (!storage) return {};

            if (property->IsA<FStrProperty>())
            {
                const auto* source = static_cast<FString*>(storage);
                const auto length = source->Len();
                if (length < 0) throw std::runtime_error{"invalid snapshot string length"};
                budget.node(static_cast<size_t>(length));
                return to_string(**source);
            }
            if (property->IsA<FNameProperty>())
            {
                const auto value = to_string(static_cast<FName*>(storage)->ToString());
                budget.node(value.size());
                return value;
            }
            if (property->IsA<FTextProperty>())
            {
                const auto value = to_string(static_cast<FText*>(storage)->ToString());
                budget.node(value.size());
                return value;
            }
            if (property->IsA<FFloatProperty>())
            {
                budget.node();
                return static_cast<double>(*static_cast<float*>(storage));
            }
            if (property->IsA<FDoubleProperty>())
            {
                budget.node();
                return *static_cast<double*>(storage);
            }
            if (property->IsA<FNumericProperty>())
            {
                auto numeric = static_cast<FNumericProperty*>(property);
                budget.node();
                return read_numeric(numeric, storage);
            }
            if (property->IsA<FEnumProperty>())
            {
                auto underlying = static_cast<FEnumProperty*>(property)->GetUnderlyingProperty();
                budget.node();
                return read_numeric(underlying, storage);
            }
            if (property->IsA<FBoolProperty>())
            {
                auto bool_property = static_cast<FBoolProperty*>(property);
                budget.node();
                return bool_property->GetPropertyValue(storage);
            }
            if (property->IsA<FStructProperty>())
            {
                auto* struct_property = static_cast<FStructProperty*>(property);
                if (to_string(struct_property->GetStruct()->GetName()) == "Guid")
                {
                    FString text;
                    property->ExportTextItem(text, storage, nullptr, nullptr, 0);
                    const auto value = to_string(*text);
                    budget.node(value.size());
                    return value;
                }
                const bool movement = to_string(property->GetName()) == "VehicleReplicatedMovement";
                return read_struct(struct_property, storage, budget, depth, movement);
            }
            if (property->IsA<FObjectProperty>())
            {
                return object_reference(static_cast<FObjectProperty*>(property)->GetObjectPropertyValue(storage), budget);
            }
            if (property->IsA<FArrayProperty>())
            {
                auto array_property = static_cast<FArrayProperty*>(property);
                auto array = static_cast<FScriptArray*>(storage);
                const int32 count = array->Num();
                if (count < 0) throw std::runtime_error{"invalid snapshot array size"};
                budget.container(static_cast<size_t>(count));
                Value::Array result;
                result.reserve(count);
                auto inner = array_property->GetInner();
                const int32 element_size = inner->GetElementSize();
                auto bytes = static_cast<uint8*>(array->GetData());
                for (int32 index = 0; index < count; ++index)
                {
                    budget.slot();
                    result.emplace_back(read_value(inner, bytes + (index * element_size), budget, depth - 1, true));
                }
                return result;
            }
            if (property->IsA<FMapProperty>())
            {
                auto map_property = static_cast<FMapProperty*>(property);
                auto map = static_cast<FScriptMap*>(storage);
                auto key = map_property->GetKeyProp();
                auto mapped = map_property->GetValueProp();
                auto layout = FScriptMap::GetScriptLayout(key->GetSize(), key->GetMinAlignment(), mapped->GetSize(), mapped->GetMinAlignment());
                budget.container(static_cast<size_t>(std::max(0, map->Num())));
                Value::Object result;
                for (int32 index = 0; index < map->GetMaxIndex(); ++index)
                {
                    budget.slot();
                    if (!map->IsValidIndex(index)) continue;
                    auto entry = static_cast<uint8*>(map->GetData(index, layout));
                    auto key_value = read_value(key, entry, budget, depth - 1, true);
                    result.emplace(scalar_key(key_value), read_value(mapped, entry + layout.ValueOffset, budget, depth - 1, true));
                }
                return result;
            }
            if (property->IsA<FSetProperty>())
            {
                auto set_property = static_cast<FSetProperty*>(property);
                auto set = static_cast<FScriptSet*>(storage);
                auto element = set_property->GetElementProp();
                auto layout = FScriptSet::GetScriptLayout(element->GetSize(), element->GetMinAlignment());
                budget.container(static_cast<size_t>(std::max(0, set->Num())));
                Value::Array result;
                result.reserve(set->Num());
                for (int32 index = 0; index < set->GetMaxIndex(); ++index)
                {
                    budget.slot();
                    if (!set->IsValidIndex(index)) continue;
                    result.emplace_back(read_value(element, set->GetData(index, layout), budget, depth - 1, true));
                }
                return result;
            }

            budget.node();
            return {};
        }

        auto project_vehicle_parts(FArrayProperty* property, UObject* object, Budget& budget) -> Value
        {
            auto* inner = property->GetInner();
            if (!inner || !inner->IsA<FStructProperty>())
                throw std::runtime_error{"Net_Parts has an unsupported element type"};

            auto* array = property->ContainerPtrToValuePtr<FScriptArray>(object);
            const int32 count = array->Num();
            if (count < 0) throw std::runtime_error{"invalid Net_Parts array size"};
            budget.container(static_cast<size_t>(count));

            static const std::unordered_set<std::string> part_fields{"Slot", "Key", "Damage"};
            auto* struct_property = static_cast<FStructProperty*>(inner);
            const int32 element_size = inner->GetElementSize();
            auto* bytes = static_cast<uint8*>(array->GetData());
            Value::Array result;
            result.reserve(count);

            for (int32 index = 0; index < count; ++index)
            {
                budget.slot();
                auto* element = bytes + (index * element_size);
                Value::Object projected_part;
                for (FProperty* field = struct_property->GetStruct()->GetPropertyLink(); field; field = field->GetPropertyLinkNext())
                {
                    const auto name = to_string(field->GetName());
                    if (!part_fields.contains(name)) continue;
                    budget.node(name.size());
                    projected_part.emplace(name, read_value(field, element, budget, 0));
                }
                budget.container(projected_part.size());
                result.emplace_back(std::move(projected_part));
            }
            return result;
        }

        auto read_identifier(UObject* object, const wchar_t* property_name) -> std::string
        {
            auto property = object->GetPropertyByNameInChain(property_name);
            if (!property) return {};
            FString text;
            auto value = property->ContainerPtrToValuePtr<void>(object);
            property->ExportTextItem(text, value, nullptr, object, 0);
            return to_string(*text);
        }

        auto project_object(UObject* object, const Query& query, Budget& budget) -> Value
        {
            Value::Object projected;
            const std::vector<std::string> vehicle_defaults{
                "Net_VehicleId", "Net_LastMovementOwnerPCName", "Net_OwnerCharacterId", "VehicleReplicatedMovement"
            };
            const std::vector<std::string> player_defaults{
                "Score", "Ping", "bIsAdmin", "bIsHost", "CharacterGuid", "VehicleKey"
            };
            const auto& fields = query.fields.empty()
                ? (query.kind == Query::Kind::Vehicles ? vehicle_defaults : player_defaults)
                : query.fields;

            for (const auto& field : fields)
            {
                auto property = object->GetPropertyByNameInChain(to_wstring(field).c_str());
                if (!property) continue;
                budget.node(field.size());
                if (query.kind == Query::Kind::Vehicles && field == "Net_Parts" && property->IsA<FArrayProperty>())
                {
                    projected.emplace(field, project_vehicle_parts(static_cast<FArrayProperty*>(property), object, budget));
                }
                else
                {
                    projected.emplace(field, read_value(property, object, budget, query.limits.max_depth));
                }
            }

            if (query.kind == Query::Kind::Vehicles)
            {
                if (auto id_property = object->GetPropertyByNameInChain(STR("Net_VehicleId")); id_property)
                {
                    projected.insert_or_assign("Net_VehicleId", read_value(id_property, object, budget, 0));
                }
            }
            else
            {
                const auto id = read_identifier(object, STR("UniqueID"));
                budget.node(id.size());
                projected.insert_or_assign("UniqueID", id);
                if (query.fields.empty())
                {
                    auto* name_property = object->GetPropertyByNameInChain(STR("PlayerNamePrivate"));
                    if (!name_property) name_property = object->GetPropertyByNameInChain(STR("PlayerName"));
                    if (name_property)
                    {
                        projected.insert_or_assign("Name", read_value(name_property, object, budget, 0));
                    }
                }
            }
            budget.container(projected.size());
            return projected;
        }

        auto capture_query(const Query& query) -> Value
        {
            if (std::chrono::steady_clock::now() >= query.deadline)
            {
                throw std::runtime_error{"snapshot deadline expired before capture"};
            }

            Budget budget{query.limits, query.deadline};
            budget.node();
            auto* game_state = Store::resolve_active_game_state();
            budget.node();
            if (!object_is_readable(game_state))
                throw std::runtime_error{"active MotorTownGameState is unavailable"};

            const wchar_t* array_name = query.kind == Query::Kind::Vehicles ? STR("Vehicles") : STR("PlayerArray");
            auto array_property_base = game_state->GetPropertyByNameInChain(array_name);
            if (!array_property_base || !array_property_base->IsA<FArrayProperty>())
            {
                throw std::runtime_error{"game-state snapshot array is unavailable"};
            }

            auto array_property = static_cast<FArrayProperty*>(array_property_base);
            auto object_property = static_cast<FObjectProperty*>(array_property->GetInner());
            if (!object_property || !array_property->GetInner()->IsA<FObjectProperty>())
            {
                throw std::runtime_error{"game-state snapshot array has an unsupported element type"};
            }
            auto array = array_property->ContainerPtrToValuePtr<FScriptArray>(game_state);
            budget.container(static_cast<size_t>(std::max(0, array->Num())));
            Value::Array result;
            result.reserve(std::min(static_cast<size_t>(array->Num()), query.limit));
            const int32 element_size = object_property->GetElementSize();
            auto bytes = static_cast<uint8*>(array->GetData());

            for (int32 index = 0; index < array->Num() && result.size() < query.limit; ++index)
            {
                budget.slot();
                auto object_storage = bytes + (index * element_size);
                auto object = *static_cast<UObject**>(static_cast<void*>(object_storage));
                if (!object_is_readable(object)) continue;

                if (query.kind == Query::Kind::Vehicles)
                {
                    auto id_property = object->GetPropertyByNameInChain(STR("Net_VehicleId"));
                    if (!id_property || !id_property->IsA<FNumericProperty>()) continue;
                    auto numeric = static_cast<FNumericProperty*>(id_property);
                    auto value = id_property->ContainerPtrToValuePtr<void>(object);
                    int64_t id{};
                    if (is_unsigned_numeric(numeric))
                    {
                        const auto unsigned_id = numeric->GetUnsignedIntPropertyValue(value);
                        if (unsigned_id > static_cast<uint64>(std::numeric_limits<int64_t>::max()))
                            throw std::runtime_error{"vehicle ID exceeds snapshot integer range"};
                        id = static_cast<int64_t>(unsigned_id);
                    }
                    else id = numeric->GetSignedIntPropertyValue(value);
                    if (query.vehicle_id && *query.vehicle_id != id) continue;
                    if (query.controlled_only)
                    {
                        auto owner_property = object->GetPropertyByNameInChain(STR("Net_MovementOwnerPC"));
                        if (!owner_property || !owner_property->IsA<FObjectProperty>() ||
                            !static_cast<FObjectProperty*>(owner_property)->GetObjectPropertyValue(
                                owner_property->ContainerPtrToValuePtr<void>(object))) continue;
                    }
                }
                else
                {
                    const auto id = read_identifier(object, STR("UniqueID"));
                    if (!query.player_id.empty() && query.player_id != id) continue;
                }
                result.emplace_back(project_object(object, query, budget));
            }
            return result;
        }
    }

    auto Store::begin(Query query) -> uint64_t
    {
        if (query.fields.size() > MaxFields) throw std::runtime_error{"too many snapshot fields"};
        if (query.player_id.size() > MaxIdLength) throw std::runtime_error{"snapshot player ID is too long"};
        if (query.limit == 0 || query.limit > query.limits.max_elements) throw std::runtime_error{"invalid snapshot result limit"};
        static const std::unordered_set<std::string> vehicle_fields{
            "Net_VehicleId", "Net_LastMovementOwnerPCName", "Net_OwnerCharacterId",
            "Net_Parts", "VehicleReplicatedMovement"
        };
        static const std::unordered_set<std::string> player_fields{
            "Score", "Ping", "bIsAdmin", "bIsHost", "CharacterGuid", "VehicleKey", "UniqueID"
        };
        const auto& allowed_fields = query.kind == Query::Kind::Vehicles ? vehicle_fields : player_fields;
        for (const auto& field : query.fields)
        {
            if (field.empty() || field.size() > MaxFieldLength || !allowed_fields.contains(field))
                throw std::runtime_error{"snapshot field is not in the schema-v2 projection"};
        }
        std::lock_guard guard{s_mutex};
        const auto now = std::chrono::steady_clock::now();
        std::erase_if(s_entries, [&](const auto& item) { return item.second.query.deadline <= now; });
        if (s_entries.size() >= MaxPending) throw std::runtime_error{"snapshot queue is full"};
        const uint64_t id = s_next_id++;
        s_entries.emplace(id, Entry{id, std::move(query), {State::Queued, {}, {}}});
        return id;
    }

    auto Store::capture(uint64_t request_id) -> void
    {
        Query query;
        uint64_t generation{};
        {
            std::lock_guard guard{s_mutex};
            auto found = s_entries.find(request_id);
            if (found == s_entries.end() || found->second.result.state != State::Queued) return;
            found->second.result.state = State::Capturing;
            query = found->second.query;
            generation = found->second.generation;
        }

        Result completed;
        try
        {
            completed = {State::Ready, capture_query(query), {}};
        }
        catch (const std::exception& error)
        {
            completed = {State::Error, {}, error.what()};
        }

        std::lock_guard guard{s_mutex};
        auto found = s_entries.find(request_id);
        if (found != s_entries.end() && found->second.generation == generation && found->second.result.state == State::Capturing)
        {
            found->second.result = std::move(completed);
        }
    }

    auto Store::take(uint64_t request_id) -> std::optional<Result>
    {
        std::lock_guard guard{s_mutex};
        auto found = s_entries.find(request_id);
        if (found == s_entries.end()) return std::nullopt;
        if (found->second.result.state == State::Queued || found->second.result.state == State::Capturing)
        {
            return found->second.result;
        }
        auto result = std::move(found->second.result);
        s_entries.erase(found);
        return result;
    }

    auto Store::cancel(uint64_t request_id) -> bool
    {
        std::lock_guard guard{s_mutex};
        return s_entries.erase(request_id) != 0;
    }

    auto Store::cancel_all() -> void
    {
        std::lock_guard guard{s_mutex};
        s_entries.clear();
    }

    auto Store::set_active_game_state(UObject* game_state, UObject* world) -> void
    {
        std::lock_guard guard{s_mutex};
        s_active_game_state = game_state;
        s_active_world = world;
    }

    auto Store::clear_active_game_state() -> void
    {
        std::lock_guard guard{s_mutex};
        s_active_game_state = nullptr;
        s_active_world = nullptr;
    }

    auto Store::resolve_active_game_state() -> UObject*
    {
        std::lock_guard guard{s_mutex};
        return ResolveActiveSnapshotRoot(s_active_game_state, s_active_world);
    }

    auto Store::push_value(const LuaMadeSimple::Lua& lua, const Value& value) -> void
    {
        if (!lua_checkstack(lua.get_lua_state(), 4))
            throw std::runtime_error{"insufficient Lua stack for snapshot result"};
        std::visit([&](const auto& item) {
            using T = std::decay_t<decltype(item)>;
            if constexpr (std::is_same_v<T, std::monostate>) lua.set_nil();
            else if constexpr (std::is_same_v<T, bool>) lua.set_bool(item);
            else if constexpr (std::is_same_v<T, int64_t>) lua.set_integer(item);
            else if constexpr (std::is_same_v<T, double>) lua.set_number(item);
            else if constexpr (std::is_same_v<T, std::string>) lua.set_string(item);
            else if constexpr (std::is_same_v<T, Value::Array>)
            {
                auto table = lua.prepare_new_table(static_cast<int32_t>(item.size()));
                for (size_t index = 0; index < item.size(); ++index)
                {
                    table.add_key(static_cast<int32_t>(index + 1));
                    push_value(lua, item[index]);
                    table.fuse_pair();
                }
                table.make_local();
            }
            else if constexpr (std::is_same_v<T, Value::Object>)
            {
                auto table = lua.prepare_new_table(static_cast<int32_t>(item.size()));
                for (const auto& [key, child] : item)
                {
                    table.add_key(key.c_str());
                    push_value(lua, child);
                    table.fuse_pair();
                }
                table.make_local();
            }
        }, value.data);
    }
}
