#include "D3D11CascadedShadowMapBuffer.h"
#include "D3D11_Helpers.h"
#include "Logger.h"
#include <algorithm>

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
    for ( auto& rtv : m_cascadeMomentRTVs ) {
        rtv.Reset();
    }
    for ( auto& srv : m_cascadeMomentSRVs ) {
        srv.Reset();
    }
    m_momentSRV.Reset();
    m_momentTexture.Reset();
    m_momentNumCascades = 0;
    m_srv.Reset();
    m_texture.Reset();
}

HRESULT D3D11CascadedShadowMapBuffer::Init(
    const Microsoft::WRL::ComPtr<ID3D11Device1>& device,
    UINT size,
    UINT numCascades ) {

    m_device = device;
    m_numCascades = std::min<UINT>( numCascades, MAX_CSM_CASCADES );
    m_numCascades = std::max<UINT>( m_numCascades, 1 );

    return Resize( size );
}

HRESULT D3D11CascadedShadowMapBuffer::Resize( UINT size ) {
    if ( !m_device ) {
        LogError() << "CascadedShadowMap::Resize - Device not initialized";
        return E_FAIL;
    }

    // Clamp size to valid range
    m_size = std::max<UINT>( size, 512 );

    Release();

    HRESULT hr = S_OK;

    // Create the texture array
    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width = m_size;
    texDesc.Height = m_size;
    texDesc.MipLevels = 1;
    texDesc.ArraySize = m_numCascades;
    texDesc.Format = DXGI_FORMAT_R16_TYPELESS;
    texDesc.SampleDesc.Count = 1;
    texDesc.SampleDesc.Quality = 0;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
    texDesc.CPUAccessFlags = 0;
    texDesc.MiscFlags = 0;

    LE( m_device->CreateTexture2D( &texDesc, nullptr, m_texture.GetAddressOf() ) );
    if ( FAILED( hr ) || !m_texture ) {
        LogError() << "CascadedShadowMap::Resize - Failed to create texture array";
        return hr;
    }
    SetDebugName( m_texture.Get(), "CascadedShadowMap_TextureArray" );

    // Create per-slice depth stencil views
    for ( UINT i = 0; i < m_numCascades; ++i ) {
        D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
        dsvDesc.Format = DXGI_FORMAT_D16_UNORM;
        dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
        dsvDesc.Texture2DArray.MipSlice = 0;
        dsvDesc.Texture2DArray.FirstArraySlice = i;
        dsvDesc.Texture2DArray.ArraySize = 1;
        dsvDesc.Flags = 0;

        LE( m_device->CreateDepthStencilView( m_texture.Get(), &dsvDesc, m_cascadeDSVs[i].GetAddressOf() ) );
        if ( FAILED( hr ) || !m_cascadeDSVs[i] ) {
            LogError() << "CascadedShadowMap::Resize - Failed to create DSV for cascade " << i;
            return hr;
        }
        SetDebugName( m_cascadeDSVs[i].Get(), "CascadedShadowMap_DSV_Cascade" + std::to_string( i ) );
    }

    // Create shader resource view for the entire array
    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_R16_UNORM;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
    srvDesc.Texture2DArray.MostDetailedMip = 0;
    srvDesc.Texture2DArray.MipLevels = 1;
    srvDesc.Texture2DArray.FirstArraySlice = 0;
    srvDesc.Texture2DArray.ArraySize = m_numCascades;

    LE( m_device->CreateShaderResourceView( m_texture.Get(), &srvDesc, m_srv.GetAddressOf() ) );
    if ( FAILED( hr ) || !m_srv ) {
        LogError() << "CascadedShadowMap::Resize - Failed to create SRV";
        return hr;
    }
    SetDebugName( m_srv.Get(), "CascadedShadowMap_SRV" );

    LogInfo() << "CascadedShadowMap: Created " << m_numCascades << " cascades at " << m_size << "x" << m_size;

    return S_OK;
}

