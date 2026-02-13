#pragma once

#include <coroutine>
#include <cstdint>
#include <functional>
#include <list>
#include <unordered_map>
#include <utility>
#include <vector>

class TimerWheel
{
public:
	struct TimerHandle
	{
		uint64_t id = 0;
		bool IsValid() const { return id != 0; }
	};

	class SleepAwaiter
	{
	public:
		SleepAwaiter(TimerWheel& wheel, uint32_t delayMs)
			: _wheel(&wheel), _delayMs(delayMs)
		{
		}

		bool AwaitReady() const noexcept { return _delayMs == 0; }
		void AwaitSuspend(std::coroutine_handle<> handle) { _wheel->ScheduleCoroutine(_delayMs, handle); }
		void AwaitResume() const noexcept {}

	private:
		TimerWheel* _wheel = nullptr;
		uint32_t _delayMs = 0;
	};

	using Callback = std::function<void()>;

	TimerWheel() = delete;
	TimerWheel(uint32_t tickMs, uint32_t slotCount);

	TimerHandle ScheduleOnce(uint32_t delayMs, Callback cb);
	TimerHandle ScheduleEvery(uint32_t intervalMs, Callback cb);
	void Cancel(TimerHandle handle);

	void AdvanceByElapsedMs(uint32_t elapsedMs);
	SleepAwaiter SleepFor(uint32_t delayMs) { return SleepAwaiter(*this, delayMs); }

	uint32_t GetTickMs() const { return _tickMs; }
	uint32_t GetSlotCount() const { return static_cast<uint32_t>(_slots.size()); }

private:
	enum class TaskKind : uint8_t
	{
		Callback,
		Coroutine
	};

	struct TimerTask
	{
		uint64_t id = 0;
		uint32_t rounds = 0;
		uint32_t intervalTicks = 0;
		bool repeating = false;
		TaskKind kind = TaskKind::Callback;
		Callback callback{};
		std::coroutine_handle<> handle{};
	};

	struct TaskLocation
	{
		uint32_t slotIndex = 0;
		std::list<TimerTask>::iterator it;
	};

	uint32_t ToTicks(uint32_t delayMs) const;
	void InsertTask(TimerTask&& task, uint32_t delayTicks);
	void AdvanceOneTick();
	void ScheduleCoroutine(uint32_t delayMs, std::coroutine_handle<> handle);
	void RescheduleRepeat(const TimerTask& task);

	uint32_t _tickMs = 0;
	uint32_t _cursor = 0;
	uint64_t _accumMs = 0;
	uint64_t _nextId = 1;
	std::vector<std::list<TimerTask>> _slots{};
	std::unordered_map<uint64_t, TaskLocation> _index{};
};

inline TimerWheel::TimerWheel(uint32_t tickMs, uint32_t slotCount)
	: _tickMs(tickMs == 0 ? 1 : tickMs),
	_cursor(0),
	_accumMs(0),
	_nextId(1),
	_slots(slotCount == 0 ? 1 : slotCount)
{
}

inline TimerWheel::TimerHandle TimerWheel::ScheduleOnce(uint32_t delayMs, Callback cb)
{
	if (!cb || _slots.empty())
		return {};

	uint64_t id = _nextId++;
	TimerTask task{};
	task.id = id;
	task.repeating = false;
	task.kind = TaskKind::Callback;
	task.callback = std::move(cb);

	uint32_t delayTicks = ToTicks(delayMs);
	InsertTask(std::move(task), delayTicks);
	return { id };
}

inline TimerWheel::TimerHandle TimerWheel::ScheduleEvery(uint32_t intervalMs, Callback cb)
{
	if (!cb || _slots.empty())
		return {};

	uint32_t intervalTicks = ToTicks(intervalMs);

	uint64_t id = _nextId++;
	TimerTask task{};
	task.id = id;
	task.repeating = true;
	task.intervalTicks = intervalTicks;
	task.kind = TaskKind::Callback;
	task.callback = std::move(cb);

	InsertTask(std::move(task), intervalTicks);
	return { id };
}

inline void TimerWheel::Cancel(TimerHandle handle)
{
	if (!handle.IsValid())
		return;

	auto it = _index.find(handle.id);
	if (it == _index.end())
		return;

	auto slotIndex = it->second.slotIndex;
	if (slotIndex < _slots.size())
		_slots[slotIndex].erase(it->second.it);
	_index.erase(it);
}

inline void TimerWheel::AdvanceByElapsedMs(uint32_t elapsedMs)
{
	if (_slots.empty() || _tickMs == 0)
		return;

	_accumMs += elapsedMs;
	while (_accumMs >= _tickMs)
	{
		_accumMs -= _tickMs;
		AdvanceOneTick();
	}
}

inline uint32_t TimerWheel::ToTicks(uint32_t delayMs) const
{
	if (_tickMs == 0)
		return 1;
	uint64_t ticks = (static_cast<uint64_t>(delayMs) + _tickMs - 1) / _tickMs;
	return ticks == 0 ? 1 : static_cast<uint32_t>(ticks);
}

inline void TimerWheel::InsertTask(TimerTask&& task, uint32_t delayTicks)
{
	if (_slots.empty())
		return;

	uint32_t slotCount = static_cast<uint32_t>(_slots.size());
	uint32_t slotIndex = (_cursor + delayTicks) % slotCount;
	task.rounds = delayTicks / slotCount;

	auto& slot = _slots[slotIndex];
	slot.push_back(std::move(task));
	auto it = std::prev(slot.end());
	_index[it->id] = { slotIndex, it };
}

inline void TimerWheel::AdvanceOneTick()
{
	if (_slots.empty())
		return;

	_cursor = (_cursor + 1) % static_cast<uint32_t>(_slots.size());
	auto& slot = _slots[_cursor];

	std::vector<TimerTask> due{};
	due.reserve(slot.size());

	for (auto it = slot.begin(); it != slot.end();)
	{
		if (it->rounds > 0)
		{
			it->rounds--;
			++it;
			continue;
		}

		due.push_back(std::move(*it));
		_index.erase(it->id);
		it = slot.erase(it);
	}

	for (auto& task : due)
	{
		if (task.repeating)
			RescheduleRepeat(task);

		if (task.kind == TaskKind::Callback)
		{
			if (task.callback)
				task.callback();
			continue;
		}

		if (task.kind == TaskKind::Coroutine)
		{
			if (task.handle && !task.handle.done())
				task.handle.resume();
		}
	}
}

inline void TimerWheel::ScheduleCoroutine(uint32_t delayMs, std::coroutine_handle<> handle)
{
	if (!handle || handle.done() || _slots.empty())
		return;

	uint64_t id = _nextId++;
	TimerTask task{};
	task.id = id;
	task.repeating = false;
	task.kind = TaskKind::Coroutine;
	task.handle = handle;

	uint32_t delayTicks = ToTicks(delayMs);
	InsertTask(std::move(task), delayTicks);
}

inline void TimerWheel::RescheduleRepeat(const TimerTask& task)
{
	if (!task.repeating || task.kind != TaskKind::Callback || !task.callback)
		return;

	TimerTask next{};
	next.id = task.id;
	next.repeating = true;
	next.intervalTicks = task.intervalTicks;
	next.kind = TaskKind::Callback;
	next.callback = task.callback;

	InsertTask(std::move(next), task.intervalTicks);
}
