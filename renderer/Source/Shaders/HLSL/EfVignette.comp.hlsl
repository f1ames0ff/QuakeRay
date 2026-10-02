struct EffectVignette_PushConst
{
    float intensity;
    float start;
    float end;
    float roundness;
};

#define EFFECT_PUSH_CONST_T EffectVignette_PushConst
#include "EfSimple.hlsli"

[numthreads(COMPUTE_EFFECT_GROUP_SIZE_X, COMPUTE_EFFECT_GROUP_SIZE_Y, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    const int2 pix = int2(dispatchThreadID.xy);
    if (!effect_isPixValid(pix))
    {
        return;
    }

    const int2 size = effect_getFramebufSize();
    const float aspect = float(size.x) / max(float(size.y), 1.0);
    const float2 shape = lerp(float2(1.0, 1.0), float2(aspect, 1.0), push.custom.roundness);
    const float2 centered = (effect_getFramebufUV(pix) - 0.5) * 2.0 * shape;
    const float distance = length(centered) / length(shape);
    const float fade = smoothstep(push.custom.start, push.custom.end, distance);
    const float3 color = effect_loadFromSource(pix) * (1.0 - push.custom.intensity * fade);

    effect_storeToTarget(color, pix);
}
