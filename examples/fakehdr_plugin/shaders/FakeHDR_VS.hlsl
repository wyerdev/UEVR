// Full-screen triangle vertex shader for FakeHDR
// Uses SV_VertexID to generate a full-screen triangle without a vertex buffer.

struct VSOutput
{
    float4 Position : SV_Position;
    float2 TexCoord : TEXCOORD0;
};

VSOutput main(uint vertexID : SV_VertexID)
{
    VSOutput output;
    
    // Generate a full-screen triangle from vertex ID (0, 1, 2)
    // This covers the entire screen without needing a vertex buffer.
    output.TexCoord = float2((vertexID << 1) & 2, vertexID & 2);
    output.Position = float4(output.TexCoord * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    
    return output;
}
