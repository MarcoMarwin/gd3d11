// Shared Forward+ lighting helpers.
#ifndef FORWARD_PLUS_LIGHTING_H
#define FORWARD_PLUS_LIGHTING_H

#ifndef MAX_CSM_CASCADES
#define MAX_CSM_CASCADES 4
#endif

#ifndef SHD_FILTER_PCSS
#define SHD_FILTER_PCSS 0
#endif

#ifndef SHADOW_ATLAS
#define SHADOW_ATLAS 0
#endif

// ============================================
// Constant Buffers
// ============================================

// Sun / CSM data (same layout as DS_ScreenQuadConstantBuffer in C++)
// Placed at b4 to avoid conflict with FFPipelineConstantBuffer at b0
cbuffer FP_ScreenQuadConstantBuffer : register( b4 )
{
    float4 SQ_ProjParams;
    matrix SQ_InvView;
    matrix SQ_View;
    matrix SQ_RainViewProj;
    float3 SQ_LightDirectionVS;
    float SQ_ShadowmapSize;
    float4 SQ_LightColor;
    matrix SQ_ShadowViewProj[MAX_CSM_CASCADES];
    float SQ_ShadowStrength;
    float SQ_ShadowAOStrength;
    float SQ_WorldAOStrength;
    float SQ_ShadowSoftness;
    uint SQ_FrameIndex;
    float2 SQ_JitterOffset;
    float SQ_LightSize;
    float4 SQ_CascadeAtlasRect[MAX_CSM_CASCADES];
    float4 SQ_CascadeLightDirectionWS[MAX_CSM_CASCADES];
    float4 SQ_ShadowRuntimeParams;
    float4 SQ_ShadowCascadeRuntimeParams;
    float4 SQ_ShadowCascadeSplits;
    float4 SQ_CascadeShadowResolution;
    float4 SQ_ShadowAtlasSize;
};

// Forward+ tile data
cbuffer FP_TileConstantBuffer : register( b5 )
{
    float2 FP_ViewportSize;
    uint FP_NumTilesX;
    uint FP_LimitLightIntensity;
    float FP_ClusterNearZ;
    float FP_ClusterFarZ;
    float2 FP_TilePad;
};

// ============================================
// Textures and Samplers
// ============================================

// CSM shadow map (t3)
#if SHADOW_ATLAS
Texture2D TX_ShadowmapAtlas : register( t3 );
#else
Texture2DArray TX_ShadowmapArray : register( t3 );
#endif

Texture2D TX_ShadowBlueNoise : register( t6 );

// Comparison sampler for shadow maps
SamplerComparisonState SS_Comp : register( s2 );
// Linear sampler for shadow map level sampling

#define FP_SS_Linear SS_Linear
// SamplerState FP_SS_Linear : register( s0 );


#include "ShadowSampling.h"
#include "PointLightShadows.h"

// ============================================
// Point Light Structures & Resources
// ============================================

#ifdef TILE_SIZE
#define FP_TILE_SIZE TILE_SIZE
#else
#define FP_TILE_SIZE 16
#endif
#define FP_NUM_Z_SLICES 16u
#define FP_MASK_WORDS 32u

struct TiledPointLight
{
    float3 PositionView;
    float Range;
    float4 Color;
    float3 PositionWorld;
    int ShadowCubeIndex;
    float ShadowStrength;
    float IsIndoor;
    float IgnoreIndoorOutdoorLimit;
    float ShadowSoftness;
    uint ShadowFilterMode;
    uint3 ShadowFilterPad;
};

struct LightGrid
{
    uint WordOccupancy;
    uint Mask[FP_MASK_WORDS];
};

StructuredBuffer<TiledPointLight> FP_Lights : register( t8 );
StructuredBuffer<LightGrid> FP_LightGrid : register( t9 );
TextureCubeArray FP_ShadowCubeArray : register( t11 );
TextureCubeArray FP_DynamicShadowCubeArray : register( t21 );
TextureCubeArray FP_StaticLowShadowCubeArray : register( t20 );
// ============================================
// Point Light Accumulation (matches CS_TiledShading.hlsl)
// ============================================

