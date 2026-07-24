#include "ImGuiShim.h"
#include "GSky.h"
#include "D3D11PfxRenderer.h"
#include "D3D11PFX_FSR3.h"
#include <VersionHelpers.h>
#include <ShellScalingApi.h>
#include <windowsx.h>

#include "zCParser.h"
#include <map>
#include <vector>
#include <algorithm>
#include <array>
#include <chrono>
#include <numeric>
#include <codecvt>
#include <cstdio>
#include <cmath>

namespace ImGui {
    void TextUnformatted( const wchar_t* text ) {
        char dest[64];
        auto len = WideCharToMultiByte(CP_UTF8, 0, text, -1, dest, sizeof(dest), NULL, NULL);
        dest[std::min(static_cast<size_t>(len), sizeof(dest) - 1)] = '\0';
        ImGui::TextUnformatted( dest );
    }
}

#if defined(BUILD_GOTHIC_1_08k) && !defined(BUILD_1_12F)
extern bool haveWindAnimations;
#endif

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler( HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam );
extern float* ShadowMapLambda;
extern float* ShadowMapBias;

void SyncGraphicsPresetSelection( GothicRendererSettings& s );

enum class TX_QUALITY : uint16_t {
    VeryLow = 128,
    Low = 256,
    Medium = 512,
    High = 1024,
    VeryHigh = 2048,
    MAX = 16384,
};

namespace {
    bool GetSettingsUiGeometry( HWND window, float& clientWidth, float& clientHeight, float& uiScale, POINT* cursorPos = nullptr )
    {
        RECT clientRect = {};
        if ( !window || !GetClientRect( window, &clientRect ) ) {
            return false;
        }

        clientWidth = static_cast<float>( std::max<LONG>( 1, clientRect.right - clientRect.left ) );
        clientHeight = static_cast<float>( std::max<LONG>( 1, clientRect.bottom - clientRect.top ) );

        float contentOffsetX = 0.0f;
        float contentOffsetY = 0.0f;
        const INT2 backbuffer = Engine::GraphicsEngine->GetBackbufferResolution();
        const auto& settings = Engine::GAPI->GetRendererState().RendererSettings;
        if ( settings.StretchWindow && backbuffer.x > 0 && backbuffer.y > 0 ) {
            const float renderAspect = static_cast<float>(backbuffer.x) / static_cast<float>(backbuffer.y);
            const float clientAspect = clientWidth / clientHeight;
            if ( std::abs( renderAspect - clientAspect ) > 0.001f ) {
                if ( clientAspect > renderAspect ) {
                    const float fittedWidth = clientHeight * renderAspect;
                    contentOffsetX = (clientWidth - fittedWidth) * 0.5f;
                    clientWidth = fittedWidth;
                } else {
                    const float fittedHeight = clientWidth / renderAspect;
                    contentOffsetY = (clientHeight - fittedHeight) * 0.5f;
                    clientHeight = fittedHeight;
                }
            }
        }
        uiScale = std::max( 0.01f, std::min( clientWidth / 1920.0f, clientHeight / 1080.0f ) );

        if ( cursorPos ) {
            if ( !GetCursorPos( cursorPos ) || !ScreenToClient( window, cursorPos ) ) {
                return false;
            }
            cursorPos->x -= static_cast<LONG>(std::lround( contentOffsetX ));
            cursorPos->y -= static_cast<LONG>(std::lround( contentOffsetY ));
        }
        return true;
    }

    bool IsMouseActionMessage( UINT msg )
    {
        switch ( msg ) {
            case WM_LBUTTONDOWN:
            case WM_LBUTTONUP:
            case WM_LBUTTONDBLCLK:
            case WM_RBUTTONDOWN:
            case WM_RBUTTONUP:
            case WM_RBUTTONDBLCLK:
            case WM_MBUTTONDOWN:
            case WM_MBUTTONUP:
            case WM_MBUTTONDBLCLK:
            case WM_XBUTTONDOWN:
            case WM_XBUTTONUP:
            case WM_XBUTTONDBLCLK:
            case WM_MOUSEWHEEL:
            case WM_MOUSEHWHEEL:
                return true;
            default:
                return false;
        }
    }

    void ApplyGD3D11DarkStyle()
    {
        ImGuiStyle& style = ImGui::GetStyle();
        style.WindowRounding = 3.0f;
        style.ChildRounding = 2.0f;
        style.FrameRounding = 2.0f;
        style.PopupRounding = 3.0f;
        style.ScrollbarRounding = 3.0f;
        style.GrabRounding = 2.0f;
        style.FrameBorderSize = 0.0f;
        style.WindowBorderSize = 1.0f;
        style.PopupBorderSize = 1.0f;

        ImVec4* colors = style.Colors;
        colors[ImGuiCol_Text] = ImVec4( 0.92f, 0.93f, 0.94f, 1.00f );
        colors[ImGuiCol_TextDisabled] = ImVec4( 0.48f, 0.50f, 0.53f, 1.00f );
        colors[ImGuiCol_WindowBg] = ImVec4( 0.045f, 0.048f, 0.052f, 0.94f );
        colors[ImGuiCol_ChildBg] = ImVec4( 0.055f, 0.058f, 0.064f, 0.92f );
        colors[ImGuiCol_PopupBg] = ImVec4( 0.070f, 0.074f, 0.082f, 0.98f );
        colors[ImGuiCol_Border] = ImVec4( 0.25f, 0.26f, 0.28f, 0.70f );
        colors[ImGuiCol_BorderShadow] = ImVec4( 0.00f, 0.00f, 0.00f, 0.00f );
        colors[ImGuiCol_FrameBg] = ImVec4( 0.115f, 0.122f, 0.135f, 0.94f );
        colors[ImGuiCol_FrameBgHovered] = ImVec4( 0.165f, 0.174f, 0.192f, 1.00f );
        colors[ImGuiCol_FrameBgActive] = ImVec4( 0.205f, 0.216f, 0.238f, 1.00f );
        colors[ImGuiCol_TitleBg] = ImVec4( 0.080f, 0.086f, 0.096f, 1.00f );
        colors[ImGuiCol_TitleBgActive] = ImVec4( 0.115f, 0.124f, 0.138f, 1.00f );
        colors[ImGuiCol_TitleBgCollapsed] = ImVec4( 0.045f, 0.048f, 0.052f, 0.90f );
        colors[ImGuiCol_MenuBarBg] = ImVec4( 0.095f, 0.102f, 0.113f, 1.00f );
        colors[ImGuiCol_ScrollbarBg] = ImVec4( 0.045f, 0.048f, 0.052f, 0.80f );
        colors[ImGuiCol_ScrollbarGrab] = ImVec4( 0.28f, 0.30f, 0.33f, 1.00f );
        colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4( 0.36f, 0.38f, 0.42f, 1.00f );
        colors[ImGuiCol_ScrollbarGrabActive] = ImVec4( 0.48f, 0.50f, 0.55f, 1.00f );
        colors[ImGuiCol_CheckMark] = ImVec4( 0.62f, 0.72f, 0.84f, 1.00f );
        colors[ImGuiCol_SliderGrab] = ImVec4( 0.42f, 0.52f, 0.65f, 1.00f );
        colors[ImGuiCol_SliderGrabActive] = ImVec4( 0.62f, 0.72f, 0.84f, 1.00f );
        colors[ImGuiCol_Button] = ImVec4( 0.145f, 0.154f, 0.170f, 0.94f );
        colors[ImGuiCol_ButtonHovered] = ImVec4( 0.205f, 0.218f, 0.240f, 1.00f );
        colors[ImGuiCol_ButtonActive] = ImVec4( 0.245f, 0.260f, 0.286f, 1.00f );
        colors[ImGuiCol_Header] = ImVec4( 0.150f, 0.160f, 0.178f, 0.94f );
        colors[ImGuiCol_HeaderHovered] = ImVec4( 0.205f, 0.218f, 0.240f, 1.00f );
        colors[ImGuiCol_HeaderActive] = ImVec4( 0.245f, 0.260f, 0.286f, 1.00f );
        colors[ImGuiCol_Separator] = ImVec4( 0.25f, 0.26f, 0.28f, 0.75f );
        colors[ImGuiCol_SeparatorHovered] = ImVec4( 0.42f, 0.52f, 0.65f, 1.00f );
        colors[ImGuiCol_SeparatorActive] = ImVec4( 0.62f, 0.72f, 0.84f, 1.00f );
        colors[ImGuiCol_ResizeGrip] = ImVec4( 0.42f, 0.52f, 0.65f, 0.45f );
        colors[ImGuiCol_ResizeGripHovered] = ImVec4( 0.62f, 0.72f, 0.84f, 0.75f );
        colors[ImGuiCol_ResizeGripActive] = ImVec4( 0.62f, 0.72f, 0.84f, 1.00f );
        colors[ImGuiCol_Tab] = ImVec4( 0.115f, 0.124f, 0.138f, 1.00f );
        colors[ImGuiCol_TabHovered] = ImVec4( 0.205f, 0.218f, 0.240f, 1.00f );
        colors[ImGuiCol_TabActive] = ImVec4( 0.165f, 0.176f, 0.195f, 1.00f );
        colors[ImGuiCol_TabUnfocused] = ImVec4( 0.080f, 0.086f, 0.096f, 1.00f );
        colors[ImGuiCol_TabUnfocusedActive] = ImVec4( 0.125f, 0.134f, 0.150f, 1.00f );
        colors[ImGuiCol_TextSelectedBg] = ImVec4( 0.42f, 0.52f, 0.65f, 0.35f );
        colors[ImGuiCol_NavHighlight] = ImVec4( 0.62f, 0.72f, 0.84f, 0.70f );
        colors[ImGuiCol_NavWindowingHighlight] = ImVec4( 0.92f, 0.93f, 0.94f, 0.70f );
        colors[ImGuiCol_NavWindowingDimBg] = ImVec4( 0.00f, 0.00f, 0.00f, 0.45f );
        colors[ImGuiCol_ModalWindowDimBg] = ImVec4( 0.00f, 0.00f, 0.00f, 0.55f );
    }
    int FindNearestStepIndex( float value, const float* levels, int levelCount )
    {
        int bestIndex = 0;
        float bestDistance = fabsf( value - levels[0] );
        for ( int i = 1; i < levelCount; ++i ) {
            const float distance = fabsf( value - levels[i] );
            if ( distance < bestDistance ) {
                bestIndex = i;
                bestDistance = distance;
            }
        }
        return bestIndex;
    }

