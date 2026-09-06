#ifndef SHADOW_SAMPLING_H
#define SHADOW_SAMPLING_H

#ifndef MAX_CSM_CASCADES
#define MAX_CSM_CASCADES 4
#endif

#ifndef SHD_FILTER_PCSS
#define SHD_FILTER_PCSS 0
#endif

#ifndef SHD_FILTER_16TAP_PCF
#define SHD_FILTER_16TAP_PCF 0
#endif

#ifndef SHADOW_ATLAS
#define SHADOW_ATLAS 0
#endif

#ifndef PCSS_BLOCKER_SEARCH_TEXEL_CAP
#define PCSS_BLOCKER_SEARCH_TEXEL_CAP 24
#endif

#ifndef SHD_BLUE_NOISE
#define SHD_BLUE_NOISE 0
#endif

#ifndef PCSS_BLOCKER_TAPS
#define PCSS_BLOCKER_TAPS 8
#endif

#ifndef PCSS_FILTER_TAPS_NEAR
#define PCSS_FILTER_TAPS_NEAR 16
#endif

#ifndef PCF_FILTER_TAPS_NEAR
#define PCF_FILTER_TAPS_NEAR 16
#endif

#ifndef PCF_FILTER_TAPS_FAR
#define PCF_FILTER_TAPS_FAR 4
#endif

bool UseTemporalShadowReconstruction()
{
    return SQ_ShadowRuntimeParams.x > 0.5f;
}

bool UseRuntimeWorldShadows()
{
    return SQ_ShadowRuntimeParams.z > 0.5f;
}

int GetShadowKernelQuality()
{
    return clamp((int)(SQ_ShadowRuntimeParams.y + 0.5f), 0, 2);
}

int GetRuntimeCascadeCount()
{
    return clamp((int)(SQ_ShadowCascadeRuntimeParams.x + 0.5f), 1, MAX_CSM_CASCADES);
}

int GetRuntimePCFLimit()
{
    return clamp((int)(SQ_ShadowCascadeRuntimeParams.y + 0.5f), 0, GetRuntimeCascadeCount());
}

bool UsePCSSShadowFilter()
{
    return GetShadowKernelQuality() >= 2;
}

int GetRuntimePCFTapCount(int cascadeIndex)
{
    const bool nearCascade = cascadeIndex < GetRuntimePCFLimit();
    const int quality = GetShadowKernelQuality();

    if ( quality <= 0 ) return 4;
    if ( quality == 1 ) return nearCascade ? 8 : 4;
    return nearCascade ? PCF_FILTER_TAPS_NEAR : PCF_FILTER_TAPS_FAR;
}

// High keeps the first two cascades on PCSS but limits them to 8 filter samples.
// Extreme uses the full 16-tap near filter. The shadow-map size is already
// supplied at runtime, so the quality distinction does not require a shader
// reload when the user changes the shadow preset.
int GetRuntimePCSSNearTapCount()
{
    const int requestedTaps = SQ_ShadowmapSize >= 8192.0f ? 16 : 8;
    return min(PCSS_FILTER_TAPS_NEAR, requestedTaps);
}

// Shadow-map sampling for the atlas and array backends.
#if SHADOW_ATLAS
float2 CascadeToAtlasUV(float2 cascadeUV, int cascadeIndex)
{
    float4 rect = SQ_CascadeAtlasRect[cascadeIndex];
    float2 atlasUV = cascadeUV * rect.zw + rect.xy;
    float2 halfTexel = 0.5 / max( SQ_ShadowAtlasSize.xy, float2( 1.0f, 1.0f ) );
    return clamp(atlasUV, rect.xy + halfTexel, rect.xy + rect.zw - halfTexel);
}

float SampleShadowMapCmp(float2 cascadeUV, int cascadeIndex, float depth)
{
    return TX_ShadowmapAtlas.SampleCmpLevelZero(SS_Comp, CascadeToAtlasUV(cascadeUV, cascadeIndex), depth);
}

