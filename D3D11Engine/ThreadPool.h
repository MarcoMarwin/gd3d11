#pragma once
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

template <typename T>
struct TaskHandle
{
    std::future<T> future;
    std::stop_source token;

    void cancel()
    {
        token.request_stop();
    }
};

class ThreadPool
{
public:
    ThreadPool(
        const wchar_t* poolIdentifier,
        size_t threads = std::clamp(
            static_cast<size_t>(std::thread::hardware_concurrency()),
            static_cast<size_t>(1),
            static_cast<size_t>(6)));

    template <typename F, typename... Args>
    auto enqueue(F&& f, Args&&... args)
    {
        using ReturnType = std::invoke_result_t<F, std::stop_token, Args...>;

        std::stop_source token;
        auto sharedTask = std::make_shared<std::packaged_task<ReturnType()>>(
            [func = std::forward<F>(f), token, ...capturedArgs = std::forward<Args>(args)]() mutable -> ReturnType
            {
                return std::invoke(std::move(func), token.get_token(), std::move(capturedArgs)...);
            });

        std::future<ReturnType> future = sharedTask->get_future();

        {
            std::scoped_lock lock(queue_mutex);
            if (stop)
            {
                throw std::runtime_error("enqueue on stopped ThreadPool");
            }

            tasks.emplace(
                [sharedTask]() mutable
                {
                    (*sharedTask)();
                },
                token);
        }

        condition.notify_one();
        return TaskHandle<ReturnType>{ std::move(future), token };
    }

    ~ThreadPool();

    size_t getNumThreads()
    {
        return numThreads;
    }

    bool getIsBusy()
    {
        std::unique_lock<std::mutex> lock(queue_mutex);
        return !tasks.empty() || activeTasks.load() > 0;
    }

    void clearAndFlush()
    {
        std::queue<std::pair<std::function<void()>, std::stop_source>> pendingTasks;

        {
            std::unique_lock<std::mutex> lock(queue_mutex);
            std::swap(tasks, pendingTasks);
        }

        while (!pendingTasks.empty())
        {
            auto taskItem = std::move(pendingTasks.front());
            pendingTasks.pop();

            taskItem.second.request_stop();
            taskItem.first();
        }

        while (getIsBusy())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

private:
    std::vector<std::thread> workers;
    std::queue<std::pair<std::function<void()>, std::stop_source>> tasks;
    std::atomic_int activeTasks{ 0 };
    std::mutex queue_mutex;
    std::condition_variable condition;
    bool stop = false;
    size_t numThreads = 0;
};

inline ThreadPool::ThreadPool(const wchar_t* poolIdentifier, size_t threads)
    : stop(false)
    , numThreads(threads)
{
    std::wstring identifier = std::wstring(L"GD3D11-") + std::wstring(poolIdentifier ? poolIdentifier : L"ThreadPool");

    for (size_t i = 0; i < threads; ++i)
    {
        workers.emplace_back(
            [](ThreadPool* pool, size_t workerId, const std::wstring& descriptionPrefix)
            {
                SetThreadDescription(GetCurrentThread(), (descriptionPrefix + std::to_wstring(workerId)).c_str());

                for (;;)
                {
                    std::function<void()> task;

                    {
                        std::unique_lock<std::mutex> lock(pool->queue_mutex);
                        pool->condition.wait(lock, [pool]
                        {
                            return pool->stop || !pool->tasks.empty();
                        });

                        if (pool->stop && pool->tasks.empty())
                        {
                            return;
                        }

                        task = std::move(pool->tasks.front().first);
                        pool->tasks.pop();
                        pool->activeTasks.fetch_add(1);
                    }

                    try
                    {
                        ZoneScopedN("ThreadPool Worker Task");
                        task();
                    }
                    catch (...)
                    {
                        pool->activeTasks.fetch_sub(1);
                        throw;
                    }

                    pool->activeTasks.fetch_sub(1);
                }
            },
            this,
            i,
            identifier);
    }
}

inline ThreadPool::~ThreadPool()
{
    {
        std::unique_lock<std::mutex> lock(queue_mutex);
        stop = true;
    }

    condition.notify_all();

    for (std::thread& worker : workers)
    {
        if (worker.joinable())
        {
            worker.join();
        }
    }
}
