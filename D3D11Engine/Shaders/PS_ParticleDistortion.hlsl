//--------------------------------------------------------------------------------------
// World/VOB-Pixelshader for G2D3D11 by Degenerated
//--------------------------------------------------------------------------------------
#include <AtmosphericScattering.h>


//--------------------------------------------------------------------------------------
// Textures and Samplers
//--------------------------------------------------------------------------------------
SamplerState SS_Linear : register( s0 );
Texture2D	TX_Texture0 : register( t0 );

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
    float vParticleLightingScale : TEXCOORD8;
	float4 vPosition		: SV_POSITION;
};

struct PS_OUTPUT
{
	float4 gb0 : SV_TARGET0;
	float4 gb1 : SV_TARGET1;
};

//--------------------------------------------------------------------------------------
// Pixel Shader
//--------------------------------------------------------------------------------------
PS_OUTPUT PSMain( PS_INPUT Input )
{
	float4 color = TX_Texture0.Sample(SS_Linear, Input.vTexcoord);
	color *= Input.vDiffuse;


	if (Input.vParticleLightingScale >= 0.0f)
	{
		float nightParticle = GetAmbientNightWeight();
		float rainParticle = max(saturate(AC_RainFXWeight), saturate(AC_SceneWettness));
		const bool groundFog = Input.vParticleLightingScale > 1.5f
			&& Input.vParticleLightingScale < 2.5f;
		const bool waterParticle = Input.vParticleLightingScale >= 2.5f;
		const float enabledStrength = saturate(
			AC_EnableParticleLighting * AC_ParticleLightingStrength);
		const float regularStrength = enabledStrength
			* saturate(Input.vParticleLightingScale);
		const float nightLightingStrength = waterParticle
			? enabledStrength : regularStrength;
		// Keep the former quarter-strength rain response independent from the
		// stronger water-particle night treatment.
		const float rainLightingStrength = waterParticle
			? enabledStrength * 0.25f : regularStrength;
		const float nightFloor = (groundFog || waterParticle) ? 0.10f : 0.28f;
		const float nightTintStrength = waterParticle ? 1.0f : 0.80f;
		color.rgb = ApplyAmbientNightTint(
			color.rgb, nightParticle * nightLightingStrength * nightTintStrength);
		if (waterParticle)
		{
			// Match the simple particle path: retain the shared cool atmosphere
			// and add a restrained steel-blue bias for overlapping water spray.
			const float waterNightTint = nightParticle * nightLightingStrength;
			color.rgb *= lerp(float3(1.0f, 1.0f, 1.0f),
				float3(0.78f, 0.90f, 1.08f), waterNightTint);
		}
		float nightDim = lerp(1.0f, nightFloor, nightParticle);
		color.rgb *= lerp(1.0f, nightDim, nightLightingStrength);
		const float waterNightAmount = saturate(nightParticle * nightLightingStrength);
		if (waterParticle && waterNightAmount > 0.0001f)
		{
			// Increase water-particle contrast, not overall brightness: keep the
			// dark midpoint fixed while separating dark spray from highlights.
			const float waterNightContrast = lerp(1.0f, 1.18f,
				waterNightAmount);
			const float waterContrastPivot = 0.06f;
			color.rgb = saturate((color.rgb - waterContrastPivot)
				* waterNightContrast + waterContrastPivot);
		}

		float rainAlpha = lerp(1.0f, 0.28f, rainParticle);
		color.a *= lerp(1.0f, rainAlpha, rainLightingStrength);
	}
	
	PS_OUTPUT o;
	// Store particle color
	o.gb0 = color;
	
	// Center the UV
	float2 uvCenter = Input.vTexcoord2 - 0.5f;
	float weight = dot(color.rgb, float3(0.333f, 0.333f, 0.333f)) * pow(color.a, 1/4.0f);
	weight *= 2.5f; // Scale the distortion down a bit
	weight *= min(1.0f, Input.vPosition.z * 8.0f);
	
	// Store the direction to the center of the uv-plane as distortion
	o.gb1 = float4(uvCenter * float2(-1,1) * weight, 0, color.a);
	return o;
}

