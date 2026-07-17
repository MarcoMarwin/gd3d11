#include "pch.h"
#include "D3D11PFX_XeGTAO.h"

#include "D3D11CShader.h"
#include "D3D11ConstantBuffer.h"
#include "D3D11GraphicsEngine.h"
#include "D3D11PShader.h"
#include "D3D11PfxRenderer.h"
#include "D3D11ShaderManager.h"
#include "D3D11VShader.h"
#include "Engine.h"
#include "GothicAPI.h"
#include "RenderToTextureBuffer.h"
#include "Shaders/XeGTAO/XeGTAO.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace {
    constexpr UINT XeGTAODepthMipCount = XE_GTAO_DEPTH_MIP_LEVELS;
    static_assert( XeGTAODepthMipCount == 5 );

    struct AOCompositeConstantBuffer {
        float Strength;
        float Padding[3];
    };

    class ScopedTrackedRendererState {
    public:
        ScopedTrackedRendererState( D3D11GraphicsEngine* graphicsEngine, GothicAPI* gapi )
            : GraphicsEngine( graphicsEngine ),
            GAPI( gapi ),
            BlendState( gapi->GetRendererState().BlendState ),
            DepthState( gapi->GetRendererState().DepthState ),
            RasterizerState( gapi->GetRendererState().RasterizerState ) {
        }

        ~ScopedTrackedRendererState() {
            Restore();
        }

        bool Restore() {
            if ( Restored ) return true;
            Restored = true;
            if ( !GraphicsEngine || !GAPI ) return false;

            auto& state = GAPI->GetRendererState();
            state.BlendState = BlendState;
            state.DepthState = DepthState;
            state.RasterizerState = RasterizerState;
            state.BlendState.SetDirty();
            state.DepthState.SetDirty();
            state.RasterizerState.SetDirty();
            return GraphicsEngine->UpdateRenderStates() == XR_SUCCESS;
        }

        ScopedTrackedRendererState( const ScopedTrackedRendererState& ) = delete;
        ScopedTrackedRendererState& operator=( const ScopedTrackedRendererState& ) = delete;

    private:
        D3D11GraphicsEngine* GraphicsEngine;
        GothicAPI* GAPI;
        GothicBlendStateInfo BlendState;
        GothicDepthBufferStateInfo DepthState;
        GothicRasterizerStateInfo RasterizerState;
        bool Restored = false;
    };

    void ClearComputeIO( ID3D11DeviceContext* context ) {
        if ( !context ) return;
        std::array<ID3D11ShaderResourceView*, 8> nullResources{};
        std::array<ID3D11UnorderedAccessView*, XeGTAODepthMipCount> nullUAVs{};
        context->CSSetShaderResources(
            0, static_cast<UINT>(nullResources.size()), nullResources.data() );
        context->CSSetUnorderedAccessViews(
            0, static_cast<UINT>(nullUAVs.size()), nullUAVs.data(), nullptr );
    }

    class ScopedShaderBindingCleanup {
    public:
        explicit ScopedShaderBindingCleanup( ID3D11DeviceContext* context )
            : Context( context ) {
            if ( !Context ) return;
            Context->CSGetSamplers( 0, 1, OldCSSampler.GetAddressOf() );
            Context->PSGetSamplers( 0, 1, OldPSSampler.GetAddressOf() );
        }

        ~ScopedShaderBindingCleanup() {
            if ( !Context ) return;
            ClearComputeIO( Context.Get() );
            Context->CSSetShader( nullptr, nullptr, 0 );
            ID3D11ShaderResourceView* nullResource = nullptr;
            Context->PSSetShaderResources( 0, 1, &nullResource );
            ID3D11SamplerState* oldCSSampler = OldCSSampler.Get();
            ID3D11SamplerState* oldPSSampler = OldPSSampler.Get();
            Context->CSSetSamplers( 0, 1, &oldCSSampler );
            Context->PSSetSamplers( 0, 1, &oldPSSampler );
        }

    private:
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> Context;
        Microsoft::WRL::ComPtr<ID3D11SamplerState> OldCSSampler;
        Microsoft::WRL::ComPtr<ID3D11SamplerState> OldPSSampler;
    };

    bool HasUsableProjection( const XMFLOAT4X4& projection ) {
        const float* values = &projection._11;
        for ( size_t index = 0; index < 16; ++index ) {
            if ( !std::isfinite( values[index] ) ) return false;
        }
        return std::abs( projection._11 ) > 1.0e-6f
            && std::abs( projection._22 ) > 1.0e-6f;
    }
}

