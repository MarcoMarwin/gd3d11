#include "D3D11ShadowMap.h"
#include <algorithm>
#include <cmath>
#include <DirectXMath.h>

// TODO: Remove circular dependencies
#include "D3D11Effect.h"
#include "D3D11GShader.h"
#include "D3D11CShader.h"
#include "D3D11PfxRenderer.h"
#include "D3D11ShaderManager.h"
#include "D3D11GraphicsEngine.h"
#include "zCCamera.h"
#include "zCVob.h"
#include "oCGame.h"
#include "GMesh.h"
#include "zCVobLight.h"
#include "zCBspTree.h"
#include "zCMaterial.h"
#include "zCTexture.h"
// ^---------------------------------

using namespace DirectX;

extern bool FeatureLevel10Compatibility;

const float NUM_FRAME_SHADOW_UPDATES = 2;  // Fraction of lights to update per frame
const int NUM_MIN_FRAME_SHADOW_UPDATES = 4;  // Minimum lights to update per frame
const int MAX_IMPORTANT_LIGHT_UPDATES = 1;

struct DirectionalLightState {
    XMFLOAT3 Direction;
    float3 Color;
    float Strength;
    float Visibility;
};

static DirectionalLightState GetDirectionalLightState() {
    GSky* sky = Engine::GAPI->GetSky();
    const auto& settings = Engine::GAPI->GetRendererState().RendererSettings;
    const float rain = std::clamp( sky->GetAtmosphereCB().AC_RainFXWeight, 0.0f, 1.0f );

    DirectionalLightState state = {};
    state.Direction = sky->GetMainLightDirection();
    state.Visibility = sky->GetMainLightVisibility();

    // Preserve the established sun/night base exactly. The moon direction and
    // visibility drive its shadow map, while moon illumination stays additive
    // in the lighting shaders and never replaces general night brightness.
    state.Color = settings.SunLightColor;
    state.Strength = Toolbox::lerp(
        settings.SunLightStrength,
        settings.RainSunLightStrength,
        std::min( 1.0f, rain * 2.0f ) );

    return state;
}

static XMVECTOR XM_CALLCONV BuildStableShadowUp( FXMVECTOR viewDir, FXMVECTOR preferredUp ) {
    XMVECTOR dir = XMVector3Normalize( viewDir );

    if ( std::abs( XMVectorGetX( XMVector3Dot( dir, preferredUp ) ) ) < 0.999f ) {
        return preferredUp;
    }

    const XMVECTOR axisX = XMVectorSet( 1.0f, 0.0f, 0.0f, 0.0f );
    const XMVECTOR axisY = XMVectorSet( 0.0f, 1.0f, 0.0f, 0.0f );
    const XMVECTOR axisZ = XMVectorSet( 0.0f, 0.0f, 1.0f, 0.0f );
    XMVECTOR candidate = axisX;
    float bestDot = std::abs( XMVectorGetX( XMVector3Dot( dir, axisX ) ) );
    const float dotY = std::abs( XMVectorGetX( XMVector3Dot( dir, axisY ) ) );
    if ( dotY < bestDot ) {
        candidate = axisY;
        bestDot = dotY;
    }
    const float dotZ = std::abs( XMVectorGetX( XMVector3Dot( dir, axisZ ) ) );
    if ( dotZ < bestDot ) {
        candidate = axisZ;
    }

    XMVECTOR right = XMVector3Normalize( XMVector3Cross( candidate, dir ) );
    return XMVector3Normalize( XMVector3Cross( dir, right ) );
}

static XMVECTOR XM_CALLCONV StabilizeShadowDirectionAtZenith( FXMVECTOR direction ) {
    XMVECTOR dir = XMVector3Normalize( direction );
    const float x = XMVectorGetX( dir );
    const float z = XMVectorGetZ( dir );
    const float horizontalLength = std::sqrt( x * x + z * z );
    constexpr float zenithFloor = 0.005f;

    static XMVECTOR lastStableAzimuth = XMVectorSet( 1.0f, 0.0f, 0.0f, 0.0f );
    if ( horizontalLength >= zenithFloor ) {
        lastStableAzimuth = XMVector3Normalize( XMVectorSet( x, 0.0f, z, 0.0f ) );
        return dir;
    }

    // Keep only the shadow camera away from its singular straight-down view.
    // The sky and lighting continue to use Gothic's unchanged sun direction.
    const float y = XMVectorGetY( dir );
    return XMVector3Normalize( XMVectorAdd(
        XMVectorScale( lastStableAzimuth, zenithFloor ),
        XMVectorSet( 0.0f, y, 0.0f, 0.0f ) ) );
}

void CalculateTemporalInterpolatedPosition(
    const XMVECTOR currentDir,
    XMVECTOR& previousDir,
    XMVECTOR& outDir,
    float frequency ) {
    // Calculate interpolation factor based on SmoothShadowFrequency
        // Higher frequency = faster updates = less smoothing
        // Lower frequency = slower updates = more smoothing (less flickering)
        // The frequency is inverted to get a blend factor: lower frequency = more blending

    // Blend factor: at frequency 500 (default), we want moderate smoothing
    // At frequency 100, we want heavy smoothing (slow updates)
    // At frequency 2000+, we want minimal smoothing (fast updates)
    // Using an exponential-ish curve for better control
    const float blendFactor = std::clamp( frequency / 10000.0f, 0.001f, 0.5f );

    // Smoothly interpolate from previous direction to current direction
    // This creates gradual shadow movement instead of discrete jumps
    XMVECTOR dir = XMVectorLerp( previousDir, currentDir, blendFactor );
    dir = XMVector3Normalize( dir );

    // Update the stored previous direction for next frame
    previousDir = dir;

    // Keep the interpolated direction continuous. Cascade centers are already
    // stabilized on a global shadow-texel grid in CalculateCascadeMatrices.
    outDir = dir;
}

static bool ShadowDirectionMovedByOneTexel(
    FXMVECTOR committedDirection,
    FXMVECTOR targetDirection,
    UINT shadowMapSize ) {
    const float safeMapSize = static_cast<float>(std::max<UINT>( shadowMapSize, 1u ));
    // A cascade spans two radii, so 2 / resolution is approximately one
    // projected texel of angular motion at the cascade radius.
    const float texelAngle = 2.0f / safeMapSize;
    const float deltaSq = XMVectorGetX( XMVector3LengthSq(
        XMVectorSubtract( committedDirection, targetDirection ) ) );
    return deltaSq >= texelAngle * texelAngle;
}

/// <summary>
/// Aligned to Bounding Sphere
/// </summary>
static void CalculateCascadeMatrices(
    CameraReplacement& outCR,
    const Frustum& playerFrustum,
    const std::vector<float>& splits,
    size_t cascadeIdx,
    size_t numCascades,
    float farPlane,
    FXMVECTOR lightPosOrig,
    FXMVECTOR lookAtOrig,
    FXMVECTOR upDirOrig,
    GXMVECTOR shadowCameraPosFallback,
    UINT shadowMapSize,
    float* outTexelWorld )
{
    XMVECTOR lightDir = XMVector3Normalize( XMVectorSubtract( lookAtOrig, lightPosOrig ) );

    XMVECTOR upDir = BuildStableShadowUp( lightDir, upDirOrig );

    XMVECTOR frustumCenter;

    float splitNear = splits[cascadeIdx];
    float splitFar = splits[cascadeIdx + 1];

    if ( !playerFrustum.IsValid() || !playerFrustum.SupportsCulling() ) {
        LogError() << "ShadowMap: Invalid Player Frustum!";
    }

    auto corners = playerFrustum.GetSliceCorners( splitNear, splitFar );

    // Calculate the OPTIMAL center of the frustum slice for a minimal bounding sphere
    XMVECTOR nearCenter = XMVectorZero();
    for ( int i = 0; i < 4; ++i ) nearCenter = XMVectorAdd( nearCenter, XMLoadFloat3( &corners[i] ) );
    nearCenter = XMVectorScale( nearCenter, 0.25f );

    XMVECTOR farCenter = XMVectorZero();
    for ( int i = 4; i < 8; ++i ) farCenter = XMVectorAdd( farCenter, XMLoadFloat3( &corners[i] ) );
    farCenter = XMVectorScale( farCenter, 0.25f );

    XMVECTOR viewDir = XMVector3Normalize( XMVectorSubtract( farCenter, nearCenter ) );
    float L = XMVectorGetX( XMVector3Length( XMVectorSubtract( farCenter, nearCenter ) ) );

    float nearRadiusSq = XMVectorGetX( XMVector3LengthSq( XMVectorSubtract( XMLoadFloat3( &corners[0] ), nearCenter ) ) );
    float farRadiusSq = XMVectorGetX( XMVector3LengthSq( XMVectorSubtract( XMLoadFloat3( &corners[4] ), farCenter ) ) );

    // Slide the center along the view axis to the exact point where Near and Far distances equal out
    float optimalX = (L * L + farRadiusSq - nearRadiusSq) / (2.0f * L);
    optimalX = std::clamp( optimalX, 0.0f, L );

    frustumCenter = XMVectorAdd( nearCenter, XMVectorScale( viewDir, optimalX ) );

    // Calculate the true bounding sphere radius covering all corners
    float invariantRadius = 0.0f;
    for ( int i = 0; i < 8; ++i ) {
        XMVECTOR corner = XMLoadFloat3( &corners[i] );
        XMVECTOR distVec = XMVector3Length( XMVectorSubtract( corner, frustumCenter ) );
        invariantRadius = std::max( invariantRadius, XMVectorGetX( distVec ) );
    }

    // Round the radius to fixed increments to prevent floating-point micro-scaling
    // which can happen due to slight FOV/Aspect ratio rounding.
    invariantRadius = std::ceil( invariantRadius * 16.0f ) / 16.0f;
    float radius = invariantRadius;

    float cascadeSize = invariantRadius * 2.0f;

    float texelSize = cascadeSize / static_cast<float>(shadowMapSize);
    if ( outTexelWorld ) {
        *outTexelWorld = texelSize;
    }

    // Establish a GLOBAL, unmoving light-space grid by using the World Origin (0,0,0)
    // By anchoring to XMVectorZero(), the grid never shifts as the player moves.
    XMMATRIX tempLightView = XMMatrixLookToLH( XMVectorZero(), lightDir, upDir );

    // Transform the moving frustum center into this global light-space grid
    XMVECTOR centerLS = XMVector3TransformCoord( frustumCenter, tempLightView );

    // Snap the X and Y coordinates to the exact size of a shadow texel.
    float snappedX = std::round( XMVectorGetX( centerLS ) / texelSize ) * texelSize;
    float snappedY = std::round( XMVectorGetY( centerLS ) / texelSize ) * texelSize;
    float centerZ = XMVectorGetZ( centerLS );

    XMVECTOR snappedCenterLS = XMVectorSet( snappedX, snappedY, centerZ, 1.0f );

    // Transform the snapped center back into world-space
    XMMATRIX tempLightViewInv = XMMatrixInverse( nullptr, tempLightView );
    XMVECTOR snappedCenterWorld = XMVector3TransformCoord( snappedCenterLS, tempLightViewInv );

    // -----------------------------------------------------------

    // Build the final light view matrix looking at the snapped center
    float pullBackDistance = std::max( 10000.0f, radius * 2.0f );
    XMVECTOR lightPos = XMVectorSubtract( snappedCenterWorld, XMVectorScale( lightDir, pullBackDistance ) );
    XMMATRIX lightView = XMMatrixLookToLH( lightPos, lightDir, upDir );

    // Z-Bounds (Clipping against Scene to prevent overdraw)

    // Find the exact Light-Space Z-bounds of the frustum slice
    float minLightZ = FLT_MAX;
    float maxLightZ = -FLT_MAX;
    for ( const auto& corner : corners ) {
        XMVECTOR vLS = XMVector3TransformCoord( XMLoadFloat3( &corner ), lightView );
        float z = XMVectorGetZ( vLS );
        minLightZ = std::min( minLightZ, z );
        maxLightZ = std::max( maxLightZ, z );
    }

    // --- Dynamic Pullback Calculation ---
    // Calculate how directly overhead the light is.
    // 1.0 = straight down (noon), 0.0 = completely horizontal (horizon)
    const XMVECTOR worldUpForPullback = XMVectorSet( 0.0f, 1.0f, 0.0f, 0.0f );
    float lightDotUp = std::abs( XMVectorGetX( XMVector3Dot( lightDir, worldUpForPullback ) ) );
    lightDotUp = std::max( lightDotUp, 0.05f ); // Prevent division by zero near the horizon

    // Assuming a max shadow caster height of ~6000 units (60 meters) above the frustum.
    // The shallower the angle, the longer the shadow, so we increase the pullback.
    float dynamicPullback = 4000.0f / lightDotUp;

    // Clamp to sensible extremes:
    // Min ~2000 units (high noon, just enough for tall objects directly overhead)
    // Max ~15000 units (sunset, catching long shadows from distant mountains)
    dynamicPullback = std::clamp( dynamicPullback, 2000.0f, 15000.0f );

    float orthoNear = std::max( 1.0f, minLightZ - dynamicPullback );
    float orthoFar = maxLightZ + 5000.0f;

    // --- Scene Bounds Optimization ---
    if ( auto worldInfo = Engine::GAPI->GetLoadedWorldInfo() ) {
        if ( auto bspTree = worldInfo->BspTree ) {
            zTBBox3D sceneBox = bspTree->GetRootNode()->BBox3D;
            std::array<XMFLOAT3, 8> sceneCorners = {
                XMFLOAT3( sceneBox.Min.x, sceneBox.Min.y, sceneBox.Min.z ), XMFLOAT3( sceneBox.Max.x, sceneBox.Min.y, sceneBox.Min.z ),
                XMFLOAT3( sceneBox.Min.x, sceneBox.Max.y, sceneBox.Min.z ), XMFLOAT3( sceneBox.Max.x, sceneBox.Max.y, sceneBox.Min.z ),
                XMFLOAT3( sceneBox.Min.x, sceneBox.Min.y, sceneBox.Max.z ), XMFLOAT3( sceneBox.Max.x, sceneBox.Min.y, sceneBox.Max.z ),
                XMFLOAT3( sceneBox.Min.x, sceneBox.Max.y, sceneBox.Max.z ), XMFLOAT3( sceneBox.Max.x, sceneBox.Max.y, sceneBox.Max.z )
            };

            float sceneMinZ = FLT_MAX;
            float sceneMaxZ = -FLT_MAX;
            for ( const auto& corner : sceneCorners ) {
                XMVECTOR vLS = XMVector3TransformCoord( XMLoadFloat3( &corner ), lightView );
                float z = XMVectorGetZ( vLS );
                sceneMinZ = std::min( sceneMinZ, z );
                sceneMaxZ = std::max( sceneMaxZ, z );
            }

            // Pushes the near plane further back if the scene geometry requires it
            orthoNear = std::min( orthoNear, sceneMinZ - 100.0f );

            // Tighten Far Plane so we don't shoot miles past the level boundaries when looking down
            orthoFar = std::min( orthoFar, sceneMaxZ + 500.0f );
        }
    }

    const XMMATRIX crProjRepl = XMMatrixTranspose( XMMatrixOrthographicLH(
        cascadeSize, cascadeSize, orthoNear, orthoFar ) );

    XMStoreFloat4x4( &outCR.ViewReplacement, XMMatrixTranspose( lightView ) );
    XMStoreFloat4x4( &outCR.ProjectionReplacement, crProjRepl );
    XMStoreFloat3( &outCR.PositionReplacement, lightPos );

    XMVECTOR lookAt = XMVectorAdd( lightPos, lightDir );
    XMStoreFloat3( &outCR.LookAtReplacement, lookAt );

    float cullingMargin = texelSize * 2.0f;
    outCR.frustum.BuildOrthographic( lightView,
        cascadeSize + cullingMargin,
        cascadeSize + cullingMargin,
        orthoNear,
        orthoFar,
        Engine::GAPI->GetRendererState().RendererSettings.DebugSettings.ShadowCascades.ExtendBack,
        Engine::GAPI->GetRendererState().RendererSettings.DebugSettings.ShadowCascades.ExtendFront,
        Engine::GAPI->GetRendererState().RendererSettings.DebugSettings.ShadowCascades.ExtendSide );
}

