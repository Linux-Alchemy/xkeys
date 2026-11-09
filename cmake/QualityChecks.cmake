find_program(CLANG_FORMAT NAMES clang-format REQUIRED)
find_program(CLANG_TIDY NAMES clang-tidy REQUIRED)

# Refresh check inputs when files are added under the project source directories.
file(GLOB_RECURSE cpp_sources CONFIGURE_DEPENDS
    "${PROJECT_SOURCE_DIR}/src/*.cpp"
    "${PROJECT_SOURCE_DIR}/tests/*.cpp")
file(GLOB_RECURSE cpp_headers CONFIGURE_DEPENDS
    "${PROJECT_SOURCE_DIR}/src/*.h"
    "${PROJECT_SOURCE_DIR}/tests/*.h")
file(GLOB_RECURSE qml_sources CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/qml/*.qml")

if(NOT qml_sources)
    # Check the Qt Quick import and lint toolchain before the editor exists.
    set(qml_sources "${PROJECT_BINARY_DIR}/BuildSmoke.qml")
    file(GENERATE OUTPUT "${qml_sources}" CONTENT [=[import QtQuick

Item {
    width: 16
    height: 16
}
]=])
endif()

add_custom_target(format-check
    COMMAND "${CLANG_FORMAT}" --dry-run --Werror --style=file ${cpp_sources} ${cpp_headers}
    COMMAND "${CMAKE_COMMAND}" "-DQMLFORMAT=$<TARGET_FILE:Qt6::qmlformat>"
        "-DQML_SOURCES=${qml_sources}" -P "${CMAKE_CURRENT_LIST_DIR}/CheckQmlFormat.cmake"
    COMMENT "Checking C++ and QML formatting"
    VERBATIM)

add_custom_target(clang-tidy
    COMMAND "${CLANG_TIDY}" -p "${PROJECT_BINARY_DIR}" ${cpp_sources}
    COMMENT "Analyzing project C++ sources"
    VERBATIM)

add_custom_target(qml-check
    COMMAND Qt6::qmllint --ignore-settings --max-warnings 0 ${qml_sources}
    COMMENT "Linting QML sources"
    VERBATIM)
