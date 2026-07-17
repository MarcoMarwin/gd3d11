#pragma once
#include "pch.h"

/** Helper structs for quickly creating render-to-texture buffers */
// DXGI_FORMAT_R8G8B8A8_UNORM
// 
const DXGI_FORMAT DXGI_FORMAT_ENGINE_SWAPCHAIN = DXGI_FORMAT_B8G8R8A8_UNORM;
const DXGI_FORMAT DXGI_FORMAT_ENGINE_DEFAULT = DXGI_FORMAT_B8G8R8A8_UNORM;

/** Struct for a texture that can be used as shader resource AND rendertarget */
struct RenderToTextureBuffer {
    ~RenderToTextureBuffer() = default;

    /** Creates the render-to-texture buffers */
    RenderToTextureBuffer( ID3D11Device* device,
        UINT sizeX,
        UINT sizeY,
        DXGI_FORMAT format,
        HRESULT* result = nullptr,
        DXGI_FORMAT rtvFormat = DXGI_FORMAT_UNKNOWN,
        DXGI_FORMAT srvFormat = DXGI_FORMAT_UNKNOWN,
        int mipLevels = 1,
        UINT arraySize = 1,
        uint32_t bindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE ) {
        if ( result ) {
            *result = E_FAIL;
        }
        if ( !device || sizeX == 0 || sizeY == 0 || format == DXGI_FORMAT_UNKNOWN
            || mipLevels <= 0 || (arraySize != 1 && arraySize != 6) ) {
            LogError() << "Invalid render-target texture description.";
            if ( result ) *result = E_INVALIDARG;
            return;
        }
        if ( bindFlags == 0 ) {
            bindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        }
        constexpr uint32_t supportedBindFlags = D3D11_BIND_RENDER_TARGET
            | D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        if ( (bindFlags & ~supportedBindFlags) != 0 ) {
            LogError() << "Unsupported render-target bind flags.";
            if ( result ) *result = E_INVALIDARG;
            return;
        }
        const uint32_t mipGenerationFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if ( mipLevels != 1 && (bindFlags & mipGenerationFlags) != mipGenerationFlags ) {
            LogError() << "Mip generation requires render-target and shader-resource bindings.";
            if ( result ) *result = E_INVALIDARG;
            return;
        }
        UINT maxMipLevels = 1;
        for ( UINT dimension = (std::max)(sizeX, sizeY); dimension > 1; dimension >>= 1 ) {
            ++maxMipLevels;
        }
        if ( static_cast<UINT>(mipLevels) > maxMipLevels ) {
            LogError() << "The requested mip count exceeds the texture dimensions.";
            if ( result ) *result = E_INVALIDARG;
            return;
        }
        if ( arraySize != 1 && (bindFlags & D3D11_BIND_UNORDERED_ACCESS) != 0 ) {
            LogError() << "Unordered-access cubemaps are not supported by RenderToTextureBuffer.";
            if ( result ) *result = E_INVALIDARG;
            return;
        }

        D3D11_TEXTURE2D_DESC textureDesc = CD3D11_TEXTURE2D_DESC(
            format, sizeX, sizeY, arraySize, static_cast<UINT>(mipLevels), static_cast<D3D11_BIND_FLAG>(bindFlags) );
        if ( arraySize == 6 ) {
            textureDesc.MiscFlags |= D3D11_RESOURCE_MISC_TEXTURECUBE;
        }
        if ( mipLevels != 1 ) {
            textureDesc.MiscFlags |= D3D11_RESOURCE_MISC_GENERATE_MIPS;
        }

        Microsoft::WRL::ComPtr<ID3D11Texture2D> newTexture;
        HRESULT hr = device->CreateTexture2D( &textureDesc, nullptr, newTexture.GetAddressOf() );
        if ( FAILED( hr ) ) {
            LogError() << "Failed to create render-target texture: 0x" << std::hex << static_cast<unsigned long>(hr);
            if ( result ) *result = hr;
            return;
        }

        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> newRTV;
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> newCubeRTVs[6];
        if ( (bindFlags & D3D11_BIND_RENDER_TARGET) != 0 ) {
            D3D11_RENDER_TARGET_VIEW_DESC viewDesc{};
            viewDesc.Format = rtvFormat != DXGI_FORMAT_UNKNOWN ? rtvFormat : format;
            if ( arraySize == 1 ) {
                viewDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
                viewDesc.Texture2D.MipSlice = 0;
            } else {
                viewDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
                viewDesc.Texture2DArray.MipSlice = 0;
                viewDesc.Texture2DArray.FirstArraySlice = 0;
                viewDesc.Texture2DArray.ArraySize = arraySize;
            }
            hr = device->CreateRenderTargetView( newTexture.Get(), &viewDesc, newRTV.GetAddressOf() );
            if ( FAILED( hr ) ) {
                LogError() << "Failed to create render-target view: 0x" << std::hex << static_cast<unsigned long>(hr);
                if ( result ) *result = hr;
                return;
            }

            if ( arraySize == 6 ) {
                viewDesc.Texture2DArray.ArraySize = 1;
                for ( UINT face = 0; face < 6; ++face ) {
                    viewDesc.Texture2DArray.FirstArraySlice = face;
                    hr = device->CreateRenderTargetView( newTexture.Get(), &viewDesc, newCubeRTVs[face].GetAddressOf() );
                    if ( FAILED( hr ) ) {
                        LogError() << "Failed to create cubemap render-target face: 0x" << std::hex << static_cast<unsigned long>(hr);
                        if ( result ) *result = hr;
                        return;
                    }
                }
            }
        }

        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> newSRV;
        if ( (bindFlags & D3D11_BIND_SHADER_RESOURCE) != 0 ) {
            D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc{};
            viewDesc.Format = srvFormat != DXGI_FORMAT_UNKNOWN ? srvFormat : format;
            if ( viewDesc.Format == DXGI_FORMAT_R32_TYPELESS ) {
                viewDesc.Format = DXGI_FORMAT_R32_FLOAT;
            }
            if ( arraySize == 1 ) {
                viewDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                viewDesc.Texture2D.MostDetailedMip = 0;
                viewDesc.Texture2D.MipLevels = static_cast<UINT>(mipLevels);
            } else {
                viewDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
                viewDesc.TextureCube.MostDetailedMip = 0;
                viewDesc.TextureCube.MipLevels = static_cast<UINT>(mipLevels);
            }
            hr = device->CreateShaderResourceView( newTexture.Get(), &viewDesc, newSRV.GetAddressOf() );
            if ( FAILED( hr ) ) {
                LogError() << "Failed to create render-target shader-resource view: 0x" << std::hex << static_cast<unsigned long>(hr);
                if ( result ) *result = hr;
                return;
            }
        }

        Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> newUAV;
        if ( (bindFlags & D3D11_BIND_UNORDERED_ACCESS) != 0 ) {
            D3D11_UNORDERED_ACCESS_VIEW_DESC viewDesc{};
            viewDesc.Format = format == DXGI_FORMAT_R32_TYPELESS ? DXGI_FORMAT_R32_FLOAT : format;
            viewDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
            viewDesc.Texture2D.MipSlice = 0;
            hr = device->CreateUnorderedAccessView( newTexture.Get(), &viewDesc, newUAV.GetAddressOf() );
            if ( FAILED( hr ) ) {
                LogError() << "Failed to create render-target unordered-access view: 0x" << std::hex << static_cast<unsigned long>(hr);
                if ( result ) *result = hr;
                return;
            }
        }

        Texture = std::move( newTexture );
        RenderTargetView = std::move( newRTV );
        ShaderResView = std::move( newSRV );
        UnorderedAccessView = std::move( newUAV );
        for ( UINT face = 0; face < 6; ++face ) {
            CubeMapRTVs[face] = std::move( newCubeRTVs[face] );
        }
        SizeX = sizeX;
        SizeY = sizeY;
        BindFlags = bindFlags;
        ArraySize = arraySize;
        if ( result ) *result = S_OK;
    }
    /** Binds the texture to the pixel shader */
    void BindToPixelShader( ID3D11DeviceContext* context, int slot ) {
        if ( context && ShaderResView && slot >= 0 && slot < D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT ) {
            context->PSSetShaderResources( slot, 1, ShaderResView.GetAddressOf() );
        }
    }

