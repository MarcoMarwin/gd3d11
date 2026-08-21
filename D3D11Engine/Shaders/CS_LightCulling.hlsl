// Clustered Forward+ light culling for D3D11.
#ifndef TILE_SIZE
#define TILE_SIZE 16u
#endif
#define MAX_ACTIVE_LIGHTS 1024u
#define MASK_WORDS (MAX_ACTIVE_LIGHTS / 32u)
#define NUM_Z_SLICES 16u
#define CULL_GROUP_SIZE 64u

// Keep this layout in sync with TiledPointLight in C++ and the lighting shaders.
struct TiledPointLight {
    float3 PositionView;
    float Range;
    float4 Color;
    float3 PositionWorld;
    int ShadowCubeIndex;
    float ShadowStrength;
    float IsIndoor;
    float IgnoreIndoorOutdoorLimit;
    float ShadowSoftness;
    uint ShadowFilterMode;
    uint3 ShadowFilterPad;
};

struct LightGrid {
    uint WordOccupancy;
    uint Mask[MASK_WORDS];
};

StructuredBuffer<TiledPointLight> SB_Lights : register( t1 );
RWStructuredBuffer<LightGrid> RW_LightGrid : register( u0 );

cbuffer LightCullingConstantBuffer : register( b0 ) {
    float2 ProjScale;    // (Proj._11, Proj._22): view -> clip x/y scale
    uint2 ScreenDimensions;
    uint TotalLights;
    uint NumTilesX;
    float NearZ;
    float FarZ;
};

groupshared uint gs_Mask[NUM_Z_SLICES * MASK_WORDS];
groupshared uint gs_Occ[NUM_Z_SLICES];

uint SliceOfViewZ( float zView, float invLogRatio ) {
    float t = log2( max( zView, NearZ ) / NearZ ) * invLogRatio;
    return (uint)clamp( floor( t * (float)NUM_Z_SLICES ), 0.0f, (float)( NUM_Z_SLICES - 1 ) );
}

[numthreads( CULL_GROUP_SIZE, 1, 1 )]
void CSMain( uint3 groupID : SV_GroupID, uint ti : SV_GroupIndex ) {
    [unroll]
    for ( uint c = 0; c < ( NUM_Z_SLICES * MASK_WORDS ) / CULL_GROUP_SIZE; ++c )
        gs_Mask[c * CULL_GROUP_SIZE + ti] = 0;
    if ( ti < NUM_Z_SLICES )
        gs_Occ[ti] = 0;

    const float2 tileMin = float2( groupID.xy ) * TILE_SIZE;
    const float2 tileMax = float2( groupID.xy + uint2( 1, 1 ) ) * TILE_SIZE;
    const float tx0 = ( tileMin.x / (float)ScreenDimensions.x * 2.0f - 1.0f ) / ProjScale.x;
    const float tx1 = ( tileMax.x / (float)ScreenDimensions.x * 2.0f - 1.0f ) / ProjScale.x;
    // NDC Y is flipped relative to pixel Y, so tileMin.y is the larger view-space Y edge.
    const float ty1 = -( tileMin.y / (float)ScreenDimensions.y * 2.0f - 1.0f ) / ProjScale.y;
    const float ty0 = -( tileMax.y / (float)ScreenDimensions.y * 2.0f - 1.0f ) / ProjScale.y;

    const float3 pL = float3( 1, 0, -tx0 ) * rsqrt( 1.0f + tx0 * tx0 );
    const float3 pR = float3( -1, 0, tx1 ) * rsqrt( 1.0f + tx1 * tx1 );
    const float3 pB = float3( 0, 1, -ty0 ) * rsqrt( 1.0f + ty0 * ty0 );
    const float3 pT = float3( 0, -1, ty1 ) * rsqrt( 1.0f + ty1 * ty1 );

    const float invLogRatio = 1.0f / log2( FarZ / NearZ );
    const uint lightCount = min( TotalLights, MAX_ACTIVE_LIGHTS );

    GroupMemoryBarrierWithGroupSync();

    // Test each light against the tile frustum and mark the covered Z slices.
    for ( uint base = 0; base < lightCount; base += CULL_GROUP_SIZE ) {
        const uint lightIndex = base + ti;
        if ( lightIndex < lightCount ) {
            const TiledPointLight light = SB_Lights[lightIndex];
            const float3 center = light.PositionView;
            const float radius = light.Range * 1.05f;
            const float zLo = center.z - radius;
            const float zHi = center.z + radius;

            if ( zHi >= NearZ && zLo <= FarZ &&
                 dot( pL, center ) >= -radius && dot( pR, center ) >= -radius &&
                 dot( pB, center ) >= -radius && dot( pT, center ) >= -radius ) {
                const uint firstSlice = SliceOfViewZ( max( zLo, NearZ ), invLogRatio );
                const uint lastSlice = SliceOfViewZ( min( zHi, FarZ ), invLogRatio );
                const uint word = lightIndex >> 5u;
                const uint bit = 1u << ( lightIndex & 31u );

                for ( uint slice = firstSlice; slice <= lastSlice; ++slice )
                    InterlockedOr( gs_Mask[slice * MASK_WORDS + word], bit );
            }
        }
    }

    GroupMemoryBarrierWithGroupSync();

    [unroll]
    for ( uint u = 0; u < ( NUM_Z_SLICES * MASK_WORDS ) / CULL_GROUP_SIZE; ++u ) {
        const uint i = u * CULL_GROUP_SIZE + ti;
        if ( gs_Mask[i] != 0 )
            InterlockedOr( gs_Occ[i >> 5u], 1u << ( i & 31u ) );
    }

    GroupMemoryBarrierWithGroupSync();

    const uint clusterBase = ( groupID.y * NumTilesX + groupID.x ) * NUM_Z_SLICES;
    [unroll]
    for ( uint o = 0; o < ( NUM_Z_SLICES * MASK_WORDS ) / CULL_GROUP_SIZE; ++o ) {
        const uint i = o * CULL_GROUP_SIZE + ti;
        RW_LightGrid[clusterBase + ( i >> 5u )].Mask[i & 31u] = gs_Mask[i];
    }
    if ( ti < NUM_Z_SLICES )
        RW_LightGrid[clusterBase + ti].WordOccupancy = gs_Occ[ti];
}
