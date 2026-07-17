#pragma once
#include <iostream>
#include <Windows.h>
#include <sstream>
#include <string>
#include "Toolbox.h"
#include <array>

//#include <DxErr.h>
//#pragma comment(lib, "Dxerr.lib")
#define USE_LOG

//#ifdef BUILD_DESKTOP
#ifdef BLERGH__
//#if defined(DEBUG) || defined(_DEBUG)
/** Checks for errors and logs them, HRESULT hr needs to be declared */
#define LE(x) { hr = (x); if (FAILED(hr)){LogError() << #x << " failed: " << DXGetErrorDescription(hr); }/*else{ LogInfo() << L#x << L" Succeeded."; }*/ }

/** Returns hr if failed (HRESULT-function, hr needs to be declared)*/
#define LE_R(x) { hr = (x); if (FAILED(hr)){LogError() << #x << " failed: " << DXGetErrorDescription(hr); return hr;} }

/** Returns nothing if failed (void-function)*/
#define LE_RV(x) { hr = (x); if (FAILED(hr)){LogError() << #x << " failed: " << DXGetErrorDescription(hr); return;} }

/** Returns false if failed (bool-function) */
#define LE_RB(x) { hr = (x); if (FAILED(hr)){LogError() << #x << " failed: " << DXGetErrorDescription(hr); return false;} }

/** throws an exceptopn if failed (HRESULT-function) */
#define LE_THROW(x) { hr = (x); if (FAILED(hr)){LogError() << #x << " failed: " << DXGetErrorDescription(hr); UT::ThrowIfFailed(hr);} }

#define ErrorBox(Msg) MessageBox(nullptr,Msg,L"Error!",MB_OK|MB_ICONERROR|MB_TOPMOST)
#define InfoBox(Msg) MessageBox(nullptr,Msg,L"Info!",MB_OK|MB_ICONASTERISK|MB_TOPMOST)
#define WarnBox(Msg) MessageBox(nullptr,Msg,L"Warning!",MB_OK|MB_ICONEXCLAMATION|MB_TOPMOST)

#else

#define XLE(x) { XRESULT xr = (x); if (xr != XRESULT::XR_SUCCESS){ LogError() << #x << " failed with code: " << std::hex << xr << " (" + Toolbox::MakeErrorString(xr) + ")";}}

/** Checks for errors and logs them, HRESULT hr needs to be declared */
#define LE(x) { hr = (x); if (FAILED(hr)){LogError() << "failed with code: " << std::hex << hr << "!"; }/*else{ LogInfo() << L#x << L" Succeeded."; }*/ }

/** Returns hr if failed (HRESULT-function, hr needs to be declared)*/
#define LE_R(x) { hr = (x); if (FAILED(hr)){LogError() << "failed with code: " << std::hex << hr << "!"; return hr;} }

/** Returns nothing if failed (void-function)*/
#define LE_RV(x) { hr = (x); if (FAILED(hr)){LogError() << "failed with code: " << std::hex << hr << "!"; return;} }

/** Returns false if failed (bool-function) */
#define LE_RB(x) { hr = (x); if (FAILED(hr)){LogError() << "failed with code: " << std::hex << hr << "!"; return false;} }

#define ErrorBox(Msg) MessageBoxA(nullptr,Msg,"GD3D11: Error!",MB_OK|MB_ICONERROR|MB_TOPMOST)
#define InfoBox(Msg) MessageBoxA(nullptr,Msg,"GD3D11: Info!",MB_OK|MB_ICONASTERISK|MB_TOPMOST)
#define WarnBox(Msg) MessageBoxA(nullptr,Msg,"GD3D11: Warning!",MB_OK|MB_ICONEXCLAMATION|MB_TOPMOST)

#endif

/*#else
#define LE(x) { hr = (x); }
#endif
*/

/** Logging macros
    Usage: LogInfo() << L"Loaded Texture: " << TextureName;
    */

#ifndef __FUNCSIG__
#define __FUNCSIG__ __builtin_FUNCSIG()
#endif

#define LogInfo() Log("Info",__FILE__, __LINE__, __FUNCSIG__)
#define LogWarn() Log("Warning",__FILE__, __LINE__, __FUNCSIG__, true)
#define LogError() Log("Error",__FILE__, __LINE__, __FUNCSIG__, true)


    /** Displays a messagebox and loggs its content */
#define LogInfoBox() Log("Info",__FILE__, __LINE__, __FUNCSIG__, false, 1)
#define LogWarnBox() Log("Warning",__FILE__, __LINE__, __FUNCSIG__, true, 2)
#define LogErrorBox() Log("Error",__FILE__, __LINE__, __FUNCSIG__, true, 3)

/** Stream logger */
#ifdef USE_LOG

namespace LoggerState {
    __declspec(selectany) SRWLOCK LogLock = SRWLOCK_INIT;
    __declspec(selectany) wchar_t LogFile[MAX_PATH + 1] = {};

    class ScopedExclusiveLock {
    public:
        ScopedExclusiveLock() noexcept { AcquireSRWLockExclusive( &LogLock ); }
        ~ScopedExclusiveLock() noexcept { ReleaseSRWLockExclusive( &LogLock ); }

        ScopedExclusiveLock( const ScopedExclusiveLock& ) = delete;
        ScopedExclusiveLock& operator=( const ScopedExclusiveLock& ) = delete;
    };
}