float SampleShadowMapLevel(float2 cascadeUV, int cascadeIndex)
{
    return TX_ShadowmapAtlas.SampleLevel(SS_Linear, CascadeToAtlasUV(cascadeUV, cascadeIndex), 0).r;
}
#else
float SampleShadowMapCmp(float2 cascadeUV, int cascadeIndex, float depth)
{
    return TX_ShadowmapArray.SampleCmpLevelZero(SS_Comp, float3(cascadeUV, (float)cascadeIndex), depth);
}

float SampleShadowMapLevel(float2 cascadeUV, int cascadeIndex)
{
    return TX_ShadowmapArray.SampleLevel(SS_Linear, float3(cascadeUV, (float)cascadeIndex), 0).r;
}
#endif

float GetCascadeShadowResolution(int cascadeIndex)
{
#if SHADOW_ATLAS
    return max(SQ_CascadeShadowResolution[cascadeIndex], 1.0f);
#else
    return max(SQ_ShadowmapSize, 1.0f);
#endif
}
// Poisson patterns used by the shadow filters.
static const float2 g_PoissonDisk16[16] = {
    float2(-0.94201624f, -0.39906216f),
    float2( 0.94558609f, -0.76890725f),
    float2(-0.09418410f, -0.92938870f),
    float2( 0.34495938f,  0.29387760f),
    float2(-0.91588581f,  0.45771432f),
    float2(-0.81544232f, -0.87912464f),
    float2(-0.38277543f,  0.27676845f),
    float2( 0.97484398f,  0.75648379f),
    float2( 0.44323325f, -0.97511554f),
    float2( 0.53742981f, -0.47373420f),
    float2(-0.26496911f, -0.41893023f),
    float2( 0.79197514f,  0.19090188f),
    float2(-0.24188840f,  0.99706507f),
    float2(-0.81409955f,  0.91437590f),
    float2( 0.19984126f,  0.78641367f),
    float2( 0.14383161f, -0.14100790f)
};

// 32-tap pattern for high-quality PCSS.
static const float2 g_PoissonDisk32[32] = {
    float2(-0.94201624f, -0.39906216f),
    float2( 0.94558609f, -0.76890725f),
    float2(-0.09418410f, -0.92938870f),
    float2( 0.34495938f,  0.29387760f),
    float2(-0.91588581f,  0.45771432f),
    float2(-0.81544232f, -0.87912464f),
    float2(-0.38277543f,  0.27676845f),
    float2( 0.97484398f,  0.75648379f),
    float2( 0.44323325f, -0.97511554f),
    float2( 0.53742981f, -0.47373420f),
    float2(-0.26496911f, -0.41893023f),
    float2( 0.79197514f,  0.19090188f),
    float2(-0.24188840f,  0.99706507f),
    float2(-0.81409955f,  0.91437590f),
    float2( 0.19984126f,  0.78641367f),
    float2( 0.14383161f, -0.14100790f),
    float2(-0.47609370f, -0.71680200f),
    float2( 0.67239900f,  0.46110100f),
    float2(-0.70447400f,  0.04610860f),
    float2( 0.26049600f, -0.73073100f),
    float2( 0.08472460f,  0.47360000f),
    float2(-0.52309600f,  0.71053100f),
    float2( 0.73020300f, -0.18908300f),
    float2(-0.16124800f,  0.16425900f),
    float2( 0.42027400f,  0.89780800f),
    float2(-0.89168800f, -0.14594500f),
    float2( 0.58721500f, -0.80065300f),
    float2(-0.30896500f, -0.18259200f),
    float2( 0.17058400f, -0.39880500f),
    float2(-0.62198700f, -0.49556300f),
    float2( 0.86741400f,  0.00426336f),
    float2(-0.04244530f,  0.71893100f)
};

// Smaller pattern for medium quality and distant cascades.
static const float2 g_PoissonDisk8[8] = {
    float2(-0.7071f,  0.7071f),
    float2(-0.0000f, -0.8750f),
    float2( 0.5303f,  0.5303f),
    float2(-0.6250f, -0.3310f),
    float2( 0.8750f,  0.0000f),
    float2(-0.3310f,  0.6250f),
    float2( 0.3310f, -0.6250f),
    float2( 0.0000f,  0.0000f)
};