float3 FP_ComputePointLighting(
    float3 wsPosition, float3 vsPosition, float3 normal,
    float4 diffuseColor, float specIntensity, float specPower,
    float2 screenPos, float twoSidedBacklitMaterial, float npcMaterial,
    float vegetationBacklitMask )
{
    uint tileX = (uint)screenPos.x / FP_TILE_SIZE;
    uint tileY = (uint)screenPos.y / FP_TILE_SIZE;
    uint tileIndex = tileY * FP_NumTilesX + tileX;
    float zView = abs( vsPosition.z );
    float zSliceT = log2( max( zView, FP_ClusterNearZ ) / FP_ClusterNearZ )
        / log2( FP_ClusterFarZ / FP_ClusterNearZ );
    uint slice = (uint)clamp( floor( zSliceT * (float)FP_NUM_Z_SLICES ), 0.0f, (float)( FP_NUM_Z_SLICES - 1u ) );
    uint cluster = tileIndex * FP_NUM_Z_SLICES + slice;
    float3 totalLighting = float3( 0, 0, 0 );
    float3 maxLighting = float3( 0, 0, 0 );

    // These only need to be calculated once per pixel
    float3 V = normalize( -vsPosition );
    float specMod = PLS_ComputeSpecMod( diffuseColor.rgb );
    float3 wsNormal = normalize( mul( float4( normal, 0 ), SQ_InvView ).xyz );
    
    uint wordMask = FP_LightGrid[cluster].WordOccupancy;
    while ( wordMask != 0 )
    {
        uint word = firstbitlow( wordMask );
        wordMask &= wordMask - 1;
        uint lightMask = FP_LightGrid[cluster].Mask[word];
        while ( lightMask != 0 )
        {
            uint bit = firstbitlow( lightMask );
            lightMask &= lightMask - 1;
            uint lightIdx = word * 32u + bit;
            TiledPointLight light = FP_Lights[lightIdx];

        float3 lightDir = light.PositionView - vsPosition;
        float distance = length( lightDir );
        
        if ( distance >= light.Range )
            continue;
            
        lightDir /= distance;

        float ndl = PLS_ComputePointLightNdlBacklit( lightDir, normal, light.PositionWorld, wsPosition, wsNormal, twoSidedBacklitMaterial, AC_EnableSSS );
        
        // instead of pow(..., 1.2f) we use a fast quadratic-like approach.
        float falloff = PLS_ComputeRangeFalloff( distance, light.Range );

        float3 H = normalize( lightDir + V );
        float spec = PLS_CalcBlinnPhongLighting( normal, H );
        float3 lighting = PLS_ComputePointLightLightingBacklit(
            diffuseColor.rgb, light.Color.rgb, ndl, falloff, spec, specIntensity, specPower, specMod,
            lightDir, normal, V, vegetationBacklitMask, twoSidedBacklitMaterial,
            AC_EnableSSS, AC_SSSIntensity, 0.42f );

        // Don't fetch shadows if the light contribution is effectively zero.
        if ( light.ShadowCubeIndex >= 0 && any(lighting > 0.001f)
            && (npcMaterial <= 0.5f || light.ShadowFilterPad[0] == 0u) )
        {
            const int shadowSlot = light.ShadowCubeIndex & 0x1fffffff;
            const bool lowStatic = (light.ShadowCubeIndex & 0x20000000) != 0;
            float shadow;
            if ( lowStatic )
                shadow = PLS_SampleShadowCubeArray( FP_StaticLowShadowCubeArray, FP_SS_Linear, SS_Comp, wsPosition, wsNormal, light.PositionWorld, light.Range, shadowSlot, light.ShadowSoftness, light.ShadowFilterMode );
            else
                shadow = PLS_SampleShadowCubeArray( FP_ShadowCubeArray, FP_SS_Linear, SS_Comp, wsPosition, wsNormal, light.PositionWorld, light.Range, shadowSlot, light.ShadowSoftness, light.ShadowFilterMode );
            if ( shadow > 0.001f && (light.ShadowCubeIndex & 0x40000000) != 0 )
            {
                shadow *= PLS_SampleShadowCubeArray( FP_DynamicShadowCubeArray, FP_SS_Linear, SS_Comp, wsPosition, wsNormal, light.PositionWorld, light.Range, shadowSlot, light.ShadowSoftness, light.ShadowFilterMode );
            }
            lighting *= lerp(1.0f, shadow, saturate(light.ShadowStrength));
        }

        // Keep indoor lights off outdoor geometry without the former discrete
        // multi-ring doorway search that produced contour bands.
        float indoorPixel = diffuseColor.a < 0.5f ? 1.0f : 0.0f;
        float indoorBoundary = saturate((1.0f - light.IsIndoor) + light.IsIndoor * indoorPixel);
        lighting *= lerp(indoorBoundary, 1.0f, saturate(light.IgnoreIndoorOutdoorLimit));

        lighting = saturate( lighting );
        totalLighting += lighting;
        maxLighting = max( maxLighting, lighting );
        }
    }

    return FP_LimitLightIntensity ? maxLighting : totalLighting;
}

// ============================================
// Sun Lighting (matches PS_DS_AtmosphericScattering.hlsl PSMain)
// ============================================

