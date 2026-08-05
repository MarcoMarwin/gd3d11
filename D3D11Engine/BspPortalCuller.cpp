#include "pch.h"
#include "BspPortalCuller.h"
#include "GothicAPI.h"
#include "Engine.h"
#include "Logger.h"
#include "zCBspTree.h"
#include "zCMaterial.h"
#include "zCPolygon.h"
#include "zCVob.h"
#include <algorithm>

using namespace DirectX;

namespace
{
    constexpr int MAX_SECTOR_DEPTH = 40;
    constexpr int MAX_SECTOR_VISITS = 4096;
    constexpr float CLIP_W_EPSILON = 1e-4f;
}

void BspPortalCuller::Clear()
{
    Portals.clear();
    Sectors.clear();
    SectorIdByPtr.clear();
    BspRoot = nullptr;
    OutdoorEntryPortals.clear();
    Stamps.clear();
    Apertures.clear();
    CurrentStamp = 0;
    WarnedBudget = false;
    VisitBudget = 0;
    LastStats = Stats{};
}

void BspPortalCuller::BuildFromWorld(zCBspTree* tree)
{
    Clear();

    if (!Enabled || !tree)
    {
        return;
    }

    if (tree->GetBspTreeMode() != zBSP_MODE_OUTDOOR)
    {
        return;
    }

    zCArray<zCBspSector*>& sectorList = tree->GetSectorList();
    if (sectorList.NumInArray <= 0 || !sectorList.Array)
    {
        return;
    }

    const int numSectors = sectorList.NumInArray;
    if (numSectors >= SECTOR_OUTDOOR)
    {
        LogWarn() << "BspPortalCuller: world has " << numSectors << " sectors, portal culling disabled";
        Clear();
        return;
    }

    Sectors.resize(numSectors);
    BspRoot = tree->GetRootNode();
    SectorIdByPtr.reserve(static_cast<size_t>(numSectors) * 2);

    for (int i = 0; i < numSectors; ++i)
    {
        if (sectorList.Array[i])
        {
            SectorIdByPtr.emplace(sectorList.Array[i], static_cast<uint16_t>(i));
        }
    }

    auto idOf = [&](zCBspSector* sector) -> uint16_t
    {
        if (!sector)
        {
            return SECTOR_OUTDOOR;
        }

        auto it = SectorIdByPtr.find(sector);
        return it != SectorIdByPtr.end() ? it->second : SECTOR_OUTDOOR;
    };

    int numLeafsTagged = 0;

    for (int i = 0; i < numSectors; ++i)
    {
        zCBspSector* sector = sectorList.Array[i];
        if (!sector)
        {
            continue;
        }

        const uint16_t sectorId = static_cast<uint16_t>(i);
        Sector& currentSector = Sectors[sectorId];

        zCArray<zCBspBase*>& nodes = sector->GetSectorNodes();
        for (int n = 0; n < nodes.NumInArray; ++n)
        {
            zCBspBase* leaf = nodes.Array[n];
            if (!leaf)
            {
                continue;
            }

            BspInfo* info = Engine::GAPI->GetNewBspNode(leaf);
            if (!info)
            {
                continue;
            }

            if (std::find(info->SectorIds.begin(), info->SectorIds.end(), sectorId) == info->SectorIds.end())
            {
                info->SectorIds.push_back(sectorId);
                ++numLeafsTagged;
            }

            currentSector.BoundsMin.x = std::min(currentSector.BoundsMin.x, leaf->BBox3D.Min.x);
            currentSector.BoundsMin.y = std::min(currentSector.BoundsMin.y, leaf->BBox3D.Min.y);
            currentSector.BoundsMin.z = std::min(currentSector.BoundsMin.z, leaf->BBox3D.Min.z);
            currentSector.BoundsMax.x = std::max(currentSector.BoundsMax.x, leaf->BBox3D.Max.x);
            currentSector.BoundsMax.y = std::max(currentSector.BoundsMax.y, leaf->BBox3D.Max.y);
            currentSector.BoundsMax.z = std::max(currentSector.BoundsMax.z, leaf->BBox3D.Max.z);
            currentSector.HasBounds = true;
        }

        zCArray<zCPolygon*>& portals = sector->GetSectorPortals();
        for (int p = 0; p < portals.NumInArray; ++p)
        {
            zCPolygon* poly = portals.Array[p];
            if (!poly || !poly->IsPortal())
            {
                continue;
            }

            zCMaterial* mat = poly->GetMaterial();
            if (!mat)
            {
                continue;
            }

            const uint8_t numVerts = poly->GetNumPolyVertices();
            zCVertex** verts = poly->getVertices();
            if (numVerts < 3 || !verts)
            {
                continue;
            }

            Portal portal;
            portal.Verts.reserve(numVerts);

            bool vertsOk = true;
            for (uint8_t v = 0; v < numVerts; ++v)
            {
                if (!verts[v])
                {
                    vertsOk = false;
                    break;
                }

                const auto& pos = verts[v]->Position;
                portal.Verts.emplace_back(pos.x, pos.y, pos.z);
            }

            if (!vertsOk)
            {
                continue;
            }

            const zTPlane& plane = poly->GetPolyPlane();
            portal.PlaneNormal = plane.Normal;
            portal.PlaneDistance = plane.Distance;

            zCBspSector* front = mat->GetBspSectorFront();
            zCBspSector* back = mat->GetBspSectorBack();

            if (!front)
            {
                portal.TargetSector = idOf(back);
                if (portal.TargetSector == SECTOR_OUTDOOR)
                {
                    continue;
                }

                OutdoorEntryPortals.push_back(static_cast<uint32_t>(Portals.size()));
                Portals.push_back(std::move(portal));
            }
            else
            {
                portal.TargetSector = idOf(back);
                const uint32_t portalIdx = static_cast<uint32_t>(Portals.size());
                Portals.push_back(std::move(portal));

                const uint16_t owner = idOf(front);
                Sectors[owner != SECTOR_OUTDOOR ? owner : sectorId].OutgoingPortals.push_back(portalIdx);
            }
        }
    }

    Stamps.assign(Sectors.size(), 0);
    Apertures.assign(Sectors.size(), ScreenBox2D{});
    BuildReachability();

    LastStats.NumSectors = numSectors;
    LastStats.NumPortals = static_cast<int>(Portals.size());

    LogInfo() << "BspPortalCuller: " << numSectors << " sectors, " << Portals.size()
              << " portals, " << numLeafsTagged << " leaf sector links";
}

