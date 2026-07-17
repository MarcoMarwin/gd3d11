#include "pch.h"
#include "D3D11GraphicsShader.h"
#include <d3dcompiler.h>
#include <limits>

#include "D3D11ConstantBuffer.h"
#include "D3D11_Helpers.h"

GraphicsShaderConstantBuffer& GraphicsShaderConstantBuffer::Bind() {
    return Bind(slot);
}

GraphicsShaderConstantBuffer& GraphicsShaderConstantBuffer::Bind(UINT slot) {
    if ( succeeded && buffer && shader && slot < MAX_SHADER_CB ) {
        shader->BindBuffer( slot, buffer );
    } else {
        succeeded = false;
    }
    return *this;
}

int32_t D3D11GraphicsShader::GetInputIndex( StringID name )
{
    auto kvp = InputSemanticToIndex.find( name );
    if (kvp != InputSemanticToIndex.end()) {
        return kvp->second;
    }
#ifdef DEBUG_D3D11
    // LogError() << "Tried to find input index for semantic '" << name << "' but it was not found in the shader!";
#endif
    return -1;
}

GraphicsShaderConstantBuffer D3D11GraphicsShader::GetBuffer(StringID name) {
    auto kvp = ConstantBuffersByName.find( name );
    if (kvp != ConstantBuffersByName.end()) {
        return GraphicsShaderConstantBuffer(kvp->second.first, kvp->second.second, this);
    }
#ifdef DEBUG_D3D11
    // LogError() << "Tried to find constant buffer for semantic '" << name << "' but it was not registered!";
#endif
    return GraphicsShaderConstantBuffer(nullptr, INVALID_SHADER_CB_SLOT, nullptr);
}

GraphicsShaderConstantBuffer D3D11GraphicsShader::GetBuffer(UINT slot) {
    if (slot >= ConstantBufferIndexBySlot.size()) {
        return {nullptr, INVALID_SHADER_CB_SLOT, nullptr};
    }
    const auto idx = ConstantBufferIndexBySlot[slot];
    if ( idx >= ConstantBuffers.size() ) {
        return { nullptr, INVALID_SHADER_CB_SLOT, nullptr };
    }
    return GraphicsShaderConstantBuffer(ConstantBuffers[idx].get(), slot, this);
}

HRESULT D3D11GraphicsShader::ReflectShaderResources( ID3DBlob* shaderBlob ) {
    if ( !shaderBlob ) {
        return E_INVALIDARG;
    }

    Microsoft::WRL::ComPtr<ID3D11ShaderReflection> pReflection;
    HRESULT hr = D3DReflect(
        shaderBlob->GetBufferPointer(),
        shaderBlob->GetBufferSize(),
        IID_PPV_ARGS( &pReflection )
    );
    if ( FAILED( hr ) ) {
        return hr;
    }

    D3D11_SHADER_DESC shaderDesc{};
    hr = pReflection->GetDesc( &shaderDesc );
    if ( FAILED( hr ) ) {
        return hr;
    }

    auto previousInputs = std::move( InputSemanticToIndex );
    auto previousBuffersByName = std::move( ConstantBuffersByName );
    auto previousBuffers = std::move( ConstantBuffers );
    const auto previousIndices = ConstantBufferIndexBySlot;

    try {
        hr = OnReflectShader( shaderBlob, pReflection.Get(), shaderDesc );
    } catch ( const std::bad_alloc& ) {
        hr = E_OUTOFMEMORY;
    } catch ( ... ) {
        hr = E_FAIL;
    }

    if ( FAILED( hr ) ) {
        InputSemanticToIndex = std::move( previousInputs );
        ConstantBuffersByName = std::move( previousBuffersByName );
        ConstantBuffers = std::move( previousBuffers );
        ConstantBufferIndexBySlot = previousIndices;
    }
    return hr;
}

HRESULT D3D11GraphicsShader::OnReflectShader(
    ID3DBlob* blob,
    ID3D11ShaderReflection* pReflection,
    const D3D11_SHADER_DESC& shaderDesc ) {
    if ( !blob || !pReflection ) {
        return E_INVALIDARG;
    }

    for ( auto& constantBuffer : ConstantBuffers ) {
        constantBuffer.reset();
    }
    ConstantBufferIndexBySlot.fill( INVALID_SHADER_CB_SLOT );
    ConstantBuffersByName.clear();
    ConstantBuffersByName.reserve( shaderDesc.ConstantBuffers );
    InputSemanticToIndex.clear();
    InputSemanticToIndex.reserve( shaderDesc.BoundResources );

    size_t cbIndex = 0;
    for ( UINT i = 0; i < shaderDesc.BoundResources; ++i ) {
        D3D11_SHADER_INPUT_BIND_DESC resourceDesc{};
        HRESULT hr = pReflection->GetResourceBindingDesc( i, &resourceDesc );
        if ( FAILED( hr ) ) {
            return hr;
        }

        OnReflectShaderResource( pReflection, shaderDesc, resourceDesc );
        if ( resourceDesc.Type != D3D_SIT_CBUFFER ) {
            continue;
        }

        if ( cbIndex >= ConstantBuffers.size() || resourceDesc.BindPoint >= ConstantBufferIndexBySlot.size() ) {
            LogError() << "Shader constant-buffer binding exceeds D3D11 limits: " << resourceDesc.Name;
            return E_INVALIDARG;
        }

        ID3D11ShaderReflectionConstantBuffer* reflectedBuffer = pReflection->GetConstantBufferByName( resourceDesc.Name );
        if ( !reflectedBuffer ) {
            return E_FAIL;
        }

        D3D11_SHADER_BUFFER_DESC cbDesc{};
        hr = reflectedBuffer->GetDesc( &cbDesc );
        if ( FAILED( hr ) ) {
            return hr;
        }

        const uint64_t requestedSize = static_cast<uint64_t>(cbDesc.Size) * resourceDesc.BindCount;
        const uint64_t paddedSize = (requestedSize + 15u) & ~uint64_t(15u);
        if ( paddedSize == 0 || paddedSize > std::numeric_limits<UINT>::max() ) {
            return E_INVALIDARG;
        }

        auto constantBuffer = std::make_unique<D3D11ConstantBuffer>( static_cast<UINT>(paddedSize), nullptr );
        if ( !constantBuffer->IsValid() ) {
            return E_FAIL;
        }
#ifdef DEBUG_D3D11
        SetDebugName( constantBuffer->Get().Get(), resourceDesc.Name );
#endif
        ConstantBuffersByName[StringID::make( resourceDesc.Name )] = {
            constantBuffer.get(), static_cast<int32_t>(resourceDesc.BindPoint)
        };
        ConstantBufferIndexBySlot[resourceDesc.BindPoint] = static_cast<byte>(cbIndex);
        ConstantBuffers[cbIndex] = std::move( constantBuffer );
        ++cbIndex;
    }

    return S_OK;
}

void D3D11GraphicsShader::OnReflectShaderResource(
    ID3D11ShaderReflection* pReflection,
    const D3D11_SHADER_DESC& shaderDesc, 
    const D3D11_SHADER_INPUT_BIND_DESC& resourceDesc)
{
    InputSemanticToIndex[StringID::make(resourceDesc.Name)] = resourceDesc.BindPoint;
}
