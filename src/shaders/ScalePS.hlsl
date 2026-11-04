Texture2D src : register(t0);
SamplerState smp : register(s0);

cbuffer Params : register(b0) {
    float2 footprint;  // one destination pixel in source UV units
    float lod;         // mip level whose texel size is ~1/4 of the footprint
    float overWhite;   // 1: premultiplied alpha, shown over white (an opened animation saved as MP4)
};

// Downscale: 4 bilinear taps spread over the destination pixel's footprint at a pre-filtered mip,
// which approximates an area (box) filter without aliasing.
float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    float2 d = footprint * 0.25;
    float4 c = src.SampleLevel(smp, uv + float2(-d.x, -d.y), lod)
             + src.SampleLevel(smp, uv + float2( d.x, -d.y), lod)
             + src.SampleLevel(smp, uv + float2(-d.x,  d.y), lod)
             + src.SampleLevel(smp, uv + float2( d.x,  d.y), lod);
    c *= 0.25;
    return float4(c.rgb + overWhite * (1.0 - c.a), 1.0);
}
