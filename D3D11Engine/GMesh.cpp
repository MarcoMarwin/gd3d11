#include "GMesh.h"

#include "assimp\Importer.hpp"
#include "assimp\postprocess.h"
#include "assimp\scene.h"
#include "Engine.h"
#include "GothicAPI.h"
#include "MeshCacheFormat.h"


#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <filesystem>
#include <limits>
#include <memory>

#pragma comment(lib, "assimp-vc143-mt.lib")

using namespace Assimp;
namespace {
    class MeshCacheReader {
    public:
        explicit MeshCacheReader( const std::vector<uint8_t>& data )
            : m_cursor( data.data() ), m_remaining( data.size() ) {
        }

        template <typename T>
        bool Read( T& value ) {
            return ReadBytes( &value, sizeof( value ) );
        }

        bool ReadBytes( void* destination, size_t size ) {
            if ( size > m_remaining || (size != 0 && !destination) ) {
                return false;
            }
            if ( size != 0 ) {
                memcpy( destination, m_cursor, size );
                m_cursor += size;
                m_remaining -= size;
            }
            return true;
        }

        size_t Remaining() const {
            return m_remaining;
        }

    private:
        const uint8_t* m_cursor;
        size_t m_remaining;
    };

    bool IsFiniteVertex( const ExVertexStruct& vertex ) {
        return std::isfinite( vertex.Position.x )
            && std::isfinite( vertex.Position.y )
            && std::isfinite( vertex.Position.z )
            && std::isfinite( vertex.Normal.x )
            && std::isfinite( vertex.Normal.y )
            && std::isfinite( vertex.Normal.z )
            && std::isfinite( vertex.TexCoord.x )
            && std::isfinite( vertex.TexCoord.y )
            && std::isfinite( vertex.TexCoord2.x )
            && std::isfinite( vertex.TexCoord2.y );
    }
}

GMesh::GMesh() {}

GMesh::~GMesh() {
    for ( unsigned int i = 0; i < Meshes.size(); i++ ) {
        SAFE_DELETE( Meshes[i] );
    }
    Meshes.clear();
}

