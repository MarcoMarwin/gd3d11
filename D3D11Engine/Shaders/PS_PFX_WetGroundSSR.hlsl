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
    float WG_WetMaterialReflectionsStrength;
    float WG_ProceduralPuddlesStrength;
    float WG_PuddleReflectionsStrength;
    float WG_WetGroundRainImpactsStrength;
    float WG_PuddleAccumulation;
    float WG_ReflectionsEnabled;
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

bool EvaluateWetGroundReceiverSample(
    float2 sampleUV,
    float3 centerWSPosition,
    float3 centerWSNormal,
    out float3 sampleWSPosition)
{
    float2 halfTexel = WG_InvResolution * 0.5f;
    if (any(sampleUV < halfTexel) || any(sampleUV > 1.0f - halfTexel))
    {
        sampleWSPosition = centerWSPosition;
        return false;
    }

    float sampleDepth = TX_Depth.SampleLevel(SS_Linear, sampleUV, 0).r;
    if (sampleDepth <= 1e-7f)
    {
        sampleWSPosition = centerWSPosition;
        return false;
    }

    sampleWSPosition = ReconstructWorldPosition(sampleDepth, sampleUV);
    float3 sampleWSNormal = DecodeWorldNormal(sampleUV);
    float3 surfaceDelta = sampleWSPosition - centerWSPosition;
    float tangentDistance = length(
        surfaceDelta - centerWSNormal * dot(surfaceDelta, centerWSNormal));
    float planeDeviation = abs(dot(surfaceDelta, centerWSNormal));
    float cameraDistance = length(centerWSPosition - WG_CameraPosition);
    float planeTolerance = max(
        12.0f,
        max(tangentDistance * 0.22f, cameraDistance * 0.0015f));
    float normalAgreement = dot(centerWSNormal, sampleWSNormal);

    return planeDeviation <= planeTolerance && normalAgreement >= 0.35f;
}

float3 CalculateGeometricWorldNormal(
    float2 uv,
    float3 centerWSPosition,
    float3 centerWSNormal)
{
    float2 offset = WG_InvResolution * 3.0f;
    float2 uvLeft = uv - float2(offset.x, 0.0f);
    float2 uvRight = uv + float2(offset.x, 0.0f);
    float2 uvUp = uv - float2(0.0f, offset.y);
    float2 uvDown = uv + float2(0.0f, offset.y);

    float3 positionLeft = centerWSPosition;
    float3 positionRight = centerWSPosition;
    float3 positionUp = centerWSPosition;
    float3 positionDown = centerWSPosition;

    bool validLeft = EvaluateWetGroundReceiverSample(
        uvLeft, centerWSPosition, centerWSNormal, positionLeft);
    bool validRight = EvaluateWetGroundReceiverSample(
        uvRight, centerWSPosition, centerWSNormal, positionRight);
    bool validUp = EvaluateWetGroundReceiverSample(
        uvUp, centerWSPosition, centerWSNormal, positionUp);
    bool validDown = EvaluateWetGroundReceiverSample(
        uvDown, centerWSPosition, centerWSNormal, positionDown);

    float3 tangentX = float3(0.0f, 0.0f, 0.0f);
    float3 tangentY = float3(0.0f, 0.0f, 0.0f);

    if (validLeft && validRight)
        tangentX = positionRight - positionLeft;
    else if (validRight)
        tangentX = positionRight - centerWSPosition;
    else if (validLeft)
        tangentX = centerWSPosition - positionLeft;

    if (validUp && validDown)
        tangentY = positionDown - positionUp;
    else if (validDown)
        tangentY = positionDown - centerWSPosition;
    else if (validUp)
        tangentY = centerWSPosition - positionUp;

    if (dot(tangentX, tangentX) <= 1e-6f || dot(tangentY, tangentY) <= 1e-6f)
    {
        return centerWSNormal;
    }

    float3 geometricNormal = cross(tangentY, tangentX);
    if (dot(geometricNormal, geometricNormal) <= 1e-8f)
        return centerWSNormal;

    geometricNormal = normalize(geometricNormal);
    if (dot(geometricNormal, centerWSNormal) < 0.0f)
        geometricNormal = -geometricNormal;

    float normalAgreement = dot(geometricNormal, centerWSNormal);
    if (normalAgreement < 0.35f)
        return centerWSNormal;

    return geometricNormal;
}

