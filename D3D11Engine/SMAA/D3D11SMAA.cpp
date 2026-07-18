#include "D3D11SMAA.h"
#include "../D3D11ShaderManager.h"
#include "../D3D11PFX_Effect.h"
#include "../Logger.h"
#include "DDSTextureLoader.h"

#include <array>
#include <algorithm>
#include <utility>

using namespace DirectX;
using namespace Microsoft::WRL;

namespace {
    class ScopedPipelineState {
    public:
        explicit ScopedPipelineState( ID3D11DeviceContext* context )
            : Context( context ) {
            if ( !Context ) return;

            Context->IAGetInputLayout( InputLayout.GetAddressOf() );
            Context->IAGetPrimitiveTopology( &Topology );
            Context->RSGetState( RasterizerState.GetAddressOf() );
            Context->OMGetDepthStencilState(
                DepthStencilState.GetAddressOf(), &StencilReference );
            Context->OMGetBlendState(
                BlendState.GetAddressOf(), BlendFactor, &SampleMask );
            Context->VSGetShader( VertexShader.GetAddressOf(), nullptr, nullptr );
            Context->PSGetShader( PixelShader.GetAddressOf(), nullptr, nullptr );
            Context->VSGetConstantBuffers(
                0, 1, VertexConstantBuffer.GetAddressOf() );
            Context->PSGetConstantBuffers(
                0, 1, PixelConstantBuffer.GetAddressOf() );
            Context->PSGetSamplers(
                0, static_cast<UINT>(PixelSamplers.size()), PixelSamplers.data() );
            Context->PSGetShaderResources(
                0, static_cast<UINT>(PixelResources.size()), PixelResources.data() );
            Captured = true;
        }

        ~ScopedPipelineState() {
            if ( Captured ) {
                Context->IASetInputLayout( InputLayout.Get() );
                Context->IASetPrimitiveTopology( Topology );
                Context->RSSetState( RasterizerState.Get() );
                Context->OMSetDepthStencilState(
                    DepthStencilState.Get(), StencilReference );
                Context->OMSetBlendState(
                    BlendState.Get(), BlendFactor, SampleMask );
                Context->VSSetShader( VertexShader.Get(), nullptr, 0 );
                Context->PSSetShader( PixelShader.Get(), nullptr, 0 );
                ID3D11Buffer* vertexBuffer = VertexConstantBuffer.Get();
                ID3D11Buffer* pixelBuffer = PixelConstantBuffer.Get();
                Context->VSSetConstantBuffers( 0, 1, &vertexBuffer );
                Context->PSSetConstantBuffers( 0, 1, &pixelBuffer );
                Context->PSSetSamplers(
                    0, static_cast<UINT>(PixelSamplers.size()), PixelSamplers.data() );
                Context->PSSetShaderResources(
                    0, static_cast<UINT>(PixelResources.size()), PixelResources.data() );
            }

            for ( auto* sampler : PixelSamplers ) {
                if ( sampler ) sampler->Release();
            }
            for ( auto* resource : PixelResources ) {
                if ( resource ) resource->Release();
            }
        }

        ScopedPipelineState( const ScopedPipelineState& ) = delete;
        ScopedPipelineState& operator=( const ScopedPipelineState& ) = delete;

    private:
        ComPtr<ID3D11DeviceContext> Context;
        ComPtr<ID3D11InputLayout> InputLayout;
        D3D11_PRIMITIVE_TOPOLOGY Topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
        ComPtr<ID3D11RasterizerState> RasterizerState;
        ComPtr<ID3D11DepthStencilState> DepthStencilState;
        UINT StencilReference = 0;
        ComPtr<ID3D11BlendState> BlendState;
        FLOAT BlendFactor[4]{};
        UINT SampleMask = 0xffffffff;
        ComPtr<ID3D11VertexShader> VertexShader;
        ComPtr<ID3D11PixelShader> PixelShader;
        ComPtr<ID3D11Buffer> VertexConstantBuffer;
        ComPtr<ID3D11Buffer> PixelConstantBuffer;
        std::array<ID3D11SamplerState*, 2> PixelSamplers{};
        std::array<ID3D11ShaderResourceView*, 5> PixelResources{};
        bool Captured = false;
    };

    bool ViewsAlias(
        ID3D11ShaderResourceView* source,
        ID3D11RenderTargetView* destination ) {
        if ( !source || !destination ) return false;

        ComPtr<ID3D11Resource> sourceResource;
        ComPtr<ID3D11Resource> destinationResource;
        source->GetResource( sourceResource.GetAddressOf() );
        destination->GetResource( destinationResource.GetAddressOf() );
        return sourceResource
            && sourceResource.Get() == destinationResource.Get();
    }
}

bool D3D11SMAA::HasResources() const {
    return m_vsEdge && m_psLumaEdge && m_vsBlend && m_psBlend
        && m_vsNeighbor && m_psNeighbor && m_areaTexSRV && m_searchTexSRV
        && m_constantBuffer && m_samplerLinear && m_samplerPoint
        && m_rasterizerState && m_disableDepthState && m_blendState;
}

bool D3D11SMAA::Init() {
    if ( m_initialized ) return HasResources();
    if ( m_initializationFailed ) return false;

    auto fail = [this]() {
        m_initializationFailed = true;
        return false;
    };
    if ( !m_device || !m_context || m_shaderPath.empty()
        || m_areaTexPath.empty() || m_searchTexPath.empty() ) {
        return fail();
    }

    const char* vsProfile = m_device->GetFeatureLevel() >= D3D_FEATURE_LEVEL_11_0
        ? "vs_5_0" : "vs_4_0";
    const char* psProfile = m_device->GetFeatureLevel() >= D3D_FEATURE_LEVEL_11_0
        ? "ps_5_0" : "ps_4_0";
    const std::vector<D3D_SHADER_MACRO> noMacros;

    auto createVS = [&]( const char* entry, ComPtr<ID3D11VertexShader>& shader ) {
        ComPtr<ID3DBlob> blob;
        const HRESULT compileResult = D3D11ShaderManager::CompileShaderFromFile(
            m_shaderPath.c_str(), entry, vsProfile, blob.GetAddressOf(), noMacros );
        return SUCCEEDED( compileResult ) && blob
            && SUCCEEDED( m_device->CreateVertexShader(
                blob->GetBufferPointer(), blob->GetBufferSize(), nullptr,
                shader.GetAddressOf() ) ) && shader;
    };
    auto createPS = [&]( const char* entry, ComPtr<ID3D11PixelShader>& shader ) {
        ComPtr<ID3DBlob> blob;
        const HRESULT compileResult = D3D11ShaderManager::CompileShaderFromFile(
            m_shaderPath.c_str(), entry, psProfile, blob.GetAddressOf(), noMacros );
        return SUCCEEDED( compileResult ) && blob
            && SUCCEEDED( m_device->CreatePixelShader(
                blob->GetBufferPointer(), blob->GetBufferSize(), nullptr,
                shader.GetAddressOf() ) ) && shader;
    };

    ComPtr<ID3D11VertexShader> vsEdge;
    ComPtr<ID3D11VertexShader> vsBlend;
    ComPtr<ID3D11VertexShader> vsNeighbor;
    ComPtr<ID3D11PixelShader> psLumaEdge;
    ComPtr<ID3D11PixelShader> psBlend;
    ComPtr<ID3D11PixelShader> psNeighbor;
    if ( !createVS( "EdgeDetectionVS", vsEdge )
        || !createPS( "LumaEdgeDetectionPS", psLumaEdge )
        || !createVS( "BlendingWeightCalculationVS", vsBlend )
        || !createPS( "BlendingWeightCalculationPS", psBlend )
        || !createVS( "NeighborhoodBlendingVS", vsNeighbor )
        || !createPS( "NeighborhoodBlendingPS", psNeighbor ) ) {
        LogError() << "SMAA shader initialization failed.";
        return fail();
    }

    ComPtr<ID3D11ShaderResourceView> areaTexture;
    ComPtr<ID3D11ShaderResourceView> searchTexture;
    if ( FAILED( CreateDDSTextureFromFile(
            m_device.Get(), m_areaTexPath.c_str(), nullptr,
            areaTexture.GetAddressOf() ) )
        || FAILED( CreateDDSTextureFromFile(
            m_device.Get(), m_searchTexPath.c_str(), nullptr,
            searchTexture.GetAddressOf() ) )
        || !areaTexture || !searchTexture ) {
        LogError() << "SMAA lookup textures could not be loaded.";
        return fail();
    }

    D3D11_BUFFER_DESC bufferDesc{};
    bufferDesc.Usage = D3D11_USAGE_DEFAULT;
    bufferDesc.ByteWidth = sizeof( SMAAConstants );
    bufferDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    ComPtr<ID3D11Buffer> constantBuffer;
    if ( FAILED( m_device->CreateBuffer(
            &bufferDesc, nullptr, constantBuffer.GetAddressOf() ) )
        || !constantBuffer ) {
        return fail();
    }

    D3D11_SAMPLER_DESC samplerDesc{};
    samplerDesc.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.ComparisonFunc = D3D11_COMPARISON_ALWAYS;
    samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
    ComPtr<ID3D11SamplerState> linearSampler;
    ComPtr<ID3D11SamplerState> pointSampler;
    if ( FAILED( m_device->CreateSamplerState(
            &samplerDesc, linearSampler.GetAddressOf() ) ) || !linearSampler ) {
        return fail();
    }
    samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    if ( FAILED( m_device->CreateSamplerState(
            &samplerDesc, pointSampler.GetAddressOf() ) ) || !pointSampler ) {
        return fail();
    }

    D3D11_RASTERIZER_DESC rasterDesc{};
    rasterDesc.FillMode = D3D11_FILL_SOLID;
    rasterDesc.CullMode = D3D11_CULL_NONE;
    rasterDesc.DepthClipEnable = TRUE;
    ComPtr<ID3D11RasterizerState> rasterizerState;
    if ( FAILED( m_device->CreateRasterizerState(
            &rasterDesc, rasterizerState.GetAddressOf() ) )
        || !rasterizerState ) {
        return fail();
    }

    D3D11_DEPTH_STENCIL_DESC depthDesc{};
    depthDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    depthDesc.DepthFunc = D3D11_COMPARISON_ALWAYS;
    ComPtr<ID3D11DepthStencilState> depthState;
    if ( FAILED( m_device->CreateDepthStencilState(
            &depthDesc, depthState.GetAddressOf() ) ) || !depthState ) {
        return fail();
    }

    D3D11_BLEND_DESC blendDesc{};
    blendDesc.RenderTarget[0].RenderTargetWriteMask =
        D3D11_COLOR_WRITE_ENABLE_ALL;
    ComPtr<ID3D11BlendState> blendState;
    if ( FAILED( m_device->CreateBlendState(
            &blendDesc, blendState.GetAddressOf() ) ) || !blendState ) {
        return fail();
    }

    m_vsEdge = std::move( vsEdge );
    m_psLumaEdge = std::move( psLumaEdge );
    m_vsBlend = std::move( vsBlend );
    m_psBlend = std::move( psBlend );
    m_vsNeighbor = std::move( vsNeighbor );
    m_psNeighbor = std::move( psNeighbor );
    m_areaTexSRV = std::move( areaTexture );
    m_searchTexSRV = std::move( searchTexture );
    m_constantBuffer = std::move( constantBuffer );
    m_samplerLinear = std::move( linearSampler );
    m_samplerPoint = std::move( pointSampler );
    m_rasterizerState = std::move( rasterizerState );
    m_disableDepthState = std::move( depthState );
    m_blendState = std::move( blendState );
    m_initialized = true;
    m_initializationFailed = false;
    m_metricsDirty = true;
    return true;
}

void D3D11SMAA::OnResize( int width, int height ) {
    const int maxDimension = m_device
        && m_device->GetFeatureLevel() >= D3D_FEATURE_LEVEL_11_0
        ? D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION
        : D3D10_REQ_TEXTURE2D_U_OR_V_DIMENSION;
    if ( width <= 0 || height <= 0
        || width > maxDimension || height > maxDimension ) {
        width = 0;
        height = 0;
    }
    if ( m_width == width && m_height == height ) return;

    m_width = width;
    m_height = height;
    m_metricsDirty = true;
}

bool D3D11SMAA::Render(
    ID3D11ShaderResourceView* inputSRV,
    ID3D11RenderTargetView* outputRTV,
    TexturePool* pool ) {
    if ( !m_device || !m_context || !inputSRV || !outputRTV || !pool
        || m_width <= 0 || m_height <= 0
        || ViewsAlias( inputSRV, outputRTV ) ) {
        return false;
    }
    if ( !m_initialized && !Init() ) return false;
    if ( !HasResources() ) return false;

    if ( m_metricsDirty ) {
        SMAAConstants constants{};
        constants.RT_Metrics = XMFLOAT4(
            1.0f / static_cast<float>(m_width),
            1.0f / static_cast<float>(m_height),
            static_cast<float>(m_width),
            static_cast<float>(m_height) );
        m_context->UpdateSubresource(
            m_constantBuffer.Get(), 0, nullptr, &constants, 0, 0 );
        m_metricsDirty = false;
    }

    auto edgesTexture = pool->Acquire(
        { m_width, m_height, DXGI_FORMAT_R8G8B8A8_UNORM } );
    auto blendTexture = pool->Acquire(
        { m_width, m_height, DXGI_FORMAT_R8G8B8A8_UNORM } );
    if ( !edgesTexture || !blendTexture
        || !edgesTexture->GetRenderTargetView()
        || !edgesTexture->GetShaderResView()
        || !blendTexture->GetRenderTargetView()
        || !blendTexture->GetShaderResView() ) {
        return false;
    }

    ScopedPipelineState pipelineState( m_context.Get() );
    D3D11PFXOutputStateGuard outputState( m_context.Get() );
    if ( !outputState.IsValid() ) return false;

    std::array<ID3D11ShaderResourceView*, 5> nullResources{};
    m_context->PSSetShaderResources(
        0, static_cast<UINT>(nullResources.size()), nullResources.data() );

    m_context->IASetInputLayout( nullptr );
    m_context->IASetPrimitiveTopology( D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST );
    m_context->RSSetState( m_rasterizerState.Get() );
    m_context->OMSetDepthStencilState( m_disableDepthState.Get(), 0 );
    m_context->OMSetBlendState( m_blendState.Get(), nullptr, 0xffffffff );

    ID3D11SamplerState* samplers[2] = {
        m_samplerLinear.Get(), m_samplerPoint.Get()
    };
    ID3D11Buffer* constantBuffer = m_constantBuffer.Get();
    m_context->PSSetSamplers( 0, 2, samplers );
    m_context->VSSetConstantBuffers( 0, 1, &constantBuffer );
    m_context->PSSetConstantBuffers( 0, 1, &constantBuffer );

    D3D11_VIEWPORT viewport{};
    viewport.Width = static_cast<float>(m_width);
    viewport.Height = static_cast<float>(m_height);
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;
    m_context->RSSetViewports( 1, &viewport );

    const FLOAT clearColor[4]{};
    ID3D11RenderTargetView* edgesRTV =
        edgesTexture->GetRenderTargetView().Get();
    m_context->ClearRenderTargetView( edgesRTV, clearColor );
    m_context->OMSetRenderTargets( 1, &edgesRTV, nullptr );
    m_context->VSSetShader( m_vsEdge.Get(), nullptr, 0 );
    m_context->PSSetShader( m_psLumaEdge.Get(), nullptr, 0 );
    m_context->PSSetShaderResources( 0, 1, &inputSRV );
    m_context->Draw( 3, 0 );
    m_context->PSSetShaderResources(
        0, static_cast<UINT>(nullResources.size()), nullResources.data() );

    ID3D11RenderTargetView* blendRTV =
        blendTexture->GetRenderTargetView().Get();
    m_context->ClearRenderTargetView( blendRTV, clearColor );
    m_context->OMSetRenderTargets( 1, &blendRTV, nullptr );
    m_context->VSSetShader( m_vsBlend.Get(), nullptr, 0 );
    m_context->PSSetShader( m_psBlend.Get(), nullptr, 0 );
    ID3D11ShaderResourceView* blendInputs[5] = {
        nullptr,
        edgesTexture->GetShaderResView().Get(),
        nullptr,
        m_areaTexSRV.Get(),
        m_searchTexSRV.Get()
    };
    m_context->PSSetShaderResources( 0, 5, blendInputs );
    m_context->Draw( 3, 0 );
    m_context->PSSetShaderResources(
        0, static_cast<UINT>(nullResources.size()), nullResources.data() );

    m_context->OMSetRenderTargets( 1, &outputRTV, nullptr );
    m_context->VSSetShader( m_vsNeighbor.Get(), nullptr, 0 );
    m_context->PSSetShader( m_psNeighbor.Get(), nullptr, 0 );
    ID3D11ShaderResourceView* neighborhoodInputs[3] = {
        inputSRV, nullptr, blendTexture->GetShaderResView().Get()
    };
    m_context->PSSetShaderResources( 0, 3, neighborhoodInputs );
    m_context->Draw( 3, 0 );
    m_context->PSSetShaderResources(
        0, static_cast<UINT>(nullResources.size()), nullResources.data() );
    return true;
}

void D3D11SMAA::ReleaseResources() {
    m_initialized = false;
    m_initializationFailed = false;
    m_metricsDirty = true;
    m_vsEdge.Reset();
    m_psLumaEdge.Reset();
    m_vsBlend.Reset();
    m_psBlend.Reset();
    m_vsNeighbor.Reset();
    m_psNeighbor.Reset();
    m_areaTexSRV.Reset();
    m_searchTexSRV.Reset();
    m_constantBuffer.Reset();
    m_samplerLinear.Reset();
    m_samplerPoint.Reset();
    m_rasterizerState.Reset();
    m_disableDepthState.Reset();
    m_blendState.Reset();
}
