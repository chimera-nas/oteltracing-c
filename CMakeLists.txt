# SPDX-FileCopyrightText: 2026 Ben Jarvis
#
# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.18)
project(oteltracing-c LANGUAGES C CXX)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
find_package(Threads REQUIRED)
if(WIN32)
    # Keep DLLs and test/CLI executables together for the Windows loader.
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/bin")
endif()
function(otel_compile_options target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W3 /WX)
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_DEPRECATE)
    else()
        target_compile_options(${target} PRIVATE -Wall -Werror -fvisibility=hidden)
    endif()
endfunction()

enable_testing()

# Unix packages provide protoc-c; vcpkg provides protoc plus protoc-gen-c.
find_program(PROTOC_C protoc-c)
if(PROTOC_C)
    set(OTEL_PROTOC_COMMAND "${PROTOC_C}")
else()
    find_program(PROTOC_EXECUTABLE protoc
        HINTS "${VCPKG_INSTALLED_DIR}/${VCPKG_HOST_TRIPLET}/tools/protobuf")
    find_program(PROTOC_GEN_C protoc-gen-c
        HINTS "${VCPKG_INSTALLED_DIR}/${VCPKG_HOST_TRIPLET}/tools/protobuf-c")
    if(NOT PROTOC_EXECUTABLE OR NOT PROTOC_GEN_C)
        message(FATAL_ERROR "Install protoc-c, or protoc and protoc-gen-c")
    endif()
    set(OTEL_PROTOC_COMMAND "${PROTOC_EXECUTABLE}" "--plugin=protoc-gen-c=${PROTOC_GEN_C}")
endif()

find_package(protobuf-c CONFIG QUIET)
if(TARGET protobuf-c::protobuf-c)
    set(PROTOBUF_C_LIB protobuf-c::protobuf-c)
    get_target_property(OTEL_PROTOBUF_C_TYPE protobuf-c::protobuf-c TYPE)
    if(WIN32 AND OTEL_PROTOBUF_C_TYPE STREQUAL "SHARED_LIBRARY")
        # protobuf-c's exported target omits this usage requirement. Its data
        # symbols (notably protobuf_c_empty_string) need dllimport in consumers.
        set_property(TARGET protobuf-c::protobuf-c APPEND PROPERTY
            INTERFACE_COMPILE_DEFINITIONS PROTOBUF_C_USE_SHARED_LIB)
    endif()
else()
    find_library(PROTOBUF_C_LIBRARY NAMES protobuf-c REQUIRED)
    find_path(PROTOBUF_C_INCLUDE protobuf-c/protobuf-c.h REQUIRED)
    add_library(otel_protobuf_c UNKNOWN IMPORTED)
    set_target_properties(otel_protobuf_c PROPERTIES
        IMPORTED_LOCATION "${PROTOBUF_C_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${PROTOBUF_C_INCLUDE}")
    set(PROTOBUF_C_LIB otel_protobuf_c)
endif()

# Generate the pb-c sources for the OTLP trace subset.  Paths are kept relative
# to the proto root so the protobuf import statements resolve.
set(OTEL_PROTO_ROOT ${CMAKE_CURRENT_SOURCE_DIR})
set(OTEL_PROTOS
    opentelemetry/proto/common/v1/common
    opentelemetry/proto/resource/v1/resource
    opentelemetry/proto/trace/v1/trace
    opentelemetry/proto/collector/trace/v1/trace_service)

set(OTEL_PB_SRCS "")
foreach(proto ${OTEL_PROTOS})
    set(pb_c ${CMAKE_CURRENT_BINARY_DIR}/${proto}.pb-c.c)
    set(pb_h ${CMAKE_CURRENT_BINARY_DIR}/${proto}.pb-c.h)
    add_custom_command(
        OUTPUT ${pb_c} ${pb_h}
        COMMAND ${OTEL_PROTOC_COMMAND}
                --c_out=${CMAKE_CURRENT_BINARY_DIR}
                --proto_path=${OTEL_PROTO_ROOT}
                ${OTEL_PROTO_ROOT}/${proto}.proto
        DEPENDS ${OTEL_PROTO_ROOT}/${proto}.proto
        COMMENT "protoc-c ${proto}.proto"
        VERBATIM)
    list(APPEND OTEL_PB_SRCS ${pb_c})
endforeach()

add_library(oteltracing-c SHARED
    oteltracing.c
    ${OTEL_PB_SRCS})

otel_compile_options(oteltracing-c)
target_compile_definitions(oteltracing-c PRIVATE OTEL_BUILD)
if(MSVC)
    # MSVC implements the C11 release/acquire atomics used by the SPSC rings.
    target_compile_options(oteltracing-c PRIVATE /experimental:c11atomics)
endif()

# Generated headers land under the binary dir; the public header alongside
# source.  The stopwatch submodule is header-only and used only internally, so
# its include path is PRIVATE.
target_include_directories(oteltracing-c
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}
    PRIVATE ${CMAKE_CURRENT_BINARY_DIR}
            ${CMAKE_CURRENT_SOURCE_DIR}/ext/stopwatch)

