Texture2D<float> DepthInput : register(t0);
RWTexture2D<float> HZBOutput : register(u0);

cbuffer HZBConstants : register(b0)
{
    uint OutputWidth;
    uint OutputHeight;
    uint InputWidth;
    uint InputHeight;
};

[numthreads(8, 8, 1)]
void CSMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= OutputWidth || dispatchThreadId.y >= OutputHeight)
        return;

    uint2 startPixel = uint2(
        (dispatchThreadId.x * InputWidth) / OutputWidth,
        (dispatchThreadId.y * InputHeight) / OutputHeight);
    uint2 endPixel = uint2(
        ((dispatchThreadId.x + 1) * InputWidth + OutputWidth - 1) / OutputWidth,
        ((dispatchThreadId.y + 1) * InputHeight + OutputHeight - 1) / OutputHeight);

    endPixel = max(endPixel, startPixel + 1);
    endPixel.x = min(endPixel.x, InputWidth);
    endPixel.y = min(endPixel.y, InputHeight);

    float conservativeDepth = 1.0f;
    for (uint y = startPixel.y; y < endPixel.y; ++y)
    {
        for (uint x = startPixel.x; x < endPixel.x; ++x)
        {
            conservativeDepth = min(conservativeDepth, DepthInput.Load(int3(x, y, 0)));
        }
    }

    HZBOutput[dispatchThreadId.xy] = conservativeDepth;
}