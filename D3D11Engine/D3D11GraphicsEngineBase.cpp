#include "D3D11GraphicsEngineBase.h"

#include "D3D11LineRenderer.h"
#include "D3D11PipelineStates.h"
#include "D3D11PointLight.h"
#include "D3D11PShader.h"
#include "D3D11ShaderManager.h"
#include "D3D11VShader.h"
#include "RenderToTextureBuffer.h"
#include "zCView.h"

// If defined, creates a debug-version of the d3d11-device
#if !PUBLIC_RELEASE
#define DEBUG_D3D11
#endif

D3D11GraphicsEngineBase::D3D11GraphicsEngineBase() {
    OutputWindow = HWND( 0 );
    PresentPending = false;

    // Match the resolution with the current desktop resolution
    Resolution = Engine::GAPI->GetRendererState().RendererSettings.LoadedResolution;
}

D3D11GraphicsEngineBase::~D3D11GraphicsEngineBase() {
    GothicDepthBufferStateInfo::DeleteCachedObjects();
    GothicBlendStateInfo::DeleteCachedObjects();
    GothicRasterizerStateInfo::DeleteCachedObjects();
}

/** Called when the game created its window */
XRESULT D3D11GraphicsEngineBase::SetWindow( HWND hWnd ) {
    if ( !hWnd || !IsWindow( hWnd ) || Resolution.x <= 0 || Resolution.y <= 0 ) {
        return XR_INVALID_ARG;
    }
    LogInfo() << "Creating swapchain";
    const HWND previousWindow = OutputWindow;
    OutputWindow = hWnd;
    const XRESULT result = OnResize( Resolution );
    if ( result != XR_SUCCESS ) OutputWindow = previousWindow;
    return result;
}

/** Called to set the current viewport */
XRESULT D3D11GraphicsEngineBase::SetViewport( const ViewportInfo& viewportInfo ) {
    if ( !GetContext() || viewportInfo.Width <= 0 || viewportInfo.Height <= 0
        || !std::isfinite( viewportInfo.MinZ ) || !std::isfinite( viewportInfo.MaxZ )
        || viewportInfo.MinZ < 0.0f || viewportInfo.MaxZ > 1.0f
        || viewportInfo.MinZ > viewportInfo.MaxZ ) return XR_INVALID_ARG;
    // Set the viewport
    D3D11_VIEWPORT viewport = {};

    viewport.TopLeftX = static_cast<float>(viewportInfo.TopLeftX);
    viewport.TopLeftY = static_cast<float>(viewportInfo.TopLeftY);
    viewport.Width = static_cast<float>(viewportInfo.Width);
    viewport.Height = static_cast<float>(viewportInfo.Height);
    viewport.MinDepth = viewportInfo.MinZ;
    viewport.MaxDepth = viewportInfo.MaxZ;

    GetContext()->RSSetViewports( 1, &viewport );

    return XR_SUCCESS;
}

/** Returns the shadermanager */
D3D11ShaderManager& D3D11GraphicsEngineBase::GetShaderManager() { return *ShaderManager; }

/** Creates a vertexbuffer object (Not registered inside) */
XRESULT D3D11GraphicsEngineBase::CreateVertexBuffer( D3D11VertexBuffer** outBuffer ) {
    if ( !outBuffer ) return XR_INVALID_ARG;
    *outBuffer = new (std::nothrow) D3D11VertexBuffer;
    return *outBuffer ? XR_SUCCESS : XR_FAILED;
}

/** Creates a texture object (Not registered inside) */
XRESULT D3D11GraphicsEngineBase::CreateTexture( D3D11Texture** outTexture ) {
    if ( !outTexture ) return XR_INVALID_ARG;
    *outTexture = new (std::nothrow) D3D11Texture;
    return *outTexture ? XR_SUCCESS : XR_FAILED;
}

/** Creates a constantbuffer object (Not registered inside) */
XRESULT D3D11GraphicsEngineBase::CreateConstantBuffer( D3D11ConstantBuffer** outCB, void* data, int size ) {
    if ( !outCB || size <= 0 ) return XR_INVALID_ARG;
    *outCB = nullptr;
    std::unique_ptr<D3D11ConstantBuffer> buffer(
        new (std::nothrow) D3D11ConstantBuffer( static_cast<unsigned int>(size), data ) );
    if ( !buffer || !buffer->IsValid() ) return XR_FAILED;
    *outCB = buffer.release();
    return XR_SUCCESS;
}