float CalculateReflectionScreenFade(float2 uv)
{
    float2 edgeDistance = min(uv, 1.0f - uv);
    return smoothstep(0.012f, 0.105f, min(edgeDistance.x, edgeDistance.y));
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
    float rangeFog = smoothstep(0.18f, 1.0f, normalizedDistance) * saturate(WG_RainFogDensity);
    float rainFogAmount = saturate(max(exponentialFog, rangeFog) * rainAmount);
    float limitedFogAmount = min(rainFogAmount, lerp(0.48f, 0.72f, normalizedDistance));
    return lerp(reflectedColor, WG_RainFogColor, limitedFogAmount);
}
float GetRainExposure(float3 wsPosition)
{
    float4 shadowPosition = mul(float4(wsPosition, 1.0f), WG_RainViewProj);
    float2 shadowUV = shadowPosition.xy * float2(0.5f, -0.5f) + 0.5f;
    if (any(shadowUV < 0.0f) || any(shadowUV > 1.0f))
        return 0.0f;

    return TX_RainShadow.SampleCmpLevelZero(SS_Comp, shadowUV, shadowPosition.z - 0.0001f);
}

float EvaluatePuddleEligibilitySample(
    float2 sampleUV,
    float3 centerWSPosition,
    float3 centerGeometricNormal,
    out float sampleValidity)
{
    float3 sampleWSPosition = centerWSPosition;
    bool validReceiverSample = EvaluateWetGroundReceiverSample(
        sampleUV, centerWSPosition, centerGeometricNormal, sampleWSPosition);

    if (!validReceiverSample)
    {
        sampleValidity = 0.0f;
        return 0.0f;
    }

    sampleValidity = 1.0f;
    float3 surfaceDelta = sampleWSPosition - centerWSPosition;
    float tangentDistance = length(
        surfaceDelta - centerGeometricNormal * dot(surfaceDelta, centerGeometricNormal));
    float planeDeviation = abs(dot(surfaceDelta, centerGeometricNormal));
    float planeTolerance = max(8.0f, tangentDistance * 0.14f);

    float continuityEligibility =
        1.0f - smoothstep(planeTolerance, planeTolerance * 3.0f, planeDeviation);
    float slopeEligibility = smoothstep(0.88f, 0.985f, centerGeometricNormal.y);

    return saturate(slopeEligibility * continuityEligibility);
}

