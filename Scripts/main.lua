local dir = os.getenv("PWD") or io.popen("cd"):read()
package.cpath = package.cpath .. ";" .. dir .. "/ue4ss/Mods/shared/?/core.dll"
package.cpath = package.cpath .. ";" .. dir .. "/ue4ss/Mods/shared/?.dll"

require("Helpers")
local json = require("JsonParser")
local logging = require("Debugging/Logging")
local statics = require("Statics")

---@deprecated Use LogOutput instead to avoid concat errors
LogMsg = logging.logMsg
LogOutput = logging.logOutput

local playerManager = require("PlayerManager")
local eventManager = require("EventManager")
local serverManager = require("ServerManager")
local cargoManager = require("CargoManager")
local chatManager = require("ChatManager")
local widgetManager = require("ViewportManager")
local companyManager = require("CompanyManager")
local characterManager = require("CharacterManager")
local propertyManager = require("PropertyManager")
local vehicleManager = require("VehicleManager")
local assetManager = require("AssetManager")

local function LoadWebserver()
  local status, err = pcall(function()
    local server = require("Webserver")

    -- General server status
    server.registerHandler("/status", "GET", serverManager.HandleGetServerStatus, false, true)
    server.registerHandler("/status/general", "GET", serverManager.HandleGetServerState)
    server.registerHandler("/status/general/*", "GET", serverManager.HandleGetZoneState)
    server.registerHandler("/settings/traffic", "GET", serverManager.HandleGetNpcTraffic)
    server.registerHandler("/settings/traffic", "POST", serverManager.HandleUpdateNpcTraffic)
    server.registerHandler("/settings", "PATCH", serverManager.HandleSetServerSettings)
    server.registerHandler("/command", "POST", serverManager.HandleServerExecCommand)

    -- Player management
    server.registerHandler("/players", "GET", playerManager.HandleGetPlayerStates, nil, true)
    server.registerHandler("/players/*/teleport", "POST", playerManager.HandleTeleportPlayer)
    server.registerHandler("/players/*/money", "POST", playerManager.HandleAddMoney)
    server.registerHandler("/players/*/gameplay/effects", "DELETE", playerManager.HandleRemoveGameplayEffect)
    server.registerHandler("/players/*", "GET", playerManager.HandleGetPlayerStates, nil, true)
    server.registerHandler("/players/*/eject", "POST", vehicleManager.HandleEjectPlayer)

    -- Event management
    server.registerHandler("/events", "GET", eventManager.HandleGetEvents)
    server.registerHandler("/events", "POST", eventManager.HandleCreateNewEvent)
    server.registerHandler("/events/*", "GET", eventManager.HandleGetEvents)
    server.registerHandler("/events/*/state", "POST", eventManager.HandleChangeEventState)
    server.registerHandler("/events/*/players", "POST", eventManager.HandlePlayerJoinEvent)
    server.registerHandler("/events/*/players", "DELETE", eventManager.HandlePlayerLeaveEvent)
    server.registerHandler("/events/*", "PATCH", eventManager.HandleUpdateEvent)
    server.registerHandler("/events/*", "DELETE", eventManager.HandleRemoveEvent)

    -- Properties management
    server.registerHandler("/houses", "GET", propertyManager.HandleGetHouses)
    server.registerHandler("/houses/*", "GET", propertyManager.HandleGetHouses)
    server.registerHandler("/houses/spawn", "POST", propertyManager.HandleSpawnHouse)

    -- Cargo management
    server.registerHandler("/delivery/points", "GET", cargoManager.HandleGetDeliveryPoints)
    server.registerHandler("/delivery/points/*", "GET", cargoManager.HandleGetDeliveryPoints)
    server.registerHandler("/delivery", "GET", cargoManager.HandleGetDeliveries)
    server.registerHandler("/delivery/*", "GET", cargoManager.HandleGetDeliveries)

    -- Vehicle management
    server.registerHandler("/vehicles", "GET", vehicleManager.HandleGetVehicles, nil, true)
    server.registerHandler("/vehicles", "PATCH", vehicleManager.HandleSetVehicleParameter)
    server.registerHandler("/vehicles/*/despawn", "POST", vehicleManager.HandleDespawnVehicle)
    server.registerHandler("/vehicles/*", "GET", vehicleManager.HandleGetVehicles, nil, true)
    server.registerHandler("/vehicles/*", "PATCH", vehicleManager.HandleSetVehicleParameter)
    server.registerHandler("/vehicles/*/fuel", "POST", vehicleManager.HandleSetVehicleFuel)
    server.registerHandler("/vehicles/*/parts/*/damage", "POST", vehicleManager.HandleSetVehiclePartDamage)
    server.registerHandler("/dealers/spawn", "POST", vehicleManager.HandleCreateVehicleDealerSpawnPoint)
    server.registerHandler("/garages", "GET", vehicleManager.HandleGetGarages)
    server.registerHandler("/garages/spawn", "POST", vehicleManager.HandleSpawnGarage)

    -- Asset management
    server.registerHandler("/assets/spawn", "POST", assetManager.HandleSpawnActor)
    server.registerHandler("/assets/despawn", "POST", assetManager.HandleDespawnActor)

    -- UI management
    server.registerHandler("/messages/popup", "POST", widgetManager.HandleShowPopupMessage)
    server.registerHandler("/messages/announce", "POST", chatManager.HandleAnnounceMessage)

    -- Company management
    server.registerHandler("/companies", "GET", companyManager.HandleGetCompanies)
    server.registerHandler("/companies/*/vehicles", "GET", companyManager.HandleGetCompanyVehicles)
    server.registerHandler("/companies/*/routes/bus", "GET", companyManager.HandleGetCompanyBusRoutes)
    server.registerHandler("/companies/*/routes/bus/*", "GET", companyManager.HandleGetCompanyBusRoutes)
    server.registerHandler("/companies/*/routes/truck", "GET", companyManager.HandleGetCompanyTruckRoutes)
    server.registerHandler("/companies/*/routes/truck/*", "GET", companyManager.HandleGetCompanyTruckRoutes)
    server.registerHandler("/companies/*/depots", "GET", companyManager.HandleGetCompanyDepots)
    server.registerHandler("/companies/*/depots/*", "GET", companyManager.HandleGetCompanyDepots)
    server.registerHandler("/companies/*", "GET", companyManager.HandleGetCompanies)
    server.registerHandler("/depots", "GET", companyManager.HandleGetDepots)

    -- Character management
    server.registerHandler("/characters", "GET", characterManager.HandleGetCharacters)

    server.run()
    return nil
  end)
  if not status then
    LogOutput("ERROR", "Webserver stopped unexpectedly due to error: %s", err)
  end
