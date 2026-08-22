// Build-213/DX11 GPU-driven static-VOB culling.
//
// The CPU still performs BSP/node and distance collection. Individual VOB
// frustum tests are skipped for the main view, then this shader tests each
// instance, compacts survivors into the same per-visual range, and patches
// the indirect draw arguments. CSM shadow collection never uses this path.

struct VobCullVisual
{
    float3 BBoxMin;
    uint   InstanceBase;
    float3 BBoxMax;
    uint   InstanceCount;
};

// Mirrors VobInstanceInfo (148 bytes). The world matrices are the four rows
// written by XMStoreFloat4x4; BuildWorldMatrix restores the matrix consumed by
// the existing VS_ExInstancedObj row-vector mul() calls.
struct VobInstanceGpu
{
    float4 World0;
    float4 World1;
    float4 World2;
    float4 World3;
    float4 PrevWorld0;
    float4 PrevWorld1;
    float4 PrevWorld2;
    float4 PrevWorld3;
    uint   Color;
    float  WindStrength;
    float  CanBeAffectedByPlayer;
    uint   GPSlot;
    uint   EmissiveColor;
};

cbuffer VobCullConstantBuffer : register( b0 )
{
    float4x4 CullViewProj;
    uint     VisualCount;
    uint     HiZWidth;
    uint     HiZHeight;
    uint     HiZMipCount;
    uint     EnableOcclusion;
    uint3    CullPadding;
};

StructuredBuffer<VobInstanceGpu> InInstances : register( t0 );
StructuredBuffer<VobCullVisual>  Visuals     : register( t1 );
Texture2D<float>                  HiZDepth    : register( t2 );
RWByteAddressBuffer               OutInstances : register( u0 );
RWByteAddressBuffer               VisibleCounts : register( u1 );

float4x4 BuildWorldMatrix( VobInstanceGpu inst )
{
    return transpose( float4x4( inst.World0, inst.World1, inst.World2, inst.World3 ) );
}

void StoreInstance( uint byteOffset, VobInstanceGpu inst )
{
    OutInstances.Store4( byteOffset +   0, asuint( inst.World0 ) );
    OutInstances.Store4( byteOffset +  16, asuint( inst.World1 ) );
    OutInstances.Store4( byteOffset +  32, asuint( inst.World2 ) );
    OutInstances.Store4( byteOffset +  48, asuint( inst.World3 ) );
    OutInstances.Store4( byteOffset +  64, asuint( inst.PrevWorld0 ) );
    OutInstances.Store4( byteOffset +  80, asuint( inst.PrevWorld1 ) );
    OutInstances.Store4( byteOffset +  96, asuint( inst.PrevWorld2 ) );
    OutInstances.Store4( byteOffset + 112, asuint( inst.PrevWorld3 ) );
    OutInstances.Store(  byteOffset + 128, asuint( inst.Color ) );
    OutInstances.Store(  byteOffset + 132, asuint( inst.WindStrength ) );
    OutInstances.Store(  byteOffset + 136, asuint( inst.CanBeAffectedByPlayer ) );
    OutInstances.Store(  byteOffset + 140, asuint( inst.GPSlot ) );
    OutInstances.Store(  byteOffset + 144, asuint( inst.EmissiveColor ) );
}