/** Load a mesh from file */
XRESULT GMesh::LoadMesh( const std::string& file, float scale ) {
    if ( file.empty() || !std::isfinite( scale ) ) {
        return XR_INVALID_ARG;
    }

    char directory[MAX_PATH]{};
    GetCurrentDirectoryA( static_cast<DWORD>(std::size( directory )), directory );
    LogInfo() << "Loading custom mesh " << directory << "\\" << file;

    std::string extension = std::filesystem::path( file ).extension().string();
    std::transform( extension.begin(), extension.end(), extension.begin(),
        []( unsigned char character ) { return static_cast<char>(std::tolower( character )); } );
    if ( extension == ".mcache" ) {
        return LoadCached( file );
    }

    Importer importer;
    importer.SetPropertyInteger( AI_CONFIG_PP_SLM_VERTEX_LIMIT, 0xFFFF - 1 );
    const aiScene* scene = importer.ReadFile(
        file, aiProcessPreset_TargetRealtime_Fast | aiProcess_SplitLargeMeshes );
    if ( !scene || !scene->mMeshes || scene->mNumMeshes == 0 ) {
        LogError() << "Failed to open custom mesh: " << file;
        LogError() << " - " << importer.GetErrorString();
        return XR_FAILED;
    }

    LogInfo() << "Loading " << scene->mNumMeshes << " submeshes";
    if ( scene->mNumMaterials <= 3 ) {
        LogWarn() << "Mesh contains only " << scene->mNumMaterials
            << " materials; check the material library and delete stale cache files after changes.";
    }

    std::vector<std::unique_ptr<MeshInfo>> decodedMeshes;
    std::vector<std::string> decodedTextures;
    try {
        decodedMeshes.reserve( scene->mNumMeshes );
        decodedTextures.reserve( scene->mNumMeshes );

        for ( unsigned int meshIndex = 0; meshIndex < scene->mNumMeshes; ++meshIndex ) {
            const aiMesh* sourceMesh = scene->mMeshes[meshIndex];
            if ( !sourceMesh || !sourceMesh->mVertices || sourceMesh->mNumVertices == 0
                || sourceMesh->mNumVertices > MeshCacheFormat::MaxVerticesPerSubmesh
                || !sourceMesh->mFaces || sourceMesh->mNumFaces == 0 ) {
                LogError() << "Custom mesh contains an invalid submesh.";
                return XR_FAILED;
            }

            const uint64_t indexCount64 = static_cast<uint64_t>(sourceMesh->mNumFaces) * 3u;
            if ( indexCount64 > MeshCacheFormat::MaxIndicesPerSubmesh ) {
                LogError() << "Custom mesh submesh contains too many indices.";
                return XR_FAILED;
            }
            const uint32_t indexCount = static_cast<uint32_t>(indexCount64);

            std::string textureName;
            if ( scene->mMaterials && sourceMesh->mMaterialIndex < scene->mNumMaterials
                && scene->mMaterials[sourceMesh->mMaterialIndex] ) {
                aiString texturePath;
                scene->mMaterials[sourceMesh->mMaterialIndex]->GetTexture(
                    aiTextureType_DIFFUSE, 0, &texturePath );
                textureName = std::filesystem::path( texturePath.C_Str() ).stem().string();
            }

            std::vector<ExVertexStruct> vertices( sourceMesh->mNumVertices );
            for ( unsigned int vertexIndex = 0; vertexIndex < sourceMesh->mNumVertices; ++vertexIndex ) {
                ExVertexStruct& vertex = vertices[vertexIndex];
                vertex.Position = float3(
                    sourceMesh->mVertices[vertexIndex].x * scale,
                    sourceMesh->mVertices[vertexIndex].y * scale,
                    sourceMesh->mVertices[vertexIndex].z * scale );
                vertex.Normal = float3( 0.0f, 1.0f, 0.0f );
                vertex.TexCoord = float2( 0.0f, 0.0f );
                vertex.TexCoord2 = float2( 0.0f, 0.0f );
                vertex.Color = 0xFFFFFFFF;

                if ( sourceMesh->HasNormals() ) {
                    vertex.Normal = float3(
                        sourceMesh->mNormals[vertexIndex].x,
                        sourceMesh->mNormals[vertexIndex].y,
                        sourceMesh->mNormals[vertexIndex].z );
                }
                if ( sourceMesh->HasTextureCoords( 0 ) ) {
                    vertex.TexCoord = float2(
                        sourceMesh->mTextureCoords[0][vertexIndex].x,
                        sourceMesh->mTextureCoords[0][vertexIndex].y );
                }
                if ( sourceMesh->HasTextureCoords( 1 ) ) {
                    vertex.TexCoord2 = float2(
                        sourceMesh->mTextureCoords[1][vertexIndex].x,
                        sourceMesh->mTextureCoords[1][vertexIndex].y );
                }
                if ( !IsFiniteVertex( vertex ) ) {
                    LogError() << "Custom mesh contains non-finite vertex data.";
                    return XR_FAILED;
                }
            }

            std::vector<VERTEX_INDEX> indices( indexCount );
            for ( unsigned int faceIndex = 0; faceIndex < sourceMesh->mNumFaces; ++faceIndex ) {
                const aiFace& face = sourceMesh->mFaces[faceIndex];
                if ( face.mNumIndices != 3 || !face.mIndices ) {
                    LogError() << "Custom mesh is not triangulated.";
                    return XR_FAILED;
                }

                for ( unsigned int corner = 0; corner < 3; ++corner ) {
                    if ( face.mIndices[corner] >= sourceMesh->mNumVertices ) {
                        LogError() << "Custom mesh contains an out-of-range index.";
                        return XR_FAILED;
                    }
                    indices[faceIndex * 3u + corner] =
                        static_cast<VERTEX_INDEX>(face.mIndices[corner]);
                }
            }

            auto mesh = std::make_unique<MeshInfo>();
            if ( mesh->Create(
                vertices.data(), static_cast<unsigned int>(vertices.size()),
                indices.data(), static_cast<unsigned int>(indices.size()) ) != XR_SUCCESS ) {
                LogError() << "Custom mesh GPU buffers could not be created.";
                return XR_FAILED;
            }

            decodedTextures.push_back( std::move( textureName ) );
            decodedMeshes.push_back( std::move( mesh ) );
        }
    } catch ( const std::bad_alloc& ) {
        return XR_FAILED;
    }

    if ( decodedMeshes.empty() || decodedMeshes.size() != decodedTextures.size() ) {
        return XR_FAILED;
    }

    for ( MeshInfo* mesh : Meshes ) {
        delete mesh;
    }
    Meshes.clear();
    Textures = std::move( decodedTextures );
    Meshes.reserve( decodedMeshes.size() );
    for ( auto& mesh : decodedMeshes ) {
        Meshes.push_back( mesh.release() );
    }
    return XR_SUCCESS;
}
/** Draws all buffers this holds */
void GMesh::DrawMesh() {
    for ( unsigned int i = 0; i < Meshes.size(); i++ ) {
        Engine::GAPI->DrawMeshInfo( nullptr, Meshes[i] );
    }
}

