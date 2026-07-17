#pragma once
#include "../pch.h"

#include <d3d11.h>
#include <wrl/client.h>
#include <DirectXMath.h>
#include <string>

#include "../TexturePool.h"

class D3D11SMAA {
public:
    D3D11SMAA(
        ID3D11Device* device,
        ID3D11DeviceContext* context,
        std::wstring shaderPath,
        std::wstring areaTexPath,
        std::wstring searchTexPath )
        : m_device( device ),
        m_context( context ),
        m_shaderPath( std::move( shaderPath ) ),
        m_areaTexPath( std::move( areaTexPath ) ),
        m_searchTexPath( std::move( searchTexPath ) ) {
    }

    ~D3D11SMAA() = default;

    bool Init();
    void OnResize( int width, int height );
    bool Render(
        ID3D11ShaderResourceView* inputSRV,
        ID3D11RenderTargetView* outputRTV,
        TexturePool* pool );
    void ReleaseResources();

private:
    struct SMAAConstants {
        DirectX::XMFLOAT4 RT_Metrics;
    };

    bool HasResources() const;

    Microsoft::WRL::ComPtr<ID3D11Device> m_device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_context;

    Microsoft::WRL::ComPtr<ID3D11VertexShader> m_vsEdge;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_psLumaEdge;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> m_vsBlend;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_psBlend;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> m_vsNeighbor;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_psNeighbor;

    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_areaTexSRV;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_searchTexSRV;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_constantBuffer;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> m_samplerLinear;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> m_samplerPoint;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> m_rasterizerState;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> m_disableDepthState;
    Microsoft::WRL::ComPtr<ID3D11BlendState> m_blendState;

    int m_width = 0;
    int m_height = 0;
    bool m_initialized = false;
    bool m_initializationFailed = false;
    bool m_metricsDirty = true;
    std::wstring m_shaderPath;
    std::wstring m_areaTexPath;
    std::wstring m_searchTexPath;
};
