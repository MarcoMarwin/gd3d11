#include <AtmosphericScattering.h>
#include <FFFog.h>
#include <DS_Defines.h>
#include <SSR.h>

static const float DIST_SMALL_SPEED = -0.01f;
static const float DIST_SMALL_AMOUNT = 0.01f;
static const float DIST_SMALL_SCALE = 0.3f;
static const float DIST_BIG_SCALE = 0.1f;
static const float DIST_BIG_SPEED = -0.005f;

#define CleanRefraction(uv, screen_uv, depthRef) (lerp(uv, screen_uv, saturate(Input.vTexcoord2.x - depthRef)))

cbuffer RefractionInfo : register(b2)
{
    float4x4 RI_Projection;
    float2 RI_ViewportSize;
    float RI_Time;
    float RI_Pad1;
    float3 RI_CameraPosition;
    float RI_Pad2;
    float4x4 RI_ViewProj;
};

cbuffer WaterMaterialInfo : register(b3)
{
    float WM_PaddingLegacy0;
    float WM_DisableRainEffects;
    float WM_OceanWaterTintStrength;
    float WM_IsOceanWater;
    float3 WM_OceanWaterTint;
    float WM_Padding0;
    float WM_CameraUnderwater;
    float3 WM_Padding1;
};

float LinearizeWaterDepth(float d)
{
    if (d <= 0.000001f) return 1000000.0f;
    float q = d - RI_Projection._33;
    if (abs(q) <= 0.000001f) return 1000000.0f;
    return RI_Projection._43 / q;
}

SamplerState SS_Linear : register(s0);
Texture2D TX_Diffuse : register(t0);
Texture2D TX_Depth : register(t2);
TextureCube TX_ReflectionCube : register(t3);
Texture2D TX_Distortion : register(t4);
Texture2D TX_Scene : register(t5);
Texture2D TX_LowClouds : register(t6);

void AccumulateSkyBackgroundSample(float2 uv, inout float3 sum, inout float w)
{
    if (any(uv < 0) || any(uv > 1)) return;
    if (TX_Depth.SampleLevel(SS_Linear, uv, 0).r > 0.000001f) return;
    sum += TX_Scene.SampleLevel(SS_Linear, uv, 0).rgb;
    w += 1;
}

float3 SampleSkyWithoutCelestialBodies(float2 uv, float3 ray, float3 fallback)
{
    float3 cur = TX_Scene.SampleLevel(SS_Linear, uv, 0).rgb;
    float3 r = normalize(ray);
    // Remove the celestial discs continuously as their engine visibility rises.
    // A binary step caused the reflected sky contribution to switch abruptly at
    // sunrise/moonrise while the separate water glints were still active.
    float sunDiscVisibility = smoothstep(0.0f, 0.08f, saturate(AC_SunVisibility));
    float moonDiscVisibility = smoothstep(0.0f, 0.08f, saturate(AC_MoonVisibility));
    float sm = smoothstep(0.9962f, 0.9988f, dot(r, normalize(AC_LightPos))) * sunDiscVisibility;
    float mm = smoothstep(0.9925f, 0.9982f, dot(r, normalize(AC_MoonPos))) * moonDiscVisibility;
    float m = saturate(max(sm, mm));
    if (m <= 0.0001f) return cur;

    float2 p = 1.0f / RI_ViewportSize;
    float2 n = p * 64;
    float2 f = p * 128;
    float3 sum = 0;
    float w = 0;

    AccumulateSkyBackgroundSample(uv + float2(n.x, 0), sum, w);
    AccumulateSkyBackgroundSample(uv - float2(n.x, 0), sum, w);
    AccumulateSkyBackgroundSample(uv + float2(0, n.y), sum, w);
    AccumulateSkyBackgroundSample(uv - float2(0, n.y), sum, w);
    AccumulateSkyBackgroundSample(uv + f, sum, w);
    AccumulateSkyBackgroundSample(uv - f, sum, w);
    AccumulateSkyBackgroundSample(uv + float2(f.x, -f.y), sum, w);
    AccumulateSkyBackgroundSample(uv + float2(-f.x, f.y), sum, w);

    return lerp(cur, w > 0.5f ? sum / w : fallback, m);
}

float3 ProjectCelestial(float3 direction)
{
    float3 worldPoint = RI_CameraPosition + normalize(direction) * 100000.0f;
    float4 p = mul(float4(worldPoint, 1), RI_ViewProj);
    float projectionValid = smoothstep(0.001f, 0.02f, p.w);
    p.xyz /= max(p.w, 0.001f);
    float2 uv = p.xy * float2(0.5f, -0.5f) + 0.5f;

    // Fade cloud sampling in across a small viewport border instead of changing
    // from completely unoccluded to fully cloud-occluded in a single frame.
    float2 viewportEdgeDistance = min(uv, 1.0f - uv);
    float viewportValid = smoothstep(
        -0.025f,
        0.025f,
        min(viewportEdgeDistance.x, viewportEdgeDistance.y));
    float valid = projectionValid * viewportValid;
    return float3(saturate(uv), valid);
}

float WaterRainImpactHash21(float2 p)
{
    p = frac(p * float2(123.34f, 456.21f));
    p += dot(p, p + 45.32f);
    return frac(p.x * p.y);
}

void AccumulateWaterRainImpactLayer(
    float2 worldXZ, float time, float cellSize, float cycleRate, float density, float layerSeed,
    inout float2 rippleVector, inout float ringMask, inout float impactMask)
{
    float2 baseCell = floor(worldXZ / cellSize);

    [unroll] for (int y = -1; y <= 1; ++y)
    {
        [unroll] for (int x = -1; x <= 1; ++x)
        {
            float2 cell = baseCell + float2((float)x, (float)y);
            float seed = WaterRainImpactHash21(
                cell + float2(layerSeed, layerSeed * 1.731f));

            float cycleTime = time * cycleRate + seed;
            float cycleIndex = floor(cycleTime);
            float phase = frac(cycleTime);
            float2 cycleOffset = float2(
                cycleIndex * 19.19f + layerSeed * 2.173f,
                cycleIndex * 47.47f + layerSeed * 0.917f);

            float eventSeed = WaterRainImpactHash21(
                cell + cycleOffset + float2(13.17f, 47.53f));
            float eventMask = step(1.0f - density, eventSeed);

            float2 pointJitter = float2(
                WaterRainImpactHash21(
                    cell + cycleOffset + float2(layerSeed + 5.31f, layerSeed + 19.73f)),
                WaterRainImpactHash21(
                    cell + cycleOffset.yx + float2(layerSeed + 31.91f, layerSeed + 7.57f)));
            float2 impactPosition = (cell + 0.15f + pointJitter * 0.70f) * cellSize;

            float2 delta = worldXZ - impactPosition;
            float distanceToImpact = length(delta);
            float2 radialDirection = delta / max(distanceToImpact, 0.001f);

            float radiusVariation = lerp(
                0.82f, 1.12f,
                WaterRainImpactHash21(
                    cell + cycleOffset + float2(layerSeed + 71.11f, layerSeed + 3.29f)));
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

            ringMask = max(
                ringMask, activePrimaryRing + activeSecondaryRing * 0.38f);
            impactMask = max(
                impactMask, activeCentralImpact);
        }
    }
}

