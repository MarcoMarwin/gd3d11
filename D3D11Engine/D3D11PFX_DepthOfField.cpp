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
#include <cmath>

extern bool FeatureLevel10Compatibility;

static bool HasCenteredNearbyNpc( float maxViewDistance, bool relaxedCenter ) {
    if ( maxViewDistance <= 0.0f ) {
        return false;
    }

    zCVob* player = Engine::GAPI->GetPlayerVob();
    zCWorld* playerWorld = player ? player->GetHomeWorld() : nullptr;
    const auto& candidates = Engine::GAPI->GetSkeletalMeshVobs();
    const INT2 resolution = Engine::GraphicsEngine->GetBackbufferResolution();
    if ( resolution.x <= 0 || resolution.y <= 0 ) {
        return false;
    }

    XMFLOAT3 cameraPosition;
    XMStoreFloat3( &cameraPosition, Engine::GAPI->GetCameraPositionXM() );

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
        XMFLOAT3 rayDirection;
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
    auto& settings = Engine::GAPI->GetRendererState().RendererSettings;
    DepthOfFieldConstantBuffer cb = {};
    cb.DoF_FocusDistance = std::max( settings.DoFFocusDistance, 0.001f );
    cb.DoF_FocusRange = std::max( settings.DoFFocusRange, 0.001f );
    const float strengthScale = std::clamp( settings.DoFBokehRadius / 8.0f, 0.004375f, 4.0f );
    const float nearBlurBlend = std::clamp( adaptiveFocusBlend, 0.0f, 1.0f );
    // Adaptive focusing affects only the near field. The configured far blur
    // and its focus distance remain untouched at all times.
    cb.DoF_BokehRadius = 8.0f * strengthScale;
    cb.DoF_MaxBlur = 12.0f * strengthScale;
    auto& proj = Engine::GAPI->GetProjectionMatrix();
    cb.DoF_ProjParams = float4( 1.0f / proj._11, 1.0f / proj._22, proj._34, proj._33 );
    cb.DoF_NearPlane = std::max( Engine::GAPI->GetRendererState().RendererInfo.NearPlane, 0.001f );
    cb.DoF_FarPlane = std::max( Engine::GAPI->GetRendererState().RendererInfo.FarPlane, cb.DoF_NearPlane );
    cb.DoF_NearBlurDistance = std::max( settings.DoFNearBlurDistance * nearBlurBlend, 0.0f );
    cb.DoF_NearBlurStrength = std::max( settings.DoFNearBlurStrength * nearBlurBlend, 0.0f );
    return cb;
}

D3D11PFX_DepthOfField::D3D11PFX_DepthOfField( D3D11PfxRenderer* rnd )
    : D3D11PFX_Effect( rnd )
    , m_FocusIndex( 0 )
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

    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width = 1;
    texDesc.Height = 1;
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.Format = DXGI_FORMAT_R32_FLOAT;
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE
        | (FeatureLevel10Compatibility ? 0 : D3D11_BIND_UNORDERED_ACCESS);

    for ( int i = 0; i < 2; i++ ) {
        HRESULT hr = engine->GetDevice()->CreateTexture2D( &texDesc, nullptr, m_FocusTexture[i].GetAddressOf() );
        if ( FAILED( hr ) || m_FocusTexture[i].Get() == nullptr ) { LogError() << "DoF: CreateTexture2D failed for FocusTexture[" << i << "]. HRESULT: " << std::hex << hr; return; }

        hr = engine->GetDevice()->CreateShaderResourceView( m_FocusTexture[i].Get(), nullptr, m_FocusSRV[i].GetAddressOf() );
        if ( FAILED( hr ) || m_FocusSRV[i].Get() == nullptr ) { LogError() << "DoF: CreateSRV failed for FocusTexture[" << i << "]. HRESULT: " << std::hex << hr; return; }

        hr = engine->GetDevice()->CreateRenderTargetView( m_FocusTexture[i].Get(), nullptr, m_FocusRTV[i].GetAddressOf() );
        if ( FAILED( hr ) || m_FocusRTV[i].Get() == nullptr ) { LogError() << "DoF: CreateRTV failed for FocusTexture[" << i << "]. HRESULT: " << std::hex << hr; return; }

        if ( !FeatureLevel10Compatibility ) {
            D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
            uavDesc.Format = DXGI_FORMAT_R32_FLOAT;
            uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
            uavDesc.Texture2D.MipSlice = 0;
            hr = engine->GetDevice()->CreateUnorderedAccessView( m_FocusTexture[i].Get(), &uavDesc, m_FocusUAV[i].GetAddressOf() );
            if ( FAILED( hr ) || m_FocusUAV[i].Get() == nullptr ) { LogError() << "DoF: CreateUAV failed for FocusTexture[" << i << "]. HRESULT: " << std::hex << hr; return; }
        }
    }
}

