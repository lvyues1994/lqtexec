#pragma once

#include <cstddef>

namespace lqtexec_test {

// Number of global operator new calls so far in this process.
std::size_t allocationCount() noexcept;

} // namespace lqtexec_test
