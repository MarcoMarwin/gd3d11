#pragma once
#include "pch.h"
#include "HookedFunctions.h"
#include "zCPolygon.h"
#include "Engine.h"
#include "GothicAPI.h"
#include "zSTRING.h"
#include "zCArrayAdapt.h"
#include "zCVisual.h"

struct zTPMTriangle {
    VERTEX_INDEX wedge[3];
};

struct zTPMWedge {
    float3 normal;
    float2 texUV;
    VERTEX_INDEX position;
};

struct zTPMTriangleEdges {
    VERTEX_INDEX edge[3];
};

struct zTPMEdge {
    VERTEX_INDEX wedge[2];
};

struct zTPMVertexUpdate {
    VERTEX_INDEX numNewTri;
    VERTEX_INDEX numNewWedge;
};

class zCSubMesh {
public:
    zCMaterial* Material;
    zCArrayAdapt<zTPMTriangle>		TriList;
    zCArrayAdapt<zTPMWedge>			WedgeList;
    zCArrayAdapt<float>				ColorList;
    zCArrayAdapt<VERTEX_INDEX>	TriPlaneIndexList;
    zCArrayAdapt<zTPlane>			TriPlaneList;
    zCArrayAdapt<zTPMTriangleEdges>	TriEdgeList;
    zCArrayAdapt<zTPMEdge>			EdgeList;
    zCArrayAdapt<float>				EdgeScoreList;

    zCArrayAdapt<VERTEX_INDEX>	WedgeMap;
    zCArrayAdapt<zTPMVertexUpdate>	VertexUpdates;

    int vbStartIndex;

    static const unsigned int CLASS_SIZE = 0x58;
};

class zCProgMeshProto : public zCVisual {
public:

    zCArrayAdapt<float3>* GetPositionList() {
        return reinterpret_cast<zCArrayAdapt<float3>*>(THISPTR_OFFSET( GothicMemoryLocations::zCProgMeshProto::Offset_PositionList ));
    }

    zCArrayAdapt<float3>* GetNormalsList() {
        return reinterpret_cast<zCArrayAdapt<float3>*>(THISPTR_OFFSET( GothicMemoryLocations::zCProgMeshProto::Offset_NormalsList ));
    }

    zCSubMesh* GetSubmesh( int n ) {
        zCSubMesh* submeshes = GetSubmeshes();
        const int count = GetNumSubmeshes();
        if ( !submeshes || n < 0 || n >= count ) return nullptr;
        return reinterpret_cast<zCSubMesh*>(reinterpret_cast<uintptr_t>(submeshes)
            + static_cast<size_t>(zCSubMesh::CLASS_SIZE) * static_cast<size_t>(n));
    }

    zCSubMesh* GetSubmeshes() {
        return *reinterpret_cast<zCSubMesh**>(THISPTR_OFFSET( GothicMemoryLocations::zCProgMeshProto::Offset_Submeshes ));
    }

    int GetNumSubmeshes() {
        return *reinterpret_cast<int*>(THISPTR_OFFSET( GothicMemoryLocations::zCProgMeshProto::Offset_NumSubmeshes ));
    }

    /** Constructs a readable mesh from the data given in the progmesh */
    void ConstructVertexBuffer( std::vector<ExVertexStruct>* vertices ) {
        if ( !vertices ) return;
        vertices->clear();
        zCArrayAdapt<float3>* positions = GetPositionList();
        if ( !positions || !positions->Array || positions->NumInArray <= 0 ) return;
        vertices->reserve( static_cast<size_t>(positions->NumInArray) );
        for ( int i = 0; i < positions->NumInArray; ++i ) {
            ExVertexStruct vertex{};
            vertex.Position = positions->Array[i];
            vertex.Normal = float3( 0.0f, 0.0f, 0.0f );
            vertex.TexCoord = float2( 0.0f, 0.0f );
            vertex.TexCoord2 = float2( 0.0f, 0.0f );
            vertex.Color = 0xFFFFFFFF;
            vertices->emplace_back( vertex );
        }
    }
};
