include(FetchContent)

FetchContent_Declare(doctest
    URL https://github.com/doctest/doctest/archive/refs/tags/v2.5.3.tar.gz
    URL_HASH SHA256=174ebc4e769928959614789c5b4e9c3d0a0f81a62bb608756b127bfebfb21331)
set(DOCTEST_WITH_MAIN_IN_STATIC_LIB OFF)
set(DOCTEST_NO_INSTALL ON)
FetchContent_MakeAvailable(doctest)

file(GLOB lqtexec_test_sources CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/support/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/support/*.h")
if(LQTEXEC_SANITIZERS)
    # Sanitizer runtimes define the global allocation functions the counter replaces.
    list(FILTER lqtexec_test_sources EXCLUDE REGEX "/(allocation_counter|allocation_test)\\.(cpp|h)$")
endif()
if(TARGET lqtexec::network)
    file(GLOB lqtexec_network_test_sources CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/network/*.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/network/*.h")
    list(APPEND lqtexec_test_sources ${lqtexec_network_test_sources})
endif()
add_executable(lqtexec_tests ${lqtexec_test_sources})
set_target_properties(lqtexec_tests PROPERTIES AUTOMOC ON)
target_include_directories(lqtexec_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(lqtexec_tests PRIVATE lqtexec::lqtexec lexec::runtime lqtexec_dev doctest::doctest)
if(TARGET lqtexec::network)
    target_link_libraries(lqtexec_tests PRIVATE lqtexec::network)
endif()
# Defining this with exceptions enabled would switch doctest's exception assertions off.
target_compile_definitions(lqtexec_tests PRIVATE
    $<$<BOOL:${LQTEXEC_DISABLE_EXCEPTIONS}>:DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS>)
if(LQTEXEC_SANITIZERS)
    target_compile_definitions(lqtexec_tests PRIVATE LQTEXEC_TEST_NO_ALLOCATION_COUNTER)
endif()
add_test(NAME lqtexec_tests COMMAND ${LQTEXEC_TEST_LAUNCHER} $<TARGET_FILE:lqtexec_tests>)

# A program of its own: SignalStop must exist before any other thread.
if(UNIX)
    add_executable(lqtexec_signal_stop_test signal_stop/signal_stop_main.cpp)
    target_link_libraries(lqtexec_signal_stop_test PRIVATE lqtexec::lqtexec lqtexec_dev)
    add_test(NAME lqtexec_signal_stop COMMAND ${LQTEXEC_TEST_LAUNCHER} $<TARGET_FILE:lqtexec_signal_stop_test>)
    set_tests_properties(lqtexec_signal_stop PROPERTIES TIMEOUT 30)
endif()

# The consumer builds without this project's flags, so only plain builds can link it.
if(LQTEXEC_INSTALL AND NOT LQTEXEC_SANITIZERS AND NOT LQTEXEC_DISABLE_EXCEPTIONS)
    add_test(NAME lqtexec_installed_package
        COMMAND ${CMAKE_COMMAND}
            "-DLQTEXEC_BUILD_DIR=${PROJECT_BINARY_DIR}"
            "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/package"
            "-DCONSUMER_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/consumer"
            "-DQT_DIR_VARIABLE=${lqtexec_qt}_DIR"
            "-DQT_DIR=${${lqtexec_qt}_DIR}"
            "-DCXX_COMPILER=${CMAKE_CXX_COMPILER}"
            "-DGENERATOR=${CMAKE_GENERATOR}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/consumer/check_package.cmake")
    set_tests_properties(lqtexec_installed_package PROPERTIES TIMEOUT 300)
endif()

# Every public header compiles on its own, twice in a row, and after Qt's headers, whose
# keyword macros must not break what lqtexec includes.
file(GLOB_RECURSE lqtexec_public_headers CONFIGURE_DEPENDS
    RELATIVE "${PROJECT_SOURCE_DIR}/lqtexec/include"
    "${PROJECT_SOURCE_DIR}/lqtexec/include/lqtexec/*.h")
if(TARGET lqtexec::network)
    file(GLOB_RECURSE lqtexec_network_headers CONFIGURE_DEPENDS
        RELATIVE "${PROJECT_SOURCE_DIR}/network/include"
        "${PROJECT_SOURCE_DIR}/network/include/lqtexec/*.h")
    list(APPEND lqtexec_public_headers ${lqtexec_network_headers})
endif()
set(lqtexec_header_check_sources "")
foreach(header IN LISTS lqtexec_public_headers)
    string(MAKE_C_IDENTIFIER "${header}" header_id)
    set(source "${CMAKE_CURRENT_BINARY_DIR}/header_check/${header_id}.cpp")
    file(CONFIGURE OUTPUT "${source}" CONTENT "#include <${header}>\n#include <${header}>\n")
    set(after_qt "${CMAKE_CURRENT_BINARY_DIR}/header_check/${header_id}_after_qt.cpp")
    file(CONFIGURE OUTPUT "${after_qt}" CONTENT "#include <QtCore>\n#include <${header}>\n")
    list(APPEND lqtexec_header_check_sources "${source}" "${after_qt}")
endforeach()
add_library(lqtexec_header_check OBJECT ${lqtexec_header_check_sources})
target_link_libraries(lqtexec_header_check PRIVATE lqtexec::lqtexec lqtexec_dev)
if(TARGET lqtexec::network)
    target_link_libraries(lqtexec_header_check PRIVATE lqtexec::network)
endif()
