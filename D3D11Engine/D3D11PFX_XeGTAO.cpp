#include "pch.h"
#include "D3D11PFX_XeGTAO.h"

#include "D3D11CShader.h"
#include "D3D11ConstantBuffer.h"
#include "D3D11GraphicsEngine.h"
#include "D3D11PShader.h"
#include "D3D11PfxRenderer.h"
#include "D3D11ShaderManager.h"
#include "D3D11Texture.h"
#include "D3D11VShader.h"
#include "Engine.h"
#include "GothicAPI.h"
#include "RenderToTextureBuffer.h"
#include "Shaders/XeGTAO/XeGTAO.h"
#include <algorithm>
#include <cmath>
#include <memory>

namespace {
    constexpr UINT XeGTAODepthMipCount = XE_GTAO_DEPTH_MIP_LEVELS;

    struct AOCompositeConstantBuffer {
        float Strength;
        float Padding[3];
    };

    struct AOCompositeHybridConstantBuffer {
        float Strength;
        float FarRadiusPixels;
        float EffectRadius;
        float PixelScaleX;
        float FullWidth;
        float FullHeight;
        float FarWidth;
        float FarHeight;
    };

    constexpr float XeGTAOHybridFarRadiusPixels = 16.0f;
}

D3D11PFX_XeGTAO::D3D11PFX_XeGTAO( D3D11PfxRenderer* renderer )
    : D3D11PFX_Effect( renderer ) {
}

void D3D11PFX_XeGTAO::ReleaseResources() {
    m_workingDepth.Reset();
    m_workingDepthSRV.Reset();
    m_workingDepthHalfSRV.Reset();
    for ( auto& uav : m_workingDepthUAVs ) uav.Reset();
    m_aoTermA = {};
    m_aoTermB = {};
    m_halfAoTermA = {};
    m_halfAoTermB = {};
    m_edges.Reset();
    m_edgesSRV.Reset();
    m_edgesUAV.Reset();
    m_halfEdges.Reset();
    m_halfEdgesSRV.Reset();
    m_halfEdgesUAV.Reset();
    m_hilbertLUT.Reset();
    m_hilbertLUTSRV.Reset();
    m_pointClampSampler.Reset();
    m_aoOutput.Reset();
    m_aoOutputRTV.Reset();
    m_aoOutputSRV.Reset();
    m_width = 0;
    m_height = 0;
    m_halfWidth = 0;
    m_halfHeight = 0;
    m_preLightReady = false;
}

bool D3D11PFX_XeGTAO::CreateAOTermTexture( UINT width, UINT height, AOTermTexture& target ) {
    auto* engine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);
    auto* device = engine->GetDevice().Get();

    D3D11_TEXTURE2D_DESC textureDesc = {};
    textureDesc.Width = width;
    textureDesc.Height = height;
    textureDesc.MipLevels = 1;
    textureDesc.ArraySize = 1;
    textureDesc.Format = DXGI_FORMAT_R8_TYPELESS;
    textureDesc.SampleDesc.Count = 1;
    textureDesc.Usage = D3D11_USAGE_DEFAULT;
    textureDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    if ( FAILED( device->CreateTexture2D( &textureDesc, nullptr, target.texture.ReleaseAndGetAddressOf() ) ) ) return false;

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;
    srvDesc.Format = DXGI_FORMAT_R8_UINT;
    if ( FAILED( device->CreateShaderResourceView( target.texture.Get(), &srvDesc, target.uintSRV.ReleaseAndGetAddressOf() ) ) ) return false;
    srvDesc.Format = DXGI_FORMAT_R8_UNORM;
    if ( FAILED( device->CreateShaderResourceView( target.texture.Get(), &srvDesc, target.unormSRV.ReleaseAndGetAddressOf() ) ) ) return false;

    D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
    uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
    uavDesc.Format = DXGI_FORMAT_R8_UINT;
    if ( FAILED( device->CreateUnorderedAccessView( target.texture.Get(), &uavDesc, target.uintUAV.ReleaseAndGetAddressOf() ) ) ) return false;
    return true;
}

