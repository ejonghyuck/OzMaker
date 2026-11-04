Texture2D src : register(t0);

cbuffer Params : register(b0) {
    float2 footprint;  // ScalePS's, unused here
    float lod;
    float mode;        // 0: opaque, 1: premultiplied alpha shown over white, 2: as it is (a caption, blended over)
};

// Same size: the source pixel under each output pixel, unfiltered.
float4 main(float4 pos : SV_Position) : SV_Target {
    float4 c = src.Load(int3(pos.xy, 0));
    if (mode > 1.5) return c;
    return float4(c.rgb + mode * (1.0 - c.a), 1.0);
}