    bool SliderSteppedIndex(
        const char* label,
        int* index,
        int maximumIndex,
        bool drawTicks,
        int emphasizedTick = -1,
        const char* displayText = nullptr,
        bool mutedTicks = false )
    {
        *index = std::clamp( *index, 0, maximumIndex );

        // Let ImGui handle mouse, keyboard and gamepad interaction with an integer
        // slider. Integer indices make the grab jump to real steps while dragging.
        ImGui::PushStyleColor( ImGuiCol_FrameBg, ImVec4( 0, 0, 0, 0 ) );
        ImGui::PushStyleColor( ImGuiCol_FrameBgHovered, ImVec4( 0, 0, 0, 0 ) );
        ImGui::PushStyleColor( ImGuiCol_FrameBgActive, ImVec4( 0, 0, 0, 0 ) );
        ImGui::PushStyleColor( ImGuiCol_SliderGrab, ImVec4( 0, 0, 0, 0 ) );
        ImGui::PushStyleColor( ImGuiCol_SliderGrabActive, ImVec4( 0, 0, 0, 0 ) );
        const bool changed = ImGui::SliderInt(
            label, index, 0, maximumIndex, "", ImGuiSliderFlags_AlwaysClamp );
        ImGui::PopStyleColor( 5 );

        const ImVec2 itemMin = ImGui::GetItemRectMin();
        const ImVec2 itemMax = ImGui::GetItemRectMax();
        const float width = itemMax.x - itemMin.x;
        const float height = itemMax.y - itemMin.y;
        if ( width <= 0.0f || height <= 0.0f || maximumIndex <= 0 ) {
            return changed;
        }

        const ImGuiStyle& style = ImGui::GetStyle();
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        const bool active = ImGui::IsItemActive();
        const bool hovered = ImGui::IsItemHovered();
        ImVec4 frameColorValue = ImGui::GetStyleColorVec4(
            active ? ImGuiCol_FrameBgActive : (hovered ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg) );
        if ( mutedTicks && !active && !hovered ) {
            frameColorValue.x *= 0.68f;
            frameColorValue.y *= 0.68f;
            frameColorValue.z *= 0.68f;
        }
        drawList->AddRectFilled( itemMin, itemMax, ImGui::GetColorU32( frameColorValue ), style.FrameRounding );
        if ( style.FrameBorderSize > 0.0f ) {
            drawList->AddRect(
                itemMin, itemMax, ImGui::GetColorU32( ImGuiCol_Border ),
                style.FrameRounding, style.FrameBorderSize );
        }

        const float grabPadding = 2.0f;
        const float sliderSize = std::max( 0.0f, width - grabPadding * 2.0f );
        const float grabSize = std::min(
            sliderSize, std::max( style.GrabMinSize, sliderSize / static_cast<float>(maximumIndex + 1) ) );
        const float usableWidth = std::max( 0.0f, sliderSize - grabSize );
        const float firstX = itemMin.x + grabPadding + grabSize * 0.5f;
        const float centerY = (itemMin.y + itemMax.y) * 0.5f;

        if ( drawTicks ) {
            const ImU32 minorTick = ImGui::GetColorU32( ImGuiCol_TextDisabled, mutedTicks ? 0.28f : 0.65f );
            const ImU32 emphasizedShadow = ImGui::GetColorU32( ImVec4( 0.0f, 0.0f, 0.0f, mutedTicks ? 0.35f : 0.85f ) );
            const ImU32 emphasizedColor = mutedTicks
                ? ImGui::GetColorU32( ImGuiCol_TextDisabled, 0.38f )
                : ImGui::GetColorU32( ImGuiCol_CheckMark );
            for ( int tick = 0; tick <= maximumIndex; ++tick ) {
                const float x = firstX + usableWidth * (static_cast<float>(tick) / maximumIndex);
                if ( tick == emphasizedTick ) {
                    drawList->AddLine(
                        ImVec2( x + 1.0f, itemMin.y + 2.0f ),
                        ImVec2( x + 1.0f, itemMax.y - 2.0f ), emphasizedShadow, 3.0f );
                    drawList->AddLine(
                        ImVec2( x, itemMin.y + 2.0f ),
                        ImVec2( x, itemMax.y - 2.0f ), emphasizedColor, 2.0f );
                } else {
                    drawList->AddLine(
                        ImVec2( x, centerY - 3.0f ),
                        ImVec2( x, centerY + 3.0f ), minorTick, 1.0f );
                }
            }
        }

        // Draw the grab last so every tick remains behind it.
        const float grabCenterX = firstX + usableWidth * (static_cast<float>(*index) / maximumIndex);
        drawList->AddRectFilled(
            ImVec2( grabCenterX - grabSize * 0.5f, itemMin.y + grabPadding ),
            ImVec2( grabCenterX + grabSize * 0.5f, itemMax.y - grabPadding ),
            ImGui::GetColorU32( active ? ImGuiCol_SliderGrabActive : ImGuiCol_SliderGrab ),
            style.GrabRounding );

        if ( displayText && displayText[0] != '\0' ) {
            const ImVec2 textSize = ImGui::CalcTextSize( displayText );
            drawList->AddText(
                ImVec2( itemMin.x + (width - textSize.x) * 0.5f,
                    itemMin.y + (height - textSize.y) * 0.5f ),
                ImGui::GetColorU32( ImGuiCol_Text ), displayText );
        }

        return changed;
    }

    bool SliderNormalizedUiStrength( const char* label, float* value, bool mutedTicks = false )
    {
        const std::array<float, 11> levels = {
            0.0f, 0.2f, 0.4f, 0.6f, 0.8f, 1.0f,
            1.2f, 1.4f, 1.6f, 1.8f, 2.0f
        };
        int index = FindNearestStepIndex( *value, levels.data(), static_cast<int>(levels.size()) );
        *value = levels[index];
        if ( SliderSteppedIndex( label, &index, 10, true, 5, nullptr, mutedTicks ) ) {
            *value = levels[index];
            return true;
        }
        return false;
    }

    struct StrengthControlMemory {
        float RestoreValue = 0.0f;
        bool RestoreOnEnable = false;
    };

    std::map<std::string, StrengthControlMemory> StrengthControlMemories;

    void ResetStrengthControlMemories()
    {
        StrengthControlMemories.clear();
    }

    bool CoupledStrengthCheckbox(
        const char* checkboxLabel, const char* stateKey, bool* enabled,
        float* normalizedValue, float defaultValue )
    {
        if ( !ImGui::Checkbox( checkboxLabel, enabled ) ) {
            return false;
        }

        StrengthControlMemory& memory = StrengthControlMemories[stateKey];
        if ( !*enabled ) {
            if ( *normalizedValue > 0.0f ) {
                memory.RestoreValue = *normalizedValue;
                memory.RestoreOnEnable = true;
            }
            *normalizedValue = 0.0f;
        } else {
            *normalizedValue = memory.RestoreOnEnable ? memory.RestoreValue : defaultValue;
            StrengthControlMemories.erase( stateKey );
        }
        return true;
    }

    bool CoupledStrengthSlider(
        const char* sliderLabel, const char* stateKey, bool* enabled, float* normalizedValue )
    {
        if ( !SliderNormalizedUiStrength( sliderLabel, normalizedValue, !*enabled ) ) {
            return false;
        }

        if ( *normalizedValue <= 0.0f ) {
            *normalizedValue = 0.0f;
            *enabled = false;
            StrengthControlMemories.erase( stateKey );
        } else {
            *enabled = true;
            StrengthControlMemories.erase( stateKey );
        }
        return true;
    }


    int SnapRenderScalePercentNonFSR( int value )
    {
        const int clamped = std::clamp( value, 100, 200 );
        return 100 + ((clamped - 100 + 2) / 5) * 5;
    }

    bool SliderRenderScalePercentNonFSR( const char* label, int* value )
    {
        *value = SnapRenderScalePercentNonFSR( *value );
        int index = (*value - 100) / 5;
        char display[16];
        std::snprintf( display, sizeof(display), "%d%%", *value );
        if ( SliderSteppedIndex( label, &index, 20, false, 0, display ) ) {
            *value = 100 + index * 5;
            return true;
        }
        return false;
    }

    float SnapDisplayTuningStrength( float value )
    {
        const float clamped = std::clamp( value, 0.0f, 2.0f );
        const int step = std::clamp( static_cast<int>(clamped * 20.0f + 0.5f), 0, 40 );
        return static_cast<float>(step) * 0.05f;
    }

    bool SliderDisplayTuningStrength( const char* label, float* value )
    {
        *value = SnapDisplayTuningStrength( *value );
        int index = std::clamp( static_cast<int>(*value * 20.0f + 0.5f), 0, 40 );
        char display[16];
        std::snprintf( display, sizeof(display), "%.2f", *value );
        if ( SliderSteppedIndex( label, &index, 40, false, 20, display ) ) {
            *value = static_cast<float>(index) * 0.05f;
            return true;
        }
        return false;
    }
}

int GetDpi( HWND hWnd )
{
    bool v81 = IsWindows8Point1OrGreater();
    bool v10 = IsWindows10OrGreater();

    if ( v81 || v10 ) {

        typedef HRESULT( WINAPI* GetDpiForMonitor_t )(
        HMONITOR, MONITOR_DPI_TYPE, UINT*, UINT*);

        HMODULE hShcore = LoadLibraryW( L"Shcore.dll" );
        if ( hShcore ) {
            GetDpiForMonitor_t pGetDpiForMonitor = reinterpret_cast<GetDpiForMonitor_t>(GetProcAddress( hShcore, "GetDpiForMonitor" ));
            if ( pGetDpiForMonitor ) {
                HMONITOR hMonitor = ::MonitorFromWindow( hWnd, MONITOR_DEFAULTTONEAREST );
                UINT xdpi, ydpi;
                LRESULT success = pGetDpiForMonitor( hMonitor, MDT_EFFECTIVE_DPI, &xdpi, &ydpi );
                if ( success == S_OK ) {
                    FreeLibrary( hShcore );
                    return static_cast<int>(ydpi);
                }
            }
            FreeLibrary( hShcore );
        }
    }

    // fallback if not available
    HDC hDC = ::GetDC( hWnd );
    INT ydpi = ::GetDeviceCaps( hDC, LOGPIXELSY );
    ::ReleaseDC( NULL, hDC );

    return ydpi;
}

void ApplyFeatureLevel10Downgrades(GothicRendererSettings& s);

void ImGuiShim::Init(
    HWND Window,
    const Microsoft::WRL::ComPtr<ID3D11Device1>& device,
    const Microsoft::WRL::ComPtr<ID3D11DeviceContext1>& context
)
{ 
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ApplyGD3D11DarkStyle();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = NULL;
    io.LogFilename = NULL;
    //io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard; //Not needed and it's annoying.
    OutputWindow = Window;
    ImGui_ImplWin32_Init( OutputWindow );
    ImGui_ImplDX11_Init( device.Get(), context.Get() );

    const auto actualDPI = GetDpi( Window );
    Initiated = true;

    std::vector<DisplayModeInfo> modes;
    Engine::GraphicsEngine->GetDisplayModeList( &modes );
    Resolutions.clear();
    for ( auto it = modes.rbegin(); it != modes.rend(); ++it ) {
        std::string s = std::to_string( (*it).Width ) + "x" + std::to_string( (*it).Height );
        Resolutions.emplace_back( std::make_pair(INT2((*it).Width, (*it).Height), s) );
    }

    //static const ImWchar euroGlyphRanges[] = {
    //    0x0020, 0x007E, // Basic Latin
    //    0x00A0, 0x00FF, // Latin-1 Supplement
    //    0x0100, 0x017F, // Latin Extended-A
    //    0x0180, 0x018F, // Latin Extended-B
    //    0x0400, 0x04FF, // Cyrillic
    //    0x2010, 0x2015, // Various dashes
    //    0x201E, 0x201E, // low-9 quotation mark
    //    0x201C, 0x201D, // high-9 quotation marks
    //    0,              // End of ranges
    //};
    ImFontConfig config = { };
    config.MergeMode = false;
    //config.GlyphRanges = euroGlyphRanges;
    const auto path = std::filesystem::current_path();
    const auto fontpath = path / "system" / "GD3D11" / "Fonts" / "Lato-Semibold.ttf";

    auto dpiScale = actualDPI / 96.0f;
    io.Fonts->AddFontFromFileTTF( fontpath.string().c_str(), 20.0f * dpiScale, &config );}


