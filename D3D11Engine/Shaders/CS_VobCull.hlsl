// Build-213/DX11 GPU-driven static-VOB culling.
//
// The CPU still performs BSP/node and distance collection. The compute path
// tests each VOB instance, compacts survivors into the same per-visual range,
// and patches the indirect draw arguments. Main-view culling additionally
// uses the world-depth Hi-Z pyramid; shadow cascades use the same conservative
// frustum test with occlusion disabled and their own cascade matrix.

struct VobCullVisual
{
    float3 BBoxMin;
    uint   InstanceBase;
    float3 BBoxMax;
    uint   InstanceCount;
};

// Mirrors VobInstanceInfo (148 bytes). The world matrices are copied through
// unchanged because CSCull only forwards surviving instance records.
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
    uint     TotalInstanceCount;
    uint     HiZWidth;
    uint     HiZHeight;
    uint     HiZMipCount;
    uint     EnableOcclusion;
    uint2    CullPadding;
};

StructuredBuffer<VobInstanceGpu> InInstances : register( t0 );
StructuredBuffer<VobCullVisual>  Visuals     : register( t1 );
Texture2D<float>                  HiZDepth    : register( t2 );
StructuredBuffer<uint>            InstanceVisualIndices : register( t3 );
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

    const float3 localBoxCenter = (visual.BBoxMin + visual.BBoxMax) * 0.5f;
    const float3 localBoxExtent = (visual.BBoxMax - visual.BBoxMin) * 0.5f;
    const bool needsDynamicBounds = abs( inst.WindStrength ) > 1.0e-4f
        || inst.CanBeAffectedByPlayer > 0.5f;
    float4x4 worldViewProj;
    float3 boxCenter;
    float3 boxExtent;

    if ( !needsDynamicBounds )
    {
        // Preserve the cheaper authored-box path for ordinary static VOBs.
        worldViewProj = mul( BuildWorldMatrix( inst ), CullViewProj );
        boxCenter = localBoxCenter;
        boxExtent = localBoxExtent;
    }
    else
    {
        // Build a world-space box in the same matrix convention as the vertex
        // shader. Wind is applied after this transform, while hero/NPC
        // interaction is applied before it; both are represented by
        // conservative expansions.
        const float4x4 instanceWorld = BuildWorldMatrix( inst );
        const float3 worldBoxCenter = mul(
            float4( localBoxCenter, 1.0f ), instanceWorld ).xyz;
        const float3 worldAxisX = mul(
            float4( localBoxExtent.x, 0.0f, 0.0f, 0.0f ), instanceWorld ).xyz;
        const float3 worldAxisY = mul(
            float4( 0.0f, localBoxExtent.y, 0.0f, 0.0f ), instanceWorld ).xyz;
        const float3 worldAxisZ = mul(
            float4( 0.0f, 0.0f, localBoxExtent.z, 0.0f ), instanceWorld ).xyz;
        const float3 transformedBoxExtent = abs( worldAxisX )
            + abs( worldAxisY ) + abs( worldAxisZ );

        float dynamicRadius = abs( inst.WindStrength ) * 32.0f;
        if ( inst.CanBeAffectedByPlayer > 0.5f )
        {
            // Hero interaction is clamped to 38 local units in
            // VS_ExInstancedObj. The Frobenius norm safely bounds the length
            // of any transformed local displacement, including non-uniformly
            // scaled VOBs.
            const float3 basisX = mul(
                float4( 1.0f, 0.0f, 0.0f, 0.0f ), instanceWorld ).xyz;
            const float3 basisY = mul(
                float4( 0.0f, 1.0f, 0.0f, 0.0f ), instanceWorld ).xyz;
            const float3 basisZ = mul(
                float4( 0.0f, 0.0f, 1.0f, 0.0f ), instanceWorld ).xyz;
            const float frobeniusNorm = sqrt(
                dot( basisX, basisX ) + dot( basisY, basisY )
                + dot( basisZ, basisZ ) );
            dynamicRadius += 38.0f * frobeniusNorm;
        }
        dynamicRadius += 0.05f;

        worldViewProj = CullViewProj;
        boxCenter = worldBoxCenter;
        boxExtent = transformedBoxExtent + dynamicRadius;
    }

    // The clip transform is linear over the local AABB. Testing the interval
    // of each clip plane is equivalent to the eight-corner reject, but avoids
    // eight matrix-vector products for the common frustum-only path. The
    // interval is conservative and therefore cannot cull a visible VOB.
    const float4 clipCenter = mul( float4( boxCenter, 1.0f ), worldViewProj );
    const float4 row0 = worldViewProj[0];
    const float4 row1 = worldViewProj[1];
    const float4 row2 = worldViewProj[2];

    const float leftCenter = clipCenter.x + clipCenter.w;
    const float leftRadius = dot( boxExtent, abs( float3(
        row0.x + row0.w, row1.x + row1.w, row2.x + row2.w ) ) );
    const float rightCenter = -clipCenter.x + clipCenter.w;
    const float rightRadius = dot( boxExtent, abs( float3(
        -row0.x + row0.w, -row1.x + row1.w, -row2.x + row2.w ) ) );
    const float bottomCenter = clipCenter.y + clipCenter.w;
    const float bottomRadius = dot( boxExtent, abs( float3(
        row0.y + row0.w, row1.y + row1.w, row2.y + row2.w ) ) );
    const float topCenter = -clipCenter.y + clipCenter.w;
    const float topRadius = dot( boxExtent, abs( float3(
        -row0.y + row0.w, -row1.y + row1.w, -row2.y + row2.w ) ) );
    const float nearCenter = clipCenter.z - clipCenter.w;
    const float nearRadius = dot( boxExtent, abs( float3(
        row0.z - row0.w, row1.z - row1.w, row2.z - row2.w ) ) );

    if ( leftCenter + leftRadius < 0.0f
        || rightCenter + rightRadius < 0.0f
        || bottomCenter + bottomRadius < 0.0f
        || topCenter + topRadius < 0.0f
        || nearCenter - nearRadius > 0.0f ) {
        return false;
    }

    if ( EnableOcclusion == 0 || HiZMipCount == 0 )
        return true;

    float2 uvMin = float2( 1e30f, 1e30f );
    float2 uvMax = -float2( 1e30f, 1e30f );
    float closestDepth = 0.0f;
    bool allInFront = true;

    // Hi-Z still needs the projected rectangle and closest depth. Keep the
    // exact corner projection for this less common, more expensive path.
    [unroll]
    for ( uint cornerIndex = 0; cornerIndex < 8; ++cornerIndex )
    {
        float3 corner = float3(
            (cornerIndex & 1) != 0 ? boxCenter.x + boxExtent.x : boxCenter.x - boxExtent.x,
            (cornerIndex & 2) != 0 ? boxCenter.y + boxExtent.y : boxCenter.y - boxExtent.y,
            (cornerIndex & 4) != 0 ? boxCenter.z + boxExtent.z : boxCenter.z - boxExtent.z );
        float4 clip = mul( float4( corner, 1.0f ), worldViewProj );

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

    if ( !allInFront )
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

[numthreads(64, 1, 1)]
void CSCull( uint3 dispatchId : SV_DispatchThreadID )
{
    const uint absoluteInstanceIndex = dispatchId.x;
    if ( absoluteInstanceIndex >= TotalInstanceCount )
        return;

    // The CPU packs this direct ownership index alongside the instance data.
    // This removes an O(log(visual count)) search from every culling thread;
    // gaps in the retained-capacity buffer carry the invalid sentinel.
    const uint visualIndex = InstanceVisualIndices[absoluteInstanceIndex];
    if ( visualIndex >= VisualCount )
        return;
    const VobCullVisual visual = Visuals[visualIndex];

    VobInstanceGpu instance = InInstances[absoluteInstanceIndex];
    if ( IsInstanceVisible( visual, instance ) )
    {
        uint compactedIndex;
        VisibleCounts.InterlockedAdd( visualIndex * 4, 1, compactedIndex );
        const uint outputByteOffset =
            (visual.InstanceBase + compactedIndex) * 148;
        StoreInstance( outputByteOffset, instance );
    }
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
