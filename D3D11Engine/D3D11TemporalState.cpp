#include "pch.h"
#include "D3D11TemporalState.h"

#include <FidelityFX/host/ffx_fsr3upscaler.h>
#include <algorithm>

#include "Engine.h"
#include "D3D11GraphicsEngine.h"
#include "GothicAPI.h"

D3D11TemporalState::D3D11TemporalState()
    : m_JitterIndex( 0 )
    , m_CurrentJitter( 0.0f, 0.0f )
    , m_CurrentJitterUnscaled( 0.0f, 0.0f ) {
    XMStoreFloat4x4( &m_UnjitteredViewProj, XMMatrixIdentity() );
}

void D3D11TemporalState::AdvanceJitter() {
    const INT2 renderSize = Engine::GraphicsEngine->GetResolution();
    const INT2 displaySize = Engine::GraphicsEngine->GetBackbufferResolution();
    const int32_t phaseCount = ffxFsr3UpscalerGetJitterPhaseCount( renderSize.x, displaySize.x );

    if ( phaseCount > 0 ) {
        m_JitterIndex = (m_JitterIndex + 1) % phaseCount;
    } else {
        m_JitterIndex = 0;
    }

    float jitterX = 0.0f;
    float jitterY = 0.0f;
    if ( phaseCount > 0 ) {
        ffxFsr3UpscalerGetJitterOffset( &jitterX, &jitterY, m_JitterIndex, phaseCount );
    }

    XMMATRIX view = Engine::GAPI->GetViewMatrixXM();
    Engine::GAPI->SetViewTransformXM( view );

    auto projF = Engine::GAPI->GetProjectionMatrix();
    projF._13 = 0.0f;
    projF._23 = 0.0f;

    XMMATRIX viewProj = XMMatrixMultiply( XMLoadFloat4x4( &projF ), view );
    XMStoreFloat4x4( &m_UnjitteredViewProj, viewProj );

    m_CurrentJitterUnscaled = XMFLOAT2( jitterX, jitterY );

    const float width = static_cast<float>(std::max( 1, renderSize.x ));
    const float height = static_cast<float>(std::max( 1, renderSize.y ));
    m_CurrentJitter = XMFLOAT2( jitterX / width, jitterY / height );

    projF._13 = m_CurrentJitter.x * 2.0f;
    projF._23 = -m_CurrentJitter.y * 2.0f;

    Engine::GAPI->GetRendererState().TransformState.TransformProj = projF;
}

void D3D11TemporalState::OnDisabled() {
    m_CurrentJitter = XMFLOAT2( 0.0f, 0.0f );
    m_CurrentJitterUnscaled = XMFLOAT2( 0.0f, 0.0f );
    m_JitterIndex = 0;

    XMMATRIX view = Engine::GAPI->GetViewMatrixXM();
    auto projF = Engine::GAPI->GetProjectionMatrix();
    projF._13 = 0.0f;
    projF._23 = 0.0f;

    XMMATRIX viewProj = XMMatrixMultiply( XMLoadFloat4x4( &projF ), view );
    XMStoreFloat4x4( &m_UnjitteredViewProj, viewProj );
}