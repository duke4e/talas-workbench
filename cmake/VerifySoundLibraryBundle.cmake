if(NOT DEFINED TALAS_SOUND_LIBRARY_ROOT
   OR NOT IS_DIRECTORY "${TALAS_SOUND_LIBRARY_ROOT}")
    message(FATAL_ERROR "Sound Library bundle root is missing")
endif()
if(IS_SYMLINK "${TALAS_SOUND_LIBRARY_ROOT}")
    message(FATAL_ERROR "Sound Library bundle root cannot be a symlink")
endif()

file(GLOB_RECURSE bundle_entries
    LIST_DIRECTORIES true
    RELATIVE "${TALAS_SOUND_LIBRARY_ROOT}"
    "${TALAS_SOUND_LIBRARY_ROOT}/*")
set(actual_files)
foreach(entry IN LISTS bundle_entries)
    set(path "${TALAS_SOUND_LIBRARY_ROOT}/${entry}")
    if(IS_SYMLINK "${path}")
        message(FATAL_ERROR
            "Sound Library bundle contains a symlink: ${entry}")
    endif()
    if(NOT IS_DIRECTORY "${path}")
        list(APPEND actual_files "${entry}")
    endif()
endforeach()
list(SORT actual_files)
set(expected_files ${TALAS_SOUND_LIBRARY_ALLOWED})
list(SORT expected_files)
if(NOT actual_files STREQUAL expected_files)
    message(FATAL_ERROR
        "Sound Library bundle differs from its exact allowlist; "
        "expected=${expected_files}; actual=${actual_files}")
endif()

if(DEFINED TALAS_SOUND_LIBRARY_REFERENCE_ROOT)
    if(NOT IS_DIRECTORY "${TALAS_SOUND_LIBRARY_REFERENCE_ROOT}"
       OR IS_SYMLINK "${TALAS_SOUND_LIBRARY_REFERENCE_ROOT}")
        message(FATAL_ERROR "Sound Library reference root is invalid")
    endif()
    foreach(entry IN LISTS expected_files)
        set(actual "${TALAS_SOUND_LIBRARY_ROOT}/${entry}")
        set(reference "${TALAS_SOUND_LIBRARY_REFERENCE_ROOT}/${entry}")
        if(NOT EXISTS "${reference}" OR IS_SYMLINK "${reference}")
            message(FATAL_ERROR
                "Sound Library reference is missing or symlinked: ${entry}")
        endif()
        file(SHA256 "${actual}" actual_hash)
        file(SHA256 "${reference}" reference_hash)
        if(NOT actual_hash STREQUAL reference_hash)
            message(FATAL_ERROR
                "Sound Library bytes differ from the accepted reference: ${entry}")
        endif()
    endforeach()
endif()