void D3D11PFX_DepthOfField::UpdateAdaptiveFocus( float configuredNearDistance ) {
    const float deltaTime = std::clamp( Engine::GAPI->GetFrameTimeSec(), 0.0f, 0.1f );
    if ( deltaTime <= 0.0f ) {
        return;
    }

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

    XMFLOAT3 cameraPosition;
    XMStoreFloat3( &cameraPosition, Engine::GAPI->GetCameraPositionXM() );
    XMFLOAT3 cameraForward;
    const XMMATRIX inverseView = XMMatrixInverse( nullptr, Engine::GAPI->GetViewMatrixXM() );
    XMStoreFloat3( &cameraForward, XMVector3Normalize( inverseView.r[2] ) );

    bool cameraStill = false;
    if ( m_HasPreviousCameraPose ) {
        const XMVECTOR positionDelta = XMLoadFloat3( &cameraPosition ) - XMLoadFloat3( &m_PreviousCameraPosition );
        const float movedDistance = XMVectorGetX( XMVector3Length( positionDelta ) );
        const float forwardDot = XMVectorGetX( XMVector3Dot(
            XMLoadFloat3( &cameraForward ), XMLoadFloat3( &m_PreviousCameraForward ) ) );
        cameraStill = movedDistance <= 2.0f && forwardDot >= 0.99998f;
    }
    m_PreviousCameraPosition = cameraPosition;
    m_PreviousCameraForward = cameraForward;
    m_HasPreviousCameraPose = true;

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
        // Stationary-camera focus reacts quickly; other transitions use the slower duration.
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
XRESULT D3D11PFX_DepthOfField::Render( ID3D11ShaderResourceView* backbuffer, ID3D11ShaderResourceView* waterMaskSRV, ID3D11ShaderResourceView* specularSRV ) {
    if ( !backbuffer || !waterMaskSRV || !specularSRV
        || !Engine::GraphicsEngine || !Engine::GAPI || !FxRenderer ) {
        return XR_FAILED;
    }

    D3D11GraphicsEngine* engine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);
    if ( !engine || !engine->GetContext() || !engine->GetDepthBuffer() ) {
        return XR_FAILED;
    }

    engine->SetDefaultStates();

    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> oldRTV;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> oldDSV;
    engine->GetContext()->OMGetRenderTargets( 1, oldRTV.GetAddressOf(), oldDSV.GetAddressOf() );
    auto& rendererSettings = Engine::GAPI->GetRendererState().RendererSettings;
    const INT2 resolution = engine->GetResolution();
    if ( resolution.x <= 0 || resolution.y <= 0 ) {
        engine->GetContext()->OMSetRenderTargets( 1, oldRTV.GetAddressOf(), oldDSV.Get() );
        return XR_FAILED;
    }
    UpdateAdaptiveFocus( rendererSettings.DoFNearBlurDistance );

    if ( m_FocusSRV[0].Get() == nullptr || m_FocusSRV[1].Get() == nullptr || m_FocusRTV[0].Get() == nullptr || m_FocusRTV[1].Get() == nullptr
        || ( !FeatureLevel10Compatibility && ( m_FocusUAV[0].Get() == nullptr || m_FocusUAV[1].Get() == nullptr ) ) ) {
        return XR_FAILED;
    }

    if ( !FeatureLevel10Compatibility ) {
        auto res = RenderCS( backbuffer, waterMaskSRV, specularSRV );
        engine->GetContext()->OMSetRenderTargets( 1, oldRTV.GetAddressOf(), oldDSV.Get() );
        return res;
    }

    auto vs = engine->GetShaderManager().GetVShader( VShaderID::VS_PFX );
    auto focusPS = engine->GetShaderManager().GetPShader( PShaderID::PS_PFX_DoF_FocusResolve );
    auto blurPS = engine->GetShaderManager().GetPShader( rendererSettings.DoFGaussBlur ? PShaderID::PS_PFX_DoF_Gauss : PShaderID::PS_PFX_DoF );
    auto compositePS = engine->GetShaderManager().GetPShader( PShaderID::PS_PFX_DoF_Composite );
    if ( !vs || !focusPS || !blurPS || !compositePS ) {
        engine->GetContext()->OMSetRenderTargets( 1, oldRTV.GetAddressOf(), oldDSV.Get() );
        return XR_FAILED;
    }
    vs->Apply();


    DepthOfFieldConstantBuffer cb = BuildDepthOfFieldConstants( m_AutoFocusBlend );

    // --- Pass 0: Focus Resolve (1x1 deterministic focus) ---
    int prevIdx = m_FocusIndex;
    int curIdx = 1 - m_FocusIndex;

    focusPS->Apply();
    focusPS->GetBuffer( "DepthOfFieldConstantBuffer" ).Update( &cb ).Bind();

    D3D11_VIEWPORT oldVP;
    UINT numVP = 1;
    engine->GetContext()->RSGetViewports( &numVP, &oldVP );
    D3D11_VIEWPORT focusVP = { 0, 0, 1, 1, 0, 1 };
    engine->GetContext()->RSSetViewports( 1, &focusVP );

    engine->GetContext()->OMSetRenderTargets( 1, m_FocusRTV[curIdx].GetAddressOf(), nullptr );
    engine->GetDepthBuffer()->BindToPixelShader( engine->GetContext().Get(), 0 );
    engine->GetContext()->PSSetShaderResources( 1, 1, m_FocusSRV[prevIdx].GetAddressOf() );

    FxRenderer->DrawFullScreenQuad();

    m_FocusIndex = curIdx;
    engine->GetContext()->RSSetViewports( 1, &oldVP );

    ID3D11ShaderResourceView* nullSRV2[2] = { nullptr, nullptr };
    engine->GetContext()->PSSetShaderResources( 0, 2, nullSRV2 );

    // --- Pass 1: Half-res bokeh blur ---
    const INT2 res = resolution;
    const INT2 halfResolution( std::max( 1, res.x / 2 ), std::max( 1, res.y / 2 ) );
    DXGI_FORMAT bbufferFormat = engine->GetBackBufferFormat();
    auto halfBuffer = FxRenderer->GetTexturePool()->Acquire( TexturePool::Description{ halfResolution.x, halfResolution.y, bbufferFormat } );
    if ( !halfBuffer || !halfBuffer->GetRenderTargetView().Get() || !halfBuffer->GetShaderResView().Get() ) {
        engine->GetContext()->RSSetViewports( 1, &oldVP );
        engine->GetContext()->OMSetRenderTargets( 1, oldRTV.GetAddressOf(), oldDSV.Get() );
        return XR_FAILED;
    }
    D3D11_VIEWPORT halfVP = { 0, 0, static_cast<float>(halfResolution.x), static_cast<float>(halfResolution.y), 0, 1 };
    engine->GetContext()->RSSetViewports( 1, &halfVP );

    blurPS->Apply();
    blurPS->GetBuffer( "DepthOfFieldConstantBuffer" ).Update( &cb ).Bind();

    engine->GetContext()->OMSetRenderTargets( 1, halfBuffer->GetRenderTargetView().GetAddressOf(), nullptr );

    // t0 = full-res scene, t1 = full-res depth, t2 = focus (1x1)
    engine->GetContext()->PSSetShaderResources( 0, 1, &backbuffer );
    engine->GetDepthBuffer()->BindToPixelShader( engine->GetContext().Get(), 1 );
    engine->GetContext()->PSSetShaderResources( 2, 1, m_FocusSRV[m_FocusIndex].GetAddressOf() );
    engine->GetContext()->PSSetShaderResources( 3, 1, &waterMaskSRV );
    engine->GetContext()->PSSetShaderResources( 4, 1, &specularSRV );

    FxRenderer->DrawFullScreenQuad();

    ID3D11ShaderResourceView* nullSRVs[6] = { nullptr, nullptr, nullptr, nullptr, nullptr, nullptr };
    engine->GetContext()->PSSetShaderResources( 0, 6, nullSRVs );
    engine->GetContext()->RSSetViewports( 1, &oldVP );

    // --- Pass 2: Full-res composite (render to temp, then blit to avoid read-write hazard) ---
    auto compositeBuffer = FxRenderer->GetTempBuffer();
    if ( !compositeBuffer || !compositeBuffer->GetRenderTargetView().Get() || !compositeBuffer->GetShaderResView().Get() ) {
        engine->GetContext()->OMSetRenderTargets( 1, oldRTV.GetAddressOf(), oldDSV.Get() );
        return XR_FAILED;
    }
    compositePS->Apply();
    compositePS->GetBuffer( "DepthOfFieldConstantBuffer" ).Update( &cb ).Bind();

    engine->GetContext()->OMSetRenderTargets(
        1,
        compositeBuffer->GetRenderTargetView().GetAddressOf(),
        nullptr );

    // t0 = full-res scene, t1 = half-res blur, t2 = full-res depth, t3 = focus (1x1)
    engine->GetContext()->PSSetShaderResources( 0, 1, &backbuffer );
    ID3D11ShaderResourceView* halfSRV = halfBuffer->GetShaderResView().Get();
    engine->GetContext()->PSSetShaderResources( 1, 1, &halfSRV );
    engine->GetDepthBuffer()->BindToPixelShader( engine->GetContext().Get(), 2 );
    engine->GetContext()->PSSetShaderResources( 3, 1, m_FocusSRV[m_FocusIndex].GetAddressOf() );
    engine->GetContext()->PSSetShaderResources( 4, 1, &waterMaskSRV );
    engine->GetContext()->PSSetShaderResources( 5, 1, &specularSRV );

    FxRenderer->DrawFullScreenQuad();

    engine->GetContext()->PSSetShaderResources( 0, 6, nullSRVs );



    // Blit composite result to backbuffer
    FxRenderer->CopyTextureToRTV( compositeBuffer->GetShaderResView(), oldRTV, res );

    engine->GetContext()->OMSetRenderTargets( 1, oldRTV.GetAddressOf(), oldDSV.Get() );

    return XR_SUCCESS;
}

