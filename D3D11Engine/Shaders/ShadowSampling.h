#ifndef SHADOW_SAMPLING_H
#define SHADOW_SAMPLING_H

#ifndef MAX_CSM_CASCADES
#define MAX_CSM_CASCADES 4
#endif

#ifndef NUM_CSM_CASCADES
#define NUM_CSM_CASCADES 3
#endif

#ifndef CSM_PCF_LIMIT
#define CSM_PCF_LIMIT 3
#endif


#ifndef SHADOW_ATLAS
#define SHADOW_ATLAS 0
#endif


#ifndef SHD_BLUE_NOISE
#define SHD_BLUE_NOISE 0
#endif


#ifndef PCF_FILTER_TAPS_NEAR
#define PCF_FILTER_TAPS_NEAR 8
#endif

#ifndef PCF_FILTER_TAPS_FAR
#define PCF_FILTER_TAPS_FAR 4
#endif

#ifndef SHD_FILTER_MSM
#define SHD_FILTER_MSM 0
#endif

#if SHD_FILTER_MSM && !SHADOW_ATLAS
Texture2DArray TX_ShadowMomentArray : register(t14);
#endif

//--------------------------------------------------------------------------------------
// Shadow map sampling helpers
// Abstracts Texture2DArray (FL11+) vs Texture2D atlas (FL10) sampling
//--------------------------------------------------------------------------------------
#if SHADOW_ATLAS
// Convert cascade-local UV [0,1] to atlas UV with clamping to prevent seam bleeding
float2 CascadeToAtlasUV(float2 cascadeUV, int cascadeIndex)
{
    float4 rect = SQ_CascadeAtlasRect[cascadeIndex];
    float2 atlasUV = cascadeUV * rect.zw + rect.xy;

    // Clamp to cascade bounds with half-texel inset to prevent bilinear filter
    // from sampling texels in neighboring cascades
    // Atlas texel size = rect.zw / cascadePixelSize = 1/atlasSize (constant for all cascades)
    // Use rect.zw / SQ_ShadowmapSize as conservative estimate (correct for cascade 0,
    // slightly conservative for smaller cascades which is fine)
    float2 halfTexel = 0.5 * rect.zw / SQ_ShadowmapSize;
    float2 minUV = rect.xy + halfTexel;
    float2 maxUV = rect.xy + rect.zw - halfTexel;
    return clamp(atlasUV, minUV, maxUV);
}

float SampleShadowMapCmp(float2 cascadeUV, int cascadeIndex, float depth)
{
    float2 atlasUV = CascadeToAtlasUV(cascadeUV, cascadeIndex);
    return TX_ShadowmapAtlas.SampleCmpLevelZero(SS_Comp, atlasUV, depth);
}

float SampleShadowMapLevel(float2 cascadeUV, int cascadeIndex)
{
    float2 atlasUV = CascadeToAtlasUV(cascadeUV, cascadeIndex);
    return TX_ShadowmapAtlas.SampleLevel(SS_Linear, atlasUV, 0).r;
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


//--------------------------------------------------------------------------------------
// High-quality Poisson disk for shadow sampling
// Rotated per-pixel for better TAA integration and reduced banding
//--------------------------------------------------------------------------------------
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


// 8-tap Poisson disk for medium quality / distant cascades
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
#if SHD_BLUE_NOISE
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
#else
    // Deterministic fallback: never animate an undersampled shadow kernel.
    float2 seed = screenPos + float2((float)sampleOffset * 13.17f, (float)cascadeIndex * 7.31f);
    return frac(52.9829189f * frac(dot(seed, float2(0.06711056f, 0.00583715f))));
#endif
}

