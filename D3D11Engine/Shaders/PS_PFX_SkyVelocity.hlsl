// Writes camera-rotation motion vectors only for true reversed-Z sky pixels.
// Geometry velocity remains untouched because non-sky pixels are discarded.
cbuffer SkyVelocityConstants : register(b0)
{
    float4x4 InvViewProj;
    float4x4 PrevViewProj;
    float2 JitterOffset;
    float2 Padding;
};

Texture2D TX_Depth : register(t0);

struct PS_INPUT
{
    float2 vTexcoord : TEXCOORD0;
    float3 vEyeRay   : TEXCOORD1;
    float4 vPosition : SV_POSITION;
};

float2 PSMain(PS_INPUT input) : SV_TARGET
{
    const float depth = TX_Depth.Load(int3(int2(input.vPosition.xy), 0)).r;
    if (depth > 1e-7f)
        discard;

    // FSR receives jitter separately; motion vectors therefore use unjittered UVs.
    const float2 currentUV = input.vTexcoord - JitterOffset;
    const float4 currentClip = float4(
        currentUV.x * 2.0f - 1.0f,
        (1.0f - currentUV.y) * 2.0f - 1.0f,
        0.0f,
        1.0f);

    // At reversed-Z infinite depth this homogeneous position represents a
    // direction (w=0), so camera translation drops out and only rotation remains.
    const float4 worldDirection = mul(currentClip, InvViewProj);
    const float4 previousClip = mul(worldDirection, PrevViewProj);
    if (abs(previousClip.w) < 1e-7f)
        return float2(0.0f, 0.0f);

    const float2 previousNDC = previousClip.xy / previousClip.w;
    const float2 previousUV = float2(
        previousNDC.x * 0.5f + 0.5f,
        1.0f - (previousNDC.y * 0.5f + 0.5f));

    // Same convention as the geometry MRT: previous UV minus current UV.
    return previousUV - currentUV;
}