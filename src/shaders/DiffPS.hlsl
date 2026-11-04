Texture2D a : register(t0);
Texture2D b : register(t1);

// Variable frame rate (#55): whether two pictures differ anywhere. One output pixel per 16x16 block; a block where
// every pixel is the same is discarded, so an occlusion query says whether any differed.
float4 main(float4 pos : SV_Position) : SV_Target {
    int2 base = int2(pos.xy) * 16;
    [loop] for (int y = 0; y < 16; ++y) {
        [loop] for (int x = 0; x < 16; ++x) {
            int3 p = int3(base + int2(x, y), 0);
            if (any(a.Load(p).rgb != b.Load(p).rgb)) return float4(1, 1, 1, 1);
        }
    }
    discard;
    return float4(0, 0, 0, 0);
}