ImGuiShim::~ImGuiShim()
{
    if ( Initiated ) {
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
    }
}

void ImGuiShim::RenderLoop()
{
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();

    // Keep the settings UI at a stable physical size when the game resolution
    // changes. Mouse coordinates use the same virtual canvas, so the smooth OS
    // cursor and ImGui hit targets remain aligned.
    if ( SettingsVisible && OutputWindow ) {
        POINT cursorPos = {};
        float clientWidth = 0.0f;
        float clientHeight = 0.0f;
        float uiScale = 1.0f;
        if ( GetSettingsUiGeometry( OutputWindow, clientWidth, clientHeight, uiScale, &cursorPos ) ) {
            const INT2 backbuffer = Engine::GraphicsEngine->GetBackbufferResolution();
            ImGuiIO& io = ImGui::GetIO();
            io.DisplaySize = ImVec2( clientWidth / uiScale, clientHeight / uiScale );
            io.DisplayFramebufferScale = ImVec2(
                static_cast<float>( std::max( 1, backbuffer.x ) ) / io.DisplaySize.x,
                static_cast<float>( std::max( 1, backbuffer.y ) ) / io.DisplaySize.y );
            io.AddMousePosEvent( cursorPos.x / uiScale, cursorPos.y / uiScale );
        }
    }
    ImGui::NewFrame();

    // Keep the F11 settings cursor on the OS cursor path; an ImGui-drawn cursor only updates with game frames.
    ImGui::GetIO().MouseDrawCursor = !SettingsVisible && GetIsActive()
        && INT2( ImGui::GetMainViewport()->Size.x, ImGui::GetMainViewport()->Size.y ) != Engine::GraphicsEngine->GetResolution();

    static zSTRING GDX_IMGUI_BEGINFRAME = "GDX_IMGUI_BEGINFRAME";
    static zSTRING GDX_IMGUI_ENDFRAME = "GDX_IMGUI_ENDFRAME";
    static int beginFrameFn = zCParser::GetParser()->GetIndex( GDX_IMGUI_BEGINFRAME );
    static int endFrameFn = zCParser::GetParser()->GetIndex( GDX_IMGUI_ENDFRAME );

    static int retryFindFuncs = 0;
    if ( retryFindFuncs > 120 ) {
        if ( beginFrameFn == -1 ) { beginFrameFn = zCParser::GetParser()->GetIndex( GDX_IMGUI_BEGINFRAME ); }
        if ( endFrameFn == -1 ) { endFrameFn = zCParser::GetParser()->GetIndex( GDX_IMGUI_ENDFRAME ); }
        retryFindFuncs = 0;
    }

    LibShowBlockingThisFrame = false;
    LibShowNonBlockingThisFrame = false;
    if ( beginFrameFn != -1 ) {
        zCParser::GetParser()->CallFunc( beginFrameFn );
    } else {
        retryFindFuncs++;
    }

    auto oldSettings = Engine::GAPI->GetRendererState().RendererSettings;
    if ( SettingsVisible ) {
        RenderSettingsWindow();
    }

    if ( memcmp( &oldSettings, &Engine::GAPI->GetRendererState().RendererSettings, sizeof( GothicRendererSettings ) ) != 0 ) {
        auto& currentSettings = Engine::GAPI->GetRendererState().RendererSettings;
        SyncGraphicsPresetSelection( currentSettings );
        if ( FeatureLevel10Compatibility ) {
            ApplyFeatureLevel10Downgrades( currentSettings );
        }
    }
    //if ( DemoVisible )
    //    ImGui::ShowDemoWindow();

    if ( GetBlockGameInput() != m_lastFrameBlockGameInput ) {
        m_lastFrameBlockGameInput = GetBlockGameInput();
        D3D11GraphicsEngine::UpdateShouldBlockGameInput();
    }

    ImGui::Render();
    ImGui_ImplDX11_RenderDrawData( ImGui::GetDrawData() );

    if ( endFrameFn != -1 ) {
        zCParser::GetParser()->CallFunc( endFrameFn );
    };
}

bool ImGuiShim::GetIsActive() {
    return Initiated && (
        SettingsVisible
        || LibShowBlockingThisFrame
        || LibShowNonBlockingThisFrame
    );
}

bool ImGuiShim::GetBlockGameInput()
{
    if ( !GetIsActive() ) {
        return false;
    }
    if ( SettingsVisible
        || LibShowBlockingThisFrame ) {
        return true;
        }
    return false;
}

LRESULT ImGuiShim::OnWindowMessage( HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam )
{
    if ( Initiated && GetIsActive() )
    {
        // Queue the virtual F11 position before actions so ImGui never
        // evaluates a click with the Win32 backend's physical coordinates.
        if ( SettingsVisible && IsMouseActionMessage( msg ) ) {
            float clientWidth = 0.0f;
            float clientHeight = 0.0f;
            float uiScale = 1.0f;
            POINT cursorPos = {};
            if ( GetSettingsUiGeometry( hWnd, clientWidth, clientHeight, uiScale, &cursorPos ) ) {
                ImGui::GetIO().AddMousePosEvent( cursorPos.x / uiScale, cursorPos.y / uiScale );
            }
        }
        return ImGui_ImplWin32_WndProcHandler( hWnd, msg, wParam, lParam );
    }
    return 0;
}

void ImGuiShim::OnResize( INT2 newSize )
{
    CurrentResolution = newSize;
    if ( SettingsVisible ) {
        m_centerSettingsWindowFrames = 3;
    }

    std::vector<DisplayModeInfo> modes;
    Engine::GraphicsEngine->GetDisplayModeList( &modes );
    Resolutions.clear();
    for ( auto it = modes.rbegin(); it != modes.rend(); ++it ) {
        std::string s = std::to_string( (*it).Width ) + "x" + std::to_string( (*it).Height );
        Resolutions.emplace_back( std::make_pair(INT2((*it).Width, (*it).Height), s) );
    }
}

template <typename Items, typename T>
bool ImComboBoxC( const char* id, const Items& items, T* storage, const std::function<void()>& selected ) {
    if ( storage == nullptr || items.size() == 0 ) {
        return ImGui::BeginCombo( id, "invalid storage" );
    }
    std::pair<const char*, T> selectedItem = items[0];
    for ( auto& it : items ) {
        if ( it.second == *storage ) {
            selectedItem = it;
            break;
        }
    }
    if ( ImGui::BeginCombo( id, selectedItem.first ) ) {
        for ( size_t i = 0; i < items.size(); i++ ) {
            bool isSelected = (*storage == items[i].second);

            if ( ImGui::Selectable( items[i].first, isSelected ) ) {
                *storage = items[i].second;
                selected();
            }

            if ( isSelected ) {
                ImGui::SetItemDefaultFocus();
            }
        }
        return true;
    }
    return false;
}

template <typename Items, typename T>
bool ImComboBoxCT( const char* id, const Items& items, T* storage, const std::function<void()>& selected ) {
    if ( storage == nullptr || items.size() == 0 ) {
        return ImGui::BeginCombo( id, "invalid storage" );
    }
    auto selectedItem = items[0];
    for ( auto& it : items ) {
        if ( std::get<1>( it ) == *storage ) {
            selectedItem = it;
            break;
        }
    }
    if ( ImGui::BeginCombo( id, std::get<0>( selectedItem )) ) {
        for ( size_t i = 0; i < items.size(); i++ ) {
            bool isSelected = (*storage == std::get<1>( items[i] ));

            if ( ImGui::Selectable( std::get<0>( items[i] ), isSelected ) ) {
                *storage = std::get<1>( items[i] );
                selected();
            }
            if ( std::get<2>(items[i]) ) {
                ImGui::SetItemTooltip( "%s", std::get<2>( items[i] ) );
            }

            if ( isSelected ) {
                ImGui::SetItemDefaultFocus();
            }
        }
        return true;
    }
    return false;
}

template <typename Items, typename T>
bool ImComboBox( const char* id, const Items& items, T* storage ) {
    if ( storage == nullptr || items.size() == 0 ) {
        return ImGui::BeginCombo( id, "invalid storage" );
    }
    std::pair<const char*, T> selectedItem = items[0];
    for ( auto& it : items ) {
        if ( it.second == *storage ) {
            selectedItem = it;
            break;
        }
    }
    if ( ImGui::BeginCombo( id, selectedItem.first ) ) {
        for ( size_t i = 0; i < items.size(); i++ ) {
            bool isSelected = (*storage == items[i].second);

            if ( ImGui::Selectable( items[i].first, isSelected ) ) {
                *storage = items[i].second;
            }

            if ( isSelected ) {
                ImGui::SetItemDefaultFocus();
            }
        }
        return true;
    }
    return false;
}

void ImText( const char* label, const ImVec2& size ) {
    auto& col = ImGui::GetStyleColorVec4( ImGuiCol_::ImGuiCol_Button );

    ImGui::PushStyleColor( ImGuiCol_::ImGuiCol_ButtonActive, col );
    ImGui::PushStyleColor( ImGuiCol_::ImGuiCol_ButtonHovered, col );
    ImGui::PushStyleVarX( ImGuiStyleVar_::ImGuiStyleVar_ButtonTextAlign, 0 );

    ImGui::Button( label, size );
    ImGui::PopStyleVar( 1 );

    ImGui::PopStyleColor( 2 );
}

void ApplyFeatureLevel10Downgrades(GothicRendererSettings& s) {
    // one 4k texture, 1/2 2k textures max.
    s.NumShadowCascades = std::min(s.NumShadowCascades, MAX_CSM_CASCADES);
    if ( s.AntiAliasingMode == GothicRendererSettings::AA_FSR3
        && s.Upscaler == GothicRendererSettings::UPSCALER_FSR_3 ) {
        s.AntiAliasingMode = GothicRendererSettings::AA_SMAA;
        s.Upscaler = GothicRendererSettings::UPSCALER_DEFAULT;
        s.ResolutionScalePercent = 100;
        s.SharpenFactor = 0.2f;
    }
    s.AoMode = AOMode::AO_NONE;

    if (s.NumShadowCascades >= 2) {
        s.DebugSettings.ShadowCascades.Lambda = D3D11ShadowMap::lambdaBiasTable[s.NumShadowCascades].lambda;
        s.DebugSettings.ShadowCascades.Bias = D3D11ShadowMap::lambdaBiasTable[s.NumShadowCascades].bias;
    }
}

namespace
{
    constexpr float OBJECT_DRAW_DISTANCE_MIN_KM = 2.5f;
    constexpr float OBJECT_DRAW_DISTANCE_MAX_KM = 25.0f;
    constexpr int OBJECT_DRAW_DISTANCE_UI_MIN = 1;
    constexpr int OBJECT_DRAW_DISTANCE_UI_MAX = 10;

    float ObjectDrawDistanceUiToMeters( int value ) {
        const int clamped = std::clamp( value, OBJECT_DRAW_DISTANCE_UI_MIN, OBJECT_DRAW_DISTANCE_UI_MAX );
        const float t = static_cast<float>(clamped - OBJECT_DRAW_DISTANCE_UI_MIN)
            / static_cast<float>(OBJECT_DRAW_DISTANCE_UI_MAX - OBJECT_DRAW_DISTANCE_UI_MIN);
        return (OBJECT_DRAW_DISTANCE_MIN_KM + t * (OBJECT_DRAW_DISTANCE_MAX_KM - OBJECT_DRAW_DISTANCE_MIN_KM)) * 1000.0f;
    }

