#include "LightGrid.h"

#include "rt_alias.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>

namespace qray
{

namespace
{

uint32_t Percentile(const std::vector<uint32_t> &sorted, int percent)
{
    if (sorted.empty())
    {
        return 0;
    }

    size_t index = size_t((double)(sorted.size() - 1) * (double)percent / 100.0);

    if (index >= sorted.size())
    {
        index = sorted.size() - 1;
    }

    return sorted[index];
}

}

void LightGrid::Reset()
{
    valid = false;
    stats = {};
    offsets.clear();
    list.clear();
    tailOffsets.clear();
    tailUids.clear();
    tailProb.clear();
    tailMarginal.clear();
    tailAlias.clear();
    tailBeta.clear();
}

void LightGrid::Build(const LightGridSource *sources, uint32_t sourceCount, const float viewOrigin[3])
{
    const auto start = std::chrono::steady_clock::now();

    Reset();

    stats.cells = kCells;
    stats.cascades = kCascades;
    stats.sources = sourceCount;
    stats.cellSize[0] = kBaseCell;
    stats.cellSize[1] = kBaseCell * 2.0f;

    if (sources == nullptr || sourceCount == 0 || viewOrigin == nullptr)
    {
        return;
    }

    float snapOrigin[3];

    for (int axis = 0; axis < 3; axis++)
    {
        const double value = std::isfinite(viewOrigin[axis]) ? double(viewOrigin[axis]) : 0.0;
        snapOrigin[axis] = float(std::floor(value / double(kSnap)) * double(kSnap));
    }

    std::vector<uint32_t> cellCounts(kCells, 0);

    auto visitSource = [&](const LightGridSource &source, uint32_t cascade, const float origin[3], float cell,
                           uint32_t cellBase, auto &&visitor)
    {
        const float reach = std::isfinite(source.reach) && source.reach > 0.0f ? source.reach : 0.0f;
        const float radius = std::isfinite(source.radius) && source.radius > 0.0f ? source.radius : 0.0f;
        const float influence = reach + radius + 32.0f;
        const float influence2 = influence * influence;
        int lo[3];
        int hi[3];
        bool inside = true;

        for (int axis = 0; axis < 3; axis++)
        {
            const float value = std::isfinite(source.origin[axis]) ? source.origin[axis] : 0.0f;
            const int low = int(std::floor((double)(value - influence - origin[axis]) / cell));
            const int high = int(std::floor((double)(value + influence - origin[axis]) / cell));

            lo[axis] = std::max(0, low);
            hi[axis] = std::min(int(kDim) - 1, high);

            if (lo[axis] > hi[axis])
            {
                inside = false;
            }
        }

        if (!inside)
        {
            return;
        }

        const double scale = std::max(1.0, double(radius) * double(radius));
        const double power = std::isfinite(source.power) && source.power > 0.0f ? double(source.power) : 0.0;

        for (int z = lo[2]; z <= hi[2]; z++)
        {
            const double centerZ = double(origin[2]) + (double(z) + 0.5) * double(cell);
            double dz = std::fabs(double(source.origin[2]) - centerZ) - double(cell) * 0.5;

            if (dz < 0.0)
            {
                dz = 0.0;
            }

            for (int y = lo[1]; y <= hi[1]; y++)
            {
                const double centerY = double(origin[1]) + (double(y) + 0.5) * double(cell);
                double dy = std::fabs(double(source.origin[1]) - centerY) - double(cell) * 0.5;

                if (dy < 0.0)
                {
                    dy = 0.0;
                }

                for (int x = lo[0]; x <= hi[0]; x++)
                {
                    const double centerX = double(origin[0]) + (double(x) + 0.5) * double(cell);
                    double dx = std::fabs(double(source.origin[0]) - centerX) - double(cell) * 0.5;

                    if (dx < 0.0)
                    {
                        dx = 0.0;
                    }

                    const double dist2 = dx * dx + dy * dy + dz * dz;

                    if (dist2 > double(influence2))
                    {
                        continue;
                    }

                    const uint32_t local = uint32_t((z * int(kDim) + y) * int(kDim) + x);
                    visitor(cellBase + local, float(dist2), float(power / std::max(dist2, scale)));
                }
            }
        }
    };

    for (uint32_t cascade = 0; cascade < kCascades; cascade++)
    {
        const float cell = kBaseCell * float(1u << cascade);
        const float bounds = cell * float(kDim);
        float origin[3];

        for (int axis = 0; axis < 3; axis++)
        {
            origin[axis] = snapOrigin[axis] - bounds * 0.5f;
            stats.origin[cascade].data[axis] = origin[axis];
        }

        const uint32_t cellBase = cascade * kCellsPerCascade;

        for (uint32_t sourceIndex = 0; sourceIndex < sourceCount; sourceIndex++)
        {
            visitSource(sources[sourceIndex], cascade, origin, cell, cellBase,
                        [&](uint32_t cellIndex, float, float) { cellCounts[cellIndex]++; });
        }
    }

    std::vector<uint32_t> cellStart(kCells + 1, 0);

    for (uint32_t i = 0; i < kCells; i++)
    {
        cellStart[i + 1] = cellStart[i] + cellCounts[i];
    }

    std::vector<Candidate> cellCandidates(cellStart[kCells]);
    std::vector<uint32_t> cellCursor(kCells);

    for (uint32_t i = 0; i < kCells; i++)
    {
        cellCursor[i] = cellStart[i];
    }

    for (uint32_t cascade = 0; cascade < kCascades; cascade++)
    {
        const float cell = kBaseCell * float(1u << cascade);
        const float bounds = cell * float(kDim);
        float origin[3];

        for (int axis = 0; axis < 3; axis++)
        {
            origin[axis] = snapOrigin[axis] - bounds * 0.5f;
        }

        const uint32_t cellBase = cascade * kCellsPerCascade;

        for (uint32_t sourceIndex = 0; sourceIndex < sourceCount; sourceIndex++)
        {
            const LightGridSource &source = sources[sourceIndex];

            visitSource(source, cascade, origin, cell, cellBase,
                        [&](uint32_t cellIndex, float dist2, float mass)
            {
                const uint32_t at = cellCursor[cellIndex]++;
                cellCandidates[at] = { source.uid, sourceIndex, dist2, mass };
            });
        }
    }

    offsets.assign(kCells + 1, 0);
    tailOffsets.assign(kCells + 1, 0);
    tailBeta.assign(kCells, 0.0f);
    list.clear();
    tailUids.clear();
    tailProb.clear();
    tailMarginal.clear();
    tailAlias.clear();

    std::vector<uint32_t> occupiedCounts;
    occupiedCounts.reserve(kCells);

    std::vector<Candidate> tailCandidates;
    std::vector<double> tailWeights;
    std::vector<float> blockProb;
    std::vector<uint32_t> blockAlias;
    std::vector<float> blockMarginal;

    for (uint32_t cellIndex = 0; cellIndex < kCells; cellIndex++)
    {
        const uint32_t candidateCount = cellCounts[cellIndex];

        offsets[cellIndex] = uint32_t(list.size());
        tailOffsets[cellIndex] = uint32_t(tailUids.size());

        if (candidateCount == 0)
        {
            continue;
        }

        Candidate *candidates = cellCandidates.data() + cellStart[cellIndex];

        occupiedCounts.push_back(candidateCount);

        std::sort(candidates, candidates + candidateCount, [](const Candidate &a, const Candidate &b)
        {
            if (a.dist2 != b.dist2)
            {
                return a.dist2 < b.dist2;
            }

            return a.uid < b.uid;
        });

        const uint32_t fastCount = std::min<uint32_t>(candidateCount, kMaxFastPerCell);
        double fastMass = 0.0;

        for (uint32_t i = 0; i < fastCount; i++)
        {
            list.push_back(candidates[i].uid);
            fastMass += double(candidates[i].mass);
        }

        tailCandidates.clear();

        for (uint32_t i = fastCount; i < candidateCount; i++)
        {
            tailCandidates.push_back(candidates[i]);
        }

        if (tailCandidates.empty())
        {
            continue;
        }

        if (tailCandidates.size() > kMaxTailPerCell)
        {
            stats.tailDropped += uint32_t(tailCandidates.size() - kMaxTailPerCell);
            std::nth_element(tailCandidates.begin(), tailCandidates.begin() + kMaxTailPerCell,
                             tailCandidates.end(), [](const Candidate &a, const Candidate &b)
            {
                if (a.mass != b.mass)
                {
                    return a.mass > b.mass;
                }

                return a.uid < b.uid;
            });
            tailCandidates.resize(kMaxTailPerCell);
        }

        double tailMass = 0.0;

        for (const Candidate &candidate : tailCandidates)
        {
            tailMass += double(candidate.mass);
        }

        float beta = 0.0f;

        if (fastCount == 0)
        {
            beta = 1.0f;
        }
        else if (fastMass > 0.0 || tailMass > 0.0)
        {
            const double ratio = tailMass / (fastMass + tailMass);
            beta = float(ratio < 0.1 ? 0.1 : (ratio > 0.9 ? 0.9 : ratio));
        }
        else
        {
            beta = 0.5f;
        }

        const double floorWeight = 0.001 * (fastMass + tailMass + 1.0);

        tailWeights.resize(tailCandidates.size());
        blockProb.resize(tailCandidates.size());
        blockAlias.resize(tailCandidates.size());
        blockMarginal.resize(tailCandidates.size());

        for (size_t i = 0; i < tailCandidates.size(); i++)
        {
            tailWeights[i] = double(tailCandidates[i].mass) + floorWeight;
        }

        if (!RT_Alias_Build(tailWeights.data(), int(tailCandidates.size()), blockProb.data(), blockAlias.data()))
        {
            continue;
        }

        RT_Alias_Marginals(blockProb.data(), blockAlias.data(), int(tailCandidates.size()), blockMarginal.data());

        tailBeta[cellIndex] = beta;

        for (size_t i = 0; i < tailCandidates.size(); i++)
        {
            tailUids.push_back(tailCandidates[i].uid);
            tailProb.push_back(blockProb[i]);
            tailAlias.push_back(blockAlias[i]);
            tailMarginal.push_back(blockMarginal[i]);
        }
    }

    offsets[kCells] = uint32_t(list.size());
    tailOffsets[kCells] = uint32_t(tailUids.size());

    stats.occupiedCells = uint32_t(occupiedCounts.size());
    stats.fastEntries = uint32_t(list.size());
    stats.tailEntries = uint32_t(tailUids.size());
    stats.cellsWithTail = 0;

    for (uint32_t cellIndex = 0; cellIndex < kCells; cellIndex++)
    {
        if (tailOffsets[cellIndex + 1] > tailOffsets[cellIndex])
        {
            stats.cellsWithTail++;
        }
    }

    if (!occupiedCounts.empty())
    {
        std::sort(occupiedCounts.begin(), occupiedCounts.end());
        stats.candidateMax = occupiedCounts.back();
        stats.candidateMedian = Percentile(occupiedCounts, 50);
        stats.candidateP95 = Percentile(occupiedCounts, 95);
    }

    valid = true;
    stats.enabled = 1;

    const auto end = std::chrono::steady_clock::now();
    stats.buildMs = float(std::chrono::duration<double, std::milli>(end - start).count());
}

void LightGrid::GetCellList(uint32_t cell, uint64_t *pUniqueIds, uint32_t maxCount, uint32_t *pCount) const
{
    if (pCount != nullptr)
    {
        *pCount = 0;
    }

    if (!valid || cell >= kCells || cell + 1 >= offsets.size())
    {
        return;
    }

    const uint32_t begin = offsets[cell];
    const uint32_t end = std::min<uint32_t>(offsets[cell + 1], uint32_t(list.size()));
    const uint32_t count = std::min<uint32_t>(end > begin ? end - begin : 0, maxCount);

    if (pUniqueIds != nullptr)
    {
        for (uint32_t i = 0; i < count; i++)
        {
            pUniqueIds[i] = list[begin + i];
        }
    }

    if (pCount != nullptr)
    {
        *pCount = count;
    }
}

void LightGrid::GetCellTail(uint32_t cell, uint64_t *pUniqueIds, float *pProb, float *pMarginal,
                            uint32_t *pAlias, float *pBeta, uint32_t maxCount, uint32_t *pCount) const
{
    if (pCount != nullptr)
    {
        *pCount = 0;
    }

    if (pBeta != nullptr)
    {
        *pBeta = 0.0f;
    }

    if (!valid || cell >= kCells || cell + 1 >= tailOffsets.size())
    {
        return;
    }

    const uint32_t begin = tailOffsets[cell];
    const uint32_t end = std::min<uint32_t>(tailOffsets[cell + 1], uint32_t(tailUids.size()));
    const uint32_t count = std::min<uint32_t>(end > begin ? end - begin : 0, maxCount);

    if (pBeta != nullptr && cell < tailBeta.size())
    {
        *pBeta = tailBeta[cell];
    }

    for (uint32_t i = 0; i < count; i++)
    {
        if (pUniqueIds != nullptr)
        {
            pUniqueIds[i] = tailUids[begin + i];
        }
        if (pProb != nullptr)
        {
            pProb[i] = tailProb[begin + i];
        }
        if (pMarginal != nullptr)
        {
            pMarginal[i] = tailMarginal[begin + i];
        }
        if (pAlias != nullptr)
        {
            pAlias[i] = tailAlias[begin + i];
        }
    }

    if (pCount != nullptr)
    {
        *pCount = count;
    }
}

}
