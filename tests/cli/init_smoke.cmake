if(NOT DEFINED TALAS_CLI_EXE)
    message(FATAL_ERROR "TALAS_CLI_EXE is required")
endif()

if(NOT DEFINED TALAS_INIT_OUTPUT)
    message(FATAL_ERROR "TALAS_INIT_OUTPUT is required")
endif()

file(REMOVE "${TALAS_INIT_OUTPUT}")

execute_process(
    COMMAND "${TALAS_CLI_EXE}" init --template oscillator --output "${TALAS_INIT_OUTPUT}"
    RESULT_VARIABLE init_result
    OUTPUT_VARIABLE init_stdout
    ERROR_VARIABLE init_stderr
)

if(NOT init_result EQUAL 0)
    message(FATAL_ERROR "talas_cli init failed with ${init_result}\nstdout: ${init_stdout}\nstderr: ${init_stderr}")
endif()

if(NOT EXISTS "${TALAS_INIT_OUTPUT}")
    message(FATAL_ERROR "talas_cli init did not create ${TALAS_INIT_OUTPUT}")
endif()

file(READ "${TALAS_INIT_OUTPUT}" init_content)

if(NOT init_content MATCHES "\"title\": \"Oscillator Starter\"")
    message(FATAL_ERROR "talas_cli init output is missing the starter title")
endif()

if(NOT init_content MATCHES "\"typeId\": \"talas.oscillator\"")
    message(FATAL_ERROR "talas_cli init output is missing the oscillator node")
endif()

if(NOT init_content MATCHES "\"typeId\": \"talas.audioOutput\"")
    message(FATAL_ERROR "talas_cli init output is missing the audio output node")
endif()
