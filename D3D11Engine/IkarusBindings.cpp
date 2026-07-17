#include "IkarusBindings.h"
#include "Engine.h"
#include "BaseGraphicsEngine.h"
#include "BaseLineRenderer.h"
#include "GothicAPI.h"

#include "D3D11GraphicsEngine.h" // TODO: Needed for the UI view. This should not be here.

#include "zSTRING.h"
#include "zCParser.h"

#include <algorithm>
#include <cmath>

namespace {
    constexpr float MAX_SCRIPT_MAGNITUDE = 100000.0f;

    GothicRendererSettings* TryGetRendererSettings() noexcept {
        return Engine::GAPI
            ? &Engine::GAPI->GetRendererState().RendererSettings
            : nullptr;
    }

    float ClampScriptMagnitude( float value ) noexcept {
        return (std::max)(-MAX_SCRIPT_MAGNITUDE, (std::min)(value, MAX_SCRIPT_MAGNITUDE));
    }

    bool IsFinitePosition( const float3& value ) noexcept {
        return std::isfinite( value.x ) && std::isfinite( value.y ) && std::isfinite( value.z );
    }
}

extern "C"
{
    __declspec(dllexport) void __cdecl GDX_AddPointLocator( float3* position, float size ) {
        if ( !position || !IsFinitePosition( *position ) || !std::isfinite( size ) || size <= 0.0f
            || !Engine::GraphicsEngine || !Engine::GraphicsEngine->GetLineRenderer() ) {
            return;
        }
        Engine::GraphicsEngine->GetLineRenderer()->AddPointLocator(
            *position->toXMFLOAT3(), (std::min)(size, MAX_SCRIPT_MAGNITUDE), XMFLOAT4( 1, 0, 0, 1 ) );
    }

    __declspec(dllexport) void __cdecl GDX_SetFogColor( DWORD color ) {
        if ( auto* settings = TryGetRendererSettings() ) {
            settings->FogColorMod = float3( color );
        }
    }

    __declspec(dllexport) void __cdecl GDX_SetFogDensity( float density ) {
        if ( auto* settings = TryGetRendererSettings(); settings && std::isfinite( density ) ) {
            settings->FogGlobalDensity = ClampScriptMagnitude( density );
        }
    }

    __declspec(dllexport) void __cdecl GDX_SetFogHeight( float height ) {
        if ( auto* settings = TryGetRendererSettings(); settings && std::isfinite( height ) ) {
            settings->FogHeight = ClampScriptMagnitude( height );
        }
    }

    __declspec(dllexport) void __cdecl GDX_SetFogHeightFalloff( float falloff ) {
        if ( auto* settings = TryGetRendererSettings(); settings && std::isfinite( falloff ) ) {
            settings->FogHeightFalloff = ClampScriptMagnitude( falloff );
        }
    }

    __declspec(dllexport) void __cdecl GDX_SetSunColor( DWORD color ) {
        if ( auto* settings = TryGetRendererSettings() ) {
            settings->SunLightColor = float3( color );
        }
    }

    __declspec(dllexport) void __cdecl GDX_SetSunStrength( float strength ) {
        if ( auto* settings = TryGetRendererSettings(); settings && std::isfinite( strength ) ) {
            settings->SunLightStrength = ClampScriptMagnitude( strength );
        }
    }

    __declspec(dllexport) void __cdecl GDX_SetShadowStrength( float strength ) {
        if ( auto* settings = TryGetRendererSettings(); settings && std::isfinite( strength ) ) {
            settings->ShadowStrength = ClampScriptMagnitude( strength );
        }
    }

    __declspec(dllexport) void __cdecl GDX_SetShadowAOStrength( float strength ) {
        if ( auto* settings = TryGetRendererSettings(); settings && std::isfinite( strength ) ) {
            settings->ShadowAOStrength = ClampScriptMagnitude( strength );
        }
    }

    __declspec(dllexport) void __cdecl GDX_SetWorldAOStrength( float strength ) {
        if ( auto* settings = TryGetRendererSettings(); settings && std::isfinite( strength ) ) {
            settings->WorldAOStrength = ClampScriptMagnitude( strength );
        }
    }

    __declspec(dllexport) void __cdecl GDX_OpenMessageBox(
        zSTRING* message, zSTRING* caption, int type, int callbackID ) {
        (void)message;
        (void)caption;
#ifndef BUILD_SPACER
        int action = type == 1 ? 1 : 0;
        if ( zCParser* parser = zCParser::GetParser(); parser && callbackID >= 0 ) {
            parser->CallFunc( callbackID, action );
        }
#else
        (void)type;
        (void)callbackID;
#endif
    }

    __declspec(dllexport) void __cdecl GDX_SetBinkVideoRunning( bool running ) {
        if ( auto* settings = TryGetRendererSettings() ) {
            settings->BinkVideoRunning = running;
        }
    }

    __declspec(dllexport) void __cdecl GDX_SetAtmosphericScattering( bool scattering ) {
        if ( auto* settings = TryGetRendererSettings() ) {
            settings->AtmosphericScattering = scattering;
        }
    }

    __declspec(dllexport) void __cdecl GDX_SetFogRange( int range ) {
        if ( auto* settings = TryGetRendererSettings() ) {
            settings->FogRange = (std::max)(0, (std::min)(range, 1000000));
        }
    }

    __declspec(dllexport) void __cdecl GDX_SetGlobalWindStrength( float strength ) {
        if ( auto* settings = TryGetRendererSettings(); settings && std::isfinite( strength ) ) {
            settings->GlobalWindStrength = ClampScriptMagnitude( strength );
        }
    }

    __declspec(dllexport) void __cdecl GDX_SetRainRadiusRange( float range ) {
        if ( auto* settings = TryGetRendererSettings(); settings && std::isfinite( range ) && range > 0.0f ) {
            settings->RainRadiusRange = (std::min)(range, MAX_SCRIPT_MAGNITUDE);
        }
    }

    __declspec(dllexport) void __cdecl GDX_SetRainHeightRange( float range ) {
        if ( auto* settings = TryGetRendererSettings(); settings && std::isfinite( range ) && range > 0.0f ) {
            settings->RainHeightRange = (std::min)(range, MAX_SCRIPT_MAGNITUDE);
        }
    }

    __declspec(dllexport) void __cdecl GDX_SetRainSceneWettness( float wettness ) {
        if ( auto* settings = TryGetRendererSettings(); settings && std::isfinite( wettness ) ) {
            settings->RainSceneWettness = ClampScriptMagnitude( wettness );
        }
    }

    __declspec(dllexport) void __cdecl GDX_SetRainFogDensity( float density ) {
        if ( auto* settings = TryGetRendererSettings(); settings && std::isfinite( density ) ) {
            settings->RainFogDensity = ClampScriptMagnitude( density );
        }
    }

    __declspec(dllexport) void __cdecl GDX_SetRainSunLightStrength( float strength ) {
        if ( auto* settings = TryGetRendererSettings(); settings && std::isfinite( strength ) ) {
            settings->RainSunLightStrength = ClampScriptMagnitude( strength );
        }
    }

    __declspec(dllexport) void __cdecl GDX_SetRainNumParticles( UINT particles ) {
        if ( auto* settings = TryGetRendererSettings() ) {
            settings->RainNumParticles = SanitizeRainParticleCount( particles );
        }
    }

    __declspec(dllexport) void __cdecl GDX_SetRainGlobalVelocity(
        float velocityX, float velocityY, float velocityZ ) {
        if ( auto* settings = TryGetRendererSettings();
            settings && std::isfinite( velocityX ) && std::isfinite( velocityY )
            && std::isfinite( velocityZ ) ) {
            settings->RainGlobalVelocity = XMFLOAT3(
                ClampScriptMagnitude( velocityX ),
                ClampScriptMagnitude( velocityY ),
                ClampScriptMagnitude( velocityZ ) );
        }
    }

    __declspec(dllexport) void __cdecl GDX_SetRainFogColor( DWORD color ) {
        if ( auto* settings = TryGetRendererSettings() ) {
            const float3 converted = float3( color );
            settings->RainFogColor = XMFLOAT3( converted.x, converted.y, converted.z );
        }
    }

    __declspec(dllexport) void __cdecl GDX_SetRainMoveParticles( bool particles ) {
        if ( auto* settings = TryGetRendererSettings() ) {
            settings->RainMoveParticles = particles;
        }
    }

    __declspec(dllexport) void __cdecl GDX_SetRainUseInitialSet( bool initial ) {
        if ( auto* settings = TryGetRendererSettings() ) {
            settings->RainUseInitialSet = initial;
        }
    }

    __declspec(dllexport) ID3D11DeviceContext1* __cdecl GDX_GetDX11RenderingContext( void ) {
        D3D11GraphicsEngine* graphics =
            reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);
        return graphics ? graphics->GetContext().Get() : nullptr;
    }

    __declspec(dllexport) const char* __cdecl GDX_GetVersionString( void ) {
        return VERSION_NUMBER;
    }

    __declspec(dllexport) GothicRendererSettings* __cdecl GDX_GetRendererSettings(
        unsigned int& structSize ) {
        if ( auto* settings = TryGetRendererSettings() ) {
            structSize = sizeof( *settings );
            return settings;
        }
        structSize = 0;
        return nullptr;
    }

    __declspec(dllexport) void __cdecl GDX_SaveRendererSettings() {
        if ( Engine::GAPI ) {
            Engine::GAPI->SaveRendererGlobalSettings(
                Engine::GAPI->GetRendererState().RendererSettings, MENU_SETTINGS_FILE );
            Engine::GAPI->SaveMenuSettings( MENU_SETTINGS_FILE );
        }
    }
};