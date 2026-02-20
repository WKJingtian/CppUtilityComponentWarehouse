#pragma once
#include <atomic>

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

    std::atomic<Node*> _head;
    std::atomic<Node*> _tail;

public:
    LockFreeQueue()
    {
        Node* dummy = new Node();
        _head.store(dummy, std::memory_order_seq_cst);
        _tail.store(dummy, std::memory_order_seq_cst);
    }
    
    ~LockFreeQueue()
    {
        Node* current = _head.load(std::memory_order_seq_cst);
        while (current)
        {
            Node* next = current->next.load(std::memory_order_seq_cst);
            delete current;
            current = next;
        }
        _head.store(nullptr, std::memory_order_seq_cst);
        _tail.store(nullptr, std::memory_order_seq_cst);
    }
    
    void Enqueue(const T& data)
    {
        Node* newNode = new Node(data);
        Node* oldTail = nullptr;
        while (1)
        {
            oldTail = _tail.load(std::memory_order_seq_cst);
            Node* next = oldTail->next.load(std::memory_order_seq_cst);
            if (oldTail == _tail.load(std::memory_order_seq_cst))
            {
                if (next == nullptr)
                {
                    if (oldTail->next.compare_exchange_weak(next, newNode, std::memory_order_seq_cst))
                    {
                        _tail.compare_exchange_weak(oldTail, newNode, std::memory_order_seq_cst);
                        return;
                    }
                }
                else
                {
                    _tail.compare_exchange_weak(oldTail, next, std::memory_order_seq_cst);
                }
            }
        }
    }
    
    bool Dequeue(T& out)
    {
        while (1)
        {
            Node* currentHead = _head.load(std::memory_order_seq_cst);
            Node* currentTail = _tail.load(std::memory_order_seq_cst);
            Node* headNext = currentHead->next.load(std::memory_order_seq_cst);
            
            if (currentHead == _head.load(std::memory_order_seq_cst))
            {
                if (currentHead == currentTail)
                {
                    if (headNext == nullptr)
                    {
                        return false;
                    }
                    _tail.compare_exchange_weak(currentTail, headNext, std::memory_order_seq_cst);
                }
                else
                {
                    if (_head.compare_exchange_weak(currentHead, headNext, std::memory_order_seq_cst))
                    {
                        out = headNext->data;
                        delete currentHead;
                        return true;
                    }
                }
            }
        }
    }
};
