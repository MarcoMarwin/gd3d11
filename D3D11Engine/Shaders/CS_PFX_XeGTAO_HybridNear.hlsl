// Build-213 XeGTAO A/B path: full-resolution near-field evaluation only.
// Distant pixels are deliberately written as neutral AO and are supplied by
// CS_PFX_XeGTAO_HybridFar.hlsl at half resolution.
#include "DS_Defines.h"

#define VA_SATURATE( x ) saturate( x )
#define XE_GTAO_USE_HALF_FLOAT_PRECISION 1
#define XE_GTAO_HILBERT_LUT_AVAILABLE
#define XE_GTAO_HYBRID_NEAR_ONLY
#define XE_GTAO_HYBRID_FAR_RADIUS_PIXELS 16.0

#include "XeGTAO/XeGTAO.h"

cbuffer GTAOConstantBuffer : register( b0 )
{
    GTAOConstants g_GTAOConsts;
}

#include "XeGTAO/XeGTAO.hlsli"

SamplerState g_samplerPointClamp : register( s0 );
Texture2D<float>     g_srcRawDepth         : register( t0 );
Texture2D<lpfloat>   g_srcWorkingDepth     : register( t0 );
Texture2D<float2>    g_srcNormalmap        : register( t1 );
Texture2D<uint>      g_srcHilbertLUT       : register( t5 );
RWTexture2D<uint>    g_outWorkingAOTerm    : register( u0 );
RWTexture2D<unorm float> g_outWorkingEdges : register( u1 );

lpfloat3 LoadNormal( int2 pos )
{
    return (lpfloat3)DecodeNormalGBuffer( g_srcNormalmap.Load( int3( pos, 0 ) ).xy );
}

lpfloat2 SpatioTemporalNoise( uint2 pixCoord, uint temporalIndex )
{
    uint index = g_srcHilbertLUT.Load( uint3( pixCoord % 64, 0 ) ).x;
    index += 288 * ( temporalIndex % 64 );
    return lpfloat2( frac( 0.5 + index * float2( 0.75487766624669276005, 0.56984029099805326591 ) ) );
}

[numthreads( XE_GTAO_NUMTHREADS_X, XE_GTAO_NUMTHREADS_Y, 1 )]
void CSGTAOHybridNearLow( uint2 pixCoord : SV_DispatchThreadID )
{
    XeGTAO_MainPass( pixCoord, 1, 2, SpatioTemporalNoise( pixCoord, g_GTAOConsts.NoiseIndex ), LoadNormal( pixCoord ),
        g_GTAOConsts, g_srcWorkingDepth, g_samplerPointClamp, g_outWorkingAOTerm, g_outWorkingEdges );
}

[numthreads( XE_GTAO_NUMTHREADS_X, XE_GTAO_NUMTHREADS_Y, 1 )]
void CSGTAOHybridNearMedium( uint2 pixCoord : SV_DispatchThreadID )
{
    XeGTAO_MainPass( pixCoord, 2, 2, SpatioTemporalNoise( pixCoord, g_GTAOConsts.NoiseIndex ), LoadNormal( pixCoord ),
        g_GTAOConsts, g_srcWorkingDepth, g_samplerPointClamp, g_outWorkingAOTerm, g_outWorkingEdges );
}

[numthreads( XE_GTAO_NUMTHREADS_X, XE_GTAO_NUMTHREADS_Y, 1 )]
void CSGTAOHybridNearHigh( uint2 pixCoord : SV_DispatchThreadID )
{
    XeGTAO_MainPass( pixCoord, 3, 3, SpatioTemporalNoise( pixCoord, g_GTAOConsts.NoiseIndex ), LoadNormal( pixCoord ),
        g_GTAOConsts, g_srcWorkingDepth, g_samplerPointClamp, g_outWorkingAOTerm, g_outWorkingEdges );
}

[numthreads( XE_GTAO_NUMTHREADS_X, XE_GTAO_NUMTHREADS_Y, 1 )]
void CSGTAOHybridNearUltra( uint2 pixCoord : SV_DispatchThreadID )
{
    XeGTAO_MainPass( pixCoord, 9, 3, SpatioTemporalNoise( pixCoord, g_GTAOConsts.NoiseIndex ), LoadNormal( pixCoord ),
        g_GTAOConsts, g_srcWorkingDepth, g_samplerPointClamp, g_outWorkingAOTerm, g_outWorkingEdges );
}