/** Compute shader path for FL11+ */
XRESULT D3D11PFX_DepthOfField::RenderLegacyCS( ID3D11ShaderResourceView* backbuffer, ID3D11ShaderResourceView* waterMaskSRV, ID3D11ShaderResourceView* specularSRV ) {
    D3D11GraphicsEngine* engine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);
    auto& context = engine->GetContext();

    engine->SetDefaultStates();

    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> oldRTV;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> oldDSV;
    context->OMGetRenderTargets( 1, oldRTV.GetAddressOf(), oldDSV.GetAddressOf() );

    ID3D11RenderTargetView* nullRtv = nullptr;
    engine->GetContext()->OMSetRenderTargets( 1, &nullRtv, nullptr );

    auto& rendererSettings = Engine::GAPI->GetRendererState().RendererSettings;
    const INT2 res = engine->GetResolution();
    if ( res.x <= 0 || res.y <= 0 ) {
        context->OMSetRenderTargets( 1, oldRTV.GetAddressOf(), oldDSV.Get() );
        return XR_FAILED;
    }
    DepthOfFieldConstantBuffer cb = BuildDepthOfFieldConstants( m_AutoFocusBlend );
    auto defaultSampler = engine->GetDefaultSamplerState();
    if ( !defaultSampler ) {
        context->OMSetRenderTargets( 1, oldRTV.GetAddressOf(), oldDSV.Get() );
        return XR_FAILED;
    }
    ID3D11UnorderedAccessView* nullUAV = nullptr;
    ID3D11ShaderResourceView* nullSRVs[6] = { nullptr, nullptr, nullptr, nullptr, nullptr, nullptr };

    // --- Pass 0: Focus Resolve (1x1 compute) ---
    int prevIdx = m_FocusIndex;
    int curIdx = 1 - m_FocusIndex;

    auto focusCS = engine->GetShaderManager().GetCShader( CShaderID::CS_PFX_DoF_FocusResolve );
    if ( !focusCS ) {
        context->OMSetRenderTargets( 1, oldRTV.GetAddressOf(), oldDSV.Get() );
        return XR_FAILED;
    }
    focusCS->Apply();
    focusCS->GetBuffer( "DepthOfFieldConstantBuffer" ).Update( &cb ).Bind();

    context->CSSetSamplers( 0, 1, &defaultSampler );

    ID3D11ShaderResourceView* focusSRVs[2] = {
        engine->GetDepthBuffer()->GetShaderResView().Get(),
        m_FocusSRV[prevIdx].Get()
    };
    context->CSSetShaderResources( 0, 2, focusSRVs );
    context->CSSetUnorderedAccessViews( 0, 1, m_FocusUAV[curIdx].GetAddressOf(), nullptr );

    context->Dispatch( 1, 1, 1 );

    context->CSSetUnorderedAccessViews( 0, 1, &nullUAV, nullptr );
    context->CSSetShaderResources( 0, 2, nullSRVs );

    m_FocusIndex = curIdx;

    // --- Pass 1: Half-res bokeh blur ---
    const INT2 halfResolution( std::max( 1, res.x / 2 ), std::max( 1, res.y / 2 ) );
    DXGI_FORMAT bbufferFormat = engine->GetBackBufferFormat();
    auto halfBuffer = FxRenderer->GetTexturePool()->Acquire( TexturePool::Description{ halfResolution.x, halfResolution.y, bbufferFormat, D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE } );
    auto blurCS = engine->GetShaderManager().GetCShader( rendererSettings.DoFGaussBlur ? CShaderID::CS_PFX_DoF_Gauss : CShaderID::CS_PFX_DoF );
    if ( !blurCS || !halfBuffer || !halfBuffer->GetUnorderedAccessView().Get() || !halfBuffer->GetShaderResView().Get() ) {
        context->CSSetShader( nullptr, nullptr, 0 );
        context->OMSetRenderTargets( 1, oldRTV.GetAddressOf(), oldDSV.Get() );
        return XR_FAILED;
    }
    blurCS->Apply();
    blurCS->GetBuffer( "DepthOfFieldConstantBuffer" ).Update( &cb ).Bind();

    context->CSSetSamplers( 0, 1, &defaultSampler );

    // t0 = full-res scene, t1 = full-res depth, t2 = focus (1x1)
    ID3D11ShaderResourceView* blurSRVs[5] = {
        backbuffer,
        engine->GetDepthBuffer()->GetShaderResView().Get(),
        m_FocusSRV[m_FocusIndex].Get(),
        waterMaskSRV,
        specularSRV
    };
    context->CSSetShaderResources( 0, 5, blurSRVs );
    context->CSSetUnorderedAccessViews( 0, 1, halfBuffer->GetUnorderedAccessView().GetAddressOf(), nullptr );

    context->Dispatch( (halfResolution.x + 7) / 8, (halfResolution.y + 7) / 8, 1 );

    context->CSSetUnorderedAccessViews( 0, 1, &nullUAV, nullptr );
    context->CSSetShaderResources( 0, 5, nullSRVs );

    // --- Pass 2: Full-res composite ---
    auto compositeBuffer = FxRenderer->GetTexturePool()->Acquire( TexturePool::Description{ res.x, res.y, bbufferFormat, D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE } );
    auto compositeCS = engine->GetShaderManager().GetCShader( CShaderID::CS_PFX_DoF_Composite );
    if ( !compositeCS || !compositeBuffer || !compositeBuffer->GetUnorderedAccessView().Get() || !compositeBuffer->GetShaderResView().Get() ) {
        context->CSSetShader( nullptr, nullptr, 0 );
        context->OMSetRenderTargets( 1, oldRTV.GetAddressOf(), oldDSV.Get() );
        return XR_FAILED;
    }
    compositeCS->Apply();
    compositeCS->GetBuffer( "DepthOfFieldConstantBuffer" ).Update( &cb ).Bind();

    context->CSSetSamplers( 0, 1, &defaultSampler );

    // t0 = full-res scene, t1 = half-res blur, t2 = full-res depth, t3 = focus (1x1)
    ID3D11ShaderResourceView* compositeSRVs[6] = {
        backbuffer,
        halfBuffer->GetShaderResView().Get(),
        engine->GetDepthBuffer()->GetShaderResView().Get(),
        m_FocusSRV[m_FocusIndex].Get(),
        waterMaskSRV,
        specularSRV
    };
    context->CSSetShaderResources( 0, 6, compositeSRVs );

    context->CSSetUnorderedAccessViews(
        0,
        1,
        compositeBuffer->GetUnorderedAccessView().GetAddressOf(),
        nullptr );

    context->Dispatch( (res.x + 7) / 8, (res.y + 7) / 8, 1 );

    context->CSSetUnorderedAccessViews(
        0,
        1,
        &nullUAV,
        nullptr );

    context->CSSetShaderResources( 0, 6, nullSRVs );
    context->CSSetShader( nullptr, nullptr, 0 );



    // Blit composite result to backbuffer
    FxRenderer->CopyTextureToRTV( compositeBuffer->GetShaderResView(), oldRTV, res );

    engine->GetContext()->OMSetRenderTargets( 1, oldRTV.GetAddressOf(), oldDSV.Get() );

    return XR_SUCCESS;
}

