#pragma once
#include "../pch.h"
#include <d3d.h>
#include "../Logger.h"
#include <vector>
#include <atomic>
#include <limits>
#include "../BaseGraphicsEngine.h"
#include "../D3D11VertexBuffer.h"
#include "../Engine.h"

class MyDirect3DVertexBuffer7 : public IDirect3DVertexBuffer7 {
public:
	MyDirect3DVertexBuffer7(
		const D3DVERTEXBUFFERDESC& originalDesc )
		: OriginalDesc( originalDesc ), VertexBuffer( nullptr ),
		RefCount( 1 ) {
		DebugWrite(
			"MyDirect3DVertexBuffer7::MyDirect3DVertexBuffer7\n" );

		const int computedVertexStride =
			ComputeFVFSize( OriginalDesc.dwFVF );
		if ( !Engine::GraphicsEngine || computedVertexStride <= 0
			|| OriginalDesc.dwNumVertices
				> (std::numeric_limits<unsigned int>::max)()
					/ static_cast<unsigned int>(computedVertexStride) ) {
			LogError() << "Invalid Direct3D 7 vertex-buffer description.";
			return;
		}

		const unsigned int vertexStride =
			static_cast<unsigned int>(computedVertexStride);

		if ( Engine::GraphicsEngine->CreateVertexBuffer(
				&VertexBuffer ) != XR_SUCCESS
			|| !VertexBuffer ) {
			VertexBuffer = nullptr;
			LogError() << "Failed to create Direct3D 7 vertex buffer.";
			return;
		}

		const unsigned int sizeInBytes =
			OriginalDesc.dwNumVertices * vertexStride;
		if ( sizeInBytes == 0
			|| VertexBuffer->Init(
				nullptr, sizeInBytes,
				D3D11VertexBuffer::B_VERTEXBUFFER,
				D3D11VertexBuffer::U_DYNAMIC,
				D3D11VertexBuffer::CA_WRITE )
				!= XR_SUCCESS
			|| !VertexBuffer->IsValid() ) {
			delete VertexBuffer;
			VertexBuffer = nullptr;
			LogError() << "Failed to initialize Direct3D 7 vertex buffer.";
		}
	}

    virtual ~MyDirect3DVertexBuffer7() {
        delete VertexBuffer;
    }

	/*** IUnknown methods ***/
	HRESULT __declspec(nothrow) STDMETHODCALLTYPE QueryInterface(
		REFIID riid, void** ppvObj ) override {
		DebugWrite( "MyDirect3DVertexBuffer7::QueryInterface\n" );
		if ( !ppvObj ) return E_POINTER;
		*ppvObj = nullptr;

		if ( IsEqualIID( riid, IID_IUnknown )
			|| IsEqualIID( riid, IID_IDirect3DVertexBuffer7 ) ) {
			*ppvObj = static_cast<IDirect3DVertexBuffer7*>(this);
			AddRef();
			return S_OK;
		}
		return E_NOINTERFACE;
	}

	ULONG __declspec(nothrow) STDMETHODCALLTYPE AddRef() override {
		DebugWrite( "MyDirect3DVertexBuffer7::AddRef\n" );
		return RefCount.fetch_add(
			1, std::memory_order_relaxed ) + 1;
	}

	ULONG __declspec(nothrow) STDMETHODCALLTYPE Release() override {
		DebugWrite( "MyDirect3DVertexBuffer7::Release\n" );
		const ULONG references = RefCount.fetch_sub(
			1, std::memory_order_acq_rel ) - 1;
		if ( references == 0 ) {
			delete this;
		}
		return references;
	}

	/*** IDirect3DVertexBuffer7 methods ***/
	HRESULT __declspec(nothrow) STDMETHODCALLTYPE GetVertexBufferDesc(
		LPD3DVERTEXBUFFERDESC lpVBDesc ) override {
		DebugWrite(
			"MyDirect3DVertexBuffer7::GetVertexBufferDesc\n" );
		if ( !lpVBDesc ) return E_POINTER;
		*lpVBDesc = OriginalDesc;
		return S_OK;
	}

	HRESULT __declspec(nothrow) STDMETHODCALLTYPE Lock(
		DWORD dwFlags, LPVOID* lplpData,
		LPDWORD lpdwSize ) override {
		DebugWrite( "MyDirect3DVertexBuffer7::Lock\n" );
		(void)dwFlags;
		if ( !lplpData ) return E_POINTER;
		*lplpData = nullptr;
		if ( lpdwSize ) *lpdwSize = 0;
		if ( !VertexBuffer || !VertexBuffer->IsValid() ) {
			return E_FAIL;
		}

		UINT size = 0;
		if ( VertexBuffer->Map(
				D3D11VertexBuffer::M_WRITE_DISCARD,
				lplpData, &size ) != XR_SUCCESS
			|| !*lplpData ) {
			LogError() << "Failed to map Direct3D 7 vertex buffer.";
			return E_FAIL;
		}
		if ( lpdwSize ) *lpdwSize = size;
		return S_OK;
	}

	HRESULT __declspec(nothrow) STDMETHODCALLTYPE Optimize( LPDIRECT3DDEVICE7 lpD3DDevice, DWORD dwFlags ) override {
		// Not needed
		return S_OK;
	}

	HRESULT __declspec(nothrow) STDMETHODCALLTYPE ProcessVertices( DWORD dwVertexOp, DWORD dwDestIndex, DWORD dwCount, LPDIRECT3DVERTEXBUFFER7 lpSrcBuffer, DWORD dwSrcIndex, LPDIRECT3DDEVICE7 lpD3DDevice, DWORD dwFlags ) override {
		LogWarn() << "Unimplemented method: MyDirect3DVertexBuffer7::ProcessVertices";
		return E_NOTIMPL;
	}

	HRESULT __declspec(nothrow) STDMETHODCALLTYPE ProcessVerticesStrided( DWORD dwVertexOp, DWORD dwDestIndex, DWORD dwCount, LPD3DDRAWPRIMITIVESTRIDEDDATA lpVertexArray, DWORD dwSrcIndex, LPDIRECT3DDEVICE7 lpD3DDevice, DWORD dwFlags ) override {
		LogWarn() << "Unimplemented method: MyDirect3DVertexBuffer7::ProcessVerticesStrided";
		return E_NOTIMPL;
	}

	HRESULT __declspec(nothrow) STDMETHODCALLTYPE Unlock() override {
		DebugWrite( "MyDirect3DVertexBuffer7::Unlock\n" );
		if ( !VertexBuffer
			|| VertexBuffer->Unmap() != XR_SUCCESS ) {
			return E_FAIL;
		}
		return S_OK;
	}

	/** Returns the number of vertices inside this buffer */
	int GetNumVertices() const {
		return static_cast<int>((std::min<DWORD>)(
			OriginalDesc.dwNumVertices,
			static_cast<DWORD>(
				(std::numeric_limits<int>::max)()) ));
	}

	bool IsValid() const {
		return VertexBuffer && VertexBuffer->IsValid();
	}

	/** Returns the actual vertex buffer */
	D3D11VertexBuffer* GetVertexBuffer() {
		return VertexBuffer;
	}

private:

	/** Original desc D3D7 created the buffer with */
	D3DVERTEXBUFFERDESC OriginalDesc;

	/** Our own vertex buffer */
	D3D11VertexBuffer* VertexBuffer;

	/** Referencecount on this */
	std::atomic<ULONG> RefCount;
};
