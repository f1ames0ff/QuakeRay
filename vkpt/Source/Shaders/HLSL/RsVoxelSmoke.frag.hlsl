#include "VoxelSmoke.hlsli"

[[vk::binding(256, 0)]] ConstantBuffer<VoxelSmokeParams> params;
[[vk::binding(0, 0)]] Texture3D<float> volume;
[[vk::binding(128, 0)]] SamplerState volumeSampler;
[[vk::binding(1, 0)]] Texture2D<float> sceneDepth;

float4 main( float4 position : SV_Position ) : SV_Target0
{
    const float2 ndc = position.xy * params.screenParams.zw * 2.0 - 1.0;

    const float4 near = mul( params.invViewProj, float4( ndc, 0.0, 1.0 ) );
    const float4 far  = mul( params.invViewProj, float4( ndc, 1.0, 1.0 ) );
    const float3 origin = params.cameraPos.xyz;
    const float3 dir    = normalize( far.xyz / far.w - near.xyz / near.w );

    float t0, t1;
    if ( !voxelSmokeBox( params.worldMin.xyz, params.worldMax.xyz, origin, dir, t0, t1 ) )
    {
        return 0.0;
    }

    const float sceneDist = sceneDepth.Load( int3( (int2)position.xy, 0 ) );

    t0 = max( t0, 0.0 );
    t1 = min( t1, max( sceneDist, 0.0 ) );

    if ( t1 <= t0 )
    {
        return 0.0;
    }

    const float steps      = max( params.marchParams.x, 1.0 );
    const float stepLength = ( t1 - t0 ) / steps;
    const float extinction = params.marchParams.y;

    float  transmittance = 1.0;
    float3 scattered = 0.0;

    for ( float i = 0.0; i < steps; i += 1.0 )
    {
        const float  t     = t0 + ( i + 0.5 ) * stepLength;
        const float3 volumePos = voxelSmokeWorldToVolume( params, origin + dir * t );
        const float  density = volume.SampleLevel( volumeSampler, volumePos, 0.0 );

        const float opacity = 1.0 - exp( -density * extinction * stepLength );

        scattered     += params.marchParams.z * opacity * transmittance;
        transmittance *= 1.0 - opacity;

        if ( transmittance < 0.01 )
        {
            break;
        }
    }

    return float4( scattered, 1.0 - transmittance );
}
