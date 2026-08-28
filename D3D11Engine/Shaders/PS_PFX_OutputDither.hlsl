SamplerState SS_Linear : register( s0 );
Texture2D TX_Texture0 : register( t0 );
Texture2D TX_OutputBlueNoise : register( t1 );

struct PS_INPUT
{
    float2 vTexcoord : TEXCOORD0;
    float3 vEyeRay : TEXCOORD1;
    float4 vPosition : SV_POSITION;
};

float4 PSMain( PS_INPUT Input ) : SV_TARGET
{
    float4 color = TX_Texture0.Sample( SS_Linear, Input.vTexcoord );
    float luminance = dot( color.rgb, float3( 0.2126f, 0.7152f, 0.0722f ) );
    float darkGradientWeight = 1.0f - smoothstep( 0.45f, 0.85f, luminance );
    uint2 noiseCoord = uint2( max( Input.vPosition.xy, 0.0f ) ) & uint2( 511u, 511u );
    float noise = TX_OutputBlueNoise.Load( int3( noiseCoord, 0 ) ).r * 2.0f - 1.0f;
    color.rgb = saturate( color.rgb + noise * ( 1.0f / 255.0f ) * darkGradientWeight );
    return color;
}
