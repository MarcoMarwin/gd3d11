// D3D11 near/far bokeh blur.
// The two fields are blurred independently so foreground silhouettes do not
// inherit the background color. The kernel follows the FidelityFX ring idea,
// but uses SM5-compatible texture sampling instead of wave intrinsics.

cbuffer DepthOfFieldConstantBuffer : register( b0 )
{
    float DoF_FocusDistance;
    float DoF_FocusRange;
    float DoF_BokehRadius;
    float DoF_MaxBlur;

    float4 DoF_ProjParams;
    float DoF_NearPlane;
    float DoF_FarPlane;
    float DoF_NearBlurDistance;
    float DoF_NearBlurStrength;
};

SamplerState SS_Linear : register( s0 );
Texture2D TX_NearSource : register( t0 );
Texture2D TX_FarSource : register( t1 );
RWTexture2D<float4> OutputNear : register( u0 );
RWTexture2D<float4> OutputFar : register( u1 );

float2 GetSpiralSample( int index, int count )
{
    const float radius = sqrt( ( float( index ) + 0.5f ) / float( count ) );
    const float angle = float( index ) * 2.39996323f;
    return float2( cos( angle ), sin( angle ) ) * radius;
}

float3 AccumulateField(
    Texture2D source,
    float2 texcoord,
    float2 texelSize,
    float radius,
    bool allowFarFill,
    out float accumulatedWeight )
{
#ifdef DOF_GAUSS_BLUR
    static const int SAMPLE_COUNT = 16;
#else
    static const int SAMPLE_COUNT = 32;
#endif

    float4 center = source.SampleLevel( SS_Linear, texcoord, 0 );
    float3 colorAccum = center.rgb;
    float weightAccum = 1.0f;
    const float effectiveRadius = max( radius, 0.5f );

    [unroll]
    for ( int i = 0; i < SAMPLE_COUNT; ++i )
    {
        const float2 offset = GetSpiralSample( i, SAMPLE_COUNT );
        const float2 sampleUV = texcoord + offset * effectiveRadius * texelSize;
        float4 sample = source.SampleLevel( SS_Linear, sampleUV, 0 );
        float sampleRadius = sample.a;

        if ( sampleRadius <= 0.01f && allowFarFill )
        {
            // FidelityFX-style near-field hole filling: a small amount of
            // valid far color closes undersampled foreground silhouettes.
            sample = TX_FarSource.SampleLevel( SS_Linear, sampleUV, 0 );
            sampleRadius = sample.a;
            sample.a *= 0.18f;
        }

        const float sampleDistance = max( length( offset ) * effectiveRadius, 0.5f );
        const float radiusCoverage = saturate( sampleRadius / sampleDistance );
        const float radialWeight = exp( -dot( offset, offset ) * 2.4f );
        const float weight = radialWeight * radiusCoverage * sample.a;

        colorAccum += sample.rgb * weight;
        weightAccum += weight;
    }

    accumulatedWeight = weightAccum;
    return colorAccum / max( weightAccum, 0.001f );
}

[numthreads( 8, 8, 1 )]
void CSMain( uint3 DTid : SV_DispatchThreadID )
{
    uint width, height;
    OutputNear.GetDimensions( width, height );
    if ( DTid.x >= width || DTid.y >= height )
        return;

    const float2 texcoord = ( float2( DTid.xy ) + 0.5f ) / float2( width, height );
    const float2 texelSize = 1.0f / float2( width, height );
    const float nearRadius = TX_NearSource.SampleLevel( SS_Linear, texcoord, 0 ).a;
    const float farRadius = TX_FarSource.SampleLevel( SS_Linear, texcoord, 0 ).a;

    if ( nearRadius > 0.01f )
    {
        float weight;
        const float3 nearColor = AccumulateField(
            TX_NearSource, texcoord, texelSize, nearRadius, true, weight );
        OutputNear[DTid.xy] = float4( nearColor, 1.0f );
    }
    else
    {
        OutputNear[DTid.xy] = 0.0f;
    }

    if ( farRadius > 0.01f )
    {
        float weight;
        const float3 farColor = AccumulateField(
            TX_FarSource, texcoord, texelSize, farRadius, false, weight );
        OutputFar[DTid.xy] = float4( farColor, 1.0f );
    }
    else
    {
        OutputFar[DTid.xy] = 0.0f;
    }
}
