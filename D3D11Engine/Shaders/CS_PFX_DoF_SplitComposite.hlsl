// D3D11 near/far DoF composite with the existing renderer controls.

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
Texture2D TX_NearBlur : register( t3 );
Texture2D TX_FarBlur : register( t4 );
Texture2D TX_WaterMask : register( t5 );
Texture2D TX_SpecularMask : register( t6 );
RWTexture2D<float4> OutputComposite : register( u0 );

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

float2 ComputeRadii( float depth, float2 texcoord, float focusDepth )
{
    if ( depth <= 1e-7f )
        return 0.0f;

    const float linearDepth = CameraDistanceFromDepth( depth, texcoord );
    const float nearRange = max( DoF_NearBlurDistance - DoF_NearPlane, 1.0f );
    const float nearDepth = max( linearDepth, DoF_NearPlane );
    const float nearCoC = saturate( ( DoF_NearBlurDistance - nearDepth ) / nearRange )
        * DoF_NearBlurStrength * ( 1.0f - ReflectionReceiverMask( texcoord ) );
    const float farCoC = saturate( ( linearDepth - focusDepth ) / max( DoF_FocusRange, 0.001f ) );
    return float2(
        min( nearCoC * 3.5f, 5.25f ),
        min( farCoC * DoF_BokehRadius, DoF_MaxBlur ) );
}

float2 GetSkyEdgeSpiralSample( int index )
{
    const float radius = sqrt( ( float( index ) + 0.5f ) / 24.0f );
    const float angle = float( index ) * 2.39996323f;
    return float2( cos( angle ), sin( angle ) ) * radius;
}

float4 GetSkyEdgeBlurSample( float2 texcoord, float2 depthTexel, float focusDepth )
{
    float3 colorAccum = 0.0f;
    float weightAccum = 0.0f;
    const float edgeRadius = clamp( max( DoF_BokehRadius, DoF_MaxBlur ) * 0.22f, 2.0f, 10.0f );

    [unroll]
    for ( int i = 0; i < 24; ++i )
    {
        const float2 offset = GetSkyEdgeSpiralSample( i );
        const float2 sampleUV = texcoord + offset * edgeRadius * depthTexel;
        const float depth = TX_Depth.SampleLevel( SS_Linear, sampleUV, 0 ).r;
        if ( depth <= 1e-7f )
            continue;

        const float2 radii = ComputeRadii( depth, sampleUV, focusDepth );
        const float farCoverage = smoothstep( 0.05f, max( DoF_BokehRadius, 0.1f ), radii.y );
        const float radialWeight = exp( -dot( offset, offset ) * 2.4f );
        const float weight = radialWeight * farCoverage;
        colorAccum += TX_FarBlur.SampleLevel( SS_Linear, sampleUV, 0 ).rgb * weight;
        weightAccum += weight;
    }

    const float coverage = saturate( weightAccum / 8.0f );
    return float4( colorAccum / max( weightAccum, 0.001f ), smoothstep( 0.01f, 0.8f, coverage ) );
}

[numthreads( 8, 8, 1 )]
void CSMain( uint3 DTid : SV_DispatchThreadID )
{
    uint width, height;
    OutputComposite.GetDimensions( width, height );
    if ( DTid.x >= width || DTid.y >= height )
        return;

    const float2 texcoord = ( float2( DTid.xy ) + 0.5f ) / float2( width, height );
    const float3 sharpColor = TX_Scene.Load( int3( DTid.xy, 0 ) ).rgb;
    const float depth = TX_Depth.Load( int3( DTid.xy, 0 ) ).r;
    const float focusDepth = TX_Focus.Load( int3( 0, 0, 0 ) ).r;

    if ( depth <= 1e-7f )
    {
        const float2 depthTexel = 1.0f / float2( width, height );
        const float4 skyBlur = GetSkyEdgeBlurSample( texcoord, depthTexel, focusDepth );
        OutputComposite[DTid.xy] = float4( lerp( sharpColor, skyBlur.rgb, skyBlur.a ), 1.0f );
        return;
    }

    const float2 radii = ComputeRadii( depth, texcoord, focusDepth );
    const float4 nearBlur = TX_NearBlur.SampleLevel( SS_Linear, texcoord, 0 );
    const float4 farBlur = TX_FarBlur.SampleLevel( SS_Linear, texcoord, 0 );

    // The normalized thresholds retain the response of the existing controls:
    // DoF_NearBlurStrength controls foreground coverage and DoF_BokehRadius
    // controls the far-field transition.
    const float nearBlend = smoothstep( 0.05f, 1.5f, radii.x ) * nearBlur.a;
    const float farBlend = smoothstep( 0.05f, max( DoF_BokehRadius, 0.1f ), radii.y ) * farBlur.a;

    float3 finalColor = lerp( sharpColor, farBlur.rgb, saturate( farBlend ) );
    // Foreground is composed last, preserving the intended near-object
    // occlusion over the far-field result.
    finalColor = lerp( finalColor, nearBlur.rgb, saturate( nearBlend ) );
    OutputComposite[DTid.xy] = float4( finalColor, 1.0f );
}