float3 FP_ComputeSunLighting(
    float3 wsPosition, float3 vsPosition, float3 normal,
    float3 diffuseColor, float specIntensity, float specPower,
    float shadow, float vertLighting,
    float twoSidedBacklitMaterial, float vegetationBacklitMask )
{
    float3 V = normalize( -vsPosition );
    float3 H = normalize( SQ_LightDirectionVS + V );
    float spec = PLS_CalcBlinnPhongLighting( normal, H );
    float specMod = pow( dot( float3( 0.333f, 0.333f, 0.333f ), diffuseColor ), 2 );

    float4 lightColor = SQ_LightColor;
    float sunStrength = dot( lightColor.rgb, float3( 0.333f, 0.333f, 0.333f ) );
    bool moonLightActive = AC_MoonVisibility > AC_SunVisibility;
    float mainLightVisibility = moonLightActive
        ? saturate( AC_MoonVisibility )
        : saturate( AC_SunVisibility );
    float3 mainLightDir = normalize( SQ_LightDirectionVS );
    float frontDirect = saturate(dot(mainLightDir, normal));
    float thinDirect = PLS_ComputeThinBacklitNdl(mainLightDir, normal, twoSidedBacklitMaterial * AC_EnableSSS);
    float backTransmissionDirect = max(thinDirect - frontDirect, 0.0f) * saturate(twoSidedBacklitMaterial * AC_EnableSSS);
    float sun = saturate((frontDirect + backTransmissionDirect) * shadow) * mainLightVisibility;

    spec = pow( spec, specPower ) * specIntensity;

    float shadowAO = lerp( 1.0f, vertLighting, SQ_ShadowAOStrength );
    float worldAO = lerp( 1.0f, vertLighting, SQ_WorldAOStrength );

    float3 litPixel;
    if ( moonLightActive )
    {
        // Preserve the old night base and add only a tiny shadowed moon term.
        litPixel = diffuseColor * SQ_ShadowStrength * sunStrength * shadowAO;

        // Match deferred rendering: preserve a small indirect night floor
        // independently of direct-light shadow visibility.
        const float3 nightAmbientColor = float3( 0.34f, 0.40f, 0.52f );
        litPixel += diffuseColor * nightAmbientColor * 0.035f * worldAO;

        const float moonLightStrength = 0.14f;
        float moonDirect = sun;
        float3 moonColor = float3( 0.42f, 0.56f, 1.0f );
        litPixel += diffuseColor * moonColor * moonLightStrength * moonDirect * worldAO;
        litPixel += spec * moonColor * (moonLightStrength * 0.25f) * moonDirect;
    }
    else
    {
        float3 specBare = spec * lightColor.rgb * sun;
        float3 specColored = saturate(
            lerp( specBare, specBare * diffuseColor, specMod ) );
        litPixel = lerp(
            diffuseColor * SQ_ShadowStrength * sunStrength * shadowAO,
            diffuseColor * lightColor.rgb * lightColor.a * worldAO,
            sun ) + specColored;
    }

    float sssSunWeight = saturate( (AC_LightPos.y + 0.08f) * 3.0f ) * AC_SunVisibility * GetRainSkyVisibility();
    float sssMoonWeight = AC_MoonVisibility * 0.12f;
    float sssLightWeight = max( sssSunWeight, sssMoonWeight );
    float materialBacklitMask = max( vegetationBacklitMask, twoSidedBacklitMaterial );
    if ( AC_EnableSSS > 0.5f && sssLightWeight > 0.001f && materialBacklitMask > 0.001f ) {
        float sssShadow = lerp( 0.55f, 1.0f, saturate( shadow ) );
        float sssVertexGate = lerp( 0.35f, 1.0f, saturate( vertLighting * 1.5f ) );
        float sss = PLS_ComputeBacklitTransmissionWeight(
            mainLightDir, normal, V, sssShadow * sssVertexGate,
            vegetationBacklitMask, twoSidedBacklitMaterial,
            AC_EnableSSS, AC_SSSIntensity, 2.4f * sssLightWeight );
        float3 sssLightColor = moonLightActive ? float3( 0.42f, 0.56f, 1.0f ) : lightColor.rgb;
        float3 transmissionLighting = diffuseColor * sssLightColor * sss;
        float3 additiveLighting = litPixel + transmissionLighting;
        float3 boundedExceptionLighting = max(litPixel, transmissionLighting);
        litPixel = lerp(additiveLighting, boundedExceptionLighting, saturate(twoSidedBacklitMaterial));
    }

    float baselineSun = moonLightActive ? 0.0f : sun;
    float fresnel = pow( 1.0f - saturate( dot( normal, V ) ), 10.0f );
    litPixel += lerp( fresnel * litPixel * 0.5f, 0.0f, baselineSun );

    return litPixel;
}

#endif // FORWARD_PLUS_LIGHTING_H
