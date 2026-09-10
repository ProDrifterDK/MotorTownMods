#pragma once

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
