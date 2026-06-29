SamplerState SS_PointClamp : register( s0 );
Texture2D TX_AO : register( t0 );

cbuffer AOCompositeConstantBuffer : register( b0 )
{
    float AO_Strength;
    float3 AO_Padding;
}

struct PS_INPUT
{
    float2 vTexcoord : TEXCOORD0;
    float3 vEyeRay   : TEXCOORD1;
    float4 vPosition : SV_POSITION;
};

float4 PSMain( PS_INPUT input ) : SV_TARGET
{
    float visibility = TX_AO.SampleLevel( SS_PointClamp, input.vTexcoord, 0 ).r;
    visibility = saturate( lerp( 1.0, visibility, AO_Strength ) );
    return float4( visibility.xxx, 1.0 );
}
