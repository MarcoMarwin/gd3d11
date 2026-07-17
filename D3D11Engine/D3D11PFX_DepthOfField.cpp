#include "pch.h"
#include "D3D11PFX_DepthOfField.h"
#include "Engine.h"
#include "D3D11GraphicsEngine.h"
#include "D3D11PfxRenderer.h"
#include "RenderToTextureBuffer.h"
#include "D3D11ShaderManager.h"
#include "D3D11VShader.h"
#include "D3D11PShader.h"
#include "D3D11CShader.h"
#include "D3D11ConstantBuffer.h"
#include "ConstantBufferStructs.h"
#include "GothicAPI.h"
#include "TexturePool.h"
#include "zCVob.h"
#include <algorithm>
#include <array>
#include <cmath>

extern bool FeatureLevel10Compatibility;

namespace {
    class ScopedTrackedRendererState {
    public:
        ScopedTrackedRendererState( D3D11GraphicsEngine* graphicsEngine, GothicAPI* gapi )
            : GraphicsEngine( graphicsEngine ),
            GAPI( gapi ),
            BlendState( gapi->GetRendererState().BlendState ),
            DepthState( gapi->GetRendererState().DepthState ),
            RasterizerState( gapi->GetRendererState().RasterizerState ) {
        }

        ~ScopedTrackedRendererState() {
            Restore();
        }

        bool Restore() {
            if ( Restored ) return true;
            Restored = true;
            if ( !GraphicsEngine || !GAPI ) return false;

            auto& state = GAPI->GetRendererState();
            state.BlendState = BlendState;
            state.DepthState = DepthState;
            state.RasterizerState = RasterizerState;
            state.BlendState.SetDirty();
            state.DepthState.SetDirty();
            state.RasterizerState.SetDirty();
            return GraphicsEngine->UpdateRenderStates() == XR_SUCCESS;
        }

    private:
        D3D11GraphicsEngine* GraphicsEngine;
        GothicAPI* GAPI;
        GothicBlendStateInfo BlendState;
        GothicDepthBufferStateInfo DepthState;
        GothicRasterizerStateInfo RasterizerState;
        bool Restored = false;
    };

    class ScopedPixelResourceClear {
    public:
        explicit ScopedPixelResourceClear( ID3D11DeviceContext* context )
            : Context( context ) {
        }

        ~ScopedPixelResourceClear() {
            if ( !Context ) return;
            std::array<ID3D11ShaderResourceView*, 4> nullResources{};
            Context->PSSetShaderResources(
                0, static_cast<UINT>(nullResources.size()), nullResources.data() );
        }

    private:
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> Context;
    };

    void ClearComputeIO( ID3D11DeviceContext* context ) {
        if ( !context ) return;
        std::array<ID3D11ShaderResourceView*, 4> nullResources{};
        ID3D11UnorderedAccessView* nullUAV = nullptr;
        context->CSSetShaderResources(
            0, static_cast<UINT>(nullResources.size()), nullResources.data() );
        context->CSSetUnorderedAccessViews( 0, 1, &nullUAV, nullptr );
    }

    class ScopedComputeBindingCleanup {
    public:
        explicit ScopedComputeBindingCleanup( ID3D11DeviceContext* context )
            : Context( context ) {
            if ( Context ) Context->CSGetSamplers( 0, 1, OldSampler.GetAddressOf() );
        }

        ~ScopedComputeBindingCleanup() {
            if ( !Context ) return;
            ClearComputeIO( Context.Get() );
            Context->CSSetShader( nullptr, nullptr, 0 );
            ID3D11SamplerState* oldSampler = OldSampler.Get();
            Context->CSSetSamplers( 0, 1, &oldSampler );
        }