void BspPortalCuller::BuildReachability()
{
    std::vector<uint16_t> queue;
    std::vector<bool> reached(Sectors.size(), false);

    for (uint32_t portalIdx : OutdoorEntryPortals)
    {
        const uint16_t target = Portals[portalIdx].TargetSector;
        if (target < Sectors.size() && !reached[target])
        {
            reached[target] = true;
            queue.push_back(target);
        }
    }

    while (!queue.empty())
    {
        const uint16_t sector = queue.back();
        queue.pop_back();

        for (uint32_t portalIdx : Sectors[sector].OutgoingPortals)
        {
            const uint16_t target = Portals[portalIdx].TargetSector;
            if (target < Sectors.size() && !reached[target])
            {
                reached[target] = true;
                queue.push_back(target);
            }
        }
    }

    int unreachable = 0;
    for (size_t i = 0; i < Sectors.size(); ++i)
    {
        if (!reached[i])
        {
            Sectors[i].AlwaysActive = true;
            ++unreachable;
        }
    }

    LastStats.UnreachableSectors = unreachable;
}

zCBspBase* BspPortalCuller::FindLeaf(const XMFLOAT3& position) const
{
    zCBspBase* node = BspRoot;
    int guard = 256;

    while (node && !node->IsLeaf() && --guard > 0)
    {
        zCBspNode* bspNode = static_cast<zCBspNode*>(node);
        const float side = bspNode->Plane.Normal.x * position.x
            + bspNode->Plane.Normal.y * position.y
            + bspNode->Plane.Normal.z * position.z
            - bspNode->Plane.Distance;

        zCBspBase* next = side > 0.0f ? bspNode->Front : bspNode->Back;
        if (!next)
        {
            break;
        }

        node = next;
    }

    return node && node->IsLeaf() ? node : nullptr;
}

ScreenBox2D XM_CALLCONV BspPortalCuller::ProjectPolygon(FXMMATRIX worldToClip, const XMFLOAT3* verts, size_t numVerts)
{
    ScreenBox2D box;
    if (!verts || numVerts < 3)
    {
        return box;
    }

    XMVECTOR prev = XMVector4Transform(XMVectorSetW(XMLoadFloat3(&verts[numVerts - 1]), 1.0f), worldToClip);
    float prevW = XMVectorGetW(prev);
    bool prevIn = prevW > CLIP_W_EPSILON;

    for (size_t i = 0; i < numVerts; ++i)
    {
        XMVECTOR cur = XMVector4Transform(XMVectorSetW(XMLoadFloat3(&verts[i]), 1.0f), worldToClip);
        const float curW = XMVectorGetW(cur);
        const bool curIn = curW > CLIP_W_EPSILON;

        if (curIn != prevIn)
        {
            const float t = (CLIP_W_EPSILON - prevW) / (curW - prevW);
            XMVECTOR mid = XMVectorLerp(prev, cur, t);
            const float midW = XMVectorGetW(mid);
            if (midW > CLIP_W_EPSILON)
            {
                box.Add(XMVectorGetX(mid) / midW, XMVectorGetY(mid) / midW);
            }
        }

        if (curIn)
        {
            box.Add(XMVectorGetX(cur) / curW, XMVectorGetY(cur) / curW);
        }

        prev = cur;
        prevW = curW;
        prevIn = curIn;
    }

    if (box.IsEmpty())
    {
        return box;
    }

    box.MinX = std::clamp(box.MinX, -1.0f, 1.0f);
    box.MinY = std::clamp(box.MinY, -1.0f, 1.0f);
    box.MaxX = std::clamp(box.MaxX, -1.0f, 1.0f);
    box.MaxY = std::clamp(box.MaxY, -1.0f, 1.0f);
    return box;
}

