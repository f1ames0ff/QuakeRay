#pragma once

#include <cstdint>
#include <vector>

#include "qray/qray.h"

namespace qray
{

struct LightGridSource
{
    uint64_t uid;
    float    origin[3];
    float    reach;
    float    radius;
    float    power;
};

class LightGrid
{
public:
    static constexpr uint32_t kCascades = 8;
    static constexpr uint32_t kDim = 16;
    static constexpr uint32_t kCellsPerCascade = kDim * kDim * kDim;
    static constexpr uint32_t kCells = kCascades * kCellsPerCascade;
    static constexpr uint32_t kMaxFastPerCell = 64;
    static constexpr uint32_t kMaxTailPerCell = 0;
    static constexpr float    kBaseCell = 80.0f;
    static constexpr float    kSnap = kBaseCell * 8.0f;

    void Reset();

    void Build(const LightGridSource *sources, uint32_t sourceCount, const float viewOrigin[3]);

    bool IsValid() const { return valid; }
    const QrLightGridStats &GetStats() const { return stats; }

    void GetCellList(uint32_t cell, uint64_t *pUniqueIds, uint32_t maxCount, uint32_t *pCount) const;
    void GetCellIds(uint32_t cell, uint32_t *pIds, uint32_t maxCount, uint32_t *pCount) const;
    void GetCellTail(uint32_t cell, uint64_t *pUniqueIds, float *pProb, float *pMarginal,
                     uint32_t *pAlias, float *pBeta, uint32_t maxCount, uint32_t *pCount) const;

private:
    struct Candidate
    {
        uint32_t source;
        float    mass;
    };

    bool valid = false;
    QrLightGridStats stats{};
    std::vector<uint64_t> sourceUids;
    std::vector<uint32_t> offsets;
    std::vector<uint32_t> ids;
};

}
