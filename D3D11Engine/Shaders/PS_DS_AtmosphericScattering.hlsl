//--------------------------------------------------------------------------------------
// World/VOB-Pixelshader for G2D3D11 by Degenerated
//--------------------------------------------------------------------------------------
#include <DS_Defines.h>
#include "DepthReconstruction.h"

#include <AtmosphericScattering.h>
// NW_MISC_OILLAMP_02 emission helpers. Kept local so shader deployment needs
// no additional include file.
float OilLampBrightnessMask(float3 diffuseColor)
{
    const float luminance = dot(diffuseColor, float3(0.2126f, 0.7152f, 0.0722f));
    return smoothstep(0.32f, 0.82f, luminance);
}

float3 DecodeOilLampLightColor(float encoded)
{
    const uint paletteIndex = min((uint)round(saturate(encoded) * (255.0f / 32.0f)), 7u);
    if (paletteIndex == 1u) return float3(237.0f, 211.0f, 165.0f) * (1.0f / 255.0f);
    if (paletteIndex == 2u) return float3(255.0f,  64.0f,  32.0f) * (1.0f / 255.0f);
    if (paletteIndex == 3u) return float3(237.0f, 211.0f, 165.0f) * (1.0f / 255.0f);
    if (paletteIndex == 4u) return float3( 64.0f, 255.0f,  80.0f) * (1.0f / 255.0f);
    if (paletteIndex == 5u) return float3( 48.0f, 220.0f, 255.0f) * (1.0f / 255.0f);
    if (paletteIndex == 6u) return float3( 64.0f, 105.0f, 255.0f) * (1.0f / 255.0f);
    if (paletteIndex == 7u) return float3(190.0f,  64.0f, 255.0f) * (1.0f / 255.0f);
    return 0.0f;
}

float3 ComputeOilLampEmission(float3 diffuseColor, float3 linkedLightColor)
{
    const float3 linearLightColor = pow(saturate(linkedLightColor), 2.2f);
    return linearLightColor * OilLampBrightnessMask(diffuseColor) * 1.10f;
}

#ifndef MAX_CSM_CASCADES
#define MAX_CSM_CASCADES 4
#endif

cbuffer DS_ScreenQuadConstantBuffer : register(b0)
{
    float4 SQ_ProjParams; // x = 1/P._11, y = 1/P._22, z = P._43, w = P._33
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

    // Cascade atlas UV rect (xy = offset, zw = scale); unused for texture arrays.
    float4 SQ_CascadeAtlasRect[MAX_CSM_CASCADES];
};

//--------------------------------------------------------------------------------------
// Textures and Samplers
//--------------------------------------------------------------------------------------
SamplerState SS_Linear : register(s0);
SamplerState SS_samMirror : register(s1);
SamplerComparisonState SS_Comp : register(s2);
Texture2D TX_Diffuse : register(t0);
Texture2D TX_Nrm : register(t1);
Texture2D TX_Depth : register(t2);
#if SHADOW_ATLAS
Texture2D TX_ShadowmapAtlas : register(t3);
#else
Texture2DArray TX_ShadowmapArray : register(t3);
#endif
Texture2D TX_SI_SP : register(t7);
Texture2D TX_ShadowBlueNoise : register(t8);

#include "ShadowSampling.h"
#include "include/PointLightShadows.h"


//--------------------------------------------------------------------------------------
// Input / Output structures
//--------------------------------------------------------------------------------------
struct PS_INPUT
{
    float2 vTexCoord : TEXCOORD0;
    float3 vEyeRay : TEXCOORD1;
    float4 vPosition : SV_POSITION;
};

float3 VSPositionFromDepth(float depth, float2 vTexCoord)
{
    return ReconstructVSPositionFromDepthReverseZInfinite( depth, vTexCoord - SQ_JitterOffset, SQ_ProjParams.xy ) * SQ_ProjParams.z;
}

//--------------------------------------------------------------------------------------
// Blinn-Phong Lighting Reflection Model
//--------------------------------------------------------------------------------------
float CalcBlinnPhongLighting(float3 N, float3 H)
{
    return saturate(dot(N, H));
}


