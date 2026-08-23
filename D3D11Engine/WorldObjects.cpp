#include "pch.h"
#include <cmath>
#include <algorithm>
#include "WorldObjects.h"
#include <cstdint>
#include "GothicAPI.h"
#include "Engine.h"
#include "BaseGraphicsEngine.h"
#include "zCVob.h"
#include "zCVobLight.h"
#include "zCParticleFX.h"
#include "zCMaterial.h"
#include "zCTexture.h"
#include "D3D11_Helpers.h"

namespace {
    std::uint32_t FlickerHash( std::uint32_t value ) {
        value ^= value >> 16;
        value *= 0x7feb352du;
        value ^= value >> 15;
        value *= 0x846ca68bu;
        value ^= value >> 16;
        return value;
    }

    float SampleFlickerNoise( float time, float frequency, float phase,
        std::uint32_t salt ) {
        const float coordinate = std::max( 0.0f, time ) * frequency + phase;
        const float cellPosition = std::floor( coordinate );
        const float cellFraction = coordinate - cellPosition;
        const float smoothFraction = cellFraction * cellFraction
            * ( 3.0f - 2.0f * cellFraction );
        const auto cell = static_cast<std::uint32_t>( cellPosition );
        const auto phaseSeed = static_cast<std::uint32_t>(
            std::max( 0.0f, phase ) * 1000.0f );
        const auto hashAt = [&]( std::uint32_t sampleCell ) {
            return static_cast<float>( FlickerHash(
                phaseSeed ^ (sampleCell * 0x9e3779b9u) ^ salt ) )
                / 4294967295.0f;
        };

        const float lower = hashAt( cell );
        const float upper = hashAt( cell + 1u );
        return (lower + (upper - lower) * smoothFraction) * 2.0f - 1.0f;
    }

    bool IsRendererLightSourceActive( zCVobLight* light ) {
        if ( !light || !light->IsEnabled() )
            return false;

        const float range = light->GetLightRange();
        return std::isfinite( range ) && range > 0.0f;
    }

    bool IsRendererFlameVisualActive( const RendererLightFlameVisual& visual ) {
        if ( !visual.Vob || !visual.Vob->GetShowVisual() )
            return false;

        if ( !visual.IsParticle )
            return true;

        zCParticleFX* particle = reinterpret_cast<zCParticleFX*>( visual.Vob->GetVisual() );
        return particle && particle->GetFirstParticle() != nullptr;
    }
}