bool D3D11PFX_XeGTAO::EnsureNeutralAOSRV() {
    if ( m_neutralAOSRV ) return true;

    auto* engine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);
    if ( !engine || !engine->GetDevice().Get() ) return false;

    uint8_t white = 255;
    D3D11_TEXTURE2D_DESC textureDesc = {};
    textureDesc.Width = 1;
    textureDesc.Height = 1;
    textureDesc.MipLevels = 1;
    textureDesc.ArraySize = 1;
    textureDesc.Format = DXGI_FORMAT_R8_UNORM;
    textureDesc.SampleDesc.Count = 1;
    textureDesc.Usage = D3D11_USAGE_IMMUTABLE;
    textureDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA data = {};
    data.pSysMem = &white;
    data.SysMemPitch = sizeof(white);
    if ( FAILED( engine->GetDevice()->CreateTexture2D( &textureDesc, &data,
        m_neutralAO.ReleaseAndGetAddressOf() ) ) ) return false;
    return SUCCEEDED( engine->GetDevice()->CreateShaderResourceView(
        m_neutralAO.Get(), nullptr, m_neutralAOSRV.ReleaseAndGetAddressOf() ) );
}

bool D3D11PFX_XeGTAO::EnsureResources( UINT width, UINT height, bool needHybridResources, bool needAOMask ) {
    const bool hybridReady = !needHybridResources
        || (m_workingDepthHalfSRV && m_halfAoTermA.texture && m_halfAoTermB.texture && m_halfEdgesUAV);
    const bool aoMaskReady = !needAOMask || (m_aoOutput && m_aoOutputRTV && m_aoOutputSRV);
    if ( m_width == width && m_height == height && m_workingDepth && m_aoTermA.texture && m_aoTermB.texture
        && hybridReady && aoMaskReady && EnsureNeutralAOSRV() ) return true;

    ReleaseResources();
    auto* engine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);
    auto* device = engine->GetDevice().Get();

    D3D11_TEXTURE2D_DESC depthDesc = {};
    depthDesc.Width = width;
    depthDesc.Height = height;
    depthDesc.MipLevels = XeGTAODepthMipCount;
    depthDesc.ArraySize = 1;
    depthDesc.Format = DXGI_FORMAT_R16_FLOAT;
    depthDesc.SampleDesc.Count = 1;
    depthDesc.Usage = D3D11_USAGE_DEFAULT;
    depthDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    if ( FAILED( device->CreateTexture2D( &depthDesc, nullptr, m_workingDepth.ReleaseAndGetAddressOf() ) ) ) return false;

    D3D11_SHADER_RESOURCE_VIEW_DESC depthSRVDesc = {};
    depthSRVDesc.Format = DXGI_FORMAT_R16_FLOAT;
    depthSRVDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    depthSRVDesc.Texture2D.MipLevels = XeGTAODepthMipCount;
    if ( FAILED( device->CreateShaderResourceView( m_workingDepth.Get(), &depthSRVDesc, m_workingDepthSRV.ReleaseAndGetAddressOf() ) ) ) return false;

    if ( needHybridResources ) {
        // The hybrid A/B path uses mip 1 as a half-resolution working-depth
        // pyramid. The regular path does not allocate these extra views.
        D3D11_SHADER_RESOURCE_VIEW_DESC halfDepthSRVDesc = depthSRVDesc;
        halfDepthSRVDesc.Texture2D.MostDetailedMip = 1;
        halfDepthSRVDesc.Texture2D.MipLevels = XeGTAODepthMipCount - 1;
        if ( FAILED( device->CreateShaderResourceView( m_workingDepth.Get(), &halfDepthSRVDesc, m_workingDepthHalfSRV.ReleaseAndGetAddressOf() ) ) ) return false;
    }

    for ( UINT mip = 0; mip < XeGTAODepthMipCount; ++mip ) {
        D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
        uavDesc.Format = DXGI_FORMAT_R16_FLOAT;
        uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
        uavDesc.Texture2D.MipSlice = mip;
        if ( FAILED( device->CreateUnorderedAccessView( m_workingDepth.Get(), &uavDesc, m_workingDepthUAVs[mip].ReleaseAndGetAddressOf() ) ) ) return false;
    }

    if ( !CreateAOTermTexture( width, height, m_aoTermA ) || !CreateAOTermTexture( width, height, m_aoTermB ) ) return false;

    if ( needHybridResources ) {
        m_halfWidth = std::max( 1u, width / 2 );
        m_halfHeight = std::max( 1u, height / 2 );
        if ( !CreateAOTermTexture( m_halfWidth, m_halfHeight, m_halfAoTermA )
            || !CreateAOTermTexture( m_halfWidth, m_halfHeight, m_halfAoTermB ) ) return false;
    }

    D3D11_TEXTURE2D_DESC edgeDesc = {};
    edgeDesc.Width = width;
    edgeDesc.Height = height;
    edgeDesc.MipLevels = 1;
    edgeDesc.ArraySize = 1;
    edgeDesc.Format = DXGI_FORMAT_R8_UNORM;
    edgeDesc.SampleDesc.Count = 1;
    edgeDesc.Usage = D3D11_USAGE_DEFAULT;
    edgeDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    if ( FAILED( device->CreateTexture2D( &edgeDesc, nullptr, m_edges.ReleaseAndGetAddressOf() ) ) ) return false;
    if ( FAILED( device->CreateShaderResourceView( m_edges.Get(), nullptr, m_edgesSRV.ReleaseAndGetAddressOf() ) ) ) return false;
    if ( FAILED( device->CreateUnorderedAccessView( m_edges.Get(), nullptr, m_edgesUAV.ReleaseAndGetAddressOf() ) ) ) return false;

    if ( needHybridResources ) {
        edgeDesc.Width = m_halfWidth;
        edgeDesc.Height = m_halfHeight;
        if ( FAILED( device->CreateTexture2D( &edgeDesc, nullptr, m_halfEdges.ReleaseAndGetAddressOf() ) ) ) return false;
        if ( FAILED( device->CreateShaderResourceView( m_halfEdges.Get(), nullptr, m_halfEdgesSRV.ReleaseAndGetAddressOf() ) ) ) return false;
        if ( FAILED( device->CreateUnorderedAccessView( m_halfEdges.Get(), nullptr, m_halfEdgesUAV.ReleaseAndGetAddressOf() ) ) ) return false;
    }

    if ( needAOMask ) {
        D3D11_TEXTURE2D_DESC aoDesc = {};
        aoDesc.Width = width;
        aoDesc.Height = height;
        aoDesc.MipLevels = 1;
        aoDesc.ArraySize = 1;
        aoDesc.Format = DXGI_FORMAT_R8_UNORM;
        aoDesc.SampleDesc.Count = 1;
        aoDesc.Usage = D3D11_USAGE_DEFAULT;
        aoDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if ( FAILED( device->CreateTexture2D( &aoDesc, nullptr, m_aoOutput.ReleaseAndGetAddressOf() ) ) ) return false;
        if ( FAILED( device->CreateRenderTargetView( m_aoOutput.Get(), nullptr, m_aoOutputRTV.ReleaseAndGetAddressOf() ) ) ) return false;
        if ( FAILED( device->CreateShaderResourceView( m_aoOutput.Get(), nullptr, m_aoOutputSRV.ReleaseAndGetAddressOf() ) ) ) return false;
    }

    std::array<uint16_t, 64 * 64> hilbertData = {};
    for ( uint32_t y = 0; y < 64; ++y ) {
        for ( uint32_t x = 0; x < 64; ++x ) hilbertData[y * 64 + x] = static_cast<uint16_t>(XeGTAO::HilbertIndex( x, y ));
    }
    D3D11_TEXTURE2D_DESC lutDesc = {};
    lutDesc.Width = 64;
    lutDesc.Height = 64;
    lutDesc.MipLevels = 1;
    lutDesc.ArraySize = 1;
    lutDesc.Format = DXGI_FORMAT_R16_UINT;
    lutDesc.SampleDesc.Count = 1;
    lutDesc.Usage = D3D11_USAGE_IMMUTABLE;
    lutDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA lutData = {};
    lutData.pSysMem = hilbertData.data();
    lutData.SysMemPitch = 64 * sizeof(uint16_t);
    if ( FAILED( device->CreateTexture2D( &lutDesc, &lutData, m_hilbertLUT.ReleaseAndGetAddressOf() ) ) ) return false;
    if ( FAILED( device->CreateShaderResourceView( m_hilbertLUT.Get(), nullptr, m_hilbertLUTSRV.ReleaseAndGetAddressOf() ) ) ) return false;

    D3D11_SAMPLER_DESC samplerDesc = {};
    samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
    if ( FAILED( device->CreateSamplerState( &samplerDesc, m_pointClampSampler.ReleaseAndGetAddressOf() ) ) ) return false;

    m_width = width;
    m_height = height;
    return EnsureNeutralAOSRV();
}