float2 CalculateWaterRainNormalDistortion(
    float3 worldPosition, float rainAmount, float horizontalWaterMask, float waterTopSide)
{
    float ringVisibility = saturate(rainAmount) * saturate(horizontalWaterMask) * saturate(waterTopSide);
    if (ringVisibility <= 0.001f)
        return float2(0.0f, 0.0f);

    float animationTime = fmod(max(RI_Time, 0.0f), 256.0f);
    float impactDensity = rainAmount * lerp(0.58f, 1.0f, rainAmount);
    float heavyRainExtraSetWeight = smoothstep(0.45f, 1.0f, rainAmount);
    float2 impactRipple = float2(0.0f, 0.0f);
    float impactRing = 0.0f;
    float impactPulse = 0.0f;

    AccumulateWaterRainImpactLayer(
        worldPosition.xz, animationTime, 58.0f, 1.08f, impactDensity, 3.17f,
        impactRipple, impactRing, impactPulse);
    AccumulateWaterRainImpactLayer(
        worldPosition.xz, animationTime, 41.0f, 1.46f, impactDensity * 0.98f, 11.83f,
        impactRipple, impactRing, impactPulse);
    AccumulateWaterRainImpactLayer(
        worldPosition.xz, animationTime, 31.0f, 1.92f, impactDensity * 0.94f, 23.41f,
        impactRipple, impactRing, impactPulse);

    float2 extraRippleA = float2(0.0f, 0.0f);
    float extraRingA = 0.0f;
    float extraPulseA = 0.0f;
    AccumulateWaterRainImpactLayer(
        worldPosition.xz + float2(17.41f, 53.27f), animationTime, 58.0f, 1.08f, impactDensity, 37.19f,
        extraRippleA, extraRingA, extraPulseA);
    AccumulateWaterRainImpactLayer(
        worldPosition.xz + float2(61.73f, 29.11f), animationTime, 41.0f, 1.46f, impactDensity * 0.98f, 47.83f,
        extraRippleA, extraRingA, extraPulseA);
    AccumulateWaterRainImpactLayer(
        worldPosition.xz + float2(43.37f, 71.59f), animationTime, 31.0f, 1.92f, impactDensity * 0.94f, 59.41f,
        extraRippleA, extraRingA, extraPulseA);

    float2 extraRippleB = float2(0.0f, 0.0f);
    float extraRingB = 0.0f;
    float extraPulseB = 0.0f;
    AccumulateWaterRainImpactLayer(
        worldPosition.xz + float2(83.13f, 19.67f), animationTime, 58.0f, 1.08f, impactDensity, 67.31f,
        extraRippleB, extraRingB, extraPulseB);
    AccumulateWaterRainImpactLayer(
        worldPosition.xz + float2(27.89f, 97.43f), animationTime, 41.0f, 1.46f, impactDensity * 0.98f, 79.53f,
        extraRippleB, extraRingB, extraPulseB);
    AccumulateWaterRainImpactLayer(
        worldPosition.xz + float2(109.21f, 41.17f), animationTime, 31.0f, 1.92f, impactDensity * 0.94f, 91.79f,
        extraRippleB, extraRingB, extraPulseB);

    impactRipple += (extraRippleA + extraRippleB) * heavyRainExtraSetWeight;
    impactRing = max(impactRing, max(extraRingA, extraRingB) * heavyRainExtraSetWeight);
    impactPulse = max(impactPulse, max(extraPulseA, extraPulseB) * heavyRainExtraSetWeight);

    const float waterRainResponse = 3.40f;
    const float waterRingNormalStrength = 0.230f;
    float visibleRing = saturate(impactRing) * ringVisibility * waterRainResponse;
    float visibleImpact = saturate(impactPulse) * ringVisibility * waterRainResponse;
    float ringShapeStrength = saturate(
        visibleRing * 0.32f + visibleImpact * 0.18f);

    return impactRipple * ringVisibility * waterRainResponse * waterRingNormalStrength * (1.0f + ringShapeStrength);
}

struct PS_INPUT
{
    float2 vTexcoord : TEXCOORD0;
    float2 vTexcoord2 : TEXCOORD1;
    float4 vDiffuse : TEXCOORD2;
    float3 vNormalWS : TEXCOORD4;
    float3 vWorldPosition : TEXCOORD5;
    float4 vPosition : SV_POSITION;
};

struct PS_OUTPUT
{
    float4 color : SV_TARGET0;
    float waterMask : SV_TARGET1;
    float fsr3ReactiveMask : SV_TARGET2;
};

