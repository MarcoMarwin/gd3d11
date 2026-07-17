#include "D3D11FileRelativeInclude.h"

#include <algorithm>
#include <cwctype>
#include <limits>

namespace {
    constexpr uintmax_t kMaxShaderIncludeBytes = 64ull * 1024ull * 1024ull;

    std::wstring NormalizePathForComparison( const std::filesystem::path& path ) {
        std::wstring value = path.native();
        std::transform( value.begin(), value.end(), value.begin(),
            []( wchar_t c ) { return static_cast<wchar_t>(std::towlower( c )); } );
        std::replace( value.begin(), value.end(), L'/', L'\\' );
        return value;
    }

    bool IsPathWithinRoot( const std::filesystem::path& root,
        const std::filesystem::path& candidate ) {
        std::wstring normalizedRoot = NormalizePathForComparison( root );
        const std::wstring normalizedCandidate = NormalizePathForComparison( candidate );
        if ( normalizedRoot.empty() || normalizedCandidate.empty() ) return false;
        if ( normalizedCandidate == normalizedRoot ) return true;
        if ( normalizedRoot.back() != L'\\' ) normalizedRoot.push_back( L'\\' );
        return normalizedCandidate.size() > normalizedRoot.size()
            && normalizedCandidate.compare( 0, normalizedRoot.size(), normalizedRoot ) == 0;
    }

    HRESULT ErrorToHRESULT( const std::error_code& error, DWORD fallback ) {
        const DWORD value = error ? static_cast<DWORD>(error.value()) : fallback;
        return HRESULT_FROM_WIN32( value );
    }
}

HRESULT __stdcall D3D11FileRelativeInclude::Open( D3D_INCLUDE_TYPE includeType,
    LPCSTR pFileName, LPCVOID pParentData, LPCVOID* ppData, UINT* pBytes ) {
    if ( !ppData || !pBytes || !pFileName
        || (includeType != D3D_INCLUDE_LOCAL && includeType != D3D_INCLUDE_SYSTEM) ) {
        return E_INVALIDARG;
    }
    *ppData = nullptr;
    *pBytes = 0;

    try {
        const std::filesystem::path requested( pFileName );
        if ( requested.empty() || requested.is_absolute()
            || requested.has_root_name() || requested.has_root_directory() ) {
            return E_ACCESSDENIED;
        }

        std::error_code error;
        const std::filesystem::path canonicalRoot =
            std::filesystem::weakly_canonical( RootDir, error );
        if ( error || canonicalRoot.empty() ) {
            return ErrorToHRESULT( error, ERROR_PATH_NOT_FOUND );
        }

        std::filesystem::path baseDir = canonicalRoot;
        if ( includeType == D3D_INCLUDE_LOCAL && pParentData ) {
            const auto parent = ParentDirByData.find( pParentData );
            if ( parent != ParentDirByData.end() ) baseDir = parent->second;
        }

        auto ResolveCandidate = [&]( const std::filesystem::path& base,
            std::filesystem::path& output ) -> bool {
            error.clear();
            output = std::filesystem::weakly_canonical( base / requested, error );
            if ( error || !IsPathWithinRoot( canonicalRoot, output ) ) return false;
            error.clear();
            return std::filesystem::is_regular_file( output, error ) && !error;
        };

        std::filesystem::path fullPath;
        if ( !ResolveCandidate( baseDir, fullPath )
            && (baseDir == canonicalRoot || !ResolveCandidate( canonicalRoot, fullPath )) ) {
            return HRESULT_FROM_WIN32( ERROR_FILE_NOT_FOUND );
        }

        error.clear();
        const uintmax_t fileSize = std::filesystem::file_size( fullPath, error );
        if ( error ) return ErrorToHRESULT( error, ERROR_READ_FAULT );
        if ( fileSize == 0 || fileSize > kMaxShaderIncludeBytes
            || fileSize > (std::numeric_limits<UINT>::max)() ) {
            return HRESULT_FROM_WIN32( ERROR_FILE_TOO_LARGE );
        }

        std::ifstream file( fullPath, std::ios::binary );
        if ( !file ) return HRESULT_FROM_WIN32( ERROR_FILE_NOT_FOUND );

        auto buffer = std::make_unique<uint8_t[]>( static_cast<size_t>(fileSize) );
        file.read( reinterpret_cast<char*>(buffer.get()),
            static_cast<std::streamsize>(fileSize) );
        if ( !file ) return HRESULT_FROM_WIN32( ERROR_READ_FAULT );

        const void* data = buffer.get();
        OwnedBuffers.emplace_back( std::move( buffer ) );
        ParentDirByData.emplace( data, fullPath.parent_path() );

        *ppData = data;
        *pBytes = static_cast<UINT>(fileSize);
        return S_OK;
    } catch ( const std::bad_alloc& ) {
        return E_OUTOFMEMORY;
    } catch ( ... ) {
        return E_FAIL;
    }
}
