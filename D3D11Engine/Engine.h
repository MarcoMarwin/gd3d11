#pragma once
#include "pch.h"
#include <atomic>

/** Global engine information */
class BaseGraphicsEngine;
class GothicAPI;
class ImGuiShim;
class GGame;
class ThreadPool;

__declspec(selectany) const char* ENGINE_BASE_DIR = "system\\GD3D11\\";

__declspec(selectany) const char* VERSION_STRING = "Version " VERSION_NUMBER;

namespace Engine {
    /** If true, we will just pass everything to the usual ddraw.dll */
    __declspec(selectany) bool PassThrough;

    /** Set before renderer/world teardown. */
    __declspec(selectany) std::atomic_bool ShuttingDown{ false };
    /** Guards the one-time cleanup path. */
    __declspec(selectany) std::atomic_bool ShutdownCleanupStarted{ false };

    inline bool IsShuttingDown() {
        return ShuttingDown.load( std::memory_order_acquire );
    }

    inline void BeginShutdown() {
        ShuttingDown.store( true, std::memory_order_release );
    }

    /** Global engine object */
    __declspec(selectany) BaseGraphicsEngine* GraphicsEngine;

    /** Global GothicAPI object */
    __declspec(selectany) GothicAPI* GAPI;

    /** Global ImGui object */
    __declspec(selectany) ImGuiShim* ImGuiHandle;

    /** Global rendering threadpool */
    __declspec(selectany) ThreadPool* RenderingThreadPool;

    /** Global worker threadpool */
    __declspec(selectany) ThreadPool* WorkerThreadPool;

    /** Refresh worker threadpool */
    void RefreshWorkerThreadpool();

    /** Creates main graphics engine */
    void CreateGraphicsEngine();

    /** Creates the Global GAPI-Object */
    void CreateGothicAPI();

    /** Called when the game is about to close */
    void OnShutDown();
};

