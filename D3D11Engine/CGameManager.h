#pragma once
#include "pch.h"
#include "HookedFunctions.h"
#include "Engine.h"
#include "GothicAPI.h"

extern bool CreatingThumbnail;

typedef void( __thiscall * CGameManagerWrite_Savegame)(void*, int);
CGameManagerWrite_Savegame original_CGameManagerWrite_Savegame;

class CGameManager {
public:

    static void Hook() {
        
#if BUILD_GOTHIC_2_6_fix
        original_CGameManagerWrite_Savegame = reinterpret_cast<CGameManagerWrite_Savegame>(0x0042a2d0);
        DetourAttachTyped( &HookedFunctions::OriginalFunctions.original_CGameManagerExitGame, hooked_ExitGame );
#endif
    }

#if BUILD_GOTHIC_2_6_fix
    static int __fastcall hooked_ExitGame( void* thisptr, void* ) {
        Engine::OnShutDown();
        return HookedFunctions::OriginalFunctions.original_CGameManagerExitGame( thisptr );
    }
#endif

    static void __fastcall hooked_Write_Savegame( void* thisptr, void* unknwn, int slot ) {
        original_CGameManagerWrite_Savegame( thisptr, slot );
    }
    
};
