#pragma once

#include <cstdint>
#include <vector>

namespace qray::rhi
{

inline bool DynamicShapeFits(const std::vector<uint32_t> &envelope, const std::vector<uint32_t> &current)
{
    if (envelope.size() % 5 != 0 || current.size() % 5 != 0 || current.size() > envelope.size())
    {
        return false;
    }

    for (size_t i = 0; i < current.size(); i += 5)
    {
        if (current[i] > envelope[i] || current[i + 1] > envelope[i + 1] ||
            current[i + 2] != envelope[i + 2] || current[i + 3] != envelope[i + 3] ||
            current[i + 4] != envelope[i + 4])
        {
            return false;
        }
    }

    return true;
}

}
