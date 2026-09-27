// Copyright (c) 2026 QuakeRay contributors

// HLSL counterpart of Q2ClusterLights.h, the three cluster accessors that master split out of
// Q2LightLists.h into a header of their own. Like the golden it includes nothing and declares no
// resource: the shader has to pull in the accessor layer first, which declares q2LightListOffsets,
// q2LightListLights and q2ClusterSkyVis under #ifdef DESC_SET_LIGHT_SOURCES. Its consumers are
// Q2LightLists.hlsli (which includes this header itself) and RsSmoke.vert.hlsl, both of which
// reach it after ShaderCommonHLSLFunc.hlsli, exactly as their GLSL halves reach the golden after
// ShaderCommonGLSLFunc.h.
//
// Spellings that had to change: none inside the three helpers -- uint, uint(...) and the shifts
// keep their names and their meaning on this side -- so the golden's bodies are transcribed line
// for line, the sky visibility comment included. The one spelling this base adds is the include
// guard (#ifndef Q2_CLUSTER_LIGHTS_HLSLI_ for the golden's Q2_CLUSTER_LIGHTS_H_), as everywhere.
//
// What did not change: the differences of the two list accessors, the early `true` of
// q2ClusterSeesSky for a cluster the host never classified, and the bit arithmetic of the
// visibility table, shift 5 and mask 31 included.

#ifndef Q2_CLUSTER_LIGHTS_HLSLI_
#define Q2_CLUSTER_LIGHTS_HLSLI_

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

#endif // Q2_CLUSTER_LIGHTS_HLSLI_
