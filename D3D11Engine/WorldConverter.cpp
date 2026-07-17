#include "pch.h"
#include "WorldConverter.h"
#include "MeshCacheFormat.h"
#include <new>
#include "Engine.h"
#include "BaseGraphicsEngine.h"
#include "D3D11VertexBuffer.h"
#include "zCPolygon.h"
#include "zCMaterial.h"
#include "zCTexture.h"
#include "zCVisual.h"
#include "zCVob.h"
#include "zCProgMeshProto.h"
#include "zCMeshSoftSkin.h"
#include "zCModel.h"
#include "zCMorphMesh.h"
#include <set>
#include <unordered_map>
#include <limits>
#include <utility>
#include "ConstantBufferStructs.h"
#include "D3D11ConstantBuffer.h"
#include "zCMesh.h"
#include "zCLightmap.h"
#include "GMesh.h"
#include "MeshModifier.h"
#include "D3D11Texture.h"
#include "D3D7\MyDirectDrawSurface7.h"
#include "zCQuadMark.h"
#include <meshoptimizer/src/meshoptimizer.h>
#include "MeshManager.h"

extern MeshManager* s_MeshManager;

WorldConverter::WorldConverter() {}

WorldConverter::~WorldConverter() {}

namespace {
    bool IsFiniteVertexData( const ExVertexStruct& vertex ) {
        return std::isfinite( vertex.Position.x ) && std::isfinite( vertex.Position.y )
            && std::isfinite( vertex.Position.z ) && std::isfinite( vertex.Normal.x )
            && std::isfinite( vertex.Normal.y ) && std::isfinite( vertex.Normal.z )
            && std::isfinite( vertex.TexCoord.x ) && std::isfinite( vertex.TexCoord.y )
            && std::isfinite( vertex.TexCoord2.x ) && std::isfinite( vertex.TexCoord2.y );
    }

    BYTE ToByteSaturated( float value ) {
        if ( !std::isfinite( value ) ) return 0;
        return static_cast<BYTE>(std::clamp( value, 0.0f, 255.0f ));
    }

    template<class Element>
    std::unique_ptr<D3D11VertexBuffer> BuildGpuBuffer(
        const std::vector<Element>& data, D3D11VertexBuffer::EBindFlags bindFlags,
        D3D11VertexBuffer::EUsageFlags usage = D3D11VertexBuffer::U_IMMUTABLE,
        D3D11VertexBuffer::ECPUAccessFlags cpuAccess = D3D11VertexBuffer::CA_NONE ) {
        const uint64_t byteSize = static_cast<uint64_t>(data.size()) * sizeof( Element );
        if ( data.empty() || byteSize > std::numeric_limits<UINT>::max() ) return nullptr;
        auto buffer = std::make_unique<D3D11VertexBuffer>();
        if ( buffer->Init( const_cast<Element*>(data.data()), static_cast<UINT>(byteSize),
                bindFlags, usage, cpuAccess ) != XR_SUCCESS ) return nullptr;
        return buffer;
    }

    template<class Mesh, class Vertex, class Index>
    bool PublishMeshBuffers( Mesh* mesh, const std::vector<Vertex>& vertices,
                             const std::vector<Index>& indices,
                             D3D11VertexBuffer::EUsageFlags vertexUsage = D3D11VertexBuffer::U_IMMUTABLE,
                             D3D11VertexBuffer::ECPUAccessFlags vertexAccess = D3D11VertexBuffer::CA_NONE ) {
        if ( !mesh ) return false;
        auto vertexBuffer = BuildGpuBuffer(
            vertices, D3D11VertexBuffer::B_VERTEXBUFFER, vertexUsage, vertexAccess );
        auto indexBuffer = BuildGpuBuffer( indices, D3D11VertexBuffer::B_INDEXBUFFER );
        if ( !vertexBuffer || !indexBuffer ) return false;
        delete mesh->MeshVertexBuffer;
        delete mesh->MeshIndexBuffer;
        mesh->MeshVertexBuffer = vertexBuffer.release();
        mesh->MeshIndexBuffer = indexBuffer.release();
        return true;
    }

    bool CreateShadowIndexBuffer( MeshInfo* meshInfo ) {
        if ( !meshInfo ) return false;
        if ( meshInfo->ShadowIndices.empty() ) return true;
        auto buffer = BuildGpuBuffer(
            meshInfo->ShadowIndices, D3D11VertexBuffer::B_INDEXBUFFER );
        if ( !buffer ) return false;
        delete meshInfo->MeshShadowIndexBuffer;
        meshInfo->MeshShadowIndexBuffer = buffer.release();
        return true;
    }
    bool BuildIndexedVertices( const std::vector<ExVertexStruct>& source,
                               std::vector<ExVertexStruct>& vertices,
                               std::vector<VERTEX_INDEX>& indices ) {
        if ( source.size() < 3 || source.size() > std::numeric_limits<unsigned int>::max() ) {
            return false;
        }
        WorldConverter::IndexVertices( source.data(),
            static_cast<unsigned int>(source.size()), vertices, indices );
        return !vertices.empty() && indices.size() >= 3 && (indices.size() % 3) == 0;
    }
    std::unique_ptr<MeshInfo> BuildProgMeshSubmesh(
        zCSubMesh* submesh, const zCArrayAdapt<float3>* positions,
        const XMMATRIX* transform, bool dynamic, int meshIndex ) {
        if ( !submesh || !positions || !positions->Array || positions->NumInArray <= 0
            || !submesh->WedgeList.Array || !submesh->TriList.Array
            || submesh->WedgeList.NumInArray < 3 || submesh->TriList.NumInArray <= 0
            || static_cast<uint64_t>(submesh->WedgeList.NumInArray)
                > static_cast<uint64_t>(std::numeric_limits<VERTEX_INDEX>::max()) + 1 ) return nullptr;

        auto mesh = std::make_unique<MeshInfo>();
        mesh->Vertices.reserve( static_cast<size_t>(submesh->WedgeList.NumInArray) );
        for ( int wedgeIndex = 0; wedgeIndex < submesh->WedgeList.NumInArray; ++wedgeIndex ) {
            const zTPMWedge& wedge = submesh->WedgeList.Array[wedgeIndex];
            if ( wedge.position >= positions->NumInArray ) return nullptr;
            ExVertexStruct vertex{};
            vertex.Position = positions->Array[wedge.position];
            vertex.Normal = wedge.normal;
            vertex.TexCoord = wedge.texUV;
            vertex.TexCoord2 = float2( 0.0f, 0.0f );
            vertex.Color = 0xFFFFFFFF;
            if ( transform ) {
                XMFLOAT3 position;
                XMFLOAT3 normal;
                XMStoreFloat3( &position, XMVector3TransformCoord(
                    XMLoadFloat3( vertex.Position.toXMFLOAT3() ), *transform ) );
                const XMVECTOR transformedNormal = XMVector3TransformNormal(
                    XMLoadFloat3( vertex.Normal.toXMFLOAT3() ), *transform );
                const float normalLengthSq = XMVectorGetX( XMVector3LengthSq( transformedNormal ) );
                if ( std::isfinite( normalLengthSq ) && normalLengthSq > 1.0e-12f ) {
                    XMStoreFloat3( &normal, XMVector3Normalize( transformedNormal ) );
                } else {
                    normal = XMFLOAT3( 0.0f, 1.0f, 0.0f );
                }
                vertex.Position = position;
                vertex.Normal = normal;
            }
            if ( !IsFiniteVertexData( vertex ) ) return nullptr;
            mesh->Vertices.emplace_back( vertex );
        }

        mesh->Indices.reserve( static_cast<size_t>(submesh->TriList.NumInArray) * 3 );
        for ( int triangleIndex = 0; triangleIndex < submesh->TriList.NumInArray; ++triangleIndex ) {
            const zTPMTriangle& triangle = submesh->TriList.Array[triangleIndex];
            if ( triangle.wedge[0] >= mesh->Vertices.size()
                || triangle.wedge[1] >= mesh->Vertices.size()
                || triangle.wedge[2] >= mesh->Vertices.size() ) return nullptr;
            mesh->Indices.emplace_back( triangle.wedge[2] );
            mesh->Indices.emplace_back( triangle.wedge[1] );
            mesh->Indices.emplace_back( triangle.wedge[0] );
        }
        mesh->MeshIndex = meshIndex;
        mesh->meshId = s_MeshManager ? s_MeshManager->RecordMesh( submesh ) : 0;

        if ( !dynamic ) {
            D3D11VertexBuffer optimizer;
            if ( optimizer.OptimizeFaces( mesh->Indices.data(),
                    reinterpret_cast<byte*>(mesh->Vertices.data()),
                    static_cast<unsigned int>(mesh->Indices.size()),
                    static_cast<unsigned int>(mesh->Vertices.size()), sizeof( ExVertexStruct ) ) != XR_SUCCESS
                || optimizer.OptimizeVertices( mesh->Indices.data(),
                    reinterpret_cast<byte*>(mesh->Vertices.data()),
                    static_cast<unsigned int>(mesh->Indices.size()),
                    static_cast<unsigned int>(mesh->Vertices.size()), sizeof( ExVertexStruct ),
                    &mesh->ShadowIndices ) != XR_SUCCESS ) return nullptr;
        }
        if ( !PublishMeshBuffers( mesh.get(), mesh->Vertices, mesh->Indices,
                dynamic ? D3D11VertexBuffer::U_DYNAMIC : D3D11VertexBuffer::U_IMMUTABLE,
                dynamic ? D3D11VertexBuffer::CA_WRITE : D3D11VertexBuffer::CA_NONE )
            || (!dynamic && !CreateShadowIndexBuffer( mesh.get() )) ) return nullptr;
        return mesh;
    }
    void ComputeWorldMeshBounds( WorldMeshInfo* meshInfo ) {
        if ( !meshInfo || meshInfo->Vertices.empty() ) {
            if ( meshInfo ) {
                meshInfo->HasBoundingBox = false;
            }
            return;
        }

        XMFLOAT3 bbMin( FLT_MAX, FLT_MAX, FLT_MAX );
        XMFLOAT3 bbMax( -FLT_MAX, -FLT_MAX, -FLT_MAX );
        for ( const auto& v : meshInfo->Vertices ) {
            bbMin.x = std::min( bbMin.x, v.Position.x );
            bbMin.y = std::min( bbMin.y, v.Position.y );
            bbMin.z = std::min( bbMin.z, v.Position.z );
            bbMax.x = std::max( bbMax.x, v.Position.x );
            bbMax.y = std::max( bbMax.y, v.Position.y );
            bbMax.z = std::max( bbMax.z, v.Position.z );
        }

        meshInfo->BoundingBox.Min = bbMin;
        meshInfo->BoundingBox.Max = bbMax;
        meshInfo->HasBoundingBox = true;
    }
}

