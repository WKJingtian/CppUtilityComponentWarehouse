#pragma once
#include <atomic>
#include <cassert>
#include <vector>

template <typename T>
class LockFreeQueue
{
private:
    struct Node
    {
        T data;
        std::atomic<Node*> next;
        Node() : next(nullptr) {}
        Node(const T& val) : data(val), next(nullptr) {}
    };

    static constexpr int kHazardSlotsPerThread = 2;
    static constexpr int kMaxThreads = 64;
    static constexpr int kMaxHazardPointers = kHazardSlotsPerThread * kMaxThreads;
    static constexpr int kRetireThreshold = 64;

    static inline std::atomic<Node*> _hazards[kMaxHazardPointers]{};
    static inline std::atomic<int> _hazardThreadCount{0};

    static int ThreadIndex()
    {
        static thread_local int index = -1;
        if (index < 0)
        {
            index = _hazardThreadCount.fetch_add(1, std::memory_order_relaxed);
            assert(index < kMaxThreads && "LockFreeQueue hazard pointers: too many threads");
        }
        return index;
    }

    static std::atomic<Node*>& HazardSlot(int slot)
    {
        const int index = ThreadIndex() * kHazardSlotsPerThread + slot;
        return _hazards[index];
    }

    static void SetHazard(int slot, Node* ptr)
    {
        HazardSlot(slot).store(ptr, std::memory_order_release);
    }

    static void ClearHazard(int slot)
    {
        HazardSlot(slot).store(nullptr, std::memory_order_release);
    }

    static std::vector<Node*>& RetiredList()
    {
        static thread_local std::vector<Node*> retired;
        return retired;
    }

    static void ScanAndReclaim(std::vector<Node*>& retired)
    {
        std::vector<Node*> hazards;
        hazards.reserve(kMaxHazardPointers);
        for (int i = 0; i < kMaxHazardPointers; ++i)
        {
            Node* p = _hazards[i].load(std::memory_order_acquire);
            if (p) hazards.push_back(p);
        }

        size_t keep = 0;
        for (Node* node : retired)
        {
            bool protectedNode = false;
            for (Node* h : hazards)
            {
                if (h == node)
                {
                    protectedNode = true;
                    break;
                }
            }
            if (protectedNode)
            {
                retired[keep++] = node;
            }
            else
            {
                delete node;
            }
        }
        retired.resize(keep);
    }

    static void RetireNode(Node* node)
    {
        auto& retired = RetiredList();
        retired.push_back(node);
        if (retired.size() >= kRetireThreshold)
        {
            ScanAndReclaim(retired);
        }
    }

    std::atomic<Node*> _head;
    std::atomic<Node*> _tail;

public:
    LockFreeQueue()
    {
        Node* dummy = new Node();
        _head.store(dummy, std::memory_order_relaxed);
        _tail.store(dummy, std::memory_order_relaxed);
    }
    
    ~LockFreeQueue()
    {
        Node* current = _head.load(std::memory_order_relaxed);
        while (current)
        {
            Node* next = current->next.load(std::memory_order_relaxed);
            delete current;
            current = next;
        }
        _head.store(nullptr, std::memory_order_relaxed);
        _tail.store(nullptr, std::memory_order_relaxed);
    }
    
    void Enqueue(const T& data)
    {
        Node* newNode = new Node(data);
        Node* oldTail = nullptr;
        while (1)
        {
            oldTail = _tail.load(std::memory_order_acquire);
            SetHazard(0, oldTail);
            if (oldTail != _tail.load(std::memory_order_acquire))
            {
                continue;
            }
            Node* next = oldTail->next.load(std::memory_order_acquire);
            if (oldTail == _tail.load(std::memory_order_acquire))
            {
                if (next == nullptr)
                {
                    if (oldTail->next.compare_exchange_weak(
                            next, newNode,
                            std::memory_order_release,
                            std::memory_order_relaxed))
                    {
                        _tail.compare_exchange_weak(
                            oldTail, newNode,
                            std::memory_order_release,
                            std::memory_order_relaxed);
                        ClearHazard(0);
                        return;
                    }
                }
                else
                {
                    _tail.compare_exchange_weak(
                        oldTail, next,
                        std::memory_order_release,
                        std::memory_order_relaxed);
                }
            }
        }
    }
    
    bool Dequeue(T& out)
    {
        while (1)
        {
            Node* currentHead = _head.load(std::memory_order_acquire);
            Node* currentTail = _tail.load(std::memory_order_acquire);
            SetHazard(0, currentHead);
            if (currentHead != _head.load(std::memory_order_acquire))
            {
                continue;
            }

            Node* headNext = currentHead->next.load(std::memory_order_acquire);
            SetHazard(1, headNext);
            
            if (currentHead == _head.load(std::memory_order_acquire))
            {
                if (currentHead == currentTail)
                {
                    if (headNext == nullptr)
                    {
                        ClearHazard(1);
                        ClearHazard(0);
                        return false;
                    }
                    _tail.compare_exchange_weak(
                        currentTail, headNext,
                        std::memory_order_release,
                        std::memory_order_relaxed);
                }
                else
                {
                    if (_head.compare_exchange_weak(
                            currentHead, headNext,
                            std::memory_order_acq_rel,
                            std::memory_order_relaxed))
                    {
                        out = headNext->data;
                        ClearHazard(1);
                        ClearHazard(0);
                        RetireNode(currentHead);
                        return true;
                    }
                }
            }
        }
    }
};