bool BspPortalCuller::IsSectorActive(uint16_t sector) const
{
    return sector < Stamps.size() && Stamps[sector] == CurrentStamp;
}

void BspPortalCuller::ActivateSector(uint16_t sector, const ScreenBox2D& aperture, uint16_t cameFrom, int depth)
{
    if (sector >= Sectors.size() || aperture.IsEmpty())
    {
        return;
    }

    if (depth > MAX_SECTOR_DEPTH)
    {
        return;
    }

    if (--VisitBudget < 0)
    {
        if (!WarnedBudget)
        {
            LogWarn() << "BspPortalCuller: sector visit budget reached";
            WarnedBudget = true;
        }
        return;
    }

    const bool firstVisit = Stamps[sector] != CurrentStamp;
    if (firstVisit)
    {
        Stamps[sector] = CurrentStamp;
        Apertures[sector] = aperture;
    }
    else
    {
        if (Apertures[sector].Contains(aperture))
        {
            return;
        }
        Apertures[sector].Merge(aperture);
    }

    for (uint32_t portalIdx : Sectors[sector].OutgoingPortals)
    {
        const Portal& portal = Portals[portalIdx];
        const float side = portal.PlaneNormal.x * SolveCameraPos.x
            + portal.PlaneNormal.y * SolveCameraPos.y
            + portal.PlaneNormal.z * SolveCameraPos.z
            - portal.PlaneDistance;

        if (side < 0.0f)
        {
            continue;
        }

        ScreenBox2D portalBox = ProjectPolygon(SolveWorldToClip, portal.Verts.data(), portal.Verts.size());
        if (portalBox.IsEmpty())
        {
            continue;
        }

        portalBox = portalBox.ClippedTo(aperture);
        if (portalBox.IsEmpty())
        {
            continue;
        }

        if (portal.TargetSector == SECTOR_OUTDOOR || portal.TargetSector == cameFrom)
        {
            continue;
        }

        ActivateSector(portal.TargetSector, portalBox, sector, depth + 1);
    }
}

