#pragma once
#include "pch.h"
#include <DirectXMath.h>
#include <algorithm>
#include <cstdint>
#include <limits>
#include <unordered_map>
#include <vector>

class zCBspTree;
class zCBspBase;
class zCBspSector;
class zCPolygon;
class zCVob;
struct BspInfo;

struct ScreenBox2D
{
    float MinX = 1.0f;
    float MinY = 1.0f;
    float MaxX = -1.0f;
    float MaxY = -1.0f;

    static ScreenBox2D FullViewport()
    {
        ScreenBox2D box;
        box.MinX = -1.0f;
        box.MinY = -1.0f;
        box.MaxX = 1.0f;
        box.MaxY = 1.0f;
        return box;
    }

    bool IsEmpty() const
    {
        return MinX > MaxX || MinY > MaxY;
    }

    void Add(float x, float y)
    {
        MinX = std::min(MinX, x);
        MinY = std::min(MinY, y);
        MaxX = std::max(MaxX, x);
        MaxY = std::max(MaxY, y);
    }

    void Merge(const ScreenBox2D& other)
    {
        if (other.IsEmpty())
        {
            return;
        }

        if (IsEmpty())
        {
            *this = other;
            return;
        }

        MinX = std::min(MinX, other.MinX);
        MinY = std::min(MinY, other.MinY);
        MaxX = std::max(MaxX, other.MaxX);
        MaxY = std::max(MaxY, other.MaxY);
    }

    bool Contains(const ScreenBox2D& other) const
    {
        if (other.IsEmpty())
        {
            return true;
        }

        return !IsEmpty()
            && MinX <= other.MinX
            && MinY <= other.MinY
            && MaxX >= other.MaxX
            && MaxY >= other.MaxY;
    }

    bool Overlaps(const ScreenBox2D& other) const
    {
        return !IsEmpty()
            && !other.IsEmpty()
            && MinX <= other.MaxX
            && MaxX >= other.MinX
            && MinY <= other.MaxY
            && MaxY >= other.MinY;
    }

    ScreenBox2D ClippedTo(const ScreenBox2D& other) const
    {
        ScreenBox2D clipped;
        clipped.MinX = std::max(MinX, other.MinX);
        clipped.MinY = std::max(MinY, other.MinY);
        clipped.MaxX = std::min(MaxX, other.MaxX);
        clipped.MaxY = std::min(MaxY, other.MaxY);
        return clipped;
    }
};

class BspPortalCuller
{
public:
    static constexpr uint16_t SECTOR_OUTDOOR = 0xFFFF;

    void Clear();
    void BuildFromWorld(zCBspTree* tree);

    bool IsActive() const
    {
        return Enabled && !Sectors.empty();
    }

    void SetEnabled(bool enabled)
    {
        Enabled = enabled;
    }

    void SetNearSectorRadius(float units)
    {
        NearSectorRadius = std::max(0.0f, units);
    }

    void XM_CALLCONV Solve(DirectX::FXMMATRIX worldToClip, const DirectX::XMFLOAT3& cameraPosition, zCVob* cameraVob);

    bool IsLeafVisible(const BspInfo& leaf) const;
    bool IsBoxVisibleInLeafSectors(const BspInfo& leaf, const DirectX::XMFLOAT3& bbMin, const DirectX::XMFLOAT3& bbMax) const;

    struct Stats
    {
        int NumSectors = 0;
        int NumPortals = 0;
        int ActiveSectors = 0;
        int UnreachableSectors = 0;
        bool CameraOutdoor = true;
        uint16_t CameraSector = SECTOR_OUTDOOR;
    };

    const Stats& GetStats() const
    {
        return LastStats;
    }

private:
    struct Portal
    {
        std::vector<DirectX::XMFLOAT3> Verts;
        DirectX::XMFLOAT3 PlaneNormal = {};
        float PlaneDistance = 0.0f;
        uint16_t TargetSector = SECTOR_OUTDOOR;
    };

    struct Sector
    {
        std::vector<uint32_t> OutgoingPortals;
        DirectX::XMFLOAT3 BoundsMin = DirectX::XMFLOAT3(
            std::numeric_limits<float>::max(),
            std::numeric_limits<float>::max(),
            std::numeric_limits<float>::max());
        DirectX::XMFLOAT3 BoundsMax = DirectX::XMFLOAT3(
            -std::numeric_limits<float>::max(),
            -std::numeric_limits<float>::max(),
            -std::numeric_limits<float>::max());
        bool HasBounds = false;
        bool AlwaysActive = false;
    };

    void BuildReachability();
    zCBspBase* FindLeaf(const DirectX::XMFLOAT3& position) const;
    static ScreenBox2D XM_CALLCONV ProjectPolygon(DirectX::FXMMATRIX worldToClip, const DirectX::XMFLOAT3* verts, size_t numVerts);
    void ActivateSector(uint16_t sector, const ScreenBox2D& aperture, uint16_t cameFrom, int depth);
    bool IsSectorActive(uint16_t sector) const;

    bool Enabled = true;
    float NearSectorRadius = 450.0f;
    std::vector<Portal> Portals;
    std::vector<Sector> Sectors;
    std::unordered_map<zCBspSector*, uint16_t> SectorIdByPtr;
    zCBspBase* BspRoot = nullptr;
    std::vector<uint32_t> OutdoorEntryPortals;
    std::vector<uint32_t> Stamps;
    std::vector<ScreenBox2D> Apertures;
    uint32_t CurrentStamp = 0;
    bool WarnedBudget = false;
    int VisitBudget = 0;
    DirectX::XMMATRIX SolveWorldToClip = DirectX::XMMatrixIdentity();
    DirectX::XMFLOAT3 SolveCameraPos = {};
    Stats LastStats;
};
