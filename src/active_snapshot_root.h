#pragma once

#include <vector>

// Pure decision logic for active-snapshot-root recovery and cache reuse.
// Nothing here touches Unreal: the caller supplies callables for the scan,
// readability and reflection reads, so the full admission/refusal sequence
// is compiled and executed by tests without engine headers. The production
// wiring lives in src/snapshot.cpp.

// Why a recovery attempt was refused or admitted. The production caller maps
// each outcome to a throttled [SnapshotDiag] line.
enum class SnapshotRecoveryOutcome
{
    OffGameThread,   // refused before the scan: UObjectArray reads are game-thread-only
    NoCurrentWorld,  // refused before the scan: no engine-established current-world identity
    Recovered,       // exactly one candidate admitted
    Unresolved,      // scan ran, zero candidates admitted
    Ambiguous        // scan ran, multiple candidates admitted: refuse, never guess
};

template <typename Candidate>
struct SnapshotRecoveryDecision
{
    Candidate* admitted{};
    SnapshotRecoveryOutcome outcome{};
};

// Recovery decision. current_world is the engine's own record of the world it
// made current (FWorldContext::GetThisCurrentWorld, captured inside the pinned
// overlay's LoadMap post callback and stored as a weak pointer). A world only
// chain-admissible in an older world is refused by the current-world identity
// comparison, which is what makes mid-travel/old-world chains fail closed
// instead of serving stale state. The scan callable is invoked at most once
// and only after both preconditions hold.
template <typename Candidate, typename Scan, typename IsReadable, typename WorldOf, typename AuthorityGameModeOf, typename GameStateOf>
auto RecoverActiveSnapshotRoot(bool on_game_thread, const void* current_world, Scan scan, IsReadable is_readable, WorldOf world_of, AuthorityGameModeOf authority_game_mode_of, GameStateOf game_state_of) -> SnapshotRecoveryDecision<Candidate>
{
    if (!on_game_thread) return {nullptr, SnapshotRecoveryOutcome::OffGameThread};
    if (!current_world) return {nullptr, SnapshotRecoveryOutcome::NoCurrentWorld};
    const auto candidates = scan();
    Candidate* admitted = nullptr;
    for (auto* candidate : candidates)
    {
        if (!candidate || !is_readable(candidate)) continue;
        auto* world = world_of(candidate);
        if (!world || !is_readable(world)) continue;
        if (static_cast<const void*>(world) != current_world) continue;
        auto* game_mode = authority_game_mode_of(world);
        if (!game_mode || !is_readable(game_mode)) continue;
        if (game_state_of(game_mode) != candidate) continue;
        // A second chain-admissible candidate means two live authoritative
        // roots (e.g. the authority backlink being re-read while the engine
        // re-points it): refuse, never guess.
        if (admitted) return {nullptr, SnapshotRecoveryOutcome::Ambiguous};
        admitted = candidate;
    }
    return {admitted, admitted ? SnapshotRecoveryOutcome::Recovered : SnapshotRecoveryOutcome::Unresolved};
}

// Cache-reuse gate. A cached pair is served only when:
//   1. both weak pointers resolve. The pinned FWeakObjectPtr::Get default
//      validity rejects null/stale-serial identities, Unreachable and
//      PendingKill referents; it does NOT check RF_BeginDestroyed or
//      RF_FinishDestroyed - those are this module's additional checks and
//      are applied by is_readable in step 2,
//   2. root and world pass the gameplay-readability gate BEFORE any engine
//      virtual is called on them (UObject::GetWorld() below is an engine
//      call, not a passive pointer comparison, so a pending-destroyed root
//      is refused without being dereferenced into it),
//   3. the resolved root's world is the cached world,
//   4. the cached world IS the engine's current world (identity compared as
//      const void* so an incomplete UWorld forward declaration compiles),
//   5. the freshly re-read authority GameMode is gameplay-readable, and
//   6. that GameMode's live GameState still points back at the exact root,
//      re-read through reflection at serve time rather than trusted from the
//      moment of registration.
// Anything else refuses: a stale, orphaned, ambiguous or dead root is never
// served.
template <typename WeakRoot, typename WeakWorld, typename IsReadable, typename AuthorityGameModeOf, typename GameStateOf>
auto ResolveServedSnapshotRoot(const WeakRoot& root, const WeakWorld& world, const void* current_world, IsReadable is_readable, AuthorityGameModeOf authority_game_mode_of, GameStateOf game_state_of) -> decltype(root.Get())
{
    auto* resolved_root = root.Get();
    auto* resolved_world = world.Get();
    if (!resolved_root || !resolved_world) return nullptr;
    // Readability BEFORE engine state: weak resolution alone does not reject
    // RF_BeginDestroyed/RF_FinishDestroyed, and GetWorld() is an engine
    // virtual wrapper, so an otherwise-authoritative root with those flags
    // must be refused before GetWorld() touches it.
    if (!is_readable(resolved_root) || !is_readable(resolved_world)) return nullptr;
    // The anchor identity is checked before the root's GetWorld() so a stale
    // anchor refuses without an engine call. Compare identity via void* so
    // the check works even when UWorld is an incomplete forward-declared
    // type in the including translation unit.
    if (static_cast<const void*>(resolved_world) != current_world) return nullptr;
    if (static_cast<const void*>(resolved_root->GetWorld()) != static_cast<const void*>(resolved_world)) return nullptr;
    auto* game_mode = authority_game_mode_of(resolved_world);
    if (!game_mode || !is_readable(game_mode)) return nullptr;
    if (game_state_of(game_mode) != resolved_root) return nullptr;
    return resolved_root;
}