float GetShadowBlueNoise(float2 screenPos, int cascadeIndex, int sampleOffset)
{
    if (UseTemporalShadowReconstruction())
    {
        uint2 pixel = uint2(screenPos);
        uint framePhase = SQ_FrameIndex & 63u;
        uint cascadePhase = (cascadeIndex >= 0) ? (uint)cascadeIndex : 0u;
        uint samplePhase = (sampleOffset >= 0) ? (uint)sampleOffset : 0u;

        uint2 noiseCoord;
        noiseCoord.x = (pixel.x + framePhase * 17u + cascadePhase * 37u + samplePhase * 23u) & 511u;
        noiseCoord.y = (pixel.y + framePhase * 29u + cascadePhase * 19u + samplePhase * 31u) & 511u;

        float4 noise = TX_ShadowBlueNoise.Load(int3(noiseCoord, 0));
        uint channel = samplePhase & 3u;
        float value = (channel == 0u) ? noise.x :
                      (channel == 1u) ? noise.y :
                      (channel == 2u) ? noise.z : noise.w;
        return frac(value + (float)((framePhase + samplePhase * 3u) & 63u) * 0.6180339887f);
    }

    // Keep non-temporal sampling deterministic.
    float2 seed = screenPos + float2((float)sampleOffset * 13.17f, (float)cascadeIndex * 7.31f);
    return frac(52.9829189f * frac(dot(seed, float2(0.06711056f, 0.00583715f))));
}

int GetBlueNoiseStartIndex(float2 screenPos, int cascadeIndex, int patternSize, int sampleOffset)
{
    int size = max(patternSize, 1);
    return UseTemporalShadowReconstruction()
        ? (int)(GetShadowBlueNoise(screenPos, cascadeIndex, sampleOffset) * (float)size) % size
        : sampleOffset % size;
}

float2x2 RotationMatrixFromNoise(float rawNoise)
{
    float angle = rawNoise * 6.283185307f;

    float s, c;
    sincos(angle, s, c);
    return float2x2(c, -s, s, c);
}

float2x2 GetPoissonRotationMatrixForCascade(float2 screenPos, int cascadeIndex)
{
    return UseTemporalShadowReconstruction()
        ? RotationMatrixFromNoise(GetShadowBlueNoise(screenPos, cascadeIndex, 0))
        : float2x2(1.0f, 0.0f, 0.0f, 1.0f);
}

float2x2 GetPoissonRotationMatrix(float2 screenPos)
{
    return GetPoissonRotationMatrixForCascade(screenPos, 0);
}

float2x2 GetPoissonRotationMatrixRForCascade(float2 screenPos, int cascadeIndex, out float rawNoise)
{
    if (UseTemporalShadowReconstruction())
    {
        rawNoise = GetShadowBlueNoise(screenPos, cascadeIndex, 0);
        return RotationMatrixFromNoise(rawNoise);
    }

    rawNoise = 0.5f;
    return float2x2(1.0f, 0.0f, 0.0f, 1.0f);
}

float2x2 GetPoissonRotationMatrixR(float2 screenPos, out float rawNoise)
{
    return GetPoissonRotationMatrixRForCascade(screenPos, 0, rawNoise);
}

//--------------------------------------------------------------------------------------
// PCSS: Blocker search - find average depth of blocking texels
// Uses non-comparison sampler to read raw depth values
//--------------------------------------------------------------------------------------
#if SHD_FILTER_PCSS
void FindBlockers(out float avgBlockerDepth, out float numBlockers,
                  float2 uv, float zReceiver, float searchRadius,
                  int cascadeIndex, float2x2 rotMat, float2 screenPos)
{
    float blockerSum = 0.0f;
    numBlockers = 0.0f;
    int startIdx = GetBlueNoiseStartIndex(screenPos, cascadeIndex, 16, 5);

    // The center sample catches thin casters that do not cover a full tap.
    float centerDepth = SampleShadowMapLevel(uv, cascadeIndex);
    if (centerDepth < zReceiver)
    {
        blockerSum += centerDepth;
        numBlockers += 1.0f;
    }

    [unroll]
    for (int i = 0; i < PCSS_BLOCKER_TAPS; ++i)
    {
        int sampleIdx = (startIdx + i * 5) & 15;
        float2 offset = mul(rotMat, g_PoissonDisk16[sampleIdx]) * searchRadius;
        float shadowMapDepth = SampleShadowMapLevel(uv + offset, cascadeIndex);

        if (shadowMapDepth < zReceiver)
        {
            blockerSum += shadowMapDepth;
            numBlockers += 1.0f;
        }
    }
    avgBlockerDepth = blockerSum / max(numBlockers, 1.0f);
}

