// XeGTAO integration for GD3D11. The algorithm itself is the unmodified MIT
// licensed Intel/GameTechDev implementation in XeGTAO/XeGTAO.h(lsli).
#include "DS_Defines.h"

#define VA_SATURATE( x ) saturate( x )
#define XE_GTAO_USE_HALF_FLOAT_PRECISION 1
#define XE_GTAO_HILBERT_LUT_AVAILABLE

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
Texture2D<uint>      g_srcWorkingAOTerm    : register( t0 );
Texture2D<lpfloat>   g_srcWorkingEdges     : register( t1 );
RWTexture2D<lpfloat> g_outWorkingDepthMIP0 : register( u0 );
RWTexture2D<lpfloat> g_outWorkingDepthMIP1 : register( u1 );
RWTexture2D<lpfloat> g_outWorkingDepthMIP2 : register( u2 );
RWTexture2D<lpfloat> g_outWorkingDepthMIP3 : register( u3 );
RWTexture2D<lpfloat> g_outWorkingDepthMIP4 : register( u4 );
RWTexture2D<uint>    g_outWorkingAOTerm    : register( u0 );
RWTexture2D<unorm float> g_outWorkingEdges : register( u1 );
RWTexture2D<uint>    g_outFinalAOTerm      : register( u0 );

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

[numthreads( 8, 8, 1 )]
void CSPrefilterDepths16x16( uint2 dispatchThreadID : SV_DispatchThreadID, uint2 groupThreadID : SV_GroupThreadID )
{
    XeGTAO_PrefilterDepths16x16( dispatchThreadID, groupThreadID, g_GTAOConsts,
        g_srcRawDepth, g_samplerPointClamp,
        g_outWorkingDepthMIP0, g_outWorkingDepthMIP1, g_outWorkingDepthMIP2,
        g_outWorkingDepthMIP3, g_outWorkingDepthMIP4 );
}

[numthreads( XE_GTAO_NUMTHREADS_X, XE_GTAO_NUMTHREADS_Y, 1 )]
void CSGTAOLow( uint2 pixCoord : SV_DispatchThreadID )
{
    XeGTAO_MainPass( pixCoord, 1, 2, SpatioTemporalNoise( pixCoord, g_GTAOConsts.NoiseIndex ), LoadNormal( pixCoord ),
        g_GTAOConsts, g_srcWorkingDepth, g_samplerPointClamp, g_outWorkingAOTerm, g_outWorkingEdges );
}

[numthreads( XE_GTAO_NUMTHREADS_X, XE_GTAO_NUMTHREADS_Y, 1 )]
void CSGTAOMedium( uint2 pixCoord : SV_DispatchThreadID )
{
    XeGTAO_MainPass( pixCoord, 2, 2, SpatioTemporalNoise( pixCoord, g_GTAOConsts.NoiseIndex ), LoadNormal( pixCoord ),
        g_GTAOConsts, g_srcWorkingDepth, g_samplerPointClamp, g_outWorkingAOTerm, g_outWorkingEdges );
}

[numthreads( XE_GTAO_NUMTHREADS_X, XE_GTAO_NUMTHREADS_Y, 1 )]
void CSGTAOHigh( uint2 pixCoord : SV_DispatchThreadID )
{
    XeGTAO_MainPass( pixCoord, 3, 3, SpatioTemporalNoise( pixCoord, g_GTAOConsts.NoiseIndex ), LoadNormal( pixCoord ),
        g_GTAOConsts, g_srcWorkingDepth, g_samplerPointClamp, g_outWorkingAOTerm, g_outWorkingEdges );
}

[numthreads( XE_GTAO_NUMTHREADS_X, XE_GTAO_NUMTHREADS_Y, 1 )]
void CSGTAOUltra( uint2 pixCoord : SV_DispatchThreadID )
{
    XeGTAO_MainPass( pixCoord, 9, 3, SpatioTemporalNoise( pixCoord, g_GTAOConsts.NoiseIndex ), LoadNormal( pixCoord ),
        g_GTAOConsts, g_srcWorkingDepth, g_samplerPointClamp, g_outWorkingAOTerm, g_outWorkingEdges );
}

[numthreads( XE_GTAO_NUMTHREADS_X, XE_GTAO_NUMTHREADS_Y, 1 )]
void CSDenoisePass( uint2 dispatchThreadID : SV_DispatchThreadID )
{
    XeGTAO_Denoise( dispatchThreadID * uint2( 2, 1 ), g_GTAOConsts, g_srcWorkingAOTerm, g_srcWorkingEdges,
        g_samplerPointClamp, g_outFinalAOTerm, false );
}

[numthreads( XE_GTAO_NUMTHREADS_X, XE_GTAO_NUMTHREADS_Y, 1 )]
void CSDenoiseLastPass( uint2 dispatchThreadID : SV_DispatchThreadID )
{
    XeGTAO_Denoise( dispatchThreadID * uint2( 2, 1 ), g_GTAOConsts, g_srcWorkingAOTerm, g_srcWorkingEdges,
        g_samplerPointClamp, g_outFinalAOTerm, true );
}
