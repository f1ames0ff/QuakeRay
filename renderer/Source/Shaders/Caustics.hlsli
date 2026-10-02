#ifndef CAUSTICS_HLSLI_
#define CAUSTICS_HLSLI_
#define CAUSTICS_FLUX_SCALE 256.0
#define CAUSTICS_DEBUG_OFF 0
#define CAUSTICS_DEBUG_PHOTONS 1
#define CAUSTICS_DEBUG_GRID 2
#define CAUSTICS_DEBUG_PREVIEW 3
#define CAUSTICS_DEBUG_MEDIA_REFR 4
#define CAUSTICS_DEBUG_MEDIA_OWN 5
#define CAUSTICS_FLAG_TRACE_VALID 1u
struct CausticsParams_BT
{
    float4 sunDirection;     // xyz: unit direction toward the sun (travel = -xyz); w: intensity, applied once by the direct pass
    float4 sunColor;         // rgb: raw directional light color
    float4 gridMinAndTexel;  // xy: receiver domain min (Quake units); z: receiver texel size; w: ray start Z
    uint4  gridSize;         // x: resolution; y: debug mode; z: flags bit0 = trace valid; w: frame id (launch jitter)
    float4 accumParams;      // x: history blend weight; y: non-zero resets the history; z/w unused
};
#endif
