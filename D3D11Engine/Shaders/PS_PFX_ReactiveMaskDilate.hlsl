//--------------------------------------------------------------------------------------
// FSR precipitation helper: widens only strong reactive-mask pixels (rain/snow), while
// keeping low alpha-test hints such as foliage/ice at their original narrow value.
//--------------------------------------------------------------------------------------

SamplerState SS_Linear : register( s0 );
Texture2D TX_ReactiveMask : register( t0 );

struct PS_INPUT
{
    float2 vTexcoord : TEXCOORD0;
    float3 vEyeRay : TEXCOORD1;
    float4 vPosition : SV_POSITION;
};

float WeatherMask(float v)
{
    return smoothstep(0.18f, 0.45f, v);
}

float PSMain(PS_INPUT Input) : SV_TARGET
{
    uint width, height;
    TX_ReactiveMask.GetDimensions(width, height);
    float2 texel = 1.0f / float2(max(width, 1), max(height, 1));

    float center = TX_ReactiveMask.SampleLevel(SS_Linear, Input.vTexcoord, 0).r;
    float weather = WeatherMask(center);

    [unroll]
    for (int y = -2; y <= 2; ++y)
    {
        [unroll]
        for (int x = -2; x <= 2; ++x)
        {
            float2 offset = float2(x, y) * texel;
            float sampleValue = TX_ReactiveMask.SampleLevel(SS_Linear, Input.vTexcoord + offset, 0).r;
            weather = max(weather, WeatherMask(sampleValue));
        }
    }

    return saturate(max(center, weather));
}