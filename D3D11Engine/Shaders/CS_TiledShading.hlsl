#include "DS_Defines.h"
#include "DepthReconstruction.h"
#include <AtmosphericScattering.h>
#include "include/PointLightShadows.h"

#define TILE_SIZE 16
#define NUM_Z_SLICES 16u
#define MASK_WORDS 32u

struct TiledPointLight {
    float3 PositionView;
    float Range;
    float4 Color;
    float3 PositionWorld;
    int ShadowCubeIndex; // -1 = no shadow, else index into TextureCubeArray
    float ShadowStrength;
    float IsIndoor;
    float IgnoreIndoorOutdoorLimit;
    float ShadowSoftness;
    uint ShadowFilterMode;
    uint3 ShadowFilterPad;
};

struct LightGrid {
    uint WordOccupancy;
    uint Mask[MASK_WORDS];
};

cbuffer TiledShadingConstantBuffer : register( b0 ) {
    float2 ViewportSize;
    float2 JitterOffset;
    float4 ProjParams; // x = 1/P._11, y = 1/P._22, z = P._43, w = P._33
    uint LimitLightIntensity;
    uint NumTilesX;
    float ClusterNearZ;
    float ClusterFarZ;
    matrix InvView; // For world-space reconstruction (shadow sampling)
};

SamplerState SS_Linear : register( s0 );
SamplerComparisonState SS_Comp : register( s2 );
Texture2D TX_Diffuse : register( t0 );
Texture2D TX_Nrm : register( t1 );
Texture2D TX_Depth : register( t2 );
Texture2D TX_SI_SP : register( t7 );

StructuredBuffer<TiledPointLight> SB_Lights : register( t8 );
StructuredBuffer<LightGrid> SB_LightGrid : register( t9 );

TextureCubeArray TX_ShadowCubeArray : register( t11 );
TextureCubeArray TX_DynamicShadowCubeArray : register( t12 );
TextureCubeArray TX_StaticLowShadowCubeArray : register( t20 );

float3 VSPositionFromDepth( float depth, uint2 pixelCoord ) {
    return ReconstructVSPositionFromDepthReverseZInfinite( depth, pixelCoord, ViewportSize, ProjParams.xy );
}


RWTexture2D<float4> RW_HDR : register( u0 );

