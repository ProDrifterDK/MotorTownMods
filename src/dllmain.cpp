#include "dllmain.h"

#include <Mod/CppUserModBase.hpp>
#include <Mod/LuaMod.hpp>
#include <LuaType/LuaUObject.hpp>
#include <LuaType/LuaUScriptStruct.hpp>
#include <Unreal/AGameModeBase.hpp>
#include <Unreal/FURL.hpp>
#include <Unreal/FWorldContext.hpp>
#include <Unreal/Hooks.hpp>
#include <Unreal/UEngine.hpp>
#include <Unreal/UFunction.hpp>
#include <Unreal/UScriptStruct.hpp>
#include <Unreal/UnrealInitializer.hpp>
#include <Unreal/World.hpp>
#include <Unreal/Property/FObjectProperty.hpp>
#include <Unreal/Property/FStrProperty.hpp>

#include <exception>

#include "webserver.h"
#include "statics.h"
#include "snapshot.h"

#include <algorithm>
#include <sstream>

using namespace RC;
using namespace RC::Unreal;


namespace
{
    auto require_game_thread(const LuaMadeSimple::Lua& lua, const char* function_name) -> void
    {
        if (!RC::LuaMod::is_in_game_thread())
        {
            lua.throw_error(std::string{function_name} + " may only read engine state on the GameThread");
        }
    }

    auto split_fields(std::string_view csv) -> std::vector<std::string>
    {
        std::vector<std::string> fields;
        std::stringstream stream{std::string{csv}};
        std::string field;
        while (std::getline(stream, field, ','))
        {
            if (!field.empty()) fields.emplace_back(std::move(field));
        }
        return fields;
    }

    auto request_game_state_snapshot(const LuaMadeSimple::Lua& lua) -> int
    {
        lua_State* state = lua.get_lua_state();
        const auto has_argument = [&](int index) {
            const int type = lua_type(state, index);
            return type != LUA_TNONE && type != LUA_TNIL;
        };
        if (lua_type(state, 1) != LUA_TSTRING) lua.throw_error("RequestGameStateSnapshot requires a query kind");
        MotorTown::Snapshot::Query query;
        const std::string kind{lua_tostring(state, 1)};
        if (kind == "vehicles") query.kind = MotorTown::Snapshot::Query::Kind::Vehicles;
        else if (kind == "players") query.kind = MotorTown::Snapshot::Query::Kind::Players;
        else lua.throw_error("unsupported snapshot query kind");

        if (has_argument(2) && lua_type(state, 2) != LUA_TSTRING) lua.throw_error("snapshot ID must be a string");
        if (lua_type(state, 2) == LUA_TSTRING)
        {
            const std::string id{lua_tostring(state, 2)};
            if (query.kind == MotorTown::Snapshot::Query::Kind::Vehicles && !id.empty())
            {
                const size_t digits_begin = id.front() == '-' ? 1 : 0;
                if (digits_begin == id.size() || !std::all_of(id.begin() + digits_begin, id.end(), [](unsigned char value) { return value >= '0' && value <= '9'; }))
                    lua.throw_error("invalid vehicle snapshot ID");
                try
                {
                    size_t consumed{};
                    const auto parsed = std::stoll(id, &consumed);
                    if (consumed != id.size()) lua.throw_error("invalid vehicle snapshot ID");
                    query.vehicle_id = parsed;
                }
                catch (const std::exception&) { lua.throw_error("invalid vehicle snapshot ID"); }
            }
            else query.player_id = id;
        }
        if (has_argument(3) && lua_type(state, 3) != LUA_TSTRING) lua.throw_error("snapshot fields must be a string");
        if (lua_type(state, 3) == LUA_TSTRING) query.fields = split_fields(lua_tostring(state, 3));
        if (has_argument(4) && !lua.is_integer(4)) lua.throw_error("snapshot limit must be an integer");
        if (lua.is_integer(4))
        {
            const auto limit = lua_tointeger(state, 4);
            if (limit < 1 || limit > 500) lua.throw_error("snapshot limit must be between 1 and 500");
            query.limit = static_cast<size_t>(limit);
        }
        if (has_argument(5) && !lua.is_bool(5)) lua.throw_error("snapshot controlled-only flag must be boolean");
        if (lua.is_bool(5)) query.controlled_only = lua_toboolean(state, 5) != 0;
        if (has_argument(6) && !lua.is_integer(6)) lua.throw_error("snapshot depth must be an integer");
        if (lua.is_integer(6))
        {
            const auto depth = lua_tointeger(state, 6);
            if (depth < 0 || depth > 8) lua.throw_error("snapshot depth must be between 0 and 8");
            query.limits.max_depth = static_cast<int32>(std::max<lua_Integer>(2, depth));
        }
        if (has_argument(7) && !lua.is_integer(7)) lua.throw_error("snapshot timeout must be an integer");
        const int64_t timeout_ms = lua.is_integer(7) ? lua_tointeger(state, 7) : 2000;
        if (timeout_ms < 50 || timeout_ms > 5000) lua.throw_error("snapshot timeout must be between 50 and 5000 ms");
        query.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);