//--------------------------------------------------------------------------------------
// Pixel Shader
//--------------------------------------------------------------------------------------
float4 PSMain(PS_INPUT Input) : SV_TARGET
{
	// Get screen UV
    float2 uv = Input.vTexCoord;

	// Look up the diffuse color
    float4 diffuse = TX_Diffuse.Sample(SS_Linear, uv);
    float vertLighting = diffuse.a;

	// Sample depth first to detect sky pixels (reversed-Z: sky has depth == 0.0)
    float expDepth = TX_Depth.Sample(SS_Linear, uv).r;
    if (!(expDepth > 0.0f))
        // Sky pixel - no geometry was written, just return the diffuse (sky) color
        return float4(diffuse.rgb, 1);

	// Get the second GBuffer
    float2 gb2 = TX_Nrm.Sample(SS_Linear, uv).xy;

	// Decode the view-space normal from octahedral R16G16_SNORM
    float3 normal = DecodeNormalGBuffer(gb2);

	// Get specular parameters
    float4 gb3 = TX_SI_SP.Sample(SS_Linear, uv);
    float twoSidedBacklitMaterial = gb3.x < -2.0f ? 1.0f : 0.0f;
    float npcMaterial = (gb3.x < -0.5f && gb3.x >= -2.0f) ? 1.0f : 0.0f;
    float specIntensity = twoSidedBacklitMaterial > 0.5f ? max(-gb3.x - 3.0f, 0.0f)
        : (npcMaterial > 0.5f ? max(-gb3.x - 1.0f, 0.0f) : gb3.x);
    float alphaTestedMaterial = gb3.y < 0.0f ? 1.0f : 0.0f;
    float vegetationMaterial = alphaTestedMaterial * (1.0f - npcMaterial);
    float specPower = alphaTestedMaterial > 0.5f ? max(-gb3.y - 1.0f, 1.0f) : gb3.y;
	float vegetationMask = PLS_ComputeBacklitVegetationMask(diffuse.rgb, vegetationMaterial, twoSidedBacklitMaterial);
	float vegetationReceiverMask = max(vegetationMask, alphaTestedMaterial * twoSidedBacklitMaterial);

	// Reconstruct VS World Position from depth
    float3 vsPosition = VSPositionFromDepth(expDepth, uv);
    float3 wsPosition = mul(float4(vsPosition, 1), SQ_InvView).xyz;
    float3 V = normalize(-vsPosition);

	float shadow = vertLighting;
#if SHD_ENABLE
	// CSM: Use soft cascaded shadow map with configurable softness
    float3 wsNormal = normalize(mul(float4(normal, 0.0f), SQ_InvView).xyz);

    if (AC_SunVisibility > 0.001f || AC_MoonVisibility > 0.001f) // sample the single active sun or moon shadow map
	{
        float3 wsLightDirection = normalize(mul(float4(SQ_LightDirectionVS, 0.0f), SQ_InvView).xyz);

        int cascadeIndex = GetPrimaryCascadeIndex(wsPosition);
        float texelWorldSize = GetCascadeWorldTexelSize(cascadeIndex);

        float3 biasedWsPosition = ApplyReceiverNormalBias(wsPosition, wsNormal, wsLightDirection, texelWorldSize, vegetationReceiverMask);

        // Use screen position for per-pixel rotation (temporal-friendly)
        shadow = ComputeCascadedShadowValueSoft(biasedWsPosition, vsPosition.z, vertLighting, 0.0f, Input.vPosition.xy, npcMaterial);
	} else {
        // Night-time sky ambient:
        // saturate(wsNormal.y) restricts the value to [0, 1].
        // Facing up = 1, Facing sides/down = 0.
        shadow = saturate(wsNormal.y) * vertLighting;
    }
#endif

    // Compute specular lighting

    float3 H = normalize(SQ_LightDirectionVS + V);
    float spec = CalcBlinnPhongLighting(normal, H);
    float specMod = pow(dot(float3(0.333f, 0.333f, 0.333f), diffuse.rgb), 2);

    //return float4(diffuse.rgb, 1);

    float4 lightColor = SQ_LightColor;

    // Apply sunlight
    float sunStrength = dot(lightColor.rgb, float3(0.333f, 0.333f, 0.333f));

	float vl = saturate(vertLighting * 2);
	float vertAO = lerp(vl * vl, 1.0f, 0.5f);

    bool moonLightActive = AC_MoonVisibility > AC_SunVisibility;
    float mainLightVisibility = moonLightActive
        ? saturate(AC_MoonVisibility)
        : saturate(AC_SunVisibility);
    float3 mainLightDir = normalize(SQ_LightDirectionVS);
    float directNoL = PLS_ComputeThinBacklitNdl(mainLightDir, normal, twoSidedBacklitMaterial * AC_EnableSSS);
    float frontDirect = saturate(dot(mainLightDir, normal));
    float backTransmissionDirect = max(directNoL - frontDirect, 0.0f) * saturate(twoSidedBacklitMaterial * AC_EnableSSS);
    float sun = saturate((frontDirect + backTransmissionDirect) * shadow) * mainLightVisibility;
    spec = pow(spec, specPower) * specIntensity;

    float shadowAO = lerp(1.0f, vertLighting, SQ_ShadowAOStrength);
    float worldAO = lerp(1.0f, vertLighting, SQ_WorldAOStrength);

    float3 litPixel;
    if (moonLightActive)
    {
        // Keep the exact pre-moon night base. Moonlight is additive only, so
        // its shadow can remove that tiny contribution but never darken the night.
        litPixel = diffuse.rgb * SQ_ShadowStrength * sunStrength * shadowAO;

        const float moonLightStrength = 0.14f;
        float moonDirect = sun;
        float3 moonColor = float3(0.42f, 0.56f, 1.0f);
        litPixel += diffuse.rgb * moonColor * moonLightStrength * moonDirect * worldAO;
        litPixel += spec * moonColor * (moonLightStrength * 0.25f) * moonDirect;
    }
    else
    {
        float3 specBare = spec * lightColor.rgb * sun;
        float3 specColored = saturate(
            lerp(specBare, specBare * diffuse.rgb, specMod));
        litPixel = lerp(
            diffuse.rgb * SQ_ShadowStrength * sunStrength * shadowAO,
            diffuse.rgb * lightColor.rgb * lightColor.a * worldAO,
            sun) + specColored;
    }

	float sssSunWeight = saturate((AC_LightPos.y + 0.08f) * 3.0f) * AC_SunVisibility * GetRainSkyVisibility();
	float sssMoonWeight = AC_MoonVisibility * 0.12f;
	float sssLightWeight = max(sssSunWeight, sssMoonWeight);
	float3 sssLightColor = moonLightActive ? float3(0.42f, 0.56f, 1.0f) : lightColor.rgb;
	float materialBacklitMask = max(vegetationMask, twoSidedBacklitMaterial);
	if (AC_EnableSSS > 0.5f && sssLightWeight > 0.001f && materialBacklitMask > 0.001f) {
		float sssShadow = lerp(0.55f, 1.0f, saturate(shadow));
		float sssVertexGate = lerp(0.35f, 1.0f, saturate(vertLighting * 1.5f));
		float sss = PLS_ComputeBacklitTransmissionWeight(
			mainLightDir, normal, V, sssShadow * sssVertexGate,
			vegetationMask, twoSidedBacklitMaterial,
			AC_EnableSSS, AC_SSSIntensity, 2.4f * sssLightWeight);
		float3 transmissionLighting = diffuse.rgb * sssLightColor * sss;
		float3 additiveLighting = litPixel + transmissionLighting;
		float3 boundedExceptionLighting = max(litPixel, transmissionLighting);
		litPixel = lerp(additiveLighting, boundedExceptionLighting, saturate(twoSidedBacklitMaterial));
	}

    float f = 1.0f - saturate(dot(normal, V));
    // float fresnel = pow(f, 10.0f);
	// use optimized pow alternative
	float f2 = f*f;
	float f4 = f2*f2;
	float f8 = f4*f4;
	float fresnel = f8*f2;
    litPixel += lerp(fresnel * litPixel * 0.5f, 0.0f, sun);

	// Atmospheric scattering affects reflected surface lighting, not light
	// emitted by the lamp itself.
	litPixel = ApplyAtmosphericScatteringGround(wsPosition, litPixel.rgb);

	// NW_MISC_OILLAMP_02 restores the linked point-light palette ID from gb3.w.
	// A VOB without an unambiguous enabled light writes zero and stays non-emissive.
	// Oil-lamp pixels are sparse, so skip palette decoding and mask evaluation
	// for the rest of this full-screen deferred pass.
	[branch]
	if (gb3.w > 0.0f)
		litPixel += ComputeOilLampEmission(diffuse.rgb, DecodeOilLampLightColor(gb3.w));


    // Fix indoor stuff
	//litPixel = lerp(diffuse * vertLighting, litPixel, vertLighting < 0.9f ? 0 : 1);
	//diffuse.rgb = lerp(diffuse.rgb, 1.0f, clamp(shaft, 0.0f, 0.4f));

	// float4 cascadeDebug = GetCascadeUVAndBounds(wsPosition, 1); // Check Cascade 0
	// if (cascadeDebug.z > 0.5f) {
		// // cascadeDebug.w is the blend factor (0 = Pure Cascade 0, 1 = Pure Cascade 1)
		// return float4(lerp(float3(0,1,0), float3(1,0,0), cascadeDebug.w), 1.0f);
	// }

	//return float4(sun.rgb, 1);
	//return float4(vertLighting.rrr, 1);
    return float4(litPixel.rgb, 1);
	//return float4(pow(spec, specPower) * specIntensity.xxx * diffuse.rgb * SQ_LightColor.rgb,1);

}
