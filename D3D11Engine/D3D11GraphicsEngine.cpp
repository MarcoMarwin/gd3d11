#include "D3D11GraphicsEngine.h"
#include "D3D11DeferredRenderer.h"
#include "D3D11ShadowMap.h"

#include "AlignedAllocator.h"
#include "D3D11Effect.h"
#include "D3D11GShader.h"
#include "D3D11LineRenderer.h"
#include "D3D11PShader.h"
#include "D3D11PfxRenderer.h"
#include "D3D11PipelineStates.h"
#include "D3D11PointLight.h"
#include "D3D11ShaderManager.h"
#include "D3D11VShader.h"
#include "D3D11IndirectBuffer.h"
#include "D3D11ConstantBuffer.h"
#include "GMesh.h"
#include "GSky.h"
#include "RenderToTextureBuffer.h"
#include "zCParticleFX.h"
#include "zCDecal.h"
#include "zCMaterial.h"
#include "zCQuadMark.h"
#include "zCTexture.h"
#include "zCView.h"
#include "zCCamera.h"
#include "zCVobLight.h"
#include "oCNPC.h"
#include "oCGame.h"
#include "zCBspTree.h"
#include "zCMesh.h"
#include <DDSTextureLoader.h>
#include <ScreenGrab.h>
#include <wincodec.h>
#include <SpriteFont.h>
#include <SpriteBatch.h>
#include <locale>
#include <codecvt>
#include <cmath>
#include <chrono>
#include <functional>
#include <limits>
#include <wrl\client.h>
#include "D3D11_Helpers.h"

#include "D3D11DXVK.h"
#include "D3D11NVAPI.h"
#include "D3D11IGDEXT.h"
#include "D3D11AGS.h"

#include "SteamOverlay.h"
#include <dxgi1_6.h>

#include "D3D11PFX_FSR3.h"
#include "D3D11TemporalState.h"
#include "ImGuiShim.h"
#include "zCModel.h"
#include "zCMorphMesh.h"
#include "zCPolygon.h"
#include "zCOption.h"
#include "RenderGraph.h"
#include "D3D11Upscaling.h"
#include <meshoptimizer/src/meshoptimizer.h>

namespace wrl = Microsoft::WRL;

const XMFLOAT4 UNDERWATER_COLOR_MOD = XMFLOAT4( 0.5f, 0.7f, 1.0f, 1.0f );

static const GUID IID_IDXGIVkInteropAdapter = { 0x3A6D8F2C, 0xB0E8, 0x4AB4, { 0xB4, 0xDC, 0x4F, 0xD2, 0x48, 0x91, 0xBF, 0xA5 } };
static const GUID IID_IDXGIDeviceRenderDoc = { 0xa7aa6116, 0x9c8d, 0x4bba, { 0x90, 0x83, 0xb4, 0xd8, 0x16, 0xb7, 0x1b, 0x78 } };

constexpr float inv255f = (1.f / 255.f);
// Negative alpha values encode renderer draw classes.
// Keep ordinary VOBs above -0.5; water uses -0.25.
constexpr float WATER_VOB_MATERIAL_CLASS_MARKER = -0.25f;
constexpr float BUILD_213_CHARACTER_INTERACTION_RANGE = 50.0f;
float vobAnimation_WindStrength = 1.0f;

constexpr DXGI_FORMAT VERTEX_INDEX_DXGI_FORMAT = sizeof( VERTEX_INDEX ) == sizeof( unsigned short ) ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT;

bool NativeSupport16BitTextures = false;
bool FeatureLevel10Compatibility = false;
bool FeatureRTArrayIndexFromAnyShader = false;

VS_ExConstantBuffer_Wind g_windBuffer;

namespace {

struct GpuVobCullVisualInfo {
    XMFLOAT3 BBoxMin;
    UINT InstanceBase;
    XMFLOAT3 BBoxMax;
    UINT InstanceCount;
};
static_assert( sizeof( GpuVobCullVisualInfo ) == 32 );
static_assert( offsetof( GpuVobCullVisualInfo, InstanceBase ) == 12 );
static_assert( offsetof( GpuVobCullVisualInfo, InstanceCount ) == 28 );

struct GpuVobCullConstantsData {
    XMFLOAT4X4 CullViewProj;
    UINT VisualCount;
    UINT HiZWidth;
    UINT HiZHeight;
    UINT HiZMipCount;
    UINT EnableOcclusion;
    UINT Padding[3];
};
static_assert( sizeof( GpuVobCullConstantsData ) == 96 );

struct GpuVobPatchConstantsData {
    UINT DrawCount;
    UINT Padding[3];
};
static_assert( sizeof( GpuVobPatchConstantsData ) == 16 );

template <typename T>
uint64_t HashGpuVobPerformanceSetting( uint64_t hash, const T& value ) {
    const auto* bytes = reinterpret_cast<const unsigned char*>( &value );
    for ( size_t index = 0; index < sizeof( value ); ++index ) {
        hash ^= bytes[index];
        hash *= 1099511628211ull;
    }
    return hash;
}

}

struct SkyVelocityConstantBuffer {
    XMFLOAT4X4 InvViewProj;
    XMFLOAT4X4 PrevViewProj;
    XMFLOAT2 JitterOffset;
    XMFLOAT2 Padding;
};
static void UpdateCharacterInteractionPositions( VS_ExConstantBuffer_Wind& windBuff ) {
    windBuff.characterInteractionRange = BUILD_213_CHARACTER_INTERACTION_RANGE;
    for ( int i = 0; i < MAX_CHARACTER_INTERACTION_INFLUENCERS; ++i ) {
        windBuff.interactionPositions[i] = float4( 0, 0, 0, 0 );
    }

    if ( !Engine::GAPI->GetRendererState().RendererSettings.HeroAffectsObjects ) {
        return;
    }

    zCVob* player = Engine::GAPI->GetPlayerVob();
    if ( !player ) {
        return;
    }

    const XMFLOAT3 playerPosition = player->GetPositionWorld();
    windBuff.interactionPositions[0] = float4( playerPosition.x, playerPosition.y, playerPosition.z, 1.0f );

    // Reserve slot 0 for the hero.
    const XMFLOAT3 interactionSelectionCenter = Engine::GAPI->GetCameraPosition();
    const float NpcInteractionSearchRadius = 1200.0f;
    Engine::GAPI->CollectNearbyNpcInteractionPositions(
        interactionSelectionCenter,
        NpcInteractionSearchRadius,
        MAX_CHARACTER_INTERACTION_NPCS,
        &windBuff.interactionPositions[1] );
}

static bool MaterialAllowsNormalmap( const MaterialInfo* info ) {
    return !info || info->buffer.NormalmapStrength > 0.0001f;
}

static bool MaterialAllowsDisplacement( const MaterialInfo* info ) {
    return !info || info->buffer.DisplacementFactor > 0.0001f;
}

static ID3D11ShaderResourceView* GetMaterialNormalmapSRV( MyDirectDrawSurface7* surface, const MaterialInfo* info ) {
    const auto& settings = Engine::GAPI->GetRendererState().RendererSettings;
    if ( !surface || !settings.AllowNormalmaps || !MaterialAllowsNormalmap( info ) || !surface->GetNormalmap() ) {
        return nullptr;
    }
    return surface->GetNormalmap()->GetShaderResourceView().Get();
}

static ID3D11ShaderResourceView* GetParallaxDisplacementSRV( MyDirectDrawSurface7* surface, const MaterialInfo* info ) {
    const auto& settings = Engine::GAPI->GetRendererState().RendererSettings;
    if ( !surface || !settings.AllowNormalmaps || !settings.EnableParallaxOcclusionMapping
        || !MaterialAllowsNormalmap( info ) || !MaterialAllowsDisplacement( info )
        || !surface->GetNormalmap() || !surface->GetDisplacementmap() ) {
        return nullptr;
    }
    return surface->GetDisplacementmap()->GetShaderResourceView().Get();
}

static bool IsOilLampEmissiveTexture( const zCTexture* texture ) {
    if ( !texture )
        return false;

    const std::string name = texture->GetNameWithoutExt();
    return _stricmp( name.c_str(), "NW_MISC_OILLAMP_02" ) == 0;
}

static MaterialInfo::Buffer GetEffectiveMaterialBuffer(
    const MaterialInfo* info, MyDirectDrawSurface7* surface,
    bool oilLampEmissive = false, bool windowGlassSplit = false ) {
    MaterialInfo defaults;
    MaterialInfo::Buffer buffer = info ? info->buffer : defaults.buffer;

    if ( !surface || !surface->GetNormalmap() || buffer.NormalmapStrength <= 0.0001f ) {
        buffer.NormalmapStrength = 0.0f;
    }

    if ( !surface || !surface->GetNormalmap() || !surface->GetDisplacementmap() || buffer.DisplacementFactor <= 0.0001f ) {
        buffer.DisplacementFactor = 0.0f;
    }

    // Transient renderer marker; deliberately not part of Materials.json.
    buffer.Padding0 = oilLampEmissive ? 1.0f : 0.0f;
    buffer.Padding1 = windowGlassSplit ? 1.0f : 0.0f;

    return buffer;
}

static void UpdateRefractionViewProjection( RefractionInfoConstantBuffer& buffer ) {
    XMMATRIX view = Engine::GAPI->GetViewMatrixXM();
    XMMATRIX proj = XMLoadFloat4x4( &Engine::GAPI->GetProjectionMatrix() );
    const XMMATRIX viewProj = XMMatrixMultiply( proj, view );
    XMStoreFloat4x4( &buffer.RI_ViewProj, viewProj );
    XMStoreFloat4x4( &buffer.RI_InvViewProj, XMMatrixInverse( nullptr, viewProj ) );
}

typedef void( __cdecl* PFN_DRAWMULTIINDEXEDINSTANCEDINDIRECT )(ID3D11DeviceContext* context, unsigned int drawCount,
    ID3D11Buffer* buffer, unsigned int alignedByteOffsetForArgs, unsigned int alignedByteStrideForArgs);
typedef void( __cdecl* PFN_BEGINUAVOVERLAP )(ID3D11DeviceContext* context);
typedef void( __cdecl* PFN_ENDUAVOVERLAP )(ID3D11DeviceContext* context);

PFN_DRAWMULTIINDEXEDINSTANCEDINDIRECT DrawMultiIndexedInstancedIndirect = nullptr;
PFN_BEGINUAVOVERLAP BeginUAVOverlap = nullptr;
PFN_ENDUAVOVERLAP EndUAVOverlap = nullptr;

static std::unique_ptr<D3D11NVAPI> nvapiDevice;
static std::unique_ptr<D3D11IGDEXT> igdextDevice;
static std::unique_ptr<D3D11AGS> agsDevice;

extern bool userHaveAMDGPU;

namespace
{
    static ID3D11ShaderResourceView* s_nullSRVs[16] = { nullptr };

    struct WaterMaterialInfoConstantBuffer {
        float WM_OceanClimate;
        float WM_DisableRainEffects;
        float WM_OceanWaterTintStrength;
        float WM_IsOceanWater;
        XMFLOAT3 WM_OceanWaterTint;
        float WM_Padding0;
        float WM_CameraUnderwater;
        XMFLOAT3 WM_Padding1;
    };
    static_assert( sizeof( WaterMaterialInfoConstantBuffer ) == 48 );

    struct WaterAnimationTimeConstantBuffer {
        float M_TotalTime;
        float M_WaterWaveTime;
        float M_Padding0;
        float M_Padding1;
    };
    static_assert( sizeof( WaterAnimationTimeConstantBuffer ) == 16 );



    std::string NormalizeVisualStemForMarker( std::string name ) {
        const size_t slash = name.find_last_of( "\\/" );
        if ( slash != std::string::npos ) {
            name.erase( 0, slash + 1 );
        }
        const size_t dot = name.find_last_of( '.' );
        if ( dot != std::string::npos ) {
            name.resize( dot );
        }
        for ( char& c : name ) {
            if ( c >= 'a' && c <= 'z' ) {
                c = static_cast<char>(c - 'a' + 'A');
            }
        }
        return name;
    }

    bool IsTwoSidedBacklitVegetationVisual( const std::string& visualName ) {
        const std::string stem = NormalizeVisualStemForMarker( visualName );
        static constexpr const char* markers[] = {
            "NW_NATURE_GRASSGROUP",
            "OW_NATURE_BUSH_02",
            "OW_NATURE_BUSH_03",
            "NW_NATURE_PLANT_03",
            "NW_KORN",
            "OW_GRASS_WINTER",
            "NW_NATURE_WATERGRASS_56P"
        };
        for ( const char* marker : markers ) {
            if ( stem.rfind( marker, 0 ) == 0 ) {
                return true;
            }
        }
        return false;
    }

    bool IsWindowGlassVisual( const std::string& visualName ) {
        const std::string stem = NormalizeVisualStemForMarker( visualName );
        static constexpr const char* windowVisuals[] = {
            "NW_CITY_WINDOW_STONE_OUT_01",
            "NW_CITY_WINDOW_WOOD_IN_01",
            "NW_CITY_WINDOW_WOOD_IN_02",
            "NW_CITY_WINDOW_WOOD_OUT_01",
        };
        for ( const char* candidate : windowVisuals ) {
            if ( stem == candidate ) {
                return true;
            }
        }
        return false;
    }

    bool IsCityWindowFeatureReady() {
        if ( !Engine::GAPI ) {
            return false;
        }
        if ( !Engine::GAPI->GetRendererState().RendererSettings.GetEffectiveCityWindowTransparency() ) {
            return false;
        }
        if ( !Engine::GAPI->IsWorldRenderCacheReady() ) {
            return false;
        }
        const WorldInfo* loadedWorld = Engine::GAPI->GetLoadedWorldInfo();
        if ( !loadedWorld || !loadedWorld->BspTree ) {
            return false;
        }
        const oCGame* game = oCGame::GetGame();
        return game && game->_zCSession_world;
    }

    bool IsIndoorWindowVisual( const std::string& visualName ) {
        const std::string stem = NormalizeVisualStemForMarker( visualName );
        return stem == "NW_CITY_WINDOW_WOOD_IN_01"
            || stem == "NW_CITY_WINDOW_WOOD_IN_02";
    }

    bool IsSunDaylightActive() {
        if ( !Engine::GAPI || !Engine::GAPI->GetSky() ) {
            return false;
        }
        return Engine::GAPI->GetSky()->GetAtmosphereCB().AC_SunVisibility > 0.001f;
    }

    XMFLOAT3 GetWindowGlassTint() {
        float daylightFactor = 1.0f;
        float rainFactor = 0.0f;
        if ( Engine::GAPI ) {
            if ( GSky* sky = Engine::GAPI->GetSky() ) {
                const auto& atmosphere = sky->GetAtmosphereCB();
                const float linearDaylight = std::clamp(
                    (atmosphere.AC_LightPos.y + 0.08f) / 0.20f, 0.0f, 1.0f );
                daylightFactor = linearDaylight * linearDaylight * (3.0f - 2.0f * linearDaylight);
                rainFactor = std::clamp( atmosphere.AC_SceneWettness, 0.0f, 1.0f );
            }
        }

        // Tint the static replacement with the current atmosphere.
        const XMFLOAT3 clearDayTint( 0.78f, 0.76f, 0.70f );
        const XMFLOAT3 rainyDayTint( 0.42f, 0.45f, 0.48f );
        const XMFLOAT3 nightTint( 0.18f, 0.24f, 0.38f );
        const XMFLOAT3 dayTint(
            std::lerp( clearDayTint.x, rainyDayTint.x, rainFactor ),
            std::lerp( clearDayTint.y, rainyDayTint.y, rainFactor ),
            std::lerp( clearDayTint.z, rainyDayTint.z, rainFactor ) );
        return XMFLOAT3(
            std::lerp( nightTint.x, dayTint.x, daylightFactor ),
            std::lerp( nightTint.y, dayTint.y, daylightFactor ),
            std::lerp( nightTint.z, dayTint.z, daylightFactor ) );
    }

    bool IsWindowGlassMaterial( const MeshVisualInfo* visual, const zCTexture* texture ) {
        if ( !visual || !texture || !IsWindowGlassVisual( visual->VisualName ) ) {
            return false;
        }
        const std::string textureName = texture->GetNameWithoutExt();
        return _stricmp( textureName.c_str(), "OBJ_CITY_WINDOWSTONE_01" ) == 0;
    }

    bool GetWindowGlassLocalBounds( MeshVisualInfo* visual, XMVECTOR& localMin, XMVECTOR& localMax ) {
        if ( !visual ) {
            return false;
        }

        if ( visual->WindowGlassBoundsInitialized ) {
            if ( visual->HasWindowGlassBounds ) {
                localMin = XMLoadFloat3( &visual->WindowGlassBounds.Min );
                localMax = XMLoadFloat3( &visual->WindowGlassBounds.Max );
            }
            return visual->HasWindowGlassBounds;
        }
        visual->WindowGlassBoundsInitialized = true;

        XMFLOAT3 minValues( FLT_MAX, FLT_MAX, FLT_MAX );
        XMFLOAT3 maxValues( -FLT_MAX, -FLT_MAX, -FLT_MAX );
        bool foundVertex = false;
        for ( const auto& [material, meshes] : visual->Meshes ) {
            zCTexture* texture = material ? material->GetTextureSingle() : nullptr;
            if ( !texture || _stricmp( texture->GetNameWithoutExt().c_str(),
                "OBJ_CITY_WINDOWSTONE_01" ) != 0 ) {
                continue;
            }
            for ( const MeshInfo* mesh : meshes ) {
                if ( !mesh ) {
                    continue;
                }
                for ( const ExVertexStruct& vertex : mesh->Vertices ) {
                    minValues.x = std::min( minValues.x, vertex.Position.x );
                    minValues.y = std::min( minValues.y, vertex.Position.y );
                    minValues.z = std::min( minValues.z, vertex.Position.z );
                    maxValues.x = std::max( maxValues.x, vertex.Position.x );
                    maxValues.y = std::max( maxValues.y, vertex.Position.y );
                    maxValues.z = std::max( maxValues.z, vertex.Position.z );
                    foundVertex = true;
                }
            }
        }
        if ( !foundVertex ) {
            return false;
        }
        visual->WindowGlassBounds.Min = minValues;
        visual->WindowGlassBounds.Max = maxValues;
        visual->HasWindowGlassBounds = true;
        localMin = XMLoadFloat3( &minValues );
        localMax = XMLoadFloat3( &maxValues );
        return true;
    }



    bool IsOceanWaterTexture( zCTexture* texture ) {
        if ( !texture ) {
            return false;
        }

        const std::string stem = NormalizeVisualStemForMarker( texture->GetNameWithoutExt() );
        return stem.rfind( "NW_WATER_LAKE", 0 ) == 0;
    }

    bool IsGrassWindVisual( const std::string& visualName ) {
        const std::string stem = NormalizeVisualStemForMarker( visualName );
        return stem.find( "GRASS" ) != std::string::npos
            || stem.find( "KORN" ) != std::string::npos;
    }

    void FillWaterMaterialInfo( WaterMaterialInfoConstantBuffer& wmcb, zCTexture* texture ) {
        const WorldInfo* world = Engine::GAPI->GetLoadedWorldInfo();
        const bool mediterraneanOcean = world
            && NormalizeVisualStemForMarker( world->WorldName ) == "ADDONWORLD";
        wmcb.WM_OceanClimate = mediterraneanOcean ? 1.0f : 0.0f;
        wmcb.WM_DisableRainEffects = 0.0f;
        wmcb.WM_OceanWaterTintStrength = mediterraneanOcean ? 0.48f : 0.18f;
        wmcb.WM_IsOceanWater = IsOceanWaterTexture( texture ) ? 1.0f : 0.0f;
        // Preserve chroma without changing exposure.
        wmcb.WM_OceanWaterTint = mediterraneanOcean
            ? XMFLOAT3( 0.426781f, 1.138082f, 1.321880f )
            : XMFLOAT3( 0.894520f, 1.026067f, 1.052377f );
        wmcb.WM_Padding0 = 0.0f;
        // Used by ocean water; legacy water ignores it.
        wmcb.WM_CameraUnderwater = Engine::GAPI->IsUnderWater() ? 1.0f : 0.0f;
        wmcb.WM_Padding1 = XMFLOAT3( 0.0f, 0.0f, 0.0f );
    }

    float4 ComputeTransparencyTextureFactor( zCMaterial* material ) {
        const float4 defaultFactor( 1.0f, 1.0f, 1.0f, 1.0f );
        if ( !material ) {
            return defaultFactor;
        }

        if ( material->GetEnvMapEnabled() ) {
            const float intensity = std::clamp( material->GetEnvMapStrength(), 0.0f, 1.0f );
            const uint8_t alpha = static_cast<uint8_t>( intensity * 255.0f );
            return zColor( 255, 255, 255, alpha ).ToFloat4();
        }

        zColor materialColor( material->GetColor() );
        return materialColor.bgra.alpha < 255
            ? materialColor.ToFloat4()
            : defaultFactor;
    }

    bool EnsureStructuredMatrixBuffer(
        std::unique_ptr<D3D11VertexBuffer>& buffer,
        UINT matrixCount,
        const char* debugName
    ) {
        const UINT safeMatrixCount = std::max<UINT>( matrixCount, 1u );
        const UINT requiredBytes = safeMatrixCount * static_cast<UINT>(sizeof( XMFLOAT4X4 ));

        if ( !buffer || buffer->GetSizeInBytes() < requiredBytes ) {
            auto newBuffer = std::make_unique<D3D11VertexBuffer>();
            if ( XR_SUCCESS != newBuffer->Init(
                nullptr,
                requiredBytes,
                D3D11VertexBuffer::B_SHADER_RESOURCE,
                D3D11VertexBuffer::U_DEFAULT,
                D3D11VertexBuffer::CA_NONE,
                debugName ? debugName : "SkeletalBoneStructuredBuffer",
                sizeof( XMFLOAT4X4 ) ) ) {
                return false;
            }

            SetDebugName( newBuffer->GetVertexBuffer().Get(), debugName ? debugName : "SkeletalBoneStructuredBuffer" );
            if ( debugName ) {
                SetDebugName( newBuffer->GetShaderResourceView().Get(), std::string( debugName ) + "_SRV" );
            }

            buffer = std::move( newBuffer );
        }

        return true;
    }

    bool UploadStructuredMatrixBuffer(
        std::unique_ptr<D3D11VertexBuffer>& buffer,
        const std::vector<XMFLOAT4X4>& matrices,
        const char* debugName
    ) {
        const UINT matrixCount = static_cast<UINT>(matrices.size());
        if ( !EnsureStructuredMatrixBuffer( buffer, matrixCount, debugName ) ) {
            return false;
        }

        if ( matrices.empty() ) {
            static const XMFLOAT4X4 identity = {
                1, 0, 0, 0,
                0, 1, 0, 0,
                0, 0, 1, 0,
                0, 0, 0, 1,
            };
            return XR_SUCCESS == buffer->UpdateBufferSubresource( &identity, sizeof( identity ) );
        }

        return XR_SUCCESS == buffer->UpdateBufferSubresource(
            matrices.data(),
            matrixCount * static_cast<UINT>(sizeof( XMFLOAT4X4 )) );
    }

    void PrintD3DFeatureLevel( D3D_FEATURE_LEVEL lvl ) {
        std::map<D3D_FEATURE_LEVEL, std::string> dxFeatureLevelsMap = {
            {D3D_FEATURE_LEVEL::D3D_FEATURE_LEVEL_12_1, "D3D_FEATURE_LEVEL_12_1"},
            {D3D_FEATURE_LEVEL::D3D_FEATURE_LEVEL_12_0, "D3D_FEATURE_LEVEL_12_0"},
            {D3D_FEATURE_LEVEL::D3D_FEATURE_LEVEL_11_1, "D3D_FEATURE_LEVEL_11_1"},
            {D3D_FEATURE_LEVEL::D3D_FEATURE_LEVEL_11_0, "D3D_FEATURE_LEVEL_11_0"},
            {D3D_FEATURE_LEVEL::D3D_FEATURE_LEVEL_10_1, "D3D_FEATURE_LEVEL_10_1"},
            {D3D_FEATURE_LEVEL::D3D_FEATURE_LEVEL_10_0, "D3D_FEATURE_LEVEL_10_0"},
            {D3D_FEATURE_LEVEL::D3D_FEATURE_LEVEL_9_3 , "D3D_FEATURE_LEVEL_9_3" },
            {D3D_FEATURE_LEVEL::D3D_FEATURE_LEVEL_9_2 , "D3D_FEATURE_LEVEL_9_2" },
            {D3D_FEATURE_LEVEL::D3D_FEATURE_LEVEL_9_1 , "D3D_FEATURE_LEVEL_9_1" },
        };
        LogInfo() << "D3D_FEATURE_LEVEL: " << dxFeatureLevelsMap.at( lvl );
    }

    FORCEINLINE uint64_t BuildSortKeyBase( zCMaterial* mat ) {
        const uint64_t isAlpha = mat->GetAniTexture()->HasAlphaChannel() ? 1ULL : 0ULL;
        const uint64_t sortKeyBase = (isAlpha << 63);
        const uint64_t texPtr = reinterpret_cast<uint64_t>(mat->GetAniTexture());
        return sortKeyBase | (texPtr << 16);
    }

    XMFLOAT3 GetBoundingBoxCenter( const zTBBox3D& bbox ) {
        return XMFLOAT3(
            (bbox.Min.x + bbox.Max.x) * 0.5f,
            (bbox.Min.y + bbox.Max.y) * 0.5f,
            (bbox.Min.z + bbox.Max.z) * 0.5f );
    }

    float ComputeWorldMeshDistanceSqFromCamera(
        const WorldMeshSectionInfo* section,
        const WorldMeshInfo* mesh,
        FXMVECTOR cameraPosition ) {
        const zTBBox3D* sourceBounds = nullptr;
        if ( mesh && mesh->HasBoundingBox ) {
            sourceBounds = &mesh->BoundingBox;
        } else if ( section ) {
            sourceBounds = &section->BoundingBox;
        }

        if ( !sourceBounds ) {
            return 0.0f;
        }

        const XMFLOAT3 center = GetBoundingBoxCenter( *sourceBounds );
        float distanceSq = 0.0f;
        XMStoreFloat( &distanceSq, XMVector3LengthSq( XMLoadFloat3( &center ) - cameraPosition ) );
        return distanceSq;
    }
}

void ConstantBufferPool::BeginFrame() {
    m_currentOffset = 0;
}

ConstantBufferAllocation ConstantBufferPool::Allocate( ID3D11DeviceContext* context, const void* pData, uint32_t sizeInBytes ) {
    uint32_t alignedSize = (sizeInBytes + 255) & ~255;

    if ( m_currentOffset + alignedSize > m_bufferSize ) {
        m_currentOffset = 0; // Reset the offset if out of memory
    }
    D3D11_MAPPED_SUBRESOURCE mappedResource;
    if ( SUCCEEDED( context->Map( m_poolBuffer.Get(), 0, m_currentOffset == 0 ? D3D11_MAP_WRITE_DISCARD : D3D11_MAP_WRITE_NO_OVERWRITE, 0, &mappedResource ) ) ) {
        memcpy( static_cast<uint8_t*>(mappedResource.pData) + m_currentOffset, pData, sizeInBytes );
        context->Unmap( m_poolBuffer.Get(), 0 );
    }

    ConstantBufferAllocation alloc;
    alloc.pBuffer = m_poolBuffer.Get();
    alloc.offsetInBytes = m_currentOffset;
    alloc.sizeInBytes = alignedSize;

    // 4. Advance the offset for the next allocation
    m_currentOffset += alignedSize;

    return alloc;
}

void ConstantBufferPool::EndFrame( ) {
}

D3D11GraphicsEngine::D3D11GraphicsEngine() :
    DebugPointlight(nullptr),
    m_LastFrameLimit(0),
    RenderingStage(DES_MAIN),
    InverseUnitSphereMesh(nullptr),
    QuadVertexBuffer(nullptr),
    QuadIndexBuffer(nullptr),
    CachedRefreshRate{0, 0},
    frameLatencyWaitableObject(nullptr),
    m_ConfiguredMaximumFrameLatency(0),
    SaveScreenshotNextFrame(false),
    m_flipWithTearing(false),
    m_swapchainflip(false),
    m_lowlatency(false),
    m_HDR(false),
    m_previousFpsLimit(0),
    m_isWindowActive(false),
    m_FrameNeedsJitter(false),
    m_previousResolutionScalePercent(-1),
    m_resizeInProgress(false)
{
    Effects = std::make_unique<D3D11Effect>();
    TemporalState = std::make_unique<D3D11TemporalState>();
    LineRenderer = std::make_unique<D3D11LineRenderer>();

    m_FrameLimiter = std::make_unique<FpsLimiter>();

    // Initialize previous view-proj matrix to identity for motion vectors
    XMStoreFloat4x4(&m_PrevViewProjMatrix, XMMatrixIdentity());

    // Match the resolution with the current desktop resolution
    Resolution = m_scaledResolution =
        Engine::GAPI->GetRendererState().RendererSettings.LoadedResolution;
    m_swapchainResolution = Resolution;
    unionCurrentCustomFontMultiplier = 1.0;
}

D3D11GraphicsEngine::~D3D11GraphicsEngine() {
    // Release pointlight handles before their PFX and shadow owners.
    if ( Engine::GAPI ) {
        Engine::GAPI->ReleasePointlightResources();
    }

    // Release post-processing and its vendor contexts before D3D device teardown.
    PfxRenderer.reset();
    // ShadowMaps owns the tiled cube arrays and keeps device/context references.
    // Destroy it while the D3D device is still valid, especially before the AMD
    // path detaches and explicitly destroys that device below.
    ShadowMaps.reset();
    GothicDepthBufferStateInfo::DeleteCachedObjects();
    GothicBlendStateInfo::DeleteCachedObjects();
    GothicRasterizerStateInfo::DeleteCachedObjects();

    SAFE_DELETE( InverseUnitSphereMesh );

    SAFE_DELETE( QuadVertexBuffer );

    if ( frameLatencyWaitableObject ) {
        CloseHandle( frameLatencyWaitableObject );
        frameLatencyWaitableObject = nullptr;
    }
    SAFE_DELETE( QuadIndexBuffer );

    ID3D11Debug* d3dDebug = nullptr;
    if ( Device ) {
        Device->QueryInterface( __uuidof(ID3D11Debug), reinterpret_cast<void**>(&d3dDebug) );
    }

    if ( d3dDebug ) {
        d3dDebug->ReportLiveDeviceObjects( D3D11_RLDO_DETAIL );
        d3dDebug->Release();
    }

    // Delete Vendor-Specific stuff here
    nvapiDevice.reset();
    igdextDevice.reset();
    if ( agsDevice ) {
        // Amd likes to be special :)
        agsDevice->DestroyD3D11Device( Device.Detach(), Context.Detach() );
        agsDevice.reset();
    }

    // Hooks may still run during GothicAPI teardown.
    if ( Engine::GraphicsEngine == this ) {
        Engine::GraphicsEngine = nullptr;
    }

}

ID3D11ShaderResourceView* D3D11GraphicsEngine::GetWindowGlassReplacementSRV() {
    // Loading screens and the main menu can reach this before the world cache exists.
    if ( !IsCityWindowFeatureReady() ) {
        return nullptr;
    }

    if ( WindowGlassReplacementTexture ) {
        return WindowGlassReplacementTexture->GetShaderResourceView().Get();
    }

    const std::filesystem::path replacementPath = std::filesystem::absolute(
        R"(system\GD3D11\Textures\City_Window.dds)" );
    if ( !Toolbox::FileExists( replacementPath.string() ) ) {
        static bool reportedMissing = false;
        if ( !reportedMissing ) {
            reportedMissing = true;
            LogWarn() << "Window glass replacement is missing: " << replacementPath.string();
        }
        return nullptr;
    }

    auto replacement = std::make_unique<D3D11Texture>();
    if ( XR_SUCCESS != replacement->Init( replacementPath.string() ) ) {
        LogWarn() << "Failed to load window glass replacement: " << replacementPath.string();
        return nullptr;
    }

    WindowGlassReplacementTexture = std::move( replacement );
    LogInfo() << "Loaded VOB-scoped window glass replacement: " << replacementPath.string();
    return WindowGlassReplacementTexture->GetShaderResourceView().Get();
}

void D3D11GraphicsEngine::EnsureFrameVobVisibilityCollected() {
    auto& cache = m_FrameGeometryCache;
    if ( cache.vobVisibilityCollected )
        return;

    cache.visibleWindowVobs.clear();
    const auto& renderSettings = Engine::GAPI->GetRendererState().RendererSettings;
    if ( !renderSettings.DrawVOBs && !renderSettings.EnableDynamicLighting )
        return;

    const WorldInfo* loadedWorld = Engine::GAPI->GetLoadedWorldInfo();
    if ( !loadedWorld || !loadedWorld->BspTree
        || !Engine::GAPI->IsWorldRenderCacheReady() ) {
        // Wait until the world cache is published.
        return;
    }

    cache.vobVisibilityCollected = true;

    const uint64_t worldGeneration = Engine::GAPI->GetCityWindowConfigurationGeneration();
    if ( m_CollectedVobWorldGeneration != worldGeneration ) {
        // Camera lists contain raw VOB pointers and must be rebuilt after a reconfiguration.
        m_CollectedCameraVobs.clear();
        m_CollectedCameraMobs.clear();
        m_CollectedVobWorldGeneration = worldGeneration;
    }

    const bool gpuVobShadersAvailable = ShaderManager
        && ShaderManager->GetCShader( CShaderID::CS_VobCull )
        && ShaderManager->GetCShader( CShaderID::CS_VobPatchArgs );
    const bool useGpuVobOcclusion = renderSettings.GetEffectiveGpuVobOcclusionCulling()
        && !FeatureLevel10Compatibility
        && renderSettings.DebugSettings.Culling.CullVobs
        && gpuVobShadersAvailable;
    if ( useGpuVobOcclusion ) {
        // The GPU path must receive the current distance/BSP candidate set;
        // retaining the previous camera list would make the GPU cull operate
        // on stale VOB pointers after the camera moved.
        m_CollectedCameraVobs.clear();
        m_CollectedCameraMobs.clear();
    }

    if ( useGpuVobOcclusion || !renderSettings.FixViewFrustum || m_CollectedCameraVobs.empty() ) {
        m_FrameLights.clear();
        ZoneScopedN( "D3D11GraphicsEngine::EnsureFrameVobVisibilityCollected" );
        Engine::GAPI->CollectVisibleVobs( m_CollectedCameraVobs, m_FrameLights,
            m_CollectedCameraMobs, EGothicCullFlags::CullAll,
            EBspTreeCollectFlags::COLLECT_ALL_MUTATE,
            useGpuVobOcclusion );
    }
    cache.cachedMobs = m_CollectedCameraMobs;

    if ( renderSettings.DrawVOBs ) {
        cache.visibleWindowVobs.reserve( std::min(
            m_CollectedCameraVobs.size(), WindowCutoutVolumeCache.size() ) );
        for ( VobInfo* vobInfo : m_CollectedCameraVobs ) {
            if ( vobInfo && vobInfo->CityWindowValidationInitialized )
                cache.visibleWindowVobs.push_back( vobInfo );
        }
        std::sort( cache.visibleWindowVobs.begin(), cache.visibleWindowVobs.end(),
            std::less<VobInfo*>{} );
    }
}

bool D3D11GraphicsEngine::EnsureGpuVobHiZResources() {
    if ( !DepthStencilBufferCopy || !DepthStencilBufferCopy->GetTexture()
        || !DepthStencilBufferCopy->GetShaderResView() ) {
        return false;
    }

    D3D11_TEXTURE2D_DESC depthDesc = {};
    DepthStencilBufferCopy->GetTexture()->GetDesc( &depthDesc );
    const UINT width = std::max( 1u, (depthDesc.Width + 1u) / 2u );
    const UINT height = std::max( 1u, (depthDesc.Height + 1u) / 2u );
    if ( width == GpuVobHiZWidth && height == GpuVobHiZHeight
        && GpuVobHiZTexture && GpuVobHiZShaderResourceView
        && !GpuVobHiZMipShaderResourceViews.empty()
        && !GpuVobHiZMipUnorderedAccessViews.empty() ) {
        return true;
    }

    UINT mipCount = 1;
    for ( UINT mipWidth = width, mipHeight = height;
        mipWidth > 1 || mipHeight > 1; ++mipCount ) {
        // D3D11 texture mip dimensions use floor(width / 2), so mirror that
        // exact layout when computing the number of levels.
        mipWidth = std::max( 1u, mipWidth / 2u );
        mipHeight = std::max( 1u, mipHeight / 2u );
    }

    D3D11_TEXTURE2D_DESC hiZDesc = {};
    hiZDesc.Width = width;
    hiZDesc.Height = height;
    hiZDesc.MipLevels = mipCount;
    hiZDesc.ArraySize = 1;
    hiZDesc.Format = DXGI_FORMAT_R32_FLOAT;
    hiZDesc.SampleDesc.Count = 1;
    hiZDesc.Usage = D3D11_USAGE_DEFAULT;
    hiZDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    if ( FAILED( GetDevice()->CreateTexture2D( &hiZDesc, nullptr, texture.GetAddressOf() ) )
        || !texture ) {
        return false;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC fullSrvDesc = {};
    fullSrvDesc.Format = DXGI_FORMAT_R32_FLOAT;
    fullSrvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    fullSrvDesc.Texture2D.MostDetailedMip = 0;
    fullSrvDesc.Texture2D.MipLevels = mipCount;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> fullSrv;
    if ( FAILED( GetDevice()->CreateShaderResourceView(
        texture.Get(), &fullSrvDesc, fullSrv.GetAddressOf() ) ) || !fullSrv ) {
        return false;
    }

    std::vector<Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>> mipSrvs( mipCount );
    std::vector<Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView>> mipUavs( mipCount );
    for ( UINT mip = 0; mip < mipCount; ++mip ) {
        D3D11_SHADER_RESOURCE_VIEW_DESC mipSrvDesc = {};
        mipSrvDesc.Format = DXGI_FORMAT_R32_FLOAT;
        mipSrvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        mipSrvDesc.Texture2D.MostDetailedMip = mip;
        mipSrvDesc.Texture2D.MipLevels = 1;
        if ( FAILED( GetDevice()->CreateShaderResourceView(
            texture.Get(), &mipSrvDesc, mipSrvs[mip].GetAddressOf() ) )
            || !mipSrvs[mip] ) {
            return false;
        }

        D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
        uavDesc.Format = DXGI_FORMAT_R32_FLOAT;
        uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
        uavDesc.Texture2D.MipSlice = mip;
        if ( FAILED( GetDevice()->CreateUnorderedAccessView(
            texture.Get(), &uavDesc, mipUavs[mip].GetAddressOf() ) )
            || !mipUavs[mip] ) {
            return false;
        }
    }

    GpuVobHiZTexture = std::move( texture );
    GpuVobHiZShaderResourceView = std::move( fullSrv );
    GpuVobHiZMipShaderResourceViews = std::move( mipSrvs );
    GpuVobHiZMipUnorderedAccessViews = std::move( mipUavs );
    GpuVobHiZWidth = width;
    GpuVobHiZHeight = height;
    GpuVobHiZMipCount = mipCount;
    return true;
}

uint64_t D3D11GraphicsEngine::BuildGpuVobPerformanceSettingsKey() const {
    const auto& settings = Engine::GAPI->GetRendererState().RendererSettings;
    uint64_t key = 1469598103934665603ull;
    const auto add = [&key]( const auto& value ) {
        key = HashGpuVobPerformanceSetting( key, value );
    };

    // Advanced switches under test. Use effective values so changing a
    // disabled Advanced control cannot contaminate the baseline measurement.
    add( settings.AdvancedPerformanceOptions );
    add( settings.GetEffectiveThreadedShadowCulling() );
    add( settings.GetEffectiveLazyCascadeUpdate() );
    add( settings.GetEffectiveDoZPrepass() );
    add( settings.GetEffectiveWorldSectionBVH() );
    add( static_cast<int>( settings.GetEffectiveShadowFrustumCullingMode() ) );
    add( settings.GetEffectiveGpuVobOcclusionCulling() );
    add( settings.DebugSettings.FeatureSet.UseMDI );
    add( settings.GetEffectiveXeGTAOHybrid() );
    add( settings.GetEffectiveXeGTAOPreLighting() );

    // Basic render state that changes the amount of work and must not be
    // mixed into the same comparison window.
    add( settings.EnableShadows );
    add( settings.ShadowQuality );
    add( settings.ShadowMapSize );
    add( settings.PointlightShadowMapSize );
    add( settings.NumShadowCascades );
    add( settings.ShadowFilterMode );
    add( settings.ShadowCascadePCFLimit );
    add( settings.MaxNumFaces );
    add( settings.ResolutionScalePercent );
    add( settings.AntiAliasingMode );
    add( settings.Upscaler );

    // Visible renderer settings that affect the amount of work must start a
    // fresh, unambiguous measurement window.
    add( settings.ShadowSoftness );
    add( settings.GetEffectiveWaterAnimation() );
    add( settings.GetEffectivePuddles() );
    add( settings.GetEffectiveWetGroundSSR() );
    add( settings.GetEffectiveNightEnhance() );
    add( settings.GetEffectiveCityWindowTransparency() );

    return key;
}

void D3D11GraphicsEngine::AccumulateGpuVobFrameRenderStats() {
    if ( m_GpuVobPerformanceStats.Frames == 0
        || m_LastGpuVobFrameStatsAccumulated == m_GpuVobPerformanceStats.Frames ) {
        return;
    }

    const auto& renderInfo = Engine::GAPI->GetRendererState().RendererInfo;
    const uint64_t triangles = renderInfo.FrameDrawnTriangles > 0
        ? static_cast<uint64_t>( renderInfo.FrameDrawnTriangles ) : 0ull;
    const uint64_t vobs = renderInfo.FrameDrawnVobs > 0
        ? static_cast<uint64_t>( renderInfo.FrameDrawnVobs ) : 0ull;

    ++m_GpuVobPerformanceStats.FrameTriangleSamples;
    m_GpuVobPerformanceStats.FrameTrianglesTotal += triangles;
    if ( m_GpuVobPerformanceStats.FrameTriangleSamples == 1
        || triangles < m_GpuVobPerformanceStats.FrameTrianglesMin ) {
        m_GpuVobPerformanceStats.FrameTrianglesMin = triangles;
    }
    m_GpuVobPerformanceStats.FrameTrianglesMax = std::max(
        m_GpuVobPerformanceStats.FrameTrianglesMax, triangles );
    m_GpuVobPerformanceStats.FrameVobsTotal += vobs;
    m_LastGpuVobFrameStatsAccumulated = m_GpuVobPerformanceStats.Frames;
}

void D3D11GraphicsEngine::ApplyGpuVobDisplayTriangleEstimate() {
    if ( !m_HasLastGpuMainVisibilityRatio
        || m_GpuVobFrameMainCandidateTriangles == 0 ) {
        return;
    }

    auto& renderInfo = Engine::GAPI->GetRendererState().RendererInfo;
    const uint64_t submittedTriangles = renderInfo.FrameDrawnTriangles > 0
        ? static_cast<uint64_t>( renderInfo.FrameDrawnTriangles ) : 0ull;
    const uint64_t nonGpuMainTriangles = submittedTriangles
        > m_GpuVobFrameMainCandidateTriangles
        ? submittedTriangles - m_GpuVobFrameMainCandidateTriangles : 0ull;
    // The visibility readback is deliberately asynchronous. Applying the raw
    // triangle count from that older frame made the in-game counter jump when
    // the camera moved. Reuse only its visibility ratio for the current frame's
    // candidate triangle count; this keeps the overlay useful without stalling
    // the D3D11 immediate context for a readback.
    const uint64_t estimatedGpuMainTriangles = static_cast<uint64_t>( std::llround(
        static_cast<double>( m_GpuVobFrameMainCandidateTriangles )
            * std::clamp( m_LastGpuMainVisibilityRatio, 0.0, 1.0 ) ) );
    const uint64_t estimatedTriangles = nonGpuMainTriangles
        + estimatedGpuMainTriangles;
    renderInfo.FrameDrawnTriangles = static_cast<int>( std::min<uint64_t>(
        estimatedTriangles, static_cast<uint64_t>( std::numeric_limits<int>::max() ) ) );
}

bool D3D11GraphicsEngine::EnsureGpuTimingQueries() {
    if ( m_GpuTimingQueriesInitialized ) {
        return m_GpuTimingAvailable;
    }

    m_GpuTimingQueriesInitialized = true;
    if ( !Device || !Context ) {
        return false;
    }

    D3D11_QUERY_DESC timestampDesc = {};
    timestampDesc.Query = D3D11_QUERY_TIMESTAMP;
    D3D11_QUERY_DESC disjointDesc = {};
    disjointDesc.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;

    for ( auto& frame : m_GpuTimingFrames ) {
        if ( FAILED( Device->CreateQuery( &disjointDesc, &frame.Disjoint ) ) ) {
            m_GpuTimingAvailable = false;
            return false;
        }
        for ( unsigned int zone = 0; zone < GpuTimingZoneCount; ++zone ) {
            for ( unsigned int interval = 0; interval < GpuTimingMaxIntervals; ++interval ) {
                if ( FAILED( Device->CreateQuery( &timestampDesc,
                    &frame.Timestamps[zone][interval][0] ) )
                    || FAILED( Device->CreateQuery( &timestampDesc,
                        &frame.Timestamps[zone][interval][1] ) ) ) {
                    m_GpuTimingAvailable = false;
                    return false;
                }
            }
        }
    }

    m_GpuTimingAvailable = true;
    return true;
}

void D3D11GraphicsEngine::BeginGpuTimingFrame(
    uint64_t settingsKey, uint64_t worldGeneration ) {
    m_GpuTimingFrameActive = false;
    if ( !EnsureGpuTimingQueries() || !Context ) {
        return;
    }

    for ( unsigned int attempt = 0; attempt < GpuTimingRingSize; ++attempt ) {
        m_GpuTimingFrameIndex = (m_GpuTimingFrameIndex + 1) % GpuTimingRingSize;
        auto& frame = m_GpuTimingFrames[m_GpuTimingFrameIndex];
        if ( frame.Pending ) {
            continue;
        }

        for ( unsigned int zone = 0; zone < GpuTimingZoneCount; ++zone ) {
            frame.IntervalCount[zone] = 0;
            frame.Active[zone] = false;
        }
        frame.SettingsKey = settingsKey;
        frame.WorldGeneration = worldGeneration;
        frame.Pending = true;
        frame.PendingFrames = 0;
        Context->Begin( frame.Disjoint.Get() );
        m_GpuTimingFrameActive = true;
        return;
    }

    ++m_GpuVobPerformanceStats.GpuTimingDroppedFrames;
}

void D3D11GraphicsEngine::EndGpuTimingFrame() {
    if ( !m_GpuTimingFrameActive || !Context ) {
        return;
    }

    auto& frame = m_GpuTimingFrames[m_GpuTimingFrameIndex];
    for ( unsigned int zone = 0; zone < GpuTimingZoneCount; ++zone ) {
        // A malformed caller must not leave a timestamp interval open across
        // frames. Normal render paths close every interval at its draw/dispatch
        // boundary, so this is only a defensive cleanup.
        if ( frame.Active[zone] ) {
            EndGpuTimingZone( static_cast<EGpuTimingZone>( zone ) );
        }
    }
    Context->End( frame.Disjoint.Get() );
    m_GpuTimingFrameActive = false;
}

void D3D11GraphicsEngine::PollGpuTimingQueries(
    uint64_t settingsKey, uint64_t worldGeneration ) {
    if ( !m_GpuTimingAvailable || !Context ) {
        return;
    }

    for ( auto& frame : m_GpuTimingFrames ) {
        if ( !frame.Pending ) {
            continue;
        }
        if ( ++frame.PendingFrames > 32 ) {
            frame.Pending = false;
            frame.PendingFrames = 0;
            ++m_GpuVobPerformanceStats.GpuTimingDroppedFrames;
            continue;
        }
        if ( frame.SettingsKey != settingsKey
            || frame.WorldGeneration != worldGeneration ) {
            frame.Pending = false;
            frame.PendingFrames = 0;
            continue;
        }

        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint = {};
        if ( Context->GetData( frame.Disjoint.Get(), &disjoint,
            sizeof( disjoint ), D3D11_ASYNC_GETDATA_DONOTFLUSH ) != S_OK ) {
            continue;
        }

        double zoneMs[GpuTimingZoneCount] = {};
        bool ready = true;
        for ( unsigned int zone = 0; zone < GpuTimingZoneCount && ready; ++zone ) {
            for ( unsigned int interval = 0;
                interval < frame.IntervalCount[zone]; ++interval ) {
                UINT64 beginTimestamp = 0;
                UINT64 endTimestamp = 0;
                if ( Context->GetData( frame.Timestamps[zone][interval][0].Get(),
                    &beginTimestamp, sizeof( beginTimestamp ),
                    D3D11_ASYNC_GETDATA_DONOTFLUSH ) != S_OK
                    || Context->GetData( frame.Timestamps[zone][interval][1].Get(),
                        &endTimestamp, sizeof( endTimestamp ),
                        D3D11_ASYNC_GETDATA_DONOTFLUSH ) != S_OK ) {
                    ready = false;
                    break;
                }
                if ( endTimestamp >= beginTimestamp && disjoint.Frequency > 0 ) {
                    zoneMs[zone] += (static_cast<double>( endTimestamp - beginTimestamp )
                        / static_cast<double>( disjoint.Frequency )) * 1000.0;
                }
            }
        }
        if ( !ready ) {
            continue;
        }

        frame.Pending = false;
        frame.PendingFrames = 0;
        if ( disjoint.Disjoint ) {
            continue;
        }

        ++m_GpuVobPerformanceStats.GpuTimingFrames;
        const auto addZone = [&zoneMs]( EGpuTimingZone zone,
            uint64_t& samples, double& total ) {
            const unsigned int index = static_cast<unsigned int>( zone );
            if ( zoneMs[index] > 0.0 ) {
                ++samples;
                total += zoneMs[index];
            }
        };
        addZone( EGpuTimingZone::HiZ, m_GpuVobPerformanceStats.GpuHiZTimingSamples,
            m_GpuVobPerformanceStats.GpuHiZMsTotal );
        addZone( EGpuTimingZone::MainCull, m_GpuVobPerformanceStats.GpuMainCullTimingSamples,
            m_GpuVobPerformanceStats.GpuMainCullMsTotal );
        addZone( EGpuTimingZone::VobDepth, m_GpuVobPerformanceStats.GpuVobDepthTimingSamples,
            m_GpuVobPerformanceStats.GpuVobDepthMsTotal );
        addZone( EGpuTimingZone::VobLit, m_GpuVobPerformanceStats.GpuVobLitTimingSamples,
            m_GpuVobPerformanceStats.GpuVobLitMsTotal );
    }
}

void D3D11GraphicsEngine::BeginGpuTimingZone( EGpuTimingZone zone ) {
    if ( !m_GpuTimingFrameActive || !Context ) {
        return;
    }
    const unsigned int index = static_cast<unsigned int>( zone );
    if ( index >= GpuTimingZoneCount || m_GpuTimingFrames[m_GpuTimingFrameIndex].Active[index] ) {
        return;
    }
    auto& frame = m_GpuTimingFrames[m_GpuTimingFrameIndex];
    if ( frame.IntervalCount[index] >= GpuTimingMaxIntervals ) {
        ++m_GpuVobPerformanceStats.GpuTimingIntervalOverflow;
        return;
    }
    const unsigned int interval = frame.IntervalCount[index];
    Context->End( frame.Timestamps[index][interval][0].Get() );
    frame.Active[index] = true;
}

void D3D11GraphicsEngine::EndGpuTimingZone( EGpuTimingZone zone ) {
    if ( !m_GpuTimingFrameActive || !Context ) {
        return;
    }
    const unsigned int index = static_cast<unsigned int>( zone );
    if ( index >= GpuTimingZoneCount ) {
        return;
    }
    auto& frame = m_GpuTimingFrames[m_GpuTimingFrameIndex];
    if ( !frame.Active[index] || frame.IntervalCount[index] >= GpuTimingMaxIntervals ) {
        return;
    }
    const unsigned int interval = frame.IntervalCount[index];
    Context->End( frame.Timestamps[index][interval][1].Get() );
    ++frame.IntervalCount[index];
    frame.Active[index] = false;
}

void D3D11GraphicsEngine::LogGpuVobPerformanceStats() {
    const auto& settings = Engine::GAPI->GetRendererState().RendererSettings;
    const double averageFrameMs = m_GpuVobPerformanceStats.FrameTimeSamples > 0
        ? m_GpuVobPerformanceStats.FrameTimeMsTotal
            / static_cast<double>( m_GpuVobPerformanceStats.FrameTimeSamples )
        : 0.0;
    const double averageFps = averageFrameMs > 0.0 ? 1000.0 / averageFrameMs : 0.0;
    const double averageFpsCounter = m_GpuVobPerformanceStats.FpsSamples > 0
        ? m_GpuVobPerformanceStats.FpsTotal
            / static_cast<double>( m_GpuVobPerformanceStats.FpsSamples )
        : 0.0;
    const double averageTriangles = m_GpuVobPerformanceStats.FrameTriangleSamples > 0
        ? static_cast<double>( m_GpuVobPerformanceStats.FrameTrianglesTotal )
            / static_cast<double>( m_GpuVobPerformanceStats.FrameTriangleSamples )
        : 0.0;
    const double averageVobs = m_GpuVobPerformanceStats.FrameTriangleSamples > 0
        ? static_cast<double>( m_GpuVobPerformanceStats.FrameVobsTotal )
            / static_cast<double>( m_GpuVobPerformanceStats.FrameTriangleSamples )
        : 0.0;
    const double averageGpuVisibleInstances = m_GpuVobPerformanceStats.GpuVisibleCountSamples > 0
        ? static_cast<double>( m_GpuVobPerformanceStats.GpuVisibleInstances )
            / static_cast<double>( m_GpuVobPerformanceStats.GpuVisibleCountSamples )
        : 0.0;
    const double averageGpuCandidateInstances = m_GpuVobPerformanceStats.GpuVisibleCountSamples > 0
        ? static_cast<double>( m_GpuVobPerformanceStats.GpuVisibleCandidateInstances )
            / static_cast<double>( m_GpuVobPerformanceStats.GpuVisibleCountSamples )
        : 0.0;
    const double averageGpuOccludedInstances = m_GpuVobPerformanceStats.GpuVisibleCountSamples > 0
        ? static_cast<double>( m_GpuVobPerformanceStats.GpuVisibleOccludedInstances )
            / static_cast<double>( m_GpuVobPerformanceStats.GpuVisibleCountSamples )
        : 0.0;
    const double averageGpuVisibleTriangles = m_GpuVobPerformanceStats.GpuVisibleCountSamples > 0
        ? static_cast<double>( m_GpuVobPerformanceStats.GpuVisibleTriangles )
            / static_cast<double>( m_GpuVobPerformanceStats.GpuVisibleCountSamples )
        : 0.0;
    const double measurementFrames = std::max<double>(
        1.0, static_cast<double>( m_GpuVobPerformanceStats.Frames ) );
    const double gpuVisibleSamplePercent =
        (static_cast<double>( m_GpuVobPerformanceStats.GpuVisibleCountSamples )
            / measurementFrames) * 100.0;
    LogInfo() << "[GpuVobPerf] frames=" << m_GpuVobPerformanceStats.Frames
        << " settingsKey=" << m_GpuVobPerformanceSettingsKey
        << " worldGeneration=" << m_GpuVobPerformanceWorldGeneration
        << " frameMsAvg=" << averageFrameMs
        << " frameMsMin=" << m_GpuVobPerformanceStats.FrameTimeMsMin
        << " frameMsMax=" << m_GpuVobPerformanceStats.FrameTimeMsMax
        << " fpsAvg=" << averageFps
        << " fpsCounterAvg=" << averageFpsCounter
        << " trisAvg=" << averageTriangles
        << " trisMin=" << m_GpuVobPerformanceStats.FrameTrianglesMin
        << " trisMax=" << m_GpuVobPerformanceStats.FrameTrianglesMax
        << " vobsAvg=" << averageVobs
        << " gpuMainVisibleSamples=" << m_GpuVobPerformanceStats.GpuVisibleCountSamples
        << " gpuMainCandidateInstancesAvg=" << averageGpuCandidateInstances
        << " gpuMainVisibleInstancesAvg=" << averageGpuVisibleInstances
        << " gpuMainOccludedInstancesAvg=" << averageGpuOccludedInstances
        << " gpuMainVisibleTrianglesAvg=" << averageGpuVisibleTriangles
        << " gpuMainDisplayTrianglesLast=" << (m_HasLastGpuMainVisibleTrianglesEstimate
            ? m_LastGpuMainVisibleTrianglesEstimate : 0ull)
        << " gpuMainDisplayVisibilityRatioLast=" << (m_HasLastGpuMainVisibilityRatio
            ? m_LastGpuMainVisibilityRatio : 0.0)
        << " gpuMainVisibleSamplePct=" << gpuVisibleSamplePercent
        << " gpuMainOcclusionRejectPct=" << (averageGpuCandidateInstances > 0.0
            ? (averageGpuOccludedInstances / averageGpuCandidateInstances) * 100.0 : 0.0)
        << " advanced=" << (settings.AdvancedPerformanceOptions ? 1 : 0)
        << " shadowSoftness=" << settings.ShadowSoftness
        << " waterWaves=" << (settings.GetEffectiveWaterAnimation() ? 1 : 0)
        << " puddles=" << (settings.GetEffectivePuddles() ? 1 : 0)
        << " wetGroundSSR=" << (settings.GetEffectiveWetGroundSSR() ? 1 : 0)
        << " nightEnhance=" << (settings.GetEffectiveNightEnhance() ? 1 : 0)
        << " cityWindowTransparency=" << (settings.GetEffectiveCityWindowTransparency() ? 1 : 0)
        << " threadedShadow=" << (settings.GetEffectiveThreadedShadowCulling() ? 1 : 0)
        << " lazyCascade=" << (settings.GetEffectiveLazyCascadeUpdate() ? 1 : 0)
        << " depthPrepass=" << (settings.GetEffectiveDoZPrepass() ? 1 : 0)
        << " sectionBvh=" << (settings.GetEffectiveWorldSectionBVH() ? 1 : 0)
        << " shadowFrustum=" << static_cast<int>( settings.GetEffectiveShadowFrustumCullingMode() )
        << " gpuOcclusion=" << (settings.GetEffectiveGpuVobOcclusionCulling() ? 1 : 0)
        << " useMDI=" << (settings.DebugSettings.FeatureSet.UseMDI ? 1 : 0)
        << " xeGTAOHybrid=" << (settings.GetEffectiveXeGTAOHybrid() ? 1 : 0)
        << " xeGTAOPreLighting=" << (settings.GetEffectiveXeGTAOPreLighting() ? 1 : 0)
        << " gpuRuntimeDisabled=" << (IsGpuVobRuntimeDisabled() ? 1 : 0)
        << " hizRuntimeDisabled=" << (IsGpuVobHiZRuntimeDisabled() ? 1 : 0)
        << " arenaRuntimeDisabled=" << (IsGpuVobGeometryArenaRuntimeDisabled() ? 1 : 0)
        << " hiz=" << m_GpuVobPerformanceStats.HiZBuilt
        << "/" << m_GpuVobPerformanceStats.HiZRequests
        << " mainCull=" << m_GpuVobPerformanceStats.MainCullActive
        << "/" << m_GpuVobPerformanceStats.MainCullAttempts
        << " mainFallback=" << m_GpuVobPerformanceStats.MainCullFallback
        << " shadowVobCull=" << m_GpuVobPerformanceStats.ShadowVobCullActive
        << "/" << m_GpuVobPerformanceStats.ShadowVobCullAttempts
        << " shadowVobFallback=" << m_GpuVobPerformanceStats.ShadowVobCullFallback
        << " shadowVobIndirectCalls=" << m_GpuVobPerformanceStats.ShadowVobIndirectDrawCalls
        << " shadowVobLodMeshes=" << m_GpuVobPerformanceStats.ShadowVobLodMeshes
        << " shadowVobLodSourceTris=" << m_GpuVobPerformanceStats.ShadowVobLodSourceTriangles
        << " shadowVobLodTris=" << m_GpuVobPerformanceStats.ShadowVobLodTriangles
        << " indirectCalls=" << m_GpuVobPerformanceStats.IndirectDrawCalls
        << " mdiBatches=" << m_GpuVobPerformanceStats.MdiBatches
        << " mdiCommands=" << m_GpuVobPerformanceStats.MdiCommands
        << " cpuDrawCalls=" << m_GpuVobPerformanceStats.CpuFallbackDrawCalls
        << " candidateInstances=" << m_GpuVobPerformanceStats.CandidateInstances
        << " candidateVisuals=" << m_GpuVobPerformanceStats.CandidateVisuals
        << " candidateDrawItems=" << m_GpuVobPerformanceStats.CandidateDrawItems
        << " cullInputMBPerFrame=" << (static_cast<double>( m_GpuVobPerformanceStats.GpuCullInputCopyBytes )
            / measurementFrames / (1024.0 * 1024.0))
        << " cullMetadataKBPerFrame=" << (static_cast<double>( m_GpuVobPerformanceStats.GpuCullMetadataUploadBytes )
            / measurementFrames / 1024.0)
        << " cullArgsKBPerFrame=" << (static_cast<double>( m_GpuVobPerformanceStats.GpuCullArgsUploadBytes )
            / measurementFrames / 1024.0)
        << " cullDispatchesPerFrame=" << (static_cast<double>( m_GpuVobPerformanceStats.GpuCullDispatches )
            / measurementFrames)
        << " hizDispatchesPerFrame=" << (static_cast<double>( m_GpuVobPerformanceStats.HiZDispatches )
            / measurementFrames)
        << " cullPrepareMsPerFrame=" << (m_GpuVobPerformanceStats.GpuCullPrepareMsTotal
            / measurementFrames)
        << " cullPrepareMsMax=" << m_GpuVobPerformanceStats.GpuCullPrepareMsMax
        << " hizPrepareMsPerFrame=" << (m_GpuVobPerformanceStats.HiZPrepareMsTotal
            / measurementFrames)
        << " hizPrepareMsMax=" << m_GpuVobPerformanceStats.HiZPrepareMsMax
        << " gpuTiming=" << (m_GpuTimingAvailable ? 1 : 0)
        << " gpuTimingFrames=" << m_GpuVobPerformanceStats.GpuTimingFrames
        << " gpuTimingDroppedFrames=" << m_GpuVobPerformanceStats.GpuTimingDroppedFrames
        << " gpuTimingIntervalOverflow=" << m_GpuVobPerformanceStats.GpuTimingIntervalOverflow
        << " gpuHiZSamples=" << m_GpuVobPerformanceStats.GpuHiZTimingSamples
        << " gpuHiZMsAvg=" << (m_GpuVobPerformanceStats.GpuHiZTimingSamples > 0
            ? m_GpuVobPerformanceStats.GpuHiZMsTotal
                / static_cast<double>( m_GpuVobPerformanceStats.GpuHiZTimingSamples ) : 0.0)
        << " gpuMainCullSamples=" << m_GpuVobPerformanceStats.GpuMainCullTimingSamples
        << " gpuMainCullMsAvg=" << (m_GpuVobPerformanceStats.GpuMainCullTimingSamples > 0
            ? m_GpuVobPerformanceStats.GpuMainCullMsTotal
                / static_cast<double>( m_GpuVobPerformanceStats.GpuMainCullTimingSamples ) : 0.0)
        << " gpuVobDepthSamples=" << m_GpuVobPerformanceStats.GpuVobDepthTimingSamples
        << " gpuVobDepthMsAvg=" << (m_GpuVobPerformanceStats.GpuVobDepthTimingSamples > 0
            ? m_GpuVobPerformanceStats.GpuVobDepthMsTotal
                / static_cast<double>( m_GpuVobPerformanceStats.GpuVobDepthTimingSamples ) : 0.0)
        << " gpuVobLitSamples=" << m_GpuVobPerformanceStats.GpuVobLitTimingSamples
        << " gpuVobLitMsAvg=" << (m_GpuVobPerformanceStats.GpuVobLitTimingSamples > 0
            ? m_GpuVobPerformanceStats.GpuVobLitMsTotal
                / static_cast<double>( m_GpuVobPerformanceStats.GpuVobLitTimingSamples ) : 0.0);
}

bool D3D11GraphicsEngine::IsGpuVobRuntimeDisabled() const {
    if ( !Engine::GAPI ) {
        return false;
    }
    return m_GpuVobRuntimeDisabledSettingsKey == BuildGpuVobPerformanceSettingsKey()
        && m_GpuVobRuntimeDisabledWorldGeneration
            == Engine::GAPI->GetCityWindowConfigurationGeneration();
}

void D3D11GraphicsEngine::DisableGpuVobRuntime( const char* reason ) {
    if ( !Engine::GAPI ) {
        return;
    }

    const uint64_t settingsKey = BuildGpuVobPerformanceSettingsKey();
    const uint64_t worldGeneration = Engine::GAPI->GetCityWindowConfigurationGeneration();
    if ( m_GpuVobRuntimeDisabledSettingsKey == settingsKey
        && m_GpuVobRuntimeDisabledWorldGeneration == worldGeneration ) {
        return;
    }

    m_GpuVobRuntimeDisabledSettingsKey = settingsKey;
    m_GpuVobRuntimeDisabledWorldGeneration = worldGeneration;

    GpuVobCullInputBuffer.reset();
    GpuVobCullVisualBuffer.reset();
    GpuVobCullDrawVisualBuffer.reset();
    GpuVobCullOutputBuffer.reset();
    GpuVobCullArgsBuffer.reset();
    GpuVobCullVisibleCountsBuffer.reset();
    GpuVobCullConstantBuffer.reset();
    GpuVobPatchConstantBuffer.reset();
    m_GpuVobVisibleCountReadbackSlots.clear();
    m_GpuVobCurrentTriangleWeights.clear();

    GpuVobHiZTexture.Reset();
    GpuVobHiZShaderResourceView.Reset();
    GpuVobHiZMipShaderResourceViews.clear();
    GpuVobHiZMipUnorderedAccessViews.clear();
    GpuVobHiZWidth = 0;
    GpuVobHiZHeight = 0;
    GpuVobHiZMipCount = 0;

    LogError() << "[GpuVobPerf] disabled optional GPU VOB path for current world/settings: "
        << (reason ? reason : "unknown failure");
}

bool D3D11GraphicsEngine::IsGpuVobHiZRuntimeDisabled() const {
    if ( !Engine::GAPI ) {
        return false;
    }
    return m_GpuVobHiZDisabledSettingsKey == BuildGpuVobPerformanceSettingsKey()
        && m_GpuVobHiZDisabledWorldGeneration
            == Engine::GAPI->GetCityWindowConfigurationGeneration();
}

void D3D11GraphicsEngine::DisableGpuVobHiZ( const char* reason ) {
    if ( !Engine::GAPI ) {
        return;
    }

    const uint64_t settingsKey = BuildGpuVobPerformanceSettingsKey();
    const uint64_t worldGeneration = Engine::GAPI->GetCityWindowConfigurationGeneration();
    if ( m_GpuVobHiZDisabledSettingsKey == settingsKey
        && m_GpuVobHiZDisabledWorldGeneration == worldGeneration ) {
        return;
    }

    m_GpuVobHiZDisabledSettingsKey = settingsKey;
    m_GpuVobHiZDisabledWorldGeneration = worldGeneration;
    GpuVobHiZTexture.Reset();
    GpuVobHiZShaderResourceView.Reset();
    GpuVobHiZMipShaderResourceViews.clear();
    GpuVobHiZMipUnorderedAccessViews.clear();
    GpuVobHiZWidth = 0;
    GpuVobHiZHeight = 0;
    GpuVobHiZMipCount = 0;

    LogError() << "[GpuVobPerf] Hi-Z disabled for current world/settings; GPU frustum culling remains eligible: "
        << (reason ? reason : "unknown failure");
}

bool D3D11GraphicsEngine::IsGpuVobGeometryArenaRuntimeDisabled() const {
    if ( !Engine::GAPI ) {
        return false;
    }
    return m_GpuVobGeometryArenaDisabledSettingsKey == BuildGpuVobPerformanceSettingsKey()
        && m_GpuVobGeometryArenaDisabledWorldGeneration
            == Engine::GAPI->GetCityWindowConfigurationGeneration();
}

void D3D11GraphicsEngine::DisableGpuVobGeometryArena( const char* reason ) {
    if ( !Engine::GAPI ) {
        return;
    }

    const uint64_t settingsKey = BuildGpuVobPerformanceSettingsKey();
    const uint64_t worldGeneration = Engine::GAPI->GetCityWindowConfigurationGeneration();
    if ( m_GpuVobGeometryArenaDisabledSettingsKey == settingsKey
        && m_GpuVobGeometryArenaDisabledWorldGeneration == worldGeneration ) {
        return;
    }

    m_GpuVobGeometryArenaDisabledSettingsKey = settingsKey;
    m_GpuVobGeometryArenaDisabledWorldGeneration = worldGeneration;
    GpuVobGeometryVertexBuffer.reset();
    GpuVobGeometryIndexBuffer.reset();
    GpuVobGeometryRanges.clear();
    GpuVobGeometryMeshes.clear();
    GpuVobGeometryWorldGeneration = static_cast<uint64_t>( -1 );

    LogError() << "[GpuVobPerf] geometry arena unavailable; continuing with per-mesh GPU geometry: "
        << (reason ? reason : "unknown failure");
}

void D3D11GraphicsEngine::BuildGpuVobHiZ() {
    TracyD3D11ZoneCGX( "GpuVobHiZ" );
    if ( GpuVobHiZBuildAttemptedThisFrame ) {
        return;
    }
    GpuVobHiZBuiltThisFrame = false;
    const auto& settings = Engine::GAPI->GetRendererState().RendererSettings;
    if ( IsGpuVobRuntimeDisabled() || IsGpuVobHiZRuntimeDisabled()
        || !settings.GetEffectiveGpuVobOcclusionCulling() || FeatureLevel10Compatibility
        || !settings.DebugSettings.Culling.CullVobs || !Context || !ShaderManager ) {
        return;
    }
    ++m_GpuVobPerformanceStats.HiZRequests;
    const auto hizPrepareStart = std::chrono::steady_clock::now();
    GpuVobHiZBuildAttemptedThisFrame = true;

    const auto copyShader = ShaderManager->GetCShader( CShaderID::CS_VobHiZCopy );
    const auto reduceShader = ShaderManager->GetCShader( CShaderID::CS_VobHiZReduce );
    if ( !copyShader || !reduceShader ) {
        DisableGpuVobHiZ( "required Hi-Z shader unavailable" );
        return;
    }
    if ( !EnsureGpuVobHiZResources() ) {
        DisableGpuVobHiZ( "Hi-Z resource creation failed" );
        return;
    }

    BeginGpuTimingZone( EGpuTimingZone::HiZ );

    // Copying the DSV is already the renderer's established way of making
    // the reversed-Z depth SRV readable for compute passes.
    CopyDepthStencil();
    ID3D11ShaderResourceView* nullSrv = nullptr;
    ID3D11UnorderedAccessView* nullUav = nullptr;
    ID3D11ShaderResourceView* depthSrv = DepthStencilBufferCopy->GetShaderResView().Get();
    ID3D11UnorderedAccessView* firstUav = GpuVobHiZMipUnorderedAccessViews[0].Get();

    GetContext()->CSSetShaderResources( 0, 1, &depthSrv );
    GetContext()->CSSetUnorderedAccessViews( 0, 1, &firstUav, nullptr );
    copyShader->Apply();
    GetContext()->Dispatch( (GpuVobHiZWidth + 7u) / 8u,
        (GpuVobHiZHeight + 7u) / 8u, 1 );
    GetContext()->CSSetUnorderedAccessViews( 0, 1, &nullUav, nullptr );
    GetContext()->CSSetShaderResources( 0, 1, &nullSrv );

    for ( UINT mip = 1; mip < GpuVobHiZMipCount; ++mip ) {
        UINT mipWidth = std::max( 1u, GpuVobHiZWidth >> mip );
        UINT mipHeight = std::max( 1u, GpuVobHiZHeight >> mip );
        ID3D11ShaderResourceView* sourceSrv = GpuVobHiZMipShaderResourceViews[mip - 1].Get();
        ID3D11UnorderedAccessView* destinationUav = GpuVobHiZMipUnorderedAccessViews[mip].Get();
        GetContext()->CSSetShaderResources( 0, 1, &sourceSrv );
        GetContext()->CSSetUnorderedAccessViews( 0, 1, &destinationUav, nullptr );
        reduceShader->Apply();
        GetContext()->Dispatch( (mipWidth + 7u) / 8u,
            (mipHeight + 7u) / 8u, 1 );
        GetContext()->CSSetUnorderedAccessViews( 0, 1, &nullUav, nullptr );
        GetContext()->CSSetShaderResources( 0, 1, &nullSrv );
    }

    GetContext()->CSSetShader( nullptr, nullptr, 0 );
    EndGpuTimingZone( EGpuTimingZone::HiZ );
    GpuVobHiZBuiltThisFrame = true;
    ++m_GpuVobPerformanceStats.HiZBuilt;
    m_GpuVobPerformanceStats.HiZDispatches += GpuVobHiZMipCount;
    const double hizPrepareMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - hizPrepareStart ).count();
    m_GpuVobPerformanceStats.HiZPrepareMsTotal += hizPrepareMs;
    m_GpuVobPerformanceStats.HiZPrepareMsMax = std::max(
        m_GpuVobPerformanceStats.HiZPrepareMsMax, hizPrepareMs );
}

bool D3D11GraphicsEngine::PrepareGpuVobGeometryArena() {
    auto& cache = m_FrameGeometryCache;
    const auto& settings = Engine::GAPI->GetRendererState().RendererSettings;
    const uint64_t worldGeneration = Engine::GAPI->GetCityWindowConfigurationGeneration();

    // Morph-updated static VOBs change their vertex stream every frame. Keep
    // only those meshes on the existing per-mesh path instead of copying
    // stale geometry into a persistent arena; ordinary static meshes remain
    // eligible even when AnimateStaticVobs is enabled globally.
    if ( IsGpuVobRuntimeDisabled()
        || IsGpuVobGeometryArenaRuntimeDisabled()
        || !settings.GetEffectiveGpuVobOcclusionCulling()
        || FeatureLevel10Compatibility ) {
        return false;
    }

    if ( cache.gpuVobGeometryArenaPrepared
        && GpuVobGeometryWorldGeneration == worldGeneration
        && GpuVobGeometryVertexBuffer
        && GpuVobGeometryIndexBuffer ) {
        cache.MainVobGpuGeometryVertexBuffer = GpuVobGeometryVertexBuffer.get();
        cache.MainVobGpuGeometryIndexBuffer = GpuVobGeometryIndexBuffer.get();
        return true;
    }

    // Build the persistent arena from the registered static-VOB geometry, not
    // from this frame's visible subset. Otherwise a camera turn that adds or
    // removes one VOB mesh would force a full arena rebuild and CPU re-upload.
    // The current frame is still included as a fallback for meshes that are
    // being published while the world is loading asynchronously. Both normal
    // and welded shadow indices are kept because the CSM path consumes the
    // latter from its per-cascade indirect arguments.
    std::vector<MeshInfo*> meshes;
    meshes.reserve( cache.sortedInstancedMeshes.size() + 256 );
    std::unordered_set<MeshInfo*> morphMeshes;
    auto addMesh = [&meshes]( MeshInfo* mesh ) {
        if ( mesh && !mesh->Vertices.empty() && !mesh->Indices.empty() ) {
            meshes.push_back( mesh );
        }
    };
    for ( const auto& [_, visual] : Engine::GAPI->GetStaticMeshVisuals() ) {
        if ( !visual ) {
            continue;
        }
        const bool isAnimatedMorph = settings.AnimateStaticVobs
            && visual->MorphMeshVisual != nullptr;
        for ( const auto& [__, meshList] : visual->MeshesByTexture ) {
            for ( MeshInfo* mesh : meshList ) {
                if ( isAnimatedMorph ) {
                    if ( mesh ) {
                        morphMeshes.insert( mesh );
                    }
                } else {
                    addMesh( mesh );
                }
            }
        }
    }
    for ( const auto& drawItem : cache.sortedInstancedMeshes ) {
        if ( morphMeshes.find( drawItem.MeshEntry ) == morphMeshes.end() ) {
            addMesh( drawItem.MeshEntry );
        }
    }

    std::sort( meshes.begin(), meshes.end(), std::less<MeshInfo*>() );
    meshes.erase( std::unique( meshes.begin(), meshes.end() ), meshes.end() );
    if ( meshes.empty() ) {
        return false;
    }

    bool reusable = worldGeneration == GpuVobGeometryWorldGeneration
        && meshes == GpuVobGeometryMeshes
        && GpuVobGeometryVertexBuffer && GpuVobGeometryIndexBuffer
        && GpuVobGeometryVertexBuffer->GetVertexBuffer()
        && GpuVobGeometryIndexBuffer->GetVertexBuffer()
        && GpuVobGeometryRanges.size() == meshes.size();
    if ( reusable ) {
        for ( MeshInfo* mesh : meshes ) {
            const auto it = GpuVobGeometryRanges.find( mesh );
            if ( it == GpuVobGeometryRanges.end()
                || it->second.VertexCount != mesh->Vertices.size()
                || it->second.IndexCount != mesh->Indices.size()
                || it->second.ShadowIndexCount != (mesh->ShadowIndices.empty()
                    ? mesh->Indices.size() : mesh->ShadowIndices.size()) ) {
                reusable = false;
                break;
            }
        }
    }

    if ( !reusable ) {
        size_t vertexCount = 0;
        size_t indexCount = 0;
        for ( MeshInfo* mesh : meshes ) {
            vertexCount += mesh->Vertices.size();
            indexCount += mesh->Indices.size();
            indexCount += mesh->ShadowIndices.empty()
                ? mesh->Indices.size() : mesh->ShadowIndices.size();
        }
        if ( vertexCount == 0 || indexCount == 0
            || vertexCount > static_cast<size_t>( std::numeric_limits<INT>::max() )
            || vertexCount > std::numeric_limits<UINT>::max()
            || indexCount > std::numeric_limits<UINT>::max()
            || vertexCount > std::numeric_limits<UINT>::max() / sizeof( ExVertexStruct )
            || indexCount > std::numeric_limits<UINT>::max() / sizeof( VERTEX_INDEX ) ) {
            DisableGpuVobGeometryArena( "geometry arena exceeds D3D11 buffer limits" );
            return false;
        }

        std::vector<ExVertexStruct> vertices;
        std::vector<VERTEX_INDEX> indices;
        vertices.reserve( vertexCount );
        indices.reserve( indexCount );
        std::unordered_map<MeshInfo*, GpuVobGeometryRange> ranges;
        ranges.reserve( meshes.size() );

        UINT vertexOffset = 0;
        UINT indexOffset = 0;
        for ( MeshInfo* mesh : meshes ) {
            GpuVobGeometryRange range;
            range.StartIndexLocation = indexOffset;
            range.BaseVertexLocation = static_cast<INT>( vertexOffset );
            range.VertexCount = static_cast<UINT>( mesh->Vertices.size() );
            range.IndexCount = static_cast<UINT>( mesh->Indices.size() );
            range.ShadowStartIndexLocation = indexOffset + range.IndexCount;
            range.ShadowIndexCount = static_cast<UINT>( mesh->ShadowIndices.empty()
                ? mesh->Indices.size() : mesh->ShadowIndices.size() );
            ranges.emplace( mesh, range );

            vertices.insert( vertices.end(), mesh->Vertices.begin(), mesh->Vertices.end() );
            indices.insert( indices.end(), mesh->Indices.begin(), mesh->Indices.end() );
            if ( mesh->ShadowIndices.empty() ) {
                indices.insert( indices.end(), mesh->Indices.begin(), mesh->Indices.end() );
            } else {
                indices.insert( indices.end(), mesh->ShadowIndices.begin(), mesh->ShadowIndices.end() );
            }
            vertexOffset += static_cast<UINT>( mesh->Vertices.size() );
            indexOffset += range.IndexCount + range.ShadowIndexCount;
        }

        const UINT vertexBytes = static_cast<UINT>( vertices.size() * sizeof( ExVertexStruct ) );
        const UINT indexBytes = static_cast<UINT>( indices.size() * sizeof( VERTEX_INDEX ) );
        auto arenaVertices = std::make_unique<D3D11VertexBuffer>();
        if ( arenaVertices->Init( vertices.data(), vertexBytes,
                D3D11VertexBuffer::B_VERTEXBUFFER,
                D3D11VertexBuffer::U_DEFAULT,
                D3D11VertexBuffer::CA_NONE,
                "GpuVobGeometryVertexArena" ) != XR_SUCCESS
            || !arenaVertices->GetVertexBuffer() ) {
            LogError() << "[GpuVobPerf] vertex geometry arena creation failed: vertices="
                << vertices.size() << " bytes=" << vertexBytes;
            DisableGpuVobGeometryArena( "vertex arena CreateBuffer failed" );
            return false;
        }

        auto arenaIndices = std::make_unique<D3D11VertexBuffer>();
        if ( arenaIndices->Init( indices.data(), indexBytes,
                D3D11VertexBuffer::B_INDEXBUFFER,
                D3D11VertexBuffer::U_DEFAULT,
                D3D11VertexBuffer::CA_NONE,
                "GpuVobGeometryIndexArena" ) != XR_SUCCESS
            || !arenaIndices->GetVertexBuffer() ) {
            LogError() << "[GpuVobPerf] index geometry arena creation failed: indices="
                << indices.size() << " bytes=" << indexBytes;
            DisableGpuVobGeometryArena( "index arena CreateBuffer failed" );
            return false;
        }

        GpuVobGeometryVertexBuffer = std::move( arenaVertices );
        GpuVobGeometryIndexBuffer = std::move( arenaIndices );
        GpuVobGeometryRanges = std::move( ranges );
        GpuVobGeometryMeshes = std::move( meshes );
        GpuVobGeometryWorldGeneration = worldGeneration;
    }

    cache.MainVobGpuGeometryVertexBuffer = GpuVobGeometryVertexBuffer.get();
    cache.MainVobGpuGeometryIndexBuffer = GpuVobGeometryIndexBuffer.get();
    cache.gpuVobGeometryArenaPrepared = true;
    return true;
}

bool D3D11GraphicsEngine::PrepareGpuVobCulling(
    D3D11VertexBuffer* inputInstanceBuffer,
    const std::vector<GpuVobCullVisualBatch>& vobVisuals,
    const std::vector<FrameGeometryCache::CachedInstancedMeshDraw>& sortedInstancedMeshes,
    D3D11VertexBuffer* geometryVertexBuffer,
    D3D11VertexBuffer* geometryIndexBuffer,
    std::unique_ptr<D3D11VertexBuffer>& cullInputBuffer,
    std::unique_ptr<D3D11VertexBuffer>& cullVisualBuffer,
    std::unique_ptr<D3D11VertexBuffer>& cullDrawVisualBuffer,
    std::unique_ptr<D3D11VertexBuffer>& cullOutputBuffer,
    std::unique_ptr<D3D11IndirectBuffer>& cullArgsBuffer,
    std::unique_ptr<D3D11IndirectBuffer>& cullVisibleCountsBuffer,
    std::unique_ptr<D3D11ConstantBuffer>& cullConstantBuffer,
    std::unique_ptr<D3D11ConstantBuffer>& patchConstantBuffer,
    D3D11VertexBuffer*& outputInstanceBuffer,
    D3D11IndirectBuffer*& outputArgsBuffer,
    const XMMATRIX* cullViewProj,
    bool enableOcclusion ) {
    TracyD3D11ZoneCGX( "GpuVobOcclusion::Prepare" );
    outputInstanceBuffer = nullptr;
    outputArgsBuffer = nullptr;
    const auto& settings = Engine::GAPI->GetRendererState().RendererSettings;
    if ( IsGpuVobRuntimeDisabled()
        || !settings.GetEffectiveGpuVobOcclusionCulling() || FeatureLevel10Compatibility
        || !settings.DebugSettings.Culling.CullVobs
        || !ShaderManager
        || !inputInstanceBuffer
        || (enableOcclusion && (!GpuVobHiZBuiltThisFrame || !GpuVobHiZShaderResourceView))
        || vobVisuals.empty()
        || sortedInstancedMeshes.empty()
        || vobVisuals.size() > 65535 ) {
        return false;
    }

    const auto failGpuVobPath = [this, enableOcclusion]( const char* reason ) {
        if ( enableOcclusion ) {
            DisableGpuVobRuntime( reason );
        } else {
            LogError() << "[GpuVobPerf] shadow VOB GPU culling fallback: " << reason;
        }
        return false;
    };

    const auto cullShader = ShaderManager->GetCShader( CShaderID::CS_VobCull );
    const auto patchShader = ShaderManager->GetCShader( CShaderID::CS_VobPatchArgs );
    if ( !cullShader || !patchShader ) {
        return failGpuVobPath( "required culling shader unavailable" );
    }
    const auto cullPrepareStart = std::chrono::steady_clock::now();

    std::vector<GpuVobCullVisualInfo> visualInfo( vobVisuals.size() );
    for ( size_t visualIndex = 0; visualIndex < vobVisuals.size(); ++visualIndex ) {
        const auto& cachedVisual = vobVisuals[visualIndex];
        if ( !cachedVisual.Visual ) {
            return failGpuVobPath( "invalid VOB visual metadata" );
        }

        visualInfo[visualIndex].BBoxMin = XMFLOAT3(
            cachedVisual.Visual->BBox.Min.x,
            cachedVisual.Visual->BBox.Min.y,
            cachedVisual.Visual->BBox.Min.z );
        visualInfo[visualIndex].BBoxMax = XMFLOAT3(
            cachedVisual.Visual->BBox.Max.x,
            cachedVisual.Visual->BBox.Max.y,
            cachedVisual.Visual->BBox.Max.z );
        visualInfo[visualIndex].InstanceBase = cachedVisual.StartInstanceNum;
        visualInfo[visualIndex].InstanceCount = cachedVisual.InstanceCount;
    }

    std::vector<UINT> drawVisualIndices( sortedInstancedMeshes.size() );
    std::vector<D3D11_DRAW_INDEXED_INSTANCED_INDIRECT_ARGS> drawArgs(
        sortedInstancedMeshes.size() );
    const UINT maxIndices = settings.MaxNumFaces > 0
        ? static_cast<UINT>( settings.MaxNumFaces ) * 3u : 0u;
    for ( size_t drawIndex = 0; drawIndex < sortedInstancedMeshes.size(); ++drawIndex ) {
        const auto& drawItem = sortedInstancedMeshes[drawIndex];
        if ( drawItem.VisualIndex >= visualInfo.size() || !drawItem.MeshEntry ) {
            return failGpuVobPath( "invalid VOB draw metadata" );
        }

        const auto geometryRange = GpuVobGeometryRanges.find( drawItem.MeshEntry );
        const bool useGeometryArena = geometryVertexBuffer
            && geometryIndexBuffer
            && !drawItem.UseShadowLodIndex
            && geometryRange != GpuVobGeometryRanges.end();
        const UINT meshIndexCount = useGeometryArena
            ? (drawItem.UseShadowIndex
                ? geometryRange->second.ShadowIndexCount
                : geometryRange->second.IndexCount)
            : (drawItem.UseShadowLodIndex
                ? static_cast<UINT>( drawItem.MeshEntry->ShadowLodIndices.size() )
                : drawItem.UseShadowIndex
                ? static_cast<UINT>( drawItem.MeshEntry->ShadowIndices.empty()
                    ? drawItem.MeshEntry->Indices.size()
                    : drawItem.MeshEntry->ShadowIndices.size() )
                : static_cast<UINT>( drawItem.MeshEntry->Indices.size() ));
        drawVisualIndices[drawIndex] = drawItem.VisualIndex;
        drawArgs[drawIndex].IndexCountPerInstance = maxIndices != 0
            ? std::min( meshIndexCount, maxIndices ) : meshIndexCount;
        drawArgs[drawIndex].InstanceCount = visualInfo[drawItem.VisualIndex].InstanceCount;
        drawArgs[drawIndex].StartIndexLocation = useGeometryArena
            ? (drawItem.UseShadowIndex
                ? geometryRange->second.ShadowStartIndexLocation
                : geometryRange->second.StartIndexLocation) : 0;
        drawArgs[drawIndex].BaseVertexLocation = useGeometryArena
            ? geometryRange->second.BaseVertexLocation : 0;
        drawArgs[drawIndex].StartInstanceLocation =
            visualInfo[drawItem.VisualIndex].InstanceBase;
    }

    const UINT visualBytes = static_cast<UINT>(
        visualInfo.size() * sizeof( GpuVobCullVisualInfo ) );
    const UINT drawVisualBytes = static_cast<UINT>(
        drawVisualIndices.size() * sizeof( UINT ) );
    const UINT instanceBytes = static_cast<UINT>( inputInstanceBuffer->GetSizeInBytes() );
    const UINT argsBytes = static_cast<UINT>(
        drawArgs.size() * sizeof( D3D11_DRAW_INDEXED_INSTANCED_INDIRECT_ARGS ) );
    const UINT visibleCountBytes = static_cast<UINT>( visualInfo.size() * sizeof( UINT ) );

    // The CPU-side instancing buffer is deliberately a dynamic vertex buffer.
    // D3D11 forbids combining CPU access flags with structured/raw misc flags,
    // so GPU culling receives a separate DEFAULT structured copy. The source
    // buffer is a retained-capacity pool allocation; only the contiguous range
    // occupied by the current visual batches is needed by CSCull.
    const UINT inputCapacityBytes = static_cast<UINT>( inputInstanceBuffer->GetSizeInBytes() );
    uint64_t activeInstanceCount = 0;
    for ( const auto& visual : visualInfo ) {
        activeInstanceCount = std::max<uint64_t>( activeInstanceCount,
            static_cast<uint64_t>( visual.InstanceBase ) + visual.InstanceCount );
    }
    if ( inputCapacityBytes == 0 || activeInstanceCount == 0
        || activeInstanceCount > std::numeric_limits<UINT>::max() / sizeof( VobInstanceInfo ) ) {
        return failGpuVobPath( "input instance buffer range is invalid" );
    }
    const UINT activeInputBytes = static_cast<UINT>( activeInstanceCount
        * sizeof( VobInstanceInfo ) );
    if ( inputCapacityBytes % sizeof( VobInstanceInfo ) != 0
        || activeInputBytes > inputCapacityBytes ) {
        return failGpuVobPath( "input instance buffer range is not stride-aligned" );
    }
    if ( !inputInstanceBuffer->GetVertexBuffer().Get() ) {
        return failGpuVobPath( "input instance buffer has no D3D11 resource" );
    }

    if ( !cullInputBuffer
        || cullInputBuffer->GetSizeInBytes() < inputCapacityBytes
        || !cullInputBuffer->GetShaderResourceView().Get() ) {
        cullInputBuffer = std::make_unique<D3D11VertexBuffer>();
        if ( cullInputBuffer->Init( nullptr, inputCapacityBytes,
            D3D11VertexBuffer::B_SHADER_RESOURCE,
            D3D11VertexBuffer::U_DEFAULT,
            D3D11VertexBuffer::CA_NONE,
            "GpuVobCullInput", sizeof( VobInstanceInfo ) ) != XR_SUCCESS ) {
            cullInputBuffer.reset();
            return failGpuVobPath( "GPU culling input buffer creation failed" );
        }
    }

    // Buffers use byte coordinates for CopySubresourceRegion. This avoids
    // copying unused tail capacity from the frame pool every frame while
    // preserving the D3D11 structured-buffer layout required by the SRV.
    D3D11_BOX activeInstanceRegion = { 0, 0, 0, activeInputBytes, 1, 1 };
    GetContext()->CopySubresourceRegion(
        cullInputBuffer->GetVertexBuffer().Get(), 0, 0, 0, 0,
        inputInstanceBuffer->GetVertexBuffer().Get(), 0, &activeInstanceRegion );

    auto ensureStructuredBuffer = []( std::unique_ptr<D3D11VertexBuffer>& buffer,
        UINT sizeInBytes, UINT stride, const char* debugName ) -> bool {
        if ( buffer && buffer->GetSizeInBytes() >= sizeInBytes
            && buffer->GetShaderResourceView().Get() ) {
            return true;
        }
        buffer = std::make_unique<D3D11VertexBuffer>();
        return buffer->Init( nullptr, sizeInBytes,
            D3D11VertexBuffer::B_SHADER_RESOURCE,
            D3D11VertexBuffer::U_DEFAULT,
            D3D11VertexBuffer::CA_NONE,
            debugName ? debugName : "GpuVobStructuredBuffer", stride ) == XR_SUCCESS;
    };

    if ( !ensureStructuredBuffer( cullVisualBuffer,
        std::max( visualBytes, 32u ), sizeof( GpuVobCullVisualInfo ),
        "GpuVobCullVisuals" )
        || !ensureStructuredBuffer( cullDrawVisualBuffer,
            std::max( drawVisualBytes, 4u ), sizeof( UINT ),
            "GpuVobCullDrawVisuals" ) ) {
        return failGpuVobPath( "GPU culling metadata buffer creation failed" );
    }

    if ( !cullOutputBuffer
        || cullOutputBuffer->GetSizeInBytes() < instanceBytes
        || !cullOutputBuffer->GetUnorderedAccessView().Get() ) {
        cullOutputBuffer = std::make_unique<D3D11VertexBuffer>();
        const auto outputFlags = static_cast<D3D11VertexBuffer::EBindFlags>(
            D3D11VertexBuffer::B_VERTEXBUFFER
            | D3D11VertexBuffer::B_UNORDERED_ACCESS );
        if ( cullOutputBuffer->Init( nullptr, instanceBytes, outputFlags,
            D3D11VertexBuffer::U_DEFAULT,
            D3D11VertexBuffer::CA_NONE,
            "GpuVobCullOutput", sizeof( UINT ) ) != XR_SUCCESS ) {
            return failGpuVobPath( "GPU culling output buffer creation failed" );
        }
    }

    if ( !cullVisibleCountsBuffer
        || cullVisibleCountsBuffer->GetSizeInBytes() < visibleCountBytes
        || !cullVisibleCountsBuffer->GetUnorderedAccessView().Get()
        || !cullVisibleCountsBuffer->GetShaderResourceView().Get() ) {
        cullVisibleCountsBuffer = std::make_unique<D3D11IndirectBuffer>();
        const auto visibleCountFlags = static_cast<D3D11IndirectBuffer::EBindFlags>(
            D3D11IndirectBuffer::B_SHADER_RESOURCE
            | D3D11IndirectBuffer::B_UNORDERED_ACCESS );
        if ( cullVisibleCountsBuffer->Init(
            nullptr, std::max( visibleCountBytes, 4u ), visibleCountFlags,
            D3D11IndirectBuffer::U_DEFAULT,
            D3D11IndirectBuffer::CA_NONE,
            "GpuVobCullVisibleCounts" ) != XR_SUCCESS ) {
            return failGpuVobPath( "GPU culling visible-count buffer creation failed" );
        }
    }

    if ( !cullArgsBuffer
        || cullArgsBuffer->GetSizeInBytes() < argsBytes
        || !cullArgsBuffer->GetUnorderedAccessView().Get() ) {
        cullArgsBuffer = std::make_unique<D3D11IndirectBuffer>();
        if ( cullArgsBuffer->Init( nullptr, argsBytes,
            D3D11IndirectBuffer::B_UNORDERED_ACCESS,
            D3D11IndirectBuffer::U_DEFAULT,
            D3D11IndirectBuffer::CA_NONE,
            "GpuVobCullIndirectArgs" ) != XR_SUCCESS ) {
            return failGpuVobPath( "GPU culling indirect-args buffer creation failed" );
        }
    }

    if ( !cullConstantBuffer ) {
        cullConstantBuffer = std::make_unique<D3D11ConstantBuffer>(
            sizeof( GpuVobCullConstantsData ), nullptr );
    }
    if ( !patchConstantBuffer ) {
        patchConstantBuffer = std::make_unique<D3D11ConstantBuffer>(
            sizeof( GpuVobPatchConstantsData ), nullptr );
    }

    if ( cullVisualBuffer->UpdateBufferSubresource( visualInfo.data(), visualBytes ) != XR_SUCCESS
        || cullDrawVisualBuffer->UpdateBufferSubresource(
            drawVisualIndices.data(), drawVisualBytes ) != XR_SUCCESS ) {
        return failGpuVobPath( "GPU culling metadata upload failed" );
    }

    // The argument buffer is DEFAULT-usage because it must be UAV-writable.
    // Update only the active argument range when the retained capacity is larger.
    if ( cullArgsBuffer->UpdateBufferSubresource( drawArgs.data(), argsBytes ) != XR_SUCCESS ) {
        return failGpuVobPath( "GPU culling indirect-args upload failed" );
    }

    if ( enableOcclusion ) {
        m_GpuVobPerformanceStats.GpuCullInputCopyBytes += activeInputBytes;
        m_GpuVobPerformanceStats.GpuCullMetadataUploadBytes +=
            static_cast<uint64_t>( visualBytes ) + drawVisualBytes;
        m_GpuVobPerformanceStats.GpuCullArgsUploadBytes += argsBytes;
    }

    ID3D11ShaderResourceView* cullSrvs[3] = {
        cullInputBuffer->GetShaderResourceView().Get(),
        cullVisualBuffer->GetShaderResourceView().Get(),
        enableOcclusion ? GpuVobHiZShaderResourceView.Get() : nullptr,
    };
    ID3D11UnorderedAccessView* cullUavs[2] = {
        cullOutputBuffer->GetUnorderedAccessView().Get(),
        cullVisibleCountsBuffer->GetUnorderedAccessView().Get(),
    };
    if ( !cullSrvs[0] || !cullSrvs[1] || !cullUavs[0] || !cullUavs[1] ) {
        return failGpuVobPath( "GPU culling SRV/UAV binding unavailable" );
    }

    ID3D11ShaderResourceView* nullSrvs[3] = {};
    ID3D11UnorderedAccessView* nullUavs[2] = {};
    ID3D11Buffer* nullConstantBuffers[2] = {};
    auto clearComputeBindings = [&]() {
        GetContext()->CSSetUnorderedAccessViews( 0, 2, nullUavs, nullptr );
        GetContext()->CSSetShaderResources( 0, 3, nullSrvs );
        GetContext()->CSSetShader( nullptr, nullptr, 0 );
        GetContext()->CSSetConstantBuffers( 0, 2, nullConstantBuffers );
    };

    GpuVobCullConstantsData cullConstants = {};
    // Keep the exact multiplication order used by SetupVS_ExConstantBuffer
    // and the DX12 reference: the shader consumes row-vector positions.
    const XMMATRIX viewProj = cullViewProj
        ? *cullViewProj
        : XMMatrixMultiply(
            XMLoadFloat4x4( &Engine::GAPI->GetProjectionMatrix() ),
            Engine::GAPI->GetViewMatrixXM() );
    XMStoreFloat4x4( &cullConstants.CullViewProj, viewProj );
    cullConstants.VisualCount = static_cast<UINT>( visualInfo.size() );
    cullConstants.HiZWidth = enableOcclusion ? GpuVobHiZWidth : 0u;
    cullConstants.HiZHeight = enableOcclusion ? GpuVobHiZHeight : 0u;
    cullConstants.HiZMipCount = enableOcclusion ? GpuVobHiZMipCount : 0u;
    cullConstants.EnableOcclusion = enableOcclusion ? 1u : 0u;
    cullConstantBuffer->UpdateBuffer( &cullConstants )
        ->BindToComputeShader( 0 );

    GetContext()->CSSetShaderResources( 0, 3, cullSrvs );
    GetContext()->CSSetUnorderedAccessViews( 0, 2, cullUavs, nullptr );
    const EGpuTimingZone cullTimingZone = EGpuTimingZone::MainCull;
    if ( enableOcclusion ) {
        BeginGpuTimingZone( cullTimingZone );
    }
    cullShader->Apply();
    GetContext()->Dispatch( static_cast<UINT>( visualInfo.size() ), 1, 1 );
    if ( enableOcclusion ) {
        ++m_GpuVobPerformanceStats.GpuCullDispatches;
    }

    GetContext()->CSSetUnorderedAccessViews( 0, 2, nullUavs, nullptr );
    GetContext()->CSSetShaderResources( 0, 3, nullSrvs );

    GpuVobPatchConstantsData patchConstants = {};
    patchConstants.DrawCount = static_cast<UINT>( drawVisualIndices.size() );
    patchConstantBuffer->UpdateBuffer( &patchConstants )
        ->BindToComputeShader( 1 );

    ID3D11ShaderResourceView* patchSrvs[3] = {
        cullVisibleCountsBuffer->GetShaderResourceView().Get(),
        cullDrawVisualBuffer->GetShaderResourceView().Get(),
        cullVisualBuffer->GetShaderResourceView().Get(),
    };
    ID3D11UnorderedAccessView* argsUav =
        cullArgsBuffer->GetUnorderedAccessView().Get();
    if ( !patchSrvs[0] || !patchSrvs[1] || !patchSrvs[2] || !argsUav ) {
        if ( enableOcclusion ) {
            EndGpuTimingZone( cullTimingZone );
        }
        clearComputeBindings();
        return failGpuVobPath( "GPU indirect-args patch binding unavailable" );
    }

    GetContext()->CSSetShaderResources( 0, 3, patchSrvs );
    GetContext()->CSSetUnorderedAccessViews( 0, 1, &argsUav, nullptr );
    patchShader->Apply();
    GetContext()->Dispatch(
        (static_cast<UINT>( drawVisualIndices.size() ) + 63u) / 64u, 1, 1 );
    if ( enableOcclusion ) {
        ++m_GpuVobPerformanceStats.GpuCullDispatches;
    }
    if ( enableOcclusion ) {
        EndGpuTimingZone( cullTimingZone );
    }

    clearComputeBindings();

    outputInstanceBuffer = cullOutputBuffer.get();
    outputArgsBuffer = cullArgsBuffer.get();
    const double cullPrepareMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - cullPrepareStart ).count();
    if ( enableOcclusion ) {
        m_GpuVobPerformanceStats.GpuCullPrepareMsTotal += cullPrepareMs;
        m_GpuVobPerformanceStats.GpuCullPrepareMsMax = std::max(
            m_GpuVobPerformanceStats.GpuCullPrepareMsMax, cullPrepareMs );
    }
    return true;
}

bool D3D11GraphicsEngine::PrepareGpuVobCulling() {
    auto& cache = m_FrameGeometryCache;
    std::vector<GpuVobCullVisualBatch> visualBatches;
    visualBatches.reserve( cache.vobVisuals.size() );
    for ( const auto& cachedVisual : cache.vobVisuals ) {
        GpuVobCullVisualBatch batch;
        batch.Visual = cachedVisual.Visual;
        batch.StartInstanceNum = cachedVisual.StartInstanceNum;
        batch.InstanceCount = static_cast<unsigned int>( cachedVisual.Instances.size() );
        visualBatches.push_back( batch );
    }
    return PrepareGpuVobCulling(
        cache.MainVobInstancingBuffer,
        visualBatches,
        cache.sortedInstancedMeshes,
        cache.MainVobGpuGeometryVertexBuffer,
        cache.MainVobGpuGeometryIndexBuffer,
        GpuVobCullInputBuffer,
        GpuVobCullVisualBuffer,
        GpuVobCullDrawVisualBuffer,
        GpuVobCullOutputBuffer,
        GpuVobCullArgsBuffer,
        GpuVobCullVisibleCountsBuffer,
        GpuVobCullConstantBuffer,
        GpuVobPatchConstantBuffer,
        cache.MainVobGpuInstanceBuffer,
        cache.MainVobGpuIndirectArgsBuffer );
}

void D3D11GraphicsEngine::PollGpuVobVisibleCountReadback(
    uint64_t settingsKey, uint64_t worldGeneration ) {
    for ( auto& slot : m_GpuVobVisibleCountReadbackSlots ) {
        if ( !slot.Pending || !slot.Buffer || !slot.Buffer->GetVertexBuffer() ) {
            continue;
        }

        if ( slot.SettingsKey != settingsKey || slot.WorldGeneration != worldGeneration ) {
            // The copy belongs to a different measurement configuration. It
            // is safe to discard it; any new copy is queued after this one on
            // the same immediate context.
            slot.Pending = false;
            slot.CandidateInstanceCounts.clear();
            slot.TriangleWeights.clear();
            continue;
        }

        D3D11_MAPPED_SUBRESOURCE mapped = {};
        const HRESULT mapResult = GetContext()->Map( slot.Buffer->GetVertexBuffer().Get(),
            0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped );
        if ( mapResult == DXGI_ERROR_WAS_STILL_DRAWING ) {
            continue;
        }
        if ( FAILED( mapResult ) ) {
            slot.Pending = false;
            slot.CandidateInstanceCounts.clear();
            slot.TriangleWeights.clear();
            continue;
        }

        const auto* visibleCounts = reinterpret_cast<const UINT*>( mapped.pData );
        const size_t visibleCountCount = std::min<size_t>(
            slot.CandidateInstanceCounts.size(), slot.Bytes / sizeof( UINT ) );
        uint64_t candidateInstances = 0;
        uint64_t visibleInstances = 0;
        for ( size_t visualIndex = 0; visualIndex < visibleCountCount; ++visualIndex ) {
            candidateInstances += slot.CandidateInstanceCounts[visualIndex];
            visibleInstances += visibleCounts[visualIndex];
        }

        uint64_t candidateTriangles = 0;
        uint64_t visibleTriangles = 0;
        for ( const auto& weight : slot.TriangleWeights ) {
            if ( weight.VisualIndex < visibleCountCount ) {
                candidateTriangles += static_cast<uint64_t>(
                    slot.CandidateInstanceCounts[weight.VisualIndex] )
                    * weight.TrianglesPerInstance;
                visibleTriangles += static_cast<uint64_t>( visibleCounts[weight.VisualIndex] )
                    * weight.TrianglesPerInstance;
            }
        }

        GetContext()->Unmap( slot.Buffer->GetVertexBuffer().Get(), 0 );
        slot.Pending = false;
        slot.CandidateInstanceCounts.clear();
        slot.TriangleWeights.clear();

        ++m_GpuVobPerformanceStats.GpuVisibleCountSamples;
        m_GpuVobPerformanceStats.GpuVisibleCandidateInstances += candidateInstances;
        m_GpuVobPerformanceStats.GpuVisibleInstances += visibleInstances;
        m_GpuVobPerformanceStats.GpuVisibleOccludedInstances +=
            candidateInstances > visibleInstances ? candidateInstances - visibleInstances : 0;
        m_GpuVobPerformanceStats.GpuVisibleTriangles += visibleTriangles;
        m_LastGpuMainVisibleTrianglesEstimate = visibleTriangles;
        m_HasLastGpuMainVisibleTrianglesEstimate = true;
        if ( candidateTriangles > 0 ) {
            m_LastGpuMainVisibilityRatio = static_cast<double>( visibleTriangles )
                / static_cast<double>( candidateTriangles );
            m_HasLastGpuMainVisibilityRatio = true;
        }
    }
}

void D3D11GraphicsEngine::ScheduleGpuVobVisibleCountReadback(
    const FrameGeometryCache& cache ) {
    // The counter is diagnostic telemetry only. Sampling every eighth frame
    // keeps the CopyResource/readback path from becoming part of the FPS A/B
    // result while still providing enough samples for a 120-frame window.
    constexpr uint64_t ReadbackInterval = 8;
    if ( m_GpuVobPerformanceStats.Frames == 0
        || (m_GpuVobPerformanceStats.Frames % ReadbackInterval) != 0 ) {
        m_GpuVobCurrentTriangleWeights.clear();
        return;
    }

    if ( !cache.vobGpuCullingActive || !GpuVobCullVisibleCountsBuffer
        || !GpuVobCullVisibleCountsBuffer->GetIndirectBuffer()
        || m_GpuVobCurrentTriangleWeights.empty() ) {
        m_GpuVobCurrentTriangleWeights.clear();
        return;
    }

    if ( m_GpuVobVisibleCountReadbackSlots.empty() ) {
        // Three slots keep the telemetry asynchronous without forcing the
        // render thread to wait for the GPU merely to read a counter buffer.
        m_GpuVobVisibleCountReadbackSlots.resize( 3 );
    }

    GpuVobVisibleCountReadbackSlot* freeSlot = nullptr;
    for ( auto& slot : m_GpuVobVisibleCountReadbackSlots ) {
        if ( !slot.Pending ) {
            freeSlot = &slot;
            break;
        }
    }
    if ( !freeSlot ) {
        m_GpuVobCurrentTriangleWeights.clear();
        return;
    }

    const UINT requiredBytes = std::max<UINT>(
        4u, GpuVobCullVisibleCountsBuffer->GetSizeInBytes() );
    if ( !freeSlot->Buffer || freeSlot->Bytes < requiredBytes ) {
        auto readback = std::make_unique<D3D11VertexBuffer>();
        if ( readback->Init( nullptr, requiredBytes,
            static_cast<D3D11VertexBuffer::EBindFlags>( 0 ),
            D3D11VertexBuffer::U_STAGING,
            D3D11VertexBuffer::CA_READ,
            "GpuVobCullVisibleCountsReadback" ) != XR_SUCCESS ) {
            m_GpuVobCurrentTriangleWeights.clear();
            return;
        }
        freeSlot->Buffer = std::move( readback );
        freeSlot->Bytes = requiredBytes;
    }

    GetContext()->CopyResource( freeSlot->Buffer->GetVertexBuffer().Get(),
        GpuVobCullVisibleCountsBuffer->GetIndirectBuffer().Get() );
    freeSlot->SettingsKey = m_GpuVobPerformanceSettingsKey;
    freeSlot->WorldGeneration = m_GpuVobPerformanceWorldGeneration;
    freeSlot->CandidateInstanceCounts.clear();
    freeSlot->CandidateInstanceCounts.reserve( cache.vobVisuals.size() );
    for ( const auto& visual : cache.vobVisuals ) {
        freeSlot->CandidateInstanceCounts.push_back(
            static_cast<UINT>( visual.Instances.size() ) );
    }
    freeSlot->TriangleWeights = std::move( m_GpuVobCurrentTriangleWeights );
    freeSlot->Pending = true;
}

void D3D11GraphicsEngine::RebuildWindowCutoutVolumeCache() {
    // Gothic mods occasionally contain invalid or enormously padded visual
    // bounding boxes. Never allow one of those to turn into a world-sized
    // pixel-shader discard volume.
    constexpr float MinWindowExtent = 0.05f;
    constexpr float MaxWindowExtent = 500.0f;
    constexpr float MaxWindowDepthExtent = 150.0f;
    constexpr float WallReachPadding = 100.0f;

    WindowCutoutVolumeCache.clear();
    WindowCutoutVolumeCache.reserve( 64 );
    for ( const auto& [visual, registeredVobs] : Engine::GAPI->GetVobsByVisual() ) {
        (void)visual;
        for ( BaseVobInfo* baseVob : registeredVobs ) {
            auto* vobInfo = dynamic_cast<VobInfo*>( baseVob );
            auto* visualInfo = vobInfo
                ? dynamic_cast<MeshVisualInfo*>( vobInfo->VisualInfo )
                : nullptr;
            if ( !vobInfo || !vobInfo->Vob || !vobInfo->VobSection || !visualInfo
                || !IsWindowGlassVisual( visualInfo->VisualName ) ) {
                continue;
            }

            // These four window VOB types are static world geometry. Cache
            // their transformed glass OBB once per world; final visibility and
            // camera-distance decisions remain dynamic below.
            const XMMATRIX world = XMMatrixTranspose( vobInfo->Vob->GetWorldMatrixXM() );
            XMVECTOR localMin;
            XMVECTOR localMax;
            if ( !GetWindowGlassLocalBounds( visualInfo, localMin, localMax ) ) {
                continue;
            }
            const XMVECTOR localCenter = (localMin + localMax) * 0.5f;
            const XMVECTOR localExtents = (localMax - localMin) * 0.5f;

            XMVECTOR axisXScaled = XMVector3TransformNormal( XMVectorSet( 1, 0, 0, 0 ), world );
            XMVECTOR axisYScaled = XMVector3TransformNormal( XMVectorSet( 0, 1, 0, 0 ), world );
            XMVECTOR axisZScaled = XMVector3TransformNormal( XMVectorSet( 0, 0, 1, 0 ), world );
            const float scaleX = XMVectorGetX( XMVector3Length( axisXScaled ) );
            const float scaleY = XMVectorGetX( XMVector3Length( axisYScaled ) );
            const float scaleZ = XMVectorGetX( XMVector3Length( axisZScaled ) );
            if ( scaleX <= 0.0001f || scaleY <= 0.0001f || scaleZ <= 0.0001f ) {
                continue;
            }

            const XMVECTOR axisX = XMVector3Normalize( axisXScaled );
            const XMVECTOR axisY = XMVector3Normalize( axisYScaled );
            const XMVECTOR axisZ = XMVector3Normalize( axisZScaled );
            float extents[3] = {
                XMVectorGetX( localExtents ) * scaleX,
                XMVectorGetY( localExtents ) * scaleY,
                XMVectorGetZ( localExtents ) * scaleZ,
            };
            const size_t thinAxis = extents[0] <= extents[1]
                ? (extents[0] <= extents[2] ? 0u : 2u)
                : (extents[1] <= extents[2] ? 1u : 2u);
            const XMVECTOR center = XMVector3TransformCoord( localCenter, world );
            XMFLOAT3 centerValues;
            XMStoreFloat3( &centerValues, center );
            if ( !std::isfinite( centerValues.x ) || !std::isfinite( centerValues.y )
                || !std::isfinite( centerValues.z )
                || !std::isfinite( extents[0] ) || !std::isfinite( extents[1] )
                || !std::isfinite( extents[2] )
                || extents[0] < 0.0f || extents[1] < 0.0f || extents[2] < 0.0f
                || extents[0] > MaxWindowExtent || extents[1] > MaxWindowExtent
                || extents[2] > MaxWindowExtent ) {
                continue;
            }
            for ( size_t axis = 0; axis < 3; ++axis ) {
                if ( axis != thinAxis && extents[axis] < MinWindowExtent ) {
                    extents[axis] = 0.0f;
                }
            }
            if ( extents[(thinAxis + 1) % 3] == 0.0f
                || extents[(thinAxis + 2) % 3] == 0.0f
                || extents[thinAxis] > MaxWindowDepthExtent ) {
                continue;
            }
            extents[thinAxis] += WallReachPadding;

            WindowCutoutVolume volume = {};
            XMStoreFloat4( &volume.CenterExtentX, XMVectorSetW( center, extents[0] ) );
            XMStoreFloat4( &volume.AxisXExtentY, XMVectorSetW( axisX, extents[1] ) );
            XMStoreFloat4( &volume.AxisYExtentZ, XMVectorSetW( axisY, extents[2] ) );
            XMStoreFloat4( &volume.AxisZPadding, axisZ );
            WindowCutoutVolumeCache.push_back( { vobInfo, volume } );
        }
    }
}

unsigned int D3D11GraphicsEngine::UpdateAndBindWindowCutouts( bool daylightPass ) {
    constexpr size_t MaxWindowCutouts = 32;
    constexpr float MaxWindowDistance = 12000.0f;

    // Loading and menu frames may render after Gothic has published a BSP
    // pointer but before BuildBspVobMapCache has completed. Never inspect or
    // retain window VOB pointers during that interval.
    if ( !Engine::GAPI || !Engine::GAPI->IsWorldRenderCacheReady() ) {
        UnbindWindowCutouts();
        return 0;
    }

    const uint64_t windowGeneration = Engine::GAPI->GetCityWindowConfigurationGeneration();
    if ( WindowCutoutCacheGeneration != windowGeneration ) {
        RebuildWindowCutoutVolumeCache();
        WindowCutoutCacheGeneration = windowGeneration;
    }
    if ( !daylightPass && !WindowCutoutVolumeCache.empty() )
        EnsureFrameVobVisibilityCollected();

    struct Candidate {
        float DistanceSq;
        WindowCutoutVolume Volume;
    };
    static thread_local std::vector<Candidate> candidates;
    candidates.clear();
    candidates.reserve( std::min( WindowCutoutVolumeCache.size(), MaxWindowCutouts + 1 ) );
    const XMVECTOR cameraPosition = Engine::GAPI->GetCameraPositionXM();

    for ( const CachedWindowCutoutVolume& cached : WindowCutoutVolumeCache ) {
        VobInfo* vobInfo = cached.Vob;
        if ( !vobInfo || !vobInfo->Vob
            || !vobInfo->Vob->GetShowVisual()
            || (vobInfo->CityWindowValidationInitialized
                && !vobInfo->CityWindowTransparencyValid) ) {
            continue;
        }

        if ( !daylightPass && !std::binary_search(
            m_FrameGeometryCache.visibleWindowVobs.begin(),
            m_FrameGeometryCache.visibleWindowVobs.end(), vobInfo,
            std::less<VobInfo*>{} ) ) {
            continue;
        }

        // The pane itself deliberately remains visible up to ten degrees into
        // its back side. Its world cutout must not share that tolerance: as
        // soon as the camera crosses the pane plane, stop removing world
        // geometry behind it. The unnormalised dot product is sufficient for
        // this exact 0-degree test and adds no square root per window.
        if ( vobInfo->CityWindowFacingInitialized ) {
            const XMVECTOR toCamera = cameraPosition
                - XMLoadFloat3( &vobInfo->CityWindowCenter );
            const float cutoutFacing = XMVectorGetX( XMVector3Dot(
                toCamera, XMLoadFloat3( &vobInfo->CityWindowFrontNormal ) ) );
            if ( cutoutFacing < 0.0f ) {
                continue;
            }
        }

        const XMVECTOR worldPosition = vobInfo->Vob->GetPositionWorldXM();
        const float distanceSq = XMVectorGetX( XMVector3LengthSq( worldPosition - cameraPosition ) );
        if ( distanceSq > MaxWindowDistance * MaxWindowDistance ) {
            continue;
        }
        candidates.push_back( { distanceSq, cached.Volume } );
    }

    if ( candidates.size() > MaxWindowCutouts ) {
        std::nth_element( candidates.begin(), candidates.begin() + MaxWindowCutouts, candidates.end(),
            []( const Candidate& a, const Candidate& b ) { return a.DistanceSq < b.DistanceSq; } );
        candidates.resize( MaxWindowCutouts );
    }

    WindowCutoutConstants constants = {};
    XMStoreFloat4x4( &constants.InvView, XMMatrixInverse( nullptr,
        XMLoadFloat4x4( &Engine::GAPI->GetRendererState().TransformState.TransformView ) ) );
    constants.Count = static_cast<unsigned int>(candidates.size());
    for ( size_t i = 0; i < candidates.size(); ++i ) {
        constants.Volumes[i] = candidates[i].Volume;
    }

    constexpr unsigned int TileCountX = 16;
    constexpr unsigned int TileCountY = 9;
    constants.TileCountX = TileCountX;
    constants.TileCountY = TileCountY;
    UINT viewportCount = 1;
    D3D11_VIEWPORT viewport = {};
    GetContext()->RSGetViewports( &viewportCount, &viewport );
    uint32_t* tileMasks = reinterpret_cast<uint32_t*>( constants.TileMasks );
    auto addToAllTiles = [&]( uint32_t bit ) {
        for ( unsigned int tile = 0; tile < TileCountX * TileCountY; ++tile ) {
            tileMasks[tile] |= bit;
        }
    };

    if ( viewportCount == 1 && viewport.Width > 0.0f && viewport.Height > 0.0f ) {
        constants.PixelToTile = XMFLOAT2( TileCountX / viewport.Width, TileCountY / viewport.Height );
        constants.TileOrigin = XMFLOAT2( viewport.TopLeftX, viewport.TopLeftY );
        const XMMATRIX view = XMMatrixTranspose( XMLoadFloat4x4(
            &Engine::GAPI->GetRendererState().TransformState.TransformView ) );
        const XMMATRIX projection = XMMatrixTranspose(
            XMLoadFloat4x4( &Engine::GAPI->GetProjectionMatrix() ) );
        const XMMATRIX identity = XMMatrixIdentity();

        for ( size_t i = 0; i < candidates.size(); ++i ) {
            const WindowCutoutVolume& volume = candidates[i].Volume;
            const XMVECTOR center = XMLoadFloat4( &volume.CenterExtentX );
            const XMVECTOR axes[3] = {
                XMLoadFloat4( &volume.AxisXExtentY ),
                XMLoadFloat4( &volume.AxisYExtentZ ),
                XMLoadFloat4( &volume.AxisZPadding ),
            };
            const float extents[3] = {
                volume.CenterExtentX.w,
                volume.AxisXExtentY.w,
                volume.AxisYExtentZ.w,
            };
            float minX = FLT_MAX;
            float minY = FLT_MAX;
            float maxX = -FLT_MAX;
            float maxY = -FLT_MAX;
            bool useAllTiles = false;
            for ( int x = -1; x <= 1; x += 2 ) {
                for ( int y = -1; y <= 1; y += 2 ) {
                    for ( int z = -1; z <= 1; z += 2 ) {
                        XMVECTOR corner = center
                            + axes[0] * (extents[0] * static_cast<float>(x))
                            + axes[1] * (extents[1] * static_cast<float>(y))
                            + axes[2] * (extents[2] * static_cast<float>(z));
                        XMFLOAT3 projected;
                        XMStoreFloat3( &projected, XMVector3Project( corner,
                            viewport.TopLeftX, viewport.TopLeftY, viewport.Width, viewport.Height,
                            viewport.MinDepth, viewport.MaxDepth, projection, view, identity ) );
                        if ( !std::isfinite( projected.x ) || !std::isfinite( projected.y )
                            || !std::isfinite( projected.z ) || projected.z <= 0.0f ) {
                            useAllTiles = true;
                            continue;
                        }
                        minX = std::min( minX, projected.x );
                        minY = std::min( minY, projected.y );
                        maxX = std::max( maxX, projected.x );
                        maxY = std::max( maxY, projected.y );
                    }
                }
            }

            const uint32_t bit = 1u << static_cast<uint32_t>(i);
            if ( useAllTiles || minX > maxX || minY > maxY ) {
                addToAllTiles( bit );
                continue;
            }
            int minTileX = static_cast<int>(std::floor(
                (minX - viewport.TopLeftX) * TileCountX / viewport.Width)) - 1;
            int minTileY = static_cast<int>(std::floor(
                (minY - viewport.TopLeftY) * TileCountY / viewport.Height)) - 1;
            int maxTileX = static_cast<int>(std::floor(
                (maxX - viewport.TopLeftX) * TileCountX / viewport.Width)) + 1;
            int maxTileY = static_cast<int>(std::floor(
                (maxY - viewport.TopLeftY) * TileCountY / viewport.Height)) + 1;
            minTileX = std::clamp( minTileX, 0, static_cast<int>(TileCountX) - 1 );
            minTileY = std::clamp( minTileY, 0, static_cast<int>(TileCountY) - 1 );
            maxTileX = std::clamp( maxTileX, 0, static_cast<int>(TileCountX) - 1 );
            maxTileY = std::clamp( maxTileY, 0, static_cast<int>(TileCountY) - 1 );
            for ( int tileY = minTileY; tileY <= maxTileY; ++tileY ) {
                for ( int tileX = minTileX; tileX <= maxTileX; ++tileX ) {
                    tileMasks[tileY * TileCountX + tileX] |= bit;
                }
            }
        }
    } else {
        for ( size_t i = 0; i < candidates.size(); ++i ) {
            addToAllTiles( 1u << static_cast<uint32_t>(i) );
        }
        constants.PixelToTile = XMFLOAT2( 0.0f, 0.0f );
    }

    if ( !WindowCutoutConstantsBuffer ) {
        WindowCutoutConstantsBuffer = std::make_unique<D3D11ConstantBuffer>( sizeof( constants ), &constants );
    } else {
        WindowCutoutConstantsBuffer->UpdateBuffer( &constants );
    }
    WindowCutoutConstantsBuffer->BindToPixelShader( 6 );
    return constants.Count;
}

void D3D11GraphicsEngine::UnbindWindowCutouts() {
    WindowCutoutConstants constants = {};
    constants.TileCountX = 1;
    constants.TileCountY = 1;
    if ( WindowCutoutConstantsBuffer ) {
        WindowCutoutConstantsBuffer->UpdateBuffer( &constants )->BindToPixelShader( 6 );
    }
}

void __cdecl Stub_DrawMultiIndexedInstancedIndirect(
    ID3D11DeviceContext* context, unsigned int drawCount, ID3D11Buffer* buffer,
    unsigned int alignedByteOffsetForArgs, unsigned int alignedByteStrideForArgs ) {
    for ( unsigned int i = 0; i < drawCount; ++i ) {
        context->DrawIndexedInstancedIndirect( buffer, alignedByteOffsetForArgs );
        alignedByteOffsetForArgs += alignedByteStrideForArgs;
    }
}

void __cdecl Stub_BeginUAVOverlap( ID3D11DeviceContext* context ) {
    (void)context;
}

void __cdecl Stub_EndUAVOverlap( ID3D11DeviceContext* context ) {
    (void)context;
}

void __cdecl DXVK_DrawMultiIndexedInstancedIndirect(
    ID3D11DeviceContext* context, unsigned int drawCount, ID3D11Buffer* buffer,
    unsigned int alignedByteOffsetForArgs, unsigned int alignedByteStrideForArgs ) {
    ID3D11VkExtContext* DXVKContext;
    if ( SUCCEEDED( context->QueryInterface( __uuidof(ID3D11VkExtContext), reinterpret_cast<void**>(&DXVKContext) ) ) ) {
        DXVKContext->MultiDrawIndexedIndirect( drawCount, buffer, alignedByteOffsetForArgs, alignedByteStrideForArgs );
        DXVKContext->Release();
    }
}

void __cdecl DXVK_BeginUAVOverlap( ID3D11DeviceContext* context ) {
    ID3D11VkExtContext* DXVKContext;
    if ( SUCCEEDED( context->QueryInterface( __uuidof(ID3D11VkExtContext), reinterpret_cast<void**>(&DXVKContext) ) ) ) {
        DXVKContext->SetBarrierControl( D3D11_VK_BARRIER_CONTROL_IGNORE_WRITE_AFTER_WRITE );
        DXVKContext->Release();
    }
}

void __cdecl DXVK_EndUAVOverlap( ID3D11DeviceContext* context ) {
    ID3D11VkExtContext* DXVKContext;
    if ( SUCCEEDED( context->QueryInterface( __uuidof(ID3D11VkExtContext), reinterpret_cast<void**>(&DXVKContext) ) ) ) {
        DXVKContext->SetBarrierControl( 0u );
        DXVKContext->Release();
    }
}

void __cdecl IGDEXT_DrawMultiIndexedInstancedIndirect(
    ID3D11DeviceContext* context, unsigned int drawCount, ID3D11Buffer* buffer,
    unsigned int alignedByteOffsetForArgs, unsigned int alignedByteStrideForArgs ) {
    igdextDevice->DrawMultiIndexedInstancedIndirect( context, drawCount, buffer, alignedByteOffsetForArgs, alignedByteStrideForArgs );
}

void __cdecl IGDEXT_BeginUAVOverlap( ID3D11DeviceContext* ) {
    igdextDevice->BeginUAVOverlap();
}

void __cdecl IGDEXT_EndUAVOverlap( ID3D11DeviceContext* ) {
    igdextDevice->EndUAVOverlap();
}

void __cdecl AGS_DrawMultiIndexedInstancedIndirect(
    ID3D11DeviceContext* context, unsigned int drawCount, ID3D11Buffer* buffer,
    unsigned int alignedByteOffsetForArgs, unsigned int alignedByteStrideForArgs ) {
    agsDevice->DrawMultiIndexedInstancedIndirect( context, drawCount, buffer, alignedByteOffsetForArgs, alignedByteStrideForArgs );
}

void __cdecl AGS_BeginUAVOverlap( ID3D11DeviceContext* context ) {
    agsDevice->BeginUAVOverlap( context );
}

void __cdecl AGS_EndUAVOverlap( ID3D11DeviceContext* context ) {
    agsDevice->EndUAVOverlap( context );
}

void D3D11GraphicsEngine::CreateAndBindDefaultSampler() {
    HRESULT hr;

    float scaleRatio = static_cast<float>(GetScaledResolution().x) / static_cast<float>(GetBackbufferResolution().x);
    // Calculate raw bias, but clamp it to a maximum of 0.0f to protect Supersampling
    float mipBias = std::min(0.0f, std::log2(scaleRatio));

    D3D11_SAMPLER_DESC samplerDesc{};
    samplerDesc.Filter = D3D11_FILTER_ANISOTROPIC;
    samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
    samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
    samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    samplerDesc.MipLODBias = mipBias;
    samplerDesc.MaxAnisotropy = 16;
    samplerDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    samplerDesc.BorderColor[0] = 1.0f;
    samplerDesc.BorderColor[1] = 1.0f;
    samplerDesc.BorderColor[2] = 1.0f;
    samplerDesc.BorderColor[3] = 1.0f;
    samplerDesc.MinLOD = -3.402823466e+38F;  // -FLT_MAX
    samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;   // FLT_MAX

    LE( GetDevice()->CreateSamplerState( &samplerDesc, DefaultSamplerState.GetAddressOf() ) );
    GetContext()->PSSetSamplers( 0, 1, DefaultSamplerState.GetAddressOf() );
    GetContext()->VSSetSamplers( 0, 1, DefaultSamplerState.GetAddressOf() );
    SetDebugName( DefaultSamplerState.Get(), "DefaultSamplerState" );
}

/** Called when the game created it's window */
XRESULT D3D11GraphicsEngine::Init() {
    // Load dynamically necessary libraries
    typedef HRESULT( WINAPI* PFN_CREATE_DXGI_FACTORY )(REFIID riid, void** ppFactory);
    typedef HRESULT( WINAPI* PFN_CREATE_DXGI_FACTORY2 )(UINT flags, REFIID riid, void** ppFactory);
    typedef HRESULT( WINAPI* PFN_D3D11_CREATE_DEVICE )(IDXGIAdapter* pAdapter, D3D_DRIVER_TYPE DriverType, HMODULE Software, UINT Flags, CONST D3D_FEATURE_LEVEL* pFeatureLevels, UINT FeatureLevels, UINT SDKVersion, ID3D11Device** ppDevice, D3D_FEATURE_LEVEL* pFeatureLevel, ID3D11DeviceContext** ppImmediateContext);

    HMODULE dxgiHandle = LoadLibraryA( "dxgi.dll" );
    HMODULE d3d11Handle = LoadLibraryA( "d3d11.dll" );
    if ( !dxgiHandle || !d3d11Handle ) {
        LogErrorBox() << "Minimum supported Operating System by GD3D11 is Windows 7 SP1 with Platform Update.";
        exit( 2 );
    }

    PFN_CREATE_DXGI_FACTORY CreateDXGIFactoryFunc = reinterpret_cast<PFN_CREATE_DXGI_FACTORY>( GetProcAddress( dxgiHandle, "CreateDXGIFactory1" ) );
    PFN_CREATE_DXGI_FACTORY2 CreateDXGIFactory2Func = reinterpret_cast<PFN_CREATE_DXGI_FACTORY2>( GetProcAddress( dxgiHandle, "CreateDXGIFactory2") );
    PFN_D3D11_CREATE_DEVICE D3D11CreateDeviceFunc = reinterpret_cast<PFN_D3D11_CREATE_DEVICE>( GetProcAddress( d3d11Handle, "D3D11CreateDevice" ) );
    if ( !D3D11CreateDeviceFunc || ( !CreateDXGIFactory2Func && !CreateDXGIFactoryFunc ) ) {
        LogErrorBox() << "Minimum supported Operating System by GD3D11 is Windows 7 SP1 with Platform Update.";
        exit( 2 );
    }

    HRESULT hr;
    LogInfo() << "Initializing Device...";

    // Create DXGI factory
    UINT factoryFlags = 0;
#ifdef DEBUG_D3D11
    factoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
#endif
    hr = (CreateDXGIFactory2Func ? CreateDXGIFactory2Func( factoryFlags, __uuidof(IDXGIFactory2), reinterpret_cast<void**>( DXGIFactory2.ReleaseAndGetAddressOf() ) )
        : CreateDXGIFactoryFunc( __uuidof(IDXGIFactory2), reinterpret_cast<void**>( DXGIFactory2.ReleaseAndGetAddressOf() ) ));
    if ( FAILED( hr ) ) {
        LogErrorBox() << "CreateDXGIFactory failed with code: " << hr << "!\n"
            "Minimum supported Operating System by GD3D11 is Windows 7 SP1 with Platform Update.";
        exit( 2 );
    }

    bool haveAdapter = false;
    Microsoft::WRL::ComPtr<IDXGIAdapter1> DXGIAdapter1;
    Microsoft::WRL::ComPtr<IDXGIFactory6> DXGIFactory6;
    hr = DXGIFactory2.As( &DXGIFactory6 );
    if ( SUCCEEDED( hr )
        && ((factoryFlags & DXGI_CREATE_FACTORY_DEBUG) == 0) // Breaks when using Graphics debugging (e.g. from Visual Studio)
    ) {
        // Windows 10, version 1803 - only
        UINT adapterIndex = 0;
        while ( DXGIFactory6->EnumAdapterByGpuPreference( adapterIndex++, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
            IID_PPV_ARGS( DXGIAdapter1.ReleaseAndGetAddressOf() ) ) != DXGI_ERROR_NOT_FOUND ) {
            DXGI_ADAPTER_DESC1 desc;
            DXGIAdapter1->GetDesc1( &desc );

            if ( desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE ) {
                // Don't select the Basic Render Driver adapter.
                continue;
            }
            haveAdapter = true;
            break;
        }
    } else {
        // Let's rate devices by their VRAM and vendors and hope we'll get atleast discrete GPU
        std::map<uint64_t, UINT> candidates;
        for ( UINT adapterIndex = 0; DXGIFactory2->EnumAdapters1( adapterIndex, DXGIAdapter1.ReleaseAndGetAddressOf() ) != DXGI_ERROR_NOT_FOUND; ++adapterIndex ) {
            DXGI_ADAPTER_DESC1 desc;
            DXGIAdapter1->GetDesc1( &desc );

            if ( desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE ) {
                // Don't select the Basic Render Driver adapter.
                continue;
            }

            uint64_t deviceRating = static_cast<uint64_t>(desc.DedicatedVideoMemory);
            if ( desc.VendorId == 0x10DE ) { // Rate NVIDIA GPU's highest
                deviceRating += 0x200000000;
            } else if ( desc.VendorId == 0x1002 ) { // Rate AMD GPU's higher than Intel IGPU
                deviceRating += 0x100000000;
            }
            candidates.emplace( deviceRating, adapterIndex );
        }

        if ( !candidates.empty() ) {
            LE( DXGIFactory2->EnumAdapters1( candidates.rbegin()->second, DXGIAdapter1.ReleaseAndGetAddressOf() ) ); // Get first suitable adapter
            haveAdapter = true;
        }
    }

    if ( !haveAdapter ) {
        LogErrorBox() << "Couldn't find any suitable GPU on your device, so it can't run GD3D11!\n"
            "It has to be at least Featurelevel 10.0 compatible, "
            "which requires at least:\n"
            " *	Nvidia GeForce 8xxx or higher\n"
            " *	AMD Radeon HD 2xxx or higher\n\n"
            "The game will now close.";
        exit( 2 );
    }

    DXGIAdapter1.As( &DXGIAdapter2 );
    // Find out what we are rendering on to write it into the logfile
    DXGI_ADAPTER_DESC2 adpDesc;
    DXGIAdapter2->GetDesc2( &adpDesc );
    std::wstring wDeviceDescription( adpDesc.Description );
    std::string deviceDescription( wDeviceDescription.begin(), wDeviceDescription.end() );
    DeviceDescription = deviceDescription;
    LogInfo() << "Rendering on: " << deviceDescription.c_str();

    bool dxvkAvailable = false;
    IUnknown* dxgiVKInterop = nullptr;
    HRESULT result = DXGIAdapter2->QueryInterface( IID_IDXGIVkInteropAdapter, reinterpret_cast<void**>(&dxgiVKInterop) );
    if ( SUCCEEDED( result ) ) {
        dxvkAvailable = true;
        dxgiVKInterop->Release();
    }

    if ( !dxvkAvailable && Engine::GAPI->GetRendererState().RendererSettings.DebugSettings.FeatureSet.EnableDriverExtensions ) {
        if ( adpDesc.VendorId == 0x10DE ) {
            nvapiDevice.reset( new D3D11NVAPI );
            if ( !nvapiDevice->InitNVAPI() ) {
                nvapiDevice.reset();
            } else {
                if ( void* NvAPI_D3D11_MultiDrawIndexedInstancedIndirect = nvapiDevice->GetDrawMultiIndexedInstancedIndirect() ) {
                    DrawMultiIndexedInstancedIndirect = reinterpret_cast<PFN_DRAWMULTIINDEXEDINSTANCEDINDIRECT>(NvAPI_D3D11_MultiDrawIndexedInstancedIndirect);
                }

                void* NvAPI_D3D11_BeginUAVOverlap = nvapiDevice->GetBeginUAVOverlap();
                void* NvAPI_D3D11_EndUAVOverlap = nvapiDevice->GetEndUAVOverlap();
                if ( NvAPI_D3D11_BeginUAVOverlap && NvAPI_D3D11_EndUAVOverlap ) {
                    BeginUAVOverlap = reinterpret_cast<PFN_BEGINUAVOVERLAP>(NvAPI_D3D11_BeginUAVOverlap);
                    EndUAVOverlap = reinterpret_cast<PFN_ENDUAVOVERLAP>(NvAPI_D3D11_EndUAVOverlap);
                }
            }
        } else if ( adpDesc.VendorId == 0x1002 ) {
            agsDevice.reset( new D3D11AGS );
            if ( !agsDevice->InitAGS() ) {
                agsDevice.reset();
            }
            userHaveAMDGPU = true;
        }
    }

    D3D_FEATURE_LEVEL maxFeatureLevel = D3D_FEATURE_LEVEL::D3D_FEATURE_LEVEL_9_1;
    D3D_FEATURE_LEVEL featureLevels[] = {
        D3D_FEATURE_LEVEL_12_1,
        D3D_FEATURE_LEVEL_12_0,
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
        D3D_FEATURE_LEVEL_9_3,
        D3D_FEATURE_LEVEL_9_2,
        D3D_FEATURE_LEVEL_9_1
    };

    // Create D3D11-Device
    int flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef DEBUG_D3D11
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    Microsoft::WRL::ComPtr<ID3D11Device> Device11;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> Context11;

    const size_t featureLevelCount = sizeof( featureLevels ) / sizeof( &featureLevels[0] );
    if ( agsDevice ) {
        for ( size_t i = 0; i < featureLevelCount; ++i ) {
            hr = agsDevice->CreateD3D11Device( DXGIAdapter2.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, &featureLevels[i], featureLevelCount - i,
                D3D11_SDK_VERSION, Device11.GetAddressOf(), &maxFeatureLevel, Context11.GetAddressOf() );
            if ( SUCCEEDED( hr ) ) {
                break;
            }

            maxFeatureLevel = D3D_FEATURE_LEVEL::D3D_FEATURE_LEVEL_9_1;
        }
    } else {
        for ( size_t i = 0; i < featureLevelCount; ++i ) {
            hr = D3D11CreateDeviceFunc( DXGIAdapter2.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, &featureLevels[i], featureLevelCount - i,
                D3D11_SDK_VERSION, Device11.GetAddressOf(), &maxFeatureLevel, Context11.GetAddressOf() );
            if ( SUCCEEDED( hr ) ) {
                break;
            }

            maxFeatureLevel = D3D_FEATURE_LEVEL::D3D_FEATURE_LEVEL_9_1;
        }
    }

    if ( FAILED( hr ) ) {
        LogErrorBox() << "D3D11CreateDevice failed with code: " << std::hex << hr << "!";
        exit( 2 );
    }

    PrintD3DFeatureLevel( maxFeatureLevel );
    if ( maxFeatureLevel < D3D_FEATURE_LEVEL::D3D_FEATURE_LEVEL_10_0 ) {
        LogErrorBox() << "Your GPU (" << deviceDescription.c_str()
            << ") does not support Direct3D 11, so it can't run GD3D11!\n"
            "It has to be at least Featurelevel 10.0 compatible, "
            "which requires at least:\n"
            " *	Nvidia GeForce 8xxx or higher\n"
            " *	AMD Radeon HD 2xxx or higher\n\n"
            "The game will now close.";
        exit( 2 );
    }

    if ( dxvkAvailable && Engine::GAPI->GetRendererState().RendererSettings.DebugSettings.FeatureSet.EnableDriverExtensions) {
        Microsoft::WRL::ComPtr<ID3D11VkExtDevice> DXVKDevice;
        if ( SUCCEEDED( Device11.As( &DXVKDevice ) ) ) {
            if ( DXVKDevice->GetExtensionSupport( D3D11_VK_EXT_MULTI_DRAW_INDIRECT ) ) {
                DrawMultiIndexedInstancedIndirect = DXVK_DrawMultiIndexedInstancedIndirect;
            }

            if ( DXVKDevice->GetExtensionSupport( D3D11_VK_EXT_BARRIER_CONTROL ) ) {
                BeginUAVOverlap = DXVK_BeginUAVOverlap;
                EndUAVOverlap = DXVK_EndUAVOverlap;
            }
        }
    } else if ( nvapiDevice ) {
        nvapiDevice->RegisterDevice( Device11.Get() );
    } else if ( agsDevice ) {
        if ( agsDevice->IsDrawMultiIndexedInstancedIndirectAvailable() ) {
            DrawMultiIndexedInstancedIndirect = AGS_DrawMultiIndexedInstancedIndirect;
        }

        if ( agsDevice->IsUAVOverlapAvailable() ) {
            BeginUAVOverlap = AGS_BeginUAVOverlap;
            EndUAVOverlap = AGS_EndUAVOverlap;
        }
    } else if ( adpDesc.VendorId == 0x8086 ) {
        // Intel extension is initialized late
        // because we need ID3D11Device object
        igdextDevice.reset( new D3D11IGDEXT );
        if ( !igdextDevice->InitIGDEXT( Device11.Get() ) ) {
            igdextDevice.reset();
        } else {
            if ( igdextDevice->IsDrawMultiIndexedInstancedIndirectAvailable() ) {
                DrawMultiIndexedInstancedIndirect = IGDEXT_DrawMultiIndexedInstancedIndirect;
            }

            if ( igdextDevice->IsUAVOverlapAvailable() ) {
                BeginUAVOverlap = IGDEXT_BeginUAVOverlap;
                EndUAVOverlap = IGDEXT_EndUAVOverlap;
            }
        }
    }


    Device11.As( &Device );
    Context11.As( &Context );
    s_tracyD3D11Ctx = TracyD3D11Context( Device.Get(), Context.Get() );

    Context.As( &m_UserDefinedAnnotation );

    // Check for Windows 10 support; older feature-level documentation is unreliable.
    NativeSupport16BitTextures = Toolbox::IsWindowsVersionOrGreater( HIBYTE( _WIN32_WINNT_WIN10 ), LOBYTE( _WIN32_WINNT_WIN10 ), 0 );
    FeatureLevel10Compatibility = (maxFeatureLevel < D3D_FEATURE_LEVEL::D3D_FEATURE_LEVEL_11_0);

    if ( Engine::GAPI->GetRendererState().RendererSettings.DebugSettings.FeatureSet.ForceFeatureLevel10 ) {
        FeatureLevel10Compatibility = true;
    }
    FetchDisplayModeList();

    ComPtr<IUnknown> renderdoc = nullptr;
    result = Device.AsIID( IID_IDXGIDeviceRenderDoc, &renderdoc );
    if ( SUCCEEDED( result ) ) {
        // Don't use extensions if they are available
        // renderdoc doesn't like them
        DrawMultiIndexedInstancedIndirect = Stub_DrawMultiIndexedInstancedIndirect;
        BeginUAVOverlap = Stub_BeginUAVOverlap;
        EndUAVOverlap = Stub_EndUAVOverlap;
    }

    if ( !DrawMultiIndexedInstancedIndirect ) {
        DrawMultiIndexedInstancedIndirect = Stub_DrawMultiIndexedInstancedIndirect;
    }

    Engine::GAPI->GetRendererState().RendererSettings.DebugSettings.FeatureSet.UseMDI =
        !FeatureLevel10Compatibility
        && DrawMultiIndexedInstancedIndirect != Stub_DrawMultiIndexedInstancedIndirect;

    if ( !BeginUAVOverlap || !EndUAVOverlap ) {
        BeginUAVOverlap = Stub_BeginUAVOverlap;
        EndUAVOverlap = Stub_EndUAVOverlap;
    }

    D3D11_FEATURE_DATA_D3D11_OPTIONS3 options3;
    hr = Device->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS3, &options3, sizeof( options3 ) );
    if ( SUCCEEDED( hr ) ) {
        FeatureRTArrayIndexFromAnyShader = options3.VPAndRTArrayIndexFromAnyShaderFeedingRasterizer;
        Engine::GAPI->GetRendererState().RendererSettings.DebugSettings.FeatureSet.UseLayeredRendering = FeatureRTArrayIndexFromAnyShader;
    }

    LogInfo() << "Creating ShaderManager";
    ShaderManager = std::make_unique<D3D11ShaderManager>();
    ShaderManager->Init();
    ShaderManager->LoadShaders();

    TempVertexBuffer = std::make_unique<D3D11VertexBuffer>();
    TempVertexBuffer->Init(
        nullptr, DRAWVERTEXARRAY_BUFFER_SIZE, D3D11VertexBuffer::B_VERTEXBUFFER,
        D3D11VertexBuffer::U_DYNAMIC, D3D11VertexBuffer::CA_WRITE );
    SetDebugName( TempVertexBuffer->GetShaderResourceView().Get(), "TempVertexBuffer->ShaderResourceView" );
    SetDebugName( TempVertexBuffer->GetVertexBuffer().Get(), "TempVertexBuffer->VertexBuffer" );

    TempPolysVertexBuffer = std::make_unique<D3D11VertexBuffer>();
    TempPolysVertexBuffer->Init(
        nullptr, POLYS_BUFFER_SIZE, D3D11VertexBuffer::B_VERTEXBUFFER,
        D3D11VertexBuffer::U_DYNAMIC, D3D11VertexBuffer::CA_WRITE );
    SetDebugName( TempPolysVertexBuffer->GetShaderResourceView().Get(), "TempVertexBuffer->ShaderResourceView" );
    SetDebugName( TempPolysVertexBuffer->GetVertexBuffer().Get(), "TempVertexBuffer->VertexBuffer" );

    TempParticlesVertexBuffer = std::make_unique<D3D11VertexBuffer>();
    TempParticlesVertexBuffer->Init(
        nullptr, PARTICLES_BUFFER_SIZE, D3D11VertexBuffer::B_VERTEXBUFFER,
        D3D11VertexBuffer::U_DYNAMIC, D3D11VertexBuffer::CA_WRITE );
    SetDebugName( TempParticlesVertexBuffer->GetShaderResourceView().Get(), "TempVertexBuffer->ShaderResourceView" );
    SetDebugName( TempParticlesVertexBuffer->GetVertexBuffer().Get(), "TempVertexBuffer->VertexBuffer" );

    TempMorphedMeshSmallVertexBuffer = std::make_unique<D3D11VertexBuffer>();
    TempMorphedMeshSmallVertexBuffer->Init(
        nullptr, MORPHEDMESH_SMALL_BUFFER_SIZE, D3D11VertexBuffer::B_VERTEXBUFFER,
        D3D11VertexBuffer::U_DYNAMIC, D3D11VertexBuffer::CA_WRITE );
    SetDebugName( TempMorphedMeshSmallVertexBuffer->GetShaderResourceView().Get(), "TempVertexBuffer->ShaderResourceView" );
    SetDebugName( TempMorphedMeshSmallVertexBuffer->GetVertexBuffer().Get(), "TempVertexBuffer->VertexBuffer" );

    TempMorphedMeshBigVertexBuffer = std::make_unique<D3D11VertexBuffer>();
    TempMorphedMeshBigVertexBuffer->Init(
        nullptr, MORPHEDMESH_HIGH_BUFFER_SIZE, D3D11VertexBuffer::B_VERTEXBUFFER,
        D3D11VertexBuffer::U_DYNAMIC, D3D11VertexBuffer::CA_WRITE );
    SetDebugName( TempMorphedMeshBigVertexBuffer->GetShaderResourceView().Get(), "TempVertexBuffer->ShaderResourceView" );
    SetDebugName( TempMorphedMeshBigVertexBuffer->GetVertexBuffer().Get(), "TempVertexBuffer->VertexBuffer" );

    TempHUDVertexBuffer = std::make_unique<D3D11VertexBuffer>();
    TempHUDVertexBuffer->Init(
        nullptr, HUD_BUFFER_SIZE, D3D11VertexBuffer::B_VERTEXBUFFER,
        D3D11VertexBuffer::U_DYNAMIC, D3D11VertexBuffer::CA_WRITE );
    SetDebugName( TempHUDVertexBuffer->GetShaderResourceView().Get(), "TempVertexBuffer->ShaderResourceView" );
    SetDebugName( TempHUDVertexBuffer->GetVertexBuffer().Get(), "TempVertexBuffer->VertexBuffer" );

    DynamicInstancingBuffer = std::make_unique<D3D11VertexBuffer>();
    DynamicInstancingBuffer->Init(
        nullptr, INSTANCING_BUFFER_SIZE, D3D11VertexBuffer::B_VERTEXBUFFER,
        D3D11VertexBuffer::U_DYNAMIC, D3D11VertexBuffer::CA_WRITE );
    SetDebugName( DynamicInstancingBuffer->GetShaderResourceView().Get(), "DynamicInstancingBuffer->ShaderResourceView" );
    SetDebugName( DynamicInstancingBuffer->GetVertexBuffer().Get(), "DynamicInstancingBuffer->VertexBuffer" );

    NodeAttachmentInstancingBuffer = std::make_unique<D3D11VertexBuffer>();
    NodeAttachmentInstancingBuffer->Init(
        nullptr, sizeof( NodeAttachmentInstanceData ) * 1024, D3D11VertexBuffer::B_VERTEXBUFFER,
        D3D11VertexBuffer::U_DYNAMIC, D3D11VertexBuffer::CA_WRITE );
    SetDebugName( NodeAttachmentInstancingBuffer->GetVertexBuffer().Get(), "NodeAttachmentInstancingBuffer" );

    DecalInstancingBuffer = std::make_unique<D3D11VertexBuffer>();
    DecalInstancingBuffer->Init(
        nullptr, sizeof( XMFLOAT4X4 ) * 1024, D3D11VertexBuffer::B_VERTEXBUFFER,
        D3D11VertexBuffer::U_DYNAMIC, D3D11VertexBuffer::CA_WRITE );
    SetDebugName( DecalInstancingBuffer->GetVertexBuffer().Get(), "DecalInstancingBuffer" );

    CreateAndBindDefaultSampler();

    D3D11_SAMPLER_DESC samplerDesc{};
    samplerDesc.Filter = D3D11_FILTER_ANISOTROPIC;
    samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
    samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
    samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    samplerDesc.MipLODBias = 0.0f;
    samplerDesc.MaxAnisotropy = 16;
    samplerDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    samplerDesc.BorderColor[0] = 1.0f;
    samplerDesc.BorderColor[1] = 1.0f;
    samplerDesc.BorderColor[2] = 1.0f;
    samplerDesc.BorderColor[3] = 1.0f;
    samplerDesc.MinLOD = -3.402823466e+38F;  // -FLT_MAX
    samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;   // FLT_MAX

    LE( GetDevice()->CreateSamplerState( &samplerDesc, LinearSamplerState.GetAddressOf() ) );
    SetDebugName( LinearSamplerState.Get(), "LinearSamplerState" );

    samplerDesc.Filter = D3D11_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR;

    samplerDesc.ComparisonFunc = D3D11_COMPARISON_LESS_EQUAL;
    // Shadow sampler and shadowmap resources moved into D3D11ShadowMap

    samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    GetDevice()->CreateSamplerState( &samplerDesc, ClampSamplerState.GetAddressOf() );
    SetDebugName( ClampSamplerState.Get(), "ClampSamplerState" );

    samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
    samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
    samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    GetDevice()->CreateSamplerState( &samplerDesc, CubeSamplerState.GetAddressOf() );
    SetDebugName( CubeSamplerState.Get(), "CubeSamplerState" );

    SetActivePixelShader( PShaderID::PS_Simple );
    SetActiveVertexShader( VShaderID::VS_Ex );

    DistortionTexture = std::make_unique<D3D11Texture>();
    DistortionTexture->Init( "system\\GD3D11\\Textures\\Distortion.dds" );
    BlueNoiseTexture = std::make_unique<D3D11Texture>();
    BlueNoiseTexture->Init( "system\\GD3D11\\Textures\\BlueNoise.dds" );
    WhiteTexture = std::make_unique<D3D11Texture>();
    uint32_t whitePixel = 0xFFFFFFFF;
    WhiteTexture->Init( {1,1}, D3D11Texture::ETextureFormat::TF_B8G8R8A8, 1, &whitePixel, "FULL_WHITE_ALPHA_OPAQUE.static-memory");

    InverseUnitSphereMesh = new GMesh;
    InverseUnitSphereMesh->LoadMesh( "system\\GD3D11\\meshes\\icoSphere.obj" );

    // Create distance-buffers
    D3D11ConstantBuffer* infiniteRangeConstantBuffer;
    D3D11ConstantBuffer* outdoorSmallVobsConstantBuffer;
    D3D11ConstantBuffer* outdoorVobsConstantBuffer;
    CreateConstantBuffer( &infiniteRangeConstantBuffer, nullptr, sizeof( float4 ) );
    CreateConstantBuffer( &outdoorSmallVobsConstantBuffer, nullptr, sizeof( float4 ) );
    CreateConstantBuffer( &outdoorVobsConstantBuffer, nullptr, sizeof( float4 ) );
    InfiniteRangeConstantBuffer.reset( infiniteRangeConstantBuffer );
    OutdoorSmallVobsConstantBuffer.reset( outdoorSmallVobsConstantBuffer );
    OutdoorVobsConstantBuffer.reset(outdoorVobsConstantBuffer);

    PerObjectMaterialInfoPooledBuffer = std::make_unique<ConstantBufferPool>();
    PerObjectMaterialInfoPooledBuffer->Initialize( GetDevice().Get() );

    // Init inf-buffer now
    static const float4 infiniteRange( FLT_MAX, 0, 0, 0 );
    InfiniteRangeConstantBuffer->UpdateBuffer( &infiniteRange );
    SetDebugName( InfiniteRangeConstantBuffer->Get().Get(), "InfiniteRangeConstantBuffer" );
    SetDebugName( OutdoorSmallVobsConstantBuffer->Get().Get(), "OutdoorSmallVobsConstantBuffer" );
    SetDebugName( OutdoorVobsConstantBuffer->Get().Get(), "OutdoorVobsConstantBuffer" );
    // Load reflection cube
    if ( S_OK != CreateDDSTextureFromFile(
        GetDevice().Get(), L"system\\GD3D11\\Textures\\SkyCubemap.dds",
        nullptr, ReflectionCube.GetAddressOf() ) )
        LogWarn()
        << "Failed to load file: system\\GD3D11\\Textures\\SkyCubemap.dds";

    // Init quad buffers
    ExVertexStruct vx[6];
    ZeroMemory( vx, sizeof( vx ) );

    const float scale = 1.0f;
    vx[0].Position = float3( -scale * 0.5f, -scale * 0.5f, 0.0f );
    vx[1].Position = float3( scale * 0.5f, -scale * 0.5f, 0.0f );
    vx[2].Position = float3( -scale * 0.5f, scale * 0.5f, 0.0f );

    vx[0].TexCoord = float2( 0, 0 );
    vx[1].TexCoord = float2( 1, 0 );
    vx[2].TexCoord = float2( 0, 1 );

    vx[0].Color = 0xFFFFFFFF;
    vx[1].Color = 0xFFFFFFFF;
    vx[2].Color = 0xFFFFFFFF;

    vx[3].Position = float3( scale * 0.5f, -scale * 0.5f, 0.0f );
    vx[4].Position = float3( scale * 0.5f, scale * 0.5f, 0.0f );
    vx[5].Position = float3( -scale * 0.5f, scale * 0.5f, 0.0f );

    vx[3].TexCoord = float2( 1, 0 );
    vx[4].TexCoord = float2( 1, 1 );
    vx[5].TexCoord = float2( 0, 1 );

    vx[3].Color = 0xFFFFFFFF;
    vx[4].Color = 0xFFFFFFFF;
    vx[5].Color = 0xFFFFFFFF;

    CreateVertexBuffer( &QuadVertexBuffer );
    QuadVertexBuffer->Init( vx, 6 * sizeof( ExVertexStruct ),
        D3D11VertexBuffer::EBindFlags::B_VERTEXBUFFER,
        D3D11VertexBuffer::EUsageFlags::U_IMMUTABLE );

    VERTEX_INDEX indices[] = { 0, 1, 2, 3, 4, 5 };
    CreateVertexBuffer( &QuadIndexBuffer );
    QuadIndexBuffer->Init( indices, sizeof( indices ),
        D3D11VertexBuffer::EBindFlags::B_INDEXBUFFER,
        D3D11VertexBuffer::EUsageFlags::U_IMMUTABLE );

    // Create shadow map manager
    ShadowMaps = std::make_unique<D3D11ShadowMap>();
    int initialShadowSize = Engine::GAPI->GetRendererState().RendererSettings.ShadowMapSize;
    ShadowMaps->Init( Device, Context, initialShadowSize );

    // Select active scene renderer based on setting
    SelectActiveRenderer();

    SteamOverlay::Init();

    Effects->LoadRainResources();

    return XR_SUCCESS;
}

void D3D11GraphicsEngine::SelectActiveRenderer() {
    auto& settings = Engine::GAPI->GetRendererState().RendererSettings;
    auto mode = settings.RendererMode;
    if ( mode == GothicRendererSettings::RM_ForwardPlus ) {
        ActiveSceneRenderer = &ForwardPlusRenderer;
        settings.EnableTiledLighting = true;
    } else {
        ActiveSceneRenderer = &DeferredRenderer;
    }
}

void D3D11GraphicsEngine::AddXeGTAOPreLightingPass( RenderGraph& graph,
                                                    RGResourceHandle normalsResource,
                                                    RGResourceHandle backBufferHandle ) {
    auto& settings = Engine::GAPI->GetRendererState().RendererSettings;
    if ( settings.AoMode != AOMode::AO_XEGTAO || !settings.GetEffectiveXeGTAOPreLighting() )
        return;

    PfxRenderer->ResetXeGTAOPreLightState();
    if ( normalsResource == RG_INVALID_HANDLE ) return;

    graph.AddPass( RG_PASS_NAME("XeGTAO Pre-Light"), [&]( RGBuilder& builder, RenderPass& pass ) {
        builder.Read( normalsResource );
        builder.Write( backBufferHandle );
        pass.m_executeCallback = [this, normalsResource]( const RenderGraph& renderGraph ) {
            TracyD3D11ZoneCGX( "D3D11GraphicsEngine::Draw XeGTAO Pre-Light" );
            auto normalsTexture = renderGraph.GetPhysicalTexture( normalsResource );
            CopyDepthStencil();
            auto* depthCopy = GetDepthBufferCopy();
            if ( !normalsTexture || !depthCopy ) return;
            PfxRenderer->RenderXeGTAOToAO(
                depthCopy->GetShaderResView().Get(),
                normalsTexture->GetShaderResView().Get() );
        };
    } );
}

namespace {
    BOOL CALLBACK EnumWindowsKillSplashProc( HWND hwnd, LPARAM lParam ) {
        // Verify the window belongs to the current process
        DWORD windowPid;
        GetWindowThreadProcessId( hwnd, &windowPid );

        if ( windowPid != GetCurrentProcessId() ) {
            return TRUE; // continue
        }

        char windowTitle[256];
        // Get the window text
        if ( GetWindowTextA( hwnd, windowTitle, sizeof( windowTitle ) ) ) {
            // Check if the title matches "Union Splash"
            if ( std::string( windowTitle ) == "Union Splash" ) {
                std::cout << "Found 'Union Splash'. Closing window handle..." << std::endl;

                // PostMessage is safer than SendMessage as it doesn't block
                PostMessage( hwnd, WM_CLOSE, 0, 0 );

                // Return FALSE to stop enumerating once found
                return FALSE;
            }
        }
        return TRUE; // Continue searching
    }
}

/** Called when the game created its window */
XRESULT D3D11GraphicsEngine::SetWindow( HWND hWnd ) {
    if ( !OutputWindow ) {
        LogInfo() << "Creating swapchain";
        OutputWindow = hWnd;

        // Force activate the window on startup
        {
            EnumWindows( EnumWindowsKillSplashProc, 0 );

            HWND hCurWnd = GetForegroundWindow();
            DWORD dwMyID = GetCurrentThreadId();
            DWORD dwCurID = GetWindowThreadProcessId( hCurWnd, NULL );
            m_isWindowActive = true;

            ShowWindow( hWnd, SW_RESTORE );
            AttachThreadInput( dwCurID, dwMyID, TRUE );
            SetWindowPos( hWnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE | SWP_SHOWWINDOW );
            SetWindowPos( hWnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE | SWP_SHOWWINDOW );
            SetForegroundWindow( hWnd );
            AttachThreadInput( dwCurID, dwMyID, FALSE );
            SetFocus( hWnd );
            SetActiveWindow( hWnd );
        }

        const INT2 res = Resolution;

#ifdef BUILD_SPACER
        RECT r;
        GetClientRect( hWnd, &r );

        res.x = r.right;
        res.y = r.bottom;
#endif
        if ( res.x != 0 && res.y != 0 ) OnResize( res );

#ifndef BUILD_SPACER_NET

        // We need to update clip cursor here because we hook the window too late to receive proper window message
        UpdateClipCursor( hWnd );

        // Force hide mouse cursor
        while ( ShowCursor( false ) >= 0 );
#endif
    }

    return XR_SUCCESS;
}

/** Reset BackBuffer */
void D3D11GraphicsEngine::OnResetBackBuffer() {
    auto res = GetResolution();
    HDRBackBuffer = std::make_unique<RenderToTextureBuffer>( GetDevice().Get(), res.x, res.y, GetBackBufferFormat(), nullptr, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, 1, 1,
        D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE | (Device->GetFeatureLevel() >= D3D_FEATURE_LEVEL_11_0 ? D3D11_BIND_UNORDERED_ACCESS : 0));
    SetDebugName( HDRBackBuffer->GetShaderResView().Get(), "Backbuffer->ShaderResourceView" );
    SetDebugName( HDRBackBuffer->GetRenderTargetView().Get(), "Backbuffer->RenderTargetView" );
}

/** Get BackBuffer Format */
DXGI_FORMAT D3D11GraphicsEngine::GetBackBufferFormat() {
    return Engine::GAPI->GetRendererState().RendererSettings.CompressBackBuffer ? DXGI_FORMAT_R11G11B10_FLOAT : DXGI_FORMAT_R16G16B16A16_FLOAT;
}


void ApplyWindowStyle(HWND window, WindowModes windowMode) {
    if (windowMode == WindowModes::WINDOW_MODE_WINDOWED) {
        // Standard window styles for a Win32 window in windowed mode
        LONG style = WS_OVERLAPPEDWINDOW | WS_VISIBLE;
        style &= ~(WS_MAXIMIZEBOX | WS_THICKFRAME); // no maximize and no resizing
        SetWindowLong(window, GWL_STYLE, style);

        LONG exStyle = WS_EX_APPWINDOW;
        SetWindowLong(window, GWL_EXSTYLE, exStyle);
    } else {
        // Remove frame border for fullscreen modes
        LONG style = GetWindowLong(window, GWL_STYLE);
        style &= ~(WS_CAPTION | WS_THICKFRAME);
        SetWindowLong(window, GWL_STYLE, style);

        LONG exStyle = GetWindowLong(window, GWL_EXSTYLE);
        exStyle &= ~(WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE);
        SetWindowLong(window, GWL_EXSTYLE, exStyle);
    }
}

/** Get Window Mode */
int D3D11GraphicsEngine::GetWindowMode() {
    return Engine::GAPI->GetRendererState().RendererSettings.StretchWindow
        ? WINDOW_MODE_FULLSCREEN_BORDERLESS
        : WINDOW_MODE_WINDOWED;
}

void D3D11GraphicsEngine::SyncGothicResolutionState( bool refreshCamera ) {
    const INT2 realResolution = GetBackbufferResolution();
    if ( realResolution.x <= 0 || realResolution.y <= 0 ) {
        return;
    }

    zCView::SetWindowMode( realResolution.x, realResolution.y, 32 );
    zCView::SetVirtualMode(
        static_cast<int>(realResolution.x),
        static_cast<int>(realResolution.y),
        32 );

    POINT virtualSize = { 8192, 8192 };
    zCViewDraw::GetScreen().SetVirtualSize( virtualSize );

    if ( refreshCamera ) {
        if ( zCCamera* camera = zCCamera::GetCamera() ) {
            camera->UpdateViewport( realResolution );
            camera->Activate();
            camera->UpdateViewport( realResolution );
            SetViewport( ViewportInfo( 0, 0, realResolution ) );
        }
    }
}
XRESULT D3D11GraphicsEngine::RecreateBuffers() {
    if ( !Engine::GAPI || !Device || !Context ) {
        return XR_FAILED;
    }

    INT2 bbres = GetBackbufferResolution();

    static INT2 lastRoundedTextureResolution{};

    auto resolutionScalePct = Engine::GAPI->GetRendererState().RendererSettings.ResolutionScalePercent;
    if ( resolutionScalePct != 100 ) {
        resolutionScalePct = std::clamp( resolutionScalePct, 25, 200 );
        Engine::GAPI->GetRendererState().RendererSettings.ResolutionScalePercent = resolutionScalePct;

        float scale = static_cast<float>(resolutionScalePct) / 100.0f;

        m_scaledResolution = INT2{
            static_cast<INT>(static_cast<float>(bbres.x) * scale),
            static_cast<INT>(static_cast<float>(bbres.y) * scale)
        };
    } else {
        m_scaledResolution = bbres;
    }


    auto roundedTextureResolution = GetResolution( );
    if ( roundedTextureResolution.x <= 0 || roundedTextureResolution.y <= 0
        || Resolution.x <= 0 || Resolution.y <= 0 ) {
        return XR_FAILED;
    }

    const bool renderBuffersAlive = DepthStencilBuffer && DepthStencilBufferCopy
        && VelocityBuffer && Backbuffer && m_SwapchainDepthStencilBuffer;
    if ( lastRoundedTextureResolution == roundedTextureResolution && renderBuffersAlive ) {
        // same resolution, just adjusting the viewport
        return XR_SUCCESS;
    }
    lastRoundedTextureResolution = roundedTextureResolution;

    // Release pointlight pool handles before resizing PFX resources.
    if ( Engine::GAPI ) {
        Engine::GAPI->ReleasePointlightShadowResources();
    }

    // Adjust DefaultSampler with negative LOD bias for upscaling
    CreateAndBindDefaultSampler();

    // Recreate DepthStencilBuffer
    DepthStencilBuffer = std::make_unique<RenderToDepthStencilBuffer>(
        GetDevice().Get(), roundedTextureResolution.x, roundedTextureResolution.y, DXGI_FORMAT_R32_TYPELESS, nullptr,
        DXGI_FORMAT_D32_FLOAT, DXGI_FORMAT_R32_FLOAT );

    DepthStencilBufferCopy = std::make_unique<RenderToTextureBuffer>(
        GetDevice().Get(), roundedTextureResolution.x, roundedTextureResolution.y, DXGI_FORMAT_R32_TYPELESS, nullptr,
        DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32_FLOAT );

    // Create PFX-Renderer
    if ( !PfxRenderer ) PfxRenderer = std::make_unique<D3D11PfxRenderer>();

    PfxRenderer->OnResize( roundedTextureResolution );

    VelocityBuffer = std::make_unique<RenderToTextureBuffer>(
        GetDevice().Get(), roundedTextureResolution.x, roundedTextureResolution.y, DXGI_FORMAT_R16G16_FLOAT );

    SetDebugName( VelocityBuffer->GetTexture().Get(), "VelocityBuffer->TEX" );
    SetDebugName( VelocityBuffer->GetShaderResView().Get(), "VelocityBuffer->SRV" );
    SetDebugName( VelocityBuffer->GetRenderTargetView().Get(), "VelocityBuffer->RTV" );

    OnResetBackBuffer();

    // actual native-resolution backbuffer for UI and copy operations !!
    Backbuffer = std::make_unique<RenderToTextureBuffer>( GetDevice().Get(), Resolution.x, Resolution.y, DXGI_FORMAT_ENGINE_SWAPCHAIN, nullptr, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, 1, 1,
    D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE | (Device->GetFeatureLevel() >= D3D_FEATURE_LEVEL_11_0 ? D3D11_BIND_UNORDERED_ACCESS : 0) );

    m_SwapchainDepthStencilBuffer = std::make_unique<RenderToDepthStencilBuffer>(
        GetDevice().Get(), Resolution.x, Resolution.y, DXGI_FORMAT_R32_TYPELESS, nullptr,
        DXGI_FORMAT_D32_FLOAT, DXGI_FORMAT_R32_FLOAT );

    SetDebugName( Backbuffer->GetTexture().Get(), "Backbuffer->TEX" );
    SetDebugName( Backbuffer->GetShaderResView().Get(), "Backbuffer->SRV" );
    SetDebugName( Backbuffer->GetRenderTargetView().Get(), "Backbuffer->RTV" );

    int s = std::min<int>( std::max<int>( Engine::GAPI->GetRendererState().RendererSettings.ShadowMapSize, 512 ), (FeatureLevel10Compatibility ? 8192 : 16384) );
    if ( !ShadowMaps ) {
        ShadowMaps = std::make_unique<D3D11ShadowMap>();
        ShadowMaps->Init( Device, Context, s );
    } else {
        ShadowMaps->Resize( s );
    }

    return XR_SUCCESS;
}



/** Called on window resize/resolution change */
XRESULT D3D11GraphicsEngine::OnResize( INT2 newSize ) {
    // WM_SIZE can report zero while minimized; keep valid resources alive.
    if ( newSize.x <= 0 || newSize.y <= 0 ) {
        return XR_SUCCESS;
    }

    if ( Engine::IsShuttingDown() || !Engine::GAPI || !Device || !Context || !DXGIFactory2
        || !OutputWindow || !IsWindow( OutputWindow ) ) {
        return XR_FAILED;
    }

    if ( m_resizeInProgress )
    {
        NewResolution = newSize;
        return XR_SUCCESS;
    }

    HRESULT hr;

    INT2 requestedSwapchainSize = newSize;
#ifndef BUILD_SPACER
    if ( Engine::GAPI->GetRendererState().RendererSettings.StretchWindow ) {
        RECT desktopRect = {};
        if ( GetClientRect( GetDesktopWindow(), &desktopRect ) ) {
            requestedSwapchainSize.x = std::max( desktopRect.right - desktopRect.left, 1L );
            requestedSwapchainSize.y = std::max( desktopRect.bottom - desktopRect.top, 1L );
        }
    }
#endif

    if ( memcmp( &Resolution, &newSize, sizeof( newSize ) ) == 0
        && memcmp( &m_swapchainResolution, &requestedSwapchainSize, sizeof( requestedSwapchainSize ) ) == 0
        && SwapChain.Get() )
        return XR_SUCCESS;  // Don't resize if neither logical nor physical output changed.

    m_resizeInProgress = true;
    struct ResizeScope final
    {
        bool& Active;
        ~ResizeScope() { Active = false; }
    } resizeScope{ m_resizeInProgress };

    ID3D11DeviceContext* resizeContext = GetContext().Get();
    if ( resizeContext )
    {
        ID3D11RenderTargetView* nullRenderTarget = nullptr;
        resizeContext->OMSetRenderTargets( 1, &nullRenderTarget, nullptr );

        ID3D11ShaderResourceView* nullShaderResources[16] = {};
        resizeContext->PSSetShaderResources( 0, 16, nullShaderResources );
        resizeContext->VSSetShaderResources( 0, 16, nullShaderResources );
        resizeContext->GSSetShaderResources( 0, 16, nullShaderResources );
        resizeContext->CSSetShaderResources( 0, 16, nullShaderResources );

        ID3D11UnorderedAccessView* nullUnorderedAccessViews[ D3D11_PS_CS_UAV_REGISTER_COUNT] = {};
        resizeContext->CSSetUnorderedAccessViews( 0, D3D11_PS_CS_UAV_REGISTER_COUNT, nullUnorderedAccessViews, nullptr );

        resizeContext->Flush();
    }

    Resolution = newSize;
    NewResolution = newSize;
    m_swapchainResolution = requestedSwapchainSize;
    INT2 bbres = GetBackbufferResolution();

    SyncGothicResolutionState( true );

#ifndef BUILD_SPACER
    BOOL isFullscreen = 0;
    if ( SwapChain.Get() ) LE( SwapChain->GetFullscreenState( &isFullscreen, nullptr ) );

    if ( isFullscreen ) {
        DXGI_MODE_DESC newMode = {};
        newMode.Width = newSize.x;
        newMode.Height = newSize.y;
        newMode.RefreshRate.Numerator = CachedRefreshRate.Numerator;
        newMode.RefreshRate.Denominator = CachedRefreshRate.Denominator;
        newMode.Format = DXGI_FORMAT_ENGINE_SWAPCHAIN ;
        SwapChain->ResizeTarget( &newMode );

        RECT desktopRect;
        GetClientRect( GetDesktopWindow(), &desktopRect );
        SetWindowPos( OutputWindow, nullptr, 0, 0, desktopRect.right, desktopRect.bottom, SWP_SHOWWINDOW );
    } else if ( Engine::GAPI->GetRendererState().RendererSettings.StretchWindow ) {
        RECT desktopRect;
        GetClientRect( GetDesktopWindow(), &desktopRect );
        ApplyWindowStyle(OutputWindow, WindowModes::WINDOW_MODE_FULLSCREEN_BORDERLESS);
        SetWindowPos( OutputWindow, nullptr, 0, 0, desktopRect.right, desktopRect.bottom,
                      SWP_SHOWWINDOW | SWP_FRAMECHANGED );
    } else {
        ApplyWindowStyle(OutputWindow, WindowModes::WINDOW_MODE_WINDOWED);

        RECT windowSize = { 0, 0, bbres.x, bbres.y };
        const DWORD style = static_cast<DWORD>(GetWindowLongPtr( OutputWindow, GWL_STYLE ));
        const DWORD exStyle = static_cast<DWORD>(GetWindowLongPtr( OutputWindow, GWL_EXSTYLE ));
        AdjustWindowRectEx( &windowSize, style, FALSE, exStyle );
        const int outerWidth = windowSize.right - windowSize.left;
        const int outerHeight = windowSize.bottom - windowSize.top;

        RECT rect;
        if ( GetWindowRect( OutputWindow, &rect ) ) {
            SetWindowPos( OutputWindow, nullptr, rect.left, rect.top, outerWidth, outerHeight, SWP_SHOWWINDOW | SWP_FRAMECHANGED );
        } else {
            SetWindowPos( OutputWindow, nullptr, 0, 0, outerWidth, outerHeight, SWP_SHOWWINDOW | SWP_FRAMECHANGED );
        }
    }
#endif

    // Release all referenced buffer resources before we can resize the swapchain. Needed!
    BackbufferRTV.Reset();
    DepthStencilBuffer.reset();

    UINT scflags = m_flipWithTearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    if ( frameLatencyWaitableObject ) {
        CloseHandle( frameLatencyWaitableObject );
        frameLatencyWaitableObject = nullptr;
    }
    m_ConfiguredMaximumFrameLatency = 0;

    static UINT lastSwapchainFlags = scflags;

    if ( !SwapChain.Get() ) {
        static std::map<DXGI_SWAP_EFFECT, std::string> swapEffectMap = {
            {DXGI_SWAP_EFFECT::DXGI_SWAP_EFFECT_DISCARD, "DXGI_SWAP_EFFECT_DISCARD"},
            {DXGI_SWAP_EFFECT::DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL, "DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL"},
            {DXGI_SWAP_EFFECT::DXGI_SWAP_EFFECT_FLIP_DISCARD, "DXGI_SWAP_EFFECT_FLIP_DISCARD"},
        };

        auto& displaySettings = Engine::GAPI->GetRendererState().RendererSettings;
        displaySettings.DisplayFlip = true;
        displaySettings.LowLatency = false;
        m_swapchainflip = true;
        if ( m_swapchainflip && Engine::GAPI->GetRendererState().RendererSettings.StretchWindow ) {
            ApplyWindowStyle(OutputWindow, WindowModes::WINDOW_MODE_FULLSCREEN_BORDERLESS);
        }

        DXGI_SWAP_CHAIN_DESC1 scd = {};
        DXGI_SWAP_EFFECT swapEffect = DXGI_SWAP_EFFECT::DXGI_SWAP_EFFECT_DISCARD;
        if ( m_swapchainflip ) {
            Microsoft::WRL::ComPtr<IDXGIFactory5> factory5;
            if ( SUCCEEDED( DXGIFactory2.As( &factory5 ) ) ) {
                BOOL allowTearing = FALSE;
                if ( factory5.Get() && SUCCEEDED( factory5->CheckFeatureSupport( DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allowTearing, sizeof( allowTearing ) ) ) ) {
                    m_flipWithTearing = allowTearing != 0;
                }
            }

            Microsoft::WRL::ComPtr<IDXGIFactory4> factory4;
            if ( SUCCEEDED( DXGIFactory2.As( &factory4 ) ) ) {
                swapEffect = DXGI_SWAP_EFFECT::DXGI_SWAP_EFFECT_FLIP_DISCARD;
            } else {
                swapEffect = DXGI_SWAP_EFFECT::DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
            }
        }

        LogInfo() << "SwapChain Mode: " << swapEffectMap.at( swapEffect );
        if ( m_swapchainflip ) {
            LogInfo() << "SwapChain: DXGI_FEATURE_PRESENT_ALLOW_TEARING = " << (m_flipWithTearing ? "Enabled" : "Disabled");
        }

        LogInfo() << "Creating new swapchain! (Format: " << DXGI_FORMAT_ENGINE_SWAPCHAIN << " )";

        if ( m_swapchainflip ) {
            scd.BufferCount = 2;
            if ( m_flipWithTearing ) scflags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
        } else {
            scd.BufferCount = 1;
        }

        Microsoft::WRL::ComPtr<IDXGIDevice3> pDXGIDevice3;
        m_lowlatency = Engine::GAPI->GetRendererState().RendererSettings.LowLatency;
        if ( FAILED( Device.As( &pDXGIDevice3 ) ) // DXGI 1.3 required
            || swapEffect == DXGI_SWAP_EFFECT::DXGI_SWAP_EFFECT_DISCARD ) { // Doesn't work with fullscreen exclusive on D3D11
            LogWarn() << "Low-latency display mode is unavailable; using regular borderless mode.";

            m_lowlatency = false;
            Engine::GAPI->GetRendererState().RendererSettings.LowLatency = false;
        }

        if ( m_lowlatency ) {
            scflags |= DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
        }

        lastSwapchainFlags = scflags;
        scd.SwapEffect = swapEffect;
        scd.Flags = scflags;
        scd.Format = DXGI_FORMAT_ENGINE_SWAPCHAIN ;
        scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT | DXGI_USAGE_SHADER_INPUT;
        scd.SampleDesc.Count = 1;
        scd.SampleDesc.Quality = 0;
        scd.Height = m_swapchainResolution.y;
        scd.Width = m_swapchainResolution.x;

        hr = DXGIFactory2->CreateSwapChainForHwnd( GetDevice().Get(), OutputWindow, &scd, nullptr, nullptr, SwapChain.GetAddressOf() );
        if ( FAILED( hr ) ) {
            LogError() << "Failed to create Swapchain! Program will now exit! HR: " << std::hex << hr;
            exit( 0 );
        }

        if ( m_swapchainflip ) {
            LE( DXGIFactory2->MakeWindowAssociation( OutputWindow, DXGI_MWA_NO_WINDOW_CHANGES ) );
        } else {
            // Perform fullscreen transition
            // According to microsoft guide it is the best practice
            // because the swapchain is created in accordance to desktop resolution
            // and we can have different resolution in fullscreen exclusive
            bool windowed = Engine::GAPI->HasCommandlineParameter( "ZWINDOW" ) ||
                Engine::GAPI->GetIntParamFromConfig( "zStartupWindowed" );
            if ( !windowed ) {
                DXGI_MODE_DESC newMode = {};
                newMode.Width = newSize.x;
                newMode.Height = newSize.y;
                newMode.RefreshRate.Numerator = CachedRefreshRate.Numerator;
                newMode.RefreshRate.Denominator = CachedRefreshRate.Denominator;
                newMode.Format = DXGI_FORMAT_ENGINE_SWAPCHAIN ;
                SwapChain->ResizeTarget( &newMode );
                SwapChain->SetFullscreenState( true, nullptr );
            }
        }

        if ( Engine::ImGuiHandle ) {
            Engine::ImGuiHandle->Init( OutputWindow, GetDevice(), GetContext() );
        }
    } else {
        LogInfo() << "Resizing swapchain  (Format: DXGI_FORMAT_SWAPCHAIN )";
        GetContext()->ClearState();
        hr = SwapChain->ResizeBuffers( 0, m_swapchainResolution.x, m_swapchainResolution.y, DXGI_FORMAT_ENGINE_SWAPCHAIN, lastSwapchainFlags );
        if ( FAILED( hr ) ) {
            LogError() << "Failed to resize swapchain! HRESULT: " << std::hex << hr;
            return XR_FAILED;
        }
    }

    if ( m_lowlatency ) {
        wrl::ComPtr<IDXGISwapChain2> swapChain2;
        if ( SUCCEEDED( SwapChain.As( &swapChain2 ) )
            && SUCCEEDED( swapChain2->SetMaximumFrameLatency( 1 ) ) ) {
            m_ConfiguredMaximumFrameLatency = 1;
            frameLatencyWaitableObject = swapChain2->GetFrameLatencyWaitableObject();
        }
        if ( frameLatencyWaitableObject ) {
            ZoneScopedN( "OnResize::frameLatencyWaitableObject" );
            DWORD waitResult = WAIT_IO_COMPLETION;
            while ( waitResult == WAIT_IO_COMPLETION ) {
                waitResult = WaitForSingleObjectEx( frameLatencyWaitableObject, INFINITE, true );
            }
            if ( waitResult == WAIT_FAILED ) {
                LogError() << "Frame-latency wait failed during resize. GetLastError: " << GetLastError();
                m_lowlatency = false;
                Engine::GAPI->GetRendererState().RendererSettings.LowLatency = false;
            } else if ( waitResult != WAIT_OBJECT_0 ) {
                LogWarn() << "Unexpected frame-latency wait result during resize: " << waitResult;
                m_lowlatency = false;
                Engine::GAPI->GetRendererState().RendererSettings.LowLatency = false;
            }
        } else {
            LogWarn() << "Low-latency waitable object is unavailable; using regular borderless mode.";
            m_lowlatency = false;
            Engine::GAPI->GetRendererState().RendererSettings.LowLatency = false;
        }
    }

    // Successfully resized swapchain, re-get buffers
    wrl::ComPtr<ID3D11Texture2D> backbuffer;
    m_HDR = Engine::GAPI->GetRendererState().RendererSettings.HDR_Monitor;
    UpdateColorSpace_SwapChain();
    hr = SwapChain->GetBuffer( 0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(backbuffer.GetAddressOf()) );
    if ( FAILED( hr ) || !backbuffer ) {
        LogError() << "Failed to get swapchain backbuffer after resize! HRESULT: " << std::hex << hr;
        return XR_FAILED;
    }

    // Recreate RenderTargetView
    BackbufferRTV.Reset();
    hr = GetDevice()->CreateRenderTargetView( backbuffer.Get(), nullptr, BackbufferRTV.GetAddressOf() );
    if ( FAILED( hr ) || !BackbufferRTV ) {
        LogError() << "Failed to create swapchain render-target view after resize! HRESULT: " << std::hex << hr;
        return XR_FAILED;
    }

    if ( RecreateBuffers() != XR_SUCCESS ) {
        LogError() << "Failed to recreate renderer buffers after swapchain resize.";
        return XR_FAILED;
    }

    if ( !HDRBackBuffer || !HDRBackBuffer->GetRenderTargetView() || !DepthStencilBuffer || !DepthStencilBuffer->GetDepthStencilView() ) {
        LogError() << "Renderer buffers are incomplete after swapchain resize.";
        return XR_FAILED;
    }

    // Bind our newly created resources
    GetContext()->OMSetRenderTargets( 1, HDRBackBuffer->GetRenderTargetView().GetAddressOf(),
        DepthStencilBuffer->GetDepthStencilView().Get() );

    // Set the viewport
    SetViewport( ViewportInfo( 0, 0, m_scaledResolution.x, m_scaledResolution.y ) );

    // Engine::AntTweakBar->OnResize( newSize );
    if ( Engine::ImGuiHandle ) {
        Engine::ImGuiHandle->OnResize( newSize );
    }

    return XR_SUCCESS;
}

void D3D11GraphicsEngine::ResetFrameTransientBufferPools() {
    m_MainWorldIndirectPool.ResetFrame();
    m_ShadowWorldIndirectPool.ResetFrame();
    m_MainVobInstancingPool.ResetFrame();
    m_ShadowVobInstancingPool.ResetFrame();
    if ( PerObjectMaterialInfoPooledBuffer ) {
        PerObjectMaterialInfoPooledBuffer->BeginFrame();
    }
}

D3D11IndirectBuffer* D3D11GraphicsEngine::AcquireFrameIndirectBuffer( FrameIndirectBufferPool& pool,
    const void* initData,
    unsigned int sizeInBytes,
    const char* debugName ) {
    if ( sizeInBytes == 0 ) {
        return nullptr;
    }

    if ( pool.NextBuffer >= pool.Buffers.size() ) {
        pool.Buffers.push_back( std::make_unique<D3D11IndirectBuffer>() );
    }

    auto& buffer = pool.Buffers[pool.NextBuffer++];
    if ( !buffer ) {
        buffer = std::make_unique<D3D11IndirectBuffer>();
    }

    const UINT currentCapacity = buffer->GetSizeInBytes();
    const UINT growthStep = std::max( 4096u, currentCapacity / 2u );
    const UINT grownCapacity = currentCapacity > (std::numeric_limits<UINT>::max)() - growthStep
        ? sizeInBytes
        : currentCapacity + growthStep;
    const UINT allocationSize = std::max( sizeInBytes, grownCapacity );
    const bool needsRecreate = currentCapacity < sizeInBytes;
    if ( needsRecreate ) {
        if ( buffer->Init( nullptr, allocationSize,
            D3D11IndirectBuffer::B_INDEXBUFFER,
            D3D11IndirectBuffer::U_DEFAULT,
            D3D11IndirectBuffer::CA_NONE,
            debugName ? debugName : "" ) != XR_SUCCESS ) {
            return nullptr;
        }

        if ( buffer->UpdateBufferSubresource( initData, sizeInBytes ) != XR_SUCCESS ) {
            return nullptr;
        }

        if ( Engine::GAPI->GetRendererState().RendererSettings.EnableDebugLog ) {
            LogInfo() << "(Re-)created new frame indirect buffer: " << (debugName ? debugName : "<unnamed>");
        }
    } else {
        if ( buffer->UpdateBufferSubresource( initData, sizeInBytes ) != XR_SUCCESS ) {
            return nullptr;
        }
    }

    return buffer.get();
}

D3D11VertexBuffer* D3D11GraphicsEngine::AcquireFrameInstancingBuffer( FrameInstancingBufferPool& pool,
    unsigned int sizeInBytes,
    const char* debugName ) {
    if ( sizeInBytes == 0 ) {
        return nullptr;
    }

    // Main- and shadow-VOB instance buffers are also copied into structured
    // GPU-culling resources. Their capacity must therefore remain a multiple
    // of the instance stride; the old 4096-byte growth rule could produce a
    // valid dynamic VB whose byte width was invalid for a structured copy.
    constexpr UINT InstanceStride = static_cast<UINT>( sizeof( VobInstanceInfo ) );
    const auto alignInstanceBytes = [&InstanceStride]( UINT bytes ) {
        const UINT remainder = bytes % InstanceStride;
        if ( remainder == 0 ) {
            return bytes;
        }
        const UINT padding = InstanceStride - remainder;
        return bytes > (std::numeric_limits<UINT>::max)() - padding
            ? 0u
            : bytes + padding;
    };
    sizeInBytes = alignInstanceBytes( sizeInBytes );
    if ( sizeInBytes == 0 ) {
        return nullptr;
    }

    if ( pool.NextBuffer >= pool.Buffers.size() ) {
        pool.Buffers.push_back( std::make_unique<D3D11VertexBuffer>() );
    }

    auto& buffer = pool.Buffers[pool.NextBuffer++];
    if ( !buffer ) {
        buffer = std::make_unique<D3D11VertexBuffer>();
    }

    const UINT currentCapacity = buffer->GetSizeInBytes();
    const UINT growthStep = std::max( 4096u, currentCapacity / 2u );
    const UINT grownCapacity = currentCapacity > (std::numeric_limits<UINT>::max)() - growthStep
        ? sizeInBytes
        : currentCapacity + growthStep;
    const UINT allocationSize = alignInstanceBytes( std::max( sizeInBytes, grownCapacity ) );
    if ( allocationSize == 0 ) {
        return nullptr;
    }
    if ( currentCapacity < sizeInBytes || currentCapacity % InstanceStride != 0 ) {
        if ( buffer->Init( nullptr, allocationSize,
            D3D11VertexBuffer::B_VERTEXBUFFER,
            D3D11VertexBuffer::U_DYNAMIC,
            D3D11VertexBuffer::CA_WRITE,
            debugName ? debugName : "" ) != XR_SUCCESS ) {
            return nullptr;
        }
        if ( Engine::GAPI->GetRendererState().RendererSettings.EnableDebugLog ) {
            LogInfo() << "(Re-)created new frame instancing buffer: " << (debugName ? debugName : "FrameInstancingBuffer");
        }
        SetDebugName( buffer->GetVertexBuffer().Get(), debugName ? debugName : "FrameInstancingBuffer" );
    }

    return buffer.get();
}
static const char* beginFrameEventName = "Frame";

/** Called when the game wants to render a new frame */
XRESULT D3D11GraphicsEngine::OnBeginFrame() {
    if ( Engine::IsShuttingDown() || !Engine::GAPI || !Device || !Context
        || !ShaderManager || !PfxRenderer || !Backbuffer || !Backbuffer->GetRenderTargetView()
        || !PerObjectMaterialInfoPooledBuffer
        || !m_FrameLimiter ) {
        return XR_FAILED;
    }
    FrameMarkStart( beginFrameEventName );

    auto& rendererState = Engine::GAPI->GetRendererState();

    if ( m_previousResolutionScalePercent < 0 ) {
        m_previousResolutionScalePercent = rendererState.RendererSettings.ResolutionScalePercent;
    }

    rendererState.RendererInfo.RenderStage = STAGE_DRAW_UNKNOWN;
    ResetFrameTransientBufferPools();

    if ( NewResolution.x > 0 && NewResolution.y > 0 && NewResolution != Resolution ) {
        if ( OnResize( NewResolution ) == XR_SUCCESS ) {
            m_previousResolutionScalePercent = rendererState.RendererSettings.ResolutionScalePercent;
        }

    } else if ( rendererState.RendererSettings.ResolutionScalePercent != m_previousResolutionScalePercent ) {
        if ( RecreateBuffers() == XR_SUCCESS ) {
            m_previousResolutionScalePercent = rendererState.RendererSettings.ResolutionScalePercent;
        }
    }

#ifdef BUILD_SPACER_NET
    rendererState.RendererSettings.EnableInactiveFpsLock = false;
#endif //  BUILD_SPACERNET
    if ( !m_isWindowActive && rendererState.RendererSettings.EnableInactiveFpsLock ) {
        m_FrameLimiter->SetLimit( 20 );
        m_FrameLimiter->Start();
    } else if ( !rendererState.RendererSettings.EnableVSync && rendererState.RendererSettings.FpsLimit != 0 ) {
        m_FrameLimiter->SetLimit( rendererState.RendererSettings.FpsLimit );
        m_FrameLimiter->Start();
    } else {
        m_FrameLimiter->Reset();
    }

    SteamOverlay::Update();
#ifdef BUILD_1_12F
    // Keep hidden windows visible on the 1.12f path.
    if ( OutputWindow && IsWindow( OutputWindow )
        && !(GetWindowLongA( OutputWindow, GWL_STYLE ) & WS_VISIBLE) ) {
        ShowWindow( OutputWindow, SW_SHOW );
    }
#endif

    // Manage deferred texture loads here
    // We don't need counting loaded mip maps because
    // gothic unlocks all mip maps only when loading is successful
    // this means we can't have half-loaded textures
    Engine::GAPI->EnterResourceCriticalSection();

    auto& stagingTextures = Engine::GAPI->GetStagingTextures();
    for ( auto& [res, texture] : stagingTextures ) {
        if ( !texture || !res.second ) {
            if ( res.second ) res.second->Release();
            if ( texture ) texture->Release();
            continue;
        }
        GetContext()->CopySubresourceRegion( texture, res.first, 0, 0, 0, res.second, 0, nullptr );
        res.second->Release();
        texture->Release();
    }
    stagingTextures.clear();

    auto& mipMaps = Engine::GAPI->GetMipMapGeneration();
    for ( D3D11Texture* texture : mipMaps ) {
        texture->GenerateMipMaps();
    }
    mipMaps.clear();

    Engine::GAPI->SetFrameProcessedTexturesReady();
    Engine::GAPI->LeaveResourceCriticalSection();

    // Notify the shader manager
    ShaderManager->OnFrameStart();

    // UI sprites use counter-clockwise winding.
    SetDefaultStates();
    rendererState.RasterizerState.CullMode = GothicRasterizerStateInfo::CM_CULL_NONE;
    rendererState.RasterizerState.SetDirty();
    UpdateRenderStates();
    GetContext()->PSSetSamplers( 0, 1, ClampSamplerState.GetAddressOf() );

    // Bind the backbuffer before the initial menu UI.

    SetViewport( ViewportInfo( 0, 0, GetBackbufferResolution() ) );
    GetContext()->OMSetRenderTargets( 1, Backbuffer->GetRenderTargetView().GetAddressOf(), nullptr);

    // Reset Render States for HUD
    Engine::GAPI->ResetRenderStates();

    SetActivePixelShader( PShaderID::PS_Simple );
    SetActiveVertexShader( VShaderID::VS_Ex );

    if ( rendererState.RendererSettings.AllowNormalmaps ) {
        Resolved_DiffuseNormalmappedFxMap = PShaderID::PS_DiffuseNormalmappedFxMap;
        Resolved_DiffuseNormalmappedAlphatestFxMap = PShaderID::PS_DiffuseNormalmappedAlphaTestFxMap;
        Resolved_DiffuseNormalmapped = PShaderID::PS_DiffuseNormalmapped;
        Resolved_DiffuseNormalmappedAlphatest = PShaderID::PS_DiffuseNormalmappedAlphaTest;
    } else {
        Resolved_DiffuseNormalmappedFxMap = PShaderID::PS_Diffuse;
        Resolved_DiffuseNormalmappedAlphatestFxMap = PShaderID::PS_DiffuseAlphaTest;
        Resolved_DiffuseNormalmapped = PShaderID::PS_Diffuse;
        Resolved_DiffuseNormalmappedAlphatest = PShaderID::PS_DiffuseAlphaTest;
    }

    return XR_SUCCESS;
}

/** Called when the game ended it's frame */
XRESULT D3D11GraphicsEngine::OnEndFrame() {
    if ( Engine::IsShuttingDown() || !Engine::GAPI || !Device || !Context || !SwapChain || !PfxRenderer
        || !PerObjectMaterialInfoPooledBuffer || !m_FrameLimiter ) {
        PresentPending = false;
        return XR_FAILED;
    }
    auto& renderInfo = Engine::GAPI->GetRendererState().RendererInfo;
    renderInfo.RenderStage = STAGE_DRAW_PRESENT;
    Present();
    EndGpuTimingFrame();

    RenderedVobs.clear();
    GetPfxRenderer()->OnEndFrame();
    ResetFrameTransientBufferPools();
    ApplyGpuVobDisplayTriangleEstimate();
    AccumulateGpuVobFrameRenderStats();
    Engine::GAPI->ResetVobFrameStats();
    PerObjectMaterialInfoPooledBuffer->EndFrame();
    FrameMarkEnd( beginFrameEventName );

    if ( !Engine::GAPI->GetRendererState().RendererSettings.BinkVideoRunning && !Engine::GAPI->IsInSavingLoadingState() ) {
        m_FrameLimiter->Wait();
    }
    return XR_SUCCESS;
}

/** Called when the game wants to clear the bound rendertarget */
XRESULT D3D11GraphicsEngine::Clear( const float4& color ) {
    const Microsoft::WRL::ComPtr<ID3D11DeviceContext1>& context = GetContext();
    if ( Engine::IsShuttingDown() || !context || !DepthStencilBuffer
        || !m_SwapchainDepthStencilBuffer || !HDRBackBuffer || !Backbuffer
        || !DepthStencilBuffer->GetDepthStencilView()
        || !m_SwapchainDepthStencilBuffer->GetDepthStencilView()
        || !HDRBackBuffer->GetRenderTargetView() || !Backbuffer->GetRenderTargetView() ) {
        return XR_FAILED;
    }
    context->ClearDepthStencilView( DepthStencilBuffer->GetDepthStencilView().Get(), D3D11_CLEAR_DEPTH, 0, 0 );
    context->ClearDepthStencilView( m_SwapchainDepthStencilBuffer->GetDepthStencilView().Get(), D3D11_CLEAR_DEPTH, 0, 0 );

    const float clearColor[4] = { 0.f, 0.f, 0.f, 0.f };
    context->ClearRenderTargetView( HDRBackBuffer->GetRenderTargetView().Get(), clearColor );
    context->ClearRenderTargetView( Backbuffer->GetRenderTargetView().Get(), clearColor );

    return XR_SUCCESS;
}

/** Fetches a list of available display modes */
XRESULT D3D11GraphicsEngine::FetchDisplayModeList() {
#pragma warning(push)
#pragma warning(disable: 6320)
    // First try to get display resolutions through DXGI
    // if it for some reason fails get resolutions through WinApi
    __try {
        XRESULT result = FetchDisplayModeListDXGI();
        if ( result == XR_FAILED || CachedDisplayModes.size() <= 1 ) {
            CachedDisplayModes.clear();
            result = FetchDisplayModeListWindows();
        }
        return result;
    } __except ( EXCEPTION_EXECUTE_HANDLER ) {
        return FetchDisplayModeListWindows();
    }
#pragma warning(pop)
}

XRESULT D3D11GraphicsEngine::FetchDisplayModeListDXGI() {
    if ( !DXGIAdapter2 ) {
        CachedDisplayModes.emplace_back( Resolution.x, Resolution.y );
        return XR_FAILED;
    }

    Microsoft::WRL::ComPtr<IDXGIOutput> output11;
    Microsoft::WRL::ComPtr<IDXGIOutput1> output;

    DXGIAdapter2->EnumOutputs( 0, output11.GetAddressOf() );
    HRESULT hr = output11.As( &output );
    if ( !output.Get() || FAILED( hr ) ) {
        CachedDisplayModes.emplace_back( Resolution.x, Resolution.y );
        return XR_FAILED;
    }

    UINT numModes = 0;
    hr = output->GetDisplayModeList1( DXGI_FORMAT_ENGINE_SWAPCHAIN , 0, &numModes, nullptr );
    if ( FAILED( hr ) || numModes == 0 ) {
        CachedDisplayModes.emplace_back( Resolution.x, Resolution.y );
        return XR_FAILED;
    }

    std::unique_ptr<DXGI_MODE_DESC1[]> displayModes = std::make_unique<DXGI_MODE_DESC1[]>( numModes );
    hr = output->GetDisplayModeList1( DXGI_FORMAT_ENGINE_SWAPCHAIN , 0, &numModes, displayModes.get() );
    if ( FAILED( hr ) ) {
        CachedDisplayModes.emplace_back( Resolution.x, Resolution.y );
        return XR_FAILED;
    }

    DEVMODEA devMode = {};
    devMode.dmSize = sizeof( DEVMODEA );
    DWORD currentRefreshRate = 0;
    if ( EnumDisplaySettingsA( nullptr, ENUM_CURRENT_SETTINGS, &devMode ) ) {
        currentRefreshRate = devMode.dmDisplayFrequency;
    }

    CachedDisplayModes.reserve( numModes );
    for ( UINT i = 0; i < numModes; i++ ) 	{
        DXGI_MODE_DESC1& displayMode = displayModes[i];
        if ( static_cast<UINT>(Resolution.x) == displayMode.Width && static_cast<UINT>(Resolution.y) == displayMode.Height ) {
            DWORD displayRefreshRate = static_cast<DWORD>(displayMode.RefreshRate.Numerator / displayMode.RefreshRate.Denominator);
            if ( currentRefreshRate >= (displayRefreshRate - 2) && currentRefreshRate <= (displayRefreshRate + 2) ) {
                CachedRefreshRate.Numerator = displayMode.RefreshRate.Numerator;
                CachedRefreshRate.Denominator = displayMode.RefreshRate.Denominator;
            }
        }

        if ( displayMode.Width >= 800 && displayMode.Height >= 600 ) {
            DisplayModeInfo info( static_cast<int>(displayMode.Width), static_cast<int>(displayMode.Height) );
            auto it = std::find_if( CachedDisplayModes.begin(), CachedDisplayModes.end(),
                [&info]( DisplayModeInfo& a ) { return (a.Width == info.Width && a.Height == info.Height); } );
            if ( it == CachedDisplayModes.end() ) {
                CachedDisplayModes.push_back( info );
            }
        }
    }
    CachedDisplayModes.shrink_to_fit();
    return XR_SUCCESS;
}

XRESULT D3D11GraphicsEngine::FetchDisplayModeListWindows() {
    for ( DWORD i = 0;; ++i ) {
        DEVMODEA devmode = {};
        devmode.dmSize = sizeof( DEVMODEA );
        devmode.dmDriverExtra = 0;
        if ( !EnumDisplaySettingsA( nullptr, i, &devmode ) || (devmode.dmFields & DM_BITSPERPEL) != DM_BITSPERPEL )
            break;

        if ( devmode.dmBitsPerPel < 24 )
            continue;

        if ( devmode.dmPelsWidth >= 800 && devmode.dmPelsHeight >= 600 ) {
            DisplayModeInfo info( static_cast<int>(devmode.dmPelsWidth), static_cast<int>(devmode.dmPelsHeight) );
            auto it = std::find_if( CachedDisplayModes.begin(), CachedDisplayModes.end(),
                [&info]( DisplayModeInfo& a ) { return (a.Width == info.Width && a.Height == info.Height); } );
            if ( it == CachedDisplayModes.end() ) {
                CachedDisplayModes.push_back( info );
            }
        }
    }
    return XR_SUCCESS;
}

/** Returns a list of available display modes */
XRESULT
D3D11GraphicsEngine::GetDisplayModeList( std::vector<DisplayModeInfo>* modeList,
    bool includeSuperSampling ) {
    modeList->reserve( CachedDisplayModes.size() );
    for ( DisplayModeInfo& mode : CachedDisplayModes ) {
        modeList->push_back( mode );
    }
    if ( includeSuperSampling ) {
        // Put supersampling resolutions in, up to just below 8k
        int i = 2;
        DisplayModeInfo ssBase = modeList->back();
        while ( ssBase.Width * i < 8192 && ssBase.Height * i < 8192 ) {
            DisplayModeInfo info( static_cast<int>(ssBase.Width * i), static_cast<int>(ssBase.Height * i) );
            modeList->push_back( info );
            ++i;
        }
    }

    return XR_SUCCESS;
}

void RenderVelocity(D3D11GraphicsEngine* engine,
    const GothicRendererSettings& settings,
    const ComPtr<ID3D11RenderTargetView>& rtv)
{
    auto ps = engine->GetShaderManager().GetPShader(PShaderID::PS_PFX_VelocityDebug);

    VelocityDebugConstantBuffer cb = {};
    cb.Amplification = 100;

    ps->GetBuffer( "VelocityDebugCB" ).Update( &cb ).Bind();
    ps->Apply();

    engine->GetPfxRenderer()->CopyTextureToRTV(
        engine->GetVelocityBuffer()->GetShaderResView(),
        rtv,
        engine->GetBackbufferResolution(),
        /* useCustomPS: */ true);
}

/** Presents the current frame to the screen */
XRESULT D3D11GraphicsEngine::Present() {
    ZoneScoped;
    auto failPresent = [this]() {
        PresentPending = false;
        TracyD3D11Collect( s_tracyD3D11Ctx );
        return XR_FAILED;
    };

    if ( Engine::IsShuttingDown() || !Engine::GAPI || !Device || !Context || !SwapChain
        || !PfxRenderer || !ShaderManager || !Backbuffer || !BackbufferRTV
        || !Backbuffer->GetShaderResView() || !Backbuffer->GetRenderTargetView()
        || !DepthStencilBuffer || !DepthStencilBufferCopy
        || !DepthStencilBuffer->GetTexture() || !DepthStencilBufferCopy->GetTexture()
        || !m_FrameLimiter
        || m_swapchainResolution.x <= 0 || m_swapchainResolution.y <= 0
        || GetBackbufferResolution().x <= 0 || GetBackbufferResolution().y <= 0 ) {
        return failPresent();
    }

    const auto& settings = Engine::GAPI->GetRendererState().RendererSettings;

    INT2 presentContentSize = m_swapchainResolution;
    INT2 presentContentOffset( 0, 0 );
    const INT2 logicalFrameSize = GetBackbufferResolution();
    const bool aspectFitPresentation =
        logicalFrameSize.x != m_swapchainResolution.x
        || logicalFrameSize.y != m_swapchainResolution.y;

    if ( aspectFitPresentation ) {
        const float sourceAspect = static_cast<float>(logicalFrameSize.x) / static_cast<float>(logicalFrameSize.y);
        const float targetAspect = static_cast<float>(m_swapchainResolution.x) / static_cast<float>(m_swapchainResolution.y);
        if ( targetAspect > sourceAspect ) {
            presentContentSize.y = m_swapchainResolution.y;
            presentContentSize.x = std::max( 1, static_cast<int>(std::lround(
                static_cast<float>(presentContentSize.y) * sourceAspect )) );
            presentContentOffset.x = (m_swapchainResolution.x - presentContentSize.x) / 2;
        } else if ( targetAspect < sourceAspect ) {
            presentContentSize.x = m_swapchainResolution.x;
            presentContentSize.y = std::max( 1, static_cast<int>(std::lround(
                static_cast<float>(presentContentSize.x) / sourceAspect )) );
            presentContentOffset.y = (m_swapchainResolution.y - presentContentSize.y) / 2;
        }
    }

    TextureHandle fittedPresentation;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> presentationRTV = BackbufferRTV;
    if ( aspectFitPresentation ) {
        fittedPresentation = PfxRenderer->GetBackbufferTempBuffer();
        if ( !fittedPresentation || !fittedPresentation->GetRenderTargetView() ) {
            return failPresent();
        }
        presentationRTV = fittedPresentation->GetRenderTargetView();
    }

    if ( !presentationRTV ) {
        return failPresent();
    }

    SetViewport( ViewportInfo( 0, 0, GetBackbufferResolution() ) );
    SetDefaultStates();
    UpdateRenderStates();

    {
        auto _ = RecordGraphicsEvent( GE_NAME( "Blit onto Swapchain" ) );
        SetActivePixelShader( PShaderID::PS_PFX_GammaCorrectInv );
        if ( !ActivePS ) {
            return failPresent();
        }
        ActivePS->Apply();

        GammaCorrectConstantBuffer gcb = {};
        gcb.G_Gamma = Engine::GAPI->GetGammaValue();
        gcb.G_Brightness = Engine::GAPI->GetBrightnessValue();
        // Dither exactly once immediately before the final 8-bit presentation.
        gcb.G_OutputDitherStrength = 1.0f / 255.0f;
        ActivePS->GetBuffer( "GammaCorrectConstantBuffer" ).Update( &gcb ).Bind();

        PfxRenderer->CopyTextureToRTV( Backbuffer->GetShaderResView(), presentationRTV, {}, true );

        static int show_velocity = 0;
        if ( settings.DebugSettings.Velocity.DisplayVelocity || show_velocity == 2 ) {
            RenderVelocity( this, settings, presentationRTV );
        }

        GetContext()->OMSetRenderTargets( 1, presentationRTV.GetAddressOf(), nullptr );
    }

    if ( Engine::ImGuiHandle ) {
        SetDefaultStates();
        UpdateRenderStates();
        if ( Engine::ImGuiHandle->Initiated ) {
            Engine::ImGuiHandle->RenderLoop();
        }
    }

    if ( aspectFitPresentation ) {
        // ImGui is part of the logical frame. Scale that completed frame uniformly
        // into the physical borderless swapchain and keep the unused area black.
        ID3D11ShaderResourceView* nullPresentationSRV = nullptr;
        GetContext()->PSSetShaderResources( 0, 1, &nullPresentationSRV );
        GetContext()->OMSetRenderTargets( 0, nullptr, nullptr );
        SetDefaultStates();
        UpdateRenderStates();
        const float black[4] = {};
        GetContext()->ClearRenderTargetView( BackbufferRTV.Get(), black );
        PfxRenderer->CopyTextureToRTV(
            fittedPresentation->GetShaderResView(), BackbufferRTV,
            presentContentSize, false, presentContentOffset );
        GetContext()->OMSetRenderTargets( 1, BackbufferRTV.GetAddressOf(), nullptr );
    }

    GetContext()->CopyResource(
        DepthStencilBuffer->GetTexture().Get(),
        DepthStencilBufferCopy->GetTexture().Get() );

    if ( Engine::GAPI->GetMainThreadID() != GetCurrentThreadId() ) {
        GetContext()->Flush();
        PresentPending = false;
        return XR_SUCCESS;
    }

    bool vsync = settings.EnableVSync;
    if ( settings.BinkVideoRunning || Engine::GAPI->IsInSavingLoadingState() ) {
        vsync = false;
    }

    UINT presentFlags = 0u;
    if ( m_flipWithTearing && !vsync ) {
        presentFlags |= DXGI_PRESENT_ALLOW_TEARING;
    }
    HRESULT hr = SwapChain->Present( vsync ? 1u : 0u, presentFlags );

    if ( hr == DXGI_ERROR_DEVICE_REMOVED ) {
        switch ( GetDevice()->GetDeviceRemovedReason() ) {
        case DXGI_ERROR_DEVICE_HUNG:
            LogErrorBox() << "Device Removed! (DXGI_ERROR_DEVICE_HUNG)";
            exit( 0 );
            break;
        case DXGI_ERROR_DEVICE_REMOVED:
            LogErrorBox() << "Device Removed! (DXGI_ERROR_DEVICE_REMOVED)";
            exit( 0 );
            break;
        case DXGI_ERROR_DEVICE_RESET:
            LogErrorBox() << "Device Removed! (DXGI_ERROR_DEVICE_RESET)";
            exit( 0 );
            break;
        case DXGI_ERROR_DRIVER_INTERNAL_ERROR:
            LogErrorBox() << "Device Removed! (DXGI_ERROR_DRIVER_INTERNAL_ERROR)";
            exit( 0 );
            break;
        case DXGI_ERROR_INVALID_CALL:
            LogErrorBox() << "Device Removed! (DXGI_ERROR_INVALID_CALL)";
            exit( 0 );
            break;
        case S_OK:
            LogInfo() << "Device removed, but we're fine!";
            break;
        default:
            LogWarnBox() << "Device Removed! (Unknown reason)";
        }
    } else if ( FAILED( hr ) ) {
        LogError() << "SwapChain::Present failed! HRESULT: " << std::hex << hr;
        PresentPending = false;
        TracyD3D11Collect( s_tracyD3D11Ctx );
        return XR_FAILED;
    } else if ( frameLatencyWaitableObject ) {
        ZoneScopedN( "Present::frameLatencyWaitableObject" );
        DWORD waitResult = WAIT_IO_COMPLETION;
        while ( waitResult == WAIT_IO_COMPLETION ) {
            waitResult = WaitForSingleObjectEx( frameLatencyWaitableObject, INFINITE, true );
        }
        if ( waitResult == WAIT_FAILED ) {
            LogError() << "Frame-latency wait failed after Present. GetLastError: " << GetLastError();
        } else if ( waitResult != WAIT_OBJECT_0 ) {
            LogWarn() << "Unexpected frame-latency wait result after Present: " << waitResult;
        }
    }

    PresentPending = false;
    TracyD3D11Collect( s_tracyD3D11Ctx );
    return XR_SUCCESS;
}

/** Called to set the current viewport */
XRESULT D3D11GraphicsEngine::SetViewport( const ViewportInfo& viewportInfo ) {
    if ( !Context || Engine::IsShuttingDown() ) {
        return XR_FAILED;
    }
    // Set the viewport
    D3D11_VIEWPORT viewport = {};

    viewport.TopLeftX = static_cast<float>(viewportInfo.TopLeftX);
    viewport.TopLeftY = static_cast<float>(viewportInfo.TopLeftY);
    viewport.Width = static_cast<float>(viewportInfo.Width);
    viewport.Height = static_cast<float>(viewportInfo.Height);
    viewport.MinDepth = viewportInfo.MinZ;
    viewport.MaxDepth = viewportInfo.MaxZ;

    GetContext()->RSSetViewports( 1, &viewport );

    return XR_SUCCESS;
}

/** Draws a vertexbuffer, non-indexed */
XRESULT D3D11GraphicsEngine::DrawVertexBuffer( D3D11VertexBuffer* vb, unsigned int numVertices, unsigned int stride ) {
#ifdef RECORD_LAST_DRAWCALL
    g_LastDrawCall.Type = DrawcallInfo::VB;
    g_LastDrawCall.NumElements = numVertices;
    g_LastDrawCall.BaseVertexLocation = 0;
    g_LastDrawCall.BaseIndexLocation = 0;
    if ( !g_LastDrawCall.Check() ) return XR_SUCCESS;
#endif

    UINT offset = 0;
    UINT uStride = stride;
    Context->IASetVertexBuffers( 0, 1, vb->GetVertexBuffer().GetAddressOf(), &uStride, &offset );

    // Draw the mesh
    Context->Draw( numVertices, 0 );

    Engine::GAPI->GetRendererState().RendererInfo.FrameDrawnTriangles +=
        numVertices / 3;

    return XR_SUCCESS;
}

XRESULT D3D11GraphicsEngine::DrawVertexBufferIndexed( D3D11VertexBuffer* vb,
    D3D11VertexBuffer* ib,
    unsigned int numIndices,
    unsigned int indexOffset ) {
#ifdef RECORD_LAST_DRAWCALL
    g_LastDrawCall.Type = DrawcallInfo::VB_IX;
    g_LastDrawCall.NumElements = numIndices;
    g_LastDrawCall.BaseVertexLocation = 0;
    g_LastDrawCall.BaseIndexLocation = indexOffset;
    if ( !g_LastDrawCall.Check() ) return XR_SUCCESS;
#endif

    if ( vb ) {
        UINT offset = 0;
        UINT uStride = sizeof( ExVertexStruct );
        Context->IASetVertexBuffers( 0, 1, vb->GetVertexBuffer().GetAddressOf(), &uStride, &offset );

        Context->IASetIndexBuffer( ib->GetVertexBuffer().Get(), VERTEX_INDEX_DXGI_FORMAT, 0 );
    }

    if ( numIndices ) {
        // Draw the mesh
        Context->DrawIndexed( numIndices, indexOffset, 0 );

        Engine::GAPI->GetRendererState().RendererInfo.FrameDrawnTriangles +=
            numIndices / 3;
    }
    return XR_SUCCESS;
}

XRESULT D3D11GraphicsEngine::DrawVertexBufferIndexedUINT(
    D3D11VertexBuffer* vb, D3D11VertexBuffer* ib, unsigned int numIndices,
    unsigned int indexOffset ) {
#ifdef RECORD_LAST_DRAWCALL
    g_LastDrawCall.Type = DrawcallInfo::VB_IX_UINT;
    g_LastDrawCall.NumElements = numIndices;
    g_LastDrawCall.BaseVertexLocation = 0;
    g_LastDrawCall.BaseIndexLocation = indexOffset;
    if ( !g_LastDrawCall.Check() ) return XR_SUCCESS;
#endif

    if ( vb ) {
        UINT offset = 0;
        UINT uStride = sizeof( ExVertexStruct );
        Context->IASetVertexBuffers( 0, 1, vb->GetVertexBuffer().GetAddressOf(), &uStride, &offset );
        Context->IASetIndexBuffer( ib->GetVertexBuffer().Get(), DXGI_FORMAT_R32_UINT, 0 );
    }

    if ( numIndices ) {
        // Draw the mesh
        Context->DrawIndexed( numIndices, indexOffset, 0 );

        Engine::GAPI->GetRendererState().RendererInfo.FrameDrawnTriangles +=
            numIndices / 3;
    }

    return XR_SUCCESS;
}

XRESULT D3D11GraphicsEngine::DrawDynamicVertexBufferIndexed(std::vector<ExVertexStruct>& vertices,
    D3D11VertexBuffer* ib, unsigned int numIndices, unsigned int indexOffset)
{
    const size_t requiredSize = std::max(vertices.size(), size_t(200)) * sizeof( ExVertexStruct );
    if ( !DynamicVertexBuffer || DynamicVertexBuffer->GetSizeInBytes() < requiredSize ) {
        DynamicVertexBuffer.reset( new D3D11VertexBuffer );
        DynamicVertexBuffer->Init(nullptr, requiredSize,
                D3D11VertexBuffer::B_VERTEXBUFFER, D3D11VertexBuffer::U_DYNAMIC,
                D3D11VertexBuffer::CA_WRITE );
    }
    DynamicVertexBuffer->UpdateBuffer( vertices.data(), vertices.size() * sizeof( ExVertexStruct ) );

    return DrawVertexBufferIndexed( DynamicVertexBuffer.get(), ib, numIndices, indexOffset );
}

/** Draws a vertexbuffer, instanced */
XRESULT D3D11GraphicsEngine::DrawVertexBufferInstanced(
    D3D11VertexBuffer* vb, unsigned int numVertices,
    unsigned int numInstances, unsigned int stride ) {
#ifdef RECORD_LAST_DRAWCALL
    g_LastDrawCall.Type = DrawcallInfo::VB;
    g_LastDrawCall.NumElements = numVertices;
    g_LastDrawCall.BaseVertexLocation = 0;
    g_LastDrawCall.BaseIndexLocation = 0;
    if ( !g_LastDrawCall.Check() ) return XR_SUCCESS;
#endif

    UINT offset = 0;
    UINT uStride = stride;
    Context->IASetVertexBuffers( 0, 1, vb->GetVertexBuffer().GetAddressOf(), &uStride, &offset );

    // Draw the mesh
    Context->DrawInstanced( numVertices, numInstances, 0, 0 );

    Engine::GAPI->GetRendererState().RendererInfo.FrameDrawnTriangles +=
        numVertices / 3;

    return XR_SUCCESS;
}

XRESULT D3D11GraphicsEngine::DrawVertexBufferInstancedIndexed(
    D3D11VertexBuffer* vb, D3D11VertexBuffer* ib,
    unsigned int numIndices, unsigned int numInstances,
    unsigned int indexOffset ) {
#ifdef RECORD_LAST_DRAWCALL
    g_LastDrawCall.Type = DrawcallInfo::VB_IX;
    g_LastDrawCall.NumElements = numIndices;
    g_LastDrawCall.BaseVertexLocation = 0;
    g_LastDrawCall.BaseIndexLocation = indexOffset;
    if ( !g_LastDrawCall.Check() ) return XR_SUCCESS;
#endif

    if ( vb ) {
        UINT offset = 0;
        UINT uStride = sizeof( ExVertexStruct );
        Context->IASetVertexBuffers( 0, 1, vb->GetVertexBuffer().GetAddressOf(), &uStride, &offset );

        Context->IASetIndexBuffer( ib->GetVertexBuffer().Get(), VERTEX_INDEX_DXGI_FORMAT, 0 );
    }

    if ( numIndices ) {
        // Draw the mesh
        Context->DrawIndexedInstanced( numIndices, numInstances, indexOffset, 0, 0 );

        Engine::GAPI->GetRendererState().RendererInfo.FrameDrawnTriangles +=
            numIndices / 3;
    }
    return XR_SUCCESS;
}

XRESULT D3D11GraphicsEngine::DrawVertexBufferInstancedIndexedUINT(
    D3D11VertexBuffer* vb, D3D11VertexBuffer* ib, unsigned int numIndices,
    unsigned int numInstances, unsigned int indexOffset ) {
#ifdef RECORD_LAST_DRAWCALL
    g_LastDrawCall.Type = DrawcallInfo::VB_IX_UINT;
    g_LastDrawCall.NumElements = numIndices;
    g_LastDrawCall.BaseVertexLocation = 0;
    g_LastDrawCall.BaseIndexLocation = indexOffset;
    if ( !g_LastDrawCall.Check() ) return XR_SUCCESS;
#endif

    if ( vb ) {
        UINT offset = 0;
        UINT uStride = sizeof( ExVertexStruct );
        Context->IASetVertexBuffers( 0, 1, vb->GetVertexBuffer().GetAddressOf(), &uStride, &offset );
        Context->IASetIndexBuffer( ib->GetVertexBuffer().Get(), DXGI_FORMAT_R32_UINT, 0 );
    }

    if ( numIndices ) {
        // Draw the mesh
        Context->DrawIndexedInstanced( numIndices, numInstances, indexOffset, 0, 0 );

        Engine::GAPI->GetRendererState().RendererInfo.FrameDrawnTriangles +=
            numIndices / 3;
    }

    return XR_SUCCESS;
}

/** Binds viewport information to the given constantbuffer slot */
XRESULT D3D11GraphicsEngine::BindViewportInformation( VShaderID shader,
    int slot ) {
    D3D11_VIEWPORT vp;
    UINT num = 1;
    GetContext()->RSGetViewports( &num, &vp );

    // Update viewport information
    float scale =
        Engine::GAPI->GetRendererState().RendererSettings.GothicUIScale;
    Temp2Float2[0].x = vp.TopLeftX / scale;
    Temp2Float2[0].y = vp.TopLeftY / scale;
    Temp2Float2[1].x = vp.Width / scale;
    Temp2Float2[1].y = vp.Height / scale;

    auto vs = ShaderManager->GetVShader( shader );

    if ( vs ) {
        vs->GetBuffer( "Viewport" ).Update( Temp2Float2 ).Bind();
    }

    return XR_SUCCESS;
}

/** Draws a screen fade effects */
XRESULT D3D11GraphicsEngine::DrawScreenFade( void* c ) {
    zCCamera* camera = reinterpret_cast<zCCamera*>(c);

    bool ResetStates = false;
    if ( camera->HasCinemaScopeEnabled() ) {
        camera->ResetCinemaScopeEnabled();
        ResetStates = true;

        zColor cinemaScopeColor = camera->GetCinemaScopeColor();

        // Default states
        SetDefaultStates();
        Engine::GAPI->GetRendererState().BlendState.SetAlphaBlending();
        Engine::GAPI->GetRendererState().BlendState.SetDirty();
        Engine::GAPI->GetRendererState().DepthState.DepthBufferCompareFunc = GothicDepthBufferStateInfo::CF_COMPARISON_ALWAYS;
        Engine::GAPI->GetRendererState().DepthState.DepthWriteEnabled = false;
        Engine::GAPI->GetRendererState().DepthState.SetDirty();

        SetActivePixelShader( PShaderID::PS_PFX_CinemaScope );
        ActivePS->Apply();

        SetActiveVertexShader( VShaderID::VS_CinemaScope );
        ActiveVS->Apply();

        ScreenFadeConstantBuffer colorBuffer;
        colorBuffer.GA_Alpha = cinemaScopeColor.bgra.alpha * inv255f;
        colorBuffer.GA_Pad.x = cinemaScopeColor.bgra.r * inv255f;
        colorBuffer.GA_Pad.y = cinemaScopeColor.bgra.g * inv255f;
        colorBuffer.GA_Pad.z = cinemaScopeColor.bgra.b * inv255f;
        ActivePS->GetBuffer( "AlphaBlendInfo" ).Update( &colorBuffer ).Bind();

        UpdateRenderStates();
        GetContext()->IASetPrimitiveTopology( D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST );
        GetContext()->Draw( 12, 0 );
    }

    if ( camera->HasScreenFadeEnabled() ) {
        camera->ResetScreenFadeEnabled();
        ResetStates = true;

        bool haveTexture = true;
        zCMaterial* material = reinterpret_cast<zCMaterial*>(camera->GetPolyMaterial());
        if ( zCTexture* texture = material->GetAniTexture() ) {
            if ( texture->CacheIn( 0.6f ) == zRES_CACHED_IN )
                texture->Bind( 0 );
            else
                goto Continue_ResetState;
        }
        else
            haveTexture = false;

        zColor screenFadeColor = camera->GetScreenFadeColor();

        // Default states
        SetDefaultStates();
        switch ( camera->GetScreenFadeBlendFunc() ) {
            case zRND_ALPHA_FUNC_BLEND:
            case zRND_ALPHA_FUNC_BLEND_TEST:
            case zRND_ALPHA_FUNC_SUB: {
                Engine::GAPI->GetRendererState().BlendState.SetAlphaBlending();
                Engine::GAPI->GetRendererState().BlendState.SetDirty();
                break;
            }
            case zRND_ALPHA_FUNC_ADD: {
                Engine::GAPI->GetRendererState().BlendState.SetAdditiveBlending();
                Engine::GAPI->GetRendererState().BlendState.SetDirty();
                break;
            }
            case zRND_ALPHA_FUNC_MUL: {
                Engine::GAPI->GetRendererState().BlendState.SetModulateBlending();
                Engine::GAPI->GetRendererState().BlendState.SetDirty();
                break;
            }
            case zRND_ALPHA_FUNC_MUL2: {
                Engine::GAPI->GetRendererState().BlendState.SetModulate2Blending();
                Engine::GAPI->GetRendererState().BlendState.SetDirty();
                break;
            }
        }
        Engine::GAPI->GetRendererState().DepthState.DepthBufferCompareFunc = GothicDepthBufferStateInfo::CF_COMPARISON_ALWAYS;
        Engine::GAPI->GetRendererState().DepthState.DepthWriteEnabled = false;
        Engine::GAPI->GetRendererState().DepthState.SetDirty();

        if ( haveTexture )
            SetActivePixelShader( PShaderID::PS_PFX_Alpha_Blend );
        else
            SetActivePixelShader( PShaderID::PS_PFX_CinemaScope );

        ActivePS->Apply();

        SetActiveVertexShader( VShaderID::VS_PFX );
        ActiveVS->Apply();

        ScreenFadeConstantBuffer colorBuffer;
        colorBuffer.GA_Alpha = screenFadeColor.bgra.alpha * inv255f;
        colorBuffer.GA_Pad.x = screenFadeColor.bgra.r * inv255f;
        colorBuffer.GA_Pad.y = screenFadeColor.bgra.g * inv255f;
        colorBuffer.GA_Pad.z = screenFadeColor.bgra.b * inv255f;
        ActivePS->GetBuffer( "AlphaBlendInfo" ).Update( &colorBuffer ).Bind();

        PfxRenderer->DrawFullScreenQuad();
    }

    Continue_ResetState:
    if ( ResetStates ) {
        // UI sprites use counter-clockwise winding.
        SetDefaultStates();
        Engine::GAPI->GetRendererState().RasterizerState.CullMode = GothicRasterizerStateInfo::CM_CULL_NONE;
        Engine::GAPI->GetRendererState().RasterizerState.SetDirty();
        UpdateRenderStates();
    }
    return XR_SUCCESS;
}

/** Draws a vertexarray, non-indexed (HUD, 2D)*/
XRESULT D3D11GraphicsEngine::DrawVertexArray( ExVertexStruct* vertices,
    unsigned int numVertices,
    unsigned int startVertex,
    unsigned int stride ) {
    UpdateRenderStates();
    auto vShader = ActiveVS;
    // Bind the FF-Info to the first PS slot
    ActivePS->GetBuffer( "FFPipelineConstantBuffer" )
        .Update( &Engine::GAPI->GetRendererState().GraphicsState )
        .Bind();

    SetupVS_ExMeshDrawCall();

    EnsureTempVertexBufferSize( TempHUDVertexBuffer, stride * numVertices );
    TempHUDVertexBuffer->UpdateBuffer( vertices, stride * numVertices );

    UINT offset = 0;
    UINT uStride = stride;
    GetContext()->IASetVertexBuffers( 0, 1, TempHUDVertexBuffer->GetVertexBuffer().GetAddressOf(), &uStride, &offset );

    // Draw the mesh
    GetContext()->Draw( numVertices, startVertex );

    Engine::GAPI->GetRendererState().RendererInfo.FrameDrawnTriangles +=
        numVertices / 3;

    return XR_SUCCESS;
}

/** Draws a vertexarray, indexed */
XRESULT D3D11GraphicsEngine::DrawIndexedVertexArray( ExVertexStruct* vertices,
    unsigned int numVertices,
    D3D11VertexBuffer* ib,
    unsigned int numIndices,
    unsigned int stride ) {

    UpdateRenderStates();
    auto vShader = ActiveVS;

    // Bind the FF-Info to the first PS slot
    ActivePS->GetBuffer( "FFPipelineConstantBuffer" )
        .Update( &Engine::GAPI->GetRendererState().GraphicsState )
        .Bind();

    SetupVS_ExMeshDrawCall();

    D3D11_BUFFER_DESC desc;
    TempVertexBuffer->GetVertexBuffer()->GetDesc( &desc );

    EnsureTempVertexBufferSize( TempVertexBuffer, stride * numVertices );
    TempVertexBuffer->UpdateBuffer( vertices, stride * numVertices );

    UINT offset = 0;
    UINT uStride = stride;
    ID3D11Buffer* buffers[2] = {
        TempVertexBuffer->GetVertexBuffer().Get(),
        ib->GetVertexBuffer().Get(),
    };
    GetContext()->IASetVertexBuffers( 0, 2, buffers, &uStride, &offset );

    // Draw the mesh
    GetContext()->DrawIndexed( numIndices, 0, 0 );

    Engine::GAPI->GetRendererState().RendererInfo.FrameDrawnTriangles +=
        numVertices / 3;

    return XR_SUCCESS;
}

/** Draws a vertexbuffer, non-indexed, binding the FF-Pipe values */
XRESULT D3D11GraphicsEngine::DrawVertexBufferFF( D3D11VertexBuffer* vb,
    unsigned int numVertices,
    unsigned int startVertex,
    unsigned int stride ) {
    SetupVS_ExMeshDrawCall();

    // Bind the FF-Info to the first PS slot
    ActivePS->GetBuffer( "FFPipelineConstantBuffer" )
        .Update( &Engine::GAPI->GetRendererState().GraphicsState )
        .Bind();

    UINT offset = 0;
    UINT uStride = stride;
    GetContext()->IASetVertexBuffers( 0, 1, vb->GetVertexBuffer().GetAddressOf(), &uStride, &offset );

    // Draw the mesh
    GetContext()->Draw( numVertices, startVertex );

    Engine::GAPI->GetRendererState().RendererInfo.FrameDrawnTriangles +=
        numVertices / 3;

    return XR_SUCCESS;
}

/** Sets up texture with normalmap and fxmap for rendering */
bool D3D11GraphicsEngine::BindTextureNRFX( zCTexture* tex, bool bindShader, bool updateMaterialInfo, float materialClassMarker ) {
    if ( tex->CacheIn( 0.6f ) != zRES_CACHED_IN ) {
        return false;
    }

    ID3D11ShaderResourceView* srvs[4] = {
        tex->GetSurface()->GetEngineTexture()->GetShaderResourceView().Get(),
        nullptr,
        nullptr,
        nullptr,
    };

    // Select shader
    if ( bindShader ) {
        BindShaderForTexture( tex );
    }

    MaterialInfo* info = nullptr;
    if ( updateMaterialInfo ) {
        info = Engine::GAPI->GetMaterialInfoFrom( tex );

        if ( info->buffer.SpecularIntensity != 0.05f ) {
            info->buffer.SpecularIntensity = 0.05f;
        }
    }

    // Bind a normalmap only when the material explicitly allows one and a real map exists.
    srvs[1] = GetMaterialNormalmapSRV( tex->GetSurface(), info );

    if ( info && GetActivePS() ) {
        auto materialBuffer = GetEffectiveMaterialBuffer( info, tex->GetSurface() );
        if ( materialClassMarker != 0.0f ) {
            materialBuffer.Color.w = materialClassMarker;
        }
        auto allocation = PerObjectMaterialInfoPooledBuffer->Allocate( GetContext().Get(), &materialBuffer, sizeof( materialBuffer ) );
        UINT firstConstant = allocation.offsetInBytes / 16;
        UINT numConstants = allocation.sizeInBytes / 16;
        GetContext()->PSSetConstantBuffers1( 2, 1, &allocation.pBuffer, &firstConstant, &numConstants );
    }

    if ( D3D11Texture* fxmap = tex->GetSurface()->GetFxMap() ) {
        srvs[2] = fxmap->GetShaderResourceView().Get();
        fxmap->BindToPixelShader( 2 );
    }
    srvs[3] = GetParallaxDisplacementSRV( tex->GetSurface(), info );
    GetContext()->PSSetShaderResources( 0, 3, srvs );
    GetContext()->PSSetShaderResources( 13, 1, &srvs[3] );



    return true;
}

XRESULT  D3D11GraphicsEngine::DrawSkeletalVertexNormals( SkeletalVobInfo* vi,
    const XMFLOAT4X4& world,
    const std::span<XMFLOAT4X4> transforms, float4 color, float fatness ) {
    std::shared_ptr<D3D11GShader> gshader = ShaderManager->GetGShader( GShaderID::GS_VertexNormals );
    gshader->Apply();

    SetActiveVertexShader( VShaderID::VS_ExSkeletalVN );
    SetActivePixelShader( PShaderID::PS_Simple );

    InfiniteRangeConstantBuffer->BindToPixelShader( 3 );

    SetupVS_ExMeshDrawCall();
    SetupVS_ExConstantBuffer();

    GetContext()->IASetPrimitiveTopology( D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST );

    VS_ExConstantBuffer_PerInstanceSkeletal cb2;
    cb2.World = world;
    color.w = (vi && vi->Vob && vi->Vob->IsIndoorVob()) ? 0.05f : 1.0f;
    cb2.PI_ModelColor = color;
    cb2.PI_ModelFatness = fatness;

    auto perInstanceBuf = ActiveVS->GetBuffer( "Matrices_PerInstances" );
    perInstanceBuf.Update( &cb2 ).Bind();
    perInstanceBuf.GetRawBuffer()->BindToGeometryShader( 1 );

    bool useStructuredBones = !FeatureLevel10Compatibility;
    if ( useStructuredBones ) {
        const std::vector<XMFLOAT4X4> packedCurrent( transforms.begin(), transforms.begin() + std::min<size_t>( transforms.size(), NUM_MAX_BONES ) );
        if ( !UploadStructuredMatrixBuffer( SkeletalBoneTransformsBufferTransient, packedCurrent, "SkeletalBoneTransformsBufferTransient" )
            || !SkeletalBoneTransformsBufferTransient
            || !SkeletalBoneTransformsBufferTransient->GetShaderResourceView().Get() ) {
            useStructuredBones = false;
        } else {
            ActiveVS->BindResource( "BoneTransforms", SkeletalBoneTransformsBufferTransient->GetShaderResourceView().Get() );
            VS_ExConstantBuffer_SkeletalBoneRange range = {};
            range.BoneCount = static_cast<unsigned int>(packedCurrent.size());
            range.UseStructuredBones = 1u;
            ActiveVS->GetBuffer( "BoneTransformRange" ).Update( &range ).Bind();
        }
    }

    if ( !useStructuredBones ) {
        // Copy bones using legacy cbuffer path.
        ActiveVS->GetBuffer( "BoneTransforms" ).Update( &transforms[0], sizeof( XMFLOAT4X4 ) * std::min<UINT>( transforms.size(), NUM_MAX_BONES ) ).Bind();
    }

    if ( transforms.size() >= NUM_MAX_BONES ) {
        LogWarn() << "SkeletalMesh has more than "
            << NUM_MAX_BONES << " bones! (" << transforms.size() << ")Up this limit!";
    }

    for ( auto const& itm : dynamic_cast<SkeletalMeshVisualInfo*>(vi->VisualInfo)->SkeletalMeshes ) {
        for ( auto& mesh : itm.second ) {
            WhiteTexture->BindToPixelShader( 0 );

            auto& vb = mesh->MeshVertexBuffer;
            auto& ib = mesh->MeshIndexBuffer;
            unsigned int numIndices = mesh->Indices.size();

            UINT offset = 0;
            UINT uStride = sizeof( ExSkelVertexStruct );
            GetContext()->IASetVertexBuffers( 0, 1, vb->GetVertexBuffer().GetAddressOf(), &uStride, &offset );

            Context->IASetIndexBuffer( ib->GetVertexBuffer().Get(), VERTEX_INDEX_DXGI_FORMAT, 0 );

            // Draw the mesh
            GetContext()->DrawIndexed( numIndices, 0, 0 );

            Engine::GAPI->GetRendererState().RendererInfo.FrameDrawnTriangles +=
                numIndices / 3;
        }
    }

    GetContext()->GSSetShader( nullptr, nullptr, 0 );
    return XR_SUCCESS;
}

/** Draws a skeletal mesh */
XRESULT D3D11GraphicsEngine::DrawSkeletalMesh( SkeletalVobInfo* vi,
    const std::span<XMFLOAT4X4> transforms, float4 color, const XMFLOAT4X4& world, float fatness ) {
    if ( GetRenderingStage() == DES_SHADOWMAP_CUBE ) {
        SetActiveVertexShader( VShaderID::VS_ExSkeletalCube );
    } else {
        SetActiveVertexShader( VShaderID::VS_ExSkeletal );
    }


    SetupVS_ExMeshDrawCall();
    SetupVS_ExConstantBuffer();

    Context->IASetPrimitiveTopology( D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST );

    VS_ExConstantBuffer_PerInstanceSkeletal cb2;
    cb2.World = world;
    color.w = (vi && vi->Vob && vi->Vob->IsIndoorVob()) ? 0.05f : 1.0f;
    cb2.PI_ModelColor = color;
    cb2.PI_ModelFatness = fatness;
    // Set PrevWorld for motion vectors (use current world if no previous is available)
    cb2.PrevWorld = vi->HasValidPrevTransforms ? vi->PrevWorldMatrix : world;

    ActiveVS->GetBuffer("Matrices_PerInstances").Update( &cb2 ).Bind();

    bool useStructuredBones = !FeatureLevel10Compatibility;
    if ( useStructuredBones ) {
        const size_t boneCount = std::min<size_t>( transforms.size(), NUM_MAX_BONES );
        std::vector<XMFLOAT4X4> packedCurrent( transforms.begin(), transforms.begin() + boneCount );
        std::vector<XMFLOAT4X4> packedPrev;
        packedPrev.reserve( boneCount );

        if ( vi->HasValidPrevTransforms && !vi->PrevBoneTransforms.empty() ) {
            const size_t copyCount = std::min<size_t>( vi->PrevBoneTransforms.size(), boneCount );
            packedPrev.insert( packedPrev.end(), vi->PrevBoneTransforms.begin(), vi->PrevBoneTransforms.begin() + copyCount );
            if ( copyCount < boneCount ) {
                packedPrev.insert( packedPrev.end(), packedCurrent.begin() + static_cast<std::ptrdiff_t>(copyCount), packedCurrent.end() );
            }
        } else {
            packedPrev = packedCurrent;
        }

        if ( !UploadStructuredMatrixBuffer( SkeletalBoneTransformsBufferTransient, packedCurrent, "SkeletalBoneTransformsBufferTransient" )
            || !UploadStructuredMatrixBuffer( SkeletalPrevBoneTransformsBufferTransient, packedPrev, "SkeletalPrevBoneTransformsBufferTransient" )
            || !SkeletalBoneTransformsBufferTransient
            || !SkeletalPrevBoneTransformsBufferTransient
            || !SkeletalBoneTransformsBufferTransient->GetShaderResourceView().Get()
            || !SkeletalPrevBoneTransformsBufferTransient->GetShaderResourceView().Get() ) {
            useStructuredBones = false;
        } else {
            ActiveVS->BindResource( "BoneTransforms", SkeletalBoneTransformsBufferTransient->GetShaderResourceView().Get() );
            ActiveVS->BindResource( "PrevBoneTransforms", SkeletalPrevBoneTransformsBufferTransient->GetShaderResourceView().Get() );

            VS_ExConstantBuffer_SkeletalBoneRange range = {};
            range.BoneCount = static_cast<unsigned int>(boneCount);
            range.UseStructuredBones = 1u;
            ActiveVS->GetBuffer( "BoneTransformRange" ).Update( &range ).Bind();
        }
    }

    if ( !useStructuredBones ) {
        // Copy bones
        ActiveVS->GetBuffer("BoneTransforms").Update( &transforms[0], sizeof( XMFLOAT4X4 ) * std::min<UINT>( transforms.size(), NUM_MAX_BONES ) ).Bind();

        // Copy previous frame bone transforms for motion vectors (only for main scene rendering, not shadow maps)
        if ( GetRenderingStage() == DES_SHADOWMAP_CUBE ) {
            // Don't bind previous, as we don't use them here yet.
        }
        else if ( GetRenderingStage() != DES_SHADOWMAP ) {
            const std::span<const XMFLOAT4X4> prevTransforms = (vi->HasValidPrevTransforms && !vi->PrevBoneTransforms.empty())
                ? std::span(vi->PrevBoneTransforms)
                : transforms;

            ActiveVS->GetBuffer("PrevBoneTransforms").Update( &prevTransforms[0], sizeof( XMFLOAT4X4 ) * std::min<UINT>( prevTransforms.size(), NUM_MAX_BONES ) ).Bind();
        } else {
            ActiveVS->GetBuffer("BoneTransforms").Bind( ActiveVS->GetInputIndex( "PrevBoneTransforms" ) ); // just bind the current bones again
        }
    }

    if ( transforms.size() >= NUM_MAX_BONES ) {
        LogWarn() << "SkeletalMesh has more than "
            << NUM_MAX_BONES << " bones! (" << transforms.size() << ")Up this limit!";
    }

    ActiveVS->Apply();

    if ( RenderingStage != DES_GHOST ) {
        bool linearDepth = (Engine::GAPI->GetRendererState().GraphicsState.FF_GSwitches & GSWITCH_LINEAR_DEPTH) != 0;
        if ( linearDepth ) {
            ActivePS = ShaderManager->GetPShader( PShaderID::PS_LinDepth );
            ActivePS->Apply();
        } else if ( RenderingStage == DES_SHADOWMAP ) {
            // Unbind PixelShader in this case
            Context->PSSetShader( nullptr, nullptr, 0 );
            ActivePS = nullptr;
        } else {
            // Select the G-buffer depth shader.
            ActivePS = ShaderManager->GetPShader( PShaderID::PS_LinDepth );
        }
    }

    for ( auto const& itm : dynamic_cast<SkeletalMeshVisualInfo*>(vi->VisualInfo)->SkeletalMeshes ) {
        if ( zCMaterial* mat = itm.first ) {
            zCTexture* tex;
            if ( ActivePS && (tex = mat->GetAniTexture()) != nullptr ) {
                const float materialClassMarker = (vi && vi->Vob)
                    ? (vi->Vob->GetVobType() == zVOB_TYPE_NSC
                        ? -1.0f : WATER_VOB_MATERIAL_CLASS_MARKER)
                    : 0.0f;
                if ( !BindTextureNRFX( tex, (RenderingStage != DES_GHOST), true, materialClassMarker ) ) {
                    continue;
                }
            }
        }
        for ( auto& mesh : itm.second ) {

            auto& vb = mesh->MeshVertexBuffer;
            auto& ib = mesh->MeshIndexBuffer;
            unsigned int numIndices = mesh->Indices.size();

            UINT offset = 0;
            UINT uStride = sizeof( ExSkelVertexStruct );
            Context->IASetVertexBuffers( 0, 1, vb->GetVertexBuffer().GetAddressOf(), &uStride, &offset );

            Context->IASetIndexBuffer( ib->GetVertexBuffer().Get(), VERTEX_INDEX_DXGI_FORMAT, 0 );

            // Draw the mesh
            Context->DrawIndexed( numIndices, 0, 0 );

            Engine::GAPI->GetRendererState().RendererInfo.FrameDrawnTriangles +=
                numIndices / 3;
        }
    }

    return XR_SUCCESS;
}

XRESULT D3D11GraphicsEngine::DrawSkeletalMesh_Layered( SkeletalVobInfo* vi,
    const std::span<XMFLOAT4X4> transforms, float4 color, XMFLOAT4X4& world, float fatness ) {
    SetActiveVertexShader( VShaderID::VS_ExSkeletalLayered );

    SetupVS_ExMeshDrawCall();
    SetupVS_ExConstantBuffer();

    Context->IASetPrimitiveTopology( D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST );

    VS_ExConstantBuffer_PerInstanceSkeletal cb2;
    cb2.World = world;
    cb2.PrevWorld = world;
    color.w = (vi && vi->Vob && vi->Vob->IsIndoorVob()) ? 0.05f : 1.0f;
    cb2.PI_ModelColor = color;
    cb2.PI_ModelFatness = fatness;
    ActiveVS->GetBuffer("Matrices_PerInstances").Update( &cb2 ).Bind();

    bool useStructuredBones = !FeatureLevel10Compatibility;
    if ( useStructuredBones ) {
        const std::vector<XMFLOAT4X4> packedCurrent( transforms.begin(), transforms.begin() + std::min<size_t>( transforms.size(), NUM_MAX_BONES ) );
        if ( !UploadStructuredMatrixBuffer( SkeletalBoneTransformsBufferTransient, packedCurrent, "SkeletalBoneTransformsBufferTransient" )
            || !SkeletalBoneTransformsBufferTransient
            || !SkeletalBoneTransformsBufferTransient->GetShaderResourceView().Get() ) {
            useStructuredBones = false;
        } else {
            ActiveVS->BindResource( "BoneTransforms", SkeletalBoneTransformsBufferTransient->GetShaderResourceView().Get() );
            VS_ExConstantBuffer_SkeletalBoneRange range = {};
            range.BoneCount = static_cast<unsigned int>(packedCurrent.size());
            range.UseStructuredBones = 1u;
            ActiveVS->GetBuffer( "BoneTransformRange" ).Update( &range ).Bind();
        }
    }

    if ( !useStructuredBones ) {
        // Copy bones
        ActiveVS->GetBuffer("BoneTransforms").Update( &transforms[0], sizeof( XMFLOAT4X4 ) * std::min<UINT>( transforms.size(), NUM_MAX_BONES ) ).Bind();
    }

    // Note: Slot b3 is used for cbPerCubeRender in VS_ExSkeletalLayered, not PrevBoneTransforms
    // Motion vectors are not needed for shadow map rendering

    if ( transforms.size() >= NUM_MAX_BONES ) {
        LogWarn() << "SkeletalMesh has more than "
            << NUM_MAX_BONES << " bones! (" << transforms.size() << ")Up this limit!";
    }

    ActiveVS->Apply();

    if ( RenderingStage != DES_GHOST ) {
        bool linearDepth = (Engine::GAPI->GetRendererState().GraphicsState.FF_GSwitches & GSWITCH_LINEAR_DEPTH) != 0;
        if ( linearDepth ) {
            ActivePS = ShaderManager->GetPShader( PShaderID::PS_LinDepth );
            ActivePS->Apply();
        } else if ( RenderingStage == DES_SHADOWMAP ) {
            // Unbind PixelShader in this case
            Context->PSSetShader( nullptr, nullptr, 0 );
            ActivePS = nullptr;
        } else {
            // Select the G-buffer depth shader.
            ActivePS = ShaderManager->GetPShader( PShaderID::PS_LinDepth );
        }
    }

    void* lastTex = nullptr;

    GetWhiteTexture()->BindToPixelShader( 0 );
    lastTex = GetWhiteTexture()->GetShaderResourceView().Get();

    for ( auto const& itm : dynamic_cast<SkeletalMeshVisualInfo*>(vi->VisualInfo)->SkeletalMeshes ) {
        if ( zCMaterial* mat = itm.first ) {
            zCTexture* tex = nullptr;
            if ( ActivePS && (tex = mat->GetAniTexture()) != nullptr ) {
                if ( tex->CacheIn( 0.6f ) != zRES_CACHED_IN ) {
                    continue; // we cant determine if we need to draw this, alpha data is only available after loading a texture.
                }
                const bool needTex =  tex != lastTex
                    && (tex->HasAlphaChannel() || mat->HasAlphaTest());

                if ( needTex ) {
                    tex->GetSurface()->GetEngineTexture()->BindToPixelShader( 0 );
                    lastTex = tex;
                } else if ( lastTex != GetWhiteTexture()->GetShaderResourceView().Get() ) {
                    GetWhiteTexture()->BindToPixelShader( 0 );
                    lastTex = GetWhiteTexture()->GetShaderResourceView().Get();
                }
            }
        }
        for ( auto& mesh : itm.second ) {

            auto& vb = mesh->MeshVertexBuffer;
            auto& ib = mesh->MeshIndexBuffer;
            unsigned int numIndices = mesh->Indices.size();

            UINT offset = 0;
            UINT uStride = sizeof( ExSkelVertexStruct );
            Context->IASetVertexBuffers( 0, 1, vb->GetVertexBuffer().GetAddressOf(), &uStride, &offset );

            Context->IASetIndexBuffer( ib->GetVertexBuffer().Get(), VERTEX_INDEX_DXGI_FORMAT, 0 );

            // Draw the mesh
            Context->DrawIndexedInstanced( numIndices, 6, 0, 0, 0 );

            Engine::GAPI->GetRendererState().RendererInfo.FrameDrawnTriangles +=
                numIndices / 3;
        }
    }

    return XR_SUCCESS;
}

/** Draws a batch of instanced geometry */
XRESULT D3D11GraphicsEngine::DrawInstanced(
    D3D11VertexBuffer* vb, D3D11VertexBuffer* ib, unsigned int numIndices,
    D3D11VertexBuffer* instanceData, unsigned int instanceDataStride,
    unsigned int numInstances, unsigned int vertexStride,
    unsigned int startInstanceNum, unsigned int indexOffset ) {
    // Bind shader and pipeline flags
    UINT offset[] = { 0, 0 };
    UINT uStride[] = { vertexStride, instanceDataStride };
    ID3D11Buffer* buffers[2] = {
        vb->GetVertexBuffer().Get(),
        instanceData->GetVertexBuffer().Get()
    };
    GetContext()->IASetVertexBuffers( 0, 2, buffers, uStride, offset );

    Context->IASetIndexBuffer( ib->GetVertexBuffer().Get(), VERTEX_INDEX_DXGI_FORMAT, 0 );

    unsigned int max =
        Engine::GAPI->GetRendererState().RendererSettings.MaxNumFaces * 3;
    numIndices = max != 0 ? (numIndices < max ? numIndices : max) : numIndices;

    // Draw the batch
    GetContext()->DrawIndexedInstanced( numIndices, numInstances, indexOffset, 0,
        startInstanceNum );

    Engine::GAPI->GetRendererState().RendererInfo.FrameDrawnTriangles +=
        (numIndices / 3) * numInstances;

    Engine::GAPI->GetRendererState().RendererInfo.FrameDrawnVobs++;

    return XR_SUCCESS;
}

void D3D11GraphicsEngine::DrawSkeletalMeshVobs(
    const std::vector<SkeletalVobInfo*>& vis,
    float distance,
    bool updateState,
    bool drawAttachments,
    bool useLayeredShadowPath ) {
    ZoneScoped;

    struct TempVobDrawInfo {
        SkeletalVobInfo* VobInfo;
        zCModel* Model;
        int BoneIdx;
        int NumBones;
        float4 ModelColor;
        float Fatness;
        XMMATRIX World;
        XMFLOAT4X4 PrevWorld;

        TempVobDrawInfo() = default;

        TempVobDrawInfo(
            SkeletalVobInfo* VobInfo,
            zCModel* Model,
            int BoneIdx,
            int NumBones,
            float4 ModelColor,
            float Fatness,
            XMMATRIX World,
            XMFLOAT4X4 PrevWorld
        ) :
            VobInfo(VobInfo),
            Model( Model),
            BoneIdx( BoneIdx ),
            NumBones( NumBones ),
            ModelColor( ModelColor),
            Fatness( Fatness),
            World( World),
            PrevWorld( PrevWorld )
        { }
    };

    static std::vector<TempVobDrawInfo> tempVobList;
    tempVobList.clear();
    BoneTransformCache.clear();
    BoneTransformCache.reserve( 150 );

    GothicGraphicsState& graphicsState = Engine::GAPI->GetRendererState().GraphicsState;

    const bool isZPrepass = GetRenderingStage() == DES_Z_PRE_PASS;
    const bool isMainStage = GetRenderingStage() == DES_MAIN;
    const bool isMainReuseStage = isZPrepass || isMainStage;
    struct ScopedGraphicsSwitchRestore {
        unsigned int& Value;
        unsigned int Previous;
        ~ScopedGraphicsSwitchRestore() { Value = Previous; }
    } graphicsSwitchRestore { graphicsState.FF_GSwitches, graphicsState.FF_GSwitches };
    const auto& rendererSettings = Engine::GAPI->GetRendererState().RendererSettings;
    if ( isMainStage
        && rendererSettings.AntiAliasingMode == GothicRendererSettings::AA_FSR3
        && rendererSettings.Upscaler == GothicRendererSettings::UPSCALER_FSR_3 ) {
        graphicsState.FF_GSwitches |= GSWITCH_FSR3_REACTIVE;
        if ( !Engine::GAPI->DialogFinished() ) {
            graphicsState.FF_GSwitches |= GSWITCH_FSR3_DIALOG_REACTIVE;
        }
    }
    bool useStructuredBones = !FeatureLevel10Compatibility;

    std::vector<VS_ExConstantBuffer_SkeletalBoneRange> structuredBoneRanges( vis.size() );

    if ( useStructuredBones ) {
        // Keep one packed bone SRV for the complete frame. Main-view batches,
        // CSM cascades and pointlight cubes all use different subsets, so the
        // old order-based cache could only be reused when the complete vector
        // happened to be identical. Store ranges by vob and append only new
        // entries; this removes repeated GetBoneTransforms() calls and avoids
        // re-uploading a transient buffer for every shadow pass.
        auto& frameCache = m_FrameGeometryCache;
        bool cacheComplete = true;
        bool cacheAdded = false;

        for ( size_t i = 0; i < vis.size(); ++i ) {
            SkeletalVobInfo* vi = vis[i];
            if ( !vi || !vi->Vob )
                continue;

            zCModel* model = static_cast<zCModel*>(vi->Vob->GetVisual());
            if ( !model || !vi->VisualInfo || !vi->Vob->GetShowVisual() )
                continue;

            // Preserve the existing per-call state snapshot. Animation itself
            // is still advanced by the main pass; shadow callers pass false.
            vi->UpdateState();

            auto cacheIt = frameCache.skeletalBoneCache.find( vi );
            if ( cacheIt != frameCache.skeletalBoneCache.end()
                && ( cacheIt->second.Model != model || cacheIt->second.VisualInfo != vi->VisualInfo ) ) {
                // A visual can be replaced while a vob remains alive. Never
                // reuse the old model's bone range for the new vertex layout.
                cacheIt = frameCache.skeletalBoneCache.erase( cacheIt );
            }
            if ( cacheIt == frameCache.skeletalBoneCache.end() ) {
                BoneTransformCache.clear();
                model->GetBoneTransforms( &BoneTransformCache );
                const int numBones = std::min<int>(
                    static_cast<int>( BoneTransformCache.size() ), NUM_MAX_BONES );
                if ( numBones <= 0 ) {
                    cacheComplete = false;
                    continue;
                }

                const auto transforms = std::span( BoneTransformCache.data(), numBones );
                FrameGeometryCache::CachedSkeletalBoneData cached = {};
                cached.Range.BoneOffset = static_cast<unsigned int>( frameCache.skeletalBoneTransforms.size() );
                cached.Range.BoneCount = static_cast<unsigned int>( numBones );
                cached.Range.PrevBoneOffset = static_cast<unsigned int>( frameCache.skeletalPrevBoneTransforms.size() );
                cached.Range.UseStructuredBones = 1u;
                cached.Model = model;
                cached.VisualInfo = vi->VisualInfo;

                frameCache.skeletalBoneTransforms.insert(
                    frameCache.skeletalBoneTransforms.end(), transforms.begin(), transforms.end() );

                if ( vi->HasValidPrevTransforms && !vi->PrevBoneTransforms.empty() ) {
                    const size_t copyCount = std::min<size_t>(
                        vi->PrevBoneTransforms.size(), static_cast<size_t>( numBones ) );
                    frameCache.skeletalPrevBoneTransforms.insert(
                        frameCache.skeletalPrevBoneTransforms.end(),
                        vi->PrevBoneTransforms.begin(), vi->PrevBoneTransforms.begin() + copyCount );
                    if ( copyCount < static_cast<size_t>( numBones ) ) {
                        frameCache.skeletalPrevBoneTransforms.insert(
                            frameCache.skeletalPrevBoneTransforms.end(),
                            transforms.begin() + static_cast<std::ptrdiff_t>( copyCount ), transforms.end() );
                    }
                } else {
                    frameCache.skeletalPrevBoneTransforms.insert(
                        frameCache.skeletalPrevBoneTransforms.end(), transforms.begin(), transforms.end() );
                }

                auto [insertedIt, inserted] = frameCache.skeletalBoneCache.emplace( vi, cached );
                if ( !inserted ) {
                    cacheComplete = false;
                    continue;
                }
                cacheIt = insertedIt;
                cacheAdded = true;
            }

            structuredBoneRanges[i] = cacheIt->second.Range;
        }

        if ( cacheAdded || !frameCache.skeletalBonesUploaded ) {
            const bool uploadedCurrent = UploadStructuredMatrixBuffer(
                SkeletalBoneTransformsBuffer,
                frameCache.skeletalBoneTransforms,
                "SkeletalBoneTransformsBuffer" );
            const bool uploadedPrevious = UploadStructuredMatrixBuffer(
                SkeletalPrevBoneTransformsBuffer,
                frameCache.skeletalPrevBoneTransforms,
                "SkeletalPrevBoneTransformsBuffer" );

            if ( !uploadedCurrent || !uploadedPrevious ) {
                useStructuredBones = false;
            } else {
                frameCache.skeletalBonesUploaded = true;
            }
        }

        if ( !cacheComplete )
            useStructuredBones = false;
    }

    BoneTransformCache.clear();

    // Setup drawing of SkeletalMeshes, attachments are deferred, to reduce api calls.
    // The layered cubemap path uses the six-instance shader variant and must not
    // be mixed with the regular cube shader, even though both use the same stage.
    useLayeredShadowPath = useLayeredShadowPath && GetRenderingStage() == DES_SHADOWMAP_CUBE;

    if ( useLayeredShadowPath ) {
        SetActiveVertexShader( VShaderID::VS_ExSkeletalLayered );
    } else if ( GetRenderingStage() == DES_SHADOWMAP_CUBE ) {
        SetActiveVertexShader( VShaderID::VS_ExSkeletalCube );
    } else {
        SetActiveVertexShader( VShaderID::VS_ExSkeletal );
    }

    ActiveVS->Apply();

    SetupVS_ExMeshDrawCall();
    SetupVS_ExConstantBuffer();

    Context->IASetPrimitiveTopology( D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST );

    auto perInstanceCb = ActiveVS->GetBuffer("Matrices_PerInstances").Bind();
    auto boneRangeCb = ActiveVS->GetBuffer( "BoneTransformRange" ).Bind();
    auto boneTransformsCb = GraphicsShaderConstantBuffer();
    auto prevBoneTransformsCb = GraphicsShaderConstantBuffer();

    if ( useStructuredBones ) {
        // The frame cache is shared by main, CSM and pointlight shadow
        // stages. Do not switch back to the old transient shadow buffers;
        // their ranges are no longer rebuilt for every pass.
        auto& currentBuffer = SkeletalBoneTransformsBuffer;
        auto& prevBuffer = SkeletalPrevBoneTransformsBuffer;

        if ( !currentBuffer || !currentBuffer->GetShaderResourceView().Get()
            || ( !useLayeredShadowPath && ( !prevBuffer || !prevBuffer->GetShaderResourceView().Get() ) ) ) {
            useStructuredBones = false;
        } else {
            ActiveVS->BindResource( "BoneTransforms", currentBuffer->GetShaderResourceView().Get() );
            if ( !useLayeredShadowPath ) {
                ActiveVS->BindResource( "PrevBoneTransforms", prevBuffer->GetShaderResourceView().Get() );
            }
        }
    }

    if ( !useStructuredBones ) {
        // Copy bones using the legacy cbuffer path.
        boneTransformsCb = ActiveVS->GetBuffer("BoneTransforms").Bind();
        if ( !useLayeredShadowPath ) {
            prevBoneTransformsCb = ActiveVS->GetBuffer("PrevBoneTransforms").Bind();
        }

        // Copy previous frame bone transforms for motion vectors (only for main scene rendering, not shadow maps)
        if ( GetRenderingStage() == DES_SHADOWMAP_CUBE ) {
            // Don't bind previous, as we don't use them here yet.
        }
        else if ( GetRenderingStage() != DES_SHADOWMAP ) {
            prevBoneTransformsCb.Bind(); // we actually should have previous bones
        } else {
            // must be shadowmap, bind current bones as previous
            boneTransformsCb.Bind( ActiveVS->GetInputIndex( "PrevBoneTransforms" ) ); // just bind the current bones again
        }
    }

    const auto now = Engine::GAPI->GetTotalTimeDW();

    bool wantShader = true;
    if ( RenderingStage != DES_GHOST ) {
        bool linearDepth = (graphicsState.FF_GSwitches & GSWITCH_LINEAR_DEPTH) != 0;
        if ( linearDepth ) {
            ActivePS = ShaderManager->GetPShader( PShaderID::PS_LinDepth );
            ActivePS->Apply();
        } else if ( RenderingStage == DES_SHADOWMAP) {
            if ( ActivePS ) {
                Context->PSSetShader( nullptr, nullptr, 0 );
                ActivePS = nullptr;
            }
            wantShader = false;
        }
    }

    if ( isZPrepass ) {
        // Unbind PS for z-prepass, we need to try to ignore any textures that require alpha(testing)
        // as this otherwise slows down prepass too much.
        Context->PSSetShader( nullptr, nullptr, 0 );
        ActivePS = nullptr;
        wantShader = true;
    }

    // Ensure we have correct Constantbuffer for eventual Alphatest stuff.
    auto cbFFPipelineConstantBuffer = ShaderManager->GetPShader( Resolved_DiffuseNormalmappedAlphatest )
        ->GetBuffer( "FFPipelineConstantBuffer" )
        .Update( &graphicsState )
        .Bind();

    const bool enableShadows = Engine::GAPI->GetRendererState().RendererSettings.EnableShadows;
    const bool isMainPass = RenderingStage == DES_MAIN;
    zCTexture* lastTex = nullptr;
    float lastMaterialClassMarker = 999.0f;
    auto bindTextureForPass = [&]( zCTexture* tex, float materialClassMarker ) {
        if (tex == lastTex && materialClassMarker == lastMaterialClassMarker)
            return true;
        lastMaterialClassMarker = materialClassMarker;

        if ( isZPrepass ) {
            if (tex->CacheIn( 0.6f ) != zRES_CACHED_IN ) {
                return false;
            }
            if (tex->HasAlphaChannel()) {
                // Use the alpha-tested depth path.
                tex->GetSurface()->GetEngineTexture()->BindToPixelShader( 0 );
                lastTex = tex;
                if (!ActivePS) {
                    ActivePS = ShaderManager->GetPShader( PShaderID::PS_DiffuseAlphaTestShadows );
                    ActivePS->Apply();
                }
            } else if (lastTex != nullptr) {
                Context->PSSetShaderResources( 0, 1, s_nullSRVs );
                lastTex = nullptr;
                Context->PSSetShader( nullptr, nullptr, 0 );
                ActivePS = nullptr;
            }
            return true;
        } else {
            lastTex = tex;
            return BindTextureNRFX( tex, isMainPass, true, materialClassMarker );
        }
    };

    {
        auto _scopeBaseMeshes = RecordGraphicsEvent( GE_NAME( "DrawSkeletalMeshVobs::BaseMeshes" ) );
        TracyD3D11ZoneCGX( "DrawSkeletalMeshVobs::BaseMeshes" );
        size_t drawIndex = 0;
        for ( SkeletalVobInfo* vi : vis ) {
            const size_t currentDrawIndex = drawIndex++;
            zCModel* model = static_cast<zCModel*>(vi->Vob->GetVisual());
            if ( !model ) {
                continue;
            }

            model->SetIsVisible( true );
            if ( !vi->VisualInfo )
                continue; // Gothic fortunately sets this to 0 when it throws the model out of the cache
            if ( !vi->Vob->GetShowVisual() )
                continue;

            const float materialClassMarker = vi->Vob->GetVobType() == zVOB_TYPE_NSC
                ? -1.0f : WATER_VOB_MATERIAL_CLASS_MARKER;
            float4 modelColor;
            if ( enableShadows ) {
                // Let shadows do the work
                modelColor = 0xFFFFFFFF;
            } else {
                if ( vi->Vob->IsIndoorVob() ) {
                    // All lightmapped polys have this color, so just use it
                    modelColor = DEFAULT_LIGHTMAP_POLY_COLOR;
                } else {
                    // Get the color from vob position of the ground poly
                    if ( zCPolygon* polygon = vi->Vob->GetGroundPoly() ) {
                        float3 vobPos = vi->Vob->GetPositionWorld();
                        float3 polyLightStat = polygon->GetLightStatAtPos( vobPos );
                        modelColor.x = polyLightStat.z * inv255f;
                        modelColor.y = polyLightStat.y * inv255f;
                        modelColor.z = polyLightStat.x * inv255f;
                        modelColor.w = 1.f;
                    } else {
                        modelColor = 0xFFFFFFFF;
                    }
                }
            }
            modelColor.w = vi->Vob->IsIndoorVob() ? 0.05f : 1.0f;

            if ( updateState ) {
                if ( vi->LastAniUpdateFrame != now ) {
                    vi->LastAniUpdateFrame = now;
                    // Update attachments
                    model->UpdateAttachedVobs();
                }
                model->UpdateMeshLibTexAniState();
            }

            XMMATRIX scale = XMMatrixScalingFromVector( model->GetModelScaleXM() );

            XMMATRIX xmWorld = vi->Vob->GetWorldMatrixXM() * scale;
            XMFLOAT4X4 world; XMStoreFloat4x4( &world, xmWorld );
            float fatness = model->GetModelFatness();

            int boneIdx = 0;
            int numBones = 0;
            std::span<const XMFLOAT4X4> transforms;
            if ( useStructuredBones
                && currentDrawIndex < structuredBoneRanges.size()
                && structuredBoneRanges[currentDrawIndex].BoneCount > 0 ) {
                const auto& range = structuredBoneRanges[currentDrawIndex];
                boneIdx = static_cast<int>( range.BoneOffset );
                numBones = static_cast<int>( range.BoneCount );
                transforms = std::span<const XMFLOAT4X4>(
                    m_FrameGeometryCache.skeletalBoneTransforms.data() + range.BoneOffset,
                    range.BoneCount );
            } else {
                // Feature-level 10 and upload failures retain the legacy
                // per-call cbuffer path as a safe fallback.
                const size_t currentBegin = BoneTransformCache.size();
                model->GetBoneTransforms( &BoneTransformCache );
                numBones = static_cast<int>( BoneTransformCache.size() - currentBegin );
                numBones = std::min<int>( numBones, NUM_MAX_BONES );
                BoneTransformCache.resize( currentBegin + static_cast<size_t>( numBones ) );
                boneIdx = static_cast<int>( currentBegin );
                transforms = std::span<const XMFLOAT4X4>(
                    BoneTransformCache.data() + currentBegin, static_cast<size_t>( numBones ) );
            }


            if ( !static_cast<SkeletalMeshVisualInfo*>(vi->VisualInfo)->SkeletalMeshes.empty() ) {
#ifdef BUILD_GOTHIC_2_6_fix
                if ( !model->GetDrawHandVisualsOnly() || *reinterpret_cast<BYTE*>(0x57A694) == 0x90 ) {
#else
                if ( !model->GetDrawHandVisualsOnly() ) {
#endif
                    const auto color = modelColor;

                    VS_ExConstantBuffer_PerInstanceSkeletal cb2;
                    cb2.World = world;
                    auto maskedColor = color;
                    maskedColor.w = vi->Vob->IsIndoorVob() ? 0.05f : 1.0f;
                    cb2.PI_ModelColor = maskedColor;
                    cb2.PI_ModelFatness = fatness;
                    // Set PrevWorld for motion vectors (use current world if no previous is available)
                    cb2.PrevWorld = vi->HasValidPrevTransforms ? vi->PrevWorldMatrix : world;

                    perInstanceCb.Update( &cb2 );

                    if ( useStructuredBones ) {
                        VS_ExConstantBuffer_SkeletalBoneRange range = {};
                        if ( currentDrawIndex < structuredBoneRanges.size() ) {
                            range = structuredBoneRanges[currentDrawIndex];
                        }

                        if ( range.BoneCount == 0 ) {
                            range.BoneOffset = static_cast<unsigned int>(boneIdx);
                            range.PrevBoneOffset = static_cast<unsigned int>(boneIdx);
                            range.BoneCount = static_cast<unsigned int>(std::min<UINT>( transforms.size(), NUM_MAX_BONES ));
                        }
                        range.UseStructuredBones = 1u;
                        boneRangeCb.Update( &range );
                    } else {
                        // Copy bones
                        boneTransformsCb.Update( &transforms[0], sizeof( XMFLOAT4X4 ) * std::min<UINT>( transforms.size(), NUM_MAX_BONES ) );

                        // Copy previous frame bone transforms for motion vectors (only for main scene rendering, not shadow maps)
                        if ( GetRenderingStage() == DES_SHADOWMAP_CUBE ) {
                            // Don't bind previous, as we don't use them here yet.
                        } else if ( GetRenderingStage() != DES_SHADOWMAP ) {
                            const std::span<const XMFLOAT4X4> prevTransforms = (vi->HasValidPrevTransforms && !vi->PrevBoneTransforms.empty())
                                ? std::span( vi->PrevBoneTransforms )
                                : transforms;

                            prevBoneTransformsCb.Update( &prevTransforms[0], sizeof( XMFLOAT4X4 ) * std::min<UINT>( prevTransforms.size(), NUM_MAX_BONES ) );
                        }
                    }

                    if ( transforms.size() >= NUM_MAX_BONES ) {
                        LogWarn() << "SkeletalMesh has more than "
                            << NUM_MAX_BONES << " bones! (" << transforms.size() << ")Up this limit!";
                    }
                    for ( auto const& itm : dynamic_cast<SkeletalMeshVisualInfo*>(vi->VisualInfo)->SkeletalMeshes ) {
                        if ( zCMaterial* mat = itm.first ) {
                            zCTexture* tex;
                            if ( wantShader && (tex = mat->GetAniTexture()) != nullptr ) {
                                if ( !bindTextureForPass( tex, materialClassMarker ) ) {
                                    continue;
                                }
                            }
                        }
                        for ( auto& mesh : itm.second ) {

                            auto& vb = mesh->MeshVertexBuffer;
                            auto& ib = mesh->MeshIndexBuffer;
                            unsigned int numIndices = mesh->Indices.size();

                            UINT offset = 0;
                            UINT uStride = sizeof( ExSkelVertexStruct );
                            Context->IASetVertexBuffers( 0, 1, vb->GetVertexBuffer().GetAddressOf(), &uStride, &offset );

                            Context->IASetIndexBuffer( ib->GetVertexBuffer().Get(), VERTEX_INDEX_DXGI_FORMAT, 0 );

                            // Draw the mesh
                            if ( useLayeredShadowPath ) {
                                Context->DrawIndexedInstanced( numIndices, 6, 0, 0, 0 );
                            } else {
                                Context->DrawIndexed( numIndices, 0, 0 );
                            }

                            Engine::GAPI->GetRendererState().RendererInfo.FrameDrawnTriangles +=
                                numIndices / 3;
                        }
                    }
                }
            } else {
                if ( model->GetMeshSoftSkinList()->NumInArray > 0 ) {
                    // Nothing to draw.
                    WorldConverter::ExtractSkeletalMeshFromVob( model, static_cast<SkeletalMeshVisualInfo*>(vi->VisualInfo) );
                }
            }

            if ( drawAttachments ) {
                tempVobList.emplace_back( vi, model, boneIdx, numBones, modelColor, fatness, xmWorld, vi->HasValidPrevTransforms ? vi->PrevWorldMatrix : world );
            }

            Engine::GAPI->GetRendererState().RendererInfo.FrameDrawnVobs++;
        }
    }

    if ( !drawAttachments || tempVobList.empty() ) {
        return;
    }

    {
        ZoneScopedN( "DrawSkeletalMeshVobs::Attachments" );
        auto _scopeNodeAttachments = RecordGraphicsEvent( GE_NAME( "DrawSkeletalMeshVobs::Attachments" ) );

        // Cube and layered shadow paths use SV_InstanceID for the cubemap faces.
        const bool useCubePath = (GetRenderingStage() == DES_SHADOWMAP_CUBE) && !useLayeredShadowPath;
        const bool isShadowPass = (GetRenderingStage() == DES_SHADOWMAP || GetRenderingStage() == DES_SHADOWMAP_CUBE);
        const bool isMainOrGhost = (GetRenderingStage() == DES_MAIN || GetRenderingStage() == DES_GHOST);
        const bool requiresMorphMeshSameAsMain = (GetRenderingStage() == DES_MAIN || GetRenderingStage() == DES_GHOST || GetRenderingStage() == DES_Z_PRE_PASS);

        // Collect all non-MorphMesh draws and handle MorphMesh/Cube per-draw

        struct NodeAttachmentDrawItem {
            uint64_t sortKey;
            MeshInfo* mesh;
            zCTexture* texture;    // null for shadow passes
            zCMaterial* material;
            float materialClassMarker;
            NodeAttachmentInstanceData instanceData;
            bool needAlpha;
        };

        static std::vector<NodeAttachmentDrawItem> instancedDrawItems;
        instancedDrawItems.clear();

        // For the cube shadow path and MorphMesh, we need the old per-draw setup

        auto ensurePerDrawShaderSetup = [&]() {
            if ( useLayeredShadowPath )
                SetActiveVertexShader( VShaderID::VS_ExNodeLayered );
            else if ( useCubePath )
                SetActiveVertexShader( VShaderID::VS_ExNodeCube );
            else
                SetActiveVertexShader( VShaderID::VS_ExNode );

            SetupVS_ExMeshDrawCall();
            SetupVS_ExConstantBuffer();
            };

        // For MorphMesh per-draw calls, lazily initialized
        bool perDrawSetupDone = false;
        GraphicsShaderConstantBuffer perDrawMPI;

        auto ensurePerDrawReady = [&]() {
            if ( !perDrawSetupDone ) {
                ensurePerDrawShaderSetup();
                perDrawMPI = GetActiveVS()->GetBuffer( "Matrices_PerInstances" ).Bind();

                if ( isMainOrGhost ) {
                    SetActivePixelShader( PShaderID::PS_DiffuseAlphaTest );
                    BindActivePixelShader();
                }
                perDrawSetupDone = true;
            }
            };

        // Cube and layered paths require per-draw setup for each attachment.
        if ( useCubePath || useLayeredShadowPath ) {
            ensurePerDrawReady();
        }

        for ( auto& data : tempVobList ) {
            auto vi = data.VobInfo;
            const float materialClassMarker = (vi && vi->Vob)
                ? (vi->Vob->GetVobType() == zVOB_TYPE_NSC
                    ? -1.0f : WATER_VOB_MATERIAL_CLASS_MARKER)
                : 0.0f;
            auto model = data.Model;
            auto modelColor = data.ModelColor;
            modelColor.w = (vi && vi->Vob && vi->Vob->IsIndoorVob()) ? 0.05f : 1.0f;
            const auto& boneStorage = useStructuredBones
                ? m_FrameGeometryCache.skeletalBoneTransforms
                : BoneTransformCache;
            if ( data.BoneIdx < 0
                || static_cast<size_t>( data.BoneIdx ) + static_cast<size_t>( data.NumBones ) > boneStorage.size() ) {
                continue;
            }
            auto transforms = std::span<const XMFLOAT4X4>(
                boneStorage.data() + data.BoneIdx, static_cast<size_t>( data.NumBones ) );
            auto fatness = data.Fatness;
            auto& world = data.World;
            auto& prevWorld = data.PrevWorld;

            auto& nodeAttachments = vi->NodeAttachments;
            for ( unsigned int i = 0; i < transforms.size(); i++ ) {
                // Check for new visual
                zCModel* mvis = static_cast<zCModel*>( vi->Vob->GetVisual() );
                zCModelNodeInst* node = mvis->GetNodeList()->Array[i];

                if ( !node->NodeVisual )
                    continue; // Happens when you pull your sword for example

                // Check if this is loaded
                if ( node->NodeVisual && nodeAttachments.find( i ) == nodeAttachments.end() ) {
                    WorldConverter::ExtractNodeVisual( i, node, nodeAttachments );
                }

                // Check for changed visual
                if ( nodeAttachments[i].size() && node->NodeVisual != nodeAttachments[i][0]->Visual ) {
                    // Check for deleted attachment
                    if ( !node->NodeVisual ) {
                        // Remove attachment
                        delete nodeAttachments[i][0];
                        nodeAttachments[i].clear();

                        LogInfo() << "Removed attachment from model " << vi->VisualInfo->VisualName;

                        continue; // Go to next attachment
                    }
                    WorldConverter::ExtractNodeVisual( i, node, nodeAttachments );
                }

                auto nodeAttachment = nodeAttachments.find( i );
                if ( nodeAttachment == nodeAttachments.end() ) {
                    continue;
                }

                if ( model->GetDrawHandVisualsOnly() ) {
                    std::string NodeName = node->ProtoNode->NodeName.ToChar();
#ifdef BUILD_GOTHIC_2_6_fix
                    if ( NodeName.find( "HAND" ) == std::string::npos && (*reinterpret_cast<BYTE*>(0x57A694) != 0x90 || NodeName.find( "ARM" ) == std::string::npos) ) {
#else
                    if ( NodeName.find( "HAND" ) == std::string::npos ) {
#endif
                        continue;
                    }
                }

                const XMMATRIX curTransform = XMLoadFloat4x4( &transforms[i] );
                XMFLOAT4X4 finalWorld; XMStoreFloat4x4( &finalWorld, world * curTransform );

                XMMATRIX prevTransform = curTransform;
                if ( vi->HasValidPrevTransforms && i < vi->PrevBoneTransforms.size() ) {
                    prevTransform = XMLoadFloat4x4( &vi->PrevBoneTransforms[i] );
                }
                const XMMATRIX prevWorldXm = XMLoadFloat4x4( &prevWorld );
                XMFLOAT4X4 finalPrevWorld; XMStoreFloat4x4( &finalPrevWorld, prevWorldXm * prevTransform );

                for ( MeshVisualInfo* mvi : nodeAttachment->second ) {

                    if ( !mvi->Visual ) {
                        LogWarn() << "Attachment without visual on model: " << model->GetVisualName();
                        continue;
                    }

                    bool isMMS = strcmp( mvi->Visual->GetFileExtension( 0 ), ".MMS" ) == 0;
                    if ( updateState ) {
                        node->TexAniState.UpdateTexList();
                        if ( isMMS ) {
                            zCMorphMesh* mm = reinterpret_cast<zCMorphMesh*>(mvi->Visual);
                            mm->GetTexAniState()->UpdateTexList();
                        }
                    }

                    // MorphMesh: always per-draw
                    if ( isMMS && distance < 1000 ) {
                        // Only 0.35f of the fatness wanted by gothic.
                        // They seem to compensate for that with the scaling.

                        ensurePerDrawReady();

                        VS_ExConstantBuffer_PerInstanceNode instanceInfo;
                        instanceInfo.Color = modelColor;
                        instanceInfo.Fatness = std::max<float>( 0.f, fatness * 0.35f );
                        instanceInfo.Scaling = fatness * 0.02f + 1.f;
                        instanceInfo.World = finalWorld;
                        instanceInfo.PrevWorld = finalPrevWorld;
                        perDrawMPI.Update( &instanceInfo );

                        if ( distance < 1000 ) {
                            if ( requiresMorphMeshSameAsMain ) {
                                zCMorphMesh* mm = reinterpret_cast<zCMorphMesh*>( mvi->Visual );
                                if ( updateState ) {
                                    if ( mvi->LastAniUpdateFrame != now ) {
                                        WorldConverter::UpdateMorphMeshVisual( mm, mvi );
                                        mvi->LastAniUpdateFrame = now;
                                    }
                                }
                                Engine::GAPI->DrawMorphMesh( mm, mvi->Meshes );
                                continue;
                            }
                        }

                        if ( isShadowPass ) {
                            for ( auto const& itm : mvi->Meshes ) {
                                for ( unsigned int m = 0; m < itm.second.size(); m++ ) {
                                    Engine::GAPI->DrawMeshInfo( itm.first, itm.second[m] );
                                }
                            }
                        } else {
                            for ( auto const& itm : mvi->Meshes ) {
                                zCTexture* texture;
                                if ( itm.first && (texture = itm.first->GetAniTexture()) != nullptr ) {
                                    if ( !bindTextureForPass( texture, materialClassMarker ) )
                                        continue;
                                }
                                for ( unsigned int m = 0; m < itm.second.size(); m++ ) {
                                    Engine::GAPI->DrawMeshInfo( itm.first, itm.second[m] );
                                }
                            }
                        }
                        continue;
                    }

                    // Cube/layered shadow paths: per-draw path (SV_InstanceID is used for faces)
                    if ( useCubePath || useLayeredShadowPath ) {
                        VS_ExConstantBuffer_PerInstanceNode instanceInfo;
                        instanceInfo.Color = modelColor;
                        instanceInfo.Fatness = 0.f;
                        instanceInfo.Scaling = 1.f;
                        instanceInfo.World = finalWorld;
                        instanceInfo.PrevWorld = finalPrevWorld;
                        perDrawMPI.Update( &instanceInfo );

                        for ( auto const& itm : mvi->Meshes ) {
                            for ( unsigned int m = 0; m < itm.second.size(); m++ ) {
                                Engine::GAPI->DrawMeshInfo( itm.first, itm.second[m] );
                            }
                        }
                        continue;
                    }

                    // Non-MMS, non-cube: collect for instanced drawing
                    NodeAttachmentInstanceData instData;
                    instData.World = finalWorld;
                    instData.PrevWorld = finalPrevWorld;
                    instData.Color = modelColor;

                    for ( auto const& itm : mvi->Meshes ) {
                        zCTexture* texture = nullptr;
                        FrameGeometryCache::SortKeyBuilder sortKeyBase = { 0 };
                        if ( itm.first ) {
                            texture = itm.first->GetAniTexture();
                            if ( !texture
                                // Skip textures unused by the shadow pass.
                                || (isShadowPass && (strncmp(texture->__GetName().ToChar(), "HUM_TEETH_V0.TGA", std::size("HUM_TEETH_V0.TGA") - 1) == 0))
                                || texture->CacheIn( 0.6f ) != zRES_CACHED_IN ) {
                                // Cache the texture before reading material state.
                                continue;
                            }
                            if ( texture->HasAlphaChannel() ) {
                                sortKeyBase.withAlphaType( 1 );
                            }
                            sortKeyBase.withTexture(reinterpret_cast<size_t>(texture));
                        }

                        for ( unsigned int m = 0; m < itm.second.size(); m++ ) {
                            FrameGeometryCache::SortKeyBuilder meshSortKey = sortKeyBase;
                            meshSortKey.withMesh( itm.second[m]->meshId );

                            instancedDrawItems.emplace_back( meshSortKey.sortKey, itm.second[m], texture, itm.first,
                                materialClassMarker, instData,
                                (texture && texture->HasAlphaChannel()) || (itm.first && itm.first->HasAlphaTest())
                            );
                        }
                    }
                }
            }
        }

        if ( instancedDrawItems.empty() )
            return;

        std::sort( instancedDrawItems.begin(), instancedDrawItems.end(),
            []( const NodeAttachmentDrawItem& a, const NodeAttachmentDrawItem& b ) {
                    return a.sortKey < b.sortKey;
            } );

        // Ensure instance buffer is large enough
        const size_t neededBytes = instancedDrawItems.size() * sizeof( NodeAttachmentInstanceData );
        if ( NodeAttachmentInstancingBuffer->GetSizeInBytes() < neededBytes ) {
            if ( XR_FAILED == NodeAttachmentInstancingBuffer->Init(
                nullptr, neededBytes,
                D3D11VertexBuffer::B_VERTEXBUFFER, D3D11VertexBuffer::U_DYNAMIC, D3D11VertexBuffer::CA_WRITE ) ) {
                LogError() << "Failed to create instance buffer for node attachments!";
                return;
            }
            SetDebugName( NodeAttachmentInstancingBuffer->GetVertexBuffer().Get(), "NodeAttachmentInstancingBuffer" );
        }

        // Build batch list and upload instance data
        struct InstanceBatch {
            MeshInfo* mesh;
            zCTexture* texture;
            zCMaterial* material;
            float materialClassMarker;
            unsigned int startInstance;
            unsigned int instanceCount;
            bool needAlpha;
        };

        static std::vector<InstanceBatch> batches;
        batches.clear();

        void* mappedData;
        UINT mappedSize;
        if ( XR_SUCCESS != NodeAttachmentInstancingBuffer->Map( D3D11VertexBuffer::M_WRITE_DISCARD,
            &mappedData, &mappedSize ) ) {
            LogError() << "Failed to map instance buffer for node attachments!";
            return;
        }

        auto* destData = static_cast<NodeAttachmentInstanceData*>(mappedData);
        unsigned int currentIdx = 0;

        for ( size_t i = 0; i < instancedDrawItems.size(); ) {
            // Find the end of this batch (same mesh + texture)
            size_t batchStart = i;
            auto batchMesh = instancedDrawItems[i].mesh;
            auto meshId = batchMesh->meshId;
            zCTexture* batchTex = instancedDrawItems[i].texture;
            zCMaterial* batchMat = instancedDrawItems[i].material;
            const float batchMaterialClassMarker = instancedDrawItems[i].materialClassMarker;

            bool needAlpha = false;
            while ( i < instancedDrawItems.size()
                    && meshId > 0 // assume meshId 0 means "not batch-able"
                    && instancedDrawItems[i].mesh->meshId == meshId
                    && instancedDrawItems[i].texture == batchTex
                    && instancedDrawItems[i].materialClassMarker == batchMaterialClassMarker ) {
                // Some of them have needAlpha false, even though they share the same texture!
                // thus we now just walk all batch items and assume if one needs alpha, all do.
                needAlpha |= instancedDrawItems[i].needAlpha;
                destData[currentIdx] = instancedDrawItems[i].instanceData;
                ++currentIdx;
                ++i;
            }

            batches.push_back( { batchMesh, batchTex, batchMat, batchMaterialClassMarker,
                static_cast<unsigned int>(batchStart),
                static_cast<unsigned int>(i - batchStart),
                needAlpha } );
        }

        NodeAttachmentInstancingBuffer->Unmap();

        // Draw calls

        SetActiveVertexShader( VShaderID::VS_ExNodeInstanced );
        ActiveVS->Apply();
        SetupVS_ExMeshDrawCall();
        SetupVS_ExConstantBuffer();

        if ( isMainOrGhost ) {
            SetActivePixelShader( PShaderID::PS_DiffuseAlphaTest );
            BindActivePixelShader();
        }

        Context->IASetPrimitiveTopology( D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST );

        // Bind instance buffer to slot 1 (persists across batches)
        UINT instOffset = 0;
        UINT instStride = sizeof( NodeAttachmentInstanceData );
        Context->IASetVertexBuffers( 1, 1, NodeAttachmentInstancingBuffer->GetVertexBuffer().GetAddressOf(), &instStride, &instOffset );

        wantShader = true;

        if ( !isMainOrGhost && !isShadowPass ) {
            // ghost or other: keep the pixel shader whatever was set
        } else if ( isShadowPass ) {
            bool linearDepth = (graphicsState.FF_GSwitches & GSWITCH_LINEAR_DEPTH) != 0;
            if ( linearDepth ) {
                ActivePS = ShaderManager->GetPShader( PShaderID::PS_LinDepth );
                ActivePS->Apply();
            } else {
                Context->PSSetShader( nullptr, nullptr, 0 );
                ActivePS = nullptr;
            }
            wantShader = false;
        }

        D3D11VertexBuffer* lastVB = nullptr;
        D3D11VertexBuffer* lastIB = nullptr;

        MaterialInfo* lastMaterialInfo = nullptr;

        void* lastBatchTex = nullptr;
        float lastMaterialClassMarker = 999.0f;
        auto lastSwitches = graphicsState.FF_GSwitches;
        void* lastPs = nullptr;

        // otherwise shadows of streetlamps are not accurate
        const bool needsAlphaTesting = isShadowPass || isZPrepass;

        for ( const auto& batch : batches ) {
            MeshInfo* mi = batch.mesh;

            MaterialInfo* info = nullptr;
            if ( batch.texture ) {
                info = Engine::GAPI->GetMaterialInfoFrom( batch.texture );
                if ( needsAlphaTesting && info->MaterialType == MaterialInfo::MT_FullAlpha ) {
                    continue;
                }
            }

            // Bind texture for non-shadow passes
            if ( needsAlphaTesting ) {
                if ( batch.needAlpha ) {
                    if ( !batch.texture || batch.texture->CacheIn( 0.6f ) != zRES_CACHED_IN ) {
                        // can't do alpha without a texture
                        continue;
                    }
                    if ( lastBatchTex != batch.texture ) {
                        batch.texture->GetSurface()->GetEngineTexture()->BindToPixelShader( 0 );
                        lastBatchTex = batch.texture;
                    }
                    SetActivePixelShader( PShaderID::PS_DiffuseAlphaTestShadows );
                    if ( ActivePS.get() != lastPs ) {
                        ActivePS->Apply();
                        lastPs = ActivePS.get();
                    }
                } else if ( lastPs != nullptr ) {
                    ActivePS = nullptr;
                    lastPs = nullptr;
                    GetContext()->PSSetShader( nullptr, nullptr, 0 );
                }
            } else if ( wantShader && batch.texture
                && (batch.texture != lastBatchTex
                    || batch.materialClassMarker != lastMaterialClassMarker) ) {
                const bool materialClassChanged =
                    batch.materialClassMarker != lastMaterialClassMarker;
                if ( !BindTextureNRFX( batch.texture, isMainOrGhost,
                    materialClassChanged || info != lastMaterialInfo,
                    batch.materialClassMarker ) ) {
                    continue;
                }
                lastMaterialInfo = info;
                lastBatchTex = batch.texture;
                lastMaterialClassMarker = batch.materialClassMarker;
            }

            // Set up alpha test state from material
            if ( batch.material ) {
                if ( batch.material->GetAlphaFunc() == zRND_ALPHA_FUNC_TEST )
                    graphicsState.FF_GSwitches |= GSWITCH_ALPHAREF;
                else
                    graphicsState.FF_GSwitches &= ~GSWITCH_ALPHAREF;

                if ( lastSwitches != graphicsState.FF_GSwitches ) {
                    lastSwitches = graphicsState.FF_GSwitches;
                    cbFFPipelineConstantBuffer.Update( &lastSwitches );
                    UpdateRenderStates();
                }
            }

            // Bind mesh VB to slot 0 (only when changed)
            if ( mi->MeshVertexBuffer != lastVB ) {
                UINT vbOffset = 0;
                UINT vbStride = sizeof( ExVertexStruct );
                Context->IASetVertexBuffers( 0, 1, mi->MeshVertexBuffer->GetVertexBuffer().GetAddressOf(), &vbStride, &vbOffset );
                lastVB = mi->MeshVertexBuffer;
            }

            // Bind IB (only when changed)
            if ( mi->MeshIndexBuffer && mi->MeshIndexBuffer != lastIB ) {
                Context->IASetIndexBuffer( mi->MeshIndexBuffer->GetVertexBuffer().Get(), VERTEX_INDEX_DXGI_FORMAT, 0 );
                lastIB = mi->MeshIndexBuffer;
            }

            // Draw instanced
            if ( mi->MeshIndexBuffer ) {
                const unsigned int numIndices = static_cast<unsigned int>(mi->Indices.size());
                Context->DrawIndexedInstanced( numIndices, batch.instanceCount, 0, 0, batch.startInstance );

                Engine::GAPI->GetRendererState().RendererInfo.FrameDrawnTriangles +=
                    (numIndices / 3) * batch.instanceCount;
            } else {
                const unsigned int numVertices = static_cast<unsigned int>(mi->Vertices.size());
                Context->DrawInstanced( numVertices, batch.instanceCount, 0, batch.startInstance );

                Engine::GAPI->GetRendererState().RendererInfo.FrameDrawnTriangles +=
                    (numVertices / 3) * batch.instanceCount;
            }
        }

        // Unbind instance buffer from slot 1
        ID3D11Buffer* nullBuf = nullptr;
        UINT nullStride = 0;
        UINT nullOffset = 0;
        Context->IASetVertexBuffers( 1, 1, &nullBuf, &nullStride, &nullOffset );

    }
}

/** Binds the active PixelShader */
XRESULT D3D11GraphicsEngine::BindActivePixelShader() {
    if ( ActivePS ) ActivePS->Apply();
    return XR_SUCCESS;
}

XRESULT D3D11GraphicsEngine::BindActiveVertexShader() {
    if ( ActiveVS ) ActiveVS->Apply();
    return XR_SUCCESS;
}

/** Unbinds the texture at the given slot */
XRESULT D3D11GraphicsEngine::UnbindTexture( int slot ) {
    GetContext()->PSSetShaderResources( slot, 1, s_nullSRVs );
    GetContext()->VSSetShaderResources( slot, 1, s_nullSRVs );

    return XR_SUCCESS;
}

/** Recreates the renderstates */
XRESULT D3D11GraphicsEngine::UpdateRenderStates() {

    auto& states = Engine::GAPI->GetRendererState();
    auto& blendState = states.BlendState;
    auto& rasterState = states.RasterizerState;
    auto& depthState = states.DepthState;

    /*
    blendState.HashThis( reinterpret_cast<char*>(&blendState), blendState.StructSize );
    rasterState.HashThis( reinterpret_cast<char*>(&rasterState), rasterState.StructSize );
    depthState.HashThis( reinterpret_cast<char*>(&depthState), depthState.StructSize );
    */

    if ( blendState.StateDirty &&
        blendState.Hash != FFBlendStateHash ) {
        D3D11BlendStateInfo* state = static_cast<D3D11BlendStateInfo*>
            (GothicStateCache::s_BlendStateMap[blendState]);

        if ( !state ) {
            // Create new state
            state =
                new D3D11BlendStateInfo( blendState );

            GothicStateCache::s_BlendStateMap[blendState] = state;
        }

        FFBlendState = state->State.Get();
        FFBlendStateHash = blendState.Hash;

        blendState.StateDirty = false;
        GetContext()->OMSetBlendState( FFBlendState.Get(), float4( 0, 0, 0, 0 ).toPtr(),
            0xFFFFFFFF );
    }

    if ( rasterState.StateDirty &&
        rasterState.Hash !=
        FFRasterizerStateHash ) {
        D3D11RasterizerStateInfo* state = static_cast<D3D11RasterizerStateInfo*>
            (GothicStateCache::s_RasterizerStateMap[rasterState]);

        if ( !state ) {
            // Create new state
            state = new D3D11RasterizerStateInfo(
                rasterState );

            GothicStateCache::s_RasterizerStateMap[rasterState] = state;
        }

        FFRasterizerState = state->State.Get();
        FFRasterizerStateHash = rasterState.Hash;

        rasterState.StateDirty = false;
        GetContext()->RSSetState( FFRasterizerState.Get() );
    }

    if ( depthState.StateDirty &&
        depthState.Hash !=
        FFDepthStencilStateHash ) {

        D3D11DepthBufferState* state = static_cast<D3D11DepthBufferState*>
            (GothicStateCache::s_DepthBufferMap[depthState]);

        if ( !state ) {
            // Create new state
            state = new D3D11DepthBufferState(
                depthState );

            GothicStateCache::s_DepthBufferMap[depthState] = state;
#ifdef DEBUG_D3D11
            std::stringstream ss;
            ss << (state->Values.DepthBufferEnabled ? "E1" : "E0") << "|";
            ss << (state->Values.DepthWriteEnabled ? "W1" : "W0") << "|";

            static const char* strs[9]{
                "NONE",
                "NEVER",
                "LESS",
                "EQUAL",
                "LESS_EQUAL",
                "GREATER",
                "NOT_EQUAL",
                "GREATER_EQUAL",
                "ALWAYS",
            };
            ss << strs[state->Values.DepthBufferCompareFunc];

            SetDebugName( state->State.Get(), ss.str() );
#endif
        }

        FFDepthStencilState = state->State.Get();
        FFDepthStencilStateHash = depthState.Hash;

        depthState.StateDirty = false;
        GetContext()->OMSetDepthStencilState( FFDepthStencilState.Get(), 0 );
    }

    return XR_SUCCESS;
}

namespace {
    // Used to notify the zEngine that we changed the viewport
    // used at the start of world rendering and when transitioning to HUD rendering to update the viewport of the zEngine's camera
    void UpdateZEngineViewport() {
        if ( auto game = oCGame::GetGame(); game && game->_zCSession_camera ) {
            ((zCCamera*)game->_zCSession_camera)->UpdateViewport();
        }
    }

    D3D11VertexBuffer* GetShadowAwareIndexBuffer( MeshInfo* mesh, bool isAlpha ) {
        if ( !mesh ) {
            return nullptr;
        }

        if ( isAlpha ) {
            return mesh->MeshIndexBuffer;
        }

        if ( mesh->MeshShadowIndexBuffer && !mesh->ShadowIndices.empty() ) {
            return mesh->MeshShadowIndexBuffer;
        }
        return mesh->MeshIndexBuffer;
    }

    unsigned int GetShadowAwareIndexCount( const MeshInfo* mesh, bool isAlpha ) {
        if ( !mesh ) {
            return 0;
        }

        if ( isAlpha ) {
            return static_cast<unsigned int>( mesh->Indices.size() );
        }
        return static_cast<unsigned int>( mesh->ShadowIndices.empty()
            ? mesh->Indices.size() : mesh->ShadowIndices.size() );
    }

    bool IsShadowWorldCasterVisible( const ShadowWorldCaster& caster, const Frustum* frustum ) {
        if ( !frustum ) {
            return true;
        }
        if ( !caster.Mesh ) {
            return false;
        }
        if ( caster.Mesh->HasBoundingBox ) {
            return frustum->Contains( caster.Mesh->BoundingBox ) != DirectX::ContainmentType::DISJOINT;
        }
        if ( caster.Section
            && caster.Section->BoundingBox.Min.x <= caster.Section->BoundingBox.Max.x
            && caster.Section->BoundingBox.Min.y <= caster.Section->BoundingBox.Max.y
            && caster.Section->BoundingBox.Min.z <= caster.Section->BoundingBox.Max.z ) {
            return frustum->Contains( caster.Section->BoundingBox ) != DirectX::ContainmentType::DISJOINT;
        }
        return true;
    }

    bool EnsureShadowLodIndexBuffer( MeshInfo* mesh ) {
        if ( !mesh || mesh->ShadowLodBuildAttempted ) {
            return mesh && mesh->MeshShadowLodIndexBuffer
                && !mesh->ShadowLodIndices.empty();
        }

        mesh->ShadowLodBuildAttempted = true;
        // Morph-updated meshes change their vertex positions after loading;
        // a persistent static LOD would become incorrect for those meshes.
        if ( mesh->Vertices.empty() || !mesh->PreviousMorphPositions.empty() ) {
            return false;
        }

        const std::vector<VERTEX_INDEX>& source = mesh->ShadowIndices.empty()
            ? mesh->Indices : mesh->ShadowIndices;
        if ( source.size() < 12 || source.size() % 3 != 0 ) {
            return false;
        }

        const size_t targetIndexCount = std::max<size_t>(
            3, (source.size() * 35u / 100u) / 3u * 3u );
        if ( targetIndexCount >= source.size() ) {
            return false;
        }

        std::vector<unsigned int> sourceIndices( source.size() );
        for ( size_t i = 0; i < source.size(); ++i ) {
            sourceIndices[i] = source[i];
        }
        std::vector<unsigned int> simplified( source.size() );
        const size_t simplifiedCount = meshopt_simplify(
            simplified.data(), sourceIndices.data(), sourceIndices.size(),
            &mesh->Vertices[0].Position.x, mesh->Vertices.size(),
            sizeof( ExVertexStruct ), targetIndexCount, 0.02f );
        if ( simplifiedCount < 3 || simplifiedCount >= source.size()
            || simplifiedCount % 3 != 0 ) {
            return false;
        }

        const unsigned int maxIndex = std::numeric_limits<VERTEX_INDEX>::max();
        mesh->ShadowLodIndices.resize( simplifiedCount );
        for ( size_t i = 0; i < simplifiedCount; ++i ) {
            if ( simplified[i] > maxIndex ) {
                mesh->ShadowLodIndices.clear();
                return false;
            }
            mesh->ShadowLodIndices[i] = static_cast<VERTEX_INDEX>( simplified[i] );
        }

        Engine::GraphicsEngine->CreateVertexBuffer( &mesh->MeshShadowLodIndexBuffer );
        if ( !mesh->MeshShadowLodIndexBuffer
            || mesh->MeshShadowLodIndexBuffer->Init(
                mesh->ShadowLodIndices.data(),
                mesh->ShadowLodIndices.size() * sizeof( VERTEX_INDEX ),
                D3D11VertexBuffer::B_INDEXBUFFER,
                D3D11VertexBuffer::U_IMMUTABLE ) != XR_SUCCESS ) {
            delete mesh->MeshShadowLodIndexBuffer;
            mesh->MeshShadowLodIndexBuffer = nullptr;
            mesh->ShadowLodIndices.clear();
            return false;
        }
        return true;
    }

    D3D11VertexBuffer* GetShadowLodIndexBuffer( MeshInfo* mesh ) {
        return EnsureShadowLodIndexBuffer( mesh )
            ? mesh->MeshShadowLodIndexBuffer : nullptr;
    }

    unsigned int GetShadowLodIndexCount( const MeshInfo* mesh ) {
        return mesh && !mesh->ShadowLodIndices.empty()
            ? static_cast<unsigned int>( mesh->ShadowLodIndices.size() ) : 0u;
    }
}

/** Called when we started to render the world */
XRESULT D3D11GraphicsEngine::OnStartWorldRendering() {
    TracyD3D11ZoneCGX( "D3D11GraphicsEngine::OnStartWorldRendering");

    if ( Engine::IsShuttingDown() || !Engine::GAPI || !Context || !PfxRenderer
        || !PfxRenderer->GetTexturePool()
        || !HDRBackBuffer || !Backbuffer || !VelocityBuffer || !DepthStencilBuffer
        || !m_SwapchainDepthStencilBuffer || !ShadowMaps
        || !HDRBackBuffer->GetRenderTargetView() || !HDRBackBuffer->GetShaderResView()
        || !Backbuffer->GetRenderTargetView() || !Backbuffer->GetShaderResView()
        || !VelocityBuffer->GetRenderTargetView() || !VelocityBuffer->GetShaderResView()
        || !DepthStencilBuffer->GetDepthStencilView()
        || !m_SwapchainDepthStencilBuffer->GetDepthStencilView()
        || !InfiniteRangeConstantBuffer || !OutdoorSmallVobsConstantBuffer
        || !OutdoorVobsConstantBuffer ) {
        PresentPending = false;
        return XR_FAILED;
    }

    const auto* loadedWorldInfo = Engine::GAPI->GetLoadedWorldInfo();
    if ( !loadedWorldInfo || !loadedWorldInfo->MainWorld || !loadedWorldInfo->BspTree
        || !Engine::GAPI->IsWorldRenderCacheReady() ) {
        PresentPending = false;
        return XR_FAILED;
    }

    SetDefaultStates();
    m_FrameNeedsJitter = false;

    auto& rendererState = Engine::GAPI->GetRendererState();
    const uint64_t performanceSettingsKey = BuildGpuVobPerformanceSettingsKey();
    const uint64_t worldGeneration = Engine::GAPI->GetCityWindowConfigurationGeneration();
    PollGpuVobVisibleCountReadback( performanceSettingsKey, worldGeneration );
    PollGpuTimingQueries( performanceSettingsKey, worldGeneration );

    if ( !m_GpuVobPerformanceSettingsInitialized ) {
        m_GpuVobPerformanceSettingsInitialized = true;
        m_GpuVobPerformanceSettingsKey = performanceSettingsKey;
        m_GpuVobPerformanceWorldGeneration = worldGeneration;
    } else if ( performanceSettingsKey != m_GpuVobPerformanceSettingsKey
        || worldGeneration != m_GpuVobPerformanceWorldGeneration ) {
        // A 120-frame result is only useful when all frames used the same
        // render configuration and world-generation snapshot. Discard the
        // incomplete window as soon as either changes.
        if ( m_GpuVobPerformanceStats.Frames > 0 ) {
            LogInfo() << "[GpuVobPerf] discarded incomplete measurement window"
                << " reason=" << (performanceSettingsKey != m_GpuVobPerformanceSettingsKey
                    ? "settings" : "world")
                << " frames=" << m_GpuVobPerformanceStats.Frames;
        }
        m_GpuVobPerformanceStats.Reset();
        m_LastGpuVobFrameStatsAccumulated = 0;
        m_HasLastGpuMainVisibleTrianglesEstimate = false;
        m_HasLastGpuMainVisibilityRatio = false;
        m_LastGpuMainVisibilityRatio = 0.0;
        m_LastGpuMainVisibleTrianglesEstimate = 0;
        m_GpuVobPerformanceSettingsKey = performanceSettingsKey;
        m_GpuVobPerformanceWorldGeneration = worldGeneration;
    }

    if ( m_GpuVobPerformanceStats.Frames >= 120 ) {
        LogGpuVobPerformanceStats();
        m_GpuVobPerformanceStats.Reset();
        m_LastGpuVobFrameStatsAccumulated = 0;
    }
    ++m_GpuVobPerformanceStats.Frames;
    if ( rendererState.RendererInfo.FPS > 0 ) {
        ++m_GpuVobPerformanceStats.FpsSamples;
        m_GpuVobPerformanceStats.FpsTotal += rendererState.RendererInfo.FPS;
    }
    const double frameTimeMs = static_cast<double>( Engine::GAPI->GetFrameTimeSec() ) * 1000.0;
    if ( frameTimeMs > 0.0 && frameTimeMs < 10000.0 ) {
        ++m_GpuVobPerformanceStats.FrameTimeSamples;
        m_GpuVobPerformanceStats.FrameTimeMsTotal += frameTimeMs;
        if ( m_GpuVobPerformanceStats.FrameTimeMsMin == 0.0
            || frameTimeMs < m_GpuVobPerformanceStats.FrameTimeMsMin ) {
            m_GpuVobPerformanceStats.FrameTimeMsMin = frameTimeMs;
        }
        m_GpuVobPerformanceStats.FrameTimeMsMax = std::max(
            m_GpuVobPerformanceStats.FrameTimeMsMax, frameTimeMs );
    }

    if ( rendererState.RendererSettings.DisableRendering )
        return XR_SUCCESS;

    if ( PresentPending ) return XR_SUCCESS;

    BeginGpuTimingFrame( performanceSettingsKey, worldGeneration );

    RenderGraph graph( GetPfxRenderer()->GetTexturePool() );
    // The pre-light mask is frame-local. Clear the previous frame's state
    // before any renderer-specific pass can bind it.
    PfxRenderer->ResetXeGTAOPreLightState();

    // TODO: Replace global Resources with RenderGraph resource
    RGResourceHandle backBufferHandle = graph.ImportResource( L"BackBuffer", HDRBackBuffer.get() );
    RGResourceHandle velocityBufferHandle = graph.ImportResource( L"VelocityBuffer", VelocityBuffer.get() );

    rendererState.RendererInfo.RenderStage = STAGE_DRAW_WORLD;

    const XMFLOAT4X4 uiProjection = rendererState.TransformState.TransformProjUnjittered;

    SetViewport( ViewportInfo( 0, 0, GetResolution() ) );

    UpdateZEngineViewport();

    GetContext()->OMSetRenderTargets( 1, HDRBackBuffer->GetRenderTargetView().GetAddressOf(), nullptr );

    D3D11Upscaling u;
    u.UpdateUpscaling( *this );

    bool requireJitter = m_FrameNeedsJitter;

    if ( TemporalState ) {
        if ( requireJitter ) {
            TemporalState->AdvanceJitter();
        } else {
            TemporalState->OnDisabled();
        }
    }

    if ( !Engine::GAPI->IsGamePaused() ) {
        ApplyWindProps( g_windBuffer );
    }

    if ( FeatureLevel10Compatibility ) {
        // Disable here what we can't draw in feature level 10 compatibility
        rendererState.RendererSettings.AoMode = AOMode::AO_NONE;
        rendererState.RendererSettings.AntiAliasingMode = GothicRendererSettings::E_AntiAliasingMode::AA_NONE;
    }

#if BUILD_SPACER_NET
    bool bDrawVobsGlobal = zCVob::GetDrawVobs();

    rendererState.RendererSettings.DrawVOBs = bDrawVobsGlobal;
    rendererState.RendererSettings.DrawMobs = bDrawVobsGlobal;
    rendererState.RendererSettings.DrawParticleEffects = bDrawVobsGlobal;
    rendererState.RendererSettings.DrawSkeletalMeshes = bDrawVobsGlobal;
#endif

    Engine::GAPI->SetFarPlane( rendererState.RendererSettings.SectionDrawRadius * WORLD_SECTION_SIZE );
    // Clear textures from the last frame
    RenderedVobs.clear();
    FrameWaterSurfaces.clear();
    FrameTransparencyMeshes.clear();
    FrameTransparencyMeshesPortal.clear();

    m_FrameGeometryCache.Reset();
    m_GpuVobFrameMainCandidateTriangles = 0;
    GpuVobHiZBuildAttemptedThisFrame = false;
    GpuVobHiZBuiltThisFrame = false;

    // Reset the per-frame texture-cache counter.
    zCTextureCacheHack::NumNotCachedTexturesInFrame = 0;

    // Re-Bind the default sampler-state in case it was overwritten
    GetContext()->PSSetSamplers( 0, 1, DefaultSamplerState.GetAddressOf() );
    GetContext()->CSSetSamplers( 0, 1, DefaultSamplerState.GetAddressOf() );

    // Update view distances
    static const float4 defaultInfiniteRange = float4( FLT_MAX, 0, 0, 0 );
    InfiniteRangeConstantBuffer->UpdateBuffer( &defaultInfiniteRange );

    const float4 outdoorSmallRange( rendererState.RendererSettings.OutdoorSmallVobDrawRadius, 0, 0, 0 );
    OutdoorSmallVobsConstantBuffer->UpdateBuffer( &outdoorSmallRange );

    const float4 outdoorRange( rendererState.RendererSettings.OutdoorVobDrawRadius, 0, 0, 0 );
    OutdoorVobsConstantBuffer->UpdateBuffer( &outdoorRange );

    rendererState.RasterizerState.FrontCounterClockwise = false;
    rendererState.RasterizerState.SetDirty();

    if ( rendererState.RendererSettings.EnableShadows ) {
        ShadowMaps->PrepareRender();
    }

    RGResourceHandle colorResource = backBufferHandle;
    graph.AddPass( RG_PASS_NAME("Initialize Buffers"), [&]( RGBuilder& builder, RenderPass& pass ) {
        auto size = GetResolution();
        if ( rendererState.RendererSettings.RendererMode == GothicRendererSettings::E_RendererMode::RM_Deferred ) {
            colorResource = builder.CreateTexture( { static_cast<uint32_t>(size.x), static_cast<uint32_t>(size.y), DXGI_FORMAT_ENGINE_DEFAULT, L"GBufferAlbedo" } );
        }

        builder.Write( colorResource );
        builder.Write( backBufferHandle );

        pass.m_executeCallback = [this, &rendererState, colorResource](const RenderGraph& graph)->void {
            const Microsoft::WRL::ComPtr<ID3D11DeviceContext1>& context = GetContext();
            context->ClearDepthStencilView( DepthStencilBuffer->GetDepthStencilView().Get(), D3D11_CLEAR_DEPTH, 0, 0 );
            context->ClearDepthStencilView( m_SwapchainDepthStencilBuffer->GetDepthStencilView().Get(), D3D11_CLEAR_DEPTH, 0, 0 );

            const float clearColor[4] = { 0.f, 0.f, 0.f, 0.f };
            context->ClearRenderTargetView( HDRBackBuffer->GetRenderTargetView().Get(), clearColor );
            context->ClearRenderTargetView( Backbuffer->GetRenderTargetView().Get(), clearColor );

            float4 fogColor( rendererState.RendererSettings.AtmosphericScattering
                ? rendererState.RendererSettings.FogColorMod
                : rendererState.GraphicsState.FF_FogColor, 0.0f );
            GetContext()->ClearRenderTargetView( graph.GetPhysicalTexture( colorResource )->GetRenderTargetView().Get(), reinterpret_cast<const float*>(&fogColor) );
        };
    });

    RGResourceHandle normalsResource;
    RGResourceHandle specularResource;
    RGResourceHandle reactiveMaskResource;
    RGResourceHandle transparencyAndCompositionMaskResource;
    // Re-evaluate active renderer each frame (allows runtime switching)
    SelectActiveRenderer();
    if ( !ActiveSceneRenderer ) {
        PresentPending = false;
        return XR_FAILED;
    }
    ActiveSceneRenderer->AddGeometryPasses( graph, *this,
        colorResource, velocityBufferHandle, backBufferHandle,
        normalsResource, specularResource, reactiveMaskResource,
        transparencyAndCompositionMaskResource );

    graph.AddPass( RG_PASS_NAME("Draw ParticleFX #1"), [&]( RGBuilder& builder, RenderPass& pass ) {
        // Setup / Declare
        builder.Write( backBufferHandle );

        pass.m_executeCallback = [this](const RenderGraph&)-> void {
            if ( !Engine::GAPI->GetRendererState().RendererSettings.DrawParticleEffects ) {
                return;
            }
            zCCamera* camera = zCCamera::GetCamera();
            if ( !camera ) {
                return;
            }
            TracyD3D11ZoneCGX( "D3D11GraphicsEngine::Draw ParticleFX #1" );
            std::vector<zCVob*> decals;
            camera->Activate();
            // Camera->Activate breaks viewport
            SetViewport( ViewportInfo( 0, 0, GetResolution() ) );

            Engine::GAPI->GetVisibleDecalList( decals );

            Engine::GAPI->ResetRenderStates();
            DrawDecalList( decals, true );
            DrawQuadMarks();
        };
    });

    if ( rendererState.RendererSettings.RendererMode == GothicRendererSettings::RM_Deferred ) {
        AddXeGTAOPreLightingPass( graph, normalsResource, backBufferHandle );
    }

    ActiveSceneRenderer->AddLightingPasses( graph, *this,
        colorResource, normalsResource, specularResource,
        transparencyAndCompositionMaskResource, backBufferHandle, m_FrameLights );

    // XeGTAO is composited before transparent alpha meshes so particles, fire and
    // other translucent effects are not darkened by opaque-world AO.
    if ( rendererState.RendererSettings.AoMode == AOMode::AO_XEGTAO ) {
        const bool preLightingRequested = rendererState.RendererSettings.GetEffectiveXeGTAOPreLighting();
        graph.AddPass( RG_PASS_NAME("XeGTAO"), [&]( RGBuilder& builder, RenderPass& pass ) {
            builder.Read( normalsResource );
            builder.Write( backBufferHandle );

            pass.m_executeCallback = [this, normalsResource, backBufferHandle, preLightingRequested]( const RenderGraph& graph ) {
                TracyD3D11ZoneCGX( "D3D11GraphicsEngine::Draw XeGTAO" );
                if ( preLightingRequested && PfxRenderer->IsXeGTAOPreLightReady() ) return;
                auto normalsTexture = graph.GetPhysicalTexture( normalsResource );
                auto backBuffer = graph.GetPhysicalTexture( backBufferHandle );
                PfxRenderer->RenderXeGTAO(
                    GetDepthBufferCopy()->GetShaderResView().Get(),
                    normalsTexture->GetShaderResView().Get(),
                    backBuffer->GetRenderTargetView().Get() );
                GetContext()->PSSetSamplers( 0, 1, DefaultSamplerState.GetAddressOf() );
            };
        } );
    }

    // Shared state for the PostFX composition pass
    ID3D11ShaderResourceView* compositionGodRaysSRV = nullptr;
    bool isOutdoor = loadedWorldInfo->BspTree->GetBspTreeMode() == zBSP_MODE_OUTDOOR;
    const bool isDragonIsland = NormalizeVisualStemForMarker( loadedWorldInfo->WorldName ) == "DRAGONISLAND";
    GSky* godRaySky = Engine::GAPI->GetSky();
    const float godRayRain = godRaySky
        ? std::clamp( godRaySky->GetAtmosphereCB().AC_RainFXWeight, 0.0f, 1.0f )
        : 0.0f;
    const float godRayRainTransition = std::clamp( (godRayRain - 0.05f) / 0.60f, 0.0f, 1.0f );
    const float godRayRainSmoothOcclusion = godRayRainTransition * godRayRainTransition
        * (3.0f - 2.0f * godRayRainTransition);
    const float godRayRainVisibility = 1.0f - godRayRainSmoothOcclusion;
    const bool godRayAtmosphereVisible = godRaySky
        && godRaySky->GetAtmosphereSettings().LightDirection.y > 0.0f
        && rendererState.RendererSettings.GodRayStrength > 0.0f
        && godRayRainVisibility > 0.0f;
    bool compositionGodRays = (rendererState.RendererSettings.AreGodRaysEnabled()
        && isOutdoor && godRayAtmosphereVisible);
    bool compositionHeightFog = (rendererState.RendererSettings.DrawFog && isOutdoor);
    const float dynamicCloudRainWeight = Engine::GAPI->GetRainFXWeight();
    const float dynamicCloudWorldFog = std::clamp( Engine::GAPI->GetFogOverride(), 0.0f, 1.0f );
    // The shader fades clouds continuously from fog override 0.08 to 0.55.
    // Once fully hidden, omit the raymarch and every dependent cloud pass.
    const bool dynamicCloudsVisibleThroughWorldFog = dynamicCloudWorldFog < 0.55f;
    bool compositionLowClouds = (!isDragonIsland && rendererState.RendererSettings.EnableDynamicClouds
        && rendererState.RendererSettings.DrawFog && isOutdoor && dynamicCloudRainWeight < 0.90f
        && dynamicCloudsVisibleThroughWorldFog);
    const bool fsr3UpscalingActive = GetDevice()->GetFeatureLevel() >= D3D_FEATURE_LEVEL_11_0
        && rendererState.RendererSettings.Upscaler == GothicRendererSettings::UPSCALER_FSR_3
        && rendererState.RendererSettings.ResolutionScalePercent <= 100
        && rendererState.RendererSettings.AntiAliasingMode == GothicRendererSettings::AA_FSR3
        && PfxRenderer && PfxRenderer->GetFSR3() && TemporalState;
    bool compositionNeedsDepth = compositionHeightFog;
    bool compositionActive = compositionGodRays || compositionNeedsDepth;

    const bool renderTemporalSkyVelocity = rendererState.RendererSettings.DrawSky
        && fsr3UpscalingActive;
    XMFLOAT4X4 skyCurrentInvViewProj;
    const XMFLOAT4X4 skyPreviousViewProj = m_PrevViewProjMatrix;
    XMFLOAT2 skyJitterOffset( 0.0f, 0.0f );
    if ( TemporalState ) {
        const XMFLOAT4X4& currentViewProj = TemporalState->GetUnjitteredViewProj();
        XMStoreFloat4x4( &skyCurrentInvViewProj,
            XMMatrixInverse( nullptr, XMLoadFloat4x4( &currentViewProj ) ) );
        skyJitterOffset = TemporalState->GetJitterOffset();
    } else {
        XMFLOAT4X4 currentProjection = Engine::GAPI->GetProjectionMatrix();
        currentProjection._13 = 0.0f;
        currentProjection._23 = 0.0f;
        const XMMATRIX currentViewProj = XMMatrixMultiply(
            XMLoadFloat4x4( &currentProjection ), Engine::GAPI->GetViewMatrixXM() );
        XMStoreFloat4x4( &skyCurrentInvViewProj, XMMatrixInverse( nullptr, currentViewProj ) );
    }
    if ( rendererState.RendererSettings.DrawSky ) {
        graph.AddPass( RG_PASS_NAME( "Draw Sky" ), [&]( RGBuilder& builder, RenderPass& pass ) {
            builder.Write( backBufferHandle );

            pass.m_executeCallback = [this, backBufferHandle]( const RenderGraph& graph )->void {
                TracyD3D11ZoneCGX( "D3D11GraphicsEngine::Draw Sky" );
                // Draw back of the sky if outdoor
                GetContext()->OMSetRenderTargets( 1, graph.GetPhysicalTexture( backBufferHandle )->GetRenderTargetView().GetAddressOf(), GetDepthBuffer()->GetDepthStencilView().Get() );

                DrawSky();
            };
        } );
    }

    // Ordinary alpha-blended VOB surfaces are composited after the sky. Window
    // glass is deliberately retained for the final scene-transparency pass so
    // every later scene element remains behind the pane.
    // geometry hides the late sky through depth, which used to mask this
    // ordering bug for ordinary glass. Window pixels looking directly at the
    // clear-depth sky have no such blocker: drawing the sky afterwards replaced
    // both their authored glass alpha and the lower-screen opacity safeguard.
    graph.AddPass( RG_PASS_NAME("Draw Frame AlphaMeshes"), [&]( RGBuilder& builder, RenderPass& pass ) {
        builder.Write( backBufferHandle );

        pass.m_executeCallback = [this](const RenderGraph&)-> void {
            TracyD3D11ZoneCGX( "D3D11GraphicsEngine::Draw Frame AlphaMeshes" );
            DrawFrameAlphaMeshes();
        };
    });

    if ( renderTemporalSkyVelocity ) {
        graph.AddPass( RG_PASS_NAME( "Sky Velocity" ), [&]( RGBuilder& builder, RenderPass& pass ) {
            builder.Read( velocityBufferHandle );
            builder.Write( velocityBufferHandle );

            pass.m_executeCallback = [this, velocityBufferHandle, skyCurrentInvViewProj,
                                      skyPreviousViewProj, skyJitterOffset]( const RenderGraph& graph ) {
                TracyD3D11ZoneCGX( "D3D11GraphicsEngine::Sky Velocity" );
                auto* velocityBuffer = graph.GetPhysicalTexture( velocityBufferHandle );
                if ( !velocityBuffer ) {
                    return;
                }

                SetDefaultStates();
                UpdateRenderStates();
                SetViewport( ViewportInfo( 0, 0, GetResolution() ) );
                GetContext()->OMSetRenderTargets(
                    1, velocityBuffer->GetRenderTargetView().GetAddressOf(), nullptr );

                auto skyVelocityPS = GetShaderManager().GetPShader( PShaderID::PS_PFX_SkyVelocity );
                if ( !skyVelocityPS ) {
                    return;
                }

                SkyVelocityConstantBuffer constants = {};
                constants.InvViewProj = skyCurrentInvViewProj;
                constants.PrevViewProj = skyPreviousViewProj;
                constants.JitterOffset = skyJitterOffset;

                GetShaderManager().GetVShader( VShaderID::VS_PFX )->Apply();
                skyVelocityPS->Apply();
                skyVelocityPS->GetBuffer( "SkyVelocityConstants" ).Update( &constants ).Bind();

                ID3D11ShaderResourceView* depthSRV = GetDepthBuffer()->GetShaderResView().Get();
                GetContext()->PSSetShaderResources( 0, 1, &depthSRV );
                PfxRenderer->DrawFullScreenQuad();

                ID3D11ShaderResourceView* nullSRV = nullptr;
                GetContext()->PSSetShaderResources( 0, 1, &nullSRV );
            };
        } );
    }
    RGResourceHandle lowCloudLayerResource = RG_INVALID_HANDLE;
    RGResourceHandle lowCloudDepthResource = RG_INVALID_HANDLE;
    RGResourceHandle skyCloudLayerResource = RG_INVALID_HANDLE;
    if ( compositionLowClouds ) {
        const INT2 cloudResolution(
            std::max( 1, GetResolution().x / 2 + GetResolution().x % 2 ),
            std::max( 1, GetResolution().y / 2 + GetResolution().y % 2 ) );

        graph.AddPass( RG_PASS_NAME("Generate Low Clouds"), [&]( RGBuilder& builder, RenderPass& pass ) {
            builder.Read( backBufferHandle );
            lowCloudLayerResource = builder.CreateTexture( {
                static_cast<uint32_t>(cloudResolution.x),
                static_cast<uint32_t>(cloudResolution.y),
                DXGI_FORMAT_R16G16B16A16_FLOAT,
                L"LowCloudLayer"
            } );
            lowCloudDepthResource = builder.CreateTexture( {
                static_cast<uint32_t>(cloudResolution.x),
                static_cast<uint32_t>(cloudResolution.y),
                DXGI_FORMAT_R32_FLOAT,
                L"LowCloudDepth"
            } );
            skyCloudLayerResource = builder.CreateTexture( {
                static_cast<uint32_t>(cloudResolution.x),
                static_cast<uint32_t>(cloudResolution.y),
                DXGI_FORMAT_R16G16B16A16_FLOAT,
                L"SkyLowCloudLayer"
            } );

            pass.m_executeCallback = [this, backBufferHandle, lowCloudLayerResource, lowCloudDepthResource, skyCloudLayerResource]( const RenderGraph& graph ) {
                TracyD3D11ZoneCGX( "D3D11GraphicsEngine::Generate Low Clouds" );

                auto* backBuffer = graph.GetPhysicalTexture( backBufferHandle );
                auto* lowCloudLayer = graph.GetPhysicalTexture( lowCloudLayerResource );
                auto* lowCloudDepth = graph.GetPhysicalTexture( lowCloudDepthResource );
                auto* skyCloudLayer = graph.GetPhysicalTexture( skyCloudLayerResource );
                auto* depthBuffer = GetDepthBuffer();
                if ( !PfxRenderer
                    || !backBuffer || !backBuffer->GetShaderResView().Get()
                    || !lowCloudLayer || !lowCloudLayer->GetRenderTargetView().Get()
                    || !lowCloudDepth || !lowCloudDepth->GetRenderTargetView().Get()
                    || !skyCloudLayer || !skyCloudLayer->GetRenderTargetView().Get()
                    || !depthBuffer || !depthBuffer->GetShaderResView().Get() ) {
                    return;
                }

                PfxRenderer->RenderLowCloudLayer(
                    lowCloudLayer->GetRenderTargetView().Get(),
                    lowCloudDepth->GetRenderTargetView().Get(),
                    skyCloudLayer->GetRenderTargetView().Get(),
                    backBuffer->GetShaderResView().Get(),
                    depthBuffer->GetShaderResView().Get() );

                GetContext()->PSSetSamplers( 0, 1, DefaultSamplerState.GetAddressOf() );
            };
        } );
    }
    const bool renderRainExclusionMask = rendererState.RendererSettings.EnableRain
        && Engine::GAPI->GetSceneWetness() > 1e-6f && isOutdoor;
    const bool renderWaterMask =
        renderRainExclusionMask
        || rendererState.RendererSettings.EnableDoF;
    // Keep the pass alive for rain impacts even when the Advanced F11 switch
    // disables wet-ground reflections. The shader gates only its reflection
    // composition through WG_ReflectionsEnabled; skipping this pass would
    // also remove the ground impact/ripple layer.
    const bool renderWetGroundPass = renderRainExclusionMask;
    RGResourceHandle waterMaskResource = RG_INVALID_HANDLE;
    if ( renderWaterMask ) {
        const auto maskSize = GetResolution();
        if ( !RainExclusionMaskBuffer
            || RainExclusionMaskBuffer->GetSizeX() != maskSize.x
            || RainExclusionMaskBuffer->GetSizeY() != maskSize.y ) {
            RainExclusionMaskBuffer = std::make_unique<RenderToTextureBuffer>(
                GetDevice().Get(), maskSize.x, maskSize.y, DXGI_FORMAT_R8_UNORM,
                nullptr, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, 1, 1,
                D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE );
            SetDebugName( RainExclusionMaskBuffer->GetTexture().Get(), "Rain Exclusion Mask" );
        }
        waterMaskResource = graph.ImportResource( L"RainExclusionMask", RainExclusionMaskBuffer.get() );
    }
    const auto waterCoverageSize = GetResolution();
    if ( !WaterCoverageBuffer
        || WaterCoverageBuffer->GetSizeX() != waterCoverageSize.x
        || WaterCoverageBuffer->GetSizeY() != waterCoverageSize.y ) {
        WaterCoverageBuffer = std::make_unique<RenderToTextureBuffer>(
            GetDevice().Get(), waterCoverageSize.x, waterCoverageSize.y, DXGI_FORMAT_R8_UNORM,
            nullptr, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, 1, 1,
            D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE );
        SetDebugName( WaterCoverageBuffer->GetTexture().Get(), "Water Coverage" );
    }
    const RGResourceHandle waterCoverageResource = graph.ImportResource(
        L"WaterCoverage", WaterCoverageBuffer.get() );
    const bool fsr3ActiveForReactiveMask = fsr3UpscalingActive;

    graph.AddPass( RG_PASS_NAME("DrawWaterSurfaces"), [&]( RGBuilder& builder, RenderPass& pass ) {
        builder.Read( backBufferHandle );
        builder.Read( specularResource );
        builder.Write( backBufferHandle );
        builder.Write( waterCoverageResource );
        if ( renderWaterMask ) {
            builder.Write( waterMaskResource );
        }
        if ( fsr3ActiveForReactiveMask ) {
            builder.Read( reactiveMaskResource );
            builder.Write( reactiveMaskResource );
        }
        if ( compositionLowClouds ) {
            builder.Read( lowCloudLayerResource );
        }

        pass.m_executeCallback = [this, renderWaterMask, waterMaskResource, waterCoverageResource, specularResource, fsr3ActiveForReactiveMask, reactiveMaskResource, compositionLowClouds, lowCloudLayerResource](const RenderGraph& graph) {
            SetViewport( ViewportInfo( 0, 0, GetResolution() ) );
            ID3D11RenderTargetView* waterMaskRTV = nullptr;
            if ( renderWaterMask ) {
                auto* waterMask = graph.GetPhysicalTexture( waterMaskResource );
                const float clearMask[4] = {};
                GetContext()->ClearRenderTargetView( waterMask->GetRenderTargetView().Get(), clearMask );
                waterMaskRTV = waterMask->GetRenderTargetView().Get();
            }
            ID3D11RenderTargetView* fsr3ReactiveMaskRTV = nullptr;
            if ( fsr3ActiveForReactiveMask ) {
                auto* fsr3Mask = graph.GetPhysicalTexture( reactiveMaskResource );
                fsr3ReactiveMaskRTV = fsr3Mask ? fsr3Mask->GetRenderTargetView().Get() : nullptr;
            }
            ID3D11ShaderResourceView* lowCloudLayerSRV = nullptr;
            if ( compositionLowClouds ) {
                auto* lowCloudLayer = graph.GetPhysicalTexture( lowCloudLayerResource );
                lowCloudLayerSRV = lowCloudLayer && lowCloudLayer->GetShaderResView().Get() ? lowCloudLayer->GetShaderResView().Get() : nullptr;
            }
            ID3D11RenderTargetView* waterCoverageRTV = nullptr;
            if ( auto* waterCoverage = graph.GetPhysicalTexture( waterCoverageResource ) ) {
                waterCoverageRTV = waterCoverage->GetRenderTargetView().Get();
                if ( waterCoverageRTV ) {
                    const float clearCoverage[4] = {};
                    GetContext()->ClearRenderTargetView( waterCoverageRTV, clearCoverage );
                }
            }
            auto* materialClassTexture = graph.GetPhysicalTexture( specularResource );
            ID3D11ShaderResourceView* materialClassSRV = materialClassTexture
                ? materialClassTexture->GetShaderResView().Get() : nullptr;
            DrawWaterSurfaces(
                waterMaskRTV,
                fsr3ReactiveMaskRTV,
                lowCloudLayerSRV,
                waterCoverageRTV,
                materialClassSRV );
        };
    });

    if ( renderWetGroundPass ) {
      graph.AddPass( RG_PASS_NAME("Wet Ground SSR"), [&]( RGBuilder& builder, RenderPass& pass )
      {
          builder.Read( normalsResource );
          builder.Read( waterMaskResource );
          builder.Read( specularResource );
          builder.Read( backBufferHandle );
          if ( compositionLowClouds )
          {
              builder.Read( lowCloudLayerResource );
          }
          builder.Write( backBufferHandle );
          pass.m_executeCallback = [this, backBufferHandle, normalsResource, waterMaskResource, specularResource, compositionLowClouds, lowCloudLayerResource](const RenderGraph& graph)
          {
              TracyD3D11ZoneCGX( "D3D11GraphicsEngine::Wet Ground SSR" );
              auto* backBuffer = graph.GetPhysicalTexture( backBufferHandle );
              auto* normals = graph.GetPhysicalTexture( normalsResource );
              auto* waterMask = graph.GetPhysicalTexture( waterMaskResource );
              auto* specular = graph.GetPhysicalTexture( specularResource );
              auto tempBuffer = PfxRenderer->GetTempBuffer();
              if ( !backBuffer || !normals || !waterMask || !specular || !tempBuffer )
              {
                  return;
              }
              ID3D11ShaderResourceView* lowCloudLayerSRV = nullptr;
              if ( compositionLowClouds )
              {
                  auto* lowCloudLayer = graph.GetPhysicalTexture( lowCloudLayerResource );
                  lowCloudLayerSRV = lowCloudLayer ? lowCloudLayer->GetShaderResView().Get() : nullptr;
              }
              GetContext()->CopyResource( tempBuffer->GetTexture().Get(), backBuffer->GetTexture().Get() );
              PfxRenderer->RenderWetGroundSSR(
                  backBuffer->GetRenderTargetView().Get(),
                  tempBuffer->GetShaderResView().Get(),
                  GetDepthBufferCopy()->GetShaderResView().Get(),
                  normals->GetShaderResView().Get(),
                  waterMask->GetShaderResView().Get(),
                  specular->GetShaderResView().Get(),
                  lowCloudLayerSRV
              );
          };
      });
    }

    graph.AddPass( RG_PASS_NAME("Draw FrameTransparencyMeshes"), [&]( RGBuilder& builder, RenderPass& pass ) {
        builder.Read( backBufferHandle );
        builder.Write( backBufferHandle );

        pass.m_executeCallback = [this](const RenderGraph&) {
            TracyD3D11ZoneCGX( "D3D11GraphicsEngine::Draw FrameTransparencyMeshes" );

            SetDefaultStates();
            // Setup renderstates
            Engine::GAPI->GetRendererState().RasterizerState.CullMode = GothicRasterizerStateInfo::CM_CULL_BACK;
            Engine::GAPI->GetRendererState().RasterizerState.SetDirty();
            DrawMeshInfoListAlphablended( FrameTransparencyMeshes );
        };
    });

    if ( rendererState.RendererSettings.DrawG1ForestPortals ) {
        graph.AddPass( RG_PASS_NAME("Draw ForestPortals"), [&]( RGBuilder& builder, RenderPass& pass ) {
            builder.Read( backBufferHandle );
            builder.Write( backBufferHandle );

            pass.m_executeCallback = [this](const RenderGraph&) {
                TracyD3D11ZoneCGX( "D3D11GraphicsEngine::Draw ForestPortals" );

                SetDefaultStates();
                // Setup renderstates
                Engine::GAPI->GetRendererState().RasterizerState.CullMode = GothicRasterizerStateInfo::CM_CULL_BACK;
                Engine::GAPI->GetRendererState().RasterizerState.SetDirty();
                DrawMeshInfoListAlphablended( FrameTransparencyMeshesPortal );
            };
        });
    }

    graph.AddPass( RG_PASS_NAME("Draw ghosts"), [&]( RGBuilder& builder, RenderPass& pass ) {
        builder.Read( backBufferHandle );
        builder.Write( backBufferHandle );

        pass.m_executeCallback = [this](const RenderGraph&) {
            TracyD3D11ZoneCGX( "D3D11GraphicsEngine::Draw Ghosts" );

            D3D11ENGINE_RENDER_STAGE oldStage = RenderingStage;
            SetRenderingStage( DES_GHOST );
            Engine::GAPI->DrawTransparencyVobs();
            SetRenderingStage( oldStage );
            Engine::GAPI->DrawSkeletalVN();

            // Post-processing uses the full viewport.
            SetViewport( ViewportInfo( 0, 0, GetResolution() ) );
        };
    });

    if (rendererState.RendererSettings.DrawFog &&
                Engine::GAPI->GetLoadedWorldInfo()->BspTree->GetBspTreeMode() ==
                zBSP_MODE_OUTDOOR && !compositionActive) {
        // Use the standalone pass when composition is unavailable.
        graph.AddPass( RG_PASS_NAME("Draw Heightfog"), [&]( RGBuilder& builder, RenderPass& pass ) {
            builder.Read( backBufferHandle );
            builder.Write( backBufferHandle );

            pass.m_executeCallback = [this](const RenderGraph&) {
                TracyD3D11ZoneCGX( "D3D11GraphicsEngine::Draw Heightfog" );
                PfxRenderer->RenderHeightfog();
            };
        });
    }

    const bool renderRainAfterUpscaling = fsr3ActiveForReactiveMask
        && Engine::GAPI->GetRainFXWeight() > 0.0f;
    if (Engine::GAPI->GetRainFXWeight() > 0.0f && !renderRainAfterUpscaling) {
        if ( FeatureLevel10Compatibility || Engine::GAPI->GetRendererState().RendererSettings.DrawRainThroughTransformFeedback ) {
            graph.AddPass( RG_PASS_NAME("Draw Rain"), [&]( RGBuilder& builder, RenderPass& pass ) {
                builder.Read( backBufferHandle );
                builder.Write( backBufferHandle );
                if ( renderRainExclusionMask ) {
                    builder.Read( waterMaskResource );
                }
                if ( fsr3ActiveForReactiveMask ) {
                    builder.Read( reactiveMaskResource );
                    builder.Write( reactiveMaskResource );
                }

                pass.m_executeCallback = [this, backBufferHandle, fsr3ActiveForReactiveMask, reactiveMaskResource, renderRainExclusionMask, waterMaskResource](const RenderGraph& graph) {
                    TracyD3D11ZoneCGX( "D3D11GraphicsEngine::Draw Rain" );
                    if ( fsr3ActiveForReactiveMask ) {
                        auto* backBuffer = graph.GetPhysicalTexture( backBufferHandle );
                        auto* fsr3Mask = graph.GetPhysicalTexture( reactiveMaskResource );
                        ID3D11RenderTargetView* rtvs[2] = {
                            backBuffer ? backBuffer->GetRenderTargetView().Get() : nullptr,
                            fsr3Mask ? fsr3Mask->GetRenderTargetView().Get() : nullptr
                        };
                        GetContext()->OMSetRenderTargets( 2, rtvs, DepthStencilBuffer->GetDepthStencilView().Get() );
                    }
                    ID3D11ShaderResourceView* rainExclusionSRV = nullptr;
                    if ( renderRainExclusionMask ) {
                        auto* rainExclusionMask = graph.GetPhysicalTexture( waterMaskResource );
                        rainExclusionSRV = rainExclusionMask ? rainExclusionMask->GetShaderResView().Get() : nullptr;
                        GetContext()->PSSetShaderResources( 2, 1, &rainExclusionSRV );
                    }
                    Effects->DrawRain( false, rainExclusionSRV != nullptr );
                    ID3D11ShaderResourceView* nullRainExclusion = nullptr;
                    GetContext()->PSSetShaderResources( 2, 1, &nullRainExclusion );
                };
            });
        } else {
            graph.AddPass( RG_PASS_NAME("Draw Rain CS"), [&]( RGBuilder& builder, RenderPass& pass ) {
                builder.Read( backBufferHandle );
                builder.Write( backBufferHandle );
                if ( renderRainExclusionMask ) {
                    builder.Read( waterMaskResource );
                }
                if ( fsr3ActiveForReactiveMask ) {
                    builder.Read( reactiveMaskResource );
                    builder.Write( reactiveMaskResource );
                }

                pass.m_executeCallback = [this, backBufferHandle, fsr3ActiveForReactiveMask, reactiveMaskResource, renderRainExclusionMask, waterMaskResource](const RenderGraph& graph) {
                    TracyD3D11ZoneCGX( "D3D11GraphicsEngine::Draw Rain (CS)" );
                    if ( fsr3ActiveForReactiveMask ) {
                        auto* backBuffer = graph.GetPhysicalTexture( backBufferHandle );
                        auto* fsr3Mask = graph.GetPhysicalTexture( reactiveMaskResource );
                        ID3D11RenderTargetView* rtvs[2] = {
                            backBuffer ? backBuffer->GetRenderTargetView().Get() : nullptr,
                            fsr3Mask ? fsr3Mask->GetRenderTargetView().Get() : nullptr
                        };
                        GetContext()->OMSetRenderTargets( 2, rtvs, DepthStencilBuffer->GetDepthStencilView().Get() );
                    }
                    ID3D11ShaderResourceView* rainExclusionSRV = nullptr;
                    if ( renderRainExclusionMask ) {
                        auto* rainExclusionMask = graph.GetPhysicalTexture( waterMaskResource );
                        rainExclusionSRV = rainExclusionMask ? rainExclusionMask->GetShaderResView().Get() : nullptr;
                        GetContext()->PSSetShaderResources( 2, 1, &rainExclusionSRV );
                    }
                    Effects->DrawRain_CS( false, rainExclusionSRV != nullptr );
                    ID3D11ShaderResourceView* nullRainExclusion = nullptr;
                    GetContext()->PSSetShaderResources( 2, 1, &nullRainExclusion );
                };
            });
        }
    }

    graph.AddPass( RG_PASS_NAME("Reset RenderTargets"), [&]( RGBuilder& builder, RenderPass& pass )
    {
        builder.Write( backBufferHandle );
        pass.m_executeCallback = [this, backBufferHandle](const RenderGraph& graph) {
            auto backBuffer = graph.GetPhysicalTexture(backBufferHandle);
            GetContext()->OMSetRenderTargets( 1, backBuffer->GetRenderTargetView().GetAddressOf(),
                DepthStencilBuffer->GetDepthStencilView().Get() );

            // Set viewport for gothics rendering
            SetViewport( ViewportInfo( 0, 0, GetResolution() ) );
        };
    });

    if (rendererState.RendererSettings.DrawParticleEffects) {
        graph.AddPass( RG_PASS_NAME("Draw ParticleFX #2"), [&]( RGBuilder& builder, RenderPass& pass ) {
            builder.Read( backBufferHandle );
            builder.Write( backBufferHandle );

            pass.m_executeCallback = [this](const RenderGraph&) {
                zCCamera* camera = zCCamera::GetCamera();
                if ( !camera ) {
                    return;
                }
                TracyD3D11ZoneCGX( "D3D11GraphicsEngine::Draw ParticleFX #2" );

                // Draw unlit decals
                // TODO: Only get them once!
                std::vector<zCVob*> decals;
                camera->Activate();
                // Camera->Activate breaks viewport
                SetViewport( ViewportInfo( 0, 0, GetResolution() ) );

                Engine::GAPI->GetVisibleDecalList( decals );

                // Draw stuff like candle-flames
                DrawDecalList( decals, false );
                DrawMQuadMarks();
            };
        });
    }

    if ( compositionGodRays && compositionActive ) {
        // GodRays compute-only pass writes to pool texture and skips the final additive blit.
        graph.AddPass( RG_PASS_NAME("GodRays Compute"), [&]( RGBuilder& builder, RenderPass& pass ) {
            builder.Read( backBufferHandle );
            if ( compositionLowClouds ) {
                builder.Read( lowCloudLayerResource );
            }

            pass.m_executeCallback = [this, backBufferHandle, compositionLowClouds, lowCloudLayerResource, &compositionGodRaysSRV](const RenderGraph& graph) {
                TracyD3D11ZoneCGX( "D3D11GraphicsEngine::Draw GodRays (Compute)" );

                Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
                GetContext()->PSSetShaderResources( 5, 1, srv.GetAddressOf() );

                auto backbufferResource = graph.GetPhysicalTexture(backBufferHandle);
                ID3D11ShaderResourceView* lowCloudLayerSRV = nullptr;
                if ( compositionLowClouds ) {
                    auto* lowCloudLayer = graph.GetPhysicalTexture( lowCloudLayerResource );
                    lowCloudLayerSRV = lowCloudLayer && lowCloudLayer->GetShaderResView().Get() ? lowCloudLayer->GetShaderResView().Get() : nullptr;
                }
                const auto godRayMode = Engine::GAPI->GetRendererState().RendererSettings.GodRayMode;
                if ( godRayMode == GothicRendererSettings::E_GodRayMode::GODRAYS_HIGH ) {
                    PfxRenderer->RenderCombinedGodRaysToTexture(
                        backbufferResource->GetShaderResView().Get(),
                        GetDepthBuffer()->GetShaderResView().Get(),
                        lowCloudLayerSRV,
                        &compositionGodRaysSRV );
                } else if ( godRayMode == GothicRendererSettings::E_GodRayMode::GODRAYS_LOW ) {
                    PfxRenderer->RenderGodRaysToTexture(
                        backbufferResource->GetShaderResView().Get(),
                        GetDepthBuffer()->GetShaderResView().Get(),
                        lowCloudLayerSRV,
                        &compositionGodRaysSRV );
                }
                GetContext()->PSSetSamplers( 0, 1, DefaultSamplerState.GetAddressOf() );
            };
        });
    }
    // PostFX Composition pass merges the enabled atmospheric effects in one full-screen blit.
    if ( compositionActive ) {
        graph.AddPass( RG_PASS_NAME("PostFX Composition"), [&]( RGBuilder& builder, RenderPass& pass ) {
            builder.Read( backBufferHandle );
            builder.Write( backBufferHandle );

            pass.m_executeCallback = [this, backBufferHandle, compositionNeedsDepth,
                                      compositionHeightFog, &compositionGodRaysSRV](const RenderGraph& graph) {
                TracyD3D11ZoneCGX( "D3D11GraphicsEngine::PostFX Composition" );

                auto backBuffer = graph.GetPhysicalTexture(backBufferHandle);
                // Copy backbuffer to a temp texture so it can be read as SRV while writing to RTV.
                auto tempBuffer = PfxRenderer->GetTempBuffer();
                GetContext()->CopyResource( tempBuffer->GetTexture().Get(), backBuffer->GetTexture().Get() );

                // Gather SRVs for composition
                ID3D11ShaderResourceView* depthSRV = compositionNeedsDepth ? GetDepthBuffer()->GetShaderResView().Get() : nullptr;

                PfxRenderer->RenderPostFXComposition(
                    backBuffer->GetRenderTargetView().Get(),
                    tempBuffer->GetShaderResView().Get(),
                    compositionGodRaysSRV,
                    depthSRV,
                    compositionHeightFog );

                GetContext()->PSSetSamplers( 0, 1, DefaultSamplerState.GetAddressOf() );
            };
        });
    }

    if ( compositionLowClouds ) {
        graph.AddPass( RG_PASS_NAME("PostFX Low Clouds"), [&]( RGBuilder& builder, RenderPass& pass ) {
            builder.Read( backBufferHandle );
            builder.Read( lowCloudLayerResource );
            builder.Read( lowCloudDepthResource );
            builder.Read( skyCloudLayerResource );
            builder.Write( backBufferHandle );

            pass.m_executeCallback = [this, backBufferHandle, lowCloudLayerResource, lowCloudDepthResource, skyCloudLayerResource](const RenderGraph& graph) {
                TracyD3D11ZoneCGX( "D3D11GraphicsEngine::PostFX Low Clouds" );

                auto* backBuffer = graph.GetPhysicalTexture(backBufferHandle);
                auto* lowCloudLayer = graph.GetPhysicalTexture(lowCloudLayerResource);
                auto* lowCloudDepth = graph.GetPhysicalTexture(lowCloudDepthResource);
                auto* skyCloudLayer = graph.GetPhysicalTexture(skyCloudLayerResource);
                auto tempBuffer = PfxRenderer->GetTempBuffer();
                if ( !PfxRenderer
                    || !backBuffer || !backBuffer->GetTexture().Get() || !backBuffer->GetShaderResView().Get()
                    || !lowCloudLayer || !lowCloudLayer->GetShaderResView().Get()
                    || !lowCloudDepth || !lowCloudDepth->GetShaderResView().Get()
                    || !skyCloudLayer || !skyCloudLayer->GetShaderResView().Get()
                    || !tempBuffer || !tempBuffer->GetTexture().Get() || !tempBuffer->GetRenderTargetView().Get() || !tempBuffer->GetShaderResView().Get() ) {
                    return;
                }

                GetContext()->CopyResource( tempBuffer->GetTexture().Get(), backBuffer->GetTexture().Get() );

                PfxRenderer->CompositeLowClouds(
                    backBuffer->GetRenderTargetView().Get(),
                    tempBuffer->GetShaderResView().Get(),
                    lowCloudLayer->GetShaderResView().Get(),
                    lowCloudDepth->GetShaderResView().Get(),
                    GetDepthBuffer()->GetShaderResView().Get(),
                    skyCloudLayer->GetShaderResView().Get() );

                GetContext()->PSSetSamplers( 0, 1, DefaultSamplerState.GetAddressOf() );
            };
        });
    }
    if ( rendererState.RendererSettings.EnableDoF ) {
        graph.AddPass( RG_PASS_NAME("Draw DepthOfField"), [&]( RGBuilder& builder, RenderPass& pass ) {
            builder.Read( backBufferHandle );
            builder.Read( waterMaskResource );
            builder.Read( specularResource );
            builder.Write( backBufferHandle );

            pass.m_executeCallback = [this, backBufferHandle, waterMaskResource, specularResource](const RenderGraph& graph) {
                TracyD3D11ZoneCGX( "D3D11GraphicsEngine::Draw DepthOfField" );
                auto backbufferResource = graph.GetPhysicalTexture( backBufferHandle );
                auto waterMaskTexture = graph.GetPhysicalTexture( waterMaskResource );
                auto specularTexture = graph.GetPhysicalTexture( specularResource );

                if ( !PfxRenderer
                    || !backbufferResource
                    || !backbufferResource->GetShaderResView().Get()
                    || !waterMaskTexture
                    || !waterMaskTexture->GetShaderResView().Get()
                    || !specularTexture
                    || !specularTexture->GetShaderResView().Get() ) {
                    return;
                }

                PfxRenderer->RenderDepthOfField(
                    backbufferResource->GetShaderResView().Get(),
                    waterMaskTexture->GetShaderResView().Get(),
                    specularTexture->GetShaderResView().Get() );
            };
        } );
    }

    graph.AddPass( RG_PASS_NAME("Draw ParticlesSimple"), [&]( RGBuilder& builder, RenderPass& pass ) {
        auto size = GetResolution();

        auto particleColorHandle = builder.CreateTexture( { static_cast<uint32_t>(size.x), static_cast<uint32_t>(size.y), DXGI_FORMAT_ENGINE_DEFAULT, L"PfxColor" } );
        auto particleDistortionHandle = builder.CreateTexture({ static_cast<uint32_t>(size.x), static_cast<uint32_t>(size.y), DXGI_FORMAT_R8G8B8A8_SNORM, L"PfxDistortion" });

        builder.Write( particleColorHandle );
        builder.Write( particleDistortionHandle );
        builder.Read( particleColorHandle );
        builder.Read( particleDistortionHandle );
        builder.Write( backBufferHandle );
        if ( fsr3ActiveForReactiveMask ) {
            builder.Read( reactiveMaskResource );
            builder.Write( reactiveMaskResource );
        }

        pass.m_executeCallback = [particleColorHandle, particleDistortionHandle, fsr3ActiveForReactiveMask, reactiveMaskResource](const RenderGraph& graph) {
            TracyD3D11ZoneCGX( "D3D11GraphicsEngine::Draw ParticlesSimple" );

            Engine::GAPI->ResetRenderStates();
            Engine::GAPI->DrawParticlesSimple(
                graph.GetPhysicalTexture( particleColorHandle ),
                graph.GetPhysicalTexture( particleDistortionHandle ),
                fsr3ActiveForReactiveMask ? graph.GetPhysicalTexture( reactiveMaskResource ) : nullptr );
        };
    });

#if (defined BUILD_GOTHIC_2_6_fix || defined BUILD_GOTHIC_1_08k)

    graph.AddPass( RG_PASS_NAME("Draw PolyStrips"), [&]( RGBuilder& builder, RenderPass& pass ) {
        builder.Write( backBufferHandle );

        pass.m_executeCallback = [this](const RenderGraph&) {
            TracyD3D11ZoneCGX( "D3D11GraphicsEngine::Draw PolyStrips" );

            // Calc weapon/effect trail mesh data
            Engine::GAPI->CalcPolyStripMeshes();
            // Calc lightning flashes mesh data
            Engine::GAPI->CalcFlashMeshes();
            // Draw those
            // For some reasons the viewport gets messed up, so set it again
            SetViewport( ViewportInfo( 0, 0, GetResolution() ) );
            DrawPolyStrips();
        };
    } );

#endif

    // Window glass is the final scene-space transparency layer. At this point
    // sky, world/VOB geometry, every decal class, water, rain, ghosts, particles
    // and poly strips are already present, so none of them can bypass the pane's
    // authored alpha. Keep HDR/AA after it so the glass still participates in
    // the normal final image processing.
    graph.AddPass( RG_PASS_NAME("Draw Window Glass"), [&]( RGBuilder& builder, RenderPass& pass ) {
        builder.Write( backBufferHandle );

        pass.m_executeCallback = [this](const RenderGraph&)-> void {
            TracyD3D11ZoneCGX( "D3D11GraphicsEngine::Draw Window Glass" );
            DrawWindowAlphaMeshes();
        };
   });

    // Draw debug lines
    graph.AddPass( RG_PASS_NAME("Draw Debug Lines"), [&]( RGBuilder& builder, RenderPass& pass ) {
        builder.Write( backBufferHandle );

        pass.m_executeCallback = [this](const RenderGraph&) {
            TracyD3D11ZoneCGX( "D3D11GraphicsEngine::Draw Debug Lines" );
            LineRenderer->Flush();
            LineRenderer->FlushScreenSpace();
        };
    } );

    // Draw debug lines
    graph.AddPass( RG_PASS_NAME("PostFX Viewport"), [&]( RGBuilder& builder, RenderPass& pass ) {
        builder.Write( backBufferHandle );

        pass.m_executeCallback = [this](const RenderGraph&) {
            // Set viewport for gothics rendering
            SetViewport( ViewportInfo( 0, 0, GetResolution() ) );
        };
    } );

    if ( rendererState.RendererSettings.EnableHDR ) {
        graph.AddPass( RG_PASS_NAME("Render HDR"), [&]( RGBuilder& builder, RenderPass& pass ) {
            builder.Read( backBufferHandle );
            builder.Write( backBufferHandle );

            pass.m_executeCallback = [this, backBufferHandle](const RenderGraph& graph) {
                TracyD3D11ZoneCGX( "D3D11GraphicsEngine::Render HDR" );
                auto backbufferTex = graph.GetPhysicalTexture( backBufferHandle );
                PfxRenderer->RenderHDR( backbufferTex->GetRenderTargetView().Get(), backbufferTex->GetShaderResView().Get() );
            };
        } );
    }

    if ( rendererState.RendererSettings.AntiAliasingMode
        == GothicRendererSettings::AA_SMAA ) {
        // SMAA should be applied before any sharpening
        graph.AddPass( RG_PASS_NAME("Render SMAA"), [&]( RGBuilder& builder, RenderPass& pass ) {
            builder.Read( backBufferHandle );
            builder.Write( backBufferHandle );

            pass.m_executeCallback = [this, backBufferHandle](const RenderGraph& graph) {
                TracyD3D11ZoneCGX( "D3D11GraphicsEngine::Render SMAA" );
                auto backbufferTex = graph.GetPhysicalTexture( backBufferHandle );
                PfxRenderer->RenderSMAA(backbufferTex->GetShaderResView().Get());
                GetContext()->PSSetSamplers( 0, 1, DefaultSamplerState.GetAddressOf() );
            };
        } );
    }


    graph.AddPass( RG_PASS_NAME("Reset Viewport"), [&]( RGBuilder& builder, RenderPass& pass ) {
        builder.Write( backBufferHandle );

        pass.m_executeCallback = [this](const RenderGraph&) {
            GetContext()->PSSetSamplers( 0, 1, DefaultSamplerState.GetAddressOf() );

            PresentPending = true;
        };
    } );

    // If we currently are underwater, then draw underwater effects
    if ( Engine::GAPI->IsUnderWater() ) {
        graph.AddPass( RG_PASS_NAME("Draw UnderwaterFX"), [&]( RGBuilder& builder, RenderPass& pass ) {
            builder.Write( backBufferHandle );

            pass.m_executeCallback = [this](const RenderGraph&) {
                TracyD3D11ZoneCGX( "D3D11GraphicsEngine::Draw UnderwaterFX" );
                DrawUnderwaterEffects();
            };
        } );
    }

    graph.AddPass( RG_PASS_NAME("Prepare finalize frame"), [&]( RGBuilder& builder, RenderPass& pass ) {
        builder.Write( backBufferHandle );

        pass.m_executeCallback = [this](const RenderGraph&) {
            // Clear here to get a working depthbuffer but no interferences with world
            // geometry for gothic UI-Rendering
            GetContext()->OMSetRenderTargets( 1, HDRBackBuffer->GetRenderTargetView().GetAddressOf(), nullptr );

            // Store the current depth state to the copy buffer before clear
            CopyDepthStencil();
        };
    } );

    const bool isUpscaling = u.AddUpscalingPass( graph,
        *this,
        Backbuffer->GetRenderTargetView().Get(),
        backBufferHandle,
        DepthStencilBufferCopy->GetShaderResView().Get(),
        velocityBufferHandle,
        reactiveMaskResource,
        transparencyAndCompositionMaskResource );

    // Before returning to gothics UI, set render target to backbuffer
    {
        // Copy HDR scene to backbuffer
        if ( isUpscaling ) {
            // do don't sharpen, scale or blit. Upscalers do the work themselves.
        } else if (rendererState.RendererSettings.SharpeningMode
                && rendererState.RendererSettings.SharpenFactor > 0.0f ) {

            graph.AddPass( RG_PASS_NAME("Sharpen"), [&]( RGBuilder& builder, RenderPass& pass ) {
                builder.Read( backBufferHandle );
                builder.Write( backBufferHandle );

                pass.m_executeCallback = [this, &rendererState, backBufferHandle](const RenderGraph& graph) {
                    TracyD3D11ZoneCGX( "D3D11GraphicsEngine::Sharpen" );
                    GetContext()->PSSetSamplers( 0, 1, LinearSamplerState.GetAddressOf() );

                    auto backbufferTex = graph.GetPhysicalTexture( backBufferHandle );

                    switch ( rendererState.RendererSettings.SharpeningMode ) {
                    case GothicRendererSettings::SHARPEN_SIMPLE:
                        {
                            // The simple path writes the native backbuffer.
                            auto _ = RecordGraphicsEvent( GE_NAME( "ApplySimpleSharpen" ) );
                            PfxRenderer->RenderSimpleSharpen( backbufferTex->GetShaderResView(), GetBackbufferResolution(), Backbuffer.get(), GetBackbufferResolution() );
                        }
                        break;

                    case GothicRendererSettings::SHARPEN_CAS:
                        {
                            // CAS operates in place; populate it first.
                            auto _ = RecordGraphicsEvent( GE_NAME( "Copy into native-size backbuffer" ) );
                            PfxRenderer->CopyTextureToRTV( backbufferTex->GetShaderResView(), Backbuffer->GetRenderTargetView(), GetBackbufferResolution() );
                        }
                        if ( !FeatureLevel10Compatibility ) {
                            auto _ = RecordGraphicsEvent( GE_NAME( "ApplyCAS" ) );
                            PfxRenderer->RenderCAS( Backbuffer->GetShaderResView(), GetBackbufferResolution(), Backbuffer->GetRenderTargetView(), GetBackbufferResolution(), *GetPfxRenderer()->GetBackbufferTempBuffer());
                        }
                        break;
                    }
                };
            } );
        } else {
            graph.AddPass( RG_PASS_NAME("Copy into native-size backbuffer"), [&]( RGBuilder& builder, RenderPass& pass ) {
                builder.Read( backBufferHandle );
                builder.Write( backBufferHandle );

                pass.m_executeCallback = [this, backBufferHandle](const RenderGraph& graph) {
                    auto backbufferTex = graph.GetPhysicalTexture( backBufferHandle );
                    PfxRenderer->CopyTextureToRTV( backbufferTex->GetShaderResView(), Backbuffer->GetRenderTargetView(), GetBackbufferResolution() );
                };
            } );
        }
        graph.Compile();
        graph.Execute();

        // Thin rain streaks can disappear before temporal upscaling. Under
        // FSR3, rasterize them once at output resolution after upscaling and
        // reject occluded pixels against the copied internal scene depth.
        if ( renderRainAfterUpscaling ) {
            SetViewport( ViewportInfo( 0, 0, GetBackbufferResolution() ) );
            GetContext()->OMSetRenderTargets( 1, Backbuffer->GetRenderTargetView().GetAddressOf(), nullptr );
            DepthStencilBufferCopy->BindToPixelShader( GetContext().Get(), 1 );
            ID3D11ShaderResourceView* rainExclusionSRV = nullptr;
            if ( renderRainExclusionMask ) {
                auto* rainExclusionMask = graph.GetPhysicalTexture( waterMaskResource );
                rainExclusionSRV = rainExclusionMask ? rainExclusionMask->GetShaderResView().Get() : nullptr;
                GetContext()->PSSetShaderResources( 2, 1, &rainExclusionSRV );
            }
            if ( FeatureLevel10Compatibility || rendererState.RendererSettings.DrawRainThroughTransformFeedback ) {
                Effects->DrawRain( true, rainExclusionSRV != nullptr );
            } else {
                Effects->DrawRain_CS( true, rainExclusionSRV != nullptr );
            }
            ID3D11ShaderResourceView* nullRainInputs[2] = {};
            GetContext()->PSSetShaderResources( 1, 2, nullRainInputs );
        }

        GetContext()->ClearDepthStencilView( DepthStencilBuffer->GetDepthStencilView().Get(), D3D11_CLEAR_DEPTH, 0, 0 );
        GetContext()->ClearDepthStencilView( m_SwapchainDepthStencilBuffer->GetDepthStencilView().Get(), D3D11_CLEAR_DEPTH, 0, 0 );
        SetDefaultStates();

        // Temporal AA/FSR3 jitter belongs to world rendering only. Gothic's HUD
        // and 3D inventory are drawn after upscaling at output resolution and
        // must use an unjittered projection to avoid subpixel shimmer.
        // Restore the exact projection supplied by Gothic. Temporal jitter
        // belongs to world rendering only.
        rendererState.TransformState.TransformProj = uiProjection;
        rendererState.TransformState.TransformProjUnjittered = uiProjection;

        // Below this, we assume UI/HUD rendering
        rendererState.RendererInfo.RenderStage = STAGE_DRAW_HUD;

        SetViewport( ViewportInfo( 0, 0, GetBackbufferResolution() ) );
        UpdateZEngineViewport();

        GetContext()->OMSetRenderTargets( 1, Backbuffer->GetRenderTargetView().GetAddressOf(), nullptr );
    }

    // UI sprites use counter-clockwise winding.
    SetDefaultStates();
    rendererState.RasterizerState.CullMode = GothicRasterizerStateInfo::CM_CULL_NONE;
    rendererState.RasterizerState.SetDirty();
    UpdateRenderStates();
    GetContext()->PSSetSamplers( 0, 1, ClampSamplerState.GetAddressOf() );

    // Save screenshot if wanted
    if ( SaveScreenshotNextFrame ) {
        SaveScreenshot();
        SaveScreenshotNextFrame = false;
    }

    // Reset Render States for HUD
    Engine::GAPI->ResetRenderStates();
    return XR_SUCCESS;
}

void D3D11GraphicsEngine::SetupVS_ExMeshDrawCall() {
    UpdateRenderStates();

    if ( ActiveVS ) {
        ActiveVS->Apply();
    }
    if ( ActivePS ) {
        ActivePS->Apply();
    }

    GetContext()->IASetPrimitiveTopology( D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST );
}

void D3D11GraphicsEngine::SetupVS_ExConstantBuffer() {
    auto& view = Engine::GAPI->GetRendererState().TransformState.TransformView;
    auto& proj = Engine::GAPI->GetProjectionMatrix();

    VS_ExConstantBuffer_PerFrame cb;
    cb.View = view;
    XMStoreFloat4x4( &cb.InvView, XMMatrixInverse( nullptr, XMLoadFloat4x4( &view ) ) );
    cb.Projection = proj;
    XMStoreFloat4x4( &cb.ViewProj, XMMatrixMultiply( XMLoadFloat4x4( &proj ), XMLoadFloat4x4( &view ) ) );
    cb.PrevViewProj = m_PrevViewProjMatrix;

    if ( TemporalState ) {
        cb.UnjitteredViewProj = TemporalState->GetUnjitteredViewProj();
    } else {
        cb.UnjitteredViewProj = cb.ViewProj;
    }

    ActiveVS->GetBuffer(0).Update(&cb).Bind();
}

void D3D11GraphicsEngine::SetupVS_ExPerInstanceConstantBuffer() {
    auto world = Engine::GAPI->GetRendererState().TransformState.TransformWorld;

    VS_ExConstantBuffer_PerInstance cb = {};
    cb.World = world;
    cb.Color = float4( 1.0f, 1.0f, 1.0f, 1.0f );

    ActiveVS->GetBuffer(1).Update(&cb).Bind();
}

bool SectionRenderlistSortCmp( std::pair<float, WorldMeshSectionInfo*>& a,
    std::pair<float, WorldMeshSectionInfo*>& b ) {
    return a.first < b.first;
}

// Sets the color space for the swap chain in order to handle HDR output.
void D3D11GraphicsEngine::UpdateColorSpace_SwapChain()
{
    Microsoft::WRL::ComPtr<IDXGISwapChain3> SwapChain3;
    if ( FAILED( SwapChain.As( &SwapChain3 ) ) ) {
        return;
    }

    bool isDisplayHDR10 = false;
    DXGI_COLOR_SPACE_TYPE colorSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    if ( m_HDR ) {
        Microsoft::WRL::ComPtr<IDXGIOutput> output;
        if ( SUCCEEDED( SwapChain3->GetContainingOutput( output.GetAddressOf() ) ) ) {
            Microsoft::WRL::ComPtr<IDXGIOutput6> output6;
            if ( SUCCEEDED( output.As( &output6 ) ) ) {
                DXGI_OUTPUT_DESC1 desc;
                output6->GetDesc1( &desc );
                if ( desc.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 ) {
                    // Display output is HDR10.
                    isDisplayHDR10 = true;
                }
            }
        }
    }

    if ( isDisplayHDR10 ) {
        switch ( GetBackBufferFormat() ) {
        case DXGI_FORMAT_R11G11B10_FLOAT: //origial DXGI_FORMAT_R10G10B10A2_UNORM
            // The application creates the HDR10 signal.
            colorSpace = DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
            break;

        case DXGI_FORMAT_R16G16B16A16_FLOAT:
            // The system creates the HDR10 signal; application uses linear values.
            colorSpace = DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709;
            break;

        default:
            break;
        }
    }

    UINT colorSpaceSupport = 0;
    if ( SUCCEEDED( SwapChain3->CheckColorSpaceSupport( colorSpace, &colorSpaceSupport ) )
        && (colorSpaceSupport & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT) ) {
        SwapChain3->SetColorSpace1( colorSpace );
        LogInfo() << "Using HDR Monitor ColorSpace";
    }
}

/** Draws a list of mesh infos */
XRESULT D3D11GraphicsEngine::DrawMeshInfoListAlphablended( const std::vector<std::pair<MeshKey, MeshInfo*>>& list ) {
    if ( list.empty() ) {
        return XR_SUCCESS;
    }

    XMMATRIX view = Engine::GAPI->GetViewMatrixXM();
    Engine::GAPI->SetViewTransformXM( view );
    Engine::GAPI->ResetWorldTransform();

    SetActivePixelShader( PShaderID::PS_Diffuse );
    SetActiveVertexShader( VShaderID::VS_Ex );

    SetupVS_ExMeshDrawCall();
    SetupVS_ExConstantBuffer();

    // Set constant buffer
    ActivePS->GetBuffer( "FFPipelineConstantBuffer" )
        .Update( &Engine::GAPI->GetRendererState().GraphicsState )
        .Bind();

    GSky* sky = Engine::GAPI->GetSky();
    ActivePS->GetBuffer( "Atmosphere" )
        .Update( &sky->GetAtmosphereCB() )
        .Bind();

    const XMMATRIX identityMatrix = XMMatrixIdentity();
    VS_ExConstantBuffer_PerInstance cbInstance = {};
    XMStoreFloat4x4( &cbInstance.World, identityMatrix );
    cbInstance.Color = float4( 1.0f, 1.0f, 1.0f, 1.0f );
    ActiveVS->GetBuffer( "Matrices_PerInstances" ).Update( &cbInstance, sizeof( cbInstance ) ).Bind();

    InfiniteRangeConstantBuffer->BindToPixelShader( 3 );

    // Bind wrapped mesh vertex buffers
    DrawVertexBufferIndexedUINT(
        Engine::GAPI->GetWrappedWorldMesh()->MeshVertexBuffer,
        Engine::GAPI->GetWrappedWorldMesh()->MeshIndexBuffer, 0, 0 );

    int lastAlphaFunc = 0;

    // Draw the list
    void* lastTex = nullptr;
    void* lastMat = nullptr;
    MaterialInfo* lastInfo = nullptr;
    for ( auto const& [meshKey, meshInfo] : list ) {
            zCTexture* texture = meshKey.Material->GetAniTexture();
            if ( !texture ) {
                texture = meshKey.Material->GetTexture();
            }
        if ( texture ) {
            PsSimpleFFdata ffdata = { };
            ffdata.textureFactor = ComputeTransparencyTextureFactor( meshKey.Material );

            float transparentWorldMeshDaylightFactor = 1.0f;
            float transparentWorldMeshRainFactor = 0.0f;
            if ( Engine::GAPI ) {
                if ( GSky* sky = Engine::GAPI->GetSky() ) {
                    const auto& atmosphere = sky->GetAtmosphereCB();
                    const float sunHeight = atmosphere.AC_LightPos.y;
                    const float linearDaylightFactor = std::clamp( (sunHeight + 0.08f) / 0.20f, 0.0f, 1.0f );
                    transparentWorldMeshDaylightFactor = linearDaylightFactor * linearDaylightFactor * (3.0f - 2.0f * linearDaylightFactor);
                    transparentWorldMeshRainFactor = std::clamp( atmosphere.AC_SceneWettness, 0.0f, 1.0f );
                }
            }

            const float transparentWorldMeshDayBrightnessMax = std::lerp( 1.00f, 0.50f, transparentWorldMeshRainFactor );
                const float transparentWorldMeshBrightness = std::lerp( 0.10f, transparentWorldMeshDayBrightnessMax, transparentWorldMeshDaylightFactor );
                const float transparentWorldMeshAlpha = std::lerp( 0.50f, 1.00f, transparentWorldMeshDaylightFactor );
                ffdata.textureFactor.x *= transparentWorldMeshBrightness;
                ffdata.textureFactor.y *= transparentWorldMeshBrightness;
                ffdata.textureFactor.z *= transparentWorldMeshBrightness;
                ffdata.textureFactor.w *= transparentWorldMeshAlpha;
            if (texture->CacheIn( 0.6f ) != zRES_CACHED_IN) {
                // Draw what? black? :)
                continue;
            }

            MyDirectDrawSurface7* surface = texture->GetSurface();
            ID3D11ShaderResourceView* srv[4];
            // Get diffuse and normalmap
            srv[0] = surface->GetEngineTexture() ->GetShaderResourceView().Get();
                srv[1] = GetMaterialNormalmapSRV( surface, meshKey.Info );
                srv[2] = surface->GetFxMap() ? surface->GetFxMap()->GetShaderResourceView().Get() : nullptr;
                srv[3] = GetParallaxDisplacementSRV( surface, meshKey.Info );
                int alphaFunc = meshKey.Material->GetAlphaFunc();

            if ( alphaFunc == 0 ) {
                alphaFunc = zColor( meshKey.Material->GetColor() ).bgra.alpha < 255
                    ? zMAT_ALPHA_FUNC_BLEND
                    : zMAT_ALPHA_FUNC_MAT_DEFAULT;
            }

            if (lastTex != texture) {
                GetContext()->PSSetShaderResources( 0, 3, srv );
                GetContext()->PSSetShaderResources( 13, 1, &srv[3] );
                lastTex = texture;
            }

            if (lastMat != meshKey.Material) {
                const MaterialInfo::EMaterialType shaderMaterialType = meshKey.Info->MaterialType;
                BindShaderForTexture( texture, false, alphaFunc, shaderMaterialType, true );
                if ( alphaFunc == zMAT_ALPHA_FUNC_BLEND || alphaFunc == zMAT_ALPHA_FUNC_ADD ) {
                    SetActivePixelShader( PShaderID::PS_Simple_FF );
                    ActivePS->Apply();
                }
                lastMat = meshKey.Material;
            }

            // Check for alphablending on world mesh
            if ( lastAlphaFunc != alphaFunc ) {
                switch ( alphaFunc ) {
                case zMAT_ALPHA_FUNC_BLEND:
                case zMAT_ALPHA_FUNC_BLEND_TEST:
                    Engine::GAPI->GetRendererState().BlendState.SetAlphaBlending();
                    break;

                case zMAT_ALPHA_FUNC_ADD:
                    Engine::GAPI->GetRendererState().BlendState.SetAdditiveBlending();
                    break;

                case zMAT_ALPHA_FUNC_MUL:
                    Engine::GAPI->GetRendererState().BlendState.SetModulateBlending();
                    break;

                case zMAT_ALPHA_FUNC_MUL2:
                    Engine::GAPI->GetRendererState().BlendState.SetModulate2Blending();
                    break;

                default:
                    auto _wtf = 1;
                    break;
                    // continue;
                }

                Engine::GAPI->GetRendererState().BlendState.SetDirty();

                Engine::GAPI->GetRendererState().DepthState.DepthWriteEnabled = false;
                Engine::GAPI->GetRendererState().DepthState.SetDirty();

                UpdateRenderStates();
                lastAlphaFunc = alphaFunc;
            }

            ActivePS->GetBuffer( "cbFFData" )
                .Update( &ffdata )
                .Bind();

            // TODO: Do we even need/use material-info for transparent meshes?
            /*MaterialInfo* info = meshKey.Info;
            if (info != lastInfo) {
                if (!lastInfo || !lastInfo->IsSame(info)) {
                    lastInfo = info;
                }
            }*/

            // Draw the section-part
            DrawVertexBufferIndexedUINT( nullptr, nullptr, meshInfo->Indices.size(),
                meshInfo->BaseIndexLocation );

        }
    }

    Engine::GAPI->GetRendererState().DepthState.DepthWriteEnabled = true;
    Engine::GAPI->GetRendererState().DepthState.SetDirty();
    Engine::GAPI->GetRendererState().BlendState.ColorWritesEnabled = false;
    Engine::GAPI->GetRendererState().BlendState.SetDirty();
    UpdateRenderStates();
    // Draw again, but only to depthbuffer this time to make them work with
    // fogging
    for ( auto const& [meshKey, meshInfo] : list ) {
        if ( meshKey.Material->GetAniTexture() != nullptr && meshKey.Info->MaterialType != MaterialInfo::MT_Portal ) {
            // Draw the section-part
            DrawVertexBufferIndexedUINT( nullptr, nullptr, meshInfo->Indices.size(),
                meshInfo->BaseIndexLocation );
        }
    }
    Engine::GAPI->GetRendererState().BlendState.ColorWritesEnabled = true;
    Engine::GAPI->GetRendererState().BlendState.SetDirty();
    UpdateRenderStates();

    return XR_SUCCESS;
}


XRESULT D3D11GraphicsEngine::DrawWorldMesh( bool noTextures ) {
    if ( !Engine::GAPI->GetRendererState().RendererSettings.DrawWorldMesh )
        return XR_SUCCESS;

    ZoneScopedN( "DrawWorldMesh" );
    auto _scopeDrawWorldMesh = RecordGraphicsEvent( GE_NAME( "DrawWorldMesh" ) );

    const bool isZPrepass = RenderingStage == DES_Z_PRE_PASS;
    if ( isZPrepass ) {
        noTextures = true;
    }

    // Resolve visible window cutouts before drawing the world.
    SetDefaultStates();
    XMMATRIX view = Engine::GAPI->GetViewMatrixXM();
    Engine::GAPI->SetViewTransformXM( view );
    Engine::GAPI->ResetWorldTransform();
    const unsigned int windowCutoutCount = UpdateAndBindWindowCutouts();

    SetActivePixelShader( PShaderID::PS_Diffuse );
    SetActiveVertexShader( VShaderID::VS_Ex );

    SetupVS_ExMeshDrawCall();
    SetupVS_ExConstantBuffer();

    // Bind reflection-cube to slot 4
    GetContext()->PSSetShaderResources( 4, 1, ReflectionCube.GetAddressOf() );

    // Set constant buffer
    const XMMATRIX identityMatrix = XMMatrixIdentity();
    VS_ExConstantBuffer_PerInstance cbInstance = {};
    XMStoreFloat4x4( &cbInstance.World, identityMatrix );
    cbInstance.Color = float4( 1.0f, 1.0f, 1.0f, 1.0f );
    ActiveVS->GetBuffer( "Matrices_PerInstances" )
        .Update( &cbInstance, sizeof( cbInstance ) )
        .Bind();

    auto updatePSBuffers = [this] {
        ActivePS->GetBuffer( "FFPipelineConstantBuffer" )
            .Update( &Engine::GAPI->GetRendererState().GraphicsState )
            .Bind();

        GSky* sky = Engine::GAPI->GetSky();
        ActivePS->GetBuffer( "Atmosphere" )
            .Update( &sky->GetAtmosphereCB() )
            .Bind();

        ActivePS->BindBuffer( "DIST_Distance", InfiniteRangeConstantBuffer.get() );

        PsSimpleFFdata ffdata = { };
        ffdata.textureFactor = float4( 1.0f, 1.0f, 1.0f, 1.0f );
        ActivePS->GetBuffer( "cbFFData" )
            .Update( &ffdata )
            .Bind();
    };
    updatePSBuffers();
    static std::vector<WorldMeshSectionInfo*> renderList;
    if ( !m_FrameGeometryCache.worldMeshBuilt ) {
        Engine::GAPI->CollectVisibleSections( m_FrameGeometryCache.visibleSections, nullptr, true );
        m_FrameGeometryCache.worldMeshBuilt = true;
    }
    renderList = m_FrameGeometryCache.visibleSections; // shallow copy of pointers, O(N_sections), not O(BSP)

    MeshInfo* meshInfo = Engine::GAPI->GetWrappedWorldMesh();
    DrawVertexBufferIndexedUINT( meshInfo->MeshVertexBuffer, meshInfo->MeshIndexBuffer, 0, 0 );

    struct WorldMeshKey {
        zCTexture* Texture;
        zCMaterial* Material;
        MaterialInfo* Info;
        int AlphaLevel; // 0 = opaque, 1 = alpha test, 2 = alpha texture
        float DistanceSq;
        //zCLightmap* Lightmap;
    };

    struct TransparencyWorldMeshEntry {
        std::pair<MeshKey, MeshInfo*> Mesh;
        float DistanceSq;
    };

    static std::vector<std::pair<WorldMeshKey, MeshInfo*>> meshList;
    meshList.clear();
    if ( meshList.capacity() == 0 ) meshList.reserve( 4096 );

    std::vector<TransparencyWorldMeshEntry> transparencyMeshes;
    std::vector<TransparencyWorldMeshEntry> portalTransparencyMeshes;

    GetContext()->IASetPrimitiveTopology( D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST );

    {
        ZoneScopedN( "DrawWorldMesh::BuildMeshList" );
        auto _scopeBuildMeshList = RecordGraphicsEvent( GE_NAME( "DrawWorldMesh::BuildMeshList" ) );
        const XMVECTOR cameraPosition = Engine::GAPI->GetCameraPositionXM();

        static std::vector<WorldMeshSectionInfo*> alphaBlendedThings;
        alphaBlendedThings.clear();
        alphaBlendedThings.reserve( 200 );

        for ( auto const& renderItem : renderList ) {
            for ( auto const& worldMesh : renderItem->WorldMeshes ) {
                if ( worldMesh.first.Material ) {
                    zCTexture* aniTex = worldMesh.first.Material->GetAniTexture();

                    if ( !aniTex ) {
                        aniTex = worldMesh.first.Material->GetTexture();
                    }
                    if ( !aniTex ) continue;

                    const float distanceSq = ComputeWorldMeshDistanceSqFromCamera( renderItem, worldMesh.second, cameraPosition );
                    const std::pair<MeshKey, MeshInfo*> transparencyMesh = { worldMesh.first, worldMesh.second };

                    // All legacy water, including steep waterfall geometry,
                    // remains in one batch and is classified in PS_Water.

                    if ( worldMesh.first.Info->MaterialType == MaterialInfo::MT_Water ) {
                        if ( !isZPrepass ) {
                            FrameWaterSurfaces[aniTex].push_back( worldMesh.second );
                        }
                        continue;
                    }

                    if ( aniTex->CacheIn( 0.6f ) != zRES_CACHED_IN ) {
                        continue;
                    }

                    if ( worldMesh.first.Info->MaterialType == MaterialInfo::MT_Portal ) {
                        if ( !isZPrepass ) {
                            portalTransparencyMeshes.push_back( { transparencyMesh, distanceSq } );
                        }
                        continue;
                    }
                    // Check for alphablending
                    if ( (worldMesh.first.Material->GetAlphaFunc() > zMAT_ALPHA_FUNC_NONE &&
                        worldMesh.first.Material->GetAlphaFunc() != zMAT_ALPHA_FUNC_TEST)
                        // || (worldMesh.first.Material->GetEnvMapEnabled())
                        ) {
                        if ( !isZPrepass ) {
                            transparencyMeshes.push_back( { transparencyMesh, distanceSq } );
                        }
                        continue;
                    } else {

                        int alphaLevel = 0;
                        if ( worldMesh.first.Texture && worldMesh.first.Texture->HasAlphaChannel() ) {
                            alphaLevel = 2;
                        } else if ( worldMesh.first.Material && worldMesh.first.Material->HasAlphaTest() ) {
                            alphaLevel = 1;
                        }

                        WorldMeshKey key = {
                            aniTex,
                            worldMesh.first.Material,
                            worldMesh.first.Info,
                            alphaLevel,
                            distanceSq,
                        };

                        // Create a new pair using the animated texture
                        meshList.emplace_back( key, worldMesh.second );
                    }
                }
            }
        }

        auto sortAndAppendTransparencyMeshes = []( std::vector<TransparencyWorldMeshEntry>& source,
            std::vector<std::pair<MeshKey, MeshInfo*>>& destination ) {
            if ( source.empty() ) {
                return;
            }

            std::sort( source.begin(), source.end(),
                []( const TransparencyWorldMeshEntry& a, const TransparencyWorldMeshEntry& b ) {
                    if ( a.DistanceSq > b.DistanceSq )
                        return true;
                    if ( a.DistanceSq < b.DistanceSq )
                        return false;
                    if ( a.Mesh.first.Material != b.Mesh.first.Material )
                        return a.Mesh.first.Material < b.Mesh.first.Material;
                    if ( a.Mesh.first.Texture != b.Mesh.first.Texture )
                        return a.Mesh.first.Texture < b.Mesh.first.Texture;
                    if ( a.Mesh.first.Info != b.Mesh.first.Info )
                        return a.Mesh.first.Info < b.Mesh.first.Info;

                    const unsigned int aBaseIndex = a.Mesh.second ? a.Mesh.second->BaseIndexLocation : 0u;
                    const unsigned int bBaseIndex = b.Mesh.second ? b.Mesh.second->BaseIndexLocation : 0u;
                    return aBaseIndex < bBaseIndex;
                } );

            destination.reserve( destination.size() + source.size() );
            for ( auto& entry : source ) {
                destination.emplace_back( std::move( entry.Mesh ) );
            }
        };

        sortAndAppendTransparencyMeshes( transparencyMeshes, FrameTransparencyMeshes );
        sortAndAppendTransparencyMeshes( portalTransparencyMeshes, FrameTransparencyMeshesPortal );
    }
    auto CompareMesh = []( std::pair<WorldMeshKey, MeshInfo*>& a, std::pair<WorldMeshKey, MeshInfo*>& b ) -> bool {
        if ( a.first.AlphaLevel != b.first.AlphaLevel )
            return a.first.AlphaLevel < b.first.AlphaLevel;
        if ( a.first.DistanceSq < b.first.DistanceSq )
            return true;
        if ( a.first.DistanceSq > b.first.DistanceSq )
            return false;
        if ( a.first.Texture != b.first.Texture )
            return a.first.Texture < b.first.Texture;
        return a.second->BaseIndexLocation < b.second->BaseIndexLocation;
    };
    std::sort( meshList.begin(), meshList.end(), CompareMesh );

    // Draw depth only
    const auto& rendererSettings = Engine::GAPI->GetRendererState().RendererSettings;
    if ( (rendererSettings.GetEffectiveDoZPrepass()
            && rendererSettings.RendererMode == GothicRendererSettings::RM_Deferred )
        || isZPrepass) {
        ZoneScopedN( "DrawWorldMesh::DepthPrepass" );
        auto _scopeDepthPrepass = RecordGraphicsEvent( GE_NAME( "DrawWorldMesh::DepthPrepass" ) );
        if ( windowCutoutCount > 0 ) {
            SetActivePixelShader( PShaderID::PS_WindowCutoutDepth );
            ActivePS->Apply();
            updatePSBuffers();
        } else {
            GetContext()->PSSetShader( nullptr, nullptr, 0 );
        }

        for ( auto const& mesh : meshList ) {
            zCTexture* texture;
            if ( ( texture = mesh.first.Texture ) == nullptr ) continue;
            const auto alphaFunc = mesh.first.Material->GetAlphaFunc();
            const auto isBlend = alphaFunc > zRND_ALPHA_FUNC_NONE && alphaFunc != zRND_ALPHA_FUNC_TEST;
            if (isBlend || zColor( mesh.first.Material->GetColor() ).bgra.alpha < 255) {
                // Skip blended meshes in z-prepass, they will be rendered in main pass
                continue;
            }

            if ( texture->HasAlphaChannel() || (mesh.first.Material && mesh.first.Material->HasAlphaTest()) ) {
                if ( texture->CacheIn( 0.6f ) != zRES_CACHED_IN ) {
                    continue;
                }

                texture->GetSurface()->GetEngineTexture()->BindToPixelShader( 0 );

                // Get the right shader for it
                if ( BindShaderForTexture( mesh.first.Texture, false,
                    zMAT_ALPHA_FUNC_MAT_DEFAULT ) ) { // default alpha stuff, we defer blend/add
                    // shader changed? update buffers.
                    updatePSBuffers();
                }
            }

            if ( mesh.first.Info->MaterialType == MaterialInfo::MT_Water )
                continue;  // Don't pre-render water

            DrawVertexBufferIndexedUINT( nullptr, nullptr, mesh.second->Indices.size(), mesh.second->BaseIndexLocation );
        }
        if ( isZPrepass ) {
            UnbindWindowCutouts();
            return XR_SUCCESS;
        }
    }

    SetActivePixelShader( PShaderID::PS_Diffuse );
    ActivePS->Apply();

    MaterialInfo defInfo = {};
    auto materialInfoBuffer = ActivePS->GetBuffer( "MI_MaterialInfo" )
        .Update( &defInfo.buffer, sizeof(defInfo.buffer) )
        .Bind();

    // Now draw the actual pixels
    zCTexture* bound = nullptr;

    if ( !meshList.empty() ) {
        ZoneScopedN( "DrawWorldMesh::OpaqueSubmission" );
        auto _scopeOpaqueSubmission = RecordGraphicsEvent( GE_NAME( "DrawWorldMesh::OpaqueSubmission" ) );

        const size_t numMeshes = meshList.size();
        std::vector<UINT> materialInfoCbOffsets( numMeshes );

        ConstantBufferAllocation INVALID_MATERIAL = PerObjectMaterialInfoPooledBuffer->Allocate( GetContext().Get(), &defInfo.buffer, sizeof( defInfo.buffer ) );
        ConstantBufferAllocation lastMatCbAllocation = INVALID_MATERIAL;
        MaterialInfo* lastInfo = nullptr;


        for ( size_t i = 0; i < numMeshes; i++ ) {
            auto const& mesh = meshList[i];

            if ( mesh.first.Texture != bound &&
                Engine::GAPI->GetRendererState().RendererSettings.DrawWorldMesh > 1 ) {
                MyDirectDrawSurface7* surface = mesh.first.Texture->GetSurface();
                ID3D11ShaderResourceView* srv[4];
                MaterialInfo* info = mesh.first.Info;

                // Get diffuse and normalmap
                srv[0] = surface->GetEngineTexture()->GetShaderResourceView().Get();
                srv[1] = GetMaterialNormalmapSRV( surface, info );
                srv[2] = surface->GetFxMap()
                    ? surface->GetFxMap()->GetShaderResourceView().Get()
                    : nullptr;
                srv[3] = GetParallaxDisplacementSRV( surface, info );

                // Bind diffuse/normal/fx like 026; POM displacement uses t13.
                GetContext()->PSSetShaderResources( 0, 3, srv );
                GetContext()->PSSetShaderResources( 13, 1, &srv[3] );

                // Get the right shader for it.
                if ( BindShaderForTexture( mesh.first.Texture, false,
                    zMAT_ALPHA_FUNC_MAT_DEFAULT, MaterialInfo::MT_None, true ) ) {
                    updatePSBuffers();
                }


                auto materialInfoBufferAllocation = lastMatCbAllocation;
                if ( info ) {
                    if ( info->IsSame( lastInfo ) ) {
                        materialInfoBufferAllocation = lastMatCbAllocation;
                    } else {
                        auto materialBuffer = GetEffectiveMaterialBuffer( info, surface );
                        materialInfoBufferAllocation = PerObjectMaterialInfoPooledBuffer->Allocate( GetContext().Get(), &materialBuffer, sizeof( materialBuffer ) );
                    }
                }
                lastInfo = info;

                UINT firstConstant = materialInfoBufferAllocation.offsetInBytes / 16;
                UINT numConstants = materialInfoBufferAllocation.sizeInBytes / 16; // aligned size

                if ( lastMatCbAllocation != materialInfoBufferAllocation ) {
                    GetContext()->PSSetConstantBuffers1( materialInfoBuffer.GetSlot(), 1, &materialInfoBufferAllocation.pBuffer, &firstConstant, &numConstants );
                    lastMatCbAllocation = materialInfoBufferAllocation;
                }
                bound = mesh.first.Texture;
            }

            if ( Engine::GAPI->GetRendererState().RendererSettings.DrawWorldMesh > 2 ) {
                DrawVertexBufferIndexedUINT( nullptr, nullptr, mesh.second->Indices.size(), mesh.second->BaseIndexLocation );
            }
        }
    }

    UnbindWindowCutouts();

    return XR_SUCCESS;
}

/** Draws the given mesh infos as water */
void D3D11GraphicsEngine::DrawWaterSurfaces() {
    DrawWaterSurfaces( nullptr, nullptr, nullptr, nullptr, nullptr );
}

void D3D11GraphicsEngine::DrawWaterSurfaces(
    ID3D11RenderTargetView* waterMaskRTV,
    ID3D11RenderTargetView* fsr3ReactiveMaskRTV,
    ID3D11ShaderResourceView* lowCloudLayerSRV,
    ID3D11RenderTargetView* waterCoverageRTV,
    ID3D11ShaderResourceView* materialClassSRV ) {
    if ( FrameWaterSurfaces.empty() ) {
        return;
    }

    ZoneScopedN( "DrawWaterSurfaces" );
    auto _scopeDrawWaterSurfaces = RecordGraphicsEvent( GE_NAME( "DrawWaterSurfaces" ) );

    SetDefaultStates();

    auto tempBuffer = PfxRenderer->GetTempBuffer();

    // Copy backbuffer
    PfxRenderer->CopyTextureToRTV(
        HDRBackBuffer->GetShaderResView(),
        tempBuffer->GetRenderTargetView(),
        GetResolution() );
    CopyDepthStencil();

    XMMATRIX view = Engine::GAPI->GetViewMatrixXM();
    Engine::GAPI->SetViewTransformXM( view );  // Update view transform

    // Bind vertex water shader
    ActivePS = nullptr;
    SetActiveVertexShader( VShaderID::VS_ExWater );
    SetupVS_ExMeshDrawCall();
    SetupVS_ExConstantBuffer();

    const auto& rendererSettings = Engine::GAPI->GetRendererState().RendererSettings;
    const float totalTime = Engine::GAPI->GetTotalTime();
    WaterAnimationTimeConstantBuffer waterTimes = {};
    // M_TotalTime keeps the existing UV animation alive. Only the vertex-wave
    // clock is gated by the Advanced F11 option.
    waterTimes.M_TotalTime = totalTime;
    waterTimes.M_WaterWaveTime = rendererSettings.GetEffectiveWaterAnimation()
        ? totalTime : 0.0f;
    ActiveVS->GetBuffer( "Matrices_PerInstances" )
        .Update( &waterTimes, sizeof( waterTimes ) )
        .Bind();

    ID3D11RenderTargetView* waterTargets[3] = {
        HDRBackBuffer->GetRenderTargetView().Get(), waterMaskRTV, fsr3ReactiveMaskRTV
    };
    const UINT waterTargetCount = fsr3ReactiveMaskRTV ? 3u : ( waterMaskRTV ? 2u : 1u );
    GetContext()->OMSetRenderTargets( waterTargetCount, waterTargets,
        DepthStencilBuffer->GetDepthStencilView().Get() );

    // Bind wrapped mesh vertex buffers
    DrawVertexBufferIndexedUINT(
        Engine::GAPI->GetWrappedWorldMesh()->MeshVertexBuffer,
        Engine::GAPI->GetWrappedWorldMesh()->MeshIndexBuffer, 0, 0 );

    // Build per-texture batch descriptors and flat indirect draw args
    struct WaterTextureBatch {
        zCTexture* texture;
        unsigned int argsOffset; // index into waterDrawArgs
        unsigned int drawCount;
    };

    static std::vector<D3D11_DRAW_INDEXED_INSTANCED_INDIRECT_ARGS> waterDrawArgs;
    static std::vector<WaterTextureBatch> waterBatches;
    waterDrawArgs.clear();
    waterBatches.clear();

    {
        ZoneScopedN( "DrawWaterSurfaces::BuildBatches" );
        auto _scopeBuildBatches = RecordGraphicsEvent( GE_NAME( "DrawWaterSurfaces::BuildBatches" ) );
        for ( const auto& [texture, meshes] : FrameWaterSurfaces ) {
            WaterTextureBatch batch;
            batch.texture = texture;
            batch.argsOffset = static_cast<unsigned int>( waterDrawArgs.size() );
            batch.drawCount = 0;

            for ( const auto& mesh : meshes ) {
                D3D11_DRAW_INDEXED_INSTANCED_INDIRECT_ARGS args;
                args.IndexCountPerInstance = static_cast<UINT>( mesh->Indices.size() );
                args.InstanceCount = 1;
                args.StartIndexLocation = mesh->BaseIndexLocation;
                args.BaseVertexLocation = 0;
                args.StartInstanceLocation = 0;
                waterDrawArgs.push_back( args );
                batch.drawCount++;
            }

            waterBatches.push_back( batch );
        }

    }

    constexpr unsigned int argStride = sizeof( D3D11_DRAW_INDEXED_INSTANCED_INDIRECT_ARGS );
    unsigned int regularWaterDrawCount = 0;
    for ( const WaterTextureBatch& batch : waterBatches ) {
        regularWaterDrawCount += batch.drawCount;
    }

    if ( !FeatureLevel10Compatibility ) {
        const size_t requiredSize = waterDrawArgs.size() * argStride;
        if ( requiredSize == 0 ) {
            WaterIndirectBuffer.reset();
        } else if ( !WaterIndirectBuffer || WaterIndirectBuffer->GetSizeInBytes() < requiredSize ) {
            WaterIndirectBuffer = std::make_unique<D3D11IndirectBuffer>();
            if ( WaterIndirectBuffer->Init(
                nullptr, static_cast<unsigned int>( requiredSize ),
                D3D11IndirectBuffer::B_INDEXBUFFER, D3D11IndirectBuffer::U_DEFAULT,
                D3D11IndirectBuffer::CA_NONE ) != XR_SUCCESS
                || WaterIndirectBuffer->UpdateBufferSubresource(
                    waterDrawArgs.data(), static_cast<unsigned int>( requiredSize ) ) != XR_SUCCESS ) {
                WaterIndirectBuffer.reset();
            }
        } else {
            WaterIndirectBuffer->UpdateBufferSubresource(
                waterDrawArgs.data(), static_cast<unsigned int>( requiredSize ) );
        }
    }

    // === Z-Prepass ===
    {
        ZoneScopedN( "DrawWaterSurfaces::ZPrepass" );
        auto _scopeWaterZPrepass = RecordGraphicsEvent( GE_NAME( "DrawWaterSurfaces::ZPrepass" ) );
        const bool writeWaterCoverage = waterCoverageRTV != nullptr;
        // The coverage pass reuses the existing water Z-prepass draw. It
        // writes a binary R8 mask and still updates the water depth buffer;
        // without a coverage target we retain the old depth-only fallback.
        Engine::GAPI->GetRendererState().BlendState.ColorWritesEnabled = writeWaterCoverage;
        Engine::GAPI->GetRendererState().BlendState.SetDirty();
        Engine::GAPI->GetRendererState().DepthState.DepthWriteEnabled = true;
        Engine::GAPI->GetRendererState().DepthState.SetDirty();
        UpdateRenderStates();

        if ( writeWaterCoverage ) {
            GetContext()->OMSetRenderTargets( 1, &waterCoverageRTV,
                DepthStencilBuffer->GetDepthStencilView().Get() );
            SetActivePixelShader( PShaderID::PS_Water );
            if ( ActivePS ) {
                ActivePS->Apply();
                WaterMaterialInfoConstantBuffer coverageCB = {};
                // WM_Padding1.x is a runtime-only coverage-pass flag. The
                // pixel shader returns before reading scene resources.
                coverageCB.WM_Padding1.x = 1.0f;
                ActivePS->GetBuffer( "WaterMaterialInfo" ).Update( &coverageCB ).Bind();
            }
        } else {
            GetContext()->PSSetShader( nullptr, nullptr, 0 );
        }

        if ( regularWaterDrawCount > 0 && !FeatureLevel10Compatibility ) {
            DrawMultiIndexedInstancedIndirect( Context.Get(),
                regularWaterDrawCount,
                WaterIndirectBuffer->GetIndirectBuffer().Get(),
                0, argStride );
        } else if ( regularWaterDrawCount > 0 ) {
            // FL10 fallback: direct DrawIndexed per mesh
            for ( unsigned int i = 0; i < regularWaterDrawCount; ++i ) {
                const auto& args = waterDrawArgs[i];
                DrawVertexBufferIndexedUINT( nullptr, nullptr,
                    args.IndexCountPerInstance, args.StartIndexLocation );
            }
        }
    }

    // === Refraction Pass ===
    {
        ZoneScopedN( "DrawWaterSurfaces::Refraction" );
        auto _scopeWaterRefraction = RecordGraphicsEvent( GE_NAME( "DrawWaterSurfaces::Refraction" ) );
        // Enable color writes, disable depth writes
        Engine::GAPI->GetRendererState().BlendState.ColorWritesEnabled = true;
        Engine::GAPI->GetRendererState().BlendState.SetDirty();
        Engine::GAPI->GetRendererState().DepthState.DepthWriteEnabled = false;
        Engine::GAPI->GetRendererState().DepthState.SetDirty();
        UpdateRenderStates();
        GetContext()->OMSetRenderTargets( waterTargetCount, waterTargets,
            DepthStencilBuffer->GetDepthStencilView().Get() );

        // Bind pixel water shader
        SetActivePixelShader( PShaderID::PS_Water );
        if ( ActivePS ) {
            ActivePS->Apply();
        }

        // Bind distortion texture
        DistortionTexture->BindToPixelShader( 4 );

        // Bind copied backbuffer
        GetContext()->PSSetShaderResources(
            5, 1, tempBuffer->GetShaderResView().GetAddressOf() );

        // Bind depth to the shader
        DepthStencilBufferCopy->BindToPixelShader( GetContext().Get(), 2 );
        GetContext()->PSSetShaderResources( 7, 1, &materialClassSRV );
        ID3D11ShaderResourceView* waterCoverageSRV = WaterCoverageBuffer
            ? WaterCoverageBuffer->GetShaderResView().Get() : nullptr;
        GetContext()->PSSetShaderResources( 8, 1, &waterCoverageSRV );

        auto Resolution = GetResolution();

        // Fill refraction info CB and bind it
        RefractionInfoConstantBuffer ricb = {};
        ricb.RI_Projection = Engine::GAPI->GetProjectionMatrix();
        ricb.RI_ViewportSize = float2( Resolution.x, Resolution.y );
        ricb.RI_Time = Engine::GAPI->GetTimeSeconds();
        ricb.RI_CameraPosition = float3( Engine::GAPI->GetCameraPosition() );
        UpdateRefractionViewProjection( ricb );

        ActivePS->GetBuffer( "RefractionInfo" ).Update( &ricb ).Bind();
        // Bind the simple reflection cube as a safe fallback. The water shader controls
        // its visibility: full strength when Water Effects or dynamic SSR are off,
        // and only in masked/missing SSR areas when dynamic SSR is active.
        ID3D11ShaderResourceView* reflectionCubeSrv = ReflectionCube.Get();
        GetContext()->PSSetShaderResources( 3, 1, &reflectionCubeSrv );
        GetContext()->PSSetShaderResources( 6, 1, &lowCloudLayerSRV );

        if ( !FeatureLevel10Compatibility ) {
            // MDI path: one MDI call per texture batch
            for ( const auto& batch : waterBatches ) {
                batch.texture->CacheIn( -1 );
                batch.texture->Bind( 0 );

                WaterMaterialInfoConstantBuffer wmcb = {};
                FillWaterMaterialInfo( wmcb, batch.texture );
                ActivePS->GetBuffer( "WaterMaterialInfo" ).Update( &wmcb ).Bind();

                DrawMultiIndexedInstancedIndirect( Context.Get(),
                    batch.drawCount,
                    WaterIndirectBuffer->GetIndirectBuffer().Get(),
                    batch.argsOffset * argStride, argStride );
            }
        } else {
            // FL10 fallback: per-texture loop with direct DrawIndexed
            for ( const auto& batch : waterBatches ) {
                batch.texture->CacheIn( -1 );
                batch.texture->Bind( 0 );

                WaterMaterialInfoConstantBuffer wmcb = {};
                FillWaterMaterialInfo( wmcb, batch.texture );
                ActivePS->GetBuffer( "WaterMaterialInfo" ).Update( &wmcb ).Bind();

                for ( unsigned int i = 0; i < batch.drawCount; i++ ) {
                    const auto& args = waterDrawArgs[batch.argsOffset + i];
                    DrawVertexBufferIndexedUINT( nullptr, nullptr,
                        args.IndexCountPerInstance, args.StartIndexLocation );
                }
            }
        }
    }

    GetContext()->PSSetShaderResources( 0, 9, s_nullSRVs );

    GetContext()->OMSetRenderTargets( 1, HDRBackBuffer->GetRenderTargetView().GetAddressOf(),
        DepthStencilBuffer->GetDepthStencilView().Get() );
}

/** Draws everything around the given position */
void XM_CALLCONV D3D11GraphicsEngine::DrawWorldAround(
    FXMVECTOR position, float range, bool cullFront, bool indoor,
    bool noNPCs, std::list<VobInfo*>* renderedVobs,
    std::list<SkeletalVobInfo*>* renderedMobs,
    std::vector<std::pair<MeshKey, MeshInfo*>>* worldMeshCache,
    unsigned int casterMask ) {

    // Setup renderstates
    Engine::GAPI->GetRendererState().RasterizerState.SetDefault();
    Engine::GAPI->GetRendererState().RasterizerState.CullMode =
        cullFront ? GothicRasterizerStateInfo::CM_CULL_FRONT
        : GothicRasterizerStateInfo::CM_CULL_NONE;
    Engine::GAPI->GetRendererState().RasterizerState.DepthClipEnable = true;
    Engine::GAPI->GetRendererState().RasterizerState.SetDirty();

    Engine::GAPI->GetRendererState().DepthState.SetDefault();
    Engine::GAPI->GetRendererState().DepthState.DepthBufferCompareFunc = GothicDepthBufferStateInfo::ECompareFunc::CF_COMPARISON_LESS_EQUAL;
    Engine::GAPI->GetRendererState().DepthState.SetDirty();

    Context->PSSetShaderResources( 0, 6, s_nullSRVs );

    bool linearDepth =
        (Engine::GAPI->GetRendererState().GraphicsState.FF_GSwitches &
            GSWITCH_LINEAR_DEPTH) != 0;
    if ( linearDepth ) {
        SetActivePixelShader( PShaderID::PS_LinDepth );
    }

    // Set constant buffer
    ActivePS->GetBuffer( "FFPipelineConstantBuffer" )
        .Update( &Engine::GAPI->GetRendererState().GraphicsState )
        .Bind();

    GSky* sky = Engine::GAPI->GetSky();
    ActivePS->GetBuffer( "Atmosphere" )
        .Update( &sky->GetAtmosphereCB() )
        .Bind();

    // Init drawcalls
    SetupVS_ExMeshDrawCall();
    SetupVS_ExConstantBuffer();

    const XMMATRIX identityMatrix = XMMatrixIdentity();
    VS_ExConstantBuffer_PerInstance cbInstance = {};
    XMStoreFloat4x4( &cbInstance.World, identityMatrix );
    cbInstance.Color = float4( 1.0f, 1.0f, 1.0f, 1.0f );
    ActiveVS->GetBuffer( "Matrices_PerInstances" ).Update( &cbInstance, sizeof( cbInstance ) ).Bind();

    // Update and bind buffer of PS
    PerObjectState ocb;
    ocb.OS_AmbientColor = float3( 1, 1, 1 );
    ActivePS->GetBuffer( "POS_MaterialInfo" ).Update( &ocb ).Bind();

    WhiteTexture->BindToPixelShader( 0 );
    void* lastTex = WhiteTexture.get();

    ActivePS->BindBuffer( "DIST_Distance", InfiniteRangeConstantBuffer.get() );

    UpdateRenderStates();

    float alphaRef = Engine::GAPI->GetRendererState().GraphicsState.FF_AlphaRef;
    bool isOutdoor = (Engine::GAPI->GetLoadedWorldInfo()->BspTree->GetBspTreeMode() == zBSP_MODE_OUTDOOR);

    std::vector<WorldMeshSectionInfo*> drawnSections;

    auto rangeSquared = range * range;
    auto vRangeSquared = XMVectorReplicate(rangeSquared);
    auto vsBufMPI = ActiveVS->GetBuffer( "Matrices_PerInstances" );

    const bool drawWorldCasters = (casterMask & SHADOW_CASTER_WORLD) != 0;
    const bool drawVobCasters = (casterMask & SHADOW_CASTER_VOBS) != 0;
    const bool drawMobCasters = (casterMask & SHADOW_CASTER_MOBS) != 0;
    const bool drawAnimatedCasters = (casterMask & SHADOW_CASTER_ANIMATED) != 0;

    if ( drawWorldCasters && Engine::GAPI->GetRendererState().RendererSettings.DrawWorldMesh ) {
        vsBufMPI.Update( &identityMatrix, sizeof( identityMatrix ) ).Bind();

        // Only use cache if we haven't already collected the vobs
        // TODO: Collect vobs in a different way than using the drawn sections!
        //		 The current solution won't use the cache at all when there are
        // no vobs near!
        if ( worldMeshCache && !worldMeshCache->empty() ) {
            for ( auto&& meshInfoByKey = worldMeshCache->begin(); meshInfoByKey != worldMeshCache->end(); ++meshInfoByKey ) {
                bool isAlpha = false;
                // Bind texture
                if ( meshInfoByKey->first.Material && meshInfoByKey->first.Material->GetTexture() ) {
                    // Check surface type

                    if ( meshInfoByKey->first.Info->MaterialType != MaterialInfo::MT_None ) {
                        continue;
                    }

                    if ( meshInfoByKey->first.Material->HasAlphaTest() || meshInfoByKey->first.Material->GetTexture()->HasAlphaChannel() ) {
                        if ( alphaRef > 0.0f && meshInfoByKey->first.Material->GetTexture()->CacheIn( 0.6f ) == zRES_CACHED_IN ) {
                            void* engineTex = meshInfoByKey->first.Material->GetTexture()->GetSurface()->GetEngineTexture();
                            if ( lastTex != engineTex ) {
                                meshInfoByKey->first.Material->GetTexture()->GetSurface()->GetEngineTexture()->BindToPixelShader( 0 );
                                lastTex = engineTex;
                            }
                            ActivePS->Apply();
                            isAlpha = true;
                        } else
                            continue;  // Don't render if not loaded
                    } else {
                        if ( !linearDepth )  // Only unbind when not rendering linear depth
                        {
                            // Unbind PS
                            Context->PSSetShader( nullptr, nullptr, 0 );
                        } else {
                            if ( lastTex != WhiteTexture.get() ) {
                                WhiteTexture->BindToPixelShader( 0 );
                                lastTex = WhiteTexture.get();
                            }
                        }
                    }
                } else if ( linearDepth ) {
                    if ( lastTex != WhiteTexture.get() ) {
                        WhiteTexture->BindToPixelShader( 0 );
                        lastTex = WhiteTexture.get();
                    }
                }

                // Draw from wrapped mesh
                MeshInfo* mesh = meshInfoByKey->second;
                DrawVertexBufferIndexed( mesh->MeshVertexBuffer,
                    GetShadowAwareIndexBuffer( mesh, isAlpha ),
                    GetShadowAwareIndexCount( mesh, isAlpha ) );
            }
        } else {
            Frustum f;
            f.BuildCubemapFace( position, range, 0 );
            std::vector<WorldMeshSectionInfo*> sections = {};
            Engine::GAPI->CollectVisibleSections( sections, &f, true );
            for ( auto* section : sections ) {
                drawnSections.emplace_back( section );

                if ( Engine::GAPI->GetRendererState().RendererSettings.FastShadows ) {
                    // Draw world mesh
                    if ( section->FullStaticMesh )
                        Engine::GAPI->DrawMeshInfo( nullptr, section->FullStaticMesh );
                } else {
                    for ( auto&& meshInfoByKey = section->WorldMeshes.begin();
                        meshInfoByKey != section->WorldMeshes.end(); ++meshInfoByKey ) {
                        // Check surface type
                        if ( meshInfoByKey->first.Info->MaterialType != MaterialInfo::MT_None ) {
                            continue;
                        }

                        bool isAlpha = false;
                        // Bind texture
                        if ( meshInfoByKey->first.Material && meshInfoByKey->first.Material->GetTexture() ) {
                            if ( meshInfoByKey->first.Material->HasAlphaTest() || meshInfoByKey->first.Material->GetTexture()->HasAlphaChannel() ) {
                                if ( alphaRef > 0.0f &&
                                    meshInfoByKey->first.Material->GetTexture()->CacheIn( 0.6f ) ==
                                    zRES_CACHED_IN ) {
                                    void* engineTex = meshInfoByKey->first.Material->GetTexture()->GetSurface()->GetEngineTexture();
                                    if ( lastTex != engineTex ) {
                                        meshInfoByKey->first.Material->GetTexture()->GetSurface()->GetEngineTexture()->BindToPixelShader( 0 );
                                        lastTex = engineTex;
                                    }
                                    ActivePS->Apply();
                                    isAlpha = true;
                                } else
                                    continue;  // Don't render if not loaded
                            } else {
                                if ( !linearDepth )  // Only unbind when not rendering linear
                                    // depth
                                {
                                    // Unbind PS
                                    Context->PSSetShader( nullptr, nullptr, 0 );
                                } else {
                                    if ( lastTex != WhiteTexture.get() ) {
                                        WhiteTexture->BindToPixelShader( 0 );
                                        lastTex = WhiteTexture.get();
                                    }
                                }
                            }
                        } else if ( linearDepth ) {
                            if ( lastTex != WhiteTexture.get() ) {
                                WhiteTexture->BindToPixelShader( 0 );
                                lastTex = WhiteTexture.get();
                            }
                        }

                        // Draw from wrapped mesh
                        MeshInfo* mesh = meshInfoByKey->second;
                        DrawVertexBufferIndexed( mesh->MeshVertexBuffer,
                            GetShadowAwareIndexBuffer( mesh, isAlpha ),
                            GetShadowAwareIndexCount( mesh, isAlpha ) );
                    }
                }
            }
        }
    }

    if ( drawVobCasters && Engine::GAPI->GetRendererState().RendererSettings.DrawVOBs ) {
        // Draw visible vobs here
        std::list<VobInfo*> rndVob;
        // construct new renderedvob list or fake one
        if ( !renderedVobs || renderedVobs->empty() ) {
            for ( size_t i = 0; i < drawnSections.size(); i++ ) {
                for ( auto it : drawnSections[i]->Vobs ) {
                    if ( !it->VisualInfo ) {
                        continue;  // Seems to happen in Gothic 1
                    }

                    if ( !it->Vob->GetShowVisual() ) {
                        continue;
                    }

                    // Check vob range
                    if ( XMVector3Greater(XMVector3LengthSq( position - XMLoadFloat3( &it->LastRenderPosition ) ), vRangeSquared) ) {
                        continue;
                    }

                    // Check for inside vob. Don't render inside-vobs when the light is
                    // outside and vice-versa.
                    if ( isOutdoor && it->IsIndoorVob != indoor ) {
                        continue;
                    }
                    rndVob.emplace_back( it );
                }
            }

            if ( renderedVobs )*renderedVobs = rndVob;
        }

        // At this point either renderedVobs or rndVob is filled with something
        D3D11Texture* lastBoundTexture = nullptr;
        std::list<VobInfo*>& rl = renderedVobs != nullptr ? *renderedVobs : rndVob;
        VS_ExConstantBuffer_PerInstance cb;

        auto buffer = GetActiveVS()->GetBuffer(1).Bind();
        for ( auto const& vobInfo : rl ) {
            // Bind per-instance buffer
            vobInfo->UpdateVobConstantBuffer(cb);
            buffer.Update(&cb, sizeof(cb));

            // Draw the vob
            for ( auto const& materialMesh : vobInfo->VisualInfo->Meshes ) {
                bool isAlpha = false;
                if ( materialMesh.first && materialMesh.first->GetTexture() ) {
                    if ( materialMesh.first->GetTexture()->CacheIn( 0.6f ) == zRES_CACHED_IN
                        && (
                            (materialMesh.first->GetAlphaFunc() != zMAT_ALPHA_FUNC_NONE && materialMesh.first->GetAlphaFunc() != zMAT_ALPHA_FUNC_MAT_DEFAULT)
                            || materialMesh.first->GetTexture()->HasAlphaChannel())
                        ) {
                        isAlpha = true;
                        if ( lastBoundTexture != materialMesh.first->GetTexture()->GetSurface()->GetEngineTexture() ) {
                            lastBoundTexture = materialMesh.first->GetTexture()->GetSurface()->GetEngineTexture();
                            lastBoundTexture->BindToPixelShader( 0 );
                        }
                    } else {
                        if (lastBoundTexture != WhiteTexture.get()) {
                            WhiteTexture->BindToPixelShader( 0 );
                            lastBoundTexture = WhiteTexture.get();
                        }
                    }
                }
                for ( auto const& meshInfo : materialMesh.second ) {
                    DrawVertexBufferIndexed(
                        meshInfo->MeshVertexBuffer,
                        GetShadowAwareIndexBuffer( meshInfo, isAlpha ),
                        GetShadowAwareIndexCount( meshInfo, isAlpha ) );
                }
            }
        }
    }

    bool renderNPCs = !noNPCs && drawAnimatedCasters;
    if ( drawMobCasters && Engine::GAPI->GetRendererState().RendererSettings.DrawMobs ) {
        // Draw visible vobs here
        std::list<SkeletalVobInfo*> rndVob;

        // construct new renderedvob list or fake one
        if ( !renderedMobs || renderedMobs->empty() ) {
            for ( auto it : Engine::GAPI->GetSkeletalMeshVobs() ) {
                if ( !it->VisualInfo ) {
                    continue;  // Seems to happen in Gothic 1
                }

                if ( !it->Vob->GetShowVisual() ) {
                    continue;
                }

                // Check vob range
                if ( XMVector3Greater(XMVector3LengthSq( position - it->Vob->GetPositionWorldXM() ), vRangeSquared) ) {
                    continue;
                }

                // Check for inside vob. Don't render inside-vobs when the light is
                // outside and vice-versa.
                if ( isOutdoor && it->Vob->IsIndoorVob() != indoor ) {
                    continue;
                }

                // Assume everything that doesn't have a skeletal-mesh won't move very
                // much This applies to usable things like chests, chairs, beds, etc
                if ( !static_cast<SkeletalMeshVisualInfo*>(it->VisualInfo)->SkeletalMeshes.empty() ) {
                    continue;
                }

                rndVob.emplace_back( it );
            }

            if ( renderedMobs ) {
                *renderedMobs = rndVob;
            }
        }

        // One of renderedMobs or rndVob is populated.
        std::list<SkeletalVobInfo*>& rl = renderedMobs != nullptr ? *renderedMobs : rndVob;
        for ( auto it : rl ) {
            Engine::GAPI->DrawSkeletalMeshVob( it, FLT_MAX );
        }
    }

    if ( drawAnimatedCasters && Engine::GAPI->GetRendererState().RendererSettings.DrawSkeletalMeshes ) {
        // Draw animated skeletal meshes if wanted
        if ( renderNPCs ) {
            static std::vector<SkeletalVobInfo*> animatedSkeletalMeshVobs;
            animatedSkeletalMeshVobs.clear();
            for ( auto const& skeletalMeshVob : Engine::GAPI->GetAnimatedSkeletalMeshVobs() ) {
                if ( !skeletalMeshVob->VisualInfo ) {
                    // Seems to happen in Gothic 1
                    continue;
                }

                // Ghosts shouldn't have shadows
                if ( skeletalMeshVob->Vob->GetVisualAlpha() && skeletalMeshVob->Vob->GetVobTransparency() < 0.7f ) {
                    continue;
                }

                // Check vob range
                if ( XMVector3Greater(XMVector3LengthSq( position - skeletalMeshVob->Vob->GetPositionWorldXM() ), vRangeSquared) ) {
                    continue;
                }

                // Check for inside vob. Don't render inside-vobs when the light is
                // outside and vice-versa.
                if ( isOutdoor && skeletalMeshVob->Vob->IsIndoorVob() != indoor ) {
                    continue;
                }

                animatedSkeletalMeshVobs.emplace_back( skeletalMeshVob );
            }

            if ( !animatedSkeletalMeshVobs.empty() ) {
                DrawSkeletalMeshVobs( animatedSkeletalMeshVobs, FLT_MAX, false, true );
            }
        }
    }
}

void XM_CALLCONV D3D11GraphicsEngine::DrawWorldAround_Layered(
    FXMVECTOR position, float range, bool cullFront, bool indoor,
    bool noNPCs, std::list<VobInfo*>* renderedVobs,
    std::list<SkeletalVobInfo*>* renderedMobs,
    std::vector<std::pair<MeshKey, MeshInfo*>>* worldMeshCache,
    unsigned int casterMask ) {

    // Setup renderstates
    Engine::GAPI->GetRendererState().RasterizerState.SetDefault();
    Engine::GAPI->GetRendererState().RasterizerState.CullMode =
        cullFront ? GothicRasterizerStateInfo::CM_CULL_FRONT
        : GothicRasterizerStateInfo::CM_CULL_NONE;
    Engine::GAPI->GetRendererState().RasterizerState.DepthClipEnable = true;
    Engine::GAPI->GetRendererState().RasterizerState.SetDirty();

    Engine::GAPI->GetRendererState().DepthState.SetDefault();
    Engine::GAPI->GetRendererState().DepthState.DepthBufferCompareFunc = GothicDepthBufferStateInfo::ECompareFunc::CF_COMPARISON_LESS_EQUAL;
    Engine::GAPI->GetRendererState().DepthState.SetDirty();

    Context->PSSetShaderResources( 0, 6, s_nullSRVs );

    bool linearDepth =
        (Engine::GAPI->GetRendererState().GraphicsState.FF_GSwitches &
            GSWITCH_LINEAR_DEPTH) != 0;
    if ( linearDepth ) {
        SetActivePixelShader( PShaderID::PS_LinDepth );
    }

    // Set constant buffer
    ActivePS->GetBuffer( "FFPipelineConstantBuffer" )
        .Update( &Engine::GAPI->GetRendererState().GraphicsState )
        .Bind();

    GSky* sky = Engine::GAPI->GetSky();
    ActivePS->GetBuffer( "Atmosphere" )
        .Update( &sky->GetAtmosphereCB() )
        .Bind();

    // Init drawcalls
    SetupVS_ExMeshDrawCall();
    SetupVS_ExConstantBuffer();

    const XMMATRIX identityMatrix = XMMatrixIdentity();
    VS_ExConstantBuffer_PerInstance cbInstance = {};
    XMStoreFloat4x4( &cbInstance.World, identityMatrix );
    cbInstance.Color = float4( 1.0f, 1.0f, 1.0f, 1.0f );
    ActiveVS->GetBuffer( "Matrices_PerInstances" ).Update( &cbInstance, sizeof( cbInstance ) ).Bind();

    // Update and bind buffer of PS
    PerObjectState ocb;
    ocb.OS_AmbientColor = float3( 1, 1, 1 );
    ActivePS->GetBuffer( "POS_MaterialInfo" ).Update( &ocb ).Bind();

    float3 pos; XMStoreFloat3( pos.toXMFLOAT3(), position );
    INT2 s = WorldConverter::GetSectionOfPos( pos );

    DistortionTexture->BindToPixelShader( 0 );

    ActivePS->BindBuffer( "DIST_Distance", InfiniteRangeConstantBuffer.get() );

    UpdateRenderStates();

    auto enableCulling = Engine::GAPI->GetRendererState().RendererSettings.IsShadowFrustumCullingEnabled();

    bool colorWritesEnabled =
        Engine::GAPI->GetRendererState().BlendState.ColorWritesEnabled;
    float alphaRef = Engine::GAPI->GetRendererState().GraphicsState.FF_AlphaRef;
    bool isOutdoor = (Engine::GAPI->GetLoadedWorldInfo()->BspTree->GetBspTreeMode() == zBSP_MODE_OUTDOOR);

    std::vector<WorldMeshSectionInfo*> drawnSections;

    auto rangeSquared = range * range;
    auto vRangeSquared = XMVectorReplicate(rangeSquared);
    const float sectionRadius = std::max( 2.0f, (range / WORLD_SECTION_SIZE) + 1.5f );

    const bool drawWorldCasters = (casterMask & SHADOW_CASTER_WORLD) != 0;
    const bool drawVobCasters = (casterMask & SHADOW_CASTER_VOBS) != 0;
    const bool drawMobCasters = (casterMask & SHADOW_CASTER_MOBS) != 0;
    const bool drawAnimatedCasters = (casterMask & SHADOW_CASTER_ANIMATED) != 0;

    void* lastTex = nullptr;
    if ( drawWorldCasters && Engine::GAPI->GetRendererState().RendererSettings.DrawWorldMesh ) {
        ActiveVS->GetBuffer( "Matrices_PerInstances" ).Update( &identityMatrix, sizeof( identityMatrix ) ).Bind();
        auto _ = RecordGraphicsEvent( GE_NAME( "DrawWorldMesh::Layered" ) );
        // Only use cache if we haven't already collected the vobs
        // TODO: Collect vobs in a different way than using the drawn sections!
        //		 The current solution won't use the cache at all when there are
        // no vobs near!
        if ( worldMeshCache && !worldMeshCache->empty() ) {
            for ( auto&& meshInfoByKey = worldMeshCache->begin(); meshInfoByKey != worldMeshCache->end(); ++meshInfoByKey ) {
                // Bind texture
                bool isAlpha = false;
                if ( meshInfoByKey->first.Material && meshInfoByKey->first.Material->GetTexture() ) {
                    // Check surface type

                    if ( meshInfoByKey->first.Info->MaterialType != MaterialInfo::MT_None ) {
                        continue;
                    }

                    if ( meshInfoByKey->first.Material->HasAlphaTest() || meshInfoByKey->first.Material->GetTexture()->HasAlphaChannel() ) {
                        if ( alphaRef > 0.0f && meshInfoByKey->first.Material->GetTexture()->CacheIn(
                            0.6f ) == zRES_CACHED_IN ) {
                            lastTex = meshInfoByKey->first.Material->GetTexture()->GetSurface()->GetEngineTexture();
                            meshInfoByKey->first.Material->GetTexture()->GetSurface()->GetEngineTexture()->BindToPixelShader( 0 );
                            ActivePS->Apply();
                            isAlpha = true;
                        } else
                            continue;  // Don't render if not loaded
                    } else {
                        if ( !linearDepth )  // Only unbind when not rendering linear depth
                        {
                            // Unbind PS
                            Context->PSSetShader( nullptr, nullptr, 0 );
                        } else {
                            if ( lastTex != WhiteTexture.get() ) {
                                WhiteTexture->BindToPixelShader( 0 );
                                lastTex = WhiteTexture.get();
                            }
                        }
                    }
                } else if ( linearDepth ) {
                    if ( lastTex != WhiteTexture.get() ) {
                        WhiteTexture->BindToPixelShader( 0 );
                        lastTex = WhiteTexture.get();
                    }
                }

                // Draw from wrapped mesh
                MeshInfo* mesh = meshInfoByKey->second;
                DrawVertexBufferInstancedIndexed( mesh->MeshVertexBuffer,
                    GetShadowAwareIndexBuffer( mesh, isAlpha ),
                    GetShadowAwareIndexCount( mesh, isAlpha ),
                    6 );
            }
        } else {
            Frustum f;
            f.BuildCubemapFace( position, range, 0 );
            std::vector<WorldMeshSectionInfo*> sections={};
            Engine::GAPI->CollectVisibleSections( sections, &f, true );
            for ( auto section : sections ) {
                drawnSections.emplace_back( section );

                if ( Engine::GAPI->GetRendererState().RendererSettings.FastShadows ) {
                    // Draw world mesh
                    if ( section->FullStaticMesh )
                        Engine::GAPI->DrawMeshInfo( nullptr, section->FullStaticMesh );
                } else {
                    for ( auto&& meshInfoByKey = section->WorldMeshes.begin();
                        meshInfoByKey != section->WorldMeshes.end(); ++meshInfoByKey ) {
                        // Check surface type
                        if ( meshInfoByKey->first.Info->MaterialType != MaterialInfo::MT_None ) {
                            continue;
                        }

                        bool isAlpha = false;
                        // Bind texture
                        if ( meshInfoByKey->first.Material && meshInfoByKey->first.Material->GetTexture() ) {
                            if ( meshInfoByKey->first.Material ->HasAlphaTest() || meshInfoByKey->first.Material->GetTexture()->HasAlphaChannel()) {
                                if ( alphaRef > 0.0f &&
                                    meshInfoByKey->first.Material->GetTexture()->CacheIn( 0.6f ) ==
                                    zRES_CACHED_IN ) {

                                    lastTex = meshInfoByKey->first.Material->GetTexture()->GetSurface()->GetEngineTexture();
                                    meshInfoByKey->first.Material->GetTexture()->GetSurface()->GetEngineTexture()->BindToPixelShader( 0 );
                                    ActivePS->Apply();
                                    isAlpha = true;
                                } else
                                    continue;  // Don't render if not loaded
                            } else {
                                if ( !linearDepth )  // Only unbind when not rendering linear
                                    // depth
                                {
                                    // Unbind PS
                                    Context->PSSetShader( nullptr, nullptr, 0 );
                                } else {
                                    if ( lastTex != WhiteTexture.get() ) {
                                        WhiteTexture->BindToPixelShader( 0 );
                                        lastTex = WhiteTexture.get();
                                    }
                                }
                            }
                        } else if ( linearDepth ) {
                            if ( lastTex != WhiteTexture.get() ) {
                                WhiteTexture->BindToPixelShader( 0 );
                                lastTex = WhiteTexture.get();
                            }
                        }

                        // Draw from wrapped mesh
                        MeshInfo* mesh = meshInfoByKey->second;
                        DrawVertexBufferInstancedIndexed( mesh->MeshVertexBuffer,
                            GetShadowAwareIndexBuffer( mesh, isAlpha ),
                            GetShadowAwareIndexCount( mesh, isAlpha ),
                            6 );
                    }
                }
            }
        }
    }

    if ( drawVobCasters && Engine::GAPI->GetRendererState().RendererSettings.DrawVOBs ) {
        // Draw visible vobs here
        std::list<VobInfo*> rndVob;
        // construct new renderedvob list or fake one
        if ( !renderedVobs || renderedVobs->empty() ) {
            for ( size_t i = 0; i < drawnSections.size(); i++ ) {
                for ( auto it : drawnSections[i]->Vobs ) {
                    if ( !it->VisualInfo ) {
                        continue;  // Seems to happen in Gothic 1
                    }

                    if ( !it->Vob->GetShowVisual() ) {
                        continue;
                    }

                    // Check vob range

                    if ( XMVector3Greater(XMVector3LengthSq( position - XMLoadFloat3( &it->LastRenderPosition ) ), vRangeSquared) ) {
                        continue;
                    }

                    // Check for inside vob. Don't render inside-vobs when the light is
                    // outside and vice-versa.
                    if ( isOutdoor && it->IsIndoorVob != indoor ) {
                        continue;
                    }
                    rndVob.emplace_back( it );
                }
            }

            if ( renderedVobs )*renderedVobs = rndVob;
        }

        // At this point either renderedVobs or rndVob is filled with something
        std::list<VobInfo*>& rl = renderedVobs != nullptr ? *renderedVobs : rndVob;
        auto _ = Engine::GraphicsEngine->RecordGraphicsEvent( GE_NAME( "Draw vobs (layered)" ) );

        VS_ExConstantBuffer_PerInstance cb;
        auto buffer = GetActiveVS()->GetBuffer(1).Bind();

        D3D11Texture* lastBoundTexture = nullptr;
        for ( auto const& vobInfo : rl ) {
            // Bind per-instance buffer
            vobInfo->UpdateVobConstantBuffer(cb);
            buffer.Update(&cb, sizeof(cb));

            // Draw the vob1
            for ( auto const& materialMesh : vobInfo->VisualInfo->Meshes ) {
                bool isAlpha = false;
                if ( materialMesh.first && materialMesh.first->GetTexture() ) {
                    if ( materialMesh.first->GetTexture()->CacheIn( 0.6f ) == zRES_CACHED_IN
                        && (
                            (materialMesh.first->GetAlphaFunc() != zMAT_ALPHA_FUNC_NONE && materialMesh.first->GetAlphaFunc() != zMAT_ALPHA_FUNC_MAT_DEFAULT)
                            || materialMesh.first->GetTexture()->HasAlphaChannel())
                        ) {
                        isAlpha = true;
                        if ( lastBoundTexture != materialMesh.first->GetTexture()->GetSurface()->GetEngineTexture() ) {
                            lastBoundTexture = materialMesh.first->GetTexture()->GetSurface()->GetEngineTexture();
                            lastBoundTexture->BindToPixelShader( 0 );
                        }
                    } else {
                        if ( lastBoundTexture != WhiteTexture.get() ) {
                            WhiteTexture->BindToPixelShader( 0 );
                            lastBoundTexture = WhiteTexture.get();
                        }
                    }
                }
                for ( auto const& meshInfo : materialMesh.second ) {
                    DrawVertexBufferInstancedIndexed(
                        meshInfo->MeshVertexBuffer,
                        GetShadowAwareIndexBuffer( meshInfo, isAlpha ),
                        GetShadowAwareIndexCount( meshInfo, isAlpha ),
                        6 );
                }
            }
        }
    }

    bool renderNPCs = !noNPCs && drawAnimatedCasters;
    if ( drawMobCasters && Engine::GAPI->GetRendererState().RendererSettings.DrawMobs ) {
        // Draw visible vobs here
        std::list<SkeletalVobInfo*> rndVob;

        // construct new renderedvob list or fake one
        if ( !renderedMobs || renderedMobs->empty() ) {
            for ( auto it : Engine::GAPI->GetSkeletalMeshVobs() ) {
                if ( !it->VisualInfo ) {
                    continue;  // Seems to happen in Gothic 1
                }

                if ( !it->Vob->GetShowVisual() ) {
                    continue;
                }

                // Check vob range
                if ( XMVector3Greater(XMVector3LengthSq( position - it->Vob->GetPositionWorldXM() ), vRangeSquared) ) {
                    continue;
                }

                // Check for inside vob. Don't render inside-vobs when the light is
                // outside and vice-versa.
                if ( isOutdoor && it->Vob->IsIndoorVob() != indoor ) {
                    continue;
                }

                // Assume everything that doesn't have a skeletal-mesh won't move very
                // much This applies to usable things like chests, chairs, beds, etc
                if ( !static_cast<SkeletalMeshVisualInfo*>(it->VisualInfo)->SkeletalMeshes.empty() ) {
                    continue;
                }

                rndVob.emplace_back( it );
            }

            if ( renderedMobs ) {
                *renderedMobs = rndVob;
            }
        }

        // One of renderedMobs or rndVob is populated.
        std::list<SkeletalVobInfo*>& rl = renderedMobs != nullptr ? *renderedMobs : rndVob;
        auto _ = Engine::GraphicsEngine->RecordGraphicsEvent( GE_NAME( "Draw static skeletal meshes (layered)" ) );
        for ( auto it : rl ) {
            Engine::GAPI->DrawSkeletalMeshVob_Layered( it, FLT_MAX );
        }
    }

    if ( drawAnimatedCasters && Engine::GAPI->GetRendererState().RendererSettings.DrawSkeletalMeshes ) {
        // Draw animated skeletal meshes if wanted
        if ( renderNPCs ) {
            auto _ = Engine::GraphicsEngine->RecordGraphicsEvent( GE_NAME( "Draw animated skeletal meshes (layered)" ) );
            static std::vector<SkeletalVobInfo*> animatedSkeletalMeshVobs;
            animatedSkeletalMeshVobs.clear();
            for ( auto const& skeletalMeshVob : Engine::GAPI->GetAnimatedSkeletalMeshVobs() ) {
                if ( !skeletalMeshVob->VisualInfo ) {
                    // Seems to happen in Gothic 1
                    continue;
                }

                // Ghosts shouldn't have shadows
                if ( skeletalMeshVob->Vob->GetVisualAlpha() && skeletalMeshVob->Vob->GetVobTransparency() < 0.7f ) {
                    continue;
                }

                // Check vob range
                if ( XMVector3Greater(XMVector3LengthSq( position - skeletalMeshVob->Vob->GetPositionWorldXM() ), vRangeSquared) ) {
                    continue;
                }
                // Check for inside vob. Don't render inside-vobs when the light is
                // outside and vice-versa.
                if ( isOutdoor && skeletalMeshVob->Vob->IsIndoorVob() != indoor ) {
                    continue;
                }

                animatedSkeletalMeshVobs.emplace_back( skeletalMeshVob );
            }

            if ( !animatedSkeletalMeshVobs.empty() ) {
                DrawSkeletalMeshVobs( animatedSkeletalMeshVobs, FLT_MAX, false, true, true );
            }
        }
    }
}

void D3D11GraphicsEngine::ShadowPass_DrawWorldMesh_Indirect(
    const std::vector<WorldMeshSectionInfo*>& visibleSections,
    const Frustum* cullingFrustum,
    const ShadowWorldCasterCache* casterCache )
{
    TracyD3D11ZoneCGX( "ShadowPass_DrawWorldMesh_Indirect" );
    auto _scopeShadowPassIndirect = RecordGraphicsEvent( GE_NAME( "ShadowPass_DrawWorldMesh_Indirect" ) );

    float alphaRef = Engine::GAPI->GetRendererState().GraphicsState.FF_AlphaRef;
    bool linearDepth = (Engine::GAPI->GetRendererState().GraphicsState.FF_GSwitches &
                GSWITCH_LINEAR_DEPTH) != 0;
    const unsigned int daylightCutoutCount = RenderingStage == DES_SHADOWMAP && IsSunDaylightActive()
        ? UpdateAndBindWindowCutouts( true )
        : 0;

    auto drawMultiIndexedInstancedIndirect = Engine::GAPI->GetRendererState().RendererSettings.DebugSettings.FeatureSet.UseMDI
        ? DrawMultiIndexedInstancedIndirect
        : Stub_DrawMultiIndexedInstancedIndirect;

    if ( Engine::GAPI->GetRendererState().RendererSettings.FastShadows && !cullingFrustum ) {
        if ( daylightCutoutCount > 0 ) {
            SetActivePixelShader( PShaderID::PS_WindowCutoutDepth );
            ActivePS->Apply();
        } else if ( !linearDepth ) {
            Context->PSSetShader( nullptr, nullptr, 0 );
        }

        for ( const WorldMeshSectionInfo* section : visibleSections ) {
            if ( section->FullStaticMesh ) {
                Engine::GAPI->DrawMeshInfo( nullptr, section->FullStaticMesh );
            }
        }
        UnbindWindowCutouts();
        return;
    }

    // Collect all meshes first, then batch by alpha requirement.
    static thread_local std::vector<D3D11_DRAW_INDEXED_INSTANCED_INDIRECT_ARGS> opaqueDrawArgs;
    static thread_local std::vector<std::pair<zCTexture*, MeshInfo*>> alphaMeshes;
    opaqueDrawArgs.clear();
    alphaMeshes.clear();
    if ( opaqueDrawArgs.capacity() == 0 ) {
        opaqueDrawArgs.reserve( 4096 );
        alphaMeshes.reserve( 512 );
    }

    {
        TracyD3D11ZoneCGX( "ShadowPass_DrawWorldMesh_Indirect::Classify" );
        auto _scopeClassify = RecordGraphicsEvent( GE_NAME( "ShadowPass_DrawWorldMesh_Indirect::Classify" ) );
        auto appendCaster = [&]( WorldMeshInfo* mesh, zCTexture* tex, bool alphaTest ) {
            if ( !mesh )
                return;

            const unsigned int indexCount = GetShadowAwareIndexCount( mesh, alphaTest );
            if ( alphaTest ) {
                alphaMeshes.emplace_back( tex, mesh );
            } else {
                D3D11_DRAW_INDEXED_INSTANCED_INDIRECT_ARGS args;
                args.IndexCountPerInstance = indexCount;
                args.InstanceCount = 1;
                args.StartIndexLocation = mesh->BaseShadowIndexLocation;
                args.BaseVertexLocation = 0;
                args.StartInstanceLocation = 0;
                opaqueDrawArgs.push_back( args );
            }
            Engine::GAPI->GetRendererState().RendererInfo.FrameDrawnTriangles += indexCount / 3;
        };

        if ( casterCache ) {
            for ( const ShadowWorldCaster& caster : *casterCache ) {
                if ( !IsShadowWorldCasterVisible( caster, cullingFrustum ) )
                    continue;
                appendCaster( caster.Mesh, caster.Texture, caster.AlphaTest );
            }
        } else {
            for ( const WorldMeshSectionInfo* section : visibleSections ) {
                for ( const auto& meshPair : section->WorldMeshes ) {
                    if ( !meshPair.first.Info || meshPair.first.Info->MaterialType != MaterialInfo::MT_None )
                        continue;

                    WorldMeshInfo* mesh = meshPair.second;
                    if ( cullingFrustum && !Engine::GAPI->IsWorldMeshVisibleInFrustum( mesh, *cullingFrustum ) )
                        continue;

                    zCMaterial* material = meshPair.first.Material;
                    if ( !material )
                        continue;
                    const int alphaFunc = material->GetAlphaFunc();
                    if ( (alphaFunc > zMAT_ALPHA_FUNC_NONE && alphaFunc != zMAT_ALPHA_FUNC_TEST)
                        || (alphaFunc == zMAT_ALPHA_FUNC_NONE && zColor( material->GetColor() ).bgra.alpha < 255) )
                        continue;

                    zCTexture* tex = material->GetTexture();
                    const bool alphaTest = tex && tex->HasAlphaChannel() && alphaRef > 0.0f;
                    if ( alphaTest && tex->CacheIn( 0.6f ) != zRES_CACHED_IN )
                        continue;
                    appendCaster( mesh, tex, alphaTest );
                }
            }
        }
    }

    MeshInfo* wrappedWorldMesh = Engine::GAPI->GetWrappedWorldMesh();

    if ( opaqueDrawArgs.empty() && alphaMeshes.empty() ) {
        UnbindWindowCutouts();
        return;
    }

    UINT offset = 0;
    UINT uStride = sizeof( ExVertexStruct );
    Context->IASetVertexBuffers( 0, 1, wrappedWorldMesh->MeshVertexBuffer->GetVertexBuffer().GetAddressOf(), &uStride, &offset );

    if ( !opaqueDrawArgs.empty() ) {
        TracyD3D11ZoneCGX( "ShadowPass_DrawWorldMesh_Indirect::OpaqueSubmission" );
        auto _scopeOpaqueSubmission = RecordGraphicsEvent( GE_NAME( "ShadowPass_DrawWorldMesh_Indirect::OpaqueSubmission" ) );
        if ( daylightCutoutCount > 0 ) {
            SetActivePixelShader( PShaderID::PS_WindowCutoutDepth );
            ActivePS->Apply();
        } else if ( !linearDepth ) {
            Context->PSSetShader( nullptr, nullptr, 0 );
        }

        const size_t requiredSize = opaqueDrawArgs.size() * sizeof( D3D11_DRAW_INDEXED_INSTANCED_INDIRECT_ARGS );
        D3D11IndirectBuffer* shadowIndirectBuffer = AcquireFrameIndirectBuffer( m_ShadowWorldIndirectPool,
            opaqueDrawArgs.data(),
            static_cast<unsigned int>( requiredSize ),
            "ShadowWorldMeshIndirectArgs" );

        if ( shadowIndirectBuffer ) {
            Context->IASetIndexBuffer( wrappedWorldMesh->MeshShadowIndexBuffer->GetVertexBuffer().Get(), DXGI_FORMAT_R32_UINT, 0 );

            drawMultiIndexedInstancedIndirect( Context.Get(),
                static_cast<unsigned int>( opaqueDrawArgs.size() ),
                shadowIndirectBuffer->GetIndirectBuffer().Get(),
                0,
                sizeof( D3D11_DRAW_INDEXED_INSTANCED_INDIRECT_ARGS ) );
        }
    }

    if ( !alphaMeshes.empty() ) {
        TracyD3D11ZoneCGX( "ShadowPass_DrawWorldMesh_Indirect::AlphaSubmission" );
        auto _scopeAlphaSubmission = RecordGraphicsEvent( GE_NAME( "ShadowPass_DrawWorldMesh_Indirect::AlphaSubmission" ) );
        std::sort( alphaMeshes.begin(), alphaMeshes.end(),
            []( const auto& a, const auto& b ) { return a.first < b.first; } );
        SetActivePixelShader( linearDepth ? PShaderID::PS_LinDepth : PShaderID::PS_DiffuseAlphaTestShadows );
        ActivePS->Apply();
        zCTexture* lastTex = nullptr;
        Context->PSSetShaderResources( 0, 3, s_nullSRVs );
        Context->IASetIndexBuffer( wrappedWorldMesh->MeshIndexBuffer->GetVertexBuffer().Get(), DXGI_FORMAT_R32_UINT, 0 );

        for ( const auto& [tex, mesh] : alphaMeshes ) {
            bool textureReady = tex == lastTex;
            if ( tex != lastTex ) {
                textureReady = tex->GetCacheState() == zRES_CACHED_IN
                    || tex->CacheIn( 0.6f ) == zRES_CACHED_IN;
                if ( textureReady ) {
                    auto t = tex->GetSurface()->GetEngineTexture()->GetShaderResourceView().Get();
                    Context->PSSetShaderResources( 0, 1, &t );
                    lastTex = tex;
                }
            }
            if ( !textureReady ) {
                continue;
            }

            DrawVertexBufferIndexedUINT( nullptr, nullptr,
                GetShadowAwareIndexCount( mesh, true ),
                mesh->BaseIndexLocation );
        }
    }
    UnbindWindowCutouts();
}

void D3D11GraphicsEngine::ShadowPass_DrawWorldMesh(
    const std::vector<WorldMeshSectionInfo*>& visibleSections,
    const Frustum* cullingFrustum,
    const ShadowWorldCasterCache* casterCache )
{
    TracyD3D11ZoneCGX( "ShadowPass_DrawWorldMesh" );
    auto _scopeShadowPass = RecordGraphicsEvent( GE_NAME( "ShadowPass_DrawWorldMesh" ) );

    float alphaRef = Engine::GAPI->GetRendererState().GraphicsState.FF_AlphaRef;
    bool linearDepth = (Engine::GAPI->GetRendererState().GraphicsState.FF_GSwitches &
                GSWITCH_LINEAR_DEPTH) != 0;
    const unsigned int daylightCutoutCount = RenderingStage == DES_SHADOWMAP && IsSunDaylightActive()
        ? UpdateAndBindWindowCutouts( true )
        : 0;

    static thread_local std::vector<WorldMeshInfo*> opaqueMeshes;
    static thread_local std::vector<std::pair<zCTexture*, MeshInfo*>> alphaMeshes;
    opaqueMeshes.clear();
    alphaMeshes.clear();

    {
        ZoneScopedN( "ShadowPass_DrawWorldMesh::Classify" );
        auto _scopeClassify = RecordGraphicsEvent( GE_NAME( "ShadowPass_DrawWorldMesh::Classify" ) );
        auto appendCaster = [&]( WorldMeshInfo* mesh, zCTexture* tex, bool alphaTest ) {
            if ( !mesh )
                return;
            if ( alphaTest )
                alphaMeshes.emplace_back( tex, mesh );
            else
                opaqueMeshes.push_back( mesh );
        };

        if ( casterCache ) {
            for ( const ShadowWorldCaster& caster : *casterCache ) {
                if ( !IsShadowWorldCasterVisible( caster, cullingFrustum ) )
                    continue;
                appendCaster( caster.Mesh, caster.Texture, caster.AlphaTest );
            }
        } else {
            for ( const WorldMeshSectionInfo* section : visibleSections ) {
                for ( const auto& meshPair : section->WorldMeshes ) {
                    // Skip non-standard materials (water, portals, etc.)
                    if ( !meshPair.first.Info || meshPair.first.Info->MaterialType != MaterialInfo::MT_None )
                        continue;

                    if ( cullingFrustum && !Engine::GAPI->IsWorldMeshVisibleInFrustum( meshPair.second, *cullingFrustum ) )
                        continue;

                    zCMaterial* material = meshPair.first.Material;
                    if ( !material )
                        continue;
                    const int alphaFunc = material->GetAlphaFunc();
                    if ( (alphaFunc > zMAT_ALPHA_FUNC_NONE && alphaFunc != zMAT_ALPHA_FUNC_TEST)
                        || (alphaFunc == zMAT_ALPHA_FUNC_NONE && zColor( material->GetColor() ).bgra.alpha < 255) )
                        continue;

                    zCTexture* tex = material->GetTexture();
                    const bool alphaTest = tex && tex->HasAlphaChannel() && alphaRef > 0.0f;
                    if ( alphaTest && tex->CacheIn( 0.6f ) != zRES_CACHED_IN )
                        continue;
                    appendCaster( meshPair.second, tex, alphaTest );
                }
            }
        }
    }

    if (opaqueMeshes.empty() && alphaMeshes.empty() ) {
        UnbindWindowCutouts();
        return;
    }

    MeshInfo* wrappedWorldMesh = Engine::GAPI->GetWrappedWorldMesh();
    UINT offset = 0;
    UINT uStride = sizeof( ExVertexStruct );
    Context->IASetVertexBuffers( 0, 1, wrappedWorldMesh->MeshVertexBuffer->GetVertexBuffer().GetAddressOf(), &uStride, &offset );

    // Draw all opaque meshes without pixel shader (depth only)
    if ( !opaqueMeshes.empty() ) {
        TracyD3D11ZoneCGX( "ShadowPass_DrawWorldMesh::OpaqueSubmission" );
        auto _scopeOpaqueSubmission = RecordGraphicsEvent( GE_NAME( "ShadowPass_DrawWorldMesh::OpaqueSubmission" ) );
        if ( daylightCutoutCount > 0 ) {
            SetActivePixelShader( PShaderID::PS_WindowCutoutDepth );
            ActivePS->Apply();
        } else if ( !linearDepth )  // Only unbind when not rendering linear depth
        {
            // Unbind PS
            Context->PSSetShader( nullptr, nullptr, 0 );
        }
        Context->IASetIndexBuffer( wrappedWorldMesh->MeshShadowIndexBuffer->GetVertexBuffer().Get(), DXGI_FORMAT_R32_UINT, 0 );

        for ( auto mesh : opaqueMeshes ) {
            DrawVertexBufferIndexedUINT( nullptr, nullptr,
                GetShadowAwareIndexCount( mesh, false ),
                mesh->BaseShadowIndexLocation );
        }
    }

    // Draw alpha-tested meshes with texture binding
    if ( !alphaMeshes.empty() ) {
        TracyD3D11ZoneCGX( "ShadowPass_DrawWorldMesh::AlphaSubmission" );
        auto _scopeAlphaSubmission = RecordGraphicsEvent( GE_NAME( "ShadowPass_DrawWorldMesh::AlphaSubmission" ) );
        // Sort by texture to minimize binding changes
        std::sort( alphaMeshes.begin(), alphaMeshes.end(),
            []( const auto& a, const auto& b ) { return a.first < b.first; } );
        SetActivePixelShader( linearDepth ? PShaderID::PS_LinDepth : PShaderID::PS_DiffuseAlphaTestShadows );
        ActivePS->Apply();
        zCTexture* lastTex = nullptr;

        Context->PSSetShaderResources( 0, 3, s_nullSRVs );
        Context->IASetIndexBuffer( wrappedWorldMesh->MeshIndexBuffer->GetVertexBuffer().Get(), DXGI_FORMAT_R32_UINT, 0 );

        for ( const auto& [tex, mesh] : alphaMeshes ) {
            bool textureReady = tex == lastTex;
            if ( tex != lastTex ) {
                textureReady = tex->GetCacheState() == zRES_CACHED_IN
                    || tex->CacheIn( 0.6f ) == zRES_CACHED_IN;
                if ( textureReady ) {
                    auto t = tex->GetSurface()->GetEngineTexture()->GetShaderResourceView().Get();
                    Context->PSSetShaderResources( 0, 1, &t );
                    lastTex = tex;
                }
            }
            if ( !textureReady ) {
                continue;
            }
            DrawVertexBufferIndexed( nullptr, nullptr,
                GetShadowAwareIndexCount( mesh, true ),
                mesh->BaseIndexLocation );
        }
    }
    UnbindWindowCutouts();
}

/** Draws everything around the given position */
void XM_CALLCONV D3D11GraphicsEngine::DrawWorldAroundForWorldShadow( FXMVECTOR position,
    float sectionRange,
    const RenderShadowmapsParams& params ) {

    if ( Engine::IsShuttingDown() || !Engine::GAPI || !Context || !InfiniteRangeConstantBuffer
        || !DistortionTexture ) {
        return;
    }

    // Setup renderstates
    Engine::GAPI->GetRendererState().RasterizerState.SetDefault();
    Engine::GAPI->GetRendererState().RasterizerState.CullMode =
        params.CullFront ? GothicRasterizerStateInfo::CM_CULL_FRONT
        : GothicRasterizerStateInfo::CM_CULL_BACK;
    if ( params.DontCull )
        Engine::GAPI->GetRendererState().RasterizerState.CullMode =
        GothicRasterizerStateInfo::CM_CULL_NONE;

    Engine::GAPI->GetRendererState().RasterizerState.DepthClipEnable = true;
    Engine::GAPI->GetRendererState().RasterizerState.SetDirty();

    Engine::GAPI->GetRendererState().DepthState.SetDefault();
    Engine::GAPI->GetRendererState().DepthState.DepthBufferCompareFunc = GothicDepthBufferStateInfo::ECompareFunc::CF_COMPARISON_LESS_EQUAL;
    Engine::GAPI->GetRendererState().DepthState.SetDirty();

    XMMATRIX view = Engine::GAPI->GetViewMatrixXM();

    Engine::GAPI->ResetWorldTransform();
    Engine::GAPI->SetViewTransformXM( view );

    // Set shader
    SetActivePixelShader( PShaderID::PS_DiffuseAlphaTestShadows );
    SetActiveVertexShader( VShaderID::VS_Ex );

    bool linearDepth =
        (Engine::GAPI->GetRendererState().GraphicsState.FF_GSwitches &
            GSWITCH_LINEAR_DEPTH) != 0;
    if ( linearDepth ) {
        SetActivePixelShader( PShaderID::PS_LinDepth );
    }
    auto defaultPS = ActivePS;

    // Set constant buffer
    Engine::GAPI->GetRendererState().GraphicsState.FF_AlphaRef = 170.0f / 255.0f; // zRnd_D3D uses 0xb0 = 170 as default alpha ref
    ActivePS->GetBuffer( "FFPipelineConstantBuffer" )
        .Update( &Engine::GAPI->GetRendererState().GraphicsState )
        .Bind();

    GSky* sky = Engine::GAPI->GetSky();
    ActivePS->GetBuffer( "Atmosphere" )
        .Update( &sky->GetAtmosphereCB() )
        .Bind();

    // Init drawcalls
    SetupVS_ExMeshDrawCall();
    SetupVS_ExConstantBuffer();

    const XMMATRIX identityMatrix = XMMatrixIdentity();
    auto cbMatrices_PerInstances = ActiveVS->GetBuffer( "Matrices_PerInstances" )
        .Update( &identityMatrix, sizeof( identityMatrix ) )
        .Bind();

    float3 fPosition; XMStoreFloat3( fPosition.toXMFLOAT3(), position );
    DistortionTexture->BindToPixelShader( 0 );

    ActivePS->BindBuffer( "DIST_Distance", InfiniteRangeConstantBuffer.get() );

    UpdateRenderStates();

    auto enableCulling = Engine::GAPI->GetRendererState().RendererSettings.IsShadowFrustumCullingEnabled();

    bool colorWritesEnabled =
        Engine::GAPI->GetRendererState().BlendState.ColorWritesEnabled;
    float alphaRef = Engine::GAPI->GetRendererState().GraphicsState.FF_AlphaRef;
    const Frustum* currentFrustum = nullptr;
    Frustum alwaysContainingFrustum;

    if ( params.CascadeIndex != -1 && params.CascadeCameraReplacements ) {
        currentFrustum = &params.CascadeCameraReplacements->at( params.CascadeIndex ).frustum;
    } else if ( Engine::GAPI->GetCameraReplacementPtr() != nullptr ) {
        currentFrustum = &Engine::GAPI->GetCameraReplacementPtr()->frustum;
    }

    if ( !currentFrustum || !currentFrustum->IsValid() ) {
        alwaysContainingFrustum = Frustum::AlwaysContainingFrustum();
        currentFrustum = &alwaysContainingFrustum;
    }

    const Frustum* worldMeshShadowFrustum = currentFrustum;

    if ( Engine::GAPI->GetRendererState().RendererSettings.DrawWorldMesh ) {
        TracyD3D11ZoneCGX( "Shadows::DrawWorldMesh" );
        auto _1 = RecordGraphicsEvent( GE_NAME( "Shadows::DrawWorldMesh" ) );
        cbMatrices_PerInstances.Update( &identityMatrix, sizeof( identityMatrix ) ).Bind();

        static thread_local std::vector<WorldMeshSectionInfo*> visibleSections;
        visibleSections.clear();
        if ( !params.WorldShadowCasters ) {
            Engine::GAPI->CollectVisibleSections( visibleSections, worldMeshShadowFrustum, false );
        }

        if ( Engine::GAPI->GetRendererState().RendererSettings.DebugSettings.FeatureSet.UseMDI ) {
            MeshInfo* wrappedWorldMesh = Engine::GAPI->GetWrappedWorldMesh();
            if ( wrappedWorldMesh
                && wrappedWorldMesh->MeshVertexBuffer
                && wrappedWorldMesh->MeshIndexBuffer
                && wrappedWorldMesh->MeshShadowIndexBuffer
                ) {
                ShadowPass_DrawWorldMesh_Indirect( visibleSections, worldMeshShadowFrustum, params.WorldShadowCasters );
            } else {
                ShadowPass_DrawWorldMesh( visibleSections, worldMeshShadowFrustum, params.WorldShadowCasters );
            }
        } else {
            ShadowPass_DrawWorldMesh( visibleSections, worldMeshShadowFrustum, params.WorldShadowCasters );
        }
    }

    if ( Engine::GAPI->GetRendererState().RendererSettings.DrawVOBs ) {
        ZoneScopedN( "Shadows::DrawVOBs" );

        static std::vector<VobInfo*> potentialCasters;
        std::vector<VobInfo*>& vobs = potentialCasters;
        if (params.CascadeIndex != -1) {
            auto renderQueue = ShadowMaps->GetRenderQueue( params.CascadeIndex );
            renderQueue->ProcessQueue();

            vobs = renderQueue->GetVobs();
        } else {
            static std::vector<VobLightInfo*> _1;
            static std::vector<SkeletalVobInfo*> _2;
            potentialCasters.reserve(1024);
            potentialCasters.clear();

            LegacyRenderQueueProxy q(potentialCasters, _1, _2);
            RndCullContext ctx;
            ctx.queue = &q;
            ctx.cameraPosition = Engine::GAPI->GetCameraPosition();
            ctx.stage = RenderStage::STAGE_DRAW_WORLD;
            ctx.frustum = *currentFrustum;
            const auto& rs = Engine::GAPI->GetRendererState().RendererSettings;
            ctx.drawDistances.OutdoorVobs = rs.OutdoorVobDrawRadius;
            ctx.drawDistances.OutdoorVobsSmall = rs.OutdoorSmallVobDrawRadius;
            ctx.drawDistances.IndoorVobs = rs.IndoorVobDrawRadius;
            ctx.drawDistances.VisualFX = rs.GetEffectiveVisualFXDrawRadius();
            ctx.drawDistancesSq.OutdoorVobs = ctx.drawDistances.OutdoorVobs * ctx.drawDistances.OutdoorVobs;
            ctx.drawDistancesSq.OutdoorVobsSmall = ctx.drawDistances.OutdoorVobsSmall * ctx.drawDistances.OutdoorVobsSmall;
            ctx.drawDistancesSq.IndoorVobs = ctx.drawDistances.IndoorVobs * ctx.drawDistances.IndoorVobs;
            ctx.drawDistancesSq.VisualFX = ctx.drawDistances.VisualFX * ctx.drawDistances.VisualFX;
            ctx.drawFlags.DrawVOBs = rs.DrawVOBs;
            ctx.drawFlags.DrawMobs = rs.DrawMobs;
            ctx.drawFlags.EnableDynamicLighting = rs.EnableDynamicLighting;
            ctx.drawFlags.CullVobs = rs.DebugSettings.Culling.CullVobs;
            ctx.drawFlags.CollectIndoorVobs = true;
            ctx.drawFlags.CollectLargeVobs = true;
            ctx.drawFlags.CollectSmallVobs = true;
            ctx.drawFlags.CollectMobs = true;
            ctx.drawFlags.CollectLights = true;
            Engine::GAPI->CollectVisibleVobs( ctx );
        }

        // clear any residue of main render pass
        for ( auto const& staticMeshVisual : Engine::GAPI->GetStaticMeshVisuals() ) {
            staticMeshVisual.second->StartNewFrame();
        }

        for ( auto& it : vobs) {
            // process any vobs only visible in this cascade
            it->UpdateState();

            VobInstanceInfo vii = {};
            vii.world = it->WorldMatrix;
            vii.prevWorld = it->HasValidPrevMatrix ? it->PrevWorldMatrix : it->WorldMatrix;
            vii.color = it->GroundColor;
            if ( it->IndoorLightMask ) {
                vii.color = (vii.color & 0x00FFFFFFu) | 0x0D000000u;
            }
            vii.windStrenth = 0.0f;
            vii.canBeAffectedByPlayer = 0;

            zTAnimationMode aniMode = it->Vob->GetVisualAniMode();
            if ( aniMode != zVISUAL_ANIMODE_NONE ) {
                vii.canBeAffectedByPlayer = (!it->Vob->GetDynColl() ? 1.0f : 0.0f);
                GothicAPI::ProcessVobAnimation( it->Vob, aniMode, vii );
            }

            auto* meshVisual = reinterpret_cast<MeshVisualInfo*>(it->VisualInfo);
            meshVisual->Instances.push_back( vii );
            meshVisual->InstanceVobs.push_back( it );
        }

        TracyD3D11ZoneNX( "Shadows::DrawVOBs" );
        auto _1 = RecordGraphicsEvent( GE_NAME( "Shadows::DrawVOBs" ) );

        const size_t shadowInstanceCount = vobs.empty() ? 1 : vobs.size();
        const unsigned int shadowInstancingBytes = static_cast<unsigned int>(
            shadowInstanceCount * sizeof( VobInstanceInfo ) );
        D3D11VertexBuffer* shadowInstancingBuffer = AcquireFrameInstancingBuffer(
            m_ShadowVobInstancingPool, shadowInstancingBytes, "ShadowVobInstancingBuffer" );
        if ( !shadowInstancingBuffer ) {
            LogError() << "Failed to acquire shadow vob instancing buffer.";
            shadowInstancingBuffer = DynamicInstancingBuffer.get();
            if ( shadowInstancingBuffer && shadowInstancingBuffer->GetSizeInBytes() < shadowInstancingBytes ) {
                shadowInstancingBuffer->Init( nullptr, shadowInstancingBytes,
                    D3D11VertexBuffer::B_VERTEXBUFFER,
                    D3D11VertexBuffer::U_DYNAMIC,
                    D3D11VertexBuffer::CA_WRITE,
                    "ShadowVobInstancingBufferFallback" );
            }
        }

        std::vector<MeshVisualInfo*> activeVisuals;
        activeVisuals.reserve(256); // Reserve enough memory to avoid allocations
        for ( auto const& pair : Engine::GAPI->GetStaticMeshVisuals() ) {
            if ( !pair.second->Instances.empty() ) {
                activeVisuals.push_back(pair.second);
            }
        }

        // Apply instancing shader early so metadata indexing can be prepared before instance upload.
        SetActiveVertexShader( VShaderID::VS_ExInstancedObj );
        ActiveVS->Apply();
        const bool useWindMetadata = PrepareAndBindWindMetadata( activeVisuals );

        byte* data = nullptr;
        UINT size = 0;
        bool shadowInstanceUploadSucceeded = false;
        if ( SUCCEEDED( shadowInstancingBuffer->Map( D3D11VertexBuffer::M_WRITE_DISCARD,
            reinterpret_cast<void**>(&data), &size ) ) ) {
            UINT loc = 0;
            for ( auto const& staticMeshVisual : activeVisuals ) {
                staticMeshVisual->StartInstanceNum = loc;
                memcpy( data + loc * sizeof( VobInstanceInfo ), staticMeshVisual->Instances.data(),
                    sizeof( VobInstanceInfo ) * staticMeshVisual->Instances.size() );
                loc += staticMeshVisual->Instances.size();
            }
            shadowInstancingBuffer->Unmap();
            shadowInstanceUploadSucceeded = true;
        } else {
            LogError() << "Failed to map dynamic instancing buffer for vobs.";
        }

        if ( !linearDepth )  // Only unbind when not rendering linear depth
        {
            // Unbind PS
            Context->PSSetShader( nullptr, nullptr, 0 );
        }

        GraphicsShaderConstantBuffer windBuffer = {};
        const auto& windSettings =
            Engine::GAPI->GetRendererState().RendererSettings;

        if ( ActiveVS
            && (windSettings.WindQuality
                    != GothicRendererSettings::EWindQuality::
                        WIND_QUALITY_NONE
                || windSettings.HeroAffectsObjects) ) {
            windBuffer = ActiveVS->GetBuffer( "WindParams" );
            windBuffer.Bind();
        }

        if ( windBuffer.GetRawBuffer() ) {
            UpdateCharacterInteractionPositions( g_windBuffer );
            windBuffer.Update( &g_windBuffer );
        }

        UINT dynOffset[] = { 0 };
        UINT dynuStride[] = { sizeof( VobInstanceInfo ) };

        // Draw all vobs the player currently sees
        D3D11PShader* currPs = nullptr;

        size_t numMeshesToDraw = 0;
        for ( auto const& staticMeshVisual : activeVisuals ) {
            if ( staticMeshVisual->Instances.empty() ) continue;
            for ( auto const& itt : staticMeshVisual->MeshesByTexture ) {
                std::vector<MeshInfo*>& mlist = staticMeshVisual->MeshesByTexture[itt.first];
                if ( mlist.empty() ) continue;
                for ( unsigned int i = 0; i < mlist.size(); i++ ) {
                    ++numMeshesToDraw;
                }
            }
        }
        std::vector<std::tuple<MeshVisualInfo*, MeshKey, MeshInfo*, uint64_t>> instancedMeshesToDraw;
        instancedMeshesToDraw.reserve( numMeshesToDraw );

        for ( auto const& staticMeshVisual : activeVisuals ) {
            if ( staticMeshVisual->Instances.empty() ) continue;
            for ( auto const& itt : staticMeshVisual->MeshesByTexture ) {
                const std::vector<MeshInfo*>& mlist = itt.second;

                uint64_t sortKeyBase = 0;
                if ( itt.first.Material && itt.first.Material->GetAniTexture() ) {
                    if ( itt.first.Material->GetAniTexture()->CacheIn( 0.6f ) != zRES_CACHED_IN ) {
                        continue; // cant draw if no texture
                    }
                    sortKeyBase = BuildSortKeyBase( itt.first.Material );
                }

                if ( mlist.empty() ) continue;
                for ( unsigned int i = 0; i < mlist.size(); i++ ) {
                    MeshInfo* mi = mlist[i];

                    instancedMeshesToDraw.emplace_back( staticMeshVisual, itt.first, mi, sortKeyBase + mi->meshId );
                }
            }
        }

        std::sort( instancedMeshesToDraw.begin(), instancedMeshesToDraw.end(), []( const std::tuple<MeshVisualInfo*, MeshKey, MeshInfo*, uint64_t>& a, const std::tuple<MeshVisualInfo*, MeshKey, MeshInfo*, uint64_t>& b ) {
            return std::get<3>( a ) < std::get<3>( b );
        } );

        // CSM VOBs use their own cascade matrix and a frustum-only compute
        // pass. The main-view Hi-Z result is never reused here: every shadow
        // cascade has a different camera and must compact its own instances.
        // This path is enabled only when real MDI is available; otherwise the
        // compute pass would add work without reducing the per-mesh CPU
        // submission cost. Far cascades additionally select the persistent
        // reduced shadow index range below.
        bool shadowGpuCullingActive = false;
        D3D11VertexBuffer* shadowGpuInstanceBuffer = nullptr;
        D3D11IndirectBuffer* shadowGpuArgsBuffer = nullptr;
        std::vector<GpuVobCullVisualBatch> shadowGpuVisuals;
        std::vector<FrameGeometryCache::CachedInstancedMeshDraw> shadowGpuDraws;
        std::vector<int> shadowGpuArgIndices( instancedMeshesToDraw.size(), -1 );
        XMMATRIX shadowCullViewProj = XMMatrixIdentity();
        const XMMATRIX* shadowCullViewProjPtr = nullptr;

        const auto& shadowSettings = Engine::GAPI->GetRendererState().RendererSettings;
        const bool isCascadeShadowPass = params.CascadeIndex >= 0
            && params.CascadeIndex < MAX_CSM_CASCADES
            && params.CascadeCameraReplacements
            && !FeatureLevel10Compatibility
            && shadowSettings.DebugSettings.FeatureSet.UseMDI;
        if ( shadowInstanceUploadSucceeded
            && isCascadeShadowPass
            && shadowSettings.GetEffectiveGpuVobOcclusionCulling()
            && shadowSettings.DebugSettings.Culling.CullVobs ) {
            const auto& cascadeCamera = params.CascadeCameraReplacements->at( params.CascadeIndex );
            shadowCullViewProj = XMLoadFloat4x4( &cascadeCamera.ProjectionReplacement )
                * XMLoadFloat4x4( &cascadeCamera.ViewReplacement );
            shadowCullViewProjPtr = &shadowCullViewProj;

            const bool shadowArenaReady = PrepareGpuVobGeometryArena();
            std::unordered_map<MeshVisualInfo*, UINT> shadowVisualIndices;
            shadowVisualIndices.reserve( activeVisuals.size() );
            shadowGpuVisuals.reserve( activeVisuals.size() );
            for ( MeshVisualInfo* visual : activeVisuals ) {
                if ( !visual || visual->Instances.empty() )
                    continue;

                const UINT visualIndex = static_cast<UINT>( shadowGpuVisuals.size() );
                shadowVisualIndices.emplace( visual, visualIndex );
                GpuVobCullVisualBatch batch;
                batch.Visual = visual;
                batch.StartInstanceNum = visual->StartInstanceNum;
                batch.InstanceCount = static_cast<UINT>( visual->Instances.size() );
                shadowGpuVisuals.push_back( batch );
            }

            shadowGpuDraws.reserve( instancedMeshesToDraw.size() );
            for ( size_t drawIndex = 0; drawIndex < instancedMeshesToDraw.size(); ++drawIndex ) {
                const auto& drawItem = instancedMeshesToDraw[drawIndex];
                MeshVisualInfo* visual = std::get<0>( drawItem );
                const MeshKey& meshKey = std::get<1>( drawItem );
                MeshInfo* mesh = std::get<2>( drawItem );
                if ( !visual || !mesh || !meshKey.Material ) {
                    continue;
                }
                zCTexture* texture = meshKey.Material
                    ? meshKey.Material->GetAniTexture() : nullptr;
                const bool directionalDaylightPass = RenderingStage == DES_SHADOWMAP
                    && IsSunDaylightActive();
                const bool windowGlassMaterial = IsCityWindowFeatureReady()
                    && IsWindowGlassMaterial( visual, texture );
                const bool indoorWindowGlass = directionalDaylightPass
                    && windowGlassMaterial
                    && IsIndoorWindowVisual( visual->VisualName );
                const bool outdoorWindowGlass = directionalDaylightPass
                    && windowGlassMaterial
                    && !IsIndoorWindowVisual( visual->VisualName );
                const bool alphaMaterial = texture
                    && (indoorWindowGlass || texture->HasAlphaChannel()
                        || colorWritesEnabled || meshKey.Material->HasAlphaTest());
                if ( outdoorWindowGlass || alphaMaterial ) {
                    continue;
                }

                const auto visualIt = shadowVisualIndices.find( visual );
                if ( visualIt == shadowVisualIndices.end() )
                    continue;

                FrameGeometryCache::CachedInstancedMeshDraw gpuDraw;
                gpuDraw.VisualIndex = visualIt->second;
                gpuDraw.Mesh = meshKey;
                gpuDraw.MeshEntry = mesh;
                gpuDraw.sortKey = std::get<3>( drawItem );
                gpuDraw.UseShadowLodIndex = params.CascadeIndex >= 2
                    && GetShadowLodIndexBuffer( mesh ) != nullptr;
                gpuDraw.UseShadowIndex = !gpuDraw.UseShadowLodIndex;
                if ( gpuDraw.UseShadowLodIndex ) {
                    ++m_GpuVobPerformanceStats.ShadowVobLodMeshes;
                    m_GpuVobPerformanceStats.ShadowVobLodSourceTriangles +=
                        static_cast<uint64_t>( GetShadowAwareIndexCount( mesh, false ) / 3u );
                    m_GpuVobPerformanceStats.ShadowVobLodTriangles +=
                        static_cast<uint64_t>( GetShadowLodIndexCount( mesh ) / 3u );
                }
                shadowGpuArgIndices[drawIndex] = static_cast<int>( shadowGpuDraws.size() );
                shadowGpuDraws.push_back( gpuDraw );
            }

            if ( !shadowGpuVisuals.empty() && !shadowGpuDraws.empty() ) {
                ++m_GpuVobPerformanceStats.ShadowVobCullAttempts;
                // The same persistent output resource is also used by the
                // main-view indirect path. Unbind any previous IA reference
                // before writing it as a compute UAV for this cascade.
                ID3D11Buffer* nullVertexBuffers[2] = {};
                UINT nullStrides[2] = {};
                UINT nullOffsets[2] = {};
                GetContext()->IASetVertexBuffers( 0, 2, nullVertexBuffers,
                    nullStrides, nullOffsets );
                GetContext()->IASetIndexBuffer( nullptr, VERTEX_INDEX_DXGI_FORMAT, 0 );
                shadowGpuCullingActive = PrepareGpuVobCulling(
                    shadowInstancingBuffer,
                    shadowGpuVisuals,
                    shadowGpuDraws,
                    shadowArenaReady ? GpuVobGeometryVertexBuffer.get() : nullptr,
                    shadowArenaReady ? GpuVobGeometryIndexBuffer.get() : nullptr,
                    GpuVobCullInputBuffer,
                    GpuVobCullVisualBuffer,
                    GpuVobCullDrawVisualBuffer,
                    GpuVobCullOutputBuffer,
                    GpuVobCullArgsBuffer,
                    GpuVobCullVisibleCountsBuffer,
                    GpuVobCullConstantBuffer,
                    GpuVobPatchConstantBuffer,
                    shadowGpuInstanceBuffer,
                    shadowGpuArgsBuffer,
                    shadowCullViewProjPtr,
                    false );
                if ( shadowGpuCullingActive ) {
                    ++m_GpuVobPerformanceStats.ShadowVobCullActive;
                } else {
                    ++m_GpuVobPerformanceStats.ShadowVobCullFallback;
                }
            }
        }

        ID3D11Buffer* shadowDrawInstanceBuffer = shadowInstancingBuffer->GetVertexBuffer().Get();
        ID3D11Buffer* lastShadowInstanceBuffer = nullptr;
        std::vector<bool> shadowGpuMdiConsumed( instancedMeshesToDraw.size(), false );
        if ( shadowDrawInstanceBuffer ) {
            GetContext()->IASetVertexBuffers( 1, 1, &shadowDrawInstanceBuffer,
                dynuStride, dynOffset );
            lastShadowInstanceBuffer = shadowDrawInstanceBuffer;
        }

        zCTexture* previousTx = nullptr;
        ID3D11ShaderResourceView* previousShadowSrv = nullptr;
        MeshVisualInfo* lastWindVisual = nullptr;

        for ( size_t drawIndex = 0; drawIndex < instancedMeshesToDraw.size(); ++drawIndex ) {
            if ( shadowGpuMdiConsumed[drawIndex] ) {
                continue;
            }
            const auto& drawItem = instancedMeshesToDraw[drawIndex];
            MeshVisualInfo* staticMeshVisual = std::get<0>( drawItem );
            const MeshKey& meshKey = std::get<1>( drawItem );
            MeshInfo* meshInfo = std::get<2>( drawItem );
            if ( !staticMeshVisual || !meshKey.Material || !meshInfo ) {
                continue;
            }
            if ( !useWindMetadata && windBuffer.GetRawBuffer() && lastWindVisual != staticMeshVisual ) {
                lastWindVisual = staticMeshVisual;
                g_windBuffer.minHeight = staticMeshVisual->BBox.Min.y;
                g_windBuffer.maxHeight = staticMeshVisual->BBox.Max.y;
                windBuffer.Update( &g_windBuffer );
            }

            zCTexture* tx = meshKey.Material->GetAniTexture();
            const bool directionalDaylightPass = RenderingStage == DES_SHADOWMAP && IsSunDaylightActive();
            const bool windowGlassMaterial = IsCityWindowFeatureReady()
                && IsWindowGlassMaterial( staticMeshVisual, tx );
            const bool indoorWindowGlass = directionalDaylightPass
                && windowGlassMaterial
                && IsIndoorWindowVisual( staticMeshVisual->VisualName );
            const bool outdoorWindowGlass = directionalDaylightPass
                && windowGlassMaterial
                && !IsIndoorWindowVisual( staticMeshVisual->VisualName );
            if ( outdoorWindowGlass ) {
                continue;
            }

            bool bindTexture = tx
                && (indoorWindowGlass || tx->HasAlphaChannel() || colorWritesEnabled || meshKey.Material->HasAlphaTest());
            const bool isAlpha = bindTexture;

            // Bind texture
            if ( bindTexture ) {
                ID3D11ShaderResourceView* shadowSrv = indoorWindowGlass
                    ? GetWindowGlassReplacementSRV()
                    : tx->GetSurface()->GetEngineTexture()->GetShaderResourceView().Get();
                if ( !shadowSrv ) {
                    shadowSrv = tx->GetSurface()->GetEngineTexture()->GetShaderResourceView().Get();
                }
                if ( previousTx != tx || previousShadowSrv != shadowSrv ) {
                    if ( tx->CacheIn( 0.6f ) == zRES_CACHED_IN ) {
                        Context->PSSetShaderResources( 0, 1, &shadowSrv );
                        auto nextPs = defaultPS.get();
                        if ( currPs != nextPs ) {
                            currPs = nextPs;
                            currPs->Apply();
                        }
                        previousTx = tx;
                        previousShadowSrv = shadowSrv;
                    } else
                        continue;
                }

            } else {
                if ( !linearDepth )  // Only unbind when not rendering linear depth
                {
                    // Unbind PS
                    if ( currPs != nullptr ) {
                        Context->PSSetShader( nullptr, nullptr, 0 );
                        currPs = nullptr;
                    }
                }
            }

            MeshInfo* mi = meshInfo;
            const int gpuArgIndex = drawIndex < shadowGpuArgIndices.size()
                ? shadowGpuArgIndices[drawIndex] : -1;
            const bool useGpuShadowDraw = shadowGpuCullingActive
                && gpuArgIndex >= 0 && shadowGpuInstanceBuffer && shadowGpuArgsBuffer;
            const auto geometryRange = GpuVobGeometryRanges.find( mi );
            const bool useShadowLod = params.CascadeIndex >= 2
                && !isAlpha && GetShadowLodIndexBuffer( mi ) != nullptr;
            if ( useShadowLod && !shadowGpuCullingActive ) {
                ++m_GpuVobPerformanceStats.ShadowVobLodMeshes;
                m_GpuVobPerformanceStats.ShadowVobLodSourceTriangles +=
                    static_cast<uint64_t>( GetShadowAwareIndexCount( mi, false ) / 3u );
                m_GpuVobPerformanceStats.ShadowVobLodTriangles +=
                    static_cast<uint64_t>( GetShadowLodIndexCount( mi ) / 3u );
            }
            const bool useShadowGeometryArena = useGpuShadowDraw
                && !isAlpha
                && !useShadowLod
                && GpuVobGeometryVertexBuffer
                && GpuVobGeometryIndexBuffer
                && geometryRange != GpuVobGeometryRanges.end();

            ID3D11Buffer* wantedShadowInstanceBuffer = useGpuShadowDraw
                ? shadowGpuInstanceBuffer->GetVertexBuffer().Get()
                : shadowInstancingBuffer->GetVertexBuffer().Get();
            if ( wantedShadowInstanceBuffer && wantedShadowInstanceBuffer != lastShadowInstanceBuffer ) {
                GetContext()->IASetVertexBuffers( 1, 1, &wantedShadowInstanceBuffer,
                    dynuStride, dynOffset );
                lastShadowInstanceBuffer = wantedShadowInstanceBuffer;
            }

            const auto vb = useShadowGeometryArena
                ? GpuVobGeometryVertexBuffer.get() : mi->MeshVertexBuffer;
            const auto ib = useShadowGeometryArena
                ? GpuVobGeometryIndexBuffer.get()
                : (useShadowLod ? mi->MeshShadowLodIndexBuffer
                    : GetShadowAwareIndexBuffer( mi, isAlpha ));

            UINT offset[] = { 0 };
            UINT uStride[] = { sizeof( ExVertexStruct ) };
            ID3D11Buffer* buffers[1] = { vb->GetVertexBuffer().Get() };

            auto numIndices = useShadowGeometryArena
                ? static_cast<size_t>( geometryRange->second.ShadowIndexCount )
                : (useShadowLod
                    ? static_cast<size_t>( GetShadowLodIndexCount( mi ) )
                    : static_cast<size_t>( GetShadowAwareIndexCount( mi, isAlpha ) ));
            const auto numInstances = staticMeshVisual->Instances.size();
            const auto startInstanceNum = staticMeshVisual->StartInstanceNum;

            size_t shadowMdiCount = 1;
            if ( useGpuShadowDraw
                && useShadowGeometryArena
                && shadowSettings.DebugSettings.FeatureSet.UseMDI ) {
                while ( drawIndex + shadowMdiCount < instancedMeshesToDraw.size() ) {
                    const size_t nextIndex = drawIndex + shadowMdiCount;
                    if ( shadowGpuMdiConsumed[nextIndex]
                        || shadowGpuArgIndices[nextIndex]
                            != gpuArgIndex + static_cast<int>( shadowMdiCount ) ) {
                        break;
                    }

                    const auto& nextItem = instancedMeshesToDraw[nextIndex];
                    if ( std::get<0>( nextItem ) != staticMeshVisual
                        || std::get<1>( nextItem ).Material != meshKey.Material
                        || std::get<1>( nextItem ).Texture != meshKey.Texture
                        || std::get<1>( nextItem ).Info != meshKey.Info ) {
                        break;
                    }

                    MeshInfo* nextMesh = std::get<2>( nextItem );
                    const auto nextRange = GpuVobGeometryRanges.find( nextMesh );
                    if ( !nextMesh || nextRange == GpuVobGeometryRanges.end()
                        || (params.CascadeIndex >= 2
                            && GetShadowLodIndexBuffer( nextMesh ) != nullptr) ) {
                        break;
                    }

                    shadowGpuMdiConsumed[nextIndex] = true;
                    ++shadowMdiCount;
                }
            }

            GetContext()->IASetVertexBuffers( 0, 1, buffers, uStride, offset );

            Context->IASetIndexBuffer( ib->GetVertexBuffer().Get(), VERTEX_INDEX_DXGI_FORMAT, 0 );

            unsigned int max =
                Engine::GAPI->GetRendererState().RendererSettings.MaxNumFaces * 3;
            numIndices = max != 0 ? (numIndices < max ? numIndices : max) : numIndices;
            size_t submittedDrawCount = 1;
            size_t submittedTriangles = numIndices / 3;

            if ( useGpuShadowDraw ) {
                ++m_GpuVobPerformanceStats.ShadowVobIndirectDrawCalls;
                if ( shadowMdiCount > 1 ) {
                    auto drawMulti = shadowSettings.DebugSettings.FeatureSet.UseMDI
                        ? DrawMultiIndexedInstancedIndirect
                        : Stub_DrawMultiIndexedInstancedIndirect;
                    drawMulti( GetContext().Get(), static_cast<UINT>( shadowMdiCount ),
                        shadowGpuArgsBuffer->GetIndirectBuffer().Get(),
                        static_cast<UINT>( gpuArgIndex
                            * sizeof( D3D11_DRAW_INDEXED_INSTANCED_INDIRECT_ARGS ) ),
                        sizeof( D3D11_DRAW_INDEXED_INSTANCED_INDIRECT_ARGS ) );
                } else {
                    GetContext()->DrawIndexedInstancedIndirect(
                        shadowGpuArgsBuffer->GetIndirectBuffer().Get(),
                        static_cast<UINT>( gpuArgIndex
                            * sizeof( D3D11_DRAW_INDEXED_INSTANCED_INDIRECT_ARGS ) ) );
                }
            } else {
                GetContext()->DrawIndexedInstanced( static_cast<UINT>( numIndices ),
                    static_cast<UINT>( numInstances ), 0, 0, static_cast<UINT>( startInstanceNum ) );
            }

            Engine::GAPI->GetRendererState().RendererInfo.FrameDrawnTriangles +=
                submittedTriangles * numInstances;

            Engine::GAPI->GetRendererState().RendererInfo.FrameDrawnVobs += submittedDrawCount;
        }

        if ( useWindMetadata ) {
            UnbindWindMetadata();
        }

        for ( auto [_, sm] : Engine::GAPI->GetStaticMeshVisuals() ) {
            sm->StartNewFrame();
        }
    }

    // Animated characters are sub-texel in the far CSM cascades but still pay
    // the full skinning and draw cost. Keep all characters in cascade 0 and
    // only the hero/near actors in cascade 1; cascades 2+ use static casters.
    const bool skeletalCascadeRelevant = params.CascadeIndex < 0 || params.CascadeIndex <= 1;
    if ( Engine::GAPI->GetRendererState().RendererSettings.DrawSkeletalMeshes
        && skeletalCascadeRelevant ) {
        ZoneScopedN( "Shadows::DrawSkeletalMeshes" );
        auto _1 = RecordGraphicsEvent( GE_NAME( "Shadows::DrawSkeletalMeshes" ) );

        auto skeletalRadiusSq = Engine::GAPI->GetRendererState().RendererSettings.SkeletalMeshDrawRadius
            * Engine::GAPI->GetRendererState().RendererSettings.SkeletalMeshDrawRadius;
        XMVECTOR vSkeletalRadiusSq = XMVectorReplicate(skeletalRadiusSq);

        // Draw skeletal meshes

        static std::vector<SkeletalVobInfo*> animatedSkeletalMeshVobs;
        animatedSkeletalMeshVobs.clear();

        for ( auto const& skeletalMeshVob : Engine::GAPI->GetSkeletalMeshVobs() ) {
            if ( !skeletalMeshVob->VisualInfo ) continue;

            // Ghosts shouldn't have shadows
            if ( skeletalMeshVob->Vob->GetVisualAlpha() && skeletalMeshVob->Vob->GetVobTransparency() < 0.7f ) {
                continue;
            }

            if ( params.CascadeIndex == 1 && skeletalMeshVob->Vob != Engine::GAPI->GetPlayerVob() ) {
                constexpr float secondCascadeActorRadius = 4000.0f;
                const XMVECTOR cameraDelta = skeletalMeshVob->Vob->GetPositionWorldXM()
                    - Engine::GAPI->GetCameraPositionXM();
                if ( XMVectorGetX( XMVector3LengthSq( cameraDelta ) )
                    > secondCascadeActorRadius * secondCascadeActorRadius ) {
                    continue;
                }
            }

            if ( XMVector3Greater(XMVector3LengthSq( skeletalMeshVob->Vob->GetPositionWorldXM() - position ), vSkeletalRadiusSq) ) {
                continue;  // Skip out of range
            }

            if ( enableCulling ) {
                if ( !currentFrustum->Intersects( skeletalMeshVob->Vob->GetBBox()) ) {
                    // Not hitting our frustum and not the active view.
                    continue;
                }
            }

            animatedSkeletalMeshVobs.push_back( skeletalMeshVob );
        }
        bool drawAttachments = true;
        if ( Engine::GAPI->GetRendererState().RendererSettings.GetEffectiveShadowFrustumCullingMode()
            == GothicRendererSettings::E_ShadowFrustumCulling::SHD_FRUSTUM_CULLING_AGGRESSIVE ) {
            drawAttachments = params.CascadeIndex <= 1; // skip attachments on higher cascades, player won't notice, hopefully
        }
        // we should not need to update the skeletal meshes again, as they were updated before drawing the main scene
        DrawSkeletalMeshVobs( animatedSkeletalMeshVobs, FLT_MAX, false, drawAttachments );
    }

    Engine::GAPI->GetRendererState().BlendState.ColorWritesEnabled = true;
    Engine::GAPI->GetRendererState().BlendState.SetDirty();
}

/** Update morph mesh visual */
void D3D11GraphicsEngine::UpdateMorphMeshVisual() {
    const auto& staticMeshVisuals = Engine::GAPI->GetStaticMeshVisuals();

    for ( auto const& staticMeshVisual : staticMeshVisuals ) {
        if ( !staticMeshVisual.second->MorphMeshVisual ) continue;
        if ( staticMeshVisual.second->Instances.empty() ) continue;
        WorldConverter::UpdateMorphMeshVisual( staticMeshVisual.second->MorphMeshVisual, staticMeshVisual.second );
    }
}
namespace {
    void UpdateMorphMeshVisuals( std::vector<MeshVisualInfo*>& staticMeshVisuals ) {
        for ( auto const& staticMeshVisual : staticMeshVisuals ) {
            if ( !staticMeshVisual->MorphMeshVisual ) continue;
            if ( staticMeshVisual->Instances.empty() ) continue;
            WorldConverter::UpdateMorphMeshVisual( staticMeshVisual->MorphMeshVisual, staticMeshVisual );
        }
    }
}

bool D3D11GraphicsEngine::PrepareAndBindWindMetadata( const std::vector<MeshVisualInfo*>& activeVisuals ) {
    if ( !ActiveVS || activeVisuals.empty() ) {
        return false;
    }

    if ( ActiveVS->GetInputIndex( "WindMetaData" ) == -1 ) {
        return false;
    }

    m_WindMetadataStaging.clear();
    size_t instanceCount = 0;
    for ( const MeshVisualInfo* visual : activeVisuals ) {
        instanceCount += visual ? visual->Instances.size() : 0;
    }
    m_WindMetadataStaging.reserve( instanceCount );

    for ( MeshVisualInfo* visual : activeVisuals ) {
        if ( !visual ) {
            continue;
        }
        const float grassShear = IsGrassWindVisual( visual->VisualName ) ? 1.0f : 0.0f;

        for ( size_t i = 0; i < visual->Instances.size(); ++i ) {
            VobWindMetadata metadata = {};
            metadata.MinHeight = visual->BBox.Min.y;
            metadata.MaxHeight = visual->BBox.Max.y;
            metadata.HorizontalExtent = float2(
                std::max( (visual->BBox.Max.x - visual->BBox.Min.x) * 0.5f, 0.001f ),
                std::max( (visual->BBox.Max.z - visual->BBox.Min.z) * 0.5f, 0.001f ) );
            metadata.GrassShear = grassShear;

            // Ground polygon vertices are already in world space. Reusing Gothic's
            // placement result avoids per-frame terrain traces and also handles slopes.
            if ( metadata.GrassShear > 0.5f && i < visual->InstanceVobs.size() ) {
                VobInfo* vobInfo = visual->InstanceVobs[i];
                if ( vobInfo && !vobInfo->WindGroundPlaneInitialized ) {
                    zCPolygon* ground = vobInfo->Vob ? vobInfo->Vob->GetGroundPoly() : nullptr;
                    vobInfo->WindGroundPlaneInitialized = true;
                    vobInfo->WindGroundPolygon = ground;
                    vobInfo->WindGroundPlane = {};
                    zCVertex** vertices = ground ? ground->getVertices() : nullptr;
                    if ( ground && ground->GetNumPolyVertices() >= 3 && vertices
                        && vertices[0] && vertices[1] && vertices[2] ) {
                        const float3& gp0 = vertices[0]->Position;
                        const float3& gp1 = vertices[1]->Position;
                        const float3& gp2 = vertices[2]->Position;
                        const XMVECTOR p0 = XMVectorSet( gp0.x, gp0.y, gp0.z, 0.0f );
                        const XMVECTOR p1 = XMVectorSet( gp1.x, gp1.y, gp1.z, 0.0f );
                        const XMVECTOR p2 = XMVectorSet( gp2.x, gp2.y, gp2.z, 0.0f );
                        XMVECTOR normal = XMVector3Cross( XMVectorSubtract( p1, p0 ), XMVectorSubtract( p2, p0 ) );
                        const float lengthSq = XMVectorGetX( XMVector3LengthSq( normal ) );
                        if ( lengthSq > 1.0e-8f ) {
                            normal = XMVector3Normalize( normal );
                            if ( XMVectorGetY( normal ) < 0.0f ) normal = XMVectorNegate( normal );
                            XMFLOAT3 n;
                            XMStoreFloat3( &n, normal );
                            // GetGroundPoly can briefly reference steep contact
                            // geometry. Only terrain-like planes are safe wind anchors.
                            if ( n.y >= 0.45f ) {
                                vobInfo->WindGroundPlane = XMFLOAT4( n.x, n.y, n.z,
                                    -XMVectorGetX( XMVector3Dot( normal, p0 ) ) );
                            }
                        }
                    }
                }
                if ( vobInfo ) metadata.GroundPlane = vobInfo->WindGroundPlane;
            }

            visual->Instances[i].GP_Slot = static_cast<DWORD>(m_WindMetadataStaging.size());
            m_WindMetadataStaging.push_back( metadata );
        }
    }

    if ( m_WindMetadataStaging.empty() ) {
        return false;
    }

    const UINT requiredSize = static_cast<UINT>(m_WindMetadataStaging.size() * sizeof( VobWindMetadata ));

    if ( !WindMetadataBuffer || WindMetadataBuffer->GetSizeInBytes() < requiredSize ) {
        WindMetadataBuffer = std::make_unique<D3D11VertexBuffer>();
        if ( XR_SUCCESS != WindMetadataBuffer->Init(
            nullptr,
            requiredSize,
            D3D11VertexBuffer::B_SHADER_RESOURCE,
            D3D11VertexBuffer::U_DEFAULT,
            D3D11VertexBuffer::CA_NONE,
            "WindMetadataBuffer",
            sizeof( VobWindMetadata ) ) ) {
            WindMetadataBuffer.reset();
            return false;
        }

        SetDebugName( WindMetadataBuffer->GetShaderResourceView().Get(), "WindMetadataBuffer->ShaderResourceView" );
        SetDebugName( WindMetadataBuffer->GetVertexBuffer().Get(), "WindMetadataBuffer->Buffer" );
    }

    if ( XR_SUCCESS != WindMetadataBuffer->UpdateBufferSubresource( m_WindMetadataStaging.data(), requiredSize ) ) {
        return false;
    }

    ActiveVS->BindResource( "WindMetaData", WindMetadataBuffer->GetShaderResourceView().Get() );
    return true;
}

void D3D11GraphicsEngine::UnbindWindMetadata() {
    if ( ActiveVS ) {
        ActiveVS->BindResource( "WindMetaData", nullptr );
    }
}

/** Updates wind direction and set time for shader */
void D3D11GraphicsEngine::ApplyWindProps( VS_ExConstantBuffer_Wind& windBuff ) {
    // Changing wind direction settings
    constexpr float CHANGE_INTERVAL_MIN = 10.0f; // seconds min
    constexpr float CHANGE_INTERVAL_MAX = 35.0f; // seconds max
    constexpr float BLEND_TIME_SEC = 5.0f; // (4 seconds to change direction)

    static XMVECTOR currentDir = XMVectorSet( 0.3f, 0.15f, 0.5f, 0.0f ); // base and current direction

    static XMVECTOR targetDir = currentDir;
    static float timeToNext = CHANGE_INTERVAL_MIN;

    float dt = Engine::GAPI->GetFrameTimeSec();
    timeToNext -= dt;

    // This code randomly creates wind direction in time
    if ( timeToNext <= 0.0f ) {
        XMVECTOR baseDir = XMVector3Normalize( currentDir );
        float baseYaw = atan2f( XMVectorGetZ( baseDir ), XMVectorGetX( baseDir ) );
        float basePitch = asinf( XMVectorGetY( baseDir ) );

        float azimuthOffset = -XM_PIDIV2 + (static_cast<float>(std::rand()) / RAND_MAX) * XM_PI; //random angle -pi/2 to pi/2
        float newYaw = baseYaw + azimuthOffset;

        XMVECTOR horiz = XMVectorSet( cosf( newYaw ), 0.0f, sinf( newYaw ), 0.0f );
        horiz = XMVector3Normalize( horiz );

        float sinPitch = sinf( basePitch );
        XMVECTOR newDir = XMVectorSet( XMVectorGetX( horiz ), sinPitch, XMVectorGetZ( horiz ), 0.0f );
        targetDir = XMVector3Normalize( newDir );
        timeToNext = CHANGE_INTERVAL_MIN + (static_cast<float>(std::rand()) / RAND_MAX) * (CHANGE_INTERVAL_MAX - CHANGE_INTERVAL_MIN);
    }

    float lerpT = dt / BLEND_TIME_SEC;

    // Smoothly turns wind's direction when it is changing
    currentDir = XMVector3Normalize( XMVectorLerp( currentDir, targetDir, lerpT ) );

    // Sets wind dir to const buffer
    XMStoreFloat3( reinterpret_cast<XMFLOAT3*>(&windBuff.windDir), currentDir );

    const auto& settings = Engine::GAPI->GetRendererState().RendererSettings;
    windBuff.accurateWindVelocity = settings.Upscaler
        == GothicRendererSettings::UPSCALER_FSR_3 ? 1.0f : 0.0f;
    XMStoreFloat4( reinterpret_cast<XMFLOAT4*>(&windBuff.cameraWorldPosition),
        XMVectorSetW( Engine::GAPI->GetCameraPositionXM(), 1.0f ) );
    windBuff.characterInteractionStrength =
        settings.HeroAffectsObjects ? 1.0f : 0.0f;
    windBuff.characterInteractionRange = BUILD_213_CHARACTER_INTERACTION_RANGE;

    static float WindGlobalTime = 0.0f;

    // get rain weight
    float rainWeight = Engine::GAPI->GetRainFXWeight();

    // limit in 0..1 range
    rainWeight = std::max<float>( 0.0f, std::min<float>( 1.0f, rainWeight ) );

    // max multiplayers when rain is 1.0 (max)
    constexpr float rainMaxStrengthMultiplier = 2.75f;
    constexpr float rainMaxSpeedMultiplier = 2.15f;

    // UI-normalized wind strength: 1.0 keeps the former effective strength of 2.0.
    vobAnimation_WindStrength = (1.0f + rainWeight * (rainMaxStrengthMultiplier - 1.0f))
        * settings.GlobalWindStrength
        * 2.0f;

    const float prevWindGlobalTime = WindGlobalTime;
    WindGlobalTime += dt * (1.5f * (1.0f + rainWeight * (rainMaxSpeedMultiplier - 1.0f)));
    windBuff.globalTime = WindGlobalTime;
    windBuff.prevGlobalTime = prevWindGlobalTime;
}

/** Draws the static vobs instanced */
XRESULT D3D11GraphicsEngine::DrawVOBsInstanced() {
    const auto& renderSettings = Engine::GAPI->GetRendererState().RendererSettings;
    auto& vobs = m_CollectedCameraVobs;
    auto& mobs = m_CollectedCameraMobs;

    {
        TracyD3D11ZoneCGX( "DrawVOBsInstanced" );
        auto _scopeDrawVOBsInstanced = RecordGraphicsEvent( GE_NAME( "DrawVOBsInstanced" ) );
        SetDefaultStates();

        SetActivePixelShader( PShaderID::PS_Diffuse );
        SetActiveVertexShader( VShaderID::VS_ExInstancedObj );

        // Set constant buffer
        ActivePS->GetBuffer( "FFPipelineConstantBuffer" )
            .Update( &Engine::GAPI->GetRendererState().GraphicsState )
            .Bind();

        if ( GSky* sky = Engine::GAPI->GetSky() ) {
            ActivePS->GetBuffer( "Atmosphere" )
                .Update( &sky->GetAtmosphereCB() )
                .Bind();
        }

        // Use default material info for now
        MaterialInfo defInfo = {};
        UINT materialInfoSlot = ActivePS->GetBuffer( "MI_MaterialInfo" ).GetSlot();
        auto defaultMaterialAllocation = PerObjectMaterialInfoPooledBuffer->Allocate( GetContext().Get(), &defInfo.buffer, sizeof( defInfo.buffer ) );
        UINT firstConstant = defaultMaterialAllocation.offsetInBytes / 16;
        UINT numConstants = defaultMaterialAllocation.sizeInBytes / 16;
        GetContext()->PSSetConstantBuffers1( materialInfoSlot, 1, &defaultMaterialAllocation.pBuffer, &firstConstant, &numConstants );

        XMMATRIX view = Engine::GAPI->GetViewMatrixXM();
        Engine::GAPI->SetViewTransformXM( view );

        if ( renderSettings.WireframeVobs ) {
            Engine::GAPI->GetRendererState().RasterizerState.Wireframe = true;
        }

        // Init drawcalls
        SetupVS_ExMeshDrawCall();
        SetupVS_ExConstantBuffer();

        GraphicsShaderConstantBuffer windBuffer = {};
        const auto& windSettings =
            Engine::GAPI->GetRendererState().RendererSettings;

        if ( ActiveVS
            && (windSettings.WindQuality
                    != GothicRendererSettings::EWindQuality::
                        WIND_QUALITY_NONE
                || windSettings.HeroAffectsObjects) ) {
            windBuffer = ActiveVS->GetBuffer( "WindParams" );
            windBuffer.Bind();
        }

        auto DIST_DistanceSlot = ActivePS->GetInputIndex( "DIST_Distance" );

        bool isZPrepass = RenderingStage == D3D11ENGINE_RENDER_STAGE::DES_Z_PRE_PASS;
        if ( !isZPrepass ) {
            m_GpuVobCurrentTriangleWeights.clear();
            m_GpuVobCurrentTriangleWeights.reserve(
                m_FrameGeometryCache.sortedInstancedMeshes.size() );
        }

        if ( !isZPrepass ) {
            m_AlphaMeshes.clear();
            m_WindowAlphaMeshes.clear();
            m_AlphaMeshes.reserve( 64 );
            m_WindowAlphaMeshes.reserve( 16 );
        }

        if ( isZPrepass ) {
            Context->PSSetShader( nullptr, nullptr, 0 );
        }

        if ( renderSettings.DrawVOBs ||
            renderSettings.EnableDynamicLighting ) {
            if ( !m_FrameGeometryCache.vobInstancesUploaded )
                EnsureFrameVobVisibilityCollected();
        }

        if ( renderSettings.DrawVOBs ) {
            auto _1 = Engine::GraphicsEngine->RecordGraphicsEvent( GE_NAME( "DrawVOBsInstanced->DrawVOBs" ) );
            TracyD3D11ZoneCGX( "DrawVOBsInstanced->VOBs" );

            auto& cache = m_FrameGeometryCache;
            D3D11VertexBuffer* instancingBuffer = cache.MainVobInstancingBuffer;

            if ( !cache.vobInstancesUploaded ) {
                ZoneScopedN( "DrawVOBsInstanced::BuildInstanceCache" );
                auto _scopeBuildInstanceCache = RecordGraphicsEvent( GE_NAME( "DrawVOBsInstanced::BuildInstanceCache" ) );
                // Build active visuals from Instances populated by CollectVisibleVobs above
                std::vector<MeshVisualInfo*> activeVisuals;
                activeVisuals.reserve( 256 );
                for ( auto const& pair : Engine::GAPI->GetStaticMeshVisuals() ) {
                    if ( !pair.second->Instances.empty() ) {
                        activeVisuals.push_back( pair.second );
                    }
                }

                if ( renderSettings.AnimateStaticVobs ) {
                    UpdateMorphMeshVisuals( activeVisuals );
                }

                cache.vobWindMetadataPrepared = PrepareAndBindWindMetadata( activeVisuals );
                cache.vobWindMetadata.clear();
                if ( cache.vobWindMetadataPrepared ) {
                    cache.vobWindMetadata = m_WindMetadataStaging;
                }

                // Snapshot visuals + instance data into cache so subsequent passes
                // don't rely on MeshVisualInfo::Instances, which may be mutated by shadow passes
                cache.vobVisuals.clear();
                cache.vobVisuals.reserve( activeVisuals.size() + 16 );
                {
                    UINT loc = 0;
                    auto appendCachedVisual = [&cache, &loc](
                        MeshVisualInfo* visual,
                        std::vector<VobInstanceInfo>&& instances,
                        bool windowFallbackOpaque ) {
                        if ( instances.empty() )
                            return;
                        FrameGeometryCache::CachedVobVisual cv;
                        cv.Visual = visual;
                        cv.Instances = std::move( instances );
                        cv.StartInstanceNum = loc;
                        cv.WindowFallbackOpaque = windowFallbackOpaque;
                        loc += static_cast<UINT>(cv.Instances.size());
                        cache.vobVisuals.push_back( std::move( cv ) );
                    };

                    for ( auto smv : activeVisuals ) {
                        const bool scopedWindowVisual = IsCityWindowFeatureReady()
                            && IsWindowGlassVisual( smv->VisualName );
                        if ( !scopedWindowVisual
                            || smv->InstanceVobs.size() != smv->Instances.size() ) {
                            appendCachedVisual( smv,
                                std::vector<VobInstanceInfo>( smv->Instances ), false );
                            continue;
                        }

                        std::vector<VobInstanceInfo> transparentInstances;
                        std::vector<VobInstanceInfo> fallbackInstances;
                        transparentInstances.reserve( smv->Instances.size() );
                        fallbackInstances.reserve( smv->Instances.size() );
                        for ( size_t instanceIndex = 0;
                            instanceIndex < smv->Instances.size(); ++instanceIndex ) {
                            const VobInfo* instanceVob = smv->InstanceVobs[instanceIndex];
                            const bool opaqueFallback = instanceVob
                                && instanceVob->CityWindowValidationInitialized
                                && !instanceVob->CityWindowTransparencyValid;
                            (opaqueFallback ? fallbackInstances : transparentInstances)
                                .push_back( smv->Instances[instanceIndex] );
                        }

                        appendCachedVisual( smv, std::move( transparentInstances ), false );
                        appendCachedVisual( smv, std::move( fallbackInstances ), true );
                    }
                }

                unsigned int totalInstances = 0;
                for ( const auto& cv : cache.vobVisuals ) {
                    totalInstances += static_cast<unsigned int>(cv.Instances.size());
                }

                const unsigned int requiredBytes = (totalInstances > 0 ? totalInstances : 1u)
                    * static_cast<unsigned int>(sizeof( VobInstanceInfo ));
                instancingBuffer = AcquireFrameInstancingBuffer( m_MainVobInstancingPool,
                    requiredBytes, "MainVobInstancingBuffer" );
                if ( !instancingBuffer ) {
                    LogError() << "Failed to acquire main vob instancing buffer.";
                    return XR_FAILED;
                }

                byte* data;
                UINT size;

                if ( SUCCEEDED( instancingBuffer->Map( D3D11VertexBuffer::M_WRITE_DISCARD,
                    reinterpret_cast<void**>(&data), &size ) ) ) {
                    for ( auto const& cv : cache.vobVisuals ) {
                        memcpy( data + cv.StartInstanceNum * sizeof( VobInstanceInfo ),
                            cv.Instances.data(),
                            sizeof( VobInstanceInfo ) * cv.Instances.size() );
                    }
                    instancingBuffer->Unmap();
                } else {
                    LogError() << "Failed to map dynamic instancing buffer for vobs.";
                }

                cache.MainVobInstancingBuffer = instancingBuffer;

                size_t numMeshesToDraw = 0;
                for ( auto const& cv : cache.vobVisuals ) {
                    for ( auto const& itt : cv.Visual->MeshesByTexture ) {
                        numMeshesToDraw += itt.second.size();
                    }
                }

                cache.sortedInstancedMeshes.clear();
                cache.sortedInstancedMeshes.reserve( numMeshesToDraw );

                for ( unsigned int visualIndex = 0; visualIndex < cache.vobVisuals.size(); ++visualIndex ) {
                    const auto& cv = cache.vobVisuals[visualIndex];
                    for ( auto const& itt : cv.Visual->MeshesByTexture ) {
                        const std::vector<MeshInfo*>& mlist = itt.second;
                        if ( mlist.empty() ) continue;

                        FrameGeometryCache::SortKeyBuilder sortKeyBase{ 0 };
                        if ( itt.first.Material ) {
                            const auto alphaFunc = itt.first.Material->GetAlphaFunc();
                            if ( alphaFunc > zMAT_ALPHA_FUNC_NONE && alphaFunc != zMAT_ALPHA_FUNC_TEST ) {
                                sortKeyBase.withAlphaType(2);
                            } else if ( alphaFunc == zMAT_ALPHA_FUNC_TEST ) {
                                sortKeyBase.withAlphaType(1);
                            }
                        }
                        if ( itt.first.Texture ) {
                            if ( itt.first.Texture->HasAlphaChannel() && sortKeyBase.GetAlphaType() == 0 ) {
                                sortKeyBase.withAlphaType(1);
                            }
                            sortKeyBase.withTexture(reinterpret_cast<size_t>(itt.first.Texture));
                        }

                        for ( MeshInfo* mi : mlist ) {
                            if ( !mi ) continue;

                            FrameGeometryCache::SortKeyBuilder meshSortKey = sortKeyBase; // copy current base key
                            meshSortKey.withMesh(mi->meshId);

                            FrameGeometryCache::CachedInstancedMeshDraw drawItem;
                            drawItem.VisualIndex = visualIndex;
                            drawItem.Mesh = itt.first;
                            drawItem.MeshEntry = mi;
                            drawItem.sortKey = meshSortKey;

                            cache.sortedInstancedMeshes.push_back( drawItem );
                        }
                    }
                }

                std::sort( cache.sortedInstancedMeshes.begin(), cache.sortedInstancedMeshes.end(),
                    []( const FrameGeometryCache::CachedInstancedMeshDraw& a, const FrameGeometryCache::CachedInstancedMeshDraw& b ) {
                        return a.sortKey < b.sortKey;
                    } );

                PrepareGpuVobGeometryArena();
                m_GpuVobPerformanceStats.CandidateVisuals += cache.vobVisuals.size();
                m_GpuVobPerformanceStats.CandidateDrawItems += cache.sortedInstancedMeshes.size();
                for ( const auto& visual : cache.vobVisuals ) {
                    m_GpuVobPerformanceStats.CandidateInstances += visual.Instances.size();
                }
                cache.vobGpuCullingPrepared = true;
                const bool gpuVobCullingRequested = renderSettings.GetEffectiveGpuVobOcclusionCulling()
                    && !IsGpuVobRuntimeDisabled();
                if ( gpuVobCullingRequested ) {
                    ++m_GpuVobPerformanceStats.MainCullAttempts;
                    cache.vobGpuCullingActive = PrepareGpuVobCulling();
                    if ( cache.vobGpuCullingActive ) {
                        ++m_GpuVobPerformanceStats.MainCullActive;
                    } else {
                        ++m_GpuVobPerformanceStats.MainCullFallback;
                    }
                } else {
                    cache.vobGpuCullingActive = false;
                }

                if ( !vobs.empty() ) {
                    RenderedVobs.insert( RenderedVobs.end(), vobs.begin(), vobs.end() );
                }

                cache.vobInstancesUploaded = true;
            } else {
                instancingBuffer = cache.MainVobInstancingBuffer;
            }

            if ( !instancingBuffer ) {
                LogError() << "Missing main vob instancing buffer in cache.";
                return XR_FAILED;
            }

            bool useWindMetadata = false;
            if ( cache.vobWindMetadataPrepared
                && ActiveVS
                && ActiveVS->GetInputIndex( "WindMetaData" ) != -1
                && !cache.vobWindMetadata.empty() ) {
                const UINT requiredSize = static_cast<UINT>(cache.vobWindMetadata.size() * sizeof( VobWindMetadata ));

                if ( !WindMetadataBuffer || WindMetadataBuffer->GetSizeInBytes() < requiredSize ) {
                    WindMetadataBuffer = std::make_unique<D3D11VertexBuffer>();
                    if ( XR_SUCCESS != WindMetadataBuffer->Init(
                        nullptr,
                        requiredSize,
                        D3D11VertexBuffer::B_SHADER_RESOURCE,
                        D3D11VertexBuffer::U_DEFAULT,
                        D3D11VertexBuffer::CA_NONE,
                        "WindMetadataBuffer",
                        sizeof( VobWindMetadata ) ) ) {
                        WindMetadataBuffer.reset();
                    } else {
                        SetDebugName( WindMetadataBuffer->GetShaderResourceView().Get(), "WindMetadataBuffer->ShaderResourceView" );
                        SetDebugName( WindMetadataBuffer->GetVertexBuffer().Get(), "WindMetadataBuffer->Buffer" );
                    }
                }

                if ( WindMetadataBuffer
                    && XR_SUCCESS == WindMetadataBuffer->UpdateBufferSubresource( cache.vobWindMetadata.data(), requiredSize )
                    && WindMetadataBuffer->GetShaderResourceView().Get() ) {
                    ActiveVS->BindResource( "WindMetaData", WindMetadataBuffer->GetShaderResourceView().Get() );
                    useWindMetadata = true;
                } else {
                    UnbindWindMetadata();
                }
            } else {
                UnbindWindMetadata();
            }

            if ( windBuffer.GetRawBuffer() ) {
                UpdateCharacterInteractionPositions( g_windBuffer );
                windBuffer.Update( &g_windBuffer );
            }

            float cachedSmallVobRadius = -1.0f;
            float cachedVobRadius = -1.0f;
            // Ensure we have correct Constantbuffer for eventual Alphatest stuff.
            ShaderManager->GetPShader( Resolved_DiffuseNormalmappedAlphatest )
                ->GetBuffer( "FFPipelineConstantBuffer" )
                .Update( &Engine::GAPI->GetRendererState().GraphicsState )
                .Bind();

            if ( isZPrepass ) {
                // force alpha testing for vobs in prepass.
                SetActivePixelShader( PShaderID::PS_DiffuseAlphaTestShadows );
                Context->PSSetShader( nullptr, nullptr, 0 );
            }

            MaterialInfo* lastMatInfo = nullptr;
            float lastMaterialClassMarker = 999.0f;
            bool lastOilLampEmissiveTexture = false;
            bool lastWindowGlassSplit = false;

            zCTexture* lastTex = nullptr;
            ID3D11ShaderResourceView* lastDiffuseTex = nullptr;
            ID3D11ShaderResourceView* lastNrmTex = nullptr;
            ID3D11ShaderResourceView* lastFxTex = nullptr;
            ID3D11ShaderResourceView* lastDispTex = nullptr;
            MeshVisualInfo* lastWindVisual = nullptr;

            if ( !cache.sortedInstancedMeshes.empty() ) {
                TracyD3D11ZoneCGX( "DrawVOBsInstanced::OpaqueSubmission" );
                auto _scopeOpaqueSubmission = RecordGraphicsEvent( GE_NAME( "DrawVOBsInstanced::OpaqueSubmission" ) );
                BeginGpuTimingZone( isZPrepass
                    ? EGpuTimingZone::VobDepth : EGpuTimingZone::VobLit );
                std::vector<bool> mdiConsumed( cache.sortedInstancedMeshes.size(), false );
                for ( size_t drawIndex = 0;
                    drawIndex < cache.sortedInstancedMeshes.size(); ++drawIndex ) {
                    if ( mdiConsumed[drawIndex] ) {
                        continue;
                    }
                    const auto& drawItem = cache.sortedInstancedMeshes[drawIndex];
                    if ( drawItem.VisualIndex >= cache.vobVisuals.size() || !drawItem.MeshEntry ) {
                        continue;
                    }

                    const auto* cachedVisual = &cache.vobVisuals[drawItem.VisualIndex];
                    const MeshKey& meshKey = drawItem.Mesh;
                    MeshInfo* meshInfo = drawItem.MeshEntry;
                    const float materialClassMarker = IsTwoSidedBacklitVegetationVisual(
                        cachedVisual->Visual->VisualName ) ? -2.0f : WATER_VOB_MATERIAL_CLASS_MARKER;
                    zCTexture* tx = meshKey.Material ? meshKey.Material->GetAniTexture() : nullptr;
                    const bool isScopedWindowMaterial = IsCityWindowFeatureReady()
                        && IsWindowGlassMaterial( cachedVisual->Visual, tx );
                    const bool isWindowFallbackMaterial = cachedVisual->WindowFallbackOpaque
                        && isScopedWindowMaterial;
                    const bool isWindowGlass = !cachedVisual->WindowFallbackOpaque
                        && isScopedWindowMaterial;
                    const bool isAlphaBlendMesh = meshKey.Material &&
                        (!isWindowFallbackMaterial && (isWindowGlass ||
                         meshKey.Material->GetAlphaFunc() == zMAT_ALPHA_FUNC_BLEND ||
                         meshKey.Material->GetAlphaFunc() == zMAT_ALPHA_FUNC_ADD));

                    if ( isAlphaBlendMesh ) {
                        if ( !isZPrepass ) {
                            AlphaMeshData alphaMesh;
                            alphaMesh.mk = meshKey;
                            alphaMesh.mi = meshInfo;
                            alphaMesh.vi = cachedVisual->Visual;
                            alphaMesh.StartInstanceNum = cachedVisual->StartInstanceNum;
                            alphaMesh.instances = cachedVisual->Instances;
                            (isWindowGlass ? m_WindowAlphaMeshes : m_AlphaMeshes)
                                .push_back( std::move( alphaMesh ) );
                        }
                        // This material combines solid lattice/frame texels with
                        // fractional-alpha glass. Keep the solid part in the normal
                        // lit VOB path; only the glass is replayed transparently.
                        if ( !isWindowGlass ) {
                            continue;
                        }
                    }

                    float expectedSmallRadius = renderSettings.OutdoorSmallVobDrawRadius - cachedVisual->Visual->MeshSize;
                    float expectedVobRadius = renderSettings.OutdoorVobDrawRadius - cachedVisual->Visual->MeshSize;

                    if ( DIST_DistanceSlot != -1 ) {
                        if ( cachedVisual->Visual->MeshSize < renderSettings.SmallVobSize ) {
                            // Only update if it changed
                            if ( std::abs( cachedSmallVobRadius - expectedSmallRadius ) > 0.1f ) {
                                OutdoorSmallVobsConstantBuffer->UpdateBuffer( float4( expectedSmallRadius, 0, 0, 0 ).toPtr() );
                                OutdoorSmallVobsConstantBuffer->BindToPixelShader( DIST_DistanceSlot );
                                cachedSmallVobRadius = expectedSmallRadius;
                            }
                        } else {
                            // Only update if it changed
                            if ( std::abs( cachedVobRadius - expectedVobRadius ) > 0.1f ) {
                                OutdoorVobsConstantBuffer->UpdateBuffer( float4( expectedVobRadius, 0, 0, 0 ).toPtr() );
                                OutdoorVobsConstantBuffer->BindToPixelShader( DIST_DistanceSlot );
                                cachedVobRadius = expectedVobRadius;
                            }
                        }
                    }

                    if ( !useWindMetadata && windBuffer.GetRawBuffer() && lastWindVisual != cachedVisual->Visual ) {
                        lastWindVisual = cachedVisual->Visual;
                        g_windBuffer.minHeight = cachedVisual->Visual->BBox.Min.y;
                        g_windBuffer.maxHeight = cachedVisual->Visual->BBox.Max.y;
                        windBuffer.Update( &g_windBuffer );
                    }

                    const bool oilLampEmissiveTexture = IsCityWindowFeatureReady()
                        && IsOilLampEmissiveTexture( tx );

                    if ( !tx ) {
#ifndef BUILD_SPACER_NET
#ifndef BUILD_SPACER
                        continue;  // Don't render meshes without texture if not in spacer
#else
                        // This is most likely some spacer helper-vob
                        WhiteTexture->BindToPixelShader( 0 );
                        ShaderManager->GetPShader( PShaderID::PS_Diffuse )->Apply();

                        /*// Apply colors for these meshes
                        MaterialInfo::Buffer b;
                        ZeroMemory(&b, sizeof(b));
                        b.Color = itt->first.Material->GetColor();
                        PS_Diffuse->GetConstantBuffer()[2]->UpdateBuffer(&b);
                        PS_Diffuse->GetConstantBuffer()[2]->BindToPixelShader(2);*/
#endif
#else
                        if ( !renderSettings.RunInSpacerNet ) {
                            continue;
                        }
                        bool showHelpers = *reinterpret_cast<int*>(GothicMemoryLocations::zCVob::s_ShowHelperVisuals) != 0;

                        if ( showHelpers ) {
                            WhiteTexture->BindToPixelShader( 0 );
                            ShaderManager->GetPShader( PShaderID::PS_DiffuseAlphaTest )->Apply();

                            MaterialInfo::Buffer b = {};

                            b.Color = meshKey.Material->GetColor();
                            ShaderManager->GetPShader( PShaderID::PS_DiffuseAlphaTest )->GetBuffer( "MI_MaterialInfo" ).Update( &b ).Bind();

                        } else {
                            continue;
                        }

#endif
                    }
                    else {
                        // Bind texture
                        if ( tx->CacheIn( 0.6f ) == zRES_CACHED_OUT ) {
                            continue;
                        }
                        // Enable alpha testing when required by the material.
                        const bool wantShader = !isZPrepass || isWindowGlass
                            || tx->HasAlphaChannel() || meshKey.Material->HasAlphaTest();

                        MyDirectDrawSurface7* surface = tx->GetSurface();
                        ID3D11ShaderResourceView* srv[4];
                        MaterialInfo* info = meshKey.Info;

                        // Get diffuse and normalmap
                        srv[0] = isWindowGlass ? GetWindowGlassReplacementSRV() : nullptr;
                        if ( !srv[0] ) {
                            srv[0] = surface->GetEngineTexture()->GetShaderResourceView().Get();
                        }
                        srv[1] = GetMaterialNormalmapSRV( surface, info );
                        srv[2] = surface->GetFxMap()
                            ? surface->GetFxMap()->GetShaderResourceView().Get()
                            : nullptr;
                        srv[3] = GetParallaxDisplacementSRV( surface, info );

                        const bool materialMarkerChanged = materialClassMarker != lastMaterialClassMarker;
                        const bool oilLampMarkerChanged = oilLampEmissiveTexture != lastOilLampEmissiveTexture;
                        const bool windowGlassMarkerChanged = isWindowGlass != lastWindowGlassSplit;
                        // Material data decides whether a real normalmap participates; no wet distortion fallback is used.
                        if ( lastTex != tx
                            || lastDiffuseTex != srv[0]
                            || lastNrmTex != srv[1]
                            || lastFxTex != srv[2]
                            || lastDispTex != srv[3]
                            || materialMarkerChanged
                            || oilLampMarkerChanged
                            || windowGlassMarkerChanged ) {

                            lastTex = tx;
                            lastDiffuseTex = srv[0];
                            lastNrmTex = srv[1];
                            lastFxTex = srv[2];
                            lastDispTex = srv[3];
                            lastMaterialClassMarker = materialClassMarker;
                            lastOilLampEmissiveTexture = oilLampEmissiveTexture;
                            lastWindowGlassSplit = isWindowGlass;

                            if ( wantShader ) {
                                GetContext()->PSSetShaderResources( 0, isZPrepass ? 1 : 3, srv );
                                if ( !isZPrepass ) {
                                    GetContext()->PSSetShaderResources( 13, 1, &srv[3] );
                                }

                                const int effectiveAlphaFunc = isWindowFallbackMaterial
                                    ? zMAT_ALPHA_FUNC_NONE
                                    : meshKey.Material->GetAlphaFunc();
                                if ( BindShaderForTexture( tx,
                                    isWindowGlass || tx->HasAlphaChannel()
                                    || meshKey.Material->HasAlphaTest()
                                    , effectiveAlphaFunc,
                                    meshKey.Info->MaterialType ) ) {

                                    PsSimpleFFdata ffdata = { };
                                    ffdata.textureFactor = float4( 1.0f, 1.0f, 1.0f, 1.0f );
                                    ActivePS->GetBuffer( "cbFFData" )
                                        .Update( &ffdata )
                                        .Bind();
                                }

                                if ( materialMarkerChanged || oilLampMarkerChanged || windowGlassMarkerChanged
                                    || (info && !info->IsSame( lastMatInfo )) ) {
                                    auto materialBuffer = GetEffectiveMaterialBuffer(
                                        info, tx->GetSurface(), oilLampEmissiveTexture,
                                        isWindowGlass );
                                    if ( materialClassMarker != 0.0f ) {
                                        materialBuffer.Color.w = materialClassMarker;
                                    }
                                    auto matAllocation = PerObjectMaterialInfoPooledBuffer->Allocate( GetContext().Get(), &materialBuffer, sizeof( materialBuffer ) );
                                    UINT firstConstant = matAllocation.offsetInBytes / 16;
                                    UINT numConstants = matAllocation.sizeInBytes / 16;
                                    GetContext()->PSSetConstantBuffers1( materialInfoSlot, 1, &matAllocation.pBuffer, &firstConstant, &numConstants );

                                    lastMatInfo = info;
                                }
                            }
                        }
                    }

                    const auto geometryRange = GpuVobGeometryRanges.find( meshInfo );
                    const bool useGeometryArena = cache.MainVobGpuGeometryVertexBuffer
                        && cache.MainVobGpuGeometryIndexBuffer
                        && geometryRange != GpuVobGeometryRanges.end();
                    size_t mdiDrawCount = 1;
                    // MDI is only used for adjacent opaque draws that share
                    // the complete material/visual state. This keeps wind,
                    // window classification and per-material constants
                    // identical for every command in the batch.
                    const bool mdiEligible = renderSettings.GetEffectiveGpuVobOcclusionCulling()
                        && cache.vobGpuCullingActive
                        && renderSettings.DebugSettings.FeatureSet.UseMDI
                        && cache.MainVobGpuInstanceBuffer
                        && cache.MainVobGpuIndirectArgsBuffer
                        && useGeometryArena && tx
                        && !isAlphaBlendMesh
                        && ((drawItem.sortKey >> 62) & 0x3u) == 0;
                    if ( mdiEligible ) {
                        while ( drawIndex + mdiDrawCount < cache.sortedInstancedMeshes.size() ) {
                            const auto& nextItem = cache.sortedInstancedMeshes[drawIndex + mdiDrawCount];
                            if ( nextItem.VisualIndex != drawItem.VisualIndex
                                || !nextItem.MeshEntry
                                || nextItem.Mesh.Material != meshKey.Material
                                || nextItem.Mesh.Texture != meshKey.Texture
                                || nextItem.Mesh.Info != meshKey.Info
                                || ((nextItem.sortKey >> 62) & 0x3u) != 0
                                || nextItem.Mesh.Material == nullptr
                                || nextItem.Mesh.Material->GetAlphaFunc() == zMAT_ALPHA_FUNC_BLEND
                                || nextItem.Mesh.Material->GetAlphaFunc() == zMAT_ALPHA_FUNC_ADD
                                || (IsCityWindowFeatureReady() && IsWindowGlassMaterial( cachedVisual->Visual,
                                    nextItem.Mesh.Material->GetAniTexture() ) ) ) {
                                break;
                            }
                            const auto nextRange = GpuVobGeometryRanges.find( nextItem.MeshEntry );
                            if ( nextRange == GpuVobGeometryRanges.end() ) {
                                break;
                            }
                            mdiConsumed[drawIndex + mdiDrawCount] = true;
                            ++mdiDrawCount;
                        }
                    }

                        // The GPU path keeps the existing per-material state setup,
                        // but lets the compute shader decide InstanceCount and
                        // StartInstanceLocation for this mesh's indirect command.
                    if ( cache.vobGpuCullingActive
                        && cache.MainVobGpuInstanceBuffer
                        && cache.MainVobGpuIndirectArgsBuffer ) {
                        UINT submittedTrianglesPerInstance = 0;
                        for ( size_t batchIndex = 0; batchIndex < mdiDrawCount; ++batchIndex ) {
                            const auto& submittedItem = cache.sortedInstancedMeshes[
                                drawIndex + batchIndex];
                            const UINT submittedIndices = renderSettings.MaxNumFaces > 0
                                ? std::min<UINT>(
                                    static_cast<UINT>( submittedItem.MeshEntry->Indices.size() ),
                                    static_cast<UINT>( renderSettings.MaxNumFaces ) * 3u )
                                : static_cast<UINT>( submittedItem.MeshEntry->Indices.size() );
                            submittedTrianglesPerInstance += submittedIndices / 3;
                            m_GpuVobCurrentTriangleWeights.push_back({
                                submittedItem.VisualIndex, submittedIndices / 3 });
                        }

                        ++m_GpuVobPerformanceStats.IndirectDrawCalls;
                        UINT offsets[2] = { 0, 0 };
                        UINT strides[2] = {
                            static_cast<UINT>( sizeof( ExVertexStruct ) ),
                            static_cast<UINT>( sizeof( VobInstanceInfo ) )
                        };
                        ID3D11Buffer* vertexBuffers[2] = {
                            (useGeometryArena
                                ? cache.MainVobGpuGeometryVertexBuffer->GetVertexBuffer().Get()
                                : meshInfo->MeshVertexBuffer->GetVertexBuffer().Get()),
                            cache.MainVobGpuInstanceBuffer->GetVertexBuffer().Get()
                        };
                        GetContext()->IASetVertexBuffers( 0, 2,
                            vertexBuffers, strides, offsets );
                        GetContext()->IASetIndexBuffer(
                            (useGeometryArena
                                ? cache.MainVobGpuGeometryIndexBuffer->GetVertexBuffer().Get()
                                : meshInfo->MeshIndexBuffer->GetVertexBuffer().Get()),
                            VERTEX_INDEX_DXGI_FORMAT, 0 );
                        ID3D11Buffer* argsBuffer =
                            cache.MainVobGpuIndirectArgsBuffer->GetIndirectBuffer().Get();
                        if ( mdiDrawCount > 1 ) {
                            auto drawMulti = Engine::GAPI->GetRendererState().RendererSettings.DebugSettings.FeatureSet.UseMDI
                                ? DrawMultiIndexedInstancedIndirect
                                : Stub_DrawMultiIndexedInstancedIndirect;
                            if ( Engine::GAPI->GetRendererState().RendererSettings.DebugSettings.FeatureSet.UseMDI ) {
                                ++m_GpuVobPerformanceStats.MdiBatches;
                                m_GpuVobPerformanceStats.MdiCommands += mdiDrawCount;
                            }
                            drawMulti( GetContext().Get(), static_cast<UINT>( mdiDrawCount ),
                                argsBuffer,
                                static_cast<UINT>( drawIndex * sizeof( D3D11_DRAW_INDEXED_INSTANCED_INDIRECT_ARGS ) ),
                                sizeof( D3D11_DRAW_INDEXED_INSTANCED_INDIRECT_ARGS ) );
                        } else {
                            GetContext()->DrawIndexedInstancedIndirect( argsBuffer,
                                static_cast<UINT>( drawIndex
                                    * sizeof( D3D11_DRAW_INDEXED_INSTANCED_INDIRECT_ARGS ) ) );
                        }

                        const uint64_t submittedMainVobTriangles =
                            static_cast<uint64_t>( submittedTrianglesPerInstance )
                            * cachedVisual->Instances.size();
                        Engine::GAPI->GetRendererState().RendererInfo.FrameDrawnTriangles +=
                            static_cast<int>( std::min<uint64_t>( submittedMainVobTriangles,
                                static_cast<uint64_t>( std::numeric_limits<int>::max() ) ) );
                        m_GpuVobFrameMainCandidateTriangles += submittedMainVobTriangles;
                        Engine::GAPI->GetRendererState().RendererInfo.FrameDrawnVobs++;
                    } else {
                        ++m_GpuVobPerformanceStats.CpuFallbackDrawCalls;
                        DrawInstanced( meshInfo->MeshVertexBuffer, meshInfo->MeshIndexBuffer,
                            meshInfo->Indices.size(), instancingBuffer,
                            sizeof( VobInstanceInfo ), cachedVisual->Instances.size(),
                            sizeof( ExVertexStruct ), cachedVisual->StartInstanceNum );
                    }
                }
                EndGpuTimingZone( isZPrepass
                    ? EGpuTimingZone::VobDepth : EGpuTimingZone::VobLit );
            }
            if ( !isZPrepass ) {
                for ( auto const& cv : cache.vobVisuals ) {
                    bool clear = true;
                    for ( auto& [meshKey, _] : cv.Visual->MeshesByTexture ) {
                        if ( meshKey.Material &&
                            (meshKey.Material->GetAlphaFunc() == zMAT_ALPHA_FUNC_BLEND ||
                                meshKey.Material->GetAlphaFunc() == zMAT_ALPHA_FUNC_ADD) ) {
                            clear = false;
                            break;
                        }
                    }
                    if ( clear ) {
                        cv.Visual->StartNewFrame();
                    }
                }
            }
            if ( useWindMetadata ) {
                UnbindWindMetadata();
            }
            if ( !isZPrepass ) {
                ScheduleGpuVobVisibleCountReadback( cache );
            }
        }

        // Draw mobs
        if ( renderSettings.DrawMobs ) {
            TracyD3D11ZoneCGX( "DrawVOBsInstanced->MOBs" );
            auto _1 = Engine::GraphicsEngine->RecordGraphicsEvent( GE_NAME( "DrawVOBsInstanced->DrawMobs" ) );

            // Reset Gothic's texture state before drawing mobs.
            Engine::GAPI->ResetRenderStates();

            static std::vector<XMFLOAT4X4> bones = {};

            DrawSkeletalMeshVobs( m_FrameGeometryCache.cachedMobs, FLT_MAX, true, true );
            for ( SkeletalVobInfo* mob : m_FrameGeometryCache.cachedMobs ) {
                zCModel* model = static_cast<zCModel*>(mob->Vob->GetVisual());
                XMMATRIX scale = XMMatrixScalingFromVector( model->GetModelScaleXM() );
                XMMATRIX world = mob->Vob->GetWorldMatrixXM() * scale;
                XMStoreFloat4x4( &mob->PrevWorldMatrix, world );

                model->GetBoneTransforms( &bones );
                mob->StorePreviousTransforms( bones );
                bones.clear();
            }
        }

        GetContext()->IASetPrimitiveTopology( D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST );

        if ( renderSettings.WireframeVobs ) {
            Engine::GAPI->GetRendererState().RasterizerState.Wireframe = false;
        }
    }

    if ( !renderSettings.FixViewFrustum ) {
        vobs.clear();
        mobs.clear();
    }
    return XR_SUCCESS;
}

/** Draws the static VOBs */
XRESULT D3D11GraphicsEngine::DrawFrameAlphaMeshes() {
    return DrawAlphaMeshList( m_AlphaMeshes, false );
}

/** Draws window glass after all scene-space effects that may lie behind it. */
XRESULT D3D11GraphicsEngine::DrawWindowAlphaMeshes() {
    return DrawAlphaMeshList( m_WindowAlphaMeshes, true );
}

XRESULT D3D11GraphicsEngine::DrawAlphaMeshList(
    std::vector<AlphaMeshData>& alphaMeshes, bool windowGlassOnly ) {
    if ( alphaMeshes.empty() ) {
        return XR_SUCCESS;
    }

    TracyD3D11ZoneCGX("DrawFrameAlphaMeshes");
    auto _scopeDrawFrameAlphaMeshes = RecordGraphicsEvent( GE_NAME( "DrawFrameAlphaMeshes" ) );

    // Make sure lighting doesn't mess up our state
    SetDefaultStates();

    SetActivePixelShader( PShaderID::PS_Simple );
    SetActiveVertexShader( VShaderID::VS_ExInstancedObj );

    SetupVS_ExMeshDrawCall();
    SetupVS_ExConstantBuffer();

    // This pass already contains only validated, visible City_Window glass.
    // A VOB's origin can be far away from its visible pane, so it must not be
    // used as a second feature gate.
    const bool windowSkyGuardEnabled = windowGlassOnly
        && DepthStencilBuffer && DepthStencilBufferCopy;
    if ( windowSkyGuardEnabled ) {
        // The regular end-of-frame depth copy happens after the final window
        // replay. Refresh it here while the current depth buffer is detached,
        // otherwise the window would classify sky from the previous frame when
        // the camera moved.
        GetContext()->OMSetRenderTargets(
            1, HDRBackBuffer->GetRenderTargetView().GetAddressOf(), nullptr );
        CopyDepthStencil();
    }

    GetContext()->OMSetRenderTargets( 1, HDRBackBuffer->GetRenderTargetView().GetAddressOf(),
        DepthStencilBuffer->GetDepthStencilView().Get() );

    bool useWindMetadata = false;
    if ( ActiveVS && ActiveVS->GetInputIndex( "WindMetaData" ) != -1 && !alphaMeshes.empty() ) {
        m_WindMetadataStaging.clear();
        size_t alphaInstanceCount = 0;
        for ( const auto& alphaData : alphaMeshes ) alphaInstanceCount += alphaData.instances.size();
        m_WindMetadataStaging.reserve( alphaInstanceCount );

        for ( auto& alphaData : alphaMeshes ) {
            MeshVisualInfo* visual = alphaData.vi;
            if ( !visual ) {
                continue;
            }

            for ( auto& instance : alphaData.instances ) {
                VobWindMetadata metadata = {};
                if ( instance.GP_Slot < m_FrameGeometryCache.vobWindMetadata.size() ) {
                    metadata = m_FrameGeometryCache.vobWindMetadata[instance.GP_Slot];
                } else {
                    metadata.MinHeight = visual->BBox.Min.y;
                    metadata.MaxHeight = visual->BBox.Max.y;
                    metadata.HorizontalExtent = float2(
                        std::max( (visual->BBox.Max.x - visual->BBox.Min.x) * 0.5f, 0.001f ),
                        std::max( (visual->BBox.Max.z - visual->BBox.Min.z) * 0.5f, 0.001f ) );
                    metadata.GrassShear = IsGrassWindVisual( visual->VisualName ) ? 1.0f : 0.0f;
                }
                instance.GP_Slot = static_cast<DWORD>(m_WindMetadataStaging.size());
                m_WindMetadataStaging.push_back( metadata );
            }
        }

        if ( !m_WindMetadataStaging.empty() ) {
            const UINT requiredSize = static_cast<UINT>(m_WindMetadataStaging.size() * sizeof( VobWindMetadata ));

            if ( !WindMetadataBuffer || WindMetadataBuffer->GetSizeInBytes() < requiredSize ) {
                WindMetadataBuffer = std::make_unique<D3D11VertexBuffer>();
                    if ( XR_SUCCESS == WindMetadataBuffer->Init(
                        nullptr,
                        requiredSize,
                        D3D11VertexBuffer::B_SHADER_RESOURCE,
                        D3D11VertexBuffer::U_DEFAULT,
                        D3D11VertexBuffer::CA_NONE,
                    "WindMetadataBuffer",
                    sizeof( VobWindMetadata ) ) ) {
                    SetDebugName( WindMetadataBuffer->GetShaderResourceView().Get(), "WindMetadataBuffer->ShaderResourceView" );
                    SetDebugName( WindMetadataBuffer->GetVertexBuffer().Get(), "WindMetadataBuffer->Buffer" );
                } else {
                    WindMetadataBuffer.reset();
                }
            }

            if ( WindMetadataBuffer
                && XR_SUCCESS == WindMetadataBuffer->UpdateBufferSubresource( m_WindMetadataStaging.data(), requiredSize ) ) {
                ActiveVS->BindResource( "WindMetaData", WindMetadataBuffer->GetShaderResourceView().Get() );
                useWindMetadata = true;
            }
        }
    }


    D3D11VertexBuffer* instancingBuffer = m_FrameGeometryCache.MainVobInstancingBuffer;
    if ( !instancingBuffer ) {
        LogError() << "Missing main vob instancing buffer for alpha mesh rendering.";
        return XR_FAILED;
    }

    GraphicsShaderConstantBuffer windBuffer = {};
    if ( ActiveVS && (Engine::GAPI->GetRendererState().RendererSettings.WindQuality > 0 || Engine::GAPI->GetRendererState().RendererSettings.HeroAffectsObjects) ) {
        windBuffer = ActiveVS->GetBuffer( "WindParams" );
        windBuffer.Bind();
        UpdateCharacterInteractionPositions( g_windBuffer );
        windBuffer.Update( &g_windBuffer );
    }

    {
        TracyD3D11ZoneCGX( "DrawFrameAlphaMeshes::Replay" );
        auto _scopeAlphaReplay = RecordGraphicsEvent( GE_NAME( "DrawFrameAlphaMeshes::Replay" ) );
        PShaderID activeAlphaShader = PShaderID::PS_Simple;
        // The two lists are disjoint. Avoid all window resource/state work in
        // the ordinary alpha pass, which is normally the much larger list.
        const XMFLOAT3 windowGlassTint = windowGlassOnly
            ? GetWindowGlassTint() : XMFLOAT3( 1.0f, 1.0f, 1.0f );
        ID3D11ShaderResourceView* windowSceneDepthSRV = windowGlassOnly && DepthStencilBufferCopy
            ? DepthStencilBufferCopy->GetShaderResView().Get()
            : nullptr;
        const bool windowSkyGuardAvailable = windowGlassOnly
            && windowSkyGuardEnabled
            && IsCityWindowFeatureReady() && windowSceneDepthSRV
            && Engine::GAPI->GetRendererState().RendererSettings.DrawWorldMesh;
        bool windowSkyGuardBound = false;
        bool twoSidedWindowGlass = false;
        enum class ReplayBlendMode { None, Alpha, Additive };
        ReplayBlendMode activeBlendMode = ReplayBlendMode::None;
        for ( auto const& alphaMesh : alphaMeshes ) {
            const MeshKey& mk = alphaMesh.mk;
            zCTexture* tx = mk.Material->GetAniTexture();
            if ( !tx ) continue;

            // Check for alphablending on world mesh
            bool blendAdd = mk.Material->GetAlphaFunc() == zMAT_ALPHA_FUNC_ADD;
            const bool isWindowGlass = windowGlassOnly;

            const PShaderID wantedAlphaShader = isWindowGlass
                ? PShaderID::PS_Simple_FF
                : PShaderID::PS_Simple;
            if ( wantedAlphaShader != activeAlphaShader ) {
                if ( windowSkyGuardBound && !isWindowGlass ) {
                    ID3D11ShaderResourceView* nullSRVs[3] = {};
                    GetContext()->PSSetShaderResources( 14, 3, nullSRVs );
                    windowSkyGuardBound = false;
                }

                SetActivePixelShader( wantedAlphaShader );
                ActivePS->Apply();
                activeAlphaShader = wantedAlphaShader;

            }

            // PS_Simple is already active when this replay begins, so window
            // state must be bound per window draw, not only on a shader change.
            if ( isWindowGlass ) {
                if ( windowSkyGuardAvailable && !windowSkyGuardBound ) {
                    GetContext()->PSSetShaderResources(
                        14, 1, &windowSceneDepthSRV );
                    windowSkyGuardBound = true;
                }

                PsSimpleFFdata windowGlassData = {};
                windowGlassData.textureFactor = float4(
                    windowGlassTint.x,
                    windowGlassTint.y,
                    windowGlassTint.z,
                    -1.0f );
                windowGlassData.windowSkyParams = float2(
                    static_cast<float>(GetResolution().y) * (2.0f / 3.0f),
                    windowSkyGuardAvailable ? 1.0f : 0.0f );
                ActivePS->GetBuffer( "cbFFData" )
                    .Update( &windowGlassData )
                    .Bind();
            }

            // Bind texture
            MeshInfo* mi = alphaMesh.mi;
            MeshVisualInfo* vi = alphaMesh.vi;
            auto& instances = alphaMesh.instances;

            if ( tx->CacheIn( 0.6f ) != zRES_CACHED_IN ) {
                continue;
            }

            MyDirectDrawSurface7* surface = tx->GetSurface();
            ID3D11ShaderResourceView* srv[4];

            // Get diffuse and normalmap
            srv[0] = isWindowGlass ? GetWindowGlassReplacementSRV() : nullptr;
            if ( !srv[0] ) {
                srv[0] = surface->GetEngineTexture()->GetShaderResourceView().Get();
            }
            srv[1] = GetMaterialNormalmapSRV( surface, mk.Info );
            srv[2] = surface->GetFxMap()
                ? surface->GetFxMap()->GetShaderResourceView().Get()
                : nullptr;
            srv[3] = GetParallaxDisplacementSRV( surface, mk.Info );

            // Bind diffuse/normal/fx like 026; POM displacement uses t13.
            GetContext()->PSSetShaderResources( 0, 3, srv );
            GetContext()->PSSetShaderResources( 13, 1, &srv[3] );

            bool renderStateChanged = false;
            if ( twoSidedWindowGlass != isWindowGlass ) {
                // VOB-level front/back selection already removed an unwanted
                // window instance. Disable triangle backface culling only for
                // the surviving glass replay, because Gothic's IN meshes can
                // use the opposite winding for their glass plane.
                Engine::GAPI->GetRendererState().RasterizerState.CullMode = isWindowGlass
                    ? GothicRasterizerStateInfo::CM_CULL_NONE
                    : GothicRasterizerStateInfo::CM_CULL_BACK;
                Engine::GAPI->GetRendererState().RasterizerState.SetDirty();
                twoSidedWindowGlass = isWindowGlass;
                renderStateChanged = true;
            }

            const ReplayBlendMode wantedBlendMode = blendAdd && !isWindowGlass
                ? ReplayBlendMode::Additive
                : ReplayBlendMode::Alpha;
            if ( activeBlendMode != wantedBlendMode ) {
                if ( wantedBlendMode == ReplayBlendMode::Additive )
                    Engine::GAPI->GetRendererState().BlendState.SetAdditiveBlending();
                else
                    Engine::GAPI->GetRendererState().BlendState.SetAlphaBlending();
                Engine::GAPI->GetRendererState().BlendState.SetDirty();
                Engine::GAPI->GetRendererState().DepthState.DepthWriteEnabled = false;
                Engine::GAPI->GetRendererState().DepthState.SetDirty();
                activeBlendMode = wantedBlendMode;
                renderStateChanged = true;
            }

            if ( renderStateChanged )
                UpdateRenderStates();

            // TODO: apply MaterialInfoBuffer.Update(&mk.Info->buffer) ?

            g_windBuffer.minHeight = vi->BBox.Min.y;
            g_windBuffer.maxHeight = vi->BBox.Max.y;

            if ( !useWindMetadata && windBuffer.GetRawBuffer() ) {
                windBuffer.Update( &g_windBuffer );
            }

            // Draw batch
            DrawInstanced( mi->MeshVertexBuffer, mi->MeshIndexBuffer, mi->Indices.size(),
                instancingBuffer, sizeof( VobInstanceInfo ),
                instances.size(), sizeof( ExVertexStruct ),
                alphaMesh.StartInstanceNum );

            // Reset visual
            vi->StartNewFrame();
        }

        if ( windowSkyGuardBound ) {
            ID3D11ShaderResourceView* nullSRV = nullptr;
            GetContext()->PSSetShaderResources( 14, 1, &nullSRV );
        }
    }

    if ( useWindMetadata ) {
        UnbindWindMetadata();
    }

    alphaMeshes.clear();

    return XR_SUCCESS;
}

XRESULT D3D11GraphicsEngine::DrawVOBs( bool noTextures ) {
    return DrawVOBsInstanced();
}

XRESULT D3D11GraphicsEngine::DrawPolyStrips( bool noTextures ) {
    //DrawMeshInfoListAlphablended was mostly used as an example to write everything below
    const std::map<zCTexture*, PolyStripInfo>& polyStripInfos = Engine::GAPI->GetPolyStripInfos();

    // No need to do a bunch of work for nothing!
    if ( polyStripInfos.empty() ) {
        return XR_SUCCESS;
    }

    SetDefaultStates();

    // Setup renderstates
    Engine::GAPI->GetRendererState().RasterizerState.CullMode = GothicRasterizerStateInfo::CM_CULL_NONE;
    Engine::GAPI->GetRendererState().RasterizerState.SetDirty();

    XMMATRIX view = Engine::GAPI->GetViewMatrixXM();
    Engine::GAPI->SetViewTransformXM( view );

    SetActivePixelShader( PShaderID::PS_Diffuse );//seems like "PS_Simple" is used anyway thanks to BindShaderForTexture function used below
    SetActiveVertexShader( VShaderID::VS_Ex );

    //No idea what these do
    SetupVS_ExMeshDrawCall();
    SetupVS_ExConstantBuffer();

    // Set constant buffer
    ActivePS->GetBuffer( "FFPipelineConstantBuffer" )
        .Update( &Engine::GAPI->GetRendererState().GraphicsState )
        .Bind();

    GSky* sky = Engine::GAPI->GetSky();
    ActivePS->GetBuffer( "Atmosphere" )
        .Update( &sky->GetAtmosphereCB() )
        .Bind();

    // Use default material info for now
    MaterialInfo defInfo{};
    auto materialInfoBuffer = ActivePS->GetBuffer( "MI_MaterialInfo" )
        .Update( &defInfo.buffer, sizeof(defInfo.buffer) )
        .Bind();

    auto vsBufMPI = ActiveVS->GetBuffer( "Matrices_PerInstances" );

    for ( auto it = polyStripInfos.begin(); it != polyStripInfos.end(); it++ ) {
        zCMaterial* mat = it->second.material;
        zCTexture* tx = it->first;

        const std::vector<ExVertexStruct>& vertices = it->second.vertices;

        if ( !vertices.size() ) continue;

        // Set the world transform matrix.
        const XMMATRIX identityMatrix = XMMatrixIdentity();
        vsBufMPI.Update( &identityMatrix, sizeof( identityMatrix ) ).Bind();

        // Check for alphablending on world mesh
        bool blendAdd = mat->GetAlphaFunc() == zMAT_ALPHA_FUNC_ADD;
        bool blendBlend = mat->GetAlphaFunc() == zMAT_ALPHA_FUNC_BLEND;


        if ( tx->CacheIn( 0.6f ) == zRES_CACHED_IN ) {
            MyDirectDrawSurface7* surface = tx->GetSurface();
            ID3D11ShaderResourceView* srv[4];

            if ( BindShaderForTexture( tx, false, mat->GetAlphaFunc() ) ) {
                PsSimpleFFdata ffdata = { };
                ffdata.textureFactor = float4( 1.0f, 1.0f, 1.0f, 1.0f );
                ActivePS->GetBuffer( "cbFFData" )
                    .Update( &ffdata )
                    .Bind();
            }

            // Get diffuse and normalmap
            srv[0] = surface->GetEngineTexture()->GetShaderResourceView().Get();
            MaterialInfo* info = Engine::GAPI->GetMaterialInfoFrom( tx );
            srv[1] = GetMaterialNormalmapSRV( surface, info );
            srv[2] = surface->GetFxMap() ? surface->GetFxMap()->GetShaderResourceView().Get() : NULL;
            srv[3] = GetParallaxDisplacementSRV( surface, info );

            // Bind diffuse/normal/fx like 026; POM displacement uses t13.
            Context->PSSetShaderResources( 0, 3, srv );
            Context->PSSetShaderResources( 13, 1, &srv[3] );

            if ( (blendAdd || blendBlend) && !Engine::GAPI->GetRendererState().BlendState.BlendEnabled ) {
                if ( blendAdd )
                    Engine::GAPI->GetRendererState().BlendState.SetAdditiveBlending();
                else if ( blendBlend )
                    Engine::GAPI->GetRendererState().BlendState.SetAlphaBlending();

                Engine::GAPI->GetRendererState().BlendState.SetDirty();

                Engine::GAPI->GetRendererState().DepthState.DepthWriteEnabled = false;
                Engine::GAPI->GetRendererState().DepthState.SetDirty();

                UpdateRenderStates();
            }

            auto materialBuffer = GetEffectiveMaterialBuffer( info, tx->GetSurface() );
            materialInfoBuffer.Update( &materialBuffer, sizeof( materialBuffer ) );

        } else {
            //Don't draw if texture is not yet cached (I have no idea how can I preload it in advance)
            continue;
        }

        //Populate TempVertexBuffer and draw it
        EnsureTempVertexBufferSize( TempPolysVertexBuffer, sizeof( ExVertexStruct ) * vertices.size() );
        TempPolysVertexBuffer->UpdateBuffer( const_cast<ExVertexStruct*>(&vertices[0]), sizeof( ExVertexStruct ) * vertices.size() );
        DrawVertexBuffer( TempPolysVertexBuffer.get(), vertices.size(), sizeof( ExVertexStruct ) );
    }

    SetDefaultStates();

    return XR_SUCCESS;
}

/** Sets up the default rendering state */
void D3D11GraphicsEngine::SetDefaultStates( bool force ) {
    Engine::GAPI->GetRendererState().RasterizerState.SetDefault();
    Engine::GAPI->GetRendererState().BlendState.SetDefault();
    Engine::GAPI->GetRendererState().DepthState.SetDefault();

    Engine::GAPI->GetRendererState().RasterizerState.SetDirty();
    Engine::GAPI->GetRendererState().BlendState.SetDirty();
    Engine::GAPI->GetRendererState().DepthState.SetDirty();

    if ( force ) {
        FFRasterizerStateHash = 0;
        FFBlendStateHash = 0;
        FFDepthStencilStateHash = 0;
        UpdateRenderStates();
    }
}

/** Draws the sky using the GSky-Object */
XRESULT D3D11GraphicsEngine::DrawSky() {
    if ( Engine::IsShuttingDown() || !Engine::GAPI || !Context ) {
        return XR_FAILED;
    }
    WorldInfo* worldInfo = Engine::GAPI->GetLoadedWorldInfo();
    zCWorld* loadedWorld = worldInfo ? worldInfo->MainWorld : nullptr;
    zCSkyController_Outdoor* outdoorSky = loadedWorld ? loadedWorld->GetSkyControllerOutdoor() : nullptr;
    GSky* sky = Engine::GAPI->GetSky();
    if ( !sky || !loadedWorld ) {
        return XR_FAILED;
    }
    sky->RenderSky();

    auto& rendererState = Engine::GAPI->GetRendererState();
    if ( !rendererState.RendererSettings.AtmosphericScattering ) {
        if ( !outdoorSky ) {
            return XR_FAILED;
        }
        // for engine sky to work with z-buffer after Geometry, we need to override Z-buffer usage and set custom TransformXYZRHW to always set max Z
        auto oldStage = rendererState.RendererInfo.RenderStage;
        rendererState.RendererInfo.RenderStage = STAGE_DRAW_SKY;

        rendererState.DepthState.DepthBufferEnabled = true;

        // Disable depth-writes so the sky always stays at max distance in the
        rendererState.DepthState.DepthWriteEnabled = false;
        rendererState.DepthState.DepthBufferCompareFunc = GothicDepthBufferStateInfo::CF_COMPARISON_GREATER_EQUAL;
        rendererState.DepthState.SetDirty();


        rendererState.RasterizerState.SetDefault();
        rendererState.RasterizerState.CullMode = GothicRasterizerStateInfo::CM_CULL_BACK;
        rendererState.RasterizerState.SetDirty();
        UpdateRenderStates();

#if defined(BUILD_GOTHIC_1_08k) && !defined(BUILD_1_12F)
        // Draw sky first
        reinterpret_cast<void( __fastcall* )(zCSkyController_Outdoor*)>(0x5C0900)(outdoorSky);

        // Draw barrier second
        if ( outdoorSky ) {
            reinterpret_cast<void( __fastcall* )(zCSkyController_Outdoor*)>(0x632140)(outdoorSky);
        }
#else
        outdoorSky->RenderSkyPre();
#endif
        Engine::GAPI->SetFarPlane(
            rendererState.RendererSettings.SectionDrawRadius *
            WORLD_SECTION_SIZE );

        rendererState.RendererInfo.RenderStage = oldStage;
        return XR_SUCCESS;
    }
    // Create a rotaion only view-matrix
    XMMATRIX scale = XMMatrixScaling(
        sky->GetAtmosphereSettings().OuterRadius,
        sky->GetAtmosphereSettings().OuterRadius,
        sky->GetAtmosphereSettings().OuterRadius );  // Upscale it a huge amount. Gothics world is big.

    XMMATRIX world = XMMatrixTranslation(
        Engine::GAPI->GetCameraPosition().x,
        Engine::GAPI->GetCameraPosition().y +
        sky->GetAtmosphereSettings().SphereOffsetY,
        Engine::GAPI->GetCameraPosition().z );

    world = XMMatrixTranspose( scale * world );

    // Apply world matrix
    Engine::GAPI->SetWorldTransformXM( world );
    Engine::GAPI->SetViewTransformXM( Engine::GAPI->GetViewMatrixXM() );

    if ( sky->GetAtmosphereCB().AC_CameraHeight > sky->GetAtmosphereCB().AC_OuterRadius ) {
        SetActivePixelShader( PShaderID::PS_AtmosphereOuter );
    } else {
        SetActivePixelShader( PShaderID::PS_Atmosphere );
    }

    SetActiveVertexShader( VShaderID::VS_ExWS );

    if ( !ActivePS || !ActiveVS ) {
        return XR_FAILED;
    }

    ActivePS->GetBuffer("Atmosphere")
        .Update(&sky->GetAtmosphereCB())
        .Bind();

    VS_ExConstantBuffer_PerInstance cbi = {};
    XMStoreFloat4x4( &cbi.World, world );
    cbi.Color = float4( 1.0f, 1.0f, 1.0f, 1.0f );
    ActiveVS->GetBuffer("Matrices_PerInstances")
        .Update(&cbi, sizeof( cbi ))
        .Bind();

    rendererState.BlendState.SetDefault();
    rendererState.BlendState.BlendEnabled = true;

    rendererState.DepthState.SetDefault();

    // Allow z-testing
    rendererState.DepthState.DepthBufferEnabled = true;

    // Disable depth-writes so the sky always stays at max distance in the
    // DepthBuffer
    rendererState.DepthState.DepthWriteEnabled = false;
    rendererState.DepthState.DepthBufferCompareFunc = GothicDepthBufferStateInfo::CF_COMPARISON_GREATER_EQUAL;

    rendererState.RasterizerState.SetDefault();
    rendererState.DepthState.SetDirty();
    rendererState.BlendState.SetDirty();

    rendererState.RasterizerState.CullMode = GothicRasterizerStateInfo::CM_CULL_BACK;
    rendererState.RasterizerState.SetDirty();

    SetupVS_ExMeshDrawCall();
    SetupVS_ExConstantBuffer();

    ID3D11ShaderResourceView* srvs[4]{};
    // Apply sky texture
    D3D11Texture* cloudsTex = Engine::GAPI->GetSky()->GetCloudTexture();
    if ( cloudsTex ) {
        srvs[0] = cloudsTex->GetShaderResourceView().Get();
    }

    D3D11Texture* nightTex = Engine::GAPI->GetSky()->GetNightTexture();
    if ( nightTex ) {
        srvs[1] = nightTex->GetShaderResourceView().Get();
    }

    D3D11Texture* moonTex = sky->GetMoonTexture();
    if ( moonTex ) {
        srvs[2] = moonTex->GetShaderResourceView().Get();
    }

    D3D11Texture* rainCloudTex = sky->GetRainCloudTexture();
    if ( rainCloudTex ) {
        srvs[3] = rainCloudTex->GetShaderResourceView().Get();
    }

    GetContext()->PSSetShaderResources( 0, std::size( srvs ), srvs);

    if ( sky->GetSkyDome() ) sky->GetSkyDome()->DrawMesh();

    #if defined(BUILD_GOTHIC_1_08k) && !defined(BUILD_1_12F)
    {
        SetDefaultStates();
        rendererState.DepthState.DepthWriteEnabled = false;
        rendererState.DepthState.SetDirty();
        UpdateRenderStates();

        // Draw barrier after sky
        if ( outdoorSky ) {
            reinterpret_cast<void( __fastcall* )(zCSkyController_Outdoor*)>(0x632140)(outdoorSky);
        }
        Engine::GAPI->SetFarPlane(
            rendererState.RendererSettings.SectionDrawRadius *
            WORLD_SECTION_SIZE );
    }
    #endif

    return XR_SUCCESS;
}

/** Called when a key got pressed */
XRESULT D3D11GraphicsEngine::OnKeyDown( unsigned int key ) {
    if ( !Engine::GAPI || Engine::IsShuttingDown() ) {
        return XR_FAILED;
    }
    switch ( key ) {
#ifndef PUBLIC_RELEASE
    case VK_NUMPAD0:
        Engine::GAPI->PrintMessageTimed( INT2( 30, 30 ), "Reloading shaders..." );
        ReloadShaders();
        break;
#endif

    case VK_NUMPAD7:
        if ( Engine::GAPI->GetRendererState().RendererSettings.AllowNumpadKeys ) {
            SaveScreenshotNextFrame = true;
        }
        break;

    default:
        break;
    }

    return XR_SUCCESS;
}

/** Reloads shaders */
XRESULT D3D11GraphicsEngine::ReloadShaders( ShaderCategory categories ) {
    XRESULT xr = ShaderManager->ReloadShaders( categories );

    return xr;
}

/** Returns the line renderer object */
BaseLineRenderer* D3D11GraphicsEngine::GetLineRenderer() {
    return LineRenderer.get();
}

/** Renders the shadowmaps for a pointlight */
void XM_CALLCONV D3D11GraphicsEngine::RenderShadowCube(
    FXMVECTOR position, float range,
    const RenderToDepthStencilBuffer& targetCube, Microsoft::WRL::ComPtr<ID3D11DepthStencilView> face,
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> debugRTV, bool cullFront, bool indoor, bool noNPCs,
    std::list<VobInfo*>* renderedVobs,
    std::list<SkeletalVobInfo*>* renderedMobs,
    std::vector<std::pair<MeshKey, MeshInfo*>>* worldMeshCache,
    bool clearDepth,
    unsigned int casterMask ) {

    if ( Engine::IsShuttingDown() || !Engine::GAPI || !ShadowMaps ) {
        return;
    }
    ShadowMaps->RenderShadowCube( position, range, targetCube, face, debugRTV,
        cullFront, indoor, noNPCs, renderedVobs, renderedMobs, worldMeshCache, clearDepth, casterMask );
}

/** Renders the shadowmaps for the sun */
void XM_CALLCONV D3D11GraphicsEngine::RenderShadowmaps( FXMVECTOR cameraPosition,
    RenderToDepthStencilBuffer* target,
    bool cullFront, bool dontCull,
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> dsvOverwrite,
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> debugRTV ) {

    if ( Engine::IsShuttingDown() || !Engine::GAPI || !ShadowMaps ) {
        return;
    }

    RenderShadowmapsParams renderParams = {};
    XMStoreFloat3( &renderParams.CameraPosition, cameraPosition );
    renderParams.Target = target;
    renderParams.CullFront = cullFront;
    renderParams.DontCull = dontCull;
    renderParams.DSVOverwrite = dsvOverwrite;
    renderParams.DebugRTV = debugRTV;
    renderParams.CascadeIndex = -1;
    renderParams.CascadeSplits = std::vector<float>();
    renderParams.CascadeCameraReplacements = nullptr;

    ShadowMaps->RenderShadowmaps( renderParams );
}

/** Draws a fullscreenquad, copying the given texture to the viewport */
void D3D11GraphicsEngine::DrawQuad( INT2 position, INT2 size ) {
    if ( Engine::IsShuttingDown() || !Context || !PfxRenderer ) {
        return;
    }
    wrl::ComPtr<ID3D11ShaderResourceView> srv;
    Context->PSGetShaderResources( 0, 1, srv.GetAddressOf() );

    wrl::ComPtr<ID3D11RenderTargetView> rtv;
    Context->OMGetRenderTargets( 1, rtv.GetAddressOf(), nullptr );

    if ( srv.Get() ) {
        if ( rtv.Get() ) {
            PfxRenderer->CopyTextureToRTV( srv, rtv, size, false, position );
        }
    }
}

/** Sets the current rendering stage */
void D3D11GraphicsEngine::SetRenderingStage( D3D11ENGINE_RENDER_STAGE stage ) {
    RenderingStage = stage;
}

/** Returns the current rendering stage */
D3D11ENGINE_RENDER_STAGE D3D11GraphicsEngine::GetRenderingStage() {
    return RenderingStage;
}

/** Draws a VOB (used for inventory) */
void D3D11GraphicsEngine::DrawVobSingle( VobInfo* vob, zCCamera& camera ) {
    Engine::GAPI->SetViewTransformXM( XMLoadFloat4x4( &camera.GetTransformDX( zCCamera::ETransformType::TT_VIEW ) ) );

    // Important: We NEED a swapchain-sized depth stencil buffer here, otherwise Advanced Inventory VOBs will be rendered without depth testing and thus look very bad.
    GetContext()->OMSetRenderTargets( 1, Backbuffer->GetRenderTargetView().GetAddressOf(), m_SwapchainDepthStencilBuffer->GetDepthStencilView().Get() );

    // Set backface culling
    Engine::GAPI->GetRendererState().RasterizerState.CullMode = GothicRasterizerStateInfo::CM_CULL_BACK;
    Engine::GAPI->GetRendererState().RasterizerState.SetDirty();
    GetContext()->PSSetSamplers( 0, 1, DefaultSamplerState.GetAddressOf() );

    SetActivePixelShader( PShaderID::PS_Preview_Textured );
    SetActiveVertexShader( VShaderID::VS_Ex );

    SetupVS_ExMeshDrawCall();
    SetupVS_ExConstantBuffer();

    VS_ExConstantBuffer_PerInstance cbInstance = {};
    cbInstance.World = *vob->Vob->GetWorldMatrixPtr();
    cbInstance.Color = float4( 1.0f, 1.0f, 1.0f, 1.0f );
    ActiveVS->GetBuffer( "Matrices_PerInstances" ).Update( &cbInstance, sizeof( cbInstance ) ).Bind();

    for ( auto const& itm : vob->VisualInfo->Meshes ) {
        // Cache & bind texture
        zCTexture* texture;
        if ( itm.first && ( texture = itm.first->GetTexture() ) != nullptr ) {
            if ( texture->CacheIn( 0.6f ) == zRES_CACHED_IN ) {
                texture->Bind( 0 );
            } else {
                continue;
            }
        } else {
            continue;
        }
        for ( auto const& itm2nd : itm.second ) {
            // Draw instances
            DrawVertexBufferIndexed(
                itm2nd->MeshVertexBuffer, itm2nd->MeshIndexBuffer,
                itm2nd->Indices.size() );
        }
    }

    GetContext()->OMSetRenderTargets( 1, Backbuffer->GetRenderTargetView().GetAddressOf(), nullptr );

    // Disable culling again
    Engine::GAPI->GetRendererState().RasterizerState.CullMode = GothicRasterizerStateInfo::CM_CULL_NONE;
    Engine::GAPI->GetRendererState().RasterizerState.SetDirty();
    GetContext()->PSSetSamplers( 0, 1, ClampSamplerState.GetAddressOf() );
}

/** Update focus window state */
void D3D11GraphicsEngine::UpdateFocus( HWND hWnd, bool focus_state )
{
    if ( !hWnd ) {
        return;
    }

    if ( m_isWindowActive == focus_state ) {
        return;
    }

    m_isWindowActive = focus_state;
    UpdateClipCursor( hWnd );
}

/** Update clipping cursor onto window */
void D3D11GraphicsEngine::UpdateClipCursor( HWND hWnd )
{
#ifndef BUILD_SPACER_NET
    if ( !hWnd ) {
        ClipCursor( nullptr );
        return;
    }

    RECT rect = {};
    static RECT last_clipped_rect = {};

    // People use open settings window to navigate to other screens
    if ( m_isWindowActive && !HasSettingsWindow() ) {
        if ( !IsWindow( hWnd ) || !GetClientRect( hWnd, &rect )
            || !ClientToScreen( hWnd, reinterpret_cast<LPPOINT>(&rect) )
            || !ClientToScreen( hWnd, reinterpret_cast<LPPOINT>(&rect) + 1 ) ) {
            ClipCursor( nullptr );
            return;
        }
        if ( ClipCursor( &rect ) ) {
            last_clipped_rect = rect;
        }
    } else {
        if ( GetClipCursor( &rect ) && memcmp( &rect, &last_clipped_rect, sizeof( RECT ) ) == 0 ) {
            ClipCursor( nullptr );
            ZeroMemory( &last_clipped_rect, sizeof( RECT ) );
        }
    }
#endif
}

/** Message-Callback for the main window */
LRESULT D3D11GraphicsEngine::OnWindowMessage( HWND hWnd, UINT msg, WPARAM wParam,
    LPARAM lParam ) {
    if ( Engine::IsShuttingDown() ) {
        return 0;
    }
    switch ( msg ) {
        case WM_NCACTIVATE: UpdateFocus( hWnd, !!wParam ); break;
        case WM_ACTIVATE: UpdateFocus( hWnd, !!LOWORD( wParam ) ); break;
        case WM_SETFOCUS: UpdateFocus( hWnd, true ); break;
        case WM_KILLFOCUS:
        case WM_ENTERIDLE: UpdateFocus( hWnd, false ); break;
        case WM_WINDOWPOSCHANGED: UpdateClipCursor( hWnd ); break;
    }
    return 0;
}

void D3D11GraphicsEngine::UpdateShouldBlockGameInput( ) {
    if ( auto hImgui = Engine::ImGuiHandle; hImgui && Engine::GAPI ) {
        auto oldIsActive = hImgui->IsActive;
        hImgui->IsActive = hImgui->SettingsVisible || hImgui->LibShowBlockingThisFrame;
        hImgui->UpdateBlockGameInput();

        if ( oldIsActive != hImgui->IsActive ) {
            Engine::GAPI->SetEnableGothicInput( !hImgui->IsActive );
        }
    }
}

/** Handles an UI-Event */
void D3D11GraphicsEngine::OnUIEvent( EUIEvent uiEvent ) {

    if ( !Engine::GAPI || Engine::IsShuttingDown() || !OutputWindow ) {
        return;
    }

    if ( uiEvent == UI_OpenSettings || uiEvent == UI_OpenSettingsFromGothicVideoSettings ) {
        if ( auto hImgui = Engine::ImGuiHandle ) {
            const bool openSettings = !hImgui->SettingsVisible;
            hImgui->SettingsVisible = openSettings;
            if ( openSettings ) {
                hImgui->BeginSettingsEdit();
            } else {
                // F11 keeps the current session values, exactly as before.
                hImgui->CommitSettingsEdit();
            }
            UpdateShouldBlockGameInput();

        }
        UpdateClipCursor( OutputWindow );
    } else if ( uiEvent == UI_ClosedSettings ) {
        // Settings can be closed in multiple ways
        if ( auto hImgui = Engine::ImGuiHandle; hImgui && hImgui->GetIsActive() ) {
            // ESC and other generic close paths retain current session values.
            hImgui->CommitSettingsEdit();
            hImgui->SettingsVisible = false;
        }
        UpdateShouldBlockGameInput();

        UpdateClipCursor( OutputWindow );
    }
}

/** Returns the data of the backbuffer */
void D3D11GraphicsEngine::GetBackbufferData( bool thumbnail, byte** data, INT2& buffersize, int& pixelsize ) {
    if ( data ) {
        *data = nullptr;
    }
    pixelsize = 0;
    if ( !data || Engine::IsShuttingDown() || !Engine::GAPI || !Device || !Context
        || !PfxRenderer || !ShaderManager || !HDRBackBuffer
        || !HDRBackBuffer->GetShaderResView() || !HDRBackBuffer->GetRenderTargetView() ) {
        buffersize = INT2( 0, 0 );
        return;
    }

    if ( thumbnail ) {
        buffersize = INT2( 256, 256 );
    } else {
        buffersize = Resolution;
    }
    if ( buffersize.x <= 0 || buffersize.y <= 0 ) {
        buffersize = INT2( 0, 0 );
        return;
    }

    // Copy HDR scene to backbuffer
    SetDefaultStates();

    SetActivePixelShader( PShaderID::PS_PFX_GammaCorrectInv );
    if ( !ActivePS ) {
        buffersize = INT2( 0, 0 );
        return;
    }
    ActivePS->Apply();

    GammaCorrectConstantBuffer gcb = {};
    gcb.G_Gamma = Engine::GAPI->GetGammaValue();
    gcb.G_Brightness = Engine::GAPI->GetBrightnessValue();

    ActivePS->GetBuffer( "GammaCorrectConstantBuffer" ).Update( &gcb ).Bind();

    HRESULT hr;
    auto rt = std::make_unique<RenderToTextureBuffer>(
        GetDevice().Get(), buffersize.x, buffersize.y, DXGI_FORMAT_ENGINE_SWAPCHAIN  );
    if ( !rt || !rt->GetTexture() || !rt->GetRenderTargetView() ) {
        buffersize = INT2( 0, 0 );
        return;
    }
    PfxRenderer->CopyTextureToRTV( HDRBackBuffer->GetShaderResView(), rt->GetRenderTargetView(), buffersize, true );
    GetContext()->Flush();

    auto outputData = std::make_unique<byte[]>(
        static_cast<size_t>( buffersize.x ) * static_cast<size_t>( buffersize.y ) * 4u );

    D3D11_TEXTURE2D_DESC texDesc;
    texDesc.ArraySize = 1;
    texDesc.BindFlags = 0;
    texDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    texDesc.Format = DXGI_FORMAT_ENGINE_SWAPCHAIN ;
    texDesc.Width = buffersize.x;
    texDesc.Height = buffersize.y;
    texDesc.MipLevels = 1;
    texDesc.MiscFlags = 0;
    texDesc.SampleDesc.Count = 1;
    texDesc.SampleDesc.Quality = 0;
    texDesc.Usage = D3D11_USAGE_STAGING;

    wrl::ComPtr<ID3D11Texture2D> texture;
    LE( GetDevice()->CreateTexture2D( &texDesc, 0, texture.GetAddressOf() ) );
    if ( !texture.Get() ) {
        if ( thumbnail ) {
            LogInfo() << "Thumbnail failed. Texture could not be created";
        } else {
            LogInfo() << "GetBackbufferData failed. Texture could not be created";
        }
        return;
    }
    GetContext()->CopyResource( texture.Get(), rt->GetTexture().Get() );
    GetContext()->Flush();

    // Get data
    D3D11_MAPPED_SUBRESOURCE res;
    if ( SUCCEEDED( GetContext()->Map( texture.Get(), 0, D3D11_MAP_READ, 0, &res ) ) ) {
        unsigned char* dstData = reinterpret_cast<unsigned char*>(res.pData);
        unsigned char* srcData = reinterpret_cast<unsigned char*>(outputData.get());
        UINT length = buffersize.x * 4;
        if ( length == res.RowPitch ) {
            memcpy( srcData, dstData, length * buffersize.y );
        } else {
            if ( length > res.RowPitch ) {
                length = res.RowPitch;
            }

            for ( int row = 0; row < buffersize.y; ++row ) {
                memcpy( srcData, dstData, length );
                srcData += length;
                dstData += res.RowPitch;
            }
        }
        GetContext()->Unmap( texture.Get(), 0 );
    } else {
        if ( thumbnail ) {
            LogInfo() << "Thumbnail failed";
        } else {
            LogInfo() << "GetBackbufferData failed";
        }
        buffersize = INT2( 0, 0 );
        return;
    }

    pixelsize = 4;
    *data = outputData.release();
}

/* Binds the right shader for the given texture */
bool D3D11GraphicsEngine::BindShaderForTexture( zCTexture* texture,
    bool forceAlphaTest,
    int zMatAlphaFunc,
    MaterialInfo::EMaterialType materialInfo,
    bool allowWetNormalFallback ) {
    return ActiveSceneRenderer->BindShaderForTexture( GetShaderManager(), ActivePS,
        texture, forceAlphaTest, zMatAlphaFunc, materialInfo,
        Resolved_DiffuseNormalmapped,
        Resolved_DiffuseNormalmappedFxMap,
        Resolved_DiffuseNormalmappedAlphatest,
        Resolved_DiffuseNormalmappedAlphatestFxMap,
        allowWetNormalFallback );
}

/** Draws the given list of decals */
void D3D11GraphicsEngine::DrawDecalList( const std::vector<zCVob*>& decals, bool lighting ) {

    SetDefaultStates();
    TracyD3D11ZoneCGX( "DrawDecalList" );
    auto _ = RecordGraphicsEvent( GE_NAME( "DrawDecalList" ) );

    Engine::GAPI->GetRendererState().RasterizerState.CullMode = GothicRasterizerStateInfo::CM_CULL_NONE;
    Engine::GAPI->GetRendererState().RasterizerState.SetDirty();

    XMMATRIX view = Engine::GAPI->GetViewMatrixXM();
    Engine::GAPI->SetViewTransformXM( view );  // Update view transform

    // Set up alpha
    if ( !lighting ) {
        SetActivePixelShader( PShaderID::PS_Transparency );
        Engine::GAPI->GetRendererState().DepthState.DepthWriteEnabled = false;
        Engine::GAPI->GetRendererState().DepthState.SetDirty();
    } else {
        SetActivePixelShader( PShaderID::PS_World_NoMV );
    }

    SetActiveVertexShader( VShaderID::VS_DecalInstanced );
    GetActivePS()->Apply();
    GetActiveVS()->Apply();

    SetupVS_ExMeshDrawCall();
    SetupVS_ExConstantBuffer();
    XMFLOAT3 camPos = Engine::GAPI->GetCameraPosition();

    struct DecalInstance {
        zCMaterial* material;
        XMFLOAT4X4 worldView;
        bool ignoreDayLight;
    };
    static std::vector<DecalInstance> instances;
    instances.clear();
    instances.reserve( decals.size() );

    for ( unsigned int i = 0; i < decals.size(); i++ ) {
        zCDecal* d = static_cast<zCDecal*>(decals[i]->GetVisual());
        if ( !d ) {
            continue;
        }

        zCMaterial* material = d->GetDecalSettings()->DecalMaterial;
        if ( !material ) {
            continue;
        }

        zCTexture* texture = material->GetTexture();
        if ( !texture ) {
            continue;
        }

        int alphaFunc = material->GetAlphaFunc();
        if ( alphaFunc == zMAT_ALPHA_FUNC_MAT_DEFAULT ) {
            alphaFunc = zMAT_ALPHA_FUNC_BLEND;
            if ( !texture->HasAlphaChannel() ) {
                alphaFunc = zMAT_ALPHA_FUNC_NONE;
            }
        }

        if ( lighting && !(alphaFunc == zMAT_ALPHA_FUNC_NONE || alphaFunc == zMAT_ALPHA_FUNC_TEST) )
            continue;  // Only allow no alpha or alpha test

        if ( !lighting ) {
            // Only keep decals with a supported blend mode (matches the draw-time switch below)
            switch ( alphaFunc ) {
            case zMAT_ALPHA_FUNC_BLEND:
            case zMAT_ALPHA_FUNC_BLEND_TEST:
            case zMAT_ALPHA_FUNC_ADD:
            case zMAT_ALPHA_FUNC_MUL:
            case zMAT_ALPHA_FUNC_MUL2:
                break;
            default:
                continue;
            }
        }

        int alignment = decals[i]->GetAlignment();
        XMMATRIX world = decals[i]->GetWorldMatrixXM();
        XMMATRIX offset =
            XMMatrixTranslation( d->GetDecalSettings()->DecalOffset.x, -d->GetDecalSettings()->DecalOffset.y, 0 );
        XMMATRIX scale =
            XMMatrixTranspose( XMMatrixScaling( d->GetDecalSettings()->DecalSize.x * 2,
                -d->GetDecalSettings()->DecalSize.y * 2, 1 ) );

        if ( alignment == zVISUAL_CAM_ALIGN_YAW ) {
            XMFLOAT3 decalPos = decals[i]->GetPositionWorld();
            XMVECTOR at = XMVectorSet( decalPos.x - camPos.x, 0.0f, decalPos.z - camPos.z, 0.0f );
            XMFLOAT4 atLengthSq = {};
            XMStoreFloat4( &atLengthSq, XMVector3LengthSq( at ) );

            // Match original Gothic cam-align yaw: SetAt/SetUp/MakeOrthonormal on object transform.
            if ( atLengthSq.x > 1e-6f ) {
                XMMATRIX worldObj = XMMatrixTranspose( world );
                XMVECTOR translation = worldObj.r[3];

                at = XMVector3Normalize( at );
                XMVECTOR up = XMVectorSet( 0.0f, 1.0f, 0.0f, 0.0f );
                XMVECTOR right = XMVector3Normalize( XMVector3Cross( up, at ) );
                up = XMVector3Normalize( XMVector3Cross( at, right ) );

                XMFLOAT3 right3 = {};
                XMFLOAT3 up3 = {};
                XMFLOAT3 at3 = {};
                XMFLOAT3 translation3 = {};
                XMStoreFloat3( &right3, right );
                XMStoreFloat3( &up3, up );
                XMStoreFloat3( &at3, at );
                XMStoreFloat3( &translation3, translation );

                worldObj.r[0] = XMVectorSet( right3.x, right3.y, right3.z, 0.0f );
                worldObj.r[1] = XMVectorSet( up3.x, up3.y, up3.z, 0.0f );
                worldObj.r[2] = XMVectorSet( at3.x, at3.y, at3.z, 0.0f );
                worldObj.r[3] = XMVectorSet( translation3.x, translation3.y, translation3.z, 1.0f );

                world = XMMatrixTranspose( worldObj );
            }
        } else if ( alignment == zVISUAL_CAM_ALIGN_FULL ) {
            XMFLOAT3 decalPos = decals[i]->GetPositionWorld();
            XMVECTOR at = XMVectorSet( decalPos.x - camPos.x, decalPos.y - camPos.y, decalPos.z - camPos.z, 0.0f );
            XMFLOAT4 atLengthSq = {};
            XMStoreFloat4( &atLengthSq, XMVector3LengthSq( at ) );

            if ( atLengthSq.x > 1e-6f ) {
                at = XMVector3Normalize( at );

                XMVECTOR upRef = XMVectorSet( 0.0f, 1.0f, 0.0f, 0.0f );
                XMFLOAT4 upDot = {};
                XMStoreFloat4( &upDot, XMVector3Dot( at, upRef ) );

                if ( fabsf( upDot.x ) > 0.999f ) {
                    upRef = XMVectorSet( 0.0f, 0.0f, 1.0f, 0.0f );
                }

                XMVECTOR right = XMVector3Normalize( XMVector3Cross( upRef, at ) );
                XMVECTOR up = XMVector3Normalize( XMVector3Cross( at, right ) );

                XMFLOAT3 right3 = {};
                XMFLOAT3 up3 = {};
                XMFLOAT3 at3 = {};
                XMStoreFloat3( &right3, right );
                XMStoreFloat3( &up3, up );
                XMStoreFloat3( &at3, at );

                XMMATRIX worldObj;
                worldObj.r[0] = XMVectorSet( right3.x, right3.y, right3.z, 0.0f );
                worldObj.r[1] = XMVectorSet( up3.x, up3.y, up3.z, 0.0f );
                worldObj.r[2] = XMVectorSet( at3.x, at3.y, at3.z, 0.0f );
                worldObj.r[3] = XMVectorSet( decalPos.x, decalPos.y, decalPos.z, 1.0f );

                world = XMMatrixTranspose( worldObj );
            } else {
                world = XMMatrixTranspose( XMMatrixTranslation( decalPos.x, decalPos.y, decalPos.z ) );
            }
        }

        DecalInstance inst;
        inst.material = material;
        inst.ignoreDayLight = d->GetDecalSettings()->IgnoreDayLight != FALSE;
        XMStoreFloat4x4( &inst.worldView, view * world * offset * scale );
        instances.push_back( inst );
    }

    if ( instances.empty() ) {
        return;
    }

    // upload instance data
    const size_t neededBytes = instances.size() * sizeof( XMFLOAT4X4 );
    if ( DecalInstancingBuffer->GetSizeInBytes() < neededBytes ) {
        if ( XR_FAILED == DecalInstancingBuffer->Init( nullptr, neededBytes,
            D3D11VertexBuffer::B_VERTEXBUFFER, D3D11VertexBuffer::U_DYNAMIC, D3D11VertexBuffer::CA_WRITE ) ) {
            LogError() << "Failed to (re)create decal instance buffer!";
            return;
        }
        SetDebugName( DecalInstancingBuffer->GetVertexBuffer().Get(), "DecalInstancingBuffer" );
    }

    void* mappedData;
    UINT mappedSize;
    if ( XR_SUCCESS != DecalInstancingBuffer->Map( D3D11VertexBuffer::M_WRITE_DISCARD, &mappedData, &mappedSize ) ) {
        LogError() << "Failed to map decal instance buffer!";
        return;
    }
    auto* destData = static_cast<XMFLOAT4X4*>(mappedData);
    for ( size_t i = 0; i < instances.size(); ++i ) {
        destData[i] = instances[i].worldView;
    }
    DecalInstancingBuffer->Unmap();

    // draw per consecutive same-material
    UINT strides[2] = { sizeof( ExVertexStruct ), sizeof( XMFLOAT4X4 ) };
    UINT offsets[2] = { 0, 0 };
    ID3D11Buffer* vbs[2] = {
        QuadVertexBuffer->GetVertexBuffer().Get(),
        DecalInstancingBuffer->GetVertexBuffer().Get()
    };
    Context->IASetVertexBuffers( 0, 2, vbs, strides, offsets );
    Context->IASetIndexBuffer( QuadIndexBuffer->GetVertexBuffer().Get(), VERTEX_INDEX_DXGI_FORMAT, 0 );

    GhostAlphaConstantBuffer gacb = {};
    gacb.GA_ViewportSize = float2( Engine::GraphicsEngine->GetResolution().x, Engine::GraphicsEngine->GetResolution().y );
    gacb.GA_Alpha = 1.0f;
    gacb.GA_LightingScale = 1.0f;
    gacb.GA_LightingTint = float3( 1.0f, 1.0f, 1.0f );
    gacb.GA_Pad = 0.0f;
    if ( !lighting ) {
        auto saturateDecal = []( float v ) { return v < 0.0f ? 0.0f : ( v > 1.0f ? 1.0f : v ); };
        if ( auto sky = Engine::GAPI->GetSky() ) {
            const auto& atmo = sky->GetAtmosphereCB();
            float night = saturateDecal( ( -atmo.AC_LightPos.y + 0.08f ) * 2.5f )
                * saturateDecal( atmo.AC_EnableNightAtmosphere );
            float rain = std::max( saturateDecal( atmo.AC_RainFXWeight ), saturateDecal( atmo.AC_SceneWettness ) );
            gacb.GA_LightingScale = ( 1.0f + ( 0.34f - 1.0f ) * night ) * ( 1.0f + ( 0.78f - 1.0f ) * rain );
            gacb.GA_LightingTint = float3(
                1.0f + ( 0.50f - 1.0f ) * night * 0.80f,
                1.0f + ( 0.65f - 1.0f ) * night * 0.80f,
                1.0f );
        }
    }
    const float ambientDecalLightingScale = gacb.GA_LightingScale;
    const float3 ambientDecalLightingTint = gacb.GA_LightingTint;

    int lastAlphaFunc = -1;
    zCTexture* lastTex = nullptr;
    float lastGhostAlpha = gacb.GA_Alpha;
    float lastLightingScale = gacb.GA_LightingScale;
    auto psBufGAI = GetActivePS()->GetBuffer( "GhostAlphaInfo" )
        .Update( &gacb )
        .Bind();

    for ( size_t i = 0; i < instances.size(); ) {
        auto material = instances[i].material;

        if ( !lighting ) {
            const auto alphaPart = (material->GetColor() >> 24);
            if ( alphaPart == 0 ) {
                i++;
                continue;  // Don't render fully transparent decals
            }
        }

        const auto& firstMatName = material->__GetName();
        std::string_view firstMaterialName = { firstMatName.ToChar(), firstMatName.Length() };
        const size_t start = i;
        while ( i < instances.size() ) {
            const auto& matName = instances[i].material->__GetName();
            std::string_view materialName = { matName.ToChar(), matName.Length() };
            if ( materialName != firstMaterialName ) {
                break;
            }
            // Unnamed materials may share a batch when their render state matches.
            if ( material->GetColor() != instances[i].material->GetColor()
                || material->GetAniTexture() != instances[i].material->GetAniTexture()
                || material->GetFlags() != instances[i].material->GetFlags()
                || instances[start].ignoreDayLight != instances[i].ignoreDayLight) {
                break;
            }
            ++i;
        }
        const unsigned int count = static_cast<unsigned int>(i - start);

        zCTexture* texture = material->GetTexture();
        int alphaFunc = material->GetAlphaFunc();
        if ( alphaFunc == zMAT_ALPHA_FUNC_MAT_DEFAULT ) {
            alphaFunc = zMAT_ALPHA_FUNC_BLEND;
            if ( !texture->HasAlphaChannel() ) {
                alphaFunc = zMAT_ALPHA_FUNC_NONE;
            }
        }

        if ( !lighting ) {
            const float lightingScale = instances[start].ignoreDayLight ? 1.0f : ambientDecalLightingScale;
            if ( lastLightingScale != lightingScale ) {
                gacb.GA_LightingScale = lightingScale;
                gacb.GA_LightingTint = instances[start].ignoreDayLight
                    ? float3( 1.0f, 1.0f, 1.0f )
                    : ambientDecalLightingTint;
                psBufGAI.Update( &gacb );
                lastLightingScale = lightingScale;
            }

            switch ( alphaFunc ) {
            case zMAT_ALPHA_FUNC_BLEND:
            case zMAT_ALPHA_FUNC_BLEND_TEST:
                Engine::GAPI->GetRendererState().BlendState.SetAlphaBlending();
                break;

            case zMAT_ALPHA_FUNC_ADD:
                Engine::GAPI->GetRendererState().BlendState.SetAdditiveBlending();
                break;

            case zMAT_ALPHA_FUNC_MUL:
                Engine::GAPI->GetRendererState().BlendState.SetModulateBlending();
                break;

            case zMAT_ALPHA_FUNC_MUL2:
                Engine::GAPI->GetRendererState().BlendState.SetModulate2Blending();
                break;

            default:
                continue;
            }

            if ( lastAlphaFunc != alphaFunc ) {
                Engine::GAPI->GetRendererState().BlendState.SetDirty();
                UpdateRenderStates();
                lastAlphaFunc = alphaFunc;
            }
        }

        if ( texture != lastTex ) {
            if ( texture->CacheIn( 0.6f ) != zRES_CACHED_IN ) {
                continue;  // Don't render not cached surfaces
            }
            auto t = texture->GetSurface()->GetEngineTexture()->GetShaderResourceView().Get();
            Context->PSSetShaderResources( 0, 1, &t );
            lastTex = texture;
        }

        if ( !lighting ) {
            const auto ghostAlpha = (material->GetColor() >> 24) * inv255f;
            if ( lastGhostAlpha != ghostAlpha ) {
                gacb.GA_Alpha = ghostAlpha;
                psBufGAI.Update( &gacb );
                lastGhostAlpha = gacb.GA_Alpha;
            }
        }

        GetContext()->DrawIndexedInstanced( 6, count, 0, 0, static_cast<unsigned int>(start) );
        Engine::GAPI->GetRendererState().RendererInfo.FrameDrawnTriangles += 2 * count;
    }

    // Unbind the instance buffer from slot 1 again
    ID3D11Buffer* nullBuf = nullptr;
    UINT nullStride = 0;
    UINT nullOffset = 0;
    Context->IASetVertexBuffers( 1, 1, &nullBuf, &nullStride, &nullOffset );
    if ( !lighting ) {
        Context->PSSetShaderResources( 3, 1, s_nullSRVs );
    }
}

/** Draws quadmarks in a simple way */
void D3D11GraphicsEngine::DrawQuadMarks() {
    const auto& quadMarks = Engine::GAPI->GetQuadMarks();
    if ( quadMarks.empty() ) return;

    SetActiveVertexShader( VShaderID::VS_Ex );
    SetActivePixelShader( PShaderID::PS_World );

    SetDefaultStates();

    XMMATRIX view = Engine::GAPI->GetViewMatrixXM();
    Engine::GAPI->SetViewTransformXM( view );  // Update view transform

    Engine::GAPI->GetRendererState().RasterizerState.CullMode = GothicRasterizerStateInfo::CM_CULL_NONE;
    Engine::GAPI->GetRendererState().RasterizerState.SetDirty();

    ActivePS->GetBuffer( "FFPipelineConstantBuffer" )
        .Update( &Engine::GAPI->GetRendererState().GraphicsState )
        .Bind();

    SetupVS_ExMeshDrawCall();
    SetupVS_ExConstantBuffer();

    int alphaFunc = zMAT_ALPHA_FUNC_NONE;

    auto vfxRadiusSq = Engine::GAPI->GetRendererState().RendererSettings.GetEffectiveVisualFXDrawRadius() * Engine::GAPI->GetRendererState().RendererSettings.GetEffectiveVisualFXDrawRadius();
    auto vVfxRadiusSq = XMVectorReplicate(vfxRadiusSq);
    const auto camPos = Engine::GAPI->GetCameraPositionXM();
    for ( auto const& it : quadMarks ) {
        if ( !it.first->GetConnectedVob() ) continue;

        if ( XMVector3Greater(XMVector3LengthSq( camPos - XMLoadFloat3( it.second.Position.toXMFLOAT3() ) ), vVfxRadiusSq) ) {
            continue;
        }

        zCMesh* mesh = it.first->GetQuadMesh();
        int numPolys = mesh->GetNumPolygons();
        zCPolygon** polys = mesh->GetPolygons();
        zCMaterial* mat = (numPolys > 0 ? polys[0]->GetMaterial() : it.first->GetMaterial());
        if ( mat ) mat->BindTexture( 0 );

        if ( alphaFunc != mat->GetAlphaFunc() ) {
            // Change alpha-func
            switch ( mat->GetAlphaFunc() ) {
            case zMAT_ALPHA_FUNC_ADD:
                Engine::GAPI->GetRendererState().BlendState.SetAdditiveBlending();
                break;

            case zMAT_ALPHA_FUNC_BLEND:
                Engine::GAPI->GetRendererState().BlendState.SetAlphaBlending();
                break;

            case zMAT_ALPHA_FUNC_NONE:
            case zMAT_ALPHA_FUNC_TEST:
                Engine::GAPI->GetRendererState().BlendState.SetDefault();
                break;

            case zMAT_ALPHA_FUNC_MUL:
            case zMAT_ALPHA_FUNC_MUL2:
                MulQuadMarks.emplace_back( it.first, &it.second );
                continue;

            default:
                continue;
            }

            alphaFunc = mat->GetAlphaFunc();

            Engine::GAPI->GetRendererState().BlendState.SetDirty();
            UpdateRenderStates();
        }

        Engine::GAPI->SetWorldTransformXM( it.first->GetConnectedVob()->GetWorldMatrixXM() );
        SetupVS_ExPerInstanceConstantBuffer();

        DrawVertexBuffer( it.second.Mesh.get(), it.second.NumVertices );
    }
}

void D3D11GraphicsEngine::DrawMQuadMarks() {
    if ( MulQuadMarks.empty() ) return;

    auto _ = RecordGraphicsEvent( GE_NAME( "DrawMQuadMarks" ) );
    TracyD3D11ZoneCGX( "DrawMQuadMarks" );

    SetActiveVertexShader( VShaderID::VS_Ex );
    SetActivePixelShader( PShaderID::PS_Simple );

    SetDefaultStates();

    XMMATRIX view = Engine::GAPI->GetViewMatrixXM();
    Engine::GAPI->SetViewTransformXM( view );  // Update view transform

    Engine::GAPI->GetRendererState().RasterizerState.CullMode = GothicRasterizerStateInfo::CM_CULL_NONE;
    Engine::GAPI->GetRendererState().RasterizerState.SetDirty();
    Engine::GAPI->GetRendererState().DepthState.DepthWriteEnabled = false;
    Engine::GAPI->GetRendererState().DepthState.SetDirty();

    SetupVS_ExMeshDrawCall();
    SetupVS_ExConstantBuffer();

    int alphaFunc = 0;
    for ( auto const& it : MulQuadMarks ) {
        zCMesh* mesh = it.first->GetQuadMesh();
        int numPolys = mesh->GetNumPolygons();
        zCPolygon** polys = mesh->GetPolygons();
        zCMaterial* mat = (numPolys > 0 ? polys[0]->GetMaterial() : it.first->GetMaterial());
        if ( mat ) mat->BindTexture( 0 );

        if ( alphaFunc != mat->GetAlphaFunc() ) {
            // Change alpha-func
            switch ( mat->GetAlphaFunc() ) {
            case zMAT_ALPHA_FUNC_MUL:
                Engine::GAPI->GetRendererState().BlendState.SetModulateBlending();
                break;

            case zMAT_ALPHA_FUNC_MUL2:
                Engine::GAPI->GetRendererState().BlendState.SetModulate2Blending();
                break;

            default:
                continue;
            }

            alphaFunc = mat->GetAlphaFunc();

            Engine::GAPI->GetRendererState().BlendState.SetDirty();
            UpdateRenderStates();
        }

        Engine::GAPI->SetWorldTransformXM( it.first->GetConnectedVob()->GetWorldMatrixXM() );
        SetupVS_ExPerInstanceConstantBuffer();

        DrawVertexBuffer( it.second->Mesh.get(), it.second->NumVertices );
    }
    MulQuadMarks.clear();
}

/** Copies the depth stencil buffer to DepthStencilBufferCopy */
void D3D11GraphicsEngine::CopyDepthStencil() {
    if ( Engine::IsShuttingDown() || !Context || !DepthStencilBuffer || !DepthStencilBufferCopy
        || !DepthStencilBuffer->GetTexture() || !DepthStencilBufferCopy->GetTexture() ) {
        return;
    }
    GetContext()->CopyResource( DepthStencilBufferCopy->GetTexture().Get(), DepthStencilBuffer->GetTexture().Get() );
}

/** Draws underwater effects */
void D3D11GraphicsEngine::DrawUnderwaterEffects() {
    if ( Engine::IsShuttingDown() || !Engine::GAPI || !Context || !PfxRenderer
        || !HDRBackBuffer || !DistortionTexture ) {
        return;
    }
    SetDefaultStates();
    UpdateRenderStates();

    auto Resolution = GetResolution();
    RefractionInfoConstantBuffer ricb = {};
    ricb.RI_Projection = Engine::GAPI->GetProjectionMatrix();
    ricb.RI_ViewportSize = float2( Resolution.x, Resolution.y );
    ricb.RI_Time = Engine::GAPI->GetTimeSeconds();
    ricb.RI_CameraPosition = Engine::GAPI->GetCameraPosition();

    // Set up water final copy
    SetActivePixelShader( PShaderID::PS_PFX_UnderwaterFinal );

    DistortionTexture->BindToPixelShader( 2 );

    PfxRenderer->BlurTexture( HDRBackBuffer.get(), false, 0.10f, UNDERWATER_COLOR_MOD,
        PShaderID::PS_PFX_UnderwaterFinal );
}

/** Returns the settings window availability */
bool D3D11GraphicsEngine::HasSettingsWindow()
{
    return ( Engine::ImGuiHandle && Engine::ImGuiHandle->GetIsActive() );
}

void D3D11GraphicsEngine::EnsureTempVertexBufferSize( std::unique_ptr<D3D11VertexBuffer>& buffer, UINT size ) {
    D3D11_BUFFER_DESC desc;
    buffer->GetVertexBuffer()->GetDesc( &desc );
    if ( desc.ByteWidth < size ) {
        if ( Engine::GAPI->GetRendererState().RendererSettings.EnableDebugLog )
            LogInfo() << "(EnsureTempVertexBufferSize) TempVertexBuffer too small (" << desc.ByteWidth << "), need " << size << " bytes. Recreating buffer.";

        // Buffer too small, recreate it
        buffer.reset( new D3D11VertexBuffer() );
        // Reinit with a bit of a margin, so it will not be reinit each time new vertex is added
        buffer->Init( NULL, size * 2, D3D11VertexBuffer::B_VERTEXBUFFER, D3D11VertexBuffer::U_DYNAMIC, D3D11VertexBuffer::CA_WRITE );
        SetDebugName( buffer->GetShaderResourceView().Get(), "TempVertexBuffer->ShaderResourceView" );
        SetDebugName( buffer->GetVertexBuffer().Get(), "TempVertexBuffer->VertexBuffer" );
    }
}

/** Draws particle meshes */
void D3D11GraphicsEngine::DrawFrameParticleMeshes( std::unordered_map<zCVob*, MeshVisualInfo*>& progMeshes ) {
    if ( progMeshes.empty() ) return;

    SetDefaultStates();

    SetActivePixelShader( PShaderID::PS_Simple );
    SetActiveVertexShader( VShaderID::VS_Ex );

    GothicRendererState& state = Engine::GAPI->GetRendererState();
    state.DepthState.DepthWriteEnabled = false;
    state.DepthState.SetDirty();

    XMMATRIX view = Engine::GAPI->GetViewMatrixXM();
    Engine::GAPI->SetViewTransformXM( view );

    SetupVS_ExMeshDrawCall();
    SetupVS_ExConstantBuffer();

    FXMVECTOR camPos = Engine::GAPI->GetCameraPositionXM();
    int lastBlend = zRND_ALPHA_FUNC_NONE;
    auto vfxRadiusSq = state.RendererSettings.GetEffectiveVisualFXDrawRadius() * state.RendererSettings.GetEffectiveVisualFXDrawRadius();
    auto vVfxRadiusSq = XMVectorReplicate(vfxRadiusSq);
    auto vsBufMPI = ActiveVS->GetBuffer( "Matrices_PerInstances" );
    vsBufMPI.Bind();

    for ( auto const& it : progMeshes ) {
        if ( XMVector3Greater(XMVector3LengthSq( it.first->GetPositionWorldXM() - camPos ), vVfxRadiusSq) ) {
            continue;
        }

        if ( zCParticleFX* particle = reinterpret_cast<zCParticleFX*>(it.first->GetVisual()) ) {
            if ( zCParticleEmitter* emitter = particle->GetEmitter() ) {
                int renderType = emitter->GetVisShpRender();
                if ( !renderType || emitter->GetVisShpType() != 5 )
                    continue;

                int currentBlend = zRND_ALPHA_FUNC_NONE;
                if ( renderType == 2 ) {
                    currentBlend = zRND_ALPHA_FUNC_ADD;
                } else if ( renderType == 3 ) {
                    currentBlend = zRND_ALPHA_FUNC_MUL;
                } else if ( renderType == 4 ) {
                    currentBlend = zRND_ALPHA_FUNC_BLEND;
                }

                if ( lastBlend != currentBlend ) {
                    switch ( currentBlend ) {
                        case zRND_ALPHA_FUNC_ADD: {
                            state.BlendState.SetAdditiveBlending();
                            state.BlendState.SetDirty();
                        } break;
                        case zRND_ALPHA_FUNC_MUL: {
                            state.BlendState.SetModulateBlending();
                            state.BlendState.SetDirty();
                        } break;
                        case zRND_ALPHA_FUNC_BLEND: {
                            state.BlendState.SetAlphaBlending();
                            state.BlendState.SetDirty();
                        } break;
                        default: {
                            state.BlendState.SetDefault();
                            state.BlendState.SetDirty();
                        } break;
                    }

                    lastBlend = currentBlend;
                    UpdateRenderStates();
                }
            } else {
                continue;
            }
        } else {
            continue;
        }

        VS_ExConstantBuffer_PerInstance cbInstance = {};
        cbInstance.World = *it.first->GetWorldMatrixPtr();
        cbInstance.Color = float4( 1.0f, 1.0f, 1.0f, 1.0f );
        vsBufMPI.Update( &cbInstance, sizeof( cbInstance ) );

        void* lastMeshBuffer = nullptr;
        void* lastIndexBuffer = nullptr;
        for ( auto const& itm : it.second->Meshes ) {
            // Cache & bind texture
            zCTexture* texture;
            if ( itm.first && (texture = itm.first->GetTexture()) != nullptr ) {
                if ( texture->CacheIn( 0.6f ) == zRES_CACHED_IN ) {
                    texture->Bind( 0 );
                } else {
                    continue;
                }
            } else {
                continue;
            }
            for ( auto const& itm2nd : itm.second ) {
                if (itm2nd->MeshVertexBuffer != lastMeshBuffer
                    || itm2nd->MeshIndexBuffer != lastIndexBuffer) {
                    // Bind them
                    DrawVertexBufferIndexed(
                        itm2nd->MeshVertexBuffer, itm2nd->MeshIndexBuffer,
                        0 );
                    lastMeshBuffer = itm2nd->MeshVertexBuffer;
                    lastIndexBuffer = itm2nd->MeshIndexBuffer;
                }

                // Draw instances
                DrawVertexBufferIndexed(
                    nullptr, nullptr,
                    itm2nd->Indices.size() );
            }
        }
    }
}

/** Draws particle effects */
void D3D11GraphicsEngine::DrawFrameParticles(
    std::map<ParticleBatchKey, std::vector<ParticleInstanceInfo>>& particles,
    std::map<ParticleBatchKey, ParticleRenderInfo>& info,
    RenderToTextureBuffer* bufferParticleColor,
    RenderToTextureBuffer* bufferParticleDistortion,
    RenderToTextureBuffer* bufferParticleReactiveMask ) {
    if ( particles.empty() ) return;
    SetDefaultStates();

    auto Resolution = GetResolution();

    XMMATRIX view = Engine::GAPI->GetViewMatrixXM();
    Engine::GAPI->SetViewTransformXM( view );  // Update view transform

    // TODO: Maybe make particles draw at a lower res and bilinear upsample the result.

    // Clear GBuffer0 to hold the refraction vectors since it's not needed anymore
    const float clearColor[4] = { 0.f, 0.f, 0.f, 0.f };
    Context->ClearRenderTargetView( bufferParticleColor->GetRenderTargetView().Get(), clearColor );
    Context->ClearRenderTargetView( bufferParticleDistortion->GetRenderTargetView().Get(), clearColor );

    SetActivePixelShader( PShaderID::PS_ParticleDistortion );
    ActivePS->Apply();
    if ( auto sky = Engine::GAPI->GetSky() ) {
        ActivePS->GetBuffer( "Atmosphere" ).Update( &sky->GetAtmosphereCB() ).Bind();
    }

    GothicRendererState& state = Engine::GAPI->GetRendererState();

    state.BlendState.SetAdditiveBlending();
    state.BlendState.SetDirty();

    state.DepthState.DepthWriteEnabled = false;
    state.DepthState.SetDirty();

    state.RasterizerState.CullMode = GothicRasterizerStateInfo::CM_CULL_NONE;
    state.RasterizerState.SetDirty();

    std::vector<std::tuple<zCTexture*, ParticleRenderInfo*, std::vector<ParticleInstanceInfo>*>> pvecAdd;
    std::vector<std::tuple<zCTexture*, ParticleRenderInfo*, std::vector<ParticleInstanceInfo>*>> pvecRest;
    for ( auto&& textureParticle : particles ) {
        if ( textureParticle.second.empty() ) continue;

        ParticleRenderInfo* ri = &info[textureParticle.first];
        zCTexture* texture = textureParticle.first.Texture;
        if ( ri->BlendMode == zRND_ALPHA_FUNC_ADD )
            pvecAdd.push_back( std::make_tuple( texture, ri, &textureParticle.second ) );
        else
            pvecRest.push_back( std::make_tuple( texture, ri, &textureParticle.second ) );
    }

    ID3D11RenderTargetView* rtv[] = {
        bufferParticleColor->GetRenderTargetView().Get(),
        bufferParticleDistortion->GetRenderTargetView().Get() };
    Context->OMSetRenderTargets( 2, rtv, DepthStencilBuffer->GetDepthStencilView().Get() );

    // Bind view/proj
    SetupVS_ExConstantBuffer();

    // Setup GS
    SetActiveVertexShader( VShaderID::VS_ParticlePoint );
    ActiveVS->Apply();

    ParticleGSInfoConstantBuffer gcb = {};
    gcb.CameraPosition = Engine::GAPI->GetCameraPosition();
    ActiveVS->GetBuffer( "ParticleGSInfo" ).Update( &gcb ).Bind();

    // Rendering points only
    Context->IASetPrimitiveTopology( D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP );
    UpdateRenderStates();

    const auto bindParticleTexture = []( zCTexture* texture, const ParticleRenderInfo& renderInfo ) {
        // Use a replacement only when it is fully available; otherwise the
        // original particle texture remains the safe fallback.
        if ( renderInfo.TextureOverrideRequired && !renderInfo.TextureOverride )
            return false;
        zCTexture* textureToBind = renderInfo.TextureOverride
            ? renderInfo.TextureOverride
            : texture;
        if ( textureToBind && textureToBind->CacheIn( 0.6f ) == zRES_CACHED_IN ) {
            textureToBind->Bind( 0 );
            return true;
        }
        return false;
    };

    for ( auto const& textureParticleRenderInfo : pvecAdd ) {
        zCTexture* tx = std::get<0>( textureParticleRenderInfo );
        ParticleRenderInfo& partInfo = *std::get<1>( textureParticleRenderInfo );
        std::vector<ParticleInstanceInfo>& instances = *std::get<2>( textureParticleRenderInfo );

        if ( instances.empty() ) continue;

        if ( !bindParticleTexture( tx, partInfo ) )
            continue;

        // Push data for the particles to the GPU
        EnsureTempVertexBufferSize( TempParticlesVertexBuffer, sizeof( ParticleInstanceInfo ) * instances.size() );
        TempParticlesVertexBuffer->UpdateBuffer( &instances[0], sizeof( ParticleInstanceInfo ) * instances.size() );
        DrawVertexBufferInstanced( TempParticlesVertexBuffer.get(), 4, instances.size(), sizeof( ParticleInstanceInfo ) );
    }

    // Set usual rendering for everything else. Alphablending mostly.
    SetActivePixelShader( PShaderID::PS_ParticleSimple );
    ActivePS->Apply();
    if ( auto sky = Engine::GAPI->GetSky() ) {
        ActivePS->GetBuffer( "Atmosphere" ).Update( &sky->GetAtmosphereCB() ).Bind();
    }

    Context->OMSetRenderTargets( 1, HDRBackBuffer->GetRenderTargetView().GetAddressOf(),
        DepthStencilBuffer->GetDepthStencilView().Get() );

    int lastBlendMode = -1;
    for ( auto const& textureParticleRenderInfo : pvecRest ) {
        zCTexture* tx = std::get<0>( textureParticleRenderInfo );
        ParticleRenderInfo& partInfo = *std::get<1>( textureParticleRenderInfo );
        std::vector<ParticleInstanceInfo>& instances = *std::get<2>( textureParticleRenderInfo );

        if ( instances.empty() ) continue;

        if ( !bindParticleTexture( tx, partInfo ) )
            continue;

        GothicBlendStateInfo& blendState = partInfo.BlendState;

        // This only happens once or twice, since the input list is sorted
        if ( partInfo.BlendMode != lastBlendMode ) {
            // Setup blend state
            state.BlendState = blendState;
            state.BlendState.SetDirty();

            lastBlendMode = partInfo.BlendMode;
            UpdateRenderStates();
        }

        // Push data for the particles to the GPU
        EnsureTempVertexBufferSize( TempParticlesVertexBuffer, sizeof( ParticleInstanceInfo ) * instances.size() );
        TempParticlesVertexBuffer->UpdateBuffer( &instances[0], sizeof( ParticleInstanceInfo ) * instances.size() );
        DrawVertexBufferInstanced( TempParticlesVertexBuffer.get(), 4, instances.size(), sizeof( ParticleInstanceInfo ) );
    }

    Context->IASetPrimitiveTopology( D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST );
    state.BlendState.SetDefault();
    state.BlendState.SetDirty();

    bufferParticleColor->BindToPixelShader( Context.Get(), 1 );
    bufferParticleDistortion->BindToPixelShader( Context.Get(), 2 );

    // Copy scene behind the particle systems
    auto tempBuffer = PfxRenderer->GetTempBuffer();
    PfxRenderer->CopyTextureToRTV(
        HDRBackBuffer->GetShaderResView(),
        tempBuffer->GetRenderTargetView(),
        GetResolution() );

    SetActivePixelShader( PShaderID::PS_PFX_ApplyParticleDistortion );
    ActivePS->Apply();

    // Copy it back, putting distortion behind it
    PfxRenderer->CopyTextureToRTV(
        tempBuffer->GetShaderResView(),
        HDRBackBuffer->GetRenderTargetView(),
        GetResolution(), true, INT2( 0, 0 ),
        bufferParticleReactiveMask ? bufferParticleReactiveMask->GetRenderTargetView().Get() : nullptr );

    GetContext()->PSSetShaderResources( 1, 2, s_nullSRVs );
}

/** Called when a vob was removed from the world */
XRESULT D3D11GraphicsEngine::OnVobRemovedFromWorld( zCVob* vob ) {
    // Remove every deferred occurrence. The queue is non-owning and duplicate
    // entries must not survive after their world light has been destroyed.
    FrameShadowUpdateLights.remove_if( [vob]( const VobLightInfo* light ) {
        return !light || light->Vob == vob;
    } );

    DebugPointlight = nullptr;

    return XR_SUCCESS;
}

/** Saves a screenshot */
void D3D11GraphicsEngine::SaveScreenshot() {
    HRESULT hr;

    // Create new folder if needed
    if ( !Toolbox::FolderExists( "system\\Screenshots" ) ) {
        if ( !Toolbox::CreateDirectoryRecursive( "system\\Screenshots" ) )
            return;
    }
    auto Resolution = GetResolution();
    // Buffer for scaling down the image
    auto rt = std::make_unique<RenderToTextureBuffer>(
        GetDevice().Get(), Resolution.x, Resolution.y, DXGI_FORMAT_ENGINE_DEFAULT  );

    // Downscale to 256x256
    PfxRenderer->CopyTextureToRTV( HDRBackBuffer->GetShaderResView(), rt->GetRenderTargetView() );

    D3D11_TEXTURE2D_DESC texDesc;
    texDesc.ArraySize = 1;
    texDesc.BindFlags = 0;
    texDesc.CPUAccessFlags = 0;
    texDesc.Format = DXGI_FORMAT_ENGINE_DEFAULT;
    texDesc.Width = Resolution.x;   // must be same as backbuffer
    texDesc.Height = Resolution.y;  // must be same as backbuffer
    texDesc.MipLevels = 1;
    texDesc.MiscFlags = 0;
    texDesc.SampleDesc.Count = 1;
    texDesc.SampleDesc.Quality = 0;
    texDesc.Usage = D3D11_USAGE_IMMUTABLE;

    wrl::ComPtr<ID3D11Texture2D> texture;
    LE( GetDevice()->CreateTexture2D( &texDesc, 0, texture.GetAddressOf() ) );
    if ( !texture.Get() ) {
        LogError() << "Could not create texture for screenshot!";
        return;
    }
    GetContext()->CopyResource( texture.Get(), rt->GetTexture().Get() );

    char date[50];
    char time[50];

    // Format the filename
    GetDateFormat( LOCALE_SYSTEM_DEFAULT, 0, nullptr, "yyyy-MM-dd", date, 50 );
    GetTimeFormat( LOCALE_SYSTEM_DEFAULT, 0, nullptr, "hh-mm-ss", time, 50 );

    std::string name = "system\\screenshots\\GD3D11_" + std::string( date ) +
        "__" + std::string( time ) + ".jpg";

    LogInfo() << "Saving screenshot to: " << name;

    // Save the Texture as jpeg using Windows Imaging Component (WIC) with 95% quality.

    LE( SaveWICTextureToFile( GetContext().Get(), texture.Get(), GUID_ContainerFormatJpeg, Toolbox::ToWideChar( name ).c_str(), nullptr, []( IPropertyBag2* props ) {
        PROPBAG2 options[1] = { 0 };
        options[0].pstrName = const_cast<wchar_t*>(L"ImageQuality");

        VARIANT varValues[1];
        varValues[0].vt = VT_R4;
        varValues[0].fltVal = 0.95f;

        props->Write( 1, options, varValues );
        }, false ) );

    // Inform the user that a screenshot has been taken
    Engine::GAPI->PrintMessageTimed( INT2( 30, 30 ), "Screenshot taken: " + name );
}

namespace UI::zFont {
    void AppendGlyphs(
        std::vector<ExVertexStruct>& vertices,
        const std::string& str, size_t strLen,
        float x, float y,
        const ::zFont* font,
        zColor fontColor, float scale = 1.0f, zCCamera* camera = nullptr ) {

        const float SpaceBetweenChars = 1.0f * scale;

        float xpos = x, ypos = y;

        float farZ;
        if ( camera ) farZ = camera->GetNearPlane() + 1.0f;
        else                       farZ = 1.0f;

        vertices.resize( strLen * 6 );
        for ( size_t i = 0; i < strLen; ++i ) {
            const unsigned char& c = str[i];

            auto topLeft = font->fontuv1[c];
            auto botRight = font->fontuv2[c];
            auto widthPx = static_cast<float>( font->width[c] ) * scale;

            ExVertexStruct* vertex = &vertices[i * 6];

            const float widthf = static_cast<float>( widthPx );
            const float heightf = static_cast<float>( font->height ) * scale;

            const float minx = static_cast<float>( xpos );
            const float miny = static_cast<float>( ypos );

            // prepare for next glyph
            if ( c == '\n' ) { ypos += heightf; xpos = x; } else if ( c == ' ' ) { xpos += widthPx; continue; } else { xpos += widthPx + SpaceBetweenChars; }

            const float maxx = (minx + widthf);
            const float maxy = (miny + heightf);

            const float minu = topLeft.pos.x;
            const float maxu = botRight.pos.x;
            const float minv = topLeft.pos.y;
            const float maxv = botRight.pos.y;

            for ( size_t j = 0; j < 6; j++ ) {
                vertex[j].Normal = { 1, 0, 0 };
                vertex[j].TexCoord2 = { 0, 1 };
                vertex[j].Position.z = farZ;
                vertex[j].Color = fontColor.dword;
            }

            vertex[0].Position.x = minx;
            vertex[0].Position.y = miny;
            vertex[0].TexCoord.x = minu;
            vertex[0].TexCoord.y = minv;

            vertex[1].Position.x = maxx;
            vertex[1].Position.y = miny;
            vertex[1].TexCoord.x = maxu;
            vertex[1].TexCoord.y = minv;

            vertex[2].Position.x = maxx;
            vertex[2].Position.y = maxy;
            vertex[2].TexCoord.x = maxu;
            vertex[2].TexCoord.y = maxv;

            vertex[3].Position.x = maxx;
            vertex[3].Position.y = maxy;
            vertex[3].TexCoord.x = maxu;
            vertex[3].TexCoord.y = maxv;

            vertex[4].Position.x = minx;
            vertex[4].Position.y = maxy;
            vertex[4].TexCoord.x = minu;
            vertex[4].TexCoord.y = maxv;

            vertex[5].Position.x = minx;
            vertex[5].Position.y = miny;
            vertex[5].TexCoord.x = minu;
            vertex[5].TexCoord.y = minv;
        }
    }
}


float  D3D11GraphicsEngine::UpdateCustomFontMultiplierFontRendering( float multiplier ) {
    float res = unionCurrentCustomFontMultiplier;
    unionCurrentCustomFontMultiplier = multiplier;
    return res;
}

void D3D11GraphicsEngine::DrawString( const std::string& str, float x, float y, const zFont* font, zColor& fontColor ) {
    if ( !font ) return;
    if ( !font->tex ) return;

    //
    // Glyphen anordnen und in den vertices Vector packen
    // Ggf. Sonderzeichen am Ende entfernen.
    //
    size_t maxLen = str.size();
    while ( maxLen > 0 && str[maxLen - 1] == '/' ) {
        --maxLen;
    }
    if ( !maxLen ) return;

    float UIScale = 1.0f;
    static int savedBarSize = -1;
    if ( oCGame* game = oCGame::GetGame(); game && game->swimBar ) {
        if ( savedBarSize == -1 ) {
            savedBarSize = game->swimBar->psizex;
        }
        UIScale = static_cast<float>(savedBarSize) / 180.f;
    }

    constexpr float FONT_CACHE_PRIO = -1;
    zCTexture* tx = font->tex;

    if ( tx->CacheIn( FONT_CACHE_PRIO ) != zRES_CACHED_IN ) {
        return;
    }

    UIScale *= unionCurrentCustomFontMultiplier;

    //
    // Set alpha blending
    //
    DWORD zrenderer = *reinterpret_cast<DWORD*>(GothicMemoryLocations::GlobalObjects::zRenderer);
    reinterpret_cast<void( __thiscall* )(DWORD, int, int)>(GothicMemoryLocations::zCRndD3D::XD3D_SetRenderState)(zrenderer, 27, 1);
    reinterpret_cast<void( __thiscall* )(DWORD, int, int)>(GothicMemoryLocations::zCRndD3D::XD3D_SetRenderState)(zrenderer, 15, 0);
    reinterpret_cast<void( __thiscall* )(DWORD, int, int)>(GothicMemoryLocations::zCRndD3D::XD3D_SetRenderState)(zrenderer, 19, 5);
    reinterpret_cast<void( __thiscall* )(DWORD, int, int)>(GothicMemoryLocations::zCRndD3D::XD3D_SetRenderState)(zrenderer, 20, 6);

    //
    // Backup old renderstates, BlendState can be ignored here.
    //
    auto oldDepthState = Engine::GAPI->GetRendererState().DepthState.Clone();

    Engine::GAPI->GetRendererState().DepthState.DepthWriteEnabled = false;
    Engine::GAPI->GetRendererState().DepthState.DepthBufferCompareFunc = GothicDepthBufferStateInfo::CF_COMPARISON_ALWAYS;
    Engine::GAPI->GetRendererState().DepthState.SetDirty();

    UpdateRenderStates();

    //
    // Setup Shaders
    //

    SetActiveVertexShader( VShaderID::VS_TransformedEx );
    SetActivePixelShader( PShaderID::PS_FixedFunctionPipe );

    GothicGraphicsState& graphicState = Engine::GAPI->GetRendererState().GraphicsState;
    FixedFunctionStage::EColorOp copyColorOp = graphicState.FF_Stages[0].ColorOp;
    FixedFunctionStage::EColorOp copyColorOp2 = graphicState.FF_Stages[1].ColorOp;
    FixedFunctionStage::ETextureArg copyColorArg1 = graphicState.FF_Stages[0].ColorArg1;
    FixedFunctionStage::ETextureArg copyColorArg2 = graphicState.FF_Stages[0].ColorArg2;
    graphicState.FF_Stages[0].ColorOp = FixedFunctionStage::EColorOp::CO_MODULATE;
    graphicState.FF_Stages[1].ColorOp = FixedFunctionStage::EColorOp::CO_DISABLE;
    graphicState.FF_Stages[0].ColorArg1 = FixedFunctionStage::ETextureArg::TA_TEXTURE;
    graphicState.FF_Stages[0].ColorArg2 = FixedFunctionStage::ETextureArg::TA_DIFFUSE;

    // Bind the FF-Info to the first PS slot
    ActivePS->GetBuffer( "FFPipelineConstantBuffer" ).Update( &graphicState ).Bind();

    BindActiveVertexShader();
    BindActivePixelShader();

    // Set vertex type
    GetContext()->IASetPrimitiveTopology( D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST );

    BindViewportInformation( VShaderID::VS_TransformedEx, 0 );

    //
    // Convert the characters to verticies which mask the Font-Texture alias
    //

    static std::vector<ExVertexStruct> vertices;
    vertices.clear();

    UI::zFont::AppendGlyphs( vertices, str, maxLen, x, y, font, fontColor, UIScale, zCCamera::GetCamera() );

    // Bind the texture.
    tx->Bind( 0 );

    //
    // Populate TempVertexBuffer
    //
    EnsureTempVertexBufferSize( TempVertexBuffer, sizeof( ExVertexStruct ) * vertices.size() );
    TempVertexBuffer->UpdateBuffer( &vertices[0], sizeof( ExVertexStruct ) * vertices.size() );

    //
    // Draw the verticies
    //
    DrawVertexBuffer( TempVertexBuffer.get(), vertices.size(), sizeof( ExVertexStruct ) );

    oldDepthState.ApplyTo( Engine::GAPI->GetRendererState().DepthState );
    Engine::GAPI->GetRendererState().DepthState.SetDirty();

    UpdateRenderStates();

    graphicState.FF_Stages[0].ColorOp = copyColorOp;
    graphicState.FF_Stages[1].ColorOp = copyColorOp2;
    graphicState.FF_Stages[0].ColorArg1 = copyColorArg1;
    graphicState.FF_Stages[0].ColorArg2 = copyColorArg2;
}

void D3D11GraphicsEngine::StorePrevViewProjMatrix() {
    if ( TemporalState ) {
        m_PrevViewProjMatrix = TemporalState->GetUnjitteredViewProj();
    } else {
        XMMATRIX view = Engine::GAPI->GetViewMatrixXM();
        auto projF = Engine::GAPI->GetProjectionMatrix();
        projF._13 = 0;
        projF._23 = 0;
        XMMATRIX viewProj = XMMatrixMultiply( XMLoadFloat4x4( &projF ), view );
        XMStoreFloat4x4( &m_PrevViewProjMatrix, viewProj );
    }
}

void D3D11GraphicsEngine::StoreVobPreviousTransforms() {
    if ( !zCCamera::GetCamera() ) {
        return; // only do this if we actually are in-game
    }

    // Store transforms for static vobs
    for ( VobInfo* vob : RenderedVobs ) {
        vob->StorePreviousTransform();
    }

    // Store transforms for skeletal meshes
    static std::vector<XMFLOAT4X4> transforms;
    for ( SkeletalVobInfo* skelVob : Engine::GAPI->GetAnimatedSkeletalMeshVobs() ) {
        zCModel* model = static_cast<zCModel*>(skelVob->Vob->GetVisual());
        if ( model ) {
            // Store world matrix with scale (same as in DrawSkeletalMeshVob)
            XMMATRIX scale = XMMatrixScalingFromVector( model->GetModelScaleXM() );
            XMMATRIX world = skelVob->Vob->GetWorldMatrixXM() * scale;
            XMStoreFloat4x4( &skelVob->PrevWorldMatrix, world );

            transforms.clear();
            model->GetBoneTransforms( &transforms );
            skelVob->StorePreviousTransforms( transforms );
        }
    }

    for (auto dynVob : Engine::GAPI->GetDynamicallyAddedVobs()) {
        dynVob->StorePreviousTransform();
    }

    // Store view-projection matrix
    StorePrevViewProjMatrix();
}
