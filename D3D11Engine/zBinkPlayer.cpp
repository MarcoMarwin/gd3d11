#include "pch.h"
#include "Detours/detours.h"
#include "zSTRING.h"
#include "Engine.h"
#include "GothicAPI.h"
#include "D3D11Texture.h"
#include "BaseGraphicsEngine.h"

#include <ddraw.h>
#include <d3d.h>
#include <filesystem>
#include <limits>
#include <memory>
#include <new>

bool NewBinkSetVolume = true;
DWORD BinkOpenWaveOut;
DWORD BinkSetSoundSystem;
DWORD BinkSetSoundOnOff;
DWORD BinkSetVolume;
DWORD BinkOpen;
DWORD BinkDoFrame;
DWORD BinkNextFrame;
DWORD BinkWait;
DWORD BinkPause;
DWORD BinkClose;
DWORD BinkGoto;
DWORD BinkCopyToBuffer;
DWORD BinkSetFrameRate;
DWORD BinkSetSimulate;

inline DWORD UTIL_power_of_2(DWORD input)
{
	DWORD value = 1;
	while(value < input) value <<= 1;
	return value;
}

struct BinkVideo
{
	BinkVideo(void* vid) : vid(vid) {}

	void* vid = nullptr;

    std::unique_ptr<unsigned char[]> textureData;
    size_t textureDataSize = 0;
    std::unique_ptr<D3D11Texture> textureY;
    std::unique_ptr<D3D11Texture> textureU;
    std::unique_ptr<D3D11Texture> textureV;
	DWORD width = 0;
	DWORD height = 0;
	bool useBGRA = false;

	float scaleTU = 1.f;
	float scaleTV = 1.f;

	float globalVolume = 1.f;
	float videoVolume = 1.f;
	bool updateVolume = true;
	bool scaleVideo = true;
};

namespace {
    bool InitializeBinkFrameResources( BinkVideo& video, DWORD width, DWORD height ) {
        if ( width == 0 || height == 0
            || width > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION
            || height > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION
            || (width & 1u) != 0 || (height & 1u) != 0 ) {
            LogError() << "Bink video reported unsupported dimensions: " << width << "x" << height;
            return false;
        }

        const uint64_t lumaSize = static_cast<uint64_t>(width) * height;
        const uint64_t chromaSize = static_cast<uint64_t>(width / 2u) * (height / 2u);
        const uint64_t totalSize = lumaSize + chromaSize * 2u;
        constexpr uint64_t MaxBinkFrameBytes = 512ull * 1024ull * 1024ull;
        if ( totalSize == 0 || totalSize > MaxBinkFrameBytes
            || totalSize > std::numeric_limits<size_t>::max() ) {
            LogError() << "Bink video frame buffer size is invalid.";
            return false;
        }

        std::unique_ptr<unsigned char[]> newData(
            new (std::nothrow) unsigned char[static_cast<size_t>(totalSize)] );
        std::unique_ptr<D3D11Texture> newY( new (std::nothrow) D3D11Texture() );
        std::unique_ptr<D3D11Texture> newU( new (std::nothrow) D3D11Texture() );
        std::unique_ptr<D3D11Texture> newV( new (std::nothrow) D3D11Texture() );
        if ( !newData || !newY || !newU || !newV ) {
            return false;
        }

        const INT2 lumaDimensions( static_cast<int>(width), static_cast<int>(height) );
        const INT2 chromaDimensions( static_cast<int>(width / 2u), static_cast<int>(height / 2u) );
        if ( newY->Init( lumaDimensions, D3D11Texture::ETextureFormat::TF_R8, 1, nullptr, "Video Texture Y" ) != XR_SUCCESS
            || newU->Init( chromaDimensions, D3D11Texture::ETextureFormat::TF_R8, 1, nullptr, "Video Texture U" ) != XR_SUCCESS
            || newV->Init( chromaDimensions, D3D11Texture::ETextureFormat::TF_R8, 1, nullptr, "Video Texture V" ) != XR_SUCCESS ) {
            LogError() << "Bink video textures could not be created.";
            return false;
        }

        unsigned char* dataY = newData.get();
        unsigned char* dataV = dataY + static_cast<size_t>(lumaSize);
        unsigned char* dataU = dataV + static_cast<size_t>(chromaSize);
        memset( dataY, 16, static_cast<size_t>(lumaSize) );
        memset( dataV, 128, static_cast<size_t>(chromaSize) );
        memset( dataU, 128, static_cast<size_t>(chromaSize) );

        video.textureData = std::move( newData );
        video.textureDataSize = static_cast<size_t>(totalSize);
        video.textureY = std::move( newY );
        video.textureU = std::move( newU );
        video.textureV = std::move( newV );
        video.width = width;
        video.height = height;
        return true;
    }
}

float BinkPlayerReadGlobalVolume(DWORD zCOption)
{
#if defined(BUILD_GOTHIC_1_08k)
#if defined(BUILD_1_12F)
    return reinterpret_cast<float(__thiscall*)(DWORD, DWORD, DWORD, float)>(0x465CD0)(zCOption, 0x8ADE00, 0x8ADF08, 1.f);
#else
    return reinterpret_cast<float(__thiscall*)(DWORD, DWORD, DWORD, float)>(0x45E370)(zCOption, 0x869190, 0x869270, 1.f);
#endif
#else
    return reinterpret_cast<float(__thiscall*)(DWORD, DWORD, DWORD, float)>(0x463A60)(zCOption, 0x8CD380, 0x8CD49C, 1.f);
#endif
}

