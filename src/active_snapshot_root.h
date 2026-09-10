#pragma once

template <typename WeakRoot, typename WeakWorld>
auto ResolveActiveSnapshotRoot(const WeakRoot& root, const WeakWorld& world) -> decltype(root.Get())
{
    auto* resolved_root = root.Get();
    auto* resolved_world = world.Get();
    if (!resolved_root || !resolved_world || resolved_root->GetWorld() != resolved_world)
    {
        return nullptr;
    }
    return resolved_root;
}
