#pragma once
#include <shared_mutex>

class ReadWriteLock
{
	std::shared_mutex _lock{};
public:
	std::unique_lock<std::shared_mutex> OnWrite()
	{
		return std::unique_lock<std::shared_mutex>(_lock);
	}
	std::shared_lock<std::shared_mutex> OnRead()
	{
		return std::shared_lock<std::shared_mutex>(_lock);
	}
};