//--------------------------------------------------------------------------------------
// FSR3 Transparency/Composition mask from screen-space lighting.
// Marks contact-shadow variation for temporal upscaling without changing lighting output.
//--------------------------------------------------------------------------------------

SamplerState SS_Linear : register( s0 );
Texture2D TX_ScreenSpaceLighting : register( t0 );

struct PS_INPUT
{
    float2 vTexcoord : TEXCOORD0;
    float3 vEyeRay : TEXCOORD1;
    float4 vPosition : SV_POSITION;
};

float4 PSMain( PS_INPUT Input ) : SV_TARGET
{
    float contactShadow = saturate(TX_ScreenSpaceLighting.SampleLevel(SS_Linear, Input.vTexcoord, 0).a);
    float mask = saturate(contactShadow * 0.55f);
    return float4(mask, mask, mask, mask);
}