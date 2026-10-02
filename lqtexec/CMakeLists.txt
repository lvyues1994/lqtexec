add_library(lqtexec STATIC
    src/EventLoopContext.cpp
    src/EventLoopThread.cpp
    src/ExecWithScope.cpp)
add_library(lqtexec::lqtexec ALIAS lqtexec)
target_include_directories(lqtexec PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>")
target_compile_features(lqtexec PUBLIC cxx_std_17)
target_link_libraries(lqtexec PUBLIC lexec::lexec Qt::Core)
if(TARGET lqtexec_dev)
    target_link_libraries(lqtexec PRIVATE lqtexec_dev)
endif()
