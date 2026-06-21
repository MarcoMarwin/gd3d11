//--------------------------------------------------------------------------------------
// World/VOB-Pixelshader for G2D3D11 by Degenerated
//--------------------------------------------------------------------------------------

#include <hdr.h>

static const float BRIGHT_PASS_OFFSET = 10.0f;


//--------------------------------------------------------------------------------------
// Textures and Samplers
//--------------------------------------------------------------------------------------
SamplerState SS_Linear : register( s0 );
SamplerState SS_samMirror : register( s1 );
Texture2D	TX_Scene : register( t0 );
Texture2D	TX_Lum : register( t1 );


//--------------------------------------------------------------------------------------
// Input / Output structures
//--------------------------------------------------------------------------------------
struct PS_INPUT
{
	float2 vTexcoord		: TEXCOORD0;
	float3 vEyeRay			: TEXCOORD1;
	float4 vPosition		: SV_POSITION;
};


//--------------------------------------------------------------------------------------
// Pixel Shader
//--------------------------------------------------------------------------------------
float TonemapDither(float2 pixelPosition)
{
	float n1 = frac(52.9829189f * frac(dot(pixelPosition, float2(0.06711056f, 0.00583715f))));
	float n2 = frac(52.9829189f * frac(dot(pixelPosition + 19.19f, float2(0.00583715f, 0.06711056f))));
	return n1 + n2 - 1.0f;
}

float4 PSMain( PS_INPUT Input ) : SV_TARGET
{
	float4 sample = TX_Scene.Sample(SS_Linear, Input.vTexcoord);
	float3 HDRColor = sample.rgb;
	
	// Determine what the pixel's value will be after tone-mapping occurs
	//float fLumAvg = TX_Lum.SampleLevel(SS_Linear, float2(0.5f, 0.5f), 9).r;
	//HDRColor *= HDR_MiddleGray/(fLumAvg + 0.001f);
	
#if USE_TONEMAP == 0
		float3 toneMapped = ToneMap_jafEq4(HDRColor, TX_Lum, SS_Linear);
#elif USE_TONEMAP == 1
		float3 toneMapped = Uncharted2Tonemap(HDRColor, TX_Lum, SS_Linear);
#elif USE_TONEMAP == 2
		float3 toneMapped = ACESFilmTonemap(HDRColor, TX_Lum, SS_Linear);
#elif USE_TONEMAP == 3
		float3 toneMapped = PerceptualQuantizerTonemap(HDRColor, TX_Lum, SS_Linear);
#elif USE_TONEMAP == 4
		float3 toneMapped = ToneMap_Simple(HDRColor, TX_Lum, SS_Linear);
#elif USE_TONEMAP == 5
		float3 toneMapped = ACESFittedTonemap(HDRColor, TX_Lum, SS_Linear);
#endif
	
	toneMapped -= HDR_Threshold;
	toneMapped = max(float3(0,0,0), toneMapped);

	// Final-output dithering survives tone mapping and suppresses visible bands
	// in dark fog without adding another render pass.
	float outputLuma = dot(toneMapped, float3(0.2126f, 0.7152f, 0.0722f));
	float darkDitherWeight = 1.0f - smoothstep(0.02f, 0.25f, outputLuma);
	toneMapped = saturate(toneMapped + TonemapDither(Input.vPosition.xy) * darkDitherWeight * (1.5f / 255.0f));
	
	// Map the resulting value into the 0 to 1 range. Higher values for
	// BRIGHT_PASS_OFFSET will isolate lights from illuminated scene 
	// objects.
	//toneMapped.rgb /= (BRIGHT_PASS_OFFSET+toneMapped);
	
	return float4(toneMapped.rgb, 1); 
}

