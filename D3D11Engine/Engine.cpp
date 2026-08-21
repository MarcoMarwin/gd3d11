#include "pch.h"
#include "Engine.h"
#include "GothicAPI.h"
#include "D3D11GraphicsEngine.h"
#include "HookExceptionFilter.h"
#include "ThreadPool.h"
#include "ImGuiShim.h"

//#define TESTING

namespace Engine {

    /** Refresh worker threadpool */
    void RefreshWorkerThreadpool() {
        // A pool destructor waits for workers, but it does not cancel queued
        // jobs. Clear them first so a world-compile refresh cannot execute a
        // queued job against VOB/pointlight data that is being replaced.
        if ( WorkerThreadPool ) {
            WorkerThreadPool->clearAndFlush();
            delete WorkerThreadPool;
            WorkerThreadPool = nullptr;
        }
        WorkerThreadPool = new ThreadPool(L"GD3D11-Worker");
    }

    /** Creates main graphics engine */
    void CreateGraphicsEngine() {
        LogInfo() << "Creating Main graphics engine";

        GraphicsEngine = new D3D11GraphicsEngine;

        if ( !GraphicsEngine ) {
            LogErrorBox() << "Failed to create GraphicsEngine! Out of memory!";
            exit( 0 );
        }

        ImGuiHandle = new ImGuiShim;

        XLE( GraphicsEngine->Init() );

        // Create threadpool
        RenderingThreadPool = new ThreadPool(L"GD3D11-Render");
        WorkerThreadPool = new ThreadPool(L"GD3D11-Worker");
    }

    /** Creates the Global GAPI-Object */
    void CreateGothicAPI() {
        LogInfo() << "GD3D11 " << VERSION_STRING;

        LogInfo() << "Loading modules for stacktracer";
        MyStackWalker::GetSingleton(); // Inits the static object in there

        LogInfo() << "Initializing GothicAPI";

        GAPI = new GothicAPI;
        if ( !GAPI ) {
            LogErrorBox() << "Failed to create GothicAPI!";
            exit( 0 );
        }
    }

    /** Called when the game is about to close */
    void OnShutDown() {
        static bool shutdownStarted = false;
        if ( shutdownStarted ) {
            return;
        }
        shutdownStarted = true;
        LogInfo() << "Shutting down...";
        if ( Engine::RenderingThreadPool ) {
            Engine::RenderingThreadPool->clearAndFlush();
        }
        if ( Engine::WorkerThreadPool ) {
            Engine::WorkerThreadPool->clearAndFlush();
        }
        // Release pointlight handles while the D3D/PFX pools are still alive.
        // The process still uses the established hard shutdown below, but no
        // renderer-owned resource is left to race the final Gothic teardown.
        if ( Engine::GAPI ) {
            Engine::GAPI->ReleasePointlightResources();
        }
        exit( 0 );
    }

};
