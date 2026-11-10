cmake_minimum_required(VERSION 3.20)
project(SecureFileTransfer VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
option(SFT_STATIC_OPENSSL "Link OpenSSL statically for deployment" OFF)
if(SFT_STATIC_OPENSSL)
    set(OPENSSL_USE_STATIC_LIBS TRUE)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
find_package(OpenSSL 3.0 REQUIRED COMPONENTS SSL Crypto)
find_package(Threads REQUIRED)

add_executable(sft
    src/main.cpp
    src/server.cpp
    src/network.cpp
    src/security.cpp
    src/protocol.cpp
    src/storage.cpp)
target_link_libraries(sft PRIVATE OpenSSL::SSL OpenSSL::Crypto Threads::Threads)
if(WIN32)
    target_link_libraries(sft PRIVATE ws2_32 advapi32)
    target_compile_definitions(sft PRIVATE _WIN32_WINNT=0x0601 NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
if(MSVC)
    target_compile_options(sft PRIVATE /W4 /utf-8 /permissive-)
else()
    target_compile_options(sft PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
endif()
if(MINGW)
    target_link_options(sft PRIVATE -municode)
endif()
option(SFT_SANITIZE "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
if(SFT_SANITIZE AND NOT MSVC)
    target_compile_options(sft PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(sft PRIVATE -fsanitize=address,undefined)
endif()
include(CTest)
if(BUILD_TESTING)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    find_program(SFT_OPENSSL_EXECUTABLE NAMES openssl
        HINTS "${OPENSSL_ROOT_DIR}/bin" "C:/Program Files/Git/usr/bin" REQUIRED)
    add_test(NAME integration COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration.py $<TARGET_FILE:sft> ${SFT_OPENSSL_EXECUTABLE})
    set_tests_properties(integration PROPERTIES TIMEOUT 180
        ENVIRONMENT "ASAN_OPTIONS=halt_on_error=1;UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1")
endif()
install(TARGETS sft RUNTIME DESTINATION bin)
