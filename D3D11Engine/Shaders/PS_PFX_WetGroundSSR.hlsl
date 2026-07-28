//--------------------------------------------------------------------------------------
// Screen-space reflections for rain-wet ground surfaces
//--------------------------------------------------------------------------------------

#include "DepthReconstruction.h"
#include "DS_Defines.h"
#include "AtmosphericScattering.h"

cbuffer WetGroundSSRConstantBuffer : register(b0)
{
    float4 WG_ProjParams;
    matrix WG_InvView;
    matrix WG_ViewProj;
    matrix WG_RainViewProj;

    float3 WG_CameraPosition;
    float WG_Wetness;

    float2 WG_InvResolution;
    float WG_Strength;
    float WG_Time;

    float WG_RainFXWeight;
    float3 WG_Pad;
};

SamplerState SS_Linear : register(s0);
SamplerComparisonState SS_Comp : register(s1);
Texture2D TX_Scene : register(t0);
Texture2D TX_Depth : register(t1);
Texture2D TX_Normals : register(t2);
Texture2D TX_RainShadow : register(t3);
Texture2D TX_Distortion : register(t4);
Texture2D TX_WaterMask : register(t5);
Texture2D TX_Material : register(t6);
Texture2D TX_LowClouds : register(t7);

struct PS_INPUT
{
    float2 vTexcoord : TEXCOORD0;
    float3 vEyeRay : TEXCOORD1;
    float4 vPosition : SV_POSITION;
};

float3 ReconstructVS(float depth, float2 uv)
{
    return ReconstructVSPositionFromDepthReverseZInfinite(depth, uv, WG_ProjParams.xy) * WG_ProjParams.z;
}

float GetRainExposure(float3 wsPosition)
{
    float4 shadowPosition = mul(float4(wsPosition, 1.0f), WG_RainViewProj);
    float2 shadowUV = shadowPosition.xy * float2(0.5f, -0.5f) + 0.5f;
    if (any(shadowUV < 0.0f) || any(shadowUV > 1.0f))
        return 0.0f;

    return TX_RainShadow.SampleCmpLevelZero(SS_Comp, shadowUV, shadowPosition.z - 0.0001f);
}

float3 SampleRoughReflection(float2 uv, float2 distortion)
{
    float2 spread = WG_InvResolution * 2.0f + abs(distortion) * 0.0040f;
    float3 color = TX_Scene.SampleLevel(SS_Linear, uv, 0).rgb * 0.40f;

    color += TX_Scene.SampleLevel(SS_Linear, uv + float2(spread.x, 0), 0).rgb * 0.15f;
    color += TX_Scene.SampleLevel(SS_Linear, uv - float2(spread.x, 0), 0).rgb * 0.15f;
    color += TX_Scene.SampleLevel(SS_Linear, uv + float2(0, spread.y), 0).rgb * 0.15f;
    color += TX_Scene.SampleLevel(SS_Linear, uv - float2(0, spread.y), 0).rgb * 0.15f;

    return color;
}

float SampleWetSSRBlockMask(float2 pixelPosition)
{
    uint width, height;
    TX_WaterMask.GetDimensions(width, height);
    int2 maxPixel = int2((int)width - 1, (int)height - 1);
    int2 center = clamp(int2(pixelPosition), int2(0, 0), maxPixel);
    float mask = 0.0f;
    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            int2 samplePixel = clamp(center + int2(x, y), int2(0, 0), maxPixel);
            mask = max(mask, TX_WaterMask.Load(int3(samplePixel, 0)).r);
        }
    }
    return mask;
}

float DecodeWetSSRBlock(float encodedMask)
{
    return saturate(encodedMask / 0.25f);
}

float PuddleHash21(float2 p)
{
    p = frac(p * float2(123.34f, 456.21f));
    p += dot(p, p + 45.32f);
    return frac(p.x * p.y);
}

float PuddleValueNoise(float2 p)
{
    float2 i = floor(p);
    float2 f = frac(p);
    float2 u = f * f * (3.0f - 2.0f * f);

    float a = PuddleHash21(i);
    float b = PuddleHash21(i + float2(1.0f, 0.0f));
    float c = PuddleHash21(i + float2(0.0f, 1.0f));
    float d = PuddleHash21(i + float2(1.0f, 1.0f));

    return lerp(lerp(a, b, u.x), lerp(c, d, u.x), u.y);
}