bool BinkPlayerReadScaleVideos( DWORD zCOption )
{
#if defined(BUILD_GOTHIC_1_08k)
#if defined(BUILD_1_12F)
    return reinterpret_cast<int(__thiscall*)(DWORD, DWORD, DWORD, int)>(GothicMemoryLocations::zCOption::ReadBool)(zCOption, 0x8AE058, 0x874228, 1);
#else
    return reinterpret_cast<int(__thiscall*)(DWORD, DWORD, DWORD, int)>(GothicMemoryLocations::zCOption::ReadBool)(zCOption, 0x869388, 0x82E6E4, 1);
#endif
#else
    return reinterpret_cast<int(__thiscall*)(DWORD, DWORD, DWORD, int)>(GothicMemoryLocations::zCOption::ReadBool)(zCOption, 0x8CD5F0, 0x8925D8, 1);
#endif
}

int __fastcall BinkPlayerPlayHandleEvents(DWORD BinkPlayer)
{
	reinterpret_cast<void(__cdecl*)(void)>(GothicMemoryLocations::GlobalObjects::sysEvents)();

	BinkVideo* video = *reinterpret_cast<BinkVideo**>(BinkPlayer + GothicMemoryLocations::zCBinkPlayer::Offset_VideoHandle);
	if(*reinterpret_cast<int*>(BinkPlayer + GothicMemoryLocations::zCBinkPlayer::Offset_DisallowInputHandling)) return 0;
	if(!video) return 0;
	if(!(*reinterpret_cast<int*>(BinkPlayer + GothicMemoryLocations::zCBinkPlayer::Offset_DoHandleEvents))) return 1;

	DWORD zInput = *reinterpret_cast<DWORD*>(GothicMemoryLocations::GlobalObjects::zCInput);
	WORD key = reinterpret_cast<WORD(__thiscall*)(DWORD, int, int)>(*reinterpret_cast<DWORD*>
        (*reinterpret_cast<DWORD*>(zInput) + GothicMemoryLocations::zCInput::GetKey_Offset))(zInput, 0, 0);
	reinterpret_cast<void(__thiscall*)(DWORD)>(*reinterpret_cast<DWORD*>
        (*reinterpret_cast<DWORD*>(zInput) + GothicMemoryLocations::zCInput::ProcessInputEvents_Offset))(zInput);
	switch(key)
	{
		case 0x01: // DIK_ESCAPE
			reinterpret_cast<void(__thiscall*)(DWORD)>(*reinterpret_cast<DWORD*>
                (*reinterpret_cast<DWORD*>(BinkPlayer) + GothicMemoryLocations::zCBinkPlayer::Stop_Offset))(BinkPlayer);
			break;
		case 0x39: // DIK_SPACE
        {
			if(*reinterpret_cast<int*>(BinkPlayer + GothicMemoryLocations::zCBinkPlayer::Offset_IsPaused))
			{
				reinterpret_cast<void(__stdcall*)(void*, int)>(BinkPause)(video->vid, 0);
				*reinterpret_cast<int*>(BinkPlayer + GothicMemoryLocations::zCBinkPlayer::Offset_IsPaused) = 0;
			}
			else
			{
				reinterpret_cast<void(__stdcall*)(void*, int)>(BinkPause)(video->vid, 1);
				*reinterpret_cast<int*>(BinkPlayer + GothicMemoryLocations::zCBinkPlayer::Offset_IsPaused) = 1;
			}
		}
		break;
		case 0x10: // DIK_Q
        {
			if(*reinterpret_cast<int*>(BinkPlayer + GothicMemoryLocations::zCBinkPlayer::Offset_SoundOn))
			{
				video->globalVolume = 0.f;
				video->updateVolume = true;
				*reinterpret_cast<int*>(BinkPlayer + GothicMemoryLocations::zCBinkPlayer::Offset_SoundOn) = 0;
			}
			else
			{
                video->globalVolume = BinkPlayerReadGlobalVolume(*reinterpret_cast<DWORD*>(GothicMemoryLocations::GlobalObjects::zCOption));
				video->updateVolume = true;
				*reinterpret_cast<int*>(BinkPlayer + GothicMemoryLocations::zCBinkPlayer::Offset_SoundOn) = 1;
			}
		}
		break;
		case 0xC8: // DIK_UP
        {
			video->videoVolume = std::min<float>(1.0f, video->videoVolume + 0.05f);
			video->updateVolume = true;
		}
		break;
		case 0xD0: // DIK_DOWN
        {
			video->videoVolume = std::max<float>(0.0f, video->videoVolume - 0.05f);
			video->updateVolume = true;
		}
		break;
	}
	return 1;
}

float __fastcall BinkPlayerSetSoundVolume( DWORD BinkPlayer, DWORD _EDX, float volume ) {
    BinkVideo* video = *reinterpret_cast<BinkVideo**>(
        BinkPlayer + GothicMemoryLocations::zCBinkPlayer::Offset_VideoHandle );
    if ( !video ) {
        return 0.0f;
    }

    video->videoVolume = std::clamp( volume, 0.0f, 1.0f );
    video->updateVolume = true;
    return 1.0f;
}

int __fastcall BinkPlayerToggleSound( DWORD BinkPlayer ) {
    BinkVideo* video = *reinterpret_cast<BinkVideo**>(
        BinkPlayer + GothicMemoryLocations::zCBinkPlayer::Offset_VideoHandle );
    if ( !video ) {
        return 0;
    }

    int* soundOn = reinterpret_cast<int*>(
        BinkPlayer + GothicMemoryLocations::zCBinkPlayer::Offset_SoundOn );
    if ( *soundOn ) {
        video->globalVolume = 0.0f;
        *soundOn = 0;
    } else {
        video->globalVolume = BinkPlayerReadGlobalVolume(
            *reinterpret_cast<DWORD*>(GothicMemoryLocations::GlobalObjects::zCOption) );
        *soundOn = 1;
    }
    video->updateVolume = true;
    return 1;
}

