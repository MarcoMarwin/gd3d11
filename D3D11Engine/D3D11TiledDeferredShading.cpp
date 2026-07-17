#include "pch.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include "D3D11TiledDeferredShading.h"

#include "D3D11GraphicsEngine.h"
#include "D3D11LegacyDeferredShading.h"
#include "D3D11PointLight.h"
#include "Engine.h"
#include "GothicAPI.h"
#include "ConstantBufferStructs.h"
#include "D3D11PfxRenderer.h"
#include "D3D11_Helpers.h"
#include "RenderToTextureBuffer.h"
#include "zCVobLight.h"
#include "GSky.h"

using namespace DirectX;
using Microsoft::WRL::ComPtr;

XRESULT D3D11TiledDeferredShading::Init(
    const ComPtr<ID3D11Device1>& device,
    const ComPtr<ID3D11DeviceContext1>& context ) {
    m_device = device;
    m_context = context;
    if ( !m_device || !m_context ) {
        return XR_INVALID_ARG;
    }

    m_LightBuffer.Reset();
    m_LightBufferSRV.Reset();
    m_IndexCounter.Reset();
    m_IndexCounterUAV.Reset();

    D3D11_BUFFER_DESC lightDesc{};
    lightDesc.ByteWidth = MAX_TILED_LIGHTS * sizeof( TiledPointLight );
    lightDesc.Usage = D3D11_USAGE_DYNAMIC;
    lightDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    lightDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    lightDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    lightDesc.StructureByteStride = sizeof( TiledPointLight );

    HRESULT hr = m_device->CreateBuffer( &lightDesc, nullptr, m_LightBuffer.GetAddressOf() );
    if ( FAILED( hr ) ) {
        LogError() << "Failed to create tiled deferred light buffer. HRESULT: 0x"
            << std::hex << static_cast<unsigned long>(hr);
        return XR_FAILED;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC lightSRVDesc{};
    lightSRVDesc.Format = DXGI_FORMAT_UNKNOWN;
    lightSRVDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    lightSRVDesc.Buffer.NumElements = MAX_TILED_LIGHTS;
    hr = m_device->CreateShaderResourceView(
        m_LightBuffer.Get(), &lightSRVDesc, m_LightBufferSRV.GetAddressOf() );
    if ( FAILED( hr ) ) {
        LogError() << "Failed to create tiled deferred light buffer SRV. HRESULT: 0x"
            << std::hex << static_cast<unsigned long>(hr);
        m_LightBuffer.Reset();
        return XR_FAILED;
    }

    D3D11_BUFFER_DESC counterDesc{};
    counterDesc.ByteWidth = sizeof( uint32_t ) * 4u;
    counterDesc.Usage = D3D11_USAGE_DEFAULT;
    counterDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    counterDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    counterDesc.StructureByteStride = sizeof( uint32_t );

    hr = m_device->CreateBuffer( &counterDesc, nullptr, m_IndexCounter.GetAddressOf() );
    if ( FAILED( hr ) ) {
        LogError() << "Failed to create tiled deferred index counter. HRESULT: 0x"
            << std::hex << static_cast<unsigned long>(hr);
        return XR_FAILED;
    }

    D3D11_UNORDERED_ACCESS_VIEW_DESC counterUAVDesc{};
    counterUAVDesc.Format = DXGI_FORMAT_UNKNOWN;
    counterUAVDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    counterUAVDesc.Buffer.NumElements = 4;
    hr = m_device->CreateUnorderedAccessView(
        m_IndexCounter.Get(), &counterUAVDesc, m_IndexCounterUAV.GetAddressOf() );
    if ( FAILED( hr ) ) {
        LogError() << "Failed to create tiled deferred index counter UAV. HRESULT: 0x"
            << std::hex << static_cast<unsigned long>(hr);
        m_IndexCounter.Reset();
        return XR_FAILED;
    }

    SetDebugName( m_LightBuffer.Get(), "TiledDeferred_LightBuffer" );
    SetDebugName( m_LightBufferSRV.Get(), "TiledDeferred_LightBuffer_SRV" );
    SetDebugName( m_IndexCounter.Get(), "TiledDeferred_IndexCounter" );
    SetDebugName( m_IndexCounterUAV.Get(), "TiledDeferred_IndexCounter_UAV" );
    return XR_SUCCESS;
}
bool D3D11TiledDeferredShading::EnsureShadowArray( uint32_t shadowCubeSize ) {
    shadowCubeSize = std::clamp<uint32_t>( shadowCubeSize, 64, 512 );
    if ( m_ShadowArrayCreated && m_ShadowCubeSize == shadowCubeSize
        && m_ShadowCubeArray && m_ShadowCubeArraySRV ) {
        return true;
    }
    if ( !m_device ) {
        return false;
    }

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = shadowCubeSize;
    desc.Height = shadowCubeSize;
    desc.MipLevels = 1;
    desc.ArraySize = MAX_SHADOW_CUBEMAPS * 6;
    desc.Format = DXGI_FORMAT_R16_TYPELESS;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
    desc.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE;

    ComPtr<ID3D11Texture2D> newShadowArray;
    HRESULT hr = m_device->CreateTexture2D( &desc, nullptr, newShadowArray.GetAddressOf() );
    if ( FAILED( hr ) ) {
        LogError() << "Failed to create tiled shadow cube array. HRESULT: 0x"
            << std::hex << static_cast<unsigned long>(hr);
        return false;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = DXGI_FORMAT_R16_UNORM;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBEARRAY;
    srvDesc.TextureCubeArray.MostDetailedMip = 0;
    srvDesc.TextureCubeArray.MipLevels = 1;
    srvDesc.TextureCubeArray.First2DArrayFace = 0;
    srvDesc.TextureCubeArray.NumCubes = MAX_SHADOW_CUBEMAPS;

    ComPtr<ID3D11ShaderResourceView> newShadowArraySRV;
    hr = m_device->CreateShaderResourceView(
        newShadowArray.Get(), &srvDesc, newShadowArraySRV.GetAddressOf() );
    if ( FAILED( hr ) ) {
        LogError() << "Failed to create tiled shadow cube array SRV. HRESULT: 0x"
            << std::hex << static_cast<unsigned long>(hr);
        return false;
    }

    std::array<ComPtr<ID3D11DepthStencilView>, MAX_SHADOW_CUBEMAPS> newSlotDSVs;
    std::array<std::unique_ptr<RenderToDepthStencilBuffer>, MAX_SHADOW_CUBEMAPS> newSlotViews;
    for ( uint32_t slot = 0; slot < MAX_SHADOW_CUBEMAPS; ++slot ) {
        D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
        dsvDesc.Format = DXGI_FORMAT_D16_UNORM;
        dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
        dsvDesc.Texture2DArray.FirstArraySlice = slot * 6;
        dsvDesc.Texture2DArray.ArraySize = 6;
        dsvDesc.Texture2DArray.MipSlice = 0;

        hr = m_device->CreateDepthStencilView(
            newShadowArray.Get(), &dsvDesc, newSlotDSVs[slot].GetAddressOf() );
        if ( FAILED( hr ) ) {
            LogError() << "Failed to create tiled shadow cube array DSV. HRESULT: 0x"
                << std::hex << static_cast<unsigned long>(hr);
            return false;
        }

        newSlotViews[slot] = std::make_unique<RenderToDepthStencilBuffer>(
            newShadowArray, newSlotDSVs[slot], newShadowArraySRV, shadowCubeSize, shadowCubeSize );
    }

    m_SlotInUse.reset();
    m_ShadowCubeArray = std::move( newShadowArray );
    m_ShadowCubeArraySRV = std::move( newShadowArraySRV );
    m_SlotDSVs = std::move( newSlotDSVs );
    m_SlotViews = std::move( newSlotViews );
    m_ShadowCubeSize = shadowCubeSize;
    m_ShadowArrayCreated = true;

    SetDebugName( m_ShadowCubeArray.Get(), "TiledDeferred_ShadowCubeArray" );
    SetDebugName( m_ShadowCubeArraySRV.Get(), "TiledDeferred_ShadowCubeArray_SRV" );
    return true;
}
int D3D11TiledDeferredShading::AllocateSlot( uint32_t shadowCubeSize ) {
    if ( !EnsureShadowArray( shadowCubeSize ) ) {
        return -1;
    }

    for ( uint32_t i = 0; i < MAX_SHADOW_CUBEMAPS; ++i ) {
        if ( !m_SlotInUse[i] && m_SlotViews[i] ) {
            m_SlotInUse[i] = true;
            return static_cast<int>(i);
        }
    }
    return -1;
}
void D3D11TiledDeferredShading::FreeSlot( int slot ) {
    if ( slot >= 0 && static_cast<uint32_t>(slot) < MAX_SHADOW_CUBEMAPS )
        m_SlotInUse[slot] = false;
}

RenderToDepthStencilBuffer* D3D11TiledDeferredShading::GetSlotTarget( int slot ) {
    if ( slot >= 0 && static_cast<uint32_t>(slot) < MAX_SHADOW_CUBEMAPS && m_ShadowArrayCreated )
        return m_SlotViews[slot].get();
    return nullptr;
}

bool D3D11TiledDeferredShading::EnsureBuffers( uint32_t numTilesX, uint32_t numTilesY ) {
    if ( !m_device || numTilesX == 0 || numTilesY == 0 ) {
        return false;
    }

    const bool existingResourcesValid = m_LightIndexList && m_LightIndexListSRV
        && m_LightIndexListUAV && m_LightGrid && m_LightGridSRV && m_LightGridUAV;
    if ( numTilesX == m_lastNumTilesX && numTilesY == m_lastNumTilesY
        && existingResourcesValid ) {
        return true;
    }

    const uint64_t totalTiles64 = static_cast<uint64_t>(numTilesX) * numTilesY;
    const uint64_t indexEntries64 = totalTiles64 * MAX_LIGHTS_PER_TILE;
    const uint64_t indexBytes64 = indexEntries64 * sizeof( uint32_t );
    const uint64_t gridBytes64 = totalTiles64 * sizeof( LightGrid );
    if ( totalTiles64 == 0 || totalTiles64 > std::numeric_limits<UINT>::max()
        || indexEntries64 > std::numeric_limits<UINT>::max()
        || indexBytes64 > std::numeric_limits<UINT>::max()
        || gridBytes64 > std::numeric_limits<UINT>::max() ) {
        LogError() << "Tiled deferred buffer dimensions overflow.";
        return false;
    }

    const UINT totalTiles = static_cast<UINT>(totalTiles64);
    const UINT indexEntries = static_cast<UINT>(indexEntries64);

    ComPtr<ID3D11Buffer> newIndexList;
    ComPtr<ID3D11ShaderResourceView> newIndexListSRV;
    ComPtr<ID3D11UnorderedAccessView> newIndexListUAV;

    D3D11_BUFFER_DESC indexDesc{};
    indexDesc.ByteWidth = static_cast<UINT>(indexBytes64);
    indexDesc.Usage = D3D11_USAGE_DEFAULT;
    indexDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    indexDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    indexDesc.StructureByteStride = sizeof( uint32_t );

    HRESULT hr = m_device->CreateBuffer( &indexDesc, nullptr, newIndexList.GetAddressOf() );
    if ( FAILED( hr ) ) {
        LogError() << "Failed to create tiled light index buffer. HRESULT: 0x"
            << std::hex << static_cast<unsigned long>(hr);
        return false;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC indexSRVDesc{};
    indexSRVDesc.Format = DXGI_FORMAT_UNKNOWN;
    indexSRVDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    indexSRVDesc.Buffer.NumElements = indexEntries;
    hr = m_device->CreateShaderResourceView(
        newIndexList.Get(), &indexSRVDesc, newIndexListSRV.GetAddressOf() );
    if ( FAILED( hr ) ) {
        LogError() << "Failed to create tiled light index SRV. HRESULT: 0x"
            << std::hex << static_cast<unsigned long>(hr);
        return false;
    }

    D3D11_UNORDERED_ACCESS_VIEW_DESC indexUAVDesc{};
    indexUAVDesc.Format = DXGI_FORMAT_UNKNOWN;
    indexUAVDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    indexUAVDesc.Buffer.NumElements = indexEntries;
    hr = m_device->CreateUnorderedAccessView(
        newIndexList.Get(), &indexUAVDesc, newIndexListUAV.GetAddressOf() );
    if ( FAILED( hr ) ) {
        LogError() << "Failed to create tiled light index UAV. HRESULT: 0x"
            << std::hex << static_cast<unsigned long>(hr);
        return false;
    }

    ComPtr<ID3D11Buffer> newLightGrid;
    ComPtr<ID3D11ShaderResourceView> newLightGridSRV;
    ComPtr<ID3D11UnorderedAccessView> newLightGridUAV;

    D3D11_BUFFER_DESC gridDesc{};
    gridDesc.ByteWidth = static_cast<UINT>(gridBytes64);
    gridDesc.Usage = D3D11_USAGE_DEFAULT;
    gridDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    gridDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    gridDesc.StructureByteStride = sizeof( LightGrid );

    hr = m_device->CreateBuffer( &gridDesc, nullptr, newLightGrid.GetAddressOf() );
    if ( FAILED( hr ) ) {
        LogError() << "Failed to create tiled light grid buffer. HRESULT: 0x"
            << std::hex << static_cast<unsigned long>(hr);
        return false;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC gridSRVDesc{};
    gridSRVDesc.Format = DXGI_FORMAT_UNKNOWN;
    gridSRVDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    gridSRVDesc.Buffer.NumElements = totalTiles;
    hr = m_device->CreateShaderResourceView(
        newLightGrid.Get(), &gridSRVDesc, newLightGridSRV.GetAddressOf() );
    if ( FAILED( hr ) ) {
        LogError() << "Failed to create tiled light grid SRV. HRESULT: 0x"
            << std::hex << static_cast<unsigned long>(hr);
        return false;
    }

    D3D11_UNORDERED_ACCESS_VIEW_DESC gridUAVDesc{};
    gridUAVDesc.Format = DXGI_FORMAT_UNKNOWN;
    gridUAVDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    gridUAVDesc.Buffer.NumElements = totalTiles;
    hr = m_device->CreateUnorderedAccessView(
        newLightGrid.Get(), &gridUAVDesc, newLightGridUAV.GetAddressOf() );
    if ( FAILED( hr ) ) {
        LogError() << "Failed to create tiled light grid UAV. HRESULT: 0x"
            << std::hex << static_cast<unsigned long>(hr);
        return false;
    }

    m_LightIndexList = std::move( newIndexList );
    m_LightIndexListSRV = std::move( newIndexListSRV );
    m_LightIndexListUAV = std::move( newIndexListUAV );
    m_LightGrid = std::move( newLightGrid );
    m_LightGridSRV = std::move( newLightGridSRV );
    m_LightGridUAV = std::move( newLightGridUAV );
    m_lastNumTilesX = numTilesX;
    m_lastNumTilesY = numTilesY;

    SetDebugName( m_LightIndexList.Get(), "TiledDeferred_LightIndexList" );
    SetDebugName( m_LightIndexListSRV.Get(), "TiledDeferred_LightIndexList_SRV" );
    SetDebugName( m_LightIndexListUAV.Get(), "TiledDeferred_LightIndexList_UAV" );
    SetDebugName( m_LightGrid.Get(), "TiledDeferred_LightGrid" );
    SetDebugName( m_LightGridSRV.Get(), "TiledDeferred_LightGrid_SRV" );
    SetDebugName( m_LightGridUAV.Get(), "TiledDeferred_LightGrid_UAV" );
    return true;
}
XRESULT D3D11TiledDeferredShading::DrawPointlightLights(
    std::vector<VobLightInfo*>& lights,
    RenderToTextureBuffer& color,
    RenderToTextureBuffer& normals,
    RenderToTextureBuffer& specular,
    RenderToTextureBuffer& depthCopy ) {

    auto* graphicsEngine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);
    if ( !graphicsEngine || !color.IsValid() || !normals.IsValid()
        || !specular.IsValid() || !depthCopy.IsValid() ) {
        return XR_INVALID_ARG;
    }

    auto _ = graphicsEngine->RecordGraphicsEvent( GE_NAME( "TiledPointlightLights" ) );
    const auto& context = graphicsEngine->GetContext();
    if ( !context ) {
        return XR_FAILED;
    }

    auto cullResult = CullLights( lights, depthCopy );
    const INT2 resolution = Engine::GraphicsEngine->GetResolution();
    if ( resolution.x <= 0 || resolution.y <= 0 ) {
        cullResult.TiledLightCount = 0;
        cullResult.LegacyLights = lights;
    }

    if ( cullResult.TiledLightCount > 0 ) {
        const uint32_t numTilesX = (static_cast<uint32_t>(resolution.x) + TILE_SIZE - 1u) / TILE_SIZE;
        const uint32_t numTilesY = (static_cast<uint32_t>(resolution.y) + TILE_SIZE - 1u) / TILE_SIZE;
        auto csTiledShading = graphicsEngine->GetShaderManager().GetCShader( CShaderID::CS_TiledShading );
        auto& hdrBuffer = graphicsEngine->GetHDRBackBuffer();
        D3D11ConstantBuffer* shadingBuffer = csTiledShading
            ? csTiledShading->GetBuffer( "TiledShadingConstantBuffer" ).GetRawBuffer()
            : nullptr;

        const bool resourcesValid = csTiledShading && shadingBuffer
            && m_LightBufferSRV && m_LightGridSRV && m_LightIndexListSRV
            && hdrBuffer.IsValid() && hdrBuffer.GetUnorderedAccessView()
            && graphicsEngine->GetShadowMaps()
            && (!cullResult.HasShadowedTiledLights
                || (m_ShadowArrayCreated && m_ShadowCubeArraySRV));
        if ( !resourcesValid || csTiledShading->Apply() != XR_SUCCESS ) {
            LogWarn() << "Tiled shading resources are unavailable; using the legacy light path.";
            cullResult.TiledLightCount = 0;
            cullResult.HasShadowedTiledLights = false;
            cullResult.LegacyLights = lights;
        } else {
            auto& settings = Engine::GAPI->GetRendererState().RendererSettings;
            const XMMATRIX viewRaw = Engine::GAPI->GetViewMatrixXM();

            TiledShadingConstantBuffer shadeCB{};
            shadeCB.ViewportSize = float2( static_cast<float>(resolution.x), static_cast<float>(resolution.y) );
            {
                const auto& proj = Engine::GAPI->GetProjectionMatrix();
                shadeCB.ProjParams = float4( 1.0f / proj._11, 1.0f / proj._22, proj._43, proj._33 );
            }
            shadeCB.LimitLightIntensity = settings.LimitLightIntesity ? 1 : 0;
            shadeCB.NumTilesX = numTilesX;
            XMStoreFloat4x4( &shadeCB.InvView, XMMatrixInverse( nullptr, viewRaw ) );
            csTiledShading->GetBuffer( "TiledShadingConstantBuffer" ).Update( &shadeCB ).Bind();

            if ( GSky* sky = Engine::GAPI->GetSky() ) {
                auto& atmoCB = sky->GetAtmosphereCB();
                csTiledShading->GetBuffer( "Atmosphere" ).Update( &atmoCB ).Bind();
            }

            ID3D11RenderTargetView* nullRTV = nullptr;
            context->OMSetRenderTargets( 1, &nullRTV, nullptr );

            ID3D11ShaderResourceView* colorSRV = color.GetShaderResView().Get();
            ID3D11ShaderResourceView* normalsSRV = normals.GetShaderResView().Get();
            ID3D11ShaderResourceView* depthSRV = depthCopy.GetShaderResView().Get();
            ID3D11ShaderResourceView* specularSRV = specular.GetShaderResView().Get();
            context->CSSetShaderResources( 0, 1, &colorSRV );
            context->CSSetShaderResources( 1, 1, &normalsSRV );
            context->CSSetShaderResources( 2, 1, &depthSRV );
            context->CSSetShaderResources( 7, 1, &specularSRV );

            ID3D11SamplerState* linearSampler = graphicsEngine->GetDefaultSamplerState();
            context->CSSetSamplers( 0, 1, &linearSampler );

            ID3D11ShaderResourceView* lightSRV = m_LightBufferSRV.Get();
            ID3D11ShaderResourceView* gridSRV = m_LightGridSRV.Get();
            ID3D11ShaderResourceView* indexSRV = m_LightIndexListSRV.Get();
            context->CSSetShaderResources( 8, 1, &lightSRV );
            context->CSSetShaderResources( 9, 1, &gridSRV );
            context->CSSetShaderResources( 10, 1, &indexSRV );

            graphicsEngine->GetShadowMaps()->BindSamplerToCS( context.Get(), 2 );
            if ( cullResult.HasShadowedTiledLights ) {
                ID3D11ShaderResourceView* shadowSRV = m_ShadowCubeArraySRV.Get();
                context->CSSetShaderResources( 11, 1, &shadowSRV );
            }

            ID3D11UnorderedAccessView* hdrUAV = hdrBuffer.GetUnorderedAccessView().Get();
            context->CSSetUnorderedAccessViews( 0, 1, &hdrUAV, nullptr );
            context->Dispatch( numTilesX, numTilesY, 1 );

            ID3D11UnorderedAccessView* nullUAV = nullptr;
            context->CSSetUnorderedAccessViews( 0, 1, &nullUAV, nullptr );
            ID3D11ShaderResourceView* nullSRVs[12]{};
            context->CSSetShaderResources( 0, 12, nullSRVs );
            context->CSSetShader( nullptr, nullptr, 0 );

            ID3D11RenderTargetView* hdrRTV = hdrBuffer.GetRenderTargetView().Get();
            context->OMSetRenderTargets(
                1, &hdrRTV, graphicsEngine->GetDepthBuffer()->GetDepthStencilView().Get() );
        }
    }

    if ( !cullResult.LegacyLights.empty() ) {
        D3D11LegacyDeferredShading legacy;
        return legacy.DrawPointlightLights(
            cullResult.LegacyLights, color, normals, specular, depthCopy );
    }

    return XR_SUCCESS;
}
D3D11TiledDeferredShading::CullResult D3D11TiledDeferredShading::CullLights(
    std::vector<VobLightInfo*>& lights,
    RenderToTextureBuffer& depthCopy ) {

    auto graphicsEngine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);
    auto _ = graphicsEngine->RecordGraphicsEvent( GE_NAME( "CullLights" ) );
    auto& settings = Engine::GAPI->GetRendererState().RendererSettings;
    const bool fsr3TemporalShadows = settings.Upscaler == GothicRendererSettings::UPSCALER_FSR_3
        && settings.AntiAliasingMode == GothicRendererSettings::AA_FSR3
        && settings.ResolutionScalePercent < 100;
    const bool temporalPointlightShadows = fsr3TemporalShadows;
    const float pointlightRenderScale = fsr3TemporalShadows
        ? std::clamp( static_cast<float>(settings.ResolutionScalePercent) * 0.01f, 0.33f, 1.0f )
        : 1.0f;
    const float temporalPointlightFilterScale = fsr3TemporalShadows
        ? std::clamp( 1.0f / std::sqrt(pointlightRenderScale), 1.0f, 1.75f )
        : 1.0f;
    const float minimumTemporalShadowSoftness = temporalPointlightShadows
        ? 0.35f * temporalPointlightFilterScale
        : 0.0f;
    const float shadowFadeStartRatio = fsr3TemporalShadows
        ? std::clamp( 0.65f + (pointlightRenderScale - 0.5f) * 0.20f, 0.65f, 0.75f )
        : 0.75f;
    auto& context = graphicsEngine->GetContext();

    XMMATRIX viewRaw = Engine::GAPI->GetViewMatrixXM();
    XMMATRIX view = XMMatrixTranspose( viewRaw );

    INT2 resolution = Engine::GraphicsEngine->GetResolution();
    uint32_t numTilesX = (resolution.x + TILE_SIZE - 1) / TILE_SIZE;
    uint32_t numTilesY = (resolution.y + TILE_SIZE - 1) / TILE_SIZE;

    CullResult result = {};

    if ( !context || !depthCopy.IsValid() || resolution.x <= 0 || resolution.y <= 0 ) {
        result.LegacyLights = lights;
        return result;
    }

    if ( !m_LightBuffer || !m_LightBufferSRV || !m_IndexCounter || !m_IndexCounterUAV ) {
        LogError() << "Tiled deferred resources are missing; using legacy light culling.";
        result.LegacyLights = lights;
        return result;
    }

    if ( !EnsureBuffers( numTilesX, numTilesY ) ) {
        LogError() << "Tiled deferred resize failed; using legacy lighting.";
        result.LegacyLights = lights;
        return result;
    }
    // Partition lights: all lights go tiled where possible.
    // Shadowed lights with a tiled slot render directly into the shared array (no copies).
    // Shadowed lights without a tiled slot (256x256 or overflow) fall back to legacy.
    bool hasShadowedTiledLights = false;

    // Map light buffer
    D3D11_MAPPED_SUBRESOURCE mapped{};
    const HRESULT mapResult = context->Map(
        m_LightBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped );
    if ( FAILED( mapResult ) ) {
        LogError() << "Failed to map tiled light buffer. HRESULT: 0x"
            << std::hex << static_cast<unsigned long>(mapResult);
        result.LegacyLights = lights;
        return result;
    }
    if ( !mapped.pData ) {
        context->Unmap( m_LightBuffer.Get(), 0 );
        result.LegacyLights = lights;
        return result;
    }
    TiledPointLight* lightData = static_cast<TiledPointLight*>(mapped.pData);
    const auto camPos = Engine::GAPI->GetCameraPositionXM();

    for ( auto const& light : lights ) {
        if ( !light || !light->Vob ) {
            continue;
        }
        zCVobLight* vob = light->Vob;

        if ( !vob->IsEnabled() ) continue;

        // Check if this light has shadows
        D3D11PointLight* pl = nullptr;
        bool hasShadow = false;
        if ( settings.EnablePointlightShadows > 0 ) {
            pl = light->LightShadowBuffers ? static_cast<D3D11PointLight*>(light->LightShadowBuffers.get()) : nullptr;
            if ( pl && pl->IsInited() && pl->HasShadowMap( 1 ) ) {
                hasShadow = true;
            }
        }

        // A missing shadow slot must never suppress the light itself. Render it
        // unshadowed until a slot becomes available again.
        if ( hasShadow && pl->GetTiledSlot() < 0 ) {
            hasShadow = false;
        }

        if ( result.TiledLightCount >= MAX_TILED_LIGHTS ) {
            result.LegacyLights.push_back( light );
            continue;
        }

        vob->DoAnimation();

        float4 lightColor = float4( vob->GetLightColor() );
        if ( !light->AllowsPointlightShadows && !light->IsDynamicVobLight && !light->IsVisualFXLight ) {
            lightColor.x *= 0.35f;
            lightColor.y *= 0.35f;
            lightColor.z *= 0.35f;
        }
        float lightRange = vob->GetLightRange();
        float3 posWorld = light->GetEffectivePositionWorld();

        // Distance fade
        float dist;
        XMStoreFloat( &dist, XMVector3Length( XMLoadFloat3( posWorld.toXMFLOAT3() ) - camPos ) );

        const float shadowFadeEnd = std::max( settings.GetEffectiveVisualFXDrawRadius() - lightRange, 1.0f );
        const float shadowFadeStart = shadowFadeEnd * shadowFadeStartRatio;
        const float shadowFadeT = std::clamp(
            (shadowFadeEnd - dist) / std::max( shadowFadeEnd - shadowFadeStart, 1.0f ), 0.0f, 1.0f );
        const float shadowDistanceFade = shadowFadeT * shadowFadeT * (3.0f - 2.0f * shadowFadeT);

        if ( dist + lightRange < settings.GetEffectiveVisualFXDrawRadius() ) {
            float fadeEnd = settings.GetEffectiveVisualFXDrawRadius();
            float fadeFactor = std::min( 1.0f, std::max( 0.0f, ((fadeEnd - (dist + lightRange)) / lightRange) ) );
            lightColor.x *= fadeFactor;
            lightColor.y *= fadeFactor;
            lightColor.z *= fadeFactor;
        }

        float lightFactor = 1.2f;
        lightColor.x *= lightFactor;
        lightColor.y *= lightFactor;
        lightColor.z *= lightFactor;

        if ( lightColor.x <= 0.0f && lightColor.y <= 0.0f && lightColor.z <= 0.0f )
            continue;

        FXMVECTOR posWorldVec = XMLoadFloat3( posWorld.toXMFLOAT3() );
        XMFLOAT3 posView;
        XMStoreFloat3( &posView, XMVector3TransformCoord( posWorldVec, view ) );

        TiledPointLight& tl = lightData[result.TiledLightCount];
        tl.PositionView = posView;
        tl.Range = lightRange;
        tl.Color = XMFLOAT4( lightColor.x, lightColor.y, lightColor.z, lightColor.w );
        tl.PositionWorld = XMFLOAT3( posWorld.x, posWorld.y, posWorld.z );
        tl.ShadowStrength = shadowDistanceFade;
        tl.IsIndoor = light->Vob && light->Vob->IsIndoorVob() ? 1.0f : 0.0f;
        tl.IgnoreIndoorOutdoorLimit = light->IgnoreIndoorOutdoorLimit ? 1.0f : 0.0f;
        tl.ShadowSoftness = std::max( settings.ShadowSoftness * 2.0f, minimumTemporalShadowSoftness );

        if ( hasShadow ) {
            tl.ShadowCubeIndex = pl->GetTiledSlot();
            hasShadowedTiledLights = true;
        } else {
            tl.ShadowCubeIndex = -1;
        }

        result.TiledLightCount++;
        Engine::GAPI->GetRendererState().RendererInfo.FrameDrawnLights++;
    }

    context->Unmap( m_LightBuffer.Get(), 0 );

    // Dispatch CS_LightCulling if we have lights
    if ( result.TiledLightCount > 0 ) {
        auto csLightCull = graphicsEngine->GetShaderManager().GetCShader( CShaderID::CS_LightCulling );
        D3D11ConstantBuffer* cullingBuffer = csLightCull
            ? csLightCull->GetBuffer( "LightCullingConstantBuffer" ).GetRawBuffer()
            : nullptr;
        if ( !csLightCull || !cullingBuffer || !m_LightGridUAV || !m_LightIndexListUAV
            || csLightCull->Apply() != XR_SUCCESS ) {
            LogWarn() << "Tiled light-culling shader is unavailable; using legacy lighting.";
            result.TiledLightCount = 0;
            result.HasShadowedTiledLights = false;
            result.LegacyLights = lights;
            return result;
        }

        LightCullingConstantBuffer cullCB = {};
        cullCB.Proj = Engine::GAPI->GetProjectionMatrix();
        cullCB.ScreenWidth = static_cast<uint32_t>(resolution.x);
        cullCB.ScreenHeight = static_cast<uint32_t>(resolution.y);
        cullCB.TotalLights = result.TiledLightCount;
        cullCB.MaxBufferIndices = (numTilesX * numTilesY) * MAX_LIGHTS_PER_TILE;

        csLightCull->GetBuffer( "LightCullingConstantBuffer" ).Update( &cullCB ).Bind();

        context->CSSetShaderResources( 0, 1, depthCopy.GetShaderResView().GetAddressOf() );
        context->CSSetShaderResources( 1, 1, m_LightBufferSRV.GetAddressOf() );

        UINT clearVal[4] = { 0, 0, 0, 0 };
        context->ClearUnorderedAccessViewUint( m_IndexCounterUAV.Get(), clearVal );

        ID3D11UnorderedAccessView* uavs[3] = { m_LightGridUAV.Get(), m_LightIndexListUAV.Get(), m_IndexCounterUAV.Get() };
        context->CSSetUnorderedAccessViews( 0, 3, uavs, nullptr );

        context->Dispatch( numTilesX, numTilesY, 1 );

        // Unbind
        ID3D11UnorderedAccessView* nullUAVs[3] = { nullptr, nullptr, nullptr };
        context->CSSetUnorderedAccessViews( 0, 3, nullUAVs, nullptr );
        ID3D11ShaderResourceView* nullSRVs[2] = { nullptr, nullptr };
        context->CSSetShaderResources( 0, 2, nullSRVs );
        context->CSSetShader( nullptr, nullptr, 0 );
    }

    result.HasShadowedTiledLights = hasShadowedTiledLights;
    return result;
}
