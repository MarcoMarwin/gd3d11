#include "D3D11RenderQueue.h"

XRESULT D3D11RenderQueue::Init()
{
    return XR_SUCCESS;
}

void D3D11RenderQueue::Reset()
{
    vobs.clear();
    skeltalVobs.clear();
    lights.clear();
    transparent.clear();
}

XRESULT D3D11RenderQueue::ProcessQueue()
{
    return XR_SUCCESS;
}


void D3D11RenderQueue::PushStaticVob( VobInfo* vobInfo )
{
    vobs.emplace_back( vobInfo );
    // TODO: actually create renderItem in here, ready for indexed/instanced drawing.
}

void D3D11RenderQueue::PushSkeletalVob( SkeletalVobInfo* vobInfo )
{
    skeltalVobs.emplace_back( vobInfo );
    // TODO: actually create renderItem in here, ready for indexed/instanced drawing.
}

void D3D11RenderQueue::PushTransparencyVob( TransparencyVobInfo vobInfo )
{
    transparent.emplace_back( std::move( vobInfo ) );
    // TODO: actually create renderItem in here, ready for indexed/instanced drawing.
}

void D3D11RenderQueue::PushLightVob( VobLightInfo* vobInfo )
{
    lights.emplace_back( vobInfo );
    // TODO: actually create renderItem in here, ready for indexed/instanced drawing.
}
