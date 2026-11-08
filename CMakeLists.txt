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

find_package(Qt6 6.12 REQUIRED COMPONENTS Core Test QmlTools)

enable_testing()
add_executable(xkeys_build_smoke tests/build_smoke.cpp)
target_link_libraries(xkeys_build_smoke PRIVATE Qt6::Core Qt6::Test)
add_test(NAME build_smoke COMMAND xkeys_build_smoke)

include(cmake/QualityChecks.cmake)