// Estimate the PCSS penumbra and return its filter radius.
float EstimatePCSSFilterRadius(float2 uv, float zReceiver, int cascadeIndex,
                               float lightSize, float2x2 rotMat, float texelSize, float2 screenPos)
{
    // Limit the blocker search in texel units.
    float searchRadius = min(lightSize, texelSize * PCSS_BLOCKER_SEARCH_TEXEL_CAP);

    float avgBlockerDepth = 0.0f;
    float numBlockers = 0.0f;
    FindBlockers(avgBlockerDepth, numBlockers, uv, zReceiver, searchRadius, cascadeIndex, rotMat, screenPos);

    if (numBlockers < 1.0f)
        return -1.0f; // No blockers found - fully lit

    float penumbraWidth = (zReceiver - avgBlockerDepth) * lightSize;

    return clamp(penumbraWidth, texelSize * 0.5f, texelSize * 32.0f);
}
#endif

#if SHADOW_ATLAS
float IsInShadow(float3 wsPosition, Texture2D shadowmapAtlas, SamplerComparisonState samplerState)
{
    float4 vShadowSamplingPos = mul(float4(wsPosition, 1), SQ_ShadowViewProj[0]);

    float2 projectedTexCoords = vShadowSamplingPos.xy * float2(0.5f, -0.5f) + float2(0.5f, 0.5f);
    return SampleShadowMapCmp(projectedTexCoords.xy, 0, vShadowSamplingPos.z);
}
#else
float IsInShadow(float3 wsPosition, Texture2DArray shadowmapArray, SamplerComparisonState samplerState)
{
    float4 vShadowSamplingPos = mul(float4(wsPosition, 1), SQ_ShadowViewProj[0]);

    float2 projectedTexCoords = vShadowSamplingPos.xy * float2(0.5f, -0.5f) + float2(0.5f, 0.5f);
    return shadowmapArray.SampleCmpLevelZero(samplerState, float3(projectedTexCoords.xy, 0), vShadowSamplingPos.z);
}
#endif

float IsWet(float3 wsPosition, Texture2D shadowmap, SamplerComparisonState samplerState, matrix viewProj)
{
    float4 vShadowSamplingPos = mul(float4(wsPosition, 1), SQ_RainViewProj);
    vShadowSamplingPos.xyz /= vShadowSamplingPos.www;

    float2 projectedTexCoords = vShadowSamplingPos.xy * float2(0.5f, -0.5f) + float2(0.5f, 0.5f);
    float bias = 0.001f;
    return shadowmap.SampleCmpLevelZero(samplerState, projectedTexCoords.xy, vShadowSamplingPos.z - bias);
}

// Project into a cascade and return its bounds and blend factor.
void GetCascadeUVAndBounds(float3 wsPosition, int cascadeIndex,
                           out float4 vShadowSamplingPos, out float2 projectedTexCoords,
                           out float inBounds, out float blendFactor)
{
    matrix viewProj = SQ_ShadowViewProj[cascadeIndex];

    vShadowSamplingPos = mul(float4(wsPosition, 1), viewProj);
    projectedTexCoords = vShadowSamplingPos.xy * float2(0.5f, -0.5f) + float2(0.5f, 0.5f);

    // Leave a small inset for the filter taps and check the projected depth.
    const float margin = 1.5f / GetCascadeShadowResolution(cascadeIndex);
    bool isInBounds = projectedTexCoords.x > margin && projectedTexCoords.x < (1.0f - margin) &&
                      projectedTexCoords.y > margin && projectedTexCoords.y < (1.0f - margin) &&
                      vShadowSamplingPos.z >= 0.0f && vShadowSamplingPos.z <= 1.0f;
    inBounds = isInBounds ? 1.0f : 0.0f;

    // Blend near the cascade edge.
    const float blendZoneStart = 0.30f;
    float distToEdge = min(min(projectedTexCoords.x, 1.0f - projectedTexCoords.x),
                           min(projectedTexCoords.y, 1.0f - projectedTexCoords.y));
    blendFactor = 1.0f - smoothstep(margin, blendZoneStart, distToEdge);
}

