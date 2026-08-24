#include "DS_Defines.h"

SamplerState SS_PointClamp : register( s0 );
Texture2D<float>  TX_AO_Full       : register( t0 );
Texture2D<float>  TX_AO_Far        : register( t1 );
Texture2D<float>  TX_WorkingDepth  : register( t2 );
Texture2D<float>  TX_FarDepth      : register( t3 );
Texture2D<float2> TX_Normals       : register( t4 );

cbuffer AOCompositeHybridConstantBuffer : register( b0 )
{
    float AO_Strength;
    float FarRadiusPixels;
    float EffectRadius;
    float PixelScaleX;
    float FullWidth;
    float FullHeight;
    float FarWidth;
    float FarHeight;
}

struct PS_INPUT
{
    float2 vTexcoord : TEXCOORD0;
    float3 vEyeRay   : TEXCOORD1;
    float4 vPosition : SV_POSITION;
};

float LoadFarAO( int2 coord )
{
    int2 maxCoord = int2( FarWidth, FarHeight ) - 1;
    coord = clamp( coord, int2( 0, 0 ), maxCoord );
    return TX_AO_Far.Load( int3( coord, 0 ) ).r;
}

float LoadFarDepth( int2 coord )
{
    int2 maxCoord = int2( FarWidth, FarHeight ) - 1;
    coord = clamp( coord, int2( 0, 0 ), maxCoord );
    return TX_FarDepth.Load( int3( coord, 0 ) ).r;
}

float3 LoadFullNormal( int2 coord )
{
    int2 maxCoord = int2( FullWidth, FullHeight ) - 1;
    coord = clamp( coord, int2( 0, 0 ), maxCoord );
    return DecodeNormalGBuffer( TX_Normals.Load( int3( coord, 0 ) ).xy );
}

float3 LoadFarNormal( int2 coord )
{
    int2 fullCoord = coord * 2 + 1;
    return LoadFullNormal( fullCoord );
}

float BilateralFarAO( float2 uv, float centerDepth, float3 centerNormal )
{
    float2 farCoord = uv * float2( FarWidth, FarHeight ) - 0.5;
    int2 baseCoord = int2( floor( farCoord ) );
    float2 fractional = saturate( frac( farCoord ) );

    float result = 0.0;
    float weightSum = 0.0;
    const float depthSigma = max( 0.05 * max( centerDepth, 1.0 ), 0.5 );

    [unroll]
    for ( int y = 0; y < 2; ++y )
    {
        [unroll]
        for ( int x = 0; x < 2; ++x )
        {
            int2 sampleCoord = baseCoord + int2( x, y );
            float bilinearWeight = ( x == 0 ? 1.0 - fractional.x : fractional.x )
                * ( y == 0 ? 1.0 - fractional.y : fractional.y );
            float sampleDepth = LoadFarDepth( sampleCoord );
            float depthWeight = saturate( 1.0 - abs( sampleDepth - centerDepth ) / depthSigma );
            float normalWeight = saturate( ( dot( centerNormal, LoadFarNormal( sampleCoord ) ) - 0.5 ) * 2.0 );
            // Keep a small floor so a depth/normal discontinuity cannot produce
            // an undefined result; the dominant weight still remains bilateral.
            float weight = bilinearWeight * ( 0.15 + 0.85 * depthWeight )
                * ( 0.25 + 0.75 * normalWeight );
            result += LoadFarAO( sampleCoord ) * weight;
            weightSum += weight;
        }
    }

    return weightSum > 0.0001 ? result / weightSum : LoadFarAO( int2( farCoord ) );
}

float4 PSMain( PS_INPUT input ) : SV_TARGET
{
    int2 fullCoord = clamp( int2( input.vPosition.xy ), int2( 0, 0 ), int2( FullWidth, FullHeight ) - 1 );
    float centerDepth = TX_WorkingDepth.Load( int3( fullCoord, 0 ) ).r;
    float screenRadius = EffectRadius / max( centerDepth * PixelScaleX, 0.0001 );
    bool useFarPath = screenRadius <= FarRadiusPixels;

    float visibility = useFarPath
        ? BilateralFarAO( input.vTexcoord, centerDepth, LoadFullNormal( fullCoord ) )
        : TX_AO_Full.SampleLevel( SS_PointClamp, input.vTexcoord, 0 ).r;
    visibility = saturate( lerp( 1.0, visibility, AO_Strength ) );
    return float4( visibility.xxx, 1.0 );
}