int __fastcall BinkPlayerPause( DWORD BinkPlayer ) {
    BinkVideo* video = *reinterpret_cast<BinkVideo**>(
        BinkPlayer + GothicMemoryLocations::zCBinkPlayer::Offset_VideoHandle );
    if ( !video || !video->vid || !BinkPause ) {
        return 0;
    }

    reinterpret_cast<void( __stdcall* )(void*, int)>(BinkPause)(video->vid, 1);
    return 1;
}

int __fastcall BinkPlayerUnpause( DWORD BinkPlayer ) {
    BinkVideo* video = *reinterpret_cast<BinkVideo**>(
        BinkPlayer + GothicMemoryLocations::zCBinkPlayer::Offset_VideoHandle );
    if ( !video || !video->vid || !BinkPause ) {
        return 0;
    }

    reinterpret_cast<void( __stdcall* )(void*, int)>(BinkPause)(video->vid, 0);
    return 1;
}

int __fastcall BinkPlayerIsPlaying( DWORD BinkPlayer ) {
    BinkVideo* video = *reinterpret_cast<BinkVideo**>(
        BinkPlayer + GothicMemoryLocations::zCBinkPlayer::Offset_VideoHandle );
    if ( video && video->vid
        && *reinterpret_cast<int*>(BinkPlayer + GothicMemoryLocations::zCBinkPlayer::Offset_IsPlaying)
        && (*reinterpret_cast<int*>(BinkPlayer + GothicMemoryLocations::zCBinkPlayer::Offset_IsLooping)
            || *reinterpret_cast<DWORD*>(reinterpret_cast<DWORD>(video->vid) + 0x08)
                > *reinterpret_cast<DWORD*>(reinterpret_cast<DWORD>(video->vid) + 0x0C)) ) {
        return 1;
    }
    return 0;
}

int __fastcall BinkPlayerPlayGotoNextFrame( DWORD BinkPlayer ) {
    BinkVideo* video = *reinterpret_cast<BinkVideo**>(
        BinkPlayer + GothicMemoryLocations::zCBinkPlayer::Offset_VideoHandle );
    if ( !video || !video->vid || !BinkNextFrame ) {
        return 0;
    }

    reinterpret_cast<void( __stdcall* )(void*)>(BinkNextFrame)(video->vid);
    return 1;
}

int __fastcall BinkPlayerPlayWaitNextFrame( DWORD BinkPlayer ) {
    BinkVideo* video = *reinterpret_cast<BinkVideo**>(
        BinkPlayer + GothicMemoryLocations::zCBinkPlayer::Offset_VideoHandle );
    if ( !video || !video->vid || !BinkWait ) {
        return 0;
    }

    while ( BinkPlayerIsPlaying( BinkPlayer )
        && reinterpret_cast<int( __stdcall* )(void*)>(BinkWait)(video->vid) ) {
        BinkPlayerPlayHandleEvents( BinkPlayer );
        std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );

        video = *reinterpret_cast<BinkVideo**>(
            BinkPlayer + GothicMemoryLocations::zCBinkPlayer::Offset_VideoHandle );
        if ( !video || !video->vid ) {
            break;
        }
    }
    return 1;
}

