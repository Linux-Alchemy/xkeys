foreach(qml_file IN LISTS QML_SOURCES)
    execute_process(
        COMMAND "${QMLFORMAT}" --ignore-settings --newline unix "${qml_file}"
        OUTPUT_VARIABLE formatted
        COMMAND_ERROR_IS_FATAL ANY)
    string(SHA256 formatted_hash "${formatted}")
    file(SHA256 "${qml_file}" source_hash)
    if(NOT formatted_hash STREQUAL source_hash)
        message(SEND_ERROR "QML formatting differs in ${qml_file}. Run ${QMLFORMAT} --ignore-settings --newline unix --inplace \"${qml_file}\".")
    endif()
endforeach()
