cmake_minimum_required(VERSION 3.25)
project(xkeys LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_AUTOMOC ON)

if(NOT CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang)$")
    message(FATAL_ERROR "xkeys supports GCC and Clang on Linux.")
endif()

add_compile_options(-Wall -Wextra -Wpedantic -Werror)

option(XKEYS_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
if(XKEYS_ENABLE_SANITIZERS)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer
                        -fno-sanitize-recover=undefined)
    add_link_options(-fsanitize=address,undefined)
endif()

find_package(Qt6 6.12 REQUIRED COMPONENTS Core Gui Network Qml Quick QuickControls2 Test)
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBUDEV REQUIRED IMPORTED_TARGET libudev)
pkg_check_modules(LIBEVDEV REQUIRED IMPORTED_TARGET libevdev)
pkg_check_modules(XKBCOMMON REQUIRED IMPORTED_TARGET xkbcommon)
qt_standard_project_setup(REQUIRES 6.12)

enable_testing()
add_executable(xkeys_build_smoke tests/build_smoke.cpp)
target_link_libraries(xkeys_build_smoke PRIVATE Qt6::Core Qt6::Test)
add_test(NAME build_smoke COMMAND xkeys_build_smoke)

add_library(xkeys_demo STATIC
    src/core/profile_store.cpp
    src/core/remapper.cpp
    src/ipc/endpoint.cpp
    src/ipc/protocol.cpp)