int __fastcall BinkPlayerPlayDoFrame( DWORD BinkPlayer ) {
    BinkVideo* video = *reinterpret_cast<BinkVideo**>(
        BinkPlayer + GothicMemoryLocations::zCBinkPlayer::Offset_VideoHandle );
    if ( !video || !video->vid || !BinkDoFrame ) {
        return 0;
    }

    if ( video->updateVolume ) {
        video->updateVolume = false;
    }
    reinterpret_cast<void( __stdcall* )(void*)>(BinkDoFrame)(video->vid);
    return 1;
}
int __fastcall BinkPlayerPlayFrame(DWORD BinkPlayer)
{
	BinkVideo* video = *reinterpret_cast<BinkVideo**>(BinkPlayer + GothicMemoryLocations::zCBinkPlayer::Offset_VideoHandle);
	if(BinkPlayerIsPlaying(BinkPlayer))
	{
        BinkPlayerPlayHandleEvents(BinkPlayer);
		if(BinkPlayerIsPlaying(BinkPlayer))
		{
            BinkPlayerPlayDoFrame(BinkPlayer);
            {
                const DWORD vidWidth = *reinterpret_cast<DWORD*>(
                    reinterpret_cast<DWORD>(video->vid) + 0x00 );
                const DWORD vidHeight = *reinterpret_cast<DWORD*>(
                    reinterpret_cast<DWORD>(video->vid) + 0x04 );
                if ( !video->textureData || !video->textureY || !video->textureU || !video->textureV
                    || video->width != vidWidth || video->height != vidHeight ) {
                    if ( !InitializeBinkFrameResources( *video, vidWidth, vidHeight ) ) {
                        BinkPlayerPlayGotoNextFrame( BinkPlayer );
                        BinkPlayerPlayWaitNextFrame( BinkPlayer );
                        return 1;
                    }
                }

                reinterpret_cast<void( __stdcall* )(void*, void*, int, DWORD, DWORD, DWORD, DWORD)>(
                    BinkCopyToBuffer )(
                        video->vid, video->textureData.get(), static_cast<int>(vidWidth),
                        vidHeight, 0, 0, 0x70000000L | 15 );

                const size_t lumaSize = static_cast<size_t>(vidWidth) * vidHeight;
                const size_t chromaSize = static_cast<size_t>(vidWidth / 2u) * (vidHeight / 2u);
                unsigned char* dataY = video->textureData.get();
                unsigned char* dataV = dataY + lumaSize;
                unsigned char* dataU = dataV + chromaSize;
                if ( video->textureY->UpdateData( dataY, 0 ) != XR_SUCCESS
                    || video->textureV->UpdateData( dataV, 0 ) != XR_SUCCESS
                    || video->textureU->UpdateData( dataU, 0 ) != XR_SUCCESS ) {
                    LogError() << "Bink video texture upload failed.";
                    BinkPlayerPlayGotoNextFrame( BinkPlayer );
                    BinkPlayerPlayWaitNextFrame( BinkPlayer );
                    return 1;
                }
                DWORD zrenderer = *reinterpret_cast<DWORD*>(GothicMemoryLocations::GlobalObjects::zRenderer);
                int oldZWrite = reinterpret_cast<int( __thiscall* )(DWORD)>(*reinterpret_cast<DWORD*>
                    (*reinterpret_cast<DWORD*>(zrenderer) + GothicMemoryLocations::zCRndD3D::GetZBufferWriteEnabled_Offset))(zrenderer);
                reinterpret_cast<void( __thiscall* )(DWORD, int)>(*reinterpret_cast<DWORD*>
                    (*reinterpret_cast<DWORD*>(zrenderer) + GothicMemoryLocations::zCRndD3D::SetZBufferWriteEnabled_Offset))(zrenderer, 0); // No depth-writes
                int oldZCompare = reinterpret_cast<int( __thiscall* )(DWORD)>(*reinterpret_cast<DWORD*>
                    (*reinterpret_cast<DWORD*>(zrenderer) + GothicMemoryLocations::zCRndD3D::GetZBufferCompare_Offset))(zrenderer);
                int newZCompare = 0; // Compare always
                reinterpret_cast<void( __thiscall* )(DWORD, int&)>(*reinterpret_cast<DWORD*>
                    (*reinterpret_cast<DWORD*>(zrenderer) + GothicMemoryLocations::zCRndD3D::SetZBufferCompare_Offset))(zrenderer, newZCompare);
                int oldAlphaFunc = reinterpret_cast<int( __thiscall* )(DWORD)>(*reinterpret_cast<DWORD*>
                    (*reinterpret_cast<DWORD*>(zrenderer) + GothicMemoryLocations::zCRndD3D::GetAlphaBlendFunc_Offset))(zrenderer);
                int oldFilter = reinterpret_cast<int( __thiscall* )(DWORD)>(*reinterpret_cast<DWORD*>
                    (*reinterpret_cast<DWORD*>(zrenderer) + GothicMemoryLocations::zCRndD3D::GetBilerpFilterEnabled_Offset))(zrenderer);
                reinterpret_cast<void( __thiscall* )(DWORD, int)>(*reinterpret_cast<DWORD*>
                    (*reinterpret_cast<DWORD*>(zrenderer) + GothicMemoryLocations::zCRndD3D::SetBilerpFilterEnabled_Offset))(zrenderer, video->scaleVideo ? 1 : 0); // Bilinear filter
                int oldFog = reinterpret_cast<int( __thiscall* )(DWORD)>(*reinterpret_cast<DWORD*>
                    (*reinterpret_cast<DWORD*>(zrenderer) + GothicMemoryLocations::zCRndD3D::GetFog_Offset))(zrenderer);
                reinterpret_cast<void( __thiscall* )(DWORD, int)>(*reinterpret_cast<DWORD*>
                    (*reinterpret_cast<DWORD*>(zrenderer) + GothicMemoryLocations::zCRndD3D::SetFog_Offset))(zrenderer, 0); // No fog

                DWORD SetTextureStageState = *reinterpret_cast<DWORD*>(*reinterpret_cast<DWORD*>(zrenderer) + GothicMemoryLocations::zCRndD3D::SetTextureStageState_Offset);
                // Disable alpha blending
                reinterpret_cast<void( __thiscall* )(DWORD, int, int)>(GothicMemoryLocations::zCRndD3D::XD3D_SetRenderState)(zrenderer, 26, 0);
                reinterpret_cast<void( __thiscall* )(DWORD, int, int)>(GothicMemoryLocations::zCRndD3D::XD3D_SetRenderState)(zrenderer, 27, 0);
                reinterpret_cast<void( __thiscall* )(DWORD, int, int)>(GothicMemoryLocations::zCRndD3D::XD3D_SetRenderState)(zrenderer, 15, 0);
                // Disable clipping
                reinterpret_cast<void( __thiscall* )(DWORD, int, int)>(GothicMemoryLocations::zCRndD3D::XD3D_SetRenderState)(zrenderer, 136, 0);
                // Disable culling
                reinterpret_cast<void( __thiscall* )(DWORD, int, int)>(GothicMemoryLocations::zCRndD3D::XD3D_SetRenderState)(zrenderer, 22, 1);
                // Set texture clamping
                reinterpret_cast<void( __thiscall* )(DWORD, int, int, int)>(SetTextureStageState)(zrenderer, 0, 12, 3);
                reinterpret_cast<void( __thiscall* )(DWORD, int, int, int)>(SetTextureStageState)(zrenderer, 0, 13, 3);
                // 0 stage AlphaOp modulate
                reinterpret_cast<void( __thiscall* )(DWORD, int, int, int)>(SetTextureStageState)(zrenderer, 0, 3, 3);
                // 1 stage AlphaOp disable
                reinterpret_cast<void( __thiscall* )(DWORD, int, int, int)>(SetTextureStageState)(zrenderer, 1, 3, 0);
                // 0 stage ColorOp modulate
                reinterpret_cast<void( __thiscall* )(DWORD, int, int, int)>(SetTextureStageState)(zrenderer, 0, 0, 3);
                // 1 stage ColorOp disable
                reinterpret_cast<void( __thiscall* )(DWORD, int, int, int)>(SetTextureStageState)(zrenderer, 1, 0, 0);
                // 0 stage AlphaArg1/2 texure/diffuse
                reinterpret_cast<void( __thiscall* )(DWORD, int, int, int)>(SetTextureStageState)(zrenderer, 0, 4, 3);
                reinterpret_cast<void( __thiscall* )(DWORD, int, int, int)>(SetTextureStageState)(zrenderer, 0, 5, 1);
                // 0 stage ColorArg1/2 texure/diffuse
                reinterpret_cast<void( __thiscall* )(DWORD, int, int, int)>(SetTextureStageState)(zrenderer, 0, 1, 3);
                reinterpret_cast<void( __thiscall* )(DWORD, int, int, int)>(SetTextureStageState)(zrenderer, 0, 2, 1);
                // 0 stage TextureTransformFlags disable
                reinterpret_cast<void( __thiscall* )(DWORD, int, int, int)>(SetTextureStageState)(zrenderer, 0, 23, 0);
                // 0 stage TexCoordIndex 0
                reinterpret_cast<void( __thiscall* )(DWORD, int, int, int)>(SetTextureStageState)(zrenderer, 0, 10, 0);

                // Set fullscreen viewport
                int gWidth = *reinterpret_cast<int*>(zrenderer + GothicMemoryLocations::zCRndD3D::Offset_Width);
                int gHeight = *reinterpret_cast<int*>(zrenderer + GothicMemoryLocations::zCRndD3D::Offset_Height);
                reinterpret_cast<void( __thiscall* )(DWORD, int, int, int, int)>(*reinterpret_cast<DWORD*>
                    (*reinterpret_cast<DWORD*>(zrenderer) + GothicMemoryLocations::zCRndD3D::SetViewport_Offset))(zrenderer, 0, 0, gWidth, gHeight);

                float scale = std::min<float>(static_cast<float>(gWidth) / static_cast<float>(video->width), static_cast<float>(gHeight) / static_cast<float>(video->height));
                int dstW = std::min<int>(static_cast<int>(video->width * scale), gWidth);
                int dstH = std::min<int>(static_cast<int>(video->height * scale), gHeight);
                int dstX = std::max<int>((gWidth / 2) - (dstW / 2), 0);
                int dstY = std::max<int>((gHeight / 2) - (dstH / 2), 0);
                if(!video->scaleVideo)
                {
                    dstX = (gWidth / 2) - (video->width / 2);
                    dstY = (gHeight / 2) - (video->height / 2);
                    dstW = video->width;
                    dstH = video->height;
                }

                float minx = static_cast<float>(dstX);
                float miny = static_cast<float>(dstY);
                float maxx = static_cast<float>(dstW) + minx;
                float maxy = static_cast<float>(dstH) + miny;

                ExVertexStruct verts[6];
                verts[0].Position = float3(minx, miny, 0.f);
                verts[0].Normal = float3(1.f, 0.f, 0.f);
                verts[0].TexCoord = float2(0.f, 0.f);
                verts[0].TexCoord2 = float2(0.f, 0.f);
                verts[0].Color = 0xFFFFFFFF;

                verts[1].Position = float3(maxx, maxy, 0.f);
                verts[1].Normal = float3(1.f, 0.f, 0.f);
                verts[1].TexCoord = float2(1.f, 1.f);
                verts[1].TexCoord2 = float2(0.f, 0.f);
                verts[1].Color = 0xFFFFFFFF;

                verts[2].Position = float3(maxx, miny, 0.f);
                verts[2].Normal = float3(1.f, 0.f, 0.f);
                verts[2].TexCoord = float2(1.f, 0.f);
                verts[2].TexCoord2 = float2(0.f, 0.f);
                verts[2].Color = 0xFFFFFFFF;

                verts[3].Position = float3(minx, miny, 0.f);
                verts[3].Normal = float3(1.f, 0.f, 0.f);
                verts[3].TexCoord = float2(0.f, 0.f);
                verts[3].TexCoord2 = float2(0.f, 0.f);
                verts[3].Color = 0xFFFFFFFF;

                verts[4].Position = float3(minx, maxy, 0.f);
                verts[4].Normal = float3(1.f, 0.f, 0.f);
                verts[4].TexCoord = float2(0.f, 1.f);
                verts[4].TexCoord2 = float2(0.f, 0.f);
                verts[4].Color = 0xFFFFFFFF;

                verts[5].Position = float3(maxx, maxy, 0.f);
                verts[5].Normal = float3(1.f, 0.f, 0.f);
                verts[5].TexCoord = float2(1.f, 1.f);
                verts[5].TexCoord2 = float2(0.f, 0.f);
                verts[5].Color = 0xFFFFFFFF;

                Engine::GraphicsEngine->SetActiveVertexShader(VShaderID::VS_TransformedEx);
                Engine::GraphicsEngine->BindViewportInformation(VShaderID::VS_TransformedEx, 0);
                Engine::GraphicsEngine->SetActivePixelShader(PShaderID::PS_Video);
                video->textureY->BindToPixelShader(0);
                video->textureU->BindToPixelShader(1);
                video->textureV->BindToPixelShader(2);
                Engine::GraphicsEngine->Clear(float4(0.f, 0.f, 0.f, 1.f));
                Engine::GraphicsEngine->DrawVertexArray(verts, 6);

                reinterpret_cast<void( __thiscall* )(DWORD, int)>(*reinterpret_cast<DWORD*>
                    (*reinterpret_cast<DWORD*>(zrenderer) + GothicMemoryLocations::zCRndD3D::SetFog_Offset))(zrenderer, oldFog);
                reinterpret_cast<void( __thiscall* )(DWORD, int)>(*reinterpret_cast<DWORD*>
                    (*reinterpret_cast<DWORD*>(zrenderer) + GothicMemoryLocations::zCRndD3D::SetBilerpFilterEnabled_Offset))(zrenderer, oldFilter);
                reinterpret_cast<void( __thiscall* )(DWORD, int&)>(*reinterpret_cast<DWORD*>
                    (*reinterpret_cast<DWORD*>(zrenderer) + GothicMemoryLocations::zCRndD3D::SetAlphaBlendFunc_Offset))(zrenderer, oldAlphaFunc);
                reinterpret_cast<void( __thiscall* )(DWORD, int&)>(*reinterpret_cast<DWORD*>
                    (*reinterpret_cast<DWORD*>(zrenderer) + GothicMemoryLocations::zCRndD3D::SetZBufferCompare_Offset))(zrenderer, oldZCompare);
                reinterpret_cast<void( __thiscall* )(DWORD, int)>(*reinterpret_cast<DWORD*>
                    (*reinterpret_cast<DWORD*>(zrenderer) + GothicMemoryLocations::zCRndD3D::SetZBufferWriteEnabled_Offset))(zrenderer, oldZWrite);
                reinterpret_cast<void( __thiscall* )(DWORD, int, void*, void*)>(*reinterpret_cast<DWORD*>
                    (*reinterpret_cast<DWORD*>(zrenderer) + GothicMemoryLocations::zCRndD3D::Vid_Blit_Offset))(zrenderer, 0, nullptr, nullptr);
            }
            BinkPlayerPlayGotoNextFrame(BinkPlayer);
            BinkPlayerPlayWaitNextFrame(BinkPlayer);
		}
	}
	return 1;
}