int GetBlueNoiseStartIndex(float2 screenPos, int cascadeIndex, int patternSize, int sampleOffset)
{
    int size = max(patternSize, 1);
#if SHD_BLUE_NOISE
    return (int)(GetShadowBlueNoise(screenPos, cascadeIndex, sampleOffset) * (float)size) % size;
#else
    return sampleOffset % size;
#endif
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
#if SHD_BLUE_NOISE
    return RotationMatrixFromNoise(GetShadowBlueNoise(screenPos, cascadeIndex, 0));
#else
    return float2x2(1.0f, 0.0f, 0.0f, 1.0f);
#endif
}

float2x2 GetPoissonRotationMatrix(float2 screenPos)
{
    return GetPoissonRotationMatrixForCascade(screenPos, 0);
}

float2x2 GetPoissonRotationMatrixRForCascade(float2 screenPos, int cascadeIndex, out float rawNoise)
{
#if SHD_BLUE_NOISE
    rawNoise = GetShadowBlueNoise(screenPos, cascadeIndex, 0);
    return RotationMatrixFromNoise(rawNoise);
#else
    rawNoise = 0.5f;
    return float2x2(1.0f, 0.0f, 0.0f, 1.0f);
#endif
}

float2x2 GetPoissonRotationMatrixR(float2 screenPos, out float rawNoise)
{
    return GetPoissonRotationMatrixRForCascade(screenPos, 0, rawNoise);
}


float SampleShadowMapDepthLitBorder(float2 cascadeUV, int cascadeIndex)
{
    if (cascadeUV.x < 0.0f || cascadeUV.x > 1.0f ||
        cascadeUV.y < 0.0f || cascadeUV.y > 1.0f)
    {
        return 1.0f;
    }

    float halfTexel = 0.5f / SQ_ShadowmapSize;
    float2 safeUV = clamp(cascadeUV, halfTexel, 1.0f - halfTexel);
    return SampleShadowMapLevel(safeUV, cascadeIndex);
}

#if SHD_FILTER_MSM && !SHADOW_ATLAS
float4 SampleShadowMomentsLitBorder(float2 cascadeUV, int cascadeIndex, float mipLevel)
{
    if (cascadeUV.x < 0.0f || cascadeUV.x > 1.0f ||
        cascadeUV.y < 0.0f || cascadeUV.y > 1.0f)
    {
        return float4(1.0f, 0.99755993f, 0.89343751f, 0.0f);
    }

    float halfTexel = 0.5f / SQ_ShadowmapSize;
    float2 safeUV = clamp(cascadeUV, halfTexel, 1.0f - halfTexel);
    return TX_ShadowMomentArray.SampleLevel(
        SS_Linear, float3(safeUV, (float)cascadeIndex), mipLevel);
}
#endif

#if SHD_FILTER_MSM && !SHADOW_ATLAS
float4 DecodeShadowMoments(float4 optimized)
{
    optimized.x -= 0.0359558848f;
    return mul(optimized, float4x4(
        0.2227744146f,  0.1549679261f,  0.1451988946f,  0.1631274430f,
        0.0771972861f,  0.1394629426f,  0.2120202157f,  0.2591432266f,
        0.7926986636f,  0.7963415838f,  0.7258694464f,  0.6539092497f,
        0.0319417555f, -0.1722823173f, -0.2758014811f, -0.3376131734f));
}

float ReduceMSMLightBleeding(float litProbability)
{
    const float bleedReduction = 0.04f;
    return saturate((litProbability - bleedReduction) / (1.0f - bleedReduction));
}

