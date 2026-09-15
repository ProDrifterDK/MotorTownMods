#include "snapshot.h"
#include "active_snapshot_root.h"

#include <Mod/LuaMod.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/World.hpp>

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <unordered_set>

#include "statics.h"
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
    FWeakObjectPtr Store::s_current_world{};
    uint64_t Store::s_travel_generation{0};
    uint64_t Store::s_open_travels{0};

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
            // Gameplay-validity per the pinned engine's own internal-flag rule
            // (UnrealFlags.hpp: PendingKill = "invalid for gameplay but valid
            // objects"). FindAllOf does not remove PendingKill objects and only
            // FWeakObjectPtr::Get rejects them, so this predicate gates the
            // objects that carry runtime state: recovery candidates, their
            // worlds and authority GameModes, the cached root/world, top-level
            // Vehicle/PlayerArray elements, and object-reference targets.
            // Scope limit: metadata reads that only observe UClass/UStruct
            // objects (GetClassPrivate(), GetStruct() property-link walks,
            // IsA<> checks) are NOT individually gated by this predicate.
            return object && !object->IsUnreachable() &&
                   !object->HasAnyInternalFlags(EInternalObjectFlags::PendingKill) &&
                   !object->HasAnyFlags(static_cast<EObjectFlags>(RF_BeginDestroyed | RF_FinishDestroyed));
        }

        auto read_object_property(UObject* object, const wchar_t* property_name) -> UObject*
        {
            // Same reflection read as the InitGameState registration path in
            // dllmain.cpp: a missing or non-object property yields nullptr so
            // the caller's authority chain fails closed.
            auto* property = object->GetPropertyByNameInChain(property_name);
            if (!property || !property->IsA<FObjectProperty>()) return nullptr;
            return static_cast<FObjectProperty*>(property)->GetObjectPropertyValue(
                property->ContainerPtrToValuePtr<void>(object));
        }

        auto log_snapshot_diag_throttled(std::wstring key, std::wstring message) -> void
        {
            // Failure-path diagnostics only: repeats are throttled so a polled
            // endpoint cannot flood UE4SS.log, while the first occurrence of a
            // state stays visible. LogLevel::Normal is pinned explicitly so
            // the evidence survives MOD_SERVER_LOG_LEVEL=2 (the frozen canary
            // level suppresses the LogLevel::Default template argument).
            static std::mutex diag_mutex{};
            static std::map<std::wstring, std::chrono::steady_clock::time_point> last_emitted{};
            {
                std::lock_guard guard{diag_mutex};
                const auto now = std::chrono::steady_clock::now();
                auto found = last_emitted.find(key);
                if (found != last_emitted.end() && now - found->second < std::chrono::seconds{30}) return;
                last_emitted[key] = now;
            }
            ModStatics::LogOutput<LogLevel::Normal>(L"[SnapshotDiag] {}", message);
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

    auto Store::fail(uint64_t request_id, std::string error) -> bool
    {
        // Run-13 RCA (D2): a queued game-thread callback that cannot capture
        // must leave an OBSERVABLE terminal state, never a silent Queued entry
        // that every poll reports as "pending" until the caller's deadline.
        // Only a Queued entry can be failed: a Capturing entry belongs to
        // Store::capture, and a terminal state is already observable.
        std::lock_guard guard{s_mutex};
        auto found = s_entries.find(request_id);
        if (found == s_entries.end() || found->second.result.state != State::Queued) return false;
        found->second.result = Result{State::Error, {}, std::move(error)};
        return true;
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

    auto Store::invalidate_travel_state() -> void
    {
        // LoadMap pre notification (dllmain.cpp), delivered-only: the outgoing
        // world's root cache and the current-world anchor are dropped together
        // under the same lock, the monotonic generation advances, and the open
        // travel count rises. Recovery is anchored to s_current_world, so
        // letting the old anchor stay live here would let recovery re-admit
        // the old world's root during the travel window and let the serve gate
        // accept it (cached world == stale anchor). Delivered-only boundary
        // (review-3): the pinned dispatcher gives this module no delivery
        // guarantee, so lifecycle authority is treated as delivered only when
        // a callback below actually runs. If the matching post never arrives
        // (an earlier-registered peer ended the interruptible loop first, the
        // LoadMap hook never became active, or a travel bypasses UEngine::
        // LoadMap entirely), the anchor stays null/unresolvable and
        // s_open_travels stays above zero: every snapshot endpoint keeps
        // refusing fail-closed with the typed 503 and the anchor-specific
        // diagnostic below. No dispatcher-side ordering or coverage guarantee
        // exists or is claimed; providing one is a dispatcher-boundary change
        // outside this module (recorded as a design gap in the lane report).
        std::lock_guard guard{s_mutex};
        s_active_game_state = nullptr;
        s_active_world = nullptr;
        s_current_world = nullptr;
        ++s_travel_generation;
        ++s_open_travels;
    }

    auto Store::set_current_world(UObject* world) -> void
    {
        // LoadMap post notification (dllmain.cpp), delivered-only: this runs
        // only if this mod's post callback actually ran. A refresh that
        // degrades to null (GetThisCurrentWorld threw for the pinned engine
        // version) still advances the generation: the pre-travel state stays
        // refused, never silently re-armed. Closing exactly ONE open travel
        // interval keeps a nested inner LoadMap's post from restoring serve
        // authority while an enclosing travel is still open.
        std::lock_guard guard{s_mutex};
        s_current_world = world;
        ++s_travel_generation;
        if (s_open_travels > 0) --s_open_travels;
    }

    auto Store::sample_lifecycle_state() -> SnapshotLifecycleState
    {
        // Caller holds s_mutex. The anchor contributes its RESOLVED identity;
        // per the pinned FWeakObjectPtr::Get default validity (verified
        // against deps/first/Unreal/src/FWeakObjectPtr.cpp + UObjectArray.cpp)
        // a null/stale-serial, Unreachable or PendingKill world resolves to
        // null, which the seams treat as unavailable authority. Anchors that
        // only weak resolution keeps alive are refused again by
        // object_is_readable (RF_BeginDestroyed/RF_FinishDestroyed are NOT
        // weak checks) inside the serve gate.
        return SnapshotLifecycleState{
            static_cast<const void*>(s_current_world.Get()),
            s_travel_generation,
            s_open_travels,
        };
    }

    auto Store::store_active_game_state_if_current(const SnapshotLifecycleState& sampled, UObject* game_state, UObject* world) -> bool
    {
        // Final decision point for the recovery path (review-3 P2): the
        // admission decision and the store share one critical section, so a
        // concurrent invalidate_travel_state/set_current_world cannot land
        // between the check and the store. The decision itself is the pure
        // seam the tests execute; a generation that advanced after the
        // resolution's entry sample refuses the store entirely.
        std::lock_guard guard{s_mutex};
        if (!AdmissionStillValid(sampled, sample_lifecycle_state())) return false;
        s_active_game_state = game_state;
        s_active_world = world;
        return true;
    }

    auto Store::resolve_active_game_state() -> UObject*
    {
        // Entry preconditions and every lifecycle admission decision below run
        // through the pure seams in active_snapshot_root.h
        // (EvaluateResolutionPreconditions / AdmissionStillValid), which the
        // test suite executes directly; production supplies the live inputs.
        const bool on_game_thread = LuaMod::is_in_game_thread();
        // The thread check must win before ANY engine read: sampling resolves
        // the anchor weak pointer (engine object-array state), so an
        // unauthorized caller leaves the sample all-unavailable and is refused
        // by the seam's first case without the read ever happening (Run-13 D2
        // defect class).
        SnapshotLifecycleState sampled;
        if (on_game_thread)
        {
            std::lock_guard guard{s_mutex};
            sampled = sample_lifecycle_state();
        }
        switch (EvaluateResolutionPreconditions(on_game_thread, sampled))
        {
            case SnapshotResolutionRefusal::OffGameThread:
            {
                log_snapshot_diag_throttled(L"off-thread",
                    L"active root resolution refused: not on the GameThread");
                return nullptr;
            }
            case SnapshotResolutionRefusal::AnchorMissing:
            {
                // Anchor-specific, observable at LogLevel::Normal. The anchor
                // is null OR its weak pointer no longer resolves; the causes
                // are listed, not diagnosed: a delivered LoadMap pre
                // invalidated it and no matching post has been delivered to
                // this mod (interrupted loop, inactive hook), the refreshed
                // anchor never resolved, or the refreshed world died. Fail
                // closed, never serve stale.
                log_snapshot_diag_throttled(L"anchor-missing",
                    L"active root unavailable: current-world anchor missing or unresolvable (a delivered LoadMap pre invalidated it and no matching post has re-established a resolvable anchor, or the LoadMap hook is inactive)");
                return nullptr;
            }
            case SnapshotResolutionRefusal::TravelInProgress:
            {
                // Non-null anchor but a travel interval is still open (e.g. a
                // nested inner LoadMap's post refreshed the anchor while the
                // enclosing travel runs). Refuse until the enclosing post is
                // delivered; never serve across an open travel.
                log_snapshot_diag_throttled(L"travel-open",
                    L"active root unavailable: a LoadMap travel interval is open (pre delivered, matching post not yet delivered to this mod); refusing until lifecycle authority is delivered");
                return nullptr;
            }
            case SnapshotResolutionRefusal::Proceed:
                break;
        }

        const auto read_authority_mode = [](UObject* world) { return read_object_property(world, STR("AuthorityGameMode")); };
        const auto read_mode_game_state = [](UObject* game_mode) { return read_object_property(game_mode, STR("GameState")); };

        // Cached fast path first: recovery is the failure path, never a
        // per-request scan. The validity check and the weak gate run under ONE
        // lock acquisition, so a concurrent travel writer cannot land between
        // them; a sample overtaken by an invalidation or refresh (generation
        // advanced) is never served here. The gate re-reads the authority
        // backlink through reflection on every serve, so a pair whose world
        // lost its authority (or whose GameMode re-pointed its GameState) is
        // refused too.
        const auto serve_cached = [&]() -> UObject* {
            std::lock_guard guard{s_mutex};
            const auto live = sample_lifecycle_state();
            if (!AdmissionStillValid(sampled, live)) return nullptr;
            return ResolveServedSnapshotRoot(
                s_active_game_state, s_active_world, live.current_world,
                [](UObject* object) { return object_is_readable(object); },
                read_authority_mode,
                read_mode_game_state);
        };
        if (auto* cached = serve_cached()) return cached;

        // If the lifecycle state advanced after the entry sample (a travel
        // interval opened or completed between sampling and here), the sampled
        // anchor identity is stale: refuse this resolution instead of scanning
        // against it. The next request re-samples fresh state; nothing stale
        // is scanned, stored or returned.
        bool lifecycle_advanced = false;
        {
            std::lock_guard guard{s_mutex};
            lifecycle_advanced = !AdmissionStillValid(sampled, sample_lifecycle_state());
        }
        if (lifecycle_advanced)
        {
            log_snapshot_diag_throttled(L"lifecycle-changed",
                L"active root resolution refused: the travel lifecycle advanced after the resolution sampled it (invalidation or anchor refresh); the next request re-samples current state");
            return nullptr;
        }

        const auto decision = RecoverActiveSnapshotRoot<UObject>(
            on_game_thread, sampled.current_world,
            [&]() {
                std::vector<UObject*> found;
                Unreal::UObjectGlobals::FindAllOf(STR("MotorTownGameState"), found);
                return found;
            },
            [](UObject* object) { return object_is_readable(object); },
            [](UObject* object) { return object->GetWorld(); },
            read_authority_mode,
            read_mode_game_state);

        switch (decision.outcome)
        {
            case SnapshotRecoveryOutcome::Unresolved:
                log_snapshot_diag_throttled(L"unresolved",
                    L"active root recovery failed: no candidate passed the current-world authority chain");
                return nullptr;
            case SnapshotRecoveryOutcome::Ambiguous:
                log_snapshot_diag_throttled(L"ambiguous",
                    L"active root recovery failed: multiple chain-admissible roots; refusing instead of guessing");
                return nullptr;
            case SnapshotRecoveryOutcome::OffGameThread:
            case SnapshotRecoveryOutcome::NoCurrentWorld:
                // Unreachable: EvaluateResolutionPreconditions enforced both
                // preconditions at entry (the pure decision re-checks them
                // before its scan) and this is the same sampled-once thread
                // flag.
                return nullptr;
            case SnapshotRecoveryOutcome::Recovered:
                break;
        }

        // Same ordering discipline as the serve gate, through the pure seam the
        // tests execute (EvaluateRecoveredTail): the recovered root passes a
        // fresh gameplay-readability check BEFORE the engine world read
        // (GetWorld() is an engine virtual wrapper) runs on it, and the world
        // is gated too, so nothing unreadable reaches the GetName() engine
        // reads in the recovery diagnostic or the cache below.
        const auto tail = EvaluateRecoveredTail(
            decision.admitted,
            [](UObject* object) { return object_is_readable(object); },
            [](UObject* object) -> UObject* { return object->GetWorld(); });
        switch (tail.verdict)
        {
            case RecoveredTailVerdict::RootUnreadable:
                log_snapshot_diag_throttled(L"recovered-unreadable",
                    L"active root recovery aborted: recovered root became unreadable before caching");
                return nullptr;
            case RecoveredTailVerdict::WorldMissingOrUnreadable:
                log_snapshot_diag_throttled(L"recovered-unreadable",
                    L"active root recovery aborted: recovered root's world is missing or unreadable before caching");
                return nullptr;
            case RecoveredTailVerdict::Cacheable:
                break;
        }

        // Final decision point (review-3 P2): check-and-store share one
        // critical section, so a pair admitted against generation N is never
        // STORED once the generation advanced.
        if (!store_active_game_state_if_current(sampled, decision.admitted, tail.world))
        {
            log_snapshot_diag_throttled(L"lifecycle-changed",
                L"active root recovery discarded: the travel lifecycle advanced between the scan and the cache; the next request recovers against the refreshed state");
            return nullptr;
        }
        log_snapshot_diag_throttled(L"recovered",
            std::wstring{L"active root recovered on demand (game_state="} + decision.admitted->GetName() +
            L" world=" + tail.world->GetName() + L")");
        // The caller observes only what the weak-cache gate re-resolves NOW:
        // serve_cached re-validates generation/anchor/open-travels under one
        // lock before returning, so a pair admitted against generation N is
        // never RETURNED after the generation advanced. That gate refuses any
        // weak-serial/unreadable state visible at the check itself; it does
        // not pin the objects, so a GC or PendingKill that lands after the
        // check is not modeled here. The recovered raw pointer never escapes
        // this function directly.
        return serve_cached();
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
