cmake_minimum_required(VERSION 3.25)
project(lqtexec_consumer LANGUAGES CXX)

# Twice: a package found again must not define its targets again.
find_package(lqtexec CONFIG REQUIRED)
find_package(lqtexec CONFIG REQUIRED)

add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE lqtexec::lqtexec)
if(TARGET lqtexec::network)
    target_link_libraries(consumer PRIVATE lqtexec::network)
    target_compile_definitions(consumer PRIVATE CONSUMER_HAS_NETWORK)
endif()
