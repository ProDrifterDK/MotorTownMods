import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import textwrap
import unittest

ROOT = Path(__file__).resolve().parents[1]


class SnapshotRegressionTests(unittest.TestCase):
    def test_sparse_slot_iteration_visits_live_slots_beyond_num(self):
        compiler = shutil.which("c++") or shutil.which("g++")
        if not compiler:
            self.skipTest("C++ compiler unavailable")
        source = textwrap.dedent(r"""
            #include "src/container_iteration.h"
            #include <cassert>
            #include <vector>
            int main() {
                const std::vector<bool> occupied{true, false, false, true};
                std::vector<int> visited;
                ForEachOccupiedSlot(4,
                    [&](int index) { return occupied[index]; },
                    [&](int index) { visited.push_back(index); });
                assert((visited == std::vector<int>{0, 3}));
            }
        """)
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "sparse.cpp"
            binary_path = Path(directory) / "sparse"
            source_path.write_text(source)
            subprocess.run(
                [compiler, "-std=c++20", "-I", str(ROOT), str(source_path), "-o", str(binary_path)],
                check=True,
            )
            subprocess.run([str(binary_path)], check=True)

    def test_serializer_filters_sparse_map_and_set_slots(self):
        source = (ROOT / "src/statics.cpp").read_text()
        map_branch = source.split("else if (property->IsA<FMapProperty>())", 1)[1].split(
            "else if (property->IsA<FSetProperty>())", 1)[0]
        set_branch = source.split("else if (property->IsA<FSetProperty>())", 1)[1]
        self.assertIn("propertyValue->GetMaxIndex()", map_branch)
        self.assertIn("propertyValue->IsValidIndex(i)", map_branch)
        self.assertIn("elem + layout.ValueOffset", map_branch)
        self.assertIn("scriptSet->GetMaxIndex()", set_branch)
        self.assertIn("scriptSet->IsValidIndex(i)", set_branch)
        self.assertIn("outputIndex++", set_branch)
        self.assertNotIn("i < helper.Num()", set_branch)

    def test_reflection_binding_rejects_worker_before_fname_read(self):
        source = (ROOT / "src/dllmain.cpp").read_text()
        binding = source.split('"GetObjectVariables"', 1)[1].split(
            '"NativeSleep"', 1)[0]
        self.assertLess(binding.index('require_game_thread(_lua'), binding.index('get_remote_cpp_object()'))
        reader = (ROOT / "src/snapshot.cpp").read_text()
        self.assertIn('static_cast<FName*>(storage)->ToString()', reader)
        self.assertIn('CaptureGameStateSnapshot', source)
        self.assertIn('require_game_thread(lua, "CaptureGameStateSnapshot")', source)

    def test_snapshot_results_are_owned_values_and_bounded(self):
        header = (ROOT / "src/snapshot.h").read_text()
        self.assertIn("std::variant<std::monostate, bool, int64_t, double, std::string, Array, Object>", header)
        self.assertNotIn("UObject*", header)
        self.assertIn("MaxPending = 64", header)
        source = (ROOT / "src/snapshot.cpp").read_text()
        self.assertIn("max_nodes", source)
        self.assertIn("max_elements", source)
        self.assertIn("max_bytes", source)
        self.assertIn("found->second.generation == generation", source)

    def test_http_pending_lifecycle_cancels_before_late_delivery(self):
        source = (ROOT / "Scripts/Webserver.lua").read_text()
        self.assertIn('pending = "pending"', source)
        self.assertIn("pollPendingSnapshots()", source)
        self.assertIn("session.pending = nil\n                pcall(CancelGameStateSnapshot", source)
        self.assertIn("Remove token authority before any fallible serialization or send", source)
        self.assertIn("Route disabled until its engine access is migrated", source)

    def test_snapshot_projection_preserves_controller_contract(self):
        source = (ROOT / "src/snapshot.cpp").read_text()
        self.assertIn('"Velocity", "Throttle", "Brake", "HandBrake"', source)
        self.assertIn('"Slot", "Key", "Damage"', source)
        self.assertIn('project_vehicle_parts(', source)
        self.assertIn('projected.insert_or_assign("Name"', source)
        self.assertIn('GetStruct()->GetName()) == "Guid"', source)

    def test_snapshot_store_reclaims_expired_and_stops_with_lua(self):
        header = (ROOT / "src/dllmain.h").read_text()
        source = (ROOT / "src/dllmain.cpp").read_text()
        store = (ROOT / "src/snapshot.cpp").read_text()
        self.assertIn("on_lua_stop(", header)
        self.assertIn("Snapshot::Store::cancel_all();", source)
        self.assertIn("std::erase_if(s_entries", store)
        self.assertIn("query.player_id.size() > MaxIdLength", store)

    def test_legacy_reflection_accepts_only_recorded_game_thread(self):
        source = (ROOT / "src/dllmain.cpp").read_text()
        guard = source.split("auto require_game_thread", 1)[1].split("auto split_fields", 1)[0]
        self.assertIn("LuaMod::is_in_game_thread()", guard)
        self.assertNotIn("is_executing_engine_tick_action", guard)

    def test_b1104_contract_is_distinct_and_runtime_pending(self):
        b1088 = json.loads((ROOT / "compatibility/motortown-0.7.19-b1088.json").read_text())
        b1104 = json.loads((ROOT / "compatibility/motortown-0.7.19-b1104.json").read_text())
        self.assertEqual(b1088["game"]["build"], "B1088")
        self.assertEqual(b1104["game"]["build"], "B1104")
        self.assertNotEqual(b1088["binary"]["sha256"], b1104["binary"]["sha256"])
        self.assertFalse(b1104["evidence"]["runtime_verified"])
        self.assertFalse(b1104["snapshot_contract"]["runtime_layouts_verified"])
        self.assertIn("Velocity", b1104["snapshot_contract"]["projections"]["VehicleReplicatedMovement"]["fields"])
        self.assertEqual(
            b1104["snapshot_contract"]["projections"]["vehicles"]["Net_Parts"]["fields"],
            ["Slot", "Key", "Damage"],
        )
        self.assertFalse(b1104["evidence"]["deployment_approved"])


if __name__ == "__main__":
    unittest.main()