float ComputeReceiverNormalBias(float3 wsNormal, float3 wsLightDirection, float texelWorldSize, float vegetationReceiverMask)
{
    float NoL = saturate(abs(dot(wsNormal, wsLightDirection)));
    float slopeScale = sqrt(saturate(1.0f - NoL * NoL));
    float verticalReceiver = 1.0f - saturate(abs(wsNormal.y));
    float vegetationBiasWeight = saturate(vegetationReceiverMask) *
        smoothstep(0.65f, 0.95f, slopeScale) *
        smoothstep(0.45f, 0.85f, verticalReceiver);
    float normalBiasMultiplier = lerp(1.5f, 4.0f, vegetationBiasWeight);
    return slopeScale * texelWorldSize * normalBiasMultiplier;
}

float3 ApplyReceiverNormalBias(float3 wsPosition, float3 wsNormal, float3 wsLightDirection, float texelWorldSize, float vegetationReceiverMask)
{
    float3 receiverBiasNormal = wsNormal;
    if (vegetationReceiverMask > 0.5f && dot(wsNormal, wsLightDirection) < 0.0f)
    {
        receiverBiasNormal = -wsNormal;
    }

    return wsPosition + receiverBiasNormal *
        ComputeReceiverNormalBias(wsNormal, wsLightDirection, texelWorldSize, vegetationReceiverMask);
}


// Sample a cascade with configurable softness.
float SampleCascadeShadowStablePCF(float4 vShadowSamplingPos, float2 projectedTexCoords,
                                   int cascadeIndex, float bias, float2 screenPos, float filterRadius)
{
    float sum = 0.0f;
    float2x2 rotMat = GetPoissonRotationMatrixForCascade(screenPos, cascadeIndex);
    const bool nearCascade = cascadeIndex < GetRuntimePCFLimit();
    const int tapCount = GetRuntimePCFTapCount(cascadeIndex);

    if (nearCascade)
    {
        int startIdx = GetBlueNoiseStartIndex(screenPos, cascadeIndex, 16, 29);
        [unroll]
        for (int i = 0; i < 16; i++)
        {
            if (i >= tapCount) break;
            int sampleIdx = (startIdx + i * 5) & 15;
            float2 offset = mul(rotMat, g_PoissonDisk16[sampleIdx]) * filterRadius;
            sum += SampleShadowMapCmp(projectedTexCoords.xy + offset, cascadeIndex,
                vShadowSamplingPos.z - bias);
        }
        return sum * (1.0f / (float)tapCount);
    }

    int startIdx = GetBlueNoiseStartIndex(screenPos, cascadeIndex, 8, 31);
    [unroll]
    for (int i = 0; i < 8; i++)
    {
        if (i >= tapCount) break;
        int sampleIdx = (startIdx + i * 3) & 7;
        float2 offset = mul(rotMat, g_PoissonDisk8[sampleIdx]) * filterRadius;
        sum += SampleShadowMapCmp(projectedTexCoords.xy + offset, cascadeIndex,
            vShadowSamplingPos.z - bias);
    }
    return sum * (1.0f / (float)tapCount);
}