XRESULT D3D11PFX_XeGTAO::Render( ID3D11ShaderResourceView* depthSRV,
                                  ID3D11ShaderResourceView* normalsSRV,
                                  ID3D11RenderTargetView* outputRTV ) {
    return RenderInternal( depthSRV, normalsSRV, outputRTV, false );
}

XRESULT D3D11PFX_XeGTAO::RenderToAO( ID3D11ShaderResourceView* depthSRV,
                                     ID3D11ShaderResourceView* normalsSRV ) {
    m_preLightReady = false;
    return RenderInternal( depthSRV, normalsSRV, nullptr, true );
}

ID3D11ShaderResourceView* D3D11PFX_XeGTAO::GetLightingAOSRV() const {
    if ( m_preLightReady && m_aoOutputSRV ) return m_aoOutputSRV.Get();
    if ( m_neutralAOSRV ) return m_neutralAOSRV.Get();
    auto* engine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);
    return engine && engine->GetWhiteTexture()
        ? engine->GetWhiteTexture()->GetShaderResourceView().Get() : nullptr;
}

XRESULT D3D11PFX_XeGTAO::RenderInternal( ID3D11ShaderResourceView* depthSRV,
                                         ID3D11ShaderResourceView* normalsSRV,
                                         ID3D11RenderTargetView* outputRTV,
                                         bool outputAOMask ) {
    if ( !depthSRV || !normalsSRV || (!outputRTV && !outputAOMask) ) return XR_FAILED;

    auto* engine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);
    auto& context = engine->GetContext();
    const INT2 resolution = engine->GetResolution();
    if ( resolution.x <= 0 || resolution.y <= 0 ) return XR_FAILED;

    auto& rendererSettings = Engine::GAPI->GetRendererState().RendererSettings;
    auto& settings = rendererSettings.XegtaoSettings;
    bool useHybrid = rendererSettings.GetEffectiveXeGTAOHybrid();
    if ( !EnsureResources( resolution.x, resolution.y, useHybrid, outputAOMask ) ) {
        // The A/B path is optional. If a device rejects its extra views, keep
        // the established full-resolution path available for the same frame.
        if ( !useHybrid || !EnsureResources( resolution.x, resolution.y, false, outputAOMask ) ) return XR_FAILED;
        useHybrid = false;
    }
    auto projection = Engine::GAPI->GetProjectionMatrix();

    XeGTAO::GTAOSettings gtaoSettings;
    gtaoSettings.QualityLevel = std::clamp( settings.QualityLevel, 0, 3 );
    gtaoSettings.DenoisePasses = std::clamp( settings.DenoisePasses, 1, 3 );
    gtaoSettings.Radius = std::max( 1.0f, settings.Radius );

    const CShaderID qualityShaders[] = {
        CShaderID::CS_PFX_XeGTAO_Low,
        CShaderID::CS_PFX_XeGTAO_Medium,
        CShaderID::CS_PFX_XeGTAO_High,
        CShaderID::CS_PFX_XeGTAO_Ultra
    };
    const CShaderID hybridNearShaders[] = {
        CShaderID::CS_PFX_XeGTAO_HybridNear_Low,
        CShaderID::CS_PFX_XeGTAO_HybridNear_Medium,
        CShaderID::CS_PFX_XeGTAO_HybridNear_High,
        CShaderID::CS_PFX_XeGTAO_HybridNear_Ultra
    };
    const CShaderID hybridFarShaders[] = {
        CShaderID::CS_PFX_XeGTAO_HybridFar_Low,
        CShaderID::CS_PFX_XeGTAO_HybridFar_Low,
        CShaderID::CS_PFX_XeGTAO_HybridFar_Medium,
        CShaderID::CS_PFX_XeGTAO_HybridFar_High
    };
    auto prefilter = engine->GetShaderManager().GetCShader( CShaderID::CS_PFX_XeGTAO_Prefilter );
    auto mainPass = engine->GetShaderManager().GetCShader( qualityShaders[gtaoSettings.QualityLevel] );
    std::shared_ptr<D3D11CShader> farPass;
    std::shared_ptr<D3D11PShader> hybridComposite;
    if ( useHybrid ) {
        mainPass = engine->GetShaderManager().GetCShader( hybridNearShaders[gtaoSettings.QualityLevel] );
        farPass = engine->GetShaderManager().GetCShader( hybridFarShaders[gtaoSettings.QualityLevel] );
    }
    auto denoisePass = engine->GetShaderManager().GetCShader( CShaderID::CS_PFX_XeGTAO_Denoise );
    auto denoiseLastPass = engine->GetShaderManager().GetCShader( CShaderID::CS_PFX_XeGTAO_DenoiseLast );
    auto fullscreenVS = engine->GetShaderManager().GetVShader( VShaderID::VS_PFX );
    auto composite = engine->GetShaderManager().GetPShader( PShaderID::PS_PFX_AOComposite );
    if ( useHybrid ) {
        hybridComposite = engine->GetShaderManager().GetPShader( PShaderID::PS_PFX_AOCompositeHybrid );
    }
    if ( useHybrid && (!mainPass || !farPass || !hybridComposite) ) {
        // Shader compilation is device-dependent; an optional test must never
        // remove the baseline AO path when its experimental shaders are absent.
        useHybrid = false;
        mainPass = engine->GetShaderManager().GetCShader( qualityShaders[gtaoSettings.QualityLevel] );
        farPass.reset();
        hybridComposite.reset();
    }
    if ( !prefilter || !mainPass || !denoisePass || !denoiseLastPass || !fullscreenVS || !composite ) return XR_FAILED;

    XeGTAO::GTAOConstants constants = {};
    XeGTAO::GTAOUpdateConstants( constants, resolution.x, resolution.y, gtaoSettings,
        reinterpret_cast<const float*>(&projection), true, m_frameIndex );
    constants.NoiseIndex = rendererSettings.GetUsesTemporalReconstruction() ? static_cast<int>(m_frameIndex % 64) : 0;
    ++m_frameIndex;

    ID3D11RenderTargetView* nullRTVs[8] = {};
    ID3D11ShaderResourceView* nullSRVs[8] = {};
    ID3D11UnorderedAccessView* nullUAVs[5] = {};
    ID3D11ShaderResourceView* nullLightingAO = nullptr;
    context->PSSetShaderResources( 10, 1, &nullLightingAO );
    context->OMSetRenderTargets( 8, nullRTVs, nullptr );
    context->CSSetShaderResources( 0, 8, nullSRVs );
    context->CSSetSamplers( 0, 1, m_pointClampSampler.GetAddressOf() );

    prefilter->Apply();
    prefilter->GetBuffer( "GTAOConstantBuffer" ).Update( &constants ).Bind();
    context->CSSetShaderResources( 0, 1, &depthSRV );
    ID3D11UnorderedAccessView* depthUAVs[XeGTAODepthMipCount] = {};
    for ( UINT i = 0; i < XeGTAODepthMipCount; ++i ) depthUAVs[i] = m_workingDepthUAVs[i].Get();
    context->CSSetUnorderedAccessViews( 0, XeGTAODepthMipCount, depthUAVs, nullptr );
    context->Dispatch( (resolution.x + 15) / 16, (resolution.y + 15) / 16, 1 );
    context->CSSetUnorderedAccessViews( 0, XeGTAODepthMipCount, nullUAVs, nullptr );
    context->CSSetShaderResources( 0, 8, nullSRVs );

    mainPass->Apply();
    mainPass->GetBuffer( "GTAOConstantBuffer" ).Update( &constants ).Bind();
    ID3D11ShaderResourceView* mainSRVs[6] = { m_workingDepthSRV.Get(), normalsSRV, nullptr, nullptr, nullptr, m_hilbertLUTSRV.Get() };
    context->CSSetShaderResources( 0, 6, mainSRVs );
    ID3D11UnorderedAccessView* mainUAVs[2] = { m_aoTermA.uintUAV.Get(), m_edgesUAV.Get() };
    context->CSSetUnorderedAccessViews( 0, 2, mainUAVs, nullptr );
    context->Dispatch( (resolution.x + 7) / 8, (resolution.y + 7) / 8, 1 );
    context->CSSetUnorderedAccessViews( 0, 2, nullUAVs, nullptr );
    context->CSSetShaderResources( 0, 8, nullSRVs );

    AOTermTexture* source = &m_aoTermA;
    AOTermTexture* destination = &m_aoTermB;
    for ( int pass = 0; pass < gtaoSettings.DenoisePasses; ++pass ) {
        const bool lastPass = pass == gtaoSettings.DenoisePasses - 1;
        auto denoise = lastPass ? denoiseLastPass : denoisePass;
        denoise->Apply();
        denoise->GetBuffer( "GTAOConstantBuffer" ).Update( &constants ).Bind();
        ID3D11ShaderResourceView* denoiseSRVs[2] = { source->uintSRV.Get(), m_edgesSRV.Get() };
        context->CSSetShaderResources( 0, 2, denoiseSRVs );
        ID3D11UnorderedAccessView* outputUAV = destination->uintUAV.Get();
        context->CSSetUnorderedAccessViews( 0, 1, &outputUAV, nullptr );
        context->Dispatch( (resolution.x + 15) / 16, (resolution.y + 7) / 8, 1 );
        context->CSSetUnorderedAccessViews( 0, 1, nullUAVs, nullptr );
        context->CSSetShaderResources( 0, 8, nullSRVs );
        std::swap( source, destination );
    }
    context->CSSetShader( nullptr, nullptr, 0 );

    AOTermTexture* farSource = nullptr;
    if ( useHybrid ) {
        // Reuse mip 1 of the already generated full-resolution depth pyramid;
        // the distant path therefore needs no second depth-prefilter pass.
        XeGTAO::GTAOConstants halfConstants = {};
        XeGTAO::GTAOUpdateConstants( halfConstants, static_cast<int>(m_halfWidth), static_cast<int>(m_halfHeight),
            gtaoSettings, reinterpret_cast<const float*>(&projection), true, m_frameIndex - 1 );
        halfConstants.NoiseIndex = constants.NoiseIndex;

        farPass->Apply();
        farPass->GetBuffer( "GTAOConstantBuffer" ).Update( &halfConstants ).Bind();
        ID3D11ShaderResourceView* farMainSRVs[6] = {
            m_workingDepthHalfSRV.Get(), normalsSRV, nullptr, nullptr, nullptr, m_hilbertLUTSRV.Get()
        };
        context->CSSetShaderResources( 0, 6, farMainSRVs );
        ID3D11UnorderedAccessView* farMainUAVs[2] = { m_halfAoTermA.uintUAV.Get(), m_halfEdgesUAV.Get() };
        context->CSSetUnorderedAccessViews( 0, 2, farMainUAVs, nullptr );
        context->Dispatch( (m_halfWidth + 7) / 8, (m_halfHeight + 7) / 8, 1 );
        context->CSSetUnorderedAccessViews( 0, 2, nullUAVs, nullptr );
        context->CSSetShaderResources( 0, 8, nullSRVs );

        farSource = &m_halfAoTermA;
        AOTermTexture* farDestination = &m_halfAoTermB;
        for ( int pass = 0; pass < gtaoSettings.DenoisePasses; ++pass ) {
            const bool lastPass = pass == gtaoSettings.DenoisePasses - 1;
            auto denoise = lastPass ? denoiseLastPass : denoisePass;
            denoise->Apply();
            denoise->GetBuffer( "GTAOConstantBuffer" ).Update( &halfConstants ).Bind();
            ID3D11ShaderResourceView* farDenoiseSRVs[2] = { farSource->uintSRV.Get(), m_halfEdgesSRV.Get() };
            context->CSSetShaderResources( 0, 2, farDenoiseSRVs );
            ID3D11UnorderedAccessView* outputUAV = farDestination->uintUAV.Get();
            context->CSSetUnorderedAccessViews( 0, 1, &outputUAV, nullptr );
            context->Dispatch( (m_halfWidth + 15) / 16, (m_halfHeight + 7) / 8, 1 );
            context->CSSetUnorderedAccessViews( 0, 1, nullUAVs, nullptr );
            context->CSSetShaderResources( 0, 8, nullSRVs );
            std::swap( farSource, farDestination );
        }
        context->CSSetShader( nullptr, nullptr, 0 );
    }

    if ( outputAOMask )
        Engine::GAPI->GetRendererState().BlendState.SetDefault();
    else
        Engine::GAPI->GetRendererState().BlendState.SetModulateBlending();
    Engine::GAPI->GetRendererState().BlendState.SetDirty();
    Engine::GAPI->GetRendererState().DepthState.DepthBufferCompareFunc = GothicDepthBufferStateInfo::CF_COMPARISON_ALWAYS;
    Engine::GAPI->GetRendererState().DepthState.DepthWriteEnabled = false;
    Engine::GAPI->GetRendererState().DepthState.SetDirty();

    fullscreenVS->Apply();
    // UI-normalized XeGTAO strength: 1.0 maps to the selected 60% composite strength.
    constexpr float XeGTAONormalizedStrength = 0.6f;
    if ( useHybrid ) {
        hybridComposite->Apply();
        AOCompositeHybridConstantBuffer compositeConstants = {
            rendererSettings.AOStrength * XeGTAONormalizedStrength,
            XeGTAOHybridFarRadiusPixels,
            gtaoSettings.Radius * 1.457f,
            std::abs( constants.NDCToViewMul_x_PixelSize.x ),
            static_cast<float>(m_width),
            static_cast<float>(m_height),
            static_cast<float>(m_halfWidth),
            static_cast<float>(m_halfHeight)
        };
        hybridComposite->GetBuffer( "AOCompositeHybridConstantBuffer" ).Update( &compositeConstants ).Bind();
    } else {
        composite->Apply();
        AOCompositeConstantBuffer compositeConstants = {
            rendererSettings.AOStrength * XeGTAONormalizedStrength, {}
        };
        composite->GetBuffer( "AOCompositeConstantBuffer" ).Update( &compositeConstants ).Bind();
    }

    D3D11_VIEWPORT viewport = {};
    viewport.Width = static_cast<float>(resolution.x);
    viewport.Height = static_cast<float>(resolution.y);
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;
    context->RSSetViewports( 1, &viewport );
    ID3D11RenderTargetView* compositeRTV = outputAOMask ? m_aoOutputRTV.Get() : outputRTV;
    context->OMSetRenderTargets( 1, &compositeRTV, nullptr );
    ID3D11ShaderResourceView* finalAO = source->unormSRV.Get();
    if ( useHybrid ) {
        ID3D11ShaderResourceView* hybridSRVs[5] = {
            finalAO, farSource->unormSRV.Get(), m_workingDepthSRV.Get(),
            m_workingDepthHalfSRV.Get(), normalsSRV
        };
        context->PSSetShaderResources( 0, 5, hybridSRVs );
    } else {
        context->PSSetShaderResources( 0, 1, &finalAO );
    }
    context->PSSetSamplers( 0, 1, m_pointClampSampler.GetAddressOf() );
    FxRenderer->DrawFullScreenQuad();
    context->PSSetShaderResources( 0, 8, nullSRVs );

    Engine::GAPI->GetRendererState().BlendState.SetDefault();
    Engine::GAPI->GetRendererState().BlendState.SetDirty();
    Engine::GAPI->GetRendererState().DepthState.DepthBufferCompareFunc = GothicDepthBufferStateInfo::DEFAULT_DEPTH_COMP_STATE;
    Engine::GAPI->GetRendererState().DepthState.DepthWriteEnabled = true;
    Engine::GAPI->GetRendererState().DepthState.SetDirty();
    if ( outputAOMask ) m_preLightReady = true;
    return XR_SUCCESS;
}