end

---Game-thread pump self-probe (D2 diagnostic): 10s after boot, request one
---vehicles snapshot and check whether the capture callback ever executed.
---If the game-thread pump is broken on this target, this logs a single loud
---ERROR with the token state instead of leaving every /vehicles and /players
---request to 504 with an ambiguous "deadline exceeded".
local function StartGameThreadPumpProbe()
  local socket = require("socket")
  LoopAsync(10000, function()
    LogOutput("INFO", "GameThread pump probe: requesting a snapshot")
    local ok, tokenOrErr = pcall(function()
      return RequestAsyncSnapshot("vehicles", nil, {}, 5, false, 0, 2000)
    end)
    if not ok then
      LogOutput("ERROR", "GameThread pump probe: RequestAsyncSnapshot failed: %s", tokenOrErr)
      return true
    end
    local token = tokenOrErr
    local deadline = (socket.gettime() * 1000) + 3000
    while socket.gettime() * 1000 < deadline do
      local okPoll, state = pcall(PollGameStateSnapshot, token)
      if not okPoll then
        LogOutput("ERROR", "GameThread pump probe: poll errored")
        return true
      end
      if state ~= "pending" then
        LogOutput("INFO", "GameThread pump probe: capture state reached '%s' - pump is alive", state)
        return true
      end
      Sleep(50)
    end
    LogOutput("ERROR", "GameThread pump probe: capture callback never executed within 3s (token still pending) - the game-thread pump is NOT running on this target; /vehicles and /players will 504")
    pcall(CancelGameStateSnapshot, token)
    return true
  end)
end

LoadWebserver()
StartGameThreadPumpProbe()
LogOutput("INFO", "Mod loaded")
