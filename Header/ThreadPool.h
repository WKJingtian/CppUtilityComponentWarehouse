#pragma once
#include <future>
#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <stdexcept>
#include <type_traits>
#include <assert.h>

class ThreadPool
{
	// jthread is better, it is more "RAII", but since this code runs OK, I will not upgrade for now...
	std::vector<std::thread> _threads{};
	std::queue<std::function<void()>> _tasks{};
	std::mutex _mutex;
	std::condition_variable _cond;
	bool _isDead = false;
public:
	static ThreadPool& Inst() { static ThreadPool inst(3); return inst; }
	ThreadPool(size_t threadMax)
	{
		for (int i = 0; i < threadMax; i++)
		{
			_threads.emplace_back(std::thread([this]()
				{
					while (true)
					{
						std::function<void()> task;
						{
							std::unique_lock<std::mutex> lock(_mutex);
							_cond.wait(lock, [this]() { return _isDead || !_tasks.empty(); });
							if (_isDead && _tasks.empty()) return;
							task = std::move(_tasks.front());
							_tasks.pop();
						}
						try {
							if (task) {
								task(); // operate packaged_task; store exception to std::future if any
							}
						} catch (const std::exception& e) {
							// thread pool failure, not task failure
							fprintf(stderr, "ThreadPool Task Exception: %s\n", e.what());
						} catch (...) {
							fprintf(stderr, "ThreadPool Unknown Exception occurred.\n");
						}
					}
				}));
		}
	}

	~ThreadPool()
	{
		{
			std::unique_lock<std::mutex> lock(_mutex);
			_isDead = true;
		}
		_cond.notify_all();
		for (auto& t : _threads) t.join();
	}

	//template<class F, class ...Args>
	//std::future<typename std::invoke_result_t<F, Args...>> EnqueueTask(
	//F&& f, Args&&... args) // old fashion
	template<class F, class ...Args> requires std::invocable<F, Args...> 
	auto EnqueueTask(F&& f, Args&&... args)
	{
		using RetType = typename std::invoke_result_t<F, Args...>;
		std::shared_ptr<std::packaged_task<RetType()>> task = std::make_shared<std::packaged_task<RetType()>>(
			//std::bind(std::forward<F>(f), std::forward<Args>(args)...) // old fashion
			std::bind_front(std::forward<F>(f), std::forward<Args>(args)...)
			);
		std::future<RetType> ret = task->get_future();
		{
			std::unique_lock<std::mutex> lock(_mutex);
			if (_isDead) assert(false && "ThreadPool::EnqueueTask error: this should never happen!");
			_tasks.emplace([task = std::move(task)]() { (*task)(); });
		}
		
		/*
		// another way of doing this, same as bind
		auto task = [f = std::forward<F>(f), ...args = std::forward<Args>(args)]() mutable {
			std::invoke(std::move(f), std::move(args)...);
		};
		std::future<RetType> ret = task->get_future();
		{
			std::unique_lock<std::mutex> lock(_mutex);
			_tasks.emplace();
		}
		*/
			
		_cond.notify_one();
		return ret;
	}
};

