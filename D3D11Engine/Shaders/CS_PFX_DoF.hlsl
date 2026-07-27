//--------------------------------------------------------------------------------------
// Depth of Field Blur Pass
//--------------------------------------------------------------------------------------

#include "DepthReconstruction.h"

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
Texture2D TX_Scene : register( t0 );   // Full-res scene color
Texture2D TX_Depth : register( t1 );   // Full-res hardware depth
Texture2D TX_Focus : register( t2 );   // 1x1 R32_FLOAT smoothed focus depth
Texture2D TX_WaterMask : register(t3);
Texture2D TX_SpecularMask : register(t4);

RWTexture2D<float4> OutputBlur : register( u0 ); // Half-res output

float CameraDistanceFromDepth( float d, float2 texcoord )
{
    const float viewZ = LinearizeDepthReverseZInfinite( d );
    const float2 ndc = texcoord * 2.0f - 1.0f;
    const float2 viewXY = ndc * DoF_ProjParams.xy * viewZ;
    return length( float3( viewXY, viewZ ) );
}

float ReflectionReceiverMask(float2 texcoord)
{
    uint width, height;
    TX_WaterMask.GetDimensions(width, height);

    int2 maxPixel = int2((int)width - 1, (int)height - 1);
    int2 center = clamp(int2(texcoord * float2(width, height)), int2(0, 0), maxPixel);
    int2 left = clamp(center + int2(-1, 0), int2(0, 0), maxPixel);
    int2 right = clamp(center + int2(1, 0), int2(0, 0), maxPixel);
    int2 up = clamp(center + int2(0, -1), int2(0, 0), maxPixel);
    int2 down = clamp(center + int2(0, 1), int2(0, 0), maxPixel);

    float centerWaterMask = TX_WaterMask.Load(int3(center, 0)).r;
    float leftWaterMask = TX_WaterMask.Load(int3(left, 0)).r;
    float rightWaterMask = TX_WaterMask.Load(int3(right, 0)).r;
    float upWaterMask = TX_WaterMask.Load(int3(up, 0)).r;
    float downWaterMask = TX_WaterMask.Load(int3(down, 0)).r;

    centerWaterMask = centerWaterMask < 0.75f ? saturate(centerWaterMask / 0.25f) : 0.0f;
    leftWaterMask = leftWaterMask < 0.75f ? saturate(leftWaterMask / 0.25f) : 0.0f;
    rightWaterMask = rightWaterMask < 0.75f ? saturate(rightWaterMask / 0.25f) : 0.0f;
    upWaterMask = upWaterMask < 0.75f ? saturate(upWaterMask / 0.25f) : 0.0f;
    downWaterMask = downWaterMask < 0.75f ? saturate(downWaterMask / 0.25f) : 0.0f;

    float reflectionMask = max(
        centerWaterMask,
        saturate(TX_SpecularMask.Load(int3(center, 0)).z));

    reflectionMask = max(reflectionMask, max(
        leftWaterMask,
        saturate(TX_SpecularMask.Load(int3(left, 0)).z)));

    reflectionMask = max(reflectionMask, max(
        rightWaterMask,
        saturate(TX_SpecularMask.Load(int3(right, 0)).z)));

    reflectionMask = max(reflectionMask, max(
        upWaterMask,
        saturate(TX_SpecularMask.Load(int3(up, 0)).z)));

    reflectionMask = max(reflectionMask, max(
        downWaterMask,
        saturate(TX_SpecularMask.Load(int3(down, 0)).z)));

    return reflectionMask;
}


float ComputeNearCoC( float linearDepth )
{
    const float nearRange = max( DoF_NearBlurDistance - DoF_NearPlane, 1.0f );
    const float nearDepth = max( linearDepth, DoF_NearPlane );
    return saturate( ( DoF_NearBlurDistance - nearDepth ) / nearRange )
        * DoF_NearBlurStrength;
}

float ComputeCoC( float linearDepth, float focusDepth, float2 texcoord )
{
    const float farCoC = saturate( ( linearDepth - focusDepth ) / DoF_FocusRange );
    const float nearCoC=ComputeNearCoC(linearDepth)*(1.0f-ReflectionReceiverMask(texcoord));
    return max( farCoC, nearCoC );
}

bool IsSkyDepth( float depth )
{
    return depth <= 1e-7f;
}

static const int SAMPLE_COUNT = 48;

float2 GetSpiralSample( int index, int count )
{
    float r = sqrt( ( float(index) + 0.5 ) / float(count) );
    float theta = float(index) * 2.39996323;
    return float2( r * cos( theta ), r * sin( theta ) );
}