float CalculatePuddleBoundaryMask(
    float2 uv,
    float3 wsPosition,
    float3 geometricNormal)
{
    float cameraDistance = length(wsPosition - WG_CameraPosition);
    float samplePixels = clamp(11.0f + cameraDistance * 0.00045f, 11.0f, 22.0f);
    float2 sampleOffset = WG_InvResolution * samplePixels;
    const float2 directions[4] =
    {
        float2(1.0f, 0.0f),
        float2(-1.0f, 0.0f),
        float2(0.0f, 1.0f),
        float2(0.0f, -1.0f)
    };

    float centerSlope = smoothstep(0.88f, 0.985f, geometricNormal.y);
    float eligibilitySum = centerSlope * 2.0f;
    float validitySum = 2.0f;

    [unroll]
    for (int sampleIndex = 0; sampleIndex < 4; ++sampleIndex)
    {
        float sampleValidity = 0.0f;
        float sampleEligibility = EvaluatePuddleEligibilitySample(
            uv + directions[sampleIndex] * sampleOffset,
            wsPosition,
            geometricNormal,
            sampleValidity);
        eligibilitySum += sampleEligibility * sampleValidity;
        validitySum += sampleValidity;
    }

    float neighborhoodEligibility = eligibilitySum / max(validitySum, 1.0f);
    float boundaryFeather = max(0.035f, fwidth(neighborhoodEligibility) * 2.5f);

    return smoothstep(
        0.34f - boundaryFeather,
        0.82f + boundaryFeather,
        neighborhoodEligibility);
}
float3 SampleRoughReflection(float2 uv, float2 distortion, float roughness)
{
    float controlledRoughness = saturate(roughness);
    float distortionVariation = saturate(length(distortion) * 0.85f);
    float roughSpread = lerp(1.10f, 3.20f, controlledRoughness)
        * lerp(0.90f, 1.30f, distortionVariation);
    float2 spread = WG_InvResolution * roughSpread
        + abs(distortion) * lerp(0.00110f, 0.00340f, controlledRoughness);
    float centerWeight = lerp(0.66f, 0.44f, controlledRoughness);
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

float CalculateRawPuddleShape(float2 worldXZ, float wetness)
{
    float broadNoise = PuddleValueNoise(worldXZ * 0.00120f);
    float mediumNoise = PuddleValueNoise(worldXZ * 0.00315f + float2(17.31f, 43.77f));
    float smallNoise = PuddleValueNoise(worldXZ * 0.00485f + float2(61.19f, 8.53f));
    float breakupNoise = PuddleValueNoise(worldXZ * 0.01350f + float2(93.41f, 27.19f));
    float puddleField = broadNoise * 0.14f + mediumNoise * 0.50f + smallNoise * 0.36f;
    float threshold = 0.640f - saturate(wetness) * 0.035f;
    float shapeFeather = 0.070f;
    float puddleShape = smoothstep(
        threshold - shapeFeather,
        threshold + shapeFeather,
        puddleField);
    float puddleBreakup = smoothstep(0.16f, 0.80f, breakupNoise);
    float separatedPuddles = puddleShape * lerp(0.24f, 1.0f, puddleBreakup);

    float fillMediumNoise = PuddleValueNoise(
        worldXZ * 0.00375f + float2(137.53f, 211.17f));
    float fillShapeNoise = PuddleValueNoise(
        worldXZ * 0.00610f + float2(271.91f, 89.47f));
    float fillBreakupNoise = PuddleValueNoise(
        worldXZ * 0.01180f + float2(43.13f, 317.29f));
    float fillField = fillMediumNoise * 0.62f + fillShapeNoise * 0.38f;
    float fillThreshold = 0.655f - saturate(wetness) * 0.025f;
    float fillPuddle = smoothstep(
        fillThreshold - 0.060f,
        fillThreshold + 0.075f,
        fillField);
    float fillBreakup = smoothstep(0.22f, 0.78f, fillBreakupNoise);
    fillPuddle *= lerp(0.28f, 0.82f, fillBreakup);
    float emptyAreaWeight = 1.0f - smoothstep(0.16f, 0.52f, separatedPuddles);
    float distributedFill = fillPuddle * emptyAreaWeight * 0.72f;

    float rareLargePuddle = smoothstep(0.82f, 0.985f, broadNoise) * 0.20f;
    float combinedPuddles = separatedPuddles + distributedFill * (1.0f - separatedPuddles);
    return saturate(combinedPuddles + rareLargePuddle * (1.0f - combinedPuddles));
}
float CalculatePuddleMask(float3 wsPosition, float wsNormalY, float wetness)
{
    float cameraDistance = length(wsPosition - WG_CameraPosition);
    float filterRadius = clamp(22.0f + cameraDistance * 0.0014f, 22.0f, 48.0f);
    float2 worldXZ = wsPosition.xz;
    float centerShape = CalculateRawPuddleShape(worldXZ, wetness);
    float axialShape = 0.0f;
    axialShape += CalculateRawPuddleShape(worldXZ + float2(filterRadius, 0.0f), wetness);
    axialShape += CalculateRawPuddleShape(worldXZ - float2(filterRadius, 0.0f), wetness);
    axialShape += CalculateRawPuddleShape(worldXZ + float2(0.0f, filterRadius), wetness);
    axialShape += CalculateRawPuddleShape(worldXZ - float2(0.0f, filterRadius), wetness);
    float filteredShape = (centerShape * 4.0f + axialShape) * 0.125f;
    float compactShape = smoothstep(0.090f, 0.590f, saturate(filteredShape));
    float slopeFeather = max(0.024f, fwidth(wsNormalY) * 3.0f);
    float puddleFlatness = smoothstep(
        0.885f - slopeFeather,
        0.992f + slopeFeather,
        wsNormalY);
    float softSlopeFade = puddleFlatness * puddleFlatness * (3.0f - 2.0f * puddleFlatness);
    return saturate(compactShape * softSlopeFade);
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

float EvaluatePuddleSurfaceSupport(
    float3 wsPosition,
    float3 geometricNormal,
    float2 sampleUV,
    out float sampleValidity)
{
    float3 sampleWSPosition = wsPosition;
    bool validReceiverSample = EvaluateWetGroundReceiverSample(
        sampleUV, wsPosition, geometricNormal, sampleWSPosition);

    if (!validReceiverSample)
    {
        sampleValidity = 0.0f;
        return 0.0f;
    }

    sampleValidity = 1.0f;
    float3 surfaceDelta = sampleWSPosition - wsPosition;
    float tangentDistance = length(
        surfaceDelta - geometricNormal * dot(surfaceDelta, geometricNormal));
    float planeDeviation = abs(dot(surfaceDelta, geometricNormal));
    float planeTolerance = max(
        8.0f, tangentDistance * 0.18f);

    return 1.0f - smoothstep(
        planeTolerance, planeTolerance * 3.25f, planeDeviation);
}

float CalculatePuddleSurfaceSupport(
    float3 wsPosition,
    float3 wsNormal,
    float2 uv)
{
    float2 supportOffset = WG_InvResolution * 18.0f;
    const float2 directions[4] =
    {
        float2(1.0f, 0.0f),
        float2(-1.0f, 0.0f),
        float2(0.0f, 1.0f),
        float2(0.0f, -1.0f)
    };

    float supportSum = 1.0f;
    float validitySum = 1.0f;

    [unroll]
    for (int sampleIndex = 0; sampleIndex < 4; ++sampleIndex)
    {
        float sampleValidity = 0.0f;
        float sampleSupport = EvaluatePuddleSurfaceSupport(
            wsPosition,
            wsNormal,
            uv + directions[sampleIndex] * supportOffset,
            sampleValidity);
        supportSum += sampleSupport * sampleValidity;
        validitySum += sampleValidity;
    }

    float support = supportSum / max(validitySum, 1.0f);
    float supportFeather = max(
        0.045f, fwidth(support) * 2.0f);

    return smoothstep(
        0.10f - supportFeather, 0.88f + supportFeather, support);
}

float4 PSMain(PS_INPUT input) : SV_TARGET
{
    float2 uv = input.vTexcoord;
    float3 sceneColor = TX_Scene.SampleLevel(SS_Linear, uv, 0).rgb;
    float wetSSRVisibility = 1.0f - DecodeWetSSRBlock(SampleWetSSRBlockMask(input.vPosition.xy));

    float depth = TX_Depth.SampleLevel(SS_Linear, uv, 0).r;
    if (depth <= 1e-7f || WG_Wetness <= 0.001f)
        return float4(sceneColor, 1.0f);
    if (wetSSRVisibility <= 0.001f)
        return float4(sceneColor, 1.0f);

    float3 wsPosition = ReconstructWorldPosition(depth, uv);
    float3 sourceWSNormal = DecodeWorldNormal(uv);
    float3 geometricWSNormal = CalculateGeometricWorldNormal(
        uv, wsPosition, sourceWSNormal);

    float materialWetGroundSSRStrength = saturate(
        TX_Material.SampleLevel(SS_Linear, uv, 0).z);
    float materialWetGroundEligibility = step(0.0001f, materialWetGroundSSRStrength);
    float reflectionsEnabled = step(0.5f, WG_ReflectionsEnabled) * step(0.001f, WG_Strength);
    float materialPuddleEligibility = materialWetGroundEligibility * reflectionsEnabled;

    float3 wsNormal = CalculateSmoothedWetGroundNormal(
        uv,
        wsPosition,
        sourceWSNormal,
        materialWetGroundSSRStrength);
    float upwardMask = smoothstep(0.38f, 0.82f, geometricWSNormal.y);
    float rainExposure = GetRainExposure(wsPosition);
    float wetness = saturate(WG_Wetness);
    float puddleAccumulation = smoothstep( 0.18f, 0.95f, saturate(WG_PuddleAccumulation));
    float commonWetMask = upwardMask * rainExposure * wetness * wetSSRVisibility;
    float puddleMask = CalculatePuddleMask(
        wsPosition, geometricWSNormal.y, puddleAccumulation);
    float puddleSurfaceSupport = CalculatePuddleSurfaceSupport(wsPosition, geometricWSNormal, uv);
    float puddleBoundaryMask = CalculatePuddleBoundaryMask(
        uv, wsPosition, geometricWSNormal);
    float puddleSupportFade = lerp(0.82f, 1.0f, smoothstep(0.05f, 0.95f, puddleSurfaceSupport));
    puddleMask *= puddleSupportFade
        * puddleBoundaryMask
        * materialPuddleEligibility;
    float materialWetStrength = materialWetGroundSSRStrength * lerp(0.72f, 1.0f, wetness);
    float materialWetMask = commonWetMask * materialWetStrength
        * max(WG_WetMaterialReflectionsStrength, 0.0f)
        * reflectionsEnabled;
    puddleMask = saturate(
        puddleMask * max(WG_ProceduralPuddlesStrength, 0.0f) * reflectionsEnabled);
    float puddleRainExposure = smoothstep(0.10f, 0.72f, rainExposure);
    float puddleWetMask = saturate(
        puddleAccumulation * wetSSRVisibility * puddleMask * puddleRainExposure * 1.12f);
    float wetMask = saturate(materialWetMask + puddleWetMask * (1.0f - materialWetMask));
    float rainAmount = saturate(WG_RainFXWeight);
    float rainImpactBaseMask = commonWetMask * materialWetGroundEligibility;
    float rainImpactVisibility =
        saturate(rainImpactBaseMask + puddleWetMask * (1.0f - rainImpactBaseMask))
        * rainAmount * smoothstep(0.05f, 0.45f, WG_Wetness);
    if (max(wetMask, rainImpactVisibility) <= 0.01f)
        return float4(sceneColor, 1.0f);

    float2 wetUV = wsPosition.xz / 1100.0f;
    float animationTime = fmod(max(WG_Time, 0.0f), 256.0f);
    float2 distortionAUV = frac(wetUV);
    float2 distortionBUV = frac(wetUV * 0.63f + float2(0.137f, 0.421f));
    float2 distortionA = TX_Distortion.SampleLevel(SS_Linear, distortionAUV, 0).xy * 2.0f - 1.0f;
    float2 distortionB = TX_Distortion.SampleLevel(SS_Linear, distortionBUV, 0).xy * 2.0f - 1.0f;
    float2 staticDistortion = distortionA + distortionB * 0.65f;
    float rainSurfaceResponse = lerp(1.10f, 2.20f, puddleMask);
    float impactDensity = rainAmount * lerp(0.64f, 1.0f, rainAmount);
    float2 impactRipple = float2(0.0f, 0.0f);
    float impactRing = 0.0f;
    float impactPulse = 0.0f;

    if (rainImpactVisibility > 0.015f)
    {
        AccumulateRainImpactLayer(
            wsPosition.xz, animationTime, 58.0f, 1.08f, impactDensity, 3.17f,
            impactRipple, impactRing, impactPulse);
        AccumulateRainImpactLayer(
            wsPosition.xz, animationTime, 41.0f, 1.46f, impactDensity * 0.98f, 11.83f,
            impactRipple, impactRing, impactPulse);
        AccumulateRainImpactLayer(
            wsPosition.xz, animationTime, 31.0f, 1.92f, impactDensity * 0.94f, 23.41f,
            impactRipple, impactRing, impactPulse);
    }
    float wetGroundRainImpactsStrength = max(WG_WetGroundRainImpactsStrength, 0.0f);
    float2 rippleDistortion = impactRipple * rainImpactVisibility * rainSurfaceResponse * wetGroundRainImpactsStrength;
    float ringVisibility = saturate(impactRing) * rainImpactVisibility * rainSurfaceResponse * wetGroundRainImpactsStrength;
    float centralImpactVisibility = saturate(impactPulse) * rainImpactVisibility * rainSurfaceResponse * wetGroundRainImpactsStrength;
    float3 viewRay = normalize(wsPosition - WG_CameraPosition);
    float puddleViewFacing = saturate(dot(-viewRay, geometricWSNormal));
    float puddleGrazing = pow(1.0f - puddleViewFacing, 2.5f);
    float puddleDistance = length(wsPosition - WG_CameraPosition);
    float puddleDistanceVisibility = smoothstep(450.0f, 2200.0f, puddleDistance);
    float puddleTransitionMask = smoothstep(0.002f, 0.42f, puddleWetMask);
    float puddleInteriorMask = smoothstep(0.12f, 0.88f, puddleWetMask);
    float slopeWaterFeather = max(0.018f, fwidth(geometricWSNormal.y) * 2.5f);
    float slopeWaterFade = smoothstep(
        0.955f - slopeWaterFeather, 0.992f + slopeWaterFeather, geometricWSNormal.y);
    slopeWaterFade = slopeWaterFade * slopeWaterFade * (3.0f - 2.0f * slopeWaterFade);
    float puddleWaterMask = puddleTransitionMask * slopeWaterFade;
    float puddleBodyMask = puddleInteriorMask * slopeWaterFade;
    float puddleCore = puddleBodyMask;
    float puddleSurfacePresence = puddleWaterMask * saturate(
        lerp(0.68f, 0.80f, puddleGrazing) + puddleDistanceVisibility * 0.020f);

    float solidGroundImpactMask = (1.0f - smoothstep(0.12f, 0.72f, puddleMask)) * centralImpactVisibility;
    float3 surfaceColor = sceneColor;
    float3 boundedImpactLift = max(1.0f - saturate(surfaceColor), 0.0f) * float3(0.075f, 0.085f, 0.095f);
    surfaceColor += boundedImpactLift * saturate(solidGroundImpactMask);
    if (reflectionsEnabled <= 0.001f)
        return float4(surfaceColor, 1.0f);

    float materialMicrostructure = lerp(0.115f, 0.045f, puddleMask);
    float2 baseNormalDistortion = float2(staticDistortion.x * 0.55f, staticDistortion.y * 0.85f);
    float3 materialWetNormal = normalize(
        wsNormal + float3(baseNormalDistortion.x, 0.0f, baseNormalDistortion.y) * materialMicrostructure);
    float3 puddlePlaneNormal = normalize(
        lerp(geometricWSNormal, float3(0.0f, 1.0f, 0.0f), 0.78f));
    float puddleNormalBlend = puddleWaterMask;
    float3 wetBaseNormal = normalize(
        lerp(materialWetNormal, puddlePlaneNormal, puddleNormalBlend));

    float ringNormalStrength = lerp(0.085f, 0.145f, puddleMask);
    float ringShapeStrength = saturate(
        ringVisibility * lerp(0.38f, 0.32f, puddleMask) +
        centralImpactVisibility * lerp(0.42f, 0.18f, puddleMask));
    float2 rainNormalDistortion = rippleDistortion * ringNormalStrength * (1.0f + ringShapeStrength);
    float3 wetNormal = normalize(
        wetBaseNormal + float3(rainNormalDistortion.x, 0.0f, rainNormalDistortion.y));

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
    for (int i = 0; i < 18; ++i)
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
            for (int refinementStep = 0; refinementStep < 4; ++refinementStep)
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
    float screenSpaceConfidence = 0.0f;

    if (hitWeight > 0.0f)
    {
        float2 reflectedUVCandidate = hitUV + reflectionRippleOffset;
        float reflectedScreenFade = CalculateReflectionScreenFade(reflectedUVCandidate);
        float2 reflectedUV = saturate(reflectedUVCandidate);
        float2 reflectedPixel = reflectedUV / WG_InvResolution;
        float reflectedWetSSRBlock = DecodeWetSSRBlock(SampleWetSSRBlockMask(reflectedPixel));

        if (reflectedWetSSRBlock <= 0.001f)
        {
            float3 directReflection = TX_Scene.SampleLevel(SS_Linear, reflectedUV, 0).rgb;
            float reflectionRoughness = lerp(0.82f, 0.25f, puddleMask);
            float2 puddleReflectionDistortion = lerp(
                baseNormalDistortion,
                reflectionRippleOffset,
                puddleNormalBlend);
            float3 roughReflection = SampleRoughReflection(
                reflectedUV,
                puddleReflectionDistortion,
                reflectionRoughness);
            float reflectedDepth = TX_Depth.SampleLevel(SS_Linear, reflectedUV, 0).r;
            float reflectedDistanceWeight = 0.0f;
            if (reflectedDepth > 1e-7f)
            {
                float3 reflectedWSPosition = ReconstructWorldPosition(reflectedDepth, reflectedUV);
                reflectedDistanceWeight = saturate(
                    length(reflectedWSPosition - WG_CameraPosition) / max(WG_FogRange, 1.0f));
            }
            float directColorWeight = smoothstep(0.32f, 0.94f, puddleMask) * 0.54f;
            directColorWeight *= lerp(
                1.0f, 0.22f, smoothstep(0.12f, 0.72f, reflectedDistanceWeight));
            reflectedColor = lerp(roughReflection, directReflection, directColorWeight);
            reflectedColor = ApplyWetGroundRainHaze(reflectedColor, reflectedDepth, reflectedUV);
            screenSpaceConfidence = saturate(hitWeight * reflectedScreenFade);
            reflectionWeight = hitWeight;
        }
    }

    float skySurfaceWeight = saturate(materialWetMask * 0.72f);
    float skyPuddleWeight = saturate(puddleWetMask * slopeWaterFade * lerp(0.35f, 1.0f, puddleMask));
    float skyReflectionWeight = max(skySurfaceWeight, skyPuddleWeight);
    float2 fallbackReflectionDistortion = lerp(
        baseNormalDistortion,
        reflectionRippleOffset,
        puddleNormalBlend);
    float3 localFallbackReflection = SampleRoughReflection(
        uv,
        fallbackReflectionDistortion,
        lerp(0.92f, 0.58f, puddleMask));
    localFallbackReflection = ApplyWetGroundRainHaze(localFallbackReflection, depth, uv);
    float validSkyFallback = step(0.001f, skyWeight);
    float2 reflectedSkyUV = saturate(
        skyUV + reflectionRippleOffset * lerp(0.18f, 0.80f, puddleMask));
    float3 skyFallbackReflection = ComposeWetGroundSky(reflectedSkyUV);
    float stableSkyMix = validSkyFallback * smoothstep(0.18f, 0.82f, skyWeight) * 0.35f;
    float3 fallbackReflection = lerp(localFallbackReflection, skyFallbackReflection, stableSkyMix);
    if (reflectionWeight > 0.0f)
    {
        float stableScreenSpaceMix = smoothstep(0.08f, 0.92f, screenSpaceConfidence);
        reflectedColor = lerp(fallbackReflection, reflectedColor, stableScreenSpaceMix);
        reflectionWeight = max(reflectionWeight, skyReflectionWeight);
    }
    else if (skyReflectionWeight > 0.0f)
    {
        reflectedColor = fallbackReflection;
        reflectionWeight = skyReflectionWeight;
    }

    if (reflectionWeight <= 0.0f)
        return float4(surfaceColor, 1.0f);

    float reflectionLuma = dot(reflectedColor, float3(0.2126f, 0.7152f, 0.0722f));
    float3 neutralWaterAmbient = lerp(
        surfaceColor * float3(0.82f, 0.86f, 0.90f),
        WG_RainFogColor,
        0.18f);
    float darkReflectionSupport = puddleWaterMask
        * (1.0f - smoothstep(0.10f, 0.34f, reflectionLuma));
    reflectedColor = lerp(
        reflectedColor,
        max(reflectedColor, neutralWaterAmbient),
        darkReflectionSupport * 0.09f);
    reflectionLuma = dot(reflectedColor, float3(0.2126f, 0.7152f, 0.0722f));
    float puddleHighlightCompression =
        max(0.0f, reflectionLuma - 0.58f) * lerp(0.14f, 0.30f, puddleMask);
    reflectedColor *= rcp(1.0f + puddleHighlightCompression);
    reflectedColor *= rcp(1.0f + max(0.0f, reflectionLuma - 0.95f) * 0.75f);

    float fresnel = pow(1.0f - saturate(dot(-viewRay, wetNormal)), 3.0f);
    float materialFresnel = lerp(0.085f, 0.28f, fresnel);
    float puddleFresnel = max(fresnel, 0.14f);
    float puddleSurfaceFresnel = puddleWaterMask * puddleGrazing;
    reflectedColor = lerp(
        reflectedColor,
        max(reflectedColor, surfaceColor * float3(0.88f, 0.92f, 0.96f)),
        puddleSurfaceFresnel * 0.075f);
    float materialReflectionBlend =
        materialWetMask * reflectionWeight * materialFresnel * WG_Strength * 2.60f;
    float puddleReflectionBlend =
        puddleWetMask * slopeWaterFade * reflectionWeight
        * lerp(0.14f, 0.28f, puddleFresnel) * WG_Strength * 5.20f
        * max(WG_PuddleReflectionsStrength, 0.0f);
    float reflectionBlend = saturate(
        materialReflectionBlend + puddleReflectionBlend * (1.0f - materialReflectionBlend));

    return float4(lerp(surfaceColor, reflectedColor, reflectionBlend), 1.0f);
}
