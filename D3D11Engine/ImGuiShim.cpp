#include "ImGuiShim.h"
#include "GSky.h"
#include "D3D11PfxRenderer.h"
#include "D3D11PFX_FSR3.h"
#include <windowsx.h>

#include "zCParser.h"
#include "zCOption.h"
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
        if ( !window || !Engine::GraphicsEngine || !Engine::GAPI
            || !GetClientRect( window, &clientRect ) ) {
            return false;
        }

        clientWidth = static_cast<float>( std::max<LONG>( 1, clientRect.right - clientRect.left ) );
        clientHeight = static_cast<float>( std::max<LONG>( 1, clientRect.bottom - clientRect.top ) );

        float contentOffsetX = 0.0f;
        float contentOffsetY = 0.0f;
        const INT2 backbuffer = Engine::GraphicsEngine->GetBackbufferResolution();
        const auto& settings = Engine::GAPI->GetRendererState().RendererSettings;
        if ( settings.StretchWindow && backbuffer.x > 0 && backbuffer.y > 0 ) {
            const float renderAspect = static_cast<float>( backbuffer.x ) / static_cast<float>( backbuffer.y );
            const float clientAspect = clientWidth / clientHeight;
            if ( std::abs( renderAspect - clientAspect ) > 0.001f ) {
                if ( clientAspect > renderAspect ) {
                    const float fittedWidth = clientHeight * renderAspect;
                    contentOffsetX = ( clientWidth - fittedWidth ) * 0.5f;
                    clientWidth = fittedWidth;
                } else {
                    const float fittedHeight = clientWidth / renderAspect;
                    contentOffsetY = ( clientHeight - fittedHeight ) * 0.5f;
                    clientHeight = fittedHeight;
                }
            }
        }
        const float monitorDpiScale = std::clamp(
            ImGui_ImplWin32_GetDpiScaleForHwnd( window ), 0.5f, 4.0f );
        uiScale = std::max( 0.01f,
            std::min( clientWidth / 1920.0f, clientHeight / 1080.0f ) * monitorDpiScale );

        if ( cursorPos ) {
            if ( !GetCursorPos( cursorPos ) || !ScreenToClient( window, cursorPos ) ) {
                return false;
            }
            cursorPos->x -= static_cast<LONG>( std::lround( contentOffsetX ) );
            cursorPos->y -= static_cast<LONG>( std::lround( contentOffsetY ) );
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
        // Keep alternating section rows below FrameBg so empty checkboxes remain visible.
        colors[ImGuiCol_TableRowBg] = ImVec4( 0.065f, 0.069f, 0.076f, 0.82f );
        colors[ImGuiCol_TableRowBgAlt] = ImVec4( 0.090f, 0.096f, 0.106f, 0.82f );
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

    bool SliderNormalizedUiStrength(
        const char* label, float* value, bool mutedTicks = false, const char* displayText = nullptr )
    {
        const std::array<float, 11> levels = {
            0.0f, 0.2f, 0.4f, 0.6f, 0.8f, 1.0f,
            1.2f, 1.4f, 1.6f, 1.8f, 2.0f
        };
        int index = FindNearestStepIndex( *value, levels.data(), static_cast<int>(levels.size()) );
        *value = levels[index];
        if ( SliderSteppedIndex( label, &index, 10, true, 5, displayText, mutedTicks ) ) {
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

    bool MenuCheckbox( const char* label, bool* value )
    {
        // Use the existing neutral border palette consistently across the F11 menu.
        ImGui::PushStyleVar( ImGuiStyleVar_FrameBorderSize, 1.0f );
        const bool changed = ImGui::Checkbox( label, value );
        ImGui::PopStyleVar();
        return changed;
    }

    void ResetStrengthControlMemories()
    {
        StrengthControlMemories.clear();
    }

    bool CoupledStrengthCheckbox(
        const char* checkboxLabel, const char* stateKey, bool* enabled,
        float* normalizedValue, float defaultValue )
    {
        if ( !MenuCheckbox( checkboxLabel, enabled ) ) {
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
    OutputWindow = Window;
    ImGui_ImplWin32_Init( OutputWindow );
    ImGui_ImplDX11_Init( device.Get(), context.Get() );

    Initiated = true;

    std::vector<DisplayModeInfo> modes;
    Engine::GraphicsEngine->GetDisplayModeList( &modes );
    Resolutions.clear();
    for ( auto it = modes.rbegin(); it != modes.rend(); ++it ) {
        std::string s = std::to_string( (*it).Width ) + "x" + std::to_string( (*it).Height );
        Resolutions.emplace_back( std::make_pair(INT2((*it).Width, (*it).Height), s) );
    }

    ImFontConfig config = { };
    config.MergeMode = false;
    const auto path = std::filesystem::current_path();
    const auto fontpath = path / "system" / "GD3D11" / "Fonts" / "Lato-Semibold.ttf";

    io.Fonts->AddFontFromFileTTF( fontpath.string().c_str(), 20.0f, &config );}


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
    if ( !Initiated || !Engine::GraphicsEngine || !Engine::GAPI ) {
        return;
    }

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();

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
    static int beginFrameFn = -1;
    static int endFrameFn = -1;
    static int retryFindFuncs = 121;

    zCParser* parser = zCParser::GetParser();

    if ( parser && retryFindFuncs > 120 ) {
        if ( beginFrameFn == -1 ) { beginFrameFn = parser->GetIndex( GDX_IMGUI_BEGINFRAME ); }
        if ( endFrameFn == -1 ) { endFrameFn = parser->GetIndex( GDX_IMGUI_ENDFRAME ); }
        retryFindFuncs = 0;
    }

    if ( !parser || beginFrameFn == -1 || endFrameFn == -1 ) {
        retryFindFuncs++;
    }

    LibShowBlockingThisFrame = false;
    LibShowNonBlockingThisFrame = false;
    if ( parser && beginFrameFn != -1 ) {
        parser->CallFunc( beginFrameFn );
    }

    auto oldSettings = Engine::GAPI->GetRendererState().RendererSettings;
    const bool settingsWasVisible = SettingsVisible;
    if ( settingsWasVisible ) {
        RenderSettingsWindow();
    }

    if ( memcmp( &oldSettings, &Engine::GAPI->GetRendererState().RendererSettings, sizeof( GothicRendererSettings ) ) != 0 ) {
        auto& currentSettings = Engine::GAPI->GetRendererState().RendererSettings;
        SyncGraphicsPresetSelection( currentSettings );
        if ( FeatureLevel10Compatibility ) {
            ApplyFeatureLevel10Downgrades( currentSettings );
        }
        if ( settingsWasVisible ) {
            m_settingsSavePending = true;
        }
    }
    if ( m_settingsSavePending && ( !SettingsVisible || !ImGui::IsAnyItemActive() ) ) {
        auto& currentSettings = Engine::GAPI->GetRendererState().RendererSettings;
        Engine::GAPI->SaveRendererGlobalSettings( currentSettings, MENU_SETTINGS_FILE );
        Engine::GAPI->SaveMenuSettings( MENU_SETTINGS_FILE );
        m_settingsSavePending = false;
    }
    if ( GetBlockGameInput() != m_lastFrameBlockGameInput ) {
        m_lastFrameBlockGameInput = GetBlockGameInput();
        D3D11GraphicsEngine::UpdateShouldBlockGameInput();
    }

    ImGui::Render();
    ImGui_ImplDX11_RenderDrawData( ImGui::GetDrawData() );

    if ( parser && endFrameFn != -1 ) {
        parser->CallFunc( endFrameFn );
    }
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
    if ( msg == WM_KILLFOCUS && Initiated ) {
        ImGuiIO& io = ImGui::GetIO();
        io.ClearInputKeys();
        io.AddMousePosEvent( -FLT_MAX, -FLT_MAX );
        for ( int i = 0; i < ImGuiMouseButton_COUNT; i++ ) {
            io.AddMouseButtonEvent( i, false );
            io.MouseDown[i] = false;
        }
    }

    if ( Initiated && GetIsActive() )
    {
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
    if ( !Engine::GraphicsEngine ) {
        return;
    }

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
    s.NumShadowCascades = s.GetStoredShadowCascadeCount();
    if ( s.AntiAliasingMode == GothicRendererSettings::AA_FSR3
        && s.Upscaler == GothicRendererSettings::UPSCALER_FSR_3 ) {
        s.AntiAliasingMode = GothicRendererSettings::AA_SMAA;
        s.Upscaler = GothicRendererSettings::UPSCALER_DEFAULT;
        s.ResolutionScalePercent = 100;
        s.SharpenFactor = 0.2f;
    }
    s.AoMode = AOMode::AO_NONE;
    s.NormalizeGodRayMode( true );
    if ( s.CSMShadowKernel == GothicRendererSettings::E_ShadowKernelQuality::SHADOW_KERNEL_PCSS ) {
        // The FL10/atlas shader variant intentionally has no PCSS path.
        s.CSMShadowKernel = GothicRendererSettings::E_ShadowKernelQuality::SHADOW_KERNEL_PCF_MEDIUM;
    }
    const int activeCascades = s.GetEffectiveShadowCascadeCount();
    if ( activeCascades >= 2 ) {
        s.DebugSettings.ShadowCascades.Lambda = D3D11ShadowMap::lambdaBiasTable[activeCascades].lambda;
        s.DebugSettings.ShadowCascades.Bias = D3D11ShadowMap::lambdaBiasTable[activeCascades].bias;
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
    bool ShadowQualityMatchesProfile( const GothicRendererSettings& settings ) {
        GothicRendererSettings profile = settings;
        profile.ApplyShadowQualitySettings();

        return settings.EnableShadows == profile.EnableShadows
            && settings.ShadowMapSize == profile.ShadowMapSize
            && settings.CSMShadowKernel == profile.CSMShadowKernel
            && settings.NumShadowCascades == profile.NumShadowCascades
            && std::abs( settings.WorldShadowRangeScale - profile.WorldShadowRangeScale ) < 0.0001f
            && settings.ShadowCascadePCFLimit == profile.ShadowCascadePCFLimit
            && std::abs( settings.ShadowSoftness - profile.ShadowSoftness ) < 0.0001f
            && std::abs( settings.PointlightShadowSoftness - profile.PointlightShadowSoftness ) < 0.0001f
            && settings.EnablePointlightShadows == profile.EnablePointlightShadows
            && settings.PointlightShadowMapSize == profile.PointlightShadowMapSize
            && settings.PointlightShadowKernel == profile.PointlightShadowKernel
            && settings.EnablePointlightDynamicCasters == profile.EnablePointlightDynamicCasters;
    }

    void ResetShadowOverridesToCurrentQuality( GothicRendererSettings& settings ) {
        settings.ApplyShadowQualitySettings();
    }

    void ResetAllAdvancedOverridesToCurrentProfile( GothicRendererSettings& settings ) {
        ResetShadowOverridesToCurrentQuality( settings );
        settings.AdvancedPerformanceOptions = false;
        settings.AdvancedWaterAnimation = true;
        settings.AdvancedNightEnhance = true;
        settings.AdvancedCityWindowTransparency = true;
        settings.AdvancedBacklitVegetation = true;
        settings.XegtaoSettings = XeGTAOConfig{};
        // Vegetation Push is an Advanced-only override. Its normal/default
        // value is enabled and is not owned by any graphics preset.
        settings.HeroAffectsObjects = true;
    }

}
struct GraphicsPresetComparable {
    int textureMaxSize;
    int ShadowQuality;
    float ShadowSoftness;
    float PointlightShadowSoftness;
    bool ShadowQualityCustom;
    int AoMode;
    bool EnableDoF;
    bool EnableDynamicClouds;
    int WindEffectsEnabled;
    bool EnableGodRays;
    bool AllowNormalmaps;
    bool EnableSSR;
    float SSRStrength;
    int GrassDetailsLevel;
    bool HeroAffectsObjects;
    bool AdvancedWaterAnimation;
    bool AdvancedNightEnhance;
    bool AdvancedCityWindowTransparency;
    bool AdvancedBacklitVegetation;
    int XegtaoQuality;
    int XegtaoDenoise;
    float XegtaoRadius;
    bool RainEffects;
    int OutdoorSmallVobDrawDistance;
    int SectionDrawRadius;
    float AOStrength;
    float DoFBokehRadius;
    float GodRayStrength;
    float WindEffectsStrength;
};

GraphicsPresetComparable MakeGraphicsPresetComparable(
    const GothicRendererSettings& s ) {
    return {
        s.textureMaxSize,
        static_cast<int>(s.ShadowQuality),
        s.ShadowSoftness,
        s.PointlightShadowSoftness,
        !ShadowQualityMatchesProfile( s ),
        static_cast<int>(s.AoMode),
        s.EnableDoF,
        s.EnableDynamicClouds,
        IsWindEffectsControlVisible() ? static_cast<int>( s.WindEffectsEnabled ) : 0,
        s.EnableGodRays,
        s.AllowNormalmaps,
        s.EnableSSR,
        s.SSRStrength,
        s.GrassDetailsLevel,
        s.HeroAffectsObjects,
        s.AdvancedWaterAnimation,
        s.AdvancedNightEnhance,
        s.AdvancedCityWindowTransparency,
        s.AdvancedBacklitVegetation,
        s.XegtaoSettings.QualityLevel,
        s.XegtaoSettings.DenoisePasses,
        s.XegtaoSettings.Radius,
        s.RainEffects,
        ObjectDrawDistanceMetersToUi(
            s.OutdoorSmallVobDrawRadius ),
        s.SectionDrawRadius,
        s.AOStrength,
        s.DoFBokehRadius,
        s.GodRayStrength,
        IsWindEffectsControlVisible() ? s.WindEffectsStrength : 0.0f,
    };
}

bool GraphicsPresetComparableEqual(
    const GraphicsPresetComparable& a,
    const GraphicsPresetComparable& b ) {
    return a.textureMaxSize == b.textureMaxSize
        && a.ShadowQuality == b.ShadowQuality
        && a.ShadowSoftness == b.ShadowSoftness
        && a.PointlightShadowSoftness == b.PointlightShadowSoftness
        && a.ShadowQualityCustom == b.ShadowQualityCustom
        && a.AoMode == b.AoMode
        && a.EnableDoF == b.EnableDoF
        && a.EnableDynamicClouds == b.EnableDynamicClouds
        && a.WindEffectsEnabled == b.WindEffectsEnabled
        && a.EnableGodRays == b.EnableGodRays
        && a.AllowNormalmaps == b.AllowNormalmaps
        && a.EnableSSR == b.EnableSSR
        && a.SSRStrength == b.SSRStrength
        && a.GrassDetailsLevel == b.GrassDetailsLevel
        && a.HeroAffectsObjects == b.HeroAffectsObjects
        && a.AdvancedWaterAnimation == b.AdvancedWaterAnimation
        && a.AdvancedNightEnhance == b.AdvancedNightEnhance
        && a.AdvancedCityWindowTransparency == b.AdvancedCityWindowTransparency
        && a.AdvancedBacklitVegetation == b.AdvancedBacklitVegetation
        && a.XegtaoQuality == b.XegtaoQuality
        && a.XegtaoDenoise == b.XegtaoDenoise
        && std::abs( a.XegtaoRadius - b.XegtaoRadius ) < 0.0001f
        && a.RainEffects == b.RainEffects
        && a.OutdoorSmallVobDrawDistance
            == b.OutdoorSmallVobDrawDistance
        && a.SectionDrawRadius == b.SectionDrawRadius
        && a.AOStrength == b.AOStrength
        && a.DoFBokehRadius == b.DoFBokehRadius
        && a.GodRayStrength == b.GodRayStrength
        && a.WindEffectsStrength == b.WindEffectsStrength;
}

void ApplyGraphicsPresets( GothicRendererSettings& s, bool applyRuntimeUpdates = true ) {
    const auto preset = s.GraphicsPreset;
    if ( preset == GothicRendererSettings::E_GraphicsPreset::GRAPHICS_CUSTOM ) {
        return;
    }

    // Presets own the quality controls displayed below the menu separator and
    // are the reset source for all Advanced overrides.
    s.EnableGodRays = true;
    s.EnableDynamicClouds = true;
    s.EnableSSR = true;
    s.SSRStrength = 1.0f;
    s.RainEffects = true;
    s.GrassDetailsLevel = 4;

    // Reset all visible effect strengths to their normalized UI defaults.
    s.AOStrength = 1.0f;
    s.GodRayStrength = 1.0f;
    s.DoFBokehRadius = 3.5f;
    if ( IsWindEffectsControlVisible() ) s.WindEffectsStrength = 1.0f;

    switch ( preset ) {
    case GothicRendererSettings::GRAPHICS_LOW:
        s.ShadowQuality = GothicRendererSettings::E_ShadowQuality::SHADOW_QUALITY_LOW;
        s.AoMode = AOMode::AO_XEGTAO;
        s.EnableDoF = false;
        s.EnableDynamicClouds = false;
        if ( IsWindEffectsControlVisible() ) s.WindEffectsEnabled = GothicRendererSettings::EWindEffectsState::ENABLED;
        s.OutdoorSmallVobDrawRadius = ObjectDrawDistanceUiToMeters( 3 );
        s.SectionDrawRadius = 3;
        s.textureMaxSize = static_cast<int>(TX_QUALITY::High);
        s.AllowNormalmaps = false;
        s.EnableSSR = true;
        s.SSRStrength = 1.0f;
        s.RainEffects = false;
        s.EnableGodRays = false;
        s.GrassDetailsLevel = 2;
        break;
    case GothicRendererSettings::GRAPHICS_MEDIUM:
        s.ShadowQuality = GothicRendererSettings::E_ShadowQuality::SHADOW_QUALITY_MEDIUM;
        s.AoMode = AOMode::AO_XEGTAO;
        s.EnableDoF = true;
        s.EnableDynamicClouds = false;
        if ( IsWindEffectsControlVisible() ) s.WindEffectsEnabled = GothicRendererSettings::EWindEffectsState::ENABLED;
        s.OutdoorSmallVobDrawRadius = ObjectDrawDistanceUiToMeters( 5 );
        s.SectionDrawRadius = 5;
        s.textureMaxSize = static_cast<int>(TX_QUALITY::MAX);
        s.AllowNormalmaps = false;
        s.EnableSSR = true;
        s.SSRStrength = 1.0f;
        s.GrassDetailsLevel = 4;
        break;
    case GothicRendererSettings::GRAPHICS_HIGH:
        s.ShadowQuality = GothicRendererSettings::E_ShadowQuality::SHADOW_QUALITY_HIGH;
        s.AoMode = AOMode::AO_XEGTAO;
        s.EnableDoF = true;
        s.EnableDynamicClouds = true;
        if ( IsWindEffectsControlVisible() ) s.WindEffectsEnabled = GothicRendererSettings::EWindEffectsState::ENABLED;
        s.OutdoorSmallVobDrawRadius = ObjectDrawDistanceUiToMeters( 7 );
        s.SectionDrawRadius = 7;
        s.textureMaxSize = static_cast<int>(TX_QUALITY::MAX);
        s.AllowNormalmaps = true;
        s.EnableSSR = true;
        s.SSRStrength = 1.0f;
        break;
    case GothicRendererSettings::GRAPHICS_VERY_HIGH:
        s.ShadowQuality = GothicRendererSettings::E_ShadowQuality::SHADOW_QUALITY_EXTREME;
        s.AoMode = AOMode::AO_XEGTAO;
        s.EnableDoF = true;
        s.EnableDynamicClouds = true;
        if ( IsWindEffectsControlVisible() ) s.WindEffectsEnabled = GothicRendererSettings::EWindEffectsState::ENABLED;
        s.OutdoorSmallVobDrawRadius = ObjectDrawDistanceUiToMeters( 9 );
        s.SectionDrawRadius = 9;
        s.textureMaxSize = static_cast<int>(TX_QUALITY::MAX);
        s.AllowNormalmaps = true;
        s.EnableSSR = true;
        s.SSRStrength = 1.0f;
        break;
    default:
        return;
    }
    s.NormalizeGodRayMode( FeatureLevel10Compatibility );
    if ( !s.EnableSSR ) s.SSRStrength = 0.0f;
    if ( s.AoMode == AOMode::AO_NONE ) s.AOStrength = 0.0f;
    if ( !s.EnableGodRays ) s.GodRayStrength = 0.0f;
    if ( !s.EnableDoF ) s.DoFBokehRadius = 0.0f;
    if ( IsWindEffectsControlVisible() && !s.AreWindEffectsEnabled() ) s.WindEffectsStrength = 0.0f;

    // Selecting a named graphics preset is a full reset boundary for all
    // Advanced overrides. The profile values become the current effective
    // values and are also the values shown by the disabled controls.
    ResetAllAdvancedOverridesToCurrentProfile( s );

    if ( FeatureLevel10Compatibility ) {
        // Preset dependency stays inside the visible AO control; display/AA settings
        // are handled by the normal hardware compatibility path outside the preset.
        s.AoMode = AOMode::AO_NONE;
        s.AOStrength = 0.0f;
    }

    if ( applyRuntimeUpdates ) {
        Engine::GAPI->UpdateTextureMaxSize();
    }
}

bool GraphicsPresetMatchesCurrentSettings( const GothicRendererSettings& settings ) {
    if ( settings.GraphicsPreset == GothicRendererSettings::GRAPHICS_CUSTOM ) {
        return false;
    }

    GothicRendererSettings expected = settings;
    expected.GraphicsPreset = settings.GraphicsPreset;
    ApplyGraphicsPresets( expected, false );
    return GraphicsPresetComparableEqual(
        MakeGraphicsPresetComparable( settings ),
        MakeGraphicsPresetComparable( expected ) );
}

void SyncGraphicsPresetSelection( GothicRendererSettings& s ) {
    if ( GraphicsPresetMatchesCurrentSettings( s ) ) {
        return;
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
        s.WindEffectsEnabled = GothicRendererSettings::WindEffectsStateOrDefault(
            static_cast<int>( s.WindEffectsEnabled ) );
        s.AoMode = GothicRendererSettings::AmbientOcclusionModeOrDefault( static_cast<int>( s.AoMode ) );
        s.GraphicsPreset = GothicRendererSettings::GraphicsPresetOrDefault(
            static_cast<int>(s.GraphicsPreset) );
        s.D3D11Language = GothicRendererSettings::D3D11LanguageOrDefault(
            static_cast<int>( s.D3D11Language ) );
        s.LimitLightIntesity = true;
        s.ShadowFilterMode = FeatureLevel10Compatibility
            ? GothicRendererSettings::E_ShadowFilterMode::SHADOW_FILTER_SIMPLE
            : GothicRendererSettings::E_ShadowFilterMode::SHADOW_FILTER_PCSS;
        s.EnableWaterAnimation = true;
        s.EnableSSS = true;
        s.SSSIntensity = 1.0f;
        s.EnableParallaxOcclusionMapping = s.AllowNormalmaps;
        // Disabled coupled controls must always display their true zero effect state.
        if ( s.AoMode == AOMode::AO_NONE ) s.AOStrength = 0.0f;
        s.NormalizeGodRayMode( FeatureLevel10Compatibility );
        if ( !s.EnableGodRays ) s.GodRayStrength = 0.0f;
        if ( !s.EnableSSR ) s.SSRStrength = 0.0f;

        if ( !s.EnableDoF ) s.DoFBokehRadius = 0.0f;
        if ( !s.AreWindEffectsEnabled() ) s.WindEffectsStrength = 0.0f;
        s.OutdoorSmallVobDrawRadius = ObjectDrawDistanceUiToMeters( ObjectDrawDistanceMetersToUi( s.OutdoorSmallVobDrawRadius ) );
        s.GrassDetailsLevel = std::clamp( s.GrassDetailsLevel, 0, 4 );
        s.ForceFOV = false;
        s.FOVHoriz = 100.0f;
        s.FOVVert = 100.0f;
    }
}

void ImGuiShim::OnSettingsOpened() {
    m_centerSettingsWindowFrames = 3;
    ResetStrengthControlMemories();
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
    // Keep labels and controls the same width. The surrounding geometry still
    // follows the existing DPI/resolution scaling path above.
    const float labelWidth = std::round( 225.0f * menuScale );
    const float controlWidth = labelWidth;
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
        // Keep the displayed preset derived from the effective F11 values,
        // including values loaded from UserSettings.ini or changed externally.
        SyncGraphicsPresetSelection( settings );
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

        const bool graphicsPresetIsCustom = !GraphicsPresetMatchesCurrentSettings( settings );
        const char* graphicsPresetPreview = Tr( "Custom", u8"Individuell" );
        if ( !graphicsPresetIsCustom ) {
            for ( const auto& preset : graphicsPresets ) {
                if ( preset.second == static_cast<int>(settings.GraphicsPreset) ) {
                    graphicsPresetPreview = preset.first;
                    break;
                }
            }
        }

        const std::string versionText = std::string( Tr( "D3D11 Version ", u8"D3D11-Version " ) ) + VERSION_NUMBER;
        const ImVec2 versionTextSize = ImGui::CalcTextSize( versionText.c_str() );

        const float topHeaderGap = style.ItemSpacing.x;
        // The spacing belongs to each pair as well: label + gap + combo must
        // occupy exactly the corresponding normal field width underneath.
        const float profileHeaderFieldWidth = std::max(
            1.0f, ( labelWidth - topHeaderGap ) * 0.5f );
        const float languageHeaderFieldWidth = std::max(
            1.0f, ( controlWidth - topHeaderGap ) * 0.5f );
        ImText( Tr( "Preset", u8"Profil" ), ImVec2( profileHeaderFieldWidth, 0.0f ) );
        ImGui::SameLine( 0.0f, topHeaderGap );

        ImGui::SetNextItemWidth( profileHeaderFieldWidth );
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
        ImGui::SetItemTooltip( "%s", Tr(
            "Applies a graphics profile and restores its Advanced values.",
            u8"Wendet ein Grafikprofil an und stellt dessen erweiterte Werte wieder her." ) );

        ImGui::SameLine( 0.0f, topHeaderGap );
        ImText( Tr( "Language", u8"Sprache" ), ImVec2( languageHeaderFieldWidth, 0.0f ) );
        ImGui::SameLine( 0.0f, topHeaderGap );
        ImGui::SetNextItemWidth( languageHeaderFieldWidth );
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
        ImGui::SetItemTooltip( "%s", Tr( "Sets the renderer language.", u8"Legt die Sprache des Renderers fest." ) );

        const float f11CloseButtonSize = ImGui::GetFrameHeight();
        const float f11CloseButtonX = std::max(
            0.0f, ImGui::GetWindowContentRegionMax().x - f11CloseButtonSize );
        const float f11VersionStartX = std::max(
            0.0f, f11CloseButtonX - topHeaderGap - versionTextSize.x );
        const char* advancedButtonText = Tr( "Advanced ...", u8"Erweitert ..." );
        const float advancedButtonWidth = 125.0f;
        const float languageComboEndX = ImGui::GetItemRectMax().x;
        const float languageComboEndLocalX = languageComboEndX - ImGui::GetWindowPos().x;
        const float advancedButtonX = std::max(
            languageComboEndLocalX,
            languageComboEndLocalX
                + ( f11VersionStartX - languageComboEndLocalX - advancedButtonWidth ) * 0.5f );
        ImGui::SameLine( 0.0f, 0.0f );
        ImGui::SetCursorPosX( advancedButtonX );
        if ( ImGui::Button( advancedButtonText, ImVec2( advancedButtonWidth, 0.0f ) ) ) {
            ImGui::OpenPopup( "##AdvancedPerformance" );
        }
        ImGui::SetItemTooltip( "%s", Tr(
            "Opens additional graphics options.",
            u8"\u00D6ffnet zus\u00E4tzliche Grafikeinstellungen." ) );
        ImGui::SameLine( 0.0f, topHeaderGap );
        ImGui::SetCursorPosX( f11VersionStartX );
        ImGui::TextDisabled( "%s", versionText.c_str() );
        ImGui::SameLine( 0.0f, topHeaderGap );
        ImGui::SetCursorPosX( f11CloseButtonX );
        if ( ImGui::Button( "X", ImVec2( f11CloseButtonSize, 0.0f ) ) ) {
            if ( Engine::GraphicsEngine ) {
                Engine::GraphicsEngine->OnUIEvent( BaseGraphicsEngine::UI_ClosedSettings );
            } else {
                SettingsVisible = false;
            }
        }
        ImGui::SetItemTooltip( "%s", Tr(
            "Closes the F11 menu.",
            u8"Schlie\u00DFt das F11-Men\u00FC." ) );
        const float advancedPopupMaxHeight = std::max( 320.0f, std::round( framebufferHeight * 0.5f ) );
        ImGui::SetNextWindowSizeConstraints(
            ImVec2( 440.0f, advancedPopupMaxHeight ),
            ImVec2( std::max( 440.0f, framebufferWidth - 32.0f ), advancedPopupMaxHeight ) );
        if ( ImGui::BeginPopup( "##AdvancedPerformance" ) ) {
            const float advancedPopupCloseButtonSize = ImGui::GetFrameHeight();
            if ( ImGui::BeginTable( "##AdvancedPerformanceHeader", 2,
                ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoSavedSettings ) ) {
                ImGui::TableSetupColumn( "##AdvancedPerformanceHeaderOptions", ImGuiTableColumnFlags_WidthStretch );
                ImGui::TableSetupColumn( "##AdvancedPerformanceHeaderClose", ImGuiTableColumnFlags_WidthFixed,
                    advancedPopupCloseButtonSize );
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex( 0 );
                const bool wasAdvancedEnabled = settings.AdvancedPerformanceOptions;
                if ( MenuCheckbox( Tr( "Enable advanced options", u8"Erweiterte Einstellungen aktivieren" ),
                    &settings.AdvancedPerformanceOptions )
                    && wasAdvancedEnabled && !settings.AdvancedPerformanceOptions ) {
                    ResetAllAdvancedOverridesToCurrentProfile( settings );
                    if ( settings.EnablePointlightShadows == GothicRendererSettings::EPointLightShadowMode::PLS_DISABLED
                        && Engine::GAPI ) {
                        Engine::GAPI->ReleasePointlightShadowResources();
                    }
                }
                ImGui::SetItemTooltip( "%s", Tr(
                    "Enables extra options; turning it off restores the profile values.",
                    u8"Aktiviert Zusatzoptionen; beim Ausschalten gelten wieder die Profilwerte." ) );
                ImGui::TableSetColumnIndex( 1 );
                if ( ImGui::Button( "X", ImVec2( advancedPopupCloseButtonSize, 0.0f ) ) ) {
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SetItemTooltip( "%s", Tr(
                    "Closes the Advanced menu.",
                    u8"Schlie\u00DFt das Men\u00FC f\u00FCr erweiterte Einstellungen." ) );
                ImGui::EndTable();
            }
            ImGui::Separator();

            if ( ImGui::BeginChild( "##AdvancedPerformanceScroll", ImVec2( 0.0f, 0.0f ),
                ImGuiChildFlags_None, ImGuiWindowFlags_AlwaysVerticalScrollbar ) ) {
            // Keep every advanced setting on a balanced two-column grid.
            const ImGuiTableFlags advancedTableFlags =
                ImGuiTableFlags_SizingStretchProp
                | ImGuiTableFlags_BordersInnerV
                | ImGuiTableFlags_RowBg
                | ImGuiTableFlags_NoSavedSettings;
            auto beginAdvancedTable = [&]( const char* id ) {
                if ( !ImGui::BeginTable( id, 2, advancedTableFlags ) ) {
                    return false;
                }
                ImGui::TableSetupColumn( "##AdvancedLabel", ImGuiTableColumnFlags_WidthStretch, 1.0f );
                ImGui::TableSetupColumn( "##AdvancedControl", ImGuiTableColumnFlags_WidthStretch, 1.0f );
                return true;
            };
            auto advancedRow = [&]( const char* label ) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex( 0 );
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted( label );
                ImGui::TableSetColumnIndex( 1 );
                ImGui::SetNextItemWidth( -1.0f );
            };

            ImGui::BeginDisabled( !settings.AdvancedPerformanceOptions );
            if ( beginAdvancedTable( "##AdvancedAdditionalEffects" ) ) {
            advancedRow( Tr( "Animate water", u8"Wasser animieren" ) );
            MenuCheckbox( "##AdvancedAnimateWater", &settings.AdvancedWaterAnimation );
            ImGui::SetItemTooltip( "%s", Tr(
                "Animates large water surfaces.",
                u8"Animiert gro\u00DFe Wasserfl\u00E4chen." ) );

            bool enhancedNightPresentation = settings.AdvancedNightEnhance;
            advancedRow( Tr( "Atmospheric night", u8"Atmosph\u00E4rische Nacht" ) );
            if ( MenuCheckbox( "##AdvancedAtmosphericNight", &enhancedNightPresentation ) ) {
                // The persisted value directly represents the checkbox:
                // true enables the atmospheric night presentation.
                settings.AdvancedNightEnhance = enhancedNightPresentation;
            }
            ImGui::SetItemTooltip( "%s", Tr(
                "Adds haze and darker distance at night.",
                u8"F\u00FCgt nachts Dunst und dunklere Ferne hinzu." ) );

            advancedRow( Tr( "Window transparency", u8"Fenstertransparenz" ) );
            MenuCheckbox( "##AdvancedCityWindowTransparency", &settings.AdvancedCityWindowTransparency );
            ImGui::SetItemTooltip( "%s", Tr(
                "Makes selected city windows transparent.",
                u8"Macht ausgew\u00E4hlte Stadtfenster durchsichtig." ) );

            advancedRow( Tr( "Backlit vegetation", u8"Gegenlicht Vegetation" ) );
            MenuCheckbox( "##AdvancedBacklitVegetation", &settings.AdvancedBacklitVegetation );
            ImGui::SetItemTooltip( "%s", Tr(
                "Highlights vegetation in backlight.",
                u8"Betont Vegetation im Gegenlicht." ) );

            advancedRow( Tr( "Vegetation interaction", u8"Vegetationsreaktion" ) );
            MenuCheckbox( "##AdvancedVegetationPush", &settings.HeroAffectsObjects );
            ImGui::SetItemTooltip( "%s", Tr(
                "Makes nearby vegetation react to passing characters.",
                u8"L\u00E4sst Pflanzen in der N\u00E4he auf vorbeigehende Figuren reagieren." ) );

            ImGui::EndTable();
            }

            ImGui::Separator();

            if ( ImGui::BeginChild( "##AdvancedCSMSection", ImVec2( 0.0f, 0.0f ),
                ImGuiChildFlags_Borders | ImGuiChildFlags_FrameStyle | ImGuiChildFlags_AutoResizeY ) ) {
            ImGui::SeparatorText( Tr( "CSM shadows", u8"CSM-Schatten" ) );
            if ( beginAdvancedTable( "##AdvancedCSMSettings" ) ) {
            bool csmShadowsEnabled = settings.EnableShadows;
            advancedRow( Tr( "Enabled", u8"Aktiv" ) );
            if ( MenuCheckbox( "##AdvancedCSMEnabled", &csmShadowsEnabled ) ) {
                settings.EnableShadows = csmShadowsEnabled;
            }
            ImGui::SetItemTooltip( "%s", Tr(
                "Enables world shadows independently of pointlight shadows.",
                u8"Aktiviert Weltschatten unabh\u00E4ngig von Punktlichtschatten." ) );

            ImGui::BeginDisabled( !csmShadowsEnabled );

            const std::array<std::pair<const char*, int>, 5> csmResolutions = {{
                { "512", 512 }, { "1024", 1024 }, { "2048", 2048 },
                { "4096", 4096 }, { "8192", 8192 },
            }};
            settings.ShadowMapSize = GothicRendererSettings::SnapCSMShadowMapSize( settings.ShadowMapSize );
            advancedRow( Tr( "Resolution", u8"Aufl\u00F6sung" ) );
            if ( ImComboBoxC( "##AdvancedCSMResolution", csmResolutions, &settings.ShadowMapSize, []{} ) ) {
                ImGui::EndCombo();
            }
            ImGui::SetItemTooltip( "%s", Tr(
                "Higher resolution sharpens world shadows but uses more VRAM.",
                u8"H\u00F6here Aufl\u00F6sung sch\u00E4rft Weltschatten, ben\u00F6tigt aber mehr Videospeicher." ) );

            const bool pcssAvailable = !FeatureLevel10Compatibility
                && !settings.DebugSettings.FeatureSet.UseShadowAtlas;
            if ( !pcssAvailable
                && settings.CSMShadowKernel == GothicRendererSettings::E_ShadowKernelQuality::SHADOW_KERNEL_PCSS ) {
                settings.CSMShadowKernel = GothicRendererSettings::E_ShadowKernelQuality::SHADOW_KERNEL_PCF_MEDIUM;
            }
            std::vector<std::pair<const char*, GothicRendererSettings::E_ShadowKernelQuality>> csmFilters = {{
                { Tr( "4-tap PCF", u8"4-Tap-PCF" ), GothicRendererSettings::E_ShadowKernelQuality::SHADOW_KERNEL_PCF_LOW },
                { Tr( "8-tap PCF", u8"8-Tap-PCF" ), GothicRendererSettings::E_ShadowKernelQuality::SHADOW_KERNEL_PCF_MEDIUM },
            }};
            if ( pcssAvailable ) {
                csmFilters.emplace_back( Tr( "PCSS", u8"PCSS" ), GothicRendererSettings::E_ShadowKernelQuality::SHADOW_KERNEL_PCSS );
            }
            advancedRow( Tr( "Filter", u8"Filter" ) );
            if ( ImComboBoxC( "##AdvancedCSMFilter", csmFilters, &settings.CSMShadowKernel, []{} ) ) {
                ImGui::EndCombo();
            }
            ImGui::SetItemTooltip( "%s", Tr(
                "Chooses how world-shadow edges are softened.",
                u8"W\u00E4hlt, wie die Kanten von Weltschatten gegl\u00E4ttet werden." ) );

            // The renderer uses the full four-cascade layout whenever CSM is
            // enabled. Keep the legacy setting field normalized as well, but
            // deliberately do not expose a redundant cascade-count control.
            const int fixedCsmCascades = settings.GetStoredShadowCascadeCount();
            settings.NumShadowCascades = fixedCsmCascades;

            advancedRow( Tr( "Softness", u8"Weichheit" ) );
            char csmSoftnessText[16] = {};
            std::snprintf( csmSoftnessText, sizeof( csmSoftnessText ), "%.1f", settings.ShadowSoftness );
            SliderNormalizedUiStrength( "##AdvancedCSMShadowSoftness", &settings.ShadowSoftness, false, csmSoftnessText );
            ImGui::SetItemTooltip( "%s", Tr(
                "Softens world-shadow edges.",
                u8"Macht die Kanten von Weltschatten weicher." ) );

            settings.WorldShadowRangeScale = std::clamp( settings.WorldShadowRangeScale, 0.5f, 2.0f );
            advancedRow( Tr( "Range", u8"Reichweite" ) );
            ImGui::SliderFloat( "##AdvancedCSMShadowRange", &settings.WorldShadowRangeScale, 0.5f, 2.0f, "%.2fx", ImGuiSliderFlags_AlwaysClamp );
            ImGui::SetItemTooltip( "%s", Tr(
                "Sets how far world shadows remain visible.",
                u8"Legt fest, wie weit Weltschatten sichtbar bleiben." ) );

            ImGui::EndDisabled();

            ImGui::EndTable();
            }
            }
            ImGui::EndChild();

            if ( ImGui::BeginChild( "##AdvancedPointlightSection", ImVec2( 0.0f, 0.0f ),
                ImGuiChildFlags_Borders | ImGuiChildFlags_FrameStyle | ImGuiChildFlags_AutoResizeY ) ) {
            ImGui::SeparatorText( Tr( "Pointlight shadows", u8"Pointlight-Schatten" ) );
            if ( beginAdvancedTable( "##AdvancedPointlightSettings" ) ) {
            // Pointlight shadows use one public on/off switch. With shadows
            // enabled, the Dynamic shadows option below selects the static or
            // animated-caster path.
            bool pointlightShadowsEnabled = settings.EnablePointlightShadows
                != GothicRendererSettings::EPointLightShadowMode::PLS_DISABLED;
            if ( pointlightShadowsEnabled ) {
                settings.EnablePointlightShadows = settings.EnablePointlightDynamicCasters
                    ? GothicRendererSettings::EPointLightShadowMode::PLS_UPDATE_DYNAMIC
                    : GothicRendererSettings::EPointLightShadowMode::PLS_STATIC_ONLY;
            }
            advancedRow( Tr( "Enabled", u8"Aktiv" ) );
            if ( MenuCheckbox( "##AdvancedPointlightEnabled", &pointlightShadowsEnabled ) ) {
                settings.EnablePointlightShadows = pointlightShadowsEnabled
                    ? ( settings.EnablePointlightDynamicCasters
                        ? GothicRendererSettings::EPointLightShadowMode::PLS_UPDATE_DYNAMIC
                        : GothicRendererSettings::EPointLightShadowMode::PLS_STATIC_ONLY )
                    : GothicRendererSettings::EPointLightShadowMode::PLS_DISABLED;
                if ( !pointlightShadowsEnabled && Engine::GAPI ) {
                    Engine::GAPI->ReleasePointlightShadowResources();
                }
            }
            ImGui::SetItemTooltip( "%s", Tr(
                "Enables pointlight shadows. Dynamic shadows can be enabled below.",
                u8"Aktiviert Punktlichtschatten. Dynamische Schatten können darunter aktiviert werden." ) );

            ImGui::BeginDisabled( !pointlightShadowsEnabled );

            const std::array<std::pair<const char*, int>, 3> pointlightResolutions = {{
                { "128", 128 }, { "256", 256 }, { "512", 512 },
            }};
            settings.PointlightShadowMapSize = GothicRendererSettings::SnapPointlightShadowMapSize( settings.PointlightShadowMapSize );
            advancedRow( Tr( "Resolution", u8"Aufl\u00F6sung" ) );
            if ( ImComboBoxC( "##AdvancedPointlightResolution", pointlightResolutions, &settings.PointlightShadowMapSize, []{
                if ( Engine::GAPI ) {
                    Engine::GAPI->ReleasePointlightShadowResources();
                }
            } ) ) {
                ImGui::EndCombo();
            }
            ImGui::SetItemTooltip( "%s", Tr(
                "Higher resolution sharpens pointlight shadows but uses more memory.",
                u8"H\u00F6here Aufl\u00F6sung sch\u00E4rft Punktlichtschatten, ben\u00F6tigt aber mehr Speicher." ) );

            const std::array<std::pair<const char*, GothicRendererSettings::E_ShadowKernelQuality>, 3> pointlightFilters = {{
                { Tr( "4-tap PCF", u8"4-Tap-PCF" ), GothicRendererSettings::E_ShadowKernelQuality::SHADOW_KERNEL_PCF_LOW },
                { Tr( "8-tap PCF", u8"8-Tap-PCF" ), GothicRendererSettings::E_ShadowKernelQuality::SHADOW_KERNEL_PCF_MEDIUM },
                { Tr( "PCSS", u8"PCSS" ), GothicRendererSettings::E_ShadowKernelQuality::SHADOW_KERNEL_PCSS },
            }};
            advancedRow( Tr( "Filter", u8"Filter" ) );
            if ( ImComboBoxC( "##AdvancedPointlightFilter", pointlightFilters, &settings.PointlightShadowKernel, []{} ) ) {
                ImGui::EndCombo();
            }
            ImGui::SetItemTooltip( "%s", Tr(
                "Chooses how pointlight-shadow edges are softened.",
                u8"W\u00E4hlt, wie die Kanten von Punktlichtschatten gegl\u00E4ttet werden." ) );

            advancedRow( Tr( "Softness", u8"Weichheit" ) );
            char pointlightSoftnessText[16] = {};
            std::snprintf( pointlightSoftnessText, sizeof( pointlightSoftnessText ), "%.1f", settings.PointlightShadowSoftness );
            SliderNormalizedUiStrength( "##AdvancedPointlightShadowSoftness", &settings.PointlightShadowSoftness, false, pointlightSoftnessText );
            ImGui::SetItemTooltip( "%s", Tr(
                "Softens shadows cast by point lights.",
                u8"Macht die Schatten von Punktlichtern weicher." ) );

            advancedRow( Tr( "Dynamic shadows", u8"Dynamische Schatten" ) );
            if ( MenuCheckbox( "##AdvancedDynamicPointlightShadows", &settings.EnablePointlightDynamicCasters ) ) {
                settings.EnablePointlightShadows = settings.EnablePointlightDynamicCasters
                    ? GothicRendererSettings::EPointLightShadowMode::PLS_UPDATE_DYNAMIC
                    : GothicRendererSettings::EPointLightShadowMode::PLS_STATIC_ONLY;
            }
            ImGui::SetItemTooltip( "%s", Tr(
                "Enables dynamic pointlight shadows for moving figures.",
                u8"Aktiviert dynamische Punktlichtschatten für bewegte Figuren." ) );

            ImGui::EndDisabled();

            ImGui::EndTable();
            }
            }
            ImGui::EndChild();

            if ( ImGui::BeginChild( "##AdvancedXeGTAOSection", ImVec2( 0.0f, 0.0f ),
                ImGuiChildFlags_Borders | ImGuiChildFlags_FrameStyle | ImGuiChildFlags_AutoResizeY ) ) {
            ImGui::SeparatorText( Tr( "Ambient Occlusion", u8"Umgebungsverdeckung" ) );

            const bool xegtaoAvailable = !FeatureLevel10Compatibility
                && settings.AoMode == AOMode::AO_XEGTAO;
            ImGui::BeginDisabled( !xegtaoAvailable );
            if ( beginAdvancedTable( "##AdvancedXeGTAOSettings" ) ) {

            const std::array<std::pair<const char*, int>, 4> xegtaoQualityOptions = {{
                { Tr( "Low", u8"Niedrig" ), 0 },
                { Tr( "Medium", u8"Mittel" ), 1 },
                { Tr( "High", u8"Hoch" ), 2 },
                { Tr( "Ultra", u8"Ultra" ), 3 },
            }};
            settings.XegtaoSettings.QualityLevel = std::clamp( settings.XegtaoSettings.QualityLevel, 0, 3 );
            advancedRow( Tr( "Quality", u8"Qualit\u00E4t" ) );
            if ( ImComboBoxC( "##AdvancedXeGTAOQuality", xegtaoQualityOptions, &settings.XegtaoSettings.QualityLevel, []{} ) ) {
                ImGui::EndCombo();
            }
            ImGui::SetItemTooltip( "%s", Tr(
                "Higher quality produces cleaner ambient occlusion but costs GPU time.",
                u8"H\u00F6here Qualit\u00E4t erzeugt sauberere Umgebungsverdeckung, kostet aber GPU-Leistung." ) );

            const std::array<std::pair<const char*, int>, 3> xegtaoDenoiseOptions = {{
                { Tr( "1 pass", u8"1 Durchlauf" ), 1 },
                { Tr( "2 passes", u8"2 Durchl\u00E4ufe" ), 2 },
                { Tr( "3 passes", u8"3 Durchl\u00E4ufe" ), 3 },
            }};
            settings.XegtaoSettings.DenoisePasses = std::clamp( settings.XegtaoSettings.DenoisePasses, 1, 3 );
            advancedRow( Tr( "AO denoise", u8"AO-Gl\u00E4ttung" ) );
            if ( ImComboBoxC( "##AdvancedXeGTAODenoise", xegtaoDenoiseOptions, &settings.XegtaoSettings.DenoisePasses, []{} ) ) {
                ImGui::EndCombo();
            }
            ImGui::SetItemTooltip( "%s", Tr(
                "More passes reduce AO noise but cost GPU time.",
                u8"Mehr Durchl\u00E4ufe verringern das AO-Rauschen, kosten aber GPU-Leistung." ) );

            settings.XegtaoSettings.Radius = std::clamp( settings.XegtaoSettings.Radius, 50.0f, 400.0f );
            advancedRow( Tr( "AO radius", u8"AO-Reichweite" ) );
            ImGui::SliderFloat( "##AdvancedXeGTAORadius", &settings.XegtaoSettings.Radius, 50.0f, 400.0f, "%.0f", ImGuiSliderFlags_AlwaysClamp );
            ImGui::SetItemTooltip( "%s", Tr(
                "Sets how far Ambient Occlusion searches for nearby shading.",
                u8"Legt fest, wie weit die Umgebungsverdeckung nach nahen Kontaktstellen sucht." ) );

            ImGui::EndTable();
            }
            ImGui::EndDisabled();
            }
            ImGui::EndChild();

            ImGui::EndDisabled();

            }
            ImGui::EndChild();
            ImGui::EndPopup();
        }
        ImGui::Separator();

        const float standardComboWidth = controlWidth;
        const float inlineToggleWidth = (buttonWidth.x - style.ItemSpacing.x) * 0.5f;
        const float inlineToggleLabelWidth = inlineToggleWidth - ImGui::GetFrameHeight() - style.ItemSpacing.x;
        const float compactAALabelTextWidth = ImGui::CalcTextSize(
            reinterpret_cast<const char*>( u8"Kantengl\u00E4ttung" ) ).x
            + style.FramePadding.x * 2.0f + 2.0f;
        const float compactAAStartGap = style.ItemSpacing.x;
        const float compactAAMethodWidth = std::min(
            100.0f,
            std::max( 1.0f, buttonWidth.x - compactAAStartGap - compactAALabelTextWidth ) );
        const float compactAALabelWidth =
            buttonWidth.x - compactAAStartGap - compactAAMethodWidth;
        const float compactAAScaleGap = style.ItemSpacing.x;
        const float compactAAValueWidth = standardComboWidth;
        
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
            ImGui::SetItemTooltip( "%s", Tr( "Changes the game's output resolution.", u8"\u00C4ndert die Ausgabeaufl\u00F6sung des Spiels." ) );

            ImText( Tr( "Display Mode *", u8"Anzeigemodus *" ), buttonWidth );
            ImGui::SetItemTooltip( "%s", Tr( "Selects fullscreen or windowed mode. * Takes effect after restarting the game.", u8"W\u00E4hlt Vollbild oder Fenstermodus. * Wird erst nach einem Neustart des Spiels wirksam." ) );
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


            ImGui::SetItemTooltip( "%s", Tr( "Selects fullscreen or windowed mode. * Takes effect after restarting the game.", u8"W\u00E4hlt Vollbild oder Fenstermodus. * Wird erst nach einem Neustart des Spiels wirksam." ) );

            const std::array<std::tuple<const char*, GothicRendererSettings::E_AntiAliasingMode, const char*>, 3> antiAliasing = {{
                {Tr( "Off", u8"Aus" ), GothicRendererSettings::E_AntiAliasingMode::AA_NONE, nullptr },
                {"SMAA", GothicRendererSettings::E_AntiAliasingMode::AA_SMAA, nullptr },
                {"FSR 3", GothicRendererSettings::E_AntiAliasingMode::AA_FSR3,
                    Tr( "Upscaling and anti-aliasing for higher performance.",
                        u8"Upscaling und Kantengl\u00E4ttung f\u00FCr mehr Leistung." )},
            }};
            {
                ImGui::PushID( "AntiAliasingSettings" );
                auto selectedMode = settings.AntiAliasingMode;
                const bool wasFSRAntiAliasing = settings.AntiAliasingMode == GothicRendererSettings::E_AntiAliasingMode::AA_FSR3;
                ImGui::PushStyleVar( ImGuiStyleVar_FramePadding, ImVec2( 1.0f, style.FramePadding.y ) );
                ImText( Tr( "Anti-Aliasing", u8"Kantengl\u00E4ttung" ), ImVec2( compactAALabelWidth, buttonWidth.y ) );
                ImGui::SameLine( 0.0f, compactAAStartGap );
                ImGui::SetNextItemWidth( compactAAMethodWidth );
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
                ImGui::SetItemTooltip( "%s", Tr( "Smooths jagged edges in the final image.", u8"Gl\u00E4ttet Treppenkanten im fertigen Bild." ) );
                ImGui::PopStyleVar();
                ImGui::SameLine( 0.0f, compactAAScaleGap );
                ImGui::SetNextItemWidth( compactAAValueWidth );
                if ( settings.Upscaler == GothicRendererSettings::UPSCALER_FSR_3 ) {
                    settings.ResolutionScalePercent = std::clamp( settings.ResolutionScalePercent, 33, 100 );
                    const std::array<std::pair<const char*, int>, 6> fsrLevels = {{
                        { Tr( "Native AA", u8"Nativ mit AA" ), 100 },
                        { Tr( "High quality", u8"Hohe Qualit\u00E4t" ), 83 },
                        { Tr( "Quality", u8"Qualit\u00E4t" ), 75 },
                        { Tr( "Balanced", u8"Ausgeglichen" ), 66 },
                        { Tr( "Performance", u8"Leistung" ), 50 },
                        { Tr( "Ultra perf.", u8"Max. Leistung" ), 33 },
                    }};
                    if ( ImComboBox( "##ResolutionScalePercent", fsrLevels, &settings.ResolutionScalePercent ) ) {
                        ImGui::EndCombo();
                    }
                    ImGui::SetItemTooltip( "%s", Tr( "Balances FSR 3 image quality and performance.", u8"Stellt das Verh\u00E4ltnis von FSR-3-Bildqualit\u00E4t und Leistung ein." ) );
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
                    ImGui::SetItemTooltip( "%s", Tr( "Changes internal resolution; higher values show more detail but cost performance.", u8"\u00C4ndert die interne Aufl\u00F6sung; h\u00F6here Werte zeigen mehr Details, kosten aber Leistung." ) );
                }
                ImGui::PopID();
            }

            ImText( Tr( "VSync", u8"VSync" ), { inlineToggleLabelWidth, buttonWidth.y } ); ImGui::SameLine();
            MenuCheckbox( "##Enable VSync", &settings.EnableVSync );
            ImGui::SetItemTooltip( "%s", Tr( "Matches frame presentation to the monitor refresh rate to prevent tearing.", u8"Passt die Bildausgabe an die Monitorfrequenz an und verhindert Bildrisse." ) );
            ImGui::SameLine();

            if ( settings.FpsLimit > 0 ) {
                settings.FpsLimitLastEnabled = std::clamp( settings.FpsLimit, 10, 300 );
            }
            bool fpsLimitEnabled = settings.FpsLimit > 0;
            ImGui::BeginDisabled( settings.EnableVSync );
            ImText( Tr( "Limit", u8"Limit" ), { inlineToggleLabelWidth, buttonWidth.y } ); ImGui::SameLine();
            if ( MenuCheckbox( "##Enable FPS Limit", &fpsLimitEnabled ) ) {
                settings.FpsLimit = fpsLimitEnabled ? settings.FpsLimitLastEnabled : 0;
            }
            ImGui::SetItemTooltip( settings.EnableVSync
                ? Tr( "VSync controls the frame rate while enabled.", u8"Bei aktivem VSync bestimmt VSync die Bildrate." )
                : Tr( "Turns the FPS limit on or off.", u8"Schaltet das FPS-Limit ein oder aus." ) );
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
                ? Tr( "VSync controls the frame rate.", u8"Bei aktivem VSync bestimmt VSync die Bildrate." )
                : (fpsLimitEnabled
                    ? Tr( "Sets the maximum frame rate.", u8"Legt die maximale Bildrate fest." )
                    : Tr( "Enable the limit to choose a maximum frame rate.", u8"Aktiviere das Limit, um eine maximale Bildrate festzulegen." )) );
            ImGui::PopItemWidth();
            ImGui::EndGroup();
        }

        ImGui::SameLine();

        {
            ImGui::BeginGroup();

            ImText( Tr( "Contrast", u8"Kontrast" ), buttonWidth ); ImGui::SameLine();
            ImGui::SetNextItemWidth( standardComboWidth );
            SliderDisplayTuningStrength( "##Contrast", &settings.GammaValue );
            ImGui::SetItemTooltip( "%s", Tr( "Adjusts image contrast.", u8"Passt den Bildkontrast an." ) );

            ImText( Tr( "Brightness", u8"Helligkeit" ), buttonWidth ); ImGui::SameLine();
            ImGui::SetNextItemWidth( standardComboWidth );
            SliderDisplayTuningStrength( "##Brightness", &settings.BrightnessValue );
            ImGui::SetItemTooltip( "%s", Tr( "Adjusts image brightness.", u8"Passt die Bildhelligkeit an." ) );

            ImText( Tr( "HDR Tonemapping", u8"HDR-Tonemapping" ), { buttonWidth.x - ImGui::GetFrameHeight() - style.ItemSpacing.x, buttonWidth.y } ); ImGui::SameLine();
            MenuCheckbox( "##Enable HDR Tone Mapping", &settings.EnableHDR );
            ImGui::SetItemTooltip( "%s", Tr( "Preserves detail in bright and dark areas through HDR tone mapping.", u8"Erh\u00E4lt durch HDR-Tonemapping Details in hellen und dunklen Bereichen." ) );
            ImText( Tr( "Rain Rendering", u8"Regendarstellung" ), { buttonWidth.x - ImGui::GetFrameHeight() - style.ItemSpacing.x, buttonWidth.y } ); ImGui::SameLine();
            MenuCheckbox( "##Enable Rain", &settings.EnableRain );
            ImGui::SetItemTooltip( "%s", Tr( "Shows rain in the game world.", u8"Zeigt Regen in der Spielwelt an." ) );
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
            ImGui::SetItemTooltip( "%s", Tr( "Higher texture quality shows finer detail but uses more video memory.", u8"H\u00F6here Texturqualit\u00E4t zeigt feinere Details, ben\u00F6tigt aber mehr Videospeicher." ) );

            const std::array<std::pair<const char*, GothicRendererSettings::E_ShadowQuality>, 5> shadowQualities = {{
                {Tr( "Off", u8"Aus" ), GothicRendererSettings::E_ShadowQuality::SHADOW_QUALITY_OFF},
                {Tr( "Low", u8"Niedrig" ), GothicRendererSettings::E_ShadowQuality::SHADOW_QUALITY_LOW},
                {Tr( "Medium", u8"Mittel" ), GothicRendererSettings::E_ShadowQuality::SHADOW_QUALITY_MEDIUM},
                {Tr( "High", u8"Hoch" ), GothicRendererSettings::E_ShadowQuality::SHADOW_QUALITY_HIGH},
                {Tr( "Extreme", u8"Extrem" ), GothicRendererSettings::E_ShadowQuality::SHADOW_QUALITY_EXTREME},
            }};

            ImText( Tr( "Shadow Quality", u8"Schattenqualit\u00E4t" ), buttonWidth ); ImGui::SameLine();
            const bool shadowQualityCustom = !ShadowQualityMatchesProfile( settings );
            const auto shadowQualityOrDefault = GothicRendererSettings::ShadowQualityOrDefault(
                static_cast<int>(settings.ShadowQuality) );
            const auto shadowQualityIt = std::find_if(
                shadowQualities.begin(), shadowQualities.end(),
                [shadowQualityOrDefault]( const auto& quality ) {
                    return quality.second == shadowQualityOrDefault;
                } );
            const char* shadowQualityPreview = shadowQualityCustom
                ? Tr( "Custom", u8"Individuell" )
                : ( shadowQualityIt != shadowQualities.end()
                    ? shadowQualityIt->first
                    : Tr( "Custom", u8"Individuell" ) );
            ImGui::SetNextItemWidth( standardComboWidth );
            if ( ImGui::BeginCombo( "##ShadowQuality", shadowQualityPreview ) ) {
                for ( const auto& quality : shadowQualities ) {
                    const bool isSelected = !shadowQualityCustom && settings.ShadowQuality == quality.second;
                    if ( ImGui::Selectable( quality.first, isSelected ) ) {
                        settings.ShadowQuality = quality.second;
                        // A direct Shadow Quality change resets only the CSM
                        // and pointlight shadow values. Other Advanced options
                        // remain untouched, so this change cannot invalidate
                        // their shader permutations.
                        ResetShadowOverridesToCurrentQuality( settings );
                        // Shadow Quality is fully represented in the runtime
                        // shadow constant buffer; no shader reload is needed.
                        // Do not synchronously flush all renderer/worker
                        // queues when a profile disables pointlight shadows.
                        // The lighting and shadow passes already skip them
                        // from the next frame; the resources remain resident
                        // but unused and are released during normal
                        // world/device teardown or reallocated if needed.
                    }
                    if ( isSelected ) {
                        ImGui::SetItemDefaultFocus();
                    }
                }
                ImGui::EndCombo();
            }
            if ( shadowQualityCustom ) {
                ImGui::SetItemTooltip( "%s", Tr(
                    "Advanced shadow settings differ from this profile.",
                    u8"Die erweiterten Schatteneinstellungen weichen von diesem Profil ab." ) );
            } else {
                ImGui::SetItemTooltip( "%s", Tr(
                    "Applies a profile for world and pointlight shadows.",
                u8"Wendet ein Profil f\u00FCr Welt- und Punktlichtschatten an." ) );
            }

            ImText( Tr( "World Draw Distance", u8"Weltsichtweite" ), buttonWidth ); ImGui::SameLine();
            ImGui::SliderInt( "##SectionDrawRadius", &settings.SectionDrawRadius, 1, 10, "%d", ImGuiSliderFlags_::ImGuiSliderFlags_ClampOnInput );
            ImGui::SetItemTooltip( "%s", Tr( "Sets how far terrain and buildings remain visible.", u8"Legt fest, wie weit Gel\u00E4nde und Geb\u00E4ude sichtbar bleiben." ) );

            int objectDrawDistance = ObjectDrawDistanceMetersToUi( settings.OutdoorSmallVobDrawRadius );
            ImText( Tr( "Object Draw Distance", u8"Objektsichtweite" ), buttonWidth ); ImGui::SameLine();
            if ( ImGui::SliderInt( "##OutdoorSmallVobDrawRadius", &objectDrawDistance, OBJECT_DRAW_DISTANCE_UI_MIN, OBJECT_DRAW_DISTANCE_UI_MAX, "%d", ImGuiSliderFlags_::ImGuiSliderFlags_ClampOnInput ) ) {
                settings.OutdoorSmallVobDrawRadius = ObjectDrawDistanceUiToMeters( objectDrawDistance );
            }
            ImGui::SetItemTooltip( "%s", Tr( "Sets how far small objects and vegetation remain visible.", u8"Legt fest, wie weit kleine Objekte und Vegetation sichtbar bleiben." ) );

            settings.GrassDetailsLevel = std::clamp( settings.GrassDetailsLevel, 0, 4 );
            ImText( Tr( "Grass Details", u8"Grasdetails" ), buttonWidth ); ImGui::SameLine();
            ImGui::SliderInt( "##GrassDetailsLevel", &settings.GrassDetailsLevel,
                0, 4, "%d", ImGuiSliderFlags_::ImGuiSliderFlags_ClampOnInput );
            ImGui::SetItemTooltip( "%s", Tr(
                "Controls grass density; higher values show more grass. Large polygons are unaffected.",
                u8"Steuert die Grasdichte; h\u00F6here Werte zeigen mehr Gras. Gro\u00DFe Polygone bleiben unver\u00E4ndert." ) );

            const bool ambientOcclusionAvailable = !FeatureLevel10Compatibility;
            bool ambientOcclusionEnabled = ambientOcclusionAvailable && settings.AoMode == AOMode::AO_XEGTAO;
            ImText( Tr( "Ambient Occlusion", u8"Umgebungsverdeckung" ), { buttonWidth.x - ImGui::GetFrameHeight() - style.ItemSpacing.x, buttonWidth.y } ); ImGui::SameLine();
            ImGui::BeginDisabled( !ambientOcclusionAvailable );
            if ( CoupledStrengthCheckbox( "##Enable Ambient Occlusion", "AOStrength",
                    &ambientOcclusionEnabled, &settings.AOStrength, 1.0f ) ) {
                settings.AoMode = ambientOcclusionEnabled ? AOMode::AO_XEGTAO : AOMode::AO_NONE;
            }
            ImGui::EndDisabled();
            ImGui::SetItemTooltip( "%s", Tr( "Adds soft shading where objects and surfaces are close together.", u8"Erzeugt weiche Abdunklungen an Kontaktstellen und zwischen nahen Objekten." ) );
            ImGui::SameLine();
            ImGui::BeginDisabled( !ambientOcclusionAvailable );
            ImGui::SetNextItemWidth( standardComboWidth );
            if ( CoupledStrengthSlider( "##AOStrength", "AOStrength",
                    &ambientOcclusionEnabled, &settings.AOStrength ) ) {
                settings.AoMode = ambientOcclusionEnabled ? AOMode::AO_XEGTAO : AOMode::AO_NONE;
            }
            ImGui::EndDisabled();
            ImGui::SetItemTooltip( "%s", Tr( "Adjusts the intensity of the ambient shading.", u8"Passt die St\u00E4rke der Umgebungsverdeckung an." ) );

            bool godRaysEnabled = settings.EnableGodRays;
            ImText( Tr( "Light Shafts", u8"Lichtstrahlen" ), { buttonWidth.x - ImGui::GetFrameHeight() - style.ItemSpacing.x, buttonWidth.y } ); ImGui::SameLine();
            if ( CoupledStrengthCheckbox( "##Enable Godrays", "GodRayStrength",
                    &godRaysEnabled, &settings.GodRayStrength, 1.0f ) ) {
                settings.EnableGodRays = godRaysEnabled;
                settings.NormalizeGodRayMode( FeatureLevel10Compatibility );
            }
            ImGui::SetItemTooltip( "%s", Tr( "Adds sunbeams and light scattering.", u8"F\u00FCgt Sonnenstrahlen und Lichtstreuung hinzu." ) );
            ImGui::SameLine();
            ImGui::SetNextItemWidth( standardComboWidth );
            if ( CoupledStrengthSlider( "##GodrayStrength", "GodRayStrength",
                    &godRaysEnabled, &settings.GodRayStrength ) ) {
                settings.EnableGodRays = godRaysEnabled;
                settings.NormalizeGodRayMode( FeatureLevel10Compatibility );
            }
            ImGui::SetItemTooltip( "%s", Tr( "Adjusts the intensity of sunbeams and light scattering.", u8"Passt die St\u00E4rke von Sonnenstrahlen und Lichtstreuung an." ) );
            ImGui::PopItemWidth();
            ImGui::EndGroup();
        }

        ImGui::SameLine();

        {
            ImGui::BeginGroup();
            bool waterReflections = settings.EnableSSR;
            ImText( Tr( "Water Reflections", u8"Wasserreflexionen" ), { buttonWidth.x - ImGui::GetFrameHeight() - style.ItemSpacing.x, buttonWidth.y } ); ImGui::SameLine();
            if ( CoupledStrengthCheckbox( "##Enable Water Reflections", "WaterReflectionsStrength",
                    &waterReflections, &settings.SSRStrength, 1.0f ) ) {
                settings.EnableSSR = waterReflections;
            }
            ImGui::SetItemTooltip( "%s", Tr( "Reflects the world on water surfaces.", u8"Spiegelt die Umgebung auf Wasserfl\u00E4chen." ) );
            ImGui::SameLine();
            ImGui::SetNextItemWidth( standardComboWidth );
            if ( CoupledStrengthSlider( "##WaterReflectionsStrength", "WaterReflectionsStrength",
                    &waterReflections, &settings.SSRStrength ) ) {
                settings.EnableSSR = waterReflections;
            }
            ImGui::SetItemTooltip( "%s", Tr( "Adjusts the intensity of water reflections.", u8"Passt die St\u00E4rke der Wasserreflexionen an." ) );

            float depthOfFieldStrength = settings.DoFBokehRadius / 3.5f;
            ImText( Tr( "Depth of Field", u8"Tiefenunsch\u00E4rfe" ), { buttonWidth.x - ImGui::GetFrameHeight() - style.ItemSpacing.x, buttonWidth.y } ); ImGui::SameLine();
            CoupledStrengthCheckbox( "##Enable Depth of Field", "DepthOfFieldBlurStrength",
                &settings.EnableDoF, &depthOfFieldStrength, 1.0f );
            ImGui::SetItemTooltip( "%s", Tr( "Makes areas outside the focus unsharp.", u8"Macht Bereiche au\u00DFerhalb des Fokus unscharf." ) );
            ImGui::SameLine();
            ImGui::SetNextItemWidth( standardComboWidth );
            CoupledStrengthSlider( "##DepthOfFieldBlurStrength", "DepthOfFieldBlurStrength",
                &settings.EnableDoF, &depthOfFieldStrength );
            settings.DoFBokehRadius = depthOfFieldStrength * 3.5f;
            ImGui::SetItemTooltip( "%s", Tr( "Adjusts the strength of depth-of-field blur.", u8"Passt die St\u00E4rke der Tiefenunsch\u00E4rfe an." ) );

#if defined(BUILD_GOTHIC_2_6_fix) || (defined(BUILD_GOTHIC_1_08k) && !defined(BUILD_1_12F))
#if defined(BUILD_GOTHIC_1_08k) && !defined(BUILD_1_12F)
            if ( haveWindAnimations )
#endif
            {
                bool windEffects = settings.AreWindEffectsEnabled();
                ImText( Tr( "Wind Effects", u8"Windeffekte" ), { buttonWidth.x - ImGui::GetFrameHeight() - style.ItemSpacing.x, buttonWidth.y } ); ImGui::SameLine();
                if ( CoupledStrengthCheckbox( "##Enable Wind Effects", "WindEffectsStrength",
                        &windEffects, &settings.WindEffectsStrength, 1.0f ) ) {
                    settings.WindEffectsEnabled = windEffects
                        ? GothicRendererSettings::EWindEffectsState::ENABLED
                        : GothicRendererSettings::EWindEffectsState::DISABLED;
                }
                ImGui::SetItemTooltip( "%s", Tr( "Moves vegetation in the wind.", u8"Bewegt die Vegetation im Wind." ) );
                ImGui::SameLine();
                ImGui::SetNextItemWidth( standardComboWidth );
                if ( CoupledStrengthSlider( "##WindEffectsStrength", "WindEffectsStrength",
                        &windEffects, &settings.WindEffectsStrength ) ) {
                    settings.WindEffectsEnabled = windEffects
                        ? GothicRendererSettings::EWindEffectsState::ENABLED
                        : GothicRendererSettings::EWindEffectsState::DISABLED;
                }
                ImGui::SetItemTooltip( "%s", Tr( "Adjusts how strongly vegetation moves in the wind.", u8"Passt an, wie stark sich Vegetation im Wind bewegt." ) );

            }
#endif //BUILD_GOTHIC_2_6_fix

            ImText( Tr( "Rain Effects", u8"Regeneffekte" ), { buttonWidth.x - ImGui::GetFrameHeight() - style.ItemSpacing.x, buttonWidth.y } ); ImGui::SameLine();
            MenuCheckbox( "##Enable Rain Effects", &settings.RainEffects );
            ImGui::SetItemTooltip( "%s", Tr(
                "Adds puddles and wet-ground reflections while it rains.",
                u8"Erzeugt bei Regen P\u00FCtzen und Reflexionen auf nassem Boden." ) );

            ImText( Tr( "Dynamic Clouds", u8"Dynamische Wolken" ), { buttonWidth.x - ImGui::GetFrameHeight() - style.ItemSpacing.x, buttonWidth.y } ); ImGui::SameLine();
            MenuCheckbox( "##Enable Dynamic Clouds", &settings.EnableDynamicClouds );
            ImGui::SetItemTooltip( "%s", Tr( "Adds moving low clouds to the sky.", u8"F\u00FCgt bewegte tiefe Wolken am Himmel hinzu." ) );

            ImText( Tr( "Surface Detail", u8"Oberfl\u00E4chendetail" ), { buttonWidth.x - ImGui::GetFrameHeight() - style.ItemSpacing.x, buttonWidth.y } ); ImGui::SameLine();
            if ( MenuCheckbox( "##Enable Surface Detail", &settings.AllowNormalmaps ) ) {
                settings.EnableParallaxOcclusionMapping = settings.AllowNormalmaps;
                Engine::GAPI->UpdateTextureMaxSize();
            }
            ImGui::SetItemTooltip( "%s", Tr( "Adds fine surface relief and depth.", u8"F\u00FCgt feine Relief- und Tiefendetails auf Oberfl\u00E4chen hinzu." ) );

            ImGui::EndGroup();
        }

    }
    ImGui::End();
    ImGui::PopStyleVar( 3 );
    if ( m_centerSettingsWindowFrames > 0 ) {
        --m_centerSettingsWindowFrames;
    }

}
