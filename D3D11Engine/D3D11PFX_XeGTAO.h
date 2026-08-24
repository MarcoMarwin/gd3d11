#pragma once
#include "D3D11PFX_Effect.h"
#include <array>
#include <wrl/client.h>

class D3D11PFX_XeGTAO : public D3D11PFX_Effect {
public:
    explicit D3D11PFX_XeGTAO( D3D11PfxRenderer* renderer );
    ~D3D11PFX_XeGTAO() override = default;

    XRESULT Render( RenderToTextureBuffer* fxbuffer ) override { return XR_FAILED; }
    XRESULT Render( ID3D11ShaderResourceView* depthSRV,
                    ID3D11ShaderResourceView* normalsSRV,
                    ID3D11RenderTargetView* outputRTV );
    XRESULT RenderToAO( ID3D11ShaderResourceView* depthSRV,
                        ID3D11ShaderResourceView* normalsSRV );

    // Returns the pre-light mask when the current frame prepared one. The
    // renderer supplies a neutral white texture otherwise, so the shader
    // binding is always valid even when the A/B switch is off.
    ID3D11ShaderResourceView* GetLightingAOSRV() const;
    bool IsPreLightReady() const { return m_preLightReady; }
    void ResetPreLightState() { m_preLightReady = false; }

private:
    struct AOTermTexture {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> uintSRV;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> unormSRV;
        Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> uintUAV;
    };

    bool EnsureResources( UINT width, UINT height, bool needHybridResources, bool needAOMask );
    bool EnsureNeutralAOSRV();
    XRESULT RenderInternal( ID3D11ShaderResourceView* depthSRV,
                            ID3D11ShaderResourceView* normalsSRV,
                            ID3D11RenderTargetView* outputRTV,
                            bool outputAOMask );
    bool CreateAOTermTexture( UINT width, UINT height, AOTermTexture& texture );
    void ReleaseResources();

    UINT m_width = 0;
    UINT m_height = 0;
    UINT m_halfWidth = 0;
    UINT m_halfHeight = 0;
    uint32_t m_frameIndex = 0;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_workingDepth;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_workingDepthSRV;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_workingDepthHalfSRV;
    std::array<Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView>, 5> m_workingDepthUAVs;

    AOTermTexture m_aoTermA;
    AOTermTexture m_aoTermB;
    AOTermTexture m_halfAoTermA;
    AOTermTexture m_halfAoTermB;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_edges;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_edgesSRV;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> m_edgesUAV;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_halfEdges;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_halfEdgesSRV;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> m_halfEdgesUAV;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_hilbertLUT;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_hilbertLUTSRV;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> m_pointClampSampler;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_aoOutput;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_aoOutputRTV;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_aoOutputSRV;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_neutralAO;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_neutralAOSRV;
    bool m_preLightReady = false;
};
