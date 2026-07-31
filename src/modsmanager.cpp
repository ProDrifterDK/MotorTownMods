#include "modsmanager.h"

static const char* modsReloadPath = "/mods/reload";

bool ModsManager::IsMatchingRequest(http::request<http::string_body> req)
{
	if (req.target().starts_with(modsReloadPath))
	{
		return true;
	}
	return false;
}

json::object ModsManager::GetResponseJson(http::request<http::string_body> req, http::status& statusCode)
{
	json::object obj;
	if (req.target() == modsReloadPath)
	{
		if (req.method() == http::verb::post)
		{
			statusCode = http::status::not_implemented;
			obj["status"] = "not_implemented";
			obj["message"] = "Mods reload is unavailable with the pinned UE4SS build.";
			return obj;
		}
	}
	return obj;
}
