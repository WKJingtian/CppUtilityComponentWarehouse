#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>

template <typename T>
class SPSCRingBuffer
{
private:
    using StorageType = std::aligned_storage_t<sizeof(T), alignof(T)>;

    const std::size_t _capacity;
    const std::size_t _bufferSize;
    std::unique_ptr<StorageType[]> _buffer;

    alignas(64) std::atomic<std::size_t> _head;
    alignas(64) std::atomic<std::size_t> _tail;

private:
    std::size_t NextIndex(std::size_t index) const
    {
        ++index;
        if (index == _bufferSize)
        {
            index = 0;
        }
        return index;
    }

    T* Slot(std::size_t index)
    {
        return std::launder(reinterpret_cast<T*>(&_buffer[index]));
    }

    const T* Slot(std::size_t index) const
    {
        return std::launder(reinterpret_cast<const T*>(&_buffer[index]));
    }

public:
    explicit SPSCRingBuffer(std::size_t capacity)
        : _capacity(capacity),
          _bufferSize(capacity + 1),
          _buffer(std::make_unique<StorageType[]>(_bufferSize)),
          _head(0),
          _tail(0)
    {
        if (_capacity == 0)
        {
            throw std::invalid_argument("SPSCRingBuffer capacity must be greater than 0");
        }
    }

    ~SPSCRingBuffer()
    {
        std::size_t tail = _tail.load(std::memory_order_relaxed);
        const std::size_t head = _head.load(std::memory_order_relaxed);

        while (tail != head)
        {
            Slot(tail)->~T();
            tail = NextIndex(tail);
        }
    }

    SPSCRingBuffer(const SPSCRingBuffer&) = delete;
    SPSCRingBuffer& operator=(const SPSCRingBuffer&) = delete;
    SPSCRingBuffer(SPSCRingBuffer&&) = delete;
    SPSCRingBuffer& operator=(SPSCRingBuffer&&) = delete;

    template <typename... Args>
    bool TryEmplace(Args&&... args)
    {
        const std::size_t head = _head.load(std::memory_order_relaxed);
        const std::size_t next = NextIndex(head);
        if (next == _tail.load(std::memory_order_acquire))
        {
            return false;
        }

        new (&_buffer[head]) T(std::forward<Args>(args)...);
        _head.store(next, std::memory_order_release);
        return true;
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
        const std::size_t tail = _tail.load(std::memory_order_relaxed);
        if (tail == _head.load(std::memory_order_acquire))
        {
            return false;
        }

        T* element = Slot(tail);
        out = std::move(*element);
        element->~T();
        _tail.store(NextIndex(tail), std::memory_order_release);
        return true;
    }

    bool Empty() const
    {
        return _head.load(std::memory_order_acquire) == _tail.load(std::memory_order_acquire);
    }

    bool Full() const
    {
        const std::size_t head = _head.load(std::memory_order_acquire);
        return NextIndex(head) == _tail.load(std::memory_order_acquire);
    }

    std::size_t Size() const
    {
        const std::size_t head = _head.load(std::memory_order_acquire);
        const std::size_t tail = _tail.load(std::memory_order_acquire);
        if (head >= tail)
        {
            return head - tail;
        }
        return _bufferSize - (tail - head);
    }

    std::size_t Capacity() const
    {
        return _capacity;
    }
};