XRESULT D3D11PFX_DepthOfField::RenderFidelityFXStyleCS(
    ID3D11ShaderResourceView* backbuffer,
    ID3D11ShaderResourceView* waterMaskSRV,
    ID3D11ShaderResourceView* specularSRV ) {
    D3D11GraphicsEngine* engine = reinterpret_cast<D3D11GraphicsEngine*>( Engine::GraphicsEngine );
    if ( !engine || !Engine::GAPI || !FxRenderer
        || !backbuffer || !waterMaskSRV || !specularSRV ) {
        return XR_FAILED;
    }

    auto& context = engine->GetContext();
    auto* depthBuffer = engine->GetDepthBuffer();
    auto* texturePool = FxRenderer->GetTexturePool();
    if ( !context || !depthBuffer || !depthBuffer->GetShaderResView().Get()
        || !texturePool ) {
        return XR_FAILED;
    }

    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> oldRTV;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> oldDSV;
    context->OMGetRenderTargets( 1, oldRTV.GetAddressOf(), oldDSV.GetAddressOf() );
    if ( !oldRTV ) {
        context->OMSetRenderTargets( 1, oldRTV.GetAddressOf(), oldDSV.Get() );
        return XR_FAILED;
    }

    auto restoreOutput = [&]() {
        ID3D11ShaderResourceView* nullSRVs[8] = {};
        ID3D11UnorderedAccessView* nullUAVs[2] = {};
        context->CSSetShaderResources( 0, 8, nullSRVs );
        context->CSSetUnorderedAccessViews( 0, 2, nullUAVs, nullptr );
        context->CSSetShader( nullptr, nullptr, 0 );
        context->OMSetRenderTargets( 1, oldRTV.GetAddressOf(), oldDSV.Get() );
    };

    auto& rendererSettings = Engine::GAPI->GetRendererState().RendererSettings;
    const INT2 resolution = engine->GetResolution();
    if ( resolution.x <= 0 || resolution.y <= 0 ) {
        restoreOutput();
        return XR_FAILED;
    }

    if ( !m_FocusSRV[0] || !m_FocusSRV[1]
        || !m_FocusUAV[0] || !m_FocusUAV[1] ) {
        restoreOutput();
        return XR_FAILED;
    }

    auto focusCS = engine->GetShaderManager().GetCShader( CShaderID::CS_PFX_DoF_FocusResolve );
    auto downsampleCS = engine->GetShaderManager().GetCShader( CShaderID::CS_PFX_DoF_SplitDownsample );
    auto blurCS = engine->GetShaderManager().GetCShader(
        rendererSettings.DoFGaussBlur
            ? CShaderID::CS_PFX_DoF_SplitBlur_Gauss
            : CShaderID::CS_PFX_DoF_SplitBlur );
    auto compositeCS = engine->GetShaderManager().GetCShader( CShaderID::CS_PFX_DoF_SplitComposite );
    if ( !focusCS || !downsampleCS || !blurCS || !compositeCS
        || !focusCS->GetShader().Get() || !downsampleCS->GetShader().Get()
        || !blurCS->GetShader().Get() || !compositeCS->GetShader().Get() ) {
        restoreOutput();
        return XR_FAILED;
    }

    const UINT halfWidth = static_cast<UINT>( std::max( 1, ( resolution.x + 1 ) / 2 ) );
    const UINT halfHeight = static_cast<UINT>( std::max( 1, ( resolution.y + 1 ) / 2 ) );
    const uint32_t splitBindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
    constexpr DXGI_FORMAT splitColorFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;

    auto nearSource = texturePool->Acquire(
        TexturePool::Description{ static_cast<int>( halfWidth ), static_cast<int>( halfHeight ), splitColorFormat, splitBindFlags } );
    auto farSource = texturePool->Acquire(
        TexturePool::Description{ static_cast<int>( halfWidth ), static_cast<int>( halfHeight ), splitColorFormat, splitBindFlags } );
    auto nearBlur = texturePool->Acquire(
        TexturePool::Description{ static_cast<int>( halfWidth ), static_cast<int>( halfHeight ), splitColorFormat, splitBindFlags } );
    auto farBlur = texturePool->Acquire(
        TexturePool::Description{ static_cast<int>( halfWidth ), static_cast<int>( halfHeight ), splitColorFormat, splitBindFlags } );
    auto compositeBuffer = texturePool->Acquire(
        TexturePool::Description{ resolution.x, resolution.y, engine->GetBackBufferFormat(), splitBindFlags } );

    if ( !nearSource || !farSource || !nearBlur || !farBlur || !compositeBuffer
        || !nearSource->GetShaderResView().Get() || !nearSource->GetUnorderedAccessView().Get()
        || !farSource->GetShaderResView().Get() || !farSource->GetUnorderedAccessView().Get()
        || !nearBlur->GetShaderResView().Get() || !nearBlur->GetUnorderedAccessView().Get()
        || !farBlur->GetShaderResView().Get() || !farBlur->GetUnorderedAccessView().Get()
        || !compositeBuffer->GetShaderResView().Get() || !compositeBuffer->GetUnorderedAccessView().Get() ) {
        restoreOutput();
        return XR_FAILED;
    }

    engine->SetDefaultStates();
    ID3D11RenderTargetView* nullRTV = nullptr;
    context->OMSetRenderTargets( 1, &nullRTV, nullptr );

    DepthOfFieldConstantBuffer cb = BuildDepthOfFieldConstants( m_AutoFocusBlend );
    ID3D11SamplerState* defaultSampler = engine->GetDefaultSamplerState();
    if ( !defaultSampler ) {
        restoreOutput();
        return XR_FAILED;
    }

    ID3D11UnorderedAccessView* nullUAV = nullptr;
    ID3D11UnorderedAccessView* nullUAVs[2] = {};
    ID3D11ShaderResourceView* nullSRVs[8] = {};

    // Focus resolve remains the existing deterministic, temporally smoothed
    // control path. The new split blur only consumes its 1x1 result.
    const int previousFocusIndex = m_FocusIndex == 1 ? 1 : 0;
    const int currentFocusIndex = 1 - previousFocusIndex;
    focusCS->Apply();
    focusCS->GetBuffer( "DepthOfFieldConstantBuffer" ).Update( &cb ).Bind();
    context->CSSetSamplers( 0, 1, &defaultSampler );
    ID3D11ShaderResourceView* focusSRVs[2] = {
        depthBuffer->GetShaderResView().Get(),
        m_FocusSRV[previousFocusIndex].Get()
    };
    context->CSSetShaderResources( 0, 2, focusSRVs );
    context->CSSetUnorderedAccessViews( 0, 1, m_FocusUAV[currentFocusIndex].GetAddressOf(), nullptr );
    context->Dispatch( 1, 1, 1 );
    context->CSSetUnorderedAccessViews( 0, 1, &nullUAV, nullptr );
    context->CSSetShaderResources( 0, 2, nullSRVs );
    m_FocusIndex = currentFocusIndex;

    // Bilateral half-resolution preparation. Near and far colors remain in
    // separate resources, which is the key edge-quality improvement.
    downsampleCS->Apply();
    downsampleCS->GetBuffer( "DepthOfFieldConstantBuffer" ).Update( &cb ).Bind();
    context->CSSetSamplers( 0, 1, &defaultSampler );
    ID3D11ShaderResourceView* downsampleSRVs[5] = {
        backbuffer,
        depthBuffer->GetShaderResView().Get(),
        m_FocusSRV[m_FocusIndex].Get(),
        waterMaskSRV,
        specularSRV
    };
    context->CSSetShaderResources( 0, 5, downsampleSRVs );
    ID3D11UnorderedAccessView* downsampleUAVs[2] = {
        nearSource->GetUnorderedAccessView().Get(),
        farSource->GetUnorderedAccessView().Get()
    };
    context->CSSetUnorderedAccessViews( 0, 2, downsampleUAVs, nullptr );
    context->Dispatch( ( halfWidth + 7 ) / 8, ( halfHeight + 7 ) / 8, 1 );
    context->CSSetUnorderedAccessViews( 0, 2, nullUAVs, nullptr );
    context->CSSetShaderResources( 0, 5, nullSRVs );

    // Independent near/far ring-like blur at half resolution.
    blurCS->Apply();
    blurCS->GetBuffer( "DepthOfFieldConstantBuffer" ).Update( &cb ).Bind();
    context->CSSetSamplers( 0, 1, &defaultSampler );
    ID3D11ShaderResourceView* blurSRVs[2] = {
        nearSource->GetShaderResView().Get(),
        farSource->GetShaderResView().Get()
    };
    context->CSSetShaderResources( 0, 2, blurSRVs );
    ID3D11UnorderedAccessView* blurUAVs[2] = {
        nearBlur->GetUnorderedAccessView().Get(),
        farBlur->GetUnorderedAccessView().Get()
    };
    context->CSSetUnorderedAccessViews( 0, 2, blurUAVs, nullptr );
    context->Dispatch( ( halfWidth + 7 ) / 8, ( halfHeight + 7 ) / 8, 1 );
    context->CSSetUnorderedAccessViews( 0, 2, nullUAVs, nullptr );
    context->CSSetShaderResources( 0, 2, nullSRVs );

    // Full-resolution composition retains the existing sky-edge transition,
    // water/specular protection and near-over-far ordering.
    compositeCS->Apply();
    compositeCS->GetBuffer( "DepthOfFieldConstantBuffer" ).Update( &cb ).Bind();
    context->CSSetSamplers( 0, 1, &defaultSampler );
    ID3D11ShaderResourceView* compositeSRVs[7] = {
        backbuffer,
        depthBuffer->GetShaderResView().Get(),
        m_FocusSRV[m_FocusIndex].Get(),
        nearBlur->GetShaderResView().Get(),
        farBlur->GetShaderResView().Get(),
        waterMaskSRV,
        specularSRV
    };
    context->CSSetShaderResources( 0, 7, compositeSRVs );
    context->CSSetUnorderedAccessViews( 0, 1, compositeBuffer->GetUnorderedAccessView().GetAddressOf(), nullptr );
    context->Dispatch( ( static_cast<UINT>( resolution.x ) + 7 ) / 8,
        ( static_cast<UINT>( resolution.y ) + 7 ) / 8, 1 );
    context->CSSetUnorderedAccessViews( 0, 1, &nullUAV, nullptr );
    context->CSSetShaderResources( 0, 7, nullSRVs );
    context->CSSetShader( nullptr, nullptr, 0 );

    FxRenderer->CopyTextureToRTV( compositeBuffer->GetShaderResView(), oldRTV,
        resolution );
    context->OMSetRenderTargets( 1, oldRTV.GetAddressOf(), oldDSV.Get() );
    return XR_SUCCESS;
}

XRESULT D3D11PFX_DepthOfField::RenderCS(
    ID3D11ShaderResourceView* backbuffer,
    ID3D11ShaderResourceView* waterMaskSRV,
    ID3D11ShaderResourceView* specularSRV ) {
    const XRESULT splitResult = RenderFidelityFXStyleCS( backbuffer, waterMaskSRV, specularSRV );
    if ( splitResult == XR_SUCCESS ) {
        return splitResult;
    }

    return RenderLegacyCS( backbuffer, waterMaskSRV, specularSRV );
}
