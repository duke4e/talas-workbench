if(NOT DEFINED TALAS_SOURCE_DIR OR NOT DEFINED TALAS_EXPECTED_COMMIT)
    message(FATAL_ERROR "Benchmark provenance verifier is missing inputs")
endif()

execute_process(
    COMMAND git rev-parse --verify HEAD
    WORKING_DIRECTORY "${TALAS_SOURCE_DIR}"
    OUTPUT_VARIABLE TALAS_ACTUAL_COMMIT
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE TALAS_HEAD_RESULT)
if(NOT TALAS_HEAD_RESULT EQUAL 0 OR NOT TALAS_ACTUAL_COMMIT STREQUAL TALAS_EXPECTED_COMMIT)
    message(FATAL_ERROR
        "Benchmark source commit changed; rerun the configure preset")
endif()

execute_process(
    COMMAND git diff --quiet HEAD --
    WORKING_DIRECTORY "${TALAS_SOURCE_DIR}"
    RESULT_VARIABLE TALAS_DIRTY_RESULT)
if(NOT TALAS_DIRTY_RESULT EQUAL 0)
    message(FATAL_ERROR
        "Benchmark build requires clean tracked sources")
endif()
