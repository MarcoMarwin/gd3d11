//--------------------------------------------------------------------------------------
// Depth of Field Composite Pass
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
Texture2D TX_Scene : register( t0 );   // Full-res sharp scene
Texture2D TX_Blur  : register( t1 );   // Half-res bokeh (rgb=blur, a=CoC)
Texture2D TX_Depth : register( t2 );   // Full-res hardware depth
Texture2D TX_Focus : register( t3 );   // 1x1 smoothed focus depth
Texture2D TX_WaterMask : register(t4);
Texture2D TX_SpecularMask : register(t5);
RWTexture2D<float4> OutputComposite : register(u0);



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

bool IsSkyDepth( float d )
{
    return d <= 1e-7f;
}


float ComputeNearCoCFromLinearDepth( float linearDepth )
{
    const float nearRange = max( DoF_NearBlurDistance - DoF_NearPlane, 1.0f );
    const float nearDepth = max( linearDepth, DoF_NearPlane );
    return saturate( ( DoF_NearBlurDistance - nearDepth ) / nearRange ) * DoF_NearBlurStrength;
}

float ComputeNearCoCFromDepth( float d, float2 texcoord )
{
    if ( IsSkyDepth( d ) )
        return 0.0f;

    return ComputeNearCoCFromLinearDepth( CameraDistanceFromDepth( d, texcoord ) );
}

float2 ComputeCoCsFromDepth( float d, float focusDepth, float2 texcoord )
{
    if ( IsSkyDepth( d ) )
        return 0.0f;

    const float linearDepth = CameraDistanceFromDepth( d, texcoord );
    const float farCoC = saturate( ( linearDepth - focusDepth ) / DoF_FocusRange );
    const float nearCoC = ComputeNearCoCFromLinearDepth( linearDepth )
        * ( 1.0f - ReflectionReceiverMask( texcoord ) );
    return float2( max( farCoC, nearCoC ), nearCoC );
}

static const int SKY_EDGE_SAMPLE_COUNT = 24;

float2 GetSkyEdgeSpiralSample( int index )
{
    float radius = sqrt( ( float( index ) + 0.5f ) / float( SKY_EDGE_SAMPLE_COUNT ) );
    float angle = float( index ) * 2.39996323f;
    return float2( cos( angle ), sin( angle ) ) * radius;
}

float4 GetSkyEdgeBlurSample( float2 texcoord, float2 dtexel, float focusDepth )
{
    float3 colorAccum = 0.0f;
    float coverageAccum = 0.0f;
    float kernelAccum = 0.0f;
    float edgeRadius = clamp( max( DoF_BokehRadius, DoF_MaxBlur ) * 0.22f, 2.0f, 10.0f );

    [unroll]
    for ( int i = 0; i < SKY_EDGE_SAMPLE_COUNT; ++i )
    {
        float2 offset = GetSkyEdgeSpiralSample( i );
        float2 sampleUV = texcoord + offset * edgeRadius * dtexel;
        float depth = TX_Depth.SampleLevel( SS_Linear, sampleUV, 0 ).r;
        float coc = IsSkyDepth( depth ) ? 0.0f : ComputeCoCsFromDepth( depth, focusDepth, sampleUV ).x;
        float4 blur = TX_Blur.SampleLevel( SS_Linear, sampleUV, 0 );
        float radialWeight = exp( -dot( offset, offset ) * 2.4f );
        float geometryWeight = IsSkyDepth( depth ) ? 0.0f : smoothstep( 0.12f, 0.65f, coc );
        float sampleWeight = radialWeight * geometryWeight;

        colorAccum += blur.rgb * sampleWeight;
        coverageAccum += sampleWeight;
        kernelAccum += radialWeight;
    }

    float coverage = saturate( coverageAccum / max( kernelAccum, 0.001f ) * 2.2f );
    float blend = smoothstep( 0.02f, 0.85f, coverage );
    return float4( colorAccum / max( coverageAccum, 0.001f ), blend );
}

