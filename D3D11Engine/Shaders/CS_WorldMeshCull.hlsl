// Build-213/DX11 current-frame Hi-Z culling for opaque world-mesh clusters.
//
// The depth prepass has already written the world before this shader runs.
// Each cluster therefore owns one indirect draw argument. A cluster is only
// disabled when its complete projected AABB is conservatively hidden by the
// reversed-Z min Hi-Z pyramid. Unknown bounds and non-opaque materials are
// submitted with Enabled=0 and always remain visible.

struct WorldMeshCullCluster
{
    float3 BBoxMin;
    uint   ArgIndex;
    float3 BBoxMax;
    uint   Enabled;
};

cbuffer WorldMeshCullConstantBuffer : register( b0 )
{
    float4x4 CullViewProj;
    uint ClusterCount;
    uint HiZWidth;
    uint HiZHeight;
    uint HiZMipCount;
    uint EnableOcclusion;
    uint3 Padding;
};

StructuredBuffer<WorldMeshCullCluster> Clusters : register( t0 );
Texture2D<float> HiZDepth : register( t1 );
RWByteAddressBuffer IndirectArgs : register( u0 );

bool IsWorldMeshClusterVisible( WorldMeshCullCluster cluster )
{
    if ( cluster.Enabled == 0 || EnableOcclusion == 0 || HiZMipCount == 0 )
        return true;

    if ( any( cluster.BBoxMin > cluster.BBoxMax ) )
        return true;

    const float3 boxCenter = ( cluster.BBoxMin + cluster.BBoxMax ) * 0.5f;
    const float3 boxExtent = ( cluster.BBoxMax - cluster.BBoxMin ) * 0.5f;

    float2 uvMin = float2( 1e30f, 1e30f );
    float2 uvMax = -float2( 1e30f, 1e30f );
    float closestDepth = 0.0f;
    bool allInFront = true;

    // The exact eight-corner projection is intentionally used here. World
    // mesh bounds are authored in world space and can be elongated or cross
    // the camera frustum; a fast center/extent approximation would be more
    // likely to reject a marginally visible wall or vegetation cluster.
    [unroll]
    for ( uint cornerIndex = 0; cornerIndex < 8; ++cornerIndex )
    {
        const float3 corner = float3(
            ( cornerIndex & 1 ) != 0 ? boxCenter.x + boxExtent.x : boxCenter.x - boxExtent.x,
            ( cornerIndex & 2 ) != 0 ? boxCenter.y + boxExtent.y : boxCenter.y - boxExtent.y,
            ( cornerIndex & 4 ) != 0 ? boxCenter.z + boxExtent.z : boxCenter.z - boxExtent.z );
        const float4 clip = mul( float4( corner, 1.0f ), CullViewProj );

        if ( clip.w > 1.0e-4f )
        {
            const float3 ndc = clip.xyz / clip.w;
            uvMin = min( uvMin, ndc.xy * float2( 0.5f, -0.5f ) + 0.5f );
            uvMax = max( uvMax, ndc.xy * float2( 0.5f, -0.5f ) + 0.5f );
            // Reversed-Z: the greatest projected depth is the nearest point
            // of the cluster and is the value used by the conservative test.
            closestDepth = max( closestDepth, ndc.z );
        }
        else
        {
            // A box crossing the camera plane is never occlusion-rejected.
            allInFront = false;
        }
    }

    if ( !allInFront )
        return true;

    // A cluster wholly outside the horizontal/vertical screen is already
    // handled by the legacy section/frustum path. Keep this shader focused on
    // occlusion and avoid introducing a second frustum convention here.
    if ( uvMax.x < 0.0f || uvMin.x > 1.0f
        || uvMax.y < 0.0f || uvMin.y > 1.0f )
        return true;

    const float2 hizSize = float2( HiZWidth, HiZHeight );
    const float2 p0 = clamp( uvMin, 0.0f, 1.0f ) * hizSize;
    const float2 p1 = clamp( uvMax, 0.0f, 1.0f ) * hizSize;
    const float2 extent = max( p1 - p0, 1.0e-4f );
    const int mip = (int)clamp(
        ceil( log2( max( extent.x, extent.y ) ) ),
        0.0f, (float)( HiZMipCount - 1u ) );

    uint mipWidth;
    uint mipHeight;
    uint mipLevels;
    HiZDepth.GetDimensions( mip, mipWidth, mipHeight, mipLevels );
    const float mipScale = exp2( (float)mip );
    const int2 mipSize = max( int2( mipWidth, mipHeight ), int2( 1, 1 ) );
    const int2 t0 = clamp( int2( p0 / mipScale ), int2( 0, 0 ), mipSize - 1 );
    const int2 t1 = clamp( int2( p1 / mipScale ), int2( 0, 0 ), mipSize - 1 );

    // At the selected mip the projected rectangle covers at most two texels
    // per axis. Sampling all four corners makes the min-depth test
    // conservative: a sky/open texel prevents rejection.
    float occluderDepth = HiZDepth.Load( int3( t0, mip ) );
    occluderDepth = min( occluderDepth, HiZDepth.Load( int3( t1.x, t0.y, mip ) ) );
    occluderDepth = min( occluderDepth, HiZDepth.Load( int3( t0.x, t1.y, mip ) ) );
    occluderDepth = min( occluderDepth, HiZDepth.Load( int3( t1, mip ) ) );

    // Reversed-Z and a min-reduced pyramid: reject only when every sampled
    // footprint remains in front of the cluster's nearest point.
    return !( occluderDepth > closestDepth );
}

[numthreads( 64, 1, 1 )]
void CSCullWorldMesh( uint3 dispatchId : SV_DispatchThreadID )
{
    const uint clusterIndex = dispatchId.x;
    if ( clusterIndex >= ClusterCount )
        return;

    const WorldMeshCullCluster cluster = Clusters[clusterIndex];
    if ( IsWorldMeshClusterVisible( cluster ) )
        return;

    // D3D11_DRAW_INDEXED_INSTANCED_INDIRECT_ARGS:
    // IndexCount, InstanceCount, StartIndex, BaseVertex, StartInstance.
    IndirectArgs.Store( cluster.ArgIndex * 20u + 4u, 0u );
}
