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


class ModsReloadCompatibilityTests(unittest.TestCase):
    def test_production_source_does_not_call_non_exported_reinstall_mods(self):
        source_paths = sorted(
            path
            for path in (ROOT / "src").rglob("*")
            if path.is_file()
            and path.suffix.lower() in {".cpp", ".cc", ".cxx", ".h", ".hh", ".hpp"}
        )
        self.assertGreater(len(source_paths), 0)

        for source_path in source_paths:
            with self.subTest(relative_path=source_path.relative_to(ROOT)):
                source = source_path.read_text(encoding="utf-8")
                self.assertNotRegex(source, r"\breinstall_mods\s*\(")

    def test_exact_post_reload_route_and_readme_fail_closed_as_not_implemented(self):
        source = (ROOT / "src/modsmanager.cpp").read_text(encoding="utf-8")
        response = source.split("ModsManager::GetResponseJson", 1)[1]

        self.assertRegex(
            response,
            r"(?s)if \(req\.target\(\) == modsReloadPath\).*"
            r"if \(req\.method\(\) == http::verb::post\).*"
            r"statusCode = http::status::not_implemented;",
        )
        self.assertIn('obj["status"] = "not_implemented";', response)
        message = "Mods reload is unavailable with the pinned UE4SS build."
        self.assertIn(f'obj["message"] = "{message}";', response)
        self.assertNotIn("http::status::accepted", response)

        readme = (ROOT / "README.md").read_text(encoding="utf-8")
        reload_guidance = readme.split("### Reloading mod", 1)[1].split(
            "## Documentation", 1
        )[0]
        self.assertIn("HTTP `501 Not Implemented`", reload_guidance)
        self.assertIn("status `not_implemented`", reload_guidance)
        self.assertIn(f"message `{message}`", reload_guidance)
        self.assertIn(
            "Do not stop the Lua server expecting this endpoint to reload mods.",
            reload_guidance,
        )
        self.assertIn(
            "Restart the dedicated server/UE4SS process to reload mods.",
            reload_guidance,
        )
        self.assertNotIn("This will reload all the Lua mods", reload_guidance)


class PinnedUE4SSSourceCompatibilityTests(unittest.TestCase):
    def test_statics_header_binds_pinned_dependency_contracts(self):
        source = (ROOT / "src/statics.h").read_text(encoding="utf-8")
        includes = re.findall(r"^#include ([^\n]+)$", source, flags=re.MULTILINE)

        unreal_core_structs = includes.index("<Unreal/UnrealCoreStructs.hpp>")
        self.assertLess(includes.index("<map>"), unreal_core_structs)
        self.assertLess(
            includes.index("<Unreal/Core/CoreTypes.hpp>"),
            unreal_core_structs,
        )

        with self.subTest(contract="UEPseudo FProperty declaration"):
            fproperty_header = "<Unreal/FProperty.hpp>"
            self.assertIn(fproperty_header, includes)
            self.assertLess(
                source.index(f"#include {fproperty_header}"),
                source.index("FProperty* property"),
            )

        with self.subTest(contract="DynamicOutput wide format arguments"):
            self.assertIn("RC_STD_MAKE_FORMAT_ARGS(args...)", source)
            self.assertNotRegex(
                source,
                r"fmt::make_format_args\s*<\s*fmt::buffer_context\s*<\s*wchar_t\s*>\s*>\s*\(",
            )

    def test_fstr_property_uses_fstring_view_conversion(self):
        source = (ROOT / "src/statics.cpp").read_text(encoding="utf-8")
        fstr_branch = source.split(
            "if (property->IsA<FStrProperty>())", 1
        )[1].split("else if (property->IsA<FNameProperty>())", 1)[0]

        self.assertIn("const auto str = to_string(**propertyValue);", fstr_branch)
        self.assertNotIn("GetCharArray", fstr_branch)
        self.assertNotIn("to_string(str).c_str()", fstr_branch)

    def test_fstring_conversions_do_not_pass_tarray_storage_to_to_string(self):
        source_paths = sorted(
            path
            for path in (ROOT / "src").rglob("*")
            if path.is_file()
            and path.suffix.lower() in {".cpp", ".cc", ".cxx", ".h", ".hh", ".hpp"}
        )
        self.assertGreater(len(source_paths), 0)

        for source_path in source_paths:
            relative_path = source_path.relative_to(ROOT)
            with self.subTest(relative_path=relative_path):
                source = source_path.read_text(encoding="utf-8")
                self.assertNotRegex(source, r"GetCharArray\s*\(")


if __name__ == "__main__":
    unittest.main()
