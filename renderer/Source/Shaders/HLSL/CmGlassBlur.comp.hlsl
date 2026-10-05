#define DESC_SET_FRAMEBUFFERS 0
#define DESC_SET_GLOBAL_UNIFORM 1
#include "ShaderCommonHLSLFunc.hlsli"
#include "GlassBlur.hlsli"
#include "GlassDenoise.hlsli"

struct GlassBlurDirection
{
    uint axis;
};

[[vk::push_constant]] ConstantBuffer<GlassBlurDirection> glassBlurDirection;

[numthreads(COMPUTE_COMPOSE_GROUP_SIZE_X, COMPUTE_COMPOSE_GROUP_SIZE_Y, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    const int2 pix = int2(dispatchThreadID.xy);
    if (pix.x >= int(globalUniform.renderWidth) || pix.y >= int(globalUniform.renderHeight))
    {
        return;
    }
    const float4 glass = framebufQ2GlassFilter_Sampled.Load(int3(getCheckerboardPix(pix), 0));
    const uint axis = glassBlurDirection.axis;
    if (globalUniform.glassBlur == 0u)
    {
        if (!isRtGlass(glass))
        {
            return;
        }
        if (axis == 0u)
        {
            const float4 color = accumulateGlass(pix, glass);
            framebufQ2GlassHistory[pix] = color;
            framebufPreFinal[pix] = float4(color.rgb, 0.0);
        }
        else
        {
            framebufBloomInput[pix] = float4(spatialGlass(pix, glass), 0.0);
        }
        return;
    }
    if (abs(glass.a) < 1.0)
    {
        return;
    }

    const float3 color = blurGlassAxis(pix, saturate(abs(glass.a) - 1.0), axis);
    if (axis == 0u)
    {
        framebufPreFinal[pix] = float4(color, 0.0);
    }
    else
    {
        const float4 reflection = framebufQ2GlassReflection_Sampled.Load(int3(pix, 0));
        framebufBloomInput[pix] = float4(color * glass.rgb * (1.0 - saturate(reflection.a)) + reflection.rgb, 0.0);
    }
}
