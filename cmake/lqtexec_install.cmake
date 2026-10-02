# Installs lqtexec as a relocatable CMake package: find_package(lqtexec CONFIG) then gives
# lqtexec::lqtexec, and lqtexec::network when it was built.

include(CMakePackageConfigHelpers)

set(lqtexec_install_targets lqtexec lqtexec_lexec_headers)
set(lqtexec_qt_components Core)
if(TARGET lqtexec_network)
    list(APPEND lqtexec_install_targets lqtexec_network)
    list(APPEND lqtexec_qt_components Network)
endif()
set(lqtexec_cmake_dir "${CMAKE_INSTALL_LIBDIR}/cmake/lqtexec")

install(TARGETS ${lqtexec_install_targets} EXPORT lqtexecTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY "${PROJECT_SOURCE_DIR}/lqtexec/include/" DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
if(TARGET lqtexec_network)
    install(DIRECTORY "${PROJECT_SOURCE_DIR}/network/include/" DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
endif()
if(LQTEXEC_BUNDLE_LEXEC_HEADERS)
    install(DIRECTORY "${lqtexec_lexec_include}/lexec" DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/lqtexec/vendor")
endif()
install(EXPORT lqtexecTargets NAMESPACE lqtexec:: DESTINATION "${lqtexec_cmake_dir}")

configure_package_config_file("${PROJECT_SOURCE_DIR}/cmake/lqtexecConfig.cmake.in"
    "${PROJECT_BINARY_DIR}/lqtexecConfig.cmake"
    INSTALL_DESTINATION "${lqtexec_cmake_dir}")
write_basic_package_version_file("${PROJECT_BINARY_DIR}/lqtexecConfigVersion.cmake"
    COMPATIBILITY SameMinorVersion)
install(FILES "${PROJECT_BINARY_DIR}/lqtexecConfig.cmake" "${PROJECT_BINARY_DIR}/lqtexecConfigVersion.cmake"
    DESTINATION "${lqtexec_cmake_dir}")