/** Loads the cache-file-format */
XRESULT GMesh::LoadCached( const std::string& file ) {
    LogInfo() << "Loading cached mesh: " << file;

    std::ifstream stream( file, std::ios::binary | std::ios::ate );
    if ( !stream ) {
        LogWarn() << "Failed to find cache file: " << file;
        return XR_FAILED;
    }

    const std::streamoff fileLength = stream.tellg();
    if ( fileLength <= 0
        || static_cast<uint64_t>(fileLength) > MeshCacheFormat::MaxFileBytes
        || static_cast<uint64_t>(fileLength) > std::numeric_limits<size_t>::max() ) {
        LogError() << "Mesh cache has an invalid file size: " << file;
        return XR_FAILED;
    }

    std::vector<uint8_t> fileData;
    try {
        fileData.resize( static_cast<size_t>(fileLength) );
    } catch ( const std::bad_alloc& ) {
        return XR_FAILED;
    }

    stream.seekg( 0, std::ios::beg );
    if ( !stream.read(
        reinterpret_cast<char*>(fileData.data()),
        static_cast<std::streamsize>(fileData.size()) ) ) {
        LogError() << "Mesh cache could not be read completely: " << file;
        return XR_FAILED;
    }

    MeshCacheReader reader( fileData );
    int32_t version = 0;
    if ( !reader.Read( version ) ) {
        return XR_FAILED;
    }

    bool legacyFormat = false;
    uint32_t textureCount = 0;
    if ( version == MeshCacheFormat::CurrentVersion ) {
        uint32_t magic = 0;
        uint32_t vertexStride = 0;
        uint32_t indexStride = 0;
        if ( !reader.Read( magic ) || !reader.Read( vertexStride )
            || !reader.Read( indexStride ) || !reader.Read( textureCount )
            || magic != MeshCacheFormat::Magic
            || vertexStride != sizeof( ExVertexStruct )
            || indexStride != sizeof( VERTEX_INDEX ) ) {
            LogError() << "Mesh cache header is incompatible: " << file;
            return XR_FAILED;
        }
    } else if ( version == MeshCacheFormat::LegacyVersion ) {
        legacyFormat = true;
        int32_t legacyTextureCount = 0;
        if ( !reader.Read( legacyTextureCount ) || legacyTextureCount < 0 ) {
            return XR_FAILED;
        }
        textureCount = static_cast<uint32_t>(legacyTextureCount);
    } else {
        LogError() << "Unsupported mesh cache version " << version << ": " << file;
        return XR_FAILED;
    }

    if ( textureCount > MeshCacheFormat::MaxTextures ) {
        LogError() << "Mesh cache contains too many textures: " << file;
        return XR_FAILED;
    }

    std::vector<std::unique_ptr<MeshInfo>> decodedMeshes;
    std::vector<std::string> decodedTextures;
    uint64_t decodedBytes = 0;

    try {
        for ( uint32_t textureIndex = 0; textureIndex < textureCount; ++textureIndex ) {
            uint32_t nameLength = 0;
            if ( legacyFormat ) {
                uint8_t legacyNameLength = 0;
                if ( !reader.Read( legacyNameLength ) ) {
                    return XR_FAILED;
                }
                nameLength = legacyNameLength;
            } else if ( !reader.Read( nameLength ) ) {
                return XR_FAILED;
            }

            if ( nameLength > MeshCacheFormat::MaxTextureNameBytes
                || nameLength > reader.Remaining() ) {
                return XR_FAILED;
            }

            std::string textureName( nameLength, '\0' );
            if ( nameLength != 0
                && !reader.ReadBytes( textureName.data(), nameLength ) ) {
                return XR_FAILED;
            }
            if ( textureName.find( '\0' ) != std::string::npos ) {
                LogError() << "Mesh cache texture name contains an embedded null byte.";
                return XR_FAILED;
            }

            uint32_t submeshCount = 0;
            if ( legacyFormat ) {
                uint8_t legacySubmeshCount = 0;
                if ( !reader.Read( legacySubmeshCount ) ) {
                    return XR_FAILED;
                }
                submeshCount = legacySubmeshCount;
            } else if ( !reader.Read( submeshCount ) ) {
                return XR_FAILED;
            }
            if ( submeshCount > MeshCacheFormat::MaxSubmeshesPerTexture ) {
                return XR_FAILED;
            }

            for ( uint32_t submeshIndex = 0; submeshIndex < submeshCount; ++submeshIndex ) {
                uint32_t vertexCount = 0;
                uint32_t indexCount = 0;
                if ( legacyFormat ) {
                    int32_t legacyVertexCount = 0;
                    if ( !reader.Read( legacyVertexCount ) || legacyVertexCount <= 0 ) {
                        return XR_FAILED;
                    }
                    vertexCount = static_cast<uint32_t>(legacyVertexCount);
                } else if ( !reader.Read( vertexCount ) || vertexCount == 0 ) {
                    return XR_FAILED;
                }

                if ( vertexCount > MeshCacheFormat::MaxVerticesPerSubmesh ) {
                    return XR_FAILED;
                }
                const uint64_t vertexBytes =
                    static_cast<uint64_t>(vertexCount) * sizeof( ExVertexStruct );
                if ( vertexBytes > reader.Remaining()
                    || decodedBytes > MeshCacheFormat::MaxDecodedBytes - vertexBytes ) {
                    return XR_FAILED;
                }

                auto mesh = std::make_unique<MeshInfo>();
                mesh->Vertices.resize( vertexCount );
                if ( !reader.ReadBytes( mesh->Vertices.data(), static_cast<size_t>(vertexBytes) ) ) {
                    return XR_FAILED;
                }
                decodedBytes += vertexBytes;

                if ( legacyFormat ) {
                    int32_t legacyIndexCount = 0;
                    if ( !reader.Read( legacyIndexCount ) || legacyIndexCount <= 0 ) {
                        return XR_FAILED;
                    }
                    indexCount = static_cast<uint32_t>(legacyIndexCount);
                } else if ( !reader.Read( indexCount ) || indexCount == 0 ) {
                    return XR_FAILED;
                }

                if ( indexCount > MeshCacheFormat::MaxIndicesPerSubmesh
                    || indexCount % 3u != 0 ) {
                    return XR_FAILED;
                }
                const uint64_t indexBytes =
                    static_cast<uint64_t>(indexCount) * sizeof( VERTEX_INDEX );
                if ( indexBytes > reader.Remaining()
                    || decodedBytes > MeshCacheFormat::MaxDecodedBytes - indexBytes ) {
                    return XR_FAILED;
                }

                mesh->Indices.resize( indexCount );
                if ( !reader.ReadBytes( mesh->Indices.data(), static_cast<size_t>(indexBytes) ) ) {
                    return XR_FAILED;
                }
                decodedBytes += indexBytes;

                for ( const ExVertexStruct& vertex : mesh->Vertices ) {
                    if ( !IsFiniteVertex( vertex ) ) {
                        LogError() << "Mesh cache contains non-finite vertex data.";
                        return XR_FAILED;
                    }
                }
                for ( const VERTEX_INDEX index : mesh->Indices ) {
                    if ( index >= vertexCount ) {
                        LogError() << "Mesh cache contains an out-of-range index.";
                        return XR_FAILED;
                    }
                }

                decodedTextures.push_back( textureName );
                decodedMeshes.push_back( std::move( mesh ) );
            }
        }
    } catch ( const std::bad_alloc& ) {
        LogError() << "Mesh cache allocation failed: " << file;
        return XR_FAILED;
    }

    if ( reader.Remaining() != 0 || decodedMeshes.empty()
        || decodedMeshes.size() != decodedTextures.size() ) {
        LogError() << "Mesh cache payload is malformed: " << file;
        return XR_FAILED;
    }

    for ( MeshInfo* mesh : Meshes ) {
        delete mesh;
    }
    Meshes.clear();
    Textures = std::move( decodedTextures );
    Meshes.reserve( decodedMeshes.size() );
    for ( auto& mesh : decodedMeshes ) {
        Meshes.push_back( mesh.release() );
    }

    return XR_SUCCESS;
}