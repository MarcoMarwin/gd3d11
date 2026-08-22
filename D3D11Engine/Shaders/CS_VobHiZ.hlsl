// Build-213/DX11 world-only reversed-Z hierarchical depth.
// The pyramid stores the minimum depth in every footprint. With reversed-Z,
// zero is the infinitely-far sky value, so this is conservative for occlusion.

Texture2D<float> SourceDepth : register( t0 );
RWTexture2D<float> DestinationMip : register( u0 );

[numthreads( 8, 8, 1 )]
void CSCopyDepth( uint3 dispatchId : SV_DispatchThreadID )
{
    uint2 dstSize;
    DestinationMip.GetDimensions( dstSize.x, dstSize.y );
    if ( dispatchId.x >= dstSize.x || dispatchId.y >= dstSize.y )
        return;

    uint2 srcSize;
    SourceDepth.GetDimensions( srcSize.x, srcSize.y );
    int2 s0 = int2( dispatchId.xy ) * 2;
    int2 s1 = int2(
        min( s0.x + 1, (int)srcSize.x - 1 ),
        min( s0.y + 1, (int)srcSize.y - 1 ) );

    float depth = SourceDepth.Load( int3( s0, 0 ) );
    depth = min( depth, SourceDepth.Load( int3( s1.x, s0.y, 0 ) ) );
    depth = min( depth, SourceDepth.Load( int3( s0.x, s1.y, 0 ) ) );
    depth = min( depth, SourceDepth.Load( int3( s1, 0 ) ) );
    DestinationMip[dispatchId.xy] = depth;
}

[numthreads( 8, 8, 1 )]
void CSReduce( uint3 dispatchId : SV_DispatchThreadID )
{
    uint2 dstSize;
    DestinationMip.GetDimensions( dstSize.x, dstSize.y );
    if ( dispatchId.x >= dstSize.x || dispatchId.y >= dstSize.y )
        return;

    uint2 srcSize;
    SourceDepth.GetDimensions( srcSize.x, srcSize.y );
    uint2 s0 = dispatchId.xy * 2;
    uint2 s1 = uint2(
        min( s0.x + 1, srcSize.x - 1 ),
        min( s0.y + 1, srcSize.y - 1 ) );

    float depth = SourceDepth.Load( int3( s0, 0 ) );
    depth = min( depth, SourceDepth.Load( int3( s1.x, s0.y, 0 ) ) );
    depth = min( depth, SourceDepth.Load( int3( s0.x, s1.y, 0 ) ) );
    depth = min( depth, SourceDepth.Load( int3( s1, 0 ) ) );

    // D3D11 uses floor(srcSize / 2) for the next mip dimension. Fold an odd
    // parent edge into the final destination texel so no source row/column
    // disappears from the conservative min pyramid.
    if ( (srcSize.x & 1u) != 0u && dispatchId.x == dstSize.x - 1u )
    {
        const uint x = srcSize.x - 1u;
        depth = min( depth, SourceDepth.Load( int3( x, s0.y, 0 ) ) );
        depth = min( depth, SourceDepth.Load( int3( x, s1.y, 0 ) ) );
    }
    if ( (srcSize.y & 1u) != 0u && dispatchId.y == dstSize.y - 1u )
    {
        const uint y = srcSize.y - 1u;
        depth = min( depth, SourceDepth.Load( int3( s0.x, y, 0 ) ) );
        depth = min( depth, SourceDepth.Load( int3( s1.x, y, 0 ) ) );
    }

    DestinationMip[dispatchId.xy] = depth;
}