HRESULT D3D11CascadedShadowMapBuffer::EnsureMomentResources( UINT activeCascades ) {
    if ( !m_device || m_size == 0 || m_numCascades == 0 ) {
        return E_FAIL;
    }

    activeCascades = std::clamp<UINT>( activeCascades, 1, m_numCascades );
    if ( HasMomentResources( activeCascades ) ) {
        return S_OK;
    }

    for ( auto& rtv : m_cascadeMomentRTVs ) {
        rtv.Reset();
    }
    for ( auto& srv : m_cascadeMomentSRVs ) {
        srv.Reset();
    }
    m_momentSRV.Reset();
    m_momentTexture.Reset();
    m_momentNumCascades = 0;

    D3D11_TEXTURE2D_DESC textureDesc = {};
    textureDesc.Width = m_size;
    textureDesc.Height = m_size;
    textureDesc.MipLevels = 1;
    textureDesc.ArraySize = activeCascades;
    textureDesc.Format = DXGI_FORMAT_R16G16B16A16_UNORM;
    textureDesc.SampleDesc.Count = 1;
    textureDesc.Usage = D3D11_USAGE_DEFAULT;
    textureDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    textureDesc.MiscFlags = 0;

    HRESULT hr = m_device->CreateTexture2D( &textureDesc, nullptr, m_momentTexture.GetAddressOf() );
    if ( FAILED( hr ) || !m_momentTexture ) {
        LogError() << "CascadedShadowMap::EnsureMomentResources - Failed to create MSM moment texture array";
        return FAILED( hr ) ? hr : E_FAIL;
    }
    SetDebugName( m_momentTexture.Get(), "CascadedShadowMap_MomentTextureArray" );

    for ( UINT i = 0; i < activeCascades; ++i ) {
        D3D11_RENDER_TARGET_VIEW_DESC rtvDesc = {};
        rtvDesc.Format = DXGI_FORMAT_R16G16B16A16_UNORM;
        rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
        rtvDesc.Texture2DArray.MipSlice = 0;
        rtvDesc.Texture2DArray.FirstArraySlice = i;
        rtvDesc.Texture2DArray.ArraySize = 1;

        hr = m_device->CreateRenderTargetView(
            m_momentTexture.Get(), &rtvDesc, m_cascadeMomentRTVs[i].GetAddressOf() );
        if ( FAILED( hr ) || !m_cascadeMomentRTVs[i] ) {
            LogError() << "CascadedShadowMap::EnsureMomentResources - Failed to create MSM moment RTV for cascade " << i;
            return FAILED( hr ) ? hr : E_FAIL;
        }
        SetDebugName( m_cascadeMomentRTVs[i].Get(),
            "CascadedShadowMap_MomentRTV_Cascade" + std::to_string( i ) );

        D3D11_SHADER_RESOURCE_VIEW_DESC cascadeSrvDesc = {};
        cascadeSrvDesc.Format = DXGI_FORMAT_R16G16B16A16_UNORM;
        cascadeSrvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
        cascadeSrvDesc.Texture2DArray.MostDetailedMip = 0;
        cascadeSrvDesc.Texture2DArray.MipLevels = 1;
        cascadeSrvDesc.Texture2DArray.FirstArraySlice = i;
        cascadeSrvDesc.Texture2DArray.ArraySize = 1;
        hr = m_device->CreateShaderResourceView(
            m_momentTexture.Get(), &cascadeSrvDesc, m_cascadeMomentSRVs[i].GetAddressOf() );
        if ( FAILED( hr ) || !m_cascadeMomentSRVs[i] ) {
            LogError() << "CascadedShadowMap::EnsureMomentResources - Failed to create MSM moment SRV for cascade " << i;
            return FAILED( hr ) ? hr : E_FAIL;
        }
        SetDebugName( m_cascadeMomentSRVs[i].Get(),
            "CascadedShadowMap_MomentSRV_Cascade" + std::to_string( i ) );
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_R16G16B16A16_UNORM;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
    srvDesc.Texture2DArray.MostDetailedMip = 0;
    srvDesc.Texture2DArray.MipLevels = 1;
    srvDesc.Texture2DArray.FirstArraySlice = 0;
    srvDesc.Texture2DArray.ArraySize = activeCascades;

    hr = m_device->CreateShaderResourceView( m_momentTexture.Get(), &srvDesc, m_momentSRV.GetAddressOf() );
    if ( FAILED( hr ) || !m_momentSRV ) {
        LogError() << "CascadedShadowMap::EnsureMomentResources - Failed to create MSM moment SRV";
        return FAILED( hr ) ? hr : E_FAIL;
    }
    SetDebugName( m_momentSRV.Get(), "CascadedShadowMap_MomentSRV" );
    m_momentNumCascades = activeCascades;
    return S_OK;
}

ID3D11DepthStencilView* D3D11CascadedShadowMapBuffer::GetCascadeDSV( UINT cascadeIndex ) const {
    if ( cascadeIndex >= m_numCascades ) {
        return nullptr;
    }
    return m_cascadeDSVs[cascadeIndex].Get();
}

ID3D11RenderTargetView* D3D11CascadedShadowMapBuffer::GetCascadeMomentRTV( UINT cascadeIndex ) const {
    if ( cascadeIndex >= m_momentNumCascades ) {
        return nullptr;
    }
    return m_cascadeMomentRTVs[cascadeIndex].Get();
}

ID3D11ShaderResourceView* D3D11CascadedShadowMapBuffer::GetShaderResourceView() const {
    return m_srv.Get();
}

ID3D11ShaderResourceView* D3D11CascadedShadowMapBuffer::GetMomentShaderResourceView() const {
    return m_momentSRV.Get();
}

void D3D11CascadedShadowMapBuffer::BindToPixelShader( ID3D11DeviceContext1* context, UINT slot ) const {
    if ( m_srv ) {
        context->PSSetShaderResources( slot, 1, m_srv.GetAddressOf() );
    }
}

void D3D11CascadedShadowMapBuffer::BindMomentsToPixelShader( ID3D11DeviceContext1* context, UINT slot ) const {
    ID3D11ShaderResourceView* srv = m_momentSRV.Get();
    context->PSSetShaderResources( slot, 1, &srv );
}

bool D3D11CascadedShadowMapBuffer::HasMomentResources( UINT activeCascades ) const {
    if ( !m_momentTexture || !m_momentSRV || m_numCascades == 0 ) {
        return false;
    }

    activeCascades = std::clamp<UINT>( activeCascades, 1, m_numCascades );
    if ( m_momentNumCascades != activeCascades ) {
        return false;
    }

    for ( UINT i = 0; i < activeCascades; ++i ) {
        if ( !m_cascadeMomentRTVs[i] || !m_cascadeMomentSRVs[i] ) {
            return false;
        }
    }
    return true;
}

void D3D11CascadedShadowMapBuffer::BindToVertexShader( ID3D11DeviceContext1* context, UINT slot ) const {
    if ( m_srv ) {
        context->VSSetShaderResources( slot, 1, m_srv.GetAddressOf() );
    }
}
