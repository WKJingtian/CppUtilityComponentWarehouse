#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>

template <typename T>
class MPMCRingBuffer
{
private:
    using StorageType = std::aligned_storage_t<sizeof(T), alignof(T)>;

    struct Slot
    {
        std::atomic<std::size_t> sequence;
        StorageType storage;
    };

    const std::size_t _capacity;
    std::unique_ptr<Slot[]> _buffer;

    alignas(64) std::atomic<std::size_t> _enqueuePos;
    alignas(64) std::atomic<std::size_t> _dequeuePos;

private:
    T* Element(Slot& slot)
    {
        return std::launder(reinterpret_cast<T*>(&slot.storage));
    }

    const T* Element(const Slot& slot) const
    {
        return std::launder(reinterpret_cast<const T*>(&slot.storage));
    }

public:
    explicit MPMCRingBuffer(std::size_t capacity)
        : _capacity(capacity),
          _buffer(std::make_unique<Slot[]>(_capacity)),
          _enqueuePos(0),
          _dequeuePos(0)
    {
        if (_capacity == 0)
        {
            throw std::invalid_argument("MPMCRingBuffer capacity must be greater than 0");
        }

        for (std::size_t i = 0; i < _capacity; ++i)
        {
            _buffer[i].sequence.store(i, std::memory_order_relaxed);
        }
    }

    ~MPMCRingBuffer()
    {
        std::size_t pos = _dequeuePos.load(std::memory_order_relaxed);
        const std::size_t tail = _enqueuePos.load(std::memory_order_relaxed);

        while (pos != tail)
        {
            Slot& slot = _buffer[pos % _capacity];
            if (slot.sequence.load(std::memory_order_acquire) == pos + 1)
            {
                Element(slot)->~T();
            }
            ++pos;
        }
    }

    MPMCRingBuffer(const MPMCRingBuffer&) = delete;
    MPMCRingBuffer& operator=(const MPMCRingBuffer&) = delete;
    MPMCRingBuffer(MPMCRingBuffer&&) = delete;
    MPMCRingBuffer& operator=(MPMCRingBuffer&&) = delete;

    template <typename... Args>
    bool TryEmplace(Args&&... args)
    {
        std::size_t pos = _enqueuePos.load(std::memory_order_relaxed);
        while (true)
        {
            Slot& slot = _buffer[pos % _capacity];
            const std::size_t seq = slot.sequence.load(std::memory_order_acquire);
            const std::intptr_t dif = static_cast<std::intptr_t>(seq) - static_cast<std::intptr_t>(pos);

            if (dif == 0)
            {
                if (_enqueuePos.compare_exchange_weak(
                        pos, pos + 1,
                        std::memory_order_relaxed,
                        std::memory_order_relaxed))
                {
                    new (&slot.storage) T(std::forward<Args>(args)...);
                    slot.sequence.store(pos + 1, std::memory_order_release);
                    return true;
                }
            }
            else if (dif < 0)
            {
                return false;
            }
            else
            {
                pos = _enqueuePos.load(std::memory_order_relaxed);
            }
        }
    }

    bool TryPush(const T& value)
    {
        return TryEmplace(value);
    }

    bool TryPush(T&& value)
    {
        return TryEmplace(std::move(value));
    }

    bool TryPop(T& out)
    {
        std::size_t pos = _dequeuePos.load(std::memory_order_relaxed);
        while (true)
        {
            Slot& slot = _buffer[pos % _capacity];
            const std::size_t seq = slot.sequence.load(std::memory_order_acquire);
            const std::intptr_t dif = static_cast<std::intptr_t>(seq) - static_cast<std::intptr_t>(pos + 1);

            if (dif == 0)
            {
                if (_dequeuePos.compare_exchange_weak(
                        pos, pos + 1,
                        std::memory_order_relaxed,
                        std::memory_order_relaxed))
                {
                    T* element = Element(slot);
                    out = std::move(*element);
                    element->~T();
                    slot.sequence.store(pos + _capacity, std::memory_order_release);
                    return true;
                }
            }
            else if (dif < 0)
            {
                return false;
            }
            else
            {
                pos = _dequeuePos.load(std::memory_order_relaxed);
            }
        }
    }

    bool Empty() const
    {
        return _enqueuePos.load(std::memory_order_acquire) == _dequeuePos.load(std::memory_order_acquire);
    }

    std::size_t Size() const
    {
        return _enqueuePos.load(std::memory_order_acquire) - _dequeuePos.load(std::memory_order_acquire);
    }

    std::size_t Capacity() const
    {
        return _capacity;
    }
};
