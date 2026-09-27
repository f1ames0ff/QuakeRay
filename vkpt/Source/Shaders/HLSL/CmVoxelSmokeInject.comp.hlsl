#include "VoxelSmoke.hlsli"

[[vk::binding(256, 0)]] ConstantBuffer<VoxelSmokeParams> params;
[[vk::binding(384, 0)]] RWTexture3D<float> volume;

[numthreads(8, 8, 8)]
void main( uint3 dtid : SV_DispatchThreadID )
{
    const uint3 res = (uint3)params.resolution.xyz;

    if ( any( dtid >= res ) )
    {
        return;
    }

    float density = volume[dtid] * saturate( 1.0 - params.emitterParams.z * params.emitterParams.w );

    const float3 world = voxelSmokeVolumeToWorld( params, ( (float3)dtid + 0.5 ) / (float3)res );
    const float  d     = distance( world, params.emitterCenter.xyz );
    const float  fall  = saturate( 1.0 - d / max( params.emitterParams.x, 1e-4 ) );

    density += fall * fall * params.emitterParams.y * params.emitterParams.w;

    volume[dtid] = saturate( density );
}
