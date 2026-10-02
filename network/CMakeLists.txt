add_library(lqtexec_network STATIC src/NetworkClient.cpp)
add_library(lqtexec::network ALIAS lqtexec_network)
set_target_properties(lqtexec_network PROPERTIES EXPORT_NAME network)
target_include_directories(lqtexec_network PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
target_link_libraries(lqtexec_network PUBLIC lqtexec::lqtexec ${lqtexec_qt}::Network)
if(TARGET lqtexec_dev)
    target_link_libraries(lqtexec_network PRIVATE "$<BUILD_INTERFACE:lqtexec_dev>")
endif()
