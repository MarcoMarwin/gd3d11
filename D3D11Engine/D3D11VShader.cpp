#include "pch.h"
#include "D3D11VShader.h"

#include <d3dcompiler.h>

#include "D3D11GraphicsEngineBase.h"
#include "Engine.h"
#include "GothicAPI.h"
#include "D3D11ShaderManager.h"
#include "D3D11_Helpers.h"

extern bool FeatureLevel10Compatibility;

namespace {
    static const D3D11_INPUT_ELEMENT_DESC layout1[] =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "DIFFUSE", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };

    static const D3D11_INPUT_ELEMENT_DESC layout3[] =
    {
        { "POSITION", 0, DXGI_FORMAT_R16G16B16A16_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "POSITION", 1, DXGI_FORMAT_R16G16B16A16_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "POSITION", 2, DXGI_FORMAT_R16G16B16A16_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "POSITION", 3, DXGI_FORMAT_R16G16B16A16_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "BONEIDS", 0, DXGI_FORMAT_R8G8B8A8_UINT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "WEIGHTS", 0, DXGI_FORMAT_R16G16B16A16_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };

    static const D3D11_INPUT_ELEMENT_DESC layout4[] =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "DIFFUSE", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },

        { "INSTANCE_WORLD_MATRIX", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_INSTANCE_DATA, 1 },
        { "INSTANCE_WORLD_MATRIX", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_INSTANCE_DATA, 1 },
        { "INSTANCE_WORLD_MATRIX", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_INSTANCE_DATA, 1 },
        { "INSTANCE_WORLD_MATRIX", 3, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_INSTANCE_DATA, 1 },
        { "INSTANCE_COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 1, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_INSTANCE_DATA, 1 },
        { "INSTANCE_SCALE", 0, DXGI_FORMAT_R32G32_FLOAT, 1, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_INSTANCE_DATA, 1 },
    };

    static const D3D11_INPUT_ELEMENT_DESC layout6[] =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "DIFFUSE", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };

    static const D3D11_INPUT_ELEMENT_DESC layout7[] =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "DIFFUSE", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };

    static const D3D11_INPUT_ELEMENT_DESC layout9[] =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },

        { "INSTANCE_WORLD_MATRIX", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_INSTANCE_DATA, 1 },
        { "INSTANCE_WORLD_MATRIX", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_INSTANCE_DATA, 1 },
        { "INSTANCE_WORLD_MATRIX", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_INSTANCE_DATA, 1 },
        { "INSTANCE_WORLD_MATRIX", 3, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_INSTANCE_DATA, 1 },
    };

    static const D3D11_INPUT_ELEMENT_DESC layout10[] =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "DIFFUSE", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },

        { "INSTANCE_WORLD_MATRIX", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_INSTANCE_DATA, 1 },
        { "INSTANCE_WORLD_MATRIX", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_INSTANCE_DATA, 1 },
        { "INSTANCE_WORLD_MATRIX", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_INSTANCE_DATA, 1 },
        { "INSTANCE_WORLD_MATRIX", 3, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_INSTANCE_DATA, 1 },
        { "INSTANCE_PREV_WORLD_MATRIX", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_INSTANCE_DATA, 1 },
        { "INSTANCE_PREV_WORLD_MATRIX", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_INSTANCE_DATA, 1 },
        { "INSTANCE_PREV_WORLD_MATRIX", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_INSTANCE_DATA, 1 },
        { "INSTANCE_PREV_WORLD_MATRIX", 3, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_INSTANCE_DATA, 1 },
        { "INSTANCE_COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 1, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_INSTANCE_DATA, 1 },
        { "INSTANCE_WINDFLUENCE", 0, DXGI_FORMAT_R32G32_FLOAT, 1, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_INSTANCE_DATA, 1 },
        { "INSTANCE_WIND_META_INDEX", 0, DXGI_FORMAT_R32_UINT, 1, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_INSTANCE_DATA, 1 },
    };

    // Decal quad (per-vertex, slot 0 = ExVertexStruct) + per-instance world-view matrix (slot 1)
    static const D3D11_INPUT_ELEMENT_DESC layout15[] =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "DIFFUSE", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },

        { "INSTANCE_WORLD_MATRIX", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_INSTANCE_DATA, 1 },
        { "INSTANCE_WORLD_MATRIX", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_INSTANCE_DATA, 1 },
        { "INSTANCE_WORLD_MATRIX", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_INSTANCE_DATA, 1 },
        { "INSTANCE_WORLD_MATRIX", 3, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_INSTANCE_DATA, 1 },
    };

    static const D3D11_INPUT_ELEMENT_DESC layout11[] =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_INSTANCE_DATA, 1 },
        { "DIFFUSE", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_INSTANCE_DATA, 1 },
        { "SIZE", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_INSTANCE_DATA, 1 },
        { "TYPE", 0, DXGI_FORMAT_R32_UINT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_INSTANCE_DATA, 1 },
        { "VELOCITY", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_INSTANCE_DATA, 1 },
        { "LIGHTINGSCALE", 0, DXGI_FORMAT_R32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_INSTANCE_DATA, 1 },
    };

    static const D3D11_INPUT_ELEMENT_DESC layout13[] =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_INSTANCE_DATA, 1 },
        { "VELOCITY", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_INSTANCE_DATA, 1 },
    };

    static const D3D11_INPUT_ELEMENT_DESC layout14[] =
    {
        // Per-vertex data (Slot 0) - ExVertexStruct
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "DIFFUSE", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },

        // Per-instance data (Slot 1) - NodeAttachmentInstanceData
        { "INSTANCE_WORLD_MATRIX", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_INSTANCE_DATA, 1 },
        { "INSTANCE_WORLD_MATRIX", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_INSTANCE_DATA, 1 },
        { "INSTANCE_WORLD_MATRIX", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_INSTANCE_DATA, 1 },
        { "INSTANCE_WORLD_MATRIX", 3, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_INSTANCE_DATA, 1 },
        { "INSTANCE_PREV_WORLD_MATRIX", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_INSTANCE_DATA, 1 },
        { "INSTANCE_PREV_WORLD_MATRIX", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_INSTANCE_DATA, 1 },
        { "INSTANCE_PREV_WORLD_MATRIX", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_INSTANCE_DATA, 1 },
        { "INSTANCE_PREV_WORLD_MATRIX", 3, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_INSTANCE_DATA, 1 },
        { "INSTANCE_COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_INSTANCE_DATA, 1 },
    };

    static const std::map<EVERTEX_INPUT_LAYOUT, std::pair<const D3D11_INPUT_ELEMENT_DESC*, size_t>> lookupTable {
        { VERTEX_INPUT_LAYOUT_1, {layout1, std::size( layout1 )} },
        { VERTEX_INPUT_LAYOUT_3_VS_ExSkeletal, {layout3, std::size( layout3 )} },
        { VERTEX_INPUT_LAYOUT_4_VS_ExInstanced, {layout4, std::size( layout4 )} },
        { VERTEX_INPUT_LAYOUT_6_Lines, {layout6, std::size( layout6 )} },
        { VERTEX_INPUT_LAYOUT_7_VS_XYZRHW_DIF_T1, {layout7, std::size( layout7 )} },
        { VERTEX_INPUT_LAYOUT_10_VS_ExInstancedObj, {layout10, std::size( layout10 )} },
        { VERTEX_INPUT_LAYOUT_11_VS_ParticlePoint, {layout11, std::size( layout11 )} },
        { VERTEX_INPUT_LAYOUT_13, {layout13, std::size( layout13 )} },
        { VERTEX_INPUT_LAYOUT_14_VS_ExNodeInstanced, {layout14, std::size( layout14 )} },
        { VERTEX_INPUT_LAYOUT_15_VS_DecalInstanced, {layout15, std::size( layout15 )} },
    };
}


