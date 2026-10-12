#pragma once

#include <algorithm>
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

inline bool DynamicShapeSortedFits(const std::vector<uint32_t> &envelope, const std::vector<uint32_t> &current)
{
    if (envelope.size() % 5 != 0 || current.size() % 5 != 0 || current.size() > envelope.size())
    {
        return false;
    }

    const size_t geometryCount = current.size() / 5;

    if (geometryCount == 0)
    {
        return true;
    }

    std::vector<uint32_t> envelopeOrder(envelope.size() / 5);
    std::vector<uint32_t> currentOrder(geometryCount);

    for (uint32_t i = 0; i < envelopeOrder.size(); i++)
    {
        envelopeOrder[i] = i;
    }
    for (uint32_t i = 0; i < currentOrder.size(); i++)
    {
        currentOrder[i] = i;
    }

    const auto tupleGreater = [](const std::vector<uint32_t> &shape, uint32_t a, uint32_t b) {
        for (uint32_t k = 0; k < 5; k++)
        {
            if (shape[size_t(a) * 5 + k] != shape[size_t(b) * 5 + k])
            {
                return shape[size_t(a) * 5 + k] > shape[size_t(b) * 5 + k];
            }
        }
        return false;
    };

    std::sort(envelopeOrder.begin(), envelopeOrder.end(),
              [&](uint32_t a, uint32_t b) { return tupleGreater(envelope, a, b); });
    std::sort(currentOrder.begin(), currentOrder.end(),
              [&](uint32_t a, uint32_t b) { return tupleGreater(current, a, b); });

    for (size_t k = 0; k < geometryCount; k++)
    {
        const uint32_t *c = &current[size_t(currentOrder[k]) * 5];
        const uint32_t *e = &envelope[size_t(envelopeOrder[k]) * 5];

        if (c[0] > e[0] || c[1] > e[1] || c[2] != e[2] || c[3] != e[3] || c[4] != e[4])
        {
            return false;
        }
    }

    return true;
}

}
