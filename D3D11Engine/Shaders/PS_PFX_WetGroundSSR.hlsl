// Screen-space reflections for rain-wet ground.

#include "DepthReconstruction.h"
#include "DS_Defines.h"
#include "AtmosphericScattering.h"
#include "SSR.h"

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
SamplerState SS_Cube : register(s2);
Texture2D TX_Scene : register(t0);
Texture2D TX_Depth : register(t1);
Texture2D TX_Normals : register(t2);
Texture2D TX_RainShadow : register(t3);
Texture2D TX_Distortion : register(t4);
Texture2D TX_WaterMask : register(t5);
Texture2D TX_Material : register(t6);
Texture2D TX_LowClouds : register(t7);
TextureCube TX_ReflectionCube : register(t8);

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
float3 CalculatePuddleGeometricWorldNormal(
    float3 wsPosition,
    out float geometryValidity)
{
    float3 tangentX = ddx(wsPosition);
    float3 tangentY = ddy(wsPosition);
    float tangentXLength = length(tangentX);
    float tangentYLength = length(tangentY);
    float cameraDistance = length(wsPosition - WG_CameraPosition);
    float maximumTangentLength = max(96.0f, cameraDistance * 0.040f);
    float3 geometricNormal = cross(tangentY, tangentX);
    float geometricNormalLengthSq = dot(geometricNormal, geometricNormal);
    geometryValidity = step(1e-5f, min(tangentXLength, tangentYLength))
        * step(max(tangentXLength, tangentYLength), maximumTangentLength)
        * step(1e-8f, geometricNormalLengthSq);
    if (geometryValidity <= 0.0f)
        return float3(0.0f, 1.0f, 0.0f);
    geometricNormal *= rsqrt(geometricNormalLengthSq);
    if (geometricNormal.y < 0.0f)
        geometricNormal = -geometricNormal;
    return geometricNormal;
}
bool EvaluatePuddleReceiverSample(
    float2 sampleUV,
    float3 centerWSPosition,
    float3 centerGeometricNormal,
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
    float3 surfaceDelta = sampleWSPosition - centerWSPosition;
    float tangentDistance = length(
        surfaceDelta - centerGeometricNormal * dot(surfaceDelta, centerGeometricNormal));
    float planeDeviation = abs(dot(surfaceDelta, centerGeometricNormal));
    float cameraDistance = length(centerWSPosition - WG_CameraPosition);
    float planeTolerance = max(
        8.0f,
        max(tangentDistance * 0.18f, cameraDistance * 0.0012f));
    return planeDeviation <= planeTolerance;
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
    uint shadowWidth;
    uint shadowHeight;
    TX_RainShadow.GetDimensions(shadowWidth, shadowHeight);
    float2 shadowTexelSize = 1.0f / max(float2(shadowWidth, shadowHeight), 1.0f);
    float comparisonDepth = shadowPosition.z - 0.0001f;
    float exposure = TX_RainShadow.SampleCmpLevelZero(
        SS_Comp, shadowUV, comparisonDepth) * 0.40f;
    exposure += TX_RainShadow.SampleCmpLevelZero(
        SS_Comp, shadowUV + float2(shadowTexelSize.x, 0.0f), comparisonDepth) * 0.15f;
    exposure += TX_RainShadow.SampleCmpLevelZero(
        SS_Comp, shadowUV - float2(shadowTexelSize.x, 0.0f), comparisonDepth) * 0.15f;
    exposure += TX_RainShadow.SampleCmpLevelZero(
        SS_Comp, shadowUV + float2(0.0f, shadowTexelSize.y), comparisonDepth) * 0.15f;
    exposure += TX_RainShadow.SampleCmpLevelZero(
        SS_Comp, shadowUV - float2(0.0f, shadowTexelSize.y), comparisonDepth) * 0.15f;
    return saturate(exposure);
}

