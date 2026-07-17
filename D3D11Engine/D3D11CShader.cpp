#include "pch.h"
#include "D3D11CShader.h"

#include <d3d11shader.h>
#include <d3dcompiler.h>

#include "D3D11GraphicsEngineBase.h"
#include "Engine.h"
#include "GothicAPI.h"
#include "D3D11ShaderManager.h"
#include "D3D11_Helpers.h"
#include "StringID.h"

D3D11CShader::D3D11CShader() = default;

D3D11CShader::~D3D11CShader() = default;

/** Loads both shaders at the same time */
XRESULT D3D11CShader::LoadShader(
    const char* file,
    const char* entryPoint,
    const std::vector<D3D_SHADER_MACRO>& macros ) {
    auto* engine = reinterpret_cast<D3D11GraphicsEngineBase*>(Engine::GraphicsEngine);
    const auto device = engine ? engine->GetDevice() : nullptr;
    if ( !file || !*file || !device
        || device->GetFeatureLevel() < D3D_FEATURE_LEVEL_11_0 ) {
        return XR_INVALID_ARG;
    }

    const char* resolvedEntryPoint =
        entryPoint && *entryPoint ? entryPoint : "CSMain";
    if ( Engine::GAPI
        && Engine::GAPI->GetRendererState().RendererSettings.EnableDebugLog ) {
        LogInfo() << "Compiling compute shader: " << file;
    }

    Microsoft::WRL::ComPtr<ID3DBlob> shaderBlob;
    const HRESULT compileResult = D3D11ShaderManager::CompileShaderFromFile(
        file, resolvedEntryPoint, "cs_5_0", shaderBlob.GetAddressOf(), macros );
    if ( FAILED( compileResult ) || !shaderBlob
        || !shaderBlob->GetBufferPointer() || shaderBlob->GetBufferSize() == 0 ) {
        return XR_FAILED;
    }

    Microsoft::WRL::ComPtr<ID3D11ComputeShader> shader;
    const HRESULT createResult = device->CreateComputeShader(
        shaderBlob->GetBufferPointer(), shaderBlob->GetBufferSize(), nullptr,
        shader.GetAddressOf() );
    if ( FAILED( createResult ) || !shader ) {
        LogError() << "Failed to create compute shader " << file << ": 0x"
            << std::hex << static_cast<unsigned long>(createResult);
        return XR_FAILED;
    }

    if ( FAILED( ReflectShaderResources( shaderBlob.Get() ) ) ) {
        return XR_FAILED;
    }

    SetDebugName( shader.Get(), file );
    ComputeShader = std::move( shader );
    return XR_SUCCESS;
}
/** Applies the shader. */
XRESULT D3D11CShader::Apply() {
    auto* engine = reinterpret_cast<D3D11GraphicsEngineBase*>(Engine::GraphicsEngine);
    const auto context = engine ? engine->GetContext() : nullptr;
    if ( !context || !ComputeShader ) return XR_FAILED;

    context->CSSetShader( ComputeShader.Get(), nullptr, 0 );
    return XR_SUCCESS;
}

void D3D11CShader::BindResource( StringID name, ID3D11ShaderResourceView* srv ) {
    const int inputIndex = GetInputIndex( name );
    auto* engine = reinterpret_cast<D3D11GraphicsEngineBase*>(Engine::GraphicsEngine);
    const auto context = engine ? engine->GetContext() : nullptr;
    if ( inputIndex < 0 || inputIndex >= D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT
        || !context ) {
        return;
    }
    context->CSSetShaderResources( static_cast<UINT>(inputIndex), 1, &srv );
}

void D3D11CShader::BindSampler( StringID name, ID3D11SamplerState* sampler ) {
    const int inputIndex = GetInputIndex( name );
    auto* engine = reinterpret_cast<D3D11GraphicsEngineBase*>(Engine::GraphicsEngine);
    const auto context = engine ? engine->GetContext() : nullptr;
    if ( inputIndex < 0 || inputIndex >= D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT
        || !context ) {
        return;
    }
    context->CSSetSamplers( static_cast<UINT>(inputIndex), 1, &sampler );
}

void D3D11CShader::BindBuffer( StringID name, D3D11ConstantBuffer* buffer ) {
    const int inputIndex = GetInputIndex( name );
    if ( inputIndex >= 0 && inputIndex < D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT
        && buffer && buffer->IsValid() ) {
        buffer->BindToComputeShader( inputIndex );
    }
}

void D3D11CShader::BindBuffer( UINT slot, D3D11ConstantBuffer* buffer ) {
    if ( slot < D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT
        && buffer && buffer->IsValid() ) {
        buffer->BindToComputeShader( static_cast<int>(slot) );
    }
}