#include "pch.h"
#include "D3D11PShader.h"

#include <d3dcompiler.h>

#include "D3D11GraphicsEngineBase.h"
#include "Engine.h"
#include "GothicAPI.h"
#include "D3D11ConstantBuffer.h"
#include "D3D11ShaderManager.h"
#include "D3D11_Helpers.h"
#include "StringID.h"

extern bool FeatureLevel10Compatibility;

D3D11PShader::D3D11PShader() = default;
D3D11PShader::~D3D11PShader() = default;

/** Loads both shaders at the same time */
XRESULT D3D11PShader::LoadShader(
    const ShaderInfo& shaderInfo,
    const std::vector<D3D_SHADER_MACRO>& macros,
    const char* filePath ) {
    auto* engine = reinterpret_cast<D3D11GraphicsEngineBase*>(Engine::GraphicsEngine);
    const auto device = engine ? engine->GetDevice() : nullptr;
    if ( !filePath || !*filePath || !device ) return XR_INVALID_ARG;

    if ( Engine::GAPI
        && Engine::GAPI->GetRendererState().RendererSettings.EnableDebugLog ) {
        LogInfo() << "Compiling pixel shader: " << shaderInfo.name;
    }

    const char* entryPoint = shaderInfo.entryPoint.empty()
        ? "PSMain" : shaderInfo.entryPoint.c_str();
    const char* profile = FeatureLevel10Compatibility ? "ps_4_0" : "ps_5_0";
    Microsoft::WRL::ComPtr<ID3DBlob> shaderBlob;
    const HRESULT compileResult = D3D11ShaderManager::CompileShaderFromFile(
        filePath, entryPoint, profile, shaderBlob.GetAddressOf(), macros );
    if ( FAILED( compileResult ) || !shaderBlob
        || !shaderBlob->GetBufferPointer() || shaderBlob->GetBufferSize() == 0 ) {
        return XR_FAILED;
    }

    Microsoft::WRL::ComPtr<ID3D11PixelShader> shader;
    const HRESULT createResult = device->CreatePixelShader(
        shaderBlob->GetBufferPointer(), shaderBlob->GetBufferSize(), nullptr,
        shader.GetAddressOf() );
    if ( FAILED( createResult ) || !shader ) {
        LogError() << "Failed to create pixel shader " << shaderInfo.name
            << ": 0x" << std::hex
            << static_cast<unsigned long>(createResult);
        return XR_FAILED;
    }

    if ( FAILED( ReflectShaderResources( shaderBlob.Get() ) ) ) {
        return XR_FAILED;
    }

    SetDebugName(
        shader.Get(), shaderInfo.name.empty() ? filePath : shaderInfo.name.c_str() );
    PixelShader = std::move( shader );
#ifdef DEBUG_D3D11
    this->filePath = filePath;
#endif
    return XR_SUCCESS;
}
/** Applies the shader. */
XRESULT D3D11PShader::Apply() {
    auto* engine = reinterpret_cast<D3D11GraphicsEngineBase*>(Engine::GraphicsEngine);
    const auto context = engine ? engine->GetContext() : nullptr;
    if ( !context || !PixelShader ) return XR_FAILED;

    context->PSSetShader( PixelShader.Get(), nullptr, 0 );
    return XR_SUCCESS;
}

void D3D11PShader::BindResource( StringID name, ID3D11ShaderResourceView* srv ) {
    const int inputIndex = GetInputIndex( name );
    auto* engine = reinterpret_cast<D3D11GraphicsEngineBase*>(Engine::GraphicsEngine);
    const auto context = engine ? engine->GetContext() : nullptr;
    if ( inputIndex < 0 || inputIndex >= D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT
        || !context ) {
        return;
    }
    context->PSSetShaderResources( static_cast<UINT>(inputIndex), 1, &srv );
}

void D3D11PShader::BindSampler( StringID name, ID3D11SamplerState* sampler ) {
    const int inputIndex = GetInputIndex( name );
    auto* engine = reinterpret_cast<D3D11GraphicsEngineBase*>(Engine::GraphicsEngine);
    const auto context = engine ? engine->GetContext() : nullptr;
    if ( inputIndex < 0 || inputIndex >= D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT
        || !context ) {
        return;
    }
    context->PSSetSamplers( static_cast<UINT>(inputIndex), 1, &sampler );
}

void D3D11PShader::BindBuffer( StringID name, D3D11ConstantBuffer* buffer ) {
    const int inputIndex = GetInputIndex( name );
    if ( inputIndex >= 0 && inputIndex < D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT
        && buffer && buffer->IsValid() ) {
        buffer->BindToPixelShader( inputIndex );
    }
}

void D3D11PShader::BindBuffer( UINT slot, D3D11ConstantBuffer* buffer ) {
    if ( slot < D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT
        && buffer && buffer->IsValid() ) {
        buffer->BindToPixelShader( static_cast<int>(slot) );
    }
}