D3D11ShadowMap::D3D11ShadowMap() {
    
}

D3D11ShadowMap::~D3D11ShadowMap() {
    if ( m_TiledDeferred ) {
        m_TiledDeferred->DetachAllOwners();
    }
}

bool D3D11ShadowMap::ShouldUseAtlas() const {
    const auto& settings = Engine::GAPI->GetRendererState().RendererSettings;
    // FL10 always needs atlas fallback. On FL11+, this can be toggled at runtime.
    return FeatureLevel10Compatibility || settings.DebugSettings.FeatureSet.UseShadowAtlas;
}

uint64_t D3D11ShadowMap::UpdateGrassDetailsShadowGeneration() {
    if ( !Engine::GAPI ) {
        return m_GrassDetailsShadowGeneration;
    }

    const auto& settings = Engine::GAPI->GetRendererState().RendererSettings;
    const int grassDetailsLevel = std::clamp( settings.GrassDetailsLevel, 0, 4 );
    if ( m_LastGrassDetailsLevel != grassDetailsLevel ) {
        m_LastGrassDetailsLevel = grassDetailsLevel;
        ++m_GrassDetailsShadowGeneration;
    }

    return m_GrassDetailsShadowGeneration;
}

void D3D11ShadowMap::RecreateShadowSampler() {
    if ( !m_device ) return;

    // Create sampler
    D3D11_SAMPLER_DESC samplerDesc = {};
    samplerDesc.Filter = D3D11_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR;
    // Atlas cascades are packed sub-rects and therefore use CLAMP.
    // Array cascades use a lit border so PCF taps cannot wrap to the opposite edge.
    auto addressMode = m_useAtlas ? D3D11_TEXTURE_ADDRESS_CLAMP : D3D11_TEXTURE_ADDRESS_BORDER;
    samplerDesc.BorderColor[0] = 1.0f;
    samplerDesc.BorderColor[1] = 1.0f;
    samplerDesc.BorderColor[2] = 1.0f;
    samplerDesc.BorderColor[3] = 1.0f;
    samplerDesc.AddressU = addressMode;
    samplerDesc.AddressV = addressMode;
    samplerDesc.AddressW = addressMode;
    samplerDesc.MipLODBias = 0;
    samplerDesc.MaxAnisotropy = 1;
    samplerDesc.ComparisonFunc = D3D11_COMPARISON_LESS_EQUAL;
    samplerDesc.MinLOD = -FLT_MAX;
    samplerDesc.MaxLOD = FLT_MAX;

    m_shadowmapSampler.Reset();
    HRESULT hr;
    LE( m_device->CreateSamplerState( &samplerDesc, m_shadowmapSampler.GetAddressOf() ) );
    SetDebugName( m_shadowmapSampler.Get(), "ShadowmapSamplerState" );
}

void D3D11ShadowMap::EnsureShadowMapBackend( int size ) {
    if ( !m_device ) return;
     
    const auto& settings = Engine::GAPI->GetRendererState().RendererSettings;
    const UINT atlasNumCascades = static_cast<UINT>( settings.GetEffectiveShadowCascadeCount() );

    bool desiredUseAtlas = ShouldUseAtlas();
    int clampedSize = std::min<int>( std::max<int>( size, 512 ), (FeatureLevel10Compatibility ? 8192 : 16384) );

    if ( desiredUseAtlas != m_useAtlas ) {
        // Switch backend at runtime.
        m_useAtlas = desiredUseAtlas;

        if ( m_useAtlas ) {
            m_cascadedShadowMap.reset();
            m_shadowAtlas = std::make_unique<D3D11ShadowAtlas>();
            const int maxAtlasCascade0Size = (atlasNumCascades <= 1) ? clampedSize : (clampedSize / 2);
            int atlasCascade0Size = std::min<int>( clampedSize, maxAtlasCascade0Size );
            m_shadowAtlas->Init( m_device, atlasCascade0Size, atlasNumCascades );
        } else {
            m_shadowAtlas.reset();
            m_cascadedShadowMap = std::make_unique<D3D11CascadedShadowMapBuffer>();
            m_cascadedShadowMap->Init( m_device, clampedSize, atlasNumCascades );
        }

        // Sampler addressing depends on atlas/array path.
        RecreateShadowSampler();

        // SHADOW_ATLAS is a compile-time shader macro; reload relevant shaders when mode flips.
        auto* graphicsEngine = reinterpret_cast<D3D11GraphicsEngine*>( Engine::GraphicsEngine );
        if ( graphicsEngine ) {
            graphicsEngine->ReloadShaders( ShaderCategory::LightsAndShadows );
        }
    }

    // Ensure resources exist even if no mode switch occurred.
    if ( m_useAtlas && !m_shadowAtlas ) {
        m_shadowAtlas = std::make_unique<D3D11ShadowAtlas>();
        const int maxAtlasCascade0Size = (atlasNumCascades <= 1) ? clampedSize : (clampedSize / 2);
        int atlasCascade0Size = std::min<int>( clampedSize, maxAtlasCascade0Size );
        m_shadowAtlas->Init( m_device, atlasCascade0Size, atlasNumCascades );
    } else if ( m_useAtlas && m_shadowAtlas ) {
        const int maxAtlasCascade0Size = (atlasNumCascades <= 1) ? clampedSize : (clampedSize / 2);
        int atlasCascade0Size = std::min<int>( clampedSize, maxAtlasCascade0Size );
        m_shadowAtlas->Resize( atlasCascade0Size, atlasNumCascades );
    } else if ( !m_useAtlas && !m_cascadedShadowMap ) {
        m_cascadedShadowMap = std::make_unique<D3D11CascadedShadowMapBuffer>();
        m_cascadedShadowMap->Init( m_device, clampedSize, atlasNumCascades );
    }
}

void D3D11ShadowMap::Init( Microsoft::WRL::ComPtr<ID3D11Device1>& device, Microsoft::WRL::ComPtr<ID3D11DeviceContext1>& context, int size ) {
    HRESULT hr;
    m_device = device;
    m_context = context;

    int s = std::min<int>( std::max<int>( size, 512 ), (FeatureLevel10Compatibility ? 8192 : 16384) );

    m_useAtlas = ShouldUseAtlas();
    RecreateShadowSampler();

    // Dummy cube RT used for fallback to satisfy pixel shader runs that expect a RTV bound
    m_dummyCubeRT = std::make_unique<RenderToTextureBuffer>( m_device.Get(), 16, 16, DXGI_FORMAT_ENGINE_DEFAULT, nullptr, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, 1, 6 );

    EnsureShadowMapBackend( s );

    for ( int i = 0; i < MAX_CSM_CASCADES; ++i ) {
        m_RenderQueues[i] = std::make_unique<D3D11RenderQueue>( device.Get(), context.Get() );
    }

    D3D11GraphicsEngineBase* engine = reinterpret_cast<D3D11GraphicsEngineBase*>( Engine::GraphicsEngine );

    // Create constantbuffer for the view-matrices
    D3D11ConstantBuffer* cb = nullptr;
    LE(engine->CreateConstantBuffer( &cb, nullptr, sizeof( CubemapGSConstantBuffer ) ));
    m_PointLightCB.reset( cb );

    Resize( s );

    if ( !FeatureLevel10Compatibility ) {
        m_TiledDeferred = std::make_unique<D3D11TiledDeferredShading>();
        m_TiledDeferred->Init( device, context );
    }
}

void D3D11ShadowMap::Resize( int size ) {

    if ( !m_device ) return;

    const int maxSize = (FeatureLevel10Compatibility ? 8192 : 16384);
    const int s = std::min<int>( std::max<int>( size, 512 ), maxSize );
    const auto& settings = Engine::GAPI->GetRendererState().RendererSettings;
    const UINT atlasNumCascades = static_cast<UINT>( settings.GetEffectiveShadowCascadeCount() );

    EnsureShadowMapBackend( s );

    if ( m_useAtlas ) {
        // Atlas path: with one cascade, use full hardware limit; otherwise reserve width for atlas packing.
        const int maxAtlasCascade0Size = (atlasNumCascades <= 1) ? maxSize : (maxSize / 2);
        int atlasCascade0Size = std::min<int>( s, maxAtlasCascade0Size );
        if ( m_shadowAtlas ) {
            m_shadowAtlas->Resize( atlasCascade0Size, atlasNumCascades );
        }
    } else {
        // Texture array path
        if ( m_cascadedShadowMap ) {
            m_cascadedShadowMap->Resize( s, atlasNumCascades );
        }
    }

    m_lastNumCascades = static_cast<int>( atlasNumCascades );
    // A resized or newly selected backend has no valid contents. The next
    // PrepareRender must rebuild every active cascade, including lazy ones.
    m_ShouldUpdateCascade.fill( true );
}

void D3D11ShadowMap::BindToPixelShader( ID3D11DeviceContext1* context, UINT slot ) {
    if ( m_useAtlas ) {
        if ( m_shadowAtlas ) m_shadowAtlas->BindToPixelShader( context, slot );
    } else {
        if ( m_cascadedShadowMap ) m_cascadedShadowMap->BindToPixelShader( context, slot );
    }
}
void D3D11ShadowMap::BindSampler( ID3D11DeviceContext1* context, UINT slot ) {
    if ( m_shadowmapSampler ) context->PSSetSamplers( slot, 1, m_shadowmapSampler.GetAddressOf() );
}

void D3D11ShadowMap::BindSamplerToCS( ID3D11DeviceContext1* context, UINT slot ) {
    if ( m_shadowmapSampler ) context->CSSetSamplers( slot, 1, m_shadowmapSampler.GetAddressOf() );
}

bool D3D11ShadowMap::ShouldRenderCSMShadows() {
    constexpr float fullRainDisableThreshold = 0.95f;
    constexpr float rainClearingReenableThreshold = 0.80f;

    if ( !Engine::GAPI || Engine::IsShuttingDown() ) {
        m_CsmSuppressedByHeavyRain = false;
        m_ForceCsmUpdateAfterHeavyRain = false;
        return true;
    }

    WorldInfo* worldInfo = Engine::GAPI->GetLoadedWorldInfo();
    const bool isOutdoor = worldInfo && worldInfo->BspTree
        && worldInfo->BspTree->GetBspTreeMode() == zBSP_MODE_OUTDOOR;
    if ( !isOutdoor ) {
        m_CsmSuppressedByHeavyRain = false;
        m_ForceCsmUpdateAfterHeavyRain = false;
        return true;
    }

    // Snow uses the same normalized rain-effect channel in the renderer, but
    // it must not implicitly disable the sun's CSM shadows.
    bool rainWeather = false;
    if ( oCGame* game = oCGame::GetGame() ) {
        if ( zCWorld* world = game->_zCSession_world ) {
            if ( zCSkyController_Outdoor* sky = world->GetSkyControllerOutdoor() ) {
                rainWeather = sky->GetWeatherType() == zTWEATHER_RAIN;
            }
        }
    }

    const float rainWeight = Engine::GAPI->GetRainFXWeight();
    const bool wasSuppressed = m_CsmSuppressedByHeavyRain;
    if ( m_CsmSuppressedByHeavyRain ) {
        if ( rainWeight <= rainClearingReenableThreshold ) {
            m_CsmSuppressedByHeavyRain = false;
        }
    } else if ( rainWeather && rainWeight >= fullRainDisableThreshold ) {
        m_CsmSuppressedByHeavyRain = true;
    }

    if ( wasSuppressed && !m_CsmSuppressedByHeavyRain ) {
        // The sun and camera can move while CSM rendering is suppressed. Do
        // not expose stale cascades when the effect becomes visible again.
        m_ForceCsmUpdateAfterHeavyRain = true;
    }

    return !m_CsmSuppressedByHeavyRain;
}

