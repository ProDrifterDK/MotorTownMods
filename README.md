# Motor Town Mods

Powered by UE4SS lua scripts! This mod focuses primarily towards dedicated server. Some commands work on client-side, though results may vary depending on the game's replication system.

## Fork maintenance and B1088 status

This fork is maintained separately from upstream release artifacts. Its current source is newer than the upstream `v0.11.7` hotfix, and both its Lua and C++ modules identify as `0.12.0-b1088.1`. Do not combine these Lua files with an older `v0.11.7` `main.dll` and assume they represent the same build.

Motor Town `0.7.19 (B1088)`, Steam dedicated-server build `24441403`, is **`static_verified_runtime_pending`**. The exact executable identity and historical signature locations pass static verification, including the expected two `FText_Constructor` candidates. This is not runtime compatibility: the custom UE4SS FText override must still disambiguate those candidates under SEH/round-trip testing, and initialization, layouts, Lua APIs, hooks, saves, and restart behavior remain unverified.

**Do not deploy this fork to a live server before it passes a disposable runtime canary.** The machine-readable evidence is in [`compatibility/motortown-0.7.19-b1088.json`](./compatibility/motortown-0.7.19-b1088.json). Its state remains explicitly `runtime_pending`, with `runtime_verified=false` and `deployment_approved=false`.

Public CI is intentionally secret-free and read-only: it runs the unit tests and validates compatibility JSON metadata. It does not clone UE4SS or UEPseudo, build DLLs or bundles, or publish releases. Exact DLL and bundle builds are produced separately in a private, pinned, secret-gated harness and remain subject to the disposable runtime canary before deployment.

Verify an acquired dedicated-server executable against that evidence with:

```shell
python3 tools/verify_compatibility.py \
  --manifest compatibility/motortown-0.7.19-b1088.json \
  --binary /path/to/MotorTownServer-Win64-Shipping.exe
```

The verifier emits JSON and exits nonzero if the size, SHA-256, signature count, or exact offsets differ. In particular, missing or extra FText candidates fail closed.

## Usage

