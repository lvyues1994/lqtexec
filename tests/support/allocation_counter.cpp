#include "support/allocation_counter.h"

#include <lexec/detail/config.hpp>

#include <atomic>
#include <cstdlib>
#include <new>

namespace {

std::atomic<std::size_t> allocations{0};

void *checked(void *const memory) {
    if (memory == nullptr) {
#if LEXEC_HAS_EXCEPTIONS
        throw std::bad_alloc{};
#else
        std::abort();
#endif
    }
    return memory;
}

void *countedAllocate(std::size_t const size) {
    allocations.fetch_add(1, std::memory_order_relaxed);
    return checked(std::malloc(size == 0 ? 1 : size));
}

void *countedAllocate(std::size_t const size, std::align_val_t const align) {
    allocations.fetch_add(1, std::memory_order_relaxed);
    auto const alignment = static_cast<std::size_t>(align);
    auto const bytes = size == 0 ? alignment : (size + alignment - 1) / alignment * alignment;
    return checked(std::aligned_alloc(alignment, bytes));
}

} // namespace

std::size_t lqtexec_test::allocationCount() noexcept { return allocations.load(std::memory_order_relaxed); }

void *operator new(std::size_t const size) { return countedAllocate(size); }
void *operator new[](std::size_t const size) { return countedAllocate(size); }
void *operator new(std::size_t const size, std::align_val_t const alignment) { return countedAllocate(size, alignment); }
void *operator new[](std::size_t const size, std::align_val_t const alignment) { return countedAllocate(size, alignment); }

void operator delete(void *const memory) noexcept { std::free(memory); }
void operator delete[](void *const memory) noexcept { std::free(memory); }
void operator delete(void *const memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void *const memory, std::size_t) noexcept { std::free(memory); }
void operator delete(void *const memory, std::align_val_t) noexcept { std::free(memory); }
void operator delete[](void *const memory, std::align_val_t) noexcept { std::free(memory); }
void operator delete(void *const memory, std::size_t, std::align_val_t) noexcept { std::free(memory); }
void operator delete[](void *const memory, std::size_t, std::align_val_t) noexcept { std::free(memory); }
