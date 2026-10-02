#pragma once

// lexec includes <execution>, which libstdc++ implements on TBB when TBB is installed, and
// TBB's headers declare members named emit(): Qt's `emit` keyword macro must not reach them.
#pragma push_macro("emit")
#undef emit
#include <lexec/execution.hpp>
#pragma pop_macro("emit")
