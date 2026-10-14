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

// The rank-wise dominance test behind `rt_dyn_blas_sorted`: the envelope's geometry count and its
// primitive counts sorted in descending order must dominate the current list's, so any reordering
// of the list fits. A BLAS allocation is a function of its geometry count and its per-geometry
// primitive counts (for a non-indexed geometry that count is the vertex count's quotient, which the
// envelope element carries; the stride, the vertex format and the transform presence do not enter
// vkGetAccelerationStructureBuildSizesKHR), and rank-wise dominance of the counts implies
// the current primitive sum is at most the envelope's, so a build this test accepts always fits the
// create-time allocation. The identity fields are not compared: the stride and the vertex format
// are invariants of the filter key (VertexCollector) and of MakeGeometryDesc, and the transform
// presence only selects a matrix address in the build range.
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

    std::vector<uint32_t> envelopeCounts(envelope.size() / 5);
    std::vector<uint32_t> currentCounts(geometryCount);

    for (size_t i = 0; i < envelopeCounts.size(); i++)
    {
        envelopeCounts[i] = envelope[i * 5];
    }
    for (size_t i = 0; i < geometryCount; i++)
    {
        currentCounts[i] = current[i * 5];
    }

    const auto greater = [](uint32_t a, uint32_t b) { return a > b; };

    std::sort(envelopeCounts.begin(), envelopeCounts.end(), greater);
    std::sort(currentCounts.begin(), currentCounts.end(), greater);

    for (size_t i = 0; i < geometryCount; i++)
    {
        if (currentCounts[i] > envelopeCounts[i])
        {
            return false;
        }
    }

    return true;
}

}
