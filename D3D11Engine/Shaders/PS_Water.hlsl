#include <AtmosphericScattering.h>
#include <FFFog.h>
#include <DS_Defines.h>

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
    float sm = smoothstep(0.9962f, 0.9988f, dot(r, normalize(AC_LightPos))) * step(0.0001f, saturate(AC_SunVisibility));
    float mm = smoothstep(0.9968f, 0.9990f, dot(r, normalize(AC_MoonPos))) * step(0.0001f, saturate(AC_MoonVisibility));
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
    float valid = step(0.001f, p.w);
    p.xyz /= max(p.w, 0.001f);
    float2 uv = p.xy * float2(0.5f, -0.5f) + 0.5f;
    valid *= step(0, uv.x) * step(uv.x, 1) * step(0, uv.y) * step(uv.y, 1);
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

    ssrEnabled *= steepWaterSsrFactor;
    ssrStrength *= steepWaterSsrFactor;
    cubeStrength *= steepWaterSsrFactor;

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
    float3 reflRay = reflect(viewDirection, wf);
    float3 reflVec = -reflRay;
    float hemi = smoothstep(0, 0.06f, reflRay.y) * waterTopSide;

    float normalSmooth = 0.38f + 0.22f * SmootherStep01(saturate((waterViewDistance - 1500) / 12000));
    float3 geoDir = reflect(viewDirection, normalize(lerp(wf, float3(0, 1, 0), normalSmooth)));
    float3 cube = max(TX_ReflectionCube.Sample(SS_Linear, reflVec).rgb, 0);
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
    float geoRaw = 0, geoQual = 0, geoInter = 0, geoCoverage = 0, geoOcc = 0, geoValid = 0, geoSceneZ = 1000000.0f;

    if (ssrActive)
    {
        float3 pos = Input.vWorldPosition;
        float3 dir = geoDir;
        float stepSize = 40;
        float2 prevUV = screenUV;
        float prevDiff = -1000000;

        for (int i = 1; i <= 52; i++)
        {
            pos += dir * stepSize;
            float4 p = mul(float4(pos, 1), RI_ViewProj);
            if (p.w <= .001f) break;
            p.xyz /= p.w;
            float2 uv = p.xy * float2(.5, -.5) + .5;
            if (any(uv < 0) || any(uv > 1) || p.z < 0 || p.z > 1) break;

            float pix = length((uv - prevUV) * RI_ViewportSize);
            float ctrl = saturate(6 / max(pix, 1));
            float nearPhase = 1 - smoothstep(8, 18, (float)i);
            float growth = lerp(lerp(1.16f, 1.22f, ctrl), lerp(.78f, 1.08f, ctrl), nearPhase);
            float nextStep = clamp(max(stepSize * growth, lerp(18, 180, smoothstep(14, 40, (float)i))), 18, 650);

            float rd = TX_Depth.SampleLevel(SS_Linear, uv, 0).r;
            if (rd <= .000001f) break;

            float sz = LinearizeWaterDepth(rd);
            float diff = p.w - sz;
            bool cross = diff > 0 && prevDiff <= 0;
            bool inside = diff > 0 && diff < max(stepSize * 2, abs(sz) * .012f);

            if (cross || inside)
            {
                float3 lo = pos - dir * stepSize, hi = pos, mid = pos;
                [unroll]
                for (int j = 0; j < 5; j++)
                {
                    mid = (lo + hi) * .5f;
                    float4 m = mul(float4(mid, 1), RI_ViewProj);
                    m.xyz /= max(m.w, .001f);
                    float2 mu = saturate(m.xy * float2(.5, -.5) + .5);
                    float mz = LinearizeWaterDepth(TX_Depth.SampleLevel(SS_Linear, mu, 0).r);
                    if (m.w - mz > 0) hi = mid; else lo = mid;
                }

                float4 f = mul(float4(mid, 1), RI_ViewProj);
                f.xyz /= max(f.w, .001f);
                uv = saturate(f.xy * float2(.5, -.5) + .5);
                float2 px = 1 / RI_ViewportSize;
                float finalZ = LinearizeWaterDepth(TX_Depth.SampleLevel(SS_Linear, uv, 0).r);
                float residual = abs(f.w - finalZ);
                float rt = max(28, abs(finalZ) * .006f);
                float iq = 1 - smoothstep(rt, rt * 4, residual);
                float2 delta = uv - prevUV;
                float2 sd = delta / max(length(delta), .00001f);
                float2 sn = float2(-sd.y, sd.x);
                float2 off = px * 1.5f;

                float zf = LinearizeWaterDepth(TX_Depth.SampleLevel(SS_Linear, saturate(uv + sd * off), 0).r);
                float zb = LinearizeWaterDepth(TX_Depth.SampleLevel(SS_Linear, saturate(uv - sd * off), 0).r);
                float za = LinearizeWaterDepth(TX_Depth.SampleLevel(SS_Linear, saturate(uv + sn * off), 0).r);
                float zz = LinearizeWaterDepth(TX_Depth.SampleLevel(SS_Linear, saturate(uv - sn * off), 0).r);
                float ct = max(38, abs(finalZ) * .011f);

                float cov = .25f * ((1 - smoothstep(ct, ct * 2, abs(zf - finalZ))) + (1 - smoothstep(ct, ct * 2, abs(zb - finalZ))) + (1 - smoothstep(ct, ct * 2, abs(za - finalZ))) + (1 - smoothstep(ct, ct * 2, abs(zz - finalZ))));
                float st = max(75, abs(finalZ) * .022f);
                float support = .25f * ((1 - smoothstep(st, st * 3, abs(zf - finalZ))) + (1 - smoothstep(st, st * 3, abs(zb - finalZ))) + (1 - smoothstep(st, st * 3, abs(za - finalZ))) + (1 - smoothstep(st, st * 3, abs(zz - finalZ))));
                float discontinuity = smoothstep(ct * 1.5f, ct * 6, max(abs(za - finalZ), abs(zz - finalZ)));
                float occ = saturate((1 - smoothstep(900, 4200, abs(finalZ))) * (1 - smoothstep(.28f, .66f, cov)) * discontinuity);
                float farRisk = smoothstep(9000, 26000, abs(finalZ)) * (1 - smoothstep(.30f, .72f, cov));
                float reject = 1 - smoothstep(.10f, .78f, max(occ, farRisk));

                float reflectionRadius =
                    lerp(
                        0.65f,
                        1.35f,
                        smoothstep(2000.0f, 18000.0f, abs(finalZ)));

                float2 reflectionOffset =
                    px * reflectionRadius;

                float reflectionDepthTolerance =
                    max(45.0f, abs(finalZ) * 0.012f);

                float3 reflectionSum =
                    TX_Scene.SampleLevel(SS_Linear, uv, 0).rgb * 4.0f;

                float reflectionWeight = 4.0f;

                float2 reflectionOffsets[4] = {
                    float2( reflectionOffset.x, 0.0f),
                    float2(-reflectionOffset.x, 0.0f),
                    float2(0.0f,  reflectionOffset.y),
                    float2(0.0f, -reflectionOffset.y)
                };

                [unroll]
                for (int reflectionSample = 0;
                     reflectionSample < 4;
                     ++reflectionSample)
                {
                    float2 reflectionUV =
                        saturate(
                            uv
                            + reflectionOffsets[reflectionSample]);

                    float reflectionDepth =
                        LinearizeWaterDepth(
                            TX_Depth.SampleLevel(
                                SS_Linear,
                                reflectionUV,
                                0).r);

                    float sampleWeight =
                        1.0f
                        - smoothstep(
                            reflectionDepthTolerance,
                            reflectionDepthTolerance * 3.0f,
                            abs(reflectionDepth - finalZ));

                    reflectionSum +=
                        TX_Scene.SampleLevel(
                            SS_Linear,
                            reflectionUV,
                            0).rgb
                        * sampleWeight;

                    reflectionWeight += sampleWeight;
                }

                geoColor =
                    reflectionSum
                    / max(reflectionWeight, 0.0001f);
                geoWorld = mid;
                geoValid = 1;
                geoSceneZ = abs(finalZ);
                float2 e = saturate(abs(uv - .5f) * 2);
                geoRaw = 1 - smoothstep(.78f, 1, max(e.x, e.y));
                geoInter = iq;
                geoCoverage = cov;
                geoOcc = occ;
                geoQual = iq * lerp(.38f * smoothstep(.34f, .70f, cov), 1, support) * lerp(.72f, 1, smoothstep(120, 900, abs(finalZ))) * reject;
                break;
            }
            prevUV = uv;
            prevDiff = diff;
            stepSize = nextStep;
        }
    }

    float shallow = saturate(max(depth - surfaceViewZ, 0) * .01f);
    float nearFade = smoothstep(100, 450, waterViewDistance);
    float contact = smoothstep(.04f, .22f, shallow);
    float fpInter = smoothstep(.015f, .12f, geoInter);
    float fpCov = lerp(.62f, 1, smoothstep(.16f, .62f, geoCoverage));
    float fpOcc = 1 - smoothstep(.42f, .90f, geoOcc);
    float footprint = step(.5f, geoValid) * geoRaw * nearFade * hemi * fpInter * fpCov * fpOcc;
    float coreWeight = geoRaw * nearFade * contact * geoQual * hemi;
    float accepted = step(.5f, geoValid) * smoothstep(.025f, .14f, geoQual);
    float stable = accepted * smoothstep(.22f, .62f, geoInter) * smoothstep(.30f, .70f, geoCoverage) * (1 - geoOcc);
    float nearHitQuality = smoothstep(700, 2200, geoSceneZ);
    float unstableNearHit = 1 - nearHitQuality;
    float nearCubemapFallback = saturate(unstableNearHit * geoValid);

    footprint *= 1 - nearCubemapFallback;
    coreWeight *= 1 - nearCubemapFallback;
    accepted *= 1 - nearCubemapFallback;
    stable *= 1 - nearCubemapFallback;

    float3 processed = max(geoColor, 0);
    float rainVis = 1;
    if (geoValid > .5f)
    {
        processed = lerp(processed, ApplyAtmosphericScatteringGround(geoWorld, processed), rainAmount);
        float fog = rainAmount * smoothstep(5000, 22000, length(geoWorld - AC_WorldCameraPos));
        rainVis = (1 - fog) * (1 - fog);
    }

    float geoLum = dot(processed, float3(.2126, .7152, .0722));
    processed *= rcp(1 + max(0, geoLum - 6) * .12f);
    float3 resolved = fallback;
    if (stable > .0001f)
    {
        float fl = dot(fallback, float3(.2126, .7152, .0722));
        resolved = lerp(fallback, fallback * min(1, max(.08f, geoLum * 1.35f + .025f) / max(fl, .0001f)), stable);
    }

    float geoConf = saturate(coreWeight * rainVis * lerp(1, .82f, rainAmount));
    float skyConf = saturate(skyWeight * lerp(.90f, .80f, rainAmount));
    float fpMask = saturate(footprint);
    float3 skyBack = lerp(resolved, skyReflection, skyConf);
    float backLum = dot(skyBack, float3(.2126, .7152, .0722));
    float nightEdgeFloor = lerp(.055f, .010f, nightAmount * step(.5f, WM_IsOceanWater));
    float3 matched = skyBack * min(1, max(nightEdgeFloor, geoLum * lerp(1.42f, 1.00f, nightAmount) + lerp(.025f, .002f, nightAmount)) / max(backLum, .0001f));
    float nightGeoBoost = lerp(1.0f, 1.85f, nightAmount) * lerp(1.0f, 1.20f, step(.5f, WM_IsOceanWater));
    float coreMask = min(accepted * smoothstep(.018f, .14f, saturate(geoConf * nightGeoBoost)), fpMask);
    float nightGeometryDominance = nightAmount * step(.5f, WM_IsOceanWater) * fpMask;
    float3 ssrColor = lerp(skyBack, matched, max(fpMask, nightGeometryDominance));
    ssrColor = lerp(ssrColor, processed, max(coreMask, nightGeometryDominance * accepted));
    float resolvedConf = max(coreMask, fpMask * lerp(.72f, .96f, nightAmount));
    float ssrConfidence = lerp(skyConf, max(skyConf, resolvedConf), fpMask);
    ssrConfidence *= steepWaterSsrFactor;
    float ssrWeight = max(skyWeight, fpMask);

    ssrColor = lerp(fallback, ssrColor, ssrEnabled);
    ssrConfidence *= ssrEnabled;
    ssrWeight *= ssrEnabled;
    fpMask *= ssrEnabled;
    coreMask *= ssrEnabled;

    float glintBlock = saturate(max(fpMask, coreMask * .85f) * 1.85f);
    float3 sunProj = ProjectCelestial(-AC_LightPos.xyz);
    float3 moonProj = ProjectCelestial(-AC_MoonPos.xyz);
    float sunCloudA = ResolveLowCloudLayer(TX_LowClouds.SampleLevel(SS_Linear, sunProj.xy, 0), TX_Scene.SampleLevel(SS_Linear, sunProj.xy, 0).rgb).a * sunProj.z;
    float moonCloudA = ResolveLowCloudLayer(TX_LowClouds.SampleLevel(SS_Linear, moonProj.xy, 0), TX_Scene.SampleLevel(SS_Linear, moonProj.xy, 0).rgb).a * moonProj.z;
    float sunCloudTransmission = 1 - smoothstep(.08f, .88f, sunCloudA);
    float moonCloudTransmission = 1 - smoothstep(.08f, .88f, moonCloudA);

    float3 finalColor;
    float maskOut;

    if (WM_IsOceanWater > .5f)
    {
        float total = lerp(cubeStrength, ssrStrength, ssrConfidence);
        float3 reflectionColor = lerp(fallback, ssrColor, ssrEnabled);
        float thick = clamp(max(depth - surfaceViewZ, 0) * viewRayScale, 0, 6000);
        float underThick = clamp(abs(depth - surfaceViewZ) * .35f, 0, 1400);
        float optical = lerp(thick, underThick, cameraBelowSurface);
        float sd = max(fwidth(thick), 1);
        float se = clamp(max(65, sd * 1.25f), 65, 160);
        float shore = SmootherStep01(saturate((thick - 1) / max(se - 1, 1)));
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
        volume = lerp(sceneClean, volume, shore);
        float4 underClouds = ResolveLowCloudLayer(TX_LowClouds.SampleLevel(SS_Linear, distUV, 0), sceneRefr);
        float3 underComposedSky = sceneRefr * (1 - underClouds.a) + underClouds.rgb;
        float underSky = cameraBelowSurface * (1 - refrValid);
        volume = lerp(volume, underComposedSky, underSky);
        float underGeometry = cameraBelowSurface * refrValid;
        volume = lerp(volume, lerp(sceneRefr, volume, .32f), underGeometry);
        float tint =
            saturate(
                WM_IsOceanWater
                * WM_OceanWaterTintStrength)
            * lerp(
                1.0f,
                0.35f,
                cameraBelowSurface)
            * lerp(
                1.0f,
                0.45f,
                nightAmount)
            * lerp(
                1.0f,
                0.35f,
                rainAmount);
        volume = lerp(volume, volume * max(WM_OceanWaterTint, 0), tint);
        float volumeLuma =
            dot(
                volume,
                float3(
                    0.2126f,
                    0.7152f,
                    0.0722f));

        float3 clearNightVolume =
            lerp(
                volume,
                volumeLuma.xxx,
                0.34f)
            * float3(
                1.04f,
                1.02f,
                0.90f);

        float3 rainNightVolume =
            lerp(
                volume,
                volumeLuma.xxx,
                0.24f)
            * float3(
                0.86f,
                0.99f,
                1.16f);

        float3 gradedNightVolume =
            lerp(
                clearNightVolume,
                rainNightVolume,
                rainAmount);

        volume =
            lerp(
                volume,
                gradedNightVolume,
                nightAmount * 0.58f);
        float reflectionLuma = dot(reflectionColor, float3(.2126f, .7152f, .0722f));
        float oceanFallbackInfluence = ssrEnabled * (1.0f - saturate(ssrConfidence));
        float oceanFallbackLimit = max(dot(volume, float3(.2126f, .7152f, .0722f)) * 1.35f, 0.04f);
        float oceanFallbackScale = min(1.0f, oceanFallbackLimit / max(reflectionLuma, 0.0001f));
        reflectionColor *= lerp(1.0f, oceanFallbackScale, oceanFallbackInfluence);
        float skySel = (1 - fpMask) * step(.001f, skyWeight) * ssrEnabled;
        float rf = lerp(fresnel, max(fresnel, .085f), skySel);
        float amount = saturate(rf * total) * shore * hemi;
        float3 color = lerp(volume, reflectionColor, amount);
    float oceanGeometryBlend = saturate(fpMask * ssrStrength * lerp(.48f, .92f, nightAmount) * rainVis);
    oceanGeometryBlend *= ssrEnabled * shore;
    color = lerp(color, processed, oceanGeometryBlend);
        float3 smallRefl = reflect(-viewDirection, ws);
        float weather = GetRainSkyVisibility();
        float sunSpot = pow(saturate(dot(smallRefl, -AC_LightPos.xyz)), 500) * .5f * smoothstep(-.04f, .08f, AC_LightPos.y) * weather * sunCloudTransmission * (1 - glintBlock);
        float glintControl = max(cubeStrength, ssrStrength) * shore;
        color += lerp(float3(1.2, .6, .2), float3(5, 5, 5), AC_LightPos.y) * sunSpot * glintControl;
        float3 moonDir = normalize(-AC_MoonPos.xyz);
        float moonSpot = pow(saturate(dot(smallRefl, moonDir)), 360) * .22f * smoothstep(-.04f, .08f, AC_MoonPos.y) * weather * moonCloudTransmission * (1 - glintBlock);
        color += float3(.667, .759, 1.15) * moonSpot * glintControl;
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
        float legacyCubeWeight = ssrActive ? saturate(1.0f - ssrWeight * saturate(ssrStrength)) : 1.0f;
        float legacyRainCubemapVisibility = lerp(1.0f, 0.12f, rainAmount) * (1.0f - rainAmount * smoothstep(5000.0f, 22000.0f, waterViewDistance));
        legacyScene += fallback * cubeStrength * legacyCubeWeight * legacyRainCubemapVisibility * rainVis * legacyFresnel * lerp(1.0f, legacyDiffuse, 0.6f);
        float legacySsrBlend = saturate(ssrWeight * legacySsrFresnel * ssrStrength * 0.78f * lerp(0.85f, 1.10f, nightAmount) * rainVis);
        float3 legacyColor = lerp(legacyScene, legacySceneClean, pow(saturate(Input.vTexcoord2.y / 35000.0f), 4.0f));
        float legacySceneLuma = max(dot(legacySceneClean, float3(0.2126f, 0.7152f, 0.0722f)), 0.001f);
        float3 legacySceneChroma = legacySceneClean / legacySceneLuma;
        float legacyColorLuma = max(dot(legacyColor, float3(0.2126f, 0.7152f, 0.0722f)), 0.001f);
        float3 legacyColorWithSceneHue = legacySceneChroma * legacyColorLuma;
        float legacySubmergedColorMask = step(0.000001f, legacyRawDepthRefracted);
        legacyColor = lerp(legacyColor, legacyColorWithSceneHue, legacySubmergedColorMask * 0.42f);
        // Legacy edge foam removed.
        float cleanLegacyDepthRefracted = LinearizeWaterDepth(TX_Depth.Sample(SS_Linear, legacyDistUV).r);
        float legacyWaterThickness = clamp(max(cleanLegacyDepthRefracted - surfaceViewZ, 0.0f) * viewRayScale, 0.0f, 6000.0f);
        float legacyShoreDerivative = max(fwidth(legacyWaterThickness), 1.0f);
        float legacyShoreFadeEnd = clamp(max(65.0f, legacyShoreDerivative * 1.25f), 65.0f, 160.0f);
        float legacyShoreVisibility = SmootherStep01(saturate((legacyWaterThickness - 1.0f) / max(legacyShoreFadeEnd - 1.0f, 1.0f)));
        legacyColor *= lerp(1.0f, 0.72f, nightAmount);
        legacyColor = lerp(sceneClean, legacyColor, legacyShoreVisibility);
        float3 legacyReflectVectorSmall = reflect(-viewDirection, legacyWavesSmall);
        float weatherLightVisibility = GetRainSkyVisibility();
        float sunSpot = pow(saturate(dot(legacyReflectVectorSmall, -AC_LightPos.xyz)), 500.0f) * 0.5f;
        sunSpot *= smoothstep(-0.04f, 0.08f, AC_LightPos.y) * weatherLightVisibility * sunCloudTransmission * (1.0f - glintBlock) * legacyShoreVisibility;
        legacyColor += lerp(float3(1.2f, 0.6f, 0.2f), float3(5.0f, 5.0f, 5.0f), AC_LightPos.y) * sunSpot;
        float moonSpot = pow(saturate(dot(legacyReflectVectorSmall, normalize(-AC_MoonPos.xyz))), 360.0f) * 0.22f;
        moonSpot *= smoothstep(-0.04f, 0.08f, AC_MoonPos.y) * weatherLightVisibility * moonCloudTransmission * (1.0f - glintBlock) * legacyShoreVisibility;
        legacyColor += float3(0.667f, 0.759f, 1.15f) * moonSpot;
        float3 legacyFinalColor = lerp(legacyColor, ssrColor, legacySsrBlend * legacyShoreVisibility);
        finalColor = legacyFinalColor;
        maskOut = lerp(0.25f, 1.0f, step(0.5f, WM_DisableRainEffects));
    }

    o.color = float4(max(finalColor, 0), 1);
    o.waterMask = maskOut;
    o.fsr3ReactiveMask = .45f;
    return o;
}
