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

float4 SampleStableSkyLowClouds( float2 texcoord )
{
    uint cloudWidth;
    uint cloudHeight;
    TX_LowClouds.GetDimensions( cloudWidth, cloudHeight );
    float2 texel = 1.0f / max( float2( cloudWidth, cloudHeight ), float2( 1.0f, 1.0f ) );

    float4 center = TX_LowClouds.SampleLevel( SS_Linear, texcoord, 0 );
    float4 bestClouds = center;
    float4 accum = center * 4.0f;
    float totalWeight = 4.0f;

    [unroll]
    for ( int y = -1; y <= 1; ++y )
    {
        [unroll]
        for ( int x = -1; x <= 1; ++x )
        {
            if ( x == 0 && y == 0 )
            {
                continue;
            }

            float2 offset = float2( x, y ) * texel;
            float4 sampleClouds = TX_LowClouds.SampleLevel( SS_Linear, texcoord + offset, 0 );
            float weight = lerp( 0.35f, 0.85f, saturate( sampleClouds.a * 2.0f ) );
            accum += sampleClouds * weight;
            totalWeight += weight;

            if ( sampleClouds.a > bestClouds.a )
            {
                bestClouds = sampleClouds;
            }
        }
    }

    float4 filteredClouds = accum / max( totalWeight, 0.00001f );
    return lerp( filteredClouds, bestClouds, 0.35f );
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

    const float skyDepthEpsilon = 0.00001f;
    bool targetIsSky = targetDepth < skyDepthEpsilon;

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

    // A confident 2x2 bilateral footprint is stable and remains the fast path.
    // Sparse alpha-tested silhouettes must not normalize a tiny single tap to
    // full cloud coverage because that tap changes with sub-pixel vegetation.
    const float confidentFootprintWeight = 0.60f;
    if ( totalWeight >= confidentFootprintWeight )
    {
        return filteredClouds / totalWeight;
    }

    // Reconstruct uncertain edges from a wider same-class neighborhood. Blend all
    // compatible samples instead of selecting one winner, so distant foliage keeps
    // a temporally stable cloud layer while sky and geometry can never contaminate
    // each other. The wider depth window is strongly weighted toward the target.
    float4 stableClouds = 0.0f;
    float stableWeight = 0.0f;
    int2 searchCenter = int2( floor( cloudPosition + 0.5f ) );
    [unroll]
    for ( int searchY = -2; searchY <= 2; ++searchY )
    {
        [unroll]
        for ( int searchX = -2; searchX <= 2; ++searchX )
        {
            int2 cloudPixel = clamp(
                searchCenter + int2( searchX, searchY ),
                int2( 0, 0 ), cloudSize - int2( 1, 1 ) );
            float sourceDepth = LoadLowCloudDepth(
                cloudPixel, cloudSize );
            bool sourceIsSky = sourceDepth < skyDepthEpsilon;
            if ( sourceIsSky != targetIsSky )
            {
                continue;
            }

            float relativeDepthDelta = 0.0f;
            float depthWeight = 1.0f;
            if ( !targetIsSky )
            {
                relativeDepthDelta = abs( targetDepth - sourceDepth )
                    / max( max( targetDepth, sourceDepth ), skyDepthEpsilon );
                if ( relativeDepthDelta >= 0.30f )
                {
                    continue;
                }
                depthWeight = exp2( -relativeDepthDelta * 24.0f );
            }

            float2 spatialDelta = float2( cloudPixel ) - cloudPosition;
            float spatialWeight = exp2(
                -dot( spatialDelta, spatialDelta ) * 0.65f );
            float weight = spatialWeight * depthWeight;
            stableClouds += TX_LowClouds.Load(
                int3( cloudPixel, 0 ) ) * weight;
            stableWeight += weight;
        }
    }

    if ( stableWeight > 0.00001f )
    {
        return stableClouds / stableWeight;
    }
    // Real sky pixels inside alpha-tested tree gaps keep Build 140's stable
    // fallback. Geometry deliberately has no sky-colour fallback; it can receive
    // clouds only from depth-compatible raymarch samples above.
    if ( targetIsSky )
    {
        float4 fallbackClouds = SampleStableSkyLowClouds( texcoord );
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

    float2 sunDelta = Input.vTexcoord - AC_LightScreenPos.xy;
    float sunDistance = length(sunDelta);
    float sunScreenVisibility = saturate(AC_LightScreenPos.z) * saturate(AC_SunVisibility);
    float sunCoreMask = (1.0f - smoothstep(0.006f, 0.020f, sunDistance)) * sunScreenVisibility;
    float sunBloomMask = (1.0f - smoothstep(0.020f, 0.070f, sunDistance)) * sunScreenVisibility;
    float sunHaloMask = (1.0f - smoothstep(0.070f, 0.160f, sunDistance)) * sunScreenVisibility;
    float preservedSunTransmission = max(
        sunCoreMask * 0.30f,
        max(sunBloomMask * 0.18f, sunHaloMask * 0.10f));
    float effectiveCloudAlpha = min(cloudAlpha, 1.0f - preservedSunTransmission);
    scene.rgb = scene.rgb * (1.0f - effectiveCloudAlpha) + clouds.rgb;
    return scene;
}