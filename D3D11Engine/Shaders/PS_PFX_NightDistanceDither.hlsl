// Final low-light quantization dither for distant night geometry.
#include <DepthReconstruction.h>

SamplerState SS_Linear : register(s0);
Texture2D TX_Scene : register(t0);
Texture2D TX_Depth : register(t1);

cbuffer NightDistanceDitherCB : register(b0)
{
    float4x4 ND_InvView;
    float4 ND_ProjParams;
    float3 ND_CameraPosition;
    float ND_NightWeight;
    float ND_FadeStart;
    float ND_FadeRange;
    float2 ND_Pad;
};

struct PS_INPUT
{
    float2 vTexcoord : TEXCOORD0;
    float3 vEyeRay : TEXCOORD1;
    float4 vPosition : SV_POSITION;
};

float NightDistanceDither(float2 pixelPosition)
{
    float n1 = frac(52.9829189f * frac(dot(pixelPosition, float2(0.06711056f, 0.00583715f))));
    float n2 = frac(52.9829189f * frac(dot(pixelPosition + 37.17f, float2(0.00583715f, 0.06711056f))));
    return n1 + n2 - 1.0f;
}

float4 PSMain(PS_INPUT Input) : SV_TARGET
{
    float4 color = TX_Scene.SampleLevel(SS_Linear, Input.vTexcoord, 0.0f);
    float depth = TX_Depth.SampleLevel(SS_Linear, Input.vTexcoord, 0.0f).r;

    // Reversed-Z clears the sky to zero. Keep the sky and nearby world untouched.
    if (ND_NightWeight > 0.001f && depth > 1e-8f)
    {
        float3 viewPosition = ReconstructVSPositionFromDepthReverseZInfinite(depth, Input.vTexcoord, ND_ProjParams.xy) * ND_ProjParams.z;
        float3 worldPosition = mul(float4(viewPosition, 1.0f), ND_InvView).xyz;
        float distanceToCamera = length(worldPosition - ND_CameraPosition);
        float distanceFade = smoothstep(ND_FadeStart, ND_FadeStart + max(1000.0f, ND_FadeRange), distanceToCamera);
        float dither = NightDistanceDither(Input.vPosition.xy) * distanceFade * ND_NightWeight * (1.0f / 255.0f);
        color.rgb = saturate(color.rgb + dither);
    }

    return color;
}