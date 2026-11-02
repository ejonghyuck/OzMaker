Texture2D src : register(t0);
SamplerState smp : register(s0);

cbuffer Params : register(b0) {
    float2 footprint;  // one destination pixel in source UV units
    float lod;         // mip level whose texel size is ~1/4 of the footprint
    float pad;
};

// Downscale: 4 bilinear taps spread over the destination pixel's footprint at a pre-filtered mip,
// which approximates an area (box) filter without aliasing.
float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    float2 d = footprint * 0.25;
    float3 c = src.SampleLevel(smp, uv + float2(-d.x, -d.y), lod).rgb
             + src.SampleLevel(smp, uv + float2( d.x, -d.y), lod).rgb
             + src.SampleLevel(smp, uv + float2(-d.x,  d.y), lod).rgb
             + src.SampleLevel(smp, uv + float2( d.x,  d.y), lod).rgb;
    return float4(c * 0.25, 1.0);
}