    int ObjectDrawDistanceMetersToUi( float meters ) {
        const float km = std::clamp( meters / 1000.0f, OBJECT_DRAW_DISTANCE_MIN_KM, OBJECT_DRAW_DISTANCE_MAX_KM );
        const float t = (km - OBJECT_DRAW_DISTANCE_MIN_KM) / (OBJECT_DRAW_DISTANCE_MAX_KM - OBJECT_DRAW_DISTANCE_MIN_KM);
        return std::clamp( static_cast<int>(std::round(OBJECT_DRAW_DISTANCE_UI_MIN + t * (OBJECT_DRAW_DISTANCE_UI_MAX - OBJECT_DRAW_DISTANCE_UI_MIN))),
            OBJECT_DRAW_DISTANCE_UI_MIN, OBJECT_DRAW_DISTANCE_UI_MAX );
    }

    int PointlightShadowSizeForWorldShadowSize( int worldShadowSize ) {
        if ( worldShadowSize >= 4096 ) return 256;
        return 128;
    }

    int NormalizeShadowMapSize( int value ) {
        if ( value <= 1024 ) return 1024;
        if ( value <= 2048 ) return 2048;
        if ( value <= 4096 ) return 4096;
        return 8192;
    }

    int NormalizePointlightShadowMapSize( int value ) {
        if ( value <= 128 ) return 128;
        if ( value <= 256 ) return 256;
        return 512;
    }

    bool UsesTemporalSharpeningBoost( const GothicRendererSettings& s ) {
        return s.AntiAliasingMode == GothicRendererSettings::E_AntiAliasingMode::AA_FSR3;
    }

    void ApplyAntiAliasingDependentSettings( GothicRendererSettings& s ) {
        s.SharpenFactor = UsesTemporalSharpeningBoost( s ) ? 1.0f : 0.2f;
    }
}
namespace {
    bool IsWindEffectsControlVisible() {
#ifdef BUILD_GOTHIC_2_6_fix
        return true;
#elif defined(BUILD_GOTHIC_1_08k) && !defined(BUILD_1_12F)
        return haveWindAnimations;
#else
        return false;
#endif
    }
}
struct GraphicsPresetComparable {
    int textureMaxSize;
    int ShadowMapSize;
    float ShadowSoftness;
    int AoMode;
    bool EnableContactShadows;
    bool EnableScreenSpaceGI;
    bool EnableDoF;
    bool EnableDynamicClouds;
    bool EnableAmbientParticles;
    int WindQuality;
    bool EnableGodRays;
    int OutdoorSmallVobDrawDistance;
    int SectionDrawRadius;
    float AOStrength;
    float ScreenSpaceGIStrength;
    float DoFBokehRadius;
    float GodRayStrength;
    float GlobalWindStrength;
};

GraphicsPresetComparable MakeGraphicsPresetComparable(
    const GothicRendererSettings& s ) {
    return {
        s.textureMaxSize,
        NormalizeShadowMapSize( s.ShadowMapSize ),
        s.ShadowSoftness,
        static_cast<int>(s.AoMode),
        s.EnableContactShadows,
        s.EnableScreenSpaceGI,
        s.EnableDoF,
        s.EnableDynamicClouds,
        s.EnableAmbientParticles,
        IsWindEffectsControlVisible() ? s.WindQuality : 0,
        s.EnableGodRays,
        ObjectDrawDistanceMetersToUi(
            s.OutdoorSmallVobDrawRadius ),
        s.SectionDrawRadius,
        s.AOStrength,
        s.ScreenSpaceGIStrength,
        s.DoFBokehRadius,
        s.GodRayStrength,
        IsWindEffectsControlVisible() ? s.GlobalWindStrength : 0.0f,
    };
}

bool GraphicsPresetComparableEqual(
    const GraphicsPresetComparable& a,
    const GraphicsPresetComparable& b ) {
    return a.textureMaxSize == b.textureMaxSize
        && a.ShadowMapSize == b.ShadowMapSize
        && a.ShadowSoftness == b.ShadowSoftness
        && a.AoMode == b.AoMode
        && a.EnableContactShadows == b.EnableContactShadows
        && a.EnableScreenSpaceGI == b.EnableScreenSpaceGI
        && a.EnableDoF == b.EnableDoF
        && a.EnableDynamicClouds == b.EnableDynamicClouds
        && a.EnableAmbientParticles == b.EnableAmbientParticles
        && a.WindQuality == b.WindQuality
        && a.EnableGodRays == b.EnableGodRays
        && a.OutdoorSmallVobDrawDistance
            == b.OutdoorSmallVobDrawDistance
        && a.SectionDrawRadius == b.SectionDrawRadius
        && a.AOStrength == b.AOStrength
        && a.ScreenSpaceGIStrength == b.ScreenSpaceGIStrength
        && a.DoFBokehRadius == b.DoFBokehRadius
        && a.GodRayStrength == b.GodRayStrength
        && a.GlobalWindStrength == b.GlobalWindStrength;
}

void ApplyGraphicsPresets( GothicRendererSettings& s, bool applyRuntimeUpdates = true ) {
    const auto preset = s.GraphicsPreset;
    if ( preset == GothicRendererSettings::E_GraphicsPreset::GRAPHICS_CUSTOM ) {
        return;
    }

    // Presets only own the quality controls displayed below the menu separator.
    s.EnableGodRays = true;
    s.EnableDynamicClouds = true;
    s.EnableAmbientParticles = true;

    // Reset all visible effect strengths to their normalized UI defaults.
    s.AOStrength = 1.0f;
    s.ScreenSpaceGIStrength = 1.0f;
    s.GodRayStrength = 1.0f;
    s.DoFBokehRadius = 3.5f;
    if ( IsWindEffectsControlVisible() ) s.GlobalWindStrength = 1.0f;
    s.ShadowSoftness = 1.0f;

    switch ( preset ) {
    case GothicRendererSettings::GRAPHICS_LOW:
        s.ShadowMapSize = 1024;
        s.AoMode = AOMode::AO_XEGTAO;
        s.EnableContactShadows = false;
        s.EnableScreenSpaceGI = false;
        s.EnableDoF = false;
        s.EnableDynamicClouds = false;
        s.EnableAmbientParticles = false;
        if ( IsWindEffectsControlVisible() ) s.WindQuality = GothicRendererSettings::EWindQuality::WIND_QUALITY_ADVANCED;
        s.OutdoorSmallVobDrawRadius = ObjectDrawDistanceUiToMeters( 2 );
        s.SectionDrawRadius = 3;
        s.textureMaxSize = static_cast<int>(TX_QUALITY::High);
        break;
    case GothicRendererSettings::GRAPHICS_MEDIUM:
        s.ShadowMapSize = 2048;
        s.AoMode = AOMode::AO_XEGTAO;
        s.EnableContactShadows = true;
        s.EnableScreenSpaceGI = false;
        s.EnableDoF = true;
        s.EnableDynamicClouds = true;
        s.EnableAmbientParticles = true;
        if ( IsWindEffectsControlVisible() ) s.WindQuality = GothicRendererSettings::EWindQuality::WIND_QUALITY_ADVANCED;
        s.OutdoorSmallVobDrawRadius = ObjectDrawDistanceUiToMeters( 4 );
        s.SectionDrawRadius = 4;
        s.textureMaxSize = static_cast<int>(TX_QUALITY::MAX);
        break;
    case GothicRendererSettings::GRAPHICS_HIGH:
        s.ShadowMapSize = 4096;
        s.AoMode = AOMode::AO_XEGTAO;
        s.EnableContactShadows = true;
        s.EnableScreenSpaceGI = false;
        s.EnableDoF = true;
        s.EnableDynamicClouds = true;
        s.EnableAmbientParticles = true;
        if ( IsWindEffectsControlVisible() ) s.WindQuality = GothicRendererSettings::EWindQuality::WIND_QUALITY_ADVANCED;
        s.OutdoorSmallVobDrawRadius = ObjectDrawDistanceUiToMeters( 6 );
        s.SectionDrawRadius = 5;
        s.textureMaxSize = static_cast<int>(TX_QUALITY::MAX);
        break;
    case GothicRendererSettings::GRAPHICS_VERY_HIGH:
        s.ShadowMapSize = 8192;
        s.AoMode = AOMode::AO_XEGTAO;
        s.EnableContactShadows = true;
        s.EnableScreenSpaceGI = true;
        s.EnableDoF = true;
        s.EnableDynamicClouds = true;
        s.EnableAmbientParticles = true;
        if ( IsWindEffectsControlVisible() ) s.WindQuality = GothicRendererSettings::EWindQuality::WIND_QUALITY_ADVANCED;
        s.OutdoorSmallVobDrawRadius = ObjectDrawDistanceUiToMeters( 8 );
        s.SectionDrawRadius = 6;
        s.textureMaxSize = static_cast<int>(TX_QUALITY::MAX);
        break;
    default:
        return;
    }

    if ( s.AoMode == AOMode::AO_NONE ) s.AOStrength = 0.0f;
    if ( !s.EnableScreenSpaceGI ) s.ScreenSpaceGIStrength = 0.0f;
    if ( !s.EnableGodRays ) s.GodRayStrength = 0.0f;
    if ( !s.EnableDoF ) s.DoFBokehRadius = 0.0f;
    if ( IsWindEffectsControlVisible() && s.WindQuality == GothicRendererSettings::EWindQuality::WIND_QUALITY_NONE ) s.GlobalWindStrength = 0.0f;

    s.ShadowMapSize = NormalizeShadowMapSize( s.ShadowMapSize );

    if ( FeatureLevel10Compatibility ) {
        // Preset dependency stays inside the visible AO control; display/AA settings
        // are handled by the normal hardware compatibility path outside the preset.
        s.AoMode = AOMode::AO_NONE;
        s.AOStrength = 0.0f;
    }

    if ( applyRuntimeUpdates ) {
        Engine::GAPI->UpdateTextureMaxSize();
        Engine::GraphicsEngine->ReloadShaders( ShaderCategory::Other );
    }
}

