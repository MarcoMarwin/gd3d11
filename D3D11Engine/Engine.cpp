#include "pch.h"
#include "Engine.h"
#include "GothicAPI.h"
#include "D3D11GraphicsEngine.h"
#include "HookExceptionFilter.h"
#include "ThreadPool.h"
#include "ImGuiShim.h"
#include <atomic>

//#define TESTING

namespace Engine {

    /** Refresh worker threadpool */
    void RefreshWorkerThreadpool() {
        try {
            auto replacement = std::make_unique<ThreadPool>( L"GD3D11-Worker" );
            ThreadPool* previous = WorkerThreadPool;
            WorkerThreadPool = replacement.release();
            delete previous;
        } catch ( const std::exception& error ) {
            LogError() << "Failed to refresh worker thread pool: " << error.what();
        } catch ( ... ) {
            LogError() << "Failed to refresh worker thread pool.";
        }
    }

    /** Creates main graphics engine */
    XRESULT CreateGraphicsEngine() {
        if ( GraphicsEngine ) {
            return XR_SUCCESS;
        }
        if ( !GAPI ) {
            LogError() << "Cannot create the graphics engine before GothicAPI.";
            return XR_FAILED;
        }

        struct InitializationGuard {
            bool committed = false;
            ~InitializationGuard() {
                if ( !committed ) {
                    Engine::GraphicsEngine = nullptr;
                    Engine::ImGuiHandle = nullptr;
                    Engine::RenderingThreadPool = nullptr;
                    Engine::WorkerThreadPool = nullptr;
                }
            }
        } guard;

        try {
            LogInfo() << "Creating Main graphics engine";
            auto graphicsEngine = std::make_unique<D3D11GraphicsEngine>();
            auto imgui = std::make_unique<ImGuiShim>();

            GraphicsEngine = graphicsEngine.get();
            ImGuiHandle = imgui.get();
            const XRESULT result = graphicsEngine->Init();
            if ( result != XR_SUCCESS ) {
                LogError() << "Graphics engine initialization failed with code " << result << ".";
                return result;
            }

            auto renderingPool = std::make_unique<ThreadPool>( L"GD3D11-Render" );
            auto workerPool = std::make_unique<ThreadPool>( L"GD3D11-Worker" );
            RenderingThreadPool = renderingPool.get();
            WorkerThreadPool = workerPool.get();

            graphicsEngine.release();
            imgui.release();
            renderingPool.release();
            workerPool.release();
            guard.committed = true;
            return XR_SUCCESS;
        } catch ( const std::exception& error ) {
            LogError() << "Graphics engine initialization threw an exception: " << error.what();
            return XR_FAILED;
        } catch ( ... ) {
            LogError() << "Graphics engine initialization failed unexpectedly.";
            return XR_FAILED;
        }
    }

    /** Creates the Global GAPI-Object */
    XRESULT CreateGothicAPI() {
        if ( GAPI ) {
            return XR_SUCCESS;
        }

        try {
            LogInfo() << "GD3D11 " << VERSION_STRING;
            LogInfo() << "Loading modules for stacktracer";
            MyStackWalker::GetSingleton();

            LogInfo() << "Initializing GothicAPI";
            auto gothicApi = std::make_unique<GothicAPI>();
            GAPI = gothicApi.release();
            return XR_SUCCESS;
        } catch ( const std::exception& error ) {
            LogError() << "Failed to create GothicAPI: " << error.what();
            return XR_FAILED;
        } catch ( ... ) {
            LogError() << "Failed to create GothicAPI.";
            return XR_FAILED;
        }
    }

    /** Called when the game is about to close */
    void OnShutDown() {
        static std::atomic_flag shutdownStarted = ATOMIC_FLAG_INIT;
        if ( shutdownStarted.test_and_set( std::memory_order_acq_rel ) ) {
            return;
        }

        LogInfo() << "Shutting down...";

        ThreadPool* renderingPool = RenderingThreadPool;
        RenderingThreadPool = nullptr;
        delete renderingPool;

        ThreadPool* workerPool = WorkerThreadPool;
        WorkerThreadPool = nullptr;
        delete workerPool;

        ImGuiShim* imgui = ImGuiHandle;
        delete imgui;
        ImGuiHandle = nullptr;

        BaseGraphicsEngine* graphicsEngine = GraphicsEngine;
        delete graphicsEngine;
        GraphicsEngine = nullptr;

        GothicAPI* gothicApi = GAPI;
        delete gothicApi;
        GAPI = nullptr;
    }

};
