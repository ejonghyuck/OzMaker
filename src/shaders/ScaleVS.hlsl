struct VSOut {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

// Fullscreen triangle from SV_VertexID; no vertex buffer needed.
VSOut main(uint id : SV_VertexID) {
    VSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.uv = uv;
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
