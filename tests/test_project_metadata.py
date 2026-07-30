import json
from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]


class ProjectMetadataTests(unittest.TestCase):
    def extract_literal(self, relative_path, pattern):
        source = (ROOT / relative_path).read_text(encoding="utf-8")
        matches = re.findall(pattern, source, flags=re.MULTILINE)
        self.assertEqual(len(matches), 1, f"expected one version literal in {relative_path}")
        return matches[0]

    def test_cpp_and_lua_version_literals_match(self):
        cpp_version = self.extract_literal(
            "src/statics.h",
            r'^\s*static std::wstring GetVersion\(\) \{ return L"([^"]+)"; \}\s*$',
        )
        lua_version = self.extract_literal(
            "Scripts/Statics.lua",
            r'^\s*ModVersion = "([^"]+)",\s*$',
        )

        self.assertEqual(cpp_version, lua_version)
        self.assertEqual(cpp_version, "0.12.0-b1088.1")

    def test_server_defaults_remain_loopback_only(self):
        cpp_default = self.extract_literal(
            "src/webserver.cpp",
            r'^\s*const char\* rawAddress = "([^"]+)";\s*$',
        )
        lua_default = self.extract_literal(
            "Scripts/Webserver.lua",
            r'^\s*or "([^"]+)"\s*$',
        )

        self.assertEqual(cpp_default, "127.0.0.1")
        self.assertEqual(lua_default, "127.0.0.1")

    def test_lua_address_precedence_when_legacy_settings_conflict(self):
        source = (ROOT / "Scripts/Webserver.lua").read_text(encoding="utf-8")
        address_block = source.split("local address =", 1)[1].split("local port =", 1)[0]

        self.assertEqual(
            re.findall(r'os\.getenv\("([A-Z_]+)"\)', address_block),
            ["MOD_SERVER_HOST", "MOD_SERVER_IP", "MOD_SERVER_ADDRESS"],
        )
        self.assertNotIn("MOD_MANAGEMENT_ADDRESS", address_block)

    def test_cpp_address_precedence_when_legacy_settings_conflict(self):
        source = (ROOT / "src/webserver.cpp").read_text(encoding="utf-8")
        address_block = source.split(
            'const char* rawAddress = "127.0.0.1";', 1
        )[1].split("const auto address", 1)[0]

        self.assertEqual(
            re.findall(r'getenv\("([A-Z_]+)"\)', address_block),
            ["MOD_MANAGEMENT_ADDRESS", "MOD_SERVER_ADDRESS"],
        )
        self.assertNotIn("MOD_SERVER_HOST", address_block)
        self.assertNotIn("MOD_SERVER_IP", address_block)

    def test_b1088_metadata_remains_fail_closed(self):
        manifest_path = ROOT / "compatibility" / "motortown-0.7.19-b1088.json"
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))

        self.assertEqual(manifest["schema_version"], 1)
        self.assertEqual(manifest["game"]["build"], "B1088")
        self.assertEqual(manifest["evidence"]["status"], "static_verified_runtime_pending")
        self.assertIs(manifest["evidence"]["runtime_verified"], False)
        self.assertIs(manifest["evidence"]["deployment_approved"], False)
        self.assertRegex(manifest["binary"]["sha256"], r"^[0-9A-Fa-f]{64}$")
        self.assertGreater(len(manifest["signatures"]), 0)
        for signature in manifest["signatures"]:
            self.assertGreater(signature["expected_count"], 0, signature["name"])
            self.assertEqual(
                len(signature["expected_offsets"]),
                signature["expected_count"],
                signature["name"],
            )


if __name__ == "__main__":
    unittest.main()
