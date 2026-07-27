//--------------------------------------------------------------------------------------
// Screen-space reflections for rain-wet ground surfaces
//--------------------------------------------------------------------------------------

#include "DepthReconstruction.h"
#include "DS_Defines.h"

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
    // 0.25 is regular water. Alpha-aware transparent geometry occupies 0.80..1.0.
    if (encodedMask < 0.75f)
        return saturate(encodedMask / 0.25f);

    float transparencyCoverage = saturate((encodedMask - 0.80f) / 0.20f);
    return smoothstep(0.015f, 0.10f, transparencyCoverage);
}

float2 CalculateRainRipples(float2 wetUV, float time)
{
    float2 rippleAUV = frac(wetUV * 2.70f + time * float2(0.0705f, -0.0465f));
    float2 rippleBUV = frac(wetUV * 5.10f + time * float2(-0.0585f, 0.0780f));
    float2 rippleA = TX_Distortion.SampleLevel(SS_Linear, rippleAUV, 0).xy * 2.0f - 1.0f;
    float2 rippleB = TX_Distortion.SampleLevel(SS_Linear, rippleBUV, 0).yx * 2.0f - 1.0f;
    float waveA = sin((rippleA.x + rippleA.y + time * 2.55f) * 6.2831853f);
    float waveB = sin((rippleB.x - rippleB.y - time * 1.05f) * 6.2831853f);
    return rippleA * waveA * 0.55f + rippleB * waveB * 0.35f;
}

