//--------------------------------------------------------------------------------------
// Multi-resolution bloom combine pass
//--------------------------------------------------------------------------------------

SamplerState SS_Linear : register( s0 );
Texture2D TX_BaseBloom : register( t0 );
Texture2D TX_WideBloom : register( t1 );

cbuffer BloomCombineSettings : register( b0 )
{
    float BC_BaseWeight;
    float BC_WideWeight;
    float2 BC_Pad;
};

struct PS_INPUT
{
    float2 vTexcoord : TEXCOORD0;
    float3 vEyeRay : TEXCOORD1;
    float4 vPosition : SV_POSITION;
};

float4 PSMain( PS_INPUT Input ) : SV_TARGET
{
    float3 baseBloom = TX_BaseBloom.Sample( SS_Linear, Input.vTexcoord ).rgb;
    float3 wideBloom = TX_WideBloom.Sample( SS_Linear, Input.vTexcoord ).rgb;
    return float4( baseBloom * BC_BaseWeight + wideBloom * BC_WideWeight, 1.0f );
}