import ctypes
import json
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
JSON_PARSER = ROOT / "Scripts" / "JsonParser.lua"


class LuaJsonSmoke:
    """Minimal ctypes binding for the system Lua 5.4 shared library.

    Runs the real Scripts/JsonParser.lua without the game, mirroring the
    compiled-harness style of test_snapshot_regressions.
    """

    def __init__(self):
        self.lib = ctypes.CDLL("liblua5.4.so.0")
        lib = self.lib
        lib.luaL_newstate.restype = ctypes.c_void_p
        lib.luaL_openlibs.argtypes = [ctypes.c_void_p]
        lib.luaL_loadstring.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
        lib.luaL_loadstring.restype = ctypes.c_int
        lib.lua_pcallk.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int,
                                   ctypes.c_int, ctypes.c_ssize_t, ctypes.c_void_p]
        lib.lua_pcallk.restype = ctypes.c_int
        lib.lua_tolstring.argtypes = [ctypes.c_void_p, ctypes.c_int,
                                      ctypes.POINTER(ctypes.c_size_t)]
        lib.lua_tolstring.restype = ctypes.c_char_p
        lib.lua_settop.argtypes = [ctypes.c_void_p, ctypes.c_int]
        lib.lua_close.argtypes = [ctypes.c_void_p]
        self.state = lib.luaL_newstate()
        if not self.state:
            raise RuntimeError("luaL_newstate failed")
        lib.luaL_openlibs(self.state)

    def run(self, chunk):
        status = self.lib.luaL_loadstring(self.state, chunk.encode())
        if status == 0:
            status = self.lib.lua_pcallk(self.state, 0, 1, 0, 0, None)
        if status != 0:
            size = ctypes.c_size_t()
            message = self.lib.lua_tolstring(self.state, -1, ctypes.byref(size))
            self.lib.lua_settop(self.state, -2)
            raise RuntimeError(f"Lua error: {message.decode(errors='replace')}")
        size = ctypes.c_size_t()
        raw = self.lib.lua_tolstring(self.state, -1, ctypes.byref(size))
        value = raw[: size.value].decode()
        self.lib.lua_settop(self.state, -2)
        return value

    def close(self):
        if self.state:
            self.lib.lua_close(self.state)
            self.state = None


class JsonSerializerArrayShapeTests(unittest.TestCase):
    """JSON shape contract: list endpoints serialize `data` as an array.

    The canary gate rejects list responses whose `data` is not a JSON array
    (an empty collection used to serialize as {}), while object-typed
    payloads (e.g. /server/state's unknown-zone response) must keep {}.
    """

    def setUp(self):
        if not JSON_PARSER.exists():
            self.skipTest("Scripts/JsonParser.lua not found")
        try:
            self.lua = LuaJsonSmoke()
        except OSError:
            self.skipTest("system Lua 5.4 shared library unavailable")
        self.addCleanup(self.lua.close)

    def serialize_envelope(self, lua_expression):
        # RequireSafe -> nil: no cjson in the test environment, so the
        # pure-Lua encoder below json.stringify is exercised directly.
        chunk = (
            "function RequireSafe(name) return nil end\n"
            f'json = dofile("{JSON_PARSER}")\n'
            f"return json.stringify({lua_expression})\n"
        )
        return self.lua.run(chunk)

    def test_empty_collection_serializes_as_array_and_nonempty_stays_array_of_records(self):
        output = self.serialize_envelope(
            '{ schemaVersion = 2, data = json.array({}) }'
        )
        payload = json.loads(output)
        self.assertIsInstance(payload["data"], list)
        self.assertEqual(payload["data"], [])

        output = self.serialize_envelope(
            '{ schemaVersion = 2, data = json.array({ { UniqueID = "alpha" }, { UniqueID = "beta" } }) }'
        )
        payload = json.loads(output)
        self.assertIsInstance(payload["data"], list)
        self.assertEqual(
            payload["data"], [{"UniqueID": "alpha"}, {"UniqueID": "beta"}]
        )

    def test_untagged_empty_tables_stay_objects(self):
        payload = json.loads(self.serialize_envelope("{ data = {} }"))
        self.assertEqual(payload["data"], {})


class SerializerSourceContractTests(unittest.TestCase):
    def test_tagged_payloads_skip_cjson_fast_path(self):
        source = JSON_PARSER.read_text()
        self.assertIn("contains_marked_array", source)
        self.assertIn("if cjson and not contains_marked_array(obj) then", source)

    def test_list_endpoints_tag_collections_as_arrays(self):
        expected = {
            "Scripts/PlayerManager.lua": ["data = json.array(data)"],
            "Scripts/VehicleManager.lua": ["data = json.array(data)"],
            "Scripts/CompanyManager.lua": [
                "data = json.array(companies)",
                "data = json.array(depots)",
                "data = json.array(data)",
                "data = json.array(vehicles)",
            ],
            "Scripts/CargoManager.lua": [
                "data = json.array(data)",
                "data = json.array(GetDeliveries(id, depth))",
            ],
            "Scripts/EventManager.lua": [
                "data = json.array(res)",
                "data = json.array(GetEvents(output))",
                "data = json.array(GetEvents(eventGuid))",
            ],
            "Scripts/CharacterManager.lua": ["data = json.array(data)"],
            "Scripts/PropertyManager.lua": ["data = json.array(houses)"],
        }
        for relative, snippets in expected.items():
            source = (ROOT / relative).read_text()
            for snippet in snippets:
                self.assertIn(snippet, source, f"{relative}: missing `{snippet}`")


if __name__ == "__main__":
    unittest.main()
