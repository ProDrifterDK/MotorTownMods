#pragma once

#include <cstdint>

template <typename IsValid, typename Visit>
inline void ForEachOccupiedSlot(int32_t max_index, IsValid&& is_valid, Visit&& visit)
{
    for (int32_t index = 0; index < max_index; ++index)
    {
        if (is_valid(index)) visit(index);
    }
}