D3D11PFX_XeGTAO::D3D11PFX_XeGTAO( D3D11PfxRenderer* renderer )
    : D3D11PFX_Effect( renderer ) {
}

void D3D11PFX_XeGTAO::ReleaseResources() {
    m_workingDepth.Reset();
    m_workingDepthSRV.Reset();
    for ( auto& uav : m_workingDepthUAVs ) uav.Reset();
    m_aoTermA = {};
    m_aoTermB = {};
    m_edges.Reset();
    m_edgesSRV.Reset();
    m_edgesUAV.Reset();
    m_hilbertLUT.Reset();
    m_hilbertLUTSRV.Reset();
    m_pointClampSampler.Reset();
    m_width = 0;
    m_height = 0;
}

bool D3D11PFX_XeGTAO::CreateAOTermTexture(
    UINT width,
    UINT height,
    AOTermTexture& target ) {
    auto* engine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);
    const auto device = engine ? engine->GetDevice() : nullptr;
    if ( !device || width == 0 || height == 0 ) return false;

    AOTermTexture created;
    D3D11_TEXTURE2D_DESC textureDesc{};
    textureDesc.Width = width;
    textureDesc.Height = height;
    textureDesc.MipLevels = 1;
    textureDesc.ArraySize = 1;
    textureDesc.Format = DXGI_FORMAT_R8_TYPELESS;
    textureDesc.SampleDesc.Count = 1;
    textureDesc.Usage = D3D11_USAGE_DEFAULT;
    textureDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    if ( FAILED( device->CreateTexture2D(
            &textureDesc, nullptr, created.texture.GetAddressOf() ) ) ) {
        return false;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;
    srvDesc.Format = DXGI_FORMAT_R8_UINT;
    if ( FAILED( device->CreateShaderResourceView(
            created.texture.Get(), &srvDesc, created.uintSRV.GetAddressOf() ) ) ) {
        return false;
    }
    srvDesc.Format = DXGI_FORMAT_R8_UNORM;
    if ( FAILED( device->CreateShaderResourceView(
            created.texture.Get(), &srvDesc, created.unormSRV.GetAddressOf() ) ) ) {
        return false;
    }

    D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
    uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
    uavDesc.Format = DXGI_FORMAT_R8_UINT;
    if ( FAILED( device->CreateUnorderedAccessView(
            created.texture.Get(), &uavDesc, created.uintUAV.GetAddressOf() ) ) ) {
        return false;
    }

    target = std::move( created );
    return true;
}