float EstimateMSMLit(float4 moments, float receiverDepth)
{
    moments = saturate(DecodeShadowMoments(moments));
    receiverDepth = saturate(receiverDepth);

    // Hamburger four-moment reconstruction. A tiny bias towards a broad
    // distribution keeps the Hankel matrix invertible on nearly flat texels.
    moments = lerp(moments, float4(0.5f, 0.5f, 0.5f, 0.5f), 0.00003f);

    float L32D22 = moments.z - moments.x * moments.y;
    float D22 = moments.y - moments.x * moments.x;
    float squaredDepthVariance = moments.w - moments.y * moments.y;
    float D33D22 = squaredDepthVariance * D22 - L32D22 * L32D22;

    // A single-depth texel has a singular moment matrix. The two-moment bound
    // is the stable limiting solution for that case.
    float depthDelta = receiverDepth - moments.x;
    float variance = max(D22, 0.00002f);
    float vsmFallback = variance / (variance + depthDelta * depthDelta);
    if (D22 <= 0.000001f || D33D22 <= 0.000001f)
        return ReduceMSMLightBleeding(vsmFallback);

    float invD22 = rcp(D22);
    float L32 = L32D22 * invD22;
    float3 coefficients = float3(1.0f, receiverDepth, receiverDepth * receiverDepth);
    coefficients.y -= moments.x;
    coefficients.z -= moments.y + L32 * coefficients.y;
    coefficients.y *= invD22;
    coefficients.z *= D22 / D33D22;
    coefficients.y -= L32 * coefficients.z;
    coefficients.x -= dot(coefficients.yz, moments.xy);

    if (abs(coefficients.z) <= 0.000001f)
        return ReduceMSMLightBleeding(vsmFallback);

    float p = coefficients.y / coefficients.z;
    float q = coefficients.x / coefficients.z;
    float discriminant = p * p * 0.25f - q;
    if (discriminant < 0.0f)
        return ReduceMSMLightBleeding(vsmFallback);

    float root = sqrt(discriminant);
    float z1 = -p * 0.5f - root;
    float z2 = -p * 0.5f + root;

    float4 switchValue;
    if (z2 < receiverDepth)
        switchValue = float4(z1, receiverDepth, 1.0f, 1.0f);
    else if (z1 < receiverDepth)
        switchValue = float4(receiverDepth, z1, 0.0f, 1.0f);
    else
        switchValue = 0.0f;

    float denominator = (z2 - switchValue.y) * (receiverDepth - z1);
    if (abs(denominator) <= 0.000001f)
        return ReduceMSMLightBleeding(vsmFallback);

    float quotient = (switchValue.x * z2
        - moments.x * (switchValue.x + z2) + moments.y) / denominator;
    float shadowed = switchValue.z + switchValue.w * quotient;
    return ReduceMSMLightBleeding(1.0f - shadowed);
}

float SampleMSMBlockerDepth(float2 cascadeUV, int cascadeIndex)
{
    if (cascadeUV.x < 0.0f || cascadeUV.x > 1.0f ||
        cascadeUV.y < 0.0f || cascadeUV.y > 1.0f)
    {
        return 1.0f;
    }

    int shadowSize = max((int)SQ_ShadowmapSize, 1);
    int2 pixel = clamp(int2(cascadeUV * (float)shadowSize),
        int2(0, 0), int2(shadowSize - 1, shadowSize - 1));
    return TX_ShadowmapArray.Load(int4(pixel, cascadeIndex, 0)).r;
}

float GetMSMCascadeWorldTexelSize(int cascadeIndex)
{
    matrix shadowViewProj = SQ_ShadowViewProj[cascadeIndex];
    float shadowScaleX = length(float3(shadowViewProj[0][0], shadowViewProj[1][0], shadowViewProj[2][0]));
    float shadowScaleY = length(float3(shadowViewProj[0][1], shadowViewProj[1][1], shadowViewProj[2][1]));
    float worldSpanX = (shadowScaleX > 1e-6f) ? (2.0f / shadowScaleX) : 0.0f;
    float worldSpanY = (shadowScaleY > 1e-6f) ? (2.0f / shadowScaleY) : 0.0f;
    return 0.5f * (worldSpanX + worldSpanY) / max(SQ_ShadowmapSize, 1.0f);
}

float GetMSMCascadeWorldDepthSpan(int cascadeIndex)
{
    matrix shadowViewProj = SQ_ShadowViewProj[cascadeIndex];
    float depthScale = length(float3(
        shadowViewProj[0][2], shadowViewProj[1][2], shadowViewProj[2][2]));
    return (depthScale > 1e-7f) ? rcp(depthScale) : 1.0f;
}

