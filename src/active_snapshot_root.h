#pragma once

#include <cstdint>
#include <type_traits>
#include <vector>

// Pure decision logic for active-snapshot-root recovery, cache reuse and
// travel-lifecycle authority. Nothing here touches Unreal: the caller
// supplies the lifecycle samples and callables for the scan, readability and
// reflection reads, so the full admission/refusal sequence is compiled and
// executed by tests without engine headers. The production wiring lives in
// src/snapshot.cpp.

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

// Recovery decision. current_world is the last engine current-world identity
// delivered to this module (FWorldContext::GetThisCurrentWorld from the pinned
// overlay's LoadMap post callback, stored as a weak pointer). The identity
// check refuses a candidate from a different world, but it is only as current
// as the notifications this module received: a competing registrar can veto
// before this module's pre callback and leave an older anchor usable. The scan
// callable is invoked at most once and only after both preconditions hold.
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

// Sampled travel-lifecycle authority state (plain data; no engine types).
// Production fills it from the Store under one lock (snapshot.cpp
// sample_lifecycle_state); tests construct and mutate it directly.
struct SnapshotLifecycleState
{
    // Resolved last-delivered engine-anchor identity
    // (FWorldContext::GetThisCurrentWorld from the pinned overlay's LoadMap
    // post callback). Null means no usable anchor: never delivered, invalidated
    // and not yet refreshed, or the weak pointer no longer resolves. Non-null
    // does not prove freshness when a competing registrar vetoed delivery.
    const void* current_world{};
    // Monotonic: advanced on every travel invalidation (LoadMap pre) and every
    // anchor refresh (LoadMap post). Any advance invalidates admissions
    // sampled before it.
    uint64_t travel_generation{};
    // Delivered LoadMap pre notifications whose matching post has not been
    // delivered. While this is non-zero a travel interval is open; an inner
    // travel's post must not restore serve authority while an enclosing
    // LoadMap is still open.
    uint64_t open_travels{};
};

enum class SnapshotResolutionRefusal
{
    Proceed,          // preconditions hold; resolution may continue
    OffGameThread,    // refused first, before any state consultation
    AnchorMissing,    // no resolved engine anchor: nothing can be proven current
    TravelInProgress  // a LoadMap interval is open; authority not yet delivered
};

// Entry precondition seam for Store::resolve_active_game_state. The thread
// check wins FIRST so an unauthorized caller is refused no matter how healthy
// the sampled state looks (production therefore samples the lifecycle state
// only after its own thread check has passed).
inline auto EvaluateResolutionPreconditions(bool on_game_thread, const SnapshotLifecycleState& state) -> SnapshotResolutionRefusal
{
    if (!on_game_thread) return SnapshotResolutionRefusal::OffGameThread;
    if (!state.current_world) return SnapshotResolutionRefusal::AnchorMissing;
    if (state.open_travels != 0) return SnapshotResolutionRefusal::TravelInProgress;
    return SnapshotResolutionRefusal::Proceed;
}

// Final decision-point seam. A pair admitted against the sampled state may be
// stored or returned only while the LIVE lifecycle state still matches the
// sample field for field: the generation has not advanced (no invalidation or
// refresh happened since sampling), no travel interval is open, and the
// anchor is the same identity. Every field is load-bearing: any observed drift
// refuses fail-closed, and the next request re-samples the latest state delivered
// to this module. A veto before this module's LoadMap callback can leave every
// field unchanged around a stale anchor.
inline auto AdmissionStillValid(const SnapshotLifecycleState& sampled, const SnapshotLifecycleState& live) -> bool
{
    return sampled.travel_generation == live.travel_generation &&
           sampled.open_travels == live.open_travels &&
           sampled.current_world == live.current_world;
}

enum class RecoveredTailVerdict
{
    Cacheable,                // world returned; caching/serving may proceed
    RootUnreadable,           // admitted root failed the gameplay gate; no engine world read ran on it
    WorldMissingOrUnreadable  // the world read returned null or failed the gameplay gate
};

template <typename World>
struct RecoveredTailResult
{
    RecoveredTailVerdict verdict{};
    World* world{};
};

// Recovery-tail seam. Same ordering discipline as the cache gate: the
// recovered root passes a fresh gameplay-readability check BEFORE the engine
// world read (world_of) runs on it, and the read result is gated too, so an
// unreadable root is never dereferenced into engine code. world_of is invoked
// at most once, and only after the root passed the gate.
template <typename Candidate, typename IsReadable, typename WorldOf>
auto EvaluateRecoveredTail(Candidate* admitted, IsReadable is_readable, WorldOf world_of) -> RecoveredTailResult<std::remove_pointer_t<decltype(world_of(admitted))>>
{
    if (!admitted || !is_readable(admitted))
        return {RecoveredTailVerdict::RootUnreadable, nullptr};
    auto* world = world_of(admitted);
    if (!world || !is_readable(world))
        return {RecoveredTailVerdict::WorldMissingOrUnreadable, nullptr};
    return {RecoveredTailVerdict::Cacheable, world};
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
//   4. the cached world IS the last LoadMap-delivered current-world anchor
//      (identity compared as const void* so an incomplete UWorld forward
//      declaration compiles),
//   5. the freshly re-read authority GameMode is gameplay-readable, and
//   6. that GameMode's live GameState still points back at the exact root,
//      re-read through reflection at serve time rather than trusted from the
//      moment of registration.
// Anything failing these checks refuses. They do not prove engine freshness if
// a competing registrar vetoes before this module's LoadMap notification: the
// prior anchor and its internally consistent authority chain may still pass.
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
