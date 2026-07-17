#pragma once
#include "ShaderCategory.h"
#include "ShaderIDs.h"
#include "D3D11VShader.h"
#include "D3D11PShader.h"
#include "D3D11GShader.h"
#include "D3D11CShader.h"
#include <functional>
#include <mutex>
#include <type_traits>
#include <string_view>

/** Struct holds initial shader data for load operation*/
struct ShaderInfo {
public:
    std::string name;				//Shader's name, used for debug logging
    std::string fileName;			//Shader's filename (without 'system\\GD3D11\\Shaders\\')
    ShaderType type;				//Shader's type: Vertex, Pixel, Geometry, Compute
    std::string entryPoint;			//Shader's entry point function name
    size_t shaderIndex;				//Per-type enum index (e.g. VShaderID/PShaderID cast to size_t)
    EVERTEX_INPUT_LAYOUT layout;						//Shader's input layout
    std::vector<D3D_SHADER_MACRO> shaderMakros;
    ShaderCategory contentCategory;	//Content category for selective reloading
    size_t compiledHash = 0;			//Hash of last successful compilation (file timestamp + macros)

    // Optional: builds per-shader dynamic macros (renderer-settings-dependent) at compile/hash time.
    // Only macros this shader actually uses should be emitted — keeps hashing precise.
    using MacroBuilder = std::function<void( std::vector<D3D_SHADER_MACRO>& )>;
    MacroBuilder macroBuilder;

    /** Builder-style factory: infers name, type, and entrypoint from enum template parameter */
    template<auto ID>
    static ShaderInfo make( std::string fn ) {
        using EnumT = decltype(ID);
        ShaderInfo si{};
        
        si.shaderIndex = static_cast<size_t>(ID);
        si.name = magic_enum::enum_name( ID );
        si.fileName = std::move( fn );
        si.type = shader_type_for<EnumT>();
        si.entryPoint = entrypoint_for<EnumT>();

        return si;
    }

    /** Chainable setters for builder pattern */
    ShaderInfo& with_layout( EVERTEX_INPUT_LAYOUT l ) { layout = l; return *this; }
    ShaderInfo& with_macros( std::vector<D3D_SHADER_MACRO> m ) { shaderMakros = std::move(m); return *this; }
    ShaderInfo& with_macros( MacroBuilder b ) { macroBuilder = std::move( b ); return *this; }
    ShaderInfo& with_category( ShaderCategory c ) { contentCategory = c; return *this; }
    ShaderInfo& with_entrypoint( std::string ep ) { entryPoint = std::move( ep ); return *this; }

private:
    ShaderInfo() : type( ShaderType::None ), shaderIndex( 0 ), layout( VERTEX_INPUT_LAYOUT_NONE ), contentCategory( ShaderCategory::Other ) {}

    template<typename EnumT>
    static constexpr ShaderType shader_type_for() {
        if constexpr ( std::is_same_v<EnumT, VShaderID> )  return ShaderType::Vertex;
        if constexpr ( std::is_same_v<EnumT, PShaderID> )  return ShaderType::Pixel;
        if constexpr ( std::is_same_v<EnumT, GShaderID> )  return ShaderType::Geometry;
        if constexpr ( std::is_same_v<EnumT, CShaderID> )  return ShaderType::Compute;
    }

    template<typename EnumT>
    static constexpr const char* entrypoint_for() {
        if constexpr ( std::is_same_v<EnumT, VShaderID> )  return "VSMain";
        if constexpr ( std::is_same_v<EnumT, PShaderID> )  return "PSMain";
        if constexpr ( std::is_same_v<EnumT, GShaderID> )  return "GSMain";
        if constexpr ( std::is_same_v<EnumT, CShaderID> )  return "CSMain";
    }
};

class D3D11ShaderManager {
public:
    D3D11ShaderManager();
    ~D3D11ShaderManager();

    /** Compiles the shader from file and outputs error messages if needed */
    static HRESULT CompileShaderFromFile( const CHAR* szFileName, LPCSTR szEntryPoint, LPCSTR szShaderModel, ID3DBlob** ppBlobOut, const std::vector<D3D_SHADER_MACRO>& makros );
    static HRESULT CompileShaderFromFile( const WCHAR* szFileName, LPCSTR szEntryPoint, LPCSTR szShaderModel, ID3DBlob** ppBlobOut, const std::vector<D3D_SHADER_MACRO>& makros );

    /** Creates list with ShaderInfos */
    XRESULT Init();

