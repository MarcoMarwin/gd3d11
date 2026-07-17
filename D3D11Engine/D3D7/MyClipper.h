#pragma once
#include "../pch.h"
#include <ddraw.h>
#include <atomic>

class MyClipper final : public IDirectDrawClipper {
public:
	MyClipper() : hWnd(nullptr), refCount(1) {
	}

	/*** IUnknown methods ***/
	HRESULT __declspec(nothrow) __stdcall QueryInterface( THIS_ REFIID riid, LPVOID FAR* ppvObj ) override {
		if ( !ppvObj ) return E_POINTER;
		*ppvObj = nullptr;
		if ( IsEqualIID( riid, IID_IUnknown )
			|| IsEqualIID( riid, IID_IDirectDrawClipper ) ) {
			*ppvObj = static_cast<IDirectDrawClipper*>(this);
			AddRef();
			return S_OK;
		}
		return E_NOINTERFACE;
	}

	ULONG __declspec(nothrow) __stdcall AddRef() override {
		return refCount.fetch_add( 1, std::memory_order_relaxed ) + 1;
	}

	ULONG __declspec(nothrow) __stdcall Release() override {
		const ULONG references =
			refCount.fetch_sub( 1, std::memory_order_acq_rel ) - 1;
		if ( references == 0 ) {
			delete this;
		}
		return references;
	}

	/*** IDirectDrawClipper methods ***/
	HRESULT __declspec(nothrow) __stdcall GetClipList( THIS_ LPRECT x, LPRGNDATA y, LPDWORD z ) override {
		(void)x;
		(void)y;
		if ( !z ) return E_POINTER;
		*z = 0;
		return DDERR_NOCLIPLIST;
	}

	HRESULT __declspec(nothrow) __stdcall GetHWnd( HWND* handle ) override {
		if ( !handle ) return E_POINTER;
		*handle = hWnd.load( std::memory_order_acquire );
		return S_OK;
	}

	HRESULT __declspec(nothrow) __stdcall Initialize( THIS_ LPDIRECTDRAW x, DWORD y ) override {
		return S_OK;
	}

	HRESULT __declspec(nothrow) __stdcall IsClipListChanged( THIS_ BOOL FAR* changed ) override {
		if ( !changed ) return E_POINTER;
		*changed = FALSE;
		return S_OK;
	}

	HRESULT __declspec(nothrow) __stdcall SetClipList( THIS_ LPRGNDATA x, DWORD y ) override {
		return S_OK;
	}

	HRESULT __declspec(nothrow) __stdcall SetHWnd( THIS_ DWORD x, HWND handle ) override {
		hWnd.store( handle, std::memory_order_release );
		return S_OK;
	}

private:
	std::atomic<HWND> hWnd;
	std::atomic<ULONG> refCount;
};
