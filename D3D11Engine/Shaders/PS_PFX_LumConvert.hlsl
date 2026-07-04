//--------------------------------------------------------------------------------------
// G2D3D11 by Degenerated
//--------------------------------------------------------------------------------------

//--------------------------------------------------------------------------------------
// Textures and Samplers
//--------------------------------------------------------------------------------------
SamplerState SS_Linear : register( s0 );
SamplerState SS_samMirror : register( s1 );
Texture2D	TX_Texture0 : register( t0 );
Texture2D	TX_Texture1 : register( t1 );

static const float MIN_LUM = 0.05f;
static const float LUM_HIGHLIGHT_KNEE = 1.0f;
static const float LUM_HIGHLIGHT_COMPRESSION = 0.35f;

static const float3 LUM_CONVERT = float3(0.2126f, 0.7152f, 0.0722f);

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
float PSMain( PS_INPUT Input ) : SV_TARGET
{
	float3 color = TX_Texture0.Sample(SS_Linear, Input.vTexcoord).rgb;
	float lum = dot(color, LUM_CONVERT);
	float highlightLum = max(0.0f, lum - LUM_HIGHLIGHT_KNEE);
	lum = min(lum, LUM_HIGHLIGHT_KNEE) + highlightLum / (1.0f + highlightLum * LUM_HIGHLIGHT_COMPRESSION);

	return max(MIN_LUM, lum);
}