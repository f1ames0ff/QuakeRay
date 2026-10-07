#define DESC_SET_GLOBAL_UNIFORM 1
#include "ShaderCommonHLSLFunc.hlsli"

#include "Q2Asvgf.hlsli"
#include "Smoke.hlsli"
#include "VoxelSmoke.hlsli"

[[vk::binding(256, 0)]] ConstantBuffer<VoxelSmokeParams> params;
[[vk::binding(0, 0)]] Texture3D<float> volume;
[[vk::binding(128, 0)]] SamplerState volumeSampler;
[[vk::binding(1, 0)]] Texture2D<float> sceneDepth;
[[vk::binding(2, 0)]] Texture2D<float4> smokeLfSh;
[[vk::binding(3, 0)]] Texture2D<float4> smokeLfCocg;

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

    const int2 cbPix = getCheckerboardPix( (int2)position.xy );
    const int2 lfPix = int2( clamp( cbPix / Q2_GRAD_DWN, (int2)0,
                                    int2( globalUniform.renderWidth, globalUniform.renderHeight ) / Q2_GRAD_DWN - (int2)1 ) );

    Q2SH lf;
    lf.shY = smokeLfSh.Load( int3( lfPix, 0 ) );
    lf.CoCg = smokeLfCocg.Load( int3( lfPix, 0 ) ).xy;
    lf.shY /= Q2_STORAGE_SCALE_LF;
    lf.CoCg /= Q2_STORAGE_SCALE_LF;

    const float3 ambient = q2SHToIrradiance( lf, (float3)0.0 ) * SMOKE_AMBIENT_GAIN;
    const float3 inScatter = ( params.marchParams.w != 0.0 )
        ? ( ambient + SMOKE_MIN_LIGHT )
        : (float3)params.marchParams.z;

    float  transmittance = 1.0;
    float3 scattered = 0.0;

    for ( float i = 0.0; i < steps; i += 1.0 )
    {
        const float  t     = t0 + ( i + 0.5 ) * stepLength;
        const float3 volumePos = voxelSmokeWorldToVolume( params, origin + dir * t );
        const float  density = volume.SampleLevel( volumeSampler, volumePos, 0.0 );

        const float opacity = 1.0 - exp( -density * extinction * stepLength );

        scattered     += inScatter * opacity * transmittance;
        transmittance *= 1.0 - opacity;

        if ( transmittance < 0.01 )
        {
            break;
        }
    }

    outColor = float4( scattered, 1.0 - transmittance );
}