void FindMSMBlockers(float2 uv, float receiverDepth, int cascadeIndex,
                     float searchRadius, float2x2 rotMat, float2 screenPos,
                     out float averageBlockerDepth, out float blockerCount)
{
    float blockerDepthSum = 0.0f;
    blockerCount = 0.0f;

    float centerDepth = SampleMSMBlockerDepth(uv, cascadeIndex);
    if (centerDepth < receiverDepth)
    {
        blockerDepthSum += centerDepth;
        blockerCount += 1.0f;
    }

    int startIdx = GetBlueNoiseStartIndex(screenPos, cascadeIndex, 16, 47);
    [unroll]
    for (int i = 0; i < 6; ++i)
    {
        int sampleIdx = (startIdx + i * 5) & 15;
        float2 disk = mul(rotMat, g_PoissonDisk16[sampleIdx]);
        float blockerDepth = SampleMSMBlockerDepth(uv + disk * searchRadius, cascadeIndex);
        if (blockerDepth < receiverDepth)
        {
            blockerDepthSum += blockerDepth;
            blockerCount += 1.0f;
        }
    }

    averageBlockerDepth = blockerDepthSum / max(blockerCount, 1.0f);
}

float SampleCascadeShadowMSM(float4 vShadowSamplingPos, float2 projectedTexCoords,
                             int cascadeIndex, float bias, float2 screenPos, float softness)
{
    if (projectedTexCoords.x < 0.0f || projectedTexCoords.x > 1.0f ||
        projectedTexCoords.y < 0.0f || projectedTexCoords.y > 1.0f)
    {
        return 1.0f;
    }

    float texelSize = 1.0f / SQ_ShadowmapSize;
    float receiverDepth = saturate(vShadowSamplingPos.z - bias);
    float baseRadiusTexels = max(0.75f, softness * 0.90f);
    float searchRadiusTexels = clamp(2.0f + softness * 2.0f, 3.0f, 12.0f);
    float2x2 rotMat = GetPoissonRotationMatrixForCascade(screenPos, cascadeIndex);
    float centerLit = EstimateMSMLit(
        SampleShadowMomentsLitBorder(projectedTexCoords, cascadeIndex, 0.0f),
        receiverDepth);

    float averageBlockerDepth;
    float blockerCount;
    FindMSMBlockers(projectedTexCoords, receiverDepth, cascadeIndex,
        searchRadiusTexels * texelSize, rotMat, screenPos,
        averageBlockerDepth, blockerCount);
    if (blockerCount <= 0.0f)
        return centerLit;

    float filterRadiusTexels = baseRadiusTexels;
    if (blockerCount > 0.0f)
    {
        float worldDepthSeparation = max(receiverDepth - averageBlockerDepth, 0.0f)
            * GetMSMCascadeWorldDepthSpan(cascadeIndex);
        float worldTexelSize = max(GetMSMCascadeWorldTexelSize(cascadeIndex), 0.001f);
        float separationInTexels = worldDepthSeparation / worldTexelSize;
        float penumbraGrowth = separationInTexels * 0.035f * max(softness, 0.5f);
        filterRadiusTexels = clamp(baseRadiusTexels + penumbraGrowth,
            baseRadiusTexels, 24.0f);
    }
    // MSM is filterable, but the core query has to remain authoritative.
    // Broad mips are only blended into actual penumbras, so umbrae do not wash out.
    float momentMip = clamp(log2(max(filterRadiusTexels, 1.0f)) - 1.25f, 0.0f, 4.0f);
    if (momentMip <= 0.001f)
        return centerLit;

    float filteredLit = EstimateMSMLit(
        SampleShadowMomentsLitBorder(projectedTexCoords, cascadeIndex, momentMip),
        receiverDepth);
    float penumbraBlend = saturate((filterRadiusTexels - baseRadiusTexels) / 8.0f);
    float corePreservation = saturate((0.35f - centerLit) / 0.35f);
    float lit = lerp(centerLit, filteredLit, penumbraBlend);
    return lerp(lit, min(lit, centerLit), corePreservation);
}
#endif

