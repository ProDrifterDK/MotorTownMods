#pragma once

#include <vector>

// On-demand recovery selector: admits a candidate root only when the
// authoritative chain proves it is the live game state of a current world:
// readable candidate -> readable world -> readable world AuthorityGameMode
// -> that GameMode's live game state is the exact candidate. A readable
// orphan (not the GameMode's game state), a world without an authority game
// mode, and unreadable/unanchored candidates are all refused. When zero or
// multiple candidates pass the chain (both the old and the new world can be
// readable mid-travel) the selection fails closed with nullptr so the
// caller refuses instead of serving stale or ambiguous state.
template <typename Candidate, typename IsReadable, typename WorldOf, typename AuthorityGameModeOf, typename GameStateOf>
auto SelectRecoveredSnapshotRoot(const std::vector<Candidate*>& candidates, IsReadable is_readable, WorldOf world_of, AuthorityGameModeOf authority_game_mode_of, GameStateOf game_state_of) -> Candidate*
{
    Candidate* admitted = nullptr;
    for (auto* candidate : candidates)
    {
        if (!candidate || !is_readable(candidate)) continue;
        auto* world = world_of(candidate);
        if (!world || !is_readable(world)) continue;
        auto* game_mode = authority_game_mode_of(world);
        if (!game_mode || !is_readable(game_mode)) continue;
        if (game_state_of(game_mode) != candidate) continue;
        // FindAllOf yields unique objects; a second chain-admissible
        // candidate means two live authoritative roots: refuse, never guess.
        if (admitted) return nullptr;
        admitted = candidate;
    }
    return admitted;
}

template <typename WeakRoot, typename WeakWorld>
auto ResolveActiveSnapshotRoot(const WeakRoot& root, const WeakWorld& world) -> decltype(root.Get())
{
    auto* resolved_root = root.Get();
    auto* resolved_world = world.Get();
    // Compare identity via void* so the check works even when UWorld is an
    // incomplete forward-declared type in the including translation unit.
    if (!resolved_root || !resolved_world ||
        static_cast<const void*>(resolved_root->GetWorld()) != static_cast<const void*>(resolved_world))
    {
        return nullptr;
    }
    return resolved_root;
}
