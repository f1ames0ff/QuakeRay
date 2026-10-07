#ifndef VOXEL_SMOKE_HLSLI_
#define VOXEL_SMOKE_HLSLI_

struct VoxelSmokeParams
{
    float4 worldMin;
    float4 worldMax;
    float4 boundsMin;
    float4 boundsMax;
    float4 emitterCenter;
    float4 emitterParams;
    float4 marchParams;
    float4 resolution;
    float4 advectParams;
    float4 prevWorldMin;
    float4 prevWorldMax;
    float4 emitterStartRadius[8];
    float4 emitterEndDensity[8];
    float4 emitterCounts;
};

float3 voxelSmokeWorldToVolume( const VoxelSmokeParams p, const float3 world )
{
    return ( world - p.worldMin.xyz ) / max( p.worldMax.xyz - p.worldMin.xyz, (float3)1e-4 );
}

float3 voxelSmokeVolumeToWorld( const VoxelSmokeParams p, const float3 volume )
{
    return p.worldMin.xyz + volume * ( p.worldMax.xyz - p.worldMin.xyz );
}

float voxelSmokeVoxelSize( const VoxelSmokeParams p )
{
    const float3 size = max( p.worldMax.xyz - p.worldMin.xyz, (float3)1e-4 );
    const float3 res  = max( p.resolution.xyz, (float3)1.0 );

    return min( size.x / res.x, min( size.y / res.y, size.z / res.z ) );
}

bool voxelSmokeBox( const float3 boxMin, const float3 boxMax,
                    const float3 rayOrigin, const float3 rayDir, out float t0, out float t1 )
{
    const float3 invDir = 1.0 / rayDir;
    const float3 a = ( boxMin - rayOrigin ) * invDir;
    const float3 b = ( boxMax - rayOrigin ) * invDir;

    const float3 tmin = min( a, b );
    const float3 tmax = max( a, b );

    t0 = max( max( tmin.x, tmin.y ), tmin.z );
    t1 = min( min( tmax.x, tmax.y ), tmax.z );

    return t1 > max( t0, 0.0 );
}

#endif
