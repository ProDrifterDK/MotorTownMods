#pragma once

#include <cstddef>
#include <vector>

// On-demand recovery selector: picks the newest admissible candidate root.
// A candidate is admissible only when it is readable (not pending
// destruction) and anchored to a live world; everything else is refused so
// the caller can fail closed instead of serving freed or unanchored state.
template <typename Candidate, typename IsReadable, typename WorldOf>
auto SelectRecoveredSnapshotRoot(const std::vector<Candidate*>& candidates, IsReadable is_readable, WorldOf world_of) -> Candidate*
{
    // UObjectArray allocation order makes the last element the newest
    // instance, so scan backwards and take the first admissible one.
    for (auto it = candidates.rbegin(); it != candidates.rend(); ++it)
    {
        auto* candidate = *it;
        if (!candidate || !is_readable(candidate)) continue;
        if (!world_of(candidate)) continue;
        return candidate;
    }
    return nullptr;
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