bool IsInstanceVisible( VobCullVisual visual, VobInstanceGpu inst )
{
    if ( any( visual.BBoxMin > visual.BBoxMax ) )
        return true;

    float4x4 worldViewProj = mul( BuildWorldMatrix( inst ), CullViewProj );
    bool outNegX = true;
    bool outPosX = true;
    bool outNegY = true;
    bool outPosY = true;
    bool outNear = true;
    float2 uvMin = float2( 1e30f, 1e30f );
    float2 uvMax = -float2( 1e30f, 1e30f );
    float closestDepth = 0.0f;
    bool allInFront = true;

    [unroll]
    for ( uint cornerIndex = 0; cornerIndex < 8; ++cornerIndex )
    {
        float3 corner = float3(
            (cornerIndex & 1) != 0 ? visual.BBoxMax.x : visual.BBoxMin.x,
            (cornerIndex & 2) != 0 ? visual.BBoxMax.y : visual.BBoxMin.y,
            (cornerIndex & 4) != 0 ? visual.BBoxMax.z : visual.BBoxMin.z );
        float4 clip = mul( float4( corner, 1.0f ), worldViewProj );

        outNegX = outNegX && (clip.x < -clip.w);
        outPosX = outPosX && (clip.x >  clip.w);
        outNegY = outNegY && (clip.y < -clip.w);
        outPosY = outPosY && (clip.y >  clip.w);
        // Gothic's renderer uses a D3D reversed-Z projection with an
        // infinite far plane. Only the near plane needs a reject here.
        outNear = outNear && (clip.z > clip.w);

        if ( clip.w > 1e-4f )
        {
            float3 ndc = clip.xyz / clip.w;
            uvMin = min( uvMin, ndc.xy * float2( 0.5f, -0.5f ) + 0.5f );
            uvMax = max( uvMax, ndc.xy * float2( 0.5f, -0.5f ) + 0.5f );
            closestDepth = max( closestDepth, ndc.z );
        }
        else
        {
            allInFront = false;
        }
    }

    if ( outNegX || outPosX || outNegY || outPosY || outNear )
        return false;

    // A box straddling the eye plane has an unbounded projected rectangle;
    // keep it rather than risk an invalid occlusion rejection.
    if ( EnableOcclusion == 0 || HiZMipCount == 0 || !allInFront )
        return true;

    float2 hizSize = float2( HiZWidth, HiZHeight );
    float2 p0 = clamp( uvMin, 0.0f, 1.0f ) * hizSize;
    float2 p1 = clamp( uvMax, 0.0f, 1.0f ) * hizSize;
    float2 extent = max( p1 - p0, 1e-4f );
    int mip = (int)clamp(
        ceil( log2( max( extent.x, extent.y ) ) ),
        0.0f, (float)( HiZMipCount - 1 ) );

    uint mipWidth;
    uint mipHeight;
    uint mipLevels;
    HiZDepth.GetDimensions( mip, mipWidth, mipHeight, mipLevels );
    float mipScale = exp2( (float)mip );
    int2 mipSize = max( int2( mipWidth, mipHeight ), int2( 1, 1 ) );
    int2 t0 = clamp( int2( p0 / mipScale ), int2( 0, 0 ), mipSize - 1 );
    int2 t1 = clamp( int2( p1 / mipScale ), int2( 0, 0 ), mipSize - 1 );

    float occluderDepth = HiZDepth.Load( int3( t0, mip ) );
    occluderDepth = min( occluderDepth, HiZDepth.Load( int3( t1.x, t0.y, mip ) ) );
    occluderDepth = min( occluderDepth, HiZDepth.Load( int3( t0.x, t1.y, mip ) ) );
    occluderDepth = min( occluderDepth, HiZDepth.Load( int3( t1, mip ) ) );

    // Reversed-Z: greater depth is closer. The min-reduced pyramid stores
    // the farthest world pixel in the tested footprint, so rejecting only
    // when it is still closer than the VOB is conservative.
    return !( occluderDepth > closestDepth );
}

groupshared uint VisibleInGroup;

[numthreads(64, 1, 1)]
void CSCull( uint3 groupId : SV_GroupID, uint groupIndex : SV_GroupIndex )
{
    const uint visualIndex = groupId.x;

    if ( groupIndex == 0 )
        VisibleInGroup = 0;
    GroupMemoryBarrierWithGroupSync();

    if ( visualIndex < VisualCount )
    {
        VobCullVisual visual = Visuals[visualIndex];
        for ( uint instanceIndex = groupIndex;
              instanceIndex < visual.InstanceCount;
              instanceIndex += 64 )
        {
            VobInstanceGpu instance = InInstances[visual.InstanceBase + instanceIndex];
            if ( IsInstanceVisible( visual, instance ) )
            {
                uint compactedIndex;
                InterlockedAdd( VisibleInGroup, 1, compactedIndex );
                const uint outputByteOffset =
                    (visual.InstanceBase + compactedIndex) * 148;
                StoreInstance( outputByteOffset, instance );
            }
        }
    }

    GroupMemoryBarrierWithGroupSync();
    if ( groupIndex == 0 && visualIndex < VisualCount )
        VisibleCounts.Store( visualIndex * 4, VisibleInGroup );
}

cbuffer VobPatchConstantBuffer : register( b1 )
{
    uint DrawCount;
    uint3 PatchPadding;
};

ByteAddressBuffer       PatchVisibleCounts : register( t0 );
StructuredBuffer<uint>  DrawVisualIndices  : register( t1 );
StructuredBuffer<VobCullVisual> PatchVisuals : register( t2 );
RWByteAddressBuffer     PatchArgs          : register( u0 );

[numthreads(64, 1, 1)]
void CSPatchArgs( uint3 dispatchThreadId : SV_DispatchThreadID )
{
    const uint drawIndex = dispatchThreadId.x;
    if ( drawIndex >= DrawCount )
        return;

    const uint visualIndex = DrawVisualIndices[drawIndex];
    const uint instanceCount = PatchVisibleCounts.Load( visualIndex * 4 );
    const uint instanceBase = PatchVisuals[visualIndex].InstanceBase;
    const uint argByteOffset = drawIndex * 20;

    // D3D11_DRAW_INDEXED_INSTANCED_INDIRECT_ARGS:
    // IndexCount, InstanceCount, StartIndex, BaseVertex, StartInstance.
    PatchArgs.Store( argByteOffset + 4,  instanceCount );
    PatchArgs.Store( argByteOffset + 16, instanceBase );
}
