//--------------------------------------------------------------------------------------
// PostFX Low Clouds Composite
// Upsamples a premultiplied low-cloud layer and blends it over the full-resolution scene.
//--------------------------------------------------------------------------------------

#include <AtmosphericScattering.h>

SamplerState SS_Linear : register( s0 );
Texture2D TX_Backbuffer : register( t0 );
Texture2D TX_LowClouds : register( t1 );
Texture2D TX_LowCloudDepth : register( t2 );
Texture2D TX_FullDepth : register( t3 );

struct PS_INPUT
{
    float2 vTexcoord : TEXCOORD0;
    float3 vEyeRay : TEXCOORD1;
    float4 vPosition : SV_POSITION;
};

float LoadLowCloudDepth( int2 cloudPixel, int2 cloudSize )
{
    cloudPixel = clamp(
        cloudPixel, int2( 0, 0 ), cloudSize - int2( 1, 1 ) );
    return TX_LowCloudDepth.Load( int3( cloudPixel, 0 ) ).r;
}

float GetLowCloudDepthWeight( float targetDepth, float sourceDepth )
{
    const float skyDepthEpsilon = 0.00001f;
    bool targetIsSky = targetDepth < skyDepthEpsilon;
    bool sourceIsSky = sourceDepth < skyDepthEpsilon;
    if ( targetIsSky != sourceIsSky )
    {
        return 0.0f;
    }
    if ( targetIsSky )
    {
        return 1.0f;
    }

    float relativeDepthDelta = abs( targetDepth - sourceDepth )
        / max( max( targetDepth, sourceDepth ), skyDepthEpsilon );
    if ( relativeDepthDelta >= 0.18f )
    {
        return 0.0f;
    }
    return exp2( -relativeDepthDelta * 32.0f );
}

