#include "pch.h"
#include "D3D11Texture.h"
#include "Engine.h"
#include "D3D11GraphicsEngineBase.h"
#include "GothicAPI.h"
#include <DDSTextureLoader.h>
#include "RenderToTextureBuffer.h"
#include "D3D11_Helpers.h"
#include "TextureConversions.h"
#include "zFILE_VDFS.h"
#include <limits>
#include <new>

extern bool NativeSupport16BitTextures;

namespace {
    bool Is16BitFormat( DXGI_FORMAT format ) {
        return format == DXGI_FORMAT_B5G6R5_UNORM
            || format == DXGI_FORMAT_B5G5R5A1_UNORM
            || format == DXGI_FORMAT_B4G4R4A4_UNORM;
    }

    bool ConvertTextureData(
        UINT textureWidth,
        UINT textureHeight,
        DXGI_FORMAT textureFormat,
        const void* sourceData,
        std::vector<unsigned char>& convertedData ) {
        if ( !sourceData || textureWidth == 0 || textureHeight == 0 || !Is16BitFormat( textureFormat ) ) {
            return false;
        }

        const uint64_t pixelCount = static_cast<uint64_t>(textureWidth) * textureHeight;
        const uint64_t outputSize = pixelCount * 4u;
        if ( outputSize == 0 || outputSize > std::numeric_limits<UINT>::max()
            || outputSize > std::numeric_limits<size_t>::max() ) {
            return false;
        }

        try {
            convertedData.resize( static_cast<size_t>(outputSize) );
        } catch ( const std::bad_alloc& ) {
            return false;
        }

        auto* destination = convertedData.data();
        const auto* source = static_cast<const unsigned char*>(sourceData);
        const UINT conversionSize = static_cast<UINT>(outputSize);
        if ( textureFormat == DXGI_FORMAT_B5G6R5_UNORM ) {
            Convert565to8888( destination, source, conversionSize );
        } else if ( textureFormat == DXGI_FORMAT_B5G5R5A1_UNORM ) {
            Convert1555to8888( destination, source, conversionSize );
        } else {
            Convert4444to8888( destination, source, conversionSize );
        }
        return true;
    }
}
D3D11Texture::D3D11Texture() {}

D3D11Texture::~D3D11Texture() {
    if ( Engine::GAPI ) {
        Engine::GAPI->RemoveMipMapGeneration( this );
    }
    ThumbnailSRV.Reset();
    Thumbnail.Reset();
    ShaderResourceView.Reset();
    Texture.Reset();
}

/** Initializes the texture object */
XRESULT D3D11Texture::Init( INT2 size, ETextureFormat format, UINT mipMapCount, void* data, const std::string& fileName ) {
    auto* engine = reinterpret_cast<D3D11GraphicsEngineBase*>(Engine::GraphicsEngine);
    if ( !engine || !engine->GetDevice() || size.x <= 0 || size.y <= 0
        || size.x > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION
        || size.y > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION
        || mipMapCount == 0 || mipMapCount > D3D11_REQ_MIP_LEVELS
        || (data && mipMapCount != 1) ) {
        return XR_INVALID_ARG;
    }

    const DXGI_FORMAT sourceFormat = static_cast<DXGI_FORMAT>(format);
    const bool convert16Bit = Is16BitFormat( sourceFormat ) && !NativeSupport16BitTextures;
    const DXGI_FORMAT resourceFormat = convert16Bit
        ? DXGI_FORMAT_B8G8R8A8_UNORM
        : sourceFormat;

    std::vector<unsigned char> convertedData;
    const void* initialPixels = data;
    if ( data && convert16Bit ) {
        if ( !ConvertTextureData(
            static_cast<UINT>(size.x), static_cast<UINT>(size.y),
            sourceFormat, data, convertedData ) ) {
            return XR_FAILED;
        }
        initialPixels = convertedData.data();
    }

    D3D11_TEXTURE2D_DESC textureDesc{};
    textureDesc.Width = static_cast<UINT>(size.x);
    textureDesc.Height = static_cast<UINT>(size.y);
    textureDesc.MipLevels = mipMapCount;
    textureDesc.ArraySize = 1;
    textureDesc.Format = resourceFormat;
    textureDesc.SampleDesc.Count = 1;
    textureDesc.Usage = data ? D3D11_USAGE_IMMUTABLE : D3D11_USAGE_DEFAULT;
    textureDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA initialData{};
    if ( initialPixels ) {
        initialData.pSysMem = initialPixels;
        if ( resourceFormat == DXGI_FORMAT_R8_UNORM ) {
            initialData.SysMemPitch = static_cast<UINT>(size.x);
        } else if ( Is16BitFormat( resourceFormat ) ) {
            initialData.SysMemPitch = static_cast<UINT>(size.x) * 2u;
        } else if ( resourceFormat == DXGI_FORMAT_BC1_UNORM
            || resourceFormat == DXGI_FORMAT_BC2_UNORM
            || resourceFormat == DXGI_FORMAT_BC3_UNORM ) {
            initialData.SysMemPitch = Toolbox::GetDDSRowPitchSize(
                size.x, resourceFormat == DXGI_FORMAT_BC1_UNORM );
        } else {
            initialData.SysMemPitch = static_cast<UINT>(size.x) * 4u;
        }
    }

    Microsoft::WRL::ComPtr<ID3D11Texture2D> newTexture;
    HRESULT hr = engine->GetDevice()->CreateTexture2D(
        &textureDesc, initialPixels ? &initialData : nullptr, newTexture.GetAddressOf() );
    if ( FAILED( hr ) ) {
        LogError() << "Failed to create texture '" << fileName << "': 0x"
            << std::hex << static_cast<unsigned long>(hr);
        return XR_FAILED;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc{};
    viewDesc.Format = resourceFormat;
    viewDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    viewDesc.Texture2D.MipLevels = mipMapCount;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> newSRV;
    hr = engine->GetDevice()->CreateShaderResourceView(
        newTexture.Get(), &viewDesc, newSRV.GetAddressOf() );
    if ( FAILED( hr ) ) {
        LogError() << "Failed to create texture SRV '" << fileName << "': 0x"
            << std::hex << static_cast<unsigned long>(hr);
        return XR_FAILED;
    }

    Texture = std::move( newTexture );
    ShaderResourceView = std::move( newSRV );
    Thumbnail.Reset();
    ThumbnailSRV.Reset();
    TextureFormat = sourceFormat;
    TextureSize = size;
    MipMapCount = static_cast<int>(mipMapCount);

    SetDebugName( Texture.Get(), "D3D11Texture(\"" + fileName + "\")->Texture" );
    SetDebugName( ShaderResourceView.Get(), "D3D11Texture(\"" + fileName + "\")->ShaderResourceView" );
    return XR_SUCCESS;
}
/** Initializes the texture from a file */
XRESULT D3D11Texture::Init( const std::string& file ) {
    auto* engine = reinterpret_cast<D3D11GraphicsEngineBase*>(Engine::GraphicsEngine);
    if ( !engine || !engine->GetDevice() || file.empty() ) {
        return XR_INVALID_ARG;
    }

    Microsoft::WRL::ComPtr<ID3D11Texture2D> newTexture;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> newSRV;
    HRESULT hr = E_FAIL;

    if ( std::filesystem::path( file ).is_absolute() ) {
        hr = CreateDDSTextureFromFileEx(
            engine->GetDevice().Get(),
            Toolbox::ToWideChar( file.c_str() ).c_str(),
            0,
            D3D11_USAGE_IMMUTABLE,
            D3D11_BIND_SHADER_RESOURCE,
            0,
            0,
            DirectX::DDS_LOADER_DEFAULT,
            reinterpret_cast<ID3D11Resource**>(newTexture.GetAddressOf()),
            newSRV.GetAddressOf() );
    } else {
        zFILE_VDFS::Ptr vdfsFile;
        if ( file[0] != '\\' ) {
            vdfsFile = zFILE_VDFS::Create( ("\\" + file).c_str() );
        } else {
            vdfsFile = zFILE_VDFS::Create( file.c_str() );
        }

        if ( !vdfsFile || !vdfsFile->Exists() || vdfsFile->Open( false ) != zERROR_NONE ) {
            LogError() << "Failed to load texture from VDFS: " << file;
            return XR_FAILED;
        }

        constexpr long MaxTextureFileSize = 512l * 1024l * 1024l;
        const long fileSize = vdfsFile->Size();
        if ( fileSize <= 0 || fileSize > MaxTextureFileSize ) {
            vdfsFile->Close();
            LogError() << "Texture file has an invalid size: " << file;
            return XR_FAILED;
        }

        std::vector<uint8_t> fileData;
        try {
            fileData.resize( static_cast<size_t>(fileSize) );
        } catch ( const std::bad_alloc& ) {
            vdfsFile->Close();
            return XR_FAILED;
        }

        const long bytesRead = vdfsFile->Read( fileData.data(), fileSize );
        vdfsFile->Close();
        if ( bytesRead != fileSize ) {
            LogError() << "Texture file could not be read completely: " << file;
            return XR_FAILED;
        }

        hr = CreateDDSTextureFromMemoryEx(
            engine->GetDevice().Get(),
            fileData.data(),
            fileData.size(),
            0,
            D3D11_USAGE_IMMUTABLE,
            D3D11_BIND_SHADER_RESOURCE,
            0,
            0,
            DirectX::DDS_LOADER_DEFAULT,
            reinterpret_cast<ID3D11Resource**>(newTexture.GetAddressOf()),
            newSRV.GetAddressOf() );
    }

    if ( FAILED( hr ) || !newTexture || !newSRV ) {
        LogError() << "Failed to create DDS texture '" << file << "': 0x"
            << std::hex << static_cast<unsigned long>(hr);
        return XR_FAILED;
    }

    D3D11_TEXTURE2D_DESC desc{};
    newTexture->GetDesc( &desc );
    Texture = std::move( newTexture );
    ShaderResourceView = std::move( newSRV );
    Thumbnail.Reset();
    ThumbnailSRV.Reset();
    TextureFormat = desc.Format;
    TextureSize = INT2( static_cast<int>(desc.Width), static_cast<int>(desc.Height) );
    MipMapCount = static_cast<int>(desc.MipLevels);

    SetDebugName( Texture.Get(), "D3D11Texture(\"" + file + "\")->Texture" );
    SetDebugName( ShaderResourceView.Get(), "D3D11Texture(\"" + file + "\")->ShaderResourceView" );
    return XR_SUCCESS;
}

XRESULT D3D11Texture::Init( const uint8_t* data, size_t size, const std::string& debugFileName ) {
    auto* engine = reinterpret_cast<D3D11GraphicsEngineBase*>(Engine::GraphicsEngine);
    if ( !engine || !engine->GetDevice() || !data || size == 0 ) {
        return XR_INVALID_ARG;
    }

    Microsoft::WRL::ComPtr<ID3D11Texture2D> newTexture;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> newSRV;
    const HRESULT hr = CreateDDSTextureFromMemory(
        engine->GetDevice().Get(), data, size,
        reinterpret_cast<ID3D11Resource**>(newTexture.GetAddressOf()),
        newSRV.GetAddressOf() );
    if ( FAILED( hr ) || !newTexture || !newSRV ) {
        LogError() << "Failed to create in-memory DDS texture '" << debugFileName << "': 0x"
            << std::hex << static_cast<unsigned long>(hr);
        return XR_FAILED;
    }

    D3D11_TEXTURE2D_DESC desc{};
    newTexture->GetDesc( &desc );
    Texture = std::move( newTexture );
    ShaderResourceView = std::move( newSRV );
    Thumbnail.Reset();
    ThumbnailSRV.Reset();
    TextureFormat = desc.Format;
    TextureSize = INT2( static_cast<int>(desc.Width), static_cast<int>(desc.Height) );
    MipMapCount = static_cast<int>(desc.MipLevels);

    SetDebugName( Texture.Get(), "D3D11Texture(\"" + debugFileName + "\")->Texture" );
    SetDebugName( ShaderResourceView.Get(), "D3D11Texture(\"" + debugFileName + "\")->ShaderResourceView" );
    return XR_SUCCESS;
}
/** Updates the Texture-Object */
XRESULT D3D11Texture::UpdateData( void* data, int mip ) {
    auto* engine = reinterpret_cast<D3D11GraphicsEngineBase*>(Engine::GraphicsEngine);
    if ( !data || !IsValid() || !engine || !engine->GetContext()
        || mip < 0 || mip >= MipMapCount ) {
        return XR_INVALID_ARG;
    }

    const UINT textureWidth = static_cast<UINT>(std::max( TextureSize.x >> mip, 1 ));
    const UINT textureHeight = static_cast<UINT>(std::max( TextureSize.y >> mip, 1 ));
    const void* sourceData = data;
    UINT rowPitch = GetRowPitchBytes( mip );

    std::vector<unsigned char> convertedData;
    if ( Is16BitTexture() && !NativeSupport16BitTextures ) {
        if ( !ConvertTextureData(
            textureWidth, textureHeight, TextureFormat, data, convertedData ) ) {
            return XR_FAILED;
        }
        sourceData = convertedData.data();
        rowPitch = textureWidth * 4u;
    }
    if ( rowPitch == 0 ) {
        return XR_FAILED;
    }

    engine->GetContext()->UpdateSubresource(
        Texture.Get(), static_cast<UINT>(mip), nullptr, sourceData, rowPitch, 0 );
    return XR_SUCCESS;
}

/** Updates the Texture-Object using the deferred context (For loading in an other thread) */
XRESULT D3D11Texture::UpdateDataDeferred( void* data, int mip ) {
    auto* engine = reinterpret_cast<D3D11GraphicsEngineBase*>(Engine::GraphicsEngine);
    if ( !data || !IsValid() || !engine || !engine->GetDevice()
        || mip < 0 || mip >= MipMapCount ) {
        return XR_INVALID_ARG;
    }

    const UINT textureWidth = static_cast<UINT>(std::max( TextureSize.x >> mip, 1 ));
    const UINT textureHeight = static_cast<UINT>(std::max( TextureSize.y >> mip, 1 ));

    D3D11_TEXTURE2D_DESC stagingDesc{};
    Texture->GetDesc( &stagingDesc );
    stagingDesc.Width = textureWidth;
    stagingDesc.Height = textureHeight;
    stagingDesc.MipLevels = 1;
    stagingDesc.BindFlags = 0;
    stagingDesc.MiscFlags = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    stagingDesc.Usage = D3D11_USAGE_STAGING;

    std::vector<unsigned char> convertedData;
    D3D11_SUBRESOURCE_DATA stagingData{};
    stagingData.pSysMem = data;
    stagingData.SysMemPitch = GetRowPitchBytes( mip );
    if ( Is16BitTexture() && !NativeSupport16BitTextures ) {
        if ( !ConvertTextureData(
            textureWidth, textureHeight, TextureFormat, data, convertedData ) ) {
            return XR_FAILED;
        }
        stagingData.pSysMem = convertedData.data();
        stagingData.SysMemPitch = textureWidth * 4u;
    }
    if ( stagingData.SysMemPitch == 0 ) {
        return XR_FAILED;
    }

    Microsoft::WRL::ComPtr<ID3D11Texture2D> stagingTexture;
    const HRESULT hr = engine->GetDevice()->CreateTexture2D(
        &stagingDesc, &stagingData, stagingTexture.GetAddressOf() );
    if ( FAILED( hr ) ) {
        LogError() << "Failed to create deferred staging texture: 0x"
            << std::hex << static_cast<unsigned long>(hr);
        return XR_FAILED;
    }

    SetDebugName( stagingTexture.Get(), "D3D11Texture->UpdateDataDeferred->stagingTexture" );
    Engine::GAPI->AddStagingTexture( mip, stagingTexture.Detach(), Texture.Get() );
    return XR_SUCCESS;
}
/** Returns the RowPitch-Bytes */
UINT D3D11Texture::GetRowPitchBytes( int mip ) {
    if ( mip < 0 || mip >= MipMapCount || TextureSize.x <= 0 ) {
        return 0;
    }

    const uint64_t width = static_cast<uint64_t>(std::max( TextureSize.x >> mip, 1 ));
    uint64_t rowPitch = 0;
    if ( TextureFormat == DXGI_FORMAT_R8_UNORM ) {
        rowPitch = width;
    } else if ( Is16BitFormat( TextureFormat ) ) {
        rowPitch = width * 2u;
    } else if ( TextureFormat == DXGI_FORMAT_BC1_UNORM
        || TextureFormat == DXGI_FORMAT_BC2_UNORM
        || TextureFormat == DXGI_FORMAT_BC3_UNORM ) {
        rowPitch = Toolbox::GetDDSRowPitchSize(
            static_cast<int>(width), TextureFormat == DXGI_FORMAT_BC1_UNORM );
    } else {
        rowPitch = width * 4u;
    }

    return rowPitch <= std::numeric_limits<UINT>::max()
        ? static_cast<UINT>(rowPitch)
        : 0u;
}
/** Returns the size of the texture in bytes */
UINT D3D11Texture::GetSizeInBytes( int mip ) {
    if ( mip < 0 || mip >= MipMapCount || TextureSize.x <= 0 || TextureSize.y <= 0 ) {
        return 0;
    }

    const uint64_t width = static_cast<uint64_t>(std::max( TextureSize.x >> mip, 1 ));
    const uint64_t height = static_cast<uint64_t>(std::max( TextureSize.y >> mip, 1 ));
    uint64_t sizeInBytes = 0;
    if ( TextureFormat == DXGI_FORMAT_R8_UNORM ) {
        sizeInBytes = width * height;
    } else if ( Is16BitFormat( TextureFormat ) ) {
        sizeInBytes = width * height * 2u;
    } else if ( TextureFormat == DXGI_FORMAT_BC1_UNORM
        || TextureFormat == DXGI_FORMAT_BC2_UNORM
        || TextureFormat == DXGI_FORMAT_BC3_UNORM ) {
        sizeInBytes = Toolbox::GetDDSStorageRequirements(
            static_cast<int>(width), static_cast<int>(height),
            TextureFormat == DXGI_FORMAT_BC1_UNORM );
    } else {
        sizeInBytes = width * height * 4u;
    }

    return sizeInBytes <= std::numeric_limits<UINT>::max()
        ? static_cast<UINT>(sizeInBytes)
        : 0u;
}
/** Returns if texture is 16bit type */
bool D3D11Texture::Is16BitTexture() {
    return Is16BitFormat( TextureFormat );
}

/** Binds this texture to a pixelshader */
XRESULT D3D11Texture::BindToPixelShader( int slot ) {
    auto* engine = reinterpret_cast<D3D11GraphicsEngineBase*>(Engine::GraphicsEngine);
    if ( !engine || !engine->GetContext() || !ShaderResourceView
        || slot < 0 || slot >= D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT ) {
        return XR_INVALID_ARG;
    }

    ID3D11ShaderResourceView* view = ShaderResourceView.Get();
    engine->GetContext()->PSSetShaderResources( static_cast<UINT>(slot), 1, &view );
    return XR_SUCCESS;
}

/** Binds this texture to a vertexshader */
XRESULT D3D11Texture::BindToVertexShader( int slot ) {
    auto* engine = reinterpret_cast<D3D11GraphicsEngineBase*>(Engine::GraphicsEngine);
    if ( !engine || !engine->GetContext() || !ShaderResourceView
        || slot < 0 || slot >= D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT ) {
        return XR_INVALID_ARG;
    }

    ID3D11ShaderResourceView* view = ShaderResourceView.Get();
    engine->GetContext()->VSSetShaderResources( static_cast<UINT>(slot), 1, &view );
    return XR_SUCCESS;
}

/** Creates a thumbnail for this */
XRESULT D3D11Texture::CreateThumbnail() {
    auto* engine = reinterpret_cast<D3D11GraphicsEngineBase*>(Engine::GraphicsEngine);
    if ( !IsValid() || !engine || !engine->GetDevice() || !engine->GetContext() ) {
        return XR_FAILED;
    }

    CD3D11_TEXTURE2D_DESC textureDesc(
        DXGI_FORMAT_ENGINE_DEFAULT,
        256,
        256,
        1,
        1,
        D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE,
        D3D11_USAGE_DEFAULT, 0, 1, 0, 0 );

    Microsoft::WRL::ComPtr<ID3D11Texture2D> newThumbnail;
    HRESULT hr = engine->GetDevice()->CreateTexture2D(
        &textureDesc, nullptr, newThumbnail.GetAddressOf() );
    if ( FAILED( hr ) ) {
        return XR_FAILED;
    }

    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> thumbnailRTV;
    hr = engine->GetDevice()->CreateRenderTargetView(
        newThumbnail.Get(), nullptr, thumbnailRTV.GetAddressOf() );
    if ( FAILED( hr ) ) {
        return XR_FAILED;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = textureDesc.Format;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;

    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> newThumbnailSRV;
    hr = engine->GetDevice()->CreateShaderResourceView(
        newThumbnail.Get(), &srvDesc, newThumbnailSRV.GetAddressOf() );
    if ( FAILED( hr ) ) {
        return XR_FAILED;
    }

    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> oldRTV;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> oldDSV;
    engine->GetContext()->OMGetRenderTargets(
        1, oldRTV.GetAddressOf(), oldDSV.GetAddressOf() );

    ID3D11ShaderResourceView* source = ShaderResourceView.Get();
    engine->GetContext()->PSSetShaderResources( 0, 1, &source );
    ID3D11RenderTargetView* target = thumbnailRTV.Get();
    engine->GetContext()->OMSetRenderTargets( 1, &target, nullptr );

    const float clearColor[4] = { 1.f, 0.f, 0.f, 1.f };
    engine->GetContext()->ClearRenderTargetView( thumbnailRTV.Get(), clearColor );
    const XRESULT drawResult = engine->DrawQuad( INT2( 0, 0 ), INT2( 256, 256 ) );

    ID3D11ShaderResourceView* nullSRV = nullptr;
    engine->GetContext()->PSSetShaderResources( 0, 1, &nullSRV );
    ID3D11RenderTargetView* previousTarget = oldRTV.Get();
    engine->GetContext()->OMSetRenderTargets( 1, &previousTarget, oldDSV.Get() );
    if ( drawResult != XR_SUCCESS ) {
        return drawResult;
    }

    Thumbnail = std::move( newThumbnail );
    ThumbnailSRV = std::move( newThumbnailSRV );
    return XR_SUCCESS;
}
/** Returns the thumbnail of this texture. If this returns nullptr, you need to create one first */
const Microsoft::WRL::ComPtr<ID3D11Texture2D>& D3D11Texture::GetThumbnail() {
    return Thumbnail;
}

/** Generates mipmaps for this texture (may be slow!) */
XRESULT D3D11Texture::GenerateMipMaps() {
    if ( MipMapCount == 1 ) return XR_SUCCESS;

    auto* engine = reinterpret_cast<D3D11GraphicsEngineBase*>(Engine::GraphicsEngine);
    if ( !IsValid() || MipMapCount <= 1 || !engine
        || !engine->GetDevice() || !engine->GetContext() ) {
        return XR_FAILED;
    }

    D3D11_TEXTURE2D_DESC sourceDesc{};
    Texture->GetDesc( &sourceDesc );
    if ( sourceDesc.ArraySize != 1 || sourceDesc.SampleDesc.Count != 1
        || sourceDesc.MipLevels <= 1 || sourceDesc.Format == DXGI_FORMAT_UNKNOWN ) {
        return XR_FAILED;
    }

    UINT formatSupport = 0;
    const HRESULT supportResult = engine->GetDevice()->CheckFormatSupport(
        sourceDesc.Format, &formatSupport );
    constexpr UINT requiredSupport = D3D11_FORMAT_SUPPORT_TEXTURE2D
        | D3D11_FORMAT_SUPPORT_SHADER_SAMPLE
        | D3D11_FORMAT_SUPPORT_RENDER_TARGET
        | D3D11_FORMAT_SUPPORT_MIP_AUTOGEN;
    if ( FAILED( supportResult ) || (formatSupport & requiredSupport) != requiredSupport ) {
        LogWarn() << "Automatic mip generation is unsupported for texture format "
            << static_cast<unsigned int>(sourceDesc.Format) << ".";
        return XR_FAILED;
    }

    D3D11_TEXTURE2D_DESC generatedDesc = sourceDesc;
    generatedDesc.Usage = D3D11_USAGE_DEFAULT;
    generatedDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    generatedDesc.CPUAccessFlags = 0;
    generatedDesc.MiscFlags |= D3D11_RESOURCE_MISC_GENERATE_MIPS;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> generatedTexture;
    HRESULT hr = engine->GetDevice()->CreateTexture2D(
        &generatedDesc, nullptr, generatedTexture.GetAddressOf() );
    if ( FAILED( hr ) || !generatedTexture ) {
        LogError() << "Failed to create automatic-mipmap texture: 0x"
            << std::hex << static_cast<unsigned long>(hr);
        return XR_FAILED;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = generatedDesc.Format;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = generatedDesc.MipLevels;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> generatedSrv;
    hr = engine->GetDevice()->CreateShaderResourceView(
        generatedTexture.Get(), &srvDesc, generatedSrv.GetAddressOf() );
    if ( FAILED( hr ) || !generatedSrv ) {
        LogError() << "Failed to create automatic-mipmap SRV: 0x"
            << std::hex << static_cast<unsigned long>(hr);
        return XR_FAILED;
    }

    engine->GetContext()->CopySubresourceRegion(
        generatedTexture.Get(), 0, 0, 0, 0, Texture.Get(), 0, nullptr );
    engine->GetContext()->GenerateMips( generatedSrv.Get() );

    SetDebugName( generatedTexture.Get(), "D3D11Texture::GeneratedMipTexture" );
    SetDebugName( generatedSrv.Get(), "D3D11Texture::GeneratedMipSRV" );
    Texture = std::move( generatedTexture );
    ShaderResourceView = std::move( generatedSrv );
    Thumbnail.Reset();
    ThumbnailSRV.Reset();
    MipMapCount = static_cast<int>(generatedDesc.MipLevels);
    return XR_SUCCESS;
}
XRESULT D3D11Texture::GenerateMipMapsDeferred() {
    if ( MipMapCount == 1 ) return XR_SUCCESS;
    if ( !IsValid() || MipMapCount <= 1 || !Engine::GAPI ) return XR_FAILED;

    Engine::GAPI->AddMipMapGeneration( this );
    return XR_SUCCESS;
}