void SyncGraphicsPresetSelection( GothicRendererSettings& s ) {
    if ( s.GraphicsPreset != GothicRendererSettings::GRAPHICS_CUSTOM ) {
        GothicRendererSettings expected = s;
        expected.GraphicsPreset = s.GraphicsPreset;
        ApplyGraphicsPresets( expected, false );
        if ( GraphicsPresetComparableEqual( MakeGraphicsPresetComparable( s ), MakeGraphicsPresetComparable( expected ) ) ) {
            return;
        }
    }

    const GothicRendererSettings::E_GraphicsPreset presets[] = {
        GothicRendererSettings::GRAPHICS_LOW,
        GothicRendererSettings::GRAPHICS_MEDIUM,
        GothicRendererSettings::GRAPHICS_HIGH,
        GothicRendererSettings::GRAPHICS_VERY_HIGH,
    };

    for ( auto preset : presets ) {
        GothicRendererSettings expected = s;
        expected.GraphicsPreset = preset;
        ApplyGraphicsPresets( expected, false );
        if ( GraphicsPresetComparableEqual( MakeGraphicsPresetComparable( s ), MakeGraphicsPresetComparable( expected ) ) ) {
            s.GraphicsPreset = preset;
            return;
        }
    }

    s.GraphicsPreset = GothicRendererSettings::GRAPHICS_CUSTOM;
}
namespace
{
    void FixupSettings( GothicRendererSettings& s ) {
        s.FixupUpscalingSettings();
        const int presetValue = static_cast<int>(s.GraphicsPreset);
        if ( presetValue == 1 ) {
            s.GraphicsPreset = GothicRendererSettings::GRAPHICS_LOW;
        } else if ( presetValue > static_cast<int>(GothicRendererSettings::GRAPHICS_VERY_HIGH) ) {
            s.GraphicsPreset = GothicRendererSettings::GRAPHICS_VERY_HIGH;
        }
        s.D3D11Language = static_cast<GothicRendererSettings::E_D3D11Language>(std::clamp<int>(
            static_cast<int>(s.D3D11Language),
            static_cast<int>(GothicRendererSettings::D3D11_LANGUAGE_ENGLISH),
            static_cast<int>(GothicRendererSettings::D3D11_LANGUAGE_GERMAN) ));
        s.LimitLightIntesity = true;
        s.VisualFXDrawRadius = VISUAL_FX_DRAW_RADIUS_FIXED;
        s.EnableShadows = true;
        s.ShadowFilterMode = FeatureLevel10Compatibility
            ? GothicRendererSettings::E_ShadowFilterMode::SHADOW_FILTER_SIMPLE
            : GothicRendererSettings::E_ShadowFilterMode::SHADOW_FILTER_PCSS;
        s.EnablePointlightShadows = GothicRendererSettings::EPointLightShadowMode::PLS_UPDATE_DYNAMIC;
        s.ShadowMapSize = NormalizeShadowMapSize( s.ShadowMapSize );
        s.PointlightShadowMapSize = NormalizePointlightShadowMapSize( s.PointlightShadowMapSize );
        s.EnableWaterAnimation = true;
        s.EnableSSS = true;
        s.SSSIntensity = 1.0f;
        const bool windEffectsEnabled =
            s.WindQuality != GothicRendererSettings::EWindQuality::WIND_QUALITY_NONE;
        if ( !windEffectsEnabled ) {
            s.HeroAffectsObjects = false;
            s.HeroAffectsObjectsStrength = 0.0f;
            s.HeroAffectsObjectsRadius = 0.0f;
        } else {
            s.HeroAffectsObjects = s.HeroAffectsObjectsRadius > 0.001f;
            s.HeroAffectsObjectsStrength = 1.0f;
        }
        s.HDRToneMapStrength = std::clamp( s.HDRToneMapStrength, 0.0f, 2.0f );
        // Disabled coupled controls must always display their true zero effect state.
        if ( !s.EnableHDR ) s.HDRToneMapStrength = 0.0f;
        if ( s.AoMode == AOMode::AO_NONE ) s.AOStrength = 0.0f;
        if ( !s.EnableScreenSpaceGI ) s.ScreenSpaceGIStrength = 0.0f;
        if ( !s.EnableGodRays ) s.GodRayStrength = 0.0f;
        if ( !s.EnableSSR ) s.SSRStrength = 0.0f;

        if ( !s.EnableDoF ) s.DoFBokehRadius = 0.0f;
        if ( s.WindQuality == GothicRendererSettings::EWindQuality::WIND_QUALITY_NONE ) s.GlobalWindStrength = 0.0f;
        s.OutdoorSmallVobDrawRadius = ObjectDrawDistanceUiToMeters( ObjectDrawDistanceMetersToUi( s.OutdoorSmallVobDrawRadius ) );
        s.ForceFOV = false;
        s.FOVHoriz = 100.0f;
        s.FOVVert = 100.0f;
    }
}

void ImGuiShim::BeginSettingsEdit() {
    if ( m_settingsEditActive ) {
        return;
    }

    m_centerSettingsWindowFrames = 3;
    ResetStrengthControlMemories();
    m_settingsSnapshot = Engine::GAPI->GetRendererState().RendererSettings;
    m_settingsResolutionSnapshot = CurrentResolution;
    m_settingsEditActive = true;
}

void ImGuiShim::CommitSettingsEdit() {
    ResetStrengthControlMemories();
    m_settingsEditActive = false;
}

void ImGuiShim::CancelSettingsEdit() {
    if ( !m_settingsEditActive ) {
        return;
    }

    auto& settings = Engine::GAPI->GetRendererState().RendererSettings;
    const bool textureQualityChanged = settings.textureMaxSize != m_settingsSnapshot.textureMaxSize;
    settings = m_settingsSnapshot;
    ResetStrengthControlMemories();
    FixupSettings( settings );
    m_settingsEditActive = false;

    if ( textureQualityChanged ) {
        Engine::GAPI->UpdateTextureMaxSize();
    }
    Engine::GraphicsEngine->TriggerResize( m_settingsResolutionSnapshot );
    Engine::GraphicsEngine->ReloadShaders( ShaderCategory::All );
}