float CalculateRainSparkle(float2 screenUV, float2 wetUV, float time)
{
    const float impactRate = 18.0f;
    float impactTime = time * impactRate;
    float frame = floor(impactTime);
    float life = frac(impactTime);
    float impactPulse = exp2(-life * 5.0f);

    float2 noiseAUV = frac(screenUV * 145.0f + frame * float2(0.071f, 0.113f));
    float2 noiseBUV = frac(wetUV * 36.0f + frame * float2(-0.037f, 0.053f));
    float2 breakupUV = frac(screenUV * 310.0f + frame * float2(0.019f, -0.029f));

    float noiseA = TX_Distortion.SampleLevel(SS_Linear, noiseAUV, 0).x;
    float noiseB = TX_Distortion.SampleLevel(SS_Linear, noiseBUV, 0).y;
    float impactSeed = noiseA * 0.65f + noiseB * 0.35f;
    float grains = smoothstep(0.72f, 0.94f, impactSeed);
    float breakup = TX_Distortion.SampleLevel(SS_Linear, breakupUV, 0).z;

    return grains * lerp(0.60f, 1.0f, breakup) * impactPulse;
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
    float wetMask = upwardMask * rainExposure * saturate(WG_Wetness) * wetSSRVisibility * materialWetGroundSSRStrength;
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

    float rainRippleWeight = wetMask * saturate(WG_RainFXWeight) * smoothstep(0.05f, 0.45f, WG_Wetness);
    float2 rippleDistortion = CalculateRainRipples(wetUV, animationTime) * rainRippleWeight;
    float rainSparkle = CalculateRainSparkle(uv, wetUV, animationTime) * rainRippleWeight;

    // Keep the reflection world-anchored while animating the tiled wet microstructure.
    // Rain adds directional ripples and short-lived micro highlights only while rain is active.
    float2 softenedRippleDistortion = float2(rippleDistortion.x * 0.12f, rippleDistortion.y);
    float2 combinedDistortion = float2(distortion.x * 0.55f, distortion.y * 0.85f) + softenedRippleDistortion * 1.35f;
    float3 wetNormal = normalize(wsNormal + float3(combinedDistortion.x, 0.0f, combinedDistortion.y) * 0.16f);

    float3 viewRay = normalize(wsPosition - WG_CameraPosition);
    float3 rayDirection = normalize(reflect(viewRay, wetNormal));
    if (rayDirection.y <= 0.015f)
        return float4(sceneColor, 1.0f);

    float3 rayOrigin = wsPosition + wetNormal * 12.0f;
    const float maxRayDistance = 12000.0f;
    float3 rayEnd = rayOrigin + rayDirection * maxRayDistance;
    float2 hitUV = 0.0f;
    float hitWeight = 0.0f;

    float4 startClip = mul(float4(rayOrigin, 1.0f), WG_ViewProj);
    float4 endClip = mul(float4(rayEnd, 1.0f), WG_ViewProj);

    if (startClip.w > 0.001f && endClip.w > 0.001f)
    {
        float3 startNdc = startClip.xyz / startClip.w;
        float3 endNdc = endClip.xyz / endClip.w;
        float2 resolution = 1.0f / WG_InvResolution;
        float2 startPixel = (startNdc.xy * float2(0.5f, -0.5f) + 0.5f) * resolution;
        float2 endPixel = (endNdc.xy * float2(0.5f, -0.5f) + 0.5f) * resolution;

        float2 pixelDelta = endPixel - startPixel;
        bool permute = abs(pixelDelta.x) < abs(pixelDelta.y);
        if (permute)
        {
            pixelDelta = pixelDelta.yx;
            startPixel = startPixel.yx;
            endPixel = endPixel.yx;
        }

        if (abs(pixelDelta.x) > 0.0001f)
        {
            float stepDirection = sign(pixelDelta.x);
            float inversePrimaryDelta = stepDirection / pixelDelta.x;
            const float pixelStride = 2.0f;
            float2 pixelStep = float2(stepDirection, pixelDelta.y * inversePrimaryDelta) * pixelStride;
            float endPrimary = endPixel.x * stepDirection;
            float2 pixelPosition = startPixel + pixelStep;
            float previousDiff = -1000000.0f;
            float previousRayDistance = 0.0f;

            [loop]
            for (int i = 0; i < 64; ++i)
            {
                if (pixelPosition.x * stepDirection > endPrimary)
                    break;

                float2 unpermutedPixel = permute ? pixelPosition.yx : pixelPosition;
                float2 sampleUV = unpermutedPixel * WG_InvResolution;

                if (any(sampleUV < 0.0f) || any(sampleUV > 1.0f))
                    break;

                float sampleWetSSRBlock = DecodeWetSSRBlock(SampleWetSSRBlockMask(unpermutedPixel));
                if (sampleWetSSRBlock > 0.001f)
                {
                    previousDiff = -1000000.0f;
                    previousRayDistance = 0.0f;
                    pixelPosition += pixelStep;
                    continue;
                }

                float primaryProgress = saturate(abs(pixelPosition.x - startPixel.x) / max(abs(endPixel.x - startPixel.x), 0.0001f));
                float rayDistance = primaryProgress * maxRayDistance;
                float3 rayPosition = rayOrigin + rayDirection * rayDistance;
                float4 projected = mul(float4(rayPosition, 1.0f), WG_ViewProj);

                if (projected.w <= 0.001f)
                    break;

                projected.xyz /= projected.w;
                if (projected.z < 0.0f || projected.z > 1.0f)
                    break;

                float sampleDepth = TX_Depth.SampleLevel(SS_Linear, sampleUV, 0).r;
                if (sampleDepth <= 1e-7f)
                {
                    previousRayDistance = rayDistance;
                    pixelPosition += pixelStep;
                    continue;
                }

                float sampleZ = WG_ProjParams.z / (sampleDepth - WG_ProjParams.w);
                float depthDifference = projected.w - sampleZ;
                float rayInterval = max(rayDistance - previousRayDistance, 1.0f);
                bool crossedSurface = depthDifference > 0.0f && previousDiff <= 0.0f;
                bool insideSurface = depthDifference > 0.0f && depthDifference < max(rayInterval * 2.0f, abs(sampleZ) * 0.012f);

                if (crossedSurface || insideSurface)
                {
                    hitUV = sampleUV;
                    float edge = max(abs(sampleUV.x - 0.5f), abs(sampleUV.y - 0.5f)) * 2.0f;
                    hitWeight = 1.0f - smoothstep(0.76f, 1.0f, edge);
                    break;
                }

                previousDiff = depthDifference;
                previousRayDistance = rayDistance;
                pixelPosition += pixelStep;
            }
        }
    }

    if (hitWeight <= 0.0f)
        return float4(sceneColor, 1.0f);

    float2 reflectionRippleOffset = float2(rippleDistortion.x * 0.0025f, rippleDistortion.y * 0.0140f);
    float2 reflectedUV = saturate(hitUV + reflectionRippleOffset);
    float2 reflectedPixel = reflectedUV / WG_InvResolution;
    float reflectedWetSSRBlock = DecodeWetSSRBlock(SampleWetSSRBlockMask(reflectedPixel));
    if (reflectedWetSSRBlock > 0.001f)
        return float4(sceneColor, 1.0f);

    float3 reflectedColor = SampleRoughReflection(reflectedUV, combinedDistortion);
    reflectedColor *= 1.0f + rainSparkle * 0.18f;
    float reflectionLuma = dot(reflectedColor, float3(0.2126f, 0.7152f, 0.0722f));
    reflectedColor *= rcp(1.0f + max(0.0f, reflectionLuma - 1.0f) * 0.7f);

    float fresnel = pow(1.0f - saturate(dot(-viewRay, wetNormal)), 3.0f);
    float reflectionBlend = wetMask * hitWeight * lerp(0.06f, 0.28f, fresnel) * WG_Strength;
    reflectionBlend *= 1.0f + rainSparkle * 0.35f;
    return float4(lerp(sceneColor, reflectedColor, saturate(reflectionBlend)), 1.0f);
}