void XM_CALLCONV BspPortalCuller::Solve(FXMMATRIX worldToClip, const XMFLOAT3& cameraPosition, zCVob* cameraVob)
{
    if (!IsActive())
    {
        return;
    }

    ZoneScopedN("BspPortalCuller::Solve");

    ++CurrentStamp;
    if (CurrentStamp == 0)
    {
        std::fill(Stamps.begin(), Stamps.end(), 0u);
        CurrentStamp = 1;
    }

    SolveWorldToClip = worldToClip;
    SolveCameraPos = cameraPosition;
    VisitBudget = MAX_SECTOR_VISITS;

    uint16_t cameraSector = SECTOR_OUTDOOR;
    bool cameraOutdoor = true;
    bool ambiguous = false;

    static thread_local std::vector<uint16_t> cameraSectors;
    cameraSectors.clear();

    if (!cameraVob)
    {
        ambiguous = true;
    }
    else
    {
        if (zCPolygon* ground = cameraVob->GetGroundPoly())
        {
            if (!ground->IsPortal())
            {
                if (zCMaterial* mat = ground->GetMaterial())
                {
                    if (zCBspSector* front = mat->GetBspSectorFront())
                    {
                        auto it = SectorIdByPtr.find(front);
                        if (it != SectorIdByPtr.end())
                        {
                            cameraSectors.push_back(it->second);
                        }
                    }
                }
            }
        }

        if (zCBspBase* leaf = FindLeaf(cameraPosition))
        {
            if (BspInfo* info = Engine::GAPI->GetNewBspNode(leaf))
            {
                for (uint16_t sector : info->SectorIds)
                {
                    if (std::find(cameraSectors.begin(), cameraSectors.end(), sector) == cameraSectors.end())
                    {
                        cameraSectors.push_back(sector);
                    }
                }
            }
        }

        if (!cameraSectors.empty())
        {
            cameraOutdoor = false;
            cameraSector = cameraSectors[0];
        }
    }

    const ScreenBox2D fullScreen = ScreenBox2D::FullViewport();

    if (!ambiguous && NearSectorRadius > 0.0f)
    {
        const float radiusSq = NearSectorRadius * NearSectorRadius;
        for (size_t i = 0; i < Sectors.size(); ++i)
        {
            const Sector& sector = Sectors[i];
            if (!sector.HasBounds)
            {
                continue;
            }

            const float dx = std::max(0.0f, std::max(sector.BoundsMin.x - cameraPosition.x, cameraPosition.x - sector.BoundsMax.x));
            const float dy = std::max(0.0f, std::max(sector.BoundsMin.y - cameraPosition.y, cameraPosition.y - sector.BoundsMax.y));
            const float dz = std::max(0.0f, std::max(sector.BoundsMin.z - cameraPosition.z, cameraPosition.z - sector.BoundsMax.z));

            if (dx * dx + dy * dy + dz * dz < radiusSq)
            {
                ActivateSector(static_cast<uint16_t>(i), fullScreen, SECTOR_OUTDOOR, 0);
            }
        }
    }

    if (ambiguous)
    {
        std::fill(Stamps.begin(), Stamps.end(), CurrentStamp);
        std::fill(Apertures.begin(), Apertures.end(), fullScreen);
    }
    else if (!cameraOutdoor)
    {
        for (uint16_t sector : cameraSectors)
        {
            ActivateSector(sector, fullScreen, SECTOR_OUTDOOR, 0);
        }
    }
    else
    {
        for (uint32_t portalIdx : OutdoorEntryPortals)
        {
            const Portal& portal = Portals[portalIdx];
            const float side = portal.PlaneNormal.x * cameraPosition.x
                + portal.PlaneNormal.y * cameraPosition.y
                + portal.PlaneNormal.z * cameraPosition.z
                - portal.PlaneDistance;

            if (side < 0.0f)
            {
                continue;
            }

            ScreenBox2D portalBox = ProjectPolygon(worldToClip, portal.Verts.data(), portal.Verts.size());
            if (!portalBox.IsEmpty())
            {
                ActivateSector(portal.TargetSector, portalBox, SECTOR_OUTDOOR, 0);
            }
        }
    }

    for (size_t i = 0; i < Sectors.size(); ++i)
    {
        if (Sectors[i].AlwaysActive)
        {
            Stamps[i] = CurrentStamp;
            Apertures[i] = fullScreen;
        }
    }

    int active = 0;
    for (uint32_t stamp : Stamps)
    {
        if (stamp == CurrentStamp)
        {
            ++active;
        }
    }

    LastStats.ActiveSectors = active;
    LastStats.CameraOutdoor = cameraOutdoor;
    LastStats.CameraSector = cameraSector;

    ZoneText("activeSectors", std::size("activeSectors") - 1);
    ZoneValue(active);
}

bool BspPortalCuller::IsLeafVisible(const BspInfo& leaf) const
{
    if (leaf.SectorIds.empty())
    {
        return true;
    }

    for (uint16_t sector : leaf.SectorIds)
    {
        if (IsSectorActive(sector))
        {
            return true;
        }
    }

    return false;
}

bool BspPortalCuller::IsBoxVisibleInLeafSectors(const BspInfo& leaf, const XMFLOAT3& bbMin, const XMFLOAT3& bbMax) const
{
    if (leaf.SectorIds.empty())
    {
        return true;
    }

    ScreenBox2D aperture;
    for (uint16_t sector : leaf.SectorIds)
    {
        if (IsSectorActive(sector))
        {
            aperture.Merge(Apertures[sector]);
        }
    }

    if (aperture.IsEmpty())
    {
        return false;
    }

    if (aperture.MinX <= -1.0f && aperture.MinY <= -1.0f && aperture.MaxX >= 1.0f && aperture.MaxY >= 1.0f)
    {
        return true;
    }

    const XMFLOAT3 corners[8] = {
        { bbMin.x, bbMin.y, bbMin.z }, { bbMax.x, bbMin.y, bbMin.z },
        { bbMin.x, bbMax.y, bbMin.z }, { bbMax.x, bbMax.y, bbMin.z },
        { bbMin.x, bbMin.y, bbMax.z }, { bbMax.x, bbMin.y, bbMax.z },
        { bbMin.x, bbMax.y, bbMax.z }, { bbMax.x, bbMax.y, bbMax.z },
    };

    ScreenBox2D boxOnScreen;
    for (const XMFLOAT3& corner : corners)
    {
        XMVECTOR clip = XMVector4Transform(XMVectorSetW(XMLoadFloat3(&corner), 1.0f), SolveWorldToClip);
        const float w = XMVectorGetW(clip);
        if (w <= CLIP_W_EPSILON)
        {
            return true;
        }

        boxOnScreen.Add(XMVectorGetX(clip) / w, XMVectorGetY(clip) / w);
    }

    if (boxOnScreen.IsEmpty())
    {
        return false;
    }

    return boxOnScreen.Overlaps(aperture);
}