#if SHADOW_ATLAS
float IsInShadow(float3 wsPosition, Texture2D shadowmapAtlas, SamplerComparisonState samplerState)
{
    float4 vShadowSamplingPos = mul(float4(wsPosition, 1), SQ_ShadowViewProj[0]);
    // vShadowSamplingPos.xyz /= vShadowSamplingPos.www; // no need for perspective divide, as this is an orthographic sun light
	
    float2 projectedTexCoords = vShadowSamplingPos.xy * float2(0.5f, -0.5f) + float2(0.5f, 0.5f);
    return SampleShadowMapCmp(projectedTexCoords.xy, 0, vShadowSamplingPos.z);
}
#else
float IsInShadow(float3 wsPosition, Texture2DArray shadowmapArray, SamplerComparisonState samplerState)
{
    float4 vShadowSamplingPos = mul(float4(wsPosition, 1), SQ_ShadowViewProj[0]);
    // vShadowSamplingPos.xyz /= vShadowSamplingPos.www; // no need for perspective divide, as this is an orthographic sun light
	
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

//--------------------------------------------------------------------------------------
// Helper: Get shadow map UV and check if position is within cascade bounds
// Returns: projectedTexCoords in xy, isInBounds as 0 or 1 in z, blend factor in w
//--------------------------------------------------------------------------------------
void GetCascadeUVAndBounds(float3 wsPosition, int cascadeIndex, 
                           out float4 vShadowSamplingPos, out float2 projectedTexCoords, 
                           out float inBounds, out float blendFactor)
{
    matrix viewProj = SQ_ShadowViewProj[cascadeIndex];
    
    // Calculate once and pass out
    vShadowSamplingPos = mul(float4(wsPosition, 1), viewProj);
    projectedTexCoords = vShadowSamplingPos.xy * float2(0.5f, -0.5f) + float2(0.5f, 0.5f);
    
    // Keep a small filter-safe XY inset. Z must also lie inside this cascade;
    // otherwise selection falls through to a farther cascade instead of producing a dark band.
    const float margin = 1.5f / SQ_ShadowmapSize;
    bool isInBounds = projectedTexCoords.x > margin && projectedTexCoords.x < (1.0f - margin) &&
                      projectedTexCoords.y > margin && projectedTexCoords.y < (1.0f - margin) &&
                      vShadowSamplingPos.z >= 0.0f && vShadowSamplingPos.z <= 1.0f;
    inBounds = isInBounds ? 1.0f : 0.0f;
    
    // Calculate blend factor based on distance to edge
    // Wide blend zone (30%) with smoothstep for gradual cascade transitions
    const float blendZoneStart = 0.30f;
    float distToEdge = min(min(projectedTexCoords.x, 1.0f - projectedTexCoords.x),
                           min(projectedTexCoords.y, 1.0f - projectedTexCoords.y));
    blendFactor = 1.0f - smoothstep(margin, blendZoneStart, distToEdge);
}

//--------------------------------------------------------------------------------------
// Returns the first cascade that contains wsPosition. If no cascade contains the
// position, returns -1.
//--------------------------------------------------------------------------------------
int GetPrimaryCascadeIndex(float3 wsPosition)
{
    float4 vShadowPos;
    float2 projCoords;
    float inBounds;
    float blendFactor;

    for (int c = 0; c < NUM_CSM_CASCADES; c++)
    {
        GetCascadeUVAndBounds(wsPosition, c, vShadowPos, projCoords, inBounds, blendFactor);
        if (inBounds > 0.5f)
            return c;
    }

    return -1;
}

//--------------------------------------------------------------------------------------
// Estimates current-cascade world-space texel size from the orthographic shadow matrix.
//--------------------------------------------------------------------------------------
float GetCascadeWorldTexelSize(int cascadeIndex)
{
    if (cascadeIndex < 0)
        return 0.0f;

    matrix shadowViewProj = SQ_ShadowViewProj[cascadeIndex];

    float shadowScaleX = length(float3(shadowViewProj[0][0], shadowViewProj[1][0], shadowViewProj[2][0]));
    float shadowScaleY = length(float3(shadowViewProj[0][1], shadowViewProj[1][1], shadowViewProj[2][1]));

    float worldSpanX = (shadowScaleX > 1e-6f) ? (2.0f / shadowScaleX) : 0.0f;
    float worldSpanY = (shadowScaleY > 1e-6f) ? (2.0f / shadowScaleY) : 0.0f;

    float cascadeResolution = SQ_ShadowmapSize;
#if SHADOW_ATLAS
    float4 atlasRect = SQ_CascadeAtlasRect[cascadeIndex];
    cascadeResolution *= max(atlasRect.z, atlasRect.w);
#endif

    return 0.5f * (worldSpanX + worldSpanY) / max(cascadeResolution, 1.0f);
}

//--------------------------------------------------------------------------------------
// High-quality shadow sampling with configurable softness
// Uses rotated Poisson disk for TAA-friendly results
//--------------------------------------------------------------------------------------
float SampleCascadeShadowSoft(float4 vShadowSamplingPos, float2 projectedTexCoords, 
                              int cascadeIndex, float bias, float2 screenPos, float softness)
{
    if (projectedTexCoords.x < 0.0f || projectedTexCoords.x > 1.0f ||
        projectedTexCoords.y < 0.0f || projectedTexCoords.y > 1.0f)
    {
        return 1.0f;
    }
    
    float shadow = 1.0f;
    float texelSize = 1.0f / SQ_ShadowmapSize;
    
    // Scale the filter radius based on softness setting
    // softness of 1.0 = default, < 1.0 = sharper, > 1.0 = softer
    float filterRadius = texelSize * softness;

#if SHD_FILTER_MSM && !SHADOW_ATLAS
    return SampleCascadeShadowMSM(vShadowSamplingPos, projectedTexCoords, cascadeIndex, bias, screenPos, softness);
#elif SHD_FILTER_16TAP_PCF
#if NUM_CSM_CASCADES <= 1
    // Single cascade - use near cascade quality profile.
    float2x2 rotMat = GetPoissonRotationMatrixForCascade(screenPos, cascadeIndex);
    float sum = 0.0f;
    int startIdx = GetBlueNoiseStartIndex(screenPos, cascadeIndex, 16, 23);
    
    [unroll]
    for (int i = 0; i < PCF_FILTER_TAPS_NEAR; i++)
    {
        int sampleIdx = (startIdx + i * 5) & 15;
        float2 offset = mul(rotMat, g_PoissonDisk16[sampleIdx]) * filterRadius;
        sum += SampleShadowMapCmp(
            projectedTexCoords.xy + offset, cascadeIndex,
            vShadowSamplingPos.z - bias);
    }
    shadow = sum / (float)max(PCF_FILTER_TAPS_NEAR, 1);
#else
    // Multiple cascades - use quality based on cascade index
    if (cascadeIndex < CSM_PCF_LIMIT) 
    {
        // High quality for close cascades.
        float2x2 rotMat = GetPoissonRotationMatrixForCascade(screenPos, cascadeIndex);
        float sum = 0.0f;
        int startIdx = GetBlueNoiseStartIndex(screenPos, cascadeIndex, 16, 29);
        
        [unroll]
        for (int i = 0; i < PCF_FILTER_TAPS_NEAR; i++)
        {
            int sampleIdx = (startIdx + i * 5) & 15;
            float2 offset = mul(rotMat, g_PoissonDisk16[sampleIdx]) * filterRadius;
            sum += SampleShadowMapCmp(
                projectedTexCoords.xy + offset, cascadeIndex,
                vShadowSamplingPos.z - bias);
        }
        shadow = sum / (float)max(PCF_FILTER_TAPS_NEAR, 1);
    } 
    else 
    {
        // Reduced quality profile for distant cascades.
        float2x2 rotMat = GetPoissonRotationMatrixForCascade(screenPos, cascadeIndex);
        float sum = 0.0f;
        int startIdx = GetBlueNoiseStartIndex(screenPos, cascadeIndex, 8, 31);
        
        [unroll]
        for (int i = 0; i < PCF_FILTER_TAPS_FAR; i++)
        {
            int sampleIdx = (startIdx + i * 3) & 7;
            float2 offset = mul(rotMat, g_PoissonDisk8[sampleIdx]) * filterRadius;
            sum += SampleShadowMapCmp(
                projectedTexCoords.xy + offset, cascadeIndex,
                vShadowSamplingPos.z - bias);
        }
        shadow = sum / (float)max(PCF_FILTER_TAPS_FAR, 1);
    }
#endif
#else
    // No PCF filtering - single sample (still uses bias)
    shadow = SampleShadowMapCmp(
        projectedTexCoords.xy, cascadeIndex,
        vShadowSamplingPos.z - bias);
#endif
    
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
    // vShadowSamplingPos.xyz /= vShadowSamplingPos.www; // no need for perspective divide, as this is an orthographic sun light
	
    float2 projectedTexCoords = vShadowSamplingPos.xy * float2(0.5f, -0.5f) + float2(0.5f, 0.5f);
    float shadow = 1.0f;
    
    // Sample shadow map if within valid bounds
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

//--------------------------------------------------------------------------------------
// CSM: Shadow-Sampling with soft shadows and cascade blending
// Uses SQ_ShadowSoftness for configurable shadow edge softness
//--------------------------------------------------------------------------------------
float ComputeCascadedShadowValueSoft(float3 wsPosition, float viewSpaceZ, float vertLighting, float bias, float2 screenPos)
{
    float shadow = vertLighting;
    // Apply distance-based softness scaling
    // Shadows get slightly softer with distance (simulating penumbra growth)
    float distanceFactor = saturate(abs(viewSpaceZ) / 5000.0f);
    float softness = SQ_ShadowSoftness * (1.0f + distanceFactor * 0.5f);

    int selectedCascade = -1;
    float4 vShadowPos;
    float2 projCoords;
    float blendFactor = 0.0f;

    // 1. Find the primary cascade WITHOUT sampling textures
    for (int c = 0; c < NUM_CSM_CASCADES; c++)
    {
        float inBounds;
        GetCascadeUVAndBounds(wsPosition, c, vShadowPos, projCoords, inBounds, blendFactor);
        
        if (inBounds > 0.5f) 
        {
            selectedCascade = c;
            break; // Standard break without [unroll] is safe and highly efficient
        }
    }

    // 2. Only sample textures if a valid cascade was found
    if (selectedCascade >= 0)
    {
        shadow = SampleCascadeShadowSoft(vShadowPos, projCoords, selectedCascade, bias, screenPos, softness);
        
        // 3. Check if we need to blend with the next cascade
        if (selectedCascade < NUM_CSM_CASCADES - 1 && blendFactor > 0.0f)
        {
            float4 nextShadowPos;
            float2 nextProjCoords;
            float nextInBounds;
            float nextBlendFactor;
            
            GetCascadeUVAndBounds(wsPosition, selectedCascade + 1, nextShadowPos, nextProjCoords, nextInBounds, nextBlendFactor);
            
            if (nextInBounds > 0.5f)
            {
                float shadowNext = SampleCascadeShadowSoft(nextShadowPos, nextProjCoords, selectedCascade + 1, bias, screenPos, softness);
                shadow = lerp(shadow, shadowNext, blendFactor);
            }
        }
    }
    
    return shadow;
}



#endif
