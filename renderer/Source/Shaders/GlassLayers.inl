#ifndef GLASS_LAYERS_INL
#define GLASS_LAYERS_INL

bool glassLayerMatches(ivec2 pix, bool reflection)
{
    const float field = texelFetch(framebufQ2GlassFilter_Sampled, getCheckerboardPix(pix), 0).a;
    return reflection ? field <= -1.0 : field >= 1.0;
}

vec4 reconstructGlassLayer(ivec2 pix, bool reflection)
{
    if (glassLayerMatches(pix, reflection))
    {
        return sampleGlassLayer(pix, reflection);
    }
    const ivec2 offsets[4] = ivec2[4](ivec2(-1, 0), ivec2(1, 0), ivec2(0, -1), ivec2(0, 1));
    const ivec2 extent = ivec2(globalUniform.renderWidth, globalUniform.renderHeight);
    vec4 sum = vec4(0.0);
    float weight = 0.0;
    for (int i = 0; i < 4; i++)
    {
        const ivec2 tap = pix + offsets[i];
        if (any(lessThan(tap, ivec2(0))) || any(greaterThanEqual(tap, extent)) || !glassLayerMatches(tap, reflection))
        {
            continue;
        }
        sum += sampleGlassLayer(tap, reflection);
        weight += 1.0;
    }
    return sum / max(weight, 1.0);
}

#endif
