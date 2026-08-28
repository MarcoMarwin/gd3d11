#include "pch.h"
#include "Engine.h"
#include "GothicAPI.h"
#include "D3D11GraphicsEngine.h"
#include "HookExceptionFilter.h"
#include "ThreadPool.h"
#include "ImGuiShim.h"

namespace Engine {

    /** Refresh worker threadpool */
    void RefreshWorkerThreadpool() {
        if ( IsShuttingDown() ) {
            return;
        }

        // Keep the pool instance and drain its queue before reusing it.
        if ( !WorkerThreadPool ) {
            WorkerThreadPool = new ThreadPool(L"GD3D11-Worker");
            return;
        }
        WorkerThreadPool->clearAndFlush();
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
        if ( ShutdownCleanupStarted.exchange( true, std::memory_order_acq_rel ) ) {
            return;
        }
        ShuttingDown.store( true, std::memory_order_release );
        LogInfo() << "Shutting down...";
        if ( Engine::RenderingThreadPool ) {
            Engine::RenderingThreadPool->shutdown();
        }
        if ( Engine::WorkerThreadPool ) {
            Engine::WorkerThreadPool->shutdown();
        }
    }

};