float SampleCascadeShadowSoft(float4 vShadowSamplingPos, float2 projectedTexCoords,
                              int cascadeIndex, float bias, float2 screenPos, float softness)
{
    if (projectedTexCoords.x < 0.0f || projectedTexCoords.x > 1.0f ||
        projectedTexCoords.y < 0.0f || projectedTexCoords.y > 1.0f)
    {
        return 1.0f;
    }

    float shadow = 1.0f;
    float texelSize = 1.0f / GetCascadeShadowResolution(cascadeIndex);
    float filterRadius = texelSize * softness;

#if SHD_FILTER_PCSS
    // PCSS is intentionally restricted to the first two cascades. Distant
    // cascades use the cheaper PCF path below and therefore skip the blocker
    // search completely.
    if (UsePCSSShadowFilter() && cascadeIndex < GetRuntimePCFLimit())
    {
        float noiseVal;
        float2x2 rotMat = GetPoissonRotationMatrixRForCascade(screenPos, cascadeIndex, noiseVal);
        float zReceiver = vShadowSamplingPos.z - bias;

        float pcssRadius = EstimatePCSSFilterRadius(projectedTexCoords.xy, zReceiver,
            cascadeIndex, SQ_LightSize, rotMat, texelSize, screenPos);
        pcssRadius *= SQ_ShadowSoftness;

        if (pcssRadius < 0.0f)
        {
            // Fall back to a receiver comparison when no blocker was found.
            shadow = SampleShadowMapCmp(projectedTexCoords.xy, cascadeIndex, zReceiver);
        }
        else
        {
            // Include the receiver center in the filter.
            float centerShadow = SampleShadowMapCmp(
                projectedTexCoords.xy, cascadeIndex, zReceiver);
            float sum = centerShadow;
            float finalRadius = pcssRadius * lerp(0.85f, 1.15f, noiseVal);
            int startIdx = GetBlueNoiseStartIndex(screenPos, cascadeIndex, 32, 11);
            const int tapCount = GetRuntimePCSSNearTapCount();
            [unroll]
            for (int i = 0; i < PCSS_FILTER_TAPS_NEAR; i++)
            {
                if (i >= tapCount) break;
                int sampleIdx = (startIdx + i * 9) & 31;
                float2 offset = mul(rotMat, g_PoissonDisk32[sampleIdx]) * finalRadius;
                sum += SampleShadowMapCmp(projectedTexCoords.xy + offset, cascadeIndex, zReceiver);
            }
            shadow = sum * (1.0f / (float)(tapCount + 1));
        }
    }
    else
    {
#else
    {
#endif
#if SHD_FILTER_16TAP_PCF
    shadow = SampleCascadeShadowStablePCF(
        vShadowSamplingPos, projectedTexCoords, cascadeIndex, bias, screenPos, filterRadius);
#else
    shadow = SampleShadowMapCmp(
        projectedTexCoords.xy, cascadeIndex, vShadowSamplingPos.z - bias);
#endif
    }

    return saturate(shadow);
}

float2 TexOffset(int u, int v)
{
    return float2(u * 1.0f / SQ_ShadowmapSize, v * 1.0f / SQ_ShadowmapSize);
}

float ComputeShadowValueDirect(float3 wsPosition, Texture2D shadowmap, SamplerComparisonState samplerState, float vertLighting, matrix viewProj, float bias = 0.01f, float softnessScale = 1.0f)
{
	// Reconstruct VS World ShadowViewPosition from depth
    float4 vShadowSamplingPos = mul(float4(wsPosition, 1), viewProj);

    float2 projectedTexCoords = vShadowSamplingPos.xy * float2(0.5f, -0.5f) + float2(0.5f, 0.5f);
    float shadow = 1.0f;

    // Sample only inside the projected shadow-map bounds.
    if (projectedTexCoords.x >= 0.0f && projectedTexCoords.x <= 1.0f &&
        projectedTexCoords.y >= 0.0f && projectedTexCoords.y <= 1.0f)
    {
#if SHD_FILTER_16TAP_PCF
		float sum = 0;
		float x, y;

		float scale = softnessScale;

		//perform PCF filtering on a 4 x 4 texel neighborhood
		[unroll] for (y = -1.5; y <= 1.5; y += 1.0)
		{
			[unroll] for (x = -1.5; x <= 1.5; x += 1.0)
			{
				sum += shadowmap.SampleCmpLevelZero( samplerState, projectedTexCoords.xy + TexOffset(x,y) * scale, vShadowSamplingPos.z - bias);
			}
		}

		float shadowFactor = sum / 16.0;

		shadow *= shadowFactor;
#else
        shadow = shadowmap.SampleCmpLevelZero(samplerState, projectedTexCoords.xy, vShadowSamplingPos.z - bias);
#endif
    }

    return saturate(shadow);
}

float ComputeShadowValue(float2 uv, float3 wsPosition, Texture2D shadowmap, SamplerComparisonState samplerState, float distance, float vertLighting, matrix viewProj, float bias = 0.01f, float softnessScale = 1.0f)
{
    return ComputeShadowValueDirect(wsPosition, shadowmap, samplerState, vertLighting, viewProj, bias, softnessScale);
}

// Sample the cascades with soft edges and blending.
//
// Cascade selection follows the Kirides path: first select the primary cascade
// from the un-biased world position, then apply that cascade's own bias for the
// actual lookup. The next cascade is biased independently for the blend sample.
// Keeping selection separate from the small receiver offset prevents the bias
// itself from moving a fragment into a different cascade at the boundary.
float ComputeCascadedShadowValueSoft(float3 wsPosition, float3 wsNormal,
                                     float3 wsLightDirection, float vegetationReceiverMask,
                                     float viewSpaceZ, float vertLighting, float bias, float2 screenPos)
{
    float shadow = vertLighting;
    const int cascadeCount = GetRuntimeCascadeCount();

    int selectedCascade = -1;
    float4 selectedShadowPos;
    float2 selectedProjCoords;
    float selectedInBounds;
    float selectedBlendFactor = 0.0f;

    // The primary cascade is selected from the original receiver position.
    // This is the stable Kirides rule and avoids cascade changes caused only
    // by the normal-offset bias.
    [unroll]
    for (int c = 0; c < MAX_CSM_CASCADES; c++)
    {
        if (c >= cascadeCount) break;

        GetCascadeUVAndBounds(wsPosition, c, selectedShadowPos,
            selectedProjCoords, selectedInBounds, selectedBlendFactor);
        if (selectedInBounds > 0.5f)
        {
            selectedCascade = c;
            break;
        }
    }

    if (selectedCascade >= 0)
    {
        // Increase softness gradually with distance only on the hit path.
        float distanceFactor = saturate(abs(viewSpaceZ) / 5000.0f);
        float softness = SQ_ShadowSoftness * (1.0f + distanceFactor * 0.5f);

        float3 biasedPos = ApplyReceiverNormalBias(
            wsPosition, wsNormal, wsLightDirection,
            SQ_CascadeTexelSize[selectedCascade], vegetationReceiverMask);

        float4 cascadeShadowPos;
        float2 cascadeProjCoords;
        float cascadeInBounds;
        float cascadeBlendFactor;
        GetCascadeUVAndBounds(biasedPos, selectedCascade,
            cascadeShadowPos, cascadeProjCoords, cascadeInBounds, cascadeBlendFactor);

        shadow = SampleCascadeShadowSoft(
            cascadeShadowPos, cascadeProjCoords, selectedCascade, bias, screenPos, softness);

        if (selectedCascade < cascadeCount - 1 && selectedBlendFactor > 0.0f)
        {
            const int nextCascade = selectedCascade + 1;
            float3 nextBiasedPos = ApplyReceiverNormalBias(
                wsPosition, wsNormal, wsLightDirection,
                SQ_CascadeTexelSize[nextCascade], vegetationReceiverMask);

            float4 nextShadowPos;
            float2 nextProjCoords;
            float nextInBounds;
            float nextBlendFactor;

            GetCascadeUVAndBounds(nextBiasedPos, nextCascade,
                nextShadowPos, nextProjCoords, nextInBounds, nextBlendFactor);

            if (nextInBounds > 0.5f)
            {
                float shadowNext = SampleCascadeShadowSoft(
                    nextShadowPos, nextProjCoords, nextCascade, bias, screenPos, softness);
                shadow = lerp(shadow, shadowNext, selectedBlendFactor);
            }
        }

    }

    return shadow;
}

#endif