        try
        {
            lua.set_integer(static_cast<int64_t>(MotorTown::Snapshot::Store::begin(std::move(query))));
            return 1;
        }
        catch (const std::exception& error)
        {
            lua.throw_error(error.what());
            return 0;
        }
    }

    auto capture_game_state_snapshot(const LuaMadeSimple::Lua& lua) -> int
    {
        // Run-13 RCA (D2): when this binding is invoked inside an
        // ExecuteInGameThread queued action and the game-thread authorization
        // fails, the former require_game_thread throw died inside the queued
        // action and left the Store entry Queued forever: every poll reported
        // "pending" until the caller's deadline, with no observable cause
        // (the UE4SS queued-action error line was absent from UE4SS.log).
        // Fail CLOSED instead: record a typed Store error so the poll returns
        // "error" with an actionable reason rather than a silent pending.
        if (!RC::LuaMod::is_in_game_thread())
        {
            if (lua.is_integer(1))
            {
                MotorTown::Snapshot::Store::fail(
                    static_cast<uint64_t>(lua.get_integer(1)),
                    "CaptureGameStateSnapshot ran outside the GameThread authorization (ExecuteInGameThread callback pumped without is_in_game_thread=true); the engine-state read was refused fail-closed");
            }
            lua.throw_error("CaptureGameStateSnapshot may only read engine state on the GameThread; the snapshot request was marked errored");
            return 0;
        }
        if (!lua.is_integer(1)) lua.throw_error("CaptureGameStateSnapshot requires a request ID");
        MotorTown::Snapshot::Store::capture(static_cast<uint64_t>(lua.get_integer(1)));
        return 0;
    }

    auto poll_game_state_snapshot(const LuaMadeSimple::Lua& lua) -> int
    {
        if (!lua.is_integer(1)) lua.throw_error("PollGameStateSnapshot requires a request ID");
        auto result = MotorTown::Snapshot::Store::take(static_cast<uint64_t>(lua.get_integer(1)));
        if (!result)
        {
            lua.set_string("missing");
            return 1;
        }
        if (result->state == MotorTown::Snapshot::State::Queued || result->state == MotorTown::Snapshot::State::Capturing)
        {
            lua.set_string("pending");
            return 1;
        }
        if (result->state == MotorTown::Snapshot::State::Error)
        {
            lua.set_string("error");
            lua.set_string(result->error);
            return 2;
        }
        lua.set_string("ready");
        try
        {
            MotorTown::Snapshot::Store::push_value(lua, result->value);
            return 2;
        }
        catch (const std::exception& error)
        {
            lua.throw_error(error.what());
            return 0;
        }
    }

    auto cancel_game_state_snapshot(const LuaMadeSimple::Lua& lua) -> int
    {
        if (!lua.is_integer(1)) lua.throw_error("CancelGameStateSnapshot requires a request ID");
        lua.set_bool(MotorTown::Snapshot::Store::cancel(static_cast<uint64_t>(lua.get_integer(1))));
        return 1;
    }
}