void ImGuiShim::RenderSettingsWindow()
{
    // Autosized settings by child objects & centered
    IM_ASSERT( ImGui::GetCurrentContext() != NULL && "Missing Dear ImGui context!" );
    IMGUI_CHECKVERSION();

    const ImVec2 displaySize = ImGui::GetIO().DisplaySize;
    const INT2 windowSize(
        std::max( 1, static_cast<int>( std::round( displaySize.x ) ) ),
        std::max( 1, static_cast<int>( std::round( displaySize.y ) ) ) );
    auto& style = ImGui::GetStyle();
    const float framebufferWidth = static_cast<float>( windowSize.x );
    const float framebufferHeight = static_cast<float>( windowSize.y );
    const float menuScale = 1.0f;
    const float labelWidth = std::round( 275.0f * menuScale );
    const float controlWidth = std::round( 250.0f * menuScale );
    const float footerHeight = std::round( 30.0f * menuScale );
    const ImVec2 scaledWindowPadding(
        std::round( style.WindowPadding.x * menuScale ),
        std::round( style.WindowPadding.y * menuScale ) );
    const ImVec2 scaledFramePadding(
        std::round( style.FramePadding.x * menuScale ),
        std::round( style.FramePadding.y * menuScale ) );
    const ImVec2 scaledItemSpacing(
        std::round( style.ItemSpacing.x * menuScale ),
        std::round( style.ItemSpacing.y * menuScale ) );
    ImVec2 buttonWidth( labelWidth, 0 );

    static const char* settingsLabel = "##GD3D11Settings";

    ShaderCategory shadersToReload = ShaderCategory::None;

    const ImVec2 maxSettingsWindowSize(
        std::max( 320.0f, framebufferWidth - 20.0f ),
        std::max( 240.0f, framebufferHeight - 20.0f ) );
    ImGui::PushStyleVar( ImGuiStyleVar_WindowPadding, scaledWindowPadding );
    ImGui::PushStyleVar( ImGuiStyleVar_FramePadding, scaledFramePadding );
    ImGui::PushStyleVar( ImGuiStyleVar_ItemSpacing, scaledItemSpacing );
    ImGui::SetNextWindowSizeConstraints( ImVec2( 0.0f, 0.0f ), maxSettingsWindowSize );
    const bool centerSettingsWindow = m_centerSettingsWindowFrames > 0;
    if ( centerSettingsWindow ) {
        ImGui::SetNextWindowPos(
            ImVec2( windowSize.x / 2, windowSize.y / 2 ),
            ImGuiCond_Always,
            ImVec2( 0.5f, 0.5f ) );
    }
    if ( ImGui::Begin( settingsLabel, nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoTitleBar ) ) {
        ImGui::SetWindowFontScale( 1.0f );
        if ( centerSettingsWindow ) {
            const ImVec2 actualWindowSize = ImGui::GetWindowSize();
            ImGui::SetWindowPos( ImVec2(
                std::round( (framebufferWidth - actualWindowSize.x) * 0.5f ),
                std::round( (framebufferHeight - actualWindowSize.y) * 0.5f ) ),
                ImGuiCond_Always );
        }
        GothicRendererSettings& settings = Engine::GAPI->GetRendererState().RendererSettings;
        FixupSettings(settings);
        bool german = Engine::GAPI->IsGermanMenuLanguage();
        const auto Tr = [&german]( const char* english, const auto* germanText ) -> const char* {
            return german ? reinterpret_cast<const char*>( germanText ) : english;
        };

        const std::array<std::pair<const char*, int>, 4> graphicsPresets = {{
            {Tr( "Low", u8"Niedrig" ), GothicRendererSettings::E_GraphicsPreset::GRAPHICS_LOW},
            {Tr( "Medium", u8"Mittel" ), GothicRendererSettings::E_GraphicsPreset::GRAPHICS_MEDIUM},
            {Tr( "High", u8"Hoch" ), GothicRendererSettings::E_GraphicsPreset::GRAPHICS_HIGH},
            {Tr( "Extreme", u8"Extrem" ), GothicRendererSettings::E_GraphicsPreset::GRAPHICS_VERY_HIGH},
        }};

        const char* graphicsPresetPreview = Tr( "Custom", u8"Individuell" );
        for ( const auto& preset : graphicsPresets ) {
            if ( preset.second == static_cast<int>(settings.GraphicsPreset) ) {
                graphicsPresetPreview = preset.first;
                break;
            }
        }

        const float topPresetLabelWidth = 170.0f;
        const float topPresetControlWidth = 145.0f;
        const float topLanguageLabelWidth = topPresetLabelWidth;
        const float topLanguageControlWidth = topPresetControlWidth;

        ImText( Tr( "Graphics Preset", u8"Grafikprofil" ), ImVec2( topPresetLabelWidth, 0.0f ) ); ImGui::SameLine();

        ImGui::PushItemWidth( topPresetControlWidth );
        if ( ImGui::BeginCombo( "##GraphicsPreset", graphicsPresetPreview ) ) {
            for ( const auto& preset : graphicsPresets ) {
                const bool isSelected = static_cast<int>(settings.GraphicsPreset) == preset.second;
                if ( ImGui::Selectable( preset.first, isSelected ) ) {
                    ResetStrengthControlMemories();
                    settings.GraphicsPreset = static_cast<GothicRendererSettings::E_GraphicsPreset>(preset.second);
                    ApplyGraphicsPresets( settings );
                }
                if ( isSelected ) {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SetItemTooltip( "%s", Tr( "Applies a predefined balance of visual quality and performance.", u8"Wendet eine vorgegebene Abstimmung von Bildqualit\u00E4t und Leistung an." ) );
        ImGui::PopItemWidth();

        ImGui::SameLine();
        ImText( Tr( "Language", u8"Sprache" ), ImVec2( topLanguageLabelWidth, 0.0f ) ); ImGui::SameLine();
        ImGui::PushItemWidth( topLanguageControlWidth );
        const char* languagePreview = settings.D3D11Language == GothicRendererSettings::D3D11_LANGUAGE_GERMAN
            ? "Deutsch"
            : "English";
        if ( ImGui::BeginCombo( "##D3D11Language", languagePreview ) ) {
            const std::array<std::pair<const char*, GothicRendererSettings::E_D3D11Language>, 2> languages = {
                std::pair{"English", GothicRendererSettings::D3D11_LANGUAGE_ENGLISH},
                std::pair{"Deutsch", GothicRendererSettings::D3D11_LANGUAGE_GERMAN},
            };
            for ( const auto& language : languages ) {
                const bool isSelected = settings.D3D11Language == language.second;
                if ( ImGui::Selectable( language.first, isSelected ) ) {
                    settings.D3D11Language = language.second;
                    german = settings.D3D11Language == GothicRendererSettings::D3D11_LANGUAGE_GERMAN;
                }
                if ( isSelected ) {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SetItemTooltip( "%s", Tr( "Selects the language used by the D3D11 renderer.", u8"W\u00E4hlt die Sprache des D3D11-Renderers aus." ) );
        ImGui::PopItemWidth();

        const std::string versionText = std::string( Tr( "D3D11 Version ", u8"D3D11-Version " ) ) + VERSION_NUMBER;
        const ImVec2 versionTextSize = ImGui::CalcTextSize( versionText.c_str() );
        ImGui::SameLine();
        ImGui::SetCursorPosX( std::max( ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - versionTextSize.x ) );
        ImGui::TextDisabled( "%s", versionText.c_str() );
        ImGui::Separator();

        const float standardComboWidth = controlWidth;
        // All right-column value controls start at the same x position.
        const float inlineToggleWidth = (buttonWidth.x - style.ItemSpacing.x) * 0.5f;
        const float inlineToggleLabelWidth = inlineToggleWidth - ImGui::GetFrameHeight() - style.ItemSpacing.x;
        
        {
            ImGui::BeginGroup();
            ImGui::PushItemWidth( controlWidth );

            for (size_t i = 0; i < Resolutions.size(); ++i){
                if (Resolutions[i].first == CurrentResolution) {
                    ResolutionState = i;
                    break;
                }
            }
            ImText( Tr( "Resolution", u8"Aufl\u00F6sung" ), buttonWidth ); ImGui::SameLine();
            if ( ImGui::BeginCombo( "##Resolution", Resolutions[ResolutionState].second.c_str() ) ) {
                for ( size_t i = 0; i < Resolutions.size(); i++ ) {
                    bool isSelected = (ResolutionState == i);

                    if ( ImGui::Selectable( Resolutions[i].second.c_str(), isSelected ) ) {
                        Engine::GraphicsEngine->TriggerResize(Resolutions[i].first);
                    }

                    if ( isSelected ) {
                        ImGui::SetItemDefaultFocus();
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::SetItemTooltip( "%s", Tr( "Changes the game output size.", u8"\u00C4ndert die Ausgabeaufl\u00F6sung des Spiels." ) );

            ImText( Tr( "Display Mode [Restart]", u8"Anzeigemodus [Neustart]" ), buttonWidth );
            ImGui::SetItemTooltip( "%s", Tr( "Changes between fullscreen and windowed mode after restarting.", u8"Wechselt nach einem Neustart zwischen Vollbild und Fenstermodus." ) );
            ImGui::SameLine();

            auto displayModeState = settings.ChangeWindowPreset
                ? static_cast<WindowModes>(settings.ChangeWindowPreset)
                : InterpretWindowMode( settings );
            if ( displayModeState != WindowModes::WINDOW_MODE_WINDOWED ) {
                displayModeState = WindowModes::WINDOW_MODE_FULLSCREEN_BORDERLESS;
            }
            const std::array<std::tuple<const char*, WindowModes, const char*>, 2> DisplayEnums = {{
                { Tr( "Fullscreen", u8"Vollbild" ), WindowModes::WINDOW_MODE_FULLSCREEN_BORDERLESS, nullptr },
                { Tr( "Windowed", u8"Fenstermodus" ), WindowModes::WINDOW_MODE_WINDOWED, nullptr},
            }};

            if ( ImComboBoxCT( "##DisplayMode", DisplayEnums, &displayModeState, [&settings, &displayModeState] {
                // selected
                settings.ChangeWindowPreset = displayModeState;
                } ) ) {
                ImGui::EndCombo();
            }


            ImGui::SetItemTooltip( "%s", Tr( "Fullscreen fills the monitor without changing its display mode.", u8"Vollbild f\u00FCllt den Monitor ohne dessen Anzeigemodus zu \u00E4ndern." ) );

            const std::array<std::tuple<const char*, GothicRendererSettings::E_AntiAliasingMode, const char*>, 3> antiAliasing = {{
                {Tr( "Disabled", u8"Aus" ), GothicRendererSettings::E_AntiAliasingMode::AA_NONE, nullptr },
                {"SMAA", GothicRendererSettings::E_AntiAliasingMode::AA_SMAA, nullptr },
                {"FSR 3", GothicRendererSettings::E_AntiAliasingMode::AA_FSR3, "FidelityFX Super Resolution 3"},
            }};
            {
                ImGui::PushID( "AntiAliasingSettings" );
                auto selectedMode = settings.AntiAliasingMode;
                const bool wasFSRAntiAliasing = settings.AntiAliasingMode == GothicRendererSettings::E_AntiAliasingMode::AA_FSR3;
                ImText( Tr( "Anti Aliasing", u8"Kantengl\u00E4ttung" ), buttonWidth ); ImGui::SameLine();
                if ( ImComboBoxCT( "##AntiAliasing", antiAliasing, &selectedMode, [&selectedMode, &settings, wasFSRAntiAliasing] {
                    const bool selectsFSRAntiAliasing = selectedMode == GothicRendererSettings::E_AntiAliasingMode::AA_FSR3;
                    if ( wasFSRAntiAliasing && !selectsFSRAntiAliasing ) {
                        settings.ResolutionScalePercent = 100;
                    }

                    if ( selectedMode == GothicRendererSettings::E_AntiAliasingMode::AA_FSR3 ) {
                        settings.Upscaler = GothicRendererSettings::E_Upscaler::UPSCALER_FSR_3;
                    }
                    settings.AntiAliasingMode = selectedMode;
                    FixupSettings( settings );
                    ApplyAntiAliasingDependentSettings( settings );
                    } ) ) {
                    ImGui::EndCombo();
                }
                ImGui::SetItemTooltip( "%s", Tr( "Smooths jagged edges using the selected method.", u8"Gl\u00E4ttet sichtbare Treppenkanten mit dem gew\u00E4hlten Verfahren." ) );
                ImGui::PopID();
            }

            ImText( Tr( "Render Scale", u8"Renderaufl\u00F6sung" ), buttonWidth ); ImGui::SameLine();
            if ( settings.Upscaler == GothicRendererSettings::UPSCALER_FSR_3 ) {
                settings.ResolutionScalePercent = std::clamp( settings.ResolutionScalePercent, 33, 100 );
                // Display "levels" as typical for FSR
                const std::array<std::pair<const char*, int>, 6> fsrLevels = {{
                    { Tr( "Native AA", u8"Nativ mit AA" ), 100 },
                    { Tr( "High Quality", u8"Sehr hohe Qualit\u00E4t" ), 83 },
                    { Tr( "Quality", u8"Qualit\u00E4t" ), 75 },
                    { Tr( "Balanced", u8"Ausgeglichen" ), 66 },
                    { Tr( "Performance", u8"Leistung" ), 50 },
                    { Tr( "Ultra Performance", u8"Maximale Leistung" ), 33 },
                }};
                if (ImComboBox( "##ResolutionScalePercent", fsrLevels, &settings.ResolutionScalePercent ) ) {
                    ImGui::EndCombo();
                }
                ImGui::SetItemTooltip( "%s", Tr( "Balances image detail and performance when using FSR 3.", u8"Bestimmt das Verh\u00E4ltnis zwischen Bilddetails und Leistung mit FSR 3." ) );
            } else {
                int resolutionScale = SnapRenderScalePercentNonFSR( settings.ResolutionScalePercent );
                if ( resolutionScale != settings.ResolutionScalePercent ) {
                    settings.ResolutionScalePercent = resolutionScale;
                    FixupSettings( settings );
                }
                if ( SliderRenderScalePercentNonFSR( "##ResolutionScalePercent", &resolutionScale ) ) {
                    settings.ResolutionScalePercent = resolutionScale;
                    FixupSettings( settings );
                }
                ImGui::SetItemTooltip( "%s", Tr( "Changes image detail and rendering performance.", u8"Bestimmt das Verh\u00E4ltnis zwischen Bilddetails und Renderleistung." ) );
            }

            ImText( Tr( "Contrast", u8"Kontrast" ), buttonWidth ); ImGui::SameLine();
            SliderDisplayTuningStrength( "##Contrast", &settings.GammaValue );
            ImGui::SetItemTooltip( "%s", Tr( "Changes the difference between dark and bright areas.", u8"Ver\u00E4ndert den Unterschied zwischen dunklen und hellen Bereichen." ) );

            ImText( Tr( "Brightness", u8"Helligkeit" ), buttonWidth ); ImGui::SameLine();
            SliderDisplayTuningStrength( "##Brightness", &settings.BrightnessValue );
            ImGui::SetItemTooltip( "%s", Tr( "Makes the overall image darker or brighter.", u8"Macht das gesamte Bild dunkler oder heller." ) );
            ImGui::PopItemWidth();
            ImGui::EndGroup();
        }

        ImGui::SameLine();

        {
            ImGui::BeginGroup();
            ImText( "VSync", { inlineToggleLabelWidth, buttonWidth.y } ); ImGui::SameLine();
            ImGui::Checkbox( "##Enable VSync", &settings.EnableVSync );
            ImGui::SetItemTooltip( "%s", Tr( "Synchronizes frames with the monitor to prevent screen tearing.", u8"Synchronisiert die Bildausgabe mit dem Monitor und verhindert Bildrisse." ) );
            ImGui::SameLine();

            if ( settings.FpsLimit > 0 ) {
                settings.FpsLimitLastEnabled = std::clamp( settings.FpsLimit, 10, 300 );
            }
            bool fpsLimitEnabled = settings.FpsLimit > 0;
            ImGui::BeginDisabled( settings.EnableVSync );
            ImText( Tr( "FPS Limit", u8"FPS-Limit" ), { inlineToggleLabelWidth, buttonWidth.y } ); ImGui::SameLine();
            if ( ImGui::Checkbox( "##Enable FPS Limit", &fpsLimitEnabled ) ) {
                settings.FpsLimit = fpsLimitEnabled ? settings.FpsLimitLastEnabled : 0;
            }
            ImGui::SetItemTooltip( settings.EnableVSync
                ? Tr( "The FPS limiter is inactive while VSync is enabled.", u8"Das FPS-Limit ist bei aktivem VSync inaktiv." )
                : Tr( "Enables an independent frame-rate limit.", u8"Aktiviert eine unabh\u00E4ngige Begrenzung der Bildrate." ) );
            ImGui::SameLine();

            int inactiveFpsLimit = settings.FpsLimitLastEnabled;
            int* displayedFpsLimit = fpsLimitEnabled ? &settings.FpsLimit : &inactiveFpsLimit;
            ImGui::BeginDisabled( !fpsLimitEnabled );
            ImGui::SetNextItemWidth( standardComboWidth );
            ImGui::SliderInt( "##FPSLimit", displayedFpsLimit, 10, 300,
                settings.EnableVSync ? Tr( "Inactive (VSync)", u8"Inaktiv (VSync)" ) : (fpsLimitEnabled ? "%d FPS" : Tr( "Off", u8"Aus" )),
                ImGuiSliderFlags_AlwaysClamp );
            ImGui::EndDisabled();
            if ( fpsLimitEnabled ) {
                settings.FpsLimitLastEnabled = settings.FpsLimit;
            }
            ImGui::EndDisabled();
            ImGui::SetItemTooltip( settings.EnableVSync
                ? Tr( "VSync controls the output frame rate.", u8"VSync steuert die ausgegebene Bildrate." )
                : (fpsLimitEnabled
                    ? Tr( "Sets the maximum rendered frames per second.", u8"Legt die maximal gerenderten Bilder pro Sekunde fest." )
                    : Tr( "Enable the FPS limiter to select a frame-rate limit.", u8"Aktiviere das FPS-Limit, um eine Bildrate auszuw\u00E4hlen." )) );

            ImText( Tr( "Surface Detail", u8"Oberfl\u00E4chendetails" ), { buttonWidth.x - ImGui::GetFrameHeight() - style.ItemSpacing.x, buttonWidth.y } ); ImGui::SameLine();
            if ( ImGui::Checkbox( "##Enable Surface Detail", &settings.AllowNormalmaps ) ) {
                Engine::GAPI->UpdateTextureMaxSize();
            }
            ImGui::SetItemTooltip( "%s", Tr( "Adds fine depth and structure to surfaces.", u8"F\u00FCgt Oberfl\u00E4chen feine Tiefe und Struktur hinzu." ) );
            ImGui::SameLine();

            const std::array<std::pair<const char*, bool>, 2> surfaceDetailModes = {{
                {Tr( "Normal Maps", u8"Normalmaps" ), false},
                {"Parallax", true},
            }};
            ImGui::BeginDisabled( !settings.AllowNormalmaps );
            ImGui::SetNextItemWidth( standardComboWidth );
            if ( ImComboBoxC( "##SurfaceDetailMode", surfaceDetailModes, &settings.EnableParallaxOcclusionMapping, [] {} ) ) {
                ImGui::EndCombo();
            }
            ImGui::EndDisabled();
            ImGui::SetItemTooltip( "%s", Tr( "Changes how surface depth is represented.", u8"Bestimmt, wie r\u00E4umliche Oberfl\u00E4chendetails dargestellt werden." ) );

            ImText( Tr( "HDR Tone Mapping", u8"HDR-Tonemapping" ), { buttonWidth.x - ImGui::GetFrameHeight() - style.ItemSpacing.x, buttonWidth.y } ); ImGui::SameLine();
            if ( CoupledStrengthCheckbox( "##Enable HDR Tone Mapping", "HDRToneMapStrength",
                    &settings.EnableHDR, &settings.HDRToneMapStrength, 1.0f ) ) {
                shadersToReload |= ShaderCategory::Tonemapping;
            }
            ImGui::SetItemTooltip( "%s", Tr( "Preserves detail in bright areas and balances scene exposure.", u8"Erh\u00E4lt Details in hellen Bereichen und gleicht die Belichtung aus." ) );
            ImGui::SameLine();
            ImGui::SetNextItemWidth( standardComboWidth );
            const bool hdrEnabledBeforeSlider = settings.EnableHDR;
            if ( CoupledStrengthSlider( "##HDRToneMapStrength", "HDRToneMapStrength",
                    &settings.EnableHDR, &settings.HDRToneMapStrength )
                && hdrEnabledBeforeSlider != settings.EnableHDR ) {
                shadersToReload |= ShaderCategory::Tonemapping;
            }
            ImGui::SetItemTooltip( "%s", Tr( "Adjusts highlight compression and exposure balancing.", u8"Regelt die Zeichnung heller Bereiche und den Belichtungsausgleich." ) );

            bool waterReflections = settings.EnableSSR;
            ImText( Tr( "Water Reflections", u8"Wasserreflektionen" ), { buttonWidth.x - ImGui::GetFrameHeight() - style.ItemSpacing.x, buttonWidth.y } ); ImGui::SameLine();
            if ( CoupledStrengthCheckbox( "##Enable Water Reflections", "WaterReflectionsStrength",
                    &waterReflections, &settings.SSRStrength, 1.0f ) ) {
                settings.EnableSSR = waterReflections;
                shadersToReload |= ShaderCategory::Water;
            }
            ImGui::SetItemTooltip( "%s", Tr( "Enables reflections on water surfaces.", u8"Aktiviert Reflexionen auf Wasseroberfl\u00E4chen." ) );
            ImGui::SameLine();
            ImGui::SetNextItemWidth( standardComboWidth );
            const bool waterReflectionsBeforeSlider = waterReflections;
            if ( CoupledStrengthSlider( "##WaterReflectionsStrength", "WaterReflectionsStrength",
                    &waterReflections, &settings.SSRStrength ) ) {
                settings.EnableSSR = waterReflections;
                if ( waterReflectionsBeforeSlider != waterReflections ) {
                    shadersToReload |= ShaderCategory::Water;
                }
            }
            ImGui::SetItemTooltip( "%s", Tr( "Makes water reflections weaker or stronger.", u8"Macht Wasserreflexionen schw\u00E4cher oder st\u00E4rker." ) );

            ImText( Tr( "Rain Rendering", u8"Regendarstellung" ), { buttonWidth.x - ImGui::GetFrameHeight() - style.ItemSpacing.x, buttonWidth.y } ); ImGui::SameLine();
            ImGui::Checkbox( "##Enable Rain", &settings.EnableRain );
            ImGui::SetItemTooltip( "%s", Tr( "Enables rain and rain effects.", u8"Aktiviert Regen und Regeneffekte." ) );
            ImGui::Dummy( ImVec2( 1.0f, ImGui::GetFrameHeight() ) );
            ImGui::EndGroup();
        }

        ImGui::Separator();

        {
            ImGui::BeginGroup();
            ImGui::PushItemWidth( controlWidth );

            ImText( Tr( "Texture Quality", u8"Texturqualit\u00E4t" ), buttonWidth ); ImGui::SameLine();
            const std::array<std::pair<const char*, int>, 6> QualityOptions = {{
                { Tr( "Very Low", u8"Sehr niedrig" ), static_cast<int>(TX_QUALITY::VeryLow) },
                { Tr( "Low", u8"Niedrig" ), static_cast<int>(TX_QUALITY::Low) },
                { Tr( "Medium", u8"Mittel" ), static_cast<int>(TX_QUALITY::Medium) },
                { Tr( "High", u8"Hoch" ), static_cast<int>(TX_QUALITY::High) },
                { Tr( "Very High", u8"Sehr hoch" ), static_cast<int>(TX_QUALITY::VeryHigh) },
                { Tr( "Extreme", u8"Extrem" ), static_cast<int>(TX_QUALITY::MAX) }, // TODO: this should depend on the GPU capabilities like in the original game
            }};

            if (settings.textureMaxSize > QualityOptions.back().second) {
                settings.textureMaxSize = QualityOptions.back().second;
                Engine::GAPI->UpdateTextureMaxSize();
            }
            if (settings.textureMaxSize < QualityOptions.front().second) {
                settings.textureMaxSize = QualityOptions.front().second;
                Engine::GAPI->UpdateTextureMaxSize();
            }

            if (ImComboBoxC("##TextureQuality", QualityOptions, &settings.textureMaxSize, []{
                Engine::GAPI->UpdateTextureMaxSize();
            } ))
            {
                ImGui::EndCombo();
            }
            ImGui::SetItemTooltip( "%s", Tr( "Higher settings keep textures sharper and more detailed.", u8"H\u00F6here Werte zeigen Texturen sch\u00E4rfer und detailreicher." ) );

            const std::array<std::pair<const char*, int>, 4> shadowMapSizes = {{
                {Tr( "Low", u8"Niedrig" ), 1024},
                {Tr( "Medium", u8"Mittel" ), 2048},
                {Tr( "High", u8"Hoch" ), 4096},
                {Tr( "Extreme", u8"Extrem" ), 8192},
            }};

            settings.EnableShadows = true;
            settings.ShadowMapSize = NormalizeShadowMapSize( settings.ShadowMapSize );
            settings.PointlightShadowMapSize = NormalizePointlightShadowMapSize( settings.PointlightShadowMapSize );
            ImText( Tr( "Shadow Quality", u8"Schattenqualit\u00E4t" ), buttonWidth ); ImGui::SameLine();
            if ( ImComboBoxC( "##ShadowQuality", shadowMapSizes, &settings.ShadowMapSize, [&settings, &shadersToReload]{
                settings.PointlightShadowMapSize = PointlightShadowSizeForWorldShadowSize( settings.ShadowMapSize );
                shadersToReload |= ShaderCategory::LightsAndShadows;
            } ) ) {
                ImGui::EndCombo();
            }
            ImGui::SetItemTooltip( "%s", Tr( "Higher settings preserve finer shadow detail.", u8"H\u00F6here Werte zeigen feinere Schattendetails." ) );

            ImText( Tr( "Shadow Softness", u8"Schattenweichheit" ), buttonWidth ); ImGui::SameLine();
            SliderNormalizedUiStrength( "##ShadowSoftness", &settings.ShadowSoftness );
            ImGui::SetItemTooltip( "%s", Tr( "Makes shadow edges sharper or softer.", u8"Macht Schattenkanten h\u00E4rter oder weicher." ) );

            const bool ambientOcclusionAvailable = !FeatureLevel10Compatibility;
            bool ambientOcclusionEnabled = ambientOcclusionAvailable && settings.AoMode == AOMode::AO_XEGTAO;
            ImText( Tr( "Ambient Occlusion", u8"Umgebungsverdeckung" ), { buttonWidth.x - ImGui::GetFrameHeight() - style.ItemSpacing.x, buttonWidth.y } ); ImGui::SameLine();
            ImGui::BeginDisabled( !ambientOcclusionAvailable );
            if ( CoupledStrengthCheckbox( "##Enable Ambient Occlusion", "AOStrength",
                    &ambientOcclusionEnabled, &settings.AOStrength, 1.0f ) ) {
                settings.AoMode = ambientOcclusionEnabled ? AOMode::AO_XEGTAO : AOMode::AO_NONE;
            }
            ImGui::EndDisabled();
            ImGui::SetItemTooltip( "%s", Tr( "Adds natural contact shading where surfaces meet.", u8"F\u00FCgt nat\u00FCrliche Abdunklung an \u00DCberg\u00E4ngen und in Vertiefungen hinzu." ) );
            ImGui::SameLine();
            ImGui::BeginDisabled( !ambientOcclusionAvailable );
            ImGui::SetNextItemWidth( standardComboWidth );
            if ( CoupledStrengthSlider( "##AOStrength", "AOStrength",
                    &ambientOcclusionEnabled, &settings.AOStrength ) ) {
                settings.AoMode = ambientOcclusionEnabled ? AOMode::AO_XEGTAO : AOMode::AO_NONE;
            }
            ImGui::EndDisabled();
            ImGui::SetItemTooltip( "%s", Tr( "Makes contact shading lighter or darker.", u8"Macht diese Abdunklung heller oder dunkler." ) );

            ImText( Tr( "World Draw Distance", u8"Weltsichtweite" ), buttonWidth ); ImGui::SameLine();
            ImGui::SliderInt( "##SectionDrawRadius", &settings.SectionDrawRadius, 1, 10, "%d", ImGuiSliderFlags_::ImGuiSliderFlags_ClampOnInput );
            ImGui::SetItemTooltip( "%s", Tr( "Higher values show terrain and buildings from farther away.", u8"H\u00F6here Werte zeigen Gel\u00E4nde und Geb\u00E4ude aus gr\u00F6\u00DFerer Entfernung." ) );

            int objectDrawDistance = ObjectDrawDistanceMetersToUi( settings.OutdoorSmallVobDrawRadius );
            ImText( Tr( "Object Draw Distance", u8"Objektsichtweite" ), buttonWidth ); ImGui::SameLine();
            if ( ImGui::SliderInt( "##OutdoorSmallVobDrawRadius", &objectDrawDistance, OBJECT_DRAW_DISTANCE_UI_MIN, OBJECT_DRAW_DISTANCE_UI_MAX, "%d", ImGuiSliderFlags_::ImGuiSliderFlags_ClampOnInput ) ) {
                settings.OutdoorSmallVobDrawRadius = ObjectDrawDistanceUiToMeters( objectDrawDistance );
            }
            ImGui::SetItemTooltip( "%s", Tr( "Higher values show small objects and vegetation from farther away.", u8"H\u00F6here Werte zeigen kleine Objekte und Vegetation aus gr\u00F6\u00DFerer Entfernung." ) );
            ImGui::PopItemWidth();
            ImGui::EndGroup();
        }

        ImGui::SameLine();

        {
            ImGui::BeginGroup();
            ImText( Tr( "Godrays", u8"Lichtstrahlen" ), { buttonWidth.x - ImGui::GetFrameHeight() - style.ItemSpacing.x, buttonWidth.y } ); ImGui::SameLine();
            if ( CoupledStrengthCheckbox( "##Enable Godrays", "GodRayStrength",
                    &settings.EnableGodRays, &settings.GodRayStrength, 1.0f ) ) {
                shadersToReload |= ShaderCategory::Other;
            }
            ImGui::SetItemTooltip( "%s", Tr( "Adds sunlight beams when the sun is partially blocked by trees, buildings, or terrain.", u8"F\u00FCgt sichtbare Sonnenstrahlen hinter B\u00E4umen, Geb\u00E4uden und Gel\u00E4nde hinzu." ) );
            ImGui::SameLine();
            ImGui::SetNextItemWidth( standardComboWidth );
            const bool godRaysBeforeSlider = settings.EnableGodRays;
            if ( CoupledStrengthSlider( "##GodrayStrength", "GodRayStrength",
                    &settings.EnableGodRays, &settings.GodRayStrength )
                && godRaysBeforeSlider != settings.EnableGodRays ) {
                shadersToReload |= ShaderCategory::Other;
            }
            ImGui::SetItemTooltip( "%s", Tr( "Makes sunlight beams weaker or stronger.", u8"Macht Sonnenstrahlen schw\u00E4cher oder st\u00E4rker." ) );

#if defined(BUILD_GOTHIC_2_6_fix) || (defined(BUILD_GOTHIC_1_08k) && !defined(BUILD_1_12F))
#if defined(BUILD_GOTHIC_1_08k) && !defined(BUILD_1_12F)
            if ( haveWindAnimations )
#endif
            {
                bool windEffects = settings.WindQuality != GothicRendererSettings::EWindQuality::WIND_QUALITY_NONE;
                ImText( Tr( "Wind Effects", u8"Windeffekte" ), { buttonWidth.x - ImGui::GetFrameHeight() - style.ItemSpacing.x, buttonWidth.y } ); ImGui::SameLine();
                if ( CoupledStrengthCheckbox( "##Enable Wind Effects", "WindEffectsStrength",
                        &windEffects, &settings.GlobalWindStrength, 1.0f ) ) {
                    settings.WindQuality = windEffects
                        ? GothicRendererSettings::EWindQuality::WIND_QUALITY_ADVANCED
                        : GothicRendererSettings::EWindQuality::WIND_QUALITY_NONE;
                    shadersToReload |= ShaderCategory::Other;
                }
                ImGui::SetItemTooltip( "%s", Tr( "Enables animated wind movement for trees, grass, and wheat.", u8"Aktiviert Windbewegungen f\u00FCr B\u00E4ume, Gras und Getreide." ) );
                ImGui::SameLine();
                ImGui::SetNextItemWidth( standardComboWidth );
                const bool windEffectsBeforeSlider = windEffects;
                if ( CoupledStrengthSlider( "##WindEffectsStrength", "WindEffectsStrength",
                        &windEffects, &settings.GlobalWindStrength ) ) {
                    settings.WindQuality = windEffects
                        ? GothicRendererSettings::EWindQuality::WIND_QUALITY_ADVANCED
                        : GothicRendererSettings::EWindQuality::WIND_QUALITY_NONE;
                    if ( windEffectsBeforeSlider != windEffects ) {
                        shadersToReload |= ShaderCategory::Other;
                    }
                }
                ImGui::SetItemTooltip( "%s", Tr( "Makes vegetation move more or less strongly in the wind.", u8"L\u00E4sst Vegetation sich schw\u00E4cher oder st\u00E4rker im Wind bewegen." ) );

                ImText( Tr( "Vegetation Displacement", u8"Vegetationsverdr\u00E4ngung" ), buttonWidth ); ImGui::SameLine();
                SliderNormalizedUiStrength( "##HeroAffectsObjectsRadius", &settings.HeroAffectsObjectsRadius );
                ImGui::SetItemTooltip( "%s", Tr( "Sets how far nearby vegetation is pushed aside.", u8"Bestimmt, wie weit Vegetation in der N\u00E4he verdr\u00E4ngt wird." ) );
            }
#endif //BUILD_GOTHIC_2_6_fix

            float depthOfFieldStrength = settings.DoFBokehRadius / 3.5f;
            ImText( Tr( "Depth of Field", u8"Tiefenunsch\u00E4rfe" ), { buttonWidth.x - ImGui::GetFrameHeight() - style.ItemSpacing.x, buttonWidth.y } ); ImGui::SameLine();
            CoupledStrengthCheckbox( "##Enable Depth of Field", "DepthOfFieldBlurStrength",
                &settings.EnableDoF, &depthOfFieldStrength, 1.0f );
            ImGui::SetItemTooltip( "%s", Tr( "Blurs out-of-focus parts of the scene.", u8"Stellt Bereiche au\u00DFerhalb des Fokus unscharf dar." ) );
            ImGui::SameLine();
            ImGui::SetNextItemWidth( standardComboWidth );
            CoupledStrengthSlider( "##DepthOfFieldBlurStrength", "DepthOfFieldBlurStrength",
                &settings.EnableDoF, &depthOfFieldStrength );
            settings.DoFBokehRadius = depthOfFieldStrength * 3.5f;
            ImGui::SetItemTooltip( "%s", Tr( "Makes out-of-focus areas clearer or more blurred.", u8"Macht unscharfe Bereiche klarer oder verschwommener." ) );

            ImText( Tr( "Dynamic Clouds", u8"Dynamische Wolken" ), { buttonWidth.x - ImGui::GetFrameHeight() - style.ItemSpacing.x, buttonWidth.y } ); ImGui::SameLine();
            ImGui::Checkbox( "##Enable Dynamic Clouds", &settings.EnableDynamicClouds );
            ImGui::SetItemTooltip( "%s", Tr( "Enables moving low cloud fields.", u8"Aktiviert bewegte tiefe Wolkenfelder." ) );

            ImText( Tr( "Ambient Particles", u8"Atmosph\u00E4renpartikel" ), { buttonWidth.x - ImGui::GetFrameHeight() - style.ItemSpacing.x, buttonWidth.y } ); ImGui::SameLine();
            ImGui::Checkbox( "##Enable Ambient Particles", &settings.EnableAmbientParticles );
            ImGui::SetItemTooltip( "%s", Tr( "Shows atmospheric particles in the environment.", u8"Zeigt atmosph\u00E4rische Partikel in der Umgebung." ) );

            ImText( "Screen-Space GI", { buttonWidth.x - ImGui::GetFrameHeight() - style.ItemSpacing.x, buttonWidth.y } ); ImGui::SameLine();
            if ( CoupledStrengthCheckbox( "##Enable Screen-Space GI", "ScreenSpaceGIStrength",
                    &settings.EnableScreenSpaceGI, &settings.ScreenSpaceGIStrength, 1.0f ) ) {
                shadersToReload |= ShaderCategory::Other;
            }
            ImGui::SetItemTooltip( "%s", Tr( "Adds soft indirect light reflected from nearby surfaces throughout the scene.", u8"F\u00FCgt in der gesamten Szene weiches, von nahen Oberfl\u00E4chen reflektiertes Licht hinzu." ) );
            ImGui::SameLine();
            ImGui::SetNextItemWidth( standardComboWidth );
            const bool screenSpaceGIBeforeSlider = settings.EnableScreenSpaceGI;
            if ( CoupledStrengthSlider( "##ScreenSpaceGIStrength", "ScreenSpaceGIStrength",
                    &settings.EnableScreenSpaceGI, &settings.ScreenSpaceGIStrength )
                && screenSpaceGIBeforeSlider != settings.EnableScreenSpaceGI ) {
                shadersToReload |= ShaderCategory::Other;
            }
            ImGui::SetItemTooltip( "%s", Tr( "Makes indirect lighting weaker or stronger.", u8"Macht die indirekte Beleuchtung schw\u00E4cher oder st\u00E4rker." ) );

            ImText( Tr( "Contact Shadows", u8"Kontaktschatten" ), { buttonWidth.x - ImGui::GetFrameHeight() - style.ItemSpacing.x, buttonWidth.y } ); ImGui::SameLine();
            if ( ImGui::Checkbox( "##Enable Contact Shadows", &settings.EnableContactShadows ) ) {
                shadersToReload |= ShaderCategory::Other;
            }
            ImGui::SetItemTooltip( "%s", Tr( "Adds small shadows where objects meet nearby indoor surfaces. Only affects indoor areas.", u8"F\u00FCgt kleine Schatten an nahen Innenraum-Kontaktstellen hinzu. Wirkt nur indoor." ) );
            ImGui::EndGroup();
        }

        ImGui::Spacing();
        const float footerButtonWidth = (ImGui::GetContentRegionAvail().x - style.ItemSpacing.x) / 2.0f;
        const bool cancelled = ImGui::Button( Tr( "Cancel", u8"Abbrechen" ), ImVec2( footerButtonWidth, footerHeight ) );
        ImGui::SetItemTooltip( "%s", Tr( "Discard changes made since opening the F11 menu.", u8"Verwirft alle seit dem \u00D6ffnen des F11-Men\u00FCs vorgenommenen \u00C4nderungen." ) );
        ImGui::SameLine();
        const bool saved = ImGui::Button( Tr( "Save Settings", u8"Einstellungen speichern" ), ImVec2( footerButtonWidth, footerHeight ) );
        ImGui::SetItemTooltip( "%s", Tr( "Saves the renderer settings globally.", u8"Speichert die Renderer-Einstellungen global." ) );
        if ( cancelled ) {
            CancelSettingsEdit();
            shadersToReload = ShaderCategory::None;
            Engine::GraphicsEngine->OnUIEvent( BaseGraphicsEngine::UI_ClosedSettings );
        } else if ( saved ) {
            CommitSettingsEdit();
            Engine::GraphicsEngine->OnUIEvent( BaseGraphicsEngine::UI_ClosedSettings );
            Engine::GAPI->SaveRendererGlobalSettings( settings, MENU_SETTINGS_FILE );
            Engine::GAPI->SaveMenuSettings( MENU_SETTINGS_FILE );
        }
    }
    ImGui::End();
    ImGui::PopStyleVar( 3 );
    if ( m_centerSettingsWindowFrames > 0 ) {
        --m_centerSettingsWindowFrames;
    }

    if ( shadersToReload != ShaderCategory::None ) {
        Engine::GraphicsEngine->ReloadShaders( shadersToReload );
    }
}