XRESULT D3D11ShadowMap::PrepareRender()
{
    ZoneScopedN("D3D11ShadowMap::PrepareRender");
    if ( !m_device || !m_context || !Engine::GAPI || Engine::IsShuttingDown() ) {
        return XR_FAILED;
    }

    if ( !ShouldRenderCSMShadows() ) {
        return XR_SUCCESS;
    }

    const bool forceCsmUpdateForCasterChange = m_ForceCsmUpdateAfterHeavyRain;
    m_ForceCsmUpdateAfterHeavyRain = false;

    // Check if shadowmap resources need to be recreated due to setting changes
    {
        auto& settings = Engine::GAPI->GetRendererState().RendererSettings;
        const int maxSize = FeatureLevel10Compatibility ? 8192 : 16384;
        const int desiredSize = std::min<int>( std::max<int>( settings.ShadowMapSize, 512 ), maxSize );
        const int desiredCascades = settings.GetEffectiveShadowCascadeCount();
        settings.NumShadowCascades = desiredCascades;
        const int desiredCascade0Size = ShouldUseAtlas() && desiredCascades > 1
            ? std::min( desiredSize, maxSize / 2 )
            : desiredSize;

        if ( GetSizeX() != desiredCascade0Size
            || m_useAtlas != ShouldUseAtlas()
            || m_lastNumCascades != desiredCascades ) {
            LogInfo() << "Shadowmap config changed, resizing to " << desiredSize << "x" << desiredSize;
            Resize( desiredSize );
            settings.ShadowMapSize = desiredSize;
        }
    }

    oCGame* game = oCGame::GetGame();
    zCCamera* camera = game ? reinterpret_cast<zCCamera*>(game->_zCSession_camera) : nullptr;
    auto* worldInfo = Engine::GAPI->GetLoadedWorldInfo();
    if ( !camera || !worldInfo || !worldInfo->BspTree
        || !Engine::GAPI->IsWorldRenderCacheReady() ) {
        return XR_SUCCESS;
    }
    camera->Activate();
    const XMVECTOR cameraPositionXm = Engine::GAPI->GetCameraPositionXM();

    auto& settings = Engine::GAPI->GetRendererState().RendererSettings;
    const uint64_t grassDetailsGeneration = UpdateGrassDetailsShadowGeneration();
    const bool grassDetailsChanged = settings.EnableShadows
        && m_CsmGrassDetailsGeneration != grassDetailsGeneration;
    if ( settings.EnableShadows ) {
        m_CsmGrassDetailsGeneration = grassDetailsGeneration;
    }

    // ********************************
    // Cascade Shadow Map Rendering (Simple Sequential Version)
    // ********************************

    const float nearPlane = std::max( 1.0f, camera->GetNearPlane() );
    // Clamp far plane to avoid extreme shadow distances
    const float baseFarPlane = std::min( camera->GetFarPlane(), 12000.0f ); // ~120 meters, fine with Fog enabled.

    // WorldShadowRangeScale is the actual CSM cutoff multiplier. Normalize it
    // here as well as in the menu/load path so external or stale values cannot
    // bypass the intended range.
    const float shadowRangeScale = std::clamp( settings.WorldShadowRangeScale, 0.5f, 2.0f );
    settings.WorldShadowRangeScale = shadowRangeScale;
    static float s_lastCsmShadowRangeScale = 1.0f;
    static bool s_hasLastCsmShadowRangeScale = false;
    const bool shadowRangeChanged = !s_hasLastCsmShadowRangeScale
        || std::abs( s_lastCsmShadowRangeScale - shadowRangeScale ) > 0.0001f;
    s_lastCsmShadowRangeScale = shadowRangeScale;
    s_hasLastCsmShadowRangeScale = true;
    const float farPlane = baseFarPlane * shadowRangeScale;
    const int numCascades = settings.GetEffectiveShadowCascadeCount();
    settings.NumShadowCascades = numCascades;

    std::vector<float> splits;
    if ( settings.DebugSettings.ShadowCascades.Lambda > 0.0001f
        || settings.DebugSettings.ShadowCascades.Bias > 0.0001f ) {

        splits = ComputeCascadeSplits( nearPlane, farPlane, numCascades,
                                                         settings.DebugSettings.ShadowCascades.Lambda,
                                                         settings.DebugSettings.ShadowCascades.Bias );
    } else {
        splits = ComputeCascadeSplits( nearPlane, farPlane, numCascades, lambdaBiasTable[numCascades].lambda, lambdaBiasTable[numCascades].bias );
    }

    splits[numCascades] = farPlane; // Let the last cascade reach the full far plane

    m_CascadeSplits.clear();
    m_CascadeSplits.insert( m_CascadeSplits.begin(), splits.begin(), splits.end() );

    // Use the sun during the day and Gothic's original moon orbit at night.
    const DirectionalLightState directionalLight = GetDirectionalLightState();
    XMVECTOR currentDir = StabilizeShadowDirectionAtZenith( XMLoadFloat3( &directionalLight.Direction ) );

    // Smooth the target direction continuously, but commit it to each shadow
    // cascade only after roughly one projected texel of angular movement.
    static struct alignas(16) {
        size_t frameCount;
        std::array<XMVECTOR, MAX_CSM_CASCADES> committedLightDirs;
        std::array<bool, MAX_CSM_CASCADES> lightDirInitialized;
    } perFrameCascadeData = {};

    static XMVECTOR s_previousLightDir = currentDir;
    static bool s_lightDirInitialized = false;
    // A savegame load can change the sky time (and therefore the light direction)
    // discontinuously. Do not spend several seconds interpolating from the previous
    // save's shadows to the new time. Normal per-frame sky movement is far below
    // this angular threshold and remains smoothed.
    bool resetCascadeDirections = false;
    constexpr float maxContinuousLightDirectionDot = 0.9995f; // about 1.8 degrees
    if ( s_lightDirInitialized &&
        XMVectorGetX( XMVector3Dot( s_previousLightDir, currentDir ) ) < maxContinuousLightDirectionDot ) {
        s_previousLightDir = currentDir;
        resetCascadeDirections = true;
    }

    XMVECTOR dir;

    if ( settings.SmoothShadowCameraUpdate ) {
        // Initialize on first frame
        if ( !s_lightDirInitialized ) {
            s_previousLightDir = currentDir;
            s_lightDirInitialized = true;
        }

        CalculateTemporalInterpolatedPosition(
            currentDir,
            s_previousLightDir,
            dir,
            std::max( 1.0f, settings.SmoothShadowFrequency ) );
    } else {
        dir = currentDir;
        s_previousLightDir = currentDir;
        s_lightDirInitialized = true;
    }

    // Feed the real camera position into the stable CSM calculation every frame.
    // CalculateCascadeMatrices performs the final movement in shadow-texel steps,
    // avoiding both coarse 64/160-unit jumps and sub-texel crawling.
    const XMVECTOR WorldShadowCP = cameraPositionXm;
    XMStoreFloat3( &m_WorldShadowPos, WorldShadowCP );

    // Indoor check
    static zTBspMode lastBspMode = zBSP_MODE_OUTDOOR;

    // Array fuer alle Cascade-Matrizen
    bool isOutdoor = worldInfo->BspTree->GetBspTreeMode() == zBSP_MODE_OUTDOOR;
    const bool reenteredOutdoor = isOutdoor && lastBspMode != zBSP_MODE_OUTDOOR;
    const bool forceCsmUpdate = forceCsmUpdateForCasterChange
        || grassDetailsChanged || reenteredOutdoor || shadowRangeChanged;

    const FXMVECTOR p = WorldShadowCP + dir * 10000.0f;
    const FXMVECTOR lookAt = WorldShadowCP;

    static const XMVECTORF32 c_XM_Up = { { { 0, 1, 0, 0 } } };
    const XMVECTOR shadowViewDir = XMVector3Normalize( XMVectorSubtract( lookAt, p ) );
    const XMVECTOR shadowUp = BuildStableShadowUp( shadowViewDir, c_XM_Up );


    if ( !isOutdoor ) {
        if ( settings.EnableShadows && lastBspMode == zBSP_MODE_OUTDOOR ) {
            // Clear all cascade DSVs
            if ( m_useAtlas && m_shadowAtlas ) {
                // Atlas: single DSV, clear once
                if ( auto dsv = m_shadowAtlas->GetDepthStencilView() ) {
                    m_context->ClearDepthStencilView( dsv, D3D11_CLEAR_DEPTH, 0.0f, 0 );
                }
            } else {
                for ( size_t cascadeIdx = 0; cascadeIdx < MAX_CSM_CASCADES; ++cascadeIdx ) {
                    if ( auto dsv = GetCascadeDSV( static_cast<UINT>( cascadeIdx ) ) ) {
                        m_context->ClearDepthStencilView( dsv, D3D11_CLEAR_DEPTH, 0.0f, 0 );
                    }
                }
            }
            lastBspMode = zBSP_MODE_INDOOR;
        }

        // Setze Default fuer Indoor
        for ( int i = 0; i < numCascades; ++i ) {
            XMStoreFloat4x4( &m_CascadeCRs[i].ViewReplacement, XMMatrixTranspose( XMMatrixLookAtLH( p, lookAt, shadowUp ) ) );
            XMStoreFloat4x4( &m_CascadeCRs[i].ProjectionReplacement, XMMatrixTranspose( XMMatrixOrthographicLH(
                farPlane, farPlane, 1.0f, 20000.f ) ) );
            XMStoreFloat3( &m_CascadeCRs[i].PositionReplacement, p );
            XMStoreFloat3( &m_CascadeCRs[i].LookAtReplacement, lookAt );
        }
    } else {
        lastBspMode = zBSP_MODE_OUTDOOR;

        // Increment frame counter for temporal cascade updates
        perFrameCascadeData.frameCount++;
        // The atlas has a single depth target and is rebuilt as a whole. Keep
        // lazy updates on the array backend only, as in the stable CSM path.
        bool lazyCascadeUpdate = !m_useAtlas
            && settings.GetEffectiveLazyCascadeUpdate();
        // BuildStableShadowUp changes its basis around overhead lighting. The
        // wider guard prevents a visible transition at the zenith.
        const bool overheadLight = std::abs(
            XMVectorGetX( XMVector3Dot( shadowViewDir, c_XM_Up ) ) ) > 0.94f;
        if ( overheadLight ) {
            lazyCascadeUpdate = false;
        }

        Frustum playerFrustum = Frustum::AlwaysContainingFrustum();
        if ( camera ) {
            const auto& view = camera->trafoView; // Column-Major, needs Transpose for DxMath
            const auto& proj = camera->trafoProjection; // Row-Major, does not need transpose.
            playerFrustum.BuildPerspective(
                XMMatrixTranspose( XMLoadFloat4x4( &view ) ),
                XMLoadFloat4x4( &proj )
            );
        }

        static XMFLOAT3 s_previousShadowCameraPosition = XMFLOAT3( 0.0f, 0.0f, 0.0f );
        static XMFLOAT3 s_previousShadowCameraForward = XMFLOAT3( 0.0f, 0.0f, 1.0f );
        static bool s_hasPreviousShadowCamera = false;

        XMFLOAT3 currentShadowCameraPosition;
        XMStoreFloat3( &currentShadowCameraPosition, cameraPositionXm );
        const XMMATRIX inverseView = XMMatrixInverse( nullptr, Engine::GAPI->GetViewMatrixXM() );
        XMFLOAT3 currentShadowCameraForward;
        XMStoreFloat3( &currentShadowCameraForward, XMVector3Normalize( inverseView.r[2] ) );

        bool forceCascadeUpdateForViewChange = !s_hasPreviousShadowCamera;
        if ( s_hasPreviousShadowCamera ) {
            float cameraMoveDistance = 0.0f;
            XMStoreFloat( &cameraMoveDistance, XMVector3Length(
                XMLoadFloat3( &currentShadowCameraPosition ) - XMLoadFloat3( &s_previousShadowCameraPosition ) ) );

            float cameraTurnDot = 1.0f;
            XMStoreFloat( &cameraTurnDot, XMVector3Dot(
                XMLoadFloat3( &currentShadowCameraForward ), XMLoadFloat3( &s_previousShadowCameraForward ) ) );

            forceCascadeUpdateForViewChange = cameraMoveDistance > 40.0f || cameraTurnDot < 0.9992f;
        }

        s_previousShadowCameraPosition = currentShadowCameraPosition;
        s_previousShadowCameraForward = currentShadowCameraForward;
        s_hasPreviousShadowCamera = true;

        if ( forceCascadeUpdateForViewChange ) {
            lazyCascadeUpdate = false;
        }
        if ( forceCsmUpdate ) {
            lazyCascadeUpdate = false;
        }
        for ( int cascadeIdx = 0; cascadeIdx < numCascades; ++cascadeIdx ) {
            // pre-calculate all cascade matrices, to be able to frustum-cull anything that is not in this or the next cascade.

            bool shouldUpdateCascade = true;
            if ( lazyCascadeUpdate ) {
                // Keep the two camera-near cascades current. Only the two
                // distant cascades use staggered updates.
                static constexpr std::array<size_t, MAX_CSM_CASCADES> updatePeriods = { 1, 1, 5, 10 };
                const size_t periodIndex = std::min<size_t>(
                    static_cast<size_t>( cascadeIdx ), updatePeriods.size() - 1 );
                shouldUpdateCascade = (perFrameCascadeData.frameCount % updatePeriods[periodIndex]) == 0;
            }
            if ( resetCascadeDirections ) {
                shouldUpdateCascade = true;
            }
            if ( forceCsmUpdate ) {
                shouldUpdateCascade = true;
            }
            if ( !m_CascadeCRs[cascadeIdx].frustum.IsValid() ) {
                shouldUpdateCascade = true;
            }
            m_ShouldUpdateCascade[cascadeIdx] = shouldUpdateCascade;

            if ( shouldUpdateCascade || !m_CascadeCRs[cascadeIdx].frustum.IsValid() ) {
                const UINT cascadePixelSize = GetCascadePixelSize( cascadeIdx );
                if ( !perFrameCascadeData.lightDirInitialized[cascadeIdx]
                    || resetCascadeDirections
                    || ShadowDirectionMovedByOneTexel(
                        perFrameCascadeData.committedLightDirs[cascadeIdx], dir, cascadePixelSize ) ) {
                    perFrameCascadeData.committedLightDirs[cascadeIdx] = dir;
                    perFrameCascadeData.lightDirInitialized[cascadeIdx] = true;
                }

                const XMVECTOR cascadeDir = perFrameCascadeData.committedLightDirs[cascadeIdx];
                const XMVECTOR cascadeP = XMVectorAdd(
                    WorldShadowCP, XMVectorScale( cascadeDir, 10000.0f ) );
                const XMVECTOR cascadeLookAt = WorldShadowCP;
                const XMVECTOR cascadeViewDir = XMVector3Normalize(
                    XMVectorSubtract( cascadeLookAt, cascadeP ) );
                const XMVECTOR cascadeUp = BuildStableShadowUp( cascadeViewDir, c_XM_Up );

                CalculateCascadeMatrices(
                    m_CascadeCRs[cascadeIdx],
                    playerFrustum,
                    splits,
                    cascadeIdx,
                    numCascades,
                    farPlane,
                    cascadeP,
                    cascadeLookAt,
                    cascadeUp,
                    WorldShadowCP,
                    cascadePixelSize,
                    &m_CascadeTexelWorld[cascadeIdx] );
            }
        }
    }

    // Build a conservative culling volume that covers all cascades rendered this frame.
    Frustum frustum = Frustum::AlwaysContainingFrustum();
    if ( isOutdoor && numCascades > 0 ) {
        int lastUpdatedCascade = 0;
        for ( int cascadeIdx = 0; cascadeIdx < numCascades; ++cascadeIdx ) {
            if ( m_ShouldUpdateCascade[cascadeIdx] ) {
                lastUpdatedCascade = cascadeIdx;
            }
        }

        std::array<XMFLOAT3, MAX_CSM_CASCADES * 8> combinedCorners = {};
        size_t combinedCornerCount = 0;

        static constexpr XMFLOAT3 ndcCorners[8] = {
            XMFLOAT3( -1.0f, -1.0f, 0.0f ), XMFLOAT3( 1.0f, -1.0f, 0.0f ),
            XMFLOAT3( -1.0f, 1.0f, 0.0f ),  XMFLOAT3( 1.0f, 1.0f, 0.0f ),
            XMFLOAT3( -1.0f, -1.0f, 1.0f ), XMFLOAT3( 1.0f, -1.0f, 1.0f ),
            XMFLOAT3( -1.0f, 1.0f, 1.0f ),  XMFLOAT3( 1.0f, 1.0f, 1.0f )
        };

        for ( int cascadeIdx = 0; cascadeIdx <= lastUpdatedCascade; ++cascadeIdx ) {
            if ( !m_CascadeCRs[cascadeIdx].frustum.IsValid() ) {
                continue;
            }

            const XMMATRIX view = XMMatrixTranspose( XMLoadFloat4x4( &m_CascadeCRs[cascadeIdx].ViewReplacement ) );
            const XMMATRIX proj = XMMatrixTranspose( XMLoadFloat4x4( &m_CascadeCRs[cascadeIdx].ProjectionReplacement ) );
            const XMMATRIX invViewProj = XMMatrixInverse( nullptr, XMMatrixMultiply( view, proj ) );

            for ( const XMFLOAT3& ndcCorner : ndcCorners ) {
                XMVECTOR worldCorner = XMVector3TransformCoord( XMLoadFloat3( &ndcCorner ), invViewProj );
                XMStoreFloat3( &combinedCorners[combinedCornerCount++], worldCorner );
            }
        }

        if ( combinedCornerCount > 0 ) {
            BoundingSphere combinedSphere;
            BoundingSphere::CreateFromPoints(
                combinedSphere,
                combinedCornerCount,
                combinedCorners.data(),
                sizeof( XMFLOAT3 ) );
            // Keep this conservative because shadow caster expansion can exceed strict cascade bounds.
            combinedSphere.Radius *= 1.2f;
            frustum.BuildCubemapFace( XMLoadFloat3( &combinedSphere.Center ), combinedSphere.Radius, 0 );
        }
    }

    static std::vector<VobInfo*> potentialCasters;
    static std::vector<VobLightInfo*> _1;
    static std::vector<SkeletalVobInfo*> _2;
    potentialCasters.reserve(1024);
    potentialCasters.clear();

    {
        RndCullContext ctx;
        LegacyRenderQueueProxy q(potentialCasters, _1, _2);

        ctx.queue = &q;
        ctx.frustum = frustum;
        ctx.cameraPosition = m_WorldShadowPos;
        ctx.stage = RenderStage::STAGE_DRAW_SHADOWS;
        ctx.drawDistances.OutdoorVobs = 20000;
        ctx.drawDistances.OutdoorVobsSmall = 20000;
        ctx.drawDistances.IndoorVobs = 20000;
        ctx.drawDistances.VisualFX = 0.0f;
        ctx.drawDistancesSq.OutdoorVobs = ctx.drawDistances.OutdoorVobs * ctx.drawDistances.OutdoorVobs;
        ctx.drawDistancesSq.OutdoorVobsSmall = ctx.drawDistances.OutdoorVobsSmall * ctx.drawDistances.OutdoorVobsSmall;
        ctx.drawDistancesSq.IndoorVobs = ctx.drawDistances.IndoorVobs * ctx.drawDistances.IndoorVobs;
        ctx.drawDistancesSq.VisualFX = 0.0f;

        const auto& rs = Engine::GAPI->GetRendererState().RendererSettings;
        // The combined cull feeds all cascades. Apply the threshold after the
        // per-cascade distribution below so each cascade uses its own texel size.
        ctx.minVobSize = 0.0f;
        ctx.drawFlags.DrawVOBs = rs.DrawVOBs;
        ctx.drawFlags.DrawMobs = rs.DrawMobs;
        ctx.drawFlags.EnableDynamicLighting = rs.EnableDynamicLighting;
        ctx.drawFlags.CullVobs = rs.DebugSettings.Culling.CullVobs;
        ctx.drawFlags.CollectIndoorVobs = false;
        ctx.drawFlags.CollectLargeVobs = true;
        ctx.drawFlags.CollectSmallVobs = true;
        ctx.drawFlags.CollectMobs = false;
        ctx.drawFlags.CollectLights = false;
        
        Engine::GAPI->CollectVisibleVobs( ctx );
    }
    
    {
        ZoneScopedN("CascadeFrustumCulling");

        for ( int i = 0; i < numCascades; ++i ) {
            m_RenderQueues[i]->Reset();
        }

        if ( numCascades > 3 ) {
            for ( auto vob : potentialCasters ) {

                auto boundingSphere = Frustum::BSphereFromzTBBox3D( vob->Vob->GetBBox() );
                if ( m_CascadeCRs[0].frustum.Intersects( boundingSphere ) ) {
                    m_RenderQueues[0]->GetVobs().push_back( vob );
                    m_RenderQueues[1]->GetVobs().push_back( vob );
                    m_RenderQueues[2]->GetVobs().push_back( vob );
                    m_RenderQueues[3]->GetVobs().push_back( vob );
                    continue;
                }

                if ( /*m_ShouldUpdateCascade[1] && */m_CascadeCRs[1].frustum.Intersects( boundingSphere ) ) {
                    m_RenderQueues[1]->GetVobs().push_back( vob );
                    m_RenderQueues[2]->GetVobs().push_back( vob );
                    m_RenderQueues[3]->GetVobs().push_back( vob );
                    continue;
                }

                if ( m_ShouldUpdateCascade[2] && m_CascadeCRs[2].frustum.Intersects( boundingSphere ) ) {
                    m_RenderQueues[2]->GetVobs().push_back( vob );
                    m_RenderQueues[3]->GetVobs().push_back( vob );
                    continue;
                }

                if ( m_ShouldUpdateCascade[3] && m_CascadeCRs[3].frustum.Intersects( boundingSphere ) )
                    m_RenderQueues[3]->GetVobs().push_back( vob );
            }
        } else if ( numCascades > 2 ) {
            for ( auto vob : potentialCasters ) {
                auto boundingSphere = Frustum::BSphereFromzTBBox3D( vob->Vob->GetBBox() );
                if ( m_CascadeCRs[0].frustum.Intersects( boundingSphere ) ) {
                    m_RenderQueues[0]->GetVobs().push_back( vob );
                    m_RenderQueues[1]->GetVobs().push_back( vob );
                    m_RenderQueues[2]->GetVobs().push_back( vob );
                    continue;
                }

                if ( /*m_ShouldUpdateCascade[1] && */m_CascadeCRs[1].frustum.Intersects( boundingSphere ) ) {
                    m_RenderQueues[1]->GetVobs().push_back( vob );
                    m_RenderQueues[2]->GetVobs().push_back( vob );
                    continue;
                }
                if ( m_ShouldUpdateCascade[2] && m_CascadeCRs[2].frustum.Intersects( boundingSphere ) )
                    m_RenderQueues[2]->GetVobs().push_back( vob );
            }
        } else if ( numCascades > 1 ) {
            for ( auto vob : potentialCasters ) {
                auto boundingSphere = Frustum::BSphereFromzTBBox3D( vob->Vob->GetBBox() );
                if ( m_CascadeCRs[0].frustum.Intersects( boundingSphere ) )                     {
                    m_RenderQueues[0]->GetVobs().push_back( vob );
                    m_RenderQueues[1]->GetVobs().push_back( vob );
                    continue;
                }

                if ( /*m_ShouldUpdateCascade[1] && */m_CascadeCRs[1].frustum.Intersects( boundingSphere ) )
                    m_RenderQueues[1]->GetVobs().push_back( vob );
            }
        } else if ( numCascades > 0 ) {
            for ( auto vob : potentialCasters ) {
                auto boundingSphere = Frustum::BSphereFromzTBBox3D( vob->Vob->GetBBox() );
                if ( m_CascadeCRs[0].frustum.Intersects( boundingSphere ) )
                    m_RenderQueues[0]->GetVobs().push_back( vob );
            }
        }

        const float casterMinTexels = std::clamp(
            settings.ShadowCasterMinTexels, 0.0f, 16.0f );
        for ( int cascadeIdx = 0; cascadeIdx < numCascades; ++cascadeIdx ) {
            const float minVobSize = m_CascadeTexelWorld[cascadeIdx] * casterMinTexels;
            auto& cascadeVobs = m_RenderQueues[cascadeIdx]->GetVobs();
            cascadeVobs.erase( std::remove_if( cascadeVobs.begin(), cascadeVobs.end(),
                [minVobSize]( const VobInfo* vob ) {
                    return minVobSize > 0.0f && vob && vob->VisualInfo
                        && vob->VisualInfo->MeshSize < minVobSize;
                } ), cascadeVobs.end() );
        }
    }

    return XR_SUCCESS;
}