MotorTownMods::MotorTownMods()
	: CppUserModBase()
{
	ModName = ModStatics::GetModName();
	ModVersion = ModStatics::GetVersion();
	ModDescription = STR("Mods for Motor Town and Motor Town dedicated server");
	ModAuthors = STR("drpsyko101");
	// Do not change this unless you want to target a UE4SS version
	// other than the one you're currently building with somehow.
	// ModIntendedSDKVersion = STR("2.6");

	ModStatics::LogOutput(L"Mod loaded");
}

auto MotorTownMods::on_unreal_init() -> void
{
	// Lifecycle notification boundary (review-3 P1): the pinned dispatcher
	// (deps/first/Unreal/src/Hooks.cpp HookedLoadMap) breaks each callback
	// loop at the first callback whose result.first is true, Register*
	// appends, and nothing synchronizes registration with the live iteration
	// of the dispatcher-owned LoadMap callback vectors. This module therefore
	// registers normally and NEVER writes those vectors (an earlier pass
	// rotated this mod's callback to the front of the live vectors;
	// unsynchronized rotation of a dispatcher that may be iterating them can
	// skip this mod's callback and run a peer's twice). Delivery is treated
	// as delivered ONLY when it is delivered: if the pre callback below runs
	// and the matching post never does, the Store stays unable to serve (null
	// anchor, open travel interval) and every snapshot endpoint keeps
	// answering the typed 503 with the anchor-specific diagnostic until a
	// later delivered LoadMap refreshes the anchor (/status is unaffected and
	// stays available). No dispatcher-side ordering, priority, or
	// notification-coverage guarantee exists. If a competing registrar vetoes
	// delivery before this mod's pre callback, neither invalidation nor the
	// later anchor refresh reaches this module; the prior anchor may remain
	// usable even after the engine changes worlds. Closing that residual gap
	// needs a dispatcher-boundary mechanism outside the interruptible peer
	// lists and is recorded as a design gap in the lane report.
	Unreal::Hook::RegisterLoadMapPreCallback(
		[](Unreal::UEngine*, Unreal::FWorldContext&, Unreal::FURL, Unreal::UPendingNetGame*, Unreal::FString&) -> std::pair<bool, bool> {
			// Travel invalidation, delivered-only: when THIS callback runs, the
			// root cache and the current-world anchor drop together and the
			// travel interval opens, so no capture between this notification
			// and the matching post notification can serve or recover the
			// outgoing world's state. The anchor returns only when this mod's
			// post callback delivers the new GetThisCurrentWorld(); if no post
			// notification reaches this mod for that travel, the store stays
			// unable to serve (fail-closed typed 503, throttled
			// anchor-missing diagnostic), never stale data. Returns
			// {false, false}: this callback never vetoes, so it cannot end a
			// peer's callback loop either.
			MotorTown::Snapshot::Store::invalidate_travel_state();
			return {false, false};
		});
	// B1104 correction: the last delivered engine-established current-world
	// identity, captured at the only pinned moment it is handed to the mod.
	// RegisterLoadMapPostCallback fires after UEngine::LoadMap returns, so
	// FWorldContext::GetThisCurrentWorld() records the world made current by
	// that delivered LoadMap. Stored as a weak pointer and required by every
	// snapshot serve/recovery path: before any anchor has been delivered, when
	// the LoadMap hook is disabled (bHookLoadMap=false silently skips detour
	// installation), after a delivered pre lacks a delivered post, or when the
	// recorded world dies, the anchor is null/unresolvable and the endpoints
	// refuse fail-closed. This is not an independent freshness oracle: a peer
	// that vetoes before this mod's pre callback can leave the prior anchor
	// usable after a world change. GetThisCurrentWorld() throws when the pinned
	// engine version has no offset for it; that degrades a delivered refresh to
	// a null anchor, never to a guessed one. The refresh closes
	// exactly one open travel interval (Store bookkeeping), so a nested
	// inner LoadMap's post cannot restore serve authority while an
	// enclosing travel is still open.
	Unreal::Hook::RegisterLoadMapPostCallback(
		[](Unreal::UEngine*, Unreal::FWorldContext& world_context, Unreal::FURL, Unreal::UPendingNetGame*, Unreal::FString&) -> std::pair<bool, bool> {
			Unreal::UWorld* current_world = nullptr;
			try
			{
				current_world = world_context.GetThisCurrentWorld();
			}
			catch (const std::exception&)
			{
				current_world = nullptr;
			}
			MotorTown::Snapshot::Store::set_current_world(current_world);
			return {false, false};
		});
	// Measured B1104 dedicated-server boot behavior: this post callback was
	// registered, but it delivered zero times across the complete run. It does
	// not populate the active root there; the first snapshot request's on-demand
	// recovery is the operative population path.
	Unreal::Hook::RegisterInitGameStatePostCallback([](Unreal::AGameModeBase* context) {
		MotorTown::Snapshot::Store::clear_active_game_state();
		// Run-17c RCA diagnostics: every early-return branch must be observable
		// in UE4SS.log; the snapshot capture path otherwise fails closed with a
		// generic 'active MotorTownGameState is unavailable' that cannot
		// distinguish a missing registration from a world-identity mismatch.
		// LogLevel::Warning is pinned explicitly so the evidence survives
		// MOD_SERVER_LOG_LEVEL=2 canaries (Default=3 is suppressed there).
		if (!context)
		{
			ModStatics::LogOutput<LogLevel::Warning>(L"[SnapshotDiag] InitGameState post: context is null; active root cleared");
			return;
		}
		auto* game_state_property = context->GetPropertyByNameInChain(STR("GameState"));
		if (!game_state_property || !game_state_property->IsA<Unreal::FObjectProperty>())
		{
			const std::wstring class_name = context->GetClassPrivate()
				? context->GetClassPrivate()->GetFullName()
				: std::wstring{STR("<null>")};
			ModStatics::LogOutput<LogLevel::Warning>(L"[SnapshotDiag] InitGameState post: GameState property missing or not FObjectProperty on GameMode class {}",
				class_name);
			return;
		}
		auto* storage = game_state_property->ContainerPtrToValuePtr<void>(context);
		auto* game_state = static_cast<Unreal::FObjectProperty*>(game_state_property)->GetObjectPropertyValue(storage);
		auto* world = context->GetWorld();
		if (!game_state || !world || game_state->GetWorld() != world)
		{
			const std::wstring game_state_state = game_state ? STR("present") : STR("null");
			const std::wstring world_state = world ? STR("present") : STR("null");
			const std::wstring world_match = (game_state && world && game_state->GetWorld() == world) ? STR("true") : STR("false");
			ModStatics::LogOutput<LogLevel::Warning>(L"[SnapshotDiag] InitGameState post: invalid root (game_state={} world={} world_match={})",
				game_state_state,
				world_state,
				world_match);
			return;
		}
		MotorTown::Snapshot::Store::set_active_game_state(game_state, world);
		ModStatics::LogOutput<LogLevel::Normal>(L"[SnapshotDiag] InitGameState post: active root registered (game_state={} world={})",
			game_state->GetName(),
			world->GetName());
	});

	// B1104 dedicated 503 RCA discriminator: one-shot registration-side
	// evidence for the lifecycle hooks. The measured run reported both hooks
	// configured, the InitGameState signature resolved, both detour objects
	// present, and this post callback registered, yet emitted zero InitGameState
	// post lines across the complete dedicated-server boot. The game did not
	// invoke this callback in that boot; on-demand recovery populated the root.
	// In the pinned overlay (0a7434d / deps/first/Unreal 121f2ff), each field
	// below proves exactly one thing:
	//   hook_configured      = the UE4SS setting that gates the hook attempt.
	//   signature_ready/address = whether the engine function was resolved.
	//   detour_object_present = a detour object was allocated/configured.
	//   pre_callbacks/post_callbacks = the callback is registered in the
	//     pinned dispatcher vector (present even when hooking is disabled).
	// The fields alone do not prove that PolyHook installed the detour or that
	// a callback was delivered. This diagnostic does not query hook state, so
	// install success remains unreported. The zero post-line result above comes
	// from the complete runtime log, not from inferring delivery from these
	// fields. This status is pinned to LogLevel::Warning so it survives
	// MOD_SERVER_LOG_LEVEL=2. The same measured log retained the on-demand
	// recovery diagnostic while suppressing default-level startup output.
	const bool lifecycle_loadmap_hook_configured = Unreal::UnrealInitializer::StaticStorage::GlobalConfig.bHookLoadMap;
	const bool lifecycle_loadmap_detour_object_present = Unreal::Hook::StaticStorage::LoadMapDetour != nullptr;
	const size_t lifecycle_loadmap_pre_callbacks = Unreal::Hook::StaticStorage::LoadMapPreCallbacks.size();
	const size_t lifecycle_loadmap_post_callbacks = Unreal::Hook::StaticStorage::LoadMapPostCallbacks.size();
	const bool lifecycle_initgamestate_hook_configured = Unreal::UnrealInitializer::StaticStorage::GlobalConfig.bHookInitGameState;
	const bool lifecycle_signature_ready = Unreal::AGameModeBase::InitGameStateInternal.is_ready();
	const uint64_t lifecycle_signature_address = lifecycle_signature_ready ? reinterpret_cast<uint64_t>(Unreal::AGameModeBase::InitGameStateInternal.get_function_address()) : 0;
	const bool lifecycle_initgamestate_detour_object_present = Unreal::Hook::StaticStorage::InitGameStateDetour != nullptr;
	const size_t lifecycle_initgamestate_pre_callbacks = Unreal::Hook::StaticStorage::InitGameStatePreCallbacks.size();
	const size_t lifecycle_initgamestate_post_callbacks = Unreal::Hook::StaticStorage::InitGameStatePostCallbacks.size();
	ModStatics::LogOutput<LogLevel::Warning>(
		L"[SnapshotDiag] lifecycle hook status: loadmap hook_configured={} detour_object_present={} pre_callbacks={} post_callbacks={} initgamestate hook_configured={} signature_ready={} signature_address={:#x} detour_object_present={} pre_callbacks={} post_callbacks={} (field semantics: hook_configured=UE4SS config flag, signature_ready/address=engine function resolved, detour_object_present=detour object allocated, pre/post_callbacks=callback registered; none of these proves hook install, callback delivery, or the cause of a missing post line)",
		lifecycle_loadmap_hook_configured,
		lifecycle_loadmap_detour_object_present,
		lifecycle_loadmap_pre_callbacks,
		lifecycle_loadmap_post_callbacks,
		lifecycle_initgamestate_hook_configured,
		lifecycle_signature_ready,
		lifecycle_signature_address,
		lifecycle_initgamestate_detour_object_present,
		lifecycle_initgamestate_pre_callbacks,
		lifecycle_initgamestate_post_callbacks);

	// Init API server
	auto server = Webserver::Get();
}

