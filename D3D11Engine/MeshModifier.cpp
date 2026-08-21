#include "pch.h"
#include "MeshModifier.h"
MeshModifier::MeshModifier() {}

MeshModifier::~MeshModifier() {}

/** Performs catmul-clark smoothing on the mesh */
void MeshModifier::DoCatmulClark( const std::vector<ExVertexStruct>& inVertices, const std::vector<unsigned short>& inIndices, std::vector<ExVertexStruct>& outVertices, std::vector<unsigned short>& outIndices, int iterations ) {
}

/** Detects borders on the mesh */
void MeshModifier::DetectBorders( const std::vector<ExVertexStruct>& inVertices, const std::vector<unsigned short>& inIndices, std::vector<ExVertexStruct>& outVertices, std::vector<unsigned short>& outIndices ) {
}

/** Drops texcoords on the given mesh, making it appear crackless */
void MeshModifier::DropTexcoords( const std::vector<ExVertexStruct>& inVertices, const std::vector<unsigned short>& inIndices, std::vector<ExVertexStruct>& outVertices, std::vector<VERTEX_INDEX>& outIndices ) {
}

/** Decimates the mesh, reducing its complexity */
void MeshModifier::Decimate( const std::vector<ExVertexStruct>& inVertices, const std::vector<unsigned short>& inIndices, std::vector<ExVertexStruct>& outVertices, std::vector<VERTEX_INDEX>& outIndices ) {
}

struct PNAENEdge {
    // "An Edge should consist of the origin index, the destination index, the origin position and the destination position"
    unsigned int iO;
    unsigned int iD;

    float3 pO;
    float3 pD;

    // "Reverse simply flips the sense of the edge"
    void ReverseEdge() {
        std::swap( iO, iD );
        std::swap( pO, pD );
    }

    bool operator == ( const PNAENEdge& o ) const {
        if ( iO == o.iO && iD == o.iD )
            return true;

        if ( pO == o.pO && pD == o.pD )
            return true;

        return false;
    }

};

bool operator< ( const PNAENEdge& lhs, const PNAENEdge& rhs ) {
    return (lhs.iO < rhs.iO) || (lhs.iO == rhs.iO && lhs.iD < rhs.iD);
}

struct PNAENKeyHasher {
    static const size_t bucket_size = 10; // mean bucket size that the container should try not to exceed
    static const size_t min_buckets = (1 << 10); // minimum number of buckets, power of 2, >0

    static std::size_t hash_value( float value ) {
        std::hash<float> hasher;
        return hasher( value );
    }

    static void hash_combine( std::size_t& seed, float value ) {
        seed ^= hash_value( value ) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
    }

    std::size_t operator()( const PNAENEdge& k ) const {
        // Start with a hash value of 0    .
        std::size_t seed = 0;

        // Hash the position components; direction indices are intentionally excluded.

        hash_combine( seed, k.pO.x );
        hash_combine( seed, k.pO.y );
        hash_combine( seed, k.pO.z );
        hash_combine( seed, k.pD.x );
        hash_combine( seed, k.pD.y );
        hash_combine( seed, k.pD.z );
        // Return the result.
        return seed;
    }

};

// Helper struct which defines == for ExVertexStruct
struct Vertex {
    Vertex( ExVertexStruct* vx ) {
        this->vx = vx;
    }

    ExVertexStruct* vx;

    bool operator == ( const Vertex& o ) const {
        if ( vx->Position == o.vx->Position )
            return true;

        return false;
    }
};

struct VertexKeyHasher {
    static const size_t bucket_size = 10; // mean bucket size that the container should try not to exceed
    static const size_t min_buckets = (1 << 10); // minimum number of buckets, power of 2, >0

    static std::size_t hash_value( float value ) {
        std::hash<float> hasher;
        return hasher( value );
    }

    static void hash_combine( std::size_t& seed, float value ) {
        seed ^= hash_value( value ) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
    }

