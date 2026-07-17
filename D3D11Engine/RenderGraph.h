#pragma once
#include "pch.h"
#include "RGTextureDesc.h"
#include <memory>
#include <vector>

#include "Logger.h"
#include "RenderPass.h"
#include "TexturePool.h"

// Helpers for bit-packing
constexpr uint32_t RG_MAX_RESOURCE_INDEX = (RG_INVALID_HANDLE >> 1) - 1;

inline bool IsValidHandle( RGResourceHandle handle ) noexcept {
    return handle != RG_INVALID_HANDLE;
}

inline bool IsExternalHandle( RGResourceHandle handle ) noexcept {
    return IsValidHandle( handle ) && (handle & 1u) != 0;
}

inline uint32_t GetHandleIndex( RGResourceHandle handle ) noexcept {
    return IsValidHandle( handle ) ? handle >> 1 : UINT32_MAX;
}

inline RGResourceHandle MakeHandle( uint32_t index, bool isExternal ) noexcept {
    if ( index > RG_MAX_RESOURCE_INDEX ) return RG_INVALID_HANDLE;
    return (index << 1) | (isExternal ? 1u : 0u);
}

class RGBuilder {
public:
    RGBuilder( class RenderGraph& graph, class RenderPass& pass )
        : m_graph( graph ), m_pass( pass ) {
    }

    // Declare that this pass READS from a resource (Source)
    RGResourceHandle Read( RGResourceHandle handle );

    // Declare that this pass WRITES to a resource (Sink)
    RGResourceHandle Write( RGResourceHandle handle );

    // Declare a brand new transient resource that lives only for this graph execution
    RGResourceHandle CreateTexture( const RGTextureDesc& desc );

private:
    RenderGraph& m_graph;
    RenderPass& m_pass;
};

class RenderGraph {
public:
    RenderGraph( TexturePool* pool ) : m_texturePool( pool ) {}

    // Bring an existing engine resource (like the DX11 BackBuffer) into the graph
    RGResourceHandle ImportResource( const std::wstring& name, RenderToTextureBuffer* externalBuffer );

    // Add a pass using modern C++ lambdas
    template<typename SetupFunc>
        requires std::invocable<SetupFunc, RGBuilder&, RenderPass&>
    void AddPass( RGPassName name, SetupFunc setupFunc );

    // Called by RGBuilder to register handles
    RGResourceHandle RegisterResource( const RGTextureDesc& desc );

    bool Compile();

    bool Execute();

    void Invalidate() noexcept { m_valid = false; m_compiled = false; }

    bool IsHandleRegistered( RGResourceHandle handle ) const {
        const uint32_t index = GetHandleIndex( handle );
        if ( index >= m_nextHandle || index >= m_externalTextures.size() ) return false;
        return IsExternalHandle( handle ) == (m_externalTextures[index] != nullptr);
    }

    RenderToTextureBuffer* GetPhysicalTexture( RGResourceHandle handle ) const {
        if ( !IsHandleRegistered( handle ) ) return nullptr;
        const uint32_t index = GetHandleIndex( handle );
        return IsExternalHandle( handle )
            ? m_externalTextures[index]
            : m_activeTextures[index].get();
    }
private:
    struct Lifetime { uint32_t firstPass; uint32_t firstReadPass; uint32_t lastPass; bool isRead; };

    TexturePool* m_texturePool;
    uint32_t m_nextHandle = 0;
    bool m_compiled = false;
    bool m_valid = true;
    std::vector<std::unique_ptr<RenderPass>> m_passes;
    std::vector<RGTextureDesc> m_resourceDescs;
    std::vector<Lifetime> m_resourceLifetimes;
    
    // Physical resource storage mapped by the Handle Index
    std::vector<TextureHandle> m_activeTextures;
    std::vector<RenderToTextureBuffer*> m_externalTextures;
    
    bool AllocateResourcesForPass( size_t passIndex );

    void ReleaseResourcesForPass( size_t passIndex ) {
        for ( size_t i = 0; i < m_resourceLifetimes.size(); ++i ) {
            if ( m_resourceLifetimes[i].lastPass == static_cast<uint32_t>(passIndex)
                && m_externalTextures[i] == nullptr ) {
                m_activeTextures[i].reset();
            }
        }
    }

    void ReleaseAllResources() {
        for ( auto& texture : m_activeTextures ) texture.reset();
    }
};

template<typename SetupFunc>
    requires std::invocable<SetupFunc, RGBuilder&, RenderPass&>
inline void RenderGraph::AddPass( RGPassName name, SetupFunc setupFunc ) {
    const uint32_t firstResource = m_nextHandle;
    try {
        if ( !name.wide ) name.wide = L"Unnamed Render Pass";
        if ( !name.narrow ) name.narrow = "Unnamed Render Pass";

        auto pass = std::make_unique<RenderPass>( name );
        RGBuilder builder( *this, *pass );
        setupFunc( builder, *pass );
        m_passes.push_back( std::move( pass ) );
        m_compiled = false;
    } catch ( const std::exception& error ) {
        m_externalTextures.resize( firstResource );
        m_activeTextures.resize( firstResource );
        m_resourceDescs.resize( firstResource );
        m_nextHandle = firstResource;
        Invalidate();
        LogError() << "RenderGraph pass setup failed: " << error.what();
    } catch ( ... ) {
        m_externalTextures.resize( firstResource );
        m_activeTextures.resize( firstResource );
        m_resourceDescs.resize( firstResource );
        m_nextHandle = firstResource;
        Invalidate();
        LogError() << "RenderGraph pass setup failed unexpectedly.";
    }
}
