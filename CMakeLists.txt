# SPDX-FileCopyrightText: 2026 Ben Jarvis
#
# SPDX-License-Identifier: Apache-2.0

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

target_link_libraries(oteltracing-c ${PROTOBUF_C_LIB} uuid pthread)

# The decode-verify test needs the generated pb-c headers/sources to unpack what
# the library produced.  Build it standalone (no collector / network required).
add_executable(oteltracing_test test/oteltracing_test.c ${OTEL_PB_SRCS})
target_include_directories(oteltracing_test
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(oteltracing_test oteltracing-c ${PROTOBUF_C_LIB})
add_test(NAME oteltracing_test COMMAND oteltracing_test)
