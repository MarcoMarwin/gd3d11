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
    float3 WG_RainFogColor;
    float WG_RainFogDensity;
    float WG_FogRange;
    float2 WG_Pad;
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


float3 ReconstructWorldPosition(float depth, float2 uv)
{
    float3 vsPosition = ReconstructVS(depth, uv);
    return mul(float4(vsPosition, 1.0f), WG_InvView).xyz;
}

float3 DecodeWorldNormal(float2 uv)
{
    float3 vsNormal = DecodeNormalGBuffer(TX_Normals.SampleLevel(SS_Linear, uv, 0).xy);
    return normalize(mul(float4(vsNormal, 0.0f), WG_InvView).xyz);
}

float3 CalculateSmoothedWetGroundNormal(
    float2 uv,
    float3 centerWSPosition,
    float3 centerWSNormal,
    float centerMaterialStrength)
{
    float3 normalSum = centerWSNormal * 2.0f;
    float weightSum = 2.0f;
    float cameraDistance = length(centerWSPosition - WG_CameraPosition);
    float maximumPositionDifference = max(18.0f, cameraDistance * 0.0025f);
    const float2 offsets[4] =
    {
        float2(1.0f, 0.0f),
        float2(-1.0f, 0.0f),
        float2(0.0f, 1.0f),
        float2(0.0f, -1.0f)
    };

    [unroll]
    for (int sampleIndex = 0; sampleIndex < 4; ++sampleIndex)
    {
        float2 sampleUV = uv + offsets[sampleIndex] * WG_InvResolution;
        if (any(sampleUV < 0.0f) || any(sampleUV > 1.0f))
            continue;

        float sampleDepth = TX_Depth.SampleLevel(SS_Linear, sampleUV, 0).r;
        if (sampleDepth <= 1e-7f)
            continue;

        float sampleMaterialStrength = saturate(TX_Material.SampleLevel(SS_Linear, sampleUV, 0).z);
        float materialWeight = 1.0f - saturate(abs(sampleMaterialStrength - centerMaterialStrength) * 8.0f);
        float3 sampleWSPosition = ReconstructWorldPosition(sampleDepth, sampleUV);
        float positionDifference = length(sampleWSPosition - centerWSPosition);
        float positionWeight = 1.0f - smoothstep(
            maximumPositionDifference * 0.35f,
            maximumPositionDifference,
            positionDifference);
        float3 sampleWSNormal = DecodeWorldNormal(sampleUV);
        float normalWeight = smoothstep(0.30f, 0.92f, saturate(dot(centerWSNormal, sampleWSNormal)));
        float upwardWeight = smoothstep(0.30f, 0.80f, sampleWSNormal.y);
        float sampleWeight = materialWeight * positionWeight * normalWeight * upwardWeight;

        normalSum += sampleWSNormal * sampleWeight;
        weightSum += sampleWeight;
    }

    float3 filteredNormal = normalize(normalSum / max(weightSum, 0.001f));
    float filterAmount = saturate((weightSum - 2.0f) * 0.28f);
    return normalize(lerp(centerWSNormal, filteredNormal, filterAmount));
}

float3 ApplyWetGroundRainHaze(float3 reflectedColor, float reflectedDepth, float2 reflectedUV)
{
    float rainAmount = saturate(max(WG_RainFXWeight, AC_RainFXWeight));
    if (rainAmount <= 0.001f || reflectedDepth <= 1e-7f || WG_RainFogDensity <= 0.0f)
        return reflectedColor;

    float3 reflectedWSPosition = ReconstructWorldPosition(reflectedDepth, reflectedUV);
    float reflectedDistance = length(reflectedWSPosition - WG_CameraPosition);
    float normalizedDistance = saturate(reflectedDistance / max(WG_FogRange, 1.0f));
    float exponentialFog = 1.0f - exp2(
        -max(WG_RainFogDensity, 0.0f) * reflectedDistance * 0.001442695f);
    float rangeFog = smoothstep(0.10f, 1.0f, normalizedDistance);
    float rainFogAmount = saturate(
        max(exponentialFog, rangeFog * WG_RainFogDensity) * rainAmount);

    return lerp(reflectedColor, WG_RainFogColor, rainFogAmount);
}

