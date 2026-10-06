#include "VoxelSmoke.hlsli"

[[vk::binding(256, 0)]] ConstantBuffer<VoxelSmokeParams> params;
[[vk::binding(0, 0)]] Texture3D<float> volumePrev;
[[vk::binding(128, 0)]] SamplerState volumeSampler;
[[vk::binding(384, 0)]] RWTexture3D<float> volumeNext;

[numthreads(8, 8, 8)]
void main( uint3 dtid : SV_DispatchThreadID )
{
    const uint3 res = (uint3)params.resolution.xyz;

    if ( any( dtid >= res ) )
    {
        return;
    }

    const float dt      = min( max( params.emitterParams.w, 0.0 ), 1.0 / 30.0 );
    const float3 volume = ( (float3)dtid + 0.5 ) / (float3)res;
    const float3 world  = voxelSmokeVolumeToWorld( params, volume );

    float density = 0.0;

    if ( params.resolution.w == 0.0 )
    {
        const float3 source = voxelSmokeWorldToVolume( params, world - float3( 0.0, 0.0, params.advectParams.x ) * dt );

        if ( all( source >= 0.0 ) && all( source <= 1.0 ) )
        {
            density = volumePrev.SampleLevel( volumeSampler, source, 0.0 );
        }
    }

    density *= exp( -max( params.emitterParams.z, 0.0 ) * dt );

    const float d    = distance( world, params.emitterCenter.xyz );
    const float fall = saturate( 1.0 - d / max( params.emitterParams.x, 1e-4 ) );

    density += fall * fall * params.emitterParams.y * dt;

    volumeNext[dtid] = saturate( density );
}