bool D3D11PFX_XeGTAO::EnsureResources( UINT width, UINT height ) {
    const bool depthUAVsValid = std::all_of(
        m_workingDepthUAVs.begin(), m_workingDepthUAVs.end(),
        []( const auto& uav ) { return uav.Get() != nullptr; } );
    if ( m_width == width && m_height == height && m_workingDepth
        && m_workingDepthSRV && depthUAVsValid
        && m_aoTermA.texture && m_aoTermA.uintSRV && m_aoTermA.unormSRV
        && m_aoTermA.uintUAV && m_aoTermB.texture && m_aoTermB.uintSRV
        && m_aoTermB.unormSRV && m_aoTermB.uintUAV
        && m_edges && m_edgesSRV && m_edgesUAV
        && m_hilbertLUT && m_hilbertLUTSRV && m_pointClampSampler ) {
        return true;
    }

    if ( width < (1u << (XeGTAODepthMipCount - 1))
        || height < (1u << (XeGTAODepthMipCount - 1))
        || width > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION
        || height > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION ) {
        return false;
    }

    auto* engine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);
    const auto device = engine ? engine->GetDevice() : nullptr;
    if ( !device ) return false;

    ReleaseResources();

    Microsoft::WRL::ComPtr<ID3D11Texture2D> workingDepth;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> workingDepthSRV;
    std::array<Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView>,
        XeGTAODepthMipCount> workingDepthUAVs;
    AOTermTexture aoTermA;
    AOTermTexture aoTermB;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> edges;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> edgesSRV;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> edgesUAV;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> hilbertLUT;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> hilbertLUTSRV;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> pointClampSampler;

    D3D11_TEXTURE2D_DESC depthDesc{};
    depthDesc.Width = width;
    depthDesc.Height = height;
    depthDesc.MipLevels = XeGTAODepthMipCount;
    depthDesc.ArraySize = 1;
    depthDesc.Format = DXGI_FORMAT_R16_FLOAT;
    depthDesc.SampleDesc.Count = 1;
    depthDesc.Usage = D3D11_USAGE_DEFAULT;
    depthDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    if ( FAILED( device->CreateTexture2D(
            &depthDesc, nullptr, workingDepth.GetAddressOf() ) ) ) {
        return false;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC depthSRVDesc{};
    depthSRVDesc.Format = DXGI_FORMAT_R16_FLOAT;
    depthSRVDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    depthSRVDesc.Texture2D.MipLevels = XeGTAODepthMipCount;
    if ( FAILED( device->CreateShaderResourceView(
            workingDepth.Get(), &depthSRVDesc, workingDepthSRV.GetAddressOf() ) ) ) {
        return false;
    }

    for ( UINT mip = 0; mip < XeGTAODepthMipCount; ++mip ) {
        D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
        uavDesc.Format = DXGI_FORMAT_R16_FLOAT;
        uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
        uavDesc.Texture2D.MipSlice = mip;
        if ( FAILED( device->CreateUnorderedAccessView(
                workingDepth.Get(), &uavDesc,
                workingDepthUAVs[mip].GetAddressOf() ) ) ) {
            return false;
        }
    }

    if ( !CreateAOTermTexture( width, height, aoTermA )
        || !CreateAOTermTexture( width, height, aoTermB ) ) {
        return false;
    }

    D3D11_TEXTURE2D_DESC edgeDesc{};
    edgeDesc.Width = width;
    edgeDesc.Height = height;
    edgeDesc.MipLevels = 1;
    edgeDesc.ArraySize = 1;
    edgeDesc.Format = DXGI_FORMAT_R8_UNORM;
    edgeDesc.SampleDesc.Count = 1;
    edgeDesc.Usage = D3D11_USAGE_DEFAULT;
    edgeDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    if ( FAILED( device->CreateTexture2D(
            &edgeDesc, nullptr, edges.GetAddressOf() ) )
        || FAILED( device->CreateShaderResourceView(
            edges.Get(), nullptr, edgesSRV.GetAddressOf() ) )
        || FAILED( device->CreateUnorderedAccessView(
            edges.Get(), nullptr, edgesUAV.GetAddressOf() ) ) ) {
        return false;
    }

    std::array<uint16_t, 64 * 64> hilbertData{};
    for ( uint32_t y = 0; y < 64; ++y ) {
        for ( uint32_t x = 0; x < 64; ++x ) {
            hilbertData[y * 64 + x] = static_cast<uint16_t>(
                XeGTAO::HilbertIndex( x, y ) );
        }
    }

    D3D11_TEXTURE2D_DESC lutDesc{};
    lutDesc.Width = 64;
    lutDesc.Height = 64;
    lutDesc.MipLevels = 1;
    lutDesc.ArraySize = 1;
    lutDesc.Format = DXGI_FORMAT_R16_UINT;
    lutDesc.SampleDesc.Count = 1;
    lutDesc.Usage = D3D11_USAGE_IMMUTABLE;
    lutDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA lutData{};
    lutData.pSysMem = hilbertData.data();
    lutData.SysMemPitch = 64 * sizeof( uint16_t );
    if ( FAILED( device->CreateTexture2D(
            &lutDesc, &lutData, hilbertLUT.GetAddressOf() ) )
        || FAILED( device->CreateShaderResourceView(
            hilbertLUT.Get(), nullptr, hilbertLUTSRV.GetAddressOf() ) ) ) {
        return false;
    }

    D3D11_SAMPLER_DESC samplerDesc{};
    samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
    if ( FAILED( device->CreateSamplerState(
            &samplerDesc, pointClampSampler.GetAddressOf() ) ) ) {
        return false;
    }

    m_workingDepth = std::move( workingDepth );
    m_workingDepthSRV = std::move( workingDepthSRV );
    m_workingDepthUAVs = std::move( workingDepthUAVs );
    m_aoTermA = std::move( aoTermA );
    m_aoTermB = std::move( aoTermB );
    m_edges = std::move( edges );
    m_edgesSRV = std::move( edgesSRV );
    m_edgesUAV = std::move( edgesUAV );
    m_hilbertLUT = std::move( hilbertLUT );
    m_hilbertLUTSRV = std::move( hilbertLUTSRV );
    m_pointClampSampler = std::move( pointClampSampler );
    m_width = width;
    m_height = height;
    return true;
}

