#pragma once
#include <memory>
#include <mutex>

template <typename T>
class Singleton
{
public:
	static T* GetInstance()
	{
		T* ptr = _instance.load(std::memory_order_acquire);
		if (ptr == nullptr)
		{
			auto lock = std::lock_guard(_mutex);
			ptr = _instance.load(std::memory_order_relaxed);
			if (ptr == nullptr)
			{
				ptr = new T();
				atexit(Destructor);
				_instance.store(ptr, std::memory_order_release);
			}
		}
		return ptr;
	}

protected:
	Singleton() {}
	~Singleton() {}

private:
	static void Destructor()
	{
		T* ptr = _instance.load();
		if (ptr != nullptr)
			delete ptr;
	}
	
	Singleton(const Singleton&) = delete;
	Singleton(Singleton&&) = delete;
	Singleton& operator= (const Singleton&) = delete;
	Singleton& operator= (Singleton&&) = delete;

	static std::atomic<T*> _instance;
	static std::mutex _mutex;
};

template<typename T>
std::atomic<T*> Singleton<T>::_instance = std::atomic<T*>();
template<typename T>
std::mutex Singleton<T>::_mutex = std::mutex();