    std::size_t operator()( const Vertex& k ) const {
        // Start with a hash value of 0    .
        std::size_t seed = 0;

        // Modify 'seed' by XORing and bit-shifting in
        // one member of 'Key' after the other:
        hash_combine( seed, k.vx->Position.x );
        hash_combine( seed, k.vx->Position.y );
        hash_combine( seed, k.vx->Position.z );

        hash_combine( seed, k.vx->Position.x );
        hash_combine( seed, k.vx->Position.y );
        hash_combine( seed, k.vx->Position.z );

        // Return the result.
        return seed;
    }
};

struct Float3KeyHasher {
    static const size_t bucket_size = 10; // mean bucket size that the container should try not to exceed
    static const size_t min_buckets = (1 << 10); // minimum number of buckets, power of 2, >0

    static std::size_t hash_value( float value ) {
        std::hash<float> hasher;
        return hasher( value );
    }

    static void hash_combine( std::size_t& seed, float value ) {
        seed ^= hash_value( value ) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
    }

    std::size_t operator()( const float3& k ) const {
        // Start with a hash value of 0    .
        std::size_t seed = 0;

        // Modify 'seed' by XORing and bit-shifting in
        // one member of 'Key' after the other:
        hash_combine( seed, k.x );
        hash_combine( seed, k.y );
        hash_combine( seed, k.z );

        hash_combine( seed, k.x );
        hash_combine( seed, k.y );
        hash_combine( seed, k.z );

        // Return the result.
        return seed;
    }
};

bool TexcoordSame( float2 a, float2 b ) {
    if ( (abs( a.x - b.x ) > 0.001f &&
        abs( (a.x + 1) - b.x ) > 0.001f &&
        abs( (a.x - 1) - b.x ) > 0.001f) ||
        (abs( a.y - b.y ) > 0.001f &&
        abs( (a.y + 1) - b.y ) > 0.001f &&
        abs( (a.y - 1) - b.y ) > 0.001f) )
        return false;

    return true;
};

/** Computes smooth normals for the given mesh */
void MeshModifier::ComputeSmoothNormals( std::vector<ExVertexStruct>& inVertices ) {
    // Map to store adj. vertices
    std::unordered_map<Vertex, std::vector<ExVertexStruct*>, VertexKeyHasher> VertexMap;

    for ( unsigned int i = 0; i < inVertices.size(); i += 3 ) {
        for ( int x = 0; x < 3; x++ ) {
            // Put adj. vertices of this face together
            VertexMap[Vertex( &inVertices[i + x] )].push_back( &inVertices[i + x] );
        }
    }

    // Run through all the adj. vertices and average the normals between them
    for ( auto& [k, vx] : VertexMap ) {
        // Average all face normals
        XMFLOAT3 avgNormal;
        XMVECTOR XMV_avgNormal = XMVectorZero();
        for ( ExVertexStruct* vert : vx ) {
            XMV_avgNormal += XMLoadFloat3( vert->Normal.toXMFLOAT3() );
        }
        XMV_avgNormal /= static_cast<float>(vx.size());
        XMStoreFloat3( &avgNormal, XMV_avgNormal );
        // Lerp between the average and the face normal for every vertex
        for ( ExVertexStruct* vert : vx ) {
            // Find out if we are a corner/border vertex
            vert->TexCoord2.x = 1.0f;
            for ( ExVertexStruct* vert2 : vx ) {
                if ( !TexcoordSame( vert->TexCoord, vert2->TexCoord ) ) {
                    vert->TexCoord2.x = 0.0f;
                    break;
                }
            }

            vert->Normal = avgNormal;
        }
    }
}

/** Fills an index array for a non-indexed mesh */
void MeshModifier::FillIndexArrayFor( unsigned int numVertices, std::vector<unsigned int>& outIndices ) {
    for ( unsigned int i = 0; i < numVertices; i++ ) {
        outIndices.push_back( i );
    }
}

/** Fills an index array for a non-indexed mesh */
void MeshModifier::FillIndexArrayFor( unsigned int numVertices, std::vector<VERTEX_INDEX>& outIndices ) {
    for ( unsigned int i = 0; i < numVertices; i++ ) {
        outIndices.push_back( i );
    }
}