float GetRainExposure(float3 wsPosition)
{
    float4 shadowPosition = mul(float4(wsPosition, 1.0f), WG_RainViewProj);
    float2 shadowUV = shadowPosition.xy * float2(0.5f, -0.5f) + 0.5f;
    if (any(shadowUV < 0.0f) || any(shadowUV > 1.0f))
        return 0.0f;

    return TX_RainShadow.SampleCmpLevelZero(SS_Comp, shadowUV, shadowPosition.z - 0.0001f);
}

float3 SampleRoughReflection(float2 uv, float2 distortion, float roughness)
{
    float controlledRoughness = saturate(roughness);
    float2 spread = (WG_InvResolution * lerp(0.75f, 2.0f, controlledRoughness))
        + abs(distortion) * lerp(0.00075f, 0.0025f, controlledRoughness);
    float centerWeight = lerp(0.72f, 0.52f, controlledRoughness);
    float sideWeight = (1.0f - centerWeight) * 0.25f;
    float3 color = TX_Scene.SampleLevel(SS_Linear, uv, 0).rgb * centerWeight;
    color += TX_Scene.SampleLevel(SS_Linear, uv + float2(spread.x, 0.0f), 0).rgb * sideWeight;
    color += TX_Scene.SampleLevel(SS_Linear, uv - float2(spread.x, 0.0f), 0).rgb * sideWeight;
    color += TX_Scene.SampleLevel(SS_Linear, uv + float2(0.0f, spread.y), 0).rgb * sideWeight;
    color += TX_Scene.SampleLevel(SS_Linear, uv - float2(0.0f, spread.y), 0).rgb * sideWeight;
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
    float broadNoise = PuddleValueNoise(worldXZ * 0.00110f);
    float mediumNoise = PuddleValueNoise(worldXZ * 0.00285f + float2(17.31f, 43.77f));
    float smallNoise = PuddleValueNoise(worldXZ * 0.00680f + float2(61.19f, 8.53f));
    float breakupNoise = PuddleValueNoise(worldXZ * 0.01250f + float2(93.41f, 27.19f));
    float puddleField = broadNoise * 0.22f + mediumNoise * 0.46f + smallNoise * 0.32f;
    float threshold = 0.625f - saturate(wetness) * 0.070f;
    float shapeFeather = max(0.165f, fwidth(puddleField) * 3.5f);
    float puddleShape = smoothstep(threshold - shapeFeather * 0.55f, threshold + shapeFeather, puddleField);
    float puddleBreakup = smoothstep(0.24f, 0.78f, breakupNoise);
    float separatedPuddles = puddleShape * lerp(0.28f, 1.0f, puddleBreakup);
    float rareLargePuddle = smoothstep(0.70f, 0.96f, broadNoise) * 0.72f;
    puddleShape = saturate(separatedPuddles + rareLargePuddle * (1.0f - separatedPuddles));
    float slopeFeather = max(0.020f, fwidth(wsNormalY) * 3.0f);
    float puddleFlatness = smoothstep(0.885f - slopeFeather, 0.998f + slopeFeather, wsNormalY);
    float puddleMask = saturate(puddleShape * puddleFlatness);
    return puddleMask * puddleMask * (3.0f - 2.0f * puddleMask);
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
            float cycleTime = time * cycleRate + seed;
            float cycleIndex = floor(cycleTime);
            float phase = frac(cycleTime);
            float2 cycleOffset = float2(
                cycleIndex * 19.19f + layerSeed * 2.173f,
                cycleIndex * 47.47f + layerSeed * 0.917f);
            float eventSeed = PuddleHash21(cell + cycleOffset + float2(13.17f, 47.53f));
            float eventMask = step(1.0f - density, eventSeed);

            float2 pointJitter = float2(
                PuddleHash21(cell + cycleOffset + float2(layerSeed + 5.31f, layerSeed + 19.73f)),
                PuddleHash21(cell + cycleOffset.yx + float2(layerSeed + 31.91f, layerSeed + 7.57f)));
            float2 impactPosition = (cell + 0.15f + pointJitter * 0.70f) * cellSize;
            float2 delta = worldXZ - impactPosition;
            float distanceToImpact = length(delta);
            float2 radialDirection = delta / max(distanceToImpact, 0.001f);

            float radiusVariation = lerp(
                0.82f, 1.12f,
                PuddleHash21(cell + cycleOffset + float2(layerSeed + 71.11f, layerSeed + 3.29f)));

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

float EvaluatePuddleSurfaceSupport(float3 wsPosition, float3 wsNormal, float2 sampleUV)
{
    if (any(sampleUV < 0.0f) || any(sampleUV > 1.0f))
        return 0.0f;

    float sampleDepth = TX_Depth.SampleLevel(SS_Linear, sampleUV, 0).r;
    if (sampleDepth <= 1e-7f)
        return 0.0f;

    float3 sampleVSPosition = ReconstructVS(sampleDepth, sampleUV);
    float3 sampleWSPosition = mul(float4(sampleVSPosition, 1.0f), WG_InvView).xyz;
    float3 sampleVSNormal = DecodeNormalGBuffer(TX_Normals.SampleLevel(SS_Linear, sampleUV, 0).xy);
    float3 sampleWSNormal = normalize(mul(float4(sampleVSNormal, 0.0f), WG_InvView).xyz);
    float3 surfaceDelta = sampleWSPosition - wsPosition;
    float tangentDistance = length(surfaceDelta - wsNormal * dot(surfaceDelta, wsNormal));
    float planeDeviation = abs(dot(surfaceDelta, wsNormal));
    float normalSupport = smoothstep(0.90f, 0.985f, dot(wsNormal, sampleWSNormal));
    float planeTolerance = max(5.0f, tangentDistance * 0.10f);
    float planeSupport = 1.0f - smoothstep(planeTolerance, planeTolerance * 2.25f, planeDeviation);
    return saturate(normalSupport * planeSupport);
}

float CalculatePuddleSurfaceSupport(float3 wsPosition, float3 wsNormal, float2 uv)
{
    float2 nearOffset = WG_InvResolution * 10.0f;
    float2 farOffset = WG_InvResolution * 26.0f;
    float nearSupport = 0.0f;
    nearSupport += EvaluatePuddleSurfaceSupport(wsPosition, wsNormal, uv + float2(nearOffset.x, 0.0f));
    nearSupport += EvaluatePuddleSurfaceSupport(wsPosition, wsNormal, uv - float2(nearOffset.x, 0.0f));
    nearSupport += EvaluatePuddleSurfaceSupport(wsPosition, wsNormal, uv + float2(0.0f, nearOffset.y));
    nearSupport += EvaluatePuddleSurfaceSupport(wsPosition, wsNormal, uv - float2(0.0f, nearOffset.y));
    nearSupport *= 0.25f;

    float farSupport = 0.0f;
    farSupport += EvaluatePuddleSurfaceSupport(wsPosition, wsNormal, uv + float2(farOffset.x, 0.0f));
    farSupport += EvaluatePuddleSurfaceSupport(wsPosition, wsNormal, uv - float2(farOffset.x, 0.0f));
    farSupport += EvaluatePuddleSurfaceSupport(wsPosition, wsNormal, uv + float2(0.0f, farOffset.y));
    farSupport += EvaluatePuddleSurfaceSupport(wsPosition, wsNormal, uv - float2(0.0f, farOffset.y));
    farSupport *= 0.25f;

    float combinedSupport = nearSupport * farSupport;
    return smoothstep(0.48f, 0.90f, combinedSupport);
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

    float3 wsPosition = ReconstructWorldPosition(depth, uv);
    float3 sourceWSNormal = DecodeWorldNormal(uv);
    float sourceUpwardMask = smoothstep(0.45f, 0.88f, sourceWSNormal.y);
    if (sourceUpwardMask <= 0.01f)
        return float4(sceneColor, 1.0f);

    float materialWetGroundSSRStrength = saturate(TX_Material.SampleLevel(SS_Linear, uv, 0).z);
    if (materialWetGroundSSRStrength <= 0.001f)
        return float4(sceneColor, 1.0f);

    float3 wsNormal = CalculateSmoothedWetGroundNormal(
        uv,
        wsPosition,
        sourceWSNormal,
        materialWetGroundSSRStrength);
    float upwardMask = smoothstep(0.45f, 0.88f, wsNormal.y);
    if (upwardMask <= 0.01f)
        return float4(sceneColor, 1.0f);
    float rainExposure = GetRainExposure(wsPosition);
    float wetness = saturate(WG_Wetness);
    float commonWetMask = upwardMask * rainExposure * wetness * wetSSRVisibility;
    float puddleMask = CalculatePuddleMask(wsPosition, wsNormal.y, materialWetGroundSSRStrength, wetness);
    float puddleSurfaceSupport = CalculatePuddleSurfaceSupport(wsPosition, wsNormal, uv);
    puddleMask *= puddleSurfaceSupport;
    float materialWetStrength = materialWetGroundSSRStrength * lerp(0.72f, 1.0f, wetness);
    float materialWetMask = commonWetMask * materialWetStrength;
    float puddleWetMask = rainExposure * wetness * wetSSRVisibility * puddleMask;
    float wetMask = saturate(materialWetMask + puddleWetMask * (1.0f - materialWetMask));
    if (wetMask <= 0.01f)
        return float4(sceneColor, 1.0f);

    float2 wetUV = wsPosition.xz / 1100.0f;
    float animationTime = fmod(max(WG_Time, 0.0f), 256.0f);
    float2 distortionAUV = frac(wetUV);
    float2 distortionBUV = frac(wetUV * 0.63f + float2(0.137f, 0.421f));
    float2 distortionA = TX_Distortion.SampleLevel(SS_Linear, distortionAUV, 0).xy * 2.0f - 1.0f;
    float2 distortionB = TX_Distortion.SampleLevel(SS_Linear, distortionBUV, 0).xy * 2.0f - 1.0f;
    float2 staticDistortion = distortionA + distortionB * 0.65f;

    float rainAmount = saturate(WG_RainFXWeight);
    float rainImpactVisibility = wetMask * rainAmount * smoothstep(0.05f, 0.45f, WG_Wetness);
    float puddleRainResponse = lerp(0.62f, 2.20f, puddleMask);
    float impactDensity = rainAmount * lerp(0.58f, 1.0f, rainAmount);
    float2 impactRipple = float2(0.0f, 0.0f);
    float impactRing = 0.0f;
    float impactPulse = 0.0f;

    AccumulateRainImpactLayer(
        wsPosition.xz, animationTime, 58.0f, 1.08f, impactDensity, 3.17f,
        impactRipple, impactRing, impactPulse);
    AccumulateRainImpactLayer(
        wsPosition.xz, animationTime, 41.0f, 1.46f, impactDensity * 0.98f, 11.83f,
        impactRipple, impactRing, impactPulse);
    AccumulateRainImpactLayer(
        wsPosition.xz, animationTime, 31.0f, 1.92f, impactDensity * 0.94f, 23.41f,
        impactRipple, impactRing, impactPulse);

    float2 rippleDistortion = impactRipple * rainImpactVisibility * puddleRainResponse;
    float ringVisibility = saturate(impactRing) * rainImpactVisibility * puddleRainResponse;
    float centralImpactVisibility = saturate(impactPulse) * rainImpactVisibility * puddleRainResponse;
    float3 surfaceColor = sceneColor;

    float materialMicrostructure = lerp(0.115f, 0.045f, puddleMask);
    float2 baseNormalDistortion = float2(staticDistortion.x * 0.55f, staticDistortion.y * 0.85f);
    float3 materialWetNormal = normalize(
        wsNormal + float3(baseNormalDistortion.x, 0.0f, baseNormalDistortion.y) * materialMicrostructure);

    float3 puddlePlaneNormal = normalize(lerp(wsNormal, float3(0.0f, 1.0f, 0.0f), 0.62f));
    float3 wetBaseNormal = normalize(lerp(materialWetNormal, puddlePlaneNormal, puddleMask * 0.88f));

    float ringNormalStrength = lerp(0.055f, 0.145f, puddleMask);
    float ringShapeStrength = saturate(ringVisibility * 0.32f + centralImpactVisibility * 0.18f);
    float2 rainNormalDistortion = rippleDistortion * ringNormalStrength * (1.0f + ringShapeStrength);
    float3 wetNormal = normalize(
        wetBaseNormal + float3(rainNormalDistortion.x, 0.0f, rainNormalDistortion.y));

    float3 viewRay = normalize(wsPosition - WG_CameraPosition);
    float3 rayDirection = normalize(reflect(viewRay, wetNormal));
    if (rayDirection.y <= 0.015f)
        return float4(surfaceColor, 1.0f);

    float3 rayPosition = wsPosition + wetNormal * 8.0f;
    float3 previousRayPosition = rayPosition;
    float stepSize = 18.0f;
    float2 hitUV = 0.0f;
    float hitWeight = 0.0f;
    float2 skyUV = 0.0f;
    float skyWeight = 0.0f;

    [loop]
    for (int i = 0; i < 24; ++i)
    {
        previousRayPosition = rayPosition;
        rayPosition += rayDirection * stepSize;

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
            stepSize *= 1.10f;
            continue;
        }

        float sampleZ = WG_ProjParams.z / (sampleDepth - WG_ProjParams.w);
        float depthDifference = projected.w - sampleZ;

        if (depthDifference > 0.0f && depthDifference < stepSize * 1.15f)
        {
            float3 refinementLow = previousRayPosition;
            float3 refinementHigh = rayPosition;
            float2 refinedUV = sampleUV;
            float refinedScreenWeight = screenWeight;
            float refinedDepthError = abs(depthDifference);

            [unroll]
            for (int refinementStep = 0; refinementStep < 6; ++refinementStep)
            {
                float3 refinementPosition = (refinementLow + refinementHigh) * 0.5f;
                float4 refinementProjected = mul(float4(refinementPosition, 1.0f), WG_ViewProj);

                if (refinementProjected.w <= 0.0f)
                {
                    refinementHigh = refinementPosition;
                    continue;
                }

                refinementProjected.xyz /= refinementProjected.w;
                float2 refinementUV = refinementProjected.xy * float2(0.5f, -0.5f) + 0.5f;

                if (any(refinementUV < 0.0f) || any(refinementUV > 1.0f)
                    || refinementProjected.z < 0.0f || refinementProjected.z > 1.0f)
                {
                    refinementHigh = refinementPosition;
                    continue;
                }

                float refinementDepth = TX_Depth.SampleLevel(SS_Linear, refinementUV, 0).r;
                if (refinementDepth <= 1e-7f)
                {
                    refinementLow = refinementPosition;
                    continue;
                }

                float refinementDenominator = refinementDepth - WG_ProjParams.w;
                if (abs(refinementDenominator) <= 1e-7f)
                {
                    refinementHigh = refinementPosition;
                    continue;
                }

                float refinementSampleZ = WG_ProjParams.z / refinementDenominator;
                float refinementDifference = refinementProjected.w - refinementSampleZ;
                float candidateDepthError = abs(refinementDifference);

                if (candidateDepthError < refinedDepthError)
                {
                    refinedDepthError = candidateDepthError;
                    refinedUV = refinementUV;
                    float refinementEdge = max(abs(refinementUV.x - 0.5f), abs(refinementUV.y - 0.5f)) * 2.0f;
                    refinedScreenWeight = 1.0f - smoothstep(0.76f, 1.0f, refinementEdge);
                }

                if (refinementDifference > 0.0f)
                    refinementHigh = refinementPosition;
                else
                    refinementLow = refinementPosition;
            }

            float maximumRefinedError = max(1.25f, stepSize * 0.22f);
            if (refinedDepthError <= maximumRefinedError)
            {
                hitUV = refinedUV;
                hitWeight = refinedScreenWeight;
            }
            break;
        }

        stepSize *= 1.10f;
    }

    float2 reflectionRippleOffset = rippleDistortion * float2(0.0040f, 0.0040f);
    float3 reflectedColor = surfaceColor;
    float reflectionWeight = 0.0f;

    if (hitWeight > 0.0f)
    {
        float2 reflectedUV = saturate(hitUV + reflectionRippleOffset);
        float2 reflectedPixel = reflectedUV / WG_InvResolution;
        float reflectedWetSSRBlock = DecodeWetSSRBlock(SampleWetSSRBlockMask(reflectedPixel));

        if (reflectedWetSSRBlock <= 0.001f)
        {
            float3 directReflection = TX_Scene.SampleLevel(SS_Linear, reflectedUV, 0).rgb;
            float reflectionRoughness = lerp(0.78f, 0.08f, puddleMask);
            float3 roughReflection = SampleRoughReflection(reflectedUV, baseNormalDistortion, reflectionRoughness);
            float directColorWeight = smoothstep(0.20f, 0.85f, puddleMask) * 0.90f;
            reflectedColor = lerp(roughReflection, directReflection, directColorWeight);
            float reflectedDepth = TX_Depth.SampleLevel(SS_Linear, reflectedUV, 0).r;
            reflectedColor = ApplyWetGroundRainHaze(reflectedColor, reflectedDepth, reflectedUV);
            reflectionWeight = hitWeight;
        }
    }

    if (reflectionWeight <= 0.0f && skyWeight > 0.0f)
    {
        float skySurfaceWeight = saturate(materialWetMask * 0.72f);
        float skyPuddleWeight = saturate(puddleWetMask);
        float skyReflectionWeight = max(skySurfaceWeight, skyPuddleWeight);
        float2 reflectedSkyUV = saturate(
            skyUV + reflectionRippleOffset * lerp(0.18f, 0.80f, puddleMask));
        reflectedColor = ComposeWetGroundSky(reflectedSkyUV);
        reflectionWeight = skyWeight * skyReflectionWeight;
    }

    if (reflectionWeight <= 0.0f)
        return float4(surfaceColor, 1.0f);

    float reflectionLuma = dot(reflectedColor, float3(0.2126f, 0.7152f, 0.0722f));
    reflectedColor *= rcp(1.0f + max(0.0f, reflectionLuma - 1.0f) * 0.7f);

    float fresnel = pow(1.0f - saturate(dot(-viewRay, wetNormal)), 3.0f);
    float materialFresnel = lerp(0.085f, 0.28f, fresnel);
    float puddleFresnel = max(fresnel, 0.14f);
    float materialReflectionBlend =
        materialWetMask * reflectionWeight * materialFresnel * WG_Strength * 2.60f;
    float puddleReflectionBlend =
        puddleWetMask * reflectionWeight * lerp(0.12f, 0.24f, puddleFresnel) * WG_Strength * 4.35f;
    float reflectionBlend = saturate(
        materialReflectionBlend + puddleReflectionBlend * (1.0f - materialReflectionBlend));

    return float4(lerp(surfaceColor, reflectedColor, reflectionBlend), 1.0f);
}
