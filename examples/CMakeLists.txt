add_executable(lqtexec_console_demo console_demo.cpp)
target_link_libraries(lqtexec_console_demo PRIVATE lqtexec::lqtexec lqtexec_dev)

find_package(${lqtexec_qt} QUIET COMPONENTS Widgets)
if(TARGET ${lqtexec_qt}::Widgets)
    add_executable(lqtexec_widgets_demo
        widgets_demo/main.cpp
        widgets_demo/Journal.cpp
        widgets_demo/JobWindow.cpp
        widgets_demo/MainWindow.cpp
        widgets_demo/Script.cpp)
    target_link_libraries(lqtexec_widgets_demo PRIVATE lqtexec::lqtexec ${lqtexec_qt}::Widgets lqtexec_dev)
    if(LQTEXEC_BUILD_TESTS)
        add_test(NAME lqtexec_widgets_demo COMMAND ${LQTEXEC_TEST_LAUNCHER} $<TARGET_FILE:lqtexec_widgets_demo> --script)
        # Clang 18's LeakSanitizer hangs scanning the main thread at the exit of this Qt 5 GUI
        # program; the library's own leaks are checked by lqtexec_tests.
        set_tests_properties(lqtexec_widgets_demo PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen;ASAN_OPTIONS=detect_leaks=0"
            TIMEOUT 60)
    endif()
else()
    message(STATUS "lqtexec: ${lqtexec_qt} Widgets not found; skipping the Widgets demo")
endif()