int __fastcall BinkPlayerPlayInit(DWORD BinkPlayer, DWORD _EDX, int frame)
{
	BinkVideo* video = *reinterpret_cast<BinkVideo**>(BinkPlayer + GothicMemoryLocations::zCBinkPlayer::Offset_VideoHandle);
	if(!video)
		return 0;

    if(!reinterpret_cast<int(__thiscall*)(DWORD, int)>(GothicMemoryLocations::zCBinkPlayer::PlayInit)(BinkPlayer, frame))
    {
        *reinterpret_cast<int*>(BinkPlayer + GothicMemoryLocations::zCBinkPlayer::Offset_IsPlaying) = 0;
        return 1;
    }

	if(frame > 0)
		reinterpret_cast<void(__stdcall*)(void*, int, int)>(BinkGoto)(video->vid, frame, 0);

	return 1;
}

int __fastcall BinkPlayerPlayDeinit(DWORD BinkPlayer)
{
	DWORD BackView = *reinterpret_cast<DWORD*>(BinkPlayer + 0x5C);
    if(BackView)
    {
#if defined(BUILD_GOTHIC_1_08k)
        reinterpret_cast<void(__thiscall*)(DWORD, int)>(*reinterpret_cast<DWORD*>(*reinterpret_cast<DWORD*>(BackView) + 0x20))(BackView, 1);
#else
        reinterpret_cast<void(__thiscall*)(DWORD, int)>(*reinterpret_cast<DWORD*>(*reinterpret_cast<DWORD*>(BackView) + 0x24))(BackView, 1);
#endif
    }

	return reinterpret_cast<int(__thiscall*)(DWORD)>(GothicMemoryLocations::zCBinkPlayer::PlayDeinit)(BinkPlayer);
}

