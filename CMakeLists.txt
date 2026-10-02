cmake_minimum_required(VERSION 3.25)
project(lqtexec VERSION 0.1.0 LANGUAGES CXX)

option(LQTEXEC_BUILD_TESTS "Build lqtexec's own tests" ${PROJECT_IS_TOP_LEVEL})
set(LQTEXEC_QT_MAJOR_VERSION "" CACHE STRING "Qt major version to build against, 5 or 6; empty prefers Qt 6")
set_property(CACHE LQTEXEC_QT_MAJOR_VERSION PROPERTY STRINGS "" 5 6)
set(LQTEXEC_LEXEC_SOURCE_DIR "" CACHE PATH "Local lexec source tree; empty fetches the pinned commit")
set(LQTEXEC_LEXEC_GIT_TAG e032d551d53d87209b83d07fd172890461db643b CACHE STRING "lexec commit fetched without a local tree")

if(LQTEXEC_BUILD_TESTS)
    include(cmake/lqtexec_dev.cmake)
endif()

if(NOT TARGET lexec::lexec)
    if(LQTEXEC_LEXEC_SOURCE_DIR)
        set(LEXEC_BUILD_TESTS OFF CACHE BOOL "Build lexec's own tests")
        add_subdirectory("${LQTEXEC_LEXEC_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/lexec" EXCLUDE_FROM_ALL)
    else()
        include(FetchContent)
        set(LEXEC_BUILD_TESTS OFF CACHE BOOL "Build lexec's own tests")
        FetchContent_Declare(lexec
            GIT_REPOSITORY https://github.com/lvyues1994/lexec.git
            GIT_TAG ${LQTEXEC_LEXEC_GIT_TAG})
        FetchContent_MakeAvailable(lexec)
    endif()
endif()

if(LQTEXEC_QT_MAJOR_VERSION)
    find_package(Qt${LQTEXEC_QT_MAJOR_VERSION} REQUIRED COMPONENTS Core)
else()
    find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Core)
    find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Core)
endif()
if(TARGET Qt5::Core AND Qt5Core_VERSION VERSION_LESS 5.15)
    message(FATAL_ERROR "lqtexec needs Qt 5.15 or Qt 6; found Qt ${Qt5Core_VERSION}")
endif()

add_subdirectory(lqtexec)

if(LQTEXEC_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