/** Presents the current frame to the screen */
XRESULT D3D11GraphicsEngineBase::Present() {
    if ( !Engine::GAPI || !GetContext() || !GetDevice() || !SwapChain || !LineRenderer
        || Resolution.x <= 0 || Resolution.y <= 0 ) {
        PresentPending = false;
        return XR_FAILED;
    }
    if ( SetViewport( ViewportInfo( 0, 0, Resolution.x, Resolution.y ) ) != XR_SUCCESS ) {
        PresentPending = false;
        return XR_FAILED;
    }
    SetDefaultStates();
    LineRenderer->Flush();
    const bool vsync = Engine::GAPI->GetRendererState().RendererSettings.EnableVSync;
    const HRESULT result = SwapChain->Present( vsync ? 1u : 0u, 0 );
    PresentPending = false;
    if ( result == DXGI_ERROR_DEVICE_REMOVED || result == DXGI_ERROR_DEVICE_RESET ) {
        LogErrorBox() << "Direct3D device lost. Reason: 0x" << std::hex
            << static_cast<unsigned long>(GetDevice()->GetDeviceRemovedReason());
        return XR_FAILED;
    }
    if ( FAILED( result ) ) {
        LogError() << "Swapchain presentation failed. HRESULT: 0x" << std::hex
            << static_cast<unsigned long>(result);
        return XR_FAILED;
    }
    return XR_SUCCESS;
}
/** Called when we started to render the world */
XRESULT D3D11GraphicsEngineBase::OnStartWorldRendering() {
    if ( PresentPending )
        return XR_FAILED;

    PresentPending = true;

    // Update transforms
    UpdateTransformsCB();

    // Force farplane
    Engine::GAPI->SetFarPlane( Engine::GAPI->GetRendererState().RendererSettings.SectionDrawRadius * WORLD_SECTION_SIZE );

    return XR_SUCCESS;
}

/** Returns the line renderer object */
BaseLineRenderer* D3D11GraphicsEngineBase::GetLineRenderer() {
    return LineRenderer.get();
}

/** Sets up the default rendering state */
void D3D11GraphicsEngineBase::SetDefaultStates() {
    Engine::GAPI->GetRendererState().RasterizerState.SetDefault();
    Engine::GAPI->GetRendererState().BlendState.SetDefault();
    Engine::GAPI->GetRendererState().DepthState.SetDefault();
    Engine::GAPI->GetRendererState().SamplerState.SetDefault();

    Engine::GAPI->GetRendererState().RasterizerState.SetDirty();
    Engine::GAPI->GetRendererState().BlendState.SetDirty();
    Engine::GAPI->GetRendererState().DepthState.SetDirty();
    Engine::GAPI->GetRendererState().SamplerState.SetDirty();

    GetContext()->PSSetSamplers( 0, 1, DefaultSamplerState.GetAddressOf() );

    UpdateRenderStates();
}

/** Draws a vertexarray, used for rendering gothics UI */
XRESULT D3D11GraphicsEngineBase::DrawVertexArray( ExVertexStruct* vertices, unsigned int numVertices, unsigned int startVertex, unsigned int stride ) {
    UpdateRenderStates();
    auto vShader = ShaderManager->GetVShader( VShaderID::VS_TransformedEx );
    auto pShader = ShaderManager->GetPShader( PShaderID::PS_FixedFunctionPipe );

    // Bind the FF-Info to the first PS slot
    pShader->GetBuffer( "FFPipelineConstantBuffer" ).Update( &Engine::GAPI->GetRendererState().GraphicsState ).Bind();

    vShader->Apply();
    pShader->Apply();

    // Set vertex type
    GetContext()->IASetPrimitiveTopology( D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST );

    // Bind the viewport information to the shader
    D3D11_VIEWPORT vp;
    UINT num = 1;
    GetContext()->RSGetViewports( &num, &vp );

    // Update viewport information
    const float scale = Engine::GAPI->GetRendererState().RendererSettings.GothicUIScale;
    float2 temp2Float2[2];
    temp2Float2[0].x = vp.TopLeftX / scale;
    temp2Float2[0].y = vp.TopLeftY / scale;
    temp2Float2[1].x = vp.Width / scale;
    temp2Float2[1].y = vp.Height / scale;

    vShader->GetBuffer( "Viewport" ).Update( temp2Float2 ).Bind();

    D3D11_BUFFER_DESC desc;
    TempVertexBuffer->GetVertexBuffer().Get()->GetDesc( &desc );

    // Check if we need a bigger vertexbuffer
    if ( desc.ByteWidth < stride * numVertices ) {
        if ( Engine::GAPI->GetRendererState().RendererSettings.EnableDebugLog )
            LogInfo() << "TempVertexBuffer too small (" << desc.ByteWidth << "), need " << stride * numVertices << " bytes. Recreating buffer.";

        // Buffer too small, recreate it
        TempVertexBuffer = std::make_unique<D3D11VertexBuffer>();

        TempVertexBuffer->Init( nullptr, stride * numVertices, D3D11VertexBuffer::B_VERTEXBUFFER, D3D11VertexBuffer::U_DYNAMIC, D3D11VertexBuffer::CA_WRITE );
    }

    // Send vertexdata to the GPU
    TempVertexBuffer->UpdateBuffer( vertices, stride * numVertices );

    UINT offset = 0;
    UINT uStride = stride;
    GetContext()->IASetVertexBuffers( 0, 1, TempVertexBuffer->GetVertexBuffer().GetAddressOf(), &uStride, &offset );

    //Draw the mesh
    GetContext()->Draw( numVertices, startVertex );

    return XR_SUCCESS;
}

/** Sets the active pixel shader object */
XRESULT D3D11GraphicsEngineBase::SetActivePixelShader( PShaderID shader ) {
    ActivePS = ShaderManager ? ShaderManager->GetPShader( shader ) : nullptr;
    if ( !ActivePS || !ActivePS->GetShader() ) {
        ActivePS.reset();
        return XR_FAILED;
    }
    return XR_SUCCESS;
}

XRESULT D3D11GraphicsEngineBase::SetActiveVertexShader( VShaderID shader ) {
    ActiveVS = ShaderManager ? ShaderManager->GetVShader( shader ) : nullptr;
    if ( !ActiveVS || !ActiveVS->GetShader() ) {
        ActiveVS.reset();
        return XR_FAILED;
    }
    return XR_SUCCESS;
}

XRESULT D3D11GraphicsEngineBase::SetActiveGShader( GShaderID shader ) {
    ActiveGS = ShaderManager ? ShaderManager->GetGShader( shader ) : nullptr;
    if ( !ActiveGS || !ActiveGS->GetShader() ) {
        ActiveGS.reset();
        return XR_FAILED;
    }
    return XR_SUCCESS;
}

//int D3D11GraphicsEngineBase::MeasureString(std::string str, zFont* zFont)
//{
//	return 0;
//}

/** Updates the transformsCB with new values from the GAPI */
void D3D11GraphicsEngineBase::UpdateTransformsCB() {
    const XMFLOAT4X4& view = Engine::GAPI->GetRendererState().TransformState.TransformView;
    const XMFLOAT4X4& proj = Engine::GAPI->GetProjectionMatrix();

    VS_ExConstantBuffer_PerFrame cb = {};
    cb.View = view;
    cb.Projection = proj;
    XMStoreFloat4x4( &cb.ViewProj, XMMatrixMultiply( XMLoadFloat4x4( &proj ), XMLoadFloat4x4( &view ) ) );

    TransformsCB->UpdateBuffer( &cb );
}

/** Creates a bufferobject for a shadowed point light */
XRESULT D3D11GraphicsEngineBase::CreateShadowedPointLight( BaseShadowedPointLight** outPL, VobLightInfo* lightInfo, bool dynamic ) {
    if ( Engine::GAPI->GetRendererState().RendererSettings.EnablePointlightShadows > 0 )
        *outPL = new D3D11PointLight( lightInfo, dynamic );
    else
        *outPL = nullptr;

    return XR_SUCCESS;
}

/** Draws a vertexbuffer, non-indexed, binding the FF-Pipe values */
XRESULT D3D11GraphicsEngineBase::DrawVertexBufferFF( D3D11VertexBuffer* vb, unsigned int numVertices, unsigned int startVertex, unsigned int stride ) {
    SetupVS_ExMeshDrawCall();

    // Bind the FF-Info to the first PS slot
    ActivePS->GetBuffer( "FFPipelineConstantBuffer" ).Update( &Engine::GAPI->GetRendererState().GraphicsState ).Bind();

    UINT offset = 0;
    UINT uStride = stride;
    GetContext()->IASetVertexBuffers( 0, 1, vb->GetVertexBuffer().GetAddressOf(), &uStride, &offset );

    //Draw the mesh
    GetContext()->Draw( numVertices, startVertex );

    Engine::GAPI->GetRendererState().RendererInfo.FrameDrawnTriangles += numVertices;

    return XR_SUCCESS;
}

/** Binds viewport information to the given constantbuffer slot */
XRESULT D3D11GraphicsEngineBase::BindViewportInformation( VShaderID shader, int slot ) {
    D3D11_VIEWPORT vp;
    UINT num = 1;
    GetContext()->RSGetViewports( &num, &vp );

    // Update viewport information
    float scale = Engine::GAPI->GetRendererState().RendererSettings.GothicUIScale;
    float2 f2[2];
    f2[0].x = vp.TopLeftX / scale;
    f2[0].y = vp.TopLeftY / scale;
    f2[1].x = vp.Width / scale;
    f2[1].y = vp.Height / scale;

    auto vs = ShaderManager->GetVShader( shader );

    if ( vs ) {
        vs->GetBuffer( "Viewport" ).Update( f2 ).Bind();
    }

    return XR_SUCCESS;
}

/** Returns the graphics-device this is running on */
const std::string& D3D11GraphicsEngineBase::GetGraphicsDeviceName() {
    return DeviceDescription;
}
