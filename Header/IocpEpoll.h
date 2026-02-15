#pragma once
#ifdef _WIN32
#include <winsock2.h>
#include <mswsock.h>
#include <windows.h>

#include <assert.h>
#include <cstdint>
#include <deque>
#include <list>
#include <mutex>
#include <utility>

#include "RBTree.h"

// IOCP-based, epoll-like readiness wrapper for SOCKET only.
// Read readiness is implemented via overlapped WSARecv + MSG_PEEK.
// Write readiness is optimistic: any socket in WRITE watch list is treated as ready.
class IocpEpoll
{
public:
	enum Event : uint32_t
	{
		EventRead = 0x1,
		EventWrite = 0x2,
		EventError = 0x4,
		EventHangup = 0x8
	};

	struct EventItem
	{
		SOCKET sock = INVALID_SOCKET;
		uint32_t events = 0;
		void* userData = nullptr;
	};

	IocpEpoll()
	{
		_iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 0);
	}

	~IocpEpoll()
	{
		Close();
	}

	IocpEpoll(const IocpEpoll&) = delete;
	IocpEpoll& operator= (const IocpEpoll&) = delete;

	bool IsValid() const { return _iocp != nullptr; }

	bool Add(SOCKET sock, uint32_t events, void* userData = nullptr)
	{
		if (!IsValid() || sock == INVALID_SOCKET)
			return false;

		std::lock_guard<std::mutex> lock(_mutex);
		if (_entries.Contains(sock))
			return false;

		HANDLE h = CreateIoCompletionPort(reinterpret_cast<HANDLE>(sock), _iocp, 0, 0);
		if (!h)
			return false;

		Entry* entry = new Entry(sock, events, userData);
		entry->allIt = _allEntries.insert(_allEntries.end(), entry);
		_entries.Insert(sock, entry);

		if (events & EventWrite)
			AddWriteWatchLocked(entry);
		if (events & EventRead)
			ArmReadLocked(entry);
		return true;
	}

	bool Mod(SOCKET sock, uint32_t events, void* userData = nullptr)
	{
		std::lock_guard<std::mutex> lock(_mutex);
		Entry** p = _entries.Find(sock);
		if (!p)
			return false;

		Entry* entry = *p;
		entry->events = events;
		entry->userData = userData;

		if (events & EventWrite)
			AddWriteWatchLocked(entry);
		else
			RemoveWriteWatchLocked(entry);

		if (events & EventRead)
			ArmReadLocked(entry);
		else if (entry->readPending)
			CancelIoEx(reinterpret_cast<HANDLE>(entry->sock), &entry->readCtx.ov);

		return true;
	}

	bool Del(SOCKET sock)
	{
		std::lock_guard<std::mutex> lock(_mutex);
		Entry** p = _entries.Find(sock);
		if (!p)
			return false;

		Entry* entry = *p;
		entry->closing = true;
		_entries.Erase(sock);
		RemoveWriteWatchLocked(entry);
		RemoveFromReadyLocked(entry);

		if (entry->readPending)
			CancelIoEx(reinterpret_cast<HANDLE>(entry->sock), &entry->readCtx.ov);

		TryDeleteLocked(entry);
		return true;
	}

	// timeoutMs < 0 => infinite
	int Wait(EventItem* outEvents, int maxEvents, int timeoutMs)
	{
		if (!outEvents || maxEvents <= 0 || !IsValid())
			return 0;

		DWORD timeout = (timeoutMs < 0) ? INFINITE : static_cast<DWORD>(timeoutMs);

		{
			std::lock_guard<std::mutex> lock(_mutex);
			QueueWriteReadyLocked();
			int count = PopReadyLocked(outEvents, maxEvents);
			if (count > 0)
				return count;
		}

		DWORD bytes = 0;
		ULONG_PTR key = 0;
		OVERLAPPED* ov = nullptr;
		BOOL ok = GetQueuedCompletionStatus(_iocp, &bytes, &key, &ov, timeout);
		if (!ok && !ov)
			return 0; // timeout or spurious error without overlapped

		DWORD err = ok ? 0 : GetLastError();
		{
			std::lock_guard<std::mutex> lock(_mutex);
			HandleCompletionLocked(ok, bytes, key, ov, err);

			// Drain additional completions without blocking.
			while (true)
			{
				bytes = 0;
				key = 0;
				ov = nullptr;
				ok = GetQueuedCompletionStatus(_iocp, &bytes, &key, &ov, 0);
				if (!ok && !ov)
					break;
				err = ok ? 0 : GetLastError();
				HandleCompletionLocked(ok, bytes, key, ov, err);
			}

			QueueWriteReadyLocked();
			return PopReadyLocked(outEvents, maxEvents);
		}
	}

	void Wakeup()
	{
		if (IsValid())
			PostQueuedCompletionStatus(_iocp, 0, kWakeKey, nullptr);
	}

	void Close()
	{
		if (!IsValid())
			return;

		std::unique_lock<std::mutex> lock(_mutex);
		for (Entry* entry : _allEntries)
		{
			entry->closing = true;
			if (_entries.Contains(entry->sock))
				_entries.Erase(entry->sock);
			RemoveWriteWatchLocked(entry);
			RemoveFromReadyLocked(entry);
			if (entry->readPending)
				CancelIoEx(reinterpret_cast<HANDLE>(entry->sock), &entry->readCtx.ov);
		}

		while (_pendingOps > 0)
		{
			lock.unlock();
			DWORD bytes = 0;
			ULONG_PTR key = 0;
			OVERLAPPED* ov = nullptr;
			BOOL ok = GetQueuedCompletionStatus(_iocp, &bytes, &key, &ov, INFINITE);
			DWORD err = ok ? 0 : GetLastError();
			lock.lock();
			HandleCompletionLocked(ok, bytes, key, ov, err);
		}

		for (Entry* entry : _allEntries)
			delete entry;
		_allEntries.clear();
		_writeWatch.clear();
		_entries.Clear();
		CloseHandle(_iocp);
		_iocp = nullptr;
	}

