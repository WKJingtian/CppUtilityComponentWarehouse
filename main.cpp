#include <iostream>
#include "Header/LRUCache.h"

int main(int argc, char** args)
{
	std::cout << "Hello World" << std::endl;

	LRUCache<int, int> testCache = LRUCache<int, int>(3);

	testCache.Put(1, 1);
	testCache.Put(2, 2);
	testCache.Put(3, 3);
	std::cout << testCache.Get(1).IsValid() << std::endl;

	return 0;
}