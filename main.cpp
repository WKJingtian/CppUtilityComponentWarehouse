#include <cassert>
#include <iostream>
#include "Header/Logger.h"

int main(int argc, char** args)
{
    LOG_WARN("test log");

    std::cout << "Hello World." << std::endl;
    return 0;
}
