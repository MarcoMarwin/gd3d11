//--------------------------------------------------------------------------------------
// World/VOB-Pixelshader for G2D3D11 by Degenerated
//--------------------------------------------------------------------------------------
#include <DS_Defines.h>
#include "DepthReconstruction.h"
#include <AtmosphericScattering.h>
#include "include/PointLightShadows.h"

cbuffer DS_PointLightConstantBuffer : register( b0 )
{
	float4 PL_Color;
	
	float PL_Range;
	float3 Pl_PositionWorld;
	
	float PL_Outdoor;
	float3 Pl_PositionView;
	
	float2 PL_ViewportSize;
	float PL_IgnoreIndoorOutdoorLimit;
	float PL_ShadowSoftness;
	
	float4 PL_ProjParams; // x = 1/P._11, y = 1/P._22, z = P._43, w = P._33
	matrix PL_InvView; // Optimize out!
	
	float3 PL_LightScreenPos;
	float PL_ShadowStrength;
	uint PL_ShadowFilterMode;
	uint3 PL_ShadowFilterPad;
};

//--------------------------------------------------------------------------------------
// Textures and Samplers
//--------------------------------------------------------------------------------------
SamplerState SS_Linear : register( s0 );
SamplerState SS_samMirror : register( s1 );
Texture2D	TX_Diffuse : register( t0 );
Texture2D	TX_Nrm : register( t1 );
Texture2D	TX_Depth : register( t2 );
Texture2D	TX_SI_SP : register( t7 );

//--------------------------------------------------------------------------------------
// Input / Output structures
//--------------------------------------------------------------------------------------
struct PS_INPUT
{
	float4 vPosition		: SV_POSITION;
};

float3 VSPositionFromDepth(float depth, float2 vTexCoord)
{
	return ReconstructVSPositionFromDepthReverseZInfinite( depth, vTexCoord, PL_ProjParams.xy ) * PL_ProjParams.z;
}

//--------------------------------------------------------------------------------------
// Blinn-Phong Lighting Reflection Model
//--------------------------------------------------------------------------------------
float CalcBlinnPhongLighting(float3 N, float3 H)
{
    return saturate(dot(N, H));
}

float GetShadow(float2 uv)
{
	// Get light direction
	float2 lightDir = PL_LightScreenPos.xy - uv;
	float distance = length(lightDir);
	lightDir /= distance; // Normalize the direction
	
	// Calculate ray steps size
	const int numSteps = 100;
	float stepSize = distance / numSteps;
	
	//float depthLight = TX_Depth.Sample(SS_Linear, PL_LightScreenPos).r;
	float depthTarget = TX_Depth.Sample(SS_Linear, uv).r;
	
	float dx = ddx(uv.xy);
	float dy = ddy(uv.xy);
	
	float2 ray = PL_LightScreenPos.xy;
	for(int i=0;i<numSteps;i++)
	{
		ray += lightDir * stepSize;
		
		float depthRay = TX_Depth.SampleGrad(SS_Linear,ray, dx, dy);
		
		if(depthRay < PL_LightScreenPos.z)
			return 0;
	}
	
	return 1;
}

//--------------------------------------------------------------------------------------
// Pixel Shader
//--------------------------------------------------------------------------------------
float4 PSMain( PS_INPUT Input ) : SV_TARGET
{
	// Get screen UV
	float2 uv = Input.vPosition.xy / PL_ViewportSize;
	
	// Look up the diffuse color
	float4 diffuse = TX_Diffuse.Sample(SS_Linear, uv);
	
	// Get the second GBuffer
	float2 gb2 = TX_Nrm.Sample(SS_Linear, uv).xy;
	
	// Decode the view-space normal from octahedral R16G16_SNORM
	float3 normal = DecodeNormalGBuffer(gb2);
	
	// Get specular parameters
	float4 gb3 = TX_SI_SP.Sample(SS_Linear, uv);
	float twoSidedBacklitMaterial = gb3.x < -2.0f ? 1.0f : 0.0f;
	float specIntensity = twoSidedBacklitMaterial > 0.5f ? max(-gb3.x - 3.0f, 0.0f)
        : (gb3.x < -0.5f ? max(-gb3.x - 1.0f, 0.0f) : gb3.x);
	float alphaTestedMaterial = gb3.y < 0.0f ? 1.0f : 0.0f;
	float vegetationBacklitMask = PLS_ComputeBacklitVegetationMask(diffuse.rgb, alphaTestedMaterial, twoSidedBacklitMaterial);
	float specPower = alphaTestedMaterial > 0.5f ? max(-gb3.y - 1.0f, 1.0f) : gb3.y;
	
	// Reconstruct VS/WS position from depth
	float expDepth = TX_Depth.Sample(SS_Linear, uv).r;
	float3 vsPosition = VSPositionFromDepth(expDepth, uv);
	float3 wsPosition = mul(float4(vsPosition, 1), PL_InvView).xyz;
	float3 wsNormal = normalize(mul(float4(normal, 0), PL_InvView).xyz);
	
	// Get direction and distance from the light to that position
	float3 lightDir = Pl_PositionView - vsPosition;
	float distance = length(lightDir);
	lightDir /= distance; // Normalize the direction
	
	// Do some simple NdL-Lighting
	float ndl = PLS_ComputePointLightNdlBacklit(lightDir, normal, Pl_PositionWorld, wsPosition, wsNormal, twoSidedBacklitMaterial, AC_EnableSSS);
	
	// Compute range falloff
	float falloff = PLS_ComputeRangeFalloff(distance, PL_Range);
	//float falloff = saturate(1.0f / (pow(distance / PL_Range * 2, 2)));
	
	// Compute specular lighting
	float3 V = normalize(-vsPosition);
	float3 H = normalize(lightDir + V);
	float spec = PLS_CalcBlinnPhongLighting(normal, H);
	float specMod = PLS_ComputeSpecMod(diffuse.rgb);
	
	// Blend this with the light color, world diffuse and specular term.
	float3 lighting = PLS_ComputePointLightLightingBacklit(
		diffuse.rgb, PL_Color.rgb, ndl, falloff, spec, specIntensity, specPower, specMod,
		lightDir, normal, V, vegetationBacklitMask, twoSidedBacklitMaterial,
		AC_EnableSSS, AC_SSSIntensity, 0.42f);
	
	// Indoor lights must not leak onto outdoor geometry. Use the exact per-pixel
	// classification; the former multi-ring doorway probe introduced quantized bands.
	float indoorPixel = diffuse.a < 0.5f ? 1.0f : 0.0f;
	float indoorBoundary = saturate(PL_Outdoor + (1.0f - PL_Outdoor) * indoorPixel);
	lighting *= lerp(indoorBoundary, 1.0f, saturate(PL_IgnoreIndoorOutdoorLimit));

	return float4(saturate(lighting),1);
}