float CalculatePuddleMask(float3 wsPosition, float wsNormalY, float materialStrength, float wetness)
{
    float2 worldXZ = wsPosition.xz;

    float macroNoise = PuddleValueNoise(worldXZ * 0.00042f);
    float mediumNoise = PuddleValueNoise(worldXZ * 0.00115f + float2(17.31f, 43.77f));
    float edgeNoise = PuddleValueNoise(worldXZ * 0.00310f + float2(61.19f, 8.53f));

    float puddleField = macroNoise * 0.58f + mediumNoise * 0.30f + edgeNoise * 0.12f;

    float materialProbability = sqrt(saturate(materialStrength));
    float threshold = lerp(0.78f, 0.51f, materialProbability);
    threshold -= saturate(wetness) * 0.10f;

    float puddleShape = smoothstep(threshold, threshold + 0.11f, puddleField);
    float puddleFlatness = smoothstep(0.82f, 0.975f, wsNormalY);

    return saturate(puddleShape * puddleFlatness);
}

float3 ComposeWetGroundSky(float2 skyUV)
{
    float3 skyBase = TX_Scene.SampleLevel(SS_Linear, skyUV, 0).rgb;
    float4 clouds = ResolveLowCloudLayer(
        TX_LowClouds.SampleLevel(SS_Linear, skyUV, 0),
        skyBase);

    return max(skyBase * (1.0f - clouds.a) + clouds.rgb, 0.0f);
}

void AccumulateRainImpactLayer(
    float2 worldXZ, float time, float cellSize, float cycleRate, float density, float layerSeed,
    inout float2 rippleVector, inout float ringMask, inout float impactMask)
{
    float2 baseCell = floor(worldXZ / cellSize);

    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            float2 cell = baseCell + float2((float)x, (float)y);
            float seed = PuddleHash21(cell + float2(layerSeed, layerSeed * 1.731f));
            float eventSeed = PuddleHash21(cell + float2(layerSeed * 2.173f + 13.17f, layerSeed * 0.917f + 47.53f));
            float eventMask = step(1.0f - density, eventSeed);

            float phase = frac(time * cycleRate + seed);
            float2 pointJitter = float2(
                PuddleHash21(cell + float2(layerSeed + 5.31f, layerSeed + 19.73f)),
                PuddleHash21(cell + float2(layerSeed + 31.91f, layerSeed + 7.57f)));
            float2 impactPosition = (cell + 0.15f + pointJitter * 0.70f) * cellSize;
            float2 delta = worldXZ - impactPosition;
            float distanceToImpact = length(delta);
            float2 radialDirection = delta / max(distanceToImpact, 0.001f);

            float radiusVariation = lerp(
                0.82f, 1.12f,
                PuddleHash21(cell + float2(layerSeed + 71.11f, layerSeed + 3.29f)));

            float maximumRadius = cellSize * 0.42f * radiusVariation;

            float primaryRadius = phase * maximumRadius;
            float primaryWidth = lerp(1.40f, 3.40f, phase);
            float primaryDelta = (distanceToImpact - primaryRadius) / primaryWidth;
            float primaryRing = exp2(-primaryDelta * primaryDelta * 2.80f);
            primaryRing *= pow(saturate(1.0f - phase), 1.40f);

            float secondaryPhase = saturate((phase - 0.16f) / 0.84f);
            float secondaryRadius = secondaryPhase * maximumRadius * 0.68f;
            float secondaryWidth = lerp(1.25f, 3.10f, secondaryPhase);
            float secondaryDelta = (distanceToImpact - secondaryRadius) / secondaryWidth;
            float secondaryRing = exp2(-secondaryDelta * secondaryDelta * 2.60f);
            secondaryRing *= smoothstep(0.14f, 0.22f, phase);
            secondaryRing *= pow(saturate(1.0f - secondaryPhase), 1.65f);

            float impactRadius = lerp(3.20f, 1.60f, saturate(phase * 5.0f));
            float impactDelta = distanceToImpact / impactRadius;
            float centralImpact = exp2(-impactDelta * impactDelta * 2.40f);
            centralImpact *= exp2(-phase * 18.0f);

            float activePrimaryRing = primaryRing * eventMask;
            float activeSecondaryRing = secondaryRing * eventMask;
            float activeCentralImpact = centralImpact * eventMask;

            float signedRipple = activePrimaryRing - activeSecondaryRing * 0.42f;
            rippleVector += radialDirection * signedRipple;

            ringMask = max(ringMask, activePrimaryRing + activeSecondaryRing * 0.38f);
            impactMask = max(impactMask, activeCentralImpact);
        }
    }
}

