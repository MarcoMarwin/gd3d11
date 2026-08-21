#include "pch.h"

#include <d3d9.h>

void zCObject_AddRef( void* o ) {
    DWORD object = reinterpret_cast<DWORD>( o );
    ++( *reinterpret_cast<DWORD*>( object + 0x04 ) );
}

void zCObject_Release( void* o ) {
    DWORD object = reinterpret_cast<DWORD>( o );
    if ( --( *reinterpret_cast<DWORD*>( object + 0x04 ) ) <= 0 )
        reinterpret_cast<void( __thiscall* )( DWORD, DWORD )>( *reinterpret_cast<DWORD*>( *reinterpret_cast<DWORD*>( object ) + 0x0C ) )( object, 1 );
}

void DebugWrite_i( LPCSTR lpDebugMessage, void* thisptr ) {
};

int ComputeFVFSize( DWORD fvf ) {
	// FVF flags determine the size of each vertex.
	int size = 0;
	DWORD test = fvf;
	// Check the FVF flags.

	if ( (fvf & D3DFVF_XYZ) == D3DFVF_XYZ ) {
		size += 3 * sizeof( float );
		test &= ~D3DFVF_XYZ;
	} else if ( (fvf & D3DFVF_XYZRHW) == D3DFVF_XYZRHW ) {
		size += 4 * sizeof( float );
		test &= ~D3DFVF_XYZRHW;
	}
	if ( (fvf & D3DFVF_NORMAL) == D3DFVF_NORMAL ) {
		size += 3 * sizeof( float );
		test &= ~D3DFVF_NORMAL;
	}
	if ( (fvf & D3DFVF_DIFFUSE) == D3DFVF_DIFFUSE ) {
		size += sizeof( D3DCOLOR );
		test &= ~D3DFVF_DIFFUSE;
	}
	if ( (fvf & D3DFVF_SPECULAR) == D3DFVF_SPECULAR ) {
		size += sizeof( D3DCOLOR );
		test &= ~D3DFVF_SPECULAR;
	}
	if ( (fvf & D3DFVF_TEX1) == D3DFVF_TEX1 ) {
		size += 2 * sizeof( float );
		test &= ~D3DFVF_TEX1;
	} else if ( (fvf & D3DFVF_TEX2) == D3DFVF_TEX2 ) {
		size += 2 * sizeof( float ) * 2;
		test &= ~D3DFVF_TEX2;
	} else if ( (fvf & D3DFVF_TEX3) == D3DFVF_TEX3 ) {
		size += 2 * sizeof( float ) * 3;
		test &= ~D3DFVF_TEX3;
	} else if ( (fvf & D3DFVF_TEX4) == D3DFVF_TEX4 ) {
		size += 2 * sizeof( float ) * 4;
		test &= ~D3DFVF_TEX4;
	} else if ( (fvf & D3DFVF_TEX5) == D3DFVF_TEX5 ) {
		size += 2 * sizeof( float ) * 5;
		test &= ~D3DFVF_TEX5;
	} else if ( (fvf & D3DFVF_TEX6) == D3DFVF_TEX6 ) {
		size += 2 * sizeof( float ) * 6;
		test &= ~D3DFVF_TEX6;
	} else if ( (fvf & D3DFVF_TEX7) == D3DFVF_TEX7 ) {
		size += 2 * sizeof( float ) * 7;
		test &= ~D3DFVF_TEX7;
	} else if ( (fvf & D3DFVF_TEX8) == D3DFVF_TEX8 ) {
		size += 2 * sizeof( float ) * 8;
		test &= ~D3DFVF_TEX8;
	}

	if ( test != 0 )
		LogWarn() << "FVF contains unknown bits! " << test << " leftover";

	//Here is the uncompleted code for the other fvfs...

	return size;
}