auto MotorTownMods::on_lua_stop(
	LuaMadeSimple::Lua& lua,
	LuaMadeSimple::Lua& main_lua,
	LuaMadeSimple::Lua& async_lua,
	std::vector<LuaMadeSimple::Lua*>& hook_luas) -> void
{
	MotorTown::Snapshot::Store::cancel_all();
}

auto MotorTownMods::on_lua_start(
	LuaMadeSimple::Lua& lua,
	LuaMadeSimple::Lua& main_lua,
	LuaMadeSimple::Lua& async_lua,
	std::vector<LuaMadeSimple::Lua*>& hook_luas) -> void
{
	lua.register_function("RequestGameStateSnapshot", &request_game_state_snapshot);
	lua.register_function("CaptureGameStateSnapshot", &capture_game_state_snapshot);
	lua.register_function("PollGameStateSnapshot", &poll_game_state_snapshot);
	lua.register_function("CancelGameStateSnapshot", &cancel_game_state_snapshot);

	lua.register_function(
		"ExportStructAsText",
		[](const LuaMadeSimple::Lua& lua_net) -> int
		{
			require_game_thread(lua_net, "ExportStructAsText");
			int32_t stack_size = lua_net.get_stack_size();

			if (stack_size <= 1)
			{
				lua_net.throw_error("Function 'UScriptStruct:GetStructTextItem' cannot be called with 1 parameters.");
			}

			auto& object = lua_net.get_userdata<RC::LuaType::UObject>();
			auto propName = lua_net.get_string();
			auto ptr = object.get_remote_cpp_object();
			if (ptr)
			{
				auto uniqueIdProp = static_cast<FStructProperty*>(ptr->GetPropertyByNameInChain(to_wstring(propName).c_str()));
				if (uniqueIdProp)
				{
					auto uniqueIdStruct = uniqueIdProp->GetStruct();
					auto uniqueId = uniqueIdProp->ContainerPtrToValuePtr<void>(ptr);

					FString uniqueIdString;
					uniqueIdProp->ExportTextItem(uniqueIdString, uniqueId, nullptr, ptr, 0);
					lua_net.set_string(to_string(*uniqueIdString));
					return 1;
				}
			}
			lua_net.set_string("");

			return 1;
		});

	lua.register_function(
		"GetObjectVariables",
		[](const LuaMadeSimple::Lua& _lua) -> int
		{
			require_game_thread(_lua, "GetObjectVariables");
			const int stack_size = _lua.get_stack_size();
			if (stack_size < 1)
			{
				_lua.throw_error("Function 'UScriptStruct:GetStructTextItem' cannot be called with 0 parameters.");
			}

			// Get the UOject from the 1st parameter
			auto& object = _lua.get_userdata<RC::LuaType::UObject>();
			auto ptr = object.get_remote_cpp_object();

			std::wstring propertyName, className;
			int32 depth = 0;

			// Parse parameter values
			if (_lua.is_integer(3)) depth = _lua.get_integer(3);
			if (_lua.is_string(2)) className = to_wstring(_lua.get_string(2));
			if (_lua.is_string()) propertyName = to_wstring(_lua.get_string());

			for (int i = 0; i < _lua.get_stack_size(); i++)
			{
				_lua.discard_value();
			}

			auto table = _lua.prepare_new_table();
			table.set_has_userdata(false);

			if (ptr)
			{
				auto ptrClass = ptr->GetClassPrivate();
				if (!propertyName.empty())
				{
					auto prop = ptr->GetPropertyByNameInChain(propertyName.c_str());
					if (prop)
					{
						// Allow object conversion only when parameter name is specified
						ModStatics::ExportPropertyAsTable(prop, ptr, table, PropertyType::None, depth);
					}
					else
					{
						_lua.throw_error("Property name " + to_string(propertyName) + " invalid");
					}
				}
				else
				{
					for (FProperty* prop = ptrClass->GetPropertyLink(); prop; prop = prop->GetPropertyLinkNext())
					{
						if (className.empty())
						{
							className = ptrClass->GetName();
						}

						// Crude way to check the owner class since FProperty::GetOwnerClass isn't supported
						if (prop->GetFullName().contains(className))
						{
							ModStatics::ExportPropertyAsTable(prop, ptr, table, PropertyType::None, depth);
						}
					}
				}
			}

			table.make_local();
			return 1;
		});

	lua.register_function(
		"NativeSleep",
		[](const LuaMadeSimple::Lua& _lua)
		{
			if (!_lua.is_integer())
			{
				_lua.throw_error("Sleep function only accept an integer parameter.");
			}

			const int duration = _lua.get_integer();
			Sleep(duration);
			return 1;
		});

	lua.register_function(
		"GetStructVariables",
		[](const LuaMadeSimple::Lua& _lua)
		{
			require_game_thread(_lua, "GetStructVariables");
			ModStatics::LogOutput<LogLevel::Verbose>(L"stackSize: {}", _lua.get_stack_size());
			if (!_lua.is_userdata())
			{
				_lua.throw_error("Invalid argument passed.");
			}

			auto& param = _lua.get_userdata<LuaType::UScriptStruct>();
			auto& wrapper = param.get_local_cpp_object();

			int depth = 0;
			if (_lua.is_integer())
			{
				depth = _lua.get_integer();
			}

			auto table = _lua.prepare_new_table();
			if (auto scriptStruct = wrapper.script_struct)
			{
				auto data = wrapper.get_data_ptr();
				for (FProperty* prop = scriptStruct->GetPropertyLink(); prop; prop = prop->GetPropertyLinkNext())
				{
					ModStatics::ExportPropertyAsTable(prop, data, table, PropertyType::None, depth);
				}
			}
			else
			{
				_lua.throw_error("Invalid struct value.");
			}

			table.make_local();
			return 1;
		});
}