/** Collects all world-polys in the specific range. Drops all materials that have no alphablending */
void WorldConverter::WorldMeshCollectPolyRange( const float3& position, float range, std::map<int, std::map<int, WorldMeshSectionInfo>>& inSections, 
    std::vector<std::pair<MeshKey, MeshInfo*>>& outMeshes ) {
    ZoneScoped;

    if ( !std::isfinite( range ) || range <= 0.0f
        || !std::isfinite( position.x ) || !std::isfinite( position.y )
        || !std::isfinite( position.z ) ) return;

    INT2 s = GetSectionOfPos( position );
    MeshKey opaqueKey{};
    opaqueKey.Material = nullptr;
    opaqueKey.Info = nullptr;
    opaqueKey.Texture = nullptr;

    MeshInfo* opaqueMesh = new (std::nothrow) MeshInfo;
    if ( !opaqueMesh ) return;
    outMeshes.emplace_back(opaqueKey, opaqueMesh);

    FXMVECTOR xmPosition = XMLoadFloat3( position.toXMFLOAT3() );


    XMVECTOR vRange2 = XMVectorReplicate( range * range );

    // Generate the meshes
    for ( auto const& itx : inSections ) {
        for ( auto const& ity : itx.second ) {
            float px = static_cast<float>(itx.first - s.x);
            float py = static_cast<float>(ity.first - s.y);
            float len = sqrtf((px * px) + (py * py));
            if ( len < 2 ) {
                // Check all polys from all meshes
                for ( auto const& it : ity.second.WorldMeshes ) {
                    MeshInfo* m = nullptr;

                    // Create new mesh-part for alphatested surfaces
                    if ( it.first.Texture && it.first.Texture->HasAlphaChannel() ) {
                        for (auto [key, msh] : outMeshes) {
                            if (it.first == key) {
                                m = msh;
                                break;
                            }
                        }
                        if ( m == nullptr ) {
                            m = new (std::nothrow) MeshInfo;
                            if ( !m ) continue;
                            outMeshes.emplace_back( it.first, m );
                        }
                    } else {
                        // Just use the same mesh for opaque surfaces
                        m = opaqueMesh;
                    }

                    if ( !it.second || it.second->Vertices.empty() || it.second->Indices.size() < 3 ) continue;
                    const size_t additionalCapacity = std::min(
                        it.second->Indices.size(),
                        m->Vertices.max_size() - m->Vertices.size() );
                    m->Vertices.reserve( m->Vertices.size() + additionalCapacity );
                    for ( size_t i = 0; i + 2 < it.second->Indices.size(); i += 3 ) {
                        const VERTEX_INDEX i0 = it.second->Indices[i];
                        const VERTEX_INDEX i1 = it.second->Indices[i + 1];
                        const VERTEX_INDEX i2 = it.second->Indices[i + 2];
                        if ( i0 >= it.second->Vertices.size() || i1 >= it.second->Vertices.size()
                            || i2 >= it.second->Vertices.size() ) continue;

                        XMVECTOR v0 = XMLoadFloat3( it.second->Vertices[i0].Position.toXMFLOAT3() );
                        XMVECTOR v1 = XMLoadFloat3( it.second->Vertices[i1].Position.toXMFLOAT3() );
                        XMVECTOR v2 = XMLoadFloat3( it.second->Vertices[i2].Position.toXMFLOAT3() );

                        if ( XMVector3Less( XMVector3LengthSq( XMVectorSubtract( xmPosition, v0 ) ), vRange2 ) ||
                            XMVector3Less( XMVector3LengthSq( XMVectorSubtract( xmPosition, v1 ) ), vRange2 ) ||
                            XMVector3Less( XMVector3LengthSq( XMVectorSubtract( xmPosition, v2 ) ), vRange2 ) ) {
                            m->Vertices.emplace_back( it.second->Vertices[i0] );
                            m->Vertices.emplace_back( it.second->Vertices[i1] );
                            m->Vertices.emplace_back( it.second->Vertices[i2] );
                        }
                    }
                }
            }
        }
    }

    // Index all meshes
    for ( size_t i = 0; i < outMeshes.size(); ) {
        auto it = outMeshes[i];

        if ( it.second->Vertices.empty() ) {
            delete it.second;
            if ( i != outMeshes.size() - 1 ) {
                outMeshes[i] = std::move( outMeshes.back() );
            }
            outMeshes.pop_back();
            continue;
        }

        std::vector<VERTEX_INDEX> indices;
        std::vector<ExVertexStruct> vertices;
        IndexVertices( it.second->Vertices.data(), it.second->Vertices.size(), vertices, indices );

        it.second->Vertices = std::move( vertices );
        it.second->Indices = std::move( indices );

        D3D11VertexBuffer optimizer;
        if ( optimizer.OptimizeFaces( it.second->Indices.data(),
                reinterpret_cast<byte*>(it.second->Vertices.data()),
                static_cast<unsigned int>(it.second->Indices.size()),
                static_cast<unsigned int>(it.second->Vertices.size()),
                sizeof( ExVertexStruct ) ) != XR_SUCCESS
            || optimizer.OptimizeVertices( it.second->Indices.data(),
                reinterpret_cast<byte*>(it.second->Vertices.data()),
                static_cast<unsigned int>(it.second->Indices.size()),
                static_cast<unsigned int>(it.second->Vertices.size()),
                sizeof( ExVertexStruct ), &it.second->ShadowIndices ) != XR_SUCCESS
            || !PublishMeshBuffers( it.second, it.second->Vertices, it.second->Indices )
            || !CreateShadowIndexBuffer( it.second ) ) {
            LogWarn() << "Skipping a nearby world mesh whose GPU buffers could not be created.";
            delete it.second;
            if ( i != outMeshes.size() - 1 ) outMeshes[i] = std::move( outMeshes.back() );
            outMeshes.pop_back();
            continue;
        }
        ++i;
    }
}

/** Converts a loaded custommesh to be the worldmesh */
XRESULT WorldConverter::LoadWorldMeshFromFile( const std::string& file, std::map<int, std::map<int, WorldMeshSectionInfo>>* outSections, WorldInfo* info, MeshInfo** outWrappedMesh ) {
    ZoneScoped;

    if ( file.empty() || !outSections || !outWrappedMesh ) {
        return XR_INVALID_ARG;
    }
    *outWrappedMesh = nullptr;
    std::map<int, std::map<int, WorldMeshSectionInfo>> sections;

    std::unique_ptr<GMesh> mesh( new (std::nothrow) GMesh() );
    if ( !mesh ) {
        return XR_FAILED;
    }

    const float worldScale = 100.0f;
    const std::string cacheFile = file + ".mcache";

    auto publishCache = [&]() {
        std::vector<MeshInfo*>& sourceMeshes = mesh->GetMeshes();
        std::vector<std::string>& sourceTextures = mesh->GetTextures();
        if ( sourceMeshes.size() != sourceTextures.size() || sourceMeshes.empty() ) {
            return XR_FAILED;
        }

        std::map<std::string, std::vector<std::pair<std::vector<ExVertexStruct>, std::vector<VERTEX_INDEX>>>> geometry;
        for ( size_t meshIndex = 0; meshIndex < sourceMeshes.size(); ++meshIndex ) {
            if ( !sourceMeshes[meshIndex] ) {
                return XR_FAILED;
            }
            geometry[sourceTextures[meshIndex]].emplace_back(
                sourceMeshes[meshIndex]->Vertices,
                sourceMeshes[meshIndex]->Indices );
        }
        return CacheMesh( geometry, cacheFile );
    };

    XRESULT loadResult = XR_FAILED;
    if ( Toolbox::FileExists( cacheFile.c_str() ) ) {
        loadResult = mesh->LoadMesh( cacheFile, worldScale );
        if ( loadResult != XR_SUCCESS ) {
            LogWarn() << "Mesh cache is invalid; rebuilding it from the source mesh: " << cacheFile;
            loadResult = mesh->LoadMesh( file, worldScale );
            if ( loadResult == XR_SUCCESS && publishCache() != XR_SUCCESS ) {
                LogWarn() << "Source mesh loaded, but its cache could not be rebuilt.";
            }
        }
    } else {
        loadResult = mesh->LoadMesh( file, worldScale );
        if ( loadResult == XR_SUCCESS && publishCache() != XR_SUCCESS ) {
            LogWarn() << "Source mesh loaded, but its cache could not be created.";
        }
    }

    if ( loadResult != XR_SUCCESS
        || mesh->GetMeshes().empty()
        || mesh->GetMeshes().size() != mesh->GetTextures().size() ) {
        return XR_FAILED;
    }


    std::vector<MeshInfo*>& meshes = mesh->GetMeshes();
    std::vector<std::string>& textures = mesh->GetTextures();
    gtl::flat_hash_set<std::string> missingTextures;

    // run through meshes and pack them into sections
    for ( size_t m = 0; m < meshes.size(); ++m ) {
        if ( !meshes[m] || meshes[m]->Vertices.empty() || meshes[m]->Indices.size() < 3
            || (meshes[m]->Indices.size() % 3) != 0 ) return XR_FAILED;
        zCMaterial* mat = Engine::GAPI->GetMaterialByTextureName( textures[m] );
        MeshKey key{};
        key.Material = mat;
        key.Texture = mat != nullptr ? mat->GetTextureSingle() : nullptr;

        // Save missing textures
        if ( !mat ) {
            missingTextures.insert( textures[m] );
        } else {
            if ( mat->GetMatGroup() == zMAT_GROUP_WATER ) {
                // Give water surfaces a water-shader
                MaterialInfo* info = Engine::GAPI->GetMaterialInfoFrom( mat->GetTextureSingle() );
                if ( info ) {
                    info->PixelShader = PShaderID::PS_Water;
                    info->MaterialType = MaterialInfo::MT_Water;
                }
            }
        }

        //key.Lightmap = poly->GetLightmap();

        for ( ExVertexStruct& vertex : meshes[m]->Vertices ) {
            if ( !IsFiniteVertexData( vertex ) ) return XR_FAILED;
            // Mesh needs to be rotated differently
            vertex.Position = float3( vertex.Position.x, vertex.Position.y, -vertex.Position.z );

            // Fix disoriented texcoords
            vertex.TexCoord = float2( vertex.TexCoord.x, -vertex.TexCoord.y );
        }

        for ( size_t i = 0; i + 2 < meshes[m]->Indices.size(); i += 3 ) {

            if ( meshes[m]->Indices[i] >= meshes[m]->Vertices.size()
                || meshes[m]->Indices[i + 1] >= meshes[m]->Vertices.size()
                || meshes[m]->Indices[i + 2] >= meshes[m]->Vertices.size() ) {
                LogError() << "Custom world mesh contains an out-of-range triangle index.";
                return XR_FAILED;
            }

            ExVertexStruct* v[3] = { &meshes[m]->Vertices[meshes[m]->Indices[i]],
                                        &meshes[m]->Vertices[meshes[m]->Indices[i + 2]],
                                        &meshes[m]->Vertices[meshes[m]->Indices[i + 1]] };


            // Calculate midpoint of this triange to get the section
            XMFLOAT3 avgPos;
            XMStoreFloat3( &avgPos, (XMLoadFloat3( &*v[0]->Position.toXMFLOAT3() ) + XMLoadFloat3( &*v[1]->Position.toXMFLOAT3() ) + XMLoadFloat3( &*v[2]->Position.toXMFLOAT3() )) / 3.0f );
            INT2 sxy = GetSectionOfPos( avgPos );

            WorldMeshSectionInfo& section = sections[sxy.x][sxy.y];
            section.WorldCoordinates = sxy;

            XMFLOAT3& bbmin = section.BoundingBox.Min;
            XMFLOAT3& bbmax = section.BoundingBox.Max;

            for ( const ExVertexStruct* vertex : v ) {
                bbmin.x = std::min( bbmin.x, vertex->Position.x );
                bbmin.y = std::min( bbmin.y, vertex->Position.y );
                bbmin.z = std::min( bbmin.z, vertex->Position.z );
                bbmax.x = std::max( bbmax.x, vertex->Position.x );
                bbmax.y = std::max( bbmax.y, vertex->Position.y );
                bbmax.z = std::max( bbmax.z, vertex->Position.z );
            }

            auto meshIt = section.WorldMeshes.find( key );
            if ( meshIt == section.WorldMeshes.end() ) {
                key.Info = Engine::GAPI->GetMaterialInfoFrom( key.Texture );
                WorldMeshInfo* worldMesh = new (std::nothrow) WorldMeshInfo;
                if ( !worldMesh ) return XR_FAILED;
                meshIt = section.WorldMeshes.emplace( key, worldMesh ).first;
            }

            for ( const ExVertexStruct* vertex : v ) {
                meshIt->second->Vertices.emplace_back( *vertex );
            }
        }
    }

    // Print textures we couldn't find any materials for if there are any
    if ( !missingTextures.empty() ) {
        std::string ms = "\nMissing materials for custom-mesh:\n";

        for ( auto it = missingTextures.begin(); it != missingTextures.end(); it++ ) {
            ms += "\t" + (*it) + "\n";
        }

        LogWarn() << ms;
    }

    // Do not retain the source representation while GPU buffers are built.
    mesh.reset();

    XMVECTOR avgSections = XMVectorZero();
    int numSections = 0;

    std::list<std::vector<ExVertexStruct>*> vertexBuffers;
    std::list<std::vector<VERTEX_INDEX>*> indexBuffers;
    std::list<std::vector<VERTEX_INDEX>*> shadowIndexBuffers;

    // Create the vertexbuffers for every material
    for ( auto const& itx : sections ) {
        for ( auto const& ity : itx.second ) {
            numSections++;
            avgSections += XMVectorSet( static_cast<float>(itx.first), static_cast<float>(ity.first), 0, 0 );

            for ( auto const& it : ity.second.WorldMeshes ) {
                std::vector<ExVertexStruct> indexedVertices;
                std::vector<VERTEX_INDEX> indices;
                IndexVertices( it.second->Vertices.data(), it.second->Vertices.size(), indexedVertices, indices );

                it.second->Vertices = std::move( indexedVertices );
                it.second->Indices = std::move( indices );
                ComputeWorldMeshBounds( it.second );

                D3D11VertexBuffer optimizer;
                if ( optimizer.OptimizeFaces( it.second->Indices.data(),
                        reinterpret_cast<byte*>(it.second->Vertices.data()),
                        static_cast<unsigned int>(it.second->Indices.size()),
                        static_cast<unsigned int>(it.second->Vertices.size()),
                        sizeof( ExVertexStruct ) ) != XR_SUCCESS
                    || optimizer.OptimizeVertices( it.second->Indices.data(),
                        reinterpret_cast<byte*>(it.second->Vertices.data()),
                        static_cast<unsigned int>(it.second->Indices.size()),
                        static_cast<unsigned int>(it.second->Vertices.size()),
                        sizeof( ExVertexStruct ), &it.second->ShadowIndices ) != XR_SUCCESS
                    || !PublishMeshBuffers( it.second, it.second->Vertices, it.second->Indices )
                    || !CreateShadowIndexBuffer( it.second ) ) {
                    LogError() << "World mesh GPU-buffer creation failed.";
                    return XR_FAILED;
                }

                // Remember them, to wrap then up later
                vertexBuffers.emplace_back( &it.second->Vertices );
                indexBuffers.emplace_back( &it.second->Indices );
                shadowIndexBuffers.emplace_back( it.second->ShadowIndices.empty()
                    ? &it.second->Indices
                    : &it.second->ShadowIndices );
            }
        }
    }

    std::vector<ExVertexStruct> wrappedVertices;
    std::vector<unsigned int> wrappedIndices;
    std::vector<unsigned int> offsets;
    std::vector<ExVertexStruct> wrappedShadowVertices;
    std::vector<unsigned int> wrappedShadowIndices;
    std::vector<unsigned int> shadowOffsets;

    // Calculate fat vertexbuffer
    WorldConverter::WrapVertexBuffers( vertexBuffers, indexBuffers, wrappedVertices, wrappedIndices, offsets );
    WorldConverter::WrapVertexBuffers( vertexBuffers, shadowIndexBuffers, wrappedShadowVertices, wrappedShadowIndices, shadowOffsets );
    const size_t wrappedMeshCount = indexBuffers.size();
    if ( wrappedMeshCount == 0 || offsets.size() != wrappedMeshCount + 1
        || shadowOffsets.size() != wrappedMeshCount + 1 || wrappedVertices.empty()
        || wrappedIndices.empty() || wrappedShadowIndices.empty() ) return XR_FAILED;

    // Propergate the offsets
    int i = 0;
    for ( auto& itx : sections ) {
        for ( auto& ity : itx.second ) {
            int numIndices = 0;
            for ( auto const& it : ity.second.WorldMeshes ) {
                it.second->BaseIndexLocation = offsets[i];
                numIndices += it.second->Indices.size();
                it.second->BaseShadowIndexLocation = shadowOffsets[i];

                i++;
            }

            ity.second.NumIndices = numIndices;

            if ( !ity.second.WorldMeshes.empty() )
                ity.second.BaseIndexLocation = (*ity.second.WorldMeshes.begin()).second->BaseIndexLocation;
        }
    }

    // Create the buffers for wrapped mesh
    auto wmi = std::make_unique<MeshInfo>();
    auto wrappedVertexBuffer = BuildGpuBuffer(
        wrappedVertices, D3D11VertexBuffer::B_VERTEXBUFFER );
    auto wrappedIndexBuffer = BuildGpuBuffer(
        wrappedIndices, D3D11VertexBuffer::B_INDEXBUFFER );
    auto wrappedShadowBuffer = BuildGpuBuffer(
        wrappedShadowIndices, D3D11VertexBuffer::B_INDEXBUFFER );
    if ( !wrappedVertexBuffer || !wrappedIndexBuffer || !wrappedShadowBuffer ) {
        return XR_FAILED;
    }
    wmi->MeshVertexBuffer = wrappedVertexBuffer.release();
    wmi->MeshIndexBuffer = wrappedIndexBuffer.release();
    wmi->MeshShadowIndexBuffer = wrappedShadowBuffer.release();

    // Calculate the approx midpoint of the world
    if ( numSections <= 0 ) return XR_FAILED;
    avgSections /= static_cast<float>(numSections);

    if ( info ) {
        WorldInfo i{};
        XMStoreFloat2( &i.MidPoint, avgSections * WORLD_SECTION_SIZE );
        i.LowestVertex = 0;
        i.HighestVertex = 0;

        *info = std::move(i);
    }


    outSections->swap( sections );
    *outWrappedMesh = wmi.release();
    return XR_SUCCESS;
}

