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
        // Some plugins or patches override savegame behavior and cause crashing.
        // Chronicles of Myrtana save-path workaround.
        // Savegame hook disabled.
#endif
    }

    static void __fastcall hooked_Write_Savegame( void* thisptr, void* unknwn, int slot ) {
        original_CGameManagerWrite_Savegame( thisptr, slot );
    }
    
};
