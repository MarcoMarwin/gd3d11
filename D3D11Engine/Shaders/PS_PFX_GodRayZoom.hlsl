//--------------------------------------------------------------------------------------
// World/VOB-Pixelshader for G2D3D11 by Degenerated
//--------------------------------------------------------------------------------------

cbuffer GodRayZoomConstantBuffer : register( b0 )
{
	float GR_Decay;
	float GR_Weight;
	float2 GR_Center;
	
	float GR_Density;
	float3 GR_ColorMod;
};

//--------------------------------------------------------------------------------------
// Textures and Samplers
//--------------------------------------------------------------------------------------
SamplerState SS_Linear : register( s0 );
SamplerState SS_samMirror : register( s1 );
Texture2D	TX_Texture0 : register( t0 );
Texture2D	TX_Texture1 : register( t1 );

//--------------------------------------------------------------------------------------
// Input / Output structures
//--------------------------------------------------------------------------------------
struct PS_INPUT
{
	float2 vTexcoord		: TEXCOORD0;
	float3 vEyeRay			: TEXCOORD1;
	float4 vPosition		: SV_POSITION;
};

// Interleaved Gradient Noise for cheap, effective dithering
float InterleavedGradientNoise(float2 uv) { float3 magic = float3(0.06711056f, 0.00583715f, 52.9829189f); return frac(magic.z * frac(dot(uv, magic.xy))); }

float LensFlareLuma(float3 color) { return dot(color, float3(0.2126f, 0.7152f, 0.0722f)); }

float LensFlareCircle(float2 uv, float2 position, float radius, float aspect) { float2 delta = uv - position; delta.x *= aspect; return 1.0f - smoothstep(radius * 0.35f, radius, length(delta)); }

float LensFlareRing(float2 uv, float2 position, float radius, float width, float aspect) { float2 delta = uv - position; delta.x *= aspect; float ringDistance = abs(length(delta) - radius); return 1.0f - smoothstep(0.0f, width, ringDistance); }

float3 BuildLensFlare(float2 uv, float2 sunPosition, float aspect, float sunVisibility) { float2 screenCenter = float2(0.5f, 0.5f); float2 flareAxis = screenCenter - sunPosition; float lookAtSun = 1.0f - smoothstep(0.10f, 0.68f, length(flareAxis)); float flareStrength = saturate(sunVisibility) * lookAtSun * max(GR_Weight, 0.0f);

float sunGlow = LensFlareCircle(uv, sunPosition, 0.105f, aspect); float sunHalo = LensFlareRing(uv, sunPosition, 0.145f, 0.030f, aspect); float ghost1 = LensFlareCircle(uv, sunPosition + flareAxis * 0.62f, 0.040f, aspect); float ghost2 = LensFlareRing(uv, sunPosition + flareAxis * 1.05f, 0.075f, 0.020f, aspect); float ghost3 = LensFlareCircle(uv, sunPosition + flareAxis * 1.48f, 0.055f, aspect); float ghost4 = LensFlareRing(uv, sunPosition + flareAxis * 1.82f, 0.095f, 0.024f, aspect);

float3 flareColor = 0.0f; flareColor += sunGlow * float3(1.00f, 0.82f, 0.56f) * 1.20f; flareColor += sunHalo * float3(1.00f, 0.62f, 0.30f) * 0.48f; flareColor += ghost1 * float3(0.35f, 0.60f, 1.00f) * 0.34f; flareColor += ghost2 * float3(0.40f, 1.00f, 0.65f) * 0.25f; flareColor += ghost3 * float3(1.00f, 0.38f, 0.26f) * 0.28f; flareColor += ghost4 * float3(0.45f, 0.55f, 1.00f) * 0.20f;

return flareColor * flareStrength; }

//--------------------------------------------------------------------------------------
// Pixel Shader
//--------------------------------------------------------------------------------------
float4 PSMain( PS_INPUT Input ) : SV_TARGET
{
    // Increased sample count for a smoother gradient
	const int NUM_SAMPLES = 64; 
	float2 center = GR_Center;
	float3 color = 0;
	float illumDecay = 1.0f;
	
	float2 deltaTexCoord = Input.vTexcoord - center;
	deltaTexCoord *= 1.0f / NUM_SAMPLES * GR_Density;
	
	float2 uv = Input.vTexcoord;
	
    // Dithering: Offset the starting UV by a random sub-texel fraction
    // Input.vPosition.xy provides screen-space pixel coordinates
    float jitter = InterleavedGradientNoise(Input.vPosition.xy);
    uv -= deltaTexCoord * jitter;

	[unroll(64)] // Must match NUM_SAMPLES
	for(int i = 0; i < NUM_SAMPLES; i++)
	{
		uv -= deltaTexCoord;
        
        // Anti-Smearing: Prevent sampling out of bounds if the sampler wraps
        if (uv.x < 0.0f || uv.x > 1.0f || uv.y < 0.0f || uv.y > 1.0f) 
        {
            continue; 
        }

		color += TX_Texture0.Sample(SS_Linear, uv).rgb * illumDecay * GR_Weight;
		
		illumDecay *= GR_Decay;
	}
color /= NUM_SAMPLES;

uint textureWidth; uint textureHeight; TX_Texture0.GetDimensions(textureWidth, textureHeight);

float aspect = textureHeight > 0 ? (float)textureWidth / (float)textureHeight : 1.0f; float2 texelSize = float2( 1.0f / max((float)textureWidth, 1.0f), 1.0f / max((float)textureHeight, 1.0f));

float3 sunSample = 0.0f;

if (center.x >= 0.0f && center.x <= 1.0f && center.y >= 0.0f && center.y <= 1.0f) { sunSample += TX_Texture0.Sample(SS_Linear, center).rgb; sunSample += TX_Texture0.Sample(SS_Linear, center + float2(texelSize.x * 2.0f, 0.0f)).rgb; sunSample += TX_Texture0.Sample(SS_Linear, center - float2(texelSize.x * 2.0f, 0.0f)).rgb; sunSample += TX_Texture0.Sample(SS_Linear, center + float2(0.0f, texelSize.y * 2.0f)).rgb; sunSample += TX_Texture0.Sample(SS_Linear, center - float2(0.0f, texelSize.y * 2.0f)).rgb; }

float sunVisibility = saturate(LensFlareLuma(sunSample / 5.0f) * 3.0f); float3 lensFlare = BuildLensFlare(Input.vTexcoord, center, aspect, sunVisibility);

return float4(color * GR_ColorMod + lensFlare, 1.0f);
}