    private:
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> Context;
        Microsoft::WRL::ComPtr<ID3D11SamplerState> OldSampler;
    };
}
static bool HasCenteredNearbyNpc( float maxViewDistance, bool relaxedCenter ) {
    if ( !Engine::GAPI || !Engine::GraphicsEngine
        || !std::isfinite( maxViewDistance ) || maxViewDistance <= 0.0f ) {
        return false;
    }

    zCVob* player = Engine::GAPI->GetPlayerVob();
    zCWorld* playerWorld = player ? player->GetHomeWorld() : nullptr;
    const auto& candidates = Engine::GAPI->GetSkeletalMeshVobs();
    const INT2 resolution = Engine::GraphicsEngine->GetBackbufferResolution();
    if ( resolution.x <= 0 || resolution.y <= 0 ) {
        return false;
    }

    XMFLOAT3 cameraPosition{};
    XMStoreFloat3( &cameraPosition, Engine::GAPI->GetCameraPositionXM() );
    if ( !std::isfinite( cameraPosition.x ) || !std::isfinite( cameraPosition.y )
        || !std::isfinite( cameraPosition.z ) ) {
        return false;
    }

    // A small cross of screen-centre rays is substantially more robust than
    // projecting one torso/head point. Very close characters fill the centre
    // even when their bounding-box centre lies outside the old NDC window.
    const float radius = relaxedCenter ? 0.035f : 0.025f;
    const float dx = static_cast<float>(resolution.x) * radius;
    const float dy = static_cast<float>(resolution.y) * radius;
    const float2 rayPixels[] = {
        { resolution.x * 0.5f, resolution.y * 0.5f },
        { resolution.x * 0.5f - dx, resolution.y * 0.5f },
        { resolution.x * 0.5f + dx, resolution.y * 0.5f },
        { resolution.x * 0.5f, resolution.y * 0.5f - dy },
        { resolution.x * 0.5f, resolution.y * 0.5f + dy },
    };

    auto intersectNpcBounds = [&]( const zTBBox3D& bounds, const XMFLOAT3& direction ) {
        float tMin = 0.0f;
        float tMax = maxViewDistance;
        const float origin[3] = { cameraPosition.x, cameraPosition.y, cameraPosition.z };
        const float dir[3] = { direction.x, direction.y, direction.z };
        const float minimum[3] = { bounds.Min.x, bounds.Min.y, bounds.Min.z };
        const float maximum[3] = { bounds.Max.x, bounds.Max.y, bounds.Max.z };

        for ( int axis = 0; axis < 3; ++axis ) {
            if ( !std::isfinite( origin[axis] ) || !std::isfinite( dir[axis] )
                || !std::isfinite( minimum[axis] ) || !std::isfinite( maximum[axis] )
                || minimum[axis] > maximum[axis] ) {
                return false;
            }
            if ( std::abs( dir[axis] ) < 1e-6f ) {
                if ( origin[axis] < minimum[axis] || origin[axis] > maximum[axis] ) {
                    return false;
                }
                continue;
            }

            const float inverseDirection = 1.0f / dir[axis];
            float nearT = (minimum[axis] - origin[axis]) * inverseDirection;
            float farT = (maximum[axis] - origin[axis]) * inverseDirection;
            if ( nearT > farT ) {
                std::swap( nearT, farT );
            }
            tMin = std::max( tMin, nearT );
            tMax = std::min( tMax, farT );
            if ( tMin > tMax ) {
                return false;
            }
        }
        return tMax >= 0.0f && tMin <= maxViewDistance;
    };

    for ( const float2& pixel : rayPixels ) {
        XMVECTOR rayPosition;
        XMVECTOR rayDirectionVector;
        Engine::GAPI->UnprojectXM( pixel, rayPosition, rayDirectionVector );
        const float directionLengthSq = XMVectorGetX(
            XMVector3LengthSq( rayDirectionVector ) );
        if ( !std::isfinite( directionLengthSq ) || directionLengthSq <= 1.0e-12f ) {
            continue;
        }
        XMFLOAT3 rayDirection{};
        XMStoreFloat3( &rayDirection, XMVector3Normalize( rayDirectionVector ) );

        for ( const SkeletalVobInfo* candidate : candidates ) {
            zCVob* npc = candidate ? candidate->Vob : nullptr;
            if ( !npc || npc == player || npc->GetVobType() != zVOB_TYPE_NSC
                || !npc->GetShowVisual() || (playerWorld && npc->GetHomeWorld() != playerWorld) ) {
                continue;
            }
            if ( intersectNpcBounds( npc->GetBBox(), rayDirection ) ) {
                return true;
            }
        }
    }

    return false;
}
static DepthOfFieldConstantBuffer BuildDepthOfFieldConstants( float adaptiveFocusBlend ) {
    const auto& settings = Engine::GAPI->GetRendererState().RendererSettings;
    const auto finiteOr = []( float value, float fallback ) {
        return std::isfinite( value ) ? value : fallback;
    };

    DepthOfFieldConstantBuffer cb{};
    cb.DoF_FocusDistance = (std::max)(finiteOr( settings.DoFFocusDistance, 1.0f ), 0.0f);
    cb.DoF_FocusRange = (std::max)(finiteOr( settings.DoFFocusRange, 1.0f ), 1.0f);
    const float radius = finiteOr( settings.DoFBokehRadius, 8.0f );
    const float strengthScale = std::clamp( radius / 8.0f, 0.004375f, 4.0f );
    const float nearBlurBlend = std::clamp( finiteOr( adaptiveFocusBlend, 1.0f ), 0.0f, 1.0f );
    cb.DoF_BokehRadius = 8.0f * strengthScale;
    cb.DoF_MaxBlur = 12.0f * strengthScale;

    const auto& proj = Engine::GAPI->GetProjectionMatrix();
    const float inverseX = std::abs( proj._11 ) > 1e-6f ? 1.0f / proj._11 : 0.0f;
    const float inverseY = std::abs( proj._22 ) > 1e-6f ? 1.0f / proj._22 : 0.0f;
    cb.DoF_ProjParams = float4(
        finiteOr( inverseX, 0.0f ), finiteOr( inverseY, 0.0f ),
        finiteOr( proj._34, 0.0f ), finiteOr( proj._33, 0.0f ) );
    cb.DoF_NearPlane = (std::max)(finiteOr(
        Engine::GAPI->GetRendererState().RendererInfo.NearPlane, 0.1f ), 0.01f);
    cb.DoF_FarPlane = (std::max)(finiteOr(
        Engine::GAPI->GetRendererState().RendererInfo.FarPlane, cb.DoF_NearPlane + 1.0f),
        cb.DoF_NearPlane + 1.0f);
    cb.DoF_NearBlurDistance = (std::max)(finiteOr(
        settings.DoFNearBlurDistance, cb.DoF_NearPlane), cb.DoF_NearPlane) * nearBlurBlend;
    cb.DoF_NearBlurStrength = std::clamp(
        finiteOr( settings.DoFNearBlurStrength, 0.0f ), 0.0f, 4.0f ) * nearBlurBlend;
    return cb;
}

D3D11PFX_DepthOfField::D3D11PFX_DepthOfField( D3D11PfxRenderer* rnd )
    : D3D11PFX_Effect( rnd )
    , m_AutoFocusBlend( 1.0f )
    , m_AutoFocusTransitionStart( 1.0f )
    , m_AutoFocusTransitionElapsed( 0.0f )
    , m_AutoFocusTransitionDuration( 2.0f )
    , m_NpcFocusHoldElapsed( 0.0f )
    , m_CameraStationaryElapsed( 0.0f )
    , m_PreviousCameraPosition( 0.0f, 0.0f, 0.0f )
    , m_PreviousCameraForward( 0.0f, 0.0f, 1.0f )
    , m_HasPreviousCameraPose( false )
    , m_NpcFocusSuppressed( false )
    , m_AutoFocusSuppressed( false ) {
    auto* engine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);
    const auto device = engine ? engine->GetDevice() : nullptr;
    const auto context = engine ? engine->GetContext() : nullptr;
    if ( !device || !context ) return;

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = 1;
    desc.Height = 1;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R32_FLOAT;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE
        | (FeatureLevel10Compatibility ? 0 : D3D11_BIND_UNORDERED_ACCESS);

    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> uav;
    if ( FAILED( device->CreateTexture2D( &desc, nullptr, texture.GetAddressOf() ) )
        || FAILED( device->CreateShaderResourceView(
            texture.Get(), nullptr, srv.GetAddressOf() ) )
        || FAILED( device->CreateRenderTargetView(
            texture.Get(), nullptr, rtv.GetAddressOf() ) ) ) {
        return;
    }

    if ( !FeatureLevel10Compatibility ) {
        D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
        uavDesc.Format = DXGI_FORMAT_R32_FLOAT;
        uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
        if ( FAILED( device->CreateUnorderedAccessView(
                texture.Get(), &uavDesc, uav.GetAddressOf() ) ) ) {
            return;
        }
    }

    const float initialFocus[4] = { 1.0f, 0.0f, 0.0f, 1.0f };
    context->ClearRenderTargetView( rtv.Get(), initialFocus );
    m_FocusTexture = std::move( texture );
    m_FocusSRV = std::move( srv );
    m_FocusRTV = std::move( rtv );
    m_FocusUAV = std::move( uav );
    m_Initialized = true;
}
void D3D11PFX_DepthOfField::UpdateAdaptiveFocus( float configuredNearDistance ) {
    if ( !Engine::GAPI ) return;
    const float rawDeltaTime = Engine::GAPI->GetFrameTimeSec();
    if ( !std::isfinite( rawDeltaTime ) || rawDeltaTime <= 0.0f ) return;
    const float deltaTime = std::min( rawDeltaTime, 0.1f );
    configuredNearDistance = std::isfinite( configuredNearDistance )
        ? std::max( configuredNearDistance, 0.0f ) : 0.0f;

    // Preserve the existing NPC-centre recognition and its enter/exit debounce.
    const bool relaxedCenter = m_NpcFocusSuppressed || m_AutoFocusBlend < 0.999f;
    const bool characterCentered = HasCenteredNearbyNpc(
        std::max( configuredNearDistance, 0.0f ), relaxedCenter );
    if ( characterCentered != m_NpcFocusSuppressed ) {
        m_NpcFocusHoldElapsed += deltaTime;
        const float requiredHold = characterCentered ? 1.0f : 0.5f;
        if ( m_NpcFocusHoldElapsed >= requiredHold ) {
            m_NpcFocusSuppressed = characterCentered;
            m_NpcFocusHoldElapsed = 0.0f;
        }
    } else {
        m_NpcFocusHoldElapsed = 0.0f;
    }

    XMFLOAT3 cameraPosition{};
    XMStoreFloat3( &cameraPosition, Engine::GAPI->GetCameraPositionXM() );
    XMVECTOR determinant{};
    const XMMATRIX inverseView = XMMatrixInverse(
        &determinant, Engine::GAPI->GetViewMatrixXM() );
    const float determinantValue = XMVectorGetX( determinant );
    const float forwardLengthSq = XMVectorGetX(
        XMVector3LengthSq( inverseView.r[2] ) );
    XMFLOAT3 cameraForward{};
    if ( std::isfinite( forwardLengthSq ) && forwardLengthSq > 1.0e-12f ) {
        XMStoreFloat3( &cameraForward, XMVector3Normalize( inverseView.r[2] ) );
    }

    const bool cameraPoseValid = std::isfinite( determinantValue )
        && std::abs( determinantValue ) > 1.0e-12f
        && std::isfinite( cameraPosition.x ) && std::isfinite( cameraPosition.y )
        && std::isfinite( cameraPosition.z ) && std::isfinite( cameraForward.x )
        && std::isfinite( cameraForward.y ) && std::isfinite( cameraForward.z );
    bool cameraStill = false;
    if ( cameraPoseValid && m_HasPreviousCameraPose ) {
        const XMVECTOR positionDelta = XMLoadFloat3( &cameraPosition )
            - XMLoadFloat3( &m_PreviousCameraPosition );
        const float movedDistance = XMVectorGetX( XMVector3Length( positionDelta ) );
        const float forwardDot = XMVectorGetX( XMVector3Dot(
            XMLoadFloat3( &cameraForward ), XMLoadFloat3( &m_PreviousCameraForward ) ) );
        cameraStill = std::isfinite( movedDistance ) && std::isfinite( forwardDot )
            && movedDistance <= 2.0f && forwardDot >= 0.99998f;
    }
    if ( cameraPoseValid ) {
        m_PreviousCameraPosition = cameraPosition;
        m_PreviousCameraForward = cameraForward;
    }
    m_HasPreviousCameraPose = cameraPoseValid;

    const bool dialogActive = !Engine::GAPI->DialogFinished();
    if ( !dialogActive && cameraStill ) {
        m_CameraStationaryElapsed = std::min( m_CameraStationaryElapsed + deltaTime, 1.0f );
    } else {
        m_CameraStationaryElapsed = 0.0f;
    }
    const bool cameraStationaryFocus = !dialogActive && m_CameraStationaryElapsed >= 0.25f;
    const bool suppressNearBlur = m_NpcFocusSuppressed || cameraStationaryFocus;

    if ( suppressNearBlur != m_AutoFocusSuppressed ) {
        m_AutoFocusSuppressed = suppressNearBlur;
        m_AutoFocusTransitionStart = m_AutoFocusBlend;
        m_AutoFocusTransitionElapsed = 0.0f;
        // Camera-stationary autofocus reacts quickly; NPC focus and every
        // return to configured blur retain their deliberately slower timing.
        const bool cameraOnlySuppression = suppressNearBlur
            && cameraStationaryFocus && !m_NpcFocusSuppressed;
        m_AutoFocusTransitionDuration = cameraOnlySuppression ? 1.0f : 2.0f;
    }

    const float targetBlend = m_AutoFocusSuppressed ? 0.0f : 1.0f;
    if ( std::abs( m_AutoFocusBlend - targetBlend ) <= 0.0001f ) {
        m_AutoFocusBlend = targetBlend;
        return;
    }

    m_AutoFocusTransitionElapsed += deltaTime;
    const float transition = std::clamp( m_AutoFocusTransitionElapsed / m_AutoFocusTransitionDuration, 0.0f, 1.0f );
    const float smoothTransition = transition * transition * (3.0f - 2.0f * transition);
    m_AutoFocusBlend = m_AutoFocusTransitionStart
        + (targetBlend - m_AutoFocusTransitionStart) * smoothTransition;
}
XRESULT D3D11PFX_DepthOfField::Render( ID3D11ShaderResourceView* backbuffer ) {
    if ( !backbuffer || !FxRenderer ) return XR_INVALID_ARG;

    auto* engine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);
    auto* gapi = Engine::GAPI;
    const auto context = engine ? engine->GetContext() : nullptr;
    auto* depthBuffer = engine ? engine->GetDepthBuffer() : nullptr;
    if ( !m_Initialized || !engine || !gapi || !context
        || !depthBuffer || !depthBuffer->IsValid()
        || !m_FocusTexture || !m_FocusSRV || !m_FocusRTV
        || (!FeatureLevel10Compatibility && !m_FocusUAV) ) {
        return XR_FAILED;
    }

    const INT2 resolution = engine->GetResolution();
    const DXGI_FORMAT format = engine->GetBackBufferFormat();
    if ( resolution.x < 2 || resolution.y < 2
        || format == DXGI_FORMAT_UNKNOWN ) {
        return XR_FAILED;
    }

    auto& rendererSettings = gapi->GetRendererState().RendererSettings;
    UpdateAdaptiveFocus( rendererSettings.DoFNearBlurDistance );
    if ( !FeatureLevel10Compatibility ) return RenderCS( backbuffer );

    TexturePool* texturePool = FxRenderer->GetTexturePool();
    if ( !texturePool ) return XR_FAILED;
    const INT2 halfResolution(
        std::max( 1, (resolution.x + 1) / 2 ),
        std::max( 1, (resolution.y + 1) / 2 ) );
    auto halfBuffer = texturePool->Acquire( TexturePool::Description{
        halfResolution.x, halfResolution.y, format } );
    auto compositeBuffer = FxRenderer->GetTempBuffer();
    if ( !halfBuffer || !compositeBuffer || !halfBuffer->IsValid()
        || !compositeBuffer->IsValid() ) {
        return XR_FAILED;
    }

    const auto fullscreenVS = engine->GetShaderManager().GetVShader( VShaderID::VS_PFX );
    const auto focusPS = engine->GetShaderManager().GetPShader(
        PShaderID::PS_PFX_DoF_FocusResolve );
    const auto blurPS = engine->GetShaderManager().GetPShader(
        rendererSettings.DoFGaussBlur
            ? PShaderID::PS_PFX_DoF_Gauss
            : PShaderID::PS_PFX_DoF );
    const auto compositePS = engine->GetShaderManager().GetPShader(
        PShaderID::PS_PFX_DoF_Composite );
    if ( !fullscreenVS || !focusPS || !blurPS || !compositePS
        || !fullscreenVS->GetShader() || !focusPS->GetShader()
        || !blurPS->GetShader() || !compositePS->GetShader() ) {
        return XR_FAILED;
    }

    const DepthOfFieldConstantBuffer constants =
        BuildDepthOfFieldConstants( m_AutoFocusBlend );
    auto focusConstants = focusPS->GetBuffer( "DepthOfFieldConstantBuffer" );
    auto blurConstants = blurPS->GetBuffer( "DepthOfFieldConstantBuffer" );
    auto compositeConstants = compositePS->GetBuffer( "DepthOfFieldConstantBuffer" );
    focusConstants.Update( &constants );
    blurConstants.Update( &constants );
    compositeConstants.Update( &constants );
    if ( !focusConstants.Succeeded() || !blurConstants.Succeeded()
        || !compositeConstants.Succeeded() ) {
        return XR_FAILED;
    }

    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> outputRTV;
    context->OMGetRenderTargets( 1, outputRTV.GetAddressOf(), nullptr );
    if ( !outputRTV ) return XR_FAILED;

    D3D11PFXOutputStateGuard outputState( context.Get() );
    if ( !outputState.IsValid() ) return XR_FAILED;
    ScopedTrackedRendererState rendererState( engine, gapi );
    ScopedPixelResourceClear clearResources( context.Get() );

    XRESULT result = [&]() -> XRESULT {
        engine->SetDefaultStates();
        if ( fullscreenVS->Apply() != XR_SUCCESS
            || focusPS->Apply() != XR_SUCCESS ) {
            return XR_FAILED;
        }
        focusConstants.Bind();
        if ( !focusConstants.Succeeded()
            || engine->SetViewport( ViewportInfo( 0, 0, INT2( 1, 1 ) ) ) != XR_SUCCESS ) {
            return XR_FAILED;
        }

        ID3D11RenderTargetView* focusRTV = m_FocusRTV.Get();
        context->OMSetRenderTargets( 1, &focusRTV, nullptr );
        if ( FxRenderer->DrawFullScreenQuad() != XR_SUCCESS ) return XR_FAILED;

        if ( blurPS->Apply() != XR_SUCCESS ) return XR_FAILED;
        blurConstants.Bind();
        if ( !blurConstants.Succeeded()
            || engine->SetViewport(
                ViewportInfo( 0, 0, halfResolution ) ) != XR_SUCCESS ) {
            return XR_FAILED;
        }

        ID3D11RenderTargetView* halfRTV = halfBuffer->GetRenderTargetView().Get();
        context->OMSetRenderTargets( 1, &halfRTV, nullptr );
        ID3D11ShaderResourceView* blurResources[3] = {
            backbuffer, depthBuffer->GetShaderResView().Get(), m_FocusSRV.Get()
        };
        context->PSSetShaderResources( 0, 3, blurResources );
        if ( FxRenderer->DrawFullScreenQuad() != XR_SUCCESS ) return XR_FAILED;

        std::array<ID3D11ShaderResourceView*, 4> nullResources{};
        context->PSSetShaderResources(
            0, static_cast<UINT>(nullResources.size()), nullResources.data() );

        if ( compositePS->Apply() != XR_SUCCESS ) return XR_FAILED;
        compositeConstants.Bind();
        if ( !compositeConstants.Succeeded()
            || engine->SetViewport(
                ViewportInfo( 0, 0, resolution ) ) != XR_SUCCESS ) {
            return XR_FAILED;
        }

        ID3D11RenderTargetView* compositeRTV =
            compositeBuffer->GetRenderTargetView().Get();
        context->OMSetRenderTargets( 1, &compositeRTV, nullptr );
        ID3D11ShaderResourceView* compositeResources[4] = {
            backbuffer, halfBuffer->GetShaderResView().Get(),
            depthBuffer->GetShaderResView().Get(), m_FocusSRV.Get()
        };
        context->PSSetShaderResources( 0, 4, compositeResources );
        if ( FxRenderer->DrawFullScreenQuad() != XR_SUCCESS ) return XR_FAILED;

        context->PSSetShaderResources(
            0, static_cast<UINT>(nullResources.size()), nullResources.data() );
        return FxRenderer->CopyTextureToRTV(
            compositeBuffer->GetShaderResView(), outputRTV, resolution );
    }();

    if ( !rendererState.Restore() ) result = XR_FAILED;
    return result;
}

