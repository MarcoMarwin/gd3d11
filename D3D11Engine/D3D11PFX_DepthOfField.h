#pragma once
#include "D3D11PFX_Effect.h"
#include <wrl/client.h>

struct ID3D11Texture2D;
struct ID3D11ShaderResourceView;
struct ID3D11RenderTargetView;
struct ID3D11UnorderedAccessView;

class D3D11PFX_DepthOfField : public D3D11PFX_Effect {
public:
    explicit D3D11PFX_DepthOfField( D3D11PfxRenderer* rnd );
    ~D3D11PFX_DepthOfField() override = default;

    XRESULT Render( RenderToTextureBuffer* fxbuffer ) override { return XR_FAILED; }
    XRESULT Render( ID3D11ShaderResourceView* backbuffer );

private:
    XRESULT RenderCS( ID3D11ShaderResourceView* backbuffer );
    void UpdateAdaptiveFocus( float configuredNearDistance );

    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_FocusTexture;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_FocusSRV;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_FocusRTV;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> m_FocusUAV;
    bool m_Initialized = false;

    float m_AutoFocusBlend;
    float m_AutoFocusTransitionStart;
    float m_AutoFocusTransitionElapsed;
    float m_AutoFocusTransitionDuration;
    float m_NpcFocusHoldElapsed;
    float m_CameraStationaryElapsed;
    DirectX::XMFLOAT3 m_PreviousCameraPosition;
    DirectX::XMFLOAT3 m_PreviousCameraForward;
    bool m_HasPreviousCameraPose;
    bool m_NpcFocusSuppressed;
    bool m_AutoFocusSuppressed;
};