class Log {
public:
    Log( const char* Type, const  char* File, int Line, const  char* Function, bool bIncludeInfo = false, UINT MessageBox = 0 ) {
        if ( bIncludeInfo ) {
            Info << Type << ": [" << File << "(" << Line << "), " << Function << "]: ";
        } else {
            Info << Type << ": ";
        }

        MessageBoxStyle = MessageBox;
    }

    ~Log() noexcept {
        Flush();
    }

    /** Clears the logfile */
    static void Clear() noexcept {
        try {
            std::array<wchar_t, MAX_PATH + 1> modulePath{};
            const DWORD length = GetModuleFileNameW( nullptr, modulePath.data(), MAX_PATH );
            if ( length == 0 || length >= MAX_PATH ) {
                LoggerState::ScopedExclusiveLock lock;
                LoggerState::LogFile[0] = L'\0';
                return;
            }

            std::wstring logPath( modulePath.data(), length );
            const size_t separator = logPath.find_last_of( L"\\/" );
            logPath = separator == std::wstring::npos
                ? std::wstring( L"Log.txt" )
                : logPath.substr( 0, separator + 1 ) + L"Log.txt";

            LoggerState::ScopedExclusiveLock lock;
            if ( wcsncpy_s( LoggerState::LogFile, logPath.c_str(), _TRUNCATE ) != 0 ) {
                LoggerState::LogFile[0] = L'\0';
                return;
            }
            if ( FILE* file = _wfopen( LoggerState::LogFile, L"wb" ) ) {
                fclose( file );
            }
        } catch ( ... ) {
            LoggerState::ScopedExclusiveLock lock;
            LoggerState::LogFile[0] = L'\0';
        }
    }

#pragma warning( push )
#pragma warning( disable : 6392 )

    /** STL stringstream feature */
    template< typename T >
    inline Log& operator << ( const T& obj ) {
        Message << obj;
        return *this;
    }

    template< typename T >
    inline Log& operator << ( const T* obj ) {
        Message << obj;
        return *this;
    }

    inline Log& operator << ( const wchar_t* wide ) {
        if ( !wide ) {
            Message << "(null)";
            return *this;
        }

        try {
            const int required = WideCharToMultiByte(
                CP_UTF8, WC_ERR_INVALID_CHARS, wide, -1, nullptr, 0, nullptr, nullptr );
            if ( required <= 0 ) {
                Message << "<invalid wide string>";
                return *this;
            }

            std::string utf8( static_cast<size_t>(required), '\0' );
            const int converted = WideCharToMultiByte(
                CP_UTF8, WC_ERR_INVALID_CHARS, wide, -1, utf8.data(), required, nullptr, nullptr );
            if ( converted <= 0 ) {
                Message << "<invalid wide string>";
                return *this;
            }

            utf8.resize( static_cast<size_t>(converted - 1) );
            Message << utf8;
        } catch ( ... ) {
            Message << "<wide string conversion failed>";
        }
        return *this;
    }

    inline Log& operator << ( std::ostream& (*fn)(std::ostream&) ) {
        Message << fn;
        return *this;
    }

    inline Log& operator << ( std::ios_base& (*fn)(std::ios_base&) ) {
        Message << fn;
        return *this;
    }

#pragma warning( pop ) 

    /** Called when the object is getting destroyed, which happens immediately if simply calling the constructor of this class */
    inline void Flush() noexcept {
        if ( Flushed ) {
            return;
        }
        Flushed = true;

        try {
            const std::string info = Info.str();
            const std::string message = Message.str();

            {
                LoggerState::ScopedExclusiveLock lock;
                if ( LoggerState::LogFile[0] != L'\0' ) {
                    if ( FILE* file = _wfopen( LoggerState::LogFile, L"ab" ) ) {
                        fwrite( info.data(), 1, info.size(), file );
                        fwrite( message.data(), 1, message.size(), file );
                        fputc( '\n', file );
                        fclose( file );
                    }
                }
            }

            switch ( MessageBoxStyle ) {
            case 1:
                InfoBox( message.c_str() );
                break;
            case 2:
                WarnBox( message.c_str() );
                break;
            case 3:
                ErrorBox( message.c_str() );
                break;
            }
        } catch ( ... ) {
            // Logging must never terminate the renderer.
        }
    }

private:

    std::stringstream Info; // Contains an information like "Info", "Warning" or "Error"
    std::stringstream Message; // Text to write into the logfile
    UINT MessageBoxStyle; // Style of the messagebox if needed
    bool Flushed = false;

    //static std::string LastErrorMessage; // The last errormessage
};
#else

class Log {
public:
    Log( const char* Type, const  char* File, int Line, const  char* Function, bool bIncludeInfo = false, UINT MessageBox = 0 ) {

    }

    ~Log() {

    }

    /** Clears the logfile */
    static void Clear() {

    }

    /** STL stringstream feature */
    template< typename T >
    inline Log& operator << ( const T& obj ) {
        return *this;
    }

    inline Log& operator << ( std::wostream& (*fn)(std::wostream&) ) {
        return *this;
    }

    /** Called when the object is getting destroyed, which happens immediately if simply calling the constructor of this class */
    inline void Flush() {

    }

private:
};

#endif
