// Alpha-aware exclusion mask for transparent world geometry.

SamplerState SS_Linear : register(s0);
Texture2D TX_Texture0 : register(t0);

struct FFData
{
    float4 textureFactor;
};

cbuffer cbFFData : register(b0)
{
    FFData cbFFData;
};

struct PS_INPUT
{
    float2 vTexcoord : TEXCOORD0;
    float4 vDiffuse : TEXCOORD2;
    float4 vPosition : SV_POSITION;
};

float PSMain(PS_INPUT input) : SV_TARGET
{
    float textureAlpha = TX_Texture0.Sample(SS_Linear, input.vTexcoord).a;
    float visibleCoverage = saturate(textureAlpha * input.vDiffuse.a * cbFFData.textureFactor.a);
    clip(visibleCoverage - (1.0f / 255.0f));

    // Every visible transparent-world fragment fully blocks Wet Ground SSR.
    return 1.0f;
}
