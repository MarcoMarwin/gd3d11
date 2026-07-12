//--------------------------------------------------------------------------------------
// World/VOB-Pixelshader for G2D3D11 by Degenerated
//--------------------------------------------------------------------------------------

//--------------------------------------------------------------------------------------
// Textures and Samplers
//--------------------------------------------------------------------------------------
SamplerState SS_Linear : register( s0 );
Texture2D	TX_Texture0 : register( t0 );

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
float FinalOutputDither(uint2 pixelPosition)
{
	// Stable integer hash: no directional gradient and no repeating 512px texture tile.
	uint hash = pixelPosition.x * 0x1f123bb5u + pixelPosition.y * 0x5f356495u + 0x9e3779b9u;
	hash ^= hash >> 15;
	hash *= 0x2c1b3c6du;
	hash ^= hash >> 12;
	hash *= 0x297a2d39u;
	hash ^= hash >> 15;
	return float(hash & 0x00ffffffu) / 16777216.0f - 0.5f;
}

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
		float outputNoise = FinalOutputDither(uint2(Input.vPosition.xy));
		corrected += outputNoise * G_OutputDitherStrength * darkGradientWeight;
	}

	return float4(saturate(corrected), saturate(color.a));
}