int __fastcall BinkPlayerOpenVideo( DWORD BinkPlayer, DWORD _EDX, zSTRING videoName ) {
    if ( !BinkSetSoundSystem || !BinkOpenWaveOut || !BinkOpen
        || !BinkSetSoundOnOff || !BinkClose ) {
        videoName.Delete();
        return 0;
    }

    const DWORD zCOption = *reinterpret_cast<DWORD*>(GothicMemoryLocations::GlobalObjects::zCOption);
#if defined(BUILD_GOTHIC_1_08k) && !defined(BUILD_1_12F)
    zSTRING& directoryRoot = reinterpret_cast<zSTRING&(__thiscall*)(DWORD, int)>(
        GothicMemoryLocations::zCOption::GetDirectory )(zCOption, 23);
#else
    zSTRING& directoryRoot = reinterpret_cast<zSTRING&(__thiscall*)(DWORD, int)>(
        GothicMemoryLocations::zCOption::GetDirectory )(zCOption, 24);
#endif
    std::string pathToVideo = std::string( directoryRoot.ToChar(), directoryRoot.Length() )
        + std::string( videoName.ToChar(), videoName.Length() );

    auto hasExtension = [&]( const char* extension ) {
        const size_t extensionLength = strlen( extension );
        return pathToVideo.size() >= extensionLength
            && _stricmp( pathToVideo.c_str() + pathToVideo.size() - extensionLength, extension ) == 0;
    };
    if ( !hasExtension( ".BIK" ) && !hasExtension( ".BK2" ) ) {
        pathToVideo.append( ".BIK" );
    }

    reinterpret_cast<void( __stdcall* )(DWORD, DWORD)>(
        BinkSetSoundSystem )(BinkOpenWaveOut, 0);
    void* videoHandle = reinterpret_cast<void*( __stdcall* )(const char*, DWORD)>(
        BinkOpen )(pathToVideo.c_str(), 0);
    if ( videoHandle ) {
        reinterpret_cast<void( __stdcall* )(void*, int)>(
            BinkSetSoundOnOff )(videoHandle, 1);

        auto* newVideo = new (std::nothrow) BinkVideo( videoHandle );
        if ( !newVideo ) {
            reinterpret_cast<void( __stdcall* )(void*)>(BinkClose)(videoHandle);
            videoName.Delete();
            return 0;
        }
        *reinterpret_cast<BinkVideo**>(
            BinkPlayer + GothicMemoryLocations::zCBinkPlayer::Offset_VideoHandle ) = newVideo;
    }

    BinkVideo* video = *reinterpret_cast<BinkVideo**>(
        BinkPlayer + GothicMemoryLocations::zCBinkPlayer::Offset_VideoHandle );
    if ( video ) {
        video->globalVolume = BinkPlayerReadGlobalVolume( zCOption );
        video->scaleVideo = BinkPlayerReadScaleVideos( zCOption );
        Engine::GAPI->GetRendererState().RendererSettings.BinkVideoRunning = true;

        reinterpret_cast<int( __thiscall* )(DWORD, zSTRING)>(
            GothicMemoryLocations::zCBinkPlayer::OpenVideo )(BinkPlayer, videoName);
        return 1;
    }

    videoName.Delete();
    return 0;
}
int __fastcall BinkPlayerCloseVideo( DWORD BinkPlayer ) {
    BinkVideo* video = *reinterpret_cast<BinkVideo**>(
        BinkPlayer + GothicMemoryLocations::zCBinkPlayer::Offset_VideoHandle );
    if ( !video ) {
        Engine::GAPI->GetRendererState().RendererSettings.BinkVideoRunning = false;
        return 0;
    }

    if ( BinkClose && video->vid ) {
        reinterpret_cast<void( __stdcall* )(void*)>(BinkClose)(video->vid);
    }
    delete video;
    *reinterpret_cast<BinkVideo**>(
        BinkPlayer + GothicMemoryLocations::zCBinkPlayer::Offset_VideoHandle ) = nullptr;
    Engine::GAPI->GetRendererState().RendererSettings.BinkVideoRunning = false;

    return reinterpret_cast<int( __thiscall* )(DWORD)>(
        GothicMemoryLocations::zCBinkPlayer::CloseVideo )(BinkPlayer);
}
void RegisterBinkPlayerHooks() {
    static bool registered = false;
    static HMODULE binkModule = nullptr;
    if ( registered ) {
        return;
    }

    binkModule = GetModuleHandleW( L"Bink2W32.dll" );
    if ( !binkModule ) {
        binkModule = GetModuleHandleW( L"BinkW32.dll" );
    }

    bool loadedByRenderer = false;
    if ( !binkModule ) {
        wchar_t executablePath[MAX_PATH]{};
        const DWORD pathLength = GetModuleFileNameW(
            nullptr, executablePath, static_cast<DWORD>(std::size( executablePath )) );
        if ( pathLength != 0 && pathLength < std::size( executablePath ) ) {
            const std::filesystem::path executableDirectory =
                std::filesystem::path( executablePath ).parent_path();
            for ( const wchar_t* dllName : { L"Bink2W32.dll", L"BinkW32.dll" } ) {
                const std::filesystem::path dllPath = executableDirectory / dllName;
                binkModule = LoadLibraryW( dllPath.c_str() );
                if ( binkModule ) {
                    loadedByRenderer = true;
                    break;
                }
            }
        }
    }

    if ( !binkModule ) {
        LogWarn() << "Bink DLL was not found; keeping Gothic's original video player.";
        return;
    }

    auto resolve = [&]( const char* decoratedName, const char* plainName ) -> DWORD {
        FARPROC address = GetProcAddress( binkModule, decoratedName );
        if ( !address && plainName ) {
            address = GetProcAddress( binkModule, plainName );
        }
        return reinterpret_cast<DWORD>(address);
    };

    NewBinkSetVolume = true;
    BinkOpenWaveOut = resolve( "_BinkOpenWaveOut@4", "BinkOpenWaveOut" );
    BinkSetSoundSystem = resolve( "_BinkSetSoundSystem@8", "BinkSetSoundSystem" );
    BinkSetSoundOnOff = resolve( "_BinkSetSoundOnOff@8", "BinkSetSoundOnOff" );
    BinkSetVolume = resolve( "_BinkSetVolume@12", nullptr );
    if ( !BinkSetVolume ) {
        BinkSetVolume = resolve( "_BinkSetVolume@8", "BinkSetVolume" );
#if defined(BUILD_GOTHIC_1_08k)
#if !defined(BUILD_1_12F)
        if ( *reinterpret_cast<BYTE*>(0x43A942) != 0xE9 ) {
            NewBinkSetVolume = false;
        }
#else
        NewBinkSetVolume = false;
#endif
#endif
    }

    BinkOpen = resolve( "_BinkOpen@8", "BinkOpen" );
    BinkDoFrame = resolve( "_BinkDoFrame@4", "BinkDoFrame" );
    BinkNextFrame = resolve( "_BinkNextFrame@4", "BinkNextFrame" );
    BinkWait = resolve( "_BinkWait@4", "BinkWait" );
    BinkPause = resolve( "_BinkPause@8", "BinkPause" );
    BinkClose = resolve( "_BinkClose@4", "BinkClose" );
    BinkGoto = resolve( "_BinkGoto@12", "BinkGoto" );
    BinkCopyToBuffer = resolve( "_BinkCopyToBuffer@28", "BinkCopyToBuffer" );
    BinkSetFrameRate = resolve( "_BinkSetFrameRate@8", "BinkSetFrameRate" );
    BinkSetSimulate = resolve( "_BinkSetSimulate@4", "BinkSetSimulate" );

    struct RequiredExport {
        const char* name;
        DWORD address;
    };
    const RequiredExport requiredExports[] = {
        { "BinkOpenWaveOut", BinkOpenWaveOut },
        { "BinkSetSoundSystem", BinkSetSoundSystem },
        { "BinkSetSoundOnOff", BinkSetSoundOnOff },
        { "BinkOpen", BinkOpen },
        { "BinkDoFrame", BinkDoFrame },
        { "BinkNextFrame", BinkNextFrame },
        { "BinkWait", BinkWait },
        { "BinkPause", BinkPause },
        { "BinkClose", BinkClose },
        { "BinkGoto", BinkGoto },
        { "BinkCopyToBuffer", BinkCopyToBuffer },
        { "BinkSetFrameRate", BinkSetFrameRate },
        { "BinkSetSimulate", BinkSetSimulate },
    };
    for ( const auto& required : requiredExports ) {
        if ( !required.address ) {
            LogError() << "Bink export is missing: " << required.name
                << ". Keeping Gothic's original video player.";
            if ( loadedByRenderer ) {
                FreeLibrary( binkModule );
                binkModule = nullptr;
            }
            return;
        }
    }

    bool hooksPrepared = true;
    auto patchHook = [&]( DWORD vtableAddress, DWORD functionAddress, auto replacement ) {
        const DWORD replacementAddress = reinterpret_cast<DWORD>(replacement);
        const bool vtablePrepared = PatchValue(
            vtableAddress, replacementAddress );
        const bool functionPrepared = PatchJMP(
            functionAddress, replacementAddress );
        hooksPrepared = vtablePrepared && functionPrepared && hooksPrepared;
    };

    patchHook(
        GothicMemoryLocations::zCBinkPlayer::PlayHandleEvents_Vtable,
        GothicMemoryLocations::zCBinkPlayer::PlayHandleEvents_Func,
        &BinkPlayerPlayHandleEvents );
    patchHook(
        GothicMemoryLocations::zCBinkPlayer::SetSoundVolume_Vtable,
        GothicMemoryLocations::zCBinkPlayer::SetSoundVolume_Func,
        &BinkPlayerSetSoundVolume );
    patchHook(
        GothicMemoryLocations::zCBinkPlayer::ToggleSound_Vtable,
        GothicMemoryLocations::zCBinkPlayer::ToggleSound_Func,
        &BinkPlayerToggleSound );
    patchHook(
        GothicMemoryLocations::zCBinkPlayer::Pause_Vtable,
        GothicMemoryLocations::zCBinkPlayer::Pause_Func,
        &BinkPlayerPause );
    patchHook(
        GothicMemoryLocations::zCBinkPlayer::Unpause_Vtable,
        GothicMemoryLocations::zCBinkPlayer::Unpause_Func,
        &BinkPlayerUnpause );
    patchHook(
        GothicMemoryLocations::zCBinkPlayer::IsPlaying_Vtable,
        GothicMemoryLocations::zCBinkPlayer::IsPlaying_Func,
        &BinkPlayerIsPlaying );
    patchHook(
        GothicMemoryLocations::zCBinkPlayer::PlayGotoNextFrame_Vtable,
        GothicMemoryLocations::zCBinkPlayer::PlayGotoNextFrame_Func,
        &BinkPlayerPlayGotoNextFrame );
    patchHook(
        GothicMemoryLocations::zCBinkPlayer::PlayWaitNextFrame_Vtable,
        GothicMemoryLocations::zCBinkPlayer::PlayWaitNextFrame_Func,
        &BinkPlayerPlayWaitNextFrame );
    patchHook(
        GothicMemoryLocations::zCBinkPlayer::PlayFrame_Vtable,
        GothicMemoryLocations::zCBinkPlayer::PlayFrame_Func,
        &BinkPlayerPlayFrame );
    patchHook(
        GothicMemoryLocations::zCBinkPlayer::PlayInit_Vtable,
        GothicMemoryLocations::zCBinkPlayer::PlayInit_Func,
        &BinkPlayerPlayInit );
    patchHook(
        GothicMemoryLocations::zCBinkPlayer::PlayDeinit_Vtable,
        GothicMemoryLocations::zCBinkPlayer::PlayDeinit_Func,
        &BinkPlayerPlayDeinit );
    patchHook(
        GothicMemoryLocations::zCBinkPlayer::OpenVideo_Vtable,
        GothicMemoryLocations::zCBinkPlayer::OpenVideo_Func,
        &BinkPlayerOpenVideo );
    patchHook(
        GothicMemoryLocations::zCBinkPlayer::CloseVideo_Vtable,
        GothicMemoryLocations::zCBinkPlayer::CloseVideo_Func,
        &BinkPlayerCloseVideo );

    if ( !hooksPrepared ) {
        LogError() << "Bink hook patch preparation failed at 0x"
            << std::hex << GothicPatching::GetFirstFailureAddress()
            << "; keeping Gothic's original video player.";
        if ( loadedByRenderer ) {
            FreeLibrary( binkModule );
            binkModule = nullptr;
        }
        return;
    }

    registered = true;
    LogInfo() << "Bink video hooks registered.";
}