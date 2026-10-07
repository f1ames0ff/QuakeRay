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
        const float3 prevMin  = params.prevWorldMin.xyz;
        const float3 prevSize = max( params.prevWorldMax.xyz - prevMin, (float3)1e-4 );
        const float3 source   = ( world - float3( 0.0, 0.0, params.advectParams.x ) * dt - prevMin ) / prevSize;

        if ( all( source >= 0.0 ) && all( source <= 1.0 ) )
        {
            density = volumePrev.SampleLevel( volumeSampler, source, 0.0 );
        }
    }

    density *= exp( -max( params.emitterParams.z, 0.0 ) * dt );

    const uint emitterCount = (uint)params.emitterCounts.x;

    if ( emitterCount == 0u )
    {
        const float d    = distance( world, params.emitterCenter.xyz );
        const float fall = saturate( 1.0 - d / max( params.emitterParams.x, 1e-4 ) );

        density += fall * fall * params.emitterParams.y * dt;
    }
    else
    {
        for ( uint e = 0u; e < emitterCount; e++ )
        {
            const float3 start  = params.emitterStartRadius[e].xyz;
            const float3 end    = params.emitterEndDensity[e].xyz;
            const float  radius = params.emitterStartRadius[e].w;
            const float  rate   = params.emitterEndDensity[e].w;

            const float3 segment = end - start;
            const float  t       = saturate( dot( world - start, segment ) / max( dot( segment, segment ), 1e-4 ) );
            const float  d       = distance( world, start + segment * t );
            const float  fall    = saturate( 1.0 - d / max( radius, 1e-4 ) );

            density += fall * fall * rate * dt;
        }
    }

    volumeNext[dtid] = saturate( density );
}
