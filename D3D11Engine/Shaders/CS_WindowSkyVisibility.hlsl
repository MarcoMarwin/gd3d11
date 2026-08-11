// Conservative cached sky-path classification for City_Window glass.
// One 64-thread group scans one reduced-resolution screen column from top to
// bottom. A 4x4 source block containing any static-world pixel closes the path;
// only an entirely clear upper-screen block opens it again. This preserves the
// full-resolution safeguard's blocker-before-sky rule without repeating a
// multi-sample vertical walk for every glass fragment.

Texture2D<float> WindowSceneDepth : register(t0);
Texture2D<float> WindowWorldMask : register(t1);
RWTexture2D<float> WindowSkyVisibility : register(u0);

static const uint Reduction = 4u;
static const uint RowsPerGroup = 64u;

groupshared int ReachEvent[RowsPerGroup];
groupshared int CarryEvent;

int ClassifyReducedCell(uint2 sourceBase, uint2 sourceSize)
{
    // Any world coverage closes the complete reduced cell. Early-out makes
    // ordinary geometry substantially cheaper than clear-sky cells.
    [loop]
    for (uint offsetY = 0u; offsetY < Reduction; ++offsetY)
    {
        [unroll]
        for (uint offsetX = 0u; offsetX < Reduction; ++offsetX)
        {
            const uint2 sourcePixel = min(
                sourceBase + uint2(offsetX, offsetY), sourceSize - 1u);
            if (WindowWorldMask.Load(int3(sourcePixel, 0)) >= 0.5f)
                return -1;
        }
    }

    if (sourceBase.y >= sourceSize.y / 2u)
        return 0;

    [loop]
    for (uint offsetY = 0u; offsetY < Reduction; ++offsetY)
    {
        [unroll]
        for (uint offsetX = 0u; offsetX < Reduction; ++offsetX)
        {
            const uint2 sourcePixel = min(
                sourceBase + uint2(offsetX, offsetY), sourceSize - 1u);
            if (WindowSceneDepth.Load(int3(sourcePixel, 0)) > 1e-7f)
                return 0;
        }
    }

    return 1;
}

[numthreads(64, 1, 1)]
void CSMain(
    uint3 groupID : SV_GroupID,
    uint3 groupThreadID : SV_GroupThreadID)
{
    uint sourceWidth;
    uint sourceHeight;
    WindowSceneDepth.GetDimensions(sourceWidth, sourceHeight);

    uint targetWidth;
    uint targetHeight;
    WindowSkyVisibility.GetDimensions(targetWidth, targetHeight);

    const uint column = groupID.x;
    const uint lane = groupThreadID.x;
    if (column >= targetWidth || sourceWidth == 0u || sourceHeight == 0u)
        return;

    if (lane == 0u)
        CarryEvent = -1;
    GroupMemoryBarrierWithGroupSync();

    [loop]
    for (uint rowBase = 0u; rowBase < targetHeight; rowBase += RowsPerGroup)
    {
        const uint targetY = rowBase + lane;
        int eventValue = 0;
        if (targetY < targetHeight)
        {
            eventValue = ClassifyReducedCell(
                uint2(column * Reduction, targetY * Reduction),
                uint2(sourceWidth, sourceHeight));
        }
        ReachEvent[lane] = eventValue;
        GroupMemoryBarrierWithGroupSync();

        // Inclusive prefix propagation of the nearest non-neutral event. Six
        // stages resolve all 64 rows while the group stays fully parallel.
        [unroll]
        for (uint offset = 1u; offset < RowsPerGroup; offset <<= 1u)
        {
            const int ownEvent = ReachEvent[lane];
            const int inheritedEvent = lane >= offset
                ? ReachEvent[lane - offset] : 0;
            GroupMemoryBarrierWithGroupSync();
            if (ownEvent == 0)
                ReachEvent[lane] = inheritedEvent;
            GroupMemoryBarrierWithGroupSync();
        }

        const int priorCarry = CarryEvent;
        const int resolvedEvent = ReachEvent[lane] != 0
            ? ReachEvent[lane] : priorCarry;
        if (targetY < targetHeight)
            WindowSkyVisibility[uint2(column, targetY)] =
                resolvedEvent > 0 ? 1.0f : 0.0f;

        if (lane == 0u && ReachEvent[RowsPerGroup - 1u] != 0)
            CarryEvent = ReachEvent[RowsPerGroup - 1u];
        GroupMemoryBarrierWithGroupSync();
    }
}