private:
	enum class Op : unsigned char
	{
		Read
	};

	struct Entry;

	struct PerIoContext
	{
		OVERLAPPED ov;
		WSABUF buf;
		char peekByte;
		Op op;
		Entry* owner;

		PerIoContext()
		{
			ZeroMemory(&ov, sizeof(ov));
			buf.len = 1;
			buf.buf = &peekByte;
			op = Op::Read;
			owner = nullptr;
		}

		void Reset()
		{
			ZeroMemory(&ov, sizeof(ov));
		}
	};

	struct Entry
	{
		SOCKET sock = INVALID_SOCKET;
		uint32_t events = 0;
		void* userData = nullptr;
		bool inReady = false;
		bool closing = false;
		bool readPending = false;
		bool watchWrite = false;
		uint32_t pendingEvents = 0;
		std::list<Entry*>::iterator allIt{};
		std::list<Entry*>::iterator writeIt{};
		PerIoContext readCtx;

		Entry(SOCKET s, uint32_t e, void* u) :
			sock(s), events(e), userData(u)
		{
			readCtx.owner = this;
		}
	};

private:
	static constexpr ULONG_PTR kWakeKey = 1;

	HANDLE _iocp = nullptr;
	int _pendingOps = 0;

	RBTree<SOCKET, Entry*> _entries;
	std::list<Entry*> _allEntries;
	std::list<Entry*> _writeWatch;
	std::deque<Entry*> _ready;
	std::mutex _mutex;

	static bool IsDisconnectError(DWORD err)
	{
		return err == WSAECONNRESET ||
			err == WSAECONNABORTED ||
			err == WSAESHUTDOWN ||
			err == ERROR_NETNAME_DELETED;
	}

	static PerIoContext* ContextFromOverlapped(OVERLAPPED* ov)
	{
		return reinterpret_cast<PerIoContext*>(ov);
	}

	void AddWriteWatchLocked(Entry* entry)
	{
		if (entry->watchWrite)
			return;
		entry->watchWrite = true;
		entry->writeIt = _writeWatch.insert(_writeWatch.end(), entry);
	}

	void RemoveWriteWatchLocked(Entry* entry)
	{
		if (!entry->watchWrite)
			return;
		entry->watchWrite = false;
		_writeWatch.erase(entry->writeIt);
	}

	void RemoveFromReadyLocked(Entry* entry)
	{
		if (!entry->inReady)
			return;
		for (auto it = _ready.begin(); it != _ready.end(); ++it)
		{
			if (*it == entry)
			{
				_ready.erase(it);
				break;
			}
		}
		entry->inReady = false;
		entry->pendingEvents = 0;
	}

	void TryDeleteLocked(Entry* entry)
	{
		if (!entry->closing || entry->readPending || entry->inReady)
			return;
		_allEntries.erase(entry->allIt);
		delete entry;
	}

	void ArmReadLocked(Entry* entry)
	{
		if (entry->closing || entry->readPending)
			return;

		entry->readCtx.Reset();
		DWORD flags = MSG_PEEK;
		DWORD bytes = 0;
		int rc = WSARecv(entry->sock, &entry->readCtx.buf, 1, &bytes, &flags, &entry->readCtx.ov, nullptr);
		if (rc == 0)
		{
			entry->readPending = true;
			_pendingOps++;
			return;
		}

		int err = WSAGetLastError();
		if (err == WSA_IO_PENDING)
		{
			entry->readPending = true;
			_pendingOps++;
			return;
		}

		uint32_t ev = EventError;
		if (IsDisconnectError(static_cast<DWORD>(err)))
			ev |= EventHangup;
		EnqueueReadyLocked(entry, ev);
	}

	void EnqueueReadyLocked(Entry* entry, uint32_t events)
	{
		if (events == 0 || entry->closing)
			return;
		entry->pendingEvents |= events;
		if (!entry->inReady)
		{
			entry->inReady = true;
			_ready.push_back(entry);
		}
	}

	void QueueWriteReadyLocked()
	{
		for (Entry* entry : _writeWatch)
		{
			if (!entry->closing)
				EnqueueReadyLocked(entry, EventWrite);
		}
	}

	int PopReadyLocked(EventItem* outEvents, int maxEvents)
	{
		int count = 0;
		while (count < maxEvents && !_ready.empty())
		{
			Entry* entry = _ready.front();
			_ready.pop_front();
			entry->inReady = false;
			uint32_t events = entry->pendingEvents;
			entry->pendingEvents = 0;

			// Mask readable/writable by interest, keep ERR/HUP always.
			uint32_t mask = 0;
			if (entry->events & EventRead) mask |= EventRead;
			if (entry->events & EventWrite) mask |= EventWrite;
			mask |= (EventError | EventHangup);
			events &= mask;

			if (events != 0)
			{
				outEvents[count].sock = entry->sock;
				outEvents[count].events = events;
				outEvents[count].userData = entry->userData;
				count++;
			}

			if (!entry->closing && (entry->events & EventRead))
				ArmReadLocked(entry);
			if (entry->closing)
				TryDeleteLocked(entry);
		}
		return count;
	}

	void HandleCompletionLocked(BOOL ok, DWORD bytes, ULONG_PTR key, OVERLAPPED* ov, DWORD err)
	{
		if (ok && ov == nullptr && key == kWakeKey)
			return;
		if (!ov)
			return;

		PerIoContext* ctx = ContextFromOverlapped(ov);
		Entry* entry = ctx->owner;
		if (!entry)
			return;

		if (entry->readPending)
		{
			entry->readPending = false;
			assert(_pendingOps > 0);
			_pendingOps--;
		}

		if (!ok && err == ERROR_OPERATION_ABORTED)
		{
			TryDeleteLocked(entry);
			return;
		}

		uint32_t ev = 0;
		if (!ok)
		{
			ev |= EventError;
			if (IsDisconnectError(err))
				ev |= EventHangup;
		}
		else
		{
			if (ctx->op == Op::Read)
			{
				ev |= EventRead;
				if (bytes == 0)
					ev |= EventHangup;
			}
		}

		// Report ERR/HUP even if no read interest. Read requires interest.
		if (!(entry->events & EventRead))
			ev &= ~EventRead;

		EnqueueReadyLocked(entry, ev);
	}
};

#endif // _WIN32