// Computes cascade splits using a interpolation between uniform and logarithmic splits, additionally modified by a bias factor.
// Returns vector with (numCascades + 1) entries: [nearPlane, split1, split2, ..., farPlane]
std::vector<float> D3D11ShadowMap::ComputeCascadeSplits( float nearPlane, float farPlane, size_t numCascades, float lambda, float bias ) {
    if ( numCascades == 0 ) return { nearPlane, farPlane };

    lambda = std::clamp( lambda, 0.0f, 1.0f );

    std::vector<float> splits;
    splits.reserve( numCascades + 1 );
    splits.push_back( nearPlane );

    for ( size_t i = 1; i <= numCascades; ++i ) {
        // Calculate the linear fraction (0.0 to 1.0)
        float linearFraction = static_cast<float>(i) / static_cast<float>(numCascades);

        // Apply the BIAS (Power Function).
        // If bias > 1 (e.g., 2.0), this pushes values closer to 0, making near cascades smaller.
        float si = std::pow( linearFraction, bias );

        // apply logarithmic and uniform split calculations
        float logSplit = nearPlane * std::pow( farPlane / nearPlane, si );
        float uniformSplit = nearPlane + (farPlane - nearPlane) * si;

        // Interpolate
        float d = lambda * logSplit + (1.0f - lambda) * uniformSplit;

        splits.push_back( d );
    }

    return splits;
}

XRESULT D3D11ShadowMap::DrawPointlightShadows( std::vector<VobLightInfo*>& lights ) {
    ZoneScopedN( "DrawPointlightShadows" );

    auto* graphicsEngine = reinterpret_cast<D3D11GraphicsEngine*>( Engine::GraphicsEngine );
    if ( !graphicsEngine || !graphicsEngine->GetPfxRenderer()
        || !Engine::GAPI || Engine::IsShuttingDown()
        || !Engine::GAPI->IsWorldRenderCacheReady() ) {
        // Loading screens can still execute renderer callbacks while the old
        // world is being destroyed. Do not touch any raw world pointers here;
        // ResetVobs clears the deferred queue after flushing renderer workers.
        return XR_SUCCESS;
    }

    auto& settings = Engine::GAPI->GetRendererState().RendererSettings;
    const uint64_t grassDetailsGeneration = UpdateGrassDetailsShadowGeneration();
    const bool grassDetailsChanged = m_PointlightGrassDetailsGeneration != grassDetailsGeneration;
    if ( settings.EnablePointlightShadows <= 0 ) {
        return XR_SUCCESS;
    }
    m_PointlightGrassDetailsGeneration = grassDetailsGeneration;

    // Shadow resources follow the same frame visibility that is already limited by VisualFXDrawRadius.
    auto releaseIfInvisible = [this]( VobLightInfo* info ) {
        if ( !info || !info->LightShadowBuffers )
            return;

        if ( D3D11PointLight* pl = dynamic_cast<D3D11PointLight*>(info->LightShadowBuffers.get()) ) {
            const bool visible = info->IsEffectivelyEnabled() && info->VisibleInFrame;
            if ( pl->ShouldReleaseForVisibility( visible ) ) {
                pl->ClearTiledSlot();
                pl->ReleaseShadowMap();
            }
        }
    };
    for ( auto& it : Engine::GAPI->VobLightMap ) {
        releaseIfInvisible( it.second );
    }
    for ( const auto& rendererLight : Engine::GAPI->GetRendererPointLights() ) {
        releaseIfInvisible( rendererLight.get() );
    }
    auto _ = graphicsEngine->RecordGraphicsEvent( GE_NAME( "DrawPointlightShadows" ) );

    graphicsEngine->SetDefaultStates();

    // ********************************
    // Draw world shadows
    // ********************************
    const bool pointlightShadowsEnabled = settings.EnablePointlightShadows
        != GothicRendererSettings::PLS_DISABLED;
    const bool dynamicMode = pointlightShadowsEnabled
        && settings.UseDynamicPointlightNpcShadows();
    const bool staticOnlyMode = pointlightShadowsEnabled && !dynamicMode;
    constexpr int kMaxBackgroundPointlightUpdatesPerFrame = 2;
    // Draw pointlight shadows
    std::list<VobLightInfo*> importantUpdates;

    DepthStencilPool* dsPool = graphicsEngine->GetPfxRenderer()->GetDepthStencilPool();
    
    const bool isTiledShadingEnabled = m_TiledDeferred && settings.EnableTiledLighting;
    const int requiredShadowMapKind = isTiledShadingEnabled ? 1 : 0;
    for ( auto const& light : lights ) {
        if ( !light || !light->IsEffectivelyEnabled() || !light->VisibleInFrame ) {
            continue;
        }
        const bool visualFxShadowsAllowed = !light->IsVisualFXLight || dynamicMode;
        const bool regularShadowLight = light->AllowsPointlightShadows && visualFxShadowsAllowed;
        if ( !regularShadowLight ) {
            light->UpdateShadows = false;
            if ( D3D11PointLight* pl = dynamic_cast<D3D11PointLight*>(light->LightShadowBuffers.get()) ) {
                pl->ClearTiledSlot();
                pl->ReleaseShadowMap();
            }
            continue;
        }
        // Create resources only when an eligible light is actually visible.
        if ( !light->LightShadowBuffers ) {
            BaseShadowedPointLight* bpl;
            // Keep the resource/cache path aligned with the source itself.
            // The global dynamic-shadow option controls caster rendering, not
            // whether a static candle should lose its persistent world cache.
            const bool shadowLightIsDynamic = light->IsVisualFXLight
                || light->IsDynamicVobLight
                || !light->IsEffectivelyStatic();
            graphicsEngine->CreateShadowedPointLight( &bpl, light, shadowLightIsDynamic );
            light->LightShadowBuffers.reset( bpl );
            light->UpdateShadows = true;
        }

        if ( D3D11PointLight* pl = dynamic_cast<D3D11PointLight*>(light->LightShadowBuffers.get()) ) {
            if ( pl->UpdateDynamicNpcShadowCasterMode() ) {
                light->UpdateShadows = true;
            }

            // Advanced point-light shadow resolution. Shadow Quality only
            // supplies the initial value; the menu can override it later.
            // Keep the runtime allocation domain identical to the Advanced
            // menu and the persisted setting: 128, 256 or 512 px.
            int desiredResolution = std::clamp( settings.PointlightShadowMapSize, 128, 512 );
            float allocationCameraDistance = FLT_MAX;
            XMStoreFloat( &allocationCameraDistance, XMVector3Length(
                light->GetEffectivePositionWorldXM() - Engine::GAPI->GetCameraPositionXM() ) );
            float allocationHeroDistance = FLT_MAX;
            if ( zCVob* hero = Engine::GAPI->GetPlayerVob() ) {
                const XMFLOAT3 heroPosition = hero->GetPositionWorld();
                XMStoreFloat( &allocationHeroDistance, XMVector3Length(
                    light->GetEffectivePositionWorldXM() - XMLoadFloat3( &heroPosition ) ) );
            }
            const float allocationRange = light->GetEffectiveLightRange();
            const bool heroRelevant = allocationHeroDistance <= allocationRange + 250.0f;
            const bool cameraNear = allocationCameraDistance <= allocationRange + 1200.0f;
            // Static lights keep their persistent world-shadow base and, when
            // enabled, receive the same animated NPC/MOB overlay as dynamic
            // lights. Static low-resolution slots never have an animated
            // target.
            const bool animatedShadowOverlayEligible = dynamicMode
                && pl->UseDynamicNpcShadowCasters()
                && !pl->IsTiledStaticLowRes();
            const bool nearDynamicShadowSource = animatedShadowOverlayEligible
                && ( heroRelevant || cameraNear );
            const float slotPriority = heroRelevant ? 0.0f
                : std::min( allocationCameraDistance * allocationCameraDistance,
                    allocationHeroDistance * allocationHeroDistance );
            const bool eligibleForStaticLowRes = light->IsEffectivelyStatic()
                && !light->IsDynamicVobLight && !light->IsVisualFXLight;
            const float lowTierBoundary = light->GetEffectiveLightRange()
                + (pl->IsTiledStaticLowRes() ? 2000.0f : 3000.0f);
            const bool staticLowRes = eligibleForStaticLowRes && !heroRelevant
                && allocationCameraDistance > lowTierBoundary;
            if ( isTiledShadingEnabled && pl->GetTiledSlot() >= 0 && !pl->IsTiledStaticLowRes() ) {
                m_TiledDeferred->TouchSlotPriority( pl->GetTiledSlot(), slotPriority );
            }

            // Acquire memory if it doesn't have it (or resolution changed)
            const bool needsResourceReallocation = !pl->HasShadowMap( requiredShadowMapKind )
                || pl->GetShadowMapResolution() != desiredResolution
                || (isTiledShadingEnabled && pl->IsTiledStaticLowRes() != staticLowRes);
            if ( needsResourceReallocation ) {
                pl->ClearTiledSlot();
                pl->ReleaseShadowMap();

                // Try tiled slot when tiled lighting is active.
                if ( isTiledShadingEnabled ) {
                    int slot = m_TiledDeferred->AllocateSlot(
                        static_cast<uint32_t>(desiredResolution), staticLowRes, pl, slotPriority );
                    if ( slot >= 0 ) {
                        pl->SetTiledSlot( slot,
                            m_TiledDeferred->GetSlotTarget( slot ),
                            m_TiledDeferred->GetDynamicSlotTarget( slot ),
                            m_TiledDeferred.get() );
                        pl->SetCurrentResolution( desiredResolution );
                    } else {
                        light->UpdateShadows = false;
                        continue; // failed to allocate tiled slot, skip shadow rendering for this light this frame
                    }
                } else {
                    pl->AcquireShadowMap( dsPool, desiredResolution );
                }

                light->UpdateShadows = true; // Force an immediate render this frame
            }

            if ( grassDetailsChanged ) {
                // Grass-card shadow geometry follows the normal shadow
                // caster path, so a detail-level change invalidates existing
                // pointlight maps as well as the CSM maps.
                light->UpdateShadows = true;
            }


            bool needsUpdate = pl->NeedsUpdate();
            bool isInited = pl->IsInited();

            // Keep nearby/hero-relevant animated overlays current. Other
            // overlays use the persistent bounded queue below; the static
            // world base is not rebuilt for that queue turn.
            if ( nearDynamicShadowSource )
                light->UpdateShadows = true;

            // Sort into Important vs Background Queue
            if ( isInited ) {
                // Immediate Priority: Light moved, was just created, or explicit flag set
                if ( needsUpdate || light->UpdateShadows ) {
                    auto& queue = graphicsEngine->FrameShadowUpdateLights;
                    auto queued = std::find( queue.begin(), queue.end(), light );
                    if ( queued != queue.end() ) {
                        queue.erase( queued );
                    }
                    importantUpdates.emplace_back( light );
                }
                // Every non-near animated overlay participates in the
                // persistent round-robin queue. Static fixture lights use the
                // same overlay path while dynamic casters are enabled.
                else if ( animatedShadowOverlayEligible ) {
                    auto& queue = graphicsEngine->FrameShadowUpdateLights;
                    if ( std::find( queue.begin(), queue.end(), light ) == queue.end() ) {
                        queue.emplace_back( light );
                    }
                } else {
                    auto& queue = graphicsEngine->FrameShadowUpdateLights;
                    queue.remove( light );
                }
            }
        }
    }

    // Render the immediate priority lights
    int lowStaticRenders = 0;
    constexpr int maxLowStaticRendersPerFrame = 4;
    for ( auto const& importantUpdate : importantUpdates ) {
        auto* pointLight = static_cast<D3D11PointLight*>(importantUpdate->LightShadowBuffers.get());
        if ( pointLight->IsTiledStaticLowRes() && !pointLight->IsStaticShadowReady()
            && lowStaticRenders >= maxLowStaticRendersPerFrame ) {
            auto& queue = graphicsEngine->FrameShadowUpdateLights;
            if ( std::find( queue.begin(), queue.end(), importantUpdate ) == queue.end() ) {
                queue.emplace_back( importantUpdate );
            }
            continue;
        }
        if ( pointLight->IsTiledStaticLowRes() && !pointLight->IsStaticShadowReady() ) {
            ++lowStaticRenders;
        }
        pointLight->RenderCubemap( true, m_PointLightCB.get() );
        importantUpdate->UpdateShadows = false;
    }

    // Process the background queue with the fixed, bounded 224pre budget.
    // Near/hero lights never enter this queue and are not counted here.
    const int maxBackgroundUpdates = kMaxBackgroundPointlightUpdatesPerFrame;
    int updatesDone = 0;

    while ( !graphicsEngine->FrameShadowUpdateLights.empty() && updatesDone < maxBackgroundUpdates ) {
        auto light = graphicsEngine->FrameShadowUpdateLights.front();
        graphicsEngine->FrameShadowUpdateLights.pop_front();

        if ( !light || !light->IsEffectivelyEnabled() || !light->VisibleInFrame ) continue;

        D3D11PointLight* l = static_cast<D3D11PointLight*>( light->LightShadowBuffers.get() );
        if ( !l ) continue;

        // A queued light can lose its shared slot to a much closer challenger.
        // Do not spend one of the bounded background updates on a stale entry.
        if ( !l->HasShadowMap( requiredShadowMapKind ) ) continue;

        if ( staticOnlyMode && l->IsStaticShadowReady() && !l->NeedsUpdate() ) {
            light->UpdateShadows = false;
            continue;
        }

        light->UpdateShadows = false;

        // FORCE the render! It waited in line for its turn, it must draw.
        l->RenderCubemap( true, m_PointLightCB.get() );
        graphicsEngine->DebugPointlight = l;

        updatesDone++;
    }

    return XR_SUCCESS;
}

void D3D11ShadowMap::BuildWorldShadowCasterCache() {
    ZoneScopedN( "D3D11ShadowMap::BuildWorldShadowCasterCache" );
    if ( !Engine::GAPI || !Engine::GAPI->IsWorldRenderCacheReady() ) {
        m_WorldShadowCasters.clear();
        m_WorldShadowCasterLookup.clear();
        m_WorldShadowVisibleCasters.clear();
        m_WorldShadowCasterCacheValid = false;
        return;
    }

    const auto& settings = Engine::GAPI->GetRendererState().RendererSettings;
    if ( !settings.DrawWorldMesh || !settings.DrawShadowGeometry ) {
        m_WorldShadowCasters.clear();
        m_WorldShadowCasterLookup.clear();
        m_WorldShadowVisibleCasters.clear();
        m_WorldShadowCasterCacheValid = false;
        return;
    }

    const uint64_t worldGeneration = Engine::GAPI->GetCityWindowConfigurationGeneration();
    // RenderShadowmaps uses Gothic's fixed alpha-test reference for the shadow
    // pass. Do not use the last regular-world FF_AlphaRef here: it can change
    // between frames and would make this cache rebuild after PrepareRender.
    constexpr float alphaRef = 170.0f / 255.0f;
    if ( m_WorldShadowCasterCacheValid
        && m_WorldShadowCasterCacheGeneration == worldGeneration
        && m_WorldShadowCasterCacheAlphaRef == alphaRef
        && m_WorldShadowCasterCacheDrawWorldMesh == settings.DrawWorldMesh
        && m_WorldShadowCasterCacheDrawShadowGeometry == settings.DrawShadowGeometry ) {
        ++m_WorldShadowCasterCacheHits;
        return;
    }

    // Build the material-classified set once for the current world. The
    // previous implementation rebuilt a camera-union snapshot whenever a
    // cascade updated, which made the cache a per-frame list in practice.
    // Every cascade now performs its own mesh bbox test against this stable
    // metadata set. That also prevents a camera move from dropping casters
    // that were outside the previous union frustum.
    m_WorldShadowCasters.clear();
    m_WorldShadowCasterLookup.clear();
    for ( auto& [sectionCoordinate, sectionRow] : Engine::GAPI->GetWorldSections() ) {
        (void)sectionCoordinate;
        for ( auto& [rowCoordinate, section] : sectionRow ) {
            (void)rowCoordinate;
            for ( const auto& meshPair : section.WorldMeshes ) {
                const MeshKey& meshKey = meshPair.first;
                WorldMeshInfo* mesh = meshPair.second;
                if ( !mesh || !meshKey.Info || meshKey.Info->MaterialType != MaterialInfo::MT_None )
                    continue;

                zCMaterial* material = meshKey.Material;
                if ( !material )
                    continue;

                const int alphaFunc = material->GetAlphaFunc();
                if ( (alphaFunc > zMAT_ALPHA_FUNC_NONE && alphaFunc != zMAT_ALPHA_FUNC_TEST)
                    || (alphaFunc == zMAT_ALPHA_FUNC_NONE
                        && zColor( material->GetColor() ).bgra.alpha < 255) ) {
                    continue;
                }

                zCTexture* texture = material->GetTexture();
                if ( !texture )
                    texture = material->GetAniTexture();

                const bool alphaTest = texture && texture->HasAlphaChannel() && alphaRef > 0.0f;
                // Do not require alpha textures to be resident here. Residency is
                // transient; the render path checks it per cascade. Caching only
                // resident textures would permanently lose a valid caster until
                // the world/configuration generation changed.
                const ShadowWorldCaster caster = { mesh, &section, texture, alphaTest };
                m_WorldShadowCasters.push_back( caster );
                m_WorldShadowCasterLookup.emplace( mesh, caster );
            }
        }
    }

    m_WorldShadowCasterCacheGeneration = worldGeneration;
    m_WorldShadowCasterCacheAlphaRef = alphaRef;
    m_WorldShadowCasterCacheDrawWorldMesh = settings.DrawWorldMesh;
    m_WorldShadowCasterCacheDrawShadowGeometry = settings.DrawShadowGeometry;
    m_WorldShadowCasterCacheValid = true;
    ++m_WorldShadowCasterCacheBuilds;
}

void D3D11ShadowMap::BuildVisibleWorldShadowCasterCache( const Frustum& cullingFrustum ) {
    m_WorldShadowVisibleCasters.clear();
    if ( !Engine::GAPI || !m_WorldShadowCasterCacheValid
        || m_WorldShadowCasterLookup.empty() ) {
        return;
    }

    // Keep the expensive material classification persistent, but use the
    // existing section/BVH query to construct only the candidates needed by
    // this cascade. This avoids scanning every world mesh for every cascade.
    static thread_local std::vector<WorldMeshSectionInfo*> visibleSections;
    visibleSections.clear();
    Engine::GAPI->CollectVisibleSections( visibleSections, &cullingFrustum, false );
    for ( WorldMeshSectionInfo* section : visibleSections ) {
        if ( !section )
            continue;
        for ( const auto& meshPair : section->WorldMeshes ) {
            auto casterIt = m_WorldShadowCasterLookup.find( meshPair.second );
            if ( casterIt != m_WorldShadowCasterLookup.end() ) {
                m_WorldShadowVisibleCasters.push_back( casterIt->second );
            }
        }
    }
}

XRESULT D3D11ShadowMap::DrawWorldShadow( )
{
    auto graphicsEngine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);
    if ( !graphicsEngine || !m_context || !Engine::GAPI || Engine::IsShuttingDown()
        || !Engine::GAPI->IsWorldRenderCacheReady()
        || !Engine::GAPI->GetLoadedWorldInfo()
        || !Engine::GAPI->GetLoadedWorldInfo()->BspTree ) {
        return XR_FAILED;
    }
    auto _ = graphicsEngine->RecordGraphicsEvent( GE_NAME( "DrawWorldShadow" ) );
    ZoneScopedN( "DrawWorldShadow" );

    if ( !ShouldRenderCSMShadows() ) {
        return XR_SUCCESS;
    }

    auto& settings = Engine::GAPI->GetRendererState().RendererSettings;
    
    const int numCascades = settings.GetEffectiveShadowCascadeCount();
    settings.NumShadowCascades = numCascades;
    bool isOutdoor = Engine::GAPI->GetLoadedWorldInfo()->BspTree->GetBspTreeMode() == zBSP_MODE_OUTDOOR;

    if ( isOutdoor ) {
        // Keep the material-classified world caster metadata across lazy
        // cascade frames. BuildWorldShadowCasterCache invalidates it when the
        // world/configuration or relevant shadow settings change.
        BuildWorldShadowCasterCache();

        // Atlas cascades share one depth-stencil view and are always rebuilt
        // as a complete atlas (lazy updates are disabled for this backend).
        // Clear the atlas once before the four viewport passes.
        if ( m_useAtlas && m_shadowAtlas ) {
            if ( auto dsv = m_shadowAtlas->GetDepthStencilView() ) {
                m_context->ClearDepthStencilView( dsv, D3D11_CLEAR_DEPTH, 1.0f, 0 );
            }
        }

        for ( int cascadeIdx = 0; cascadeIdx < numCascades; ++cascadeIdx ) {
            // only update every Nth frame for higher cascades to save performance
            bool shouldUpdateCascade = m_ShouldUpdateCascade[cascadeIdx];

            if ( !shouldUpdateCascade ) continue;

            BuildVisibleWorldShadowCasterCache( m_CascadeCRs[cascadeIdx].frustum );

            // Render diese Cascade using the new CascadedShadowMap
            Engine::GAPI->SetCameraReplacementPtr( &m_CascadeCRs[cascadeIdx] );

            // Build render params
            RenderShadowmapsParams renderParams = {};
            renderParams.CameraPosition = m_WorldShadowPos;
            renderParams.Target = nullptr;
            renderParams.CullFront = true;
            renderParams.DontCull = false;
            renderParams.DSVOverwrite = GetCascadeDSV( static_cast<UINT>(cascadeIdx) );
            renderParams.DebugRTV = nullptr;
            renderParams.CascadeIndex = static_cast<int>(cascadeIdx);
            renderParams.CascadeSplits = m_CascadeSplits;
            renderParams.CascadeCameraReplacements = &m_CascadeCRs;
            renderParams.WorldShadowCasters = &m_WorldShadowVisibleCasters;

            // Atlas path: provide per-cascade viewport and skip per-cascade clear
            if ( m_useAtlas && m_shadowAtlas ) {
                renderParams.ViewportOverride = m_shadowAtlas->GetCascadeViewport( static_cast<UINT>(cascadeIdx) );
                renderParams.UseViewportOverride = true;
                renderParams.SkipClear = true;
            }

            RenderShadowmaps( renderParams );
            Engine::GAPI->SetCameraReplacementPtr( nullptr );
            m_RenderQueues[cascadeIdx]->Reset();
        }
    }

    // Restore gothics camera
    Engine::GAPI->SetCameraReplacementPtr( nullptr );
    
    return XR_SUCCESS;
}

XRESULT D3D11ShadowMap::DrawRainShadowmap() {
    auto graphicsEngine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);
    if ( !graphicsEngine || !graphicsEngine->Effects || !m_context
        || !Engine::GAPI || Engine::IsShuttingDown() ) {
        return XR_FAILED;
    }

    constexpr float activeThreshold = 0.00001f;
    constexpr float updateDistance = 1000.0f;
    constexpr float updateDistanceSq = updateDistance * updateDistance;
    static bool hasLastUpdatePosition = false;
    static XMFLOAT3 lastUpdatePosition = {};
    static zCWorld* lastWorld = nullptr;
    const float rainWeight = Engine::GAPI->GetRainFXWeight();
    const float puddleAccumulation = Engine::GAPI->GetPuddleAccumulation();
    zCWorld* currentWorld = nullptr;
    if ( oCGame* game = oCGame::GetGame() ) {
        currentWorld = game->_zCSession_world;
    }
    if ( rainWeight <= activeThreshold && puddleAccumulation <= activeThreshold ) {
        hasLastUpdatePosition = false;
        lastWorld = currentWorld;
        return XR_SUCCESS;
    }
    const XMFLOAT3 currentPosition = Engine::GAPI->GetCameraPosition();
    const XMVECTOR movement = XMVectorSubtract(
        XMLoadFloat3( &currentPosition ),
        XMLoadFloat3( &lastUpdatePosition ));
    const float movementDistanceSq = XMVectorGetX( XMVector3LengthSq( movement ));
    const bool updateRequired = !hasLastUpdatePosition
        || currentWorld != lastWorld
        || movementDistanceSq >= updateDistanceSq;
    if ( updateRequired ) {
        auto _ = graphicsEngine->RecordGraphicsEvent( GE_NAME( "DrawRainShadowmap" ) );
        ZoneScopedN( "DrawRainShadowmap" );
        graphicsEngine->Effects->DrawRainShadowmap();
        lastUpdatePosition = currentPosition;
        hasLastUpdatePosition = true;
        lastWorld = currentWorld;
    }
    return XR_SUCCESS;
}

XRESULT D3D11ShadowMap::DrawPointlightLights(
    std::vector<VobLightInfo*>& lights,
    RenderToTextureBuffer& color,
    RenderToTextureBuffer& normals,
    RenderToTextureBuffer& specular,
    RenderToTextureBuffer& depthCopy
    ) {
    if ( !m_context || !Engine::GAPI || Engine::IsShuttingDown() ) {
        return XR_FAILED;
    }

    auto& settings = Engine::GAPI->GetRendererState().RendererSettings;

    if ( m_TiledDeferred && settings.EnableTiledLighting ) {
        return m_TiledDeferred->DrawPointlightLights( lights, color, normals, specular, depthCopy );
    }

    return m_LegacyDeferred.DrawPointlightLights( lights, color, normals, specular, depthCopy );
}

XRESULT D3D11ShadowMap::DrawLighting( 
    std::vector<VobLightInfo*>& lights,
    RenderToTextureBuffer& color,
    RenderToTextureBuffer& normals,
    RenderToTextureBuffer& specular,
    RenderToTextureBuffer* rainExclusionMask,
    RenderToTextureBuffer& depthCopy) {
    auto graphicsEngine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);
    if ( !graphicsEngine || !m_context || !Engine::GAPI || Engine::IsShuttingDown()
        || !Engine::GAPI->IsWorldRenderCacheReady()
        || !graphicsEngine->GetHDRBackBufferPtr()
        || !graphicsEngine->GetHDRBackBufferPtr()->GetRenderTargetView()
        || !graphicsEngine->GetDepthBuffer()
        || !graphicsEngine->GetDepthBuffer()->GetDepthStencilView() ) {
        return XR_FAILED;
    }

    auto& settings = Engine::GAPI->GetRendererState().RendererSettings;

    graphicsEngine->SetDefaultStates();

    // Draw pointlight shadows
    DrawPointlightShadows(lights);

    const bool renderCsmShadows = settings.EnableShadows && ShouldRenderCSMShadows();
    if ( renderCsmShadows ) {
        DrawWorldShadow();
    }

    graphicsEngine->SetDefaultStates();

    DrawRainShadowmap();

    Engine::GAPI->SetFarPlane(static_cast<float>(settings.SectionDrawRadius) * WORLD_SECTION_SIZE );

    DrawPointlightLights(lights, color, normals, specular, depthCopy);

    m_context->OMSetRenderTargets( 1, graphicsEngine->GetHDRBackBuffer().GetRenderTargetView().GetAddressOf(),
        nullptr );

    ID3D11ShaderResourceView* srvs[3] = {
        color.GetShaderResView().Get(),
        normals.GetShaderResView().Get(),
        depthCopy.GetShaderResView().Get(),
    };
    m_context->PSSetShaderResources( 0, 3, srvs );

    srvs[0] = specular.GetShaderResView().Get();
    m_context->PSSetShaderResources( 7, 1, srvs );

    ID3D11ShaderResourceView* rainExclusionMaskSRV = rainExclusionMask ? rainExclusionMask->GetShaderResView().Get() : nullptr;
    m_context->PSSetShaderResources( 9, 1, &rainExclusionMaskSRV );

    DrawWorldLights();

    ID3D11ShaderResourceView* nullRainExclusionMask = nullptr;
    m_context->PSSetShaderResources( 9, 1, &nullRainExclusionMask );
    m_context->OMSetRenderTargets( 1, graphicsEngine->GetHDRBackBuffer().GetRenderTargetView().GetAddressOf(),
        graphicsEngine->GetDepthBuffer()->GetDepthStencilView().Get() );

    return XR_SUCCESS;
}



/** Renders the shadowmaps for the sun */
void D3D11ShadowMap::RenderShadowmaps( const RenderShadowmapsParams& params ) {

    auto graphicsEngine = (D3D11GraphicsEngine*)Engine::GraphicsEngine;
    if ( !graphicsEngine || !m_context || !Engine::GAPI || Engine::IsShuttingDown()
        || !Engine::GAPI->IsWorldRenderCacheReady() ) {
        return;
    }

    // We now assume that "target" always is something else than the world shadowmap
    UINT targetSize;
    if ( params.UseViewportOverride ) {
        targetSize = static_cast<UINT>( params.ViewportOverride.Width );
    } else if ( params.Target ) {
        targetSize = params.Target->GetSizeX();
    } else if ( m_useAtlas && m_shadowAtlas ) {
        targetSize = m_shadowAtlas->GetCascade0Size();
    } else {
        targetSize = m_cascadedShadowMap ? m_cascadedShadowMap->GetSize() : 0;
    }
    if ( targetSize == 0 ) {
        return;
    }

    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> dsvOverwrite = params.DSVOverwrite;
    if ( params.Target && !dsvOverwrite.Get() ) dsvOverwrite = params.Target->GetDepthStencilView().Get();
    const bool isNotWorldShadowMap = params.Target != nullptr;

    auto _ = graphicsEngine->RecordGraphicsEvent( GE_NAME( "RenderShadowmaps" ) );

    D3D11_VIEWPORT oldVP;
    UINT n = 1;
    m_context->RSGetViewports( &n, &oldVP );

    // Apply new viewport
    if ( params.UseViewportOverride ) {
        m_context->RSSetViewports( 1, &params.ViewportOverride );
    } else {
        D3D11_VIEWPORT vp;
        vp.TopLeftX = 0;
        vp.TopLeftY = 0;
        vp.MinDepth = 0.0f;
        vp.MaxDepth = 1.0f;
        vp.Width = static_cast<float>(targetSize);
        vp.Height = vp.Width;
        m_context->RSSetViewports( 1, &vp );
    }

    // Set the rendering stage
    D3D11ENGINE_RENDER_STAGE oldStage = graphicsEngine->GetRenderingStage();
    graphicsEngine->SetRenderingStage( DES_SHADOWMAP );

    // Clear and Bind the shadowmap

    ID3D11ShaderResourceView* nullShadowSRVs[15] = {};
    m_context->PSSetShaderResources( 3, 15, nullShadowSRVs );

    if ( !params.DebugRTV.Get() ) {
        m_context->OMSetRenderTargets( 0, nullptr, dsvOverwrite.Get() );
        Engine::GAPI->GetRendererState().BlendState.ColorWritesEnabled = false;
    } else {
        m_context->OMSetRenderTargets( 1, params.DebugRTV.GetAddressOf(), dsvOverwrite.Get() );
        Engine::GAPI->GetRendererState().BlendState.ColorWritesEnabled = true;
    }
    Engine::GAPI->GetRendererState().BlendState.SetDirty();

    // Render the main shadow map for the visible sun or moon.
    const DirectionalLightState directionalLight = GetDirectionalLightState();
    if ( isNotWorldShadowMap ||
        (directionalLight.Visibility > 0.001f &&
            Engine::GAPI->GetRendererState().RendererSettings.DrawShadowGeometry &&
            Engine::GAPI->GetRendererState().RendererSettings.EnableShadows) ) {
        if ( !params.SkipClear ) {
            m_context->ClearDepthStencilView( dsvOverwrite.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0 );
        }

        // Draw the world mesh without textures        

        XMVECTOR cameraPosition = XMLoadFloat3( &params.CameraPosition );
        ZoneScopedN( "Shadows::DrawCascade" );
        graphicsEngine->DrawWorldAroundForWorldShadow( cameraPosition, 2, params );

    } else if ( !params.SkipClear ) {
        m_context->ClearDepthStencilView(
            dsvOverwrite.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0 );
    }

    // Restore state
    graphicsEngine->SetRenderingStage( oldStage );
    m_context->RSSetViewports( 1, &oldVP );

    Engine::GAPI->SetFarPlane(
        Engine::GAPI->GetRendererState().RendererSettings.SectionDrawRadius *
        WORLD_SECTION_SIZE );
}

DS_ScreenQuadConstantBuffer D3D11ShadowMap::FillSunCSMConstantBuffer() const {
    DS_ScreenQuadConstantBuffer scb = {};
    if ( Engine::IsShuttingDown() || !Engine::GAPI ) {
        return scb;
    }

    auto& settings = Engine::GAPI->GetRendererState().RendererSettings;

    XMMATRIX viewRaw = Engine::GAPI->GetViewMatrixXM();
    XMMATRIX view = XMMatrixTranspose( viewRaw );

    auto& proj = Engine::GAPI->GetProjectionMatrix();

    scb.SQ_ProjParams = float4( 1.0f / proj._11, 1.0f / proj._22, proj._43, proj._33 );
    scb.SQ_JitterOffset = float2( proj._13 * 0.5f, -proj._23 * 0.5f );
    XMStoreFloat4x4( &scb.SQ_InvView, XMMatrixInverse( nullptr, viewRaw ) );
    XMStoreFloat4x4( &scb.SQ_View, viewRaw );

    static uint32_t frameCounter = 0;
    if ( proj._13 != 0 || proj._23 != 0 ) {
        scb.SQ_FrameIndex = frameCounter++;
    }

    const DirectionalLightState directionalLight = GetDirectionalLightState();
    XMStoreFloat3( scb.SQ_LightDirectionVS.toXMFLOAT3(),
        XMVector3TransformNormal( XMLoadFloat3( &directionalLight.Direction ), view ) );
    scb.SQ_LightColor = float4(
        directionalLight.Color.x,
        directionalLight.Color.y,
        directionalLight.Color.z,
        directionalLight.Strength );

    for ( size_t cascadeIdx = 0; cascadeIdx < MAX_CSM_CASCADES; ++cascadeIdx ) {
        XMStoreFloat4x4( &scb.SQ_ShadowViewProj[cascadeIdx],
            XMLoadFloat4x4( &m_CascadeCRs[cascadeIdx].ProjectionReplacement ) *
                XMLoadFloat4x4( &m_CascadeCRs[cascadeIdx].ViewReplacement ) );

        const XMVECTOR cascadePosition = XMLoadFloat3( &m_CascadeCRs[cascadeIdx].PositionReplacement );
        const XMVECTOR cascadeLookAt = XMLoadFloat3( &m_CascadeCRs[cascadeIdx].LookAtReplacement );
        const XMVECTOR cascadeDirectionDelta = XMVectorSubtract( cascadePosition, cascadeLookAt );
        const XMVECTOR cascadeLightDirection =
            XMVectorGetX( XMVector3LengthSq( cascadeDirectionDelta ) ) > 1.0e-8f
                ? XMVector3Normalize( cascadeDirectionDelta )
                : XMVector3Normalize( XMLoadFloat3( &directionalLight.Direction ) );
        XMStoreFloat4( scb.SQ_CascadeLightDirectionWS[cascadeIdx].toXMFLOAT4(),
            XMVectorSetW( cascadeLightDirection, 0.0f ) );
    }

    scb.SQ_ShadowmapSize = static_cast<float>( this->GetSizeX() );
    scb.SQ_CascadeShadowResolution = float4(
        static_cast<float>( GetCascadePixelSize( 0 ) ),
        static_cast<float>( GetCascadePixelSize( 1 ) ),
        static_cast<float>( GetCascadePixelSize( 2 ) ),
        static_cast<float>( GetCascadePixelSize( 3 ) ) );

    // Precompute world-space units per texel for the per-cascade receiver bias.
    // This matches the Kirides CSM path and avoids matrix reconstruction in the
    // pixel shader. In atlas mode use each cascade's local resolution.
    {
        const float mapSize = static_cast<float>( this->GetSizeX() );
        float* texelSize = scb.SQ_CascadeTexelSize.toPtr();
        for ( size_t i = 0; i < MAX_CSM_CASCADES; ++i ) {
            const XMFLOAT4X4& m = scb.SQ_ShadowViewProj[i];
            const float sx = std::sqrt( m._11 * m._11 + m._21 * m._21 + m._31 * m._31 );
            const float sy = std::sqrt( m._12 * m._12 + m._22 * m._22 + m._32 * m._32 );
            const float wx = sx > 1.0e-6f ? 2.0f / sx : 0.0f;
            const float wy = sy > 1.0e-6f ? 2.0f / sy : 0.0f;
            const float resolution = m_useAtlas
                ? static_cast<float>( GetCascadePixelSize( static_cast<UINT>( i ) ) )
                : mapSize;
            texelSize[i] = 0.5f * ( wx + wy ) / std::max( resolution, 1.0f );
        }
    }
    scb.SQ_ShadowAtlasSize = float4( 0, 0, 0, 0 );

    if ( m_useAtlas && m_shadowAtlas ) {
        for ( size_t i = 0; i < MAX_CSM_CASCADES; ++i ) {
            scb.SQ_CascadeAtlasRect[i] = m_shadowAtlas->GetCascadeUVRect( static_cast<UINT>( i ) );
        }
        scb.SQ_ShadowAtlasSize = float4(
            static_cast<float>( m_shadowAtlas->GetAtlasWidth() ),
            static_cast<float>( m_shadowAtlas->GetAtlasHeight() ), 0, 0 );
    }

    auto* graphicsEngine = dynamic_cast<D3D11GraphicsEngine*>( Engine::GraphicsEngine );
    if ( graphicsEngine && graphicsEngine->Effects ) {
        XMStoreFloat4x4( &scb.SQ_RainViewProj,
            XMLoadFloat4x4( &graphicsEngine->Effects->GetRainShadowmapCameraRepl().ProjectionReplacement ) *
            XMLoadFloat4x4( &graphicsEngine->Effects->GetRainShadowmapCameraRepl().ViewReplacement ) );
    }

    scb.SQ_ShadowStrength = settings.ShadowStrength;
    scb.SQ_ShadowAOStrength = settings.ShadowAOStrength;
    scb.SQ_WorldAOStrength = settings.WorldAOStrength;
    scb.SQ_ShadowSoftness = settings.ShadowSoftness;
    scb.SQ_LightSize = std::clamp( settings.PCSSLightSize, 0.005f, 0.5f );
    const int runtimeCascadeCount = settings.GetEffectiveShadowCascadeCount();
    const int runtimePCFLimit = settings.GetEffectiveShadowCascadePCFLimit( !m_useAtlas );
    const int runtimeShadowKernel = m_useAtlas
        && settings.CSMShadowKernel == GothicRendererSettings::E_ShadowKernelQuality::SHADOW_KERNEL_PCSS
        ? static_cast<int>( GothicRendererSettings::E_ShadowKernelQuality::SHADOW_KERNEL_PCF_MEDIUM )
        : static_cast<int>( settings.GetShadowKernelQuality() );
    scb.SQ_ShadowRuntimeParams = float4(
        settings.GetUsesTemporalReconstruction() ? 1.0f : 0.0f,
        static_cast<float>( runtimeShadowKernel ),
        settings.EnableShadows && !m_CsmSuppressedByHeavyRain ? 1.0f : 0.0f, 0.0f );
    scb.SQ_ShadowCascadeRuntimeParams = float4(
        static_cast<float>( runtimeCascadeCount ), static_cast<float>( runtimePCFLimit ), 0.0f, 0.0f );
    scb.SQ_ShadowCascadeSplits = float4(
        m_CascadeSplits.size() > 1 ? m_CascadeSplits[1] : 0.0f,
        m_CascadeSplits.size() > 2 ? m_CascadeSplits[2] : 0.0f,
        m_CascadeSplits.size() > 3 ? m_CascadeSplits[3] : 0.0f,
        m_CascadeSplits.size() > 4 ? m_CascadeSplits[4] : 0.0f );
    WorldInfo* worldInfo = Engine::GAPI->GetLoadedWorldInfo();
    if ( worldInfo && worldInfo->BspTree ) {
        auto bspTree = worldInfo->BspTree;
        if ( bspTree->GetBspTreeMode() == zBSP_MODE_INDOOR ) {
#if BUILD_GOTHIC_1_08k
            if ( worldInfo->WorldName == "ORCTEMPEL" )
                scb.SQ_ShadowStrength = 0.15f;
            else
                scb.SQ_ShadowStrength = 0.3f;
#else
            scb.SQ_ShadowStrength = 0.0f;
#endif
            scb.SQ_WorldAOStrength = 1.0f;
            scb.SQ_LightColor = float4( 1, 1, 1, DEFAULT_INDOOR_VOB_AMBIENT.x );
        }
    }

    return scb;
}

XRESULT D3D11ShadowMap::DrawWorldLights()
{
    auto graphicsEngine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);
    WorldInfo* worldInfo = Engine::GAPI ? Engine::GAPI->GetLoadedWorldInfo() : nullptr;
    if ( !graphicsEngine || !m_context || !Engine::GAPI || Engine::IsShuttingDown()
        || !Engine::GAPI->IsWorldRenderCacheReady()
        || !worldInfo || !worldInfo->BspTree
        || !graphicsEngine->Effects || !graphicsEngine->GetPfxRenderer()
        || !Engine::GAPI->GetSky() ) {
        return XR_FAILED;
    }

    auto _ = graphicsEngine->RecordGraphicsEvent( GE_NAME( "DrawWorldLights" ) );
    TracyD3D11ZoneCGX( "D3D11ShadowMap::DrawWorldLights");
    auto& settings = Engine::GAPI->GetRendererState().RendererSettings;

    Engine::GAPI->GetRendererState().BlendState.SetAdditiveBlending();
    Engine::GAPI->GetRendererState().BlendState.SetDirty();

    Engine::GAPI->GetRendererState().DepthState.DepthBufferCompareFunc = GothicDepthBufferStateInfo::CF_COMPARISON_ALWAYS;
    Engine::GAPI->GetRendererState().DepthState.SetDirty();

    Engine::GAPI->GetRendererState().RasterizerState.CullMode = GothicRasterizerStateInfo::CM_CULL_NONE;
    Engine::GAPI->GetRendererState().RasterizerState.SetDirty();

    // Modify light when raining
    float wetness = Engine::GAPI->GetSceneWetness();

    XMMATRIX viewRaw = Engine::GAPI->GetViewMatrixXM();
    XMMATRIX view = XMMatrixTranspose( viewRaw );

    bool isSnow = oCGame::GetGame()
        && oCGame::GetGame()->_zCSession_world
        && oCGame::GetGame()->_zCSession_world->GetSkyControllerOutdoor()
        && oCGame::GetGame()->_zCSession_world->GetSkyControllerOutdoor()->GetWeatherType() == zTWEATHER_SNOW;

    // Switch global light shader when raining
    if ( wetness > 0.0f && !isSnow ) {
        // Same shader, just has a DEFINE set to enable rain-related effects
        graphicsEngine->SetActivePixelShader( PShaderID::PS_DS_AtmosphericScattering_Rain );
    } else {
        graphicsEngine->SetActivePixelShader( PShaderID::PS_DS_AtmosphericScattering );
    }

    graphicsEngine->SetActiveVertexShader( VShaderID::VS_PFX );

    auto psAtmo = graphicsEngine->GetActivePS();
    auto vsPfx = graphicsEngine->GetActiveVS();
    if ( !psAtmo || !vsPfx ) {
        return XR_FAILED;
    }

    graphicsEngine->SetupVS_ExMeshDrawCall();

    GSky* sky = Engine::GAPI->GetSky();
    psAtmo->GetBuffer("Atmosphere").Update(&sky->GetAtmosphereCB()).Bind();

    auto& proj = Engine::GAPI->GetProjectionMatrix();
    DS_ScreenQuadConstantBuffer scb = {};
    scb.SQ_ProjParams = float4( 1.0f / proj._11, 1.0f / proj._22, proj._43, proj._33 );
    scb.SQ_JitterOffset = float2( proj._13 * 0.5f, -proj._23 * 0.5f );
    XMStoreFloat4x4( &scb.SQ_InvView, XMMatrixInverse( nullptr, viewRaw ) );
    XMStoreFloat4x4( &scb.SQ_View, viewRaw );

    static uint32_t frameCounter = 0;
    if ( proj._13 != 0 || proj._23 != 0) {
        // only when we have jitter in the frame
        scb.SQ_FrameIndex = frameCounter++;
    }

    const DirectionalLightState directionalLight = GetDirectionalLightState();
    XMStoreFloat3( scb.SQ_LightDirectionVS.toXMFLOAT3(),
        XMVector3TransformNormal( XMLoadFloat3( &directionalLight.Direction ), view ) );
    scb.SQ_LightColor = float4(
        directionalLight.Color.x,
        directionalLight.Color.y,
        directionalLight.Color.z,
        directionalLight.Strength );

    // CSM: Alle Cascade-Matrizen setzen

    for ( size_t cascadeIdx = 0; cascadeIdx < MAX_CSM_CASCADES; ++cascadeIdx ) {
        XMStoreFloat4x4( &scb.SQ_ShadowViewProj[cascadeIdx],
            XMLoadFloat4x4( &m_CascadeCRs[cascadeIdx].ProjectionReplacement ) *
                XMLoadFloat4x4( &m_CascadeCRs[cascadeIdx].ViewReplacement )
        );

        const XMVECTOR cascadePosition = XMLoadFloat3( &m_CascadeCRs[cascadeIdx].PositionReplacement );
        const XMVECTOR cascadeLookAt = XMLoadFloat3( &m_CascadeCRs[cascadeIdx].LookAtReplacement );
        const XMVECTOR cascadeDirectionDelta = XMVectorSubtract( cascadePosition, cascadeLookAt );
        const XMVECTOR cascadeLightDirection =
            XMVectorGetX( XMVector3LengthSq( cascadeDirectionDelta ) ) > 1.0e-8f
                ? XMVector3Normalize( cascadeDirectionDelta )
                : XMVector3Normalize( XMLoadFloat3( &directionalLight.Direction ) );
        XMStoreFloat4( scb.SQ_CascadeLightDirectionWS[cascadeIdx].toXMFLOAT4(),
            XMVectorSetW( cascadeLightDirection, 0.0f ) );
    }

    scb.SQ_ShadowmapSize = static_cast<float>( this->GetSizeX() );
    scb.SQ_CascadeShadowResolution = float4(
        static_cast<float>( GetCascadePixelSize( 0 ) ),
        static_cast<float>( GetCascadePixelSize( 1 ) ),
        static_cast<float>( GetCascadePixelSize( 2 ) ),
        static_cast<float>( GetCascadePixelSize( 3 ) ) );

    // Keep the second constant-buffer fill path identical to the regular CSM
    // path; both forward and fullscreen shadow sampling use this value.
    {
        const float mapSize = static_cast<float>( this->GetSizeX() );
        float* texelSize = scb.SQ_CascadeTexelSize.toPtr();
        for ( size_t i = 0; i < MAX_CSM_CASCADES; ++i ) {
            const XMFLOAT4X4& m = scb.SQ_ShadowViewProj[i];
            const float sx = std::sqrt( m._11 * m._11 + m._21 * m._21 + m._31 * m._31 );
            const float sy = std::sqrt( m._12 * m._12 + m._22 * m._22 + m._32 * m._32 );
            const float wx = sx > 1.0e-6f ? 2.0f / sx : 0.0f;
            const float wy = sy > 1.0e-6f ? 2.0f / sy : 0.0f;
            const float resolution = m_useAtlas
                ? static_cast<float>( GetCascadePixelSize( static_cast<UINT>( i ) ) )
                : mapSize;
            texelSize[i] = 0.5f * ( wx + wy ) / std::max( resolution, 1.0f );
        }
    }
    scb.SQ_ShadowAtlasSize = float4( 0, 0, 0, 0 );

    // Atlas mode stores per-cascade UV rectangles.
    if ( m_useAtlas && m_shadowAtlas ) {
        for ( size_t i = 0; i < MAX_CSM_CASCADES; ++i ) {
            scb.SQ_CascadeAtlasRect[i] = m_shadowAtlas->GetCascadeUVRect( static_cast<UINT>( i ) );
        }
        scb.SQ_ShadowAtlasSize = float4(
            static_cast<float>( m_shadowAtlas->GetAtlasWidth() ),
            static_cast<float>( m_shadowAtlas->GetAtlasHeight() ), 0, 0 );
    }

    // Get rain matrix
    
    XMStoreFloat4x4( &scb.SQ_RainViewProj,
        XMLoadFloat4x4( &graphicsEngine->Effects->GetRainShadowmapCameraRepl().ProjectionReplacement ) *
        XMLoadFloat4x4( &graphicsEngine->Effects->GetRainShadowmapCameraRepl().ViewReplacement )
    );

    scb.SQ_ShadowStrength = settings.ShadowStrength;
    scb.SQ_ShadowAOStrength = settings.ShadowAOStrength;
    scb.SQ_WorldAOStrength = settings.WorldAOStrength;
    scb.SQ_ShadowSoftness = settings.ShadowSoftness;
    scb.SQ_LightSize = std::clamp( settings.PCSSLightSize, 0.005f, 0.5f );
    const int runtimeCascadeCount = settings.GetEffectiveShadowCascadeCount();
    const int runtimePCFLimit = settings.GetEffectiveShadowCascadePCFLimit( !m_useAtlas );
    const int runtimeShadowKernel = m_useAtlas
        && settings.CSMShadowKernel == GothicRendererSettings::E_ShadowKernelQuality::SHADOW_KERNEL_PCSS
        ? static_cast<int>( GothicRendererSettings::E_ShadowKernelQuality::SHADOW_KERNEL_PCF_MEDIUM )
        : static_cast<int>( settings.GetShadowKernelQuality() );
    scb.SQ_ShadowRuntimeParams = float4(
        settings.GetUsesTemporalReconstruction() ? 1.0f : 0.0f,
        static_cast<float>( runtimeShadowKernel ),
        settings.EnableShadows && !m_CsmSuppressedByHeavyRain ? 1.0f : 0.0f, 0.0f );
    scb.SQ_ShadowCascadeRuntimeParams = float4(
        static_cast<float>( runtimeCascadeCount ), static_cast<float>( runtimePCFLimit ), 0.0f, 0.0f );
    scb.SQ_ShadowCascadeSplits = float4(
        m_CascadeSplits.size() > 1 ? m_CascadeSplits[1] : 0.0f,
        m_CascadeSplits.size() > 2 ? m_CascadeSplits[2] : 0.0f,
        m_CascadeSplits.size() > 3 ? m_CascadeSplits[3] : 0.0f,
        m_CascadeSplits.size() > 4 ? m_CascadeSplits[4] : 0.0f );
    // Modify lightsettings when indoor
    if ( auto bspTree = worldInfo->BspTree )
        if ( bspTree->GetBspTreeMode() == zBSP_MODE_INDOOR ) {
            // TODO: fix caves in Gothic 1 being way too dark. Remove this workaround.
#if BUILD_GOTHIC_1_08k
            // Kirides: Nah, just make it dark enough.
            if ( worldInfo->WorldName == "ORCTEMPEL" )
                scb.SQ_ShadowStrength = 0.15f;
            else
                scb.SQ_ShadowStrength = 0.3f;
#else
            // Turn off shadows
            scb.SQ_ShadowStrength = 0.0f;
#endif

            // Only use world AO
            scb.SQ_WorldAOStrength = 1.0f;
            // Darken the lights
            scb.SQ_LightColor = float4( 1, 1, 1, DEFAULT_INDOOR_VOB_AMBIENT.x );
        }

    psAtmo->GetBuffer( "DS_ScreenQuadConstantBuffer" ).Update( &scb ).Bind();

    // CSM: Bind the cascade array to a single slot (Texture2DArray)
    BindToPixelShader( m_context.Get(), TX_ShadowmapArray );

    if ( graphicsEngine->Effects->GetRainShadowmap() )
        graphicsEngine->Effects->GetRainShadowmap()->BindToPixelShader( m_context.Get(), TX_RainShadowmap );

    this->BindSampler( m_context.Get(), 2 );

    m_context->PSSetShaderResources( TX_ReflectionCube, 1, graphicsEngine->ReflectionCube.GetAddressOf() );
    graphicsEngine->GetDistortionTexture()->BindToPixelShader( TX_Distortion );
    graphicsEngine->GetBlueNoiseTexture()->BindToPixelShader( TX_BlueNoise512 );

    // CSM: Nur 1x rendern!
    graphicsEngine->GetPfxRenderer()->DrawFullScreenQuad();

    // Reset state
    static ID3D11ShaderResourceView* nullSrv[] = { nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr };
    m_context->PSSetShaderResources( 3, std::size( nullSrv ), nullSrv );

    return XR_SUCCESS;
}


/** Renders the shadowmaps for a pointlight */
void XM_CALLCONV D3D11ShadowMap::RenderShadowCube(
    FXMVECTOR position, float range,
    const RenderToDepthStencilBuffer& targetCube, Microsoft::WRL::ComPtr<ID3D11DepthStencilView> face,
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> debugRTV, bool cullFront, bool indoor, bool noNPCs,
    std::list<VobInfo*>* renderedVobs,
    std::list<SkeletalVobInfo*>* renderedMobs,
    std::vector<std::pair<MeshKey, MeshInfo*>>* worldMeshCache,
    bool clearDepth,
    unsigned int casterMask ) {

    auto graphicsEngine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);
    if ( !graphicsEngine || !m_context || !Engine::GAPI || Engine::IsShuttingDown()
        || !Engine::GAPI->IsWorldRenderCacheReady() ) {
        return;
    }

    D3D11_VIEWPORT oldVP;
    UINT n = 1;
    m_context->RSGetViewports( &n, &oldVP );

    // Apply new viewport
    D3D11_VIEWPORT vp;
    vp.TopLeftX = 0;
    vp.TopLeftY = 0;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    vp.Width = static_cast<float>(targetCube.GetSizeX());
    vp.Height = static_cast<float>(targetCube.GetSizeX());
    m_context->RSSetViewports( 1, &vp );

    bool useLayeredPath = false;
    if ( !face.Get() ) {
        if ( Engine::GAPI->GetRendererState().RendererSettings.DebugSettings.FeatureSet.UseLayeredRendering ) {
            useLayeredPath = true;
            face = targetCube.GetDepthStencilView().Get();

            // Set layered shader
            graphicsEngine->SetActiveVertexShader( VShaderID::VS_ExLayered );
        } else {
            // Set cubemap shader
            graphicsEngine->SetActiveGShader( GShaderID::GS_Cubemap );
            graphicsEngine->GetActiveGS().get()->Apply();
            face = targetCube.GetDepthStencilView().Get();

            graphicsEngine->SetActiveVertexShader( VShaderID::VS_ExCube );
        }
    }

    // Set the rendering stage
    D3D11ENGINE_RENDER_STAGE oldStage = graphicsEngine->GetRenderingStage();
    graphicsEngine->SetRenderingStage( DES_SHADOWMAP_CUBE );

    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
    m_context->PSSetShaderResources( 3, 1, srv.GetAddressOf() );

    if ( !debugRTV.Get() ) {
        m_context->OMSetRenderTargets( 0, nullptr, face.Get() );

        Engine::GAPI->GetRendererState().BlendState.ColorWritesEnabled =
            true;  // Should be false, but needs to be true for SV_Depth to work
        Engine::GAPI->GetRendererState().BlendState.SetDirty();
    } else {
        m_context->OMSetRenderTargets( 1, debugRTV.GetAddressOf(), face.Get() );

        Engine::GAPI->GetRendererState().BlendState.ColorWritesEnabled = true;
        Engine::GAPI->GetRendererState().BlendState.SetDirty();
    }

    if ( clearDepth ) {
        m_context->ClearDepthStencilView( face.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0 );
    }

    // Draw the world mesh without textures
    if ( useLayeredPath ) {
        graphicsEngine->DrawWorldAround_Layered( position, range, cullFront, indoor, noNPCs, renderedVobs,
            renderedMobs, worldMeshCache, casterMask );
    } else {
        graphicsEngine->DrawWorldAround( position, range, cullFront, indoor, noNPCs, renderedVobs,
            renderedMobs, worldMeshCache, casterMask );
    }

    // Restore state
    graphicsEngine->SetRenderingStage( oldStage );
    m_context->RSSetViewports( 1, &oldVP );
    m_context->GSSetShader( nullptr, nullptr, 0 );
    graphicsEngine->SetActiveVertexShader( VShaderID::VS_Ex );

    Engine::GAPI->SetFarPlane(
        Engine::GAPI->GetRendererState().RendererSettings.SectionDrawRadius *
        WORLD_SECTION_SIZE );

    graphicsEngine->SetRenderingStage( DES_MAIN );
}
