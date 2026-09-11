# SPDX-License-Identifier: MIT
# Copyright (c) Huawei Technologies Co., Ltd. 2021-2025. All rights reserved.

add_executable(urma_perftest
    urma_perftest.c
    perftest_log.c
    perftest_mgmt.c
    perftest_mgmt_tcp.c
    perftest_mgmt_ub.c
    perftest_parameters.c
    perftest_resources.c
    perftest_run_test.c
    perftest_metax.c
)

set_target_properties(urma_perftest
    PROPERTIES
        C_STANDARD 99
)

target_include_directories(urma_perftest
    PRIVATE
        ${CMAKE_SOURCE_DIR}/urma/lib/urma/core/include
        ${CMAKE_SOURCE_DIR}/urma/lib/urma/bond/include
        ${CMAKE_SOURCE_DIR}/urma/common/include
)

target_link_libraries(urma_perftest
    PRIVATE
        urma pthread m
)

if(NOT DEFINED MACA_PATH AND DEFINED ENV{MACA_PATH})
    set(MACA_PATH $ENV{MACA_PATH})
endif()
if(NOT MACA_PATH)
    set(MACA_PATH /opt/maca)
endif()

if(EXISTS "${MACA_PATH}/include/mc_runtime.h")
    message(STATUS "MetaX MACA found: ${MACA_PATH}")
    target_compile_definitions(urma_perftest PRIVATE HAVE_METAX_MACA=1)
    target_include_directories(urma_perftest PRIVATE ${MACA_PATH}/include)
    find_library(METAX_RUNTIME_LIB
        NAMES mcruntime maca macart
        PATHS ${MACA_PATH}/lib ${MACA_PATH}/lib64
        NO_DEFAULT_PATH)
    if(METAX_RUNTIME_LIB)
        target_link_libraries(urma_perftest PRIVATE ${METAX_RUNTIME_LIB})
    else()
        target_link_libraries(urma_perftest PRIVATE mcruntime)
    endif()
else()
    message(STATUS "MetaX MACA not found under ${MACA_PATH}; --use_metax will fail at runtime")
endif()

install(TARGETS urma_perftest
    DESTINATION /usr/bin
)
