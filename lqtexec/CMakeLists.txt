add_library(lqtexec STATIC
    src/EventLoopContext.cpp
    src/EventLoopThread.cpp
    src/ExecWithScope.cpp)
if(UNIX)
    target_sources(lqtexec PRIVATE src/SignalStop.cpp)
endif()
add_library(lqtexec::lqtexec ALIAS lqtexec)
target_include_directories(lqtexec PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
target_compile_features(lqtexec PUBLIC cxx_std_17)
target_link_libraries(lqtexec PUBLIC lqtexec_lexec_headers ${lqtexec_qt}::Core)
if(TARGET lqtexec_dev)
    target_link_libraries(lqtexec PRIVATE "$<BUILD_INTERFACE:lqtexec_dev>")
endif()