D3D11VShader::D3D11VShader() = default;

D3D11VShader::~D3D11VShader() = default;

/** Loads shader */
XRESULT D3D11VShader::LoadShader(
    const ShaderInfo& shaderInfo,
    const std::vector<D3D_SHADER_MACRO>& macros,
    const char* filePath ) {
    auto* engine = reinterpret_cast<D3D11GraphicsEngineBase*>(Engine::GraphicsEngine);
    const auto device = engine ? engine->GetDevice() : nullptr;
    if ( !filePath || !*filePath || !device ) return XR_INVALID_ARG;

    if ( Engine::GAPI
        && Engine::GAPI->GetRendererState().RendererSettings.EnableDebugLog ) {
        LogInfo() << "Compiling vertex shader: " << shaderInfo.name;
    }

    const char* entryPoint = shaderInfo.entryPoint.empty()
        ? "VSMain" : shaderInfo.entryPoint.c_str();
    const char* profile = FeatureLevel10Compatibility ? "vs_4_0" : "vs_5_0";
    Microsoft::WRL::ComPtr<ID3DBlob> shaderBlob;
    const HRESULT compileResult = D3D11ShaderManager::CompileShaderFromFile(
        filePath, entryPoint, profile, shaderBlob.GetAddressOf(), macros );
    if ( FAILED( compileResult ) || !shaderBlob
        || !shaderBlob->GetBufferPointer() || shaderBlob->GetBufferSize() == 0 ) {
        return XR_FAILED;
    }

    Microsoft::WRL::ComPtr<ID3D11VertexShader> shader;
    HRESULT createResult = device->CreateVertexShader(
        shaderBlob->GetBufferPointer(), shaderBlob->GetBufferSize(), nullptr,
        shader.GetAddressOf() );
    if ( FAILED( createResult ) || !shader ) {
        LogError() << "Failed to create vertex shader " << shaderInfo.name
            << ": 0x" << std::hex
            << static_cast<unsigned long>(createResult);
        return XR_FAILED;
    }

    Microsoft::WRL::ComPtr<ID3D11InputLayout> inputLayout;
    if ( shaderInfo.layout != VERTEX_INPUT_LAYOUT_NONE ) {
        const auto layout = lookupTable.find( shaderInfo.layout );
        if ( layout == lookupTable.end() ) {
            LogError() << "Invalid input layout for vertex shader "
                << shaderInfo.name << ": "
                << static_cast<unsigned int>(shaderInfo.layout);
            return XR_INVALID_ARG;
        }

        createResult = device->CreateInputLayout(
            layout->second.first,
            static_cast<UINT>(layout->second.second),
            shaderBlob->GetBufferPointer(),
            shaderBlob->GetBufferSize(),
            inputLayout.GetAddressOf() );
        if ( FAILED( createResult ) || !inputLayout ) {
            LogError() << "Failed to create input layout for "
                << shaderInfo.name << ": 0x" << std::hex
                << static_cast<unsigned long>(createResult);
            return XR_FAILED;
        }
    }

    if ( FAILED( ReflectShaderResources( shaderBlob.Get() ) ) ) {
        return XR_FAILED;
    }

    SetDebugName(
        shader.Get(), shaderInfo.name.empty() ? filePath : shaderInfo.name.c_str() );
    VertexShader = std::move( shader );
    InputLayout = std::move( inputLayout );
#ifdef DEBUG_D3D11
    this->filePath = filePath;
#endif
    return XR_SUCCESS;
}
/** Applies the shader. */
XRESULT D3D11VShader::Apply() {
    auto* engine = reinterpret_cast<D3D11GraphicsEngineBase*>(Engine::GraphicsEngine);
    const auto context = engine ? engine->GetContext() : nullptr;
    if ( !context || !VertexShader ) return XR_FAILED;

    context->IASetInputLayout( InputLayout.Get() );
    context->VSSetShader( VertexShader.Get(), nullptr, 0 );
    return XR_SUCCESS;
}

