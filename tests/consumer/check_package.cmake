# Installs lqtexec, moves the installation elsewhere, then builds and runs a consumer
# that finds it with find_package. Run with cmake -P and:
#   LQTEXEC_BUILD_DIR  the configured and built lqtexec tree
#   WORK_DIR           scratch directory, emptied first
#   CONSUMER_SOURCE    this directory
#   QT_DIR_VARIABLE    Qt5_DIR or Qt6_DIR, and QT_DIR its value
#   CXX_COMPILER, GENERATOR

function(run)
    execute_process(COMMAND ${ARGN} RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE output)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "Failed (${result}): ${ARGN}\n${output}")
    endif()
endfunction()

file(REMOVE_RECURSE "${WORK_DIR}")
set(staging "${WORK_DIR}/staging")
set(prefix "${WORK_DIR}/moved")
run("${CMAKE_COMMAND}" --install "${LQTEXEC_BUILD_DIR}" --prefix "${staging}")
file(RENAME "${staging}" "${prefix}")
run("${CMAKE_COMMAND}" -S "${CONSUMER_SOURCE}" -B "${WORK_DIR}/build" -G "${GENERATOR}"
    "-DCMAKE_PREFIX_PATH=${prefix}" "-D${QT_DIR_VARIABLE}=${QT_DIR}" "-DCMAKE_CXX_COMPILER=${CXX_COMPILER}")
run("${CMAKE_COMMAND}" --build "${WORK_DIR}/build")
run("${WORK_DIR}/build/consumer")
