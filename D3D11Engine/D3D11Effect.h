#pragma once
#include "pch.h"
#include "WorldConverter.h"
#include "GothicAPI.h"

/** Wrapper-class for some generic effects */

class D3D11VertexBuffer;
struct RenderToDepthStencilBuffer;
class D3D11Effect {
public:
    D3D11Effect();
    ~D3D11Effect();

    /** Draws GPU-Based rain */
    XRESULT DrawRain( bool outputResolution = false, bool useRainExclusionMask = false );
    XRESULT DrawRain_CS( bool outputResolution = false, bool useRainExclusionMask = false );

    XRESULT LoadRainResources();

    /** Renders the rain-shadowmap */
    XRESULT DrawRainShadowmap();

    /** Returns the current rain-shadowmap camera replacement */
    CameraReplacement& GetRainShadowmapCameraRepl() { return RainShadowmapCameraRepl; }

    /** Returns the rain shadowmap */
    RenderToDepthStencilBuffer* GetRainShadowmap() { return RainShadowmap.get(); }
protected:

    /** Fills vectors of random raindrop data, split into mutable and immutable parts */
    void FillRandomRaindropData( std::vector<RainParticleDynamic>& dynamicData, std::vector<RainParticleStatic>& staticData );
    XRESULT EnsureRainBuffers( UINT numParticles, bool useCompute );
    ID3D11BlendState* GetRainReactiveBlendState();

    /** Rain */
    D3D11VertexBuffer* RainBufferStatic;
    D3D11VertexBuffer* RainBufferInitial;
    D3D11VertexBuffer* RainBufferDrawFrom;
    D3D11VertexBuffer* RainBufferStreamTo;
    float RainBufferRadius = -1.0f;
    float RainBufferHeight = -1.0f;
    UINT RainBufferParticleCount = 0;
    bool RainBuffersUseCompute = false;
    bool RainStreamOutFirstFrame = true;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> RainTextureArray;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> RainTextureArraySRV;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> SnowTextureArray;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> SnowTextureArraySRV;
    std::unique_ptr<RenderToDepthStencilBuffer> RainShadowmap;
    CameraReplacement RainShadowmapCameraRepl;

    Microsoft::WRL::ComPtr<ID3D11SamplerState> m_RainDropShadowSamplerState;
    Microsoft::WRL::ComPtr<ID3D11BlendState> m_RainReactiveBlendState;
};