XRESULT D3D11PFX_XeGTAO::Render(
    ID3D11ShaderResourceView* depthSRV,
    ID3D11ShaderResourceView* normalsSRV,
    ID3D11RenderTargetView* outputRTV ) {
    if ( !depthSRV || !normalsSRV || !outputRTV || !FxRenderer ) {
        return XR_INVALID_ARG;
    }

    auto* engine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);
    auto* gapi = Engine::GAPI;
    const auto context = engine ? engine->GetContext() : nullptr;
    if ( !engine || !gapi || !context ) return XR_FAILED;

    const INT2 resolution = engine->GetResolution();
    if ( resolution.x <= 0 || resolution.y <= 0
        || resolution.x > static_cast<int>(D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION)
        || resolution.y > static_cast<int>(D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION)
        || !EnsureResources(
            static_cast<UINT>(resolution.x), static_cast<UINT>(resolution.y) ) ) {
        return XR_FAILED;
    }

    const XMFLOAT4X4 projection = gapi->GetProjectionMatrix();
    if ( !HasUsableProjection( projection ) ) return XR_FAILED;

    auto& rendererSettings = gapi->GetRendererState().RendererSettings;
    const auto& configuredSettings = rendererSettings.XegtaoSettings;
    XeGTAO::GTAOSettings gtaoSettings{};
    gtaoSettings.QualityLevel = std::clamp( configuredSettings.QualityLevel, 0, 3 );
    gtaoSettings.DenoisePasses = std::clamp( configuredSettings.DenoisePasses, 1, 3 );
    gtaoSettings.Radius = std::isfinite( configuredSettings.Radius )
        ? std::clamp( configuredSettings.Radius, 1.0f, 100000.0f )
        : 200.0f;

    constexpr std::array<CShaderID, 4> qualityShaders = {
        CShaderID::CS_PFX_XeGTAO_Low,
        CShaderID::CS_PFX_XeGTAO_Medium,
        CShaderID::CS_PFX_XeGTAO_High,
        CShaderID::CS_PFX_XeGTAO_Ultra
    };
    const auto prefilter = engine->GetShaderManager().GetCShader(
        CShaderID::CS_PFX_XeGTAO_Prefilter );
    const auto mainPass = engine->GetShaderManager().GetCShader(
        qualityShaders[static_cast<size_t>(gtaoSettings.QualityLevel)] );
    const auto denoisePass = engine->GetShaderManager().GetCShader(
        CShaderID::CS_PFX_XeGTAO_Denoise );
    const auto denoiseLastPass = engine->GetShaderManager().GetCShader(
        CShaderID::CS_PFX_XeGTAO_DenoiseLast );
    const auto fullscreenVS = engine->GetShaderManager().GetVShader( VShaderID::VS_PFX );
    const auto composite = engine->GetShaderManager().GetPShader( PShaderID::PS_PFX_AOComposite );
    if ( !prefilter || !mainPass || !denoisePass || !denoiseLastPass
        || !fullscreenVS || !composite
        || !prefilter->GetShader() || !mainPass->GetShader()
        || !denoisePass->GetShader() || !denoiseLastPass->GetShader()
        || !fullscreenVS->GetShader() || !composite->GetShader() ) {
        return XR_FAILED;
    }

    XeGTAO::GTAOConstants constants{};
    XeGTAO::GTAOUpdateConstants(
        constants, resolution.x, resolution.y, gtaoSettings,
        reinterpret_cast<const float*>(&projection), true, m_frameIndex );
    constants.NoiseIndex = rendererSettings.GetUsesTemporalReconstruction()
        ? static_cast<int>(m_frameIndex % 64) : 0;

    auto prefilterConstants = prefilter->GetBuffer( "GTAOConstantBuffer" );
    auto mainConstants = mainPass->GetBuffer( "GTAOConstantBuffer" );
    auto denoiseConstants = denoisePass->GetBuffer( "GTAOConstantBuffer" );
    auto denoiseLastConstants = denoiseLastPass->GetBuffer( "GTAOConstantBuffer" );
    prefilterConstants.Update( &constants );
    mainConstants.Update( &constants );
    denoiseConstants.Update( &constants );
    denoiseLastConstants.Update( &constants );

    const float rawStrength = rendererSettings.AOStrength;
    constexpr float XeGTAONormalizedStrength = 0.6f;
    AOCompositeConstantBuffer compositeConstants{};
    compositeConstants.Strength = (std::isfinite( rawStrength )
        ? std::clamp( rawStrength, 0.0f, 2.0f ) : 1.0f)
        * XeGTAONormalizedStrength;
    auto compositeBuffer = composite->GetBuffer( "AOCompositeConstantBuffer" );
    compositeBuffer.Update( &compositeConstants );

    if ( !prefilterConstants.Succeeded() || !mainConstants.Succeeded()
        || !denoiseConstants.Succeeded() || !denoiseLastConstants.Succeeded()
        || !compositeBuffer.Succeeded() ) {
        return XR_FAILED;
    }

    D3D11PFXOutputStateGuard outputState( context.Get() );
    if ( !outputState.IsValid() ) return XR_FAILED;
    ScopedTrackedRendererState rendererState( engine, gapi );
    ScopedShaderBindingCleanup bindingCleanup( context.Get() );

    XRESULT result = [&]() -> XRESULT {
        context->OMSetRenderTargets( 0, nullptr, nullptr );
        ClearComputeIO( context.Get() );
        ID3D11SamplerState* pointSampler = m_pointClampSampler.Get();
        context->CSSetSamplers( 0, 1, &pointSampler );

        if ( prefilter->Apply() != XR_SUCCESS ) return XR_FAILED;
        prefilterConstants.Bind();
        if ( !prefilterConstants.Succeeded() ) return XR_FAILED;
        context->CSSetShaderResources( 0, 1, &depthSRV );
        std::array<ID3D11UnorderedAccessView*, XeGTAODepthMipCount> depthUAVs{};
        for ( UINT index = 0; index < XeGTAODepthMipCount; ++index ) {
            depthUAVs[index] = m_workingDepthUAVs[index].Get();
        }
        context->CSSetUnorderedAccessViews(
            0, XeGTAODepthMipCount, depthUAVs.data(), nullptr );
        context->Dispatch(
            static_cast<UINT>((resolution.x + 15) / 16),
            static_cast<UINT>((resolution.y + 15) / 16), 1 );
        ClearComputeIO( context.Get() );

        if ( mainPass->Apply() != XR_SUCCESS ) return XR_FAILED;
        mainConstants.Bind();
        if ( !mainConstants.Succeeded() ) return XR_FAILED;
        ID3D11ShaderResourceView* mainResources[6] = {
            m_workingDepthSRV.Get(), normalsSRV, nullptr, nullptr,
            nullptr, m_hilbertLUTSRV.Get()
        };
        ID3D11UnorderedAccessView* mainOutputs[2] = {
            m_aoTermA.uintUAV.Get(), m_edgesUAV.Get()
        };
        context->CSSetShaderResources( 0, 6, mainResources );
        context->CSSetUnorderedAccessViews( 0, 2, mainOutputs, nullptr );
        context->Dispatch(
            static_cast<UINT>((resolution.x + 7) / 8),
            static_cast<UINT>((resolution.y + 7) / 8), 1 );
        ClearComputeIO( context.Get() );

        AOTermTexture* source = &m_aoTermA;
        AOTermTexture* destination = &m_aoTermB;
        for ( int pass = 0; pass < gtaoSettings.DenoisePasses; ++pass ) {
            const bool lastPass = pass == gtaoSettings.DenoisePasses - 1;
            const auto& denoise = lastPass ? denoiseLastPass : denoisePass;
            auto& denoiseBuffer = lastPass ? denoiseLastConstants : denoiseConstants;
            if ( denoise->Apply() != XR_SUCCESS ) return XR_FAILED;
            denoiseBuffer.Bind();
            if ( !denoiseBuffer.Succeeded() ) return XR_FAILED;

            ID3D11ShaderResourceView* denoiseResources[2] = {
                source->uintSRV.Get(), m_edgesSRV.Get()
            };
            ID3D11UnorderedAccessView* denoiseOutput = destination->uintUAV.Get();
            context->CSSetShaderResources( 0, 2, denoiseResources );
            context->CSSetUnorderedAccessViews( 0, 1, &denoiseOutput, nullptr );
            context->Dispatch(
                static_cast<UINT>((resolution.x + 15) / 16),
                static_cast<UINT>((resolution.y + 7) / 8), 1 );
            ClearComputeIO( context.Get() );
            std::swap( source, destination );
        }
        context->CSSetShader( nullptr, nullptr, 0 );

        auto& state = gapi->GetRendererState();
        state.BlendState.SetModulateBlending();
        state.BlendState.SetDirty();
        state.DepthState.DepthBufferCompareFunc =
            GothicDepthBufferStateInfo::CF_COMPARISON_ALWAYS;
        state.DepthState.DepthWriteEnabled = false;
        state.DepthState.SetDirty();
        state.RasterizerState.CullMode = GothicRasterizerStateInfo::CM_CULL_NONE;
        state.RasterizerState.SetDirty();

        if ( fullscreenVS->Apply() != XR_SUCCESS || composite->Apply() != XR_SUCCESS ) {
            return XR_FAILED;
        }
        compositeBuffer.Bind();
        if ( !compositeBuffer.Succeeded()
            || engine->SetViewport( ViewportInfo( 0, 0, resolution ) ) != XR_SUCCESS ) {
            return XR_FAILED;
        }

        context->OMSetRenderTargets( 1, &outputRTV, nullptr );
        ID3D11ShaderResourceView* finalAO = source->unormSRV.Get();
        context->PSSetShaderResources( 0, 1, &finalAO );
        context->PSSetSamplers( 0, 1, &pointSampler );
        return FxRenderer->DrawFullScreenQuad();
    }();

    if ( !rendererState.Restore() ) result = XR_FAILED;
    if ( result == XR_SUCCESS ) ++m_frameIndex;
    return result;
}