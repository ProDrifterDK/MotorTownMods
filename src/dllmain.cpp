#include "dllmain.h"

#include <Mod/CppUserModBase.hpp>
#include <Mod/LuaMod.hpp>
#include <LuaType/LuaUObject.hpp>
#include <LuaType/LuaUScriptStruct.hpp>
#include <Unreal/UFunction.hpp>
#include <Unreal/UScriptStruct.hpp>
#include <Unreal/Property/FStrProperty.hpp>

#include "webserver.h"
#include "statics.h"
#include "snapshot.h"

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
        if (!lua.is_string(1)) lua.throw_error("RequestGameStateSnapshot requires a query kind");
        MotorTown::Snapshot::Query query;
        const std::string kind{lua_tostring(state, 1)};
        if (kind == "vehicles") query.kind = MotorTown::Snapshot::Query::Kind::Vehicles;
        else if (kind == "players") query.kind = MotorTown::Snapshot::Query::Kind::Players;
        else lua.throw_error("unsupported snapshot query kind");

        if (lua.is_string(2))
        {
            const std::string id{lua_tostring(state, 2)};
            if (query.kind == MotorTown::Snapshot::Query::Kind::Vehicles && !id.empty())
            {
                try { query.vehicle_id = std::stoll(id); }
                catch (...) { lua.throw_error("invalid vehicle snapshot ID"); }
            }
            else query.player_id = id;
        }
        if (lua.is_string(3)) query.fields = split_fields(lua_tostring(state, 3));
        if (lua.is_integer(4)) query.limit = static_cast<size_t>(lua_tointeger(state, 4));
        if (lua.is_bool(5)) query.controlled_only = lua_toboolean(state, 5) != 0;
        if (lua.is_integer(6))
        {
            const auto depth = lua_tointeger(state, 6);
            if (depth < 0 || depth > 8) lua.throw_error("snapshot depth must be between 0 and 8");
            query.limits.max_depth = static_cast<int32>(std::max<lua_Integer>(2, depth));
        }
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
        require_game_thread(lua, "CaptureGameStateSnapshot");
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