float EvaluatePuddleEligibilitySample(
    float2 sampleUV,
    float3 centerWSPosition,
    float3 centerGeometricNormal,
    out float sampleValidity)
{
    float3 sampleWSPosition = centerWSPosition;
    bool validReceiverSample = EvaluatePuddleReceiverSample(
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
    // Five clamped linear samples are sufficient for the water exclusion
    // edge and avoid nine integer texture loads for every fullscreen pixel.
    float2 uv = saturate(pixelPosition * WG_InvResolution);
    float2 texel = WG_InvResolution;
    float mask = TX_WaterMask.SampleLevel(SS_Linear, uv, 0).r;
    mask = max(mask, TX_WaterMask.SampleLevel(
        SS_Linear, saturate(uv + float2(texel.x, 0.0f)), 0).r);
    mask = max(mask, TX_WaterMask.SampleLevel(
        SS_Linear, saturate(uv - float2(texel.x, 0.0f)), 0).r);
    mask = max(mask, TX_WaterMask.SampleLevel(
        SS_Linear, saturate(uv + float2(0.0f, texel.y)), 0).r);
    mask = max(mask, TX_WaterMask.SampleLevel(
        SS_Linear, saturate(uv - float2(0.0f, texel.y)), 0).r);
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
    // One world cell per layer is sufficient for this deliberately subtle
    // effect. The former 3x3 neighborhood multiplied this full-screen pass
    // by nine without materially improving the visible impact pattern.
    float2 cell = floor(worldXZ / cellSize);
    float seed = PuddleHash21(cell + float2(layerSeed, layerSeed * 1.731f));
    float cycleTime = time * cycleRate + seed;
    float cycleIndex = floor(cycleTime);
    float phase = frac(cycleTime);
    float2 cycleOffset = float2(
        cycleIndex * 19.19f + layerSeed * 2.173f,
        cycleIndex * 47.47f + layerSeed * 0.917f);
    float eventSeed = PuddleHash21(cell + cycleOffset + float2(13.17f, 47.53f));
    float eventMask = step(1.0f - density, eventSeed);
    [branch]
    if (eventMask <= 0.001f)
        return;

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

float EvaluatePuddleSurfaceSupport(
    float3 wsPosition,
    float3 geometricNormal,
    float2 sampleUV,
    out float sampleValidity)
{
    float3 sampleWSPosition = wsPosition;
    bool validReceiverSample = EvaluatePuddleReceiverSample(
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
struct WetGroundReflectionTrace
{
    float3 Color;
    float Weight;
};
static const int WETGROUND_MATERIAL_SSR_TRACE_STEPS = 16;
static const int PUDDLE_SSR_TRACE_STEPS = 48;
WetGroundReflectionTrace TraceWetGroundReflection(
    float activeMask,
    int maxTraceSteps,
    float3 wsPosition,
    float3 reflectionNormal,
    float3 viewRay,
    float2 uv,
    float depth,
    float3 surfaceColor,
    float2 hitUVOffset,
    float2 roughDistortion,
    float hitRoughness,
    float fallbackRoughness,
    float directColorWeightBase,
    float skyReflectionWeight,
    float2 skyUVOffset,
    float fallbackVisibilityWeight,
    float puddleSkyLeakSuppression)
{
    WetGroundReflectionTrace result;
    result.Color = surfaceColor;
    result.Weight = 0.0f;
    if (activeMask <= 0.001f)
        return result;

    float3 rayDirection = normalize(reflect(viewRay, reflectionNormal));
    if (rayDirection.y <= 0.015f)
        return result;

    float2 viewportSize = 1.0f / max(WG_InvResolution, float2(1e-6f, 1e-6f));
    float traceRoughness = saturate(hitRoughness);
    float maxTraceDistance = lerp(3000.0f, 1800.0f, traceRoughness);
    float traceThickness = lerp(22.0f, 36.0f, traceRoughness);
    SSRTraceResult trace = SSRCore_TraceWorldRay(
        TX_Depth,
        wsPosition + reflectionNormal * 4.0f,
        rayDirection,
        WG_ViewProj,
        viewportSize,
        WG_ProjParams.z,
        WG_ProjParams.w,
        maxTraceDistance,
        maxTraceSteps,
        5,
        6.0f,
        traceThickness,
        0.10f);

    float3 localFallbackReflection = SampleRoughReflection(
        uv,
        roughDistortion,
        fallbackRoughness);
    localFallbackReflection = ApplyWetGroundRainHaze(localFallbackReflection, depth, uv);

    float validSkyFallback = step(0.001f, trace.skyWeight);
    float2 reflectedSkyUV = saturate(trace.skyUV + skyUVOffset);
    float3 skyFallbackReflection = ComposeWetGroundSky(reflectedSkyUV);
    float skyGeometryProximity = 0.0f;
    if (validSkyFallback > 0.0f && puddleSkyLeakSuppression > 0.0f)
    {
        float2 skyProbeOffset = WG_InvResolution * 5.0f;
        const float2 skyProbeDirections[8] =
        {
            float2(1.0f, 0.0f),
            float2(-1.0f, 0.0f),
            float2(0.0f, 1.0f),
            float2(0.0f, -1.0f),
            float2(0.7071f, 0.7071f),
            float2(-0.7071f, 0.7071f),
            float2(0.7071f, -0.7071f),
            float2(-0.7071f, -0.7071f)
        };
        float geometryProbeCount = 0.0f;
        [unroll]
        for (int skyProbeIndex = 0; skyProbeIndex < 8; ++skyProbeIndex)
        {
            float2 skyProbeUV = reflectedSkyUV
                + skyProbeDirections[skyProbeIndex] * skyProbeOffset;
            float probeInsideScreen = step(0.0f, skyProbeUV.x)
                * step(skyProbeUV.x, 1.0f)
                * step(0.0f, skyProbeUV.y)
                * step(skyProbeUV.y, 1.0f);
            float probeDepth = TX_Depth.SampleLevel(
                SS_Linear, saturate(skyProbeUV), 0).r;
            geometryProbeCount += probeInsideScreen * step(1e-7f, probeDepth);
        }
        skyGeometryProximity = smoothstep(1.0f, 4.0f, geometryProbeCount);
    }
    float skyLeakAttenuation = 1.0f
        - skyGeometryProximity * saturate(puddleSkyLeakSuppression) * 0.88f;
    float stableSkyMix = validSkyFallback
        * smoothstep(0.18f, 0.82f, trace.skyWeight)
        * saturate(skyReflectionWeight)
        * skyLeakAttenuation;
    float3 fallbackReflection = lerp(
        localFallbackReflection,
        skyFallbackReflection,
        stableSkyMix);

    float stableFallbackWeight = saturate(fallbackVisibilityWeight);
    float screenSpaceConfidence = 0.0f;
    float3 screenSpaceReflection = fallbackReflection;
    if (trace.hit > 0.5f)
    {
        float2 reflectedUVCandidate = trace.hitUV + hitUVOffset;
        float reflectedScreenFade = SSRCore_CalculateEdgeFade(reflectedUVCandidate, 0.10f);
        float2 reflectedUV = saturate(reflectedUVCandidate);
        float2 reflectedPixel = reflectedUV * viewportSize;
        float reflectedWetSSRBlock = DecodeWetSSRBlock(
            SampleWetSSRBlockMask(reflectedPixel));
        if (reflectedWetSSRBlock <= 0.001f)
        {
            float3 directReflection = TX_Scene.SampleLevel(SS_Linear, reflectedUV, 0).rgb;
            float3 roughReflection = SampleRoughReflection(
                reflectedUV,
                roughDistortion,
                hitRoughness);
            float reflectedDepth = TX_Depth.SampleLevel(SS_Linear, reflectedUV, 0).r;
            float reflectedDistanceWeight = 0.0f;
            if (reflectedDepth > 1e-7f)
            {
                float3 reflectedWSPosition = ReconstructWorldPosition(
                    reflectedDepth,
                    reflectedUV);
                reflectedDistanceWeight = saturate(
                    length(reflectedWSPosition - WG_CameraPosition)
                    / max(WG_FogRange, 1.0f));
            }
            float directColorWeight = directColorWeightBase * lerp(
                1.0f,
                0.22f,
                smoothstep(0.12f, 0.72f, reflectedDistanceWeight));
            screenSpaceReflection = lerp(
                roughReflection,
                directReflection,
                directColorWeight);
            screenSpaceReflection = ApplyWetGroundRainHaze(
                screenSpaceReflection,
                reflectedDepth,
                reflectedUV);
            screenSpaceConfidence = saturate(
                trace.confidence * reflectedScreenFade);
        }
    }

    if (stableFallbackWeight > 0.0f)
    {
        float missingScreenReflection = 1.0f - screenSpaceConfidence;
        float downwardView = smoothstep(0.18f, 0.82f, saturate(-viewRay.y));
        float3 rawCubeReflection = max(
            TX_ReflectionCube.SampleLevel(SS_Cube, -rayDirection, 1.5f).rgb,
            0.0f);
        float3 cubeLumaWeights = float3(0.2126f, 0.7152f, 0.0722f);
        float rawCubeLumaUnclamped = dot(rawCubeReflection, cubeLumaWeights);
        float cubeResourceAvailable = step(0.0001f, rawCubeLumaUnclamped);
        float rawCubeLuma = max(rawCubeLumaUnclamped, 0.0001f);
        float surfaceLuma = max(dot(surfaceColor, cubeLumaWeights), 0.0001f);
        float cubeLumaLimit = surfaceLuma * 1.30f + 0.018f;
        float3 limitedCubeReflection = rawCubeReflection
            * min(1.0f, cubeLumaLimit / rawCubeLuma);
        float limitedCubeLuma = dot(limitedCubeReflection, cubeLumaWeights);
        float3 desaturatedCubeReflection = lerp(
            limitedCubeLuma.xxx,
            limitedCubeReflection,
            0.34f);
        float3 atmosphericCubeReflection = lerp(
            desaturatedCubeReflection,
            max(desaturatedCubeReflection, WG_RainFogColor * 0.42f),
            saturate(WG_RainFXWeight) * 0.22f);
        float cubeFallbackMix = stableFallbackWeight
            * missingScreenReflection
            * cubeResourceAvailable
            * lerp(0.12f, 0.30f, downwardView);
        fallbackReflection = lerp(
            fallbackReflection,
            atmosphericCubeReflection,
            cubeFallbackMix);
    }

    float stableScreenSpaceMix = smoothstep(
        0.04f,
        0.92f,
        screenSpaceConfidence);
    result.Color = lerp(
        fallbackReflection,
        screenSpaceReflection,
        stableScreenSpaceMix);
    result.Weight = max(
        max(screenSpaceConfidence, stableSkyMix),
        stableFallbackWeight);
    return result;
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

    float materialWetGroundSSRStrength = saturate(
        TX_Material.SampleLevel(SS_Linear, uv, 0).z);
    float materialWetGroundEligibility = step(0.0001f, materialWetGroundSSRStrength);
    float reflectionsEnabled = step(0.5f, WG_ReflectionsEnabled) * step(0.001f, WG_Strength);

    // Rain impacts remain visible when Rain effects are disabled, but they
    // must not keep the complete puddle/SSR analysis alive. In this mode the
    // original result consists only of the small central impact lift on
    // eligible, rain-exposed ground pixels, so evaluate exactly that part and
    // return before all puddle geometry, noise and reflection work.
    if (reflectionsEnabled <= 0.001f && WG_ProceduralPuddlesStrength <= 0.001f)
    {
        float upwardMask = smoothstep(0.38f, 0.82f, sourceWSNormal.y);
        float rainExposure = GetRainExposure(wsPosition);
        float wetness = saturate(WG_Wetness);
        float commonWetMask = upwardMask * rainExposure * wetness * wetSSRVisibility;
        float rainAmount = saturate(WG_RainFXWeight);
        float rainImpactVisibility = commonWetMask * materialWetGroundEligibility
            * rainAmount * smoothstep(0.05f, 0.45f, WG_Wetness);
        if (rainImpactVisibility <= 0.01f)
            return float4(sceneColor, 1.0f);

        float2 wetUV = wsPosition.xz / 1100.0f;
        float animationTime = fmod(max(WG_Time, 0.0f), 256.0f);
        float impactDensity = rainAmount * lerp(0.64f, 1.0f, rainAmount);
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

        float centralImpactVisibility = saturate(impactPulse)
            * rainImpactVisibility * 1.10f * max(WG_WetGroundRainImpactsStrength, 0.0f);
        float rainImpactNightAmount = GetAmbientNightWeight();
        float rainImpactBrightness = lerp(1.0f, 0.40f, rainImpactNightAmount);
        float3 boundedImpactLift = max(1.0f - saturate(sceneColor), 0.0f)
            * float3(0.075f, 0.085f, 0.095f)
            * rainImpactBrightness;
        float3 impactColor = sceneColor + boundedImpactLift * centralImpactVisibility;
        return float4(impactColor, 1.0f);
    }

    float3 geometricWSNormal = CalculateGeometricWorldNormal(
        uv, wsPosition, sourceWSNormal);
    float puddleGeometryValidity = 0.0f;
    float3 puddleGeometricWSNormal = CalculatePuddleGeometricWorldNormal(
        wsPosition, puddleGeometryValidity);

    // The Rain effects switch controls both puddles and the reflection layer
    // of the surrounding wet material.
    float materialPuddleEligibility = materialWetGroundEligibility;

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
        wsPosition, puddleGeometricWSNormal.y, puddleAccumulation);
    float puddleSurfaceSupport = CalculatePuddleSurfaceSupport(
        wsPosition, puddleGeometricWSNormal, uv);
    float puddleBoundaryMask = CalculatePuddleBoundaryMask(
        uv, wsPosition, puddleGeometricWSNormal);
    float puddleSupportFade = lerp(0.82f, 1.0f, smoothstep(0.05f, 0.95f, puddleSurfaceSupport));
    puddleMask *= puddleSupportFade
        * puddleBoundaryMask
        * materialPuddleEligibility
        * puddleGeometryValidity;
    float materialWetStrength = materialWetGroundSSRStrength * lerp(0.72f, 1.0f, wetness);
    float materialWetMask = commonWetMask * materialWetStrength
        * max(WG_WetMaterialReflectionsStrength, 0.0f)
        * reflectionsEnabled;
    puddleMask = saturate(
        puddleMask * max(WG_ProceduralPuddlesStrength, 0.0f));
    float puddleRainExposure = smoothstep(0.10f, 0.72f, rainExposure);
    float puddleExposure = puddleRainExposure;
    float puddleWetMask = saturate(
        puddleAccumulation * wetSSRVisibility * puddleMask * puddleExposure * 1.12f);
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
    float puddleViewFacing = saturate(dot(-viewRay, puddleGeometricWSNormal));
    float puddleGrazing = pow(1.0f - puddleViewFacing, 2.5f);
    float puddleDistance = length(wsPosition - WG_CameraPosition);
    float puddleDistanceVisibility = smoothstep(450.0f, 2200.0f, puddleDistance);
    float puddleTransitionMask = smoothstep(0.002f, 0.42f, puddleWetMask);
    float puddleInteriorMask = smoothstep(0.12f, 0.88f, puddleWetMask);
    float slopeWaterFeather = max(0.018f, fwidth(puddleGeometricWSNormal.y) * 2.5f);
    float slopeWaterFade = smoothstep(
        0.955f - slopeWaterFeather, 0.992f + slopeWaterFeather, puddleGeometricWSNormal.y)
        * puddleGeometryValidity;
    slopeWaterFade = slopeWaterFade * slopeWaterFade * (3.0f - 2.0f * slopeWaterFade);
    float puddleWaterMask = puddleTransitionMask * slopeWaterFade;
    float puddleBodyMask = puddleInteriorMask * slopeWaterFade;
    float puddleCore = puddleBodyMask;
    float puddleSurfacePresence = puddleWaterMask * saturate(
        lerp(0.68f, 0.80f, puddleGrazing) + puddleDistanceVisibility * 0.020f);

    float solidGroundImpactMask = (1.0f - smoothstep(0.12f, 0.72f, puddleMask)) * centralImpactVisibility;
    float3 surfaceColor = sceneColor;
    float rainImpactNightAmount = GetAmbientNightWeight();
    float rainImpactBrightness = lerp(1.0f, 0.40f, rainImpactNightAmount);
    float3 boundedImpactLift = max(1.0f - saturate(surfaceColor), 0.0f)
        * float3(0.075f, 0.085f, 0.095f)
        * rainImpactBrightness;
    surfaceColor += boundedImpactLift * saturate(solidGroundImpactMask);

    float puddleBasePresence = saturate(
        puddleWaterMask * lerp(0.62f, 1.0f, puddleBodyMask));
    float puddleBaseLuma = dot(
        surfaceColor, float3(0.2126f, 0.7152f, 0.0722f));
    float3 warmPuddleEarthTone = float3(
        puddleBaseLuma * 0.70f,
        puddleBaseLuma * 0.55f,
        puddleBaseLuma * 0.38f);
    float3 coolPuddleEarthTone = ApplyAmbientNightTint(
        warmPuddleEarthTone, 0.85f);
    float3 puddleEarthTone = lerp(
        warmPuddleEarthTone,
        coolPuddleEarthTone,
        rainImpactNightAmount);
    float3 warmPuddleBase = surfaceColor * float3(0.80f, 0.74f, 0.66f);
    float3 coolPuddleBase = ApplyAmbientNightTint(
        warmPuddleBase, 0.70f);
    float3 darkenedPuddleBase = lerp(
        warmPuddleBase,
        coolPuddleBase,
        rainImpactNightAmount);
    float3 turbidPuddleBase = lerp(
        darkenedPuddleBase,
        puddleEarthTone,
        0.26f);
    float puddleBaseBlend = puddleBasePresence
        * lerp(0.14f, 0.24f, puddleBodyMask);
    surfaceColor = lerp(
        surfaceColor,
        turbidPuddleBase,
        puddleBaseBlend);

    float materialMicrostructure = lerp(0.115f, 0.045f, puddleMask);
    float2 baseNormalDistortion = float2(staticDistortion.x * 0.55f, staticDistortion.y * 0.85f);
    float3 materialWetNormal = normalize(
        wsNormal + float3(baseNormalDistortion.x, 0.0f, baseNormalDistortion.y) * materialMicrostructure);
    float3 puddlePlaneNormal = normalize(
        lerp(puddleGeometricWSNormal, float3(0.0f, 1.0f, 0.0f), 0.78f));
    float ringNormalStrength = lerp(0.085f, 0.145f, puddleMask);
    float ringShapeStrength = saturate(
        ringVisibility * lerp(0.38f, 0.32f, puddleMask) +
        centralImpactVisibility * lerp(0.42f, 0.18f, puddleMask));
    float2 rainNormalDistortion = rippleDistortion * ringNormalStrength * (1.0f + ringShapeStrength);
    float3 puddleWetNormal = normalize(
        puddlePlaneNormal + float3(rainNormalDistortion.x, 0.0f, rainNormalDistortion.y));
    float2 reflectionRippleOffset = rippleDistortion * float2(0.0040f, 0.0040f);
    float materialTraceMask = saturate(materialWetMask);
    float puddleTraceMask = saturate(puddleWetMask * slopeWaterFade);
    float3 composedColor = surfaceColor;
    if (reflectionsEnabled > 0.001f)
    {
        WetGroundReflectionTrace materialReflection = TraceWetGroundReflection(
            materialTraceMask,
            WETGROUND_MATERIAL_SSR_TRACE_STEPS,
            wsPosition,
            materialWetNormal,
            viewRay,
            uv,
            depth,
            surfaceColor,
            float2(0.0f, 0.0f),
            baseNormalDistortion,
            0.82f,
            0.92f,
            0.0f,
            saturate(materialWetMask * 0.72f),
            float2(0.0f, 0.0f),
            0.0f,
            0.0f);
        float materialFresnelBase = pow(
            1.0f - saturate(dot(-viewRay, materialWetNormal)), 3.0f);
        float materialFresnel = lerp(0.085f, 0.28f, materialFresnelBase);
        float materialReflectionBlend = saturate(
            materialWetMask * materialReflection.Weight
            * materialFresnel * WG_Strength * 2.60f);
        composedColor = lerp(
            composedColor,
            materialReflection.Color,
            materialReflectionBlend);
    }
    float puddleDirectColorWeight = smoothstep(0.32f, 0.94f, puddleMask) * 0.42f;
    WetGroundReflectionTrace puddleReflection = TraceWetGroundReflection(
        puddleTraceMask,
        PUDDLE_SSR_TRACE_STEPS,
        wsPosition,
        puddleWetNormal,
        viewRay,
        uv,
        depth,
        surfaceColor,
        reflectionRippleOffset,
        reflectionRippleOffset,
        0.36f,
        0.68f,
        puddleDirectColorWeight,
        saturate(puddleWetMask * slopeWaterFade * lerp(0.35f, 1.0f, puddleMask)),
        reflectionRippleOffset * lerp(0.18f, 0.80f, puddleMask),
        1.0f,
        1.0f);
    float puddleFresnelBase = pow(
        1.0f - saturate(dot(-viewRay, puddleWetNormal)), 3.0f);
    float puddleFresnel = max(puddleFresnelBase, 0.14f);
    float3 puddleReflectedColor = puddleReflection.Color;
    float puddleContaminationNoise = PuddleValueNoise(
        wsPosition.xz * 0.0055f + float2(191.73f, 73.19f));
    float puddleSedimentNoise = PuddleValueNoise(
        wsPosition.xz * 0.0120f + float2(37.11f, 241.57f));
    float puddleContamination = saturate(
        0.40f
        + (puddleContaminationNoise - 0.5f) * 0.12f
        + (puddleSedimentNoise - 0.5f) * 0.04f);
    float3 groundLumaWeights = float3(0.2126f, 0.7152f, 0.0722f);
    float surfaceLuma = dot(surfaceColor, groundLumaWeights);
    float reflectedLuma = dot(puddleReflectedColor, groundLumaWeights);
    float3 warmReflectionTone = float3(
        reflectedLuma * 1.02f,
        reflectedLuma * 0.78f,
        reflectedLuma * 0.54f);
    float3 coolReflectionTone = ApplyAmbientNightTint(
        warmReflectionTone, 0.90f);
    float3 earthyReflectionTone = lerp(
        warmReflectionTone,
        coolReflectionTone,
        rainImpactNightAmount);
    float3 warmGroundTone = float3(
        surfaceLuma * 0.82f,
        surfaceLuma * 0.68f,
        surfaceLuma * 0.49f);
    float3 coolGroundTone = ApplyAmbientNightTint(
        warmGroundTone, 0.80f);
    float3 earthyGroundColor = lerp(
        surfaceColor,
        lerp(warmGroundTone, coolGroundTone, rainImpactNightAmount),
        0.24f);
    float warmBrownTint = lerp(0.16f, 0.23f, puddleContamination);
    float uniformBrownTint = lerp(
        warmBrownTint,
        warmBrownTint * 0.22f,
        rainImpactNightAmount);
    puddleReflectedColor = lerp(
        puddleReflectedColor,
        earthyReflectionTone,
        uniformBrownTint);
    puddleReflectedColor *= lerp(0.92f, 0.84f, puddleContamination);
    puddleReflectedColor = lerp(
        puddleReflectedColor,
        earthyGroundColor * float3(0.76f, 0.68f, 0.56f),
        lerp(0.07f, 0.12f, puddleContamination) * puddleBodyMask);
    float puddleReflectionLuma = dot(
        puddleReflectedColor, groundLumaWeights);
    float3 neutralWaterAmbient = lerp(
        earthyGroundColor * float3(0.76f, 0.68f, 0.56f),
        WG_RainFogColor * float3(0.74f, 0.70f, 0.64f),
        0.08f);
    float darkReflectionSupport = puddleWaterMask
        * (1.0f - smoothstep(0.08f, 0.28f, puddleReflectionLuma));
    puddleReflectedColor = lerp(
        puddleReflectedColor,
        max(puddleReflectedColor, neutralWaterAmbient),
        darkReflectionSupport * 0.045f);
    puddleReflectionLuma = dot(
        puddleReflectedColor, groundLumaWeights);
    float puddleHighlightCompression =
        max(0.0f, puddleReflectionLuma - 0.50f)
        * lerp(0.22f, 0.40f, puddleMask);
    puddleReflectedColor *= rcp(1.0f + puddleHighlightCompression);
    puddleReflectedColor *= rcp(
        1.0f + max(0.0f, puddleReflectionLuma - 0.86f) * 0.82f);
    float puddleSurfaceFresnel = puddleWaterMask * puddleGrazing;
    puddleReflectedColor = lerp(
        puddleReflectedColor,
        max(puddleReflectedColor, earthyGroundColor * float3(0.82f, 0.76f, 0.68f)),
        puddleSurfaceFresnel * 0.040f);
    float puddleReflectionBlend = saturate(
        puddleWetMask * slopeWaterFade * puddleReflection.Weight
        * lerp(0.11f, 0.22f, puddleFresnel) * WG_Strength * 4.10f
        * max(WG_PuddleReflectionsStrength, 0.0f));
    composedColor = lerp(
        composedColor,
        puddleReflectedColor,
        puddleReflectionBlend);
    return float4(composedColor, 1.0f);
}