float4 SampleDepthAwareLowClouds(
    float2 texcoord, float4 pixelPosition )
{
    uint cloudWidth;
    uint cloudHeight;
    uint depthWidth;
    uint depthHeight;
    TX_LowClouds.GetDimensions( cloudWidth, cloudHeight );
    TX_FullDepth.GetDimensions( depthWidth, depthHeight );

    int2 cloudSize = max( int2( cloudWidth, cloudHeight ), int2( 1, 1 ) );
    int2 depthSize = max( int2( depthWidth, depthHeight ), int2( 1, 1 ) );
    int2 targetPixel = clamp(
        int2( pixelPosition.xy ), int2( 0, 0 ), depthSize - int2( 1, 1 ) );
    float targetDepth = TX_FullDepth.Load( int3( targetPixel, 0 ) ).r;

    float2 cloudPosition = texcoord * float2( cloudSize ) - 0.5f;
    int2 baseCloudPixel = int2( floor( cloudPosition ) );
    float2 cloudFraction = frac( cloudPosition );

    float4 filteredClouds = 0.0f;
    float totalWeight = 0.0f;
    [unroll]
    for ( int y = 0; y < 2; ++y )
    {
        [unroll]
        for ( int x = 0; x < 2; ++x )
        {
            int2 cloudPixel = clamp(
                baseCloudPixel + int2( x, y ),
                int2( 0, 0 ), cloudSize - int2( 1, 1 ) );
            float spatialWeight =
                (x == 0 ? 1.0f - cloudFraction.x : cloudFraction.x)
                * (y == 0 ? 1.0f - cloudFraction.y : cloudFraction.y);
            float sourceDepth = LoadLowCloudDepth(
                cloudPixel, cloudSize );
            float weight = spatialWeight
                * GetLowCloudDepthWeight( targetDepth, sourceDepth );
            filteredClouds +=
                TX_LowClouds.Load( int3( cloudPixel, 0 ) ) * weight;
            totalWeight += weight;
        }
    }

    if ( totalWeight > 0.00001f )
    {
        return filteredClouds / totalWeight;
    }

    // Thin silhouettes can miss the four bilinear taps. Search only nearby
    // samples from the same depth class instead of leaking sky clouds across them.
    const float skyDepthEpsilon = 0.00001f;
    bool targetIsSky = targetDepth < skyDepthEpsilon;
    float bestMetric = 1000000.0f;
    float4 bestClouds = 0.0f;
    bool foundCompatibleSample = false;
    [unroll]
    for ( int searchY = -1; searchY <= 1; ++searchY )
    {
        [unroll]
        for ( int searchX = -1; searchX <= 1; ++searchX )
        {
            int2 cloudPixel = clamp(
                int2( floor( cloudPosition + 0.5f ) )
                    + int2( searchX, searchY ),
                int2( 0, 0 ), cloudSize - int2( 1, 1 ) );
            float sourceDepth = LoadLowCloudDepth(
                cloudPixel, cloudSize );
            bool sourceIsSky = sourceDepth < skyDepthEpsilon;
            if ( sourceIsSky != targetIsSky )
            {
                continue;
            }

            float relativeDepthDelta = 0.0f;
            if ( !targetIsSky )
            {
                relativeDepthDelta = abs( targetDepth - sourceDepth )
                    / max( max( targetDepth, sourceDepth ), skyDepthEpsilon );
                if ( relativeDepthDelta >= 0.35f )
                {
                    continue;
                }
            }

            float2 spatialDelta = float2( cloudPixel ) - cloudPosition;
            float metric = dot( spatialDelta, spatialDelta )
                + relativeDepthDelta * 16.0f;
            if ( metric < bestMetric )
            {
                bestMetric = metric;
                bestClouds = TX_LowClouds.Load( int3( cloudPixel, 0 ) );
                foundCompatibleSample = true;
            }
        }
    }

    if ( foundCompatibleSample )
    {
        return bestClouds;
    }

    // Very distant alpha-tested vegetation often contributes a foreground depth pixel
    // while the half-res low-cloud layer only saw sky around it. In that case, keeping
    // clouds out makes the tree contour reveal raw sky. For far silhouettes, use the
    // nearby cloud layer as the background veil instead of punching a cloud hole.
    if ( !targetIsSky && targetDepth < 0.020f )
    {
        float4 fallbackClouds = TX_LowClouds.SampleLevel( SS_Linear, texcoord, 0 );
        if ( fallbackClouds.a > 0.001f )
        {
            return fallbackClouds;
        }
    }

    return float4( 0.0f, 0.0f, 0.0f, 0.0f );
}

float4 PSMain( PS_INPUT Input ) : SV_TARGET
{
    float4 scene = TX_Backbuffer.Sample( SS_Linear, Input.vTexcoord );
    float4 clouds = SampleDepthAwareLowClouds(
        Input.vTexcoord, Input.vPosition );

    float rainWeight = saturate(AC_RainFXWeight);
    float nightTimeBlend = smoothstep(0.0f, 1.0f, saturate(-AC_LightPos.y * 4.0f))
        * saturate(AC_EnableNightAtmosphere);
    float rainCloudVisibility = 1.0f - smoothstep(0.18f, 0.88f, rainWeight);
    float rainVeil = rainWeight * lerp(0.050f, 0.22f, nightTimeBlend);
    float dryNightVeil = (1.0f - rainWeight) * nightTimeBlend * 0.12f;
    float totalVeil = saturate(rainVeil + dryNightVeil);
    float cloudAlpha = saturate(clouds.a) * rainCloudVisibility;
    clouds.rgb *= rainCloudVisibility;

    if (cloudAlpha > 0.001f && totalVeil > 0.0001f)
    {
        float3 cloudColor = clouds.rgb / max(cloudAlpha, 0.001f);
        cloudColor = lerp(cloudColor, scene.rgb, totalVeil * lerp(0.65f, 1.0f, nightTimeBlend));
        cloudAlpha *= 1.0f - totalVeil * lerp(0.08f, 0.22f, nightTimeBlend);
        clouds.rgb = cloudColor * cloudAlpha;
        clouds.a = cloudAlpha;
    }

    scene.rgb = scene.rgb * (1.0f - cloudAlpha) + clouds.rgb;
    return scene;
}