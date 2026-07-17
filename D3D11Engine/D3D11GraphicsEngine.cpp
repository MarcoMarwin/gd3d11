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
#include <array>
#include <cmath>
#include <functional>
#include <limits>
#include <new>
#include <span>
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

#ifdef BUILD_SPACER
#define IS_SPACER_BUILD true
#else
#define IS_SPACER_BUILD false
#endif

namespace wrl = Microsoft::WRL;

const float DEFAULT_NORMALMAP_STRENGTH = 0.10f;
const XMFLOAT4 UNDERWATER_COLOR_MOD = XMFLOAT4( 0.5f, 0.7f, 1.0f, 1.0f );

static const GUID IID_IDXGIVkInteropAdapter = { 0x3A6D8F2C, 0xB0E8, 0x4AB4, { 0xB4, 0xDC, 0x4F, 0xD2, 0x48, 0x91, 0xBF, 0xA5 } };
static const GUID IID_IDXGIDeviceRenderDoc = { 0xa7aa6116, 0x9c8d, 0x4bba, { 0x90, 0x83, 0xb4, 0xd8, 0x16, 0xb7, 0x1b, 0x78 } };

constexpr float inv255f = (1.f / 255.f);
float vobAnimation_WindStrength = 1.0f;

constexpr DXGI_FORMAT VERTEX_INDEX_DXGI_FORMAT = sizeof( VERTEX_INDEX ) == sizeof( unsigned short ) ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT;

bool NativeSupport16BitTextures = false;
bool FeatureLevel10Compatibility = false;
bool FeatureRTArrayIndexFromAnyShader = false;

VS_ExConstantBuffer_Wind g_windBuffer;

struct SkyVelocityConstantBuffer {
    XMFLOAT4X4 InvViewProj;
    XMFLOAT4X4 PrevViewProj;
    XMFLOAT2 JitterOffset;
    XMFLOAT2 Padding;
};
static void UpdateCharacterInteractionPositions( VS_ExConstantBuffer_Wind& windBuff ) {
    for ( int i = 0; i < MAX_CHARACTER_INTERACTION_INFLUENCERS; ++i ) {
        windBuff.interactionPositions[i] = float4( 0, 0, 0, 0 );
    }

    if ( Engine::GAPI->GetRendererState()
            .RendererSettings.WindQuality
        == GothicRendererSettings::WIND_QUALITY_NONE ) {
        return;
    }

    zCVob* player = Engine::GAPI->GetPlayerVob();
    if ( !player ) {
        return;
    }

    const XMFLOAT3 playerPosition = player->GetPositionWorld();
    windBuff.interactionPositions[0] = float4( playerPosition.x, playerPosition.y, playerPosition.z, 1.0f );

    // The hero always affects nearby objects, but choose additional NPC
    // influencers around the camera. That keeps the limited influence slots
    // focused on what the player can actually see.
    const XMFLOAT3 interactionSelectionCenter = Engine::GAPI->GetCameraPosition();
    constexpr float NpcInteractionSearchRadius = 1200.0f; // 12 meters in Gothic world units.
    Engine::GAPI->CollectNearbyNpcInteractionPositions(
        interactionSelectionCenter,
        NpcInteractionSearchRadius,
        MAX_CHARACTER_INTERACTION_NPCS,
        &windBuff.interactionPositions[1] );
}

static ID3D11ShaderResourceView* GetParallaxDisplacementSRV( MyDirectDrawSurface7* surface ) {
    const auto& settings = Engine::GAPI->GetRendererState().RendererSettings;
    if ( !surface || !settings.AllowNormalmaps || !settings.EnableParallaxOcclusionMapping
        || !surface->GetNormalmap() || !surface->GetDisplacementmap() ) {
        return nullptr;
    }
    return surface->GetDisplacementmap()->GetShaderResourceView().Get();
}

static MaterialInfo::Buffer GetEffectiveMaterialBuffer( const MaterialInfo* info, MyDirectDrawSurface7* surface ) {
    MaterialInfo defaults;
    MaterialInfo::Buffer buffer = info ? info->buffer : defaults.buffer;

    const auto& settings = Engine::GAPI->GetRendererState().RendererSettings;
    if ( surface && surface->GetDisplacementmap() ) {
        // A present *_disp.dds is the primary opt-in for POM. If old material data
        // contains displacementFactor=0, keep the map testable by falling back to
        // the default material strength instead of silently disabling POM.
        if ( buffer.DisplacementFactor <= 0.0001f ) {
            buffer.DisplacementFactor = defaults.buffer.DisplacementFactor;
        }
        buffer.DisplacementFactor *= std::clamp( settings.ParallaxOcclusionStrength, 0.0f, 4.0f );
    }

    return buffer;
}
static ID3D11ShaderResourceView* GetWetNormalFallbackSRV( MyDirectDrawSurface7* surface, D3D11Texture* distortionTexture ) {
    if ( !surface || !distortionTexture || Engine::GAPI->GetSceneWetness() <= 1e-6f ) {
        return nullptr;
    }

    const auto& settings = Engine::GAPI->GetRendererState().RendererSettings;
    if ( settings.AllowNormalmaps && surface->GetNormalmap() ) {
        return nullptr;
    }

    return distortionTexture->GetShaderResourceView().Get();
}

static void UpdateRefractionViewProjection( RefractionInfoConstantBuffer& buffer ) {
    XMMATRIX view = Engine::GAPI->GetViewMatrixXM();
    XMMATRIX proj = XMLoadFloat4x4( &Engine::GAPI->GetProjectionMatrix() );
    XMStoreFloat4x4( &buffer.RI_ViewProj, XMMatrixMultiply( proj, view ) );
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
        float WM_DisableSSR;
        float WM_DisableRainEffects;
        float WM_OceanWaterTintStrength;
        float WM_IsOceanWater;
        XMFLOAT3 WM_OceanWaterTint;
        float WM_Pad;
    };
    static_assert( sizeof( WaterMaterialInfoConstantBuffer ) == 32 );

    bool TextureNameContainsMarker( const std::string& name, const char* marker ) {
        if ( !marker || !*marker ) {
            return false;
        }

        size_t markerLen = 0;
        while ( marker[markerLen] ) {
            ++markerLen;
        }
        if ( name.size() < markerLen ) {
            return false;
        }

        for ( size_t i = 0; i + markerLen <= name.size(); ++i ) {
            size_t j = 0;
            for ( ; j < markerLen; ++j ) {
                char c = name[i + j];
                if ( c >= 'a' && c <= 'z' ) {
                    c = static_cast<char>(c - 'a' + 'A');
                }
                if ( c != marker[j] ) {
                    break;
                }
            }
            if ( j == markerLen ) {
                return true;
            }
        }
        return false;
    }

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

    bool IsWaterTextureExcludedFromSSR( zCTexture* texture ) {
        if ( !texture ) {
            return false;
        }

        const std::string name = texture->GetNameWithoutExt();
        return TextureNameContainsMarker( name, "WATERFALL" )
            || TextureNameContainsMarker( name, "WASSERFALL" );
    }

    bool IsOceanWaterTexture( zCTexture* texture ) {
        if ( !texture ) {
            return false;
        }

        const std::string name = texture->GetNameWithoutExt();
        if ( TextureNameContainsMarker( name, "RIVER" )
            || TextureNameContainsMarker( name, "FLUSS" )
            || TextureNameContainsMarker( name, "BACH" )
            || TextureNameContainsMarker( name, "STREAM" )
            || TextureNameContainsMarker( name, "WATERFALL" )
            || TextureNameContainsMarker( name, "WASSERFALL" ) ) {
            return false;
        }

        return TextureNameContainsMarker( name, "OCEAN" )
            || TextureNameContainsMarker( name, "SEA" )
            || TextureNameContainsMarker( name, "MEER" )
            || TextureNameContainsMarker( name, "SEAWATER" )
            || TextureNameContainsMarker( name, "HARBOUR" )
            || TextureNameContainsMarker( name, "HARBOR" );
    }

    void FillWaterMaterialInfo( WaterMaterialInfoConstantBuffer& wmcb, zCTexture* texture ) {
        const auto& settings = Engine::GAPI->GetRendererState().RendererSettings;
        wmcb.WM_DisableSSR = IsWaterTextureExcludedFromSSR( texture ) ? 1.0f : 0.0f;
        wmcb.WM_DisableRainEffects = 0.0f;
        wmcb.WM_OceanWaterTintStrength = settings.OceanWaterColorStrength;
        wmcb.WM_IsOceanWater = IsOceanWaterTexture( texture ) ? 1.0f : 0.0f;
        wmcb.WM_OceanWaterTint = settings.OceanWaterColor;
        wmcb.WM_Pad = 0.0f;
    }

    float4 ComputeTransparencyTextureFactor( zCMaterial* material ) {
        const float4 defaultFactor( 1.0f, 1.0f, 1.0f, 1.0f );
        if ( !material ) {
            return defaultFactor;
        }

        if ( material->GetEnvMapEnabled() ) {
            float intensity = material->GetEnvMapStrength() * 0.1f;
            if ( Engine::GAPI ) {
                if ( GSky* sky = Engine::GAPI->GetSky() ) {
                    const float sunHeight = sky->GetAtmosphereCB().AC_LightPos.y;
                    if ( sunHeight > 0.0f ) {
                        const float lerpFactor = std::clamp( sunHeight, 0.0f, 1.0f );
                        intensity = material->GetEnvMapStrength()
                            * std::lerp( 0.1f, 0.7f, lerpFactor );
                    }
                }
            }
            const uint8_t alpha = static_cast<uint8_t>(
                std::clamp( intensity, 0.0f, 1.0f ) * 255.0f );
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
                D3D11VertexBuffer::U_DYNAMIC,
                D3D11VertexBuffer::CA_WRITE,
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
        std::span<const XMFLOAT4X4> matrices,
        const char* debugName
    ) {
        if ( matrices.size() > (std::numeric_limits<UINT>::max)() / sizeof( XMFLOAT4X4 ) ) {
            return false;
        }

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
            return XR_SUCCESS == buffer->UpdateBuffer( const_cast<XMFLOAT4X4*>( &identity ), sizeof( identity ) );
        }

        return XR_SUCCESS == buffer->UpdateBuffer(
            const_cast<XMFLOAT4X4*>(matrices.data()),
            matrixCount * static_cast<UINT>(sizeof( XMFLOAT4X4 )) );
    }

    std::span<const XMFLOAT4X4> SelectPreviousBoneTransforms(
        std::span<const XMFLOAT4X4> current,
        bool hasValidPrevious,
        const std::vector<XMFLOAT4X4>& previous,
        std::array<XMFLOAT4X4, NUM_MAX_BONES>& scratch
    ) {
        if ( !hasValidPrevious || previous.empty() ) {
            return current;
        }

        const size_t copyCount = (std::min)(previous.size(), current.size());
        if ( copyCount == current.size() ) {
            return std::span<const XMFLOAT4X4>( previous.data(), copyCount );
        }

        std::copy_n( previous.begin(), copyCount, scratch.begin() );
        std::copy( current.begin() + static_cast<std::ptrdiff_t>(copyCount), current.end(),
            scratch.begin() + static_cast<std::ptrdiff_t>(copyCount) );
        return std::span<const XMFLOAT4X4>( scratch.data(), current.size() );
    }

    bool IsDrawableSkeletalMesh( const SkeletalMeshInfo* mesh ) {
        return mesh
            && mesh->MeshVertexBuffer
            && mesh->MeshVertexBuffer->IsValid()
            && mesh->MeshIndexBuffer
            && mesh->MeshIndexBuffer->IsValid()
            && !mesh->Indices.empty()
            && mesh->Indices.size() <= (std::numeric_limits<UINT>::max)();
    }

    bool IsDrawableMeshInfo( const MeshInfo* mesh ) {
        if ( !mesh || !mesh->MeshVertexBuffer || !mesh->MeshVertexBuffer->IsValid() ) {
            return false;
        }

        if ( mesh->MeshIndexBuffer ) {
            return mesh->MeshIndexBuffer->IsValid()
                && !mesh->Indices.empty()
                && mesh->Indices.size() <= (std::numeric_limits<UINT>::max)();
        }

        return !mesh->Vertices.empty()
            && mesh->Vertices.size() <= (std::numeric_limits<UINT>::max)();
    }

    bool HasMatchingSkeletalVisOrder(
        std::span<SkeletalVobInfo* const> a,
        const std::vector<SkeletalVobInfo*>& b
    ) {
        return a.size() == b.size() && std::equal( a.begin(), a.end(), b.begin() );
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

XRESULT ConstantBufferPool::Initialize( ID3D11Device* device, uint32_t totalSizeInBytes ) {
    m_pages.clear();
    m_currentPage = 0;
    m_currentOffset = 0;

    if ( !device || totalSizeInBytes == 0 ) {
        return XR_INVALID_ARG;
    }

    const uint64_t pageCount64 = (static_cast<uint64_t>(totalSizeInBytes) + PageSizeInBytes - 1u) / PageSizeInBytes;
    if ( pageCount64 == 0 || pageCount64 > std::numeric_limits<uint32_t>::max() ) {
        return XR_INVALID_ARG;
    }

    m_pages.reserve( static_cast<size_t>(pageCount64) );
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = PageSizeInBytes;
    desc.Usage = D3D11_USAGE_DYNAMIC;
    desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    for ( uint32_t pageIndex = 0; pageIndex < static_cast<uint32_t>(pageCount64); ++pageIndex ) {
        Microsoft::WRL::ComPtr<ID3D11Buffer> page;
        const HRESULT hr = device->CreateBuffer( &desc, nullptr, page.GetAddressOf() );
        if ( FAILED( hr ) ) {
            LogError() << "Failed to create constant-buffer pool page " << pageIndex
                << ": 0x" << std::hex << static_cast<unsigned long>(hr);
            m_pages.clear();
            return XR_FAILED;
        }
#ifdef DEBUG_D3D11
        SetDebugName( page.Get(), "ConstantBufferPool_Page_" + std::to_string( pageIndex ) );
#endif
        m_pages.push_back( std::move( page ) );
    }

    return XR_SUCCESS;
}

void ConstantBufferPool::BeginFrame() {
    m_currentPage = 0;
    m_currentOffset = 0;
}

ConstantBufferAllocation ConstantBufferPool::Allocate( ID3D11DeviceContext* context, const void* pData, uint32_t sizeInBytes ) {
    if ( !context || !pData || m_pages.empty() || sizeInBytes == 0 || sizeInBytes > PageSizeInBytes ) {
        return {};
    }

    const uint64_t alignedSize64 = (static_cast<uint64_t>(sizeInBytes) + 255u) & ~uint64_t(255u);
    if ( alignedSize64 == 0 || alignedSize64 > PageSizeInBytes ) {
        return {};
    }
    const uint32_t alignedSize = static_cast<uint32_t>(alignedSize64);

    if ( m_currentOffset > PageSizeInBytes - alignedSize ) {
        m_currentPage = (m_currentPage + 1u) % static_cast<uint32_t>(m_pages.size());
        m_currentOffset = 0;
    }

    ID3D11Buffer* page = m_pages[m_currentPage].Get();
    D3D11_MAPPED_SUBRESOURCE mappedResource{};
    const D3D11_MAP mapType = m_currentOffset == 0
        ? D3D11_MAP_WRITE_DISCARD
        : D3D11_MAP_WRITE_NO_OVERWRITE;
    const HRESULT hr = context->Map( page, 0, mapType, 0, &mappedResource );
    if ( FAILED( hr ) || !mappedResource.pData ) {
        LogError() << "Failed to map constant-buffer pool page: 0x"
            << std::hex << static_cast<unsigned long>(hr);
        return {};
    }

    byte* destination = static_cast<byte*>(mappedResource.pData) + m_currentOffset;
    memcpy( destination, pData, sizeInBytes );
    if ( alignedSize > sizeInBytes ) {
        memset( destination + sizeInBytes, 0, alignedSize - sizeInBytes );
    }
    context->Unmap( page, 0 );

    ConstantBufferAllocation allocation{ page, m_currentOffset, alignedSize };
    m_currentOffset += alignedSize;
    return allocation;
}

void ConstantBufferPool::EndFrame() {
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
    m_FrameNeedsJitter(false)
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
    // Release post-processing and its vendor contexts before D3D device teardown.
    PfxRenderer.reset();
    GothicDepthBufferStateInfo::DeleteCachedObjects();
    GothicBlendStateInfo::DeleteCachedObjects();
    GothicRasterizerStateInfo::DeleteCachedObjects();

    SAFE_DELETE( InverseUnitSphereMesh );

    SAFE_DELETE( QuadVertexBuffer );
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

    // MemTrackerFinalReport();
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

XRESULT D3D11GraphicsEngine::CreateAndBindDefaultSampler() {
    if ( !GetDevice() || !GetContext() ) {
        return XR_FAILED;
    }

    const INT2 scaledResolution = GetScaledResolution();
    const INT2 backbufferResolution = GetBackbufferResolution();
    if ( scaledResolution.x <= 0 || backbufferResolution.x <= 0 ) {
        LogError() << "Cannot create the default sampler for an invalid render resolution.";
        return XR_INVALID_ARG;
    }

    const float scaleRatio = static_cast<float>(scaledResolution.x)
        / static_cast<float>(backbufferResolution.x);
    if ( !std::isfinite( scaleRatio ) || scaleRatio <= 0.0f ) {
        LogError() << "Cannot create the default sampler for an invalid render scale.";
        return XR_INVALID_ARG;
    }

    // Clamp to zero so supersampling never selects lower-resolution mip levels.
    const float mipBias = (std::min)(0.0f, std::log2( scaleRatio ));

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
    samplerDesc.MinLOD = -FLT_MAX;
    samplerDesc.MaxLOD = FLT_MAX;

    Microsoft::WRL::ComPtr<ID3D11SamplerState> newSampler;
    const HRESULT hr = GetDevice()->CreateSamplerState( &samplerDesc, newSampler.GetAddressOf() );
    if ( FAILED( hr ) || !newSampler ) {
        LogError() << "Failed to create the default sampler: 0x"
            << std::hex << static_cast<unsigned long>(hr);
        return XR_FAILED;
    }

    DefaultSamplerState = std::move( newSampler );
    GetContext()->PSSetSamplers( 0, 1, DefaultSamplerState.GetAddressOf() );
    GetContext()->VSSetSamplers( 0, 1, DefaultSamplerState.GetAddressOf() );
    SetDebugName( DefaultSamplerState.Get(), "DefaultSamplerState" );
    return XR_SUCCESS;
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
        return XR_FAILED;
    }

    PFN_CREATE_DXGI_FACTORY CreateDXGIFactoryFunc = reinterpret_cast<PFN_CREATE_DXGI_FACTORY>( GetProcAddress( dxgiHandle, "CreateDXGIFactory1" ) );
    PFN_CREATE_DXGI_FACTORY2 CreateDXGIFactory2Func = reinterpret_cast<PFN_CREATE_DXGI_FACTORY2>( GetProcAddress( dxgiHandle, "CreateDXGIFactory2") );
    PFN_D3D11_CREATE_DEVICE D3D11CreateDeviceFunc = reinterpret_cast<PFN_D3D11_CREATE_DEVICE>( GetProcAddress( d3d11Handle, "D3D11CreateDevice" ) );
    if ( !D3D11CreateDeviceFunc || ( !CreateDXGIFactory2Func && !CreateDXGIFactoryFunc ) ) {
        LogErrorBox() << "Minimum supported Operating System by GD3D11 is Windows 7 SP1 with Platform Update.";
        return XR_FAILED;
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
        return XR_FAILED;
    }

    bool haveAdapter = false;
    Microsoft::WRL::ComPtr<IDXGIAdapter1> DXGIAdapter1;
    Microsoft::WRL::ComPtr<IDXGIFactory6> DXGIFactory6;
    if ( SUCCEEDED( DXGIFactory2.As( &DXGIFactory6 ) )
        && (factoryFlags & DXGI_CREATE_FACTORY_DEBUG) == 0 ) {
        for ( UINT adapterIndex = 0; ; ++adapterIndex ) {
            Microsoft::WRL::ComPtr<IDXGIAdapter1> candidate;
            const HRESULT enumResult = DXGIFactory6->EnumAdapterByGpuPreference(
                adapterIndex, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                IID_PPV_ARGS( candidate.GetAddressOf() ) );
            if ( enumResult == DXGI_ERROR_NOT_FOUND ) break;
            if ( FAILED( enumResult ) ) {
                LogWarn() << "GPU-preference adapter enumeration failed: 0x"
                    << std::hex << static_cast<unsigned long>(enumResult);
                break;
            }
            DXGI_ADAPTER_DESC1 description{};
            if ( FAILED( candidate->GetDesc1( &description ) )
                || (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0 ) continue;
            DXGIAdapter1 = std::move( candidate );
            haveAdapter = true;
            break;
        }
    }

    if ( !haveAdapter ) {
        uint64_t bestRating = 0;
        for ( UINT adapterIndex = 0; ; ++adapterIndex ) {
            Microsoft::WRL::ComPtr<IDXGIAdapter1> candidate;
            const HRESULT enumResult = DXGIFactory2->EnumAdapters1(
                adapterIndex, candidate.GetAddressOf() );
            if ( enumResult == DXGI_ERROR_NOT_FOUND ) break;
            if ( FAILED( enumResult ) ) {
                LogWarn() << "Adapter enumeration failed: 0x"
                    << std::hex << static_cast<unsigned long>(enumResult);
                break;
            }
            DXGI_ADAPTER_DESC1 description{};
            if ( FAILED( candidate->GetDesc1( &description ) )
                || (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0 ) continue;
            uint64_t rating = static_cast<uint64_t>(description.DedicatedVideoMemory);
            if ( description.VendorId == 0x10DE ) rating += 0x200000000ull;
            else if ( description.VendorId == 0x1002 ) rating += 0x100000000ull;
            if ( !haveAdapter || rating > bestRating ) {
                bestRating = rating;
                DXGIAdapter1 = std::move( candidate );
                haveAdapter = true;
            }
        }
    }
    if ( !haveAdapter ) {
        LogErrorBox() << "Couldn't find any suitable GPU on your device, so it can't run GD3D11!\n"
            "It has to be at least Featurelevel 10.0 compatible, "
            "which requires at least:\n"
            " *	Nvidia GeForce 8xxx or higher\n"
            " *	AMD Radeon HD 2xxx or higher\n\n"
            "The game will now close.";
        return XR_FAILED;
    }

    if ( FAILED( DXGIAdapter1.As( &DXGIAdapter2 ) ) || !DXGIAdapter2 ) {
        LogErrorBox() << "The selected graphics adapter does not expose DXGI 1.2.";
        return XR_FAILED;
    }
    // Find out what we are rendering on to write it into the logfile
    DXGI_ADAPTER_DESC2 adpDesc{};
    if ( FAILED( DXGIAdapter2->GetDesc2( &adpDesc ) ) ) {
        LogErrorBox() << "Could not query the selected graphics adapter.";
        return XR_FAILED;
    }
    std::wstring wDeviceDescription( adpDesc.Description );
    std::string deviceDescription( wDeviceDescription.begin(), wDeviceDescription.end() );
    DeviceDescription = deviceDescription;
    LogInfo() << "Rendering on: " << deviceDescription.c_str();

    Microsoft::WRL::ComPtr<IUnknown> dxgiVKInterop;
    const HRESULT interopResult = DXGIAdapter2->QueryInterface(
        IID_IDXGIVkInteropAdapter, reinterpret_cast<void**>(dxgiVKInterop.GetAddressOf()) );
    const bool dxvkAvailable = SUCCEEDED( interopResult ) && dxgiVKInterop;

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

    const size_t featureLevelCount = std::size( featureLevels );
    if ( agsDevice ) {
        for ( size_t i = 0; i < featureLevelCount; ++i ) {
            hr = agsDevice->CreateD3D11Device( DXGIAdapter2.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, &featureLevels[i], static_cast<UINT>(featureLevelCount - i),
                D3D11_SDK_VERSION, Device11.ReleaseAndGetAddressOf(), &maxFeatureLevel,
                Context11.ReleaseAndGetAddressOf() );
            if ( SUCCEEDED( hr ) ) {
                break;
            }

            maxFeatureLevel = D3D_FEATURE_LEVEL::D3D_FEATURE_LEVEL_9_1;
        }
    } else {
        for ( size_t i = 0; i < featureLevelCount; ++i ) {
            hr = D3D11CreateDeviceFunc( DXGIAdapter2.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, &featureLevels[i], static_cast<UINT>(featureLevelCount - i),
                D3D11_SDK_VERSION, Device11.ReleaseAndGetAddressOf(), &maxFeatureLevel,
                Context11.ReleaseAndGetAddressOf() );
            if ( SUCCEEDED( hr ) ) {
                break;
            }

            maxFeatureLevel = D3D_FEATURE_LEVEL::D3D_FEATURE_LEVEL_9_1;
        }
    }

    if ( FAILED( hr ) || !Device11 || !Context11 ) {
        LogErrorBox() << "D3D11CreateDevice failed with code: " << std::hex << hr << "!";
        return XR_FAILED;
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
        return XR_FAILED;
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


    if ( FAILED( Device11.As( &Device ) ) || !Device
        || FAILED( Context11.As( &Context ) ) || !Context ) {
        LogErrorBox() << "The Direct3D 11.1 device interfaces are unavailable.";
        return XR_FAILED;
    }
    s_tracyD3D11Ctx = TracyD3D11Context( Device.Get(), Context.Get() );

    Context.As( &m_UserDefinedAnnotation );

    // Check for windows 10 - pretend 8 doesn't exist because I can't verify if they actually works on windows 8
    // and you can't trust Microsoft feature level documentation
    NativeSupport16BitTextures = Toolbox::IsWindowsVersionOrGreater( HIBYTE( _WIN32_WINNT_WIN10 ), LOBYTE( _WIN32_WINNT_WIN10 ), 0 );
    FeatureLevel10Compatibility = (maxFeatureLevel < D3D_FEATURE_LEVEL::D3D_FEATURE_LEVEL_11_0);

    if ( Engine::GAPI->GetRendererState().RendererSettings.DebugSettings.FeatureSet.ForceFeatureLevel10 ) {
        FeatureLevel10Compatibility = true;
    }
    FetchDisplayModeList();

    ComPtr<IUnknown> renderdoc = nullptr;
    const HRESULT result = Device.AsIID( IID_IDXGIDeviceRenderDoc, &renderdoc );
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

    D3D11_FEATURE_DATA_D3D11_OPTIONS3 options3{};
    hr = Device->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS3, &options3, sizeof( options3 ) );
    if ( SUCCEEDED( hr ) ) {
        FeatureRTArrayIndexFromAnyShader = options3.VPAndRTArrayIndexFromAnyShaderFeedingRasterizer;
        Engine::GAPI->GetRendererState().RendererSettings.DebugSettings.FeatureSet.UseLayeredRendering = FeatureRTArrayIndexFromAnyShader;
    }

    LogInfo() << "Creating ShaderManager";
    ShaderManager = std::make_unique<D3D11ShaderManager>();
    if ( ShaderManager->Init() != XR_SUCCESS || ShaderManager->LoadShaders() != XR_SUCCESS ) {
        LogError() << "Renderer initialization aborted because one or more required shaders failed to load.";
        return XR_FAILED;
    }

    auto initializeDynamicBuffer = [&]( std::unique_ptr<D3D11VertexBuffer>& destination,
                                        UINT byteSize, const char* debugName ) -> bool {
        auto buffer = std::make_unique<D3D11VertexBuffer>();
        if ( buffer->Init( nullptr, byteSize, D3D11VertexBuffer::B_VERTEXBUFFER,
                D3D11VertexBuffer::U_DYNAMIC, D3D11VertexBuffer::CA_WRITE ) != XR_SUCCESS ) {
            LogError() << "Failed to create required dynamic vertex buffer: " << debugName;
            return false;
        }
        SetDebugName( buffer->GetShaderResourceView().Get(), std::string( debugName ) + "_SRV" );
        SetDebugName( buffer->GetVertexBuffer().Get(), debugName );
        destination = std::move( buffer );
        return true;
    };

    if ( !initializeDynamicBuffer( TempVertexBuffer, DRAWVERTEXARRAY_BUFFER_SIZE, "TempVertexBuffer" )
        || !initializeDynamicBuffer( TempPolysVertexBuffer, POLYS_BUFFER_SIZE, "TempPolysVertexBuffer" )
        || !initializeDynamicBuffer( TempParticlesVertexBuffer, PARTICLES_BUFFER_SIZE, "TempParticlesVertexBuffer" )
        || !initializeDynamicBuffer( TempMorphedMeshSmallVertexBuffer, MORPHEDMESH_SMALL_BUFFER_SIZE,
            "TempMorphedMeshSmallVertexBuffer" )
        || !initializeDynamicBuffer( TempMorphedMeshBigVertexBuffer, MORPHEDMESH_HIGH_BUFFER_SIZE,
            "TempMorphedMeshBigVertexBuffer" )
        || !initializeDynamicBuffer( TempHUDVertexBuffer, HUD_BUFFER_SIZE, "TempHUDVertexBuffer" )
        || !initializeDynamicBuffer( DynamicInstancingBuffer, INSTANCING_BUFFER_SIZE, "DynamicInstancingBuffer" )
        || !initializeDynamicBuffer( NodeAttachmentInstancingBuffer,
            static_cast<UINT>(sizeof( NodeAttachmentInstanceData ) * 1024), "NodeAttachmentInstancingBuffer" )
        || !initializeDynamicBuffer( DecalInstancingBuffer,
            static_cast<UINT>(sizeof( XMFLOAT4X4 ) * 1024), "DecalInstancingBuffer" ) ) {
        return XR_FAILED;
    }

    if ( CreateAndBindDefaultSampler() != XR_SUCCESS ) {
        return XR_FAILED;
    }

    auto createSampler = [&]( const D3D11_SAMPLER_DESC& descriptor,
                              Microsoft::WRL::ComPtr<ID3D11SamplerState>& destination,
                              const char* debugName ) -> bool {
        Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler;
        const HRESULT samplerResult = GetDevice()->CreateSamplerState(
            &descriptor, sampler.GetAddressOf() );
        if ( FAILED( samplerResult ) || !sampler ) {
            LogError() << "Failed to create sampler " << debugName << ": 0x"
                << std::hex << static_cast<unsigned long>(samplerResult);
            return false;
        }
        SetDebugName( sampler.Get(), debugName );
        destination = std::move( sampler );
        return true;
    };

    D3D11_SAMPLER_DESC samplerDesc{};
    samplerDesc.Filter = D3D11_FILTER_ANISOTROPIC;
    samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
    samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
    samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    samplerDesc.MaxAnisotropy = 16;
    samplerDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    samplerDesc.BorderColor[0] = 1.0f;
    samplerDesc.BorderColor[1] = 1.0f;
    samplerDesc.BorderColor[2] = 1.0f;
    samplerDesc.BorderColor[3] = 1.0f;
    samplerDesc.MinLOD = -FLT_MAX;
    samplerDesc.MaxLOD = FLT_MAX;
    if ( !createSampler( samplerDesc, LinearSamplerState, "LinearSamplerState" ) ) {
        return XR_FAILED;
    }

    samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.MaxAnisotropy = 1;
    if ( !createSampler( samplerDesc, ClampSamplerState, "ClampSamplerState" ) ) {
        return XR_FAILED;
    }

    samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
    samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
    samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    if ( !createSampler( samplerDesc, CubeSamplerState, "CubeSamplerState" ) ) {
        return XR_FAILED;
    }
    SetActivePixelShader( PShaderID::PS_Simple );
    SetActiveVertexShader( VShaderID::VS_Ex );

    auto loadRequiredTexture = []( std::unique_ptr<D3D11Texture>& destination,
                                   const char* fileName ) -> bool {
        auto texture = std::make_unique<D3D11Texture>();
        if ( texture->Init( fileName ) != XR_SUCCESS || !texture->IsValid() ) {
            LogError() << "Failed to load required renderer texture: " << fileName;
            return false;
        }
        destination = std::move( texture );
        return true;
    };

    if ( !loadRequiredTexture( DistortionTexture, "system\\GD3D11\\textures\\distortion.dds" )
        || !loadRequiredTexture( BlueNoise512BGRA,
            "system\\GD3D11\\textures\\bluenoise-rgba-512-bgra.dds" ) ) {
        return XR_FAILED;
    }

    auto whiteTexture = std::make_unique<D3D11Texture>();
    uint32_t whitePixel = 0xFFFFFFFF;
    if ( whiteTexture->Init( { 1, 1 }, D3D11Texture::ETextureFormat::TF_B8G8R8A8,
            1, &whitePixel, "FULL_WHITE_ALPHA_OPAQUE.static-memory" ) != XR_SUCCESS
        || !whiteTexture->IsValid() ) {
        LogError() << "Failed to create the required white fallback texture.";
        return XR_FAILED;
    }
    WhiteTexture = std::move( whiteTexture );

    auto inverseUnitSphereMesh = std::make_unique<GMesh>();
    if ( inverseUnitSphereMesh->LoadMesh(
            "system\\GD3D11\\meshes\\icoSphere.obj" ) != XR_SUCCESS ) {
        LogError() << "Failed to load the required inverse unit sphere mesh.";
        return XR_FAILED;
    }
    InverseUnitSphereMesh = inverseUnitSphereMesh.release();

    static float4 infiniteRange( FLT_MAX, 0, 0, 0 );
    auto infiniteRangeConstantBuffer = std::make_unique<D3D11ConstantBuffer>(
        sizeof( float4 ), &infiniteRange );
    auto outdoorSmallVobsConstantBuffer = std::make_unique<D3D11ConstantBuffer>(
        sizeof( float4 ), nullptr );
    auto outdoorVobsConstantBuffer = std::make_unique<D3D11ConstantBuffer>(
        sizeof( float4 ), nullptr );
    if ( !infiniteRangeConstantBuffer->IsValid()
        || !outdoorSmallVobsConstantBuffer->IsValid()
        || !outdoorVobsConstantBuffer->IsValid() ) {
        LogError() << "Failed to create required distance constant buffers.";
        return XR_FAILED;
    }
    InfiniteRangeConstantBuffer = std::move( infiniteRangeConstantBuffer );
    OutdoorSmallVobsConstantBuffer = std::move( outdoorSmallVobsConstantBuffer );
    OutdoorVobsConstantBuffer = std::move( outdoorVobsConstantBuffer );

    PerObjectMaterialInfoPooledBuffer = std::make_unique<ConstantBufferPool>();
    if ( PerObjectMaterialInfoPooledBuffer->Initialize( GetDevice().Get() ) != XR_SUCCESS ) {
        LogError() << "Renderer initialization aborted because the material constant-buffer pool could not be created.";
        return XR_FAILED;
    }
    SetDebugName( InfiniteRangeConstantBuffer->Get().Get(), "InfiniteRangeConstantBuffer" );
    SetDebugName( OutdoorSmallVobsConstantBuffer->Get().Get(), "OutdoorSmallVobsConstantBuffer" );
    SetDebugName( OutdoorVobsConstantBuffer->Get().Get(), "OutdoorVobsConstantBuffer" );
    // Load reflectioncube

    if ( S_OK != CreateDDSTextureFromFile(
        GetDevice().Get(), L"system\\GD3D11\\Textures\\reflect_cube.dds",
        nullptr,
        ReflectionCube.GetAddressOf() ) )
        LogWarn()
        << "Failed to load file: system\\GD3D11\\Textures\\reflect_cube.dds";

    if ( S_OK != CreateDDSTextureFromFile(
        GetDevice().Get(), L"system\\GD3D11\\Textures\\SkyCubemap2.dds",
        nullptr, ReflectionCube2.GetAddressOf() ) )
        LogWarn()
        << "Failed to load file: system\\GD3D11\\Textures\\SkyCubemap2.dds";

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

    auto quadVertexBuffer = std::make_unique<D3D11VertexBuffer>();
    if ( quadVertexBuffer->Init( vx, static_cast<UINT>(sizeof( vx )),
            D3D11VertexBuffer::B_VERTEXBUFFER, D3D11VertexBuffer::U_IMMUTABLE ) != XR_SUCCESS ) {
        LogError() << "Failed to create the screen-quad vertex buffer.";
        return XR_FAILED;
    }

    VERTEX_INDEX indices[] = { 0, 1, 2, 3, 4, 5 };
    auto quadIndexBuffer = std::make_unique<D3D11VertexBuffer>();
    if ( quadIndexBuffer->Init( indices, static_cast<UINT>(sizeof( indices )),
            D3D11VertexBuffer::B_INDEXBUFFER, D3D11VertexBuffer::U_IMMUTABLE ) != XR_SUCCESS ) {
        LogError() << "Failed to create the screen-quad index buffer.";
        return XR_FAILED;
    }
    QuadVertexBuffer = quadVertexBuffer.release();
    QuadIndexBuffer = quadIndexBuffer.release();
    // Create shadow map manager
    auto shadowMaps = std::make_unique<D3D11ShadowMap>();
    const int initialShadowSize = Engine::GAPI->GetRendererState().RendererSettings.ShadowMapSize;
    if ( shadowMaps->Init( Device, Context, initialShadowSize ) != XR_SUCCESS ) {
        LogError() << "Failed to initialize the shadow-map manager.";
        return XR_FAILED;
    }
    ShadowMaps = std::move( shadowMaps );

    // Select active scene renderer based on setting
    SelectActiveRenderer();

    SteamOverlay::Init();

    if ( Effects->LoadRainResources() != XR_SUCCESS ) {
        LogWarn() << "Rain resources are unavailable; rain rendering has been disabled.";
        Engine::GAPI->GetRendererState().RendererSettings.EnableRain = false;
    }

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
    if ( !hWnd || !IsWindow( hWnd ) ) return XR_INVALID_ARG;
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

        INT2 res = Resolution;

#ifdef BUILD_SPACER
        RECT r;
        GetClientRect( hWnd, &r );

        res.x = r.right;
        res.y = r.bottom;
#endif
        if ( res.x > 0 && res.y > 0 && OnResize( res ) != XR_SUCCESS ) {
            OutputWindow = nullptr;
            return XR_FAILED;
        }

#ifndef BUILD_SPACER_NET

        // We need to update clip cursor here because we hook the window too late to receive proper window message
        UpdateClipCursor( hWnd );

        // Force hide mouse cursor
        while ( ShowCursor( false ) >= 0 );
#endif
    }

    return XR_SUCCESS;
}

/** Get BackBuffer Format */
DXGI_FORMAT D3D11GraphicsEngine::GetBackBufferFormat() {
    return Engine::GAPI->GetRendererState().RendererSettings.CompressBackBuffer ? DXGI_FORMAT_R11G11B10_FLOAT : DXGI_FORMAT_R16G16B16A16_FLOAT;
}

void D3D11GraphicsEngine::OnResetBackBuffer() {
    const INT2 resolution = GetResolution();
    if ( resolution.x <= 0 || resolution.y <= 0 || !GetDevice() ) return;

    const uint32_t bindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE
        | (GetDevice()->GetFeatureLevel() >= D3D_FEATURE_LEVEL_11_0
            ? D3D11_BIND_UNORDERED_ACCESS : 0);
    auto hdrBackbuffer = std::make_unique<RenderToTextureBuffer>(
        GetDevice().Get(), resolution.x, resolution.y, GetBackBufferFormat(),
        nullptr, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, 1, 1, bindFlags );
    if ( !hdrBackbuffer || !hdrBackbuffer->IsValid() ) return;

    HDRBackBuffer = std::move( hdrBackbuffer );
    if ( PfxRenderer ) PfxRenderer->OnResize( resolution );
    SetDebugName( HDRBackBuffer->GetShaderResView().Get(), "Backbuffer->ShaderResourceView" );
    SetDebugName( HDRBackBuffer->GetRenderTargetView().Get(), "Backbuffer->RenderTargetView" );
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
    const INT2 backbufferResolution = GetBackbufferResolution();
    const INT2 previousScaledResolution = m_scaledResolution;
    static INT2 lastRoundedTextureResolution{};

    auto resolutionScalePct = Engine::GAPI->GetRendererState().RendererSettings.ResolutionScalePercent;
    if ( resolutionScalePct != 100 ) {
        resolutionScalePct = std::clamp( resolutionScalePct, 25, 200 );
        Engine::GAPI->GetRendererState().RendererSettings.ResolutionScalePercent = resolutionScalePct;
        const float scale = static_cast<float>(resolutionScalePct) / 100.0f;
        m_scaledResolution = INT2{
            static_cast<INT>(static_cast<float>(backbufferResolution.x) * scale),
            static_cast<INT>(static_cast<float>(backbufferResolution.y) * scale)
        };
    } else {
        m_scaledResolution = backbufferResolution;
    }

    const INT2 roundedTextureResolution = GetResolution();
    if ( roundedTextureResolution.x <= 0 || roundedTextureResolution.y <= 0
        || Resolution.x <= 0 || Resolution.y <= 0 || !GetDevice() || !GetContext() ) {
        m_scaledResolution = previousScaledResolution;
        return XR_INVALID_ARG;
    }
    const bool renderBuffersAlive = DepthStencilBuffer && DepthStencilBuffer->IsValid()
        && DepthStencilBufferCopy && DepthStencilBufferCopy->IsValid()
        && VelocityBuffer && VelocityBuffer->IsValid() && Backbuffer && Backbuffer->IsValid()
        && m_SwapchainDepthStencilBuffer && m_SwapchainDepthStencilBuffer->IsValid()
        && HDRBackBuffer && HDRBackBuffer->IsValid();
    if ( lastRoundedTextureResolution == roundedTextureResolution && renderBuffersAlive ) {
        return XR_SUCCESS;
    }
    if ( CreateAndBindDefaultSampler() != XR_SUCCESS ) {
        m_scaledResolution = previousScaledResolution;
        return XR_FAILED;
    }

    auto depthStencil = std::make_unique<RenderToDepthStencilBuffer>(
        GetDevice().Get(), roundedTextureResolution.x, roundedTextureResolution.y,
        DXGI_FORMAT_R32_TYPELESS, nullptr, DXGI_FORMAT_D32_FLOAT, DXGI_FORMAT_R32_FLOAT );
    auto depthCopy = std::make_unique<RenderToTextureBuffer>(
        GetDevice().Get(), roundedTextureResolution.x, roundedTextureResolution.y,
        DXGI_FORMAT_R32_TYPELESS, nullptr, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32_FLOAT );
    auto velocity = std::make_unique<RenderToTextureBuffer>(
        GetDevice().Get(), roundedTextureResolution.x, roundedTextureResolution.y,
        DXGI_FORMAT_R16G16_FLOAT );
    auto hdrBackbuffer = std::make_unique<RenderToTextureBuffer>(
        GetDevice().Get(), roundedTextureResolution.x, roundedTextureResolution.y,
        GetBackBufferFormat(), nullptr, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, 1, 1,
        D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE
            | (Device->GetFeatureLevel() >= D3D_FEATURE_LEVEL_11_0
                ? D3D11_BIND_UNORDERED_ACCESS : 0) );
    auto backbuffer = std::make_unique<RenderToTextureBuffer>(
        GetDevice().Get(), Resolution.x, Resolution.y, DXGI_FORMAT_ENGINE_SWAPCHAIN, nullptr,
        DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, 1, 1,
        D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE
            | (Device->GetFeatureLevel() >= D3D_FEATURE_LEVEL_11_0
                ? D3D11_BIND_UNORDERED_ACCESS : 0) );
    auto swapchainDepth = std::make_unique<RenderToDepthStencilBuffer>(
        GetDevice().Get(), Resolution.x, Resolution.y, DXGI_FORMAT_R32_TYPELESS, nullptr,
        DXGI_FORMAT_D32_FLOAT, DXGI_FORMAT_R32_FLOAT );
    if ( !depthStencil->IsValid() || !depthCopy->IsValid() || !velocity->IsValid()
        || !hdrBackbuffer->IsValid() || !backbuffer->IsValid() || !swapchainDepth->IsValid() ) {
        m_scaledResolution = previousScaledResolution;
        LogError() << "Failed to recreate one or more required render targets.";
        return XR_FAILED;
    }

    const int shadowMapSize = std::min<int>(
        std::max<int>( Engine::GAPI->GetRendererState().RendererSettings.ShadowMapSize, 512 ),
        FeatureLevel10Compatibility ? 8192 : 16384 );
    std::unique_ptr<D3D11ShadowMap> newShadowMaps;
    if ( !ShadowMaps ) {
        newShadowMaps = std::make_unique<D3D11ShadowMap>();
        if ( newShadowMaps->Init( Device, Context, shadowMapSize ) != XR_SUCCESS ) {
            m_scaledResolution = previousScaledResolution;
            return XR_FAILED;
        }
    } else if ( ShadowMaps->Resize( shadowMapSize ) != XR_SUCCESS ) {
        m_scaledResolution = previousScaledResolution;
        return XR_FAILED;
    }

    DepthStencilBuffer = std::move( depthStencil );
    DepthStencilBufferCopy = std::move( depthCopy );
    VelocityBuffer = std::move( velocity );
    HDRBackBuffer = std::move( hdrBackbuffer );
    Backbuffer = std::move( backbuffer );
    m_SwapchainDepthStencilBuffer = std::move( swapchainDepth );
    if ( newShadowMaps ) ShadowMaps = std::move( newShadowMaps );
    if ( !PfxRenderer ) PfxRenderer = std::make_unique<D3D11PfxRenderer>();
    PfxRenderer->OnResize( roundedTextureResolution );

    SetDebugName( VelocityBuffer->GetTexture().Get(), "VelocityBuffer->TEX" );
    SetDebugName( VelocityBuffer->GetShaderResView().Get(), "VelocityBuffer->SRV" );
    SetDebugName( VelocityBuffer->GetRenderTargetView().Get(), "VelocityBuffer->RTV" );
    SetDebugName( Backbuffer->GetTexture().Get(), "Backbuffer->TEX" );
    SetDebugName( Backbuffer->GetShaderResView().Get(), "Backbuffer->SRV" );
    SetDebugName( Backbuffer->GetRenderTargetView().Get(), "Backbuffer->RTV" );
    SetDebugName( HDRBackBuffer->GetShaderResView().Get(), "Backbuffer->ShaderResourceView" );
    SetDebugName( HDRBackBuffer->GetRenderTargetView().Get(), "Backbuffer->RenderTargetView" );
    lastRoundedTextureResolution = roundedTextureResolution;
    return XR_SUCCESS;
}
/** Called on window resize/resolution change */
XRESULT D3D11GraphicsEngine::OnResize( INT2 newSize ) {
    if ( newSize.x <= 0 || newSize.y <= 0 || !Engine::GAPI || !GetDevice()
        || !GetContext() || !DXGIFactory2 || !OutputWindow ) return XR_INVALID_ARG;
    HRESULT hr = E_FAIL;
    const INT2 previousResolution = Resolution;
    const INT2 previousNewResolution = NewResolution;
    const INT2 previousSwapchainResolution = m_swapchainResolution;

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

    auto restorePreviousState = [&]() {
        Resolution = previousResolution;
        NewResolution = previousNewResolution;
        m_swapchainResolution = previousSwapchainResolution;
        SyncGothicResolutionState( true );
        if ( !SwapChain ) return;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> view;
        if ( SUCCEEDED( SwapChain->GetBuffer( 0, IID_PPV_ARGS( texture.GetAddressOf() ) ) )
            && SUCCEEDED( GetDevice()->CreateRenderTargetView(
                texture.Get(), nullptr, view.GetAddressOf() ) ) ) {
            BackbufferRTV = std::move( view );
        }
    };

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

    UINT scflags = 0;
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

        Microsoft::WRL::ComPtr<IDXGISwapChain1> newSwapChain;
        hr = DXGIFactory2->CreateSwapChainForHwnd( GetDevice().Get(), OutputWindow,
            &scd, nullptr, nullptr, newSwapChain.GetAddressOf() );
        if ( FAILED( hr ) || !newSwapChain ) {
            LogError() << "Failed to create swapchain. HRESULT: " << std::hex << hr;
            restorePreviousState();
            return XR_FAILED;
        }
        SwapChain = std::move( newSwapChain );

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

        // Need to init AntTweakBar now that we have a working swapchain
        // XLE( Engine::AntTweakBar->Init() );

        Engine::ImGuiHandle->Init( OutputWindow, GetDevice(), GetContext() );
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
            if ( WaitForSingleObject( frameLatencyWaitableObject, 1000 ) != WAIT_OBJECT_0 ) {
                LogWarn() << "Low-latency wait timed out; disabling the waitable path.";
                CloseHandle( frameLatencyWaitableObject );
                frameLatencyWaitableObject = nullptr;
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
    wrl::ComPtr<ID3D11RenderTargetView> backbufferView;
    m_HDR = Engine::GAPI->GetRendererState().RendererSettings.HDR_Monitor;
    UpdateColorSpace_SwapChain();
    hr = SwapChain->GetBuffer( 0, IID_PPV_ARGS( backbuffer.GetAddressOf() ) );
    if ( FAILED( hr ) || !backbuffer ) {
        LogError() << "Could not acquire the resized swapchain buffer. HRESULT: "
            << std::hex << hr;
        return XR_FAILED;
    }
    hr = GetDevice()->CreateRenderTargetView(
        backbuffer.Get(), nullptr, backbufferView.GetAddressOf() );
    if ( FAILED( hr ) || !backbufferView ) {
        LogError() << "Could not create the swapchain render target. HRESULT: "
            << std::hex << hr;
        return XR_FAILED;
    }
    BackbufferRTV = std::move( backbufferView );
    if ( RecreateBuffers() != XR_SUCCESS ) return XR_FAILED;

    // Bind our newly created resources
    GetContext()->OMSetRenderTargets( 1, HDRBackBuffer->GetRenderTargetView().GetAddressOf(),
        DepthStencilBuffer->GetDepthStencilView().Get() );

    // Set the viewport
    SetViewport( ViewportInfo( 0, 0, m_scaledResolution.x, m_scaledResolution.y ) );

    // Engine::AntTweakBar->OnResize( newSize );
    Engine::ImGuiHandle->OnResize( newSize );


    return XR_SUCCESS;
}

void D3D11GraphicsEngine::ResetFrameTransientBufferPools() {
    m_MainWorldIndirectPool.ResetFrame();
    m_ShadowWorldIndirectPool.ResetFrame();
    m_MainVobInstancingPool.ResetFrame();
    m_ShadowVobInstancingPool.ResetFrame();
    PerObjectMaterialInfoPooledBuffer->BeginFrame();
}

D3D11IndirectBuffer* D3D11GraphicsEngine::AcquireFrameIndirectBuffer( FrameIndirectBufferPool& pool,
    const void* initData,
    unsigned int sizeInBytes,
    const char* debugName ) {
    if ( !initData || sizeInBytes == 0 ) return nullptr;

    if ( pool.NextBuffer >= pool.Buffers.size() ) {
        pool.Buffers.push_back( std::make_unique<D3D11IndirectBuffer>() );
    }

    auto& buffer = pool.Buffers[pool.NextBuffer];
    if ( !buffer ) {
        buffer = std::make_unique<D3D11IndirectBuffer>();
    }

    const bool needsRecreate = !buffer->IsValid() || buffer->GetSizeInBytes() < sizeInBytes;
    if ( needsRecreate ) {
        if ( buffer->Init( const_cast<void*>(initData), sizeInBytes,
            D3D11IndirectBuffer::B_INDEXBUFFER,
            D3D11IndirectBuffer::U_DYNAMIC,
            D3D11IndirectBuffer::CA_WRITE,
            debugName ? debugName : "" ) != XR_SUCCESS ) {
            return nullptr;
        }

        if ( Engine::GAPI
            && Engine::GAPI->GetRendererState().RendererSettings.EnableDebugLog ) {
            LogInfo() << "(Re-)created new frame indirect buffer: "
                << (debugName ? debugName : "<unnamed>");
        }
    } else if ( buffer->UpdateBuffer(
        const_cast<void*>(initData), sizeInBytes ) != XR_SUCCESS ) {
        return nullptr;
    }

    ++pool.NextBuffer;
    return buffer.get();
}

D3D11VertexBuffer* D3D11GraphicsEngine::AcquireFrameInstancingBuffer( FrameInstancingBufferPool& pool,
    unsigned int sizeInBytes,
    const char* debugName ) {
    if ( sizeInBytes == 0 ) return nullptr;

    if ( pool.NextBuffer >= pool.Buffers.size() ) {
        pool.Buffers.push_back( std::make_unique<D3D11VertexBuffer>() );
    }

    auto& buffer = pool.Buffers[pool.NextBuffer];
    if ( !buffer ) {
        buffer = std::make_unique<D3D11VertexBuffer>();
    }

    if ( !buffer->IsValid() || buffer->GetSizeInBytes() < sizeInBytes ) {
        if ( buffer->Init( nullptr, sizeInBytes,
            D3D11VertexBuffer::B_VERTEXBUFFER,
            D3D11VertexBuffer::U_DYNAMIC,
            D3D11VertexBuffer::CA_WRITE,
            debugName ? debugName : "" ) != XR_SUCCESS ) {
            return nullptr;
        }
        if ( Engine::GAPI
            && Engine::GAPI->GetRendererState().RendererSettings.EnableDebugLog ) {
            LogInfo() << "(Re-)created new frame instancing buffer: "
                << (debugName ? debugName : "FrameInstancingBuffer");
        }
        SetDebugName( buffer->GetVertexBuffer().Get(),
            debugName ? debugName : "FrameInstancingBuffer" );
    }

    ++pool.NextBuffer;
    return buffer.get();
}
static const char* beginFrameEventName = "Frame";

/** Called when the game wants to render a new frame */
XRESULT D3D11GraphicsEngine::OnBeginFrame() {
    FrameMarkStart( beginFrameEventName );

    auto& rendererState = Engine::GAPI->GetRendererState();

    static int s_oldResolutionScalePercent = rendererState.RendererSettings.ResolutionScalePercent;

    rendererState.RendererInfo.RenderStage = STAGE_DRAW_UNKNOWN;
    ResetFrameTransientBufferPools();

    if ( NewResolution != Resolution ) {
        if ( OnResize( NewResolution ) != XR_SUCCESS ) return XR_FAILED;
        s_oldResolutionScalePercent = rendererState.RendererSettings.ResolutionScalePercent;
    } else if ( rendererState.RendererSettings.ResolutionScalePercent != s_oldResolutionScalePercent ) {
        if ( RecreateBuffers() != XR_SUCCESS ) return XR_FAILED;
        s_oldResolutionScalePercent = rendererState.RendererSettings.ResolutionScalePercent;
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
    // Some shitty workaround for weird hidden window bug that happen on d3d11 renderer
    if ( !(GetWindowLongA( OutputWindow, GWL_STYLE ) & WS_VISIBLE) ) {
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
            continue;
        }
        GetContext()->CopySubresourceRegion( texture, res.first, 0, 0, 0, res.second, 0, nullptr );
        res.second->Release();
    }
    stagingTextures.clear();

    auto& mipMaps = Engine::GAPI->GetMipMapGeneration();
    for ( D3D11Texture* texture : mipMaps ) {
        if ( texture && texture->GenerateMipMaps() != XR_SUCCESS ) {
            LogWarn() << "Deferred mip-map generation failed; the previous texture remains active.";
        }
    }
    mipMaps.clear();

    Engine::GAPI->SetFrameProcessedTexturesReady();
    Engine::GAPI->LeaveResourceCriticalSection();

    // Notify the shader manager
    ShaderManager->OnFrameStart();

    // Disable culling for ui rendering(Sprite from LeGo needs it since it use CCW instead of CW order)
    SetDefaultStates();
    rendererState.RasterizerState.CullMode = GothicRasterizerStateInfo::CM_CULL_NONE;
    rendererState.RasterizerState.SetDirty();
    if ( UpdateRenderStates() != XR_SUCCESS ) {
        return XR_FAILED;
    }
    if ( ClampSamplerState ) {
        Context->PSSetSamplers( 0, 1, ClampSamplerState.GetAddressOf() );
    }

    // Bind the backbuffer, as otherwise Gothic can't render its initial menu UI

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
    auto& renderInfo = Engine::GAPI->GetRendererState().RendererInfo;
    renderInfo.RenderStage = STAGE_DRAW_PRESENT;
    const XRESULT presentResult = Present();

    RenderedVobs.clear();
    GetPfxRenderer()->OnEndFrame();
    ResetFrameTransientBufferPools();
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
    if ( !Engine::GAPI || !GetContext() || !SwapChain || !PfxRenderer
        || !BackbufferRTV || !Backbuffer || !Backbuffer->IsValid()
        || !DepthStencilBuffer || !DepthStencilBuffer->IsValid()
        || !DepthStencilBufferCopy || !DepthStencilBufferCopy->IsValid()
        || GetBackbufferResolution().x <= 0 || GetBackbufferResolution().y <= 0
        || m_swapchainResolution.x <= 0 || m_swapchainResolution.y <= 0 ) {
        PresentPending = false;
        return XR_FAILED;
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
        if ( !fittedPresentation || !fittedPresentation->IsValid() ) {
            PresentPending = false;
            return XR_FAILED;
        }
        presentationRTV = fittedPresentation->GetRenderTargetView();
    }
    if ( !presentationRTV ) {
        PresentPending = false;
        return XR_FAILED;
    }

    SetViewport( ViewportInfo( 0, 0, GetBackbufferResolution() ) );
    SetDefaultStates();
    UpdateRenderStates();

    {
        auto _ = RecordGraphicsEvent( GE_NAME( "Blit onto Swapchain" ) );
        SetActivePixelShader( PShaderID::PS_PFX_GammaCorrectInv );
        ActivePS->Apply();

        GammaCorrectConstantBuffer gcb = {};
        gcb.G_Gamma = Engine::GAPI->GetGammaValue();
        gcb.G_Brightness = Engine::GAPI->GetBrightnessValue();
        // Dither exactly once immediately before the final 8-bit presentation.
        gcb.G_OutputDitherStrength = 1.0f / 255.0f;
        ActivePS->GetBuffer( "GammaCorrectConstantBuffer" ).Update( &gcb ).Bind();

        if ( PfxRenderer->CopyTextureToRTV(
                Backbuffer->GetShaderResView(), presentationRTV, {}, true ) != XR_SUCCESS ) {
            PresentPending = false;
            return XR_FAILED;
        }

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
        if ( PfxRenderer->CopyTextureToRTV(
                fittedPresentation->GetShaderResView(), BackbufferRTV,
                presentContentSize, false, presentContentOffset ) != XR_SUCCESS ) {
            PresentPending = false;
            return XR_FAILED;
        }
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

    if ( hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET ) {
        const HRESULT reason = GetDevice()->GetDeviceRemovedReason();
        LogErrorBox() << "Direct3D device lost. Present HRESULT: 0x" << std::hex
            << static_cast<unsigned long>(hr) << ", reason: 0x"
            << static_cast<unsigned long>(reason) << ".";
        PresentPending = false;
        return XR_FAILED;
    }
    if ( FAILED( hr ) ) {
        LogError() << "Swapchain presentation failed. HRESULT: 0x" << std::hex
            << static_cast<unsigned long>(hr);
        PresentPending = false;
        return XR_FAILED;
    }
    if ( hr == S_OK && frameLatencyWaitableObject ) {
        ZoneScopedN( "Present::frameLatencyWaitableObject" );
        if ( WaitForSingleObject( frameLatencyWaitableObject, 1000 ) != WAIT_OBJECT_0 ) {
            LogWarn() << "Low-latency wait timed out; disabling the waitable path.";
            CloseHandle( frameLatencyWaitableObject );
            frameLatencyWaitableObject = nullptr;
            m_lowlatency = false;
            Engine::GAPI->GetRendererState().RendererSettings.LowLatency = false;
        }
    }
    PresentPending = false;
    TracyD3D11Collect( s_tracyD3D11Ctx );

    return XR_SUCCESS;
}

/** Called to set the current viewport */
XRESULT D3D11GraphicsEngine::SetViewport( const ViewportInfo& viewportInfo ) {
    const auto context = GetContext();
    if ( !context || viewportInfo.Width <= 0 || viewportInfo.Height <= 0
        || !std::isfinite( viewportInfo.MinZ )
        || !std::isfinite( viewportInfo.MaxZ )
        || viewportInfo.MinZ < 0.0f || viewportInfo.MaxZ > 1.0f
        || viewportInfo.MinZ > viewportInfo.MaxZ ) {
        return XR_INVALID_ARG;
    }

    D3D11_VIEWPORT viewport{};
    viewport.TopLeftX = static_cast<float>(viewportInfo.TopLeftX);
    viewport.TopLeftY = static_cast<float>(viewportInfo.TopLeftY);
    viewport.Width = static_cast<float>(viewportInfo.Width);
    viewport.Height = static_cast<float>(viewportInfo.Height);
    viewport.MinDepth = viewportInfo.MinZ;
    viewport.MaxDepth = viewportInfo.MaxZ;
    context->RSSetViewports( 1, &viewport );
    return XR_SUCCESS;
}

/** Draws a vertexbuffer, non-indexed */
XRESULT D3D11GraphicsEngine::DrawVertexBuffer( D3D11VertexBuffer* vb, unsigned int numVertices, unsigned int stride ) {
    if ( !vb || !vb->IsValid() || stride == 0 || numVertices == 0 ) return XR_INVALID_ARG;
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
    const bool usePreboundBuffers = !vb && !ib;
    if ( !Context || !Engine::GAPI || numIndices == 0
        || (!usePreboundBuffers
            && (!vb || !ib || !vb->IsValid() || !ib->IsValid())) ) {
        return XR_INVALID_ARG;
    }
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
    const bool usePreboundBuffers = !vb && !ib;
    if ( !Context || !Engine::GAPI || numIndices == 0
        || (!usePreboundBuffers
            && (!vb || !ib || !vb->IsValid() || !ib->IsValid())) ) {
        return XR_INVALID_ARG;
    }
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

XRESULT D3D11GraphicsEngine::DrawDynamicVertexBufferIndexed(
    std::vector<ExVertexStruct>& vertices, D3D11VertexBuffer* ib,
    unsigned int numIndices, unsigned int indexOffset ) {
    if ( vertices.empty() || !ib || !ib->IsValid() || numIndices == 0 ) {
        return XR_INVALID_ARG;
    }

    const size_t requiredSize = (std::max)(vertices.size(), size_t(200))
        * sizeof( ExVertexStruct );
    if ( requiredSize > std::numeric_limits<UINT>::max() ) return XR_INVALID_ARG;
    if ( !DynamicVertexBuffer || DynamicVertexBuffer->GetSizeInBytes() < requiredSize ) {
        auto buffer = std::make_unique<D3D11VertexBuffer>();
        if ( buffer->Init( nullptr, static_cast<UINT>(requiredSize),
                D3D11VertexBuffer::B_VERTEXBUFFER, D3D11VertexBuffer::U_DYNAMIC,
                D3D11VertexBuffer::CA_WRITE ) != XR_SUCCESS ) return XR_FAILED;
        DynamicVertexBuffer = std::move( buffer );
    }
    if ( DynamicVertexBuffer->UpdateBuffer( vertices.data(),
            static_cast<UINT>(vertices.size() * sizeof( ExVertexStruct )) ) != XR_SUCCESS ) {
        return XR_FAILED;
    }
    return DrawVertexBufferIndexed( DynamicVertexBuffer.get(), ib, numIndices, indexOffset );
}

/** Draws a vertexbuffer, instanced */
XRESULT D3D11GraphicsEngine::DrawVertexBufferInstanced(
    D3D11VertexBuffer* vb, unsigned int numVertices,
    unsigned int numInstances, unsigned int stride ) {
    if ( !vb || !vb->IsValid() || numVertices == 0 || numInstances == 0 || stride == 0 ) return XR_INVALID_ARG;
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
    if ( !vb || !ib || !vb->IsValid() || !ib->IsValid() || numIndices == 0 || numInstances == 0 ) return XR_INVALID_ARG;
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
    if ( !vb || !ib || !vb->IsValid() || !ib->IsValid() || numIndices == 0 || numInstances == 0 ) return XR_INVALID_ARG;
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
        // Disable culling for ui rendering(Sprite from LeGo needs it since it use CCW instead of CW order)
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
    // ShaderManager->GetVShader("VS_TransformedEx");

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
    auto vShader = ActiveVS;  // ShaderManager->GetVShader("VS_TransformedEx");

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
    if ( !vb || !vb->IsValid() || numVertices == 0 || stride == 0 || !ActivePS ) return XR_INVALID_ARG;
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

    // Bind a normalmap only when the material really has one. Wet scenes keep
    // the old rain distortion fallback, but dry materials without normalmaps
    // stay without a fallback texture.
    if ( D3D11Texture* nrm = tex->GetSurface()->GetNormalmap() ) {
        srvs[1] = nrm->GetShaderResourceView().Get();
    } else if ( ID3D11ShaderResourceView* wetFallback = GetWetNormalFallbackSRV( tex->GetSurface(), DistortionTexture.get() ) ) {
        if ( info &&
            info->buffer.NormalmapStrength != DEFAULT_NORMALMAP_STRENGTH ) {
            info->buffer.NormalmapStrength = DEFAULT_NORMALMAP_STRENGTH;
        }
        srvs[1] = wetFallback;
    }

    if ( info && GetActivePS() ) {
        auto materialBuffer = GetEffectiveMaterialBuffer( info, tex->GetSurface() );
        if ( materialClassMarker != 0.0f ) {
            materialBuffer.Color.w = materialClassMarker;
        }
        auto allocation = PerObjectMaterialInfoPooledBuffer->Allocate(
            GetContext().Get(), &materialBuffer, sizeof( materialBuffer ) );
        if ( allocation ) {
            UINT firstConstant = allocation.offsetInBytes / 16;
            UINT numConstants = allocation.sizeInBytes / 16;
            GetContext()->PSSetConstantBuffers1(
                2, 1, &allocation.pBuffer, &firstConstant, &numConstants );
        } else {
            GetActivePS()->GetBuffer( "MI_MaterialInfo" )
                .Update( &materialBuffer, sizeof( materialBuffer ) )
                .Bind();
        }
    }

    if ( D3D11Texture* fxmap = tex->GetSurface()->GetFxMap() ) {
        srvs[2] = fxmap->GetShaderResourceView().Get();
        fxmap->BindToPixelShader( 2 );
    }
    srvs[3] = GetParallaxDisplacementSRV( tex->GetSurface() );
    GetContext()->PSSetShaderResources( 0, 3, srvs );
    GetContext()->PSSetShaderResources( 13, 1, &srvs[3] );



    return true;
}

XRESULT D3D11GraphicsEngine::DrawSkeletalVertexNormals( SkeletalVobInfo* vi,
    const XMFLOAT4X4& world,
    const std::span<XMFLOAT4X4> transforms, float4 color, float fatness ) {
    auto* visual = vi ? dynamic_cast<SkeletalMeshVisualInfo*>(vi->VisualInfo) : nullptr;
    if ( !Context || !Engine::GAPI || !ShaderManager || !visual || transforms.empty()
        || !WhiteTexture || !WhiteTexture->IsValid()
        || !InfiniteRangeConstantBuffer || !InfiniteRangeConstantBuffer->IsValid() ) {
        return XR_INVALID_ARG;
    }

    const auto gshader = ShaderManager->GetGShader( GShaderID::GS_VertexNormals );
    if ( !gshader || !gshader->GetShader()
        || SetActiveVertexShader( VShaderID::VS_ExSkeletalVN ) != XR_SUCCESS
        || SetActivePixelShader( PShaderID::PS_Simple ) != XR_SUCCESS
        || !ActiveVS || !ActivePS ) {
        return XR_FAILED;
    }

    SetupVS_ExConstantBuffer();

    VS_ExConstantBuffer_PerInstanceSkeletal cb2 = {};
    cb2.World = world;
    cb2.PrevWorld = world;
    color.w = (vi->Vob && vi->Vob->IsIndoorVob()) ? 0.05f : 1.0f;
    cb2.PI_ModelColor = color;
    cb2.PI_ModelFatness = fatness;

    auto perInstanceBuffer = ActiveVS->GetBuffer( "Matrices_PerInstances" );
    if ( !perInstanceBuffer.Update( &cb2 ).Bind().Succeeded()
        || !perInstanceBuffer.GetRawBuffer()
        || !perInstanceBuffer.GetRawBuffer()->IsValid() ) {
        return XR_FAILED;
    }
    perInstanceBuffer.GetRawBuffer()->BindToGeometryShader( 1 );

    const size_t boneCount = (std::min)(transforms.size(), static_cast<size_t>(NUM_MAX_BONES));
    const std::span<const XMFLOAT4X4> currentBones( transforms.data(), boneCount );
    if ( !FeatureLevel10Compatibility ) {
        if ( !UploadStructuredMatrixBuffer(
                SkeletalBoneTransformsBufferTransient, currentBones, "SkeletalBoneTransformsBufferTransient" )
            || !SkeletalBoneTransformsBufferTransient
            || !SkeletalBoneTransformsBufferTransient->GetShaderResourceView() ) {
            return XR_FAILED;
        }

        ActiveVS->BindResource(
            "BoneTransforms", SkeletalBoneTransformsBufferTransient->GetShaderResourceView().Get() );
        VS_ExConstantBuffer_SkeletalBoneRange range = {};
        range.BoneCount = static_cast<unsigned int>(boneCount);
        range.UseStructuredBones = 1u;
        if ( !ActiveVS->GetBuffer( "BoneTransformRange" ).Update( &range ).Bind().Succeeded() ) {
            return XR_FAILED;
        }
    } else if ( !ActiveVS->GetBuffer( "BoneTransforms" )
        .Update( currentBones.data(), static_cast<UINT>(boneCount * sizeof( XMFLOAT4X4 )) )
        .Bind().Succeeded() ) {
        return XR_FAILED;
    }

    UpdateRenderStates();
    if ( ActiveVS->Apply() != XR_SUCCESS
        || ActivePS->Apply() != XR_SUCCESS
        || gshader->Apply() != XR_SUCCESS ) {
        GetContext()->GSSetShader( nullptr, nullptr, 0 );
        return XR_FAILED;
    }

    InfiniteRangeConstantBuffer->BindToPixelShader( 3 );
    GetContext()->IASetPrimitiveTopology( D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST );

    if ( transforms.size() > static_cast<size_t>(NUM_MAX_BONES) ) {
        LogWarn() << "SkeletalMesh has more than " << NUM_MAX_BONES
            << " bones (" << transforms.size() << ").";
    }

    for ( const auto& item : visual->SkeletalMeshes ) {
        for ( SkeletalMeshInfo* mesh : item.second ) {
            if ( !IsDrawableSkeletalMesh( mesh ) ) {
                continue;
            }

            WhiteTexture->BindToPixelShader( 0 );
            const UINT numIndices = static_cast<UINT>(mesh->Indices.size());
            UINT offset = 0;
            UINT stride = sizeof( ExSkelVertexStruct );
            GetContext()->IASetVertexBuffers(
                0, 1, mesh->MeshVertexBuffer->GetVertexBuffer().GetAddressOf(), &stride, &offset );
            GetContext()->IASetIndexBuffer(
                mesh->MeshIndexBuffer->GetVertexBuffer().Get(), VERTEX_INDEX_DXGI_FORMAT, 0 );
            GetContext()->DrawIndexed( numIndices, 0, 0 );
            Engine::GAPI->GetRendererState().RendererInfo.FrameDrawnTriangles += numIndices / 3;
        }
    }

    GetContext()->GSSetShader( nullptr, nullptr, 0 );
    return XR_SUCCESS;
}

/** Draws a skeletal mesh */
XRESULT D3D11GraphicsEngine::DrawSkeletalMesh( SkeletalVobInfo* vi,
    const std::span<XMFLOAT4X4> transforms, float4 color, const XMFLOAT4X4& world, float fatness ) {
    auto* visual = vi ? dynamic_cast<SkeletalMeshVisualInfo*>(vi->VisualInfo) : nullptr;
    if ( !Context || !Engine::GAPI || !ShaderManager || !visual || transforms.empty() ) {
        return XR_INVALID_ARG;
    }

    const bool cubeShadow = GetRenderingStage() == DES_SHADOWMAP_CUBE;
    const VShaderID vertexShader = cubeShadow
        ? VShaderID::VS_ExSkeletalCube
        : VShaderID::VS_ExSkeletal;
    if ( SetActiveVertexShader( vertexShader ) != XR_SUCCESS || !ActiveVS ) {
        return XR_FAILED;
    }

    SetupVS_ExMeshDrawCall();
    SetupVS_ExConstantBuffer();
    Context->IASetPrimitiveTopology( D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST );

    VS_ExConstantBuffer_PerInstanceSkeletal cb2 = {};
    cb2.World = world;
    color.w = (vi->Vob && vi->Vob->IsIndoorVob()) ? 0.05f : 1.0f;
    cb2.PI_ModelColor = color;
    cb2.PI_ModelFatness = fatness;
    cb2.PrevWorld = vi->HasValidPrevTransforms ? vi->PrevWorldMatrix : world;
    if ( !ActiveVS->GetBuffer( "Matrices_PerInstances" ).Update( &cb2 ).Bind().Succeeded() ) {
        return XR_FAILED;
    }

    const size_t boneCount = (std::min)(transforms.size(), static_cast<size_t>(NUM_MAX_BONES));
    const std::span<const XMFLOAT4X4> currentBones( transforms.data(), boneCount );
    std::array<XMFLOAT4X4, NUM_MAX_BONES> previousScratch{};
    const std::span<const XMFLOAT4X4> previousBones = SelectPreviousBoneTransforms(
        currentBones, vi->HasValidPrevTransforms, vi->PrevBoneTransforms, previousScratch );

    if ( !FeatureLevel10Compatibility ) {
        if ( !UploadStructuredMatrixBuffer(
                SkeletalBoneTransformsBufferTransient, currentBones, "SkeletalBoneTransformsBufferTransient" )
            || !SkeletalBoneTransformsBufferTransient
            || !SkeletalBoneTransformsBufferTransient->GetShaderResourceView() ) {
            return XR_FAILED;
        }
        ActiveVS->BindResource(
            "BoneTransforms", SkeletalBoneTransformsBufferTransient->GetShaderResourceView().Get() );

        if ( !cubeShadow ) {
            if ( !UploadStructuredMatrixBuffer(
                    SkeletalPrevBoneTransformsBufferTransient, previousBones,
                    "SkeletalPrevBoneTransformsBufferTransient" )
                || !SkeletalPrevBoneTransformsBufferTransient
                || !SkeletalPrevBoneTransformsBufferTransient->GetShaderResourceView() ) {
                return XR_FAILED;
            }
            ActiveVS->BindResource(
                "PrevBoneTransforms", SkeletalPrevBoneTransformsBufferTransient->GetShaderResourceView().Get() );
        }

        VS_ExConstantBuffer_SkeletalBoneRange range = {};
        range.BoneCount = static_cast<unsigned int>(boneCount);
        range.UseStructuredBones = 1u;
        if ( !ActiveVS->GetBuffer( "BoneTransformRange" ).Update( &range ).Bind().Succeeded() ) {
            return XR_FAILED;
        }
    } else {
        auto currentBuffer = ActiveVS->GetBuffer( "BoneTransforms" );
        if ( !currentBuffer
            .Update( currentBones.data(), static_cast<UINT>(boneCount * sizeof( XMFLOAT4X4 )) )
            .Bind().Succeeded() ) {
            return XR_FAILED;
        }

        if ( !cubeShadow ) {
            auto previousBuffer = ActiveVS->GetBuffer( "PrevBoneTransforms" );
            if ( GetRenderingStage() != DES_SHADOWMAP ) {
                if ( !previousBuffer
                    .Update( previousBones.data(), static_cast<UINT>(boneCount * sizeof( XMFLOAT4X4 )) )
                    .Bind().Succeeded() ) {
                    return XR_FAILED;
                }
            } else if ( !previousBuffer.Succeeded()
                || !currentBuffer.Bind( previousBuffer.GetSlot() ).Succeeded() ) {
                return XR_FAILED;
            }
        }
    }

    if ( transforms.size() > static_cast<size_t>(NUM_MAX_BONES) ) {
        LogWarn() << "SkeletalMesh has more than " << NUM_MAX_BONES
            << " bones (" << transforms.size() << ").";
    }

    if ( ActiveVS->Apply() != XR_SUCCESS ) {
        return XR_FAILED;
    }

    if ( RenderingStage != DES_GHOST ) {
        const bool linearDepth =
            (Engine::GAPI->GetRendererState().GraphicsState.FF_GSwitches & GSWITCH_LINEAR_DEPTH) != 0;
        if ( linearDepth ) {
            ActivePS = ShaderManager->GetPShader( PShaderID::PS_LinDepth );
            if ( !ActivePS || ActivePS->Apply() != XR_SUCCESS ) {
                return XR_FAILED;
            }
        } else if ( RenderingStage == DES_SHADOWMAP ) {
            Context->PSSetShader( nullptr, nullptr, 0 );
            ActivePS = nullptr;
        } else {
            ActivePS = ShaderManager->GetPShader( PShaderID::PS_LinDepth );
            if ( !ActivePS ) {
                return XR_FAILED;
            }
        }
    }

    for ( const auto& item : visual->SkeletalMeshes ) {
        if ( zCMaterial* material = item.first ) {
            zCTexture* texture = nullptr;
            if ( ActivePS && (texture = material->GetAniTexture()) != nullptr
                && !BindTextureNRFX(
                    texture, RenderingStage != DES_GHOST, true,
                    (vi->Vob && vi->Vob->GetVobType() == zVOB_TYPE_NSC) ? -1.0f : 0.0f ) ) {
                continue;
            }
        }

        for ( SkeletalMeshInfo* mesh : item.second ) {
            if ( !IsDrawableSkeletalMesh( mesh ) ) {
                continue;
            }

            const UINT numIndices = static_cast<UINT>(mesh->Indices.size());
            UINT offset = 0;
            UINT stride = sizeof( ExSkelVertexStruct );
            Context->IASetVertexBuffers(
                0, 1, mesh->MeshVertexBuffer->GetVertexBuffer().GetAddressOf(), &stride, &offset );
            Context->IASetIndexBuffer(
                mesh->MeshIndexBuffer->GetVertexBuffer().Get(), VERTEX_INDEX_DXGI_FORMAT, 0 );
            Context->DrawIndexed( numIndices, 0, 0 );
            Engine::GAPI->GetRendererState().RendererInfo.FrameDrawnTriangles += numIndices / 3;
        }
    }

    return XR_SUCCESS;
}

XRESULT D3D11GraphicsEngine::DrawSkeletalMesh_Layered( SkeletalVobInfo* vi,
    const std::span<XMFLOAT4X4> transforms, float4 color, XMFLOAT4X4& world, float fatness ) {
    auto* visual = vi ? dynamic_cast<SkeletalMeshVisualInfo*>(vi->VisualInfo) : nullptr;
    if ( !Context || !Engine::GAPI || !ShaderManager || !visual || transforms.empty()
        || !WhiteTexture || !WhiteTexture->IsValid() ) {
        return XR_INVALID_ARG;
    }
    if ( SetActiveVertexShader( VShaderID::VS_ExSkeletalLayered ) != XR_SUCCESS || !ActiveVS ) {
        return XR_FAILED;
    }

    SetupVS_ExMeshDrawCall();
    SetupVS_ExConstantBuffer();
    Context->IASetPrimitiveTopology( D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST );

    VS_ExConstantBuffer_PerInstanceSkeletal cb2 = {};
    cb2.World = world;
    cb2.PrevWorld = world;
    color.w = (vi->Vob && vi->Vob->IsIndoorVob()) ? 0.05f : 1.0f;
    cb2.PI_ModelColor = color;
    cb2.PI_ModelFatness = fatness;
    if ( !ActiveVS->GetBuffer( "Matrices_PerInstances" ).Update( &cb2 ).Bind().Succeeded() ) {
        return XR_FAILED;
    }

    const size_t boneCount = (std::min)(transforms.size(), static_cast<size_t>(NUM_MAX_BONES));
    const std::span<const XMFLOAT4X4> currentBones( transforms.data(), boneCount );
    if ( !FeatureLevel10Compatibility ) {
        if ( !UploadStructuredMatrixBuffer(
                SkeletalBoneTransformsBufferTransient, currentBones, "SkeletalBoneTransformsBufferTransient" )
            || !SkeletalBoneTransformsBufferTransient
            || !SkeletalBoneTransformsBufferTransient->GetShaderResourceView() ) {
            return XR_FAILED;
        }

        ActiveVS->BindResource(
            "BoneTransforms", SkeletalBoneTransformsBufferTransient->GetShaderResourceView().Get() );
        VS_ExConstantBuffer_SkeletalBoneRange range = {};
        range.BoneCount = static_cast<unsigned int>(boneCount);
        range.UseStructuredBones = 1u;
        if ( !ActiveVS->GetBuffer( "BoneTransformRange" ).Update( &range ).Bind().Succeeded() ) {
            return XR_FAILED;
        }
    } else if ( !ActiveVS->GetBuffer( "BoneTransforms" )
        .Update( currentBones.data(), static_cast<UINT>(boneCount * sizeof( XMFLOAT4X4 )) )
        .Bind().Succeeded() ) {
        return XR_FAILED;
    }

    if ( transforms.size() > static_cast<size_t>(NUM_MAX_BONES) ) {
        LogWarn() << "SkeletalMesh has more than " << NUM_MAX_BONES
            << " bones (" << transforms.size() << ").";
    }

    if ( ActiveVS->Apply() != XR_SUCCESS ) {
        return XR_FAILED;
    }

    if ( RenderingStage != DES_GHOST ) {
        const bool linearDepth =
            (Engine::GAPI->GetRendererState().GraphicsState.FF_GSwitches & GSWITCH_LINEAR_DEPTH) != 0;
        if ( linearDepth ) {
            ActivePS = ShaderManager->GetPShader( PShaderID::PS_LinDepth );
            if ( !ActivePS || ActivePS->Apply() != XR_SUCCESS ) {
                return XR_FAILED;
            }
        } else if ( RenderingStage == DES_SHADOWMAP ) {
            Context->PSSetShader( nullptr, nullptr, 0 );
            ActivePS = nullptr;
        } else {
            ActivePS = ShaderManager->GetPShader( PShaderID::PS_LinDepth );
            if ( !ActivePS ) {
                return XR_FAILED;
            }
        }
    }

    zCTexture* lastTexture = nullptr;
    WhiteTexture->BindToPixelShader( 0 );
    bool whiteTextureBound = true;

    for ( const auto& item : visual->SkeletalMeshes ) {
        zCMaterial* const material = item.first;
        zCTexture* const texture = material ? material->GetAniTexture() : nullptr;
        if ( texture && texture->CacheIn( 0.6f ) != zRES_CACHED_IN ) {
            continue;
        }

        const bool needsTexture = texture && material
            && (texture->HasAlphaChannel() || material->HasAlphaTest());
        if ( needsTexture ) {
            auto* surface = texture->GetSurface();
            auto* engineTexture = surface ? surface->GetEngineTexture() : nullptr;
            if ( !engineTexture || !engineTexture->IsValid() ) {
                continue;
            }
            if ( whiteTextureBound || texture != lastTexture ) {
                engineTexture->BindToPixelShader( 0 );
                lastTexture = texture;
                whiteTextureBound = false;
            }
        } else if ( !whiteTextureBound ) {
            WhiteTexture->BindToPixelShader( 0 );
            lastTexture = nullptr;
            whiteTextureBound = true;
        }

        for ( SkeletalMeshInfo* mesh : item.second ) {
            if ( !IsDrawableSkeletalMesh( mesh ) ) {
                continue;
            }

            const UINT numIndices = static_cast<UINT>(mesh->Indices.size());
            UINT offset = 0;
            UINT stride = sizeof( ExSkelVertexStruct );
            Context->IASetVertexBuffers(
                0, 1, mesh->MeshVertexBuffer->GetVertexBuffer().GetAddressOf(), &stride, &offset );
            Context->IASetIndexBuffer(
                mesh->MeshIndexBuffer->GetVertexBuffer().Get(), VERTEX_INDEX_DXGI_FORMAT, 0 );
            Context->DrawIndexedInstanced( numIndices, 6, 0, 0, 0 );
            Engine::GAPI->GetRendererState().RendererInfo.FrameDrawnTriangles += numIndices / 3;
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
    if ( !Context || !Engine::GAPI || !vb || !ib || !instanceData
        || !vb->IsValid() || !ib->IsValid() || !instanceData->IsValid()
        || numIndices == 0 || numInstances == 0
        || vertexStride == 0 || instanceDataStride == 0
        || vb->GetSizeInBytes() < vertexStride ) {
        return XR_INVALID_ARG;
    }

    const uint64_t indexCapacity =
        ib->GetSizeInBytes() / sizeof( VERTEX_INDEX );
    if ( indexOffset >= indexCapacity
        || numIndices > indexCapacity - indexOffset ) {
        return XR_INVALID_ARG;
    }

    const uint64_t instanceCapacity =
        instanceData->GetSizeInBytes() / instanceDataStride;
    if ( startInstanceNum >= instanceCapacity
        || numInstances > instanceCapacity - startInstanceNum ) {
        return XR_INVALID_ARG;
    }

    const int maxFaces =
        Engine::GAPI->GetRendererState().RendererSettings.MaxNumFaces;
    if ( maxFaces > 0 ) {
        const uint64_t maxIndices = (std::min<uint64_t>)(
            static_cast<uint64_t>(maxFaces) * 3u,
            (std::numeric_limits<unsigned int>::max)() );
        numIndices = (std::min)(
            numIndices, static_cast<unsigned int>(maxIndices) );
    }
    if ( numIndices == 0 ) return XR_SUCCESS;

    UINT offsets[] = { 0, 0 };
    UINT strides[] = { vertexStride, instanceDataStride };
    ID3D11Buffer* buffers[] = {
        vb->GetVertexBuffer().Get(),
        instanceData->GetVertexBuffer().Get()
    };
    Context->IASetVertexBuffers( 0, 2, buffers, strides, offsets );
    Context->IASetIndexBuffer(
        ib->GetVertexBuffer().Get(), VERTEX_INDEX_DXGI_FORMAT, 0 );
    Context->DrawIndexedInstanced(
        numIndices, numInstances, indexOffset, 0, startInstanceNum );

    auto& rendererInfo = Engine::GAPI->GetRendererState().RendererInfo;
    const uint64_t drawnTriangles =
        static_cast<uint64_t>(numIndices / 3u) * numInstances;
    const uint64_t existingTriangles = static_cast<uint64_t>(
        (std::max)(rendererInfo.FrameDrawnTriangles, 0) );
    rendererInfo.FrameDrawnTriangles = static_cast<int>(
        (std::min<uint64_t>)(
            existingTriangles + drawnTriangles,
            static_cast<uint64_t>((std::numeric_limits<int>::max)()) ) );
    if ( rendererInfo.FrameDrawnVobs
        < (std::numeric_limits<int>::max)() ) {
        ++rendererInfo.FrameDrawnVobs;
    }

    return XR_SUCCESS;
}

/** Draws skeletal meshes */
void D3D11GraphicsEngine::DrawSkeletalMeshVobs(
    std::span<SkeletalVobInfo* const> vis,
    float distance,
    bool updateState,
    bool drawAttachments ) {
    ZoneScoped;
    if ( !Context || !Engine::GAPI || !ShaderManager || vis.empty() ) {
        return;
    }

    //// Skeletal meshes use bone-driven animation that can change between passes.
    //// Skip them during the depth prepass to avoid depth mismatch in the lit pass.
    //if ( GetRenderingStage() == DES_Z_PRE_PASS )
    //    return;

    struct TempVobDrawInfo {
        SkeletalVobInfo* VobInfo;
        zCModel* Model;
        size_t BoneIdx;
        size_t NumBones;
        float4 ModelColor;
        float Fatness;
        XMMATRIX World;
        XMFLOAT4X4 PrevWorld;

        TempVobDrawInfo() = default;

        TempVobDrawInfo(
            SkeletalVobInfo* VobInfo,
            zCModel* Model,
            size_t BoneIdx,
            size_t NumBones,
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
    static std::vector<XMFLOAT4X4> packedPrevBoneTransforms;
    packedPrevBoneTransforms.clear();

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
    const bool useStructuredBones = !FeatureLevel10Compatibility;

    static std::vector<VS_ExConstantBuffer_SkeletalBoneRange> structuredBoneRanges;
    structuredBoneRanges.assign( vis.size(), {} );
    bool reuseMainPackedUpload = false;

    if ( useStructuredBones
        && isMainStage
        && m_FrameGeometryCache.skeletalBonesUploaded
        && m_FrameGeometryCache.skeletalBoneRanges.size() == vis.size()
        && HasMatchingSkeletalVisOrder( vis, m_FrameGeometryCache.skeletalBoneVisOrder ) ) {
        structuredBoneRanges = m_FrameGeometryCache.skeletalBoneRanges;
        reuseMainPackedUpload = true;
    }

    if ( useStructuredBones && !reuseMainPackedUpload ) {
        BoneTransformCache.clear();
        packedPrevBoneTransforms.clear();

        size_t packedBoneOffset = 0;
        for ( size_t i = 0; i < vis.size(); ++i ) {
            SkeletalVobInfo* vi = vis[i];
            auto& range = structuredBoneRanges[i];

            if ( !vi || !vi->Vob ) {
                continue;
            }

            zCModel* model = static_cast<zCModel*>(vi->Vob->GetVisual());
            if ( !model || !dynamic_cast<SkeletalMeshVisualInfo*>(vi->VisualInfo)
                || !vi->Vob->GetShowVisual() ) {
                continue;
            }
            vi->UpdateState();

            const size_t currentBegin = BoneTransformCache.size();
            model->GetBoneTransforms( &BoneTransformCache );
            if ( BoneTransformCache.size() <= currentBegin ) {
                continue;
            }

            const size_t numBones = (std::min)(
                BoneTransformCache.size() - currentBegin,
                static_cast<size_t>(NUM_MAX_BONES) );
            BoneTransformCache.resize( currentBegin + numBones );
            if ( packedBoneOffset > (std::numeric_limits<UINT>::max)() - numBones
                || packedPrevBoneTransforms.size()
                    > (std::numeric_limits<UINT>::max)() - numBones ) {
                LogError() << "Skeletal batching: Bone range exceeds the shader address space.";
                return;
            }

            range.BoneOffset = static_cast<unsigned int>(packedBoneOffset);
            range.BoneCount = static_cast<unsigned int>(numBones);
            range.PrevBoneOffset = static_cast<unsigned int>(packedPrevBoneTransforms.size());
            range.UseStructuredBones = 1u;

            const std::span<const XMFLOAT4X4> transforms(
                BoneTransformCache.data() + currentBegin, numBones );
            std::array<XMFLOAT4X4, NUM_MAX_BONES> previousScratch{};
            const auto previousTransforms = SelectPreviousBoneTransforms(
                transforms, vi->HasValidPrevTransforms, vi->PrevBoneTransforms, previousScratch );
            packedPrevBoneTransforms.insert(
                packedPrevBoneTransforms.end(), previousTransforms.begin(), previousTransforms.end() );
            packedBoneOffset += numBones;
        }

        auto& currentBuffer = isMainReuseStage ? SkeletalBoneTransformsBuffer : SkeletalBoneTransformsBufferTransient;
        auto& prevBuffer = isMainReuseStage ? SkeletalPrevBoneTransformsBuffer : SkeletalPrevBoneTransformsBufferTransient;

        const bool uploadedCurrent = UploadStructuredMatrixBuffer(
            currentBuffer,
            BoneTransformCache,
            isMainReuseStage ? "SkeletalBoneTransformsBuffer" : "SkeletalBoneTransformsBufferTransient" );
        const bool uploadedPrevious = UploadStructuredMatrixBuffer(
            prevBuffer,
            packedPrevBoneTransforms,
            isMainReuseStage ? "SkeletalPrevBoneTransformsBuffer" : "SkeletalPrevBoneTransformsBufferTransient" );

        if ( !uploadedCurrent || !uploadedPrevious ) {
            LogError() << "Skeletal batching: Failed to upload structured bone transforms.";
            return;
        } else if ( isMainReuseStage ) {
            m_FrameGeometryCache.skeletalBonesUploaded = true;
            m_FrameGeometryCache.skeletalBoneVisOrder.assign(
                vis.begin(), vis.end() );
            m_FrameGeometryCache.skeletalBoneRanges = structuredBoneRanges;
        }
    }

    BoneTransformCache.clear();

    // Setup drawing of SkeletalMeshes, attachments are deferred, to reduce api calls

    const bool cubeShadow = GetRenderingStage() == DES_SHADOWMAP_CUBE;
    const VShaderID vertexShader = cubeShadow
        ? VShaderID::VS_ExSkeletalCube
        : VShaderID::VS_ExSkeletal;
    if ( SetActiveVertexShader( vertexShader ) != XR_SUCCESS || !ActiveVS ) {
        return;
    }

    SetupVS_ExMeshDrawCall();
    SetupVS_ExConstantBuffer();
    Context->IASetPrimitiveTopology( D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST );

    auto perInstanceCb = ActiveVS->GetBuffer( "Matrices_PerInstances" ).Bind();
    if ( !perInstanceCb.Succeeded() ) {
        return;
    }

    auto boneRangeCb = GraphicsShaderConstantBuffer();
    auto boneTransformsCb = GraphicsShaderConstantBuffer();
    auto prevBoneTransformsCb = GraphicsShaderConstantBuffer();
    size_t structuredCurrentCapacity = 0;
    size_t structuredPreviousCapacity = 0;

    if ( useStructuredBones ) {
        auto& currentBuffer = isMainReuseStage
            ? SkeletalBoneTransformsBuffer
            : SkeletalBoneTransformsBufferTransient;
        auto& previousBuffer = isMainReuseStage
            ? SkeletalPrevBoneTransformsBuffer
            : SkeletalPrevBoneTransformsBufferTransient;

        if ( !currentBuffer || !currentBuffer->GetShaderResourceView()
            || (!cubeShadow && (!previousBuffer || !previousBuffer->GetShaderResourceView())) ) {
            LogError() << "Skeletal batching: Structured bone resources are unavailable.";
            return;
        }

        structuredCurrentCapacity = currentBuffer->GetSizeInBytes() / sizeof( XMFLOAT4X4 );
        structuredPreviousCapacity = previousBuffer
            ? previousBuffer->GetSizeInBytes() / sizeof( XMFLOAT4X4 )
            : 0;
        if ( structuredCurrentCapacity == 0 || (!cubeShadow && structuredPreviousCapacity == 0) ) {
            LogError() << "Skeletal batching: Structured bone buffers have invalid capacities.";
            return;
        }

        ActiveVS->BindResource( "BoneTransforms", currentBuffer->GetShaderResourceView().Get() );
        if ( !cubeShadow ) {
            ActiveVS->BindResource( "PrevBoneTransforms", previousBuffer->GetShaderResourceView().Get() );
        }

        boneRangeCb = ActiveVS->GetBuffer( "BoneTransformRange" ).Bind();
        if ( !boneRangeCb.Succeeded() ) {
            return;
        }
    } else {
        boneTransformsCb = ActiveVS->GetBuffer( "BoneTransforms" ).Bind();
        if ( !boneTransformsCb.Succeeded() ) {
            return;
        }

        if ( !cubeShadow ) {
            prevBoneTransformsCb = ActiveVS->GetBuffer( "PrevBoneTransforms" );
            if ( !prevBoneTransformsCb.Succeeded() ) {
                return;
            }
            if ( GetRenderingStage() != DES_SHADOWMAP ) {
                if ( !prevBoneTransformsCb.Bind().Succeeded() ) {
                    return;
                }
            } else if ( !boneTransformsCb.Bind( prevBoneTransformsCb.GetSlot() ).Succeeded() ) {
                return;
            }
        }
    }

    if ( ActiveVS->Apply() != XR_SUCCESS ) {
        return;
    }

    const auto now = Engine::GAPI->GetTotalTimeDW();

    bool wantShader = true;
    if ( RenderingStage != DES_GHOST ) {
        bool linearDepth = (graphicsState.FF_GSwitches & GSWITCH_LINEAR_DEPTH) != 0;
        if ( linearDepth ) {
            ActivePS = ShaderManager->GetPShader( PShaderID::PS_LinDepth );
            if ( !ActivePS || ActivePS->Apply() != XR_SUCCESS ) {
                return;
            }
        } else if ( RenderingStage == DES_SHADOWMAP ) {
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

    // Ensure we have the correct constant buffer for eventual alpha-test draws.
    const auto alphaTestShader = ShaderManager->GetPShader( Resolved_DiffuseNormalmappedAlphatest );
    if ( !alphaTestShader ) {
        return;
    }
    auto cbFFPipelineConstantBuffer = alphaTestShader->GetBuffer( "FFPipelineConstantBuffer" )
        .Update( &graphicsState )
        .Bind();
    if ( !cbFFPipelineConstantBuffer.Succeeded() ) {
        return;
    }

    const bool enableShadows = Engine::GAPI->GetRendererState().RendererSettings.EnableShadows;
    const bool isMainPass = RenderingStage == DES_MAIN;
    zCTexture* lastTex = nullptr;
    float lastMaterialClassMarker = 999.0f;
    auto bindTextureForPass = [&]( zCTexture* tex, float materialClassMarker ) {
        if ( !tex ) {
            return false;
        }
        if ( tex == lastTex && materialClassMarker == lastMaterialClassMarker ) {
            return true;
        }
        lastMaterialClassMarker = materialClassMarker;

        if ( isZPrepass ) {
            if ( tex->CacheIn( 0.6f ) != zRES_CACHED_IN ) {
                return false;
            }
            if ( tex->HasAlphaChannel() ) {
                auto* surface = tex->GetSurface();
                auto* engineTexture = surface ? surface->GetEngineTexture() : nullptr;
                if ( !engineTexture || !engineTexture->IsValid() ) {
                    return false;
                }
                if ( !ActivePS ) {
                    ActivePS = ShaderManager->GetPShader( PShaderID::PS_DiffuseAlphaTestShadows );
                    if ( !ActivePS || ActivePS->Apply() != XR_SUCCESS ) {
                        ActivePS.reset();
                        return false;
                    }
                }
                engineTexture->BindToPixelShader( 0 );
                lastTex = tex;
            } else if ( lastTex != nullptr ) {
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
            if ( !vi || !vi->Vob ) {
                continue;
            }

            auto* visual = dynamic_cast<SkeletalMeshVisualInfo*>(vi->VisualInfo);
            zCModel* model = static_cast<zCModel*>(vi->Vob->GetVisual());
            if ( !model || !visual || !vi->Vob->GetShowVisual() ) {
                continue;
            }

            model->SetIsVisible( true );

            const float materialClassMarker = vi->Vob->GetVobType() == zVOB_TYPE_NSC ? -1.0f : 0.0f;
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

            // Get the bone transforms
            const size_t boneIdx = BoneTransformCache.size();
            model->GetBoneTransforms( &BoneTransformCache );
            if ( BoneTransformCache.size() <= boneIdx ) {
                continue;
            }
            const size_t numBones = BoneTransformCache.size() - boneIdx;

            if ( !visual->SkeletalMeshes.empty() ) {
#ifdef BUILD_GOTHIC_2_6_fix
                if ( !model->GetDrawHandVisualsOnly() || *reinterpret_cast<BYTE*>(0x57A694) == 0x90 ) {
#else
                if ( !model->GetDrawHandVisualsOnly() ) {
#endif
                    const size_t gpuBoneCount =
                        (std::min)(numBones, static_cast<size_t>(NUM_MAX_BONES));
                    const std::span<const XMFLOAT4X4> transforms(
                        BoneTransformCache.data() + boneIdx, gpuBoneCount );

                    VS_ExConstantBuffer_PerInstanceSkeletal cb2 = {};
                    cb2.World = world;
                    auto maskedColor = modelColor;
                    maskedColor.w = vi->Vob->IsIndoorVob() ? 0.05f : 1.0f;
                    cb2.PI_ModelColor = maskedColor;
                    cb2.PI_ModelFatness = fatness;
                    cb2.PrevWorld = vi->HasValidPrevTransforms ? vi->PrevWorldMatrix : world;
                    if ( !perInstanceCb.Update( &cb2 ).Succeeded() ) {
                        LogError() << "Skeletal batching: Failed to update per-instance data.";
                        return;
                    }

                    if ( useStructuredBones ) {
                        if ( currentDrawIndex >= structuredBoneRanges.size() ) {
                            return;
                        }
                        VS_ExConstantBuffer_SkeletalBoneRange range =
                            structuredBoneRanges[currentDrawIndex];
                        if ( range.BoneCount == 0 || range.BoneCount > NUM_MAX_BONES ) {
                            continue;
                        }
                        const uint64_t currentRangeEnd =
                            static_cast<uint64_t>(range.BoneOffset) + range.BoneCount;
                        const uint64_t previousRangeEnd =
                            static_cast<uint64_t>(range.PrevBoneOffset) + range.BoneCount;
                        if ( range.BoneCount != gpuBoneCount
                            || currentRangeEnd > structuredCurrentCapacity
                            || (!cubeShadow && previousRangeEnd > structuredPreviousCapacity) ) {
                            LogError() << "Skeletal batching: Structured bone range is out of bounds.";
                            return;
                        }
                        range.UseStructuredBones = 1u;
                        if ( !boneRangeCb.Update( &range ).Succeeded() ) {
                            LogError() << "Skeletal batching: Failed to update the bone range.";
                            return;
                        }
                    } else {
                        if ( !boneTransformsCb.Update(
                                transforms.data(),
                                static_cast<UINT>(gpuBoneCount * sizeof( XMFLOAT4X4 )) ).Succeeded() ) {
                            LogError() << "Skeletal batching: Failed to update bone transforms.";
                            return;
                        }

                        if ( !cubeShadow && GetRenderingStage() != DES_SHADOWMAP ) {
                            std::array<XMFLOAT4X4, NUM_MAX_BONES> previousScratch{};
                            const auto previousTransforms = SelectPreviousBoneTransforms(
                                transforms, vi->HasValidPrevTransforms,
                                vi->PrevBoneTransforms, previousScratch );
                            if ( !prevBoneTransformsCb.Update(
                                    previousTransforms.data(),
                                    static_cast<UINT>(gpuBoneCount * sizeof( XMFLOAT4X4 )) ).Succeeded() ) {
                                LogError() << "Skeletal batching: Failed to update previous bone transforms.";
                                return;
                            }
                        }
                    }

                    if ( numBones > static_cast<size_t>(NUM_MAX_BONES) ) {
                        LogWarn() << "SkeletalMesh has more than " << NUM_MAX_BONES
                            << " bones (" << numBones << ").";
                    }
                    for ( const auto& itm : visual->SkeletalMeshes ) {
                        if ( zCMaterial* mat = itm.first ) {
                            zCTexture* tex;
                            if ( wantShader && (tex = mat->GetAniTexture()) != nullptr ) {
                                if ( !bindTextureForPass( tex, materialClassMarker ) ) {
                                    continue;
                                }
                            }
                        }
                        for ( SkeletalMeshInfo* mesh : itm.second ) {
                            if ( !IsDrawableSkeletalMesh( mesh ) ) {
                                continue;
                            }

                            const UINT numIndices = static_cast<UINT>(mesh->Indices.size());
                            UINT offset = 0;
                            UINT stride = sizeof( ExSkelVertexStruct );
                            Context->IASetVertexBuffers(
                                0, 1, mesh->MeshVertexBuffer->GetVertexBuffer().GetAddressOf(),
                                &stride, &offset );
                            Context->IASetIndexBuffer(
                                mesh->MeshIndexBuffer->GetVertexBuffer().Get(),
                                VERTEX_INDEX_DXGI_FORMAT, 0 );
                            Context->DrawIndexed( numIndices, 0, 0 );
                            Engine::GAPI->GetRendererState().RendererInfo.FrameDrawnTriangles +=
                                numIndices / 3;
                        }
                    }
                }
            } else {
                if ( model->GetMeshSoftSkinList()->NumInArray > 0 ) {
                    // Just in case somehow we end up without skeletal meshes and they are available
                    WorldConverter::ExtractSkeletalMeshFromVob( model, visual );
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

        // For DES_SHADOWMAP_CUBE we need the existing per-draw path (SV_InstanceID used for cubemap faces)
        const bool useCubePath = (GetRenderingStage() == DES_SHADOWMAP_CUBE);
        const bool isShadowPass = (GetRenderingStage() == DES_SHADOWMAP || GetRenderingStage() == DES_SHADOWMAP_CUBE);
        const bool isMainOrGhost = (GetRenderingStage() == DES_MAIN || GetRenderingStage() == DES_GHOST);
        const bool requiresMorphMeshSameAsMain = (GetRenderingStage() == DES_MAIN || GetRenderingStage() == DES_GHOST || GetRenderingStage() == DES_Z_PRE_PASS);

        // Collect all non-MorphMesh draws and handle MorphMesh/Cube per-draw

        struct NodeAttachmentDrawItem {
            uint64_t sortKey;
            MeshInfo* mesh;
            zCTexture* texture;    // null for shadow passes
            zCMaterial* material;
            NodeAttachmentInstanceData instanceData;
            bool needAlpha;
        };

        static std::vector<NodeAttachmentDrawItem> instancedDrawItems;
        instancedDrawItems.clear();

        // For the cube shadow path and MorphMesh, we need the old per-draw setup

        auto ensurePerDrawShaderSetup = [&]() {
            const VShaderID shader = useCubePath
                ? VShaderID::VS_ExNodeCube
                : VShaderID::VS_ExNode;
            if ( SetActiveVertexShader( shader ) != XR_SUCCESS || !ActiveVS ) {
                return false;
            }

            SetupVS_ExMeshDrawCall();
            SetupVS_ExConstantBuffer();
            return ActiveVS->Apply() == XR_SUCCESS;
        };

        // For MorphMesh per-draw calls, lazily initialized
        bool perDrawSetupDone = false;
        GraphicsShaderConstantBuffer perDrawMPI;

        auto ensurePerDrawReady = [&]() {
            if ( perDrawSetupDone ) {
                return perDrawMPI.Succeeded();
            }
            if ( !ensurePerDrawShaderSetup() ) {
                return false;
            }

            perDrawMPI = ActiveVS->GetBuffer( "Matrices_PerInstances" ).Bind();
            if ( !perDrawMPI.Succeeded() ) {
                return false;
            }

            if ( isMainOrGhost ) {
                if ( SetActivePixelShader( PShaderID::PS_DiffuseAlphaTest ) != XR_SUCCESS
                    || !ActivePS || ActivePS->Apply() != XR_SUCCESS ) {
                    return false;
                }
            }
            perDrawSetupDone = true;
            return true;
        };

        // If cube path, set up per-draw immediately since everything goes through it
        if ( useCubePath && !ensurePerDrawReady() ) {
            return;
        }

        for ( auto& data : tempVobList ) {
            SkeletalVobInfo* const vi = data.VobInfo;
            zCModel* const model = data.Model;
            if ( !vi || !vi->Vob || !model || vi->Vob->GetVisual() != model
                || data.BoneIdx > BoneTransformCache.size()
                || data.NumBones > BoneTransformCache.size() - data.BoneIdx ) {
                continue;
            }

            auto* nodeList = model->GetNodeList();
            if ( !nodeList || nodeList->NumInArray <= 0 || !nodeList->Array ) {
                continue;
            }

            const size_t nodeCount = (std::min)(
                data.NumBones, static_cast<size_t>(nodeList->NumInArray) );
            const std::span<const XMFLOAT4X4> transforms(
                BoneTransformCache.data() + data.BoneIdx, nodeCount );
            const float materialClassMarker =
                vi->Vob->GetVobType() == zVOB_TYPE_NSC ? -1.0f : 0.0f;
            auto modelColor = data.ModelColor;
            modelColor.w = vi->Vob->IsIndoorVob() ? 0.05f : 1.0f;
            const auto fatness = data.Fatness;
            auto& world = data.World;
            auto& prevWorld = data.PrevWorld;
            auto& nodeAttachments = vi->NodeAttachments;

            for ( size_t i = 0; i < transforms.size()
                && i <= static_cast<size_t>((std::numeric_limits<int>::max)()); ++i ) {
                const int nodeIndex = static_cast<int>(i);
                zCModelNodeInst* const node = nodeList->Array[i];
                if ( !node ) {
                    continue;
                }

                auto nodeAttachment = nodeAttachments.find( nodeIndex );
                if ( !node->NodeVisual ) {
                    if ( nodeAttachment != nodeAttachments.end() ) {
                        for ( MeshVisualInfo* cachedVisual : nodeAttachment->second ) {
                            delete cachedVisual;
                        }
                        nodeAttachments.erase( nodeAttachment );
                        LogInfo() << "Removed attachment from model " << model->GetVisualName();
                    }
                    continue;
                }

                if ( nodeAttachment == nodeAttachments.end() ) {
                    WorldConverter::ExtractNodeVisual( nodeIndex, node, nodeAttachments );
                    nodeAttachment = nodeAttachments.find( nodeIndex );
                } else if ( nodeAttachment->second.empty()
                    || !nodeAttachment->second.front()
                    || nodeAttachment->second.front()->Visual != node->NodeVisual ) {
                    for ( MeshVisualInfo* cachedVisual : nodeAttachment->second ) {
                        delete cachedVisual;
                    }
                    nodeAttachments.erase( nodeAttachment );
                    WorldConverter::ExtractNodeVisual( nodeIndex, node, nodeAttachments );
                    nodeAttachment = nodeAttachments.find( nodeIndex );
                }

                if ( nodeAttachment == nodeAttachments.end() || nodeAttachment->second.empty() ) {
                    continue;
                }

                if ( model->GetDrawHandVisualsOnly() ) {
                    if ( !node->ProtoNode ) {
                        continue;
                    }
                    const char* nodeNameText = node->ProtoNode->NodeName.ToChar();
                    if ( !nodeNameText ) {
                        continue;
                    }
                    const std::string nodeName = nodeNameText;
#ifdef BUILD_GOTHIC_2_6_fix
                    if ( nodeName.find( "HAND" ) == std::string::npos && (*reinterpret_cast<BYTE*>(0x57A694) != 0x90 || nodeName.find( "ARM" ) == std::string::npos) ) {
#else
                    if ( nodeName.find( "HAND" ) == std::string::npos ) {
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
                    if ( !mvi || !mvi->Visual ) {
                        LogWarn() << "Attachment without visual on model: " << model->GetVisualName();
                        continue;
                    }

                    const char* extension = mvi->Visual->GetFileExtension( 0 );
                    const bool isMMS = extension && strcmp( extension, ".MMS" ) == 0;
                    if ( updateState ) {
                        node->TexAniState.UpdateTexList();
                        if ( isMMS ) {
                            zCMorphMesh* mm = reinterpret_cast<zCMorphMesh*>(mvi->Visual);
                            if ( auto* textureAnimation = mm->GetTexAniState() ) {
                                textureAnimation->UpdateTexList();
                            }
                        }
                    }

                    // MorphMesh: always per-draw
                    if ( isMMS && distance < 1000 ) {
                        // Only 0.35f of the fatness wanted by gothic.
                        // They seem to compensate for that with the scaling.

                        if ( !ensurePerDrawReady() ) {
                            return;
                        }

                        VS_ExConstantBuffer_PerInstanceNode instanceInfo{};
                        instanceInfo.Color = modelColor;
                        instanceInfo.Fatness = std::max<float>( 0.f, fatness * 0.35f );
                        instanceInfo.Scaling = fatness * 0.02f + 1.f;
                        instanceInfo.World = finalWorld;
                        instanceInfo.PrevWorld = finalPrevWorld;
                        if ( !perDrawMPI.Update( &instanceInfo ).Succeeded() ) {
                            LogError() << "Skeletal attachments: Failed to update per-draw data.";
                            return;
                        }

                        if ( requiresMorphMeshSameAsMain ) {
                            zCMorphMesh* mm = reinterpret_cast<zCMorphMesh*>( mvi->Visual );
                            if ( updateState && mvi->LastAniUpdateFrame != now ) {
                                WorldConverter::UpdateMorphMeshVisual( mm, mvi );
                                mvi->LastAniUpdateFrame = now;
                            }
                            Engine::GAPI->DrawMorphMesh( mm, mvi->Meshes );
                            continue;
                        }

                        if ( isShadowPass ) {
                            for ( auto const& itm : mvi->Meshes ) {
                                for ( MeshInfo* mesh : itm.second ) {
                                    if ( IsDrawableMeshInfo( mesh ) ) {
                                        Engine::GAPI->DrawMeshInfo( itm.first, mesh );
                                    }
                                }
                            }
                        } else {
                            for ( auto const& itm : mvi->Meshes ) {
                                zCTexture* texture = nullptr;
                                if ( itm.first && (texture = itm.first->GetAniTexture()) != nullptr
                                    && !bindTextureForPass( texture, materialClassMarker ) ) {
                                    continue;
                                }
                                for ( MeshInfo* mesh : itm.second ) {
                                    if ( IsDrawableMeshInfo( mesh ) ) {
                                        Engine::GAPI->DrawMeshInfo( itm.first, mesh );
                                    }
                                }
                            }
                        }
                        continue;
                    }

                    // DES_SHADOWMAP_CUBE: per-draw path (SV_InstanceID conflict with instancing)
                    if ( useCubePath ) {
                        VS_ExConstantBuffer_PerInstanceNode instanceInfo{};
                        instanceInfo.Color = modelColor;
                        instanceInfo.Fatness = 0.f;
                        instanceInfo.Scaling = 1.f;
                        instanceInfo.World = finalWorld;
                        instanceInfo.PrevWorld = finalPrevWorld;
                        if ( !perDrawMPI.Update( &instanceInfo ).Succeeded() ) {
                            LogError() << "Skeletal attachments: Failed to update cube per-draw data.";
                            return;
                        }

                        for ( auto const& itm : mvi->Meshes ) {
                            for ( MeshInfo* mesh : itm.second ) {
                                if ( IsDrawableMeshInfo( mesh ) ) {
                                    Engine::GAPI->DrawMeshInfo( itm.first, mesh );
                                }
                            }
                        }
                        continue;
                    }

                    // Non-MMS, non-cube: collect for instanced drawing
                    NodeAttachmentInstanceData instData{};
                    instData.World = finalWorld;
                    instData.PrevWorld = finalPrevWorld;
                    instData.Color = modelColor;

                    for ( auto const& itm : mvi->Meshes ) {
                        zCTexture* texture = nullptr;
                        FrameGeometryCache::SortKeyBuilder sortKeyBase = { 0 };
                        if ( itm.first ) {
                            texture = itm.first->GetAniTexture();
                            const char* textureName = texture ? texture->__GetName().ToChar() : nullptr;
                            const bool skipShadowTexture = isShadowPass && textureName
                                && strncmp( textureName, "HUM_TEETH_V0.TGA",
                                    std::size( "HUM_TEETH_V0.TGA" ) - 1 ) == 0;
                            if ( !texture || skipShadowTexture
                                || texture->CacheIn( 0.6f ) != zRES_CACHED_IN ) {
                                // Cache first so alpha and material state are reliable.
                                continue;
                            }
                            if ( texture->HasAlphaChannel() ) {
                                sortKeyBase.withAlphaType( 1 );
                            }
                            sortKeyBase.withTexture(reinterpret_cast<size_t>(texture));
                        }

                        for ( MeshInfo* mesh : itm.second ) {
                            if ( !IsDrawableMeshInfo( mesh ) ) {
                                continue;
                            }

                            FrameGeometryCache::SortKeyBuilder meshSortKey = sortKeyBase;
                            meshSortKey.withMesh( mesh->meshId );
                            instancedDrawItems.emplace_back(
                                meshSortKey.sortKey, mesh, texture, itm.first, instData,
                                (texture && texture->HasAlphaChannel())
                                    || (itm.first && itm.first->HasAlphaTest()) );
                        }
                    }
                }
            }
        }

        if ( instancedDrawItems.empty() )
            return;

        std::sort( instancedDrawItems.begin(), instancedDrawItems.end(),
            []( const NodeAttachmentDrawItem& a, const NodeAttachmentDrawItem& b ) {
                if ( a.sortKey != b.sortKey ) {
                    return a.sortKey < b.sortKey;
                }
                return std::less<zCMaterial*>{}( a.material, b.material );
            } );

        // Ensure instance buffer is large enough
        constexpr size_t maxInstanceCount =
            (std::numeric_limits<UINT>::max)() / sizeof( NodeAttachmentInstanceData );
        if ( !NodeAttachmentInstancingBuffer
            || instancedDrawItems.size() > maxInstanceCount ) {
            LogError() << "Node attachment instance data exceeds the D3D11 buffer limit.";
            return;
        }
        const UINT neededBytes = static_cast<UINT>(
            instancedDrawItems.size() * sizeof( NodeAttachmentInstanceData ) );
        if ( !NodeAttachmentInstancingBuffer->IsValid()
            || NodeAttachmentInstancingBuffer->GetSizeInBytes() < neededBytes ) {
            if ( NodeAttachmentInstancingBuffer->Init(
                    nullptr, neededBytes,
                    D3D11VertexBuffer::B_VERTEXBUFFER, D3D11VertexBuffer::U_DYNAMIC,
                    D3D11VertexBuffer::CA_WRITE ) != XR_SUCCESS ) {
                LogError() << "Failed to create instance buffer for node attachments!";
                return;
            }
            SetDebugName( NodeAttachmentInstancingBuffer->GetVertexBuffer().Get(),
                "NodeAttachmentInstancingBuffer" );
        }

        // Build batch list and upload instance data
        struct InstanceBatch {
            MeshInfo* mesh;
            zCTexture* texture;
            zCMaterial* material;
            unsigned int startInstance;
            unsigned int instanceCount;
            bool needAlpha;
        };

        static std::vector<InstanceBatch> batches;
        batches.clear();

        void* mappedData = nullptr;
        UINT mappedSize = 0;
        if ( NodeAttachmentInstancingBuffer->Map( D3D11VertexBuffer::M_WRITE_DISCARD,
                &mappedData, &mappedSize ) != XR_SUCCESS ) {
            LogError() << "Failed to map instance buffer for node attachments!";
            return;
        }
        if ( !mappedData || mappedSize < neededBytes ) {
            NodeAttachmentInstancingBuffer->Unmap();
            LogError() << "Mapped node attachment buffer is smaller than requested.";
            return;
        }

        auto* destData = static_cast<NodeAttachmentInstanceData*>(mappedData);
        unsigned int currentIdx = 0;

        for ( size_t i = 0; i < instancedDrawItems.size(); ) {
            MeshInfo* const batchMesh = instancedDrawItems[i].mesh;
            const uint16_t meshId = batchMesh->meshId;
            zCTexture* const batchTex = instancedDrawItems[i].texture;
            zCMaterial* const batchMat = instancedDrawItems[i].material;
            const unsigned int batchStartInstance = currentIdx;

            bool needAlpha = false;
            if ( meshId == 0 ) {
                needAlpha = instancedDrawItems[i].needAlpha;
                destData[currentIdx++] = instancedDrawItems[i].instanceData;
                ++i;
            } else {
                while ( i < instancedDrawItems.size()
                    && instancedDrawItems[i].mesh->meshId == meshId
                    && instancedDrawItems[i].texture == batchTex
                    && instancedDrawItems[i].material == batchMat ) {
                    needAlpha |= instancedDrawItems[i].needAlpha;
                    destData[currentIdx++] = instancedDrawItems[i].instanceData;
                    ++i;
                }
            }

            batches.push_back( { batchMesh, batchTex, batchMat,
                batchStartInstance, currentIdx - batchStartInstance, needAlpha } );
        }

        if ( NodeAttachmentInstancingBuffer->Unmap() != XR_SUCCESS ) {
            LogError() << "Failed to unmap instance buffer for node attachments!";
            return;
        }

        // Draw calls

        if ( SetActiveVertexShader( VShaderID::VS_ExNodeInstanced ) != XR_SUCCESS
            || !ActiveVS ) {
            return;
        }
        SetupVS_ExMeshDrawCall();
        SetupVS_ExConstantBuffer();
        if ( ActiveVS->Apply() != XR_SUCCESS ) {
            return;
        }

        if ( isMainOrGhost
            && (SetActivePixelShader( PShaderID::PS_DiffuseAlphaTest ) != XR_SUCCESS
                || !ActivePS || ActivePS->Apply() != XR_SUCCESS) ) {
            return;
        }

        Context->IASetPrimitiveTopology( D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST );

        // Bind instance buffer to slot 1 (persists across batches)
        UINT instOffset = 0;
        UINT instStride = sizeof( NodeAttachmentInstanceData );
        Context->IASetVertexBuffers( 1, 1,
            NodeAttachmentInstancingBuffer->GetVertexBuffer().GetAddressOf(),
            &instStride, &instOffset );
        struct ScopedInstanceBufferUnbind {
            ID3D11DeviceContext* Context = nullptr;
            ~ScopedInstanceBufferUnbind() {
                if ( !Context ) {
                    return;
                }
                ID3D11Buffer* nullBuffer = nullptr;
                UINT zero = 0;
                Context->IASetVertexBuffers( 1, 1, &nullBuffer, &zero, &zero );
            }
        } instanceBufferUnbind { Context.Get() };

        wantShader = true;
        if ( isShadowPass ) {
            const bool linearDepth =
                (graphicsState.FF_GSwitches & GSWITCH_LINEAR_DEPTH) != 0;
            if ( linearDepth ) {
                ActivePS = ShaderManager->GetPShader( PShaderID::PS_LinDepth );
                if ( !ActivePS || ActivePS->Apply() != XR_SUCCESS ) {
                    return;
                }
            } else {
                Context->PSSetShader( nullptr, nullptr, 0 );
                ActivePS.reset();
            }
            wantShader = false;
        } else if ( isZPrepass ) {
            Context->PSSetShader( nullptr, nullptr, 0 );
            ActivePS.reset();
            wantShader = false;
        }
        const auto opaquePixelShader = ActivePS;

        D3D11VertexBuffer* lastVB = nullptr;
        D3D11VertexBuffer* lastIB = nullptr;

        MaterialInfo* lastMaterialInfo = nullptr;

        zCTexture* lastBatchTex = nullptr;
        auto lastSwitches = graphicsState.FF_GSwitches;
        D3D11PShader* lastPs = ActivePS.get();

        // otherwise shadows of streetlamps are not accurate
        const bool needsAlphaTesting = isShadowPass || isZPrepass;

        for ( const auto& batch : batches ) {
            MeshInfo* const mi = batch.mesh;
            if ( !IsDrawableMeshInfo( mi ) || batch.instanceCount == 0 ) {
                continue;
            }

            MaterialInfo* info = nullptr;
            if ( batch.texture ) {
                info = Engine::GAPI->GetMaterialInfoFrom( batch.texture );
                if ( needsAlphaTesting && info
                    && info->MaterialType == MaterialInfo::MT_FullAlpha ) {
                    continue;
                }
            }

            // Bind texture for non-shadow passes
            if ( needsAlphaTesting ) {
                if ( batch.needAlpha ) {
                    if ( !batch.texture
                        || batch.texture->CacheIn( 0.6f ) != zRES_CACHED_IN ) {
                        continue;
                    }
                    auto* surface = batch.texture->GetSurface();
                    auto* engineTexture = surface ? surface->GetEngineTexture() : nullptr;
                    if ( !engineTexture || !engineTexture->IsValid() ) {
                        continue;
                    }
                    if ( lastBatchTex != batch.texture ) {
                        engineTexture->BindToPixelShader( 0 );
                        lastBatchTex = batch.texture;
                    }
                    if ( SetActivePixelShader(
                            PShaderID::PS_DiffuseAlphaTestShadows ) != XR_SUCCESS
                        || !ActivePS ) {
                        return;
                    }
                    if ( ActivePS.get() != lastPs ) {
                        if ( ActivePS->Apply() != XR_SUCCESS ) {
                            return;
                        }
                        lastPs = ActivePS.get();
                    }
                } else if ( ActivePS != opaquePixelShader ) {
                    ActivePS = opaquePixelShader;
                    if ( ActivePS ) {
                        if ( ActivePS->Apply() != XR_SUCCESS ) {
                            return;
                        }
                    } else {
                        Context->PSSetShader( nullptr, nullptr, 0 );
                    }
                    lastPs = ActivePS.get();
                }
            } else if ( wantShader && batch.texture && batch.texture != lastBatchTex ) {
                if ( !BindTextureNRFX( batch.texture, isMainOrGhost, info != lastMaterialInfo ) ) {
                    continue;
                }
                lastMaterialInfo = info;
                lastBatchTex = batch.texture;
            }

            // Set up alpha test state from material.
            if ( batch.material
                && batch.material->GetAlphaFunc() == zRND_ALPHA_FUNC_TEST ) {
                graphicsState.FF_GSwitches |= GSWITCH_ALPHAREF;
            } else {
                graphicsState.FF_GSwitches &= ~GSWITCH_ALPHAREF;
            }

            if ( lastSwitches != graphicsState.FF_GSwitches ) {
                lastSwitches = graphicsState.FF_GSwitches;
                if ( !cbFFPipelineConstantBuffer.Update( &graphicsState ).Succeeded()
                    || UpdateRenderStates() != XR_SUCCESS ) {
                    LogError() << "Skeletal attachments: Failed to update alpha-test state.";
                    return;
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

    }
}

/** Binds the active PixelShader */
XRESULT D3D11GraphicsEngine::BindActivePixelShader() {
    return ActivePS ? ActivePS->Apply() : XR_SUCCESS;
}

XRESULT D3D11GraphicsEngine::BindActiveVertexShader() {
    return ActiveVS ? ActiveVS->Apply() : XR_SUCCESS;
}

/** Unbinds the texture at the given slot */
XRESULT D3D11GraphicsEngine::UnbindTexture( int slot ) {
    GetContext()->PSSetShaderResources( slot, 1, s_nullSRVs );
    GetContext()->VSSetShaderResources( slot, 1, s_nullSRVs );

    return XR_SUCCESS;
}

/** Recreates the renderstates */
XRESULT D3D11GraphicsEngine::UpdateRenderStates() {
    if ( !Engine::GAPI || !GetContext() ) {
        return XR_INVALID_ARG;
    }

    auto& states = Engine::GAPI->GetRendererState();
    auto& blendState = states.BlendState;
    auto& rasterState = states.RasterizerState;
    auto& depthState = states.DepthState;
    auto& context = GetContext();

    if ( blendState.StateDirty ) {
        if ( !FFBlendState || !(blendState == m_BoundBlendState) ) {
            D3D11BlendStateInfo* state = nullptr;
            auto entry = GothicStateCache::s_BlendStateMap.find( blendState );
            if ( entry != GothicStateCache::s_BlendStateMap.end() ) {
                state = static_cast<D3D11BlendStateInfo*>(entry->second);
            } else {
                auto candidate = std::make_unique<D3D11BlendStateInfo>( blendState );
                if ( !candidate->IsValid() ) {
                    LogError() << "Could not create the requested blend state.";
                    return XR_FAILED;
                }
                const auto [insertedEntry, inserted] =
                    GothicStateCache::s_BlendStateMap.emplace( blendState, candidate.get() );
                state = static_cast<D3D11BlendStateInfo*>(insertedEntry->second);
                if ( inserted ) candidate.release();
            }
            if ( !state || !state->IsValid() ) {
                LogError() << "The cached blend state is invalid.";
                return XR_FAILED;
            }

            FFBlendState = state->State.Get();
            m_BoundBlendState = blendState;
            const float blendFactor[4] = {};
            context->OMSetBlendState( FFBlendState.Get(), blendFactor, 0xFFFFFFFF );
        }
        blendState.StateDirty = false;
    }

    if ( rasterState.StateDirty ) {
        if ( !FFRasterizerState || !(rasterState == m_BoundRasterizerState) ) {
            D3D11RasterizerStateInfo* state = nullptr;
            auto entry = GothicStateCache::s_RasterizerStateMap.find( rasterState );
            if ( entry != GothicStateCache::s_RasterizerStateMap.end() ) {
                state = static_cast<D3D11RasterizerStateInfo*>(entry->second);
            } else {
                auto candidate = std::make_unique<D3D11RasterizerStateInfo>( rasterState );
                if ( !candidate->IsValid() ) {
                    LogError() << "Could not create the requested rasterizer state.";
                    return XR_FAILED;
                }
                const auto [insertedEntry, inserted] =
                    GothicStateCache::s_RasterizerStateMap.emplace( rasterState, candidate.get() );
                state = static_cast<D3D11RasterizerStateInfo*>(insertedEntry->second);
                if ( inserted ) candidate.release();
            }
            if ( !state || !state->IsValid() ) {
                LogError() << "The cached rasterizer state is invalid.";
                return XR_FAILED;
            }

            FFRasterizerState = state->State.Get();
            m_BoundRasterizerState = rasterState;
            context->RSSetState( FFRasterizerState.Get() );
        }
        rasterState.StateDirty = false;
    }

    if ( depthState.StateDirty ) {
        if ( !FFDepthStencilState || !(depthState == m_BoundDepthState) ) {
            D3D11DepthBufferState* state = nullptr;
            auto entry = GothicStateCache::s_DepthBufferMap.find( depthState );
            if ( entry != GothicStateCache::s_DepthBufferMap.end() ) {
                state = static_cast<D3D11DepthBufferState*>(entry->second);
            } else {
                auto candidate = std::make_unique<D3D11DepthBufferState>( depthState );
                if ( !candidate->IsValid() ) {
                    LogError() << "Could not create the requested depth-stencil state.";
                    return XR_FAILED;
                }
                const auto [insertedEntry, inserted] =
                    GothicStateCache::s_DepthBufferMap.emplace( depthState, candidate.get() );
                state = static_cast<D3D11DepthBufferState*>(insertedEntry->second);
                if ( inserted ) candidate.release();
            }
            if ( !state || !state->IsValid() ) {
                LogError() << "The cached depth-stencil state is invalid.";
                return XR_FAILED;
            }

#ifdef DEBUG_D3D11
            static const char* comparisonNames[] = {
                "NONE", "NEVER", "LESS", "EQUAL", "LESS_EQUAL",
                "GREATER", "NOT_EQUAL", "GREATER_EQUAL", "ALWAYS"
            };
            const int comparisonIndex = static_cast<int>(state->Values.DepthBufferCompareFunc);
            const char* comparisonName = comparisonIndex >= 0
                && comparisonIndex < static_cast<int>(std::size( comparisonNames ))
                ? comparisonNames[comparisonIndex]
                : "INVALID";
            std::stringstream name;
            name << (state->Values.DepthBufferEnabled ? "E1" : "E0") << "|"
                << (state->Values.DepthWriteEnabled ? "W1" : "W0") << "|"
                << comparisonName;
            SetDebugName( state->State.Get(), name.str() );
#endif

            FFDepthStencilState = state->State.Get();
            m_BoundDepthState = depthState;
            context->OMSetDepthStencilState( FFDepthStencilState.Get(), 0 );
        }
        depthState.StateDirty = false;
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

    bool UsesShadowIndexBuffer( const MeshInfo* mesh, bool isAlpha ) {
        return mesh && !isAlpha
            && mesh->MeshShadowIndexBuffer
            && mesh->MeshShadowIndexBuffer->IsValid()
            && !mesh->ShadowIndices.empty()
            && mesh->ShadowIndices.size()
                <= (std::numeric_limits<unsigned int>::max)();
    }

    D3D11VertexBuffer* GetShadowAwareIndexBuffer( MeshInfo* mesh, bool isAlpha ) {
        if ( UsesShadowIndexBuffer( mesh, isAlpha ) ) {
            return mesh->MeshShadowIndexBuffer;
        }
        if ( !mesh || !mesh->MeshIndexBuffer
            || !mesh->MeshIndexBuffer->IsValid()
            || mesh->Indices.empty()
            || mesh->Indices.size()
                > (std::numeric_limits<unsigned int>::max)() ) {
            return nullptr;
        }
        return mesh->MeshIndexBuffer;
    }

    unsigned int GetShadowAwareIndexCount( const MeshInfo* mesh, bool isAlpha ) {
        if ( UsesShadowIndexBuffer( mesh, isAlpha ) ) {
            return static_cast<unsigned int>(mesh->ShadowIndices.size());
        }
        if ( !mesh || !mesh->MeshIndexBuffer
            || !mesh->MeshIndexBuffer->IsValid()
            || mesh->Indices.empty()
            || mesh->Indices.size()
                > (std::numeric_limits<unsigned int>::max)() ) {
            return 0;
        }
        return static_cast<unsigned int>(mesh->Indices.size());
    }

    bool IsDrawableShadowMesh( MeshInfo* mesh, bool isAlpha ) {
        return mesh && mesh->MeshVertexBuffer
            && mesh->MeshVertexBuffer->IsValid()
            && GetShadowAwareIndexBuffer( mesh, isAlpha )
            && GetShadowAwareIndexCount( mesh, isAlpha ) > 0;
    }
}

/** Called when we started to render the world */
XRESULT D3D11GraphicsEngine::OnStartWorldRendering() {
    TracyD3D11ZoneCGX( "D3D11GraphicsEngine::OnStartWorldRendering");

    if ( !Engine::GAPI || !Context || !GetDevice() || !PfxRenderer
        || !PfxRenderer->GetTexturePool()
        || !HDRBackBuffer || !HDRBackBuffer->IsValid()
        || !VelocityBuffer || !VelocityBuffer->IsValid()
        || !Backbuffer || !Backbuffer->IsValid()
        || !DepthStencilBuffer || !DepthStencilBuffer->IsValid()
        || !DepthStencilBufferCopy || !DepthStencilBufferCopy->IsValid()
        || !m_SwapchainDepthStencilBuffer
        || !m_SwapchainDepthStencilBuffer->IsValid() ) {
        LogError() << "World rendering resources are unavailable.";
        return XR_FAILED;
    }

    SetDefaultStates();
    m_FrameNeedsJitter = false;

    auto& rendererState = Engine::GAPI->GetRendererState();

    if ( rendererState.RendererSettings.DisableRendering )
        return XR_SUCCESS;

    // return XR_SUCCESS;
    if ( PresentPending ) return XR_SUCCESS;

    RenderGraph graph( GetPfxRenderer()->GetTexturePool() );

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
    FrameTransparencyMeshesWaterfall.clear();
    FrameTransparencyMeshesWetSSRBlockers.clear();
    m_FrameGeometryCache.Reset();

    // TODO: TODO: Hack for texture caching!
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
        LogError() << "No active scene renderer is available.";
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
            TracyD3D11ZoneCGX( "D3D11GraphicsEngine::Draw ParticleFX #1" );
            zCCamera* const camera = zCCamera::GetCamera();
            if ( !camera ) {
                return;
            }
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

    ActiveSceneRenderer->AddLightingPasses( graph, *this,
        colorResource, normalsResource, specularResource,
        transparencyAndCompositionMaskResource, backBufferHandle, m_FrameLights );

    // XeGTAO is composited before transparent alpha meshes so particles, fire and
    // other translucent effects are not darkened by opaque-world AO.
    if ( rendererState.RendererSettings.AoMode == AOMode::AO_XEGTAO ) {
        graph.AddPass( RG_PASS_NAME("XeGTAO"), [&]( RGBuilder& builder, RenderPass& pass ) {
            builder.Read( normalsResource );
            builder.Write( backBufferHandle );

            pass.m_executeCallback = [this, normalsResource, backBufferHandle]( const RenderGraph& graph ) {
                TracyD3D11ZoneCGX( "D3D11GraphicsEngine::Draw XeGTAO" );
                auto context = GetContext();
                auto* normalsTexture = graph.GetPhysicalTexture( normalsResource );
                auto* backBuffer = graph.GetPhysicalTexture( backBufferHandle );
                auto* depthCopy = GetDepthBufferCopy();
                if ( !context || !PfxRenderer
                    || !normalsTexture || !normalsTexture->IsValid()
                    || !backBuffer || !backBuffer->IsValid()
                    || !depthCopy || !depthCopy->IsValid() ) {
                    return;
                }
                PfxRenderer->RenderXeGTAO(
                    depthCopy->GetShaderResView().Get(),
                    normalsTexture->GetShaderResView().Get(),
                    backBuffer->GetRenderTargetView().Get() );
                if ( DefaultSamplerState ) {
                    context->PSSetSamplers(
                        0, 1, DefaultSamplerState.GetAddressOf() );
                }
            };
        } );
    }

    graph.AddPass( RG_PASS_NAME("Draw Frame AlphaMeshes"), [&]( RGBuilder& builder, RenderPass& pass ) {
        // Setup / Declare
        builder.Write( backBufferHandle );

        pass.m_executeCallback = [this](const RenderGraph&)-> void {
            TracyD3D11ZoneCGX( "D3D11GraphicsEngine::Draw Frame AlphaMeshes" );
            DrawFrameAlphaMeshes();
            };
        }
    );

    // Shared state for the PostFX composition pass
    ID3D11ShaderResourceView* compositionGodRaysSRV = nullptr;
    ID3D11ShaderResourceView* compositionScreenSpaceLightingSRV = nullptr;
    auto* loadedWorldInfo = Engine::GAPI->GetLoadedWorldInfo();
    const bool isOutdoor = loadedWorldInfo && loadedWorldInfo->BspTree
        && loadedWorldInfo->BspTree->GetBspTreeMode() == zBSP_MODE_OUTDOOR;
    bool compositionGodRays = (rendererState.RendererSettings.EnableGodRays && isOutdoor);
    bool compositionHeightFog = (rendererState.RendererSettings.DrawFog && isOutdoor);
    const float dynamicCloudRainWeight = Engine::GAPI->GetRainFXWeight();
    bool compositionLowClouds = rendererState.RendererSettings.EnableDynamicClouds
        && rendererState.RendererSettings.DrawFog && isOutdoor
        && dynamicCloudRainWeight < 0.999f;
    bool compositionContactShadows = rendererState.RendererSettings.EnableContactShadows;
    bool compositionSSGI = rendererState.RendererSettings.EnableScreenSpaceGI && rendererState.RendererSettings.ScreenSpaceGIStrength > 0.0f;
    bool compositionNeedsGeometry = compositionContactShadows || compositionSSGI;
    bool compositionNeedsDepth = compositionHeightFog;
    bool compositionActive = compositionGodRays || compositionNeedsDepth
        || compositionNeedsGeometry;

    const bool fsr3UpscalingActive = GetDevice()->GetFeatureLevel() >= D3D_FEATURE_LEVEL_11_0
        && rendererState.RendererSettings.Upscaler == GothicRendererSettings::UPSCALER_FSR_3
        && rendererState.RendererSettings.ResolutionScalePercent <= 100
        && rendererState.RendererSettings.AntiAliasingMode == GothicRendererSettings::AA_FSR3
        && PfxRenderer && PfxRenderer->GetFSR3() && TemporalState;
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
            //// Setup / Declare
            //RGTextureDesc albedoDesc{ 1920, 1080, 28 /* DXGI_FORMAT_R8G8B8A8_UNORM */, "Albedo" };
            //albedoTarget = builder.CreateTexture( albedoDesc );
            builder.Write( backBufferHandle );

            pass.m_executeCallback = [this, backBufferHandle]( const RenderGraph& graph )->void {
                TracyD3D11ZoneCGX( "D3D11GraphicsEngine::Draw Sky" );
                // Draw back of the sky if outdoor
                GetContext()->OMSetRenderTargets( 1, graph.GetPhysicalTexture( backBufferHandle )->GetRenderTargetView().GetAddressOf(), GetDepthBuffer()->GetDepthStencilView().Get() );

                DrawSky();
            };
        } );
    }
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
    if ( compositionLowClouds ) {
        const INT2 cloudResolution(
            std::max( 1, GetResolution().x / 2 + GetResolution().x % 2 ),
            std::max( 1, GetResolution().y / 2 + GetResolution().y % 2 ) );

        graph.AddPass( RG_PASS_NAME("Generate Low Clouds"), [&]( RGBuilder& builder, RenderPass& pass ) {
            builder.Read( backBufferHandle );
            lowCloudLayerResource = builder.CreateTexture( {
                static_cast<uint32_t>(cloudResolution.x),
                static_cast<uint32_t>(cloudResolution.y),
                GetBackBufferFormat(),
                L"LowCloudLayer"
            } );
            lowCloudDepthResource = builder.CreateTexture( {
                static_cast<uint32_t>(cloudResolution.x),
                static_cast<uint32_t>(cloudResolution.y),
                DXGI_FORMAT_R32_FLOAT,
                L"LowCloudDepth"
            } );

            pass.m_executeCallback = [
                this, backBufferHandle, lowCloudLayerResource,
                lowCloudDepthResource
            ]( const RenderGraph& graph ) {
                TracyD3D11ZoneCGX( "D3D11GraphicsEngine::Generate Low Clouds" );

                auto context = GetContext();
                auto* backBuffer = graph.GetPhysicalTexture( backBufferHandle );
                auto* lowCloudLayer =
                    graph.GetPhysicalTexture( lowCloudLayerResource );
                auto* lowCloudDepth =
                    graph.GetPhysicalTexture( lowCloudDepthResource );
                auto* depthBuffer = GetDepthBuffer();
                if ( !context || !PfxRenderer
                    || !backBuffer || !backBuffer->IsValid()
                    || !lowCloudLayer || !lowCloudLayer->IsValid()
                    || !lowCloudDepth || !lowCloudDepth->IsValid()
                    || !depthBuffer || !depthBuffer->IsValid() ) {
                    return;
                }

                const float clearValue[4] = {};
                context->ClearRenderTargetView(
                    lowCloudLayer->GetRenderTargetView().Get(), clearValue );
                context->ClearRenderTargetView(
                    lowCloudDepth->GetRenderTargetView().Get(), clearValue );

                PfxRenderer->RenderLowCloudLayer(
                    lowCloudLayer->GetRenderTargetView().Get(),
                    lowCloudDepth->GetRenderTargetView().Get(),
                    backBuffer->GetShaderResView().Get(),
                    depthBuffer->GetShaderResView().Get() );

                if ( DefaultSamplerState ) {
                    context->PSSetSamplers(
                        0, 1, DefaultSamplerState.GetAddressOf() );
                }
            };
        } );
    }
    const bool renderRainExclusionMask = rendererState.RendererSettings.EnableRain
        && Engine::GAPI->GetSceneWetness() > 1e-6f && isOutdoor;
    const bool renderWaterMask = renderRainExclusionMask || compositionNeedsGeometry;
    const bool renderWetGroundSSR = rendererState.RendererSettings.EnableSSR
        && renderRainExclusionMask;
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
    const bool fsr3ActiveForReactiveMask = rendererState.RendererSettings.AntiAliasingMode == GothicRendererSettings::AA_FSR3
        && rendererState.RendererSettings.Upscaler == GothicRendererSettings::UPSCALER_FSR_3;

    graph.AddPass( RG_PASS_NAME("DrawWaterSurfaces"), [&]( RGBuilder& builder, RenderPass& pass ) {
        builder.Read( backBufferHandle );
        builder.Write( backBufferHandle );
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

        pass.m_executeCallback = [
            this, renderWaterMask, waterMaskResource,
            fsr3ActiveForReactiveMask, reactiveMaskResource,
            compositionLowClouds, lowCloudLayerResource
        ]( const RenderGraph& graph ) {
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
                auto* lowCloudLayer =
                    graph.GetPhysicalTexture( lowCloudLayerResource );
                lowCloudLayerSRV = lowCloudLayer && lowCloudLayer->IsValid()
                    ? lowCloudLayer->GetShaderResView().Get() : nullptr;
            }
            DrawWaterSurfaces(
                waterMaskRTV, fsr3ReactiveMaskRTV, lowCloudLayerSRV );
            if ( renderWaterMask ) {
                DrawWaterfallMask( waterMaskRTV );
            }
        };
    });

    if ( renderWetGroundSSR ) {
        graph.AddPass( RG_PASS_NAME("Wet Ground SSR"), [&]( RGBuilder& builder, RenderPass& pass ) {
            builder.Read( normalsResource );
            builder.Read( waterMaskResource );
            builder.Read( backBufferHandle );
            builder.Write( backBufferHandle );

            pass.m_executeCallback = [this, backBufferHandle, normalsResource, waterMaskResource](const RenderGraph& graph) {
                TracyD3D11ZoneCGX( "D3D11GraphicsEngine::Wet Ground SSR" );
                auto* backBuffer = graph.GetPhysicalTexture( backBufferHandle );
                auto* normals = graph.GetPhysicalTexture( normalsResource );
                auto* waterMask = graph.GetPhysicalTexture( waterMaskResource );
                auto tempBuffer = PfxRenderer->GetTempBuffer();

                GetContext()->CopyResource( tempBuffer->GetTexture().Get(), backBuffer->GetTexture().Get() );
                PfxRenderer->RenderWetGroundSSR(
                    backBuffer->GetRenderTargetView().Get(),
                    tempBuffer->GetShaderResView().Get(),
                    GetDepthBufferCopy()->GetShaderResView().Get(),
                    normals->GetShaderResView().Get(),
                    waterMask->GetShaderResView().Get() );
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

    graph.AddPass( RG_PASS_NAME("Draw FrameTransparencyMeshesWaterfall"), [&]( RGBuilder& builder, RenderPass& pass ) {
        builder.Read( backBufferHandle );
        builder.Write( backBufferHandle );

        pass.m_executeCallback = [this](const RenderGraph&) {
            TracyD3D11ZoneCGX( "D3D11GraphicsEngine::Draw FrameTransparencyMeshesWaterfall" );

            SetDefaultStates();

            // Setup renderstates
            Engine::GAPI->GetRendererState().RasterizerState.CullMode = GothicRasterizerStateInfo::CM_CULL_BACK;
            Engine::GAPI->GetRendererState().RasterizerState.SetDirty();

            DrawMeshInfoListAlphablended( FrameTransparencyMeshesWaterfall );
        };
    });

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

            // for Post-Processing FX we use the full viewport for now
            // TODO: introduce UV-scaling to PostFX
            SetViewport( ViewportInfo( 0, 0, GetResolution() ) );
        };
    });

    if (rendererState.RendererSettings.DrawFog &&
                Engine::GAPI->GetLoadedWorldInfo()->BspTree->GetBspTreeMode() ==
                zBSP_MODE_OUTDOOR && !compositionActive) {
        // Standalone heightfog pass is only used when composition is not active.
        // Kept as fallback for FL10 or edge cases.
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
                TracyD3D11ZoneCGX( "D3D11GraphicsEngine::Draw ParticleFX #2" );

                // Draw unlit decals
                // TODO: Only get them once!
                std::vector<zCVob*> decals;
                zCCamera::GetCamera()->Activate();
                // Camera->Activate breaks viewport
                SetViewport( ViewportInfo( 0, 0, GetResolution() ) );

                Engine::GAPI->GetVisibleDecalList( decals );

                // Draw stuff like candle-flames
                DrawDecalList( decals, false );
                DrawMQuadMarks();
            };
        });
    }

    if ( compositionGodRays ) {
        if ( compositionActive ) {
            // GodRays compute-only pass writes to pool texture and skips the final additive blit.
            graph.AddPass( RG_PASS_NAME("GodRays Compute"), [&]( RGBuilder& builder, RenderPass& pass ) {
                builder.Read( backBufferHandle );
                if ( compositionLowClouds ) {
                    builder.Read( lowCloudLayerResource );
                }

                pass.m_executeCallback = [
                    this, backBufferHandle, compositionLowClouds,
                    lowCloudLayerResource, &compositionGodRaysSRV
                ]( const RenderGraph& graph ) {
                    TracyD3D11ZoneCGX( "D3D11GraphicsEngine::Draw GodRays (Compute)" );

                    compositionGodRaysSRV = nullptr;
                    auto context = GetContext();
                    auto* backbufferResource =
                        graph.GetPhysicalTexture( backBufferHandle );
                    auto* depthBuffer = GetDepthBuffer();
                    if ( !context || !PfxRenderer
                        || !backbufferResource || !backbufferResource->IsValid()
                        || !depthBuffer || !depthBuffer->IsValid() ) {
                        return;
                    }

                    ID3D11ShaderResourceView* nullSRV = nullptr;
                    context->PSSetShaderResources( 5, 1, &nullSRV );
                    ID3D11ShaderResourceView* lowCloudLayerSRV = nullptr;
                    if ( compositionLowClouds ) {
                        auto* lowCloudLayer =
                            graph.GetPhysicalTexture( lowCloudLayerResource );
                        lowCloudLayerSRV = lowCloudLayer && lowCloudLayer->IsValid()
                            ? lowCloudLayer->GetShaderResView().Get() : nullptr;
                    }
                    if ( PfxRenderer->RenderGodRaysToTexture(
                            backbufferResource->GetShaderResView().Get(),
                            depthBuffer->GetShaderResView().Get(),
                            lowCloudLayerSRV,
                            &compositionGodRaysSRV ) != XR_SUCCESS ) {
                        compositionGodRaysSRV = nullptr;
                    }
                    if ( DefaultSamplerState ) {
                        context->PSSetSamplers(
                            0, 1, DefaultSamplerState.GetAddressOf() );
                    }
                };
            });
        } else {
            // Standalone GodRays pass (fallback when composition is not active)
            graph.AddPass( RG_PASS_NAME("Draw Godrays"), [&]( RGBuilder& builder, RenderPass& pass ) {
                builder.Read( backBufferHandle );
                builder.Write( backBufferHandle );

                pass.m_executeCallback = [this, backBufferHandle](const RenderGraph& graph) {
                    TracyD3D11ZoneCGX( "D3D11GraphicsEngine::Draw GodRays" );
                    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
                    GetContext()->PSSetShaderResources( 5, 1, srv.GetAddressOf() );

                    auto* backbufferResource = graph.GetPhysicalTexture( backBufferHandle );
                    auto* depthBuffer = GetDepthBuffer();
                    if ( PfxRenderer && backbufferResource && backbufferResource->IsValid()
                        && depthBuffer && depthBuffer->IsValid() ) {
                        PfxRenderer->RenderGodRays(
                            backbufferResource->GetShaderResView().Get(),
                            depthBuffer->GetShaderResView().Get(),
                            nullptr );
                    }
                    GetContext()->PSSetSamplers( 0, 1, DefaultSamplerState.GetAddressOf() );
                };
            });
        }
    }
    if ( compositionNeedsGeometry ) {
        graph.AddPass( RG_PASS_NAME("Screen-Space Lighting"), [&]( RGBuilder& builder, RenderPass& pass ) {
            builder.Read( backBufferHandle );
            builder.Read( normalsResource );
            builder.Read( specularResource );
            builder.Read( waterMaskResource );
            builder.Read( velocityBufferHandle );

            pass.m_executeCallback = [this, backBufferHandle, normalsResource, specularResource, waterMaskResource, velocityBufferHandle,
                                      &compositionScreenSpaceLightingSRV]( const RenderGraph& graph ) {
                TracyD3D11ZoneCGX( "D3D11GraphicsEngine::Screen-Space Lighting" );

                compositionScreenSpaceLightingSRV = nullptr;
                auto context = GetContext();
                auto* depthBuffer = GetDepthBuffer();
                auto* backBuffer = graph.GetPhysicalTexture( backBufferHandle );
                auto* normalsTexture = graph.GetPhysicalTexture( normalsResource );
                auto* specularTexture = graph.GetPhysicalTexture( specularResource );
                auto* waterMaskTexture = graph.GetPhysicalTexture( waterMaskResource );
                auto* velocityTexture = graph.GetPhysicalTexture( velocityBufferHandle );
                auto tempBuffer = PfxRenderer ? PfxRenderer->GetTempBuffer() : TextureHandle{};
                if ( !context || !PfxRenderer
                    || !backBuffer || !backBuffer->IsValid()
                    || !normalsTexture || !normalsTexture->IsValid()
                    || !specularTexture || !specularTexture->IsValid()
                    || !waterMaskTexture || !waterMaskTexture->IsValid()
                    || (velocityTexture && !velocityTexture->IsValid())
                    || !depthBuffer || !depthBuffer->IsValid()
                    || !tempBuffer || !tempBuffer->IsValid()
                    || !backBuffer->GetTexture() || !tempBuffer->GetTexture() ) {
                    return;
                }

                context->CopyResource(
                    tempBuffer->GetTexture().Get(), backBuffer->GetTexture().Get() );
                if ( PfxRenderer->RenderScreenSpaceLighting(
                        tempBuffer->GetShaderResView().Get(),
                        depthBuffer->GetShaderResView().Get(),
                        normalsTexture->GetShaderResView().Get(),
                        waterMaskTexture->GetShaderResView().Get(),
                        specularTexture->GetShaderResView().Get(),
                        velocityTexture
                            ? velocityTexture->GetShaderResView().Get() : nullptr,
                        &compositionScreenSpaceLightingSRV ) != XR_SUCCESS ) {
                    compositionScreenSpaceLightingSRV = nullptr;
                }
                if ( DefaultSamplerState ) {
                    context->PSSetSamplers(
                        0, 1, DefaultSamplerState.GetAddressOf() );
                }
            };
        } );
    }
    if ( fsr3ActiveForReactiveMask && compositionNeedsGeometry ) {
        graph.AddPass( RG_PASS_NAME("FSR3 Contact Shadow Mask"), [&]( RGBuilder& builder, RenderPass& pass ) {
            builder.Read( transparencyAndCompositionMaskResource );
            builder.Read( backBufferHandle );
            builder.Write( transparencyAndCompositionMaskResource );

            pass.m_executeCallback = [this, transparencyAndCompositionMaskResource, &compositionScreenSpaceLightingSRV]( const RenderGraph& graph ) {
                TracyD3D11ZoneCGX( "D3D11GraphicsEngine::FSR3 Contact Shadow Mask" );

                auto context = GetContext();
                auto device = GetDevice();
                if ( !compositionScreenSpaceLightingSRV || !context || !device
                    || !Engine::GAPI || !ShaderManager || !DefaultSamplerState ) {
                    return;
                }

                auto* maskTexture =
                    graph.GetPhysicalTexture( transparencyAndCompositionMaskResource );
                auto vs = ShaderManager->GetVShader( VShaderID::VS_PFX );
                auto ps = ShaderManager->GetPShader(
                    PShaderID::PS_PFX_FSR3TransparencyMask );
                if ( !maskTexture || !maskTexture->IsValid()
                    || !maskTexture->GetRenderTargetView()
                    || !vs || !vs->GetShader() || !ps || !ps->GetShader() ) {
                    return;
                }

                static Microsoft::WRL::ComPtr<ID3D11Device> contactMaskDevice;
                static Microsoft::WRL::ComPtr<ID3D11BlendState> contactMaskBlendState;
                if ( !contactMaskBlendState || contactMaskDevice.Get() != device.Get() ) {
                    contactMaskBlendState.Reset();
                    contactMaskDevice.Reset();
                    D3D11_BLEND_DESC blendDesc{};
                    blendDesc.RenderTarget[0].BlendEnable = TRUE;
                    blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
                    blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
                    blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_MAX;
                    blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
                    blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
                    blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_MAX;
                    blendDesc.RenderTarget[0].RenderTargetWriteMask =
                        D3D11_COLOR_WRITE_ENABLE_RED;
                    if ( FAILED( device->CreateBlendState(
                            &blendDesc, contactMaskBlendState.GetAddressOf() ) ) ) {
                        return;
                    }
                    contactMaskDevice = device;
                }

                Microsoft::WRL::ComPtr<ID3D11RenderTargetView> oldRTV;
                Microsoft::WRL::ComPtr<ID3D11DepthStencilView> oldDSV;
                Microsoft::WRL::ComPtr<ID3D11BlendState> oldBlendState;
                Microsoft::WRL::ComPtr<ID3D11RasterizerState> oldRasterizerState;
                Microsoft::WRL::ComPtr<ID3D11DepthStencilState> oldDepthState;
                Microsoft::WRL::ComPtr<ID3D11VertexShader> oldVS;
                Microsoft::WRL::ComPtr<ID3D11PixelShader> oldPS;
                Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> oldSRV;
                Microsoft::WRL::ComPtr<ID3D11SamplerState> oldSampler;
                FLOAT oldBlendFactor[4]{};
                UINT oldSampleMask = 0xffffffff;
                UINT oldStencilRef = 0;
                D3D11_PRIMITIVE_TOPOLOGY oldTopology{};
                std::array<D3D11_VIEWPORT,
                    D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE> oldViewports{};
                UINT oldViewportCount = static_cast<UINT>(oldViewports.size());
                context->OMGetRenderTargets(
                    1, oldRTV.GetAddressOf(), oldDSV.GetAddressOf() );
                context->OMGetBlendState(
                    oldBlendState.GetAddressOf(), oldBlendFactor, &oldSampleMask );
                context->OMGetDepthStencilState(
                    oldDepthState.GetAddressOf(), &oldStencilRef );
                context->RSGetState( oldRasterizerState.GetAddressOf() );
                context->RSGetViewports( &oldViewportCount, oldViewports.data() );
                context->IAGetPrimitiveTopology( &oldTopology );
                context->VSGetShader( oldVS.GetAddressOf(), nullptr, nullptr );
                context->PSGetShader( oldPS.GetAddressOf(), nullptr, nullptr );
                context->PSGetShaderResources( 0, 1, oldSRV.GetAddressOf() );
                context->PSGetSamplers( 0, 1, oldSampler.GetAddressOf() );

                auto& savedRendererState = Engine::GAPI->GetRendererState();
                const GothicBlendStateInfo previousBlendState =
                    savedRendererState.BlendState;
                const GothicDepthBufferStateInfo previousDepthState =
                    savedRendererState.DepthState;
                const GothicRasterizerStateInfo previousRasterizerState =
                    savedRendererState.RasterizerState;

                SetDefaultStates();
                savedRendererState.DepthState.DepthBufferCompareFunc =
                    GothicDepthBufferStateInfo::CF_COMPARISON_ALWAYS;
                savedRendererState.DepthState.DepthWriteEnabled = false;
                savedRendererState.DepthState.SetDirty();
                savedRendererState.RasterizerState.CullMode =
                    GothicRasterizerStateInfo::CM_CULL_NONE;
                savedRendererState.RasterizerState.SetDirty();

                bool passReady =
                    SetViewport( ViewportInfo( 0, 0, GetResolution() ) ) == XR_SUCCESS
                    && UpdateRenderStates() == XR_SUCCESS
                    && vs->Apply() == XR_SUCCESS
                    && ps->Apply() == XR_SUCCESS;
                if ( passReady ) {
                    ID3D11RenderTargetView* rtv =
                        maskTexture->GetRenderTargetView().Get();
                    context->OMSetRenderTargets( 1, &rtv, nullptr );
                    const FLOAT blendFactor[4]{};
                    context->OMSetBlendState(
                        contactMaskBlendState.Get(), blendFactor, 0xffffffff );

                    ID3D11ShaderResourceView* srv =
                        compositionScreenSpaceLightingSRV;
                    context->PSSetShaderResources( 0, 1, &srv );
                    context->PSSetSamplers(
                        0, 1, DefaultSamplerState.GetAddressOf() );
                    context->IASetPrimitiveTopology(
                        D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST );
                    context->Draw( 3, 0 );
                }

                ID3D11ShaderResourceView* nullSRV = nullptr;
                context->PSSetShaderResources( 0, 1, &nullSRV );

                savedRendererState.BlendState = previousBlendState;
                savedRendererState.DepthState = previousDepthState;
                savedRendererState.RasterizerState = previousRasterizerState;
                savedRendererState.BlendState.SetDirty();
                savedRendererState.DepthState.SetDirty();
                savedRendererState.RasterizerState.SetDirty();
                UpdateRenderStates();

                context->OMSetRenderTargets(
                    1, oldRTV.GetAddressOf(), oldDSV.Get() );
                context->OMSetBlendState(
                    oldBlendState.Get(), oldBlendFactor, oldSampleMask );
                context->OMSetDepthStencilState(
                    oldDepthState.Get(), oldStencilRef );
                context->RSSetState( oldRasterizerState.Get() );
                if ( oldViewportCount > 0 ) {
                    context->RSSetViewports(
                        oldViewportCount, oldViewports.data() );
                }
                context->IASetPrimitiveTopology( oldTopology );
                context->VSSetShader( oldVS.Get(), nullptr, 0 );
                context->PSSetShader( oldPS.Get(), nullptr, 0 );
                ID3D11ShaderResourceView* oldSRVRaw = oldSRV.Get();
                ID3D11SamplerState* oldSamplerRaw = oldSampler.Get();
                context->PSSetShaderResources( 0, 1, &oldSRVRaw );
                context->PSSetSamplers( 0, 1, &oldSamplerRaw );
            };
        } );
    }
    // PostFX Composition pass merges enabled atmospheric and lighting effects in one full-screen blit.
    if ( compositionActive ) {
        graph.AddPass( RG_PASS_NAME("PostFX Composition"), [&]( RGBuilder& builder, RenderPass& pass ) {
            builder.Read( backBufferHandle );
            builder.Write( backBufferHandle );

            pass.m_executeCallback = [this, backBufferHandle, compositionNeedsDepth, compositionNeedsGeometry,
                                      compositionHeightFog, &compositionGodRaysSRV, &compositionScreenSpaceLightingSRV](const RenderGraph& graph) {
                TracyD3D11ZoneCGX( "D3D11GraphicsEngine::PostFX Composition" );

                auto context = GetContext();
                auto* backBuffer = graph.GetPhysicalTexture( backBufferHandle );
                auto tempBuffer = PfxRenderer ? PfxRenderer->GetTempBuffer() : TextureHandle{};
                auto* depthBuffer = compositionNeedsDepth ? GetDepthBuffer() : nullptr;
                if ( !context || !PfxRenderer
                    || !backBuffer || !backBuffer->IsValid()
                    || !tempBuffer || !tempBuffer->IsValid()
                    || !backBuffer->GetTexture() || !tempBuffer->GetTexture()
                    || (compositionNeedsDepth
                        && (!depthBuffer || !depthBuffer->IsValid()))
                    || (compositionNeedsGeometry
                        && !compositionScreenSpaceLightingSRV) ) {
                    return;
                }

                // Copy the scene so it can be read while the graph target is written.
                context->CopyResource(
                    tempBuffer->GetTexture().Get(), backBuffer->GetTexture().Get() );
                ID3D11ShaderResourceView* depthSRV = depthBuffer
                    ? depthBuffer->GetShaderResView().Get() : nullptr;

                const XRESULT compositionResult =
                    PfxRenderer->RenderPostFXComposition(
                    backBuffer->GetRenderTargetView().Get(),
                    tempBuffer->GetShaderResView().Get(),
                    compositionGodRaysSRV,
                    depthSRV,
                    compositionScreenSpaceLightingSRV,
                    compositionHeightFog );

                if ( DefaultSamplerState ) {
                    context->PSSetSamplers(
                        0, 1, DefaultSamplerState.GetAddressOf() );
                }
                if ( compositionResult != XR_SUCCESS ) {
                    return;
                }
            };
        });
    }

    if ( compositionLowClouds ) {
        graph.AddPass( RG_PASS_NAME("PostFX Low Clouds"), [&]( RGBuilder& builder, RenderPass& pass ) {
            builder.Read( backBufferHandle );
            builder.Read( lowCloudLayerResource );
            builder.Read( lowCloudDepthResource );
            builder.Write( backBufferHandle );

            pass.m_executeCallback = [
                this, backBufferHandle, lowCloudLayerResource,
                lowCloudDepthResource
            ]( const RenderGraph& graph ) {
                TracyD3D11ZoneCGX( "D3D11GraphicsEngine::PostFX Low Clouds" );

                auto context = GetContext();
                auto* backBuffer = graph.GetPhysicalTexture( backBufferHandle );
                auto* lowCloudLayer =
                    graph.GetPhysicalTexture( lowCloudLayerResource );
                auto* lowCloudDepth =
                    graph.GetPhysicalTexture( lowCloudDepthResource );
                auto tempBuffer = PfxRenderer ? PfxRenderer->GetTempBuffer() : TextureHandle{};
                auto* depthBuffer = GetDepthBuffer();
                if ( !context || !PfxRenderer
                    || !backBuffer || !backBuffer->IsValid()
                    || !lowCloudLayer || !lowCloudLayer->IsValid()
                    || !lowCloudDepth || !lowCloudDepth->IsValid()
                    || !tempBuffer || !tempBuffer->IsValid()
                    || !depthBuffer || !depthBuffer->IsValid()
                    || !backBuffer->GetTexture() || !tempBuffer->GetTexture() ) {
                    return;
                }

                context->CopyResource(
                    tempBuffer->GetTexture().Get(), backBuffer->GetTexture().Get() );
                const XRESULT lowCloudResult = PfxRenderer->CompositeLowClouds(
                    backBuffer->GetRenderTargetView().Get(),
                    tempBuffer->GetShaderResView().Get(),
                    lowCloudLayer->GetShaderResView().Get(),
                    lowCloudDepth->GetShaderResView().Get(),
                    depthBuffer->GetShaderResView().Get() );

                if ( DefaultSamplerState ) {
                    context->PSSetSamplers(
                        0, 1, DefaultSamplerState.GetAddressOf() );
                }
                if ( lowCloudResult != XR_SUCCESS ) {
                    return;
                }
            };
        } );
    }
    if ( rendererState.RendererSettings.EnableDoF ) {
        graph.AddPass( RG_PASS_NAME("Draw DepthOfField"), [&]( RGBuilder& builder, RenderPass& pass ) {
            builder.Read( backBufferHandle );
            builder.Write( backBufferHandle );

            pass.m_executeCallback = [this, backBufferHandle](const RenderGraph& graph) {
                TracyD3D11ZoneCGX( "D3D11GraphicsEngine::Draw DepthOfField" );
                auto* backbufferResource =
                    graph.GetPhysicalTexture( backBufferHandle );
                if ( PfxRenderer && backbufferResource
                    && backbufferResource->IsValid() ) {
                    PfxRenderer->RenderDepthOfField(
                        backbufferResource->GetShaderResView().Get() );
                }
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

            auto* particleColor = graph.GetPhysicalTexture( particleColorHandle );
            auto* particleDistortion =
                graph.GetPhysicalTexture( particleDistortionHandle );
            auto* reactiveMask = fsr3ActiveForReactiveMask
                ? graph.GetPhysicalTexture( reactiveMaskResource ) : nullptr;
            if ( !Engine::GAPI
                || !particleColor || !particleColor->IsValid()
                || !particleDistortion || !particleDistortion->IsValid()
                || (fsr3ActiveForReactiveMask
                    && (!reactiveMask || !reactiveMask->IsValid())) ) {
                return;
            }
            Engine::GAPI->ResetRenderStates();
            Engine::GAPI->DrawParticlesSimple(
                particleColor, particleDistortion, reactiveMask );
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

    // Draw debug lines
    graph.AddPass( RG_PASS_NAME("Draw Debug Lines"), [&]( RGBuilder& builder, RenderPass& pass ) {
        builder.Write( backBufferHandle );

        pass.m_executeCallback = [this](const RenderGraph&) {
            TracyD3D11ZoneCGX( "D3D11GraphicsEngine::Draw Debug Lines" );
            if ( LineRenderer ) {
                LineRenderer->Flush();
                LineRenderer->FlushScreenSpace();
            }
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
                auto* backbufferTex =
                    graph.GetPhysicalTexture( backBufferHandle );
                if ( PfxRenderer && backbufferTex && backbufferTex->IsValid() ) {
                    PfxRenderer->RenderHDR(
                        backbufferTex->GetRenderTargetView().Get(),
                        backbufferTex->GetShaderResView().Get() );
                }
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
                auto context = GetContext();
                auto* backbufferTex =
                    graph.GetPhysicalTexture( backBufferHandle );
                if ( !context || !PfxRenderer
                    || !backbufferTex || !backbufferTex->IsValid() ) {
                    return;
                }
                PfxRenderer->RenderSMAA(
                    backbufferTex->GetShaderResView().Get() );
                if ( DefaultSamplerState ) {
                    context->PSSetSamplers(
                        0, 1, DefaultSamplerState.GetAddressOf() );
                }
            };
        } );
    }


    graph.AddPass( RG_PASS_NAME("Reset Viewport"), [&]( RGBuilder& builder, RenderPass& pass ) {
        builder.Write( backBufferHandle );

        pass.m_executeCallback = [this](const RenderGraph&) {
            auto context = GetContext();
            if ( !context ) {
                return;
            }
            if ( DefaultSamplerState ) {
                context->PSSetSamplers(
                    0, 1, DefaultSamplerState.GetAddressOf() );
            }

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
            auto context = GetContext();
            if ( !context || !HDRBackBuffer || !HDRBackBuffer->IsValid()
                || !DepthStencilBuffer || !DepthStencilBuffer->IsValid()
                || !DepthStencilBufferCopy
                || !DepthStencilBufferCopy->IsValid() ) {
                return;
            }
            context->OMSetRenderTargets(
                1, HDRBackBuffer->GetRenderTargetView().GetAddressOf(), nullptr );

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
                    auto context = GetContext();
                    auto* backbufferTex =
                        graph.GetPhysicalTexture( backBufferHandle );
                    if ( !context || !PfxRenderer || !LinearSamplerState
                        || !backbufferTex || !backbufferTex->IsValid()
                        || !Backbuffer || !Backbuffer->IsValid() ) {
                        return;
                    }
                    context->PSSetSamplers(
                        0, 1, LinearSamplerState.GetAddressOf() );

                    const INT2 outputResolution = GetBackbufferResolution();
                    switch ( rendererState.RendererSettings.SharpeningMode ) {
                    case GothicRendererSettings::SHARPEN_SIMPLE:
                        {
                            auto event =
                                RecordGraphicsEvent( GE_NAME( "ApplySimpleSharpen" ) );
                            PfxRenderer->RenderSimpleSharpen(
                                backbufferTex->GetShaderResView(),
                                outputResolution, Backbuffer.get(), outputResolution );
                        }
                        break;

                    case GothicRendererSettings::SHARPEN_CAS:
                        {
                            auto event = RecordGraphicsEvent(
                                GE_NAME( "Copy into native-size backbuffer" ) );
                            if ( PfxRenderer->CopyTextureToRTV(
                                    backbufferTex->GetShaderResView(),
                                    Backbuffer->GetRenderTargetView(),
                                    outputResolution ) != XR_SUCCESS ) {
                                return;
                            }
                        }
                        if ( !FeatureLevel10Compatibility ) {
                            auto tempBuffer =
                                PfxRenderer->GetBackbufferTempBuffer();
                            if ( !tempBuffer || !tempBuffer->IsValid() ) {
                                return;
                            }
                            auto event =
                                RecordGraphicsEvent( GE_NAME( "ApplyCAS" ) );
                            PfxRenderer->RenderCAS(
                                Backbuffer->GetShaderResView(), outputResolution,
                                Backbuffer->GetRenderTargetView(), outputResolution,
                                *tempBuffer );
                        }
                        break;

                    default:
                        PfxRenderer->CopyTextureToRTV(
                            backbufferTex->GetShaderResView(),
                            Backbuffer->GetRenderTargetView(), outputResolution );
                        break;
                    }
                };
            } );
        } else {
            graph.AddPass( RG_PASS_NAME("Copy into native-size backbuffer"), [&]( RGBuilder& builder, RenderPass& pass ) {
                builder.Read( backBufferHandle );
                builder.Write( backBufferHandle );

                pass.m_executeCallback = [this, backBufferHandle](const RenderGraph& graph) {
                    auto* backbufferTex =
                        graph.GetPhysicalTexture( backBufferHandle );
                    if ( PfxRenderer && backbufferTex && backbufferTex->IsValid()
                        && Backbuffer && Backbuffer->IsValid() ) {
                        PfxRenderer->CopyTextureToRTV(
                            backbufferTex->GetShaderResView(),
                            Backbuffer->GetRenderTargetView(),
                            GetBackbufferResolution() );
                    }
                };
            } );
        }
        if ( !graph.Compile() || !graph.Execute() ) {
            LogError() << "World RenderGraph execution failed.";
            return XR_FAILED;
        }

        // Thin rain streaks can disappear before temporal upscaling. Under
        // FSR3, rasterize them once at output resolution after upscaling and
        // reject occluded pixels against the copied internal scene depth.
        if ( renderRainAfterUpscaling && Effects ) {
            SetViewport( ViewportInfo( 0, 0, GetBackbufferResolution() ) );
            Context->OMSetRenderTargets(
                1, Backbuffer->GetRenderTargetView().GetAddressOf(), nullptr );
            DepthStencilBufferCopy->BindToPixelShader( Context.Get(), 1 );
            ID3D11ShaderResourceView* rainExclusionSRV = nullptr;
            if ( renderRainExclusionMask ) {
                auto* rainExclusionMask =
                    graph.GetPhysicalTexture( waterMaskResource );
                rainExclusionSRV = rainExclusionMask
                    && rainExclusionMask->IsValid()
                    ? rainExclusionMask->GetShaderResView().Get() : nullptr;
                Context->PSSetShaderResources( 2, 1, &rainExclusionSRV );
            }
            if ( FeatureLevel10Compatibility || rendererState.RendererSettings.DrawRainThroughTransformFeedback ) {
                Effects->DrawRain( true, rainExclusionSRV != nullptr );
            } else {
                Effects->DrawRain_CS( true, rainExclusionSRV != nullptr );
            }
            ID3D11ShaderResourceView* nullRainInputs[2] = {};
            Context->PSSetShaderResources( 1, 2, nullRainInputs );
        }

        Context->ClearDepthStencilView(
            DepthStencilBuffer->GetDepthStencilView().Get(),
            D3D11_CLEAR_DEPTH, 0, 0 );
        Context->ClearDepthStencilView(
            m_SwapchainDepthStencilBuffer->GetDepthStencilView().Get(),
            D3D11_CLEAR_DEPTH, 0, 0 );
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

    // Disable culling for ui rendering(Sprite from LeGo needs it since it use CCW instead of CW order)
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
XRESULT D3D11GraphicsEngine::DrawMeshInfoListAlphablended(
    const std::vector<std::pair<MeshKey, MeshInfo*>>& list ) {
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
        if ( zCTexture* texture = meshKey.Material->GetAniTexture() ) {
            PsSimpleFFdata ffdata = { };
            ffdata.textureFactor = ComputeTransparencyTextureFactor( meshKey.Material );

            if (texture->CacheIn( 0.6f ) != zRES_CACHED_IN) {
                // Draw what? black? :)
                continue;
            }

            MyDirectDrawSurface7* surface = texture->GetSurface();
            ID3D11ShaderResourceView* srv[4];

            // Get diffuse and normalmap
            srv[0] = surface->GetEngineTexture()
                ->GetShaderResourceView().Get();
            srv[1] = Engine::GAPI->GetRendererState().RendererSettings.AllowNormalmaps && surface->GetNormalmap()
                ? surface->GetNormalmap()->GetShaderResourceView().Get()
                : nullptr;
            srv[2] = surface->GetFxMap()
                ? surface->GetFxMap()->GetShaderResourceView().Get()
                : nullptr;
            srv[3] = GetParallaxDisplacementSRV( surface );
            if ( !srv[1] ) {
                srv[1] = GetWetNormalFallbackSRV( surface, DistortionTexture.get() );
                if ( srv[1] && meshKey.Info &&
                    meshKey.Info->buffer.NormalmapStrength != DEFAULT_NORMALMAP_STRENGTH ) {
                    meshKey.Info->buffer.NormalmapStrength = DEFAULT_NORMALMAP_STRENGTH;
                }
            }

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
                //Get the right shader for it
                BindShaderForTexture( texture, false, alphaFunc, meshKey.Info->MaterialType, true );
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

    // Waterfall foam needs scene depth for fogging. General transparent world
    // geometry must keep the opaque depth/G-buffer pair intact for later effects.
    for ( auto const& [meshKey, meshInfo] : list ) {
        if ( meshKey.Material && meshInfo && meshKey.Info &&
            meshKey.Material->GetAniTexture() != nullptr &&
            meshKey.Info->MaterialType == MaterialInfo::MT_WaterfallFoam ) {
            // Draw the section-part
            DrawVertexBufferIndexedUINT( nullptr, nullptr, meshInfo->Indices.size(),
                meshInfo->BaseIndexLocation );
        }
    }

    return XR_SUCCESS;
}


XRESULT D3D11GraphicsEngine::DrawWaterfallMask( ID3D11RenderTargetView* waterMaskRTV ) {
    if ( (FrameTransparencyMeshesWaterfall.empty()
        && FrameTransparencyMeshesWetSSRBlockers.empty()) || !waterMaskRTV ) {
        return XR_SUCCESS;
    }

    auto& rendererState = Engine::GAPI->GetRendererState();
    const GothicBlendStateInfo previousBlendState = rendererState.BlendState;
    const GothicDepthBufferStateInfo previousDepthState = rendererState.DepthState;
    const GothicRasterizerStateInfo previousRasterizerState = rendererState.RasterizerState;
    XRESULT result = XR_SUCCESS;

    auto restoreMaskState = [&]() {
        ID3D11ShaderResourceView* nullSRV = nullptr;
        GetContext()->PSSetShaderResources( 0, 1, &nullSRV );
        rendererState.BlendState = previousBlendState;
        rendererState.DepthState = previousDepthState;
        rendererState.RasterizerState = previousRasterizerState;
        rendererState.BlendState.SetDirty();
        rendererState.DepthState.SetDirty();
        rendererState.RasterizerState.SetDirty();
        if ( UpdateRenderStates() != XR_SUCCESS ) {
            result = XR_FAILED;
        }
        GetContext()->OMSetRenderTargets( 1, HDRBackBuffer->GetRenderTargetView().GetAddressOf(),
            DepthStencilBuffer->GetDepthStencilView().Get() );
    };

    GetContext()->OMSetRenderTargets( 1, &waterMaskRTV,
        DepthStencilBuffer->GetDepthStencilView().Get() );

    XMMATRIX view = Engine::GAPI->GetViewMatrixXM();
    Engine::GAPI->SetViewTransformXM( view );
    Engine::GAPI->ResetWorldTransform();

    SetActiveVertexShader( VShaderID::VS_Ex );
    if ( !ActiveVS || ActiveVS->Apply() != XR_SUCCESS ) {
        restoreMaskState();
        return XR_FAILED;
    }
    SetupVS_ExMeshDrawCall();
    SetupVS_ExConstantBuffer();

    const XMMATRIX identityMatrix = XMMatrixIdentity();
    VS_ExConstantBuffer_PerInstance cbInstance = {};
    XMStoreFloat4x4( &cbInstance.World, identityMatrix );
    cbInstance.Color = float4( 1.0f, 1.0f, 1.0f, 1.0f );
    ActiveVS->GetBuffer( "Matrices_PerInstances" ).Update( &cbInstance, sizeof( cbInstance ) ).Bind();

    rendererState.DepthState.DepthBufferEnabled = true;
    rendererState.DepthState.DepthWriteEnabled = false;
    rendererState.DepthState.DepthBufferCompareFunc =
        GothicDepthBufferStateInfo::CF_COMPARISON_GREATER_EQUAL;
    rendererState.DepthState.SetDirty();

    rendererState.BlendState.SetDefault();
    rendererState.BlendState.SrcBlend = GothicBlendStateInfo::BF_ONE;
    rendererState.BlendState.DestBlend = GothicBlendStateInfo::BF_ONE;
    rendererState.BlendState.BlendOp = GothicBlendStateInfo::BO_BLEND_OP_MAX;
    rendererState.BlendState.SrcBlendAlpha = GothicBlendStateInfo::BF_ONE;
    rendererState.BlendState.DestBlendAlpha = GothicBlendStateInfo::BF_ONE;
    rendererState.BlendState.BlendOpAlpha = GothicBlendStateInfo::BO_BLEND_OP_MAX;
    rendererState.BlendState.BlendEnabled = true;
    rendererState.BlendState.SetDirty();

    // Reverse-Z uses larger depth values near the camera. One bias unit keeps
    // coplanar transparency masks stable without pulling them through foreground geometry.
    rendererState.RasterizerState.ZBias = 1;
    rendererState.RasterizerState.SetDirty();
    if ( UpdateRenderStates() != XR_SUCCESS ) {
        restoreMaskState();
        return XR_FAILED;
    }

    if ( DrawVertexBufferIndexedUINT(
        Engine::GAPI->GetWrappedWorldMesh()->MeshVertexBuffer,
        Engine::GAPI->GetWrappedWorldMesh()->MeshIndexBuffer, 0, 0 ) != XR_SUCCESS ) {
        restoreMaskState();
        return XR_FAILED;
    }

    if ( !FrameTransparencyMeshesWaterfall.empty() ) {
        SetActivePixelShader( PShaderID::PS_WaterMask );
        if ( !ActivePS || ActivePS->Apply() != XR_SUCCESS ) {
            result = XR_FAILED;
        } else {
            for ( auto const& [meshKey, meshInfo] : FrameTransparencyMeshesWaterfall ) {
                if ( meshKey.Material && meshInfo && !meshInfo->Indices.empty()
                    && meshKey.Material->GetAniTexture() ) {
                    if ( DrawVertexBufferIndexedUINT( nullptr, nullptr, meshInfo->Indices.size(),
                        meshInfo->BaseIndexLocation ) != XR_SUCCESS ) {
                        result = XR_FAILED;
                        break;
                    }
                }
            }
        }
    }

    if ( result == XR_SUCCESS && !FrameTransparencyMeshesWetSSRBlockers.empty() ) {
        SetActivePixelShader( PShaderID::PS_TransparencyWetMask );
        if ( !ActivePS || ActivePS->Apply() != XR_SUCCESS ) {
            result = XR_FAILED;
        } else {
            GetContext()->PSSetSamplers( 0, 1, DefaultSamplerState.GetAddressOf() );
            zCTexture* lastTexture = nullptr;
            for ( auto const& [meshKey, meshInfo] : FrameTransparencyMeshesWetSSRBlockers ) {
                if ( !meshKey.Material || !meshInfo || meshInfo->Indices.empty() ) {
                    continue;
                }

                zCTexture* texture = meshKey.Material->GetAniTexture();
                if ( !texture || texture->CacheIn( 0.6f ) != zRES_CACHED_IN ) {
                    continue;
                }
                MyDirectDrawSurface7* surface = texture->GetSurface();
                if ( !surface || !surface->GetEngineTexture() ) {
                    continue;
                }

                if ( texture != lastTexture ) {
                    ID3D11ShaderResourceView* textureSRV =
                        surface->GetEngineTexture()->GetShaderResourceView().Get();
                    if ( !textureSRV ) {
                        continue;
                    }
                    GetContext()->PSSetShaderResources( 0, 1, &textureSRV );
                    lastTexture = texture;
                }

                PsSimpleFFdata ffdata = {};
                ffdata.textureFactor = ComputeTransparencyTextureFactor( meshKey.Material );
                ActivePS->GetBuffer( "cbFFData" ).Update( &ffdata ).Bind();
                if ( DrawVertexBufferIndexedUINT( nullptr, nullptr, meshInfo->Indices.size(),
                    meshInfo->BaseIndexLocation ) != XR_SUCCESS ) {
                    result = XR_FAILED;
                    break;
                }
            }
        }
    }

    restoreMaskState();
    return result;
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

    // Setup default renderstates
    SetDefaultStates();

    XMMATRIX view = Engine::GAPI->GetViewMatrixXM();
    Engine::GAPI->SetViewTransformXM( view );
    Engine::GAPI->ResetWorldTransform();

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
    std::vector<TransparencyWorldMeshEntry> waterfallTransparencyMeshes;

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
                    zCTexture* aniTex = worldMesh.first.Material->GetTexture();
                    if ( !aniTex ) continue;

                    // Check surface type
                    if ( worldMesh.first.Info->MaterialType == MaterialInfo::MT_Water ) {
                        if ( !isZPrepass ) {
                            FrameWaterSurfaces[aniTex].push_back( worldMesh.second );
                        }
                        continue;
                    }

                    if ( aniTex->CacheIn( 0.6f ) != zRES_CACHED_IN ) {
                        continue;
                    }

                    const float distanceSq = ComputeWorldMeshDistanceSqFromCamera( renderItem, worldMesh.second, cameraPosition );
                    const std::pair<MeshKey, MeshInfo*> transparencyMesh = { worldMesh.first, worldMesh.second };

                    if ( worldMesh.first.Info->MaterialType == MaterialInfo::MT_Portal ) {
                        if ( !isZPrepass ) {
                            portalTransparencyMeshes.push_back( { transparencyMesh, distanceSq } );
                        }
                        continue;
                    } else if ( worldMesh.first.Info->MaterialType == MaterialInfo::MT_WaterfallFoam ) {
                        if ( !isZPrepass ) {
                            waterfallTransparencyMeshes.push_back( { transparencyMesh, distanceSq } );
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
                            FrameTransparencyMeshesWetSSRBlockers.push_back( transparencyMesh );
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
        sortAndAppendTransparencyMeshes( waterfallTransparencyMeshes, FrameTransparencyMeshesWaterfall );
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
    if ( (Engine::GAPI->GetRendererState().RendererSettings.DoZPrepass && Engine::GAPI->GetRendererState().RendererSettings.RendererMode == GothicRendererSettings::RM_Deferred )
        || isZPrepass) {
        ZoneScopedN( "DrawWorldMesh::DepthPrepass" );
        auto _scopeDepthPrepass = RecordGraphicsEvent( GE_NAME( "DrawWorldMesh::DepthPrepass" ) );
        GetContext()->PSSetShader( nullptr, nullptr, 0 );

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

        ConstantBufferAllocation INVALID_MATERIAL = PerObjectMaterialInfoPooledBuffer->Allocate(
            GetContext().Get(), &defInfo.buffer, sizeof( defInfo.buffer ) );
        if ( !INVALID_MATERIAL ) {
            LogError() << "DrawWorldMesh: Material constant-buffer pool is unavailable.";
            return XR_FAILED;
        }
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
                srv[1] = Engine::GAPI->GetRendererState().RendererSettings.AllowNormalmaps && surface->GetNormalmap()
                    ? surface->GetNormalmap()->GetShaderResourceView().Get()
                    : nullptr;
                srv[2] = surface->GetFxMap()
                    ? surface->GetFxMap()->GetShaderResourceView().Get()
                    : nullptr;
                srv[3] = GetParallaxDisplacementSRV( surface );
                if ( !srv[1] ) {
                    srv[1] = GetWetNormalFallbackSRV( surface, DistortionTexture.get() );
                    if ( srv[1] && info &&
                        info->buffer.NormalmapStrength != DEFAULT_NORMALMAP_STRENGTH ) {
                        info->buffer.NormalmapStrength = DEFAULT_NORMALMAP_STRENGTH;
                    }
                }

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
                        const auto candidate = PerObjectMaterialInfoPooledBuffer->Allocate(
                            GetContext().Get(), &materialBuffer, sizeof( materialBuffer ) );
                        materialInfoBufferAllocation = candidate ? candidate : INVALID_MATERIAL;
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

    return XR_SUCCESS;
}

/** Draws the given mesh infos as water */
void D3D11GraphicsEngine::DrawWaterSurfaces() {
    DrawWaterSurfaces( nullptr, nullptr, nullptr );
}

void D3D11GraphicsEngine::DrawWaterSurfaces(
    ID3D11RenderTargetView* waterMaskRTV,
    ID3D11RenderTargetView* fsr3ReactiveMaskRTV,
    ID3D11ShaderResourceView* lowCloudLayerSRV ) {
    if ( FrameWaterSurfaces.empty() ) return;

    auto* gapi = Engine::GAPI;
    const auto context = GetContext();
    auto* wrappedWorldMesh = gapi ? gapi->GetWrappedWorldMesh() : nullptr;
    if ( !gapi || !context || !PfxRenderer || !HDRBackBuffer || !HDRBackBuffer->IsValid()
        || !DepthStencilBuffer || !DepthStencilBuffer->IsValid()
        || !DepthStencilBufferCopy || !DepthStencilBufferCopy->IsValid()
        || !DistortionTexture || !DistortionTexture->IsValid()
        || !wrappedWorldMesh || !wrappedWorldMesh->MeshVertexBuffer
        || !wrappedWorldMesh->MeshVertexBuffer->IsValid()
        || !wrappedWorldMesh->MeshIndexBuffer
        || !wrappedWorldMesh->MeshIndexBuffer->IsValid() ) {
        LogError() << "Water rendering skipped because required GPU resources are unavailable.";
        return;
    }

    ZoneScopedN( "DrawWaterSurfaces" );
    auto _scopeDrawWaterSurfaces = RecordGraphicsEvent( GE_NAME( "DrawWaterSurfaces" ) );

    SetDefaultStates();

    auto tempBuffer = PfxRenderer->GetTempBuffer();
    if ( !tempBuffer || !tempBuffer->IsValid()
        || PfxRenderer->CopyTextureToRTV(
            HDRBackBuffer->GetShaderResView(),
            tempBuffer->GetRenderTargetView(),
            GetResolution() ) != XR_SUCCESS
        || CopyDepthStencil() != XR_SUCCESS ) {
        LogError() << "Water rendering skipped because its copy targets are unavailable.";
        return;
    }

    const XMMATRIX view = gapi->GetViewMatrixXM();
    gapi->SetViewTransformXM( view );

    // Bind vertex water shader
    ActivePS = nullptr;
    SetActiveVertexShader( VShaderID::VS_ExWater );
    if ( !ActiveVS ) {
        LogError() << "Water rendering skipped because VS_ExWater is unavailable.";
        return;
    }
    SetupVS_ExMeshDrawCall();
    SetupVS_ExConstantBuffer();

    float totalTime = gapi->GetTotalTime();
    ActiveVS->GetBuffer( "Matrices_PerInstances" ).Update(
        &totalTime, sizeof( totalTime ) ).Bind();

    auto restoreWaterTargets = [&]() {
        context->PSSetShaderResources( 0, 7, s_nullSRVs );
        context->OMSetRenderTargets( 1, HDRBackBuffer->GetRenderTargetView().GetAddressOf(),
            DepthStencilBuffer->GetDepthStencilView().Get() );
    };

    ID3D11RenderTargetView* waterTargets[3] = {
        HDRBackBuffer->GetRenderTargetView().Get(), waterMaskRTV, fsr3ReactiveMaskRTV
    };
    const UINT waterTargetCount = fsr3ReactiveMaskRTV ? 3u : ( waterMaskRTV ? 2u : 1u );
    GetContext()->OMSetRenderTargets( waterTargetCount, waterTargets,
        DepthStencilBuffer->GetDepthStencilView().Get() );

    // Bind wrapped mesh vertex buffers
    if ( DrawVertexBufferIndexedUINT(
        wrappedWorldMesh->MeshVertexBuffer,
        wrappedWorldMesh->MeshIndexBuffer, 0, 0 ) != XR_SUCCESS ) {
        restoreWaterTargets();
        return;
    }

    // Build per-texture batch descriptors and flat indirect draw args
    struct WaterTextureBatch {
        zCTexture* texture;
        size_t argsOffset; // index into waterDrawArgs
        size_t drawCount;
    };

    static std::vector<D3D11_DRAW_INDEXED_INSTANCED_INDIRECT_ARGS> waterDrawArgs;
    static std::vector<WaterTextureBatch> waterBatches;
    waterDrawArgs.clear();
    waterBatches.clear();

    {
        ZoneScopedN( "DrawWaterSurfaces::BuildBatches" );
        auto _scopeBuildBatches = RecordGraphicsEvent( GE_NAME( "DrawWaterSurfaces::BuildBatches" ) );
        for ( const auto& [texture, meshes] : FrameWaterSurfaces ) {
            if ( !texture ) continue;

            WaterTextureBatch batch{};
            batch.texture = texture;
            batch.argsOffset = waterDrawArgs.size();
            batch.drawCount = 0;

            for ( const auto& mesh : meshes ) {
                if ( !mesh || mesh->Indices.empty()
                    || mesh->Indices.size() > (std::numeric_limits<UINT>::max)() ) {
                    continue;
                }

                D3D11_DRAW_INDEXED_INSTANCED_INDIRECT_ARGS args{};
                args.IndexCountPerInstance = static_cast<UINT>(mesh->Indices.size());
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
    bool useWaterIndirect = !FeatureLevel10Compatibility && !waterDrawArgs.empty();
    if ( useWaterIndirect ) {
        if ( waterDrawArgs.size() > (std::numeric_limits<UINT>::max)() / argStride ) {
            useWaterIndirect = false;
        } else {
            const UINT requiredSize = static_cast<UINT>(waterDrawArgs.size() * argStride);
            if ( !WaterIndirectBuffer || WaterIndirectBuffer->GetSizeInBytes() < requiredSize ) {
                auto newBuffer = std::make_unique<D3D11IndirectBuffer>();
                if ( newBuffer->Init( waterDrawArgs.data(), requiredSize,
                    D3D11IndirectBuffer::B_INDEXBUFFER, D3D11IndirectBuffer::U_DYNAMIC,
                    D3D11IndirectBuffer::CA_WRITE, "WaterIndirectArgs" ) == XR_SUCCESS ) {
                    WaterIndirectBuffer = std::move( newBuffer );
                } else {
                    useWaterIndirect = false;
                }
            } else if ( WaterIndirectBuffer->UpdateBuffer(
                waterDrawArgs.data(), requiredSize ) != XR_SUCCESS ) {
                useWaterIndirect = false;
            }
        }
    }
    if ( useWaterIndirect && !WaterIndirectBuffer->IsValid() ) {
        useWaterIndirect = false;
    }

    // === Z-Prepass ===
    {
        ZoneScopedN( "DrawWaterSurfaces::ZPrepass" );
        auto _scopeWaterZPrepass = RecordGraphicsEvent( GE_NAME( "DrawWaterSurfaces::ZPrepass" ) );
        // Disable color writes for depth-only rendering
        Engine::GAPI->GetRendererState().BlendState.ColorWritesEnabled = false;
        Engine::GAPI->GetRendererState().BlendState.SetDirty();
        if ( UpdateRenderStates() != XR_SUCCESS ) {
            restoreWaterTargets();
            return;
        }

        GetContext()->PSSetShader( nullptr, nullptr, 0 );

        if ( useWaterIndirect ) {
            DrawMultiIndexedInstancedIndirect( Context.Get(),
                static_cast<unsigned int>(waterDrawArgs.size()),
                WaterIndirectBuffer->GetIndirectBuffer().Get(),
                0, argStride );
        } else {
            // FL10 fallback: direct DrawIndexed per mesh
            for ( const auto& args : waterDrawArgs ) {
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
        if ( UpdateRenderStates() != XR_SUCCESS ) {
            restoreWaterTargets();
            return;
        }

        // Bind pixel water shader
        SetActivePixelShader( PShaderID::PS_Water );
        if ( !ActivePS || ActivePS->Apply() != XR_SUCCESS
            || DistortionTexture->BindToPixelShader( 4 ) != XR_SUCCESS ) {
            LogError() << "Water refraction skipped because its shader resources are unavailable.";
            restoreWaterTargets();
            return;
        }

        // Bind copied backbuffer
        GetContext()->PSSetShaderResources(
            5, 1, tempBuffer->GetShaderResView().GetAddressOf() );

        // Bind depth to the shader
        DepthStencilBufferCopy->BindToPixelShader( GetContext().Get(), 2 );

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

        if ( useWaterIndirect ) {
            // MDI path: one MDI call per texture batch
            for ( const auto& batch : waterBatches ) {
                batch.texture->CacheIn( -1 );
                batch.texture->Bind( 0 );

                WaterMaterialInfoConstantBuffer wmcb = {};
                FillWaterMaterialInfo( wmcb, batch.texture );
                ActivePS->GetBuffer( "WaterMaterialInfo" ).Update( &wmcb ).Bind();

                DrawMultiIndexedInstancedIndirect( Context.Get(),
                    static_cast<unsigned int>(batch.drawCount),
                    WaterIndirectBuffer->GetIndirectBuffer().Get(),
                    static_cast<unsigned int>(batch.argsOffset * argStride), argStride );
            }
        } else {
            // FL10 fallback: per-texture loop with direct DrawIndexed
            for ( const auto& batch : waterBatches ) {
                batch.texture->CacheIn( -1 );
                batch.texture->Bind( 0 );

                WaterMaterialInfoConstantBuffer wmcb = {};
                FillWaterMaterialInfo( wmcb, batch.texture );
                ActivePS->GetBuffer( "WaterMaterialInfo" ).Update( &wmcb ).Bind();

                for ( size_t i = 0; i < batch.drawCount; ++i ) {
                    const auto& args = waterDrawArgs[batch.argsOffset + i];
                    DrawVertexBufferIndexedUINT( nullptr, nullptr,
                        args.IndexCountPerInstance, args.StartIndexLocation );
                }
            }
        }
    }

    restoreWaterTargets();
}

/** Draws everything around the given position */
void XM_CALLCONV D3D11GraphicsEngine::DrawWorldAround(
    FXMVECTOR position, float range, bool cullFront, bool indoor,
    bool noNPCs, std::list<VobInfo*>* renderedVobs,
    std::list<SkeletalVobInfo*>* renderedMobs,
    std::vector<std::pair<MeshKey, MeshInfo*>>* worldMeshCache,
    unsigned int casterMask ) {
    auto* const gapi = Engine::GAPI;
    if ( !gapi || !Context || !ShaderManager || !ActiveVS
        || !std::isfinite( range ) || range <= 0.0f
        || !WhiteTexture || !WhiteTexture->IsValid()
        || !InfiniteRangeConstantBuffer
        || !InfiniteRangeConstantBuffer->IsValid() ) {
        return;
    }

    XMFLOAT3 lightPosition{};
    XMStoreFloat3( &lightPosition, position );
    if ( !std::isfinite( lightPosition.x )
        || !std::isfinite( lightPosition.y )
        || !std::isfinite( lightPosition.z ) ) {
        return;
    }

    const bool drawWorldCasters = (casterMask & SHADOW_CASTER_WORLD) != 0;
    const bool drawVobCasters = (casterMask & SHADOW_CASTER_VOBS) != 0;
    const bool drawMobCasters = (casterMask & SHADOW_CASTER_MOBS) != 0;
    const bool drawAnimatedCasters =
        (casterMask & SHADOW_CASTER_ANIMATED) != 0;
    if ( !drawWorldCasters && !drawVobCasters
        && !drawMobCasters && !drawAnimatedCasters ) {
        return;
    }

    auto& rendererState = gapi->GetRendererState();
    rendererState.RasterizerState.SetDefault();
    rendererState.RasterizerState.CullMode =
        cullFront ? GothicRasterizerStateInfo::CM_CULL_FRONT
                  : GothicRasterizerStateInfo::CM_CULL_NONE;
    rendererState.RasterizerState.DepthClipEnable = true;
    rendererState.RasterizerState.SetDirty();
    rendererState.DepthState.SetDefault();
    rendererState.DepthState.DepthBufferCompareFunc =
        GothicDepthBufferStateInfo::ECompareFunc::CF_COMPARISON_LESS_EQUAL;
    rendererState.DepthState.SetDirty();

    Context->PSSetShaderResources( 0, 6, s_nullSRVs );
    const bool linearDepth =
        (rendererState.GraphicsState.FF_GSwitches
            & GSWITCH_LINEAR_DEPTH) != 0;
    if ( SetActivePixelShader(
            linearDepth ? PShaderID::PS_LinDepth
                        : PShaderID::PS_DiffuseAlphaTestShadows ) != XR_SUCCESS
        || !ActivePS ) {
        return;
    }
    D3D11PShader* const shadowPixelShader = ActivePS.get();

    GSky* const sky = gapi->GetSky();
    if ( !sky ) {
        return;
    }
    auto fixedFunctionBuffer = shadowPixelShader
        ->GetBuffer( "FFPipelineConstantBuffer" )
        .Update( &rendererState.GraphicsState )
        .Bind();
    auto atmosphereBuffer = shadowPixelShader
        ->GetBuffer( "Atmosphere" )
        .Update( &sky->GetAtmosphereCB() )
        .Bind();

    SetupVS_ExMeshDrawCall();
    SetupVS_ExConstantBuffer();
    const XMMATRIX identityMatrix = XMMatrixIdentity();
    VS_ExConstantBuffer_PerInstance instanceState{};
    XMStoreFloat4x4( &instanceState.World, identityMatrix );
    instanceState.Color = float4( 1.0f, 1.0f, 1.0f, 1.0f );
    auto instanceBuffer = ActiveVS
        ->GetBuffer( "Matrices_PerInstances" )
        .Update( &instanceState, sizeof( instanceState ) )
        .Bind();

    PerObjectState materialState{};
    materialState.OS_AmbientColor = float3( 1.0f, 1.0f, 1.0f );
    auto materialBuffer = shadowPixelShader
        ->GetBuffer( "POS_MaterialInfo" )
        .Update( &materialState )
        .Bind();
    if ( !fixedFunctionBuffer.Succeeded()
        || !atmosphereBuffer.Succeeded()
        || !instanceBuffer.Succeeded()
        || !materialBuffer.Succeeded() ) {
        return;
    }

    WhiteTexture->BindToPixelShader( 0 );
    D3D11Texture* lastBoundTexture = WhiteTexture.get();
    shadowPixelShader->BindBuffer(
        "DIST_Distance", InfiniteRangeConstantBuffer.get() );
    if ( UpdateRenderStates() != XR_SUCCESS
        || ActiveVS->Apply() != XR_SUCCESS
        || shadowPixelShader->Apply() != XR_SUCCESS ) {
        return;
    }

    auto* const loadedWorld = gapi->GetLoadedWorldInfo();
    const bool isOutdoor = loadedWorld && loadedWorld->BspTree
        && loadedWorld->BspTree->GetBspTreeMode() == zBSP_MODE_OUTDOOR;
    const float safeRange = (std::min)(
        range, std::sqrt((std::numeric_limits<float>::max)()) );
    const XMVECTOR vRangeSquared =
        XMVectorReplicate( safeRange * safeRange );
    const float alphaRef = rendererState.GraphicsState.FF_AlphaRef;

    const auto prepareShadowMaterial =
        [&]( zCMaterial* material, bool includeBlendAlpha,
            bool requireAlphaRef ) {
        zCTexture* const texture =
            material ? material->GetTexture() : nullptr;
        const int alphaFunction =
            material ? material->GetAlphaFunc() : zMAT_ALPHA_FUNC_NONE;
        const bool blendedMaterial = includeBlendAlpha && material
            && alphaFunction != zMAT_ALPHA_FUNC_NONE
            && alphaFunction != zMAT_ALPHA_FUNC_MAT_DEFAULT;
        const bool needsAlphaTexture = texture && material
            && (material->HasAlphaTest()
                || texture->HasAlphaChannel()
                || blendedMaterial);

        if ( needsAlphaTexture ) {
            if ( (requireAlphaRef && alphaRef <= 0.0f)
                || texture->CacheIn( 0.6f ) != zRES_CACHED_IN ) {
                return -1;
            }
            auto* surface = texture->GetSurface();
            auto* engineTexture =
                surface ? surface->GetEngineTexture() : nullptr;
            if ( !engineTexture || !engineTexture->IsValid() ) {
                return -1;
            }
            if ( lastBoundTexture != engineTexture ) {
                engineTexture->BindToPixelShader( 0 );
                lastBoundTexture = engineTexture;
            }
            if ( shadowPixelShader->Apply() != XR_SUCCESS ) {
                return -1;
            }
            return 1;
        }

        if ( linearDepth ) {
            if ( lastBoundTexture != WhiteTexture.get() ) {
                WhiteTexture->BindToPixelShader( 0 );
                lastBoundTexture = WhiteTexture.get();
            }
            if ( shadowPixelShader->Apply() != XR_SUCCESS ) {
                return -1;
            }
        } else {
            Context->PSSetShader( nullptr, nullptr, 0 );
        }
        return 0;
    };

    std::vector<WorldMeshSectionInfo*> drawnSections;
    const bool hasWorldCache =
        worldMeshCache && !worldMeshCache->empty();
    const bool collectForWorld = drawWorldCasters
        && rendererState.RendererSettings.DrawWorldMesh
        && !hasWorldCache;
    const bool collectForVobs = drawVobCasters
        && rendererState.RendererSettings.DrawVOBs
        && (!renderedVobs || renderedVobs->empty());
    if ( collectForWorld || collectForVobs ) {
        Frustum pointLightBounds;
        pointLightBounds.BuildCubemapFace( position, safeRange, 0 );
        gapi->CollectVisibleSections(
            drawnSections, &pointLightBounds, true );
    }

    if ( drawWorldCasters
        && rendererState.RendererSettings.DrawWorldMesh ) {
        auto event = RecordGraphicsEvent( GE_NAME( "DrawWorldMesh" ) );
        if ( hasWorldCache ) {
            for ( const auto& meshByKey : *worldMeshCache ) {
                const MeshKey& key = meshByKey.first;
                MeshInfo* const mesh = meshByKey.second;
                if ( !key.Info
                    || key.Info->MaterialType != MaterialInfo::MT_None ) {
                    continue;
                }

                const int materialMode =
                    prepareShadowMaterial( key.Material, false, true );
                const bool isAlpha = materialMode > 0;
                if ( materialMode < 0
                    || !IsDrawableShadowMesh( mesh, isAlpha ) ) {
                    continue;
                }
                DrawVertexBufferIndexed(
                    mesh->MeshVertexBuffer,
                    GetShadowAwareIndexBuffer( mesh, isAlpha ),
                    GetShadowAwareIndexCount( mesh, isAlpha ) );
            }
        } else {
            for ( WorldMeshSectionInfo* section : drawnSections ) {
                if ( !section ) {
                    continue;
                }

                if ( rendererState.RendererSettings.FastShadows ) {
                    MeshInfo* const mesh = section->FullStaticMesh;
                    if ( IsDrawableMeshInfo( mesh )
                        && prepareShadowMaterial(
                            nullptr, false, true ) >= 0 ) {
                        gapi->DrawMeshInfo( nullptr, mesh );
                    }
                    continue;
                }

                for ( const auto& meshByKey : section->WorldMeshes ) {
                    const MeshKey& key = meshByKey.first;
                    MeshInfo* const mesh = meshByKey.second;
                    if ( !key.Info
                        || key.Info->MaterialType != MaterialInfo::MT_None ) {
                        continue;
                    }

                    const int materialMode =
                        prepareShadowMaterial( key.Material, false, true );
                    const bool isAlpha = materialMode > 0;
                    if ( materialMode < 0
                        || !IsDrawableShadowMesh( mesh, isAlpha ) ) {
                        continue;
                    }
                    DrawVertexBufferIndexed(
                        mesh->MeshVertexBuffer,
                        GetShadowAwareIndexBuffer( mesh, isAlpha ),
                        GetShadowAwareIndexCount( mesh, isAlpha ) );
                }
            }
        }
    }

    if ( drawVobCasters && rendererState.RendererSettings.DrawVOBs ) {
        std::list<VobInfo*> collectedVobs;
        if ( !renderedVobs || renderedVobs->empty() ) {
            for ( WorldMeshSectionInfo* section : drawnSections ) {
                if ( !section ) {
                    continue;
                }
                for ( VobInfo* vobInfo : section->Vobs ) {
                    if ( !vobInfo || !vobInfo->Vob
                        || !vobInfo->VisualInfo
                        || !vobInfo->Vob->GetShowVisual()
                        || std::find(
                            collectedVobs.begin(), collectedVobs.end(),
                            vobInfo ) != collectedVobs.end() ) {
                        continue;
                    }
                    if ( XMVector3Greater(
                        XMVector3LengthSq(
                            position - XMLoadFloat3(
                                &vobInfo->LastRenderPosition ) ),
                        vRangeSquared ) ) {
                        continue;
                    }
                    if ( isOutdoor && vobInfo->IsIndoorVob != indoor ) {
                        continue;
                    }
                    collectedVobs.emplace_back( vobInfo );
                }
            }
            if ( renderedVobs ) {
                *renderedVobs = collectedVobs;
            }
        }

        std::list<VobInfo*>& vobsToDraw =
            renderedVobs ? *renderedVobs : collectedVobs;
        auto event = RecordGraphicsEvent( GE_NAME( "Draw vobs" ) );
        if ( ActiveVS ) {
            SetupVS_ExMeshDrawCall();
            SetupVS_ExConstantBuffer();
            auto perVobBuffer =
                ActiveVS->GetBuffer( "Matrices_PerInstances" ).Bind();
            if ( perVobBuffer.Succeeded()
                && ActiveVS->Apply() == XR_SUCCESS ) {
                VS_ExConstantBuffer_PerInstance perVobState{};
                for ( VobInfo* vobInfo : vobsToDraw ) {
                    if ( !vobInfo || !vobInfo->Vob
                        || !vobInfo->VisualInfo
                        || !vobInfo->Vob->GetShowVisual() ) {
                        continue;
                    }
                    vobInfo->UpdateVobConstantBuffer( perVobState );
                    if ( !perVobBuffer.Update(
                            &perVobState,
                            sizeof( perVobState ) ).Succeeded() ) {
                        break;
                    }

                    for ( const auto& materialMeshes :
                        vobInfo->VisualInfo->Meshes ) {
                        const int materialMode = prepareShadowMaterial(
                            materialMeshes.first, true, false );
                        const bool isAlpha = materialMode > 0;
                        if ( materialMode < 0 ) {
                            continue;
                        }
                        for ( MeshInfo* mesh : materialMeshes.second ) {
                            if ( !IsDrawableShadowMesh(
                                    mesh, isAlpha ) ) {
                                continue;
                            }
                            DrawVertexBufferIndexed(
                                mesh->MeshVertexBuffer,
                                GetShadowAwareIndexBuffer(
                                    mesh, isAlpha ),
                                GetShadowAwareIndexCount(
                                    mesh, isAlpha ) );
                        }
                    }
                }
            }
        }
    }

    const bool renderNPCs = !noNPCs && drawAnimatedCasters;
    if ( drawMobCasters && rendererState.RendererSettings.DrawMobs ) {
        std::list<SkeletalVobInfo*> collectedMobs;
        if ( !renderedMobs || renderedMobs->empty() ) {
            for ( SkeletalVobInfo* mob : gapi->GetSkeletalMeshVobs() ) {
                auto* visual = mob
                    ? dynamic_cast<SkeletalMeshVisualInfo*>(mob->VisualInfo)
                    : nullptr;
                if ( !mob || !mob->Vob || !visual
                    || !mob->Vob->GetShowVisual() ) {
                    continue;
                }
                if ( XMVector3Greater(
                    XMVector3LengthSq(
                        position - mob->Vob->GetPositionWorldXM() ),
                    vRangeSquared ) ) {
                    continue;
                }
                if ( isOutdoor
                    && mob->Vob->IsIndoorVob() != indoor ) {
                    continue;
                }
                if ( !visual->SkeletalMeshes.empty() ) {
                    continue;
                }
                collectedMobs.emplace_back( mob );
            }
            if ( renderedMobs ) {
                *renderedMobs = collectedMobs;
            }
        }

        std::list<SkeletalVobInfo*>& mobsToDraw =
            renderedMobs ? *renderedMobs : collectedMobs;
        auto event = RecordGraphicsEvent(
            GE_NAME( "Draw static skeletal meshes" ) );
        for ( SkeletalVobInfo* mob : mobsToDraw ) {
            if ( mob && mob->Vob && mob->VisualInfo
                && mob->Vob->GetShowVisual() ) {
                gapi->DrawSkeletalMeshVob( mob, FLT_MAX );
            }
        }
    }

    if ( drawAnimatedCasters && renderNPCs
        && rendererState.RendererSettings.DrawSkeletalMeshes ) {
        auto event = RecordGraphicsEvent(
            GE_NAME( "Draw animated skeletal meshes" ) );
        for ( SkeletalVobInfo* skeletalVob :
            gapi->GetAnimatedSkeletalMeshVobs() ) {
            if ( !skeletalVob || !skeletalVob->Vob
                || !skeletalVob->VisualInfo
                || !skeletalVob->Vob->GetShowVisual() ) {
                continue;
            }
            if ( skeletalVob->Vob->GetVisualAlpha()
                && skeletalVob->Vob->GetVobTransparency() < 0.7f ) {
                continue;
            }
            if ( XMVector3Greater(
                XMVector3LengthSq(
                    position
                    - skeletalVob->Vob->GetPositionWorldXM() ),
                vRangeSquared ) ) {
                continue;
            }
            if ( isOutdoor
                && skeletalVob->Vob->IsIndoorVob() != indoor ) {
                continue;
            }
            gapi->DrawSkeletalMeshVob( skeletalVob, FLT_MAX );
        }
    }
}

void XM_CALLCONV D3D11GraphicsEngine::DrawWorldAround_Layered(
    FXMVECTOR position, float range, bool cullFront, bool indoor,
    bool noNPCs, std::list<VobInfo*>* renderedVobs,
    std::list<SkeletalVobInfo*>* renderedMobs,
    std::vector<std::pair<MeshKey, MeshInfo*>>* worldMeshCache,
    unsigned int casterMask ) {
    auto* const gapi = Engine::GAPI;
    if ( !gapi || !Context || !ShaderManager
        || !std::isfinite( range ) || range <= 0.0f
        || !WhiteTexture || !WhiteTexture->IsValid()
        || !DistortionTexture || !DistortionTexture->IsValid()
        || !InfiniteRangeConstantBuffer
        || !InfiniteRangeConstantBuffer->IsValid() ) {
        return;
    }

    XMFLOAT3 lightPosition{};
    XMStoreFloat3( &lightPosition, position );
    if ( !std::isfinite( lightPosition.x )
        || !std::isfinite( lightPosition.y )
        || !std::isfinite( lightPosition.z ) ) {
        return;
    }

    const bool drawWorldCasters = (casterMask & SHADOW_CASTER_WORLD) != 0;
    const bool drawVobCasters = (casterMask & SHADOW_CASTER_VOBS) != 0;
    const bool drawMobCasters = (casterMask & SHADOW_CASTER_MOBS) != 0;
    const bool drawAnimatedCasters =
        (casterMask & SHADOW_CASTER_ANIMATED) != 0;
    if ( !drawWorldCasters && !drawVobCasters
        && !drawMobCasters && !drawAnimatedCasters ) {
        return;
    }

    auto& rendererState = gapi->GetRendererState();
    rendererState.RasterizerState.SetDefault();
    rendererState.RasterizerState.CullMode =
        cullFront ? GothicRasterizerStateInfo::CM_CULL_FRONT
                  : GothicRasterizerStateInfo::CM_CULL_NONE;
    rendererState.RasterizerState.DepthClipEnable = true;
    rendererState.RasterizerState.SetDirty();
    rendererState.DepthState.SetDefault();
    rendererState.DepthState.DepthBufferCompareFunc =
        GothicDepthBufferStateInfo::ECompareFunc::CF_COMPARISON_LESS_EQUAL;
    rendererState.DepthState.SetDirty();

    Context->PSSetShaderResources( 0, 6, s_nullSRVs );
    const bool linearDepth =
        (rendererState.GraphicsState.FF_GSwitches
            & GSWITCH_LINEAR_DEPTH) != 0;
    if ( SetActiveVertexShader( VShaderID::VS_ExLayered ) != XR_SUCCESS
        || SetActivePixelShader(
            linearDepth ? PShaderID::PS_LinDepth
                        : PShaderID::PS_DiffuseAlphaTestShadows ) != XR_SUCCESS
        || !ActiveVS || !ActivePS ) {
        return;
    }
    D3D11PShader* const shadowPixelShader = ActivePS.get();

    GSky* const sky = gapi->GetSky();
    if ( !sky ) {
        return;
    }
    auto fixedFunctionBuffer = shadowPixelShader
        ->GetBuffer( "FFPipelineConstantBuffer" )
        .Update( &rendererState.GraphicsState )
        .Bind();
    auto atmosphereBuffer = shadowPixelShader
        ->GetBuffer( "Atmosphere" )
        .Update( &sky->GetAtmosphereCB() )
        .Bind();

    SetupVS_ExMeshDrawCall();
    SetupVS_ExConstantBuffer();
    const XMMATRIX identityMatrix = XMMatrixIdentity();
    VS_ExConstantBuffer_PerInstance instanceState{};
    XMStoreFloat4x4( &instanceState.World, identityMatrix );
    instanceState.Color = float4( 1.0f, 1.0f, 1.0f, 1.0f );
    auto instanceBuffer = ActiveVS
        ->GetBuffer( "Matrices_PerInstances" )
        .Update( &instanceState, sizeof( instanceState ) )
        .Bind();

    PerObjectState materialState{};
    materialState.OS_AmbientColor = float3( 1.0f, 1.0f, 1.0f );
    auto materialBuffer = shadowPixelShader
        ->GetBuffer( "POS_MaterialInfo" )
        .Update( &materialState )
        .Bind();
    if ( !fixedFunctionBuffer.Succeeded()
        || !atmosphereBuffer.Succeeded()
        || !instanceBuffer.Succeeded()
        || !materialBuffer.Succeeded() ) {
        return;
    }

    DistortionTexture->BindToPixelShader( 0 );
    D3D11Texture* lastBoundTexture = DistortionTexture.get();
    shadowPixelShader->BindBuffer(
        "DIST_Distance", InfiniteRangeConstantBuffer.get() );
    if ( UpdateRenderStates() != XR_SUCCESS
        || ActiveVS->Apply() != XR_SUCCESS
        || shadowPixelShader->Apply() != XR_SUCCESS ) {
        return;
    }

    auto* const loadedWorld = gapi->GetLoadedWorldInfo();
    const bool isOutdoor = loadedWorld && loadedWorld->BspTree
        && loadedWorld->BspTree->GetBspTreeMode() == zBSP_MODE_OUTDOOR;
    const float safeRange = (std::min)(
        range, std::sqrt((std::numeric_limits<float>::max)()) );
    const XMVECTOR vRangeSquared =
        XMVectorReplicate( safeRange * safeRange );
    const float alphaRef = rendererState.GraphicsState.FF_AlphaRef;

    const auto prepareShadowMaterial =
        [&]( zCMaterial* material, bool includeBlendAlpha,
            bool requireAlphaRef ) {
        zCTexture* const texture =
            material ? material->GetTexture() : nullptr;
        const int alphaFunction =
            material ? material->GetAlphaFunc() : zMAT_ALPHA_FUNC_NONE;
        const bool blendedMaterial = includeBlendAlpha && material
            && alphaFunction != zMAT_ALPHA_FUNC_NONE
            && alphaFunction != zMAT_ALPHA_FUNC_MAT_DEFAULT;
        const bool needsAlphaTexture = texture && material
            && (material->HasAlphaTest()
                || texture->HasAlphaChannel()
                || blendedMaterial);

        if ( needsAlphaTexture ) {
            if ( (requireAlphaRef && alphaRef <= 0.0f)
                || texture->CacheIn( 0.6f ) != zRES_CACHED_IN ) {
                return -1;
            }
            auto* surface = texture->GetSurface();
            auto* engineTexture =
                surface ? surface->GetEngineTexture() : nullptr;
            if ( !engineTexture || !engineTexture->IsValid() ) {
                return -1;
            }
            if ( lastBoundTexture != engineTexture ) {
                engineTexture->BindToPixelShader( 0 );
                lastBoundTexture = engineTexture;
            }
            if ( shadowPixelShader->Apply() != XR_SUCCESS ) {
                return -1;
            }
            return 1;
        }

        if ( linearDepth ) {
            if ( lastBoundTexture != WhiteTexture.get() ) {
                WhiteTexture->BindToPixelShader( 0 );
                lastBoundTexture = WhiteTexture.get();
            }
            if ( shadowPixelShader->Apply() != XR_SUCCESS ) {
                return -1;
            }
        } else {
            Context->PSSetShader( nullptr, nullptr, 0 );
        }
        return 0;
    };

    std::vector<WorldMeshSectionInfo*> drawnSections;
    const bool hasWorldCache =
        worldMeshCache && !worldMeshCache->empty();
    const bool collectForWorld = drawWorldCasters
        && rendererState.RendererSettings.DrawWorldMesh
        && !hasWorldCache;
    const bool collectForVobs = drawVobCasters
        && rendererState.RendererSettings.DrawVOBs
        && (!renderedVobs || renderedVobs->empty());
    if ( collectForWorld || collectForVobs ) {
        Frustum pointLightBounds;
        pointLightBounds.BuildCubemapFace( position, safeRange, 0 );
        gapi->CollectVisibleSections(
            drawnSections, &pointLightBounds, true );
    }

    if ( drawWorldCasters
        && rendererState.RendererSettings.DrawWorldMesh ) {
        auto event =
            RecordGraphicsEvent( GE_NAME( "DrawWorldMesh::Layered" ) );
        if ( hasWorldCache ) {
            for ( const auto& meshByKey : *worldMeshCache ) {
                const MeshKey& key = meshByKey.first;
                MeshInfo* const mesh = meshByKey.second;
                if ( !key.Info
                    || key.Info->MaterialType != MaterialInfo::MT_None ) {
                    continue;
                }

                const int materialMode =
                    prepareShadowMaterial( key.Material, false, true );
                const bool isAlpha = materialMode > 0;
                if ( materialMode < 0
                    || !IsDrawableShadowMesh( mesh, isAlpha ) ) {
                    continue;
                }
                DrawVertexBufferInstancedIndexed(
                    mesh->MeshVertexBuffer,
                    GetShadowAwareIndexBuffer( mesh, isAlpha ),
                    GetShadowAwareIndexCount( mesh, isAlpha ), 6 );
            }
        } else {
            for ( WorldMeshSectionInfo* section : drawnSections ) {
                if ( !section ) {
                    continue;
                }

                if ( rendererState.RendererSettings.FastShadows ) {
                    MeshInfo* const mesh = section->FullStaticMesh;
                    if ( IsDrawableMeshInfo( mesh )
                        && prepareShadowMaterial(
                            nullptr, false, true ) >= 0 ) {
                        gapi->DrawMeshInfo_Layered( nullptr, mesh );
                    }
                    continue;
                }

                for ( const auto& meshByKey : section->WorldMeshes ) {
                    const MeshKey& key = meshByKey.first;
                    MeshInfo* const mesh = meshByKey.second;
                    if ( !key.Info
                        || key.Info->MaterialType != MaterialInfo::MT_None ) {
                        continue;
                    }

                    const int materialMode =
                        prepareShadowMaterial( key.Material, false, true );
                    const bool isAlpha = materialMode > 0;
                    if ( materialMode < 0
                        || !IsDrawableShadowMesh( mesh, isAlpha ) ) {
                        continue;
                    }
                    DrawVertexBufferInstancedIndexed(
                        mesh->MeshVertexBuffer,
                        GetShadowAwareIndexBuffer( mesh, isAlpha ),
                        GetShadowAwareIndexCount( mesh, isAlpha ), 6 );
                }
            }
        }
    }

    if ( drawVobCasters && rendererState.RendererSettings.DrawVOBs ) {
        std::list<VobInfo*> collectedVobs;
        if ( !renderedVobs || renderedVobs->empty() ) {
            for ( WorldMeshSectionInfo* section : drawnSections ) {
                if ( !section ) {
                    continue;
                }
                for ( VobInfo* vobInfo : section->Vobs ) {
                    if ( !vobInfo || !vobInfo->Vob
                        || !vobInfo->VisualInfo
                        || !vobInfo->Vob->GetShowVisual()
                        || std::find(
                            collectedVobs.begin(), collectedVobs.end(),
                            vobInfo ) != collectedVobs.end() ) {
                        continue;
                    }
                    if ( XMVector3Greater(
                        XMVector3LengthSq(
                            position - XMLoadFloat3(
                                &vobInfo->LastRenderPosition ) ),
                        vRangeSquared ) ) {
                        continue;
                    }
                    if ( isOutdoor && vobInfo->IsIndoorVob != indoor ) {
                        continue;
                    }
                    collectedVobs.emplace_back( vobInfo );
                }
            }
            if ( renderedVobs ) {
                *renderedVobs = collectedVobs;
            }
        }

        std::list<VobInfo*>& vobsToDraw =
            renderedVobs ? *renderedVobs : collectedVobs;
        auto event =
            RecordGraphicsEvent( GE_NAME( "Draw vobs (layered)" ) );
        if ( SetActiveVertexShader( VShaderID::VS_ExLayered ) == XR_SUCCESS
            && ActiveVS ) {
            SetupVS_ExMeshDrawCall();
            SetupVS_ExConstantBuffer();
            auto perVobBuffer =
                ActiveVS->GetBuffer( "Matrices_PerInstances" ).Bind();
            if ( perVobBuffer.Succeeded()
                && ActiveVS->Apply() == XR_SUCCESS ) {
                VS_ExConstantBuffer_PerInstance perVobState{};
                for ( VobInfo* vobInfo : vobsToDraw ) {
                    if ( !vobInfo || !vobInfo->Vob
                        || !vobInfo->VisualInfo
                        || !vobInfo->Vob->GetShowVisual() ) {
                        continue;
                    }
                    vobInfo->UpdateVobConstantBuffer( perVobState );
                    if ( !perVobBuffer.Update(
                            &perVobState,
                            sizeof( perVobState ) ).Succeeded() ) {
                        break;
                    }

                    for ( const auto& materialMeshes :
                        vobInfo->VisualInfo->Meshes ) {
                        const int materialMode = prepareShadowMaterial(
                            materialMeshes.first, true, false );
                        const bool isAlpha = materialMode > 0;
                        if ( materialMode < 0 ) {
                            continue;
                        }
                        for ( MeshInfo* mesh : materialMeshes.second ) {
                            if ( !IsDrawableShadowMesh(
                                    mesh, isAlpha ) ) {
                                continue;
                            }
                            DrawVertexBufferInstancedIndexed(
                                mesh->MeshVertexBuffer,
                                GetShadowAwareIndexBuffer(
                                    mesh, isAlpha ),
                                GetShadowAwareIndexCount(
                                    mesh, isAlpha ), 6 );
                        }
                    }
                }
            }
        }
    }

    const bool renderNPCs = !noNPCs && drawAnimatedCasters;
    if ( drawMobCasters && rendererState.RendererSettings.DrawMobs ) {
        std::list<SkeletalVobInfo*> collectedMobs;
        if ( !renderedMobs || renderedMobs->empty() ) {
            for ( SkeletalVobInfo* mob : gapi->GetSkeletalMeshVobs() ) {
                auto* visual = mob
                    ? dynamic_cast<SkeletalMeshVisualInfo*>(mob->VisualInfo)
                    : nullptr;
                if ( !mob || !mob->Vob || !visual
                    || !mob->Vob->GetShowVisual() ) {
                    continue;
                }
                if ( XMVector3Greater(
                    XMVector3LengthSq(
                        position - mob->Vob->GetPositionWorldXM() ),
                    vRangeSquared ) ) {
                    continue;
                }
                if ( isOutdoor
                    && mob->Vob->IsIndoorVob() != indoor ) {
                    continue;
                }
                if ( !visual->SkeletalMeshes.empty() ) {
                    continue;
                }
                collectedMobs.emplace_back( mob );
            }
            if ( renderedMobs ) {
                *renderedMobs = collectedMobs;
            }
        }

        std::list<SkeletalVobInfo*>& mobsToDraw =
            renderedMobs ? *renderedMobs : collectedMobs;
        auto event = RecordGraphicsEvent(
            GE_NAME( "Draw static skeletal meshes (layered)" ) );
        for ( SkeletalVobInfo* mob : mobsToDraw ) {
            if ( mob && mob->Vob && mob->VisualInfo
                && mob->Vob->GetShowVisual() ) {
                gapi->DrawSkeletalMeshVob_Layered( mob, FLT_MAX );
            }
        }
    }

    if ( drawAnimatedCasters && renderNPCs
        && rendererState.RendererSettings.DrawSkeletalMeshes ) {
        auto event = RecordGraphicsEvent(
            GE_NAME( "Draw animated skeletal meshes (layered)" ) );
        for ( SkeletalVobInfo* skeletalVob :
            gapi->GetAnimatedSkeletalMeshVobs() ) {
            if ( !skeletalVob || !skeletalVob->Vob
                || !skeletalVob->VisualInfo
                || !skeletalVob->Vob->GetShowVisual() ) {
                continue;
            }
            if ( skeletalVob->Vob->GetVisualAlpha()
                && skeletalVob->Vob->GetVobTransparency() < 0.7f ) {
                continue;
            }
            if ( XMVector3Greater(
                XMVector3LengthSq(
                    position
                    - skeletalVob->Vob->GetPositionWorldXM() ),
                vRangeSquared ) ) {
                continue;
            }
            if ( isOutdoor
                && skeletalVob->Vob->IsIndoorVob() != indoor ) {
                continue;
            }
            gapi->DrawSkeletalMeshVob_Layered(
                skeletalVob, FLT_MAX );
        }
    }
}

void D3D11GraphicsEngine::ShadowPass_DrawWorldMesh_Indirect(
    const std::vector<WorldMeshSectionInfo*>& visibleSections,
    const Frustum* cullingFrustum ) {
    TracyD3D11ZoneCGX( "ShadowPass_DrawWorldMesh_Indirect" );
    auto event = RecordGraphicsEvent(
        GE_NAME( "ShadowPass_DrawWorldMesh_Indirect" ) );

    auto* const gapi = Engine::GAPI;
    if ( !gapi || !Context ) {
        return;
    }
    auto& rendererState = gapi->GetRendererState();
    const float alphaRef = rendererState.GraphicsState.FF_AlphaRef;
    const bool linearDepth =
        (rendererState.GraphicsState.FF_GSwitches
            & GSWITCH_LINEAR_DEPTH) != 0;
    auto drawMultiIndexedInstancedIndirect =
        rendererState.RendererSettings.DebugSettings.FeatureSet.UseMDI
            && DrawMultiIndexedInstancedIndirect
        ? DrawMultiIndexedInstancedIndirect
        : Stub_DrawMultiIndexedInstancedIndirect;

    if ( rendererState.RendererSettings.FastShadows
        && !cullingFrustum ) {
        if ( !linearDepth ) {
            Context->PSSetShader( nullptr, nullptr, 0 );
        }
        for ( const WorldMeshSectionInfo* section : visibleSections ) {
            if ( section && IsDrawableMeshInfo(
                    section->FullStaticMesh ) ) {
                gapi->DrawMeshInfo(
                    nullptr, section->FullStaticMesh );
            }
        }
        return;
    }

    MeshInfo* const wrappedWorldMesh = gapi->GetWrappedWorldMesh();
    if ( !wrappedWorldMesh
        || !wrappedWorldMesh->MeshVertexBuffer
        || !wrappedWorldMesh->MeshVertexBuffer->IsValid()
        || !wrappedWorldMesh->MeshIndexBuffer
        || !wrappedWorldMesh->MeshIndexBuffer->IsValid() ) {
        return;
    }
    const bool hasGlobalShadowBuffer =
        wrappedWorldMesh->MeshShadowIndexBuffer
        && wrappedWorldMesh->MeshShadowIndexBuffer->IsValid();

    static thread_local
        std::vector<D3D11_DRAW_INDEXED_INSTANCED_INDIRECT_ARGS>
            opaqueDrawArgs;
    static thread_local std::vector<WorldMeshInfo*> opaqueMainMeshes;
    static thread_local std::vector<
        std::pair<zCTexture*, WorldMeshInfo*>> alphaMeshes;
    opaqueDrawArgs.clear();
    opaqueMainMeshes.clear();
    alphaMeshes.clear();
    if ( opaqueDrawArgs.capacity() == 0 ) {
        opaqueDrawArgs.reserve( 4096 );
        opaqueMainMeshes.reserve( 256 );
        alphaMeshes.reserve( 512 );
    }

    {
        TracyD3D11ZoneCGX(
            "ShadowPass_DrawWorldMesh_Indirect::Classify" );
        auto classifyEvent = RecordGraphicsEvent(
            GE_NAME(
                "ShadowPass_DrawWorldMesh_Indirect::Classify" ) );
        for ( const WorldMeshSectionInfo* section : visibleSections ) {
            if ( !section ) {
                continue;
            }
            for ( const auto& meshPair : section->WorldMeshes ) {
                const MeshKey& key = meshPair.first;
                WorldMeshInfo* const mesh = meshPair.second;
                if ( !key.Info || !mesh
                    || key.Info->MaterialType != MaterialInfo::MT_None
                    || (cullingFrustum
                        && !gapi->IsWorldMeshVisibleInFrustum(
                            mesh, *cullingFrustum )) ) {
                    continue;
                }

                zCMaterial* const material = key.Material;
                if ( material ) {
                    const int alphaFunction = material->GetAlphaFunc();
                    if ( (alphaFunction > zMAT_ALPHA_FUNC_NONE
                            && alphaFunction != zMAT_ALPHA_FUNC_TEST)
                        || (alphaFunction == zMAT_ALPHA_FUNC_NONE
                            && zColor( material->GetColor() )
                                .bgra.alpha < 255) ) {
                        continue;
                    }
                }

                zCTexture* const texture =
                    material ? material->GetTexture() : nullptr;
                const bool needsAlphaTest = texture && alphaRef > 0.0f
                    && (texture->HasAlphaChannel()
                        || material->HasAlphaTest());
                if ( needsAlphaTest ) {
                    auto* surface = texture->CacheIn( 0.6f )
                            == zRES_CACHED_IN
                        ? texture->GetSurface() : nullptr;
                    auto* engineTexture =
                        surface ? surface->GetEngineTexture() : nullptr;
                    if ( engineTexture && engineTexture->IsValid()
                        && IsDrawableShadowMesh( mesh, true ) ) {
                        alphaMeshes.emplace_back( texture, mesh );
                    }
                    continue;
                }

                if ( hasGlobalShadowBuffer
                    && UsesShadowIndexBuffer( mesh, false ) ) {
                    D3D11_DRAW_INDEXED_INSTANCED_INDIRECT_ARGS args{};
                    args.IndexCountPerInstance =
                        static_cast<unsigned int>(
                            mesh->ShadowIndices.size() );
                    args.InstanceCount = 1;
                    args.StartIndexLocation =
                        mesh->BaseShadowIndexLocation;
                    opaqueDrawArgs.emplace_back( args );
                } else if ( IsDrawableMeshInfo( mesh ) ) {
                    opaqueMainMeshes.emplace_back( mesh );
                }
            }
        }
    }

    if ( opaqueDrawArgs.empty() && opaqueMainMeshes.empty()
        && alphaMeshes.empty() ) {
        return;
    }

    UINT offset = 0;
    UINT stride = sizeof( ExVertexStruct );
    Context->IASetVertexBuffers(
        0, 1,
        wrappedWorldMesh->MeshVertexBuffer
            ->GetVertexBuffer().GetAddressOf(),
        &stride, &offset );

    if ( !opaqueDrawArgs.empty()
        || !opaqueMainMeshes.empty() ) {
        if ( linearDepth ) {
            if ( !WhiteTexture || !WhiteTexture->IsValid()
                || SetActivePixelShader( PShaderID::PS_LinDepth )
                    != XR_SUCCESS
                || !ActivePS ) {
                return;
            }
            WhiteTexture->BindToPixelShader( 0 );
            if ( ActivePS->Apply() != XR_SUCCESS ) {
                return;
            }
        } else {
            Context->PSSetShader( nullptr, nullptr, 0 );
        }
    }

    if ( !opaqueDrawArgs.empty() ) {
        TracyD3D11ZoneCGX(
            "ShadowPass_DrawWorldMesh_Indirect::OpaqueSubmission" );
        auto opaqueEvent = RecordGraphicsEvent(
            GE_NAME(
                "ShadowPass_DrawWorldMesh_Indirect::OpaqueSubmission" ) );
        Context->IASetIndexBuffer(
            wrappedWorldMesh->MeshShadowIndexBuffer
                ->GetVertexBuffer().Get(),
            DXGI_FORMAT_R32_UINT, 0 );

        D3D11IndirectBuffer* indirectBuffer = nullptr;
        constexpr size_t argumentSize =
            sizeof( D3D11_DRAW_INDEXED_INSTANCED_INDIRECT_ARGS );
        if ( opaqueDrawArgs.size()
                <= (std::numeric_limits<unsigned int>::max)()
                    / argumentSize
            && opaqueDrawArgs.size()
                <= (std::numeric_limits<unsigned int>::max)() ) {
            const auto requiredSize = static_cast<unsigned int>(
                opaqueDrawArgs.size() * argumentSize );
            indirectBuffer = AcquireFrameIndirectBuffer(
                m_ShadowWorldIndirectPool, opaqueDrawArgs.data(),
                requiredSize, "ShadowWorldMeshIndirectArgs" );
        }

        bool submittedIndirect = false;
        if ( indirectBuffer && indirectBuffer->IsValid()
            && indirectBuffer->GetIndirectBuffer().Get() ) {
            drawMultiIndexedInstancedIndirect(
                Context.Get(),
                static_cast<unsigned int>(opaqueDrawArgs.size()),
                indirectBuffer->GetIndirectBuffer().Get(), 0,
                sizeof(
                    D3D11_DRAW_INDEXED_INSTANCED_INDIRECT_ARGS ) );
            submittedIndirect = true;
        }

        if ( submittedIndirect ) {
            for ( const auto& args : opaqueDrawArgs ) {
                rendererState.RendererInfo.FrameDrawnTriangles +=
                    args.IndexCountPerInstance / 3;
            }
        } else {
            LogWarn() << "Falling back to direct world shadow draws.";
            for ( const auto& args : opaqueDrawArgs ) {
                DrawVertexBufferIndexedUINT(
                    nullptr, nullptr,
                    args.IndexCountPerInstance,
                    args.StartIndexLocation );
            }
        }
    }

    if ( !opaqueMainMeshes.empty() ) {
        Context->IASetIndexBuffer(
            wrappedWorldMesh->MeshIndexBuffer
                ->GetVertexBuffer().Get(),
            DXGI_FORMAT_R32_UINT, 0 );
        for ( WorldMeshInfo* mesh : opaqueMainMeshes ) {
            DrawVertexBufferIndexedUINT(
                nullptr, nullptr,
                static_cast<unsigned int>(mesh->Indices.size()),
                mesh->BaseIndexLocation );
        }
    }

    if ( alphaMeshes.empty() ) {
        return;
    }

    TracyD3D11ZoneCGX(
        "ShadowPass_DrawWorldMesh_Indirect::AlphaSubmission" );
    auto alphaEvent = RecordGraphicsEvent(
        GE_NAME(
            "ShadowPass_DrawWorldMesh_Indirect::AlphaSubmission" ) );
    std::sort(
        alphaMeshes.begin(), alphaMeshes.end(),
        []( const auto& left, const auto& right ) {
            return left.first < right.first;
        } );
    if ( SetActivePixelShader(
            linearDepth ? PShaderID::PS_LinDepth
                        : PShaderID::PS_DiffuseAlphaTestShadows )
            != XR_SUCCESS
        || !ActivePS || ActivePS->Apply() != XR_SUCCESS ) {
        return;
    }

    Context->PSSetShaderResources( 0, 3, s_nullSRVs );
    Context->IASetIndexBuffer(
        wrappedWorldMesh->MeshIndexBuffer
            ->GetVertexBuffer().Get(),
        DXGI_FORMAT_R32_UINT, 0 );

    zCTexture* lastTexture = nullptr;
    for ( const auto& [texture, mesh] : alphaMeshes ) {
        if ( !texture || !mesh ) {
            continue;
        }
        if ( texture != lastTexture ) {
            auto* surface = texture->CacheIn( 0.6f )
                    == zRES_CACHED_IN
                ? texture->GetSurface() : nullptr;
            auto* engineTexture =
                surface ? surface->GetEngineTexture() : nullptr;
            ID3D11ShaderResourceView* const textureView =
                engineTexture && engineTexture->IsValid()
                ? engineTexture->GetShaderResourceView().Get() : nullptr;
            if ( !textureView ) {
                continue;
            }
            Context->PSSetShaderResources( 0, 1, &textureView );
            lastTexture = texture;
        }

        DrawVertexBufferIndexedUINT(
            nullptr, nullptr,
            static_cast<unsigned int>(mesh->Indices.size()),
            mesh->BaseIndexLocation );
    }
}

void D3D11GraphicsEngine::ShadowPass_DrawWorldMesh(
    const std::vector<WorldMeshSectionInfo*>& visibleSections,
    const Frustum* cullingFrustum ) {
    TracyD3D11ZoneCGX( "ShadowPass_DrawWorldMesh" );
    auto event =
        RecordGraphicsEvent( GE_NAME( "ShadowPass_DrawWorldMesh" ) );

    auto* const gapi = Engine::GAPI;
    if ( !gapi || !Context ) {
        return;
    }
    MeshInfo* const wrappedWorldMesh = gapi->GetWrappedWorldMesh();
    if ( !wrappedWorldMesh
        || !wrappedWorldMesh->MeshVertexBuffer
        || !wrappedWorldMesh->MeshVertexBuffer->IsValid()
        || !wrappedWorldMesh->MeshIndexBuffer
        || !wrappedWorldMesh->MeshIndexBuffer->IsValid() ) {
        return;
    }

    auto& rendererState = gapi->GetRendererState();
    const float alphaRef = rendererState.GraphicsState.FF_AlphaRef;
    const bool linearDepth =
        (rendererState.GraphicsState.FF_GSwitches
            & GSWITCH_LINEAR_DEPTH) != 0;
    const bool hasGlobalShadowBuffer =
        wrappedWorldMesh->MeshShadowIndexBuffer
        && wrappedWorldMesh->MeshShadowIndexBuffer->IsValid();

    static thread_local std::vector<WorldMeshInfo*> opaqueShadowMeshes;
    static thread_local std::vector<WorldMeshInfo*> opaqueMainMeshes;
    static thread_local std::vector<std::pair<zCTexture*, WorldMeshInfo*>>
        alphaMeshes;
    opaqueShadowMeshes.clear();
    opaqueMainMeshes.clear();
    alphaMeshes.clear();

    {
        ZoneScopedN( "ShadowPass_DrawWorldMesh::Classify" );
        auto classifyEvent = RecordGraphicsEvent(
            GE_NAME( "ShadowPass_DrawWorldMesh::Classify" ) );
        for ( const WorldMeshSectionInfo* section : visibleSections ) {
            if ( !section ) {
                continue;
            }
            for ( const auto& meshPair : section->WorldMeshes ) {
                const MeshKey& key = meshPair.first;
                WorldMeshInfo* const mesh = meshPair.second;
                if ( !key.Info || !mesh
                    || key.Info->MaterialType != MaterialInfo::MT_None
                    || (cullingFrustum
                        && !gapi->IsWorldMeshVisibleInFrustum(
                            mesh, *cullingFrustum )) ) {
                    continue;
                }

                zCMaterial* const material = key.Material;
                if ( material ) {
                    const int alphaFunction = material->GetAlphaFunc();
                    if ( (alphaFunction > zMAT_ALPHA_FUNC_NONE
                            && alphaFunction != zMAT_ALPHA_FUNC_TEST)
                        || (alphaFunction == zMAT_ALPHA_FUNC_NONE
                            && zColor( material->GetColor() )
                                .bgra.alpha < 255) ) {
                        continue;
                    }
                }

                zCTexture* const texture =
                    material ? material->GetTexture() : nullptr;
                const bool needsAlphaTest = texture && alphaRef > 0.0f
                    && (texture->HasAlphaChannel()
                        || material->HasAlphaTest());
                if ( needsAlphaTest ) {
                    auto* surface = texture->CacheIn( 0.6f )
                            == zRES_CACHED_IN
                        ? texture->GetSurface() : nullptr;
                    auto* engineTexture =
                        surface ? surface->GetEngineTexture() : nullptr;
                    if ( engineTexture && engineTexture->IsValid()
                        && IsDrawableShadowMesh( mesh, true ) ) {
                        alphaMeshes.emplace_back( texture, mesh );
                    }
                    continue;
                }

                if ( hasGlobalShadowBuffer
                    && UsesShadowIndexBuffer( mesh, false ) ) {
                    opaqueShadowMeshes.emplace_back( mesh );
                } else if ( IsDrawableMeshInfo( mesh ) ) {
                    opaqueMainMeshes.emplace_back( mesh );
                }
            }
        }
    }

    if ( opaqueShadowMeshes.empty() && opaqueMainMeshes.empty()
        && alphaMeshes.empty() ) {
        return;
    }

    UINT offset = 0;
    UINT stride = sizeof( ExVertexStruct );
    Context->IASetVertexBuffers(
        0, 1,
        wrappedWorldMesh->MeshVertexBuffer
            ->GetVertexBuffer().GetAddressOf(),
        &stride, &offset );

    if ( !opaqueShadowMeshes.empty()
        || !opaqueMainMeshes.empty() ) {
        TracyD3D11ZoneCGX(
            "ShadowPass_DrawWorldMesh::OpaqueSubmission" );
        auto opaqueEvent = RecordGraphicsEvent(
            GE_NAME( "ShadowPass_DrawWorldMesh::OpaqueSubmission" ) );
        if ( linearDepth ) {
            if ( !WhiteTexture || !WhiteTexture->IsValid()
                || SetActivePixelShader( PShaderID::PS_LinDepth )
                    != XR_SUCCESS
                || !ActivePS ) {
                return;
            }
            WhiteTexture->BindToPixelShader( 0 );
            if ( ActivePS->Apply() != XR_SUCCESS ) {
                return;
            }
        } else {
            Context->PSSetShader( nullptr, nullptr, 0 );
        }

        if ( !opaqueShadowMeshes.empty() ) {
            Context->IASetIndexBuffer(
                wrappedWorldMesh->MeshShadowIndexBuffer
                    ->GetVertexBuffer().Get(),
                DXGI_FORMAT_R32_UINT, 0 );
            for ( WorldMeshInfo* mesh : opaqueShadowMeshes ) {
                DrawVertexBufferIndexedUINT(
                    nullptr, nullptr,
                    static_cast<unsigned int>(
                        mesh->ShadowIndices.size() ),
                    mesh->BaseShadowIndexLocation );
            }
        }

        if ( !opaqueMainMeshes.empty() ) {
            Context->IASetIndexBuffer(
                wrappedWorldMesh->MeshIndexBuffer
                    ->GetVertexBuffer().Get(),
                DXGI_FORMAT_R32_UINT, 0 );
            for ( WorldMeshInfo* mesh : opaqueMainMeshes ) {
                DrawVertexBufferIndexedUINT(
                    nullptr, nullptr,
                    static_cast<unsigned int>(mesh->Indices.size()),
                    mesh->BaseIndexLocation );
            }
        }
    }

    if ( alphaMeshes.empty() ) {
        return;
    }

    TracyD3D11ZoneCGX(
        "ShadowPass_DrawWorldMesh::AlphaSubmission" );
    auto alphaEvent = RecordGraphicsEvent(
        GE_NAME( "ShadowPass_DrawWorldMesh::AlphaSubmission" ) );
    std::sort(
        alphaMeshes.begin(), alphaMeshes.end(),
        []( const auto& left, const auto& right ) {
            return left.first < right.first;
        } );
    if ( SetActivePixelShader(
            linearDepth ? PShaderID::PS_LinDepth
                        : PShaderID::PS_DiffuseAlphaTestShadows )
            != XR_SUCCESS
        || !ActivePS || ActivePS->Apply() != XR_SUCCESS ) {
        return;
    }

    Context->PSSetShaderResources( 0, 3, s_nullSRVs );
    Context->IASetIndexBuffer(
        wrappedWorldMesh->MeshIndexBuffer
            ->GetVertexBuffer().Get(),
        DXGI_FORMAT_R32_UINT, 0 );

    zCTexture* lastTexture = nullptr;
    for ( const auto& [texture, mesh] : alphaMeshes ) {
        if ( !texture || !mesh ) {
            continue;
        }
        if ( texture != lastTexture ) {
            auto* surface = texture->CacheIn( 0.6f )
                    == zRES_CACHED_IN
                ? texture->GetSurface() : nullptr;
            auto* engineTexture =
                surface ? surface->GetEngineTexture() : nullptr;
            ID3D11ShaderResourceView* const textureView =
                engineTexture && engineTexture->IsValid()
                ? engineTexture->GetShaderResourceView().Get() : nullptr;
            if ( !textureView ) {
                continue;
            }
            Context->PSSetShaderResources( 0, 1, &textureView );
            lastTexture = texture;
        }

        DrawVertexBufferIndexedUINT(
            nullptr, nullptr,
            static_cast<unsigned int>(mesh->Indices.size()),
            mesh->BaseIndexLocation );
    }
}

/** Draws everything around the given position */
void XM_CALLCONV D3D11GraphicsEngine::DrawWorldAroundForWorldShadow( FXMVECTOR position,
    float sectionRange,
    const RenderShadowmapsParams& params ) {

    auto* const gapi = Engine::GAPI;
    if ( !gapi || !Context || !ShaderManager
        || !DistortionTexture || !DistortionTexture->IsValid()
        || !InfiniteRangeConstantBuffer
        || !InfiniteRangeConstantBuffer->IsValid()
        || !std::isfinite( sectionRange ) || sectionRange <= 0.0f
        || params.CascadeIndex < -1
        || params.CascadeIndex >= static_cast<int>(MAX_CSM_CASCADES) ) {
        LogError() << "Invalid world-shadow render parameters or resources.";
        return;
    }

    float3 shadowPosition{};
    XMStoreFloat3( shadowPosition.toXMFLOAT3(), position );
    if ( !std::isfinite( shadowPosition.x )
        || !std::isfinite( shadowPosition.y )
        || !std::isfinite( shadowPosition.z ) ) {
        LogError() << "World-shadow position is not finite.";
        return;
    }

    auto& rendererState = gapi->GetRendererState();
    const bool linearDepth =
        (rendererState.GraphicsState.FF_GSwitches & GSWITCH_LINEAR_DEPTH) != 0;
    if ( SetActiveVertexShader( VShaderID::VS_Ex ) != XR_SUCCESS
        || SetActivePixelShader( linearDepth
            ? PShaderID::PS_LinDepth
            : PShaderID::PS_DiffuseAlphaTestShadows ) != XR_SUCCESS
        || !ActiveVS || !ActivePS ) {
        LogError() << "Required world-shadow shaders are unavailable.";
        return;
    }
    const auto defaultPS = ActivePS;
    GSky* const sky = gapi->GetSky();
    if ( !sky ) {
        LogError() << "World-shadow rendering requires a valid sky state.";
        return;
    }

    rendererState.RasterizerState.SetDefault();
    rendererState.RasterizerState.CullMode =
        params.CullFront ? GothicRasterizerStateInfo::CM_CULL_FRONT
        : GothicRasterizerStateInfo::CM_CULL_BACK;
    if ( params.DontCull ) {
        rendererState.RasterizerState.CullMode =
            GothicRasterizerStateInfo::CM_CULL_NONE;
    }
    rendererState.RasterizerState.DepthClipEnable = true;
    rendererState.RasterizerState.SetDirty();

    rendererState.DepthState.SetDefault();
    rendererState.DepthState.DepthBufferCompareFunc =
        GothicDepthBufferStateInfo::ECompareFunc::CF_COMPARISON_LESS_EQUAL;
    rendererState.DepthState.SetDirty();

    const XMMATRIX view = gapi->GetViewMatrixXM();
    gapi->ResetWorldTransform();
    gapi->SetViewTransformXM( view );

    rendererState.GraphicsState.FF_AlphaRef = 170.0f / 255.0f;
    auto pipelineBuffer = ActivePS->GetBuffer( "FFPipelineConstantBuffer" );
    auto atmosphereBuffer = ActivePS->GetBuffer( "Atmosphere" );
    pipelineBuffer.Update( &rendererState.GraphicsState ).Bind();
    atmosphereBuffer.Update( &sky->GetAtmosphereCB() ).Bind();
    if ( !pipelineBuffer.Succeeded() || !atmosphereBuffer.Succeeded()
        || !ActiveVS->GetBuffer( 0 ).GetRawBuffer() ) {
        LogError() << "World-shadow shader constant buffers are unavailable.";
        return;
    }

    if ( UpdateRenderStates() != XR_SUCCESS
        || ActiveVS->Apply() != XR_SUCCESS
        || ActivePS->Apply() != XR_SUCCESS ) {
        LogError() << "Failed to bind world-shadow render state.";
        return;
    }
    Context->IASetPrimitiveTopology( D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST );
    SetupVS_ExConstantBuffer();

    const XMMATRIX identityMatrix = XMMatrixIdentity();
    auto cbMatrices_PerInstances =
        ActiveVS->GetBuffer( "Matrices_PerInstances" );
    cbMatrices_PerInstances
        .Update( &identityMatrix, sizeof( identityMatrix ) ).Bind();
    if ( !cbMatrices_PerInstances.Succeeded() ) {
        LogError() << "World-shadow instance matrix buffer is unavailable.";
        return;
    }

    DistortionTexture->BindToPixelShader( 0 );
    ActivePS->BindBuffer(
        "DIST_Distance", InfiniteRangeConstantBuffer.get() );

    const bool enableCulling =
        rendererState.RendererSettings.IsShadowFrustumCullingEnabled();
    const bool colorWritesEnabled =
        rendererState.BlendState.ColorWritesEnabled;
    const float alphaRef = rendererState.GraphicsState.FF_AlphaRef;
    const Frustum* currentFrustum = nullptr;
    Frustum alwaysContainingFrustum;

    if ( params.CascadeIndex >= 0 && params.CascadeCameraReplacements ) {
        currentFrustum = &(*params.CascadeCameraReplacements)[
            static_cast<size_t>(params.CascadeIndex)].frustum;
    } else if ( const auto* cameraReplacement =
        gapi->GetCameraReplacementPtr() ) {
        currentFrustum = &cameraReplacement->frustum;
    }

    if ( !currentFrustum || !currentFrustum->IsValid() ) {
        alwaysContainingFrustum = Frustum::AlwaysContainingFrustum();
        currentFrustum = &alwaysContainingFrustum;
    }

    // World geometry is already reduced by section collection. A cascade frustum
    // edge-case around sun zenith can reject only worldmesh while VOB/NPC casters
    // still render, so keep worldmesh shadow culling conservative here.
    Frustum relaxedWorldShadowFrustum = Frustum::AlwaysContainingFrustum();
    const Frustum* worldMeshShadowFrustum = &relaxedWorldShadowFrustum;

    if ( rendererState.RendererSettings.DrawWorldMesh ) {
        TracyD3D11ZoneCGX( "Shadows::DrawWorldMesh" );
        auto worldMeshEvent =
            RecordGraphicsEvent( GE_NAME( "Shadows::DrawWorldMesh" ) );

        static thread_local std::vector<WorldMeshSectionInfo*> visibleSections;
        visibleSections.clear();
        gapi->CollectVisibleSections(
            visibleSections, worldMeshShadowFrustum, false );

        MeshInfo* const wrappedWorldMesh = gapi->GetWrappedWorldMesh();
        const bool canUseIndirect =
            rendererState.RendererSettings.DebugSettings.FeatureSet.UseMDI
            && wrappedWorldMesh
            && wrappedWorldMesh->MeshVertexBuffer
            && wrappedWorldMesh->MeshVertexBuffer->IsValid()
            && wrappedWorldMesh->MeshIndexBuffer
            && wrappedWorldMesh->MeshIndexBuffer->IsValid()
            && wrappedWorldMesh->MeshShadowIndexBuffer
            && wrappedWorldMesh->MeshShadowIndexBuffer->IsValid();
        if ( canUseIndirect ) {
            ShadowPass_DrawWorldMesh_Indirect(
                visibleSections, worldMeshShadowFrustum );
        } else {
            ShadowPass_DrawWorldMesh(
                visibleSections, worldMeshShadowFrustum );
        }
    }

    if ( rendererState.RendererSettings.DrawVOBs ) {
        ZoneScopedN( "Shadows::DrawVOBs" );
        auto shadowVobEvent =
            RecordGraphicsEvent( GE_NAME( "Shadows::DrawVOBs" ) );

        static thread_local std::vector<VobInfo*> potentialCasters;
        static thread_local std::vector<VobLightInfo*> unusedLights;
        static thread_local std::vector<SkeletalVobInfo*> unusedSkeletalVobs;
        std::vector<VobInfo*>* vobs = nullptr;

        if ( params.CascadeIndex >= 0 ) {
            auto* renderQueue = ShadowMaps
                ? ShadowMaps->GetRenderQueue( params.CascadeIndex ) : nullptr;
            if ( !renderQueue
                || renderQueue->ProcessQueue() != XR_SUCCESS ) {
                LogError() << "Invalid or unavailable shadow render queue for cascade "
                    << params.CascadeIndex << ".";
            } else {
                vobs = &renderQueue->GetVobs();
            }
        } else {
            potentialCasters.clear();
            unusedLights.clear();
            unusedSkeletalVobs.clear();
            if ( potentialCasters.capacity() < 1024 ) {
                potentialCasters.reserve( 1024 );
            }

            LegacyRenderQueueProxy queue(
                potentialCasters, unusedLights, unusedSkeletalVobs );
            RndCullContext cullContext{};
            cullContext.queue = &queue;
            cullContext.cameraPosition = gapi->GetCameraPosition();
            cullContext.stage = RenderStage::STAGE_DRAW_WORLD;
            cullContext.frustum = *currentFrustum;

            const auto& settings = rendererState.RendererSettings;
            cullContext.drawDistances.OutdoorVobs =
                settings.OutdoorVobDrawRadius;
            cullContext.drawDistances.OutdoorVobsSmall =
                settings.OutdoorSmallVobDrawRadius;
            cullContext.drawDistances.IndoorVobs =
                settings.IndoorVobDrawRadius;
            cullContext.drawDistances.VisualFX =
                settings.GetEffectiveVisualFXDrawRadius();
            cullContext.drawDistancesSq.OutdoorVobs =
                cullContext.drawDistances.OutdoorVobs
                * cullContext.drawDistances.OutdoorVobs;
            cullContext.drawDistancesSq.OutdoorVobsSmall =
                cullContext.drawDistances.OutdoorVobsSmall
                * cullContext.drawDistances.OutdoorVobsSmall;
            cullContext.drawDistancesSq.IndoorVobs =
                cullContext.drawDistances.IndoorVobs
                * cullContext.drawDistances.IndoorVobs;
            cullContext.drawDistancesSq.VisualFX =
                cullContext.drawDistances.VisualFX
                * cullContext.drawDistances.VisualFX;
            cullContext.drawFlags.DrawVOBs = settings.DrawVOBs;
            cullContext.drawFlags.DrawMobs = settings.DrawMobs;
            cullContext.drawFlags.EnableDynamicLighting =
                settings.EnableDynamicLighting;
            cullContext.drawFlags.CullVobs =
                settings.DebugSettings.Culling.CullVobs;
            cullContext.drawFlags.CollectIndoorVobs = true;
            cullContext.drawFlags.CollectLargeVobs = true;
            cullContext.drawFlags.CollectSmallVobs = true;
            cullContext.drawFlags.CollectMobs = true;
            cullContext.drawFlags.CollectLights = true;
            gapi->CollectVisibleVobs( cullContext );
            vobs = &potentialCasters;
        }

        const auto& staticMeshVisuals = gapi->GetStaticMeshVisuals();
        for ( const auto& pair : staticMeshVisuals ) {
            if ( pair.second ) pair.second->StartNewFrame();
        }

        if ( vobs ) {
            for ( VobInfo* vobInfo : *vobs ) {
                if ( !vobInfo || !vobInfo->Vob
                    || !vobInfo->VisualInfo
                    || !vobInfo->Vob->GetShowVisual() ) {
                    continue;
                }
                auto* visual =
                    dynamic_cast<MeshVisualInfo*>(vobInfo->VisualInfo);
                if ( !visual ) continue;

                vobInfo->UpdateState();
                VobInstanceInfo instance{};
                instance.world = vobInfo->WorldMatrix;
                instance.prevWorld = vobInfo->HasValidPrevMatrix
                    ? vobInfo->PrevWorldMatrix : vobInfo->WorldMatrix;
                instance.color = vobInfo->GroundColor;
                if ( vobInfo->IndoorLightMask ) {
                    instance.color =
                        (instance.color & 0x00FFFFFFu) | 0x0D000000u;
                }
                instance.windStrenth = 0.0f;
                instance.canBeAffectedByPlayer = 0;

                const zTAnimationMode animationMode =
                    vobInfo->Vob->GetVisualAniMode();
                if ( animationMode != zVISUAL_ANIMODE_NONE ) {
                    instance.canBeAffectedByPlayer =
                        vobInfo->Vob->GetDynColl() ? 0.0f : 1.0f;
                    GothicAPI::ProcessVobAnimation(
                        vobInfo->Vob, animationMode, instance );
                }
                visual->Instances.push_back( instance );
            }
        }

        std::vector<MeshVisualInfo*> activeVisuals;
        activeVisuals.reserve(
            (std::min<size_t>)(staticMeshVisuals.size(), 256u) );
        size_t totalInstances = 0;
        constexpr size_t maxShadowInstances =
            (std::numeric_limits<UINT>::max)() / sizeof( VobInstanceInfo );
        bool instanceCountValid = true;
        for ( const auto& pair : staticMeshVisuals ) {
            MeshVisualInfo* const visual = pair.second;
            if ( !visual || visual->Instances.empty() ) continue;
            if ( visual->Instances.size()
                > maxShadowInstances - totalInstances ) {
                instanceCountValid = false;
                break;
            }
            totalInstances += visual->Instances.size();
            activeVisuals.push_back( visual );
        }
        if ( !instanceCountValid ) {
            LogError() << "Shadow VOB instance data exceeds D3D11 limits.";
        }

        auto drawStaticShadowVobs = [&]() {
            if ( !instanceCountValid || totalInstances == 0 ) return;

            const UINT requiredBytes = static_cast<UINT>(
                totalInstances * sizeof( VobInstanceInfo ) );
            D3D11VertexBuffer* instancingBuffer =
                AcquireFrameInstancingBuffer(
                    m_ShadowVobInstancingPool, requiredBytes,
                    "ShadowVobInstancingBuffer" );
            if ( !instancingBuffer ) {
                LogWarn() << "Using fallback shadow VOB instancing buffer.";
                instancingBuffer = DynamicInstancingBuffer.get();
                if ( !instancingBuffer ) {
                    LogError() << "No shadow VOB instancing buffer is available.";
                    return;
                }
                if ( !instancingBuffer->IsValid()
                    || instancingBuffer->GetSizeInBytes() < requiredBytes ) {
                    if ( instancingBuffer->Init(
                            nullptr, requiredBytes,
                            D3D11VertexBuffer::B_VERTEXBUFFER,
                            D3D11VertexBuffer::U_DYNAMIC,
                            D3D11VertexBuffer::CA_WRITE,
                            "ShadowVobInstancingBufferFallback" )
                        != XR_SUCCESS ) {
                        LogError() << "Failed to initialize fallback shadow VOB instancing buffer.";
                        return;
                    }
                }
            }
            if ( !instancingBuffer->IsValid()
                || !instancingBuffer->GetVertexBuffer() ) {
                LogError() << "Shadow VOB instancing buffer is invalid.";
                return;
            }

            if ( SetActiveVertexShader( VShaderID::VS_ExInstancedObj )
                    != XR_SUCCESS
                || !ActiveVS || !ActiveVS->GetBuffer( 0 ).GetRawBuffer()
                || UpdateRenderStates() != XR_SUCCESS
                || ActiveVS->Apply() != XR_SUCCESS
                || !defaultPS || defaultPS->Apply() != XR_SUCCESS ) {
                LogError() << "Failed to bind the instanced shadow VOB shaders.";
                return;
            }
            Context->IASetPrimitiveTopology(
                D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST );
            SetupVS_ExConstantBuffer();

            const bool useWindMetadata =
                PrepareAndBindWindMetadata( activeVisuals );

            byte* mappedData = nullptr;
            UINT mappedBytes = 0;
            const XRESULT mapResult = instancingBuffer->Map(
                D3D11VertexBuffer::M_WRITE_DISCARD,
                reinterpret_cast<void**>(&mappedData), &mappedBytes );
            if ( mapResult != XR_SUCCESS || !mappedData
                || mappedBytes < requiredBytes ) {
                if ( mapResult == XR_SUCCESS ) instancingBuffer->Unmap();
                if ( useWindMetadata ) UnbindWindMetadata();
                LogError() << "Failed to map the shadow VOB instancing buffer.";
                return;
            }

            UINT instanceOffset = 0;
            for ( MeshVisualInfo* visual : activeVisuals ) {
                const UINT instanceCount =
                    static_cast<UINT>(visual->Instances.size());
                visual->StartInstanceNum = instanceOffset;
                memcpy(
                    mappedData
                        + static_cast<size_t>(instanceOffset)
                            * sizeof( VobInstanceInfo ),
                    visual->Instances.data(),
                    static_cast<size_t>(instanceCount)
                        * sizeof( VobInstanceInfo ) );
                instanceOffset += instanceCount;
            }
            if ( instancingBuffer->Unmap() != XR_SUCCESS ) {
                if ( useWindMetadata ) UnbindWindMetadata();
                LogError() << "Failed to unmap the shadow VOB instancing buffer.";
                return;
            }

            D3D11PShader* currentPixelShader = defaultPS.get();
            if ( !linearDepth ) {
                Context->PSSetShader( nullptr, nullptr, 0 );
                currentPixelShader = nullptr;
            }

            GraphicsShaderConstantBuffer windBuffer{};
            if ( rendererState.RendererSettings.WindQuality > 0 ) {
                windBuffer = ActiveVS->GetBuffer( "WindParams" );
                windBuffer.Bind();
                if ( !windBuffer.Succeeded() ) {
                    windBuffer = GraphicsShaderConstantBuffer{};
                }
            }
            if ( windBuffer.GetRawBuffer() ) {
                UpdateCharacterInteractionPositions( g_windBuffer );
                windBuffer.Update( &g_windBuffer );
            }

            UINT instanceStride = sizeof( VobInstanceInfo );
            UINT instanceBufferOffset = 0;
            ID3D11Buffer* instanceBuffer =
                instancingBuffer->GetVertexBuffer().Get();
            Context->IASetVertexBuffers(
                1, 1, &instanceBuffer,
                &instanceStride, &instanceBufferOffset );

            size_t meshCount = 0;
            bool meshCountValid = true;
            for ( MeshVisualInfo* visual : activeVisuals ) {
                for ( const auto& materialMeshes :
                    visual->MeshesByTexture ) {
                    if ( materialMeshes.second.size()
                        > (std::numeric_limits<size_t>::max)() - meshCount ) {
                        meshCountValid = false;
                        break;
                    }
                    meshCount += materialMeshes.second.size();
                }
                if ( !meshCountValid ) break;
            }
            if ( !meshCountValid ) {
                if ( useWindMetadata ) UnbindWindMetadata();
                LogError() << "Shadow VOB mesh count overflow.";
                return;
            }

            using ShadowVobDraw = std::tuple<
                MeshVisualInfo*, MeshKey, MeshInfo*, uint64_t>;
            std::vector<ShadowVobDraw> meshesToDraw;
            try {
                meshesToDraw.reserve( meshCount );
                for ( MeshVisualInfo* visual : activeVisuals ) {
                    for ( const auto& materialMeshes :
                        visual->MeshesByTexture ) {
                        const MeshKey& meshKey = materialMeshes.first;
                        uint64_t sortKeyBase = 0;
                        if ( meshKey.Material ) {
                            if ( zCTexture* texture =
                                    meshKey.Material->GetAniTexture() ) {
                                if ( texture->CacheIn( 0.6f )
                                    != zRES_CACHED_IN ) {
                                    continue;
                                }
                                sortKeyBase =
                                    BuildSortKeyBase( meshKey.Material );
                            }
                        }
                        for ( MeshInfo* mesh : materialMeshes.second ) {
                            if ( !mesh ) continue;
                            meshesToDraw.emplace_back(
                                visual, meshKey, mesh,
                                sortKeyBase + mesh->meshId );
                        }
                    }
                }
                std::sort(
                    meshesToDraw.begin(), meshesToDraw.end(),
                    []( const ShadowVobDraw& left,
                        const ShadowVobDraw& right ) {
                        return std::get<3>( left ) < std::get<3>( right );
                    } );
            } catch ( const std::bad_alloc& ) {
                if ( useWindMetadata ) UnbindWindMetadata();
                LogError() << "Out of memory while building shadow VOB batches.";
                return;
            }

            zCTexture* previousTexture = nullptr;
            MeshVisualInfo* lastWindVisual = nullptr;
            for ( const auto& [visual, meshKey, mesh, sortKey] :
                meshesToDraw ) {
                (void)sortKey;
                if ( !visual || !mesh || visual->Instances.empty() ) {
                    continue;
                }

                if ( !useWindMetadata && windBuffer.GetRawBuffer()
                    && lastWindVisual != visual ) {
                    lastWindVisual = visual;
                    g_windBuffer.minHeight = visual->BBox.Min.y;
                    g_windBuffer.maxHeight = visual->BBox.Max.y;
                    windBuffer.Update( &g_windBuffer );
                }

                zCMaterial* const material = meshKey.Material;
                zCTexture* const texture =
                    material ? material->GetAniTexture() : nullptr;
                const bool bindTexture = texture
                    && (texture->HasAlphaChannel()
                        || colorWritesEnabled
                        || material->HasAlphaTest());
                const bool isAlpha = bindTexture;

                if ( bindTexture ) {
                    if ( texture->CacheIn( 0.6f ) != zRES_CACHED_IN ) {
                        continue;
                    }
                    auto* const surface = texture->GetSurface();
                    auto* const engineTexture =
                        surface ? surface->GetEngineTexture() : nullptr;
                    ID3D11ShaderResourceView* const textureView =
                        engineTexture
                        ? engineTexture->GetShaderResourceView().Get()
                        : nullptr;
                    if ( !textureView ) continue;

                    if ( previousTexture != texture ) {
                        Context->PSSetShaderResources(
                            0, 1, &textureView );
                        previousTexture = texture;
                    }
                    if ( currentPixelShader != defaultPS.get() ) {
                        if ( defaultPS->Apply() != XR_SUCCESS ) {
                            LogError() << "Failed to bind shadow alpha-test shader.";
                            break;
                        }
                        currentPixelShader = defaultPS.get();
                    }
                } else {
                    previousTexture = nullptr;
                    if ( !linearDepth && currentPixelShader ) {
                        Context->PSSetShader( nullptr, nullptr, 0 );
                        currentPixelShader = nullptr;
                    }
                }

                if ( !IsDrawableShadowMesh( mesh, isAlpha ) ) continue;
                D3D11VertexBuffer* const indexBuffer =
                    GetShadowAwareIndexBuffer( mesh, isAlpha );
                UINT indexCount =
                    GetShadowAwareIndexCount( mesh, isAlpha );
                if ( !indexBuffer || indexCount == 0 ) continue;

                const int maxFaces =
                    rendererState.RendererSettings.MaxNumFaces;
                if ( maxFaces > 0 ) {
                    const uint64_t maxIndices =
                        static_cast<uint64_t>(maxFaces) * 3u;
                    indexCount = static_cast<UINT>(
                        (std::min<uint64_t>)(indexCount, maxIndices) );
                }
                if ( indexCount == 0 ) continue;

                const size_t instanceCount64 = visual->Instances.size();
                const size_t startInstance64 = visual->StartInstanceNum;
                if ( instanceCount64 > (std::numeric_limits<UINT>::max)()
                    || startInstance64 > totalInstances
                    || instanceCount64 > totalInstances - startInstance64 ) {
                    LogError() << "Invalid shadow VOB draw range.";
                    continue;
                }
                const UINT instanceCount =
                    static_cast<UINT>(instanceCount64);
                const UINT startInstance =
                    static_cast<UINT>(startInstance64);

                UINT vertexStride = sizeof( ExVertexStruct );
                UINT vertexOffset = 0;
                ID3D11Buffer* vertexBuffer =
                    mesh->MeshVertexBuffer->GetVertexBuffer().Get();
                ID3D11Buffer* rawIndexBuffer =
                    indexBuffer->GetVertexBuffer().Get();
                if ( !vertexBuffer || !rawIndexBuffer ) continue;

                Context->IASetVertexBuffers(
                    0, 1, &vertexBuffer, &vertexStride, &vertexOffset );
                Context->IASetIndexBuffer(
                    rawIndexBuffer, VERTEX_INDEX_DXGI_FORMAT, 0 );
                Context->DrawIndexedInstanced(
                    indexCount, instanceCount, 0, 0, startInstance );

                auto& rendererInfo = rendererState.RendererInfo;
                const uint64_t drawnTriangles =
                    static_cast<uint64_t>(indexCount / 3u)
                    * instanceCount;
                const uint64_t existingTriangles =
                    static_cast<uint64_t>(
                        (std::max)(rendererInfo.FrameDrawnTriangles, 0) );
                rendererInfo.FrameDrawnTriangles =
                    static_cast<int>((std::min<uint64_t>)(
                        existingTriangles + drawnTriangles,
                        static_cast<uint64_t>(
                            (std::numeric_limits<int>::max)()) ));
                if ( rendererInfo.FrameDrawnVobs
                    < (std::numeric_limits<int>::max)() ) {
                    ++rendererInfo.FrameDrawnVobs;
                }
            }

            if ( useWindMetadata ) UnbindWindMetadata();
        };

        drawStaticShadowVobs();
        for ( const auto& pair : staticMeshVisuals ) {
            if ( pair.second ) pair.second->StartNewFrame();
        }
    }






    if ( rendererState.RendererSettings.DrawSkeletalMeshes ) {
        ZoneScopedN( "Shadows::DrawSkeletalMeshes" );
        auto skeletalEvent =
            RecordGraphicsEvent( GE_NAME( "Shadows::DrawSkeletalMeshes" ) );

        const float configuredRadius =
            rendererState.RendererSettings.SkeletalMeshDrawRadius;
        if ( std::isfinite( configuredRadius ) && configuredRadius > 0.0f ) {
            const float safeRadius = (std::min)(
                configuredRadius,
                std::sqrt((std::numeric_limits<float>::max)()) );
            const XMVECTOR radiusSquared =
                XMVectorReplicate( safeRadius * safeRadius );

            static thread_local std::vector<SkeletalVobInfo*>
                animatedSkeletalMeshVobs;
            animatedSkeletalMeshVobs.clear();
            const auto& skeletalVobs = gapi->GetSkeletalMeshVobs();
            if ( animatedSkeletalMeshVobs.capacity() < skeletalVobs.size() ) {
                animatedSkeletalMeshVobs.reserve( skeletalVobs.size() );
            }

            for ( SkeletalVobInfo* skeletalVob : skeletalVobs ) {
                if ( !skeletalVob || !skeletalVob->VisualInfo
                    || !skeletalVob->Vob
                    || !skeletalVob->Vob->GetShowVisual() ) {
                    continue;
                }

                const float transparency =
                    skeletalVob->Vob->GetVobTransparency();
                if ( !std::isfinite( transparency )
                    || (skeletalVob->Vob->GetVisualAlpha()
                        && transparency < 0.7f) ) {
                    continue;
                }

                const XMVECTOR vobPosition =
                    skeletalVob->Vob->GetPositionWorldXM();
                XMFLOAT3 finitePosition{};
                XMStoreFloat3( &finitePosition, vobPosition );
                if ( !std::isfinite( finitePosition.x )
                    || !std::isfinite( finitePosition.y )
                    || !std::isfinite( finitePosition.z )
                    || XMVector3Greater(
                        XMVector3LengthSq( vobPosition - position ),
                        radiusSquared ) ) {
                    continue;
                }
                if ( enableCulling
                    && !currentFrustum->Intersects(
                        skeletalVob->Vob->GetBBox() ) ) {
                    continue;
                }
                animatedSkeletalMeshVobs.push_back( skeletalVob );
            }

            bool drawAttachments = true;
            if ( rendererState.RendererSettings.ShadowFrustumCullingMode
                == GothicRendererSettings::E_ShadowFrustumCulling::
                    SHD_FRUSTUM_CULLING_AGGRESSIVE ) {
                drawAttachments = params.CascadeIndex <= 1;
            }
            if ( !animatedSkeletalMeshVobs.empty() ) {
                DrawSkeletalMeshVobs(
                    animatedSkeletalMeshVobs, FLT_MAX,
                    false, drawAttachments );
            }
        }
    }

    rendererState.BlendState.ColorWritesEnabled = true;
    rendererState.BlendState.SetDirty();
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

bool D3D11GraphicsEngine::UploadAndBindWindMetadata(
    const std::vector<VobWindMetadata>& metadata ) {
    if ( !ActiveVS || metadata.empty()
        || ActiveVS->GetInputIndex( "WindMetaData" ) < 0 ) {
        return false;
    }

    constexpr size_t maxMetadataCount =
        (std::numeric_limits<UINT>::max)() / sizeof( VobWindMetadata );
    if ( metadata.size() > maxMetadataCount ) {
        LogError() << "Wind metadata exceeds D3D11 buffer limits.";
        return false;
    }

    const UINT requiredSize = static_cast<UINT>(
        metadata.size() * sizeof( VobWindMetadata ) );
    if ( !WindMetadataBuffer || !WindMetadataBuffer->IsValid()
        || WindMetadataBuffer->GetSizeInBytes() < requiredSize
        || !WindMetadataBuffer->GetShaderResourceView() ) {
        std::unique_ptr<D3D11VertexBuffer> replacement;
        try {
            replacement = std::make_unique<D3D11VertexBuffer>();
        } catch ( const std::bad_alloc& ) {
            return false;
        }

        if ( replacement->Init(
                nullptr, requiredSize,
                D3D11VertexBuffer::B_SHADER_RESOURCE,
                D3D11VertexBuffer::U_DYNAMIC,
                D3D11VertexBuffer::CA_WRITE,
                "WindMetadataBuffer",
                sizeof( VobWindMetadata ) ) != XR_SUCCESS
            || !replacement->IsValid()
            || !replacement->GetShaderResourceView() ) {
            return false;
        }

        SetDebugName(
            replacement->GetShaderResourceView().Get(),
            "WindMetadataBuffer->ShaderResourceView" );
        SetDebugName(
            replacement->GetVertexBuffer().Get(),
            "WindMetadataBuffer->Buffer" );
        WindMetadataBuffer = std::move( replacement );
    }

    if ( WindMetadataBuffer->UpdateBuffer(
            metadata.data(), requiredSize ) != XR_SUCCESS ) {
        return false;
    }

    ActiveVS->BindResource(
        "WindMetaData",
        WindMetadataBuffer->GetShaderResourceView().Get() );
    return true;
}

bool D3D11GraphicsEngine::PrepareAndBindWindMetadata(
    const std::vector<MeshVisualInfo*>& activeVisuals ) {
    if ( !ActiveVS || activeVisuals.empty()
        || ActiveVS->GetInputIndex( "WindMetaData" ) < 0 ) {
        return false;
    }

    constexpr size_t maxMetadataCount = (std::min<size_t>)(
        (std::numeric_limits<DWORD>::max)(),
        (std::numeric_limits<UINT>::max)()
            / sizeof( VobWindMetadata ) );
    if ( activeVisuals.size() > maxMetadataCount ) {
        LogError() << "Wind metadata exceeds D3D11 buffer limits.";
        return false;
    }

    m_WindMetadataStaging.clear();
    try {
        m_WindMetadataStaging.reserve( activeVisuals.size() );
        for ( MeshVisualInfo* visual : activeVisuals ) {
            if ( !visual ) continue;

            VobWindMetadata metadata{};
            metadata.MinHeight = std::isfinite( visual->BBox.Min.y )
                ? visual->BBox.Min.y : 0.0f;
            metadata.MaxHeight = std::isfinite( visual->BBox.Max.y )
                ? visual->BBox.Max.y : metadata.MinHeight;
            if ( metadata.MaxHeight < metadata.MinHeight ) {
                std::swap( metadata.MinHeight, metadata.MaxHeight );
            }
            m_WindMetadataStaging.push_back( metadata );
        }
    } catch ( const std::bad_alloc& ) {
        m_WindMetadataStaging.clear();
        return false;
    }

    if ( !UploadAndBindWindMetadata( m_WindMetadataStaging ) ) {
        return false;
    }

    DWORD metadataIndex = 0;
    for ( MeshVisualInfo* visual : activeVisuals ) {
        if ( !visual ) continue;
        for ( auto& instance : visual->Instances ) {
            instance.GP_Slot = metadataIndex;
        }
        ++metadataIndex;
    }
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
    windBuff.characterInteractionStrength =
        settings.WindQuality != GothicRendererSettings::WIND_QUALITY_NONE
        ? 1.0f : 0.0f;

    //LogInfo() << windBuff.windDir.x << " " << windBuff.windDir.y << " " << windBuff.windDir.z;

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
    if ( !Engine::GAPI || !Context || !ShaderManager
        || !PerObjectMaterialInfoPooledBuffer || !ActiveSceneRenderer ) {
        return XR_FAILED;
    }

    static std::vector<VobInfo*> vobs;
    static std::vector<SkeletalVobInfo*> mobs;

    const auto& renderSettings =
        Engine::GAPI->GetRendererState().RendererSettings;
    bool wireframeStateChanged = false;
    auto failDraw = [&]() {
        UnbindWindMetadata();
        for ( const auto& alphaMesh : m_AlphaMeshes ) {
            if ( alphaMesh.vi ) alphaMesh.vi->StartNewFrame();
        }
        m_AlphaMeshes.clear();
        if ( wireframeStateChanged ) {
            Engine::GAPI->GetRendererState().RasterizerState.Wireframe = false;
        }
        if ( !renderSettings.FixViewFrustum ) {
            vobs.clear();
            mobs.clear();
        }
        return XR_FAILED;
    };

    {
        TracyD3D11ZoneCGX( "DrawVOBsInstanced" );
        auto _scopeDrawVOBsInstanced = RecordGraphicsEvent( GE_NAME( "DrawVOBsInstanced" ) );
        SetDefaultStates();

        if ( SetActivePixelShader( PShaderID::PS_Diffuse ) != XR_SUCCESS
            || SetActiveVertexShader( VShaderID::VS_ExInstancedObj )
                != XR_SUCCESS ) {
            LogError() << "Required instanced VOB shaders are unavailable.";
            return failDraw();
        }

        auto ffPipelineBuffer =
            ActivePS->GetBuffer( "FFPipelineConstantBuffer" );
        if ( !ffPipelineBuffer
                .Update( &Engine::GAPI->GetRendererState().GraphicsState )
                .Bind()
                .Succeeded() ) {
            LogError() << "Failed to bind the VOB pipeline constant buffer.";
            return failDraw();
        }

        if ( GSky* sky = Engine::GAPI->GetSky() ) {
            auto atmosphereBuffer = ActivePS->GetBuffer( "Atmosphere" );
            if ( !atmosphereBuffer
                    .Update( &sky->GetAtmosphereCB() )
                    .Bind()
                    .Succeeded() ) {
                LogError() << "Failed to bind the VOB atmosphere buffer.";
                return failDraw();
            }
        }

        MaterialInfo defInfo = {};
        auto defaultMaterialBuffer =
            ActivePS->GetBuffer( "MI_MaterialInfo" );
        if ( !defaultMaterialBuffer.GetRawBuffer() ) {
            LogError() << "VOB material constant buffer is unavailable.";
            return failDraw();
        }
        const UINT materialInfoSlot = defaultMaterialBuffer.GetSlot();
        auto defaultMaterialAllocation =
            PerObjectMaterialInfoPooledBuffer->Allocate(
                Context.Get(), &defInfo.buffer, sizeof( defInfo.buffer ) );
        if ( defaultMaterialAllocation ) {
            const UINT firstConstant =
                defaultMaterialAllocation.offsetInBytes / 16;
            const UINT numConstants =
                defaultMaterialAllocation.sizeInBytes / 16;
            Context->PSSetConstantBuffers1(
                materialInfoSlot, 1, &defaultMaterialAllocation.pBuffer,
                &firstConstant, &numConstants );
        } else if ( !defaultMaterialBuffer
                        .Update( &defInfo.buffer, sizeof( defInfo.buffer ) )
                        .Bind()
                        .Succeeded() ) {
            LogError() << "Failed to bind the default VOB material.";
            return failDraw();
        }

        XMMATRIX view = Engine::GAPI->GetViewMatrixXM();
        Engine::GAPI->SetViewTransformXM( view );

        if ( renderSettings.WireframeVobs ) {
            Engine::GAPI->GetRendererState().RasterizerState.Wireframe = true;
            wireframeStateChanged = true;
        }

        // Init drawcalls
        SetupVS_ExMeshDrawCall();
        SetupVS_ExConstantBuffer();

        GraphicsShaderConstantBuffer windBuffer = {};
        if ( ActiveVS &&
            Engine::GAPI->GetRendererState().RendererSettings.WindQuality > 0 ) {
            windBuffer = ActiveVS->GetBuffer( "WindParams" );
            windBuffer.Bind();
        }

        const auto DIST_DistanceSlot =
            ActivePS->GetInputIndex( "DIST_Distance" );
        if ( DIST_DistanceSlot != -1
            && (!OutdoorSmallVobsConstantBuffer
                || !OutdoorSmallVobsConstantBuffer->IsValid()
                || !OutdoorVobsConstantBuffer
                || !OutdoorVobsConstantBuffer->IsValid()) ) {
            LogError() << "VOB distance buffers are unavailable.";
            return failDraw();
        }

        bool isZPrepass = RenderingStage == D3D11ENGINE_RENDER_STAGE::DES_Z_PRE_PASS;

        if ( !isZPrepass ) {
            m_AlphaMeshes.clear();
            m_AlphaMeshes.reserve( 64 );
        }

        if ( isZPrepass ) {
            Context->PSSetShader( nullptr, nullptr, 0 );
        }

        if ( renderSettings.DrawVOBs ||
            renderSettings.EnableDynamicLighting ) {
            if ( !m_FrameGeometryCache.vobInstancesUploaded ) {
                if ( !renderSettings.FixViewFrustum ||
                    (renderSettings.FixViewFrustum &&
                        vobs.empty()) ) {
                    m_FrameLights.clear();

                    UINT collect = EBspTreeCollectFlags::COLLECT_ALL_MUTATE;
                    if ( isZPrepass ) {
                        // collect &= ~(EBspTreeCollectFlags::COLLECT_LIGHTS);
                    }

                    ZoneScopedN( "DrawVOBsInstanced::CollectVisibleVobs" );
                    auto _scopeCollectVisibleVobs = RecordGraphicsEvent( GE_NAME( "DrawVOBsInstanced::CollectVisibleVobs" ) );
                    Engine::GAPI->CollectVisibleVobs( vobs, m_FrameLights, mobs, EGothicCullFlags::CullAll, (EBspTreeCollectFlags)collect );
                }
                // Snapshot mobs into cache; the static mobs vector is cleared at
                // end of this function, so the lit pass would find it empty otherwise.
                m_FrameGeometryCache.cachedMobs = mobs;
            }
        }

        if ( renderSettings.DrawVOBs ) {
            auto _1 = RecordGraphicsEvent( GE_NAME( "DrawVOBsInstanced->DrawVOBs" ) );
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
                    if ( pair.second && !pair.second->Instances.empty() ) {
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
                if ( activeVisuals.size()
                    > (std::numeric_limits<unsigned int>::max)() ) {
                    LogError() << "Too many active VOB visuals for one frame.";
                    return failDraw();
                }

                constexpr size_t maxInstanceCount =
                    (std::numeric_limits<unsigned int>::max)()
                    / sizeof( VobInstanceInfo );
                size_t totalInstances = 0;
                try {
                    cache.vobVisuals.reserve( activeVisuals.size() );
                    for ( MeshVisualInfo* visual : activeVisuals ) {
                        if ( !visual || visual->Instances.empty() ) continue;
                        if ( visual->Instances.size()
                            > maxInstanceCount - totalInstances ) {
                            LogError()
                                << "VOB instance data exceeds D3D11 limits.";
                            cache.vobVisuals.clear();
                            return failDraw();
                        }

                        FrameGeometryCache::CachedVobVisual cachedVisual;
                        cachedVisual.Visual = visual;
                        cachedVisual.StartInstanceNum =
                            static_cast<unsigned int>(totalInstances);
                        cachedVisual.Instances = visual->Instances;
                        totalInstances += cachedVisual.Instances.size();
                        cache.vobVisuals.push_back(
                            std::move( cachedVisual ) );
                    }
                } catch ( const std::bad_alloc& ) {
                    cache.vobVisuals.clear();
                    LogError() << "Failed to allocate the VOB instance cache.";
                    return failDraw();
                }

                const unsigned int requiredBytes =
                    static_cast<unsigned int>(
                        (std::max<size_t>)(totalInstances, 1u)
                        * sizeof( VobInstanceInfo ) );
                instancingBuffer = AcquireFrameInstancingBuffer(
                    m_MainVobInstancingPool, requiredBytes,
                    "MainVobInstancingBuffer" );
                if ( !instancingBuffer || !instancingBuffer->IsValid()
                    || instancingBuffer->GetSizeInBytes() < requiredBytes ) {
                    LogError()
                        << "Failed to acquire main VOB instancing buffer.";
                    return failDraw();
                }

                byte* mappedData = nullptr;
                UINT mappedSize = 0;
                const XRESULT mapResult = instancingBuffer->Map(
                    D3D11VertexBuffer::M_WRITE_DISCARD,
                    reinterpret_cast<void**>(&mappedData), &mappedSize );
                if ( mapResult != XR_SUCCESS || !mappedData
                    || mappedSize < requiredBytes ) {
                    if ( mapResult == XR_SUCCESS ) {
                        instancingBuffer->Unmap();
                    }
                    LogError()
                        << "Failed to map the VOB instancing buffer.";
                    return failDraw();
                }

                if ( totalInstances == 0 ) {
                    memset( mappedData, 0, sizeof( VobInstanceInfo ) );
                } else {
                    for ( const auto& cachedVisual : cache.vobVisuals ) {
                        const size_t copySize =
                            cachedVisual.Instances.size()
                            * sizeof( VobInstanceInfo );
                        const size_t copyOffset =
                            static_cast<size_t>(
                                cachedVisual.StartInstanceNum )
                            * sizeof( VobInstanceInfo );
                        memcpy(
                            mappedData + copyOffset,
                            cachedVisual.Instances.data(), copySize );
                    }
                }

                if ( instancingBuffer->Unmap() != XR_SUCCESS ) {
                    LogError()
                        << "Failed to unmap the VOB instancing buffer.";
                    return failDraw();
                }
                cache.MainVobInstancingBuffer = instancingBuffer;

                cache.sortedInstancedMeshes.clear();
                size_t numMeshesToDraw = 0;
                for ( const auto& cachedVisual : cache.vobVisuals ) {
                    if ( !cachedVisual.Visual ) continue;
                    for ( const auto& meshes :
                        cachedVisual.Visual->MeshesByTexture ) {
                        if ( meshes.second.size()
                            > cache.sortedInstancedMeshes.max_size()
                                - numMeshesToDraw ) {
                            LogError()
                                << "VOB mesh cache exceeds container limits.";
                            return failDraw();
                        }
                        numMeshesToDraw += meshes.second.size();
                    }
                }

                try {
                    cache.sortedInstancedMeshes.reserve(
                        numMeshesToDraw );
                    for ( size_t visualIndex = 0;
                        visualIndex < cache.vobVisuals.size();
                        ++visualIndex ) {
                        const auto& cachedVisual =
                            cache.vobVisuals[visualIndex];
                        if ( !cachedVisual.Visual ) continue;

                        for ( const auto& meshes :
                            cachedVisual.Visual->MeshesByTexture ) {
                            const std::vector<MeshInfo*>& meshList =
                                meshes.second;
                            if ( meshList.empty() ) continue;

                            FrameGeometryCache::SortKeyBuilder
                                sortKeyBase{ 0 };
                            if ( meshes.first.Material ) {
                                const auto alphaFunc =
                                    meshes.first.Material->GetAlphaFunc();
                                if ( alphaFunc > zMAT_ALPHA_FUNC_NONE
                                    && alphaFunc
                                        != zMAT_ALPHA_FUNC_TEST ) {
                                    sortKeyBase.withAlphaType( 2 );
                                } else if (
                                    alphaFunc
                                        == zMAT_ALPHA_FUNC_TEST ) {
                                    sortKeyBase.withAlphaType( 1 );
                                }
                            }
                            if ( meshes.first.Texture ) {
                                if ( meshes.first.Texture
                                        ->HasAlphaChannel()
                                    && sortKeyBase.GetAlphaType() == 0 ) {
                                    sortKeyBase.withAlphaType( 1 );
                                }
                                sortKeyBase.withTexture(
                                    reinterpret_cast<size_t>(
                                        meshes.first.Texture ) );
                            }

                            for ( MeshInfo* meshInfo : meshList ) {
                                if ( !meshInfo ) continue;

                                auto meshSortKey = sortKeyBase;
                                meshSortKey.withMesh(
                                    meshInfo->meshId );

                                FrameGeometryCache::
                                    CachedInstancedMeshDraw drawItem;
                                drawItem.VisualIndex =
                                    static_cast<unsigned int>(
                                        visualIndex );
                                drawItem.Mesh = meshes.first;
                                drawItem.MeshEntry = meshInfo;
                                drawItem.sortKey = meshSortKey;
                                cache.sortedInstancedMeshes.push_back(
                                    drawItem );
                            }
                        }
                    }
                } catch ( const std::bad_alloc& ) {
                    cache.sortedInstancedMeshes.clear();
                    LogError()
                        << "Failed to allocate the VOB mesh cache.";
                    return failDraw();
                }

                std::sort( cache.sortedInstancedMeshes.begin(), cache.sortedInstancedMeshes.end(),
                    []( const FrameGeometryCache::CachedInstancedMeshDraw& a, const FrameGeometryCache::CachedInstancedMeshDraw& b ) {
                        return a.sortKey < b.sortKey;
                    } );

                if ( !vobs.empty() ) {
                    try {
                        RenderedVobs.insert(
                            RenderedVobs.end(), vobs.begin(), vobs.end() );
                    } catch ( const std::bad_alloc& ) {
                        LogError()
                            << "Failed to grow the rendered VOB list.";
                        return failDraw();
                    }
                }

                cache.vobInstancesUploaded = true;
            } else {
                instancingBuffer = cache.MainVobInstancingBuffer;
            }

            if ( !instancingBuffer || !instancingBuffer->IsValid() ) {
                LogError()
                    << "Missing main VOB instancing buffer in cache.";
                return failDraw();
            }

            const bool useWindMetadata =
                cache.vobWindMetadataPrepared
                && UploadAndBindWindMetadata(
                    cache.vobWindMetadata );
            if ( !useWindMetadata ) {
                UnbindWindMetadata();
            }

            if ( windBuffer.GetRawBuffer() ) {
                UpdateCharacterInteractionPositions( g_windBuffer );
                windBuffer.Update( &g_windBuffer );
            }

            float cachedSmallVobRadius = -1.0f;
            float cachedVobRadius = -1.0f;
            auto alphaTestPipelineShader =
                ShaderManager->GetPShader(
                    Resolved_DiffuseNormalmappedAlphatest );
            if ( !alphaTestPipelineShader
                || !alphaTestPipelineShader
                        ->GetBuffer( "FFPipelineConstantBuffer" )
                        .Update(
                            &Engine::GAPI->GetRendererState()
                                .GraphicsState )
                        .Bind()
                        .Succeeded() ) {
                LogError()
                    << "VOB alpha-test pipeline is unavailable.";
                return failDraw();
            }

            if ( isZPrepass ) {
                if ( SetActivePixelShader(
                        PShaderID::PS_DiffuseAlphaTestShadows )
                    != XR_SUCCESS ) {
                    LogError()
                        << "VOB depth prepass shader is unavailable.";
                    return failDraw();
                }
                Context->PSSetShader( nullptr, nullptr, 0 );
            }

            MaterialInfo* lastMatInfo = nullptr;
            float lastMaterialClassMarker = 999.0f;

            zCTexture* lastTex = nullptr;
            ID3D11ShaderResourceView* lastNrmTex = nullptr;
            ID3D11ShaderResourceView* lastFxTex = nullptr;
            ID3D11ShaderResourceView* lastDispTex = nullptr;
            MeshVisualInfo* lastWindVisual = nullptr;

            if ( !cache.sortedInstancedMeshes.empty() ) {
                TracyD3D11ZoneCGX( "DrawVOBsInstanced::OpaqueSubmission" );
                auto _scopeOpaqueSubmission = RecordGraphicsEvent( GE_NAME( "DrawVOBsInstanced::OpaqueSubmission" ) );
                for ( const auto& drawItem :
                    cache.sortedInstancedMeshes ) {
                    if ( drawItem.VisualIndex
                            >= cache.vobVisuals.size()
                        || !drawItem.MeshEntry ) {
                        continue;
                    }

                    const auto* cachedVisual =
                        &cache.vobVisuals[drawItem.VisualIndex];
                    MeshInfo* meshInfo = drawItem.MeshEntry;
                    if ( !cachedVisual->Visual
                        || cachedVisual->Instances.empty()
                        || cachedVisual->Instances.size()
                            > (std::numeric_limits<unsigned int>::max)()
                        || meshInfo->Indices.empty()
                        || meshInfo->Indices.size()
                            > (std::numeric_limits<unsigned int>::max)() ) {
                        continue;
                    }

                    const MeshKey& meshKey = drawItem.Mesh;
                    const float materialClassMarker =
                        IsTwoSidedBacklitVegetationVisual(
                            cachedVisual->Visual->VisualName )
                        ? -2.0f : 0.0f;
                    const bool isAlphaBlendMesh =
                        meshKey.Material
                        && (meshKey.Material->GetAlphaFunc()
                                == zMAT_ALPHA_FUNC_BLEND
                            || meshKey.Material->GetAlphaFunc()
                                == zMAT_ALPHA_FUNC_ADD);

                    if ( isAlphaBlendMesh ) {
                        if ( !isZPrepass ) {
                            try {
                                AlphaMeshData alphaMesh;
                                alphaMesh.mk = meshKey;
                                alphaMesh.mi = meshInfo;
                                alphaMesh.vi = cachedVisual->Visual;
                                alphaMesh.StartInstanceNum =
                                    cachedVisual->StartInstanceNum;
                                alphaMesh.instances =
                                    cachedVisual->Instances;
                                m_AlphaMeshes.push_back(
                                    std::move( alphaMesh ) );
                            } catch ( const std::bad_alloc& ) {
                                LogError()
                                    << "Failed to cache alpha VOB instances.";
                                return failDraw();
                            }
                        }
                        continue;
                    }

                    const float meshSize =
                        cachedVisual->Visual->MeshSize;
                    if ( !std::isfinite( meshSize ) ) continue;
                    const float expectedSmallRadius =
                        renderSettings.OutdoorSmallVobDrawRadius - meshSize;
                    const float expectedVobRadius =
                        renderSettings.OutdoorVobDrawRadius - meshSize;
                    if ( DIST_DistanceSlot != -1
                        && (!std::isfinite( expectedSmallRadius )
                            || !std::isfinite( expectedVobRadius )) ) {
                        continue;
                    }

                    if ( DIST_DistanceSlot != -1 ) {
                        if ( meshSize < renderSettings.SmallVobSize ) {
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

                    if ( !useWindMetadata
                        && windBuffer.GetRawBuffer()
                        && lastWindVisual != cachedVisual->Visual ) {
                        lastWindVisual = cachedVisual->Visual;
                        g_windBuffer.minHeight = std::isfinite(
                            cachedVisual->Visual->BBox.Min.y )
                            ? cachedVisual->Visual->BBox.Min.y : 0.0f;
                        g_windBuffer.maxHeight = std::isfinite(
                            cachedVisual->Visual->BBox.Max.y )
                            ? cachedVisual->Visual->BBox.Max.y
                            : g_windBuffer.minHeight;
                        if ( g_windBuffer.maxHeight
                            < g_windBuffer.minHeight ) {
                            std::swap(
                                g_windBuffer.minHeight,
                                g_windBuffer.maxHeight );
                        }
                        windBuffer.Update( &g_windBuffer );
                    }

                    zCTexture* tx = meshKey.Material ? meshKey.Material->GetAniTexture() : nullptr;

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
                        // Previously this forced alpha testing, now we need to check material flags as well for that and only enable the shader if absolutely necessery
                        const bool wantShader = !isZPrepass || (tx->HasAlphaChannel() || meshKey.Material->HasAlphaTest());

                        MyDirectDrawSurface7* surface =
                            tx->GetSurface();
                        D3D11Texture* engineTexture =
                            surface ? surface->GetEngineTexture() : nullptr;
                        if ( !surface || !engineTexture ) continue;

                        ID3D11ShaderResourceView* srv[4] = {};
                        MaterialInfo* info = meshKey.Info;

                        srv[0] =
                            engineTexture->GetShaderResourceView().Get();
                        if ( !srv[0] ) continue;
                        srv[1] = surface->GetNormalmap()
                            ? surface->GetNormalmap()->GetShaderResourceView().Get()
                            : nullptr;
                        srv[2] = surface->GetFxMap()
                            ? surface->GetFxMap()->GetShaderResourceView().Get()
                            : nullptr;
                        srv[3] = GetParallaxDisplacementSRV( surface );
                        if ( !srv[1] && ( wantShader && !isZPrepass ) ) {
                            if ( ID3D11ShaderResourceView* wetFallback = GetWetNormalFallbackSRV( surface, DistortionTexture.get() ) ) {
                                if ( info && info->buffer.NormalmapStrength != DEFAULT_NORMALMAP_STRENGTH ) {
                                    info->buffer.NormalmapStrength = DEFAULT_NORMALMAP_STRENGTH;
                                    lastMatInfo = info;
                                }
                                srv[1] = wetFallback;
                            }
                        }

                        const bool materialMarkerChanged = materialClassMarker != lastMaterialClassMarker;
                        // Wet scenes can use the distortion texture as a temporary normalmap fallback.
                        if ( lastTex != tx
                            || lastNrmTex != srv[1]
                            || lastFxTex != srv[2]
                            || lastDispTex != srv[3]
                            || materialMarkerChanged ) {

                            lastTex = tx;
                            lastNrmTex = srv[1];
                            lastFxTex = srv[2];
                            lastDispTex = srv[3];
                            lastMaterialClassMarker = materialClassMarker;

                            if ( wantShader ) {
                                GetContext()->PSSetShaderResources( 0, isZPrepass ? 1 : 3, srv );
                                if ( !isZPrepass ) {
                                    GetContext()->PSSetShaderResources( 13, 1, &srv[3] );
                                }

                                if ( BindShaderForTexture( tx,
                                    tx->HasAlphaChannel()
                                    || meshKey.Material->HasAlphaTest()
                                    , meshKey.Material->GetAlphaFunc(),
                                    info ? info->MaterialType
                                         : MaterialInfo::MT_None ) ) {

                                    PsSimpleFFdata ffdata = {};
                                    ffdata.textureFactor =
                                        float4( 1.0f, 1.0f, 1.0f, 1.0f );
                                    if ( !ActivePS
                                        || !ActivePS
                                                ->GetBuffer( "cbFFData" )
                                                .Update( &ffdata )
                                                .Bind()
                                                .Succeeded() ) {
                                        LogError()
                                            << "Failed to bind VOB shader data.";
                                        return failDraw();
                                    }
                                }

                                if ( materialMarkerChanged || (info && !info->IsSame( lastMatInfo )) ) {
                                    auto materialBuffer = GetEffectiveMaterialBuffer( info, tx->GetSurface() );
                                    if ( materialClassMarker != 0.0f ) {
                                        materialBuffer.Color.w = materialClassMarker;
                                    }
                                    auto matAllocation = PerObjectMaterialInfoPooledBuffer->Allocate(
                                        GetContext().Get(), &materialBuffer, sizeof( materialBuffer ) );
                                    if ( matAllocation ) {
                                        UINT firstConstant = matAllocation.offsetInBytes / 16;
                                        UINT numConstants = matAllocation.sizeInBytes / 16;
                                        GetContext()->PSSetConstantBuffers1(
                                            materialInfoSlot, 1, &matAllocation.pBuffer, &firstConstant, &numConstants );
                                    } else if ( !ActivePS
                                        || !ActivePS
                                                ->GetBuffer(
                                                    "MI_MaterialInfo" )
                                                .Update(
                                                    &materialBuffer,
                                                    sizeof(
                                                        materialBuffer ) )
                                                .Bind()
                                                .Succeeded() ) {
                                        LogError()
                                            << "Failed to bind VOB material data.";
                                        return failDraw();
                                    }

                                    lastMatInfo = info;
                                }
                            }
                        }
                    }

                    DrawInstanced(
                        meshInfo->MeshVertexBuffer,
                        meshInfo->MeshIndexBuffer,
                        static_cast<unsigned int>(
                            meshInfo->Indices.size() ),
                        instancingBuffer,
                        sizeof( VobInstanceInfo ),
                        static_cast<unsigned int>(
                            cachedVisual->Instances.size() ),
                        sizeof( ExVertexStruct ),
                        cachedVisual->StartInstanceNum );
                }
            }
            if ( !isZPrepass ) {
                for ( const auto& cv : cache.vobVisuals ) {
                    if ( !cv.Visual ) continue;
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
                        cv.Visual->Instances.clear();
                    }
                }
            }
            if ( useWindMetadata ) {
                UnbindWindMetadata();
            }
        }

        // Draw mobs
        if ( renderSettings.DrawMobs ) {
            TracyD3D11ZoneCGX( "DrawVOBsInstanced->MOBs" );
            auto _1 = RecordGraphicsEvent( GE_NAME( "DrawVOBsInstanced->DrawMobs" ) );

            // Mobs use zengine functions for binding textures so let's reset zengine texture state
            Engine::GAPI->ResetRenderStates();

            static std::vector<XMFLOAT4X4> bones = {};

            DrawSkeletalMeshVobs( m_FrameGeometryCache.cachedMobs, FLT_MAX, true, true );
            for ( SkeletalVobInfo* mob :
                m_FrameGeometryCache.cachedMobs ) {
                if ( !mob || !mob->Vob ) continue;
                zCModel* model = static_cast<zCModel*>(
                    mob->Vob->GetVisual() );
                if ( !model ) continue;

                const XMMATRIX scale = XMMatrixScalingFromVector(
                    model->GetModelScaleXM() );
                const XMMATRIX world =
                    mob->Vob->GetWorldMatrixXM() * scale;
                XMStoreFloat4x4( &mob->PrevWorldMatrix, world );

                bones.clear();
                model->GetBoneTransforms( &bones );
                mob->StorePreviousTransforms( bones );
            }
        }

        GetContext()->IASetPrimitiveTopology( D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST );

        if ( wireframeStateChanged ) {
            Engine::GAPI->GetRendererState().RasterizerState.Wireframe = false;
            wireframeStateChanged = false;
        }
    }

    if ( !renderSettings.FixViewFrustum ) {
        vobs.clear();
        mobs.clear();
    }
    return XR_SUCCESS;
}

/** Draws the static VOBs */
XRESULT D3D11GraphicsEngine::DrawFrameAlphaMeshes()
{
    if ( m_AlphaMeshes.empty() ) {
        return XR_SUCCESS;
    }

    auto resetAlphaMeshes = [&]() {
        for ( const auto& alphaMesh : m_AlphaMeshes ) {
            if ( alphaMesh.vi ) alphaMesh.vi->StartNewFrame();
        }
        m_AlphaMeshes.clear();
    };
    auto failAlphaDraw = [&]() {
        UnbindWindMetadata();
        resetAlphaMeshes();
        return XR_FAILED;
    };

    if ( !Engine::GAPI || !Context || !ShaderManager
        || !ActiveSceneRenderer || !HDRBackBuffer
        || !HDRBackBuffer->GetRenderTargetView()
        || !DepthStencilBuffer
        || !DepthStencilBuffer->GetDepthStencilView() ) {
        return failAlphaDraw();
    }

    TracyD3D11ZoneCGX( "DrawFrameAlphaMeshes" );
    auto _scopeDrawFrameAlphaMeshes =
        RecordGraphicsEvent( GE_NAME( "DrawFrameAlphaMeshes" ) );

    SetDefaultStates();
    if ( SetActivePixelShader( PShaderID::PS_Simple ) != XR_SUCCESS
        || SetActiveVertexShader( VShaderID::VS_ExInstancedObj )
            != XR_SUCCESS ) {
        LogError() << "Required alpha VOB shaders are unavailable.";
        return failAlphaDraw();
    }

    SetupVS_ExMeshDrawCall();
    SetupVS_ExConstantBuffer();

    Context->OMSetRenderTargets(
        1, HDRBackBuffer->GetRenderTargetView().GetAddressOf(),
        DepthStencilBuffer->GetDepthStencilView().Get() );

    const bool useWindMetadata =
        m_FrameGeometryCache.vobWindMetadataPrepared
        && UploadAndBindWindMetadata(
            m_FrameGeometryCache.vobWindMetadata );
    if ( !useWindMetadata ) {
        UnbindWindMetadata();
    }

    D3D11VertexBuffer* instancingBuffer =
        m_FrameGeometryCache.MainVobInstancingBuffer;
    if ( !instancingBuffer || !instancingBuffer->IsValid() ) {
        LogError()
            << "Missing main VOB instancing buffer for alpha rendering.";
        return failAlphaDraw();
    }

    GraphicsShaderConstantBuffer windBuffer = {};
    const auto& rendererSettings =
        Engine::GAPI->GetRendererState().RendererSettings;
    if ( ActiveVS
        && rendererSettings.WindQuality > 0 ) {
        windBuffer = ActiveVS->GetBuffer( "WindParams" );
        UpdateCharacterInteractionPositions( g_windBuffer );
        if ( !windBuffer.Bind().Update( &g_windBuffer ).Succeeded() ) {
            windBuffer = GraphicsShaderConstantBuffer{};
        }
    }

    int activeBlendMode = 0;
    {
        TracyD3D11ZoneCGX( "DrawFrameAlphaMeshes::Replay" );
        auto _scopeAlphaReplay =
            RecordGraphicsEvent(
                GE_NAME( "DrawFrameAlphaMeshes::Replay" ) );

        for ( const auto& alphaMesh : m_AlphaMeshes ) {
            const MeshKey& meshKey = alphaMesh.mk;
            MeshInfo* meshInfo = alphaMesh.mi;
            MeshVisualInfo* visual = alphaMesh.vi;
            const auto& instances = alphaMesh.instances;
            if ( !meshKey.Material || !meshInfo || !visual
                || instances.empty()
                || instances.size()
                    > (std::numeric_limits<unsigned int>::max)()
                || meshInfo->Indices.empty()
                || meshInfo->Indices.size()
                    > (std::numeric_limits<unsigned int>::max)() ) {
                continue;
            }

            zCTexture* texture =
                meshKey.Material->GetAniTexture();
            if ( !texture
                || texture->CacheIn( 0.6f ) != zRES_CACHED_IN ) {
                continue;
            }

            MyDirectDrawSurface7* surface = texture->GetSurface();
            D3D11Texture* engineTexture =
                surface ? surface->GetEngineTexture() : nullptr;
            if ( !surface || !engineTexture ) continue;

            ID3D11ShaderResourceView* resources[4] = {};
            resources[0] =
                engineTexture->GetShaderResourceView().Get();
            if ( !resources[0] ) continue;
            resources[1] = surface->GetNormalmap()
                ? surface->GetNormalmap()
                    ->GetShaderResourceView().Get()
                : nullptr;
            resources[2] = surface->GetFxMap()
                ? surface->GetFxMap()
                    ->GetShaderResourceView().Get()
                : nullptr;
            resources[3] =
                GetParallaxDisplacementSRV( surface );
            if ( !resources[1] ) {
                resources[1] = GetWetNormalFallbackSRV(
                    surface, DistortionTexture.get() );
            }

            Context->PSSetShaderResources( 0, 3, resources );
            Context->PSSetShaderResources(
                13, 1, &resources[3] );

            const bool blendAdd =
                meshKey.Material->GetAlphaFunc()
                == zMAT_ALPHA_FUNC_ADD;
            const bool blendAlpha =
                meshKey.Material->GetAlphaFunc()
                == zMAT_ALPHA_FUNC_BLEND;
            if ( !blendAdd && !blendAlpha ) continue;

            const int requestedBlendMode = blendAdd ? 1 : 2;
            if ( requestedBlendMode != activeBlendMode ) {
                auto& rendererState =
                    Engine::GAPI->GetRendererState();
                if ( blendAdd ) {
                    rendererState.BlendState
                        .SetAdditiveBlending();
                } else {
                    rendererState.BlendState
                        .SetAlphaBlending();
                }
                rendererState.BlendState.SetDirty();
                rendererState.DepthState.DepthWriteEnabled = false;
                rendererState.DepthState.SetDirty();
                UpdateRenderStates();
                activeBlendMode = requestedBlendMode;
            }

            g_windBuffer.minHeight = std::isfinite(
                visual->BBox.Min.y )
                ? visual->BBox.Min.y : 0.0f;
            g_windBuffer.maxHeight = std::isfinite(
                visual->BBox.Max.y )
                ? visual->BBox.Max.y : g_windBuffer.minHeight;
            if ( g_windBuffer.maxHeight
                < g_windBuffer.minHeight ) {
                std::swap(
                    g_windBuffer.minHeight,
                    g_windBuffer.maxHeight );
            }
            if ( !useWindMetadata
                && windBuffer.GetRawBuffer() ) {
                windBuffer.Update( &g_windBuffer );
            }

            DrawInstanced(
                meshInfo->MeshVertexBuffer,
                meshInfo->MeshIndexBuffer,
                static_cast<unsigned int>(
                    meshInfo->Indices.size() ),
                instancingBuffer, sizeof( VobInstanceInfo ),
                static_cast<unsigned int>(instances.size()),
                sizeof( ExVertexStruct ),
                alphaMesh.StartInstanceNum );
        }
    }

    UnbindWindMetadata();
    if ( activeBlendMode != 0 ) {
        SetDefaultStates();
    }
    resetAlphaMeshes();
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

        //Setting world transform matrix/////////////

        //vob->GetWorldMatrix(&id);
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
            srv[1] = surface->GetNormalmap() ? surface->GetNormalmap()->GetShaderResourceView().Get() : NULL;
            srv[2] = surface->GetFxMap() ? surface->GetFxMap()->GetShaderResourceView().Get() : NULL;
            srv[3] = GetParallaxDisplacementSRV( surface );
            if ( !srv[1] ) {
                srv[1] = GetWetNormalFallbackSRV( surface, DistortionTexture.get() );
            }

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

            MaterialInfo* info = Engine::GAPI->GetMaterialInfoFrom( tx );
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
        FFRasterizerState.Reset();
        FFBlendState.Reset();
        FFDepthStencilState.Reset();
        if ( UpdateRenderStates() != XR_SUCCESS ) {
            LogError() << "Failed to bind the default render states.";
        }
    }
}

/** Draws the sky using the GSky-Object */
XRESULT D3D11GraphicsEngine::DrawSky() {
    if ( !Engine::GAPI || !Context || !ShaderManager ) {
        return XR_FAILED;
    }

    GSky* sky = Engine::GAPI->GetSky();
    if ( !sky || sky->RenderSky() != XR_SUCCESS ) {
        return XR_FAILED;
    }

    auto& rendererState = Engine::GAPI->GetRendererState();
    if ( !rendererState.RendererSettings.AtmosphericScattering ) {
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
        reinterpret_cast<void( __fastcall* )(zCSkyController_Outdoor*)>(0x5C0900)(Engine::GAPI->GetLoadedWorldInfo()->MainWorld->GetSkyControllerOutdoor());

        // Draw barrier second
        reinterpret_cast<void( __fastcall* )(zCSkyController_Outdoor*)>(0x632140)(Engine::GAPI->GetLoadedWorldInfo()->MainWorld->GetSkyControllerOutdoor());
#else
        Engine::GAPI->GetLoadedWorldInfo()
            ->MainWorld->GetSkyControllerOutdoor()
            ->RenderSkyPre();
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

    const PShaderID atmosphereShader =
        sky->GetAtmosphereCB().AC_CameraHeight > sky->GetAtmosphereCB().AC_OuterRadius
        ? PShaderID::PS_AtmosphereOuter
        : PShaderID::PS_Atmosphere;
    if ( SetActivePixelShader( atmosphereShader ) != XR_SUCCESS
        || SetActiveVertexShader( VShaderID::VS_ExWS ) != XR_SUCCESS
        || !ActivePS || !ActiveVS ) {
        return XR_FAILED;
    }

    auto atmosphereBuffer = ActivePS->GetBuffer( "Atmosphere" );
    VS_ExConstantBuffer_PerInstance cbi = {};
    XMStoreFloat4x4( &cbi.World, world );
    cbi.Color = float4( 1.0f, 1.0f, 1.0f, 1.0f );
    auto instanceBuffer = ActiveVS->GetBuffer( "Matrices_PerInstances" );
    if ( !atmosphereBuffer.Update( &sky->GetAtmosphereCB() ).Bind().Succeeded()
        || !instanceBuffer.Update( &cbi, sizeof( cbi ) ).Bind().Succeeded() ) {
        return XR_FAILED;
    }

    D3D11Texture* cloudsTexture = sky->GetCloudTexture();
    D3D11Texture* nightTexture = sky->GetNightTexture();
    D3D11Texture* moonTexture = sky->GetMoonTexture();
    D3D11Texture* rainCloudTexture = sky->GetRainCloudTexture();
    GMesh* skyDome = sky->GetSkyDome();
    if ( !cloudsTexture || !nightTexture || !moonTexture || !rainCloudTexture
        || !skyDome || skyDome->GetMeshes().empty() ) {
        return XR_FAILED;
    }

    ID3D11ShaderResourceView* skyResources[] = {
        cloudsTexture->GetShaderResourceView().Get(),
        nightTexture->GetShaderResourceView().Get(),
        moonTexture->GetShaderResourceView().Get(),
        rainCloudTexture->GetShaderResourceView().Get(),
    };
    if ( std::any_of( std::begin( skyResources ), std::end( skyResources ),
        []( ID3D11ShaderResourceView* resource ) { return resource == nullptr; } ) ) {
        return XR_FAILED;
    }

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
    if ( ActiveVS->Apply() != XR_SUCCESS || ActivePS->Apply() != XR_SUCCESS ) {
        return XR_FAILED;
    }

    GetContext()->PSSetShaderResources(
        0, static_cast<UINT>(std::size( skyResources )), skyResources );
    skyDome->DrawMesh();

    #if defined(BUILD_GOTHIC_1_08k) && !defined(BUILD_1_12F)
    {
        SetDefaultStates();
        rendererState.DepthState.DepthWriteEnabled = false;
        rendererState.DepthState.SetDirty();
        UpdateRenderStates();

        // Draw barrier after sky
        reinterpret_cast<void( __fastcall* )(zCSkyController_Outdoor*)>(0x632140)(Engine::GAPI->GetLoadedWorldInfo()->MainWorld->GetSkyControllerOutdoor());
        Engine::GAPI->SetFarPlane(
            rendererState.RendererSettings.SectionDrawRadius *
            WORLD_SECTION_SIZE );
    }
    #endif

    return XR_SUCCESS;
}

/** Called when a key got pressed */
XRESULT D3D11GraphicsEngine::OnKeyDown( unsigned int key ) {
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

    ShadowMaps->RenderShadowCube( position, range, targetCube, face, debugRTV,
        cullFront, indoor, noNPCs, renderedVobs, renderedMobs, worldMeshCache, clearDepth, casterMask );
}

/** Renders the shadowmaps for the sun */
void XM_CALLCONV D3D11GraphicsEngine::RenderShadowmaps( FXMVECTOR cameraPosition,
    RenderToDepthStencilBuffer* target,
    bool cullFront, bool dontCull,
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> dsvOverwrite,
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> debugRTV ) {

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
    bool has_focus = (GetForegroundWindow() == hWnd);
    if ( m_isWindowActive == has_focus || has_focus != focus_state ) {
        return;
    }

    m_isWindowActive = has_focus;
    UpdateClipCursor( hWnd );
}

/** Update clipping cursor onto window */
void D3D11GraphicsEngine::UpdateClipCursor( HWND hWnd )
{
#ifndef BUILD_SPACER_NET
    RECT rect;
    static RECT last_clipped_rect;

    // People use open settings window to navigate to other screens
    if ( m_isWindowActive && !HasSettingsWindow() ) {
        GetClientRect( hWnd, &rect );
        ClientToScreen( hWnd, reinterpret_cast<LPPOINT>(&rect) + 0 );
        ClientToScreen( hWnd, reinterpret_cast<LPPOINT>(&rect) + 1 );
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
    if ( auto hImgui = Engine::ImGuiHandle ) {
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
        if ( auto hImgui = Engine::ImGuiHandle; hImgui->GetIsActive() ) {
            // ESC and other generic close paths retain current session values.
            hImgui->CommitSettingsEdit();
            hImgui->SettingsVisible = false;
        }
        // else if ( auto antBar = Engine::AntTweakBar; antBar->GetActive() ) {
        //     antBar->SetActive( false );
        // }
        UpdateShouldBlockGameInput();

        UpdateClipCursor( OutputWindow );
    }
}

/** Returns the data of the backbuffer */
XRESULT D3D11GraphicsEngine::GetBackbufferData( bool thumbnail, byte** data, INT2& buffersize, int& pixelsize ) {
    if ( !data ) {
        return XR_INVALID_ARG;
    }
    *data = nullptr;
    buffersize = {};
    pixelsize = 0;

    const INT2 targetSize = thumbnail ? INT2( 256, 256 ) : Resolution;
    if ( targetSize.x <= 0 || targetSize.y <= 0 || !HDRBackBuffer || !HDRBackBuffer->IsValid() ) {
        return XR_FAILED;
    }

    const size_t width = static_cast<size_t>(targetSize.x);
    const size_t height = static_cast<size_t>(targetSize.y);
    if ( width > std::numeric_limits<size_t>::max() / 4u
        || height > std::numeric_limits<size_t>::max() / (width * 4u) ) {
        return XR_INVALID_ARG;
    }
    const size_t rowBytes = width * 4u;
    const size_t dataSize = rowBytes * height;

    SetDefaultStates();
    SetActivePixelShader( PShaderID::PS_PFX_GammaCorrectInv );
    if ( !ActivePS || ActivePS->Apply() != XR_SUCCESS ) {
        return XR_FAILED;
    }

    GammaCorrectConstantBuffer gamma{};
    gamma.G_Gamma = Engine::GAPI->GetGammaValue();
    gamma.G_Brightness = Engine::GAPI->GetBrightnessValue();
    ActivePS->GetBuffer( "GammaCorrectConstantBuffer" ).Update( &gamma ).Bind();

    auto renderTarget = std::make_unique<RenderToTextureBuffer>(
        GetDevice().Get(), targetSize.x, targetSize.y, DXGI_FORMAT_ENGINE_SWAPCHAIN );
    if ( !renderTarget->IsValid()
        || PfxRenderer->CopyTextureToRTV(
            HDRBackBuffer->GetShaderResView(), renderTarget->GetRenderTargetView(), targetSize, true ) != XR_SUCCESS ) {
        return XR_FAILED;
    }

    D3D11_TEXTURE2D_DESC stagingDesc{};
    stagingDesc.ArraySize = 1;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingDesc.Format = DXGI_FORMAT_ENGINE_SWAPCHAIN;
    stagingDesc.Width = static_cast<UINT>(targetSize.x);
    stagingDesc.Height = static_cast<UINT>(targetSize.y);
    stagingDesc.MipLevels = 1;
    stagingDesc.SampleDesc.Count = 1;
    stagingDesc.Usage = D3D11_USAGE_STAGING;

    wrl::ComPtr<ID3D11Texture2D> stagingTexture;
    HRESULT hr = GetDevice()->CreateTexture2D( &stagingDesc, nullptr, stagingTexture.GetAddressOf() );
    if ( FAILED( hr ) ) {
        LogError() << "Backbuffer readback texture creation failed: 0x" << std::hex << static_cast<unsigned long>(hr);
        return XR_FAILED;
    }

    GetContext()->CopyResource( stagingTexture.Get(), renderTarget->GetTexture().Get() );
    D3D11_MAPPED_SUBRESOURCE mapped{};
    hr = GetContext()->Map( stagingTexture.Get(), 0, D3D11_MAP_READ, 0, &mapped );
    if ( FAILED( hr ) ) {
        LogError() << "Backbuffer readback mapping failed: 0x" << std::hex << static_cast<unsigned long>(hr);
        return XR_FAILED;
    }
    if ( !mapped.pData || mapped.RowPitch < rowBytes ) {
        GetContext()->Unmap( stagingTexture.Get(), 0 );
        LogError() << "Backbuffer readback returned an invalid row layout.";
        return XR_FAILED;
    }
    std::unique_ptr<byte[]> resultData( new (std::nothrow) byte[dataSize] );
    if ( !resultData ) {
        GetContext()->Unmap( stagingTexture.Get(), 0 );
        return XR_FAILED;
    }

    const byte* sourceRow = static_cast<const byte*>(mapped.pData);
    byte* destinationRow = resultData.get();
    for ( size_t row = 0; row < height; ++row ) {
        memcpy( destinationRow, sourceRow, rowBytes );
        sourceRow += mapped.RowPitch;
        destinationRow += rowBytes;
    }
    GetContext()->Unmap( stagingTexture.Get(), 0 );

    buffersize = targetSize;
    pixelsize = 4;
    *data = resultData.release();
    return XR_SUCCESS;
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
void D3D11GraphicsEngine::DrawDecalList( const std::vector<zCVob*>& decals,
    bool lighting ) {
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
    if ( !lighting ) {
        auto saturateDecal = []( float v ) { return v < 0.0f ? 0.0f : ( v > 1.0f ? 1.0f : v ); };
        if ( auto sky = Engine::GAPI->GetSky() ) {
            const auto& atmo = sky->GetAtmosphereCB();
            float night = saturateDecal( ( -atmo.AC_LightPos.y + 0.08f ) * 2.5f );
            float rain = std::max( saturateDecal( atmo.AC_RainFXWeight ), saturateDecal( atmo.AC_SceneWettness ) );
            gacb.GA_LightingScale = ( 1.0f + ( 0.34f - 1.0f ) * night ) * ( 1.0f + ( 0.78f - 1.0f ) * rain );
        }
    }
    const float ambientDecalLightingScale = gacb.GA_LightingScale;

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
            // Some materials have identical properties, but are "unique" as in they have no name
            // we should still be able to batch them if texture, flags and color match - i hope?
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
XRESULT D3D11GraphicsEngine::CopyDepthStencil() {
    const auto context = GetContext();
    if ( !context || !DepthStencilBufferCopy || !DepthStencilBuffer
        || !DepthStencilBufferCopy->IsValid() || !DepthStencilBuffer->IsValid() ) {
        return XR_FAILED;
    }

    context->CopyResource(
        DepthStencilBufferCopy->GetTexture().Get(), DepthStencilBuffer->GetTexture().Get() );
    return XR_SUCCESS;
}

/** Draws underwater effects */
void D3D11GraphicsEngine::DrawUnderwaterEffects() {
    SetDefaultStates();
    UpdateRenderStates();

    auto Resolution = GetResolution();
    RefractionInfoConstantBuffer ricb;
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

    for ( auto const& textureParticleRenderInfo : pvecAdd ) {
        zCTexture* tx = std::get<0>( textureParticleRenderInfo );
        std::vector<ParticleInstanceInfo>& instances = *std::get<2>( textureParticleRenderInfo );

        if ( instances.empty() ) continue;

        if ( tx ) {
            // Bind it
            if ( tx->CacheIn( 0.6f ) == zRES_CACHED_IN )
                tx->Bind( 0 );
            else
                continue;
        }

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

        if ( tx ) {
            // Bind it
            if ( tx->CacheIn( 0.6f ) == zRES_CACHED_IN )
                tx->Bind( 0 );
            else
                continue;
        }

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
    // Take out of shadowupdate queue
    for ( auto&& it = FrameShadowUpdateLights.begin(); it != FrameShadowUpdateLights.end(); ++it ) {
        if ( (*it)->Vob == vob ) {
            FrameShadowUpdateLights.erase( it );
            break;
        }
    }

    DebugPointlight = nullptr;

    return XR_SUCCESS;
}

/** Saves a screenshot */
void D3D11GraphicsEngine::SaveScreenshot() {
    if ( !Toolbox::FolderExists( "system\\Screenshots" )
        && !Toolbox::CreateDirectoryRecursive( "system\\Screenshots" ) ) {
        LogError() << "Could not create screenshot directory.";
        return;
    }

    const INT2 screenshotSize = GetResolution();
    if ( screenshotSize.x <= 0 || screenshotSize.y <= 0 || !HDRBackBuffer || !HDRBackBuffer->IsValid() ) {
        LogError() << "Screenshot source is unavailable.";
        return;
    }

    auto screenshotTexture = std::make_unique<RenderToTextureBuffer>(
        GetDevice().Get(), screenshotSize.x, screenshotSize.y, DXGI_FORMAT_ENGINE_DEFAULT );
    if ( !screenshotTexture->IsValid() ) {
        LogError() << "Could not create screenshot texture.";
        return;
    }
    if ( PfxRenderer->CopyTextureToRTV(
        HDRBackBuffer->GetShaderResView(), screenshotTexture->GetRenderTargetView(), screenshotSize ) != XR_SUCCESS ) {
        LogError() << "Could not render screenshot texture.";
        return;
    }

    SYSTEMTIME localTime{};
    GetLocalTime( &localTime );
    char fileName[MAX_PATH]{};
    const int fileNameLength = sprintf_s(
        fileName, "system\\Screenshots\\GD3D11_%04u-%02u-%02u__%02u-%02u-%02u-%03u.jpg",
        localTime.wYear, localTime.wMonth, localTime.wDay,
        localTime.wHour, localTime.wMinute, localTime.wSecond, localTime.wMilliseconds );
    if ( fileNameLength <= 0 ) {
        LogError() << "Could not format screenshot filename.";
        return;
    }

    const HRESULT saveResult = SaveWICTextureToFile(
        GetContext().Get(), screenshotTexture->GetTexture().Get(), GUID_ContainerFormatJpeg,
        Toolbox::ToWideChar( fileName ).c_str(), nullptr, []( IPropertyBag2* props ) {
            PROPBAG2 options[1]{};
            options[0].pstrName = const_cast<wchar_t*>(L"ImageQuality");
            VARIANT values[1]{};
            values[0].vt = VT_R4;
            values[0].fltVal = 0.95f;
            props->Write( 1, options, values );
        }, false );
    if ( FAILED( saveResult ) ) {
        LogError() << "Saving screenshot failed: 0x" << std::hex << static_cast<unsigned long>(saveResult);
        return;
    }

    const std::string screenshotPath = fileName;
    LogInfo() << "Saved screenshot to: " << screenshotPath;
    Engine::GAPI->PrintMessageTimed( INT2( 30, 30 ), "Screenshot taken: " + screenshotPath );
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
    if ( oCGame::GetGame() ) {
        if ( savedBarSize == -1 ) {
            savedBarSize = oCGame::GetGame()->swimBar->psizex;
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
