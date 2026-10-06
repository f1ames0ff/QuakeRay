#define DESC_SET_GLOBAL_UNIFORM 1
#include "ShaderCommonHLSLFunc.hlsli"

#include "VoxelSmoke.hlsli"

[[vk::binding(256, 0)]] ConstantBuffer<VoxelSmokeParams> params;
[[vk::binding(0, 0)]] Texture3D<float> volume;
[[vk::binding(128, 0)]] SamplerState volumeSampler;
[[vk::binding(1, 0)]] Texture2D<float> sceneDepth;

void main( float4 position : SV_Position,
           out float4 outColor : SV_Target0,
           out float3 outEmission : SV_Target1 )
{
    outEmission = 0.0;

    const float2 uv = position.xy * float2( 1.0 / globalUniform.renderWidth, 1.0 / globalUniform.renderHeight );
    const float4 ndc = float4( uv * 2.0 - 1.0, 1.0, 1.0 );

    const float4 farPoint = mul( mul( globalUniform.invView, globalUniform.invProjection ), ndc );
    const float3 origin = globalUniform.cameraPosition.xyz;
    const float3 dir = normalize( farPoint.xyz / farPoint.w - origin );

    float t0, t1;
    if ( !voxelSmokeBox( max( params.boundsMin.xyz, params.worldMin.xyz ),
                         min( params.boundsMax.xyz, params.worldMax.xyz ), origin, dir, t0, t1 ) )
    {
        outColor = 0.0;
        return;
    }

    const float ndcZ = sceneDepth.Load( int3( (int2)position.xy, 0 ) ).r;
    const float4 hitPoint = mul( mul( globalUniform.invView, globalUniform.invProjection ),
                                 float4( uv * 2.0 - 1.0, ndcZ, 1.0 ) );
    const float sceneDist = length( hitPoint.xyz / hitPoint.w - origin );

    t0 = max( t0, 0.0 );
    t1 = min( t1, max( sceneDist, 0.0 ) );

    if ( t1 <= t0 )
    {
        outColor = 0.0;
        return;
    }

    const float budget     = max( params.marchParams.x, 1.0 );
    const float stepLength = max( voxelSmokeVoxelSize( params ), ( t1 - t0 ) / budget );
    const float steps      = ceil( ( t1 - t0 ) / stepLength );
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

    outColor = float4( scattered, 1.0 - transmittance );
}