[numthreads( TILE_SIZE, TILE_SIZE, 1 )]
void CSMain( uint3 groupID : SV_GroupID, uint3 threadID : SV_GroupThreadID, uint3 dispatchThreadID : SV_DispatchThreadID ) {
    uint2 pixelCoord = dispatchThreadID.xy;

    if ( pixelCoord.x >= (uint)ViewportSize.x || pixelCoord.y >= (uint)ViewportSize.y )
        return;

    // Integer loads keep the G-buffer lookup exact.
    float4 diffuse = TX_Diffuse.Load( int3( pixelCoord, 0 ) );
    float3 normal = DecodeNormalGBuffer( TX_Nrm.Load( int3( pixelCoord, 0 ) ).xy );
    float4 gb3 = TX_SI_SP.Load( int3( pixelCoord, 0 ) );
    float twoSidedBacklitMaterial = gb3.x < -2.0f ? 1.0f : 0.0f;
    float npcMaterial = (gb3.x < -0.5f && gb3.x >= -2.0f) ? 1.0f : 0.0f;
    float specIntensity = twoSidedBacklitMaterial > 0.5f ? max(-gb3.x - 3.0f, 0.0f)
        : (gb3.x < -0.5f ? max(-gb3.x - 1.0f, 0.0f) : gb3.x);
    float alphaTestedMaterial = gb3.y < 0.0f ? 1.0f : 0.0f;
    float vegetationBacklitMask = PLS_ComputeBacklitVegetationMask(diffuse.rgb, alphaTestedMaterial, twoSidedBacklitMaterial);
    float specPower = alphaTestedMaterial > 0.5f ? max(-gb3.y - 1.0f, 1.0f) : gb3.y;

    float expDepth = TX_Depth.Load( int3( pixelCoord, 0 ) ).r;
    float3 vsPosition = VSPositionFromDepth( expDepth, pixelCoord );

    // Reconstruct world space once for shadow sampling.
    float3 wsPosition = mul( float4( vsPosition, 1 ), InvView ).xyz;
    float3 wsNormal = normalize( mul( float4( normal, 0 ), InvView ).xyz );

    // Find the cluster for this pixel.
    uint tileX = pixelCoord.x / TILE_SIZE;
    uint tileY = pixelCoord.y / TILE_SIZE;
    uint tileIndex = tileY * NumTilesX + tileX;
    float zView = abs( vsPosition.z );
    float zSliceT = log2( max( zView, ClusterNearZ ) / ClusterNearZ )
        / log2( ClusterFarZ / ClusterNearZ );
    uint slice = (uint)clamp( floor( zSliceT * (float)NUM_Z_SLICES ), 0.0f, (float)( NUM_Z_SLICES - 1 ) );
    uint cluster = tileIndex * NUM_Z_SLICES + slice;

    // Values shared by all lights.
    float3 V = normalize( -vsPosition );
    float specMod = PLS_ComputeSpecMod( diffuse.rgb );

    float3 totalLighting = float3( 0, 0, 0 );
    float3 maxLighting = float3( 0, 0, 0 );

    uint wordMask = SB_LightGrid[cluster].WordOccupancy;
    while ( wordMask != 0 ) {
        uint word = firstbitlow( wordMask );
        wordMask &= wordMask - 1;
        uint lightMask = SB_LightGrid[cluster].Mask[word];
        while ( lightMask != 0 ) {
            uint bit = firstbitlow( lightMask );
            lightMask &= lightMask - 1;
            uint lightIdx = word * 32u + bit;
            TiledPointLight light = SB_Lights[lightIdx];

            float3 lightDir = light.PositionView - vsPosition;
            float distance = length( lightDir );

            if ( distance >= light.Range )
                continue;

            lightDir /= distance;

            float ndl = PLS_ComputePointLightNdlBacklit( lightDir, normal, light.PositionWorld, wsPosition, wsNormal, twoSidedBacklitMaterial, AC_EnableSSS );
            float falloff = PLS_ComputeRangeFalloff( distance, light.Range );

            float3 H = normalize( lightDir + V );
            float spec = PLS_CalcBlinnPhongLighting( normal, H );
            float3 lighting = PLS_ComputePointLightLightingBacklit(
                diffuse.rgb, light.Color.rgb, ndl, falloff, spec, specIntensity, specPower, specMod,
                lightDir, normal, V, vegetationBacklitMask, twoSidedBacklitMaterial,
                AC_EnableSSS, AC_SSSIntensity, 0.42f );

            // Sample shadows only for lights that have a visible contribution.
            if ( light.ShadowCubeIndex >= 0 && any( lighting > 0.001f )
                && (npcMaterial <= 0.5f || light.ShadowFilterPad[0] == 0u) ) {
                const int shadowSlot = light.ShadowCubeIndex & 0x1fffffff;
                const bool lowStatic = (light.ShadowCubeIndex & 0x20000000) != 0;
                float shadow;
                if ( lowStatic )
                    shadow = PLS_SampleShadowCubeArray( TX_StaticLowShadowCubeArray, SS_Linear, SS_Comp, wsPosition, wsNormal, light.PositionWorld, light.Range, shadowSlot, light.ShadowSoftness, light.ShadowFilterMode );
                else
                    shadow = PLS_SampleShadowCubeArray( TX_ShadowCubeArray, SS_Linear, SS_Comp, wsPosition, wsNormal, light.PositionWorld, light.Range, shadowSlot, light.ShadowSoftness, light.ShadowFilterMode );
                if ( shadow > 0.001f && (light.ShadowCubeIndex & 0x40000000) != 0 )
                {
                    shadow *= PLS_SampleShadowCubeArray( TX_DynamicShadowCubeArray, SS_Linear, SS_Comp, wsPosition, wsNormal, light.PositionWorld, light.Range, shadowSlot, light.ShadowSoftness, light.ShadowFilterMode );
                }
                lighting *= lerp(1.0f, shadow, saturate(light.ShadowStrength));
            }

            // Keep indoor lights from leaking into outdoor pixels.
            float indoorPixel = diffuse.a < 0.5f ? 1.0f : 0.0f;
            float indoorBoundary = saturate((1.0f - light.IsIndoor) + light.IsIndoor * indoorPixel);
            lighting *= lerp(indoorBoundary, 1.0f, saturate(light.IgnoreIndoorOutdoorLimit));

            lighting = saturate( lighting );

            totalLighting += lighting;
            maxLighting = max( maxLighting, lighting );
        }
    }

    float3 activeLighting = LimitLightIntensity ? maxLighting : totalLighting;
    if ( any( activeLighting > 0 ) ) {
        float4 existing = RW_HDR[pixelCoord];
        if ( LimitLightIntensity ) {
            // Match legacy MAX blend: each light uses max(light, existing) individually.
            // Since we see all lights at once, take the per-light max.
            RW_HDR[pixelCoord] = float4( max( existing.rgb, maxLighting ), existing.a );
        } else {
            RW_HDR[pixelCoord] = float4( existing.rgb + totalLighting, existing.a );
        }
    }
}
