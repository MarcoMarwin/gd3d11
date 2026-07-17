#include "VersionCheck.h"

#include "Logger.h"

namespace VersionCheck {

    bool CheckExecutable() {
        if ( !Toolbox::FileExists( "GD3D11\\Shaders\\VS_Ex.hlsl" ) ) {
            LogErrorBox() << "Failed to find GD3D11 system files.\n"
                "The GD3D11 folder is missing or incomplete. Installing only ddraw.dll is not sufficient.\n\n"
                "Please repair the renderer installation.";
            return false;
        }
        return true;
    }

}