[numthreads(8, 8, 1)]
void CSMain( uint3 DTid : SV_DispatchThreadID )
{
    uint2 outSize;
    OutputComposite.GetDimensions( outSize.x, outSize.y );

    if ( DTid.x >= outSize.x || DTid.y >= outSize.y )
        return;

    float2 texcoord = ( float2( DTid.xy ) + 0.5 ) / float2( outSize );

    float3 sharpColor = TX_Scene.SampleLevel( SS_Linear, texcoord, 0 ).rgb;

    float focusDepth = TX_Focus.SampleLevel( SS_Linear, float2( 0.5, 0.5 ), 0 ).r;

    // Compute CoC at center and 4 neighbours. Normal/far edges still use erosion;
    // near foreground edges are handled separately so close objects blur across silhouettes.
    float2 depthSize;
    TX_Depth.GetDimensions( depthSize.x, depthSize.y );
    float2 dtexel = 1.0 / depthSize;
    float depthC = TX_Depth.SampleLevel( SS_Linear, texcoord, 0 ).r;
    float4 blurSample = TX_Blur.SampleLevel( SS_Linear, texcoord, 0 );
    if ( IsSkyDepth( depthC ) )
    {
        float4 skyEdgeBlur = GetSkyEdgeBlurSample( texcoord, dtexel, focusDepth );
        OutputComposite[DTid.xy] = float4( lerp( sharpColor, skyEdgeBlur.rgb, skyEdgeBlur.a ), 1.0f );
        return;
    }

    const float2 uvL = texcoord + float2( -dtexel.x, 0.0f );
    const float2 uvR = texcoord + float2(  dtexel.x, 0.0f );
    const float2 uvU = texcoord + float2( 0.0f, -dtexel.y );
    const float2 uvD = texcoord + float2( 0.0f,  dtexel.y );
    float depthL = TX_Depth.SampleLevel( SS_Linear, uvL, 0 ).r;
    float depthR = TX_Depth.SampleLevel( SS_Linear, uvR, 0 ).r;
    float depthU = TX_Depth.SampleLevel( SS_Linear, uvU, 0 ).r;
    float depthD = TX_Depth.SampleLevel( SS_Linear, uvD, 0 ).r;

    const float2 cocNearC = ComputeCoCsFromDepth( depthC, focusDepth, texcoord );
    const float2 cocNearL = IsSkyDepth( depthL ) ? float2( cocNearC.x, 0.0f ) : ComputeCoCsFromDepth( depthL, focusDepth, uvL );
    const float2 cocNearR = IsSkyDepth( depthR ) ? float2( cocNearC.x, 0.0f ) : ComputeCoCsFromDepth( depthR, focusDepth, uvR );
    const float2 cocNearU = IsSkyDepth( depthU ) ? float2( cocNearC.x, 0.0f ) : ComputeCoCsFromDepth( depthU, focusDepth, uvU );
    const float2 cocNearD = IsSkyDepth( depthD ) ? float2( cocNearC.x, 0.0f ) : ComputeCoCsFromDepth( depthD, focusDepth, uvD );

    const float cocC = cocNearC.x;
    const float cocL = cocNearL.x;
    const float cocR = cocNearR.x;
    const float cocU = cocNearU.x;
    const float cocD = cocNearD.x;
    const float nearC = cocNearC.y;
    const float nearL = cocNearL.y;
    const float nearR = cocNearR.y;
    const float nearU = cocNearU.y;
    const float nearD = cocNearD.y;
    float nearNeighbourCoC = max( max( nearC, nearL ), max( nearR, max( nearU, nearD ) ) );

    float2 inwardShift = float2(
        (IsSkyDepth(depthL) ? 1.0f : 0.0f) - (IsSkyDepth(depthR) ? 1.0f : 0.0f),
        (IsSkyDepth(depthU) ? 1.0f : 0.0f) - (IsSkyDepth(depthD) ? 1.0f : 0.0f));

    const float nearEdgeThreshold = 0.04f;
    inwardShift.x += (nearL > nearC + nearEdgeThreshold ? -1.0f : 0.0f)
        + (nearR > nearC + nearEdgeThreshold ? 1.0f : 0.0f);
    inwardShift.y += (nearU > nearC + nearEdgeThreshold ? -1.0f : 0.0f)
        + (nearD > nearC + nearEdgeThreshold ? 1.0f : 0.0f);

    float inwardLength = length(inwardShift);
    if ( inwardLength > 0.0f )
    {
        float shiftStrength = saturate((DoF_BokehRadius - 2.0f) / 8.0f);
        float2 inwardDirection = inwardShift / inwardLength;
        float2 shiftedBlurUV = texcoord + inwardDirection * dtexel * shiftStrength;
        float2 edgeTangent = float2(-inwardDirection.y, inwardDirection.x) * dtexel * 0.5f;
        blurSample = 0.5f * (TX_Blur.SampleLevel( SS_Linear, shiftedBlurUV - edgeTangent, 0 )
            + TX_Blur.SampleLevel( SS_Linear, shiftedBlurUV + edgeTangent, 0 ));
    }

    float minCoC = min( min( cocC, cocL ), min( cocR, min( cocU, cocD ) ) );
    // Keep the old erosion for normal/far DoF edges, but not for near foreground blur:
    // close NPCs and objects must soften across their silhouette instead of staying cut out.
    float nearForegroundWeight = smoothstep( 0.02f, 0.20f, nearNeighbourCoC );
    float compositeCoC = lerp( minCoC, max( cocC, blurSample.a ), nearForegroundWeight );
    // Bilinear-upsampled half-res bokeh blur. Match Build 096 background DoF strength.
    float blendFactor = smoothstep( 0.0f, 1.0f, compositeCoC );
    float3 finalColor = lerp( sharpColor, blurSample.rgb, blendFactor );
    OutputComposite[DTid.xy] = float4( finalColor, 1.0f );
}