    const Microsoft::WRL::ComPtr<ID3D11Texture2D>& GetTexture() { return Texture; }
    const Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& GetShaderResView() { return ShaderResView; }
    const Microsoft::WRL::ComPtr<ID3D11RenderTargetView>& GetRenderTargetView() { return RenderTargetView; }
    const Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView>& GetUnorderedAccessView() { return UnorderedAccessView; }

    //void SetTexture( ID3D11Texture2D* tx ) { Texture = tx; }
    //void SetShaderResView( Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv ) { ShaderResView = srv.Get(); }
    //void SetRenderTargetView( Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv ) { RenderTargetView = rtv.Get(); }

    const Microsoft::WRL::ComPtr<ID3D11RenderTargetView>& GetRTVCubemapFace( UINT i ) const {
        static const Microsoft::WRL::ComPtr<ID3D11RenderTargetView> nullView;
        return i < 6 ? CubeMapRTVs[i] : nullView;
    }

    UINT GetSizeX() { return SizeX; }
    UINT GetSizeY() { return SizeY; }
    bool IsValid() const {
        if ( !Texture ) return false;
        if ( (BindFlags & D3D11_BIND_RENDER_TARGET) != 0 && !RenderTargetView ) return false;
        if ( (BindFlags & D3D11_BIND_SHADER_RESOURCE) != 0 && !ShaderResView ) return false;
        if ( (BindFlags & D3D11_BIND_UNORDERED_ACCESS) != 0 && !UnorderedAccessView ) return false;
        if ( ArraySize == 6 && (BindFlags & D3D11_BIND_RENDER_TARGET) != 0 ) {
            for ( const auto& face : CubeMapRTVs ) if ( !face ) return false;
        }
        return true;
    }
private:

    /** The Texture object */
    Microsoft::WRL::ComPtr<ID3D11Texture2D> Texture;

    /** Shader and rendertarget resource views */
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> ShaderResView;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> RenderTargetView;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> UnorderedAccessView;

    // Rendertargets for the cubemap-faces, if this is a cubemap
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> CubeMapRTVs[6];

    UINT SizeX = 0;
    UINT SizeY = 0;
    uint32_t BindFlags = 0;
    UINT ArraySize = 0;

};

/** Struct for a texture that can be used as shader resource AND depth stencil target */
struct RenderToDepthStencilBuffer {
    ~RenderToDepthStencilBuffer() = default;

    /** Wraps pre-existing resources without allocating  -  used for views into a shared TextureCubeArray */
    RenderToDepthStencilBuffer(
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture,
        Microsoft::WRL::ComPtr<ID3D11DepthStencilView> dsv,
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv,
        UINT SizeX, UINT SizeY )
        : Texture( std::move( texture ) ), DepthStencilView( std::move( dsv ) ),
          ShaderResView( std::move( srv ) ), SizeX( SizeX ), SizeY( SizeY ) {
    }

    /** Creates the render-to-texture buffers */
    RenderToDepthStencilBuffer( ID3D11Device* device, UINT sizeX, UINT sizeY, DXGI_FORMAT format, HRESULT* result = nullptr, DXGI_FORMAT dsvFormat = DXGI_FORMAT_UNKNOWN, DXGI_FORMAT srvFormat = DXGI_FORMAT_UNKNOWN, UINT arraySize = 1 ) {
        if ( result ) {
            *result = E_FAIL;
        }
        if ( !device || sizeX == 0 || sizeY == 0 || format == DXGI_FORMAT_UNKNOWN
            || (arraySize != 1 && arraySize != 6) ) {
            LogError() << "Invalid depth-stencil texture description.";
            if ( result ) *result = E_INVALIDARG;
            return;
        }

        D3D11_TEXTURE2D_DESC textureDesc = CD3D11_TEXTURE2D_DESC(
            format, sizeX, sizeY, arraySize, 1, D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE );
        if ( arraySize == 6 ) {
            textureDesc.MiscFlags |= D3D11_RESOURCE_MISC_TEXTURECUBE;
        }

        Microsoft::WRL::ComPtr<ID3D11Texture2D> newTexture;
        HRESULT hr = device->CreateTexture2D( &textureDesc, nullptr, newTexture.GetAddressOf() );
        if ( FAILED( hr ) ) {
            LogError() << "Failed to create depth-stencil texture: 0x" << std::hex << static_cast<unsigned long>(hr);
            if ( result ) *result = hr;
            return;
        }

        D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
        dsvDesc.Format = dsvFormat != DXGI_FORMAT_UNKNOWN ? dsvFormat : format;
        if ( arraySize == 1 ) {
            dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
            dsvDesc.Texture2D.MipSlice = 0;
        } else {
            dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
            dsvDesc.Texture2DArray.MipSlice = 0;
            dsvDesc.Texture2DArray.FirstArraySlice = 0;
            dsvDesc.Texture2DArray.ArraySize = arraySize;
        }

        Microsoft::WRL::ComPtr<ID3D11DepthStencilView> newDSV;
        hr = device->CreateDepthStencilView( newTexture.Get(), &dsvDesc, newDSV.GetAddressOf() );
        if ( FAILED( hr ) ) {
            LogError() << "Failed to create depth-stencil view: 0x" << std::hex << static_cast<unsigned long>(hr);
            if ( result ) *result = hr;
            return;
        }

        Microsoft::WRL::ComPtr<ID3D11DepthStencilView> newCubeDSVs[6];
        if ( arraySize == 6 ) {
            dsvDesc.Texture2DArray.ArraySize = 1;
            for ( UINT face = 0; face < 6; ++face ) {
                dsvDesc.Texture2DArray.FirstArraySlice = face;
                hr = device->CreateDepthStencilView( newTexture.Get(), &dsvDesc, newCubeDSVs[face].GetAddressOf() );
                if ( FAILED( hr ) ) {
                    LogError() << "Failed to create cubemap depth-stencil face: 0x" << std::hex << static_cast<unsigned long>(hr);
                    if ( result ) *result = hr;
                    return;
                }
            }
        }

        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = srvFormat != DXGI_FORMAT_UNKNOWN ? srvFormat : format;
        if ( arraySize == 1 ) {
            srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            srvDesc.Texture2D.MostDetailedMip = 0;
            srvDesc.Texture2D.MipLevels = 1;
        } else {
            srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
            srvDesc.TextureCube.MostDetailedMip = 0;
            srvDesc.TextureCube.MipLevels = 1;
        }

        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> newSRV;
        hr = device->CreateShaderResourceView( newTexture.Get(), &srvDesc, newSRV.GetAddressOf() );
        if ( FAILED( hr ) ) {
            LogError() << "Failed to create depth shader-resource view: 0x" << std::hex << static_cast<unsigned long>(hr);
            if ( result ) *result = hr;
            return;
        }

        Texture = std::move( newTexture );
        DepthStencilView = std::move( newDSV );
        ShaderResView = std::move( newSRV );
        for ( UINT face = 0; face < 6; ++face ) {
            CubeMapDSVs[face] = std::move( newCubeDSVs[face] );
        }
        SizeX = sizeX;
        SizeY = sizeY;
        ArraySize = arraySize;
        if ( result ) *result = S_OK;
    }
    void BindToVertexShader( const Microsoft::WRL::ComPtr<ID3D11DeviceContext1>& context, int slot ) {
        if ( context && ShaderResView && slot >= 0 && slot < D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT ) {
            context->VSSetShaderResources( slot, 1, ShaderResView.GetAddressOf() );
        }
    }

    void BindToPixelShader( const Microsoft::WRL::ComPtr<ID3D11DeviceContext1>& context, int slot ) {
        if ( context && ShaderResView && slot >= 0 && slot < D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT ) {
            context->PSSetShaderResources( slot, 1, ShaderResView.GetAddressOf() );
        }
    }

    const Microsoft::WRL::ComPtr<ID3D11Texture2D>& GetTexture() const { return Texture; }
    const Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& GetShaderResView() const { return ShaderResView; }
    const Microsoft::WRL::ComPtr<ID3D11DepthStencilView>& GetDepthStencilView() const { return DepthStencilView; }
    UINT GetSizeX() const { return SizeX; }
    UINT GetSizeY() const { return SizeY; }
    bool IsValid() const {
        if ( !Texture || !DepthStencilView || !ShaderResView || SizeX == 0 || SizeY == 0 ) return false;
        if ( ArraySize == 6 ) {
            for ( const auto& face : CubeMapDSVs ) if ( !face ) return false;
        }
        return true;
    }

    const Microsoft::WRL::ComPtr<ID3D11DepthStencilView>& GetDSVCubemapFace( UINT i ) const {
        static const Microsoft::WRL::ComPtr<ID3D11DepthStencilView> nullView;
        return i < ArraySize && i < 6 ? CubeMapDSVs[i] : nullView;
    }

    //void SetTexture( Microsoft::WRL::ComPtr<ID3D11Texture2D> tx ) { Texture = tx.Get(); }
    //void SetShaderResView( Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv ) { ShaderResView = srv.Get(); }
    //void SetDepthStencilView( Microsoft::WRL::ComPtr<ID3D11DepthStencilView> dsv ) { DepthStencilView = dsv.Get(); }

private:

    // The Texture object
    Microsoft::WRL::ComPtr<ID3D11Texture2D> Texture;

    UINT SizeX = 0;
    UINT SizeY = 0;
    UINT ArraySize = 0;

    // Shader and rendertarget resource views
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> ShaderResView;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> DepthStencilView;

    // Rendertargets for the cubemap-faces, if this is a cubemap
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> CubeMapDSVs[6];
};
