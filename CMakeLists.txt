# SPDX-FileCopyrightText: 2026 Ben Jarvis
#
# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.16)
project(oteltracing-c LANGUAGES C)

enable_testing()

# protoc-c is required to generate the OTLP protobuf-c sources.
find_program(PROTOC_C protoc-c)
if(NOT PROTOC_C)
    message(FATAL_ERROR "protoc-c not found (install protobuf-c-compiler)")
endif()

find_library(PROTOBUF_C_LIB protobuf-c)
if(NOT PROTOBUF_C_LIB)
    message(FATAL_ERROR "libprotobuf-c not found (install libprotobuf-c-dev)")
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
        COMMAND ${PROTOC_C}
                --c_out=${CMAKE_CURRENT_BINARY_DIR}
                --proto_path=${OTEL_PROTO_ROOT}
                ${OTEL_PROTO_ROOT}/${proto}.proto
        DEPENDS ${OTEL_PROTO_ROOT}/${proto}.proto
        COMMENT "protoc-c ${proto}.proto")
    list(APPEND OTEL_PB_SRCS ${pb_c})
endforeach()

add_library(oteltracing-c SHARED
    oteltracing.c
    ${OTEL_PB_SRCS})

target_compile_options(oteltracing-c PRIVATE -O3 -Wall -Werror -fvisibility=hidden)

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
if(APPLE)
    set(UUID_LIBS "")
else()
    set(UUID_LIBS uuid)
endif()

target_link_libraries(oteltracing-c ${PROTOBUF_C_LIB} ${UUID_LIBS} pthread)

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

find_library(SQLITE3_LIB sqlite3)
find_path(SQLITE3_INCLUDE sqlite3.h)

if(OTEL_SQLITE AND SQLITE3_LIB AND SQLITE3_INCLUDE)
    # The sink: registers a span sink with the core tracer and persists spans to
    # SQLite from a dedicated flusher thread.  Depends on core + sqlite3.
    add_library(oteltracing-sqlite SHARED otel_sqlite.c)
    target_compile_options(oteltracing-sqlite PRIVATE -O3 -Wall -Werror -fvisibility=hidden)
    target_include_directories(oteltracing-sqlite
        PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}
        PRIVATE ${SQLITE3_INCLUDE})
    target_link_libraries(oteltracing-sqlite oteltracing-c ${SQLITE3_LIB} pthread)

    # The CLI: read-only query tool.  Only needs sqlite3 + the shared schema.
    add_executable(otel-trace tools/otel-trace.c)
    target_compile_options(otel-trace PRIVATE -O2 -Wall -Werror)
    target_include_directories(otel-trace
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${SQLITE3_INCLUDE})
    target_link_libraries(otel-trace ${SQLITE3_LIB})

    # Round-trip test: emit spans through the sink, then read the DB back.
    add_executable(otel_sqlite_test test/otel_sqlite_test.c)
    target_compile_options(otel_sqlite_test PRIVATE -O2 -Wall -Werror)
    target_include_directories(otel_sqlite_test
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${SQLITE3_INCLUDE})
    target_link_libraries(otel_sqlite_test
        oteltracing-c oteltracing-sqlite ${SQLITE3_LIB})
    add_test(NAME otel_sqlite_test COMMAND otel_sqlite_test)
elseif(OTEL_SQLITE)
    message(STATUS "oteltracing-c: libsqlite3 not found; "
                   "skipping SQLite sink and otel-trace CLI")
endif()