    /** Loads/Compiles Shaderes from list */
    XRESULT LoadShaders( ShaderCategory categories = ShaderCategory::All );

    /** Deletes all shaders and loads them again */
    XRESULT ReloadShaders( ShaderCategory categories = ShaderCategory::All );

    /** Called on frame start */
    XRESULT OnFrameStart();

    /** Deletes all shaders */
    XRESULT DeleteShaders();
    void UpdateShaderInfo( ShaderInfo& shader );

    /** Return a specific shader. The shared_ptr copy keeps hot-reloaded shaders alive for callers. */
    std::shared_ptr<D3D11VShader> GetVShader( VShaderID id ) {
        std::lock_guard<std::mutex> lock( _VShaderMutex );
        const size_t index = static_cast<size_t>(id);
        return index < VShaders.size() ? VShaders[index] : nullptr;
    }
    std::shared_ptr<D3D11PShader> GetPShader( PShaderID id ) {
        std::lock_guard<std::mutex> lock( _PShaderMutex );
        const size_t index = static_cast<size_t>(id);
        return index < PShaders.size() ? PShaders[index] : nullptr;
    }
    std::shared_ptr<D3D11GShader> GetGShader( GShaderID id ) {
        std::lock_guard<std::mutex> lock( _GShaderMutex );
        const size_t index = static_cast<size_t>(id);
        return index < GShaders.size() ? GShaders[index] : nullptr;
    }
    std::shared_ptr<D3D11CShader> GetCShader( CShaderID id ) {
        std::lock_guard<std::mutex> lock( _CShaderMutex );
        const size_t index = static_cast<size_t>(id);
        return index < CShaders.size() ? CShaders[index] : nullptr;
    }

private:
    XRESULT CompileShader( ShaderInfo& si );

    void UpdateVShader( size_t index, D3D11VShader* shader ) {
        std::shared_ptr<D3D11VShader> replacement( shader );
        std::lock_guard<std::mutex> lock( _VShaderMutex );
        if ( index < VShaders.size() ) VShaders[index] = std::move( replacement );
    }
    void UpdatePShader( size_t index, D3D11PShader* shader ) {
        std::shared_ptr<D3D11PShader> replacement( shader );
        std::lock_guard<std::mutex> lock( _PShaderMutex );
        if ( index < PShaders.size() ) PShaders[index] = std::move( replacement );
    }
    void UpdateGShader( size_t index, D3D11GShader* shader ) {
        std::shared_ptr<D3D11GShader> replacement( shader );
        std::lock_guard<std::mutex> lock( _GShaderMutex );
        if ( index < GShaders.size() ) GShaders[index] = std::move( replacement );
    }
    void UpdateCShader( size_t index, D3D11CShader* shader ) {
        std::shared_ptr<D3D11CShader> replacement( shader );
        std::lock_guard<std::mutex> lock( _CShaderMutex );
        if ( index < CShaders.size() ) CShaders[index] = std::move( replacement );
    }

    bool IsVShaderKnown( size_t index ) {
        std::lock_guard<std::mutex> lock( _VShaderMutex );
        return index < VShaders.size() && VShaders[index] != nullptr;
    }
    bool IsPShaderKnown( size_t index ) {
        std::lock_guard<std::mutex> lock( _PShaderMutex );
        return index < PShaders.size() && PShaders[index] != nullptr;
    }
    bool IsGShaderKnown( size_t index ) {
        std::lock_guard<std::mutex> lock( _GShaderMutex );
        return index < GShaders.size() && GShaders[index] != nullptr;
    }
    bool IsCShaderKnown( size_t index ) {
        std::lock_guard<std::mutex> lock( _CShaderMutex );
        return index < CShaders.size() && CShaders[index] != nullptr;
    }

private:
    std::vector<ShaderInfo> Shaders;							//Initial shader list for loading
    std::vector<std::shared_ptr<D3D11VShader>> VShaders;
    std::vector<std::shared_ptr<D3D11PShader>> PShaders;
    std::vector<std::shared_ptr<D3D11GShader>> GShaders;
    std::vector<std::shared_ptr<D3D11CShader>> CShaders;

    std::mutex _VShaderMutex;
    std::mutex _PShaderMutex;
    std::mutex _GShaderMutex;
    std::mutex _CShaderMutex;
    std::mutex _ReloadMutex;

    /** Shader categories to reload next frame (OR-ed together from multiple calls) */
    ShaderCategory ShaderCategoriesToReloadNextFrame;
};
