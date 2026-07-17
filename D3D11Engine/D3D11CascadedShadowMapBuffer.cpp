#include "D3D11CascadedShadowMapBuffer.h"
#include "D3D11_Helpers.h"
#include "Logger.h"

D3D11CascadedShadowMapBuffer::D3D11CascadedShadowMapBuffer()
    : m_size( 0 )
    , m_numCascades( 0 ) {
}

D3D11CascadedShadowMapBuffer::~D3D11CascadedShadowMapBuffer() {
    Release();
}

void D3D11CascadedShadowMapBuffer::Release() {
    for ( auto& dsv : m_cascadeDSVs ) {
        dsv.Reset();
    }
    m_srv.Reset();
    m_texture.Reset();
}

HRESULT D3D11CascadedShadowMapBuffer::Init(
    const Microsoft::WRL::ComPtr<ID3D11Device1>& device,
    UINT size,
    UINT numCascades ) {

    if ( !device ) {
        return E_INVALIDARG;
    }

    const auto previousDevice = m_device;
    const UINT previousCascadeCount = m_numCascades;
    m_device = device;
    m_numCascades = std::clamp<UINT>( numCascades, 1, MAX_CSM_CASCADES );

    const HRESULT hr = Resize( size );
    if ( FAILED( hr ) ) {
        m_device = previousDevice;
        m_numCascades = previousCascadeCount;
    }
    return hr;
}

HRESULT D3D11CascadedShadowMapBuffer::Resize( UINT size ) {
    if ( !m_device || m_numCascades == 0 ) {
        LogError() << "CascadedShadowMap::Resize - Device not initialized";
        return E_FAIL;
    }

    const UINT maxTextureSize = m_device->GetFeatureLevel() >= D3D_FEATURE_LEVEL_11_0
        ? D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION
        : D3D10_REQ_TEXTURE2D_U_OR_V_DIMENSION;
    const UINT newSize = std::clamp<UINT>( size, 512, maxTextureSize );

    D3D11_TEXTURE2D_DESC texDesc{};
    texDesc.Width = newSize;
    texDesc.Height = newSize;
    texDesc.MipLevels = 1;
    texDesc.ArraySize = m_numCascades;
    texDesc.Format = DXGI_FORMAT_R16_TYPELESS;
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> newTexture;
    HRESULT hr = m_device->CreateTexture2D( &texDesc, nullptr, newTexture.GetAddressOf() );
    if ( FAILED( hr ) || !newTexture ) {
        LogError() << "CascadedShadowMap::Resize - Failed to create texture array: 0x"
            << std::hex << static_cast<unsigned long>(hr);
        return FAILED( hr ) ? hr : E_FAIL;
    }

    std::array<Microsoft::WRL::ComPtr<ID3D11DepthStencilView>, MAX_CSM_CASCADES> newCascadeDSVs;
    for ( UINT i = 0; i < m_numCascades; ++i ) {
        D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
        dsvDesc.Format = DXGI_FORMAT_D16_UNORM;
        dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
        dsvDesc.Texture2DArray.MipSlice = 0;
        dsvDesc.Texture2DArray.FirstArraySlice = i;
        dsvDesc.Texture2DArray.ArraySize = 1;

        hr = m_device->CreateDepthStencilView(
            newTexture.Get(), &dsvDesc, newCascadeDSVs[i].GetAddressOf() );
        if ( FAILED( hr ) || !newCascadeDSVs[i] ) {
            LogError() << "CascadedShadowMap::Resize - Failed to create DSV for cascade "
                << i << ": 0x" << std::hex << static_cast<unsigned long>(hr);
            return FAILED( hr ) ? hr : E_FAIL;
        }
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = DXGI_FORMAT_R16_UNORM;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
    srvDesc.Texture2DArray.MostDetailedMip = 0;
    srvDesc.Texture2DArray.MipLevels = 1;
    srvDesc.Texture2DArray.ArraySize = m_numCascades;

    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> newSrv;
    hr = m_device->CreateShaderResourceView(
        newTexture.Get(), &srvDesc, newSrv.GetAddressOf() );
    if ( FAILED( hr ) || !newSrv ) {
        LogError() << "CascadedShadowMap::Resize - Failed to create SRV: 0x"
            << std::hex << static_cast<unsigned long>(hr);
        return FAILED( hr ) ? hr : E_FAIL;
    }

    SetDebugName( newTexture.Get(), "CascadedShadowMap_TextureArray" );
    for ( UINT i = 0; i < m_numCascades; ++i ) {
        SetDebugName( newCascadeDSVs[i].Get(),
            "CascadedShadowMap_DSV_Cascade" + std::to_string( i ) );
    }
    SetDebugName( newSrv.Get(), "CascadedShadowMap_SRV" );

    m_texture = std::move( newTexture );
    m_cascadeDSVs = std::move( newCascadeDSVs );
    m_srv = std::move( newSrv );
    m_size = newSize;

    LogInfo() << "CascadedShadowMap: Created " << m_numCascades
        << " cascades at " << m_size << "x" << m_size;
    return S_OK;
}
ID3D11DepthStencilView* D3D11CascadedShadowMapBuffer::GetCascadeDSV( UINT cascadeIndex ) const {
    if ( cascadeIndex >= m_numCascades ) {
        return nullptr;
    }
    return m_cascadeDSVs[cascadeIndex].Get();
}

ID3D11ShaderResourceView* D3D11CascadedShadowMapBuffer::GetShaderResourceView() const {
    return m_srv.Get();
}

void D3D11CascadedShadowMapBuffer::BindToPixelShader( ID3D11DeviceContext1* context, UINT slot ) const {
    if ( context && m_srv && slot < D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT ) {
        context->PSSetShaderResources( slot, 1, m_srv.GetAddressOf() );
    }
}

void D3D11CascadedShadowMapBuffer::BindToVertexShader( ID3D11DeviceContext1* context, UINT slot ) const {
    if ( context && m_srv && slot < D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT ) {
        context->VSSetShaderResources( slot, 1, m_srv.GetAddressOf() );
    }
}
