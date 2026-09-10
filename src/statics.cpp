#include "statics.h"
#include "container_iteration.h"
#include <limits>
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UScriptStruct.hpp>
#include <Unreal/Property/FStructProperty.hpp>
#include <Unreal/Property/FStrProperty.hpp>
#include <Unreal/Property/FNameProperty.hpp>
#include <Unreal/Property/FTextProperty.hpp>
#include <Unreal/Property/NumericPropertyTypes.hpp>
#include <Unreal/Property/FBoolProperty.hpp>
#include <Unreal/Property/FArrayProperty.hpp>
#include <Unreal/Property/FEnumProperty.hpp>
#include <Unreal/Property/FObjectProperty.hpp>
#include <Unreal/Property/FMapProperty.hpp>
#include <Unreal/Property/FSetProperty.hpp>
#include <LuaType/LuaUObject.hpp>

int ModStatics::GetLogLevel()
{
	const auto lvl = std::getenv("MOD_SERVER_LOG_LEVEL");
	if (lvl) return std::atoi(lvl);

	return 2;
}

const std::string ModStatics::GetWebhookUrl()
{
	std::string test = getenv("MOD_WEBHOOK_URL");
	return getenv("MOD_WEBHOOK_URL");
}

void ModStatics::ExportPropertyAsTable(
	FProperty* property,
	void* data,
	Lua::Table& table,
	const PropertyType propertyType,
	const int32 depth,
	const bool valueAddress)
{
	if (!property || !data) return;
	void* propertyStorage = valueAddress ? data : property->ContainerPtrToValuePtr<void>(data);
	if (!propertyStorage) return;

	// Limit recursive depth search
	std::vector<int> empty;
	if (depth < 0)
	{
		throw std::format_error("Depth limit reached");
	}

	auto propName = to_string(property->GetName());
	std::wstring propWName = to_wstring(property->GetName());
	std::wstring propClass = to_wstring(property->GetClass().GetName());

	if (property->IsA<FStrProperty>())
	{
		auto propertyValue = static_cast<FString*>(propertyStorage);
		const auto str = to_string(**propertyValue);
		switch (propertyType)
		{
		case PropertyType::Array:
			table.add_value(str.c_str());
			break;
		case PropertyType::Map:
			table.add_key(str.c_str());
			break;
		default:
			table.add_pair(propName.c_str(), str.c_str());
		}
	}
	else if (property->IsA<FNameProperty>())
	{
		auto propertyValue = static_cast<FName*>(propertyStorage);
		const auto str = propertyValue->ToString();
		switch (propertyType)
		{
		case PropertyType::Array:
			table.add_value(to_string(str).c_str());
			break;
		case PropertyType::Map:
			table.add_key(to_string(str).c_str());
			break;
		default:
			table.add_pair(propName.c_str(), to_string(str).c_str());
		}
	}
	else if (property->IsA<FTextProperty>())
	{
		auto propertyValue = static_cast<FText*>(propertyStorage);
		const auto str = propertyValue->ToString();
		switch (propertyType)
		{
		case PropertyType::Array:
			table.add_value(to_string(str).c_str());
			break;
		case PropertyType::Map:
			table.add_key(to_string(str).c_str());
			break;
		default:
			table.add_pair(propName.c_str(), to_string(str).c_str());
		}
	}
	else if (property->IsA<FFloatProperty>())
	{
		const auto propertyValue = *static_cast<float*>(propertyStorage);
		switch (propertyType)
		{
		case PropertyType::Array:
			table.add_value(propertyValue);
			break;
		case PropertyType::Map:
			table.add_key(std::to_string(propertyValue));
			break;
		default:
			table.add_pair(propName.c_str(), propertyValue);
		}
	}
	else if (property->IsA<FDoubleProperty>())
	{
		const auto propertyValue = *static_cast<double*>(propertyStorage);
		switch (propertyType)
		{
		case PropertyType::Array:
			table.add_value(propertyValue);
			break;
		case PropertyType::Map:
			table.add_key(std::to_string(propertyValue));
			break;
		default:
			table.add_pair(propName.c_str(), propertyValue);
		}
	}
	else if (property->IsA<FIntProperty>() || property->IsA<FInt64Property>() || property->IsA<FUInt32Property>() || property->IsA<FUInt64Property>() || property->IsA<FInt8Property>() || property->IsA<FInt16Property>())
	{
		auto numericProperty = static_cast<FNumericProperty*>(property);
		auto valuePtr = propertyStorage;
		const bool isUnsigned = property->IsA<FUInt32Property>() || property->IsA<FUInt64Property>();
		const uint64 unsignedValue = isUnsigned ? numericProperty->GetUnsignedIntPropertyValue(valuePtr) : 0;
		if (isUnsigned && unsignedValue > static_cast<uint64>(std::numeric_limits<int64>::max()))
			throw std::format_error("Unsigned integer exceeds Lua integer range");
		const int64 propertyValue = isUnsigned
			? static_cast<int64>(unsignedValue)
			: numericProperty->GetSignedIntPropertyValue(valuePtr);
		switch (propertyType)
		{
		case PropertyType::Array:
			table.add_value(propertyValue);
			break;
		case PropertyType::Map:
			table.add_key(std::to_string(propertyValue));
			break;
		default:
			table.add_pair(propName.c_str(), propertyValue);
		}
	}
	else if (property->IsA<FEnumProperty>() || property->IsA<FByteProperty>())
	{
		const auto propertyValueRaw = *static_cast<uint8*>(propertyStorage);
		const int propertyValue = static_cast<int>(propertyValueRaw);
		switch (propertyType)
		{
		case PropertyType::Array:
			table.add_value(propertyValue);
			break;
		case PropertyType::Map:
			table.add_key(std::to_string(propertyValue).c_str());
			break;
		default:
			table.add_pair(propName.c_str(), propertyValue);
		}
	}
	else if (property->IsA<FUInt16Property>())
	{
		const auto propertyValueRaw = *static_cast<uint16*>(propertyStorage);
		const int propertyValue = static_cast<int>(propertyValueRaw);
		switch (propertyType)
		{
		case PropertyType::Array:
			table.add_value(propertyValue);
			break;
		case PropertyType::Map:
			table.add_key(std::to_string(propertyValue).c_str());
			break;
		default:
			table.add_pair(propName.c_str(), propertyValue);
		}
	}
	else if (property->IsA<FBoolProperty>())
	{
		auto boolProperty = static_cast<FBoolProperty*>(property);
		const auto propertyValue = boolProperty->GetPropertyValue(propertyStorage);
		switch (propertyType)
		{
		case PropertyType::Array:
			table.add_value(propertyValue);
			break;
		case PropertyType::Map:
			table.add_key(propertyValue);
			break;
		default:
			table.add_pair(propName.c_str(), propertyValue);
		}
	}
	else if (property->IsA<FStructProperty>())
	{
		// Table::add_key only supports char, int, unsigned int
		if (propertyType == PropertyType::Map)
			throw std::format_error("Unable to set struct as TMap key");

		auto _prop = static_cast<FStructProperty*>(property);
		auto _struct = _prop->GetStruct();
		auto structName = _struct->GetName();

		if (structName == STR("Vector"))
		{

			auto propertyValue = static_cast<FVector*>(propertyStorage);
			if (propertyValue)
			{
				if (propertyType != PropertyType::Array)
					table.add_key(propName.c_str());

				auto inner_table = table.get_lua_instance().prepare_new_table();
				inner_table.add_pair("X", propertyValue->GetX());
				inner_table.add_pair("Y", propertyValue->GetY());
				inner_table.add_pair("Z", propertyValue->GetZ());
				inner_table.make_local();

				if (propertyType != PropertyType::Array)
					table.fuse_pair();
			}
		}
		else if (structName == STR("Rotator"))
		{
			auto propertyValue = static_cast<FRotator*>(propertyStorage);

			if (propertyType != PropertyType::Array)
				table.add_key(propName.c_str());

			auto inner_table = table.get_lua_instance().prepare_new_table();
			inner_table.add_pair("Pitch", propertyValue->GetPitch());
			inner_table.add_pair("Roll", propertyValue->GetRoll());
			inner_table.add_pair("Yaw", propertyValue->GetYaw());
			inner_table.make_local();

			if (propertyType != PropertyType::Array)
				table.fuse_pair();
		}
		else if (structName == STR("Guid"))
		{
			FString value;
			auto propertyValue = propertyStorage;
			property->ExportTextItem(value, propertyValue, nullptr, static_cast<UObject*>(data), 0);
			const auto str = to_string(*value);
			switch (propertyType)
			{
			case PropertyType::Array:
				table.add_value(str.c_str());
				break;
			default:
				table.add_pair(propName.c_str(), str.c_str());
			}
		}
		else
		{
			auto propertyValue = propertyStorage;
			auto structProp = static_cast<FStructProperty*>(property);

			if (propertyType != PropertyType::Array)
				table.add_key(propName.c_str());

			if (propertyValue && structProp)
			{
				auto inner_table = table.get_lua_instance().prepare_new_table();
				for (FProperty* innerProp = structProp->GetStruct()->GetPropertyLink(); innerProp; innerProp = innerProp->GetPropertyLinkNext())
				{
					ExportPropertyAsTable(innerProp, propertyValue, inner_table, PropertyType::None, depth);
				}
				inner_table.make_local();
			}

			if (propertyType != PropertyType::Array)
				table.fuse_pair();
		}
	}
	else if (property->IsA<FArrayProperty>())
	{
		if (propertyType == PropertyType::Map)
			throw std::format_error("Unable to set array as TMap key");
		if (propertyType == PropertyType::Array)
			throw std::format_error("Unable to set array within an array");

		auto _prop = static_cast<FArrayProperty*>(property);
		auto propertyValue = static_cast<FScriptArray*>(propertyStorage);

		table.add_key(propName.c_str());
		auto innerTable = table.get_lua_instance().prepare_new_table();

		if (!propertyValue || !propertyValue->GetData())
		{
			innerTable.vector_to_table(empty);
			innerTable.make_local();
			table.fuse_pair();
			return;
		}

		auto innerProp = _prop->GetInner();
		const int32 elemSize = innerProp->GetElementSize();
		const int32 arrayCount = propertyValue->Num();

		if (arrayCount > 0)
		{
			for (int32_t i = 0; i < arrayCount; i++)
			{
				innerTable.add_key(i + 1);
				const int32 offset = i * elemSize;
				auto elem = static_cast<uint8*>(propertyValue->GetData()) + offset;
				try
				{
					ExportPropertyAsTable(innerProp, elem, innerTable, PropertyType::Array, depth, true);
				}
				catch (const std::exception&)
				{
					auto emptyInnerTable = innerTable.get_lua_instance().prepare_new_table();
					emptyInnerTable.vector_to_table(empty);
					emptyInnerTable.make_local();
				}
				innerTable.fuse_pair();
			}
		}
		else
		{
			innerTable.vector_to_table(empty);
		}
		innerTable.make_local();
		table.fuse_pair();
	}
	else if (property->IsA<FObjectProperty>())
	{
		if (propertyType == PropertyType::Map)
			throw std::format_error("Unable to set object as TMap key");
		if (propertyType == PropertyType::Array)
			throw std::format_error("Unable to explicitly iterate array");

		auto propertyValue = *static_cast<UObject**>(propertyStorage);

		if (propertyValue)
		{
			auto propertyClass = propertyValue->GetClassPrivate();
			if (propertyType != PropertyType::Array)
				table.add_key(propName.c_str());

			auto innerTable = table.get_lua_instance().prepare_new_table();
			try
			{
				for (FProperty* innerProp = propertyClass->GetPropertyLink(); innerProp; innerProp = innerProp->GetPropertyLinkNext())
				{
					ExportPropertyAsTable(innerProp, propertyValue, innerTable, PropertyType::None, depth - 1);
				}
			}
			catch (std::exception&)
			{
				innerTable.vector_to_table(empty);
			}
			innerTable.make_local();

			if (propertyType != PropertyType::Array)
				table.fuse_pair();
		}
	}
	else if (property->IsA<FMapProperty>())
	{
		if (propertyType == PropertyType::Map)
			throw std::format_error("Unable to set TMap as TMap key");
		if (propertyType == PropertyType::Array)
			throw std::format_error("Unable to iterate TMap");

		auto innerProp = static_cast<FMapProperty*>(property);
		auto propertyValue = static_cast<FScriptMap*>(propertyStorage);

		if (innerProp && propertyValue)
		{
			table.add_key(propName.c_str());
			auto innerTable = table.get_lua_instance().prepare_new_table();

			const int32 mapSize = propertyValue->GetMaxIndex();

			if (mapSize > 0)
			{
				auto keyProp = innerProp->GetKeyProp();
				auto valueProp = innerProp->GetValueProp();
				auto layout = Unreal::FScriptMap::GetScriptLayout(
					keyProp->GetSize(),
					keyProp->GetMinAlignment(),
					valueProp->GetSize(),
					valueProp->GetMinAlignment());

				for (int32 i = 0; i < mapSize; ++i)
				{
					if (!propertyValue->IsValidIndex(i)) continue;
					auto elem = static_cast<uint8*>(propertyValue->GetData(i, layout));
					try
					{
						ExportPropertyAsTable(keyProp, elem, innerTable, PropertyType::Map, depth, true);
					}
					catch (std::exception& err)
					{
						LogOutput<LogLevel::Verbose>(
							L"Unable to parse TMap {} with key type {}: {}",
							propWName,
							keyProp->GetClass().GetName(),
							to_wstring(err.what()));

						innerTable.vector_to_table(empty);
						break;
					}
					try
					{
						ExportPropertyAsTable(valueProp, elem + layout.ValueOffset, innerTable, PropertyType::Array, depth, true);
					}
					catch (const std::exception& e)
					{
						LogOutput<LogLevel::Verbose>(L"Unable to parse TMap value: {}", to_wstring(e.what()));
						auto emptyInnerTable = innerTable.get_lua_instance().prepare_new_table();
						emptyInnerTable.vector_to_table(empty);
						emptyInnerTable.make_local();
						innerTable.fuse_pair();
						break;
					}
					innerTable.fuse_pair();
				}
			}
			else
			{
				innerTable.vector_to_table(empty);
			}
			innerTable.make_local();
			table.fuse_pair();
		}
		else
		{
			LogOutput<LogLevel::Verbose>(L"Unable to parse {} of type {}", propWName, propClass);
		}
	}
	else if (property->IsA<FSetProperty>())
	{
		if (propertyType == PropertyType::Map)
			throw std::format_error("Unable to set TSet as TMap key");
		if (propertyType == PropertyType::Array)
			throw std::format_error("Unable to set TSet as array");

		auto setProp = static_cast<FSetProperty*>(property);
		auto setValue = propertyStorage;

		table.add_key(propName.c_str());
		auto innerTable = table.get_lua_instance().prepare_new_table();
		if (!setProp || !setValue)
		{
			innerTable.vector_to_table(empty);
			innerTable.make_local();
			table.fuse_pair();
			return;
		}

		auto innerProp = setProp->GetElementProp();

		auto scriptSet = static_cast<FScriptSet*>(setValue);
		auto layout = FScriptSet::GetScriptLayout(innerProp->GetSize(), innerProp->GetMinAlignment());

		if (scriptSet->Num() > 0)
		{
			int32 outputIndex = 1;
			ForEachOccupiedSlot(scriptSet->GetMaxIndex(), [&](int32 i) { return scriptSet->IsValidIndex(i); }, [&](int32 i) {
				{
					innerTable.add_key(outputIndex++);
					uint8* elemPtr = static_cast<uint8*>(scriptSet->GetData(i, layout));
					try
					{
						ExportPropertyAsTable(innerProp, elemPtr, innerTable, PropertyType::Array, depth, true);
					}
					catch (std::exception&)
					{
						auto emptyInnerTable = innerTable.get_lua_instance().prepare_new_table();
						emptyInnerTable.vector_to_table(empty);
						emptyInnerTable.make_local();
					}
					innerTable.fuse_pair();
				}
			});
		}
		else
		{
			innerTable.vector_to_table(empty);
		}
		innerTable.make_local();
		table.fuse_pair();
	}
	else
	{
		if (propertyType == PropertyType::Map)
			throw std::format_error("Unable to set anything as TMap key");
		LogOutput<LogLevel::Verbose>(L"Unable to parse {} of type {}", propWName, propClass);
	}
}
