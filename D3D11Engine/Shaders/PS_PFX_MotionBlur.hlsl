//--------------------------------------------------------------------------------------
// Velocity-based scene motion blur
//--------------------------------------------------------------------------------------

cbuffer MotionBlurConstants : register(b0)
{
    float2 MB_InvResolution;
    float MB_Strength;
    float MB_MaxPixels;
    float MB_DepthTolerance;
    float MB_MinVelocityPixels;
    float2 MB_Padding;
};

SamplerState SS_Linear : register(s0);
SamplerState SS_Point : register(s1);
Texture2D TX_Scene : register(t0);
Texture2D TX_Velocity : register(t1);
Texture2D TX_Depth : register(t2);

struct PS_INPUT
{
    float2 vTexcoord : TEXCOORD0;
    float3 vEyeRay : TEXCOORD1;
    float4 vPosition : SV_POSITION;
};

bool IsSkyDepth(float depth)
{
    return depth <= 1e-7f;
}

float DepthWeight(float centerDepth, float sampleDepth)
{
    if (IsSkyDepth(centerDepth) || IsSkyDepth(sampleDepth)) {
        return 0.0f;
    }

    // Reversed-Z: near pixels have high depth. Keep near silhouettes strict so
    // character edges do not pull bright background samples into the blur.
    float farFactor = saturate(1.0f - centerDepth);
    float tolerance = MB_DepthTolerance + farFactor * 0.004f;
    float depthDelta = abs(sampleDepth - centerDepth);
    float weight = saturate(1.0f - depthDelta / max(tolerance, 0.0001f));
    return weight * weight;
}

float4 PSMain(PS_INPUT Input) : SV_TARGET
{
    float2 uv = Input.vTexcoord;
    float4 centerColor = TX_Scene.SampleLevel(SS_Linear, uv, 0);
    float centerDepth = TX_Depth.SampleLevel(SS_Point, uv, 0).r;
    if (IsSkyDepth(centerDepth)) {
        return centerColor;
    }

    float2 resolution = 1.0f / MB_InvResolution;
    float2 velocity = TX_Velocity.SampleLevel(SS_Point, uv, 0).rg * MB_Strength;

    float aspect = resolution.x / max(resolution.y, 1.0f);
    float2 centered = uv - float2(0.5f, 0.5f);
    centered.x *= aspect;
    float peripheralWeight = smoothstep(0.18f, 0.75f, length(centered));
    velocity *= lerp(0.70f, 1.15f, peripheralWeight);

    float velocityPixels = length(velocity * resolution);
    if (velocityPixels < MB_MinVelocityPixels) {
        return centerColor;
    }

    if (velocityPixels > MB_MaxPixels) {
        velocity *= MB_MaxPixels / velocityPixels;
        velocityPixels = MB_MaxPixels;
    }

    float4 colorSum = centerColor;
    float weightSum = 1.0f;

    [unroll]
    for (int i = 0; i < 6; ++i) {
        float shutter = ((float)i + 0.5f) / 6.0f - 0.5f;
        float2 sampleUV = uv + velocity * shutter;
        if (sampleUV.x < 0.0f || sampleUV.x > 1.0f || sampleUV.y < 0.0f || sampleUV.y > 1.0f) {
            continue;
        }

        float sampleDepth = TX_Depth.SampleLevel(SS_Point, sampleUV, 0).r;
        float weight = DepthWeight(centerDepth, sampleDepth);
        if (weight <= 0.001f) {
            continue;
        }

        colorSum += TX_Scene.SampleLevel(SS_Linear, sampleUV, 0) * weight;
        weightSum += weight;
    }

    return colorSum / max(weightSum, 0.0001f);
}