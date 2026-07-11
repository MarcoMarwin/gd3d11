//--------------------------------------------------------------------------------------
// World/VOB-Pixelshader for G2D3D11 by Degenerated
//--------------------------------------------------------------------------------------

//--------------------------------------------------------------------------------------
// Textures and Samplers
//--------------------------------------------------------------------------------------
SamplerState SS_Linear : register( s0 );
Texture2D	TX_Texture0 : register( t0 );
Texture2D<float4> TX_BlueNoise : register( t1 );

cbuffer GammaCorrectConstantBuffer : register( b0 )
{
	float G_Gamma;
	float G_Brightness;
	float G_OutputDitherStrength;
	float G_Pad;
}

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
float4 PSMain( PS_INPUT Input ) : SV_TARGET
{
	float4 color = TX_Texture0.Sample(SS_Linear, Input.vTexcoord);
	float baselineBrightness = 1.20f;
	float baselineContrast = 0.70f;
	float3 corrected = pow(saturate(color.rgb * G_Brightness * baselineBrightness), G_Gamma * baselineContrast);
	[branch] if (G_OutputDitherStrength > 0.0f)
	{
		float outputLuminance = dot(corrected, float3(0.2126f, 0.7152f, 0.0722f));
		float darkGradientWeight = 1.0f - smoothstep(0.45f, 0.85f, outputLuminance);
		uint2 noiseCoord = uint2(Input.vPosition.xy) & 511u;
		float outputNoise = TX_BlueNoise.Load(int3(noiseCoord, 0)).x - 0.5f;
		corrected += outputNoise * G_OutputDitherStrength * darkGradientWeight;
	}

	return float4(saturate(corrected), saturate(color.a));
}
