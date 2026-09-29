// Copyright (c) 2025-2026 f1ames0ff <f1am3sdev.github@protonmail.com>

#ifndef Q2_CLUSTER_LIGHTS_H_
#define Q2_CLUSTER_LIGHTS_H_

uint q2GetClusterLightCount(const uint cluster)
{
    return q2LightListOffsets[cluster + 1] - q2LightListOffsets[cluster];
}

uint q2GetClusterLight(const uint cluster, const uint slot)
{
    return q2LightListLights[q2LightListOffsets[cluster] + slot];
}

/* Per-cluster sky visibility of the host (Q2RTX's sky_visibility): bit c of the table is
   1 when a sun ray from cluster c can still reach the sky, which is the union of the PVS
   of every cluster that holds a sky surface. A cluster the host never classified - the
   solid cluster 0, or one past the table - keeps its sun ray, as Q2RTX keeps it for an
   invalid cluster. */
bool q2ClusterSeesSky(const uint cluster)
{
    if (cluster >= uint(Q2_MAX_CLUSTERS))
    {
        return true;
    }

    return (q2ClusterSkyVis[cluster >> 5] & (1u << (cluster & 31u))) != 0u;
}

#endif // Q2_CLUSTER_LIGHTS_H_