Upstream releases use a forked UE4SS release available [here](https://github.com/drpsyko101/RE-UE4SS/releases). Those artifacts are not proof of B1088 runtime support. B1088 testing must use a reviewed build from the commits pinned in the compatibility manifest and must pass the disposable canary gate above.

### Installation

**No deployable B1088 installation is approved.** Do not install a generic upstream UE4SS release for B1088. Only an exact reviewed bundle built from the commits pinned in the compatibility manifest may enter a disposable canary, and canary admission is not deployment approval.

The following steps describe historical/upstream release installation only; they do not apply to B1088:

1. Download and extract the matching [UE4SS release](https://github.com/drpsyko101/RE-UE4SS/releases) into the game `Binaries/Win64` directory.
2. Download the corresponding MotorTownMods release and extract it to `path/to/ue4ss/Mods/`. If that historical release supplies the Lua static modules as `shared.zip`, extract them into `path/to/ue4ss/Mods/shared`.
3. Add these game-specific UE4SS signatures into the `path/to/ue4ss/UE4SS_Signatures`:

   ```lua
   -- FText_Constructor.lua
   function Register()
      return "40 53 57 48 83 EC 38 48 89 6C 24 58 48 8B FA"
   end

   function OnMatchFound(matchAddress)
      return matchAddress
   end
   ```

   ```lua
   -- GUObjectArray.lua
   function Register()
      return "48 8D 0D ?? ?? ?? ?? 48 8B D7 89 5C 24 20 44 8D 4B ??"
   end

   function OnMatchFound(matchAddress)
      local displacement = DerefToInt32(matchAddress + 0x3)
      return matchAddress + 0x7 + displacement
   end
   ```

### Building from source

Build artifacts are branch-specific. The public repository validates source and metadata but does not reproduce the distribution build. Exact DLL and bundle builds use the UE4SS/UEPseudo revisions pinned in the compatibility manifest inside the private build harness. The steps below are for local development only.

1. Clone this repository into your `ue4ss/Mods` directory.

   ```shell
   git -C <path/to/ue4ss/Mods> clone https://github.com/ProDrifterDK/MotorTownMods.git
   ```

#### (Optional) Lua module

For a full functionality of the mod, download and extract [luasocket](https://github.com/alain-riedinger/luasocket/releases/tag/3.1-5.4.7) to `path/to/ue4ss/Mods/shared` directory to use the HTTP server. For webhook functionality or any instability while using the REST API, build [Luasec](https://github.com/lunarmodules/luasec) either from source or using [Luarocks](https://luarocks.org/) for Win64. Install [lua-bcrypt](https://github.com/mikejsavage/lua-bcrypt) to enable server API authentication with `bcrypt` hashing algorithm.

#### (Optional) C++ module

The C++ module are included in the release. Any changes to the C++ files need a recompile. These steps are similar to [creating a C++ mod](https://docs.ue4ss.com/dev/guides/creating-a-c++-mod.html) tutorial. Please read them before proceeding for a full understanding of the project structure.

1. Clone the RE-UE4SS into the `ue4ss` directory.

   ```shell
   git -C <path/to/ue4ss> clone --recurse-submodules https://github.com/drpsyko101/RE-UE4SS.git
   ```

2. Create a `xmake.lua` file at `ue4ss/` directory with these contents:

   ```lua
   includes("RE-UE4SS")
   includes("MotorTownMods")
   ```

3. Configure the project with `xmake`:

   ```shell
   xmake f -m "Game__Shipping__Win64" -y
   ```

4. Generate the Visual Studio solution file:

   ```shell
   xmake project -k vsxmake2022 -m "Game__Shipping__Win64" -y
   ```

5. Open the generated solution in `vsxmake2022/*.sln`, and right click on the `mods/MotorTownMods` in the solution explorer and click **Build**.
6. If the build is successfull, copy or create symlink the generated `ue4ss\Binaries\Game__Shipping__Win64\MotorTownMods\MotorTownMods.dll` to `ue4ss/MotorTownMods/dlls/main.dll`.

### Configuration

Most of the server settings can be configured using environment variables:

| Variable name               | Default value | Description                                                                                                                |
| --------------------------- | ------------- | -------------------------------------------------------------------------------------------------------------------------- |
| `MOD_SERVER_HOST`           | _unset_       | Legacy Lua HTTP listen host (highest Lua precedence). Existing values such as `*` remain Lua-only.                          |
| `MOD_SERVER_IP`             | _unset_       | Legacy Lua HTTP listen address (second Lua precedence).                                                                     |
| `MOD_MANAGEMENT_ADDRESS`    | _unset_       | Legacy C++ management listen address (highest management precedence).                                                       |
| `MOD_SERVER_ADDRESS`        | _unset_       | Shared fallback for both servers. Set a non-loopback address only for intentional, secured remote exposure.                |
| `MOD_MANAGEMENT_PORT`       | `5000`        | Management port. Used for managing mods in the dedicated server                                                            |
| `MOD_SERVER_PORT`           | `5001`        | Lua HTTP port. This only applies if `luasocket` module is installed                                                        |
| `MOD_SERVER_PROCESS_AMOUNT` | 5             | The amount of cumulative connection to process. Higher number correspond to quicker response, in return slower event hook. |
| `MOD_SERVER_LOG_LEVEL`      | 2             | The server log level. `0=ERROR`, `1=WARNING`, `2=INFO`, `3=VERBOSE`, `4=DEBUG`                                             |
| `MOD_WEBHOOK_URL`           | _none_        | Webhook URL to send the events to. Requires `luasec` to function                                                           |
| `MOD_WEBHOOK_METHOD`        | `POST`        | Webhook request method                                                                                                     |
| `MOD_WEBHOOK_EXTRA_HEADERS` | _none_        | Webhook extra headers in a JSON object                                                                                     |
| `MOD_WEBHOOK_ENABLE_EVENTS` | `all`         | Enable event hook individually. See [webhook documentation](./docs/Webhooks.md) for a complete list.                       |
| `MOD_SERVER_API_URL`        | _none_        | Server API to call from client side                                                                                        |
| `MOD_SERVER_PASSWORD`       | _none_        | Authenticate server request with `Authorization: Basic ` header                                                            |
| `MOD_SERVER_SEND_PARTIAL`   | _none_        | Limit server response chunks to 40 bytes (Set to `true` for older `luasocket` compatibility)                               |

Address precedence is server-specific and backward-compatible:

- Lua HTTP server: `MOD_SERVER_HOST`, then `MOD_SERVER_IP`, then shared `MOD_SERVER_ADDRESS`, then `127.0.0.1`.
- C++ management server: `MOD_MANAGEMENT_ADDRESS`, then shared `MOD_SERVER_ADDRESS`, then `127.0.0.1`.

The legacy Lua host/IP settings never configure the management server, and the legacy management setting never configures the Lua server. A Lua-only `MOD_SERVER_HOST=*` therefore remains valid and does not expose the management server. Shared management values must be numeric IP addresses; literal `*` is not accepted by the C++ server. Use a non-loopback address, including `0.0.0.0` or `::` for all interfaces, only for intentional secured exposure with appropriate authentication and firewalling.

### Reloading mod

Due to the webserver being ran on a separate thread, a stop command must be issued before reloading the mods.

1. Send **POST** `/stop` to the `MOD_SERVER_PORT` server. A message `Webserver stopped` will show up in the `UE4SS.log` indicating the webserver has stopped.
2. Send **POST** `/mods/reload` to the `MOD_MANAGEMENT_PORT` server. This will reload all the Lua mods including this mod.

## Documentation

More detailed instructions can be found in the [docs](./docs).

## Contributing

More contributions are welcomed! Read how to contribute [here](./docs/CONTRIBUTING.md).
