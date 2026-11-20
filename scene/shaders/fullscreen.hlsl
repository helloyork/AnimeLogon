// A full-screen triangle for authors' layer shaders; TEXCOORD0 is in the register the contract says.
struct V { float2 uv : TEXCOORD0; float4 position : SV_Position; };
V main(uint id : SV_VertexID) {
    V v; v.uv = float2((id << 1) & 2, id & 2);
    v.position = float4(v.uv * float2(2,-2) + float2(-1,1),0,1); return v;
}
