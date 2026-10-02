# Build policy for lqtexec's own library, tests and examples. Consumers never see it.

if(NOT CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang)$")
    message(FATAL_ERROR "lqtexec development builds support GCC and Clang only")
endif()

set(LQTEXEC_CXX_STANDARD 17 CACHE STRING "C++ standard for lqtexec's own builds")
set_property(CACHE LQTEXEC_CXX_STANDARD PROPERTY STRINGS 17 20 23)
set(LQTEXEC_SANITIZERS "" CACHE STRING "Value passed to -fsanitize= for lqtexec's own builds")
set(LQTEXEC_TEST_LAUNCHER "" CACHE STRING "Command prefix, as a CMake list, for running lqtexec's tests")
option(LQTEXEC_DISABLE_EXCEPTIONS "Build lqtexec's own targets with -fno-exceptions" OFF)
option(LQTEXEC_WARNINGS_AS_ERRORS "Treat warnings as errors in lqtexec's own targets" ON)

set(CMAKE_CXX_STANDARD ${LQTEXEC_CXX_STANDARD})
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# The exception mode and the sanitizers apply to the whole tree, lexec's runtime included:
# lexec's inline functions must be compiled alike in every translation unit.
if(LQTEXEC_DISABLE_EXCEPTIONS)
    string(APPEND CMAKE_CXX_FLAGS " -fno-exceptions")
endif()
if(LQTEXEC_SANITIZERS)
    string(APPEND CMAKE_CXX_FLAGS
        " -fsanitize=${LQTEXEC_SANITIZERS} -fno-sanitize-recover=all -fno-omit-frame-pointer")
    string(APPEND CMAKE_EXE_LINKER_FLAGS " -fsanitize=${LQTEXEC_SANITIZERS}")
endif()

add_library(lqtexec_dev INTERFACE)
target_compile_options(lqtexec_dev INTERFACE
    -Wall -Wextra -Wpedantic
    -Wconversion -Wsign-conversion -Wshadow
    -Wold-style-cast -Wnon-virtual-dtor -Wzero-as-null-pointer-constant
    $<$<BOOL:${LQTEXEC_WARNINGS_AS_ERRORS}>:-Werror>)
target_compile_definitions(lqtexec_dev INTERFACE
    LQTEXEC_TEST_EXCEPTIONS_ENABLED=$<IF:$<BOOL:${LQTEXEC_DISABLE_EXCEPTIONS}>,0,1>)
