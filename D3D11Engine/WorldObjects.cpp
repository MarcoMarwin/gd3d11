#include "pch.h"
#include "WorldObjects.h"
#include <limits>
#include <memory>
#include <new>
#include "GothicAPI.h"
#include "Engine.h"
#include "BaseGraphicsEngine.h"
#include "zCVob.h"
#include "zCVobLight.h"
#include "zCMaterial.h"
#include "zCTexture.h"
#include "D3D11_Helpers.h"

XMVECTOR VobLightInfo::GetEffectivePositionWorldXM() const {
    if ( !Vob )
        return XMVectorZero();

    XMVECTOR position = Vob->GetPositionWorldXM();
    if ( HasFlameAnchor )
        position += XMLoadFloat3( &FlameAnchorOffset );
    return position;
}

float3 VobLightInfo::GetEffectivePositionWorld() const {
    XMFLOAT3 position;
    XMStoreFloat3( &position, GetEffectivePositionWorldXM() );
    return float3( position );
}


/** Updates the vobs constantbuffer */
void VobInfo::UpdateVobConstantBuffer(VS_ExConstantBuffer_PerInstance& cb) {
    UpdateState();
    cb.World = WorldMatrix;
    cb.Color = {0.0f, 0.0f, 0.0f, 1.0f};
}

bool VobInfo::ComputeIndoorLightMask() const {
    return Vob && Vob->IsIndoorVob();
}

void VobInfo::UpdateState() {
    if ( !Vob ) {
        WorldMatrix = g_MatIdentity;
        LastRenderPosition = {};
        IsIndoorVob = false;
        IndoorLightMask = false;
        HasIndoorLightMaskSample = false;
        GroundColor = 0xFFFFFFFF;
        return;
    }

    const XMFLOAT4X4* const worldMatrix = Vob->GetWorldMatrixPtr();
    WorldMatrix = worldMatrix ? *worldMatrix : g_MatIdentity;
    LastRenderPosition = Vob->GetPositionWorld();

    const bool currentIndoorVob = Vob->IsIndoorVob();
    if ( !HasIndoorLightMaskSample || currentIndoorVob != IsIndoorVob ) {
        IsIndoorVob = currentIndoorVob;
        IndoorLightMask = ComputeIndoorLightMask();
        HasIndoorLightMaskSample = true;
    } else {
        IsIndoorVob = currentIndoorVob;
    }

    if ( IndoorLightMask ) {
        GroundColor = DEFAULT_LIGHTMAP_POLY_COLOR;
        return;
    }

    GroundColor = 0xFFFFFFFF;
    zCPolygon* const groundPolygon = Vob->GetGroundPoly();
    if ( !groundPolygon || groundPolygon->GetNumPolyVertices() <= 0 ) {
        return;
    }
    zCVertFeature** const features = groundPolygon->getFeatures();
    if ( features && features[0] ) {
        GroundColor = features[0]->lightStatic;
    }
}

/** Updates the vobs constantbuffer */
void SkeletalVobInfo::UpdateVobConstantBuffer(VS_ExConstantBuffer_PerInstance& cb) {
    UpdateState();
    cb.World = WorldMatrix;
    cb.Color = {0.0f, 0.0f, 0.0f, 1.0f};
}

void SkeletalVobInfo::UpdateState() {
    if ( !Vob ) {
        WorldMatrix = g_MatIdentity;
        return;
    }
    const XMFLOAT4X4* const worldMatrix = Vob->GetWorldMatrixPtr();
    WorldMatrix = worldMatrix ? *worldMatrix : g_MatIdentity;
}

SectionInstanceCache::~SectionInstanceCache() {
    InstanceCache.clear();
}

MeshInfo::~MeshInfo() {

    delete MeshVertexBuffer;
    delete MeshIndexBuffer;
    delete MeshShadowIndexBuffer;
}

SkeletalMeshInfo::~SkeletalMeshInfo() {
    delete MeshVertexBuffer;
    delete MeshIndexBuffer;
}

/** Clears the cache for the given progmesh */
void SectionInstanceCache::ClearCacheForStatic( MeshVisualInfo* pm ) {
    if ( InstanceCache.find( pm ) != InstanceCache.end() ) {
        InstanceCache[pm].reset();
        InstanceCacheData[pm].clear();
    }
}

/** Creates buffers for this mesh info */
XRESULT MeshInfo::Create( ExVertexStruct* vertices, unsigned int numVertices, VERTEX_INDEX* indices, unsigned int numIndices ) {
    if ( !vertices || !indices || numVertices == 0 || numIndices == 0
        || numVertices > std::numeric_limits<UINT>::max() / sizeof( ExVertexStruct )
        || numIndices > std::numeric_limits<UINT>::max() / sizeof( VERTEX_INDEX )
        || !Engine::GraphicsEngine ) {
        return XR_INVALID_ARG;
    }

    std::vector<ExVertexStruct> newVertices;
    std::vector<VERTEX_INDEX> newIndices;
    try {
        newVertices.assign( vertices, vertices + numVertices );
        newIndices.assign( indices, indices + numIndices );
    } catch ( const std::bad_alloc& ) {
        return XR_FAILED;
    }

    auto newVertexBuffer = std::make_unique<D3D11VertexBuffer>();
    auto newIndexBuffer = std::make_unique<D3D11VertexBuffer>();
    if ( newVertexBuffer->Init(
            vertices, numVertices * sizeof( ExVertexStruct ) ) != XR_SUCCESS
        || newIndexBuffer->Init(
            indices, numIndices * sizeof( VERTEX_INDEX ),
            D3D11VertexBuffer::B_INDEXBUFFER ) != XR_SUCCESS ) {
        return XR_FAILED;
    }

    SAFE_DELETE( MeshVertexBuffer );
    SAFE_DELETE( MeshIndexBuffer );
    MeshVertexBuffer = newVertexBuffer.release();
    MeshIndexBuffer = newIndexBuffer.release();
    Vertices = std::move( newVertices );
    Indices = std::move( newIndices );


    return XR_SUCCESS;
}