target_include_directories(xkeys_demo PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(xkeys_demo PUBLIC Qt6::Core Qt6::Network)

add_executable(xkeys_profile_test tests/core/profile_test.cpp)
target_link_libraries(xkeys_profile_test PRIVATE xkeys_demo Qt6::Test)
add_test(NAME profiles COMMAND xkeys_profile_test)
set_tests_properties(profiles PROPERTIES LABELS profiles TIMEOUT 30)

add_executable(xkeys_remapper_test tests/core/remapper_test.cpp)
target_link_libraries(xkeys_remapper_test PRIVATE xkeys_demo Qt6::Test)
add_test(NAME remapper COMMAND xkeys_remapper_test)
set_tests_properties(remapper PROPERTIES LABELS remapper TIMEOUT 30)

add_executable(xkeysd src/service/main.cpp src/service/session.cpp)
target_link_libraries(xkeysd PRIVATE xkeys_demo)

add_library(xkeys_recorder STATIC src/service/shortcut_recorder.cpp)
target_include_directories(xkeys_recorder PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(xkeys_recorder PUBLIC Qt6::Core)

add_library(xkeys_keyboard_inventory STATIC src/linux/keyboard_inventory.cpp)
target_link_libraries(xkeys_keyboard_inventory PUBLIC xkeys_recorder
    PRIVATE PkgConfig::LIBEVDEV PkgConfig::LIBUDEV)

add_library(xkeys_keyboard_capture STATIC src/linux/keyboard_capture.cpp)
target_link_libraries(xkeys_keyboard_capture PUBLIC xkeys_keyboard_inventory
    PRIVATE PkgConfig::LIBEVDEV PkgConfig::LIBUDEV)

add_executable(xkeys-record src/service/capture_main.cpp)
target_link_libraries(xkeys-record PRIVATE xkeys_keyboard_capture)

add_library(xkeys_record_ui STATIC
    src/ui/capture_backend.h src/ui/capture_backend.cpp
    src/ui/hyprland_context.h src/ui/hyprland_context.cpp)
target_link_libraries(xkeys_record_ui PUBLIC xkeys_keyboard_inventory
    PRIVATE Qt6::Gui PkgConfig::XKBCOMMON)

add_executable(xkeys_recorder_test tests/core/recorder_test.cpp)
target_link_libraries(xkeys_recorder_test PRIVATE xkeys_recorder Qt6::Test)
add_test(NAME recorder COMMAND xkeys_recorder_test)
set_tests_properties(recorder PROPERTIES LABELS recording TIMEOUT 30)

add_executable(xkeys_capture_test tests/integration/capture_test.cpp)
target_link_libraries(xkeys_capture_test PRIVATE xkeys_keyboard_capture Qt6::Test)
add_test(NAME capture COMMAND xkeys_capture_test)
set_tests_properties(capture PROPERTIES LABELS recording TIMEOUT 30)

add_executable(xkeys_capture_backend_test tests/service/capture_backend_test.cpp)
target_link_libraries(xkeys_capture_backend_test PRIVATE xkeys_record_ui Qt6::Test)
add_test(NAME capture_backend COMMAND xkeys_capture_backend_test)
set_tests_properties(capture_backend PROPERTIES LABELS recording TIMEOUT 30)

add_executable(xkeys_hyprland_context_test tests/core/hyprland_context_test.cpp)
target_link_libraries(xkeys_hyprland_context_test PRIVATE xkeys_record_ui Qt6::Test)
add_test(NAME hyprland_context COMMAND xkeys_hyprland_context_test)
set_tests_properties(hyprland_context PROPERTIES LABELS recording TIMEOUT 30)

set_tests_properties(capture_backend hyprland_context PROPERTIES
    ENVIRONMENT_MODIFICATION "__GLX_VENDOR_LIBRARY_NAME=unset:")

set(demo_qml_files qml/Main.qml qml/Keypad.qml qml/TestPopup.qml qml/BindingEditor.qml)
foreach(qml_file IN LISTS demo_qml_files)
    get_filename_component(qml_name "${qml_file}" NAME)
    set_source_files_properties("${qml_file}" PROPERTIES QT_RESOURCE_ALIAS "${qml_name}")
endforeach()

qt_add_qml_module(xkeys_ui STATIC
    URI XKeys
    OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/XKeys"
    SOURCES src/ui/controller.h src/ui/controller.cpp
    QML_FILES ${demo_qml_files})
target_include_directories(xkeys_ui PRIVATE "${PROJECT_SOURCE_DIR}/src/ui")
target_link_libraries(xkeys_ui PUBLIC xkeys_demo xkeys_record_ui Qt6::Quick Qt6::QuickControls2)

qt_add_executable(xkeys src/ui/main.cpp)
target_link_libraries(xkeys PRIVATE xkeys_ui xkeys_uiplugin)
add_dependencies(xkeys xkeysd xkeys-record)

add_executable(xkeys_service_demo_test tests/service/demo_test.cpp)
target_link_libraries(xkeys_service_demo_test PRIVATE xkeys_demo Qt6::Test)
add_dependencies(xkeys_service_demo_test xkeys)
add_test(NAME service_demo COMMAND xkeys_service_demo_test)
set_tests_properties(service_demo PROPERTIES LABELS demo TIMEOUT 30
    ENVIRONMENT "QT_FORCE_STDERR_LOGGING=1"
    ENVIRONMENT_MODIFICATION "__GLX_VENDOR_LIBRARY_NAME=unset:")

qt_add_executable(xkeys_ui_demo_test tests/ui/demo_test.cpp)
target_include_directories(xkeys_ui_demo_test PRIVATE "${PROJECT_SOURCE_DIR}")
target_link_libraries(xkeys_ui_demo_test PRIVATE xkeys_ui xkeys_uiplugin Qt6::Test)
add_dependencies(xkeys_ui_demo_test xkeysd)
add_test(NAME ui_demo COMMAND xkeys_ui_demo_test)
set_tests_properties(ui_demo PROPERTIES LABELS demo)

qt_add_executable(xkeys_ui_recording_test tests/ui/recording_test.cpp)
target_include_directories(xkeys_ui_recording_test PRIVATE "${PROJECT_SOURCE_DIR}")
target_link_libraries(xkeys_ui_recording_test PRIVATE xkeys_ui xkeys_uiplugin Qt6::Test)
add_dependencies(xkeys_ui_recording_test xkeysd)
add_test(NAME ui_recording COMMAND xkeys_ui_recording_test)
set_tests_properties(ui_recording PROPERTIES LABELS "recording;editor")

set_tests_properties(ui_demo ui_recording PROPERTIES TIMEOUT 30
    ENVIRONMENT
        "QT_QPA_PLATFORM=offscreen;QT_QPA_PLATFORMTHEME=;QT_QUICK_BACKEND=software;QT_FORCE_STDERR_LOGGING=1"
    ENVIRONMENT_MODIFICATION
        "__GLX_VENDOR_LIBRARY_NAME=unset:;QT_IM_MODULE=unset:;QT_IM_MODULES=unset:")

include(cmake/QualityChecks.cmake)