float4 PSMain(PS_INPUT input) : SV_TARGET
{
    float2 uv = input.vTexcoord;
    float3 sceneColor = TX_Scene.SampleLevel(SS_Linear, uv, 0).rgb;
    float wetSSRVisibility = 1.0f - DecodeWetSSRBlock(SampleWetSSRBlockMask(input.vPosition.xy));
    if (wetSSRVisibility <= 0.001f)
        return float4(sceneColor, 1.0f);


    float depth = TX_Depth.SampleLevel(SS_Linear, uv, 0).r;
    if (depth <= 1e-7f || WG_Wetness <= 0.001f || WG_Strength <= 0.001f)
        return float4(sceneColor, 1.0f);

    float3 vsPosition = ReconstructVS(depth, uv);
    float3 wsPosition = mul(float4(vsPosition, 1.0f), WG_InvView).xyz;
    float3 vsNormal = DecodeNormalGBuffer(TX_Normals.SampleLevel(SS_Linear, uv, 0).xy);
    float3 wsNormal = normalize(mul(float4(vsNormal, 0.0f), WG_InvView).xyz);

    float upwardMask = smoothstep(0.45f, 0.88f, wsNormal.y);
    if (upwardMask <= 0.01f)
        return float4(sceneColor, 1.0f);

    float materialWetGroundSSRStrength = saturate(TX_Material.SampleLevel(SS_Linear, uv, 0).z);
    if (materialWetGroundSSRStrength <= 0.001f)
        return float4(sceneColor, 1.0f);


    float rainExposure = GetRainExposure(wsPosition);
    float wetness = saturate(WG_Wetness);

    float commonWetMask = upwardMask * rainExposure * wetness * wetSSRVisibility;
    float puddleMask = CalculatePuddleMask(wsPosition, wsNormal.y, materialWetGroundSSRStrength, wetness);

    float baseWetStrength = materialWetGroundSSRStrength * lerp(0.34f, 0.58f, wetness);
    float localWetStrength = saturate(max(baseWetStrength, puddleMask));

    float wetMask = commonWetMask * localWetStrength;
    if (wetMask <= 0.01f)
        return float4(sceneColor, 1.0f);

    float2 wetUV = wsPosition.xz / 1100.0f;
    float animationTime = fmod(max(WG_Time, 0.0f), 256.0f);
    float2 slowFlowA = float2(0.0075f, -0.0050f) * animationTime;
    float2 slowFlowB = float2(-0.0040f, 0.0065f) * animationTime;
    float2 distortionAUV = frac(wetUV + slowFlowA);
    float2 distortionBUV = frac(wetUV * 0.63f + float2(0.137f, 0.421f) + slowFlowB);
    float2 distortionA = TX_Distortion.SampleLevel(SS_Linear, distortionAUV, 0).xy * 2.0f - 1.0f;
    float2 distortionB = TX_Distortion.SampleLevel(SS_Linear, distortionBUV, 0).xy * 2.0f - 1.0f;
    float2 distortion = distortionA + distortionB * 0.65f;

    float rainAmount = saturate(WG_RainFXWeight);
    float rainImpactVisibility = wetMask * rainAmount * smoothstep(0.05f, 0.45f, WG_Wetness);
    float impactDensity = rainAmount * lerp(0.12f, 0.58f, rainAmount);
    float2 impactRipple = float2(0.0f, 0.0f);
    float impactRing = 0.0f;
    float impactPulse = 0.0f;
    AccumulateRainImpactLayer(
        wsPosition.xz, animationTime, 180.0f, 0.56f, impactDensity, 3.17f,
        impactRipple, impactRing, impactPulse);
    AccumulateRainImpactLayer(
        wsPosition.xz, animationTime, 127.0f, 0.79f, impactDensity * 0.72f, 11.83f,
        impactRipple, impactRing, impactPulse);

    float puddleImpactScale = lerp(0.42f, 1.0f, puddleMask);
    float2 rippleDistortion = impactRipple * rainImpactVisibility * puddleImpactScale;
    float ringVisibility = saturate(impactRing) * rainImpactVisibility * puddleImpactScale;
    float centralImpactVisibility = saturate(impactPulse) * rainImpactVisibility;

    float impactGlint = saturate(centralImpactVisibility * 0.82f + ringVisibility * 0.08f);

    float sceneLuma = dot(sceneColor, float3(0.2126f, 0.7152f, 0.0722f));
    float3 surfaceColor = sceneColor + impactGlint * max(sceneLuma, 0.08f) * 0.018f;

    // Keep the wet microstructure and all rain impacts anchored in world space.
    // The impacts deform real SSR or puddle sky reflections instead of creating fake reflections.
    float2 softenedRippleDistortion = float2(rippleDistortion.x * 0.72f, rippleDistortion.y);
    float puddleMicrostructure = lerp(1.0f, 0.30f, puddleMask);
    float2 combinedDistortion = float2(distortion.x * 0.55f, distortion.y * 0.85f) * puddleMicrostructure + softenedRippleDistortion * 1.55f;

    float3 wetNormal = normalize(wsNormal + float3(combinedDistortion.x, 0.0f, combinedDistortion.y) * lerp(0.16f, 0.075f, puddleMask));
    float3 puddlePlaneNormal = normalize(lerp(wsNormal, float3(0.0f, 1.0f, 0.0f), 0.78f));
    wetNormal = normalize(lerp(wetNormal, puddlePlaneNormal, puddleMask * 0.82f));

    float3 viewRay = normalize(wsPosition - WG_CameraPosition);
    float3 rayDirection = normalize(reflect(viewRay, wetNormal));
    if (rayDirection.y <= 0.015f)
        return float4(surfaceColor, 1.0f);

    float3 rayPosition = wsPosition + wetNormal * 12.0f;
    float stepSize = 35.0f;
    float2 hitUV = 0.0f;
    float hitWeight = 0.0f;

    float2 skyUV = 0.0f;
    float skyWeight = 0.0f;

    [loop]
    for (int i = 0; i < 16; ++i)
    {
        rayPosition += rayDirection * stepSize;
        stepSize *= 1.15f;

        float4 projected = mul(float4(rayPosition, 1.0f), WG_ViewProj);
        if (projected.w <= 0.0f)
            break;

        projected.xyz /= projected.w;
        float2 sampleUV = projected.xy * float2(0.5f, -0.5f) + 0.5f;

        if (any(sampleUV < 0.0f) || any(sampleUV > 1.0f) || projected.z < 0.0f || projected.z > 1.0f)
            break;

        float edge = max(abs(sampleUV.x - 0.5f), abs(sampleUV.y - 0.5f)) * 2.0f;
        float screenWeight = 1.0f - smoothstep(0.76f, 1.0f, edge);

        float sampleDepth = TX_Depth.SampleLevel(SS_Linear, sampleUV, 0).r;
        if (sampleDepth <= 1e-7f)
        {
            skyUV = sampleUV;
            skyWeight = screenWeight;
            continue;
        }

        float sampleZ = WG_ProjParams.z / (sampleDepth - WG_ProjParams.w);
        float depthDifference = projected.w - sampleZ;

        if (depthDifference > 0.0f && depthDifference < stepSize * 2.0f)
        {
            hitUV = sampleUV;
            hitWeight = screenWeight;
            break;
        }
    }

    float2 reflectionRippleOffset = float2(rippleDistortion.x * 0.0045f, rippleDistortion.y * 0.0160f);

    float3 reflectedColor = surfaceColor;
    float reflectionWeight = 0.0f;

    if (hitWeight > 0.0f)
    {
        float2 reflectedUV = saturate(hitUV + reflectionRippleOffset);
        float2 reflectedPixel = reflectedUV / WG_InvResolution;
        float reflectedWetSSRBlock = DecodeWetSSRBlock(SampleWetSSRBlockMask(reflectedPixel));

        if (reflectedWetSSRBlock <= 0.001f)
        {
            reflectedColor = SampleRoughReflection(reflectedUV, combinedDistortion);
            reflectionWeight = hitWeight;
        }
    }

    if (reflectionWeight <= 0.0f && skyWeight > 0.0f && puddleMask > 0.001f)
    {
        float2 reflectedSkyUV = saturate(skyUV + reflectionRippleOffset * lerp(0.25f, 0.65f, puddleMask));
        reflectedColor = ComposeWetGroundSky(reflectedSkyUV);
        reflectionWeight = skyWeight * puddleMask;
    }

    if (reflectionWeight <= 0.0f)
        return float4(surfaceColor, 1.0f);

    float reflectionLuma = dot(reflectedColor, float3(0.2126f, 0.7152f, 0.0722f));
    reflectedColor *= rcp(1.0f + max(0.0f, reflectionLuma - 1.0f) * 0.7f);

    float fresnel = pow(1.0f - saturate(dot(-viewRay, wetNormal)), 3.0f);
    float localReflectionStrength = lerp(1.75f, 8.00f, puddleMask);
    float puddleFresnel = lerp(fresnel, max(fresnel, 0.18f), puddleMask);

    float reflectionBlend = wetMask * reflectionWeight * lerp(0.06f, 0.28f, puddleFresnel) * WG_Strength * localReflectionStrength;
    return float4(lerp(surfaceColor, reflectedColor, saturate(reflectionBlend)), 1.0f);
}
