#include <AtmosphericScattering.h>

//--------------------------------------------------------------------------------------
// Textures and Samplers
//--------------------------------------------------------------------------------------
SamplerState SS_Linear : register(s0);
SamplerState SS_samMirror : register(s1);
Texture2D	TX_Texture0 : register(t0);

//--------------------------------------------------------------------------------------
// Input / Output structures
//--------------------------------------------------------------------------------------
struct PS_INPUT
{
	float2 vTexcoord		: TEXCOORD0;
	float2 vTexcoord2		: TEXCOORD1;
	float4 vDiffuse			: TEXCOORD2;
	float3 vNormalVS		: TEXCOORD4;
	float3 vViewPosition	: TEXCOORD5;
	float4 vCurrClipPos     : TEXCOORD6;
	float4 vPrevClipPos     : TEXCOORD7;
	float4 vPosition		: SV_POSITION;
};

//--------------------------------------------------------------------------------------
// Pixel Shader
//--------------------------------------------------------------------------------------
float4 PSMain(PS_INPUT Input) : SV_TARGET
{
	float4 colour = TX_Texture0.Sample(SS_Linear, Input.vTexcoord);

	// Waterfall foam is not emissive: keep it readable, but let night and rain darken it with the scene.
	float night = saturate((-AC_LightPos.y + 0.08f) * 2.5f);
	float rain = max(saturate(AC_RainFXWeight), saturate(AC_SceneWettness));
	float daylight = saturate(AC_LightPos.y * 1.25f + 0.25f);
	float foamLight = lerp(max(0.18f, daylight * 0.75f), 0.16f, night);
	foamLight *= lerp(1.0f, 0.72f, rain);
	float3 foamTint = lerp(float3(1.0f, 1.0f, 1.0f), float3(0.55f, 0.66f, 0.86f), night);
	colour *= float4(foamTint * foamLight, 0.80f);
	return colour;
}
