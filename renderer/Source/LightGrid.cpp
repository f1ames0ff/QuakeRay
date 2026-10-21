#include "LightGrid.h"

#include <algorithm>
#include <chrono>
#include <cmath>

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
    sourceUids.clear();
    offsets.clear();
    ids.clear();
}

void LightGrid::Build(const LightGridSource *sources, uint32_t sourceCount, const float viewOrigin[3])
{
    const auto start = std::chrono::steady_clock::now();

    Reset();

    stats.cells = kCells;
    stats.cascades = kCascades;
    stats.sources = sourceCount;

    if (sources == nullptr || sourceCount == 0 || viewOrigin == nullptr)
    {
        return;
    }

    sourceUids.resize(sourceCount);

    for (uint32_t i = 0; i < sourceCount; i++)
    {
        sourceUids[i] = sources[i].uid;
    }

    float snapOrigin[3];

    for (int axis = 0; axis < 3; axis++)
    {
        const double value = std::isfinite(viewOrigin[axis]) ? double(viewOrigin[axis]) : 0.0;
        snapOrigin[axis] = float(std::floor(value / double(kSnap)) * double(kSnap));
    }

    std::vector<uint32_t> cellCounts(kCells, 0);

    auto visitSource = [&](const LightGridSource &source, const float origin[3], float cell, uint32_t cellBase,
                           auto &&visitor)
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
                    visitor(cellBase + local, float(power / std::max(dist2, scale)));
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
            stats.cellSize[cascade] = cell;
        }

        const uint32_t cellBase = cascade * kCellsPerCascade;

        for (uint32_t sourceIndex = 0; sourceIndex < sourceCount; sourceIndex++)
        {
            visitSource(sources[sourceIndex], origin, cell, cellBase,
                        [&](uint32_t cellIndex, float) { cellCounts[cellIndex]++; });
        }
    }

    std::vector<uint32_t> cellStart(kCells + 1, 0);

    for (uint32_t i = 0; i < kCells; i++)
    {
        cellStart[i + 1] = cellStart[i] + cellCounts[i];
    }

    std::vector<Candidate> candidates(cellStart[kCells]);
    std::vector<uint32_t> cursor(kCells);

    for (uint32_t i = 0; i < kCells; i++)
    {
        cursor[i] = cellStart[i];
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
            visitSource(sources[sourceIndex], origin, cell, cellBase,
                        [&](uint32_t cellIndex, float mass)
            {
                const uint32_t at = cursor[cellIndex]++;
                candidates[at] = { sourceIndex, mass };
            });
        }
    }

    offsets.assign(kCells + 1, 0);
    ids.clear();

    uint32_t dropped = 0;
    std::vector<uint32_t> occupiedCounts;
    occupiedCounts.reserve(kCells);

    for (uint32_t cellIndex = 0; cellIndex < kCells; cellIndex++)
    {
        const uint32_t candidateCount = cellCounts[cellIndex];

        offsets[cellIndex] = uint32_t(ids.size());

        if (candidateCount == 0)
        {
            continue;
        }

        Candidate *cellCandidates = candidates.data() + cellStart[cellIndex];

        occupiedCounts.push_back(candidateCount);
        std::sort(cellCandidates, cellCandidates + candidateCount, [](const Candidate &a, const Candidate &b)
        {
            if (a.mass != b.mass)
            {
                return a.mass > b.mass;
            }

            return a.source < b.source;
        });

        const uint32_t keep = std::min<uint32_t>(candidateCount, kMaxFastPerCell);
        dropped += candidateCount - keep;

        for (uint32_t i = 0; i < keep; i++)
        {
            ids.push_back(cellCandidates[i].source & QR_LIGHT_GRID_ID_INDEX_MASK);
        }
    }

    offsets[kCells] = uint32_t(ids.size());

    stats.occupiedCells = uint32_t(occupiedCounts.size());
    stats.fastEntries = uint32_t(ids.size());
    stats.tailEntries = 0;
    stats.tailDropped = dropped;
    stats.cellsWithTail = 0;

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
    const uint32_t end = std::min<uint32_t>(offsets[cell + 1], uint32_t(ids.size()));
    uint32_t       written = 0;

    for (uint32_t i = begin; i < end && written < maxCount; i++)
    {
        const uint32_t id = ids[i];
        const uint32_t type = (id >> QR_LIGHT_GRID_ID_TYPE_SHIFT) & QR_LIGHT_GRID_ID_TYPE_MASK;
        const uint32_t index = id & QR_LIGHT_GRID_ID_INDEX_MASK;

        if (type != QR_LIGHT_GRID_ID_TYPE_LIGHT || index >= sourceUids.size())
        {
            continue;
        }

        if (pUniqueIds != nullptr)
        {
            pUniqueIds[written] = sourceUids[index];
        }

        written++;
    }

    if (pCount != nullptr)
    {
        *pCount = written;
    }
}

void LightGrid::GetCellIds(uint32_t cell, uint32_t *pIds, uint32_t maxCount, uint32_t *pCount) const
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
    const uint32_t end = std::min<uint32_t>(offsets[cell + 1], uint32_t(ids.size()));
    const uint32_t count = std::min<uint32_t>(end > begin ? end - begin : 0, maxCount);

    if (pIds != nullptr)
    {
        for (uint32_t i = 0; i < count; i++)
        {
            pIds[i] = ids[begin + i];
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
}

}
