// D3D11 DoF near/far preparation.
//
// This is the D3D11-compatible part of the FidelityFX-style approach: the
// sharp source is reduced to half resolution twice, once for each depth
// field, while the blur radius is kept with the corresponding color. Keeping
// the fields separate prevents foreground colors from contaminating the far
// blur at silhouettes.

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
Texture2D TX_Scene : register( t0 );
Texture2D TX_Depth : register( t1 );
Texture2D TX_Focus : register( t2 );
Texture2D TX_WaterMask : register( t3 );
Texture2D TX_SpecularMask : register( t4 );

RWTexture2D<float4> OutputNear : register( u0 );
RWTexture2D<float4> OutputFar : register( u1 );

float CameraDistanceFromDepth( float depth, float2 texcoord )
{
    const float viewZ = LinearizeDepthReverseZInfinite( depth );
    const float2 ndc = texcoord * 2.0f - 1.0f;
    const float2 viewXY = ndc * DoF_ProjParams.xy * viewZ;
    return length( float3( viewXY, viewZ ) );
}

float ReflectionReceiverMask( float2 texcoord )
{
    uint width, height;
    TX_WaterMask.GetDimensions( width, height );
    int2 maxPixel = int2( (int)width - 1, (int)height - 1 );
    int2 center = clamp( int2( texcoord * float2( width, height ) ), int2( 0, 0 ), maxPixel );
    int2 left = clamp( center + int2( -1, 0 ), int2( 0, 0 ), maxPixel );
    int2 right = clamp( center + int2( 1, 0 ), int2( 0, 0 ), maxPixel );
    int2 up = clamp( center + int2( 0, -1 ), int2( 0, 0 ), maxPixel );
    int2 down = clamp( center + int2( 0, 1 ), int2( 0, 0 ), maxPixel );

    float centerWater = TX_WaterMask.Load( int3( center, 0 ) ).r;
    float leftWater = TX_WaterMask.Load( int3( left, 0 ) ).r;
    float rightWater = TX_WaterMask.Load( int3( right, 0 ) ).r;
    float upWater = TX_WaterMask.Load( int3( up, 0 ) ).r;
    float downWater = TX_WaterMask.Load( int3( down, 0 ) ).r;

    centerWater = centerWater < 0.75f ? saturate( centerWater / 0.25f ) : 0.0f;
    leftWater = leftWater < 0.75f ? saturate( leftWater / 0.25f ) : 0.0f;
    rightWater = rightWater < 0.75f ? saturate( rightWater / 0.25f ) : 0.0f;
    upWater = upWater < 0.75f ? saturate( upWater / 0.25f ) : 0.0f;
    downWater = downWater < 0.75f ? saturate( downWater / 0.25f ) : 0.0f;

    float mask = max( centerWater, saturate( TX_SpecularMask.Load( int3( center, 0 ) ).z ) );
    mask = max( mask, max( leftWater, saturate( TX_SpecularMask.Load( int3( left, 0 ) ).z ) ) );
    mask = max( mask, max( rightWater, saturate( TX_SpecularMask.Load( int3( right, 0 ) ).z ) ) );
    mask = max( mask, max( upWater, saturate( TX_SpecularMask.Load( int3( up, 0 ) ).z ) ) );
    mask = max( mask, max( downWater, saturate( TX_SpecularMask.Load( int3( down, 0 ) ).z ) ) );
    return mask;
}

float2 ComputeRadii( float depth, float2 texcoord, float focusDepth, float reflectionMask )
{
    if ( depth <= 1e-7f )
        return 0.0f;

    const float linearDepth = CameraDistanceFromDepth( depth, texcoord );
    const float nearRange = max( DoF_NearBlurDistance - DoF_NearPlane, 1.0f );
    const float nearDepth = max( linearDepth, DoF_NearPlane );
    const float nearCoC = saturate( ( DoF_NearBlurDistance - nearDepth ) / nearRange )
        * DoF_NearBlurStrength * ( 1.0f - reflectionMask );
    const float farCoC = saturate( ( linearDepth - focusDepth ) / max( DoF_FocusRange, 0.001f ) );

    // Match the existing near-field footprint while expressing both fields
    // in half-resolution pixels, as in the FidelityFX technique.
    const float nearRadius = min( nearCoC * 3.5f, 5.25f );
    const float farRadius = min( farCoC * DoF_BokehRadius, DoF_MaxBlur );
    return float2( nearRadius, farRadius );
}

[numthreads( 8, 8, 1 )]
void CSMain( uint3 DTid : SV_DispatchThreadID )
{
    uint halfWidth, halfHeight;
    OutputNear.GetDimensions( halfWidth, halfHeight );
    if ( DTid.x >= halfWidth || DTid.y >= halfHeight )
        return;

    uint fullWidth, fullHeight;
    TX_Scene.GetDimensions( fullWidth, fullHeight );
    int2 fullMax = int2( (int)fullWidth - 1, (int)fullHeight - 1 );
    const float focusDepth = TX_Focus.Load( int3( 0, 0, 0 ) ).r;
    const float2 fullSize = float2( fullWidth, fullHeight );

    float3 nearColorAccum = 0.0f;
    float3 farColorAccum = 0.0f;
    float nearWeightAccum = 0.0f;
    float farWeightAccum = 0.0f;
    float maxNearRadius = 0.0f;
    float maxFarRadius = 0.0f;
    const float2 basePixel = float2( DTid.xy * 2u );
    const float reflectionMask = ReflectionReceiverMask( ( basePixel + 1.0f ) / fullSize );

    [unroll]
    for ( uint y = 0; y < 2; ++y )
    {
        [unroll]
        for ( uint x = 0; x < 2; ++x )
        {
            const int2 pixel = clamp( int2( basePixel + float2( x, y ) ), int2( 0, 0 ), fullMax );
            const float2 uv = ( float2( pixel ) + 0.5f ) / fullSize;
            const float depth = TX_Depth.Load( int3( pixel, 0 ) ).r;
            if ( depth <= 1e-7f )
                continue;

            const float2 radii = ComputeRadii( depth, uv, focusDepth, reflectionMask );
            const float3 color = TX_Scene.Load( int3( pixel, 0 ) ).rgb;
            const float nearWeight = radii.x > 0.01f ? ( 1.0f + radii.x ) : 0.0f;
            const float farWeight = radii.y > 0.01f ? ( 1.0f + radii.y ) : 0.0f;

            nearColorAccum += color * nearWeight;
            farColorAccum += color * farWeight;
            nearWeightAccum += nearWeight;
            farWeightAccum += farWeight;
            maxNearRadius = max( maxNearRadius, radii.x );
            maxFarRadius = max( maxFarRadius, radii.y );
        }
    }

    OutputNear[DTid.xy] = float4(
        nearColorAccum / max( nearWeightAccum, 0.001f ), maxNearRadius );
    OutputFar[DTid.xy] = float4(
        farColorAccum / max( farWeightAccum, 0.001f ), maxFarRadius );
}