XMVECTOR VobLightInfo::GetEffectivePositionWorldXM() const {
    if ( IsRendererLight ) {
        if ( RendererLightAnchorVob ) {
            return RendererLightAnchorVob->GetPositionWorldXM()
                + XMLoadFloat3( &RendererLightPositionOffset );
        }
        return XMLoadFloat3( &RendererLightPosition );
    }

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

DWORD VobLightInfo::GetEffectiveLightColor() const {
    if ( IsRendererLight )
        return RendererLightBaseColor;

    return Vob ? Vob->GetLightColor() : 0u;
}

float VobLightInfo::GetEffectiveLightIntensity() const {
    if ( !IsRendererLight )
        return 1.0f;

    float flicker = 1.0f;
    if ( RendererLightFlicker ) {
        const float time = Engine::GAPI ? Engine::GAPI->GetTimeSeconds() : 0.0f;
        // Blend slow, fast and shimmer noise.
        const float slow = SampleFlickerNoise(
            time, 1.45f, RendererLightFlickerPhase, 0xA341316Cu );
        const float fast = SampleFlickerNoise(
            time, 5.20f, RendererLightFlickerPhase * 1.73f, 0xC8013EA4u );
        const float shimmer = SampleFlickerNoise(
            time, 11.50f, RendererLightFlickerPhase * 2.41f, 0xAD90777Du );
        flicker += 0.100f * slow + 0.035f * fast + 0.012f * shimmer;
    }
    return std::max( 0.0f, RendererLightIntensity * flicker );
}

float VobLightInfo::GetEffectiveLightRange() const {
    if ( IsRendererLight )
        return std::max( RendererLightRange, 0.0f );

    return Vob ? std::max( Vob->GetLightRange(), 0.0f ) : 0.0f;
}

bool VobLightInfo::IsEffectivelyEnabled() const {
    if ( IsRendererLight ) {
        if ( IsRendererLightSuppressed || !RendererLightEnabled )
            return false;

        if ( RendererLightSourceA || RendererLightSourceB ) {
            const bool sourceAEnabled = IsRendererLightSourceActive( RendererLightSourceA );
            const bool sourceBEnabled = IsRendererLightSourceActive( RendererLightSourceB );
            if ( !sourceAEnabled && !sourceBEnabled )
                return false;
        } else if ( !RendererLightFlicker ) {
            // Keep a fixed replacement while a source light remains enabled.
            return false;
        }

        if ( RendererLightFollowsFlameState
            && !std::any_of( RendererLightFlameVisuals.begin(), RendererLightFlameVisuals.end(),
                []( const RendererLightFlameVisual& visual ) {
                    return IsRendererFlameVisualActive( visual );
                } ) ) {
            return false;
        }

        return !RendererLightAnchorVob || RendererLightAnchorVob->GetShowVisual();
    }

    return Vob && Vob->IsEnabled() && !IsRendererLightSuppressed;
}

bool VobLightInfo::IsEffectivelyStatic() const {
    if ( IsRendererLight )
        return RendererLightStatic;

    return Vob && Vob->IsStatic();
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
    WorldMatrix = *Vob->GetWorldMatrixPtr();
    LastRenderPosition = Vob->GetPositionWorld();
    LastRenderBBox = Vob->GetBBox();

    const bool currentIndoorVob = Vob->IsIndoorVob();
    if ( !HasIndoorLightMaskSample || currentIndoorVob != IsIndoorVob ) {
        IsIndoorVob = currentIndoorVob;
        IndoorLightMask = ComputeIndoorLightMask();
        HasIndoorLightMaskSample = true;
    } else {
        IsIndoorVob = currentIndoorVob;
    }

    // Colorize the vob according to the underlaying polygon
    if ( IndoorLightMask ) {
        // All lightmapped polys have this color, so just use it
        GroundColor = DEFAULT_LIGHTMAP_POLY_COLOR;
    } else {
        // Get the color of the first found feature of the ground poly
        GroundColor = Vob->GetGroundPoly() ? Vob->GetGroundPoly()->getFeatures()[0]->lightStatic : 0xFFFFFFFF;
    }
}

/** Updates the vobs constantbuffer */
void SkeletalVobInfo::UpdateVobConstantBuffer(VS_ExConstantBuffer_PerInstance& cb) {
    UpdateState();
    cb.World = WorldMatrix;
    cb.Color = {0.0f, 0.0f, 0.0f, 1.0f};
}

void SkeletalVobInfo::UpdateState() {
    WorldMatrix = *Vob->GetWorldMatrixPtr();
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
    if ( Engine::GAPI && !Engine::IsShuttingDown() ) {
        Engine::GAPI->GetRendererState().RendererInfo.SkeletalVerticesDataSize -= Indices.size() * sizeof( VERTEX_INDEX );
        Engine::GAPI->GetRendererState().RendererInfo.SkeletalVerticesDataSize -= Vertices.size() * sizeof( ExSkelVertexStruct );
    }

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

/** Saves this sections mesh to a file */
void WorldMeshSectionInfo::SaveSectionMeshToFile( const std::string& name ) {
    FILE* f;
    fopen_s( &f, name.c_str(), "wb" );

    if ( !f )
        return;
}

/** Creates buffers for this mesh info */
XRESULT MeshInfo::Create( ExVertexStruct* vertices, unsigned int numVertices, VERTEX_INDEX* indices, unsigned int numIndices ) {
    Vertices.resize( numVertices );
    memcpy( &Vertices[0], vertices, numVertices * sizeof( ExVertexStruct ) );

    Indices.resize( numIndices );
    memcpy( &Indices[0], indices, numIndices * sizeof( VERTEX_INDEX ) );

    // Create the buffers
    Engine::GraphicsEngine->CreateVertexBuffer( &MeshVertexBuffer );
    Engine::GraphicsEngine->CreateVertexBuffer( &MeshIndexBuffer );

    // Init and fill it
    MeshVertexBuffer->Init( vertices, numVertices * sizeof( ExVertexStruct ) );
    MeshIndexBuffer->Init( indices, numIndices * sizeof( VERTEX_INDEX ), D3D11VertexBuffer::B_INDEXBUFFER );

    Engine::GAPI->GetRendererState().RendererInfo.VOBVerticesDataSize += numVertices * sizeof( ExVertexStruct );
    Engine::GAPI->GetRendererState().RendererInfo.VOBVerticesDataSize += numIndices * sizeof( VERTEX_INDEX );

    return XR_SUCCESS;
}