XRESULT D3D11PFX_DepthOfField::RenderCS( ID3D11ShaderResourceView* backbuffer ) {
    if ( !backbuffer || !FxRenderer ) return XR_INVALID_ARG;

    auto* engine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);
    auto* gapi = Engine::GAPI;
    const auto context = engine ? engine->GetContext() : nullptr;
    auto* depthBuffer = engine ? engine->GetDepthBuffer() : nullptr;
    TexturePool* texturePool = FxRenderer->GetTexturePool();
    if ( !engine || !gapi || !context || !depthBuffer || !depthBuffer->IsValid()
        || !texturePool || !m_FocusSRV || !m_FocusUAV ) {
        return XR_FAILED;
    }

    const INT2 resolution = engine->GetResolution();
    const DXGI_FORMAT format = engine->GetBackBufferFormat();
    if ( resolution.x < 2 || resolution.y < 2
        || format == DXGI_FORMAT_UNKNOWN ) {
        return XR_FAILED;
    }

    const INT2 halfResolution(
        std::max( 1, (resolution.x + 1) / 2 ),
        std::max( 1, (resolution.y + 1) / 2 ) );
    constexpr uint32_t computeTextureFlags =
        D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
    auto halfBuffer = texturePool->Acquire( TexturePool::Description{
        halfResolution.x, halfResolution.y, format, computeTextureFlags } );
    auto compositeBuffer = texturePool->Acquire( TexturePool::Description{
        resolution.x, resolution.y, format, computeTextureFlags } );
    if ( !halfBuffer || !compositeBuffer || !halfBuffer->IsValid()
        || !compositeBuffer->IsValid() ) {
        return XR_FAILED;
    }

    auto& settings = gapi->GetRendererState().RendererSettings;
    const auto focusCS = engine->GetShaderManager().GetCShader(
        CShaderID::CS_PFX_DoF_FocusResolve );
    const auto blurCS = engine->GetShaderManager().GetCShader(
        settings.DoFGaussBlur ? CShaderID::CS_PFX_DoF_Gauss : CShaderID::CS_PFX_DoF );
    const auto compositeCS = engine->GetShaderManager().GetCShader(
        CShaderID::CS_PFX_DoF_Composite );
    ID3D11SamplerState* defaultSampler = engine->GetDefaultSamplerState();
    if ( !focusCS || !blurCS || !compositeCS || !defaultSampler
        || !focusCS->GetShader() || !blurCS->GetShader()
        || !compositeCS->GetShader() ) {
        return XR_FAILED;
    }

    const DepthOfFieldConstantBuffer constants =
        BuildDepthOfFieldConstants( m_AutoFocusBlend );
    auto focusConstants = focusCS->GetBuffer( "DepthOfFieldConstantBuffer" );
    auto blurConstants = blurCS->GetBuffer( "DepthOfFieldConstantBuffer" );
    auto compositeConstants = compositeCS->GetBuffer( "DepthOfFieldConstantBuffer" );
    focusConstants.Update( &constants );
    blurConstants.Update( &constants );
    compositeConstants.Update( &constants );
    if ( !focusConstants.Succeeded() || !blurConstants.Succeeded()
        || !compositeConstants.Succeeded() ) {
        return XR_FAILED;
    }

    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> outputRTV;
    context->OMGetRenderTargets( 1, outputRTV.GetAddressOf(), nullptr );
    if ( !outputRTV ) return XR_FAILED;

    D3D11PFXOutputStateGuard outputState( context.Get() );
    if ( !outputState.IsValid() ) return XR_FAILED;
    ScopedTrackedRendererState rendererState( engine, gapi );
    ScopedComputeBindingCleanup bindingCleanup( context.Get() );

    XRESULT result = [&]() -> XRESULT {
        engine->SetDefaultStates();
        context->OMSetRenderTargets( 0, nullptr, nullptr );
        ClearComputeIO( context.Get() );
        context->CSSetSamplers( 0, 1, &defaultSampler );

        if ( focusCS->Apply() != XR_SUCCESS ) return XR_FAILED;
        focusConstants.Bind();
        if ( !focusConstants.Succeeded() ) return XR_FAILED;
        ID3D11UnorderedAccessView* focusUAV = m_FocusUAV.Get();
        context->CSSetUnorderedAccessViews( 0, 1, &focusUAV, nullptr );
        context->Dispatch( 1, 1, 1 );
        ClearComputeIO( context.Get() );

        if ( blurCS->Apply() != XR_SUCCESS ) return XR_FAILED;
        blurConstants.Bind();
        if ( !blurConstants.Succeeded() ) return XR_FAILED;
        ID3D11ShaderResourceView* blurResources[3] = {
            backbuffer, depthBuffer->GetShaderResView().Get(), m_FocusSRV.Get()
        };
        ID3D11UnorderedAccessView* halfUAV =
            halfBuffer->GetUnorderedAccessView().Get();
        context->CSSetShaderResources( 0, 3, blurResources );
        context->CSSetUnorderedAccessViews( 0, 1, &halfUAV, nullptr );
        context->Dispatch(
            static_cast<UINT>((halfResolution.x + 7) / 8),
            static_cast<UINT>((halfResolution.y + 7) / 8), 1 );
        ClearComputeIO( context.Get() );

        if ( compositeCS->Apply() != XR_SUCCESS ) return XR_FAILED;
        compositeConstants.Bind();
        if ( !compositeConstants.Succeeded() ) return XR_FAILED;
        ID3D11ShaderResourceView* compositeResources[4] = {
            backbuffer, halfBuffer->GetShaderResView().Get(),
            depthBuffer->GetShaderResView().Get(), m_FocusSRV.Get()
        };
        ID3D11UnorderedAccessView* compositeUAV =
            compositeBuffer->GetUnorderedAccessView().Get();
        context->CSSetShaderResources( 0, 4, compositeResources );
        context->CSSetUnorderedAccessViews( 0, 1, &compositeUAV, nullptr );
        context->Dispatch(
            static_cast<UINT>((resolution.x + 7) / 8),
            static_cast<UINT>((resolution.y + 7) / 8), 1 );
        ClearComputeIO( context.Get() );
        context->CSSetShader( nullptr, nullptr, 0 );

        return FxRenderer->CopyTextureToRTV(
            compositeBuffer->GetShaderResView(), outputRTV, resolution );
    }();

    if ( !rendererState.Restore() ) result = XR_FAILED;
    return result;
}