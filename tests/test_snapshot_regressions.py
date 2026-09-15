import json
import re
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
        # Run-13 RCA (D2): the capture binding no longer uses the generic
        # require_game_thread throw (a silent swallow inside a queued action
        # left the entry pending forever); it must fail CLOSED by recording a
        # typed Store error before refusing the engine-state read.
        self.assertIn('MotorTown::Snapshot::Store::fail(', source)
        self.assertIn('is_in_game_thread()', source.split('capture_game_state_snapshot', 1)[1].split('poll_game_state_snapshot', 1)[0])

    def test_snapshot_results_are_owned_values_and_bounded(self):
        header = (ROOT / "src/snapshot.h").read_text()
        self.assertIn("std::variant<std::monostate, bool, int64_t, double, std::string, Array, Object>", header)
        value_contract = header.split("struct Value", 1)[1].split("enum class State", 1)[0]
        self.assertNotIn("UObject*", value_contract)
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
        # Lifecycle-order evidence: token authority is released before any
        # fallible serialization/send, the post-deadline poll happens BEFORE
        # the cancel (cancel erases the entry, making the state ambiguous),
        # and the cancel always runs after the poll on the timeout path.
        self.assertIn("session.pending = nil", source)
        self.assertIn("local okPoll, pollState = pcall(PollGameStateSnapshot, token)", source)
        self.assertIn("pcall(CancelGameStateSnapshot, token)", source)
        timeout_block = source[source.index("if time() >= pending.deadline"):]
        timeout_block = timeout_block[:timeout_block.index("local sent, err = pcall(sendResponse")]
        poll_at = timeout_block.index("pcall(PollGameStateSnapshot, token)")
        cancel_at = timeout_block.index("pcall(CancelGameStateSnapshot, token)")
        self.assertLess(poll_at, cancel_at, "timeout path must poll before cancel")
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

    def test_root_discovery_is_cached_and_deadline_checked(self):
        source = (ROOT / "src/snapshot.cpp").read_text()
        capture = source.split("auto capture_query", 1)[1].split("auto Store::begin", 1)[0]
        self.assertNotIn("FindFirstOf", capture)
        self.assertIn("Store::resolve_active_game_state()", capture)
        self.assertLess(capture.index("Budget budget"), capture.index("Store::resolve_active_game_state()"))
        root_helper = (ROOT / "src/active_snapshot_root.h").read_text()
        self.assertIn("ResolveServedSnapshotRoot", root_helper)
        dll = (ROOT / "src/dllmain.cpp").read_text()
        self.assertIn("RegisterLoadMapPreCallback", dll)
        self.assertIn("RegisterInitGameStatePostCallback", dll)

    def test_query_validation_rejects_fractional_and_partial_values(self):
        helpers = (ROOT / "Scripts/Helpers.lua").read_text()
        vehicles = (ROOT / "Scripts/VehicleManager.lua").read_text()
        players = (ROOT / "Scripts/PlayerManager.lua").read_text()
        binding = (ROOT / "src/dllmain.cpp").read_text()
        self.assertIn("function ParseIntegerQuery", helpers)
        self.assertIn("parsed % 1 ~= 0", helpers)
        self.assertIn('vehicle ID must be a complete decimal integer', vehicles)
        self.assertIn("consumed != id.size()", binding)
        self.assertIn("lua_type(state, 2) != LUA_TSTRING", binding)
        self.assertIn('ParseIntegerQuery(session.queryComponents.limit', vehicles)
        self.assertIn('ParseIntegerQuery(session.queryComponents.depth', players)

    def test_webhook_loop_stops_cleanly_without_luasocket(self):
        source = (ROOT / "Scripts/Webclient.lua").read_text()
        callback = source.split("LoopAsync(delay, function()", 1)[1]
        self.assertIn("if not socket then return true end", callback)
        self.assertLess(callback.index("if not socket then return true end"), callback.index("socket.gettime()"))

    def test_recovery_refuses_off_thread_unanchored_and_unreadable_chains(self):
        # Named failures this catches: (1) removing the off-game-thread or
        # no-anchor precondition must be observable in the scan itself: the
        # FindAllOf stand-in records its invocations, and both refusals must
        # happen BEFORE it is ever called (review-1 mutation: deleting the
        # guard's return left the suite green because only source order was
        # checked); (2) a fully authoritative chain (correct world, live
        # backlink, current-world match) must still be refused when the
        # candidate, its world, or its authority GameMode is PendingKill,
        # unreachable or pending-destroyed - FindAllOf does not filter those
        # states, so these readability gates are the only defense; (3) a
        # readable old-world chain behind a current-world anchor is refused;
        # (4) two candidates passing the chain on successive reads refuse as
        # ambiguous instead of picking one.
        compiler = shutil.which("c++") or shutil.which("g++")
        if not compiler:
            self.skipTest("C++ compiler unavailable")
        source = textwrap.dedent(r"""
            #include "src/active_snapshot_root.h"
            #include <cassert>
            #include <vector>
            enum class Flag { None, PendingKill, Unreachable, BeginDestroyed };
            struct GameState;
            struct GameMode { GameState* game_state; Flag flag = Flag::None; };
            struct World { GameMode* game_mode; Flag flag = Flag::None; };
            struct GameState { World* world; Flag flag = Flag::None; };
            struct ScanSpy {
                int invocations = 0;
                std::vector<GameState*> objects;
                std::vector<GameState*> operator()() { ++invocations; return objects; }
            };
            int main() {
                const auto readable = [](const auto* object) { return object && object->flag == Flag::None; };
                const auto world_of = [](GameState* game_state) { return game_state->world; };
                const auto mode_of = [](World* world) { return world->game_mode; };
                const auto state_of = [](GameMode* game_mode) { return game_mode->game_state; };
                const auto decide = [&](bool on_thread, const void* anchor, ScanSpy& spy) {
                    // The scan is passed as a reference-capturing lambda so the
                    // template cannot silently take the spy by value.
                    return RecoverActiveSnapshotRoot<GameState>(on_thread, anchor, [&] { return spy(); }, readable, world_of, mode_of, state_of);
                };

                GameMode mode{nullptr};
                World world{&mode};
                GameState live{&world};
                mode.game_state = &live;

                // Fully authoritative and current: admitted, scan ran exactly once.
                {
                    ScanSpy spy; spy.objects = {&live};
                    const auto decision = decide(true, &world, spy);
                    assert(decision.outcome == SnapshotRecoveryOutcome::Recovered);
                    assert(decision.admitted == &live);
                    assert(spy.invocations == 1);
                }
                // Off-game-thread: refused BEFORE the scan (spy stays at zero).
                {
                    ScanSpy spy; spy.objects = {&live};
                    const auto decision = decide(false, &world, spy);
                    assert(decision.outcome == SnapshotRecoveryOutcome::OffGameThread);
                    assert(decision.admitted == nullptr);
                    assert(spy.invocations == 0);
                }
                // No current-world identity (LoadMap anchor unavailable): refused BEFORE the scan.
                {
                    ScanSpy spy; spy.objects = {&live};
                    const auto decision = decide(true, nullptr, spy);
                    assert(decision.outcome == SnapshotRecoveryOutcome::NoCurrentWorld);
                    assert(decision.admitted == nullptr);
                    assert(spy.invocations == 0);
                }
                // A fully authoritative chain in a NON-current world is refused:
                // membership in a world is not membership in the current world.
                {
                    GameMode old_mode{nullptr};
                    World old_world{&old_mode};
                    GameState stale{&old_world};
                    old_mode.game_state = &stale;
                    ScanSpy spy; spy.objects = {&stale};
                    const auto decision = decide(true, &world, spy);
                    assert(decision.outcome == SnapshotRecoveryOutcome::Unresolved);
                    assert(decision.admitted == nullptr);
                }
                // Fully authoritative but PendingKill candidate: refused. The
                // candidate IS the GameMode's live GameState here, so ONLY the
                // readability gate rejects it (review-2 X1: an orphaned fixture
                // is already rejected by the backlink, which proves nothing
                // about readability).
                {
                    GameState pending{&world, Flag::PendingKill};
                    mode.game_state = &pending;
                    ScanSpy spy; spy.objects = {&pending};
                    assert(decide(true, &world, spy).outcome == SnapshotRecoveryOutcome::Unresolved);
                    mode.game_state = &live;
                }
                // PendingKill world on an otherwise complete chain: refused.
                {
                    GameMode anchored_mode{nullptr};
                    World dying_world{&anchored_mode, Flag::PendingKill};
                    GameState anchored{&dying_world};
                    anchored_mode.game_state = &anchored;
                    ScanSpy spy; spy.objects = {&anchored};
                    assert(decide(true, &dying_world, spy).outcome == SnapshotRecoveryOutcome::Unresolved);
                }
                // PendingKill authority GameMode: refused.
                {
                    GameMode dying_mode{nullptr, Flag::PendingKill};
                    World modeless{&dying_mode};
                    GameState anchored{&modeless};
                    dying_mode.game_state = &anchored;
                    ScanSpy spy; spy.objects = {&anchored};
                    assert(decide(true, &modeless, spy).outcome == SnapshotRecoveryOutcome::Unresolved);
                }
                // Unreachable / pending-destroyed candidates: refused. Same
                // fixture discipline: each is the GameMode's live GameState, so
                // the backlink alone cannot reject it.
                {
                    GameState gone{&world, Flag::Unreachable};
                    mode.game_state = &gone;
                    ScanSpy spy; spy.objects = {&gone};
                    assert(decide(true, &world, spy).outcome == SnapshotRecoveryOutcome::Unresolved);
                    mode.game_state = &live;
                }
                {
                    GameState dying{&world, Flag::BeginDestroyed};
                    mode.game_state = &dying;
                    ScanSpy spy; spy.objects = {&dying};
                    assert(decide(true, &world, spy).outcome == SnapshotRecoveryOutcome::Unresolved);
                    mode.game_state = &live;
                }
                // Orphan: readable, current world, but not the GameMode's live GameState.
                {
                    GameState orphan{&world};
                    ScanSpy spy; spy.objects = {&orphan};
                    assert(decide(true, &world, spy).outcome == SnapshotRecoveryOutcome::Unresolved);
                }
                // Mixed-order list: an INVALID first candidate must not shadow
                // a later valid one (review-2 X3: a recovery result of
                // 'admitted ? candidates.front() : nullptr' stays green without
                // this case).
                {
                    GameState bogus{&world}; // readable, current, but orphaned
                    ScanSpy spy; spy.objects = {&bogus, &live};
                    const auto decision = decide(true, &world, spy);
                    assert(decision.outcome == SnapshotRecoveryOutcome::Recovered);
                    assert(decision.admitted == &live);
                }
                // Two candidates that each pass the chain on successive backlink
                // reads (models the authority property being re-pointed while the
                // scan runs): ambiguous, never guess.
                {
                    GameState second{&world};
                    GameState* flipping = &live;
                    const auto racing_state_of = [&](GameMode*) { GameState* seen = flipping; flipping = (flipping == &live) ? &second : &live; return seen; };
                    ScanSpy spy; spy.objects = {&live, &second};
                    const auto decision = RecoverActiveSnapshotRoot<GameState>(true, &world, spy, readable, world_of, mode_of, racing_state_of);
                    assert(decision.outcome == SnapshotRecoveryOutcome::Ambiguous);
                    assert(decision.admitted == nullptr);
                }
            }
        """)
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "recovery.cpp"
            binary_path = Path(directory) / "recovery"
            source_path.write_text(source)
            subprocess.run(
                [compiler, "-std=c++17", "-I", str(ROOT), str(source_path), "-o", str(binary_path)],
                check=True,
            )
            subprocess.run([str(binary_path)], check=True)

    def test_served_root_requires_current_world_live_backlink_and_weak_resolution(self):
        # Named failures this catches: (1) cache reuse bound only to weak-world
        # equality keeps serving a pair after the engine's current world moved
        # on (review-1: recover/cache old root, second authoritative world
        # appears, cache still returns the old root); (2) a cached root whose
        # authority backlink changed is still served unless the backlink is
        # re-read at serve time; (3) an unreadable root, world or GameMode must
        # refuse even when the pointer identity still matches; (4) dead weak
        # pointers refuse; (5) a weak-resolvable, otherwise-authoritative
        # pending-destroyed root refuses BEFORE the GetWorld engine virtual
        # executes on it (review-2: pinned weak resolution does not check
        # RF_BeginDestroyed/RF_FinishDestroyed - that is the module's gate, and
        # it must run first).
        compiler = shutil.which("c++") or shutil.which("g++")
        if not compiler:
            self.skipTest("C++ compiler unavailable")
        source = textwrap.dedent(r"""
            #include "src/active_snapshot_root.h"
            #include <cassert>
            enum class Flag { None, PendingKill, Unreachable, BeginDestroyed, FinishDestroyed };
            struct GameState;
            struct GameMode { GameState* game_state; Flag flag = Flag::None; };
            struct World { GameMode* game_mode; Flag flag = Flag::None; };
            struct GameState {
                World* world; Flag flag = Flag::None;
                mutable int get_world_calls = 0; // engine-virtual access spy
                World* GetWorld() const { ++get_world_calls; return world; }
            };
            // Models the pinned FWeakObjectPtr::Get() DEFAULT validity, verified
            // against deps/first/Unreal (FWeakObjectPtr.cpp delegates to
            // UObjectArray::IsValid; every located versioned container rejects
            // Unreachable OR PendingKill): rejects null/stale-serial (modeled as
            // null), Unreachable and PendingKill. RF_BeginDestroyed and
            // RF_FinishDestroyed are NOT weak checks - the module's
            // object_is_readable adds them, which is exactly what the
            // BeginDestroyed/FinishDestroyed fixtures below isolate.
            template <typename T>
            struct Weak {
                const T* value{};
                const T* Get() const {
                    return (value && value->flag != Flag::PendingKill && value->flag != Flag::Unreachable) ? value : nullptr;
                }
            };
            int main() {
                const auto readable = [](const auto* object) { return object && object->flag == Flag::None; };
                const auto mode_of = [](const World* world) { return world->game_mode; };
                const auto state_of = [](const GameMode* game_mode) { return game_mode->game_state; };
                const auto serve = [&](const Weak<GameState>& root, const Weak<World>& world, const void* anchor) {
                    return ResolveServedSnapshotRoot(root, world, anchor, readable, mode_of, state_of);
                };

                GameMode mode{nullptr};
                World world{&mode};
                GameState live{&world};
                mode.game_state = &live;

                Weak<GameState> live_weak{&live};
                Weak<World> world_weak{&world};

                // Healthy pair anchored to the current world: served (and the
                // engine virtual ran exactly once on a readable root).
                assert(serve(live_weak, world_weak, &world) == &live);
                assert(live.get_world_calls == 1);
                // Cache reuse without an anchor refuses (identity unavailable).
                assert(serve(live_weak, world_weak, nullptr) == nullptr);
                // The current world moved on (second world became current):
                // the cached pair refuses even though its own chain is intact.
                {
                    GameMode new_mode{nullptr};
                    World new_world{&new_mode};
                    GameState second{&new_world};
                    new_mode.game_state = &second;
                    assert(serve(live_weak, world_weak, &new_world) == nullptr);
                    Weak<GameState> second_weak{&second};
                    Weak<World> new_world_weak{&new_world};
                    assert(serve(second_weak, new_world_weak, &new_world) == &second);
                }
                // Authority backlink changed after caching: refuse.
                {
                    GameState other{&world};
                    mode.game_state = &other;
                    assert(serve(live_weak, world_weak, &world) == nullptr);
                    mode.game_state = &live;
                    assert(serve(live_weak, world_weak, &world) == &live);
                }
                // Authority GameMode unreadable at serve time: refuse.
                {
                    mode.flag = Flag::PendingKill;
                    assert(serve(live_weak, world_weak, &world) == nullptr);
                    mode.flag = Flag::None;
                }
                // Root or world gone (weak pointer unresolvable): refuse.
                assert(serve(Weak<GameState>{}, world_weak, &world) == nullptr);
                assert(serve(live_weak, Weak<World>{}, &world) == nullptr);
                // PendingKill root: rejected by the pinned weak validity itself;
                // GetWorld must not run on it.
                {
                    GameState dying{&world, Flag::PendingKill};
                    assert(serve(Weak<GameState>{&dying}, world_weak, &world) == nullptr);
                    assert(dying.get_world_calls == 0);
                }
                // Isolated weak-resolvable, otherwise-authoritative roots that
                // only the module-added RF flags reject (review-2 X5): the serve
                // gate must refuse BEFORE the GetWorld engine virtual executes.
                {
                    GameMode anchored_mode{nullptr};
                    World anchored_world{&anchored_mode};
                    GameState begin_destroyed{&anchored_world, Flag::BeginDestroyed};
                    anchored_mode.game_state = &begin_destroyed;
                    assert(serve(Weak<GameState>{&begin_destroyed}, Weak<World>{&anchored_world}, &anchored_world) == nullptr);
                    assert(begin_destroyed.get_world_calls == 0);
                }
                {
                    GameMode anchored_mode{nullptr};
                    World anchored_world{&anchored_mode};
                    GameState finish_destroyed{&anchored_world, Flag::FinishDestroyed};
                    anchored_mode.game_state = &finish_destroyed;
                    assert(serve(Weak<GameState>{&finish_destroyed}, Weak<World>{&anchored_world}, &anchored_world) == nullptr);
                    assert(finish_destroyed.get_world_calls == 0);
                }
                // Unreachable root or world: the pinned weak validity itself
                // rejects Unreachable; refuse at resolution, GetWorld not run.
                {
                    GameState gone{&world, Flag::Unreachable};
                    assert(serve(Weak<GameState>{&gone}, world_weak, &world) == nullptr);
                    assert(gone.get_world_calls == 0);
                }
                {
                    world.flag = Flag::Unreachable;
                    assert(serve(live_weak, world_weak, &world) == nullptr);
                    world.flag = Flag::None;
                }
                // Root/world pair mismatch: refuse.
                {
                    Weak<World> other_world_weak{&world};
                    GameState alien{nullptr};
                    assert(serve(Weak<GameState>{&alien}, other_world_weak, &world) == nullptr);
                }
            }
        """)
        with tempfile.TemporaryDirectory() as directory:
            source_path = Path(directory) / "served.cpp"
            binary_path = Path(directory) / "served"
            source_path.write_text(source)
            subprocess.run([compiler, "-std=c++17", "-I", str(ROOT), str(source_path), "-o", str(binary_path)], check=True)
            subprocess.run([str(binary_path)], check=True)

    def test_loadmap_travel_invalidates_anchor_and_fails_closed_until_post(self):
        # Named failure (review-2 P1): the LoadMap pre notification used to
        # clear only the cached root pair, leaving s_current_world pointing at
        # the OUTGOING world. In the pinned dispatcher (deps/first/Unreal/src/
        # Hooks.cpp HookedLoadMap) each callback loop breaks at the first
        # callback returning {true, _} and Register* appends, so the post loop
        # is interruptible in general. While the stale anchor stayed live,
        # recovery re-admitted the old world's root and the serve gate accepted
        # it (cached world == stale anchor). The fix invalidates cache AND
        # anchor together inside the pre notification and rotates this mod's
        # callback to the FRONT of both vectors, so an earlier-registered peer
        # cannot end a loop before ours has run; a post notification that still
        # never reaches this mod leaves a permanent typed 503 with the
        # throttled anchor-missing diagnostic instead of stale state.
        # The compiled half executes the real production gate/recovery logic
        # over the pre/capture/post event sequence; the source half pins the
        # Store and dllmain wiring that feeds it.
        compiler = shutil.which("c++") or shutil.which("g++")
        if compiler:
            source = textwrap.dedent(r"""
                #include "src/active_snapshot_root.h"
                #include <cassert>
                #include <vector>
                enum class Flag { None, PendingKill, Unreachable };
                struct GameState;
                struct GameMode { GameState* game_state; Flag flag = Flag::None; };
                struct World { GameMode* game_mode; Flag flag = Flag::None; };
                struct GameState { World* world; Flag flag = Flag::None; World* GetWorld() const { return world; } };
                struct ScanSpy {
                    int invocations = 0;
                    std::vector<GameState*> objects;
                    std::vector<GameState*> operator()() { ++invocations; return objects; }
                };
                template <typename T>
                struct Weak {
                    const T* value{};
                    const T* Get() const {
                        return (value && value->flag != Flag::PendingKill && value->flag != Flag::Unreachable) ? value : nullptr;
                    }
                };
                int main() {
                    const auto readable = [](const auto* object) { return object && object->flag == Flag::None; };
                    const auto world_of = [](GameState* game_state) { return game_state->world; };
                    const auto mode_of = [](const World* world) { return world->game_mode; };
                    const auto state_of = [](const GameMode* game_mode) { return game_mode->game_state; };

                    GameMode old_mode{nullptr};
                    World old_world{&old_mode};
                    GameState stale{&old_world};
                    old_mode.game_state = &stale;
                    Weak<GameState> stale_weak{&stale};
                    Weak<World> old_world_weak{&old_world};

                    // (1) The reviewed defect's mechanism, kept as contrast: if
                    // pre left the OLD anchor live (only the cache cleared), a
                    // capture during the travel window re-admits the old root.
                    {
                        ScanSpy spy; spy.objects = {&stale};
                        const auto decision = RecoverActiveSnapshotRoot<GameState>(true, &old_world, spy, readable, world_of, mode_of, state_of);
                        assert(decision.outcome == SnapshotRecoveryOutcome::Recovered);
                        assert(decision.admitted == &stale);
                    }
                    // (2) Fixed behavior: pre invalidated cache AND anchor, so
                    // the same capture between pre and post refuses BEFORE the
                    // scan and the serve gate refuses the (cleared) cache too.
                    {
                        ScanSpy spy; spy.objects = {&stale};
                        const auto decision = RecoverActiveSnapshotRoot<GameState>(true, nullptr, spy, readable, world_of, mode_of, state_of);
                        assert(decision.outcome == SnapshotRecoveryOutcome::NoCurrentWorld);
                        assert(decision.admitted == nullptr);
                        assert(spy.invocations == 0);
                        assert(ResolveServedSnapshotRoot(stale_weak, old_world_weak, nullptr, readable, mode_of, state_of) == nullptr);
                    }
                    // (3) Post notification never delivered to this mod: the
                    // anchor never returns; later captures still refuse.
                    // Permanent typed 503 beats serving stale state.
                    {
                        ScanSpy spy; spy.objects = {&stale};
                        const auto decision = RecoverActiveSnapshotRoot<GameState>(true, nullptr, spy, readable, world_of, mode_of, state_of);
                        assert(decision.outcome == SnapshotRecoveryOutcome::NoCurrentWorld);
                        assert(ResolveServedSnapshotRoot(stale_weak, old_world_weak, nullptr, readable, mode_of, state_of) == nullptr);
                    }
                    // (4) Post notification delivered: anchor = new world. The
                    // old pair refuses (anchor moved on); the new world's root
                    // is admitted through the chain and serves once cached.
                    {
                        GameMode new_mode{nullptr};
                        World new_world{&new_mode};
                        GameState fresh{&new_world};
                        new_mode.game_state = &fresh;
                        ScanSpy spy; spy.objects = {&fresh};
                        const auto decision = RecoverActiveSnapshotRoot<GameState>(true, &new_world, spy, readable, world_of, mode_of, state_of);
                        assert(decision.outcome == SnapshotRecoveryOutcome::Recovered);
                        assert(decision.admitted == &fresh);
                        Weak<GameState> fresh_weak{&fresh};
                        Weak<World> new_world_weak{&new_world};
                        assert(ResolveServedSnapshotRoot(fresh_weak, new_world_weak, &new_world, readable, mode_of, state_of) == &fresh);
                        assert(ResolveServedSnapshotRoot(stale_weak, old_world_weak, &new_world, readable, mode_of, state_of) == nullptr);
                    }
                }
            """)
            with tempfile.TemporaryDirectory() as directory:
                source_path = Path(directory) / "travel.cpp"
                binary_path = Path(directory) / "travel"
                source_path.write_text(source)
                subprocess.run(
                    [compiler, "-std=c++17", "-I", str(ROOT), str(source_path), "-o", str(binary_path)],
                    check=True,
                )
                subprocess.run([str(binary_path)], check=True)
        else:
            self.skipTest("C++ compiler unavailable")

        # Store wiring: one lock, all three travel-derived pointers dropped.
        source = (ROOT / "src/snapshot.cpp").read_text()
        invalidate_body = source.split("auto Store::invalidate_travel_state", 1)[1].split("auto Store::set_current_world", 1)[0]
        self.assertIn("s_active_game_state = nullptr;", invalidate_body)
        self.assertIn("s_active_world = nullptr;", invalidate_body)
        self.assertIn("s_current_world = nullptr;", invalidate_body)
        # The anchor-missing refusal is observable at a level <= Normal.
        self.assertIn('L"anchor-missing"', source)

        # dllmain wiring: the PRE notification invalidates travel state (not
        # merely the root cache), the POST notification refreshes the anchor,
        # and each just-registered callback is moved to the FRONT of its
        # pinned dispatcher vector so no earlier-registered peer can end the
        # interruptible loop before ours has run.
        dll = (ROOT / "src/dllmain.cpp").read_text()
        pre_body = dll.split("RegisterLoadMapPreCallback(", 1)[1].split("RegisterLoadMapPostCallback(", 1)[0]
        self.assertIn("Store::invalidate_travel_state()", pre_body)
        self.assertNotIn("clear_active_game_state", pre_body)
        post_body = dll.split("RegisterLoadMapPostCallback(", 1)[1].split("RegisterInitGameStatePostCallback", 1)[0]
        self.assertIn("Store::set_current_world(", post_body)
        pre_rotate = "move_just_registered_to_front(Unreal::Hook::StaticStorage::LoadMapPreCallbacks)"
        post_rotate = "move_just_registered_to_front(Unreal::Hook::StaticStorage::LoadMapPostCallbacks)"
        self.assertIn(pre_rotate, dll)
        self.assertIn(post_rotate, dll)
        self.assertLess(dll.index("RegisterLoadMapPreCallback("), dll.index(pre_rotate))
        self.assertLess(dll.index("RegisterLoadMapPostCallback("), dll.index(post_rotate))

    def test_snapshot_recovery_runs_only_on_authorized_game_thread(self):
        # Named failure this catches: the recovery path scans the global
        # UObjectArray (FindAllOf). If that scan can run from the HTTP worker
        # thread, it races the game thread's object mutations exactly like the
        # Run-13 D2 defect class. The behavioral bite (the guard prevents the
        # scan, spy stays at zero) is compiled-tested in
        # test_recovery_refuses_off_thread_unanchored_and_unreadable_chains;
        # this check pins the production WIRING to that compiled logic: the
        # whole resolution (cache gate included) sits behind the GameThread
        # guard, and the FindAllOf call exists exactly once, inside the
        # recovery decision's scan lambda.
        source = (ROOT / "src/snapshot.cpp").read_text()
        resolve = source.split("auto Store::resolve_active_game_state", 1)[1].split("auto Store::push_value", 1)[0]
        self.assertIn("LuaMod::is_in_game_thread()", resolve)
        # The refusal must live INSIDE the guard's braced body (review-2 X2:
        # deleting only the guard's 'return nullptr' kept the suite green when
        # merely the guard text and its order were checked).
        guard = re.search(r"if \(!LuaMod::is_in_game_thread\(\)\)\s*\{([^{}]*)\}", resolve)
        self.assertTrue(guard, "the off-thread refusal must be a braced guard block")
        guard_body = guard.group(1)
        self.assertIn("log_snapshot_diag_throttled", guard_body)
        self.assertIn("return nullptr;", guard_body)
        self.assertLess(guard_body.index("log_snapshot_diag_throttled"), guard_body.index("return nullptr;"))
        self.assertLess(resolve.index("LuaMod::is_in_game_thread()"), resolve.index("ResolveServedSnapshotRoot"))
        self.assertEqual(source.count("UObjectGlobals::FindAllOf"), 1, "the UObjectArray scan must exist exactly once")
        self.assertIn("FindAllOf", resolve)
        self.assertLess(resolve.index("LuaMod::is_in_game_thread()"), resolve.index("FindAllOf"))
        # The only class scanned is the contract's native GameState class.
        self.assertIn('STR("MotorTownGameState")', resolve)
        # The compiled decision is actually wired in (with the live thread flag).
        self.assertIn("RecoverActiveSnapshotRoot<UObject>", resolve)

    def test_snapshot_recovery_preserves_fail_closed_contract(self):
        # Named failure this catches: recovery must cache only through the
        # Store's weak-pointer registration and must still refuse with the
        # typed 503 error when nothing resolvable exists. The served candidate
        # must pass through the same weak-cache gate as the cache path (never
        # a raw pointer that escapes weak resolution), the serve gate must
        # re-read the authority backlink, and the current-world anchor must be
        # wired from the LoadMap post callback.
        source = (ROOT / "src/snapshot.cpp").read_text()
        self.assertIn('"active MotorTownGameState is unavailable"', source)
        resolve = source.split("auto Store::resolve_active_game_state", 1)[1].split("auto Store::push_value", 1)[0]
        self.assertIn("Store::set_active_game_state(", resolve)
        self.assertNotIn("s_active_game_state =", resolve)
        self.assertEqual(source.count("serve_cached()"), 2, "cache gate before recovery and again before serving a recovered root")
        # The recovered raw pointer must never escape directly: the resolver's
        # final statement is the weak re-resolution itself (review-2 X4:
        # inserting 'return decision.admitted;' before the final serve gate
        # kept every source-occurrence count green).
        self.assertNotIn("return decision.admitted", resolve)
        self.assertTrue(
            re.search(r"return serve_cached\(\);\s*\}\s*\Z", resolve),
            "the resolver's final statement must be the serve_cached() weak re-resolution",
        )
        # Serve-time backlink revalidation: the gate re-reads the authority
        # chain through reflection instead of trusting registration time.
        self.assertIn("ResolveServedSnapshotRoot", resolve)
        self.assertIn('STR("AuthorityGameMode")', resolve)
        self.assertIn('STR("GameState")', resolve)
        self.assertIn("resolve_current_world()", resolve)
        # Pinned C++ completeness contract: recovery feeds the UWorld* returned
        # by UObject::GetWorld() to code that needs the complete type (the
        # UObject* upcast into set_active_game_state, GetName). The pinned
        # overlay only forward-declares UWorld, so the explicit
        # <Unreal/World.hpp> include is required for the MSVC build.
        self.assertIn("#include <Unreal/World.hpp>", source)
        # PendingKill gameplay-validity gate on every reflection read.
        readable_body = source.split("auto object_is_readable", 1)[1].split("auto read_object_property", 1)[0]
        self.assertIn("HasAnyInternalFlags(EInternalObjectFlags::PendingKill)", readable_body)
        # Current-world anchor wiring in dllmain: the engine hands over its own
        # current world only inside the LoadMap post callback.
        dll = (ROOT / "src/dllmain.cpp").read_text()
        self.assertIn("RegisterLoadMapPostCallback", dll)
        self.assertIn("GetThisCurrentWorld()", dll)
        self.assertIn("Store::set_current_world(", dll)

    def test_lifecycle_diagnostics_survive_canary_log_level(self):
        # Named failure this catches: (1) [SnapshotDiag] lines emitted with
        # the default LogLevel::Default template argument are suppressed at
        # the frozen canary level (MOD_SERVER_LOG_LEVEL=2), which is why 18
        # canary runs produced zero [SnapshotDiag] evidence; every lifecycle
        # registration/resolution diagnostic must pin an explicit level of
        # Normal or stronger. (2) Labeling the PolyHook detour pointer
        # 'detour_installed' overclaims: the pinned overlay assigns the detour
        # object before calling hook() and discards hook()'s result, so the
        # boot line must use the truthful 'detour_object_present' label. The
        # reported fields carry the hook configuration flags, resolved
        # signature and callback counts as their own facts; none of them is a
        # statement about install, delivery, or the cause of a missing post
        # line.
        # (3) The one-shot lifecycle status call spans multiple lines, so a
        # line-based check never inspects the line carrying LogOutput and its
        # template argument together; a per-call-site regex is required to
        # catch a silent revert to Default there.
        snapshot_diag_call = re.compile(
            r"ModStatics::LogOutput\s*(?:<(?P<level>[^>]*)>)?\s*\(\s*L?\"(?P<text>[^\"]*)"
        )

        def require_explicit_diag_levels(source_name, source_text):
            for match in snapshot_diag_call.finditer(source_text):
                if not match.group("text").startswith("[SnapshotDiag]"):
                    continue
                level = match.group("level") or ""
                self.assertTrue(
                    "LogLevel::Normal" in level or "LogLevel::Warning" in level,
                    f"{source_name}: [SnapshotDiag] LogOutput lacks explicit level <= Normal: {match.group(0)[:120]!r}",
                )

        dll = (ROOT / "src/dllmain.cpp").read_text()
        self.assertNotIn('ModStatics::LogOutput(L"[SnapshotDiag]', dll)
        require_explicit_diag_levels("dllmain.cpp", dll)
        status = dll.split("[SnapshotDiag] lifecycle hook status", 1)[1]
        self.assertIn("signature_ready", status)
        self.assertIn("signature_address", status)
        # Truthful labeling: the detour pointer proves an object exists, not
        # that PolyHook installed (hook()'s result is discarded upstream).
        self.assertIn("detour_object_present", status)
        self.assertNotIn("detour_installed", dll)
        # Registration-decision inputs from the pinned overlay config.
        self.assertIn("hook_configured", status)
        self.assertIn("GlobalConfig.bHookInitGameState", dll)
        self.assertIn("GlobalConfig.bHookLoadMap", dll)
        self.assertIn("#include <Unreal/UnrealInitializer.hpp>", dll)
        # Callback registration is observable.
        self.assertIn("LoadMapPreCallbacks", dll)
        self.assertIn("LoadMapPostCallbacks", dll)
        self.assertIn("InitGameStatePreCallbacks", dll)
        self.assertIn("InitGameStatePostCallbacks", dll)
        self.assertIn("InitGameStateDetour", dll)
        # The boot line reports both LoadMap vector sizes as registration
        # counts (the current-world anchor's source). A count proves only that
        # a callback is registered in the pinned dispatcher vector; it does not
        # prove hook install or callback delivery.
        self.assertGreaterEqual(status.count("post_callbacks"), 2)
        snapshot = (ROOT / "src/snapshot.cpp").read_text()
        self.assertNotIn('ModStatics::LogOutput(L"[SnapshotDiag]', snapshot)
        require_explicit_diag_levels("snapshot.cpp", snapshot)

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