static bool findStringIC( const std::string_view strHaystack, const std::string_view strNeedle )
{
    const auto it = std::search(
      strHaystack.begin(), strHaystack.end(),
      strNeedle.begin(), strNeedle.end(),
      []( unsigned char ch1, unsigned char ch2 ) { return std::toupper( ch1 ) == std::toupper( ch2 ); }
    );
    return (it != strHaystack.end());
}

bool AdditionalCheckWaterFall(zCTexture* texture)
{
    if ( !texture ) {
        return false;
    }
    const std::string_view textureName = texture->GetNameView();
#ifdef BUILD_GOTHIC_2_6_fix
    if ( findStringIC( textureName, "FALL" )
        && findStringIC( textureName, "A0" ) 
        && !findStringIC( textureName, "SURFACE" )
        && !findStringIC( textureName, "STONE" )
    ) {
#else
    if ( findStringIC( textureName, "FALL" ) && (findStringIC( textureName, "SURFACE" ) || findStringIC( textureName, "STONE" )) ) {
#endif
        // Let's make it work at least with og waterfall foam
        return true;
    }
    return false;
}

static bool IsPortalMaterial( std::string_view matName )
{
    return matName.starts_with( "P:" )
        || matName.starts_with( "PN:" )
        || matName.starts_with( "PI:" );
}

/** Converts the worldmesh into a more usable format */
HRESULT WorldConverter::ConvertWorldMesh( zCPolygon** polys, unsigned int numPolygons, std::map<int, std::map<int, WorldMeshSectionInfo>>* outSections, WorldInfo* info, MeshInfo** outWrappedMesh, bool indoorLocation ) {
    ZoneScoped;
    if ( !polys || numPolygons == 0 || !outSections || !outWrappedMesh ) return E_INVALIDARG;
    *outWrappedMesh = nullptr;
    std::map<int, std::map<int, WorldMeshSectionInfo>> sections;

    // Go through every polygon and put it into its section
    std::vector<ExVertexStruct> polyVertices;
    for ( unsigned int i = 0; i < numPolygons; i++ ) {
        zCPolygon* poly = polys[i];
        if ( !poly || poly->GetNumPolyVertices() < 3 ) continue;
        auto* flags = poly->GetPolyFlags();
        auto vertices = poly->getVertices();
        auto features = poly->getFeatures();
        if ( !flags || !vertices || !features || flags->GhostOccluder ) continue;
        bool validPolygon = true;
        for ( int vertexIndex = 0; vertexIndex < poly->GetNumPolyVertices(); ++vertexIndex ) {
            const zCVertex* vertex = vertices[vertexIndex];
            const zCVertFeature* feature = features[vertexIndex];
            if ( !vertex || !feature || !std::isfinite( vertex->Position.x )
                || !std::isfinite( vertex->Position.y ) || !std::isfinite( vertex->Position.z )
                || !std::isfinite( feature->normal.x ) || !std::isfinite( feature->normal.y )
                || !std::isfinite( feature->normal.z ) || !std::isfinite( feature->texCoord.x )
                || !std::isfinite( feature->texCoord.y ) ) {
                validPolygon = false;
                break;
            }
        }
        if ( !validPolygon ) continue;

        zCMaterial* mat = poly->GetMaterial();
        if ( !mat ) continue;
        std::string_view matName = mat->__GetName().ToChar();
        // std::string_view textureName = mat->GetTextureSingle() ? mat->GetTextureSingle()->__GetName().ToChar() : "";

        // Flag portals so that we can apply a different PS shader later
        zCTexture* _tex = nullptr;
        if ( flags->PortalPoly || IsPortalMaterial( matName ) ) {
            if ( const zCTexture* tex = mat->GetTextureSingle() ) {
                std::string_view textureName = tex->__GetName().ToChar();
                if ( textureName.starts_with("OWODFLWOODGROUND.") ) {
                    continue; // this is a ground texture that is sometimes re-used for visual tricks to darken tunnels, etc. We don't want to treat this as a portal.
                } else {
                    // unsafe hack to avoid portal polys assigning material for valid normal polygons
                    // it only work because DrawMeshInfoListAlphablended use texture from material
                    _tex = reinterpret_cast<zCTexture*>(reinterpret_cast<uintptr_t>(tex) + 1u);

                    MaterialInfo* info = Engine::GAPI->GetMaterialInfoFrom( _tex, textureName );
                    if ( info ) info->MaterialType = MaterialInfo::MT_Portal;
                }
            } else {
                continue;
            }
        }

        // Calculate midpoint of this triange to get the section
        XMFLOAT3 avgPos;
        XMStoreFloat3( &avgPos, (XMLoadFloat3( poly->getVertices()[0]->Position.toXMFLOAT3() ) + XMLoadFloat3( poly->getVertices()[1]->Position.toXMFLOAT3() ) + XMLoadFloat3( poly->getVertices()[2]->Position.toXMFLOAT3() )) / 3.0f );
 
        INT2 section = GetSectionOfPos( avgPos );
        WorldMeshSectionInfo& sectionInfo = sections[section.x][section.y];
        sectionInfo.WorldCoordinates = section;

        XMFLOAT3& bbmin = sectionInfo.BoundingBox.Min;
        XMFLOAT3& bbmax = sectionInfo.BoundingBox.Max;


        // Use the map to put the polygon to those using the same material
        MeshKey key{};
        key.Texture = _tex ? _tex : mat->GetTextureSingle();
        key.Material = mat;

        auto it = sectionInfo.WorldMeshes.find( key );
        if ( it == sectionInfo.WorldMeshes.end() ) {
            key.Info = Engine::GAPI->GetMaterialInfoFrom( key.Texture );
            WorldMeshInfo* worldMesh = new (std::nothrow) WorldMeshInfo;
            if ( !worldMesh ) return E_OUTOFMEMORY;
            it = sectionInfo.WorldMeshes.emplace( key, worldMesh ).first;
        }

        int matGroup = mat->GetMatGroup();
#ifdef BUILD_GOTHIC_2_6_fix
        if ( matGroup != zMAT_GROUP_WATER && !_tex ) {
            if ( AdditionalCheckWaterFall( key.Texture ) ) {
                matGroup = zMAT_GROUP_WATER;
            }
        }
#endif

        // Extract poly vertices
        polyVertices.clear();
        polyVertices.reserve( poly->GetNumPolyVertices() );
        for ( int v = 0; v < poly->GetNumPolyVertices(); v++ ) {
            zCVertex* vertex = poly->getVertices()[v];
            zCVertFeature* feature = poly->getFeatures()[v];

            polyVertices.emplace_back();
            ExVertexStruct& t = polyVertices.back();
            t.Position = vertex->Position;
            t.TexCoord = feature->texCoord;
            t.Normal = feature->normal;
            t.Color = feature->lightStatic;

            // Check bounding box
            bbmin.x = bbmin.x > vertex->Position.x ? vertex->Position.x : bbmin.x;
            bbmin.y = bbmin.y > vertex->Position.y ? vertex->Position.y : bbmin.y;
            bbmin.z = bbmin.z > vertex->Position.z ? vertex->Position.z : bbmin.z;

            bbmax.x = bbmax.x < vertex->Position.x ? vertex->Position.x : bbmax.x;
            bbmax.y = bbmax.y < vertex->Position.y ? vertex->Position.y : bbmax.y;
            bbmax.z = bbmax.z < vertex->Position.z ? vertex->Position.z : bbmax.z;

            if ( poly->GetLightmap() ) {
                t.TexCoord2 = poly->GetLightmap()->GetLightmapUV( *t.Position.toXMFLOAT3() );
                t.Color = DEFAULT_LIGHTMAP_POLY_COLOR;
            } else if ( indoorLocation ) {
                t.TexCoord2 = float2( 0.0f, 0.0f );
                t.Color = DEFAULT_LIGHTMAP_POLY_COLOR;
            } else {
                t.TexCoord2 = float2( 0.0f, 0.0f );
                if ( matGroup == zMAT_GROUP_WATER ) {
                    t.Color = 0xFFFFFFFF;
                }
            }

            if ( matGroup == zMAT_GROUP_WATER ) {
                if ( mat->HasTexAniMap() ) {
                    t.TexCoord2 = mat->GetTexAniMapDelta();
                } else {
                    t.TexCoord2 = float2( 0.0f, 0.0f );
                }

                if ( mat->GetWaveMode() != zTMode_NONE ) {
                    reinterpret_cast<BYTE*>(&t.Color)[2] = ToByteSaturated( mat->GetWaveMaxAmplitude() / 5.0f );
                    reinterpret_cast<BYTE*>(&t.Color)[3] = ToByteSaturated( mat->GetWaveSpeed() * 10.0f );
                } else {
                    reinterpret_cast<BYTE*>(&t.Color)[2] = 0;
                    reinterpret_cast<BYTE*>(&t.Color)[3] = 0;
                }
            }
            if ( !IsFiniteVertexData( t ) ) return E_FAIL;
        }

        const size_t generatedVertexCount = (polyVertices.size() - 2) * 3;
        if ( generatedVertexCount > it->second->Vertices.max_size() - it->second->Vertices.size() )
            return E_OUTOFMEMORY;
        it->second->Vertices.reserve( it->second->Vertices.size() + generatedVertexCount );
        TriangleFanToList( polyVertices.data(), static_cast<unsigned int>(polyVertices.size()), &it->second->Vertices );
        if ( matGroup == zMAT_GROUP_WATER && !mat->HasAlphaTest() ) {
#ifdef BUILD_GOTHIC_1_08k
            MaterialInfo* info = Engine::GAPI->GetMaterialInfoFrom( key.Texture );
            if ( !(AdditionalCheckWaterFall( key.Texture )) ) { 
                // Give water surfaces a water-shader
                if ( info ) {
                    info->PixelShader = PShaderID::PS_Water;
                    info->MaterialType = MaterialInfo::MT_Water;
                }
            }
            else {
                //apply alpha blend to waterfall foam and flag it as water fall foam to apply shader later
                if ( info ) {
                    poly->GetMaterial()->SetAlphaFunc( zMAT_ALPHA_FUNC_BLEND );
                    info->MaterialType = MaterialInfo::MT_WaterfallFoam;
                }
            }
#else
            // Give water surfaces a water-shader
            MaterialInfo* info = Engine::GAPI->GetMaterialInfoFrom( key.Texture );
            if ( info ) {
                info->PixelShader = PShaderID::PS_Water;
                info->MaterialType = MaterialInfo::MT_Water;
            }
#endif
        }
    }

    XMVECTOR avgSections = XMVectorZero();
    int numSections = 0;

    std::list<std::vector<ExVertexStruct>*> vertexBuffers;
    std::list<std::vector<VERTEX_INDEX>*> indexBuffers;
    std::list<std::vector<VERTEX_INDEX>*> shadowIndexBuffers;

    // Create the vertexbuffers for every material
    for ( auto const& itx : sections ) {
        for ( auto const& ity : itx.second ) {
            numSections++;
            avgSections += XMVectorSet( (float)itx.first, (float)ity.first, 0, 0 );

            for ( auto const& it : ity.second.WorldMeshes ) {
                std::vector<ExVertexStruct> indexedVertices;
                std::vector<VERTEX_INDEX> indices;
                IndexVertices( it.second->Vertices.data(), it.second->Vertices.size(), indexedVertices, indices );

                it.second->Vertices = std::move( indexedVertices );
                it.second->Indices = std::move( indices );
                ComputeWorldMeshBounds( it.second );

                GenerateVertexNormals( it.second->Vertices, it.second->Indices );
                D3D11VertexBuffer optimizer;
                if ( optimizer.OptimizeFaces( it.second->Indices.data(),
                        reinterpret_cast<byte*>(it.second->Vertices.data()),
                        static_cast<unsigned int>(it.second->Indices.size()),
                        static_cast<unsigned int>(it.second->Vertices.size()),
                        sizeof( ExVertexStruct ) ) != XR_SUCCESS
                    || optimizer.OptimizeVertices( it.second->Indices.data(),
                        reinterpret_cast<byte*>(it.second->Vertices.data()),
                        static_cast<unsigned int>(it.second->Indices.size()),
                        static_cast<unsigned int>(it.second->Vertices.size()),
                        sizeof( ExVertexStruct ), &it.second->ShadowIndices ) != XR_SUCCESS
                    || !PublishMeshBuffers( it.second, it.second->Vertices, it.second->Indices )
                    || !CreateShadowIndexBuffer( it.second ) ) {
                    LogError() << "World mesh GPU-buffer creation failed.";
                    return E_FAIL;
                }

                // Remember them, to wrap then up later
                vertexBuffers.emplace_back( &it.second->Vertices );
                indexBuffers.emplace_back( &it.second->Indices );
                shadowIndexBuffers.emplace_back( it.second->ShadowIndices.empty()
                    ? &it.second->Indices
                    : &it.second->ShadowIndices );
            }
        }
    }

    std::vector<ExVertexStruct> wrappedVertices;
    std::vector<unsigned int> wrappedIndices;
    std::vector<unsigned int> offsets;
    std::vector<ExVertexStruct> wrappedShadowVertices;
    std::vector<unsigned int> wrappedShadowIndices;
    std::vector<unsigned int> shadowOffsets;

    // Calculate fat vertexbuffer
    WorldConverter::WrapVertexBuffers( vertexBuffers, indexBuffers, wrappedVertices, wrappedIndices, offsets );
    WorldConverter::WrapVertexBuffers( vertexBuffers, shadowIndexBuffers, wrappedShadowVertices, wrappedShadowIndices, shadowOffsets );
    const size_t wrappedMeshCount = indexBuffers.size();
    if ( wrappedMeshCount == 0 || offsets.size() != wrappedMeshCount + 1
        || shadowOffsets.size() != wrappedMeshCount + 1 || wrappedVertices.empty()
        || wrappedIndices.empty() || wrappedShadowIndices.empty() ) return E_FAIL;

    // Propergate the offsets
    int i = 0;
    for ( auto const& itx : sections ) {
        for ( auto const& ity : itx.second ) {
            for ( auto const& it : ity.second.WorldMeshes ) {
                it.second->BaseIndexLocation = offsets[i];
                it.second->BaseShadowIndexLocation = shadowOffsets[i];

                i++;
            }
        }
    }

    // Create the buffers for wrapped mesh
    auto wmi = std::make_unique<MeshInfo>();
    auto wrappedVertexBuffer = BuildGpuBuffer( wrappedVertices, D3D11VertexBuffer::B_VERTEXBUFFER );
    auto wrappedIndexBuffer = BuildGpuBuffer( wrappedIndices, D3D11VertexBuffer::B_INDEXBUFFER );
    auto wrappedShadowBuffer = BuildGpuBuffer( wrappedShadowIndices, D3D11VertexBuffer::B_INDEXBUFFER );
    if ( !wrappedVertexBuffer || !wrappedIndexBuffer || !wrappedShadowBuffer ) return E_FAIL;
    wmi->MeshVertexBuffer = wrappedVertexBuffer.release();
    wmi->MeshIndexBuffer = wrappedIndexBuffer.release();
    wmi->MeshShadowIndexBuffer = wrappedShadowBuffer.release();

    // Calculate the approx midpoint of the world
    if ( numSections <= 0 ) return E_FAIL;
    avgSections /= static_cast<float>(numSections);

    if ( info ) {
        /*WorldInfo i;
        i.MidPoint = avgSections * WORLD_SECTION_SIZE;
        i.LowestVertex = 0;
        i.HighestVertex = 0;

        memcpy(info, &i, sizeof(WorldInfo));*/

        XMStoreFloat2( &info->MidPoint, avgSections * WORLD_SECTION_SIZE );
        info->LowestVertex = 0;
        info->HighestVertex = 0;
    }
    //SaveSectionsToObjUnindexed("Test.obj", sections);

    outSections->swap( sections );
    *outWrappedMesh = wmi.release();
    return S_OK;
}

/** Creates the FullSectionMesh for the given section */
void WorldConverter::GenerateFullSectionMesh( WorldMeshSectionInfo& section ) {
    ZoneScoped;

    std::vector<ExVertexStruct> vertices;
    auto appendTriangles = [&]( const MeshInfo* mesh, const XMMATRIX* transform ) {
        if ( !mesh || mesh->Vertices.empty() || mesh->Indices.size() < 3 ) return;
        for ( size_t i = 0; i + 2 < mesh->Indices.size(); i += 3 ) {
            const VERTEX_INDEX indices[3] = {
                mesh->Indices[i], mesh->Indices[i + 1], mesh->Indices[i + 2]
            };
            if ( indices[0] >= mesh->Vertices.size() || indices[1] >= mesh->Vertices.size()
                || indices[2] >= mesh->Vertices.size() ) continue;

            ExVertexStruct triangle[3] = {
                mesh->Vertices[indices[0]], mesh->Vertices[indices[1]], mesh->Vertices[indices[2]]
            };
            bool valid = IsFiniteVertexData( triangle[0] )
                && IsFiniteVertexData( triangle[1] ) && IsFiniteVertexData( triangle[2] );
            if ( transform ) {
                for ( ExVertexStruct& vertex : triangle ) {
                    XMFLOAT3 position;
                    XMStoreFloat3( &position, XMVector3TransformCoord(
                        XMLoadFloat3( vertex.Position.toXMFLOAT3() ), *transform ) );
                    vertex.Position = position;
                    valid = valid && IsFiniteVertexData( vertex );
                }
            }
            if ( !valid ) continue;
            vertices.insert( vertices.end(), std::begin( triangle ), std::end( triangle ) );
        }
    };

    for ( const auto& entry : section.WorldMeshes ) {
        if ( !entry.first.Material || entry.first.Material->HasAlphaTest() ) continue;
        appendTriangles( entry.second, nullptr );
    }

    for ( const VobInfo* vobInfo : section.Vobs ) {
        if ( !vobInfo || vobInfo->IsIndoorVob || !vobInfo->Vob || !vobInfo->VisualInfo ) continue;
        XMFLOAT4X4 world{};
        vobInfo->Vob->GetWorldMatrix( &world );
        const XMMATRIX transform = XMMatrixTranspose( XMLoadFloat4x4( &world ) );
        for ( const auto& materialMeshes : vobInfo->VisualInfo->Meshes ) {
            if ( !materialMeshes.first || materialMeshes.first->HasAlphaTest() ) continue;
            for ( const MeshInfo* mesh : materialMeshes.second ) {
                appendTriangles( mesh, &transform );
            }
        }
    }

    if ( vertices.empty() ) {
        delete section.FullStaticMesh;
        section.FullStaticMesh = nullptr;
        return;
    }

    auto vertexBuffer = BuildGpuBuffer( vertices, D3D11VertexBuffer::B_VERTEXBUFFER );
    auto mesh = std::make_unique<MeshInfo>();
    if ( !vertexBuffer || !mesh ) {
        LogWarn() << "Could not create a full-section mesh.";
        return;
    }
    mesh->Vertices = std::move( vertices );
    mesh->MeshVertexBuffer = vertexBuffer.release();
    delete section.FullStaticMesh;
    section.FullStaticMesh = mesh.release();
}
/** Returns what section the given position is in */
INT2 WorldConverter::GetSectionOfPos( const float3& pos ) {
    auto toSection = []( float coordinate ) {
        if ( !std::isfinite( coordinate ) ) return 0;
        const double value = static_cast<double>(coordinate) / WORLD_SECTION_SIZE + 0.5;
        if ( value <= std::numeric_limits<int>::lowest() ) return std::numeric_limits<int>::lowest();
        if ( value >= std::numeric_limits<int>::max() ) return std::numeric_limits<int>::max();
        return static_cast<int>(value);
    };
    return INT2( toSection( pos.x ), toSection( pos.z ) );
}
/** Converts a triangle fan to a list */
void WorldConverter::TriangleFanToList( const ExVertexStruct* input, unsigned int numInputVertices, std::vector<ExVertexStruct>* outVertices ) {
    if ( !input || !outVertices || numInputVertices < 3 ) return;
    for ( UINT i = 1; i + 1 < numInputVertices; ++i ) {
        outVertices->emplace_back( input[0] );
        outVertices->emplace_back( input[i + 1] );
        outVertices->emplace_back( input[i] );
    }
}

/** Saves the given section-array to an obj file */
void WorldConverter::SaveSectionsToObjUnindexed( const char* file, const std::map<int, std::map<int, WorldMeshSectionInfo>>& sections ) {
    FILE* f = fopen( file, "w" );

    if ( !f ) {
        LogError() << "Failed to open file " << file << " for writing!";
        return;
    }

    fputs( "o World\n", f );

    for ( auto const& itx : sections ) {
        for ( auto const& ity : itx.second ) {
            for ( auto const& it : ity.second.WorldMeshes ) {
                for ( auto const& vtx : it.second->Vertices ) {
                    std::string ln = "v " + std::to_string( vtx.Position.x ) + " " + std::to_string( vtx.Position.y ) + " " + std::to_string( vtx.Position.z ) + "\n";
                    fputs( ln.c_str(), f );
                }
            }
        }
    }

    fclose( f );
}

/** Extracts a 3DS-Mesh from a zCVisual */
void WorldConverter::Extract3DSMeshFromVisual( zCProgMeshProto* visual, MeshVisualInfo* meshInfo ) {
    ZoneScoped;
    if ( !visual || !meshInfo ) return;

    zCArrayAdapt<float3>* positions = visual->GetPositionList();
    const int submeshCount = visual->GetNumSubmeshes();
    if ( !positions || !positions->Array || positions->NumInArray <= 0
        || submeshCount <= 0 || submeshCount > 4096 ) return;

    for ( int submeshIndex = 0; submeshIndex < submeshCount; ++submeshIndex ) {
        zCSubMesh* submesh = visual->GetSubmesh( submeshIndex );
        if ( !submesh || !submesh->WedgeList.Array || !submesh->TriList.Array
            || submesh->WedgeList.NumInArray < 3 || submesh->TriList.NumInArray <= 0
            || static_cast<uint64_t>(submesh->WedgeList.NumInArray)
                > static_cast<uint64_t>(std::numeric_limits<VERTEX_INDEX>::max()) + 1 ) continue;

        std::vector<ExVertexStruct> vertices;
        vertices.reserve( static_cast<size_t>(submesh->WedgeList.NumInArray) );
        bool valid = true;
        for ( int wedgeIndex = 0; wedgeIndex < submesh->WedgeList.NumInArray; ++wedgeIndex ) {
            const zTPMWedge& wedge = submesh->WedgeList.Array[wedgeIndex];
            if ( wedge.position >= positions->NumInArray ) {
                valid = false;
                break;
            }
            ExVertexStruct vertex{};
            vertex.Position = positions->Array[wedge.position];
            vertex.Normal = wedge.normal;
            vertex.TexCoord = wedge.texUV;
            vertex.TexCoord2 = float2( 0.0f, 0.0f );
            vertex.Color = 0xFFFFFFFF;
            if ( !IsFiniteVertexData( vertex ) ) {
                valid = false;
                break;
            }
            vertices.emplace_back( vertex );
        }
        if ( !valid ) continue;

        std::vector<VERTEX_INDEX> indices;
        indices.reserve( static_cast<size_t>(submesh->TriList.NumInArray) * 3 );
        for ( int triangleIndex = 0; triangleIndex < submesh->TriList.NumInArray; ++triangleIndex ) {
            const zTPMTriangle& triangle = submesh->TriList.Array[triangleIndex];
            for ( int corner = 0; corner < 3; ++corner ) {
                if ( triangle.wedge[corner] >= submesh->WedgeList.NumInArray ) {
                    valid = false;
                    break;
                }
                indices.emplace_back( triangle.wedge[corner] );
            }
            if ( !valid ) break;
        }
        if ( !valid || indices.empty() ) continue;

        auto mesh = std::make_unique<MeshInfo>();
        mesh->Vertices = std::move( vertices );
        mesh->Indices = std::move( indices );
        mesh->meshId = s_MeshManager ? s_MeshManager->RecordMesh( submesh ) : 0;
        D3D11VertexBuffer optimizer;
        if ( optimizer.OptimizeFaces( mesh->Indices.data(),
                reinterpret_cast<byte*>(mesh->Vertices.data()),
                static_cast<unsigned int>(mesh->Indices.size()),
                static_cast<unsigned int>(mesh->Vertices.size()), sizeof( ExVertexStruct ) ) != XR_SUCCESS
            || optimizer.OptimizeVertices( mesh->Indices.data(),
                reinterpret_cast<byte*>(mesh->Vertices.data()),
                static_cast<unsigned int>(mesh->Indices.size()),
                static_cast<unsigned int>(mesh->Vertices.size()), sizeof( ExVertexStruct ),
                &mesh->ShadowIndices ) != XR_SUCCESS
            || !PublishMeshBuffers( mesh.get(), mesh->Vertices, mesh->Indices )
            || !CreateShadowIndexBuffer( mesh.get() ) ) {
            LogWarn() << "Skipping a prog-mesh submesh whose GPU data is invalid.";
            continue;
        }
        meshInfo->Meshes[submesh->Material].emplace_back( mesh.release() );
    }
    meshInfo->Visual = visual;
}
/** Extracts a skeletal mesh from a zCMeshSoftSkin */
void WorldConverter::ExtractSkeletalMeshFromVob( zCModel* model, SkeletalMeshVisualInfo* skeletalMeshInfo ) {
    ZoneScoped;
    if ( !model || !skeletalMeshInfo ) return;
    auto* softSkins = model->GetMeshSoftSkinList();
    if ( !softSkins || !softSkins->Array || softSkins->NumInArray <= 0
        || softSkins->NumInArray > 1024 ) return;

    for ( int skinIndex = 0; skinIndex < softSkins->NumInArray; ++skinIndex ) {
        zCMeshSoftSkin* skin = softSkins->Array[skinIndex];
        if ( !skin ) continue;
        auto* positions = skin->GetPositionList();
        char* stream = skin->GetVertWeightStream();
        if ( !positions || !positions->Array || positions->NumInArray <= 0 || !stream ) continue;

        std::vector<ExSkelVertexStruct> weightedPositions;
        weightedPositions.reserve( static_cast<size_t>(positions->NumInArray) );
        bool validSkin = true;
        for ( int positionIndex = 0; positionIndex < positions->NumInArray; ++positionIndex ) {
            int nodeCount = 0;
            memcpy( &nodeCount, stream, sizeof( nodeCount ) );
            stream += sizeof( nodeCount );
            if ( nodeCount < 0 || nodeCount > 256 ) {
                validSkin = false;
                break;
            }

            ExSkelVertexStruct vertex{};
            for ( int nodeIndex = 0; nodeIndex < nodeCount; ++nodeIndex ) {
                zTWeightEntry entry{};
                memcpy( &entry, stream, sizeof( entry ) );
                stream += sizeof( entry );
                if ( !std::isfinite( entry.Weight ) || !std::isfinite( entry.VertexPosition.x )
                    || !std::isfinite( entry.VertexPosition.y )
                    || !std::isfinite( entry.VertexPosition.z ) ) {
                    validSkin = false;
                    break;
                }
                if ( nodeIndex >= 4 ) continue;
                alignas(16) float values[4] = {
                    std::clamp( entry.VertexPosition.x, -65504.0f, 65504.0f ),
                    std::clamp( entry.VertexPosition.y, -65504.0f, 65504.0f ),
                    std::clamp( entry.VertexPosition.z, -65504.0f, 65504.0f ),
                    std::clamp( entry.Weight, 0.0f, 1.0f )
                };
                alignas(16) unsigned short halfs[4]{};
                QuantizeHalfFloat_X4( values, halfs );
                vertex.weights[nodeIndex] = halfs[3];
                vertex.boneIndices[nodeIndex] = entry.NodeIndex;
                vertex.Position[nodeIndex][0] = halfs[0];
                vertex.Position[nodeIndex][1] = halfs[1];
                vertex.Position[nodeIndex][2] = halfs[2];
            }
            if ( !validSkin ) break;
            weightedPositions.emplace_back( vertex );
        }
        if ( !validSkin || weightedPositions.size() != static_cast<size_t>(positions->NumInArray) ) continue;

        const int submeshCount = skin->GetNumSubmeshes();
        if ( submeshCount <= 0 || submeshCount > 4096 ) continue;
        for ( int submeshIndex = 0; submeshIndex < submeshCount; ++submeshIndex ) {
            zCSubMesh* submesh = skin->GetSubmesh( submeshIndex );
            if ( !submesh || !submesh->WedgeList.Array || !submesh->TriList.Array
                || submesh->WedgeList.NumInArray < 3 || submesh->TriList.NumInArray <= 0
                || static_cast<uint64_t>(submesh->WedgeList.NumInArray)
                    > static_cast<uint64_t>(std::numeric_limits<VERTEX_INDEX>::max()) + 1 ) continue;

            std::vector<ExSkelVertexStruct> vertices;
            std::vector<ExVertexStruct> bindPoseVertices;
            vertices.reserve( static_cast<size_t>(submesh->WedgeList.NumInArray) );
            bindPoseVertices.reserve( static_cast<size_t>(submesh->WedgeList.NumInArray) );
            zCArrayAdapt<float3>* normals = skin->GetNormalsList();
            bool valid = true;
            for ( int wedgeIndex = 0; wedgeIndex < submesh->WedgeList.NumInArray; ++wedgeIndex ) {
                const zTPMWedge& wedge = submesh->WedgeList.Array[wedgeIndex];
                if ( wedge.position >= weightedPositions.size() ) {
                    valid = false;
                    break;
                }
                ExSkelVertexStruct skinned = weightedPositions[wedge.position];
                skinned.Normal = normals && normals->Array && wedge.position < normals->NumInArray
                    ? normals->Array[wedge.position] : wedge.normal;
                skinned.BindPoseNormal = wedge.normal;
                skinned.TexCoord = wedge.texUV;
                ExVertexStruct bindPose{};
                bindPose.Position = positions->Array[wedge.position];
                bindPose.Normal = skinned.Normal;
                bindPose.TexCoord = skinned.TexCoord;
                bindPose.TexCoord2 = float2( 0.0f, 0.0f );
                bindPose.Color = 0xFFFFFFFF;
                if ( !IsFiniteVertexData( bindPose ) || !std::isfinite( skinned.BindPoseNormal.x )
                    || !std::isfinite( skinned.BindPoseNormal.y )
                    || !std::isfinite( skinned.BindPoseNormal.z ) ) {
                    valid = false;
                    break;
                }
                vertices.emplace_back( skinned );
                bindPoseVertices.emplace_back( bindPose );
            }
            if ( !valid ) continue;

            std::vector<VERTEX_INDEX> indices;
            indices.reserve( static_cast<size_t>(submesh->TriList.NumInArray) * 3 );
            for ( int triangleIndex = 0; triangleIndex < submesh->TriList.NumInArray; ++triangleIndex ) {
                const zTPMTriangle& triangle = submesh->TriList.Array[triangleIndex];
                if ( triangle.wedge[0] >= vertices.size() || triangle.wedge[1] >= vertices.size()
                    || triangle.wedge[2] >= vertices.size() ) {
                    valid = false;
                    break;
                }
                indices.emplace_back( triangle.wedge[2] );
                indices.emplace_back( triangle.wedge[1] );
                indices.emplace_back( triangle.wedge[0] );
            }
            if ( !valid || indices.empty() ) continue;

            auto mesh = std::make_unique<SkeletalMeshInfo>();
            auto bindPoseMesh = std::make_unique<MeshInfo>();
            mesh->Vertices = std::move( vertices );
            mesh->Indices = indices;
            mesh->visual = skin;
            mesh->meshId = s_MeshManager ? s_MeshManager->RecordMesh( submesh ) : 0;
            bindPoseMesh->Vertices = std::move( bindPoseVertices );
            bindPoseMesh->Indices = std::move( indices );
            if ( !PublishMeshBuffers( mesh.get(), mesh->Vertices, mesh->Indices )
                || !PublishMeshBuffers( bindPoseMesh.get(), bindPoseMesh->Vertices,
                    bindPoseMesh->Indices ) ) continue;

            auto& skeletalMeshes = skeletalMeshInfo->SkeletalMeshes[submesh->Material];
            auto& bindPoseMeshes = skeletalMeshInfo->Meshes[submesh->Material];
            skeletalMeshes.reserve( skeletalMeshes.size() + 1 );
            bindPoseMeshes.reserve( bindPoseMeshes.size() + 1 );
            skeletalMeshes.emplace_back( mesh.get() );
            bindPoseMeshes.emplace_back( bindPoseMesh.get() );

            mesh.release();
            bindPoseMesh.release();
        }
    }

    const char* visualName = model->GetVisualName();
    skeletalMeshInfo->VisualName = visualName ? visualName : "";
    skeletalMeshInfo->Visual = model;
}
/** Extracts a zCProgMeshProto from a zCModel */
void WorldConverter::ExtractProgMeshProtoFromModel( zCModel* model, MeshVisualInfo* meshInfo ) {
    ZoneScoped;
    if ( !model || !meshInfo ) return;

    model->UpdateAttachedVobs();
    model->UpdateMeshLibTexAniState();
    zSTRING modelName = model->GetModelName();
    const char* modelNameData = modelName.ToChar();
    const std::string visualName = modelNameData ? modelNameData : "";
    modelName.Delete();

    zCArray<zCModelNodeInst*>* nodes = model->GetNodeList();
    if ( !nodes || !nodes->Array || nodes->NumInArray <= 0 || nodes->NumInArray > 65536 ) return;

    XMFLOAT3 minimum( FLT_MAX, FLT_MAX, FLT_MAX );
    XMFLOAT3 maximum( -FLT_MAX, -FLT_MAX, -FLT_MAX );
    bool hasBounds = false;

    for ( int nodeIndex = 0; nodeIndex < nodes->NumInArray; ++nodeIndex ) {
        zCModelNodeInst* node = nodes->Array[nodeIndex];
        if ( !node || !node->NodeVisual ) continue;
        const char* extension = node->NodeVisual->GetFileExtension( 0 );
        if ( !extension ) continue;
        const bool isMorphMesh = strcmp( extension, ".MMS" ) == 0;
        if ( !isMorphMesh && strcmp( extension, ".3DS" ) != 0 ) continue;

        zCProgMeshProto* visual = isMorphMesh
            ? reinterpret_cast<zCMorphMesh*>(node->NodeVisual)->GetMorphMesh()
            : static_cast<zCProgMeshProto*>(node->NodeVisual);
        if ( !visual ) continue;
        if ( node->ParentNode ) {
            XMStoreFloat4x4( &node->TrafoObjToCam,
                XMLoadFloat4x4( &node->ParentNode->TrafoObjToCam ) * XMLoadFloat4x4( &node->Trafo ) );
        } else {
            node->TrafoObjToCam = node->Trafo;
        }
        const XMMATRIX transform = XMMatrixTranspose( XMLoadFloat4x4( &node->TrafoObjToCam ) );
        zCArrayAdapt<float3>* positions = visual->GetPositionList();
        const int submeshCount = visual->GetNumSubmeshes();
        if ( submeshCount <= 0 || submeshCount > 4096 ) continue;

        for ( int submeshIndex = 0; submeshIndex < submeshCount; ++submeshIndex ) {
            zCSubMesh* submesh = visual->GetSubmesh( submeshIndex );
            auto mesh = BuildProgMeshSubmesh(
                submesh, positions, &transform, false, submeshIndex );
            if ( !mesh || !submesh ) {
                LogWarn() << "Skipping invalid model submesh " << submeshIndex
                    << " on visual " << visualName;
                continue;
            }
            for ( const ExVertexStruct& vertex : mesh->Vertices ) {
                minimum.x = std::min( minimum.x, vertex.Position.x );
                minimum.y = std::min( minimum.y, vertex.Position.y );
                minimum.z = std::min( minimum.z, vertex.Position.z );
                maximum.x = std::max( maximum.x, vertex.Position.x );
                maximum.y = std::max( maximum.y, vertex.Position.y );
                maximum.z = std::max( maximum.z, vertex.Position.z );
                hasBounds = true;
            }

            MeshKey key{};
            key.Material = submesh->Material;
            key.Texture = key.Material ? key.Material->GetTextureSingle() : nullptr;
            key.Info = Engine::GAPI ? Engine::GAPI->GetMaterialInfoFrom( key.Texture ) : nullptr;
            auto& materialMeshes = meshInfo->Meshes[key.Material];
            auto& textureMeshes = meshInfo->MeshesByTexture[key];
            materialMeshes.reserve( materialMeshes.size() + 1 );
            textureMeshes.reserve( textureMeshes.size() + 1 );
            MeshInfo* rawMesh = mesh.get();
            materialMeshes.emplace_back( rawMesh );
            textureMeshes.emplace_back( rawMesh );
            if ( key.Texture && key.Texture->HasAlphaChannel() ) meshInfo->NeedsAlphaTesting = true;

            mesh.release();
        }
    }

    if ( hasBounds ) {
        meshInfo->BBox.Min = minimum;
        meshInfo->BBox.Max = maximum;
        XMStoreFloat( &meshInfo->MeshSize,
            XMVector3Length( XMLoadFloat3( &minimum ) - XMLoadFloat3( &maximum ) ) );
        XMStoreFloat3( &meshInfo->MidPoint,
            0.5f * (XMLoadFloat3( &minimum ) + XMLoadFloat3( &maximum )) );
    } else {
        meshInfo->BBox = {};
        meshInfo->MeshSize = 0.0f;
        meshInfo->MidPoint = {};
    }
    meshInfo->Visual = model;
    meshInfo->VisualName = visualName;
}
/** Extracts a zCProgMeshProto from a zCMesh */
void WorldConverter::ExtractProgMeshProtoFromMesh( zCMesh* mesh, MeshVisualInfo* meshInfo ) {
    ZoneScoped;
    if ( !mesh || !meshInfo ) return;
    zCPolygon** polygons = mesh->GetPolygons();
    const int polygonCount = mesh->GetNumPolygons();
    if ( !polygons || polygonCount <= 0 || polygonCount > 10000000 ) return;

    std::map<zCMaterial*, std::vector<ExVertexStruct>> materialVertices;
    std::vector<ExVertexStruct> polygonVertices;
    for ( int polygonIndex = 0; polygonIndex < polygonCount; ++polygonIndex ) {
        zCPolygon* polygon = polygons[polygonIndex];
        if ( !polygon || polygon->GetNumPolyVertices() < 3
            || !polygon->getVertices() || !polygon->getFeatures() ) continue;
        polygonVertices.clear();
        polygonVertices.reserve( static_cast<size_t>(polygon->GetNumPolyVertices()) );
        bool valid = true;
        for ( int vertexIndex = 0; vertexIndex < polygon->GetNumPolyVertices(); ++vertexIndex ) {
            zCVertex* sourceVertex = polygon->getVertices()[vertexIndex];
            zCVertFeature* feature = polygon->getFeatures()[vertexIndex];
            if ( !sourceVertex || !feature ) {
                valid = false;
                break;
            }
            ExVertexStruct vertex{};
            vertex.Position = sourceVertex->Position;
            vertex.Normal = feature->normal;
            vertex.TexCoord = feature->texCoord;
            vertex.TexCoord2 = float2( 0.0f, 0.0f );
            vertex.Color = feature->lightStatic;
            if ( !IsFiniteVertexData( vertex ) ) {
                valid = false;
                break;
            }
            polygonVertices.emplace_back( vertex );
        }
        if ( !valid ) continue;
        auto& vertices = materialVertices[polygon->GetMaterial()];
        const size_t generatedCount = (polygonVertices.size() - 2) * 3;
        if ( generatedCount > vertices.max_size() - vertices.size() ) continue;
        vertices.reserve( vertices.size() + generatedCount );
        TriangleFanToList( polygonVertices.data(),
            static_cast<unsigned int>(polygonVertices.size()), &vertices );
    }

    for ( auto& [material, sourceVertices] : materialVertices ) {
        std::vector<ExVertexStruct> vertices;
        std::vector<VERTEX_INDEX> indices;
        if ( !BuildIndexedVertices( sourceVertices, vertices, indices ) ) continue;
        auto convertedMesh = std::make_unique<MeshInfo>();
        convertedMesh->Vertices = std::move( vertices );
        convertedMesh->Indices = std::move( indices );
        D3D11VertexBuffer optimizer;
        if ( optimizer.OptimizeFaces( convertedMesh->Indices.data(),
                reinterpret_cast<byte*>(convertedMesh->Vertices.data()),
                static_cast<unsigned int>(convertedMesh->Indices.size()),
                static_cast<unsigned int>(convertedMesh->Vertices.size()), sizeof( ExVertexStruct ) ) != XR_SUCCESS
            || optimizer.OptimizeVertices( convertedMesh->Indices.data(),
                reinterpret_cast<byte*>(convertedMesh->Vertices.data()),
                static_cast<unsigned int>(convertedMesh->Indices.size()),
                static_cast<unsigned int>(convertedMesh->Vertices.size()), sizeof( ExVertexStruct ),
                &convertedMesh->ShadowIndices ) != XR_SUCCESS
            || !PublishMeshBuffers( convertedMesh.get(), convertedMesh->Vertices,
                convertedMesh->Indices )
            || !CreateShadowIndexBuffer( convertedMesh.get() ) ) continue;
        meshInfo->Meshes[material].emplace_back( convertedMesh.release() );
    }
    meshInfo->Visual = reinterpret_cast<zCVisual*>(mesh);
}
/** Extracts a node-visual */
void WorldConverter::ExtractNodeVisual( int index, zCModelNodeInst* node,
    gtl::flat_hash_map<int, std::vector<MeshVisualInfo*>>& attachments ) {
    ZoneScoped;
    if ( !node || !node->NodeVisual ) return;
    const char* extension = node->NodeVisual->GetFileExtension( 0 );
    if ( !extension ) return;

    auto visualInfo = std::make_unique<MeshVisualInfo>();
    const bool isMorphMesh = strcmp( extension, ".MMS" ) == 0;
    if ( isMorphMesh || strcmp( extension, ".3DS" ) == 0 ) {
        zCProgMeshProto* progMesh = isMorphMesh
            ? reinterpret_cast<zCMorphMesh*>(node->NodeVisual)->GetMorphMesh()
            : static_cast<zCProgMeshProto*>(node->NodeVisual);
        if ( !progMesh || progMesh->GetNumSubmeshes() <= 0 ) return;
        if ( isMorphMesh ) {
            visualInfo->MorphMeshVisual = node->NodeVisual;
            zCObject_AddRef( visualInfo->MorphMeshVisual );
        }
        Extract3DSMeshFromVisual2( progMesh, visualInfo.get() );
        if ( isMorphMesh ) visualInfo->Visual = node->NodeVisual;
    } else if ( strcmp( extension, ".MDS" ) == 0 || strcmp( extension, ".ASC" ) == 0 ) {
        ExtractProgMeshProtoFromModel( static_cast<zCModel*>(node->NodeVisual), visualInfo.get() );
    } else {
        return;
    }
    if ( visualInfo->Meshes.empty() ) return;

    auto& destination = attachments[index];
    for ( MeshVisualInfo* previous : destination ) delete previous;
    destination.clear();
    destination.emplace_back( visualInfo.release() );
}
/** Packs the previous-to-current local morph displacement into the otherwise unused vertex color. */
static DWORD PackPreviousMorphDelta( const XMFLOAT3& currentPosition, const XMFLOAT3& previousPosition ) {
    constexpr float morphDeltaRange = 16.0f;
    const auto packComponent = [=]( float delta ) {
        if ( !std::isfinite( delta ) ) return DWORD{ 128 };
        const float normalized = std::clamp( delta / morphDeltaRange, -1.0f, 1.0f );
        return static_cast<DWORD>(std::lround( normalized * 127.0f + 128.0f ));
    };

    const DWORD x = packComponent( previousPosition.x - currentPosition.x );
    const DWORD y = packComponent( previousPosition.y - currentPosition.y );
    const DWORD z = packComponent( previousPosition.z - currentPosition.z );
    return x | (y << 8) | (z << 16); // Alpha 0 marks valid morph history.
}

/** Updates a Morph-Mesh visual */
void WorldConverter::UpdateMorphMeshVisual( void* value, MeshVisualInfo* meshInfo ) {
    ZoneScoped;
    if ( !value || !meshInfo || !Engine::GAPI ) return;
    zCMorphMesh* visual = reinterpret_cast<zCMorphMesh*>(value);
    if ( auto* textureAnimation = visual->GetTexAniState() ) textureAnimation->UpdateTexList();

    const auto now = Engine::GAPI->GetTotalTimeDW();
    const auto previousUpdateTime = meshInfo->LastAniUpdateFrame;
    if ( previousUpdateTime == now ) return;
    const bool continuousMorphHistory = previousUpdateTime != 0
        && now >= previousUpdateTime && now - previousUpdateTime <= 250;
    meshInfo->LastAniUpdateFrame = now;

    visual->AdvanceAnis();
    visual->CalcVertexPositions();
    zCProgMeshProto* morphMesh = visual->GetMorphMesh();
    if ( !morphMesh ) return;
    zCArrayAdapt<float3>* positions = morphMesh->GetPositionList();
    const int submeshCount = morphMesh->GetNumSubmeshes();
    if ( !positions || !positions->Array || positions->NumInArray <= 0
        || submeshCount <= 0 || submeshCount > 4096 ) return;

    std::unordered_map<int, MeshInfo*> targets;
    for ( const auto& materialMeshes : meshInfo->Meshes ) {
        for ( MeshInfo* mesh : materialMeshes.second ) {
            if ( mesh ) targets.emplace( static_cast<int>(mesh->MeshIndex), mesh );
        }
    }

    for ( int submeshIndex = 0; submeshIndex < submeshCount; ++submeshIndex ) {
        const auto target = targets.find( submeshIndex );
        if ( target == targets.end() || !target->second || !target->second->MeshVertexBuffer ) continue;
        MeshInfo* targetMesh = target->second;
        zCSubMesh* submesh = morphMesh->GetSubmesh( submeshIndex );
        if ( !submesh || !submesh->WedgeList.Array || submesh->WedgeList.NumInArray <= 0
            || targetMesh->Vertices.size() != static_cast<size_t>(submesh->WedgeList.NumInArray) ) continue;

        std::vector<ExVertexStruct> vertices;
        std::vector<XMFLOAT3> currentPositions;
        vertices.reserve( static_cast<size_t>(submesh->WedgeList.NumInArray) );
        currentPositions.reserve( static_cast<size_t>(submesh->WedgeList.NumInArray) );
        const bool hasPreviousPositions = continuousMorphHistory
            && targetMesh->PreviousMorphPositions.size()
                == static_cast<size_t>(submesh->WedgeList.NumInArray);
        bool valid = true;
        for ( int vertexIndex = 0; vertexIndex < submesh->WedgeList.NumInArray; ++vertexIndex ) {
            const zTPMWedge& wedge = submesh->WedgeList.Array[vertexIndex];
            if ( wedge.position >= positions->NumInArray ) {
                valid = false;
                break;
            }
            const XMFLOAT3 currentPosition = positions->Array[wedge.position];
            const XMFLOAT3 previousPosition = hasPreviousPositions
                ? targetMesh->PreviousMorphPositions[vertexIndex] : currentPosition;
            ExVertexStruct vertex{};
            vertex.Position = currentPosition;
            vertex.Normal = wedge.normal;
            vertex.TexCoord = wedge.texUV;
            vertex.TexCoord2 = float2( 0.0f, 0.0f );
            vertex.Color = PackPreviousMorphDelta( currentPosition, previousPosition );
            if ( !IsFiniteVertexData( vertex ) ) {
                valid = false;
                break;
            }
            vertices.emplace_back( vertex );
            currentPositions.emplace_back( currentPosition );
        }
        const uint64_t byteSize = static_cast<uint64_t>(vertices.size()) * sizeof( ExVertexStruct );
        if ( !valid || vertices.empty() || byteSize > std::numeric_limits<UINT>::max() ) continue;
        if ( targetMesh->MeshVertexBuffer->UpdateBuffer(
                vertices.data(), static_cast<unsigned int>(byteSize) ) != XR_SUCCESS ) continue;
        targetMesh->Vertices = std::move( vertices );
        targetMesh->PreviousMorphPositions = std::move( currentPositions );
    }
}
/** Extracts a 3DS-Mesh from a zCVisual */
void WorldConverter::Extract3DSMeshFromVisual2( zCProgMeshProto* visual, MeshVisualInfo* meshInfo ) {
    ZoneScoped;
    if ( !visual || !meshInfo ) return;
    zCArrayAdapt<float3>* positions = visual->GetPositionList();
    const int submeshCount = visual->GetNumSubmeshes();
    if ( !positions || !positions->Array || positions->NumInArray <= 0
        || submeshCount <= 0 || submeshCount > 4096 ) return;

    XMFLOAT3 minimum( FLT_MAX, FLT_MAX, FLT_MAX );
    XMFLOAT3 maximum( -FLT_MAX, -FLT_MAX, -FLT_MAX );
    bool hasBounds = false;
    const bool dynamic = meshInfo->MorphMeshVisual != nullptr;
    for ( int submeshIndex = 0; submeshIndex < submeshCount; ++submeshIndex ) {
        zCSubMesh* submesh = visual->GetSubmesh( submeshIndex );
        auto mesh = BuildProgMeshSubmesh(
            submesh, positions, nullptr, dynamic, submeshIndex );
        if ( !mesh || !submesh ) {
            LogWarn() << "Skipping invalid visual submesh " << submeshIndex;
            continue;
        }
        for ( const ExVertexStruct& vertex : mesh->Vertices ) {
            minimum.x = std::min( minimum.x, vertex.Position.x );
            minimum.y = std::min( minimum.y, vertex.Position.y );
            minimum.z = std::min( minimum.z, vertex.Position.z );
            maximum.x = std::max( maximum.x, vertex.Position.x );
            maximum.y = std::max( maximum.y, vertex.Position.y );
            maximum.z = std::max( maximum.z, vertex.Position.z );
            hasBounds = true;
        }

        MeshKey key{};
        key.Material = submesh->Material;
        key.Texture = key.Material ? key.Material->GetTextureSingle() : nullptr;
        key.Info = Engine::GAPI ? Engine::GAPI->GetMaterialInfoFrom( key.Texture ) : nullptr;
        auto& materialMeshes = meshInfo->Meshes[key.Material];
        auto& textureMeshes = meshInfo->MeshesByTexture[key];
        materialMeshes.reserve( materialMeshes.size() + 1 );
        textureMeshes.reserve( textureMeshes.size() + 1 );
        MeshInfo* rawMesh = mesh.get();
        materialMeshes.emplace_back( rawMesh );
        textureMeshes.emplace_back( rawMesh );
        if ( key.Texture && key.Texture->HasAlphaChannel() ) meshInfo->NeedsAlphaTesting = true;

        mesh.release();
    }

    if ( hasBounds ) {
        meshInfo->BBox.Min = minimum;
        meshInfo->BBox.Max = maximum;
        XMStoreFloat( &meshInfo->MeshSize,
            XMVector3Length( XMLoadFloat3( &minimum ) - XMLoadFloat3( &maximum ) ) );
        XMStoreFloat3( &meshInfo->MidPoint,
            0.5f * (XMLoadFloat3( &minimum ) + XMLoadFloat3( &maximum )) );
    } else {
        meshInfo->BBox = {};
        meshInfo->MeshSize = 0.0f;
        meshInfo->MidPoint = {};
    }
    meshInfo->Visual = visual;
    const char* visualName = visual->GetObjectName();
    meshInfo->VisualName = visualName ? visualName : "";
}
constexpr float kVertexMergeEpsilon = 0.001f;

static int64_t QuantizeVertexComponent( float value ) {
    const double scaled = static_cast<double>(value) / kVertexMergeEpsilon;
    const double low = static_cast<double>(std::numeric_limits<int64_t>::min());
    const double high = static_cast<double>(std::numeric_limits<int64_t>::max());
    if ( scaled <= low ) return std::numeric_limits<int64_t>::min();
    if ( scaled >= high ) return std::numeric_limits<int64_t>::max();
    return static_cast<int64_t>(std::llround( scaled ));
}


struct CmpClass {
    bool operator()( const std::pair<ExVertexStruct, int>& left,
                     const std::pair<ExVertexStruct, int>& right ) const {
        const ExVertexStruct& a = left.first;
        const ExVertexStruct& b = right.first;
        return std::make_tuple(
            QuantizeVertexComponent( a.Position.x ), QuantizeVertexComponent( a.Position.y ),
            QuantizeVertexComponent( a.Position.z ), QuantizeVertexComponent( a.TexCoord.x ),
            QuantizeVertexComponent( a.TexCoord.y ) )
            < std::make_tuple(
                QuantizeVertexComponent( b.Position.x ), QuantizeVertexComponent( b.Position.y ),
                QuantizeVertexComponent( b.Position.z ), QuantizeVertexComponent( b.TexCoord.x ),
                QuantizeVertexComponent( b.TexCoord.y ) );
    }
};

/** Indexes the given vertex array */
void WorldConverter::IndexVertices( const ExVertexStruct* input, unsigned int numInputVertices, std::vector<ExVertexStruct>& outVertices, std::vector<VERTEX_INDEX>& outIndices ) {
    outVertices.clear();
    outIndices.clear();
    if ( !input || numInputVertices < 3 || (numInputVertices % 3) != 0 ) return;

    std::set<std::pair<ExVertexStruct, int>, CmpClass> vertices;
    std::vector<VERTEX_INDEX> indexed;
    indexed.reserve( numInputVertices );
    int nextIndex = 0;

    for ( unsigned int i = 0; i < numInputVertices; ++i ) {
        if ( !IsFiniteVertexData( input[i] ) ) {
            LogWarn() << "Ignoring mesh with non-finite vertex data.";
            return;
        }
        auto it = vertices.find( std::make_pair( input[i], 0 ) );
        if ( it != vertices.end() ) {
            indexed.emplace_back( static_cast<VERTEX_INDEX>(it->second) );
            continue;
        }
        if ( vertices.size() > std::numeric_limits<VERTEX_INDEX>::max() ) {
            LogWarn() << "Ignoring mesh that exceeds the 16-bit vertex-index limit.";
            return;
        }
        vertices.insert( std::make_pair( input[i], nextIndex ) );
        indexed.emplace_back( static_cast<VERTEX_INDEX>(nextIndex++) );
    }

    gtl::flat_hash_set<std::tuple<VERTEX_INDEX, VERTEX_INDEX, VERTEX_INDEX>> triangles;
    triangles.reserve( indexed.size() / 3 );
    std::vector<VERTEX_INDEX> cleanedIndices;
    cleanedIndices.reserve( indexed.size() );
    for ( size_t i = 0; i + 2 < indexed.size(); i += 3 ) {
        const auto triangle = std::make_tuple( indexed[i], indexed[i + 1], indexed[i + 2] );
        if ( triangles.find( triangle ) != triangles.end() ) continue;
        triangles.insert( triangle );
        cleanedIndices.emplace_back( indexed[i] );
        cleanedIndices.emplace_back( indexed[i + 1] );
        cleanedIndices.emplace_back( indexed[i + 2] );
    }

    std::vector<ExVertexStruct> indexedVertices( vertices.size() );
    for ( const auto& vertex : vertices ) {
        indexedVertices[static_cast<size_t>(vertex.second)] = vertex.first;
    }
    outVertices = std::move( indexedVertices );
    outIndices = std::move( cleanedIndices );
}
void WorldConverter::IndexVertices( const ExVertexStruct* input, unsigned int numInputVertices, std::vector<ExVertexStruct>& outVertices, std::vector<unsigned int>& outIndices ) {
    outVertices.clear();
    outIndices.clear();
    if ( !input || numInputVertices == 0 ) return;

    std::set<std::pair<ExVertexStruct, int>, CmpClass> vertices;
    std::vector<unsigned int> indexed;
    indexed.reserve( numInputVertices );
    unsigned int nextIndex = 0;
    for ( unsigned int i = 0; i < numInputVertices; ++i ) {
        if ( !IsFiniteVertexData( input[i] ) ) {
            LogWarn() << "Ignoring mesh with non-finite vertex data.";
            return;
        }
        auto it = vertices.find( std::make_pair( input[i], 0 ) );
        if ( it != vertices.end() ) {
            indexed.emplace_back( static_cast<unsigned int>(it->second) );
            continue;
        }
        vertices.insert( std::make_pair( input[i], static_cast<int>(nextIndex) ) );
        indexed.emplace_back( nextIndex++ );
    }

    std::vector<ExVertexStruct> indexedVertices( vertices.size() );
    for ( const auto& vertex : vertices ) {
        indexedVertices[static_cast<size_t>(vertex.second)] = vertex.first;
    }
    outVertices = std::move( indexedVertices );
    outIndices = std::move( indexed );
}
/** Computes vertex normals for a mesh with face normals */
void WorldConverter::GenerateVertexNormals( std::vector<ExVertexStruct>& vertices, std::vector<VERTEX_INDEX>& indices ) {
    if ( vertices.empty() || indices.size() < 3 ) return;
    std::vector<XMFLOAT3> normals( vertices.size(), XMFLOAT3( 0, 0, 0 ) );

    for ( size_t i = 0; i + 2 < indices.size(); i += 3 ) {
        const VERTEX_INDEX triangle[3] = { indices[i], indices[i + 1], indices[i + 2] };
        if ( triangle[0] >= vertices.size() || triangle[1] >= vertices.size()
            || triangle[2] >= vertices.size() ) continue;

        XMVECTOR positions[3] = {
            XMLoadFloat3( vertices[triangle[0]].Position.toXMFLOAT3() ),
            XMLoadFloat3( vertices[triangle[1]].Position.toXMFLOAT3() ),
            XMLoadFloat3( vertices[triangle[2]].Position.toXMFLOAT3() )
        };
        const XMVECTOR faceNormal = XMVector3Cross(
            positions[1] - positions[0], positions[2] - positions[0] );
        const float faceLengthSq = XMVectorGetX( XMVector3LengthSq( faceNormal ) );
        if ( !std::isfinite( faceLengthSq ) || faceLengthSq <= 1.0e-12f ) continue;

        for ( size_t corner = 0; corner < 3; ++corner ) {
            const XMVECTOR a = positions[(corner + 1) % 3] - positions[corner];
            const XMVECTOR b = positions[(corner + 2) % 3] - positions[corner];
            const float aLengthSq = XMVectorGetX( XMVector3LengthSq( a ) );
            const float bLengthSq = XMVectorGetX( XMVector3LengthSq( b ) );
            if ( aLengthSq <= 1.0e-12f || bLengthSq <= 1.0e-12f ) continue;
            float cosine = XMVectorGetX( XMVector3Dot( a, b ) )
                / std::sqrt( aLengthSq * bLengthSq );
            if ( !std::isfinite( cosine ) ) continue;
            cosine = std::clamp( cosine, -1.0f, 1.0f );
            const float weight = std::acos( cosine );
            XMVECTOR accumulated = XMLoadFloat3( &normals[triangle[corner]] );
            accumulated += faceNormal * weight;
            XMStoreFloat3( &normals[triangle[corner]], accumulated );
        }
    }

    for ( size_t i = 0; i < normals.size(); ++i ) {
        const XMVECTOR normal = XMLoadFloat3( &normals[i] );
        const float lengthSq = XMVectorGetX( XMVector3LengthSq( normal ) );
        if ( std::isfinite( lengthSq ) && lengthSq > 1.0e-12f ) {
            XMFLOAT3 normalized;
            XMStoreFloat3( &normalized, XMVector3Normalize( normal ) );
            vertices[i].Normal = normalized;
        } else if ( !std::isfinite( vertices[i].Normal.x )
            || !std::isfinite( vertices[i].Normal.y )
            || !std::isfinite( vertices[i].Normal.z ) ) {
            vertices[i].Normal = float3( 0.0f, 1.0f, 0.0f );
        }
    }
}
/** Marks the edges of the mesh */
void WorldConverter::MarkEdges( std::vector<ExVertexStruct>& vertices, std::vector<VERTEX_INDEX>& indices ) {

}

/** Builds a big vertexbuffer from the world sections */
void WorldConverter::WrapVertexBuffers( const std::list<std::vector<ExVertexStruct>*>& vertexBuffers,
    const std::list<std::vector<VERTEX_INDEX>*>& indexBuffers,
    std::vector<ExVertexStruct>& outVertices,
    std::vector<unsigned int>& outIndices,
    std::vector<unsigned int>& outOffsets ) {
    outVertices.clear();
    outIndices.clear();
    outOffsets.clear();
    if ( vertexBuffers.empty() || vertexBuffers.size() != indexBuffers.size() ) return;

    uint64_t totalVertices = 0;
    uint64_t totalIndices = 0;
    auto vertexIt = vertexBuffers.begin();
    auto indexIt = indexBuffers.begin();
    for ( ; vertexIt != vertexBuffers.end(); ++vertexIt, ++indexIt ) {
        const auto* vertices = *vertexIt;
        const auto* indices = *indexIt;
        if ( !vertices || !indices || vertices->empty() || indices->empty() ) return;
        if ( vertices->size() > std::numeric_limits<unsigned int>::max() - totalVertices
            || indices->size() > std::numeric_limits<unsigned int>::max() - totalIndices ) return;
        for ( VERTEX_INDEX index : *indices ) {
            if ( index >= vertices->size() ) return;
        }
        totalVertices += vertices->size();
        totalIndices += indices->size();
    }

    outVertices.reserve( static_cast<size_t>(totalVertices) );
    outIndices.reserve( static_cast<size_t>(totalIndices) );
    outOffsets.reserve( indexBuffers.size() + 1 );
    std::vector<unsigned int> vertexOffsets;
    vertexOffsets.reserve( vertexBuffers.size() + 1 );
    vertexOffsets.emplace_back( 0 );

    for ( const auto* vertices : vertexBuffers ) {
        outVertices.insert( outVertices.end(), vertices->begin(), vertices->end() );
        vertexOffsets.emplace_back(
            vertexOffsets.back() + static_cast<unsigned int>(vertices->size()) );
    }

    outOffsets.emplace_back( 0 );
    size_t meshIndex = 0;
    for ( const auto* indices : indexBuffers ) {
        const unsigned int vertexOffset = vertexOffsets[meshIndex++];
        for ( VERTEX_INDEX index : *indices ) {
            outIndices.emplace_back( vertexOffset + static_cast<unsigned int>(index) );
        }
        outOffsets.emplace_back(
            outOffsets.back() + static_cast<unsigned int>(indices->size()) );
    }
}
/** Caches a mesh */
XRESULT WorldConverter::CacheMesh(
    const std::map<std::string, std::vector<std::pair<std::vector<ExVertexStruct>, std::vector<VERTEX_INDEX>>>>& geometry,
    const std::string& file ) {
    if ( geometry.empty() || geometry.size() > MeshCacheFormat::MaxTextures || file.empty() ) {
        return XR_INVALID_ARG;
    }

    uint64_t decodedBytes = 0;
    for ( const auto& [textureName, submeshes] : geometry ) {
        if ( textureName.size() > MeshCacheFormat::MaxTextureNameBytes
            || submeshes.empty()
            || submeshes.size() > MeshCacheFormat::MaxSubmeshesPerTexture ) {
            return XR_INVALID_ARG;
        }

        for ( const auto& [vertices, indices] : submeshes ) {
            if ( vertices.empty() || indices.empty()
                || vertices.size() > MeshCacheFormat::MaxVerticesPerSubmesh
                || indices.size() > MeshCacheFormat::MaxIndicesPerSubmesh
                || indices.size() % 3u != 0 ) {
                return XR_INVALID_ARG;
            }

            const uint64_t vertexBytes =
                static_cast<uint64_t>(vertices.size()) * sizeof( ExVertexStruct );
            const uint64_t indexBytes =
                static_cast<uint64_t>(indices.size()) * sizeof( VERTEX_INDEX );
            if ( decodedBytes > MeshCacheFormat::MaxDecodedBytes - vertexBytes
                || decodedBytes + vertexBytes > MeshCacheFormat::MaxDecodedBytes - indexBytes ) {
                return XR_INVALID_ARG;
            }
            decodedBytes += vertexBytes + indexBytes;

            for ( const VERTEX_INDEX index : indices ) {
                if ( index >= vertices.size() ) {
                    return XR_INVALID_ARG;
                }
            }
        }
    }

    const std::string temporaryFile = file + ".tmp";
    FILE* output = nullptr;
    if ( fopen_s( &output, temporaryFile.c_str(), "wb" ) != 0 || !output ) {
        LogError() << "Could not create mesh cache: " << temporaryFile;
        return XR_FAILED;
    }

    bool writeSucceeded = true;
    auto writeBytes = [&]( const void* data, size_t size ) {
        if ( !writeSucceeded || (size != 0 && !data) ) {
            writeSucceeded = false;
            return;
        }
        if ( size != 0 && fwrite( data, 1, size, output ) != size ) {
            writeSucceeded = false;
        }
    };
    auto writeValue = [&]( const auto& value ) {
        writeBytes( &value, sizeof( value ) );
    };

    const int32_t version = MeshCacheFormat::CurrentVersion;
    const uint32_t magic = MeshCacheFormat::Magic;
    const uint32_t vertexStride = sizeof( ExVertexStruct );
    const uint32_t indexStride = sizeof( VERTEX_INDEX );
    const uint32_t textureCount = static_cast<uint32_t>(geometry.size());
    writeValue( version );
    writeValue( magic );
    writeValue( vertexStride );
    writeValue( indexStride );
    writeValue( textureCount );

    for ( const auto& [textureName, submeshes] : geometry ) {
        const uint32_t nameLength = static_cast<uint32_t>(textureName.size());
        const uint32_t submeshCount = static_cast<uint32_t>(submeshes.size());
        writeValue( nameLength );
        writeBytes( textureName.data(), textureName.size() );
        writeValue( submeshCount );

        for ( const auto& [vertices, indices] : submeshes ) {
            const uint32_t vertexCount = static_cast<uint32_t>(vertices.size());
            const uint32_t indexCount = static_cast<uint32_t>(indices.size());
            writeValue( vertexCount );
            writeBytes( vertices.data(), vertices.size() * sizeof( ExVertexStruct ) );
            writeValue( indexCount );
            writeBytes( indices.data(), indices.size() * sizeof( VERTEX_INDEX ) );
        }
    }

    if ( fflush( output ) != 0 ) {
        writeSucceeded = false;
    }
    if ( fclose( output ) != 0 ) {
        writeSucceeded = false;
    }
    output = nullptr;

    if ( !writeSucceeded ) {
        DeleteFileA( temporaryFile.c_str() );
        LogError() << "Mesh cache write failed: " << file;
        return XR_FAILED;
    }

    if ( !MoveFileExA(
        temporaryFile.c_str(), file.c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH ) ) {
        DeleteFileA( temporaryFile.c_str() );
        LogError() << "Mesh cache could not be published: " << file
            << " (Win32 error " << GetLastError() << ")";
        return XR_FAILED;
    }

    return XR_SUCCESS;
}
/** Updates a quadmark info */
void WorldConverter::UpdateQuadMarkInfo( QuadMarkInfo* info, zCQuadMark* mark,
    const float3& position ) {
    if ( !info || !mark || !std::isfinite( position.x )
        || !std::isfinite( position.y ) || !std::isfinite( position.z ) ) return;
    zCMesh* mesh = mark->GetQuadMesh();
    if ( !mesh ) return;
    const int polygonCount = mesh->GetNumPolygons();
    zCPolygon** polygons = mesh->GetPolygons();
    if ( !polygons || polygonCount <= 0 || polygonCount > 1000000 ) return;

    zCMaterial* material = mark->GetMaterial();
    if ( polygons[0] && polygons[0]->GetMaterial() ) material = polygons[0]->GetMaterial();
    std::vector<ExVertexStruct> quadVertices;
    std::vector<ExVertexStruct> polygonVertices;
    for ( int polygonIndex = 0; polygonIndex < polygonCount; ++polygonIndex ) {
        zCPolygon* polygon = polygons[polygonIndex];
        if ( !polygon || polygon->GetNumPolyVertices() < 3
            || !polygon->getVertices() || !polygon->getFeatures() ) continue;
        polygonVertices.clear();
        polygonVertices.reserve( static_cast<size_t>(polygon->GetNumPolyVertices()) );
        bool valid = true;
        for ( int vertexIndex = 0; vertexIndex < polygon->GetNumPolyVertices(); ++vertexIndex ) {
            zCVertex* sourceVertex = polygon->getVertices()[vertexIndex];
            zCVertFeature* feature = polygon->getFeatures()[vertexIndex];
            if ( !sourceVertex || !feature ) {
                valid = false;
                break;
            }
            ExVertexStruct vertex{};
            vertex.Position = sourceVertex->Position;
            vertex.Normal = feature->normal;
            vertex.TexCoord = float2(
                std::clamp( feature->texCoord.x, 0.0f, 1.0f ),
                std::clamp( feature->texCoord.y, 0.0f, 1.0f ) );
            vertex.TexCoord2 = float2( 0.0f, 0.0f );
            vertex.Color = material && (material->GetAlphaFunc() == zMAT_ALPHA_FUNC_MUL
                || material->GetAlphaFunc() == zMAT_ALPHA_FUNC_MUL2)
                ? 0xFFFFFFFF : feature->lightStatic;
            if ( !IsFiniteVertexData( vertex ) ) {
                valid = false;
                break;
            }
            polygonVertices.emplace_back( vertex );
        }
        if ( !valid ) continue;
        const size_t generatedCount = (polygonVertices.size() - 2) * 3;
        if ( generatedCount > quadVertices.max_size() - quadVertices.size() ) return;
        quadVertices.reserve( quadVertices.size() + generatedCount );
        TriangleFanToList( polygonVertices.data(),
            static_cast<unsigned int>(polygonVertices.size()), &quadVertices );
    }
    auto buffer = BuildGpuBuffer( quadVertices, D3D11VertexBuffer::B_VERTEXBUFFER );
    if ( !buffer ) return;
    info->Mesh = std::move( buffer );
    info->NumVertices = static_cast<int>(quadVertices.size());
    info->Position = position;
    info->Visual = mark;
}
/** Converts ExVertexStruct into a zCPolygon*-Attay */
void WorldConverter::ConvertExVerticesTozCPolygons(
    const std::vector<ExVertexStruct>& vertices,
    const std::vector<VERTEX_INDEX>& indices,
    zCMaterial* material,
    std::vector<zCPolygon*>& polyArray ) {
    if ( vertices.empty() || indices.size() < 3 ) return;
    std::vector<std::unique_ptr<zCPolygon>> generated;
    generated.reserve( indices.size() / 3 );
    for ( size_t i = 0; i + 2 < indices.size(); i += 3 ) {
        const VERTEX_INDEX triangle[3] = { indices[i], indices[i + 1], indices[i + 2] };
        if ( triangle[0] >= vertices.size() || triangle[1] >= vertices.size()
            || triangle[2] >= vertices.size()
            || !IsFiniteVertexData( vertices[triangle[0]] )
            || !IsFiniteVertexData( vertices[triangle[1]] )
            || !IsFiniteVertexData( vertices[triangle[2]] ) ) continue;

        std::unique_ptr<zCPolygon> polygon( new (std::nothrow) zCPolygon() );
        if ( !polygon ) return;
        polygon->Constructor();
        polygon->AllocVertPointers( 3 );
        try {
            polygon->AllocVertData();
        } catch ( const std::bad_alloc& ) {
            return;
        }
        zCVertex** polygonVertices = polygon->getVertices();
        zCVertFeature** features = polygon->getFeatures();
        if ( !polygonVertices || !features ) return;
        polygon->SetMaterial( material );
        bool valid = true;
        for ( int corner = 0; corner < 3; ++corner ) {
            if ( !polygonVertices[corner] || !features[corner] ) {
                valid = false;
                break;
            }
            const ExVertexStruct& source = vertices[triangle[corner]];
            polygonVertices[corner]->MyIndex = corner;
            polygonVertices[corner]->TransformedIndex = 0;
            polygonVertices[corner]->Position = source.Position;
            features[corner]->lightStatic = 0xFFFFFFFF;
            features[corner]->normal = source.Normal;
            features[corner]->texCoord = source.TexCoord;
        }
        if ( !valid ) return;
        std::swap( polygonVertices[1], polygonVertices[2] );
        std::swap( features[1], features[2] );
        polygon->CalcNormal();
        generated.emplace_back( std::move( polygon ) );
    }

    if ( generated.size() > polyArray.max_size() - polyArray.size() ) return;
    polyArray.reserve( polyArray.size() + generated.size() );
    for ( auto& polygon : generated ) polyArray.emplace_back( polygon.release() );
}