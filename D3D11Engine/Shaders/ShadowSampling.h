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
float4 SampleShadowMomentsLitBorder(float2 cascadeUV, int cascadeIndex)
{
    if (cascadeUV.x < 0.0f || cascadeUV.x > 1.0f ||
        cascadeUV.y < 0.0f || cascadeUV.y > 1.0f)
    {
        return float4(0.0f, 0.0f, 0.9811252243f, 1.0f);
    }

    float halfTexel = 0.5f / SQ_ShadowmapSize;
    float2 safeUV = clamp(cascadeUV, halfTexel, 1.0f - halfTexel);
    return TX_ShadowMomentArray.SampleLevel(
        SS_Linear, float3(safeUV, (float)cascadeIndex), 0.0f);
}

float4 DecodeShadowMoments(float4 optimized)
{
    // Inverse of the sparse signed-depth transform used by PS_ShadowMoments.
    float odd0 = optimized.x - 0.5f;
    float odd1 = (optimized.z - 0.5f) * 0.5773502692f;
    float b1 = 3.0f * odd1 - odd0 / 3.0f;
    float b3 = (9.0f * odd1 - 3.0f * odd0) * 0.25f;
    float b2 = optimized.w + optimized.y * 0.125f;
    float b4 = optimized.w - optimized.y * 0.125f;
    return float4(b1, b2, b3, b4);
}

float EstimateMSMLit(float4 decodedMoments, float receiverDepth)
{
    // Hamburger 4MSM from Peters/Klein. The moment bias makes the Hankel
    // matrix positive definite after 16-bit quantization and filtering.
    float4 b = lerp(decodedMoments, float4(0.0f, 0.628f, 0.0f, 0.628f), 0.00006f);
    float z0 = clamp(receiverDepth, -1.0f, 1.0f);

    float L32D22 = mad(-b.x, b.y, b.z);
    float D22 = max(mad(-b.x, b.x, b.y), 1e-7f);
    float squaredDepthVariance = mad(-b.y, b.y, b.w);
    float D33D22 = max(dot(float2(squaredDepthVariance, -L32D22),
        float2(D22, L32D22)), 1e-10f);
    float invD22 = rcp(D22);
    float L32 = L32D22 * invD22;

    float3 c = float3(1.0f, z0, z0 * z0);
    c.y -= b.x;
    c.z -= b.y + L32 * c.y;
    c.y *= invD22;
    c.z *= D22 / D33D22;
    c.y -= L32 * c.z;
    c.x -= dot(c.yz, b.xy);

    float safeC2 = (abs(c.z) < 1e-7f) ? (c.z < 0.0f ? -1e-7f : 1e-7f) : c.z;
    float p = c.y / safeC2;
    float q = c.x / safeC2;
    float root = sqrt(max(p * p * 0.25f - q, 0.0f));
    float z1 = -p * 0.5f - root;
    float z2 = -p * 0.5f + root;

    float4 switchValue = (z2 < z0)
        ? float4(z1, z0, 1.0f, 1.0f)
        : ((z1 < z0) ? float4(z0, z1, 0.0f, 1.0f) : float4(0.0f, 0.0f, 0.0f, 0.0f));
    float denominator = (z2 - switchValue.y) * (z0 - z1);
    denominator = (abs(denominator) < 1e-7f)
        ? (denominator < 0.0f ? -1e-7f : 1e-7f)
        : denominator;
    float quotient = (switchValue.x * z2
        - b.x * (switchValue.x + z2) + b.y) / denominator;
    float shadowIntensity = saturate(switchValue.z + switchValue.w * quotient);
    // The reference implementation removes the faint final two percent of
    // light leaking before converting shadow intensity back to visibility.
    shadowIntensity = saturate(shadowIntensity / 0.98f);
    return 1.0f - shadowIntensity;
}

float GetMSMCascadeWorldDepthSpan(int cascadeIndex)
{
    matrix shadowViewProj = SQ_ShadowViewProj[cascadeIndex];
    float depthScale = length(float3(
        shadowViewProj[0][2], shadowViewProj[1][2], shadowViewProj[2][2]));
    return (depthScale > 1e-7f) ? rcp(depthScale) : 1.0f;
}

float SampleCascadeShadowMSM(float4 vShadowSamplingPos, float2 projectedTexCoords,
                             int cascadeIndex, float bias, float2 screenPos, float softness)
{
    if (projectedTexCoords.x < 0.0f || projectedTexCoords.x > 1.0f ||
        projectedTexCoords.y < 0.0f || projectedTexCoords.y > 1.0f)
    {
        return 1.0f;
    }

    float receiverDepth = saturate(vShadowSamplingPos.z - bias);
    float signedReceiverDepth = receiverDepth * 2.0f - 1.0f;
    float hardShadow = SampleShadowMapCmp(projectedTexCoords, cascadeIndex, vShadowSamplingPos.z - bias);

    // MSM stays active even at the leftmost softness step. A tiny footprint gives
    // stable moment antialiasing without turning the zero-softness setting mushy.
    float momentWeight = lerp(0.38f, 1.0f, smoothstep(0.0f, 0.24f, softness));
    float filterRadius = (0.18f + softness * 0.92f) / SQ_ShadowmapSize;
    float2 offset = float2(filterRadius, filterRadius);

    // Four bilinear moment reads form a stable separable box footprint. This
    // replaces the old depth blocker search and coarse mip selection.
    float4 optimizedMoments =
        SampleShadowMomentsLitBorder(projectedTexCoords + float2(-offset.x, -offset.y), cascadeIndex) +
        SampleShadowMomentsLitBorder(projectedTexCoords + float2( offset.x, -offset.y), cascadeIndex) +
        SampleShadowMomentsLitBorder(projectedTexCoords + float2(-offset.x,  offset.y), cascadeIndex) +
        SampleShadowMomentsLitBorder(projectedTexCoords + float2( offset.x,  offset.y), cascadeIndex);
    float4 decodedMoments = DecodeShadowMoments(optimizedMoments * 0.25f);
    float msmLit = EstimateMSMLit(decodedMoments, signedReceiverDepth);

    // A wide moment kernel may leak light into short-range contact shadows.
    // Keep the hardware comparison authoritative near the blocker and release
    // it smoothly as receiver/blocker separation grows into a real penumbra.
    float worldDepthSeparation = max(signedReceiverDepth - decodedMoments.x, 0.0f)
        * (0.5f * GetMSMCascadeWorldDepthSpan(cascadeIndex));
    float contactRelease = smoothstep(15.0f, 150.0f, worldDepthSeparation);
    float contactPreservedLit = lerp(min(msmLit, hardShadow), msmLit, contactRelease);
    return lerp(hardShadow, contactPreservedLit, momentWeight);
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
