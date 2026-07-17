#include "pch.h"
#include "D3D11GShader.h"
#include "D3D11GraphicsEngineBase.h"
#include "Engine.h"
#include "GothicAPI.h"
#include "D3D11ConstantBuffer.h"
#include "D3D11ShaderManager.h"
#include "D3D11_Helpers.h"
#include "ShaderCategory.h"

extern bool FeatureLevel10Compatibility;

D3D11GShader::D3D11GShader() = default;

D3D11GShader::~D3D11GShader() = default;

/** Loads both shaders at the same time */
XRESULT D3D11GShader::LoadShader(
    const char* shaderPath,
    const std::vector<D3D_SHADER_MACRO>& macros,
    bool createStreamOutFromVS,
    int streamOutLayout ) {
    auto* engine = reinterpret_cast<D3D11GraphicsEngineBase*>(Engine::GraphicsEngine);
    const auto device = engine ? engine->GetDevice() : nullptr;
    if ( !shaderPath || !*shaderPath || !device ) return XR_INVALID_ARG;

    if ( Engine::GAPI
        && Engine::GAPI->GetRendererState().RendererSettings.EnableDebugLog ) {
        LogInfo() << "Compiling geometry shader: " << shaderPath;
    }

    const char* entryPoint = createStreamOutFromVS ? "VSMain" : "GSMain";
    const char* profile = createStreamOutFromVS
        ? (FeatureLevel10Compatibility ? "vs_4_0" : "vs_5_0")
        : (FeatureLevel10Compatibility ? "gs_4_0" : "gs_5_0");
    Microsoft::WRL::ComPtr<ID3DBlob> shaderBlob;
    const HRESULT compileResult = D3D11ShaderManager::CompileShaderFromFile(
        shaderPath, entryPoint, profile, shaderBlob.GetAddressOf(), macros );
    if ( FAILED( compileResult ) || !shaderBlob
        || !shaderBlob->GetBufferPointer() || shaderBlob->GetBufferSize() == 0 ) {
        return XR_FAILED;
    }

    Microsoft::WRL::ComPtr<ID3D11GeometryShader> shader;
    HRESULT createResult = E_FAIL;
    if ( !createStreamOutFromVS ) {
        createResult = device->CreateGeometryShader(
            shaderBlob->GetBufferPointer(), shaderBlob->GetBufferSize(), nullptr,
            shader.GetAddressOf() );
    } else {
        const int particleLayout = static_cast<int>(VERTEX_INPUT_LAYOUT_13);
        if ( streamOutLayout != particleLayout && streamOutLayout != 11 ) {
            LogError() << "Unsupported stream-output layout: " << streamOutLayout;
            return XR_INVALID_ARG;
        }

        struct ParticleStreamOutput {
            float3 Position;
            float3 Velocity;
        };
        const D3D11_SO_DECLARATION_ENTRY declaration[] = {
            { 0, "POSITION", 0, 0, 3, 0 },
            { 0, "VELOCITY", 0, 0, 3, 0 },
        };
        const UINT stride = sizeof( ParticleStreamOutput );
        createResult = device->CreateGeometryShaderWithStreamOutput(
            shaderBlob->GetBufferPointer(),
            shaderBlob->GetBufferSize(),
            declaration,
            static_cast<UINT>(std::size( declaration )),
            &stride,
            1,
            D3D11_SO_NO_RASTERIZED_STREAM,
            nullptr,
            shader.GetAddressOf() );
    }

    if ( FAILED( createResult ) || !shader ) {
        LogError() << "Failed to create "
            << (createStreamOutFromVS ? "stream-output " : "")
            << "geometry shader " << shaderPath << ": 0x"
            << std::hex << static_cast<unsigned long>(createResult);
        return XR_FAILED;
    }

    if ( FAILED( ReflectShaderResources( shaderBlob.Get() ) ) ) {
        return XR_FAILED;
    }

    SetDebugName( shader.Get(), shaderPath );
    GeometryShader = std::move( shader );
    return XR_SUCCESS;
}
/** Applies the shader. */
XRESULT D3D11GShader::Apply() {
    auto* engine = reinterpret_cast<D3D11GraphicsEngineBase*>(Engine::GraphicsEngine);
    const auto context = engine ? engine->GetContext() : nullptr;
    if ( !context || !GeometryShader ) return XR_FAILED;

    context->GSSetShader( GeometryShader.Get(), nullptr, 0 );
    return XR_SUCCESS;
}

void D3D11GShader::BindResource( StringID name, ID3D11ShaderResourceView* srv ) {
    const int inputIndex = GetInputIndex( name );
    auto* engine = reinterpret_cast<D3D11GraphicsEngineBase*>(Engine::GraphicsEngine);
    const auto context = engine ? engine->GetContext() : nullptr;
    if ( inputIndex < 0 || inputIndex >= D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT
        || !context ) {
        return;
    }
    context->GSSetShaderResources( static_cast<UINT>(inputIndex), 1, &srv );
}

void D3D11GShader::BindSampler( StringID name, ID3D11SamplerState* sampler ) {
    const int inputIndex = GetInputIndex( name );
    auto* engine = reinterpret_cast<D3D11GraphicsEngineBase*>(Engine::GraphicsEngine);
    const auto context = engine ? engine->GetContext() : nullptr;
    if ( inputIndex < 0 || inputIndex >= D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT
        || !context ) {
        return;
    }
    context->GSSetSamplers( static_cast<UINT>(inputIndex), 1, &sampler );
}

void D3D11GShader::BindBuffer( StringID name, D3D11ConstantBuffer* buffer ) {
    const int inputIndex = GetInputIndex( name );
    if ( inputIndex >= 0 && inputIndex < D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT
        && buffer && buffer->IsValid() ) {
        buffer->BindToGeometryShader( inputIndex );
    }
}

void D3D11GShader::BindBuffer( UINT slot, D3D11ConstantBuffer* buffer ) {
    if ( slot < D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT
        && buffer && buffer->IsValid() ) {
        buffer->BindToGeometryShader( static_cast<int>(slot) );
    }
}