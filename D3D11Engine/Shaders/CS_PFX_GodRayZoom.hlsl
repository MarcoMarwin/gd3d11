//--------------------------------------------------------------------------------------
// Compute Shader - God Ray Zoom (Radial Blur) Pass
// Reads mask texture, applies radial blur toward sun center, writes to UAV
//--------------------------------------------------------------------------------------

cbuffer GodRayZoomConstantBuffer : register( b0 )
{
    float GR_Decay;
    float GR_Weight;
    float2 GR_Center;

    float GR_Density;
    float3 GR_ColorMod;
};

SamplerState SS_Linear : register( s0 );
Texture2D TX_Texture0 : register( t0 ); // Mask from pass 1

RWTexture2D<float4> OutputTexture : register( u0 );

// Interleaved Gradient Noise for cheap, effective dithering
float InterleavedGradientNoise( float2 uv ) { float3 magic = float3( 0.06711056f, 0.00583715f, 52.9829189f ); return frac( magic.z * frac( dot( uv, magic.xy ) ) ); }

float LensFlareLuma(float3 color) { return dot(color, float3(0.2126f, 0.7152f, 0.0722f)); }

float LensFlareCircle(float2 uv, float2 position, float radius, float aspect) { float2 delta = uv - position; delta.x *= aspect; return 1.0f - smoothstep(radius * 0.35f, radius, length(delta)); }

float3 BuildLensFlare(float2 uv, float2 sunPosition, float aspect, float sunVisibility) { float2 screenCenter = float2(0.5f, 0.5f); float2 flareAxis = screenCenter - sunPosition; float lookAtSun = 1.0f - smoothstep(0.08f, 0.58f, length(flareAxis)); float flareStrength = saturate(sunVisibility) * lookAtSun * max(GR_Weight, 0.0f) * 0.35f; float sunGlow = LensFlareCircle(uv, sunPosition, 0.052f, aspect); float ghost1 = LensFlareCircle(uv, sunPosition + flareAxis * 0.72f, 0.022f, aspect); float ghost2 = LensFlareCircle(uv, sunPosition + flareAxis * 1.46f, 0.032f, aspect); float3 flareColor = 0.0f; flareColor += sunGlow * float3(1.00f, 0.90f, 0.72f) * 0.34f; flareColor += ghost1 * float3(0.62f, 0.72f, 0.88f) * 0.10f; flareColor += ghost2 * float3(0.72f, 0.68f, 0.62f) * 0.07f; return flareColor * flareStrength; }

[numthreads(8, 8, 1)]
void CSMain( uint3 DTid : SV_DispatchThreadID )
{
    uint2 texSize;
    OutputTexture.GetDimensions( texSize.x, texSize.y );

    if ( DTid.x >= texSize.x || DTid.y >= texSize.y )
        return;

    float2 texcoord = ( float2( DTid.xy ) + 0.5 ) / float2( texSize );

    const int NUM_SAMPLES = 64;
    float2 center = GR_Center;
    float3 color = 0;
    float illumDecay = 1.0f;

    float2 deltaTexCoord = texcoord - center;
    deltaTexCoord *= 1.0f / NUM_SAMPLES * GR_Density;

    float2 uv = texcoord;

    // Dithering: Offset the starting UV by a random sub-texel fraction
    float jitter = InterleavedGradientNoise( float2( DTid.xy ) );
    uv -= deltaTexCoord * jitter;

    [unroll(64)]
    for ( int i = 0; i < NUM_SAMPLES; i++ )
    {
        uv -= deltaTexCoord;

        // Anti-Smearing: Prevent sampling out of bounds
        if ( uv.x < 0.0f || uv.x > 1.0f || uv.y < 0.0f || uv.y > 1.0f )
        {
            continue;
        }

        color += TX_Texture0.SampleLevel( SS_Linear, uv, 0 ).rgb * illumDecay * GR_Weight;

        illumDecay *= GR_Decay;
    }
color /= NUM_SAMPLES;

float aspect = texSize.y > 0 ? (float)texSize.x / (float)texSize.y : 1.0f;

float2 texelSize = 1.0f / max( float2(texSize), float2(1.0f, 1.0f));

float3 sunSample = 0.0f;

if (center.x >= 0.0f && center.x <= 1.0f && center.y >= 0.0f && center.y <= 1.0f) { sunSample += TX_Texture0.SampleLevel(SS_Linear, center, 0).rgb; sunSample += TX_Texture0.SampleLevel(SS_Linear, center + float2(texelSize.x * 2.0f, 0.0f), 0).rgb; sunSample += TX_Texture0.SampleLevel(SS_Linear, center - float2(texelSize.x * 2.0f, 0.0f), 0).rgb; sunSample += TX_Texture0.SampleLevel(SS_Linear, center + float2(0.0f, texelSize.y * 2.0f), 0).rgb; sunSample += TX_Texture0.SampleLevel(SS_Linear, center - float2(0.0f, texelSize.y * 2.0f), 0).rgb; }

float sunVisibility = saturate(LensFlareLuma(sunSample / 5.0f) * 3.0f); float3 lensFlare = BuildLensFlare(texcoord, center, aspect, sunVisibility);

OutputTexture[DTid.xy] = float4( color * GR_ColorMod + lensFlare, 1.0f);
}