PS_OUTPUT PSMain(PS_INPUT Input)
{
    PS_OUTPUT o;
    float2 screenUV = Input.vPosition.xy / RI_ViewportSize;
    float surfaceViewZ = Input.vTexcoord2.x;
    float rawCenterDepth = TX_Depth.Sample(SS_Linear, screenUV).r;
    float sceneViewZ = LinearizeWaterDepth(rawCenterDepth);
    float viewRayScale = clamp(abs(Input.vTexcoord2.y) / max(abs(surfaceViewZ), 1), 1, 8);
    float waterThickness = clamp(max(sceneViewZ - surfaceViewZ, 0) * viewRayScale, 0, 6000);
    float shoreD = max(fwidth(waterThickness), 1);
    float shoreEnd = clamp(max(65.0f, shoreD * 1.25f), 65, 160);
    float shoreVisibility = SmootherStep01(saturate((waterThickness - 1) / max(shoreEnd - 1, 1)));

    float3 viewDirection = normalize(Input.vWorldPosition - RI_CameraPosition);
    float waterViewDistance = abs(Input.vTexcoord2.y);
    float cameraBelowSurface = step(0.5f, WM_CameraUnderwater) * step(0.5f, WM_IsOceanWater);
    float waterTopSide = 1.0f - cameraBelowSurface;
    float waterRainTopSide = 1.0f - step(0.5f, WM_CameraUnderwater);
    float materialAllows = 1.0f;
    float ssrEnabled = step(0.5f, AC_EnableSSR) * materialAllows;
    float rainAmount = saturate(AC_RainFXWeight) * (1 - step(0.5f, WM_DisableRainEffects));
    float nightAmount = saturate((-AC_LightPos.y + 0.12f) * 2.2f);
    float ssrStrength = max(0, AC_SSRStrength) * ssrEnabled;
    float cubeStrength = lerp(0.34f, max(0.30f, saturate(ssrStrength * 0.82f)), ssrEnabled) * waterTopSide;

    float waterGeometryUp = abs(normalize(Input.vNormalWS).y);

    // Surface inclination relative to the horizontal plane:
    // 0-39 degrees: full reflections.
    // 39-50 degrees: smooth fade.
    // 50-90 degrees: no reflections.
    const float waterfallSsrOffCos = 0.64278761f;  // cos(50 degrees)
    const float waterfallSsrFullCos = 0.77714596f; // cos(39 degrees)

    float steepWaterSsrFactor = smoothstep(
        waterfallSsrOffCos,
        waterfallSsrFullCos,
        waterGeometryUp);

    float waterReflectionSuppress = lerp(0.12f, 1.0f, steepWaterSsrFactor);
    ssrStrength *= lerp(0.45f, 1.0f, steepWaterSsrFactor);
    cubeStrength *= lerp(0.70f, 1.0f, steepWaterSsrFactor);

    const float waterRainRingOffCos = 0.93969262f;
    const float waterRainRingFullCos = 0.98480775f;
    float horizontalWaterRainMask = smoothstep(
        waterRainRingOffCos,
        waterRainRingFullCos,
        waterGeometryUp);

    float2 waterRainNormalDistortion = CalculateWaterRainNormalDistortion(
        Input.vWorldPosition,
        rainAmount,
        horizontalWaterRainMask,
        waterRainTopSide);

    bool ssrActive = ssrStrength > 0.0001f;

    float2 wt = Input.vWorldPosition.xz / 1000.0f;
    float3 ds = TX_Distortion.Sample(SS_Linear, wt * DIST_SMALL_SCALE + RI_Time * DIST_SMALL_SPEED).xyz * 2 - 1;
    ds += TX_Distortion.Sample(SS_Linear, wt * float2(-1, 0.7f) * DIST_SMALL_SCALE + RI_Time * DIST_SMALL_SPEED * 2).xyz * 2 - 1;
    ds *= 0.5f;
    float3 db = TX_Distortion.Sample(SS_Linear, wt * DIST_BIG_SCALE + RI_Time * DIST_BIG_SPEED).xyz * 2 - 1;
    db += TX_Distortion.Sample(SS_Linear, wt * float2(-1, 0.7f) * DIST_BIG_SCALE + RI_Time * DIST_BIG_SPEED * 1.2f).xyz * 2 - 1;
    db *= 0.5f;

    float farScale = lerp(1, 0.58f, SmootherStep01(saturate((waterViewDistance - 14000) / 38000)));
    float waveScale = farScale * lerp(1, 1.12f, rainAmount);
    float refrFade = SmootherStep01(saturate((waterThickness - 12) / 98));
    float oceanRefractionEdgeFade = smoothstep(0.0f, 0.060f, 1.0f - screenUV.y);
    float refractionEdgeFade = lerp(1.0f, oceanRefractionEdgeFade, step(0.5f, WM_IsOceanWater) * waterTopSide);
    float2 distUV = screenUV + (ds.xy + db.xy) * DIST_SMALL_AMOUNT * shoreVisibility * refrFade * waveScale * refractionEdgeFade;
    float3 diffuse = TX_Diffuse.Sample(SS_Linear, Input.vTexcoord + ds.xy * DIST_SMALL_AMOUNT * 0.5f * waveScale).rgb;

    float rawDepth = TX_Depth.Sample(SS_Linear, distUV).r;
    float depth = LinearizeWaterDepth(rawDepth);
    distUV = saturate(CleanRefraction(distUV, screenUV, depth));
    rawDepth = TX_Depth.Sample(SS_Linear, distUV).r;
    depth = LinearizeWaterDepth(rawDepth);

    float refrValid = step(0.000001f, rawDepth);
    float sceneValid = saturate(refrValid + cameraBelowSurface);
    float3 wf = normalize(
        float3(
            db.x * waveScale + waterRainNormalDistortion.x,
            db.z * 10,
            db.y * waveScale + waterRainNormalDistortion.y));
    float3 ws = normalize(
        float3(
            ds.x * waveScale + waterRainNormalDistortion.x,
            ds.z * 10,
            ds.y * waveScale + waterRainNormalDistortion.y));
    float3 sceneClean = TX_Scene.Sample(SS_Linear, screenUV).rgb;
    float3 sceneRefr = TX_Scene.Sample(SS_Linear, distUV).rgb;
    float ndv = saturate(dot(-viewDirection, wf));
    float fresnel = 0.02f + 0.98f * pow(1 - ndv, 5);
    float reflectFresnel = pow(1.0f - ndv, 3.0f);
    float3 reflRay = reflect(viewDirection, wf);
    float3 reflVec = -reflRay;
    float hemi = smoothstep(0, 0.06f, reflRay.y) * waterTopSide;

    // Keep reflections stable while allowing more of the existing water-wave
    // normal to shape the ray.  This softens the mirror-like appearance for
    // both Ocean and Legacy water without additional texture samples.
    float normalSmooth = 0.34f + 0.18f * SmootherStep01(saturate((waterViewDistance - 1500) / 12000));
    float3 geoDir = reflect(viewDirection, normalize(lerp(wf, float3(0, 1, 0), normalSmooth)));
    float3 cube = max(TX_ReflectionCube.Sample(SS_Linear, reflVec).rgb, 0);
    float cubeOnlyReflectionAmount = saturate(
        lerp(0.35f, 1.0f, reflectFresnel) * 0.5f * reflectFresnel)
        * waterReflectionSuppress;
    float3 cubeOnlyReflectionColor = cube * lerp(1.0f, diffuse, 0.6f);
    float lum = dot(cube, float3(.2126, .7152, .0722));
    float3 gray = lum.xxx;
    float3 dayRain = lerp(gray * .46f, float3(.18, .20, .21), .55f) * lerp(1, max(AC_LowCloudRainColor, 0), .30f);
    float3 clearNight = lerp(cube * .025f, float3(.004, .009, .023), .72f);
float3 oceanNightRainFallback =
    lerp(
        max(
            AC_NightRainSkyColor * 0.32f,
            float3(0.006f, 0.008f, 0.012f)),
        float3(0.018f, 0.027f, 0.040f),
        0.70f);

float3 rainNight =
    WM_IsOceanWater > 0.5f
        ? oceanNightRainFallback
        : max(
            AC_NightRainSkyColor * 0.46f,
            float3(0.012f, 0.018f, 0.030f));
    float3 fallback = lerp(lerp(cube, dayRain, rainAmount), lerp(clearNight, rainNight, rainAmount), nightAmount);

    float3 skyNormal = normalize(lerp(wf, float3(0, 1, 0), .46f));
    float3 skyDir = reflect(viewDirection, skyNormal);
    float2 skyUV = screenUV;
    float skyValid = 0;
    float3 skyPos = Input.vWorldPosition;
    float skyStepSize = 180;

    [unroll]
    for (int k = 0; k < 14; k++)
    {
        skyPos += skyDir * skyStepSize;
        skyStepSize *= 1.42f;
        float4 q = mul(float4(skyPos, 1), RI_ViewProj);
        if (q.w <= .001f) break;
        q.xyz /= q.w;
        float2 u = q.xy * float2(.5, -.5) + .5;
        if (any(u < 0) || any(u > 1) || q.z < 0 || q.z > 1) break;
        if (TX_Depth.SampleLevel(SS_Linear, u, 0).r <= .000001f)
        {
            skyUV = u;
            skyValid = 1;
        }
    }

    skyValid *= step(.0001f, skyDir.y) * waterTopSide * ssrEnabled;
float3 skyBase =
    SampleSkyWithoutCelestialBodies(
        skyUV,
        skyDir,
        fallback);

float4 clouds =
    ResolveLowCloudLayer(
        TX_LowClouds.SampleLevel(
            SS_Linear,
            skyUV,
            0),
        skyBase);

float3 skyReflection =
    max(
        skyBase
        + (
            skyBase * (1.0f - clouds.a)
            + clouds.rgb
            - skyBase)
        * lerp(
            1.12f,
            1.30f,
            saturate(clouds.a)),
        0.0f);
    float2 skyEdge = saturate(abs(skyUV - .5f) * 2);
    float skyWeight = skyValid * (1 - smoothstep(.78f, 1, max(skyEdge.x, skyEdge.y))) * hemi;

    float3 geoColor = skyReflection;
    float3 geoWorld = Input.vWorldPosition;
    float geoValid = 0.0f;
    float geoSceneZ = 1000000.0f;
    float ssrConfidence = 0.0f;

    if (ssrActive)
    {
        SSRTraceResult trace = SSRCore_TraceWorldRay(
            TX_Depth, Input.vWorldPosition, geoDir, RI_ViewProj, RI_ViewportSize,
            RI_Projection._43, RI_Projection._33,
            30000.0f, 24, 5, 2.0f, 350.0f, 0.001f);

        if (trace.hit > 0.5f && trace.confidence > 0.0f)
        {
            geoValid = 1.0f;
            geoColor = TX_Scene.SampleLevel(SS_Linear, trace.hitUV, 0).rgb;
            geoSceneZ = abs(LinearizeWaterDepth(TX_Depth.SampleLevel(SS_Linear, trace.hitUV, 0).r));
            geoWorld = Input.vWorldPosition + geoDir * geoSceneZ;
            ssrConfidence = saturate(trace.confidence);
        }
    }

    float3 processedReflection = max(geoColor, 0.0f);
    float rainVis = 1.0f;
    if (geoValid > .5f)
    {
        processedReflection = lerp(
            processedReflection,
            ApplyAtmosphericScatteringGround(geoWorld, processedReflection),
            rainAmount);
        float fog = rainAmount * smoothstep(
            5000.0f,
            22000.0f,
            length(geoWorld - AC_WorldCameraPos));
        rainVis = (1.0f - fog) * (1.0f - fog);
    }

    // Preserve the proven color response of the old water shader while using
    // the new shared SSR tracer only for hit position and confidence.
    float3 reflectionLumaWeights = float3(.2126f, .7152f, .0722f);
    float processedLuma = dot(processedReflection, reflectionLumaWeights);
    processedReflection *= rcp(1.0f + max(0.0f, processedLuma - 6.0f) * .12f);
    processedLuma = dot(processedReflection, reflectionLumaWeights);

    // Improve reflected geometry definition without increasing its luminance.
    // Daylight receives the stronger contrast lift; night keeps a restrained
    // lift inside the already dark, scene-derived reflection range.
    float reflectionContrast = lerp(1.10f, 1.05f, nightAmount);
    processedReflection = max(
        lerp(processedLuma.xxx, processedReflection, reflectionContrast),
        0.0f);
    processedLuma = dot(processedReflection, reflectionLumaWeights);

    // Apply a soft shoulder only to bright daytime geometry reflections.  This
    // keeps the stronger dark structure while preventing pale rock and wall
    // highlights from becoming as dominant as the directly visible geometry.
    float daylightReflectionControl = 1.0f - nightAmount;
    float daylightHighlightStart = 0.62f;
    float daylightHighlightExcess = max(
        processedLuma - daylightHighlightStart,
        0.0f);
    float compressedDaylightLuma =
        daylightHighlightStart
        + daylightHighlightExcess
        / (1.0f + daylightHighlightExcess * 1.65f);
    float daylightHighlightScale = min(
        1.0f,
        compressedDaylightLuma / max(processedLuma, 0.0001f));
    processedReflection *= lerp(
        1.0f,
        daylightHighlightScale,
        daylightReflectionControl);
    processedLuma = dot(processedReflection, reflectionLumaWeights);

    float geometryHit = saturate(ssrConfidence);
    float skyConf = saturate(skyWeight * lerp(.90f, .80f, rainAmount));
    float3 skyBack = lerp(fallback, skyReflection, skyConf);
    float skyBackLuma = max(dot(skyBack, reflectionLumaWeights), .0001f);
    float nightEdgeFloor = lerp(
        .055f,
        .010f,
        nightAmount * step(.5f, WM_IsOceanWater));
    float matchedLuma = max(
        nightEdgeFloor,
        processedLuma * lerp(1.42f, 1.00f, nightAmount)
            + lerp(.025f, .002f, nightAmount));
    float3 matchedReflection = skyBack * min(
        1.0f,
        matchedLuma / skyBackLuma);

    // A valid hit keeps the old shader's dark, scene-colored reflection.
    // Confidence controls coverage only; it no longer recolors the hit white.
    float3 hitReflection = lerp(
        matchedReflection,
        processedReflection,
        geometryHit);
    float3 ssrColor = lerp(
        skyBack,
        hitReflection,
        geometryHit);
    ssrColor = lerp(fallback, ssrColor, ssrEnabled);

    // Screen-space source availability.  These flags only decide whether the
    // cubemap fallback is allowed; they do not replace the proven v3 color,
    // confidence or reflection-strength calculations.
    float geometrySourceAvailable = geoValid * ssrEnabled;
    float skySourceAvailable =
        (1.0f - geometrySourceAvailable)
        * step(0.0001f, skyWeight)
        * ssrEnabled;
    float cubeFallbackAvailable =
        1.0f - saturate(geometrySourceAvailable + skySourceAvailable);

    ssrConfidence = saturate(
        geometryHit * waterReflectionSuppress * ssrEnabled);

    float glintBlock = saturate(ssrConfidence * 1.45f);
    float3 sunProj = ProjectCelestial(-AC_LightPos.xyz);
    float3 moonProj = ProjectCelestial(-AC_MoonPos.xyz);
    float sunCloudA = ResolveLowCloudLayer(TX_LowClouds.SampleLevel(SS_Linear, sunProj.xy, 0), TX_Scene.SampleLevel(SS_Linear, sunProj.xy, 0).rgb).a * sunProj.z;
    float moonCloudA = ResolveLowCloudLayer(TX_LowClouds.SampleLevel(SS_Linear, moonProj.xy, 0), TX_Scene.SampleLevel(SS_Linear, moonProj.xy, 0).rgb).a * moonProj.z;
    float reflectedCloudTransmission = 1 - smoothstep(.08f, .88f, saturate(clouds.a));
    float sunCloudTransmission = min(
        1 - smoothstep(.08f, .88f, sunCloudA),
        reflectedCloudTransmission);
    float moonCloudTransmission = min(
        1 - smoothstep(.08f, .88f, moonCloudA),
        reflectedCloudTransmission);

    float3 finalColor;
    float maskOut;

    if (WM_IsOceanWater > .5f)
    {
        float total = lerp(
            cubeStrength,
            ssrStrength,
            saturate(geometrySourceAvailable + skySourceAvailable));
        float3 oceanScreenSpaceReflection = lerp(
            skyReflection,
            hitReflection,
            geometrySourceAvailable);
        float oceanScreenSpaceAvailable = saturate(
            geometrySourceAvailable + skySourceAvailable);
        float3 reflectionColor =
            oceanScreenSpaceReflection * oceanScreenSpaceAvailable;
        float thick = clamp(max(depth - surfaceViewZ, 0) * viewRayScale, 0, 6000);
        float underThick = clamp(abs(depth - surfaceViewZ) * .35f, 0, 1400);
        float optical = lerp(thick, underThick, cameraBelowSurface);
        float sd = max(fwidth(thick), 1);
        float se = clamp(max(65, sd * 1.25f), 65, 160);
        // Use the same proven soft shoreline at every time of day.  Day and
        // night now share one transition range without any time-dependent
        // widening, minimum color floor or separate volume mask.
        float shoreEnd = max(240.0f, se * 2.60f);
        float shore = SmootherStep01(saturate(
            (thick - 1.0f)
            / max(shoreEnd - 1.0f, 1.0f)));
        float3 absDay = float3(.0024, .00115, .00062);
        float3 absRain = float3(.003, .00155, .00088);
        float3 absorb = lerp(absDay, absRain, rainAmount) * lerp(1, .58f, cameraBelowSurface);
        float3 trans = exp(-absorb * optical);
        float3 oceanClearDayScatter =
            float3(0.035f, 0.120f, 0.150f);

        float3 oceanDayRainScatter =
            float3(0.070f, 0.066f, 0.064f);

        float3 oceanClearNightScatter =
            float3(0.008f, 0.017f, 0.034f);

        float3 oceanAtmosphericNightRainScatter =
            max(
                AC_NightRainSkyColor * 0.52f,
                float3(0.010f, 0.014f, 0.017f));

        float3 oceanNightRainScatter =
            lerp(
                oceanAtmosphericNightRainScatter,
                float3(0.020f, 0.032f, 0.044f),
                0.72f);

        float3 scatter =
            lerp(
                lerp(
                    oceanClearDayScatter,
                    oceanDayRainScatter,
                    rainAmount),
                lerp(
                    oceanClearNightScatter,
                    oceanNightRainScatter,
                    rainAmount),
                nightAmount);
        scatter *= lerp(.94f, 1.06f, saturate(dot(diffuse, float3(.2126, .7152, .0722)) * 1.4f));
        float3 volume = sceneRefr * trans + scatter * (1 - trans);
        volume = lerp(scatter, volume, sceneValid);
        // Do not apply a global material tint.  The Ocean color now comes only
        // from depth absorption, atmospheric scatter, refraction and reflection.
        // The same rule applies at day and night, so no uniform color layer can
        // outline the water mesh against the bank.
        volume = lerp(sceneClean, volume, shore);
        float4 underClouds = ResolveLowCloudLayer(TX_LowClouds.SampleLevel(SS_Linear, distUV, 0), sceneRefr);
        float3 underComposedSky = sceneRefr * (1 - underClouds.a) + underClouds.rgb;
        float underSky = cameraBelowSurface * (1 - refrValid);
        volume = lerp(volume, underComposedSky, underSky);
        float underGeometry = cameraBelowSurface * refrValid;
        volume = lerp(volume, lerp(sceneRefr, volume, .32f), underGeometry);
        // A true screen-space miss keeps some cubemap structure, but adapts
        // it to the current water/atmosphere base.  This prevents bright dusk
        // spots and strong directional color casts without leaving empty areas.
        float3 fallbackLumaWeights = float3(.2126f, .7152f, .0722f);
        float oceanBaseLuma = max(dot(volume, fallbackLumaWeights), .0001f);
        float rawCubeLuma = max(dot(fallback, fallbackLumaWeights), .0001f);
        float oceanCubeLumaLimit =
            oceanBaseLuma * lerp(1.55f, 1.20f, nightAmount)
            + lerp(.025f, .006f, nightAmount);
        float3 oceanLimitedCube =
            fallback * min(1.0f, oceanCubeLumaLimit / rawCubeLuma);
        float oceanLimitedCubeLuma = dot(
            oceanLimitedCube,
            fallbackLumaWeights);
        float3 oceanDesaturatedCube = lerp(
            oceanLimitedCubeLuma.xxx,
            oceanLimitedCube,
            .58f);
        float oceanCubeStructureWeight = lerp(
            .52f,
            .82f,
            1.0f - ssrEnabled);
        float3 oceanHybridFallback = lerp(
            volume,
            oceanDesaturatedCube,
            oceanCubeStructureWeight);
        reflectionColor +=
            oceanHybridFallback * cubeFallbackAvailable;

        float reflectionLuma = dot(reflectionColor, fallbackLumaWeights);
        float skyReflectionProtection = skySourceAvailable;
        float oceanFallbackInfluence = cubeFallbackAvailable;
        float oceanFallbackLimit = max(dot(volume, float3(.2126f, .7152f, .0722f)) * 2.15f + skyReflectionProtection * 0.18f, 0.08f);
        float oceanFallbackScale = min(1.0f, oceanFallbackLimit / max(reflectionLuma, 0.0001f));
        reflectionColor *= lerp(1.0f, oceanFallbackScale, oceanFallbackInfluence);
        float skySel = step(.001f, skyWeight) * (1.0f - saturate(ssrConfidence)) * ssrEnabled;
        float backupDayRf = lerp(fresnel, max(fresnel, .085f), skySel);
        float currentNightRf = lerp(fresnel, max(fresnel, .120f), skySel);
        float skyReflectionLift = skySel * (1.0f - saturate(ssrConfidence));
        float reflectionDriver = max(reflectFresnel, skyReflectionLift * 0.18f);
        float reflectAmount = saturate(
            lerp(0.42f, 1.0f, reflectFresnel) *
            lerp(0.68f, 1.0f, saturate(max(ssrConfidence, skyConf))) *
            reflectionDriver) *
            waterReflectionSuppress;
        float backupDayAmount = saturate(backupDayRf * total);
        float currentNightAmount = saturate(max(
            currentNightRf * total * 0.92f,
            reflectAmount * total));
        float amount = lerp(
            backupDayAmount,
            currentNightAmount,
            nightAmount) * shore * hemi;
        float3 oceanSsrColor = lerp(volume, reflectionColor, amount);
        float oceanGeometryBlend = saturate(
            ssrConfidence *
            ssrStrength *
            lerp(.48f, .92f, nightAmount) *
            rainVis) * shore;
        oceanSsrColor = lerp(
            oceanSsrColor,
            processedReflection,
            oceanGeometryBlend);
        float rawOceanCubeOnlyLuma = max(dot(
            cubeOnlyReflectionColor,
            reflectionLumaWeights), .0001f);
        float oceanNightCubeOnlyLumaLimit = oceanBaseLuma * 1.15f + .003f;
        float3 oceanNightCubeOnlyColor = cubeOnlyReflectionColor * min(
            1.0f,
            oceanNightCubeOnlyLumaLimit / rawOceanCubeOnlyLuma);
        float oceanNightCubeOnlyLuma = dot(
            oceanNightCubeOnlyColor,
            reflectionLumaWeights);
        oceanNightCubeOnlyColor = lerp(
            oceanNightCubeOnlyLuma.xxx,
            oceanNightCubeOnlyColor,
            .28f);
        float3 oceanAdaptiveCubeOnlyColor = lerp(
            cubeOnlyReflectionColor,
            oceanNightCubeOnlyColor,
            nightAmount);
        float oceanCubeOnlyAmount = cubeOnlyReflectionAmount
            * lerp(1.0f, .55f, nightAmount)
            * shore
            * hemi;
        float3 oceanCubeOnlyColor = lerp(
            volume,
            oceanAdaptiveCubeOnlyColor,
            oceanCubeOnlyAmount);
        float3 color = lerp(oceanCubeOnlyColor, oceanSsrColor, ssrEnabled);
        float3 smallRefl = reflect(-viewDirection, ws);
        float weather = GetRainSkyVisibility();
        float sunSpot = pow(saturate(dot(smallRefl, -AC_LightPos.xyz)), 500) * .5f * smoothstep(-.04f, .08f, AC_LightPos.y) * smoothstep(0.0f, 0.08f, saturate(AC_SunVisibility)) * weather * sunCloudTransmission * (1 - glintBlock);
        float glintControl = max(cubeStrength, ssrStrength) * shore;
        color += lerp(float3(1.2, .6, .2), float3(5, 5, 5), AC_LightPos.y) * sunSpot * glintControl;
        float3 moonDir = normalize(-AC_MoonPos.xyz);
        float moonSpot = pow(saturate(dot(smallRefl, moonDir)), 420) * .15f * smoothstep(-.04f, .08f, AC_MoonPos.y) * smoothstep(0.0f, 0.08f, saturate(AC_MoonVisibility)) * weather * moonCloudTransmission * (1 - glintBlock);
        color += float3(.60f, .70f, 1.00f) * moonSpot * glintControl;
        finalColor = color;
        maskOut = lerp(.25f * shore, 1, step(.5f, WM_DisableRainEffects));
    }
    else
    {
        float legacyShallowDepth = saturate(max(sceneViewZ - surfaceViewZ, 0.0f) * 0.01f);
        float2 legacyDistUV = screenUV + ds.xy * DIST_SMALL_AMOUNT + db.xy * DIST_SMALL_AMOUNT;
        float legacyRawDepthRefracted = TX_Depth.Sample(SS_Linear, legacyDistUV).r;
        float legacyDepthRefracted = LinearizeWaterDepth(legacyRawDepthRefracted);
        legacyDistUV = saturate(CleanRefraction(legacyDistUV, screenUV, legacyDepthRefracted));
        legacyRawDepthRefracted = TX_Depth.Sample(SS_Linear, legacyDistUV).r;
        legacyDepthRefracted = LinearizeWaterDepth(legacyRawDepthRefracted);

        // Floating leaves can be partly above and partly below the surface.  At
        // their alpha/depth contour the refracted depth may suddenly collapse
        // to an almost surface-level sample, which was interpreted as a real
        // shoreline and exposed the pond bottom.  Detect only this local depth
        // collapse and fall back smoothly to the undistorted center depth.
        float legacyRefractedThickness = max(
            legacyDepthRefracted - surfaceViewZ,
            0.0f) * viewRayScale;
        float legacyCenterThickness = max(
            sceneViewZ - surfaceViewZ,
            0.0f) * viewRayScale;
        float legacyDepthCollapse = smoothstep(
            20.0f,
            95.0f,
            legacyCenterThickness - legacyRefractedThickness);
        legacyDepthCollapse *= 1.0f - smoothstep(
            0.0f,
            26.0f,
            legacyRefractedThickness);
        float2 legacyStableDistUV = lerp(
            legacyDistUV,
            screenUV,
            legacyDepthCollapse);
        float legacyStableRawDepth = TX_Depth.Sample(
            SS_Linear,
            legacyStableDistUV).r;
        float legacyStableDepth = LinearizeWaterDepth(legacyStableRawDepth);
        legacyDistUV = legacyStableDistUV;
        legacyRawDepthRefracted = lerp(
            legacyRawDepthRefracted,
            legacyStableRawDepth,
            legacyDepthCollapse);
        legacyDepthRefracted = lerp(
            legacyDepthRefracted,
            legacyStableDepth,
            legacyDepthCollapse);
        float3 legacyDiffuse = TX_Diffuse.Sample(SS_Linear, Input.vTexcoord + ds.xy * DIST_SMALL_AMOUNT * 0.5f).rgb;
        legacyDiffuse = ApplyAtmosphericScatteringGround(Input.vWorldPosition, legacyDiffuse);
        float3 legacyWavesFres = normalize(
            float3(
                db.x + waterRainNormalDistortion.x,
                db.z * 10.0f,
                db.y + waterRainNormalDistortion.y));
        float3 legacyWavesSmall = normalize(
            float3(
                ds.x + waterRainNormalDistortion.x,
                ds.z * 10.0f,
                ds.y + waterRainNormalDistortion.y));
        float legacyFresnel = min(0.5f, saturate(pow(1.0f - saturate(dot(-viewDirection, legacyWavesFres)), 10.0f)));
        float3 legacyScene = TX_Scene.Sample(SS_Linear, legacyDistUV).rgb;
        float legacyVolumeValid = step(0.000001f, legacyRawDepthRefracted);
        float legacyGeometricDepth = clamp(max(legacyDepthRefracted - surfaceViewZ, 0.0f) * viewRayScale, 0.0f, 6000.0f);
        float legacyVolumeInfluence = SmootherStep01(saturate((legacyGeometricDepth - 18.0f) / 180.0f)) * legacyVolumeValid;
        float legacyOpticalDepth = min(1400.0f, 1400.0f * (1.0f - exp(-legacyGeometricDepth / 1400.0f)));
        float3 legacyAbsorption = float3(0.0020f, 0.00115f, 0.00165f);
        float3 legacyTransmittance = exp(-legacyAbsorption * legacyOpticalDepth);
        float3 legacyScatter = ApplyAtmosphericScatteringGround(Input.vWorldPosition, float3(0.050f, 0.080f, 0.060f));
        float3 legacyVolume = legacyScene * legacyTransmittance + legacyScatter * (1.0f - legacyTransmittance);
        legacyScene = lerp(legacyScene, legacyVolume, legacyVolumeInfluence);
        float3 legacySceneClean = TX_Scene.Sample(SS_Linear, lerp(legacyDistUV, screenUV, pow(1.0f - legacyShallowDepth, 20.0f))).rgb;
        legacyScene = lerp(legacyScene, legacyDiffuse, 0.73f * max(pow(legacyFresnel, 8.0f), 0.5f));
        float legacySsrFresnel = lerp(0.55f, 0.80f, saturate(pow(1.0f - saturate(dot(-viewDirection, legacyWavesFres)), 2.0f)));
        // Do not add the static reflection cubemap to Legacy water.  The fixed
        // world-direction texture sector is not scene-faithful and creates a
        // permanent brown patch plus dark borders at geometry/sky transitions.
        // Legacy reflections below are reconstructed only from live sky and
        // stable scene-color geometry hits.
        // Preserve coherent reflections while suppressing isolated dark tree
        // pixels caused by weak, rapidly changing geometry hits.  Sky coverage
        // remains independent; geometry must pass a smooth confidence gate.
        float legacyStableGeometryConfidence = smoothstep(
            0.22f,
            0.72f,
            ssrConfidence);
        float legacyReflectionCoverage = max(
            saturate(skyWeight * ssrEnabled),
            legacyStableGeometryConfidence);
        float legacySsrBlend = saturate(
            legacyReflectionCoverage *
            legacySsrFresnel *
            ssrStrength *
            0.86f *
            lerp(0.93f, 1.08f, nightAmount) *
            rainVis *
            waterReflectionSuppress);
        float3 legacyColor = lerp(legacyScene, legacySceneClean, pow(saturate(Input.vTexcoord2.y / 35000.0f), 4.0f));
        float legacySceneLuma = max(dot(legacySceneClean, float3(0.2126f, 0.7152f, 0.0722f)), 0.001f);
        float3 legacySceneChroma = legacySceneClean / legacySceneLuma;
        float legacyColorLuma = max(dot(legacyColor, float3(0.2126f, 0.7152f, 0.0722f)), 0.001f);
        float3 legacyColorWithSceneHue = legacySceneChroma * legacyColorLuma;
        float legacySubmergedColorMask = step(0.000001f, legacyRawDepthRefracted);
        legacyColor = lerp(legacyColor, legacyColorWithSceneHue, legacySubmergedColorMask * 0.42f);
        // Legacy edge foam removed.
        float cleanLegacyDepthRefracted = LinearizeWaterDepth(TX_Depth.Sample(SS_Linear, legacyDistUV).r);
        float legacyWaterThickness = clamp(
            max(cleanLegacyDepthRefracted - surfaceViewZ, 0.0f) * viewRayScale,
            0.0f,
            6000.0f);
        legacyWaterThickness = lerp(
            legacyWaterThickness,
            clamp(legacyCenterThickness, 0.0f, 6000.0f),
            legacyDepthCollapse);
        float legacyShoreDerivative = max(fwidth(legacyWaterThickness), 1.0f);
        float legacyShoreFadeEnd = clamp(max(65.0f, legacyShoreDerivative * 1.25f), 65.0f, 160.0f);
        float legacyShoreVisibility = SmootherStep01(saturate((legacyWaterThickness - 1.0f) / max(legacyShoreFadeEnd - 1.0f, 1.0f)));
        // Avoid a uniform global Legacy tint at night.  Darken only relative
        // to the live scene color, then restore sceneClean at the shoreline.
        float3 legacyNightRelativeColor = lerp(
            legacyColor,
            legacyColor * saturate(sceneClean * 1.35f + 0.18f),
            nightAmount);
        legacyColor = lerp(
            sceneClean,
            legacyNightRelativeColor,
            legacyShoreVisibility);
        float3 legacyReflectVectorSmall = reflect(-viewDirection, legacyWavesSmall);
        float weatherLightVisibility = GetRainSkyVisibility();
        float sunSpot = pow(saturate(dot(legacyReflectVectorSmall, -AC_LightPos.xyz)), 500.0f) * 0.5f;
        sunSpot *= smoothstep(-0.04f, 0.08f, AC_LightPos.y) * smoothstep(0.0f, 0.08f, saturate(AC_SunVisibility)) * weatherLightVisibility * sunCloudTransmission * (1.0f - glintBlock) * legacyShoreVisibility;
        legacyColor += lerp(float3(1.2f, 0.6f, 0.2f), float3(5.0f, 5.0f, 5.0f), AC_LightPos.y) * sunSpot;
        float moonSpot = pow(saturate(dot(legacyReflectVectorSmall, normalize(-AC_MoonPos.xyz))), 360.0f) * 0.22f;
        moonSpot *= smoothstep(-0.04f, 0.08f, AC_MoonPos.y) * smoothstep(0.0f, 0.08f, saturate(AC_MoonVisibility)) * weatherLightVisibility * moonCloudTransmission * (1.0f - glintBlock) * legacyShoreVisibility;
        legacyColor += float3(0.667f, 0.759f, 1.15f) * moonSpot;
        // Increase reflected color separation without lifting its average
        // luminance.  This makes trees and rocks more readable while avoiding
        // a brighter, glass-like surface over the visible pond bottom.
        float legacySkyAvailable =
            (1.0f - legacyStableGeometryConfidence)
            * step(0.0001f, skyWeight)
            * ssrEnabled;
        float legacyCubeFallback =
            1.0f - saturate(
                legacyStableGeometryConfidence + legacySkyAvailable);
        float legacyBaseLuma = max(dot(
            legacyColor,
            float3(.2126f, .7152f, .0722f)), .0001f);
        float legacyRawCubeLuma = max(dot(
            fallback,
            float3(.2126f, .7152f, .0722f)), .0001f);
        float legacyCubeLumaLimit =
            legacyBaseLuma * lerp(1.30f, 1.12f, nightAmount)
            + lerp(.015f, .004f, nightAmount);
        float3 legacyLimitedCube =
            fallback * min(1.0f, legacyCubeLumaLimit / legacyRawCubeLuma);
        float legacyLimitedCubeLuma = dot(
            legacyLimitedCube,
            float3(.2126f, .7152f, .0722f));
        float3 legacyDesaturatedCube = lerp(
            legacyLimitedCubeLuma.xxx,
            legacyLimitedCube,
            .38f);
        float legacyCubeStructureWeight = lerp(
            .28f,
            .72f,
            1.0f - ssrEnabled);
        float3 legacyHybridFallback = lerp(
            legacyColor,
            legacyDesaturatedCube,
            legacyCubeStructureWeight);
        float3 legacySceneReflection =
            processedReflection * legacyStableGeometryConfidence
            + skyReflection * legacySkyAvailable
            + legacyHybridFallback * legacyCubeFallback;
        float legacyReflectionLuma = dot(
            legacySceneReflection,
            float3(0.2126f, 0.7152f, 0.0722f));
        float legacyDefinitionStrength =
            legacyStableGeometryConfidence *
            lerp(0.10f, 0.06f, nightAmount);
        float3 legacyDefinedReflection = max(
            lerp(
                legacyReflectionLuma.xxx,
                legacySceneReflection,
                1.0f + legacyDefinitionStrength),
            0.0f);
        float3 legacySsrFinalColor = lerp(
            legacyColor,
            legacyDefinedReflection,
            legacySsrBlend * legacyShoreVisibility);
        float rawLegacyCubeOnlyLuma = max(dot(
            cubeOnlyReflectionColor,
            reflectionLumaWeights), .0001f);
        float legacyNightCubeOnlyLumaLimit = legacyBaseLuma * 1.10f + .002f;
        float3 legacyNightCubeOnlyColor = cubeOnlyReflectionColor * min(
            1.0f,
            legacyNightCubeOnlyLumaLimit / rawLegacyCubeOnlyLuma);
        float legacyNightCubeOnlyLuma = dot(
            legacyNightCubeOnlyColor,
            reflectionLumaWeights);
        legacyNightCubeOnlyColor = lerp(
            legacyNightCubeOnlyLuma.xxx,
            legacyNightCubeOnlyColor,
            .24f);
        float3 legacyAdaptiveCubeOnlyColor = lerp(
            cubeOnlyReflectionColor,
            legacyNightCubeOnlyColor,
            nightAmount);
        float legacyCubeOnlyAmount = cubeOnlyReflectionAmount
            * lerp(1.0f, .50f, nightAmount)
            * legacyShoreVisibility;
        float3 legacyCubeOnlyColor = lerp(
            legacyColor,
            legacyAdaptiveCubeOnlyColor,
            legacyCubeOnlyAmount);
        float3 legacyFinalColor = lerp(legacyCubeOnlyColor, legacySsrFinalColor, ssrEnabled);
        finalColor = legacyFinalColor;
        maskOut = lerp(0.25f, 1.0f, step(0.5f, WM_DisableRainEffects));
    }

    o.color = float4(max(finalColor, 0), 1);
    o.waterMask = maskOut;
    o.fsr3ReactiveMask = .45f;
    return o;
}