# uuid_generate(3) is in libSystem on Darwin: <uuid/uuid.h> resolves, but there
# is no separate libuuid, so asking to link one fails.  Everywhere else it comes
# from util-linux and does have to be named.
if(WIN32)
    set(UUID_LIBS bcrypt)
elseif(APPLE)
    set(UUID_LIBS "")
else()
    set(UUID_LIBS uuid)
endif()

target_link_libraries(oteltracing-c ${PROTOBUF_C_LIB} ${UUID_LIBS} Threads::Threads)

# The decode-verify test needs the generated pb-c headers/sources to unpack what
# the library produced.  Build it standalone (no collector / network required).
add_executable(oteltracing_test test/oteltracing_test.c ${OTEL_PB_SRCS})
target_include_directories(oteltracing_test
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(oteltracing_test oteltracing-c ${PROTOBUF_C_LIB})
add_test(NAME oteltracing_test COMMAND oteltracing_test)

# Compile-time-disabled build (OTEL_TRACING=0 set in the source).  Deliberately
# NOT linked against oteltracing-c: it must build and run with the header alone,
# proving the API strips to no library dependency.
add_executable(otel_disabled_test test/otel_disabled_test.c)
target_include_directories(otel_disabled_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME otel_disabled_test COMMAND otel_disabled_test)

# ---------------------------------------------------------------------------
# Optional SQLite span sink + otel-trace CLI.  Enabled when libsqlite3 is found;
# the core library above never depends on SQLite.
# ---------------------------------------------------------------------------
option(OTEL_SQLITE "Build the optional SQLite span sink and otel-trace CLI" ON)

if(OTEL_SQLITE)
    find_package(SQLite3 QUIET)
endif()

if(OTEL_SQLITE AND SQLite3_FOUND)
    if(TARGET SQLite3::SQLite3)
        set(SQLITE3_LIB SQLite3::SQLite3)
    else()
        set(SQLITE3_LIB SQLite::SQLite3)
    endif()
    set(SQLITE3_INCLUDE ${SQLite3_INCLUDE_DIRS})
    # The sink: registers a span sink with the core tracer and persists spans to
    # SQLite from a dedicated flusher thread.  Depends on core + sqlite3.
    add_library(oteltracing-sqlite SHARED otel_sqlite.c)
    otel_compile_options(oteltracing-sqlite)
    target_compile_definitions(oteltracing-sqlite PRIVATE OTEL_SQLITE_BUILD)
    target_include_directories(oteltracing-sqlite
        PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}
        PRIVATE ${SQLITE3_INCLUDE})
    target_link_libraries(oteltracing-sqlite oteltracing-c ${SQLITE3_LIB} Threads::Threads)

    # The CLI: read-only query tool.  Only needs sqlite3 + the shared schema.
    add_executable(otel-trace tools/otel-trace.c)
    otel_compile_options(otel-trace)
    target_include_directories(otel-trace
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${SQLITE3_INCLUDE})
    target_link_libraries(otel-trace ${SQLITE3_LIB})

    # Round-trip test: emit spans through the sink, then read the DB back.
    add_executable(otel_sqlite_test test/otel_sqlite_test.c)
    otel_compile_options(otel_sqlite_test)
    target_include_directories(otel_sqlite_test
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${SQLITE3_INCLUDE})
    target_link_libraries(otel_sqlite_test
        oteltracing-c oteltracing-sqlite ${SQLITE3_LIB})
    add_test(NAME otel_sqlite_test COMMAND otel_sqlite_test)
    add_test(NAME otel_trace_help COMMAND otel-trace --help)
    install(TARGETS oteltracing-sqlite otel-trace
        RUNTIME DESTINATION bin LIBRARY DESTINATION lib ARCHIVE DESTINATION lib)
    install(FILES otel_sqlite.h DESTINATION include)
elseif(OTEL_SQLITE)
    message(STATUS "oteltracing-c: libsqlite3 not found; "
                   "skipping SQLite sink and otel-trace CLI")
endif()
install(TARGETS oteltracing-c
    RUNTIME DESTINATION bin LIBRARY DESTINATION lib ARCHIVE DESTINATION lib)
install(FILES oteltracing.h DESTINATION include)

add_executable(otel_concurrent_test test/otel_concurrent_test.c)
target_link_libraries(otel_concurrent_test PRIVATE oteltracing-c Threads::Threads ${UUID_LIBS})
otel_compile_options(otel_concurrent_test)
if(MSVC)
    target_compile_options(otel_concurrent_test PRIVATE /experimental:c11atomics /UNDEBUG)
else()
    target_compile_options(otel_concurrent_test PRIVATE -UNDEBUG)
endif()
add_test(NAME otel_concurrent_test COMMAND otel_concurrent_test)