[numthreads(8, 8, 1)]
void CSMain( uint3 DTid : SV_DispatchThreadID )
{
    uint2 outSize;
    OutputBlur.GetDimensions( outSize.x, outSize.y );

    if ( DTid.x >= outSize.x || DTid.y >= outSize.y )
        return;

    float2 texcoord = ( float2( DTid.xy ) + 0.5 ) / float2( outSize );

    // Texel size of the full-res scene for sampling offsets
    float2 sceneSize;
    TX_Scene.GetDimensions( sceneSize.x, sceneSize.y );
    float2 texelSize = 1.0 / sceneSize;

    float focusDepth = TX_Focus.SampleLevel( SS_Linear, float2( 0.5, 0.5 ), 0 ).r;

    float centerDepth = TX_Depth.SampleLevel( SS_Linear, texcoord, 0 ).r;
    float3 centerColor = TX_Scene.SampleLevel( SS_Linear, texcoord, 0 ).rgb;
    if ( IsSkyDepth( centerDepth ) )
    {
        OutputBlur[DTid.xy] = float4( centerColor, 0.0f );
        return;
    }

    float centerLinear = CameraDistanceFromDepth( centerDepth, texcoord );
    float centerCoC = ComputeCoC( centerLinear, focusDepth, texcoord );

    // Early out: pass through sharp pixel
    if ( centerCoC < 0.01 )
    {
        OutputBlur[DTid.xy] = float4( centerColor, 0.0 );
        return;
    }
    const float nearCoC = ComputeNearCoC( centerLinear )
        * (1.0f - ReflectionReceiverMask(texcoord));
    const float nearBlurRadius = 3.5f;
    const float nearMaxBlur = 5.25f;
    float blurRadius = nearCoC > 0.001f
        ? min( nearCoC * nearBlurRadius, nearMaxBlur )
        : min( centerCoC * DoF_BokehRadius, DoF_MaxBlur );

#ifdef DOF_GAUSS_BLUR
    // --- Simple Gaussian blur (16 taps) ---
    // Uses a radial Gaussian kernel with exp(-r^2 * 3) weights.
    // Much cheaper than the bokeh path; no highlight boost or
    // foreground rejection — just a smooth, uniform blur.
    static const int GAUSS_SAMPLE_COUNT = 16;

    float3 colorAccum = centerColor;
    float weightAccum = 1.0f;

    [unroll]
    for ( int i = 0; i < GAUSS_SAMPLE_COUNT; i++ )
    {
        float2 offset = GetSpiralSample( i, GAUSS_SAMPLE_COUNT );
        float2 sampleUV = texcoord + offset * blurRadius * texelSize;

        float3 sampleColor = TX_Scene.SampleLevel( SS_Linear, sampleUV, 0 ).rgb;
        float sampleDepth = TX_Depth.SampleLevel( SS_Linear, sampleUV, 0 ).r;

        float r2 = dot( offset, offset );
        float weight = IsSkyDepth( sampleDepth ) ? 0.0f : exp( -r2 * 3.0f );

        colorAccum += sampleColor * weight;
        weightAccum += weight;
    }
#else
    // --- Bokeh spiral blur (48 taps) ---
    // Seed accumulator with the center pixel so that if all 48 spiral
    // samples are foreground-rejected (e.g. background visible through
    // a leaf gap), the result falls back to the center color instead
    // of producing black.  Weight uses the same luminance-boost formula
    // applied to every spiral sample.
    float centerLum = dot( centerColor, float3( 0.2126, 0.7152, 0.0722 ) );
    float3 colorAccum = centerColor * ( 1.0 + centerLum * 2.0 );
    float weightAccum = 1.0 + centerLum * 2.0;

    [unroll]
    for ( int i = 0; i < SAMPLE_COUNT; i++ )
    {
        float2 offset = GetSpiralSample( i, SAMPLE_COUNT );
        float2 sampleUV = texcoord + offset * blurRadius * texelSize;

        float3 sampleColor = TX_Scene.SampleLevel( SS_Linear, sampleUV, 0 ).rgb;
        float sampleDepth = TX_Depth.SampleLevel( SS_Linear, sampleUV, 0 ).r;
        if ( IsSkyDepth( sampleDepth ) )
            continue;

        float sampleLinear = CameraDistanceFromDepth( sampleDepth, sampleUV );
        float sampleCoC = ComputeCoC( sampleLinear, focusDepth, sampleUV );

        float weight = ( sampleCoC >= length( offset ) * centerCoC ) ? 1.0 : sampleCoC;

        // Asymmetric foreground rejection
        float depthMargin = max( centerLinear * 0.05, 5.0 );
        float foregroundReject = saturate( ( sampleLinear - centerLinear + depthMargin ) / depthMargin );
        weight *= foregroundReject;

        float luminance = dot( sampleColor, float3( 0.2126, 0.7152, 0.0722 ) );
        weight *= 1.0 + luminance * 2.0;

        colorAccum += sampleColor * weight;
        weightAccum += weight;
    }
#endif

    colorAccum /= max( weightAccum, 0.001 );

    OutputBlur[DTid.xy] = float4( colorAccum, centerCoC );
}
