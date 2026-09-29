#include "stdafx.h"
#include "helpers/MemoryHelper.h"
#include <cstdlib>
#include <stdexcept>

void* MemoryHelper::alloc(size_t size)
{
    void* memory = nullptr;
    if (posix_memalign(&memory, 32, std::max<size_t>(size, 32)) != 0)
        throw std::bad_alloc();
    return memory;
}

void MemoryHelper::free(void* ptr) { std::free(ptr); }
