#include <cassert>
#include <iostream>
#include "Header/LockFreeQueue.h"

int main(int argc, char** args)
{
    (void)argc;
    (void)args;

    LockFreeQueue<int> q;
    int value = 0;

    assert(!q.Dequeue(value));

    q.Enqueue(1);
    q.Enqueue(2);
    q.Enqueue(3);

    assert(q.Dequeue(value) && value == 1);
    assert(q.Dequeue(value) && value == 2);
    assert(q.Dequeue(value) && value == 3);
    assert(!q.Dequeue(value));

    std::cout << "LockFreeQueue basic test passed." << std::endl;
    return 0;
}