void D3D11VShader::BindResource( StringID name, ID3D11ShaderResourceView* srv ) {
    const int inputIndex = GetInputIndex( name );
    auto* engine = reinterpret_cast<D3D11GraphicsEngineBase*>(Engine::GraphicsEngine);
    const auto context = engine ? engine->GetContext() : nullptr;
    if ( inputIndex < 0 || inputIndex >= D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT
        || !context ) {
        return;
    }
    context->VSSetShaderResources( static_cast<UINT>(inputIndex), 1, &srv );
}

void D3D11VShader::BindSampler( StringID name, ID3D11SamplerState* sampler ) {
    const int inputIndex = GetInputIndex( name );
    auto* engine = reinterpret_cast<D3D11GraphicsEngineBase*>(Engine::GraphicsEngine);
    const auto context = engine ? engine->GetContext() : nullptr;
    if ( inputIndex < 0 || inputIndex >= D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT
        || !context ) {
        return;
    }
    context->VSSetSamplers( static_cast<UINT>(inputIndex), 1, &sampler );
}

void D3D11VShader::BindBuffer( StringID name, D3D11ConstantBuffer* buffer ) {
    const int inputIndex = GetInputIndex( name );
    if ( inputIndex >= 0 && inputIndex < D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT
        && buffer && buffer->IsValid() ) {
        buffer->BindToVertexShader( inputIndex );
    }
}

void D3D11VShader::BindBuffer( UINT slot, D3D11ConstantBuffer* buffer ) {
    if ( slot < D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT
        && buffer && buffer->IsValid() ) {
        buffer->BindToVertexShader( static_cast<int>(slot) );
    }
}