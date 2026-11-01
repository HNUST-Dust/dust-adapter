# SPDX-License-Identifier: Apache-2.0
# 本仓库原生（宿主）测试的统一入口：不依赖 Zephyr / west，可独立构建。
#
#   cmake -S tests -B build/native_tests -G Ninja
#   cmake --build build/native_tests -j
#   ctest --test-dir build/native_tests --output-on-failure
#
# 可选接线：
#   -DDUST_HPM_SUPPORT_DIR=<hpm_support 根>  外部板级支持 include 根（默认 ../hpm_support）
#   -DDUST_HOST_ROOT=<dust-control 根>       仅设置时编译/运行主机布局 crosscheck
cmake_minimum_required(VERSION 3.20)
project(dust_adapter_native_tests LANGUAGES C CXX)

enable_testing()

get_filename_component(DUST_ADAPTER_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
get_filename_component(DUST_WORKSPACE_ROOT "${DUST_ADAPTER_ROOT}/.." ABSOLUTE)
set(DUST_HPM_SUPPORT_DIR "${DUST_WORKSPACE_ROOT}/hpm_support" CACHE PATH
    "hpm_support checkout providing board/driver includes")
set(DUST_HOST_ROOT "" CACHE PATH
    "Optional dust-control checkout for the host layout crosscheck")

find_package(Python3 COMPONENTS Interpreter REQUIRED)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_EXTENSIONS OFF)

set(UNIT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/unit")
set(FAKES_DIR "${CMAKE_CURRENT_SOURCE_DIR}/fakes")
set(SRC_DIR "${DUST_ADAPTER_ROOT}/src")
set(TOOLS_DIR "${DUST_ADAPTER_ROOT}/tools")
set(GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(SSC_STAGE_DIR "${CMAKE_CURRENT_BINARY_DIR}/ssc")
file(MAKE_DIRECTORY "${GEN_DIR}")

# ---------------------------------------------------------------------------
# 生成物：CAN profile / 网关 ESI+SII+gateway_config.h / SSC 源码暂存
# ---------------------------------------------------------------------------
set(CAN_PROFILE_HEADER "${GEN_DIR}/can_profile_generated.hpp")
add_custom_command(
    OUTPUT "${CAN_PROFILE_HEADER}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${GEN_DIR}"
    COMMAND "${Python3_EXECUTABLE}" "${TOOLS_DIR}/generate_can_profile.py"
            --input "${DUST_ADAPTER_ROOT}/config/can_devices.yaml"
            --output "${CAN_PROFILE_HEADER}"
    DEPENDS "${TOOLS_DIR}/generate_can_profile.py"
            "${DUST_ADAPTER_ROOT}/config/can_devices.yaml"
    COMMENT "Generating can_profile_generated.hpp")
add_custom_target(dust_generated_can_profile DEPENDS "${CAN_PROFILE_HEADER}")

set(GATEWAY_CONFIG "${GEN_DIR}/gateway_config.h")
add_custom_command(
    OUTPUT "${GATEWAY_CONFIG}" "${GEN_DIR}/eeprom.h" "${GEN_DIR}/gateway.sii"
           "${GEN_DIR}/DUST_HPM5E31_Gateway.xml"
    COMMAND "${Python3_EXECUTABLE}" "${TOOLS_DIR}/generate_gateway_esi.py"
            --output "${GEN_DIR}"
    DEPENDS "${TOOLS_DIR}/generate_gateway_esi.py"
            "${DUST_ADAPTER_ROOT}/config/ethercat_gateway.json"
            "${DUST_ADAPTER_ROOT}/config/esi/DUST_HPM5E31_Gateway.xml"
    COMMENT "Generating gateway ESI/SII and gateway_config.h")
add_custom_target(dust_generated_gateway
    DEPENDS "${GATEWAY_CONFIG}" "${GEN_DIR}/eeprom.h" "${GEN_DIR}/gateway.sii"
            "${GEN_DIR}/DUST_HPM5E31_Gateway.xml")

# SSC 上游源码必须复制到暂存目录，才能用仓库自己的 sampleappl.h 覆盖示例头。
file(GLOB SSC_UPSTREAM_C CONFIGURE_DEPENDS "${DUST_ADAPTER_ROOT}/src/ecat/generated/*.c")
file(GLOB SSC_UPSTREAM_H CONFIGURE_DEPENDS "${DUST_ADAPTER_ROOT}/src/ecat/generated/*.h")
set(SSC_STAGED_C "${SSC_STAGE_DIR}/objdef.c" "${SSC_STAGE_DIR}/coeappl.c")
set(SSC_STAMP "${SSC_STAGE_DIR}/.stamp")
add_custom_command(
    OUTPUT "${SSC_STAMP}"
    COMMAND "${CMAKE_COMMAND}" -E rm -rf "${SSC_STAGE_DIR}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${SSC_STAGE_DIR}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different ${SSC_UPSTREAM_H} "${SSC_STAGE_DIR}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different ${SSC_UPSTREAM_C} "${SSC_STAGE_DIR}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${SRC_DIR}/ecat/sampleappl.h" "${SSC_STAGE_DIR}/sampleappl.h"
    COMMAND "${CMAKE_COMMAND}" -E touch "${SSC_STAMP}"
    DEPENDS ${SSC_UPSTREAM_C} ${SSC_UPSTREAM_H} "${SRC_DIR}/ecat/sampleappl.h"
    COMMENT "Staging external SSC sources with repository sampleappl.h")
add_custom_target(dust_ssc_stage DEPENDS "${SSC_STAMP}")
set_source_files_properties(${SSC_STAGED_C} PROPERTIES GENERATED TRUE)

# ---------------------------------------------------------------------------
# 公共编译/链接标志与注册辅助
# ---------------------------------------------------------------------------
set(DUST_WARNINGS -Wall -Wextra -Werror)
set(DUST_SANITIZER -fsanitize=address,undefined)

# 所有可执行目标统一带 sanitizer；GCC/Clang 链接时也必须重复该标志。
function(dust_test_flags target)
    target_compile_options(${target} PRIVATE ${DUST_WARNINGS} ${DUST_SANITIZER}
                           -fno-omit-frame-pointer)
    target_link_options(${target} PRIVATE ${DUST_SANITIZER})
endfunction()

# 测试统一在仓库根目录运行：python 契约依赖 ROOT 相对路径。
function(dust_add_test name)
    add_test(NAME ${name} COMMAND ${ARGN})
    set_tests_properties(${name} PROPERTIES
        WORKING_DIRECTORY "${DUST_ADAPTER_ROOT}")
endfunction()

# ---------------------------------------------------------------------------
# protocol：纯契约
# ---------------------------------------------------------------------------
add_executable(protocol_core_contracts
    "${SRC_DIR}/protocol/layout.cpp" "${SRC_DIR}/protocol/core.cpp"
    "${UNIT_DIR}/ethercat_core_contracts.cpp")
target_include_directories(protocol_core_contracts PRIVATE "${SRC_DIR}")
target_compile_options(protocol_core_contracts PRIVATE -fno-exceptions)
dust_test_flags(protocol_core_contracts)
dust_add_test(protocol_core_contracts protocol_core_contracts)

add_executable(protocol_mapping_contracts
    "${SRC_DIR}/protocol/pdo_dictionary.cpp"
    "${UNIT_DIR}/ethercat_mapping_contracts.cpp")
target_include_directories(protocol_mapping_contracts PRIVATE "${SRC_DIR}")
target_compile_options(protocol_mapping_contracts PRIVATE -fno-exceptions)
dust_test_flags(protocol_mapping_contracts)
dust_add_test(protocol_mapping_contracts protocol_mapping_contracts)

add_executable(protocol_compact_layout
    "${SRC_DIR}/protocol/layout.cpp" "${UNIT_DIR}/ethercat_compact_layout.cpp")
target_include_directories(protocol_compact_layout PRIVATE "${SRC_DIR}")
target_compile_options(protocol_compact_layout PRIVATE -fno-exceptions)
dust_test_flags(protocol_compact_layout)
dust_add_test(protocol_compact_layout protocol_compact_layout)

# ---------------------------------------------------------------------------
# software optimizations：digest 与 reference/optimized 字节等价
# ---------------------------------------------------------------------------
add_executable(digest_contracts "${UNIT_DIR}/digest_contracts.cpp")
target_include_directories(digest_contracts PRIVATE "${SRC_DIR}")
dust_test_flags(digest_contracts)
dust_add_test(digest_contracts digest_contracts)

set(PUBLISH_REFERENCE_BIN "${CMAKE_CURRENT_BINARY_DIR}/publish_reference.bin")
set(PUBLISH_OPTIMIZED_BIN "${CMAKE_CURRENT_BINARY_DIR}/publish_optimized.bin")

foreach(mode reference optimized)
    if(mode STREQUAL "reference")
        set(mode_define DUST_TEST_REFERENCE_DIGEST=1)
    else()
        set(mode_define CONFIG_DUST_ECAT_PUBLISH_SKIP_OVERWRITE_CLEAR=1)
    endif()
    add_executable(publish_${mode}
        "${SRC_DIR}/protocol/core.cpp" "${SRC_DIR}/protocol/layout.cpp"
        "${UNIT_DIR}/publish_equivalence.cpp")
    target_include_directories(publish_${mode} PRIVATE "${SRC_DIR}")
    target_compile_definitions(publish_${mode} PRIVATE ${mode_define})
    target_compile_options(publish_${mode} PRIVATE -fno-exceptions)
    dust_test_flags(publish_${mode})

    add_executable(core_${mode}
        "${SRC_DIR}/protocol/core.cpp" "${SRC_DIR}/protocol/layout.cpp"
        "${UNIT_DIR}/ethercat_core_contracts.cpp")
    target_include_directories(core_${mode} PRIVATE "${SRC_DIR}")
    target_compile_definitions(core_${mode} PRIVATE ${mode_define})
    target_compile_options(core_${mode} PRIVATE -fno-exceptions)
    dust_test_flags(core_${mode})
    dust_add_test(core_contracts_${mode} core_${mode})
endforeach()

dust_add_test(publish_equivalence_reference
    "${CMAKE_COMMAND}"
    "-DEXE=$<TARGET_FILE:publish_reference>"
    "-DOUT=${PUBLISH_REFERENCE_BIN}"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/capture_output.cmake")
dust_add_test(publish_equivalence_optimized
    "${CMAKE_COMMAND}"
    "-DEXE=$<TARGET_FILE:publish_optimized>"
    "-DOUT=${PUBLISH_OPTIMIZED_BIN}"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/capture_output.cmake")
dust_add_test(publish_equivalence_compare
    "${CMAKE_COMMAND}" -E compare_files "${PUBLISH_REFERENCE_BIN}" "${PUBLISH_OPTIMIZED_BIN}")
set_tests_properties(publish_equivalence_compare PROPERTIES
    DEPENDS "publish_equivalence_reference;publish_equivalence_optimized")

# ---------------------------------------------------------------------------
# latest_can / buzzer
# ---------------------------------------------------------------------------
add_executable(latest_can_contracts "${UNIT_DIR}/latest_can_contracts.cpp")
target_include_directories(latest_can_contracts PRIVATE "${SRC_DIR}")
dust_test_flags(latest_can_contracts)
dust_add_test(latest_can_contracts latest_can_contracts)

add_executable(buzzer_contracts "${FAKES_DIR}/buzzer_native/contracts.c")
target_include_directories(buzzer_contracts PRIVATE "${FAKES_DIR}/buzzer_native")
target_compile_options(buzzer_contracts PRIVATE -Wall -Wextra -Werror ${DUST_SANITIZER})
target_link_options(buzzer_contracts PRIVATE ${DUST_SANITIZER})
dust_add_test(buzzer_contracts buzzer_contracts)

# ---------------------------------------------------------------------------
# IO 后端 / 启动计数
# ---------------------------------------------------------------------------
add_executable(io_contracts
    "${UNIT_DIR}/ethercat_io_contracts.cpp" "${SRC_DIR}/ecat/io_backend.cpp"
    "${SRC_DIR}/ecat/can_io.cpp" "${SRC_DIR}/ecat/uart_io.cpp"
    "${SRC_DIR}/drivers/uart_receive.cpp" "${SRC_DIR}/protocol/core.cpp"
    "${SRC_DIR}/protocol/layout.cpp")
target_include_directories(io_contracts PRIVATE
    "${FAKES_DIR}/io_native" "${DUST_HPM_SUPPORT_DIR}/include" "${SRC_DIR}"
    "${GEN_DIR}")
target_compile_definitions(io_contracts PRIVATE CONFIG_UART_HPMICRO=1)
dust_test_flags(io_contracts)
# 必须在 -Wextra 之后，否则会被重新启用。
target_compile_options(io_contracts PRIVATE -fno-exceptions
    -Wno-missing-field-initializers)
add_dependencies(io_contracts dust_generated_can_profile)
dust_add_test(io_contracts io_contracts)

add_executable(boot_contracts
    "${UNIT_DIR}/ethercat_boot_contracts.cpp" "${SRC_DIR}/ecat/boot_identity.cpp"
    "${SRC_DIR}/protocol/layout.cpp")
target_include_directories(boot_contracts PRIVATE
    "${FAKES_DIR}/io_native" "${SRC_DIR}")
target_compile_options(boot_contracts PRIVATE -fno-exceptions)
dust_test_flags(boot_contracts)
foreach(scenario success zero_uid mount_error read_error corrupt
        write_short write_error verify_error)
    dust_add_test(boot_contracts_${scenario} boot_contracts ${scenario})
endforeach()

# ---------------------------------------------------------------------------
# SSC：真实上游 objdef.c/coeappl.c + 原生 fake ESC/时钟边界
# ---------------------------------------------------------------------------
add_library(ssc_upstream OBJECT ${SSC_STAGED_C})
add_dependencies(ssc_upstream dust_ssc_stage dust_generated_gateway)
target_include_directories(ssc_upstream PRIVATE
    "${SSC_STAGE_DIR}" "${GEN_DIR}" "${FAKES_DIR}/ssc_native" "${SRC_DIR}")
target_compile_options(ssc_upstream PRIVATE
    -ffunction-sections -fdata-sections ${DUST_SANITIZER} -fno-omit-frame-pointer
    "-include${FAKES_DIR}/ssc_native/platform.h"
    "-include${GATEWAY_CONFIG}")

if(APPLE)
    set(SSC_GC_FLAGS -Wl,-dead_strip)
else()
    set(SSC_GC_FLAGS -Wl,--gc-sections)
endif()

foreach(mode baseline late_refresh)
    set(mode_defines)
    if(mode STREQUAL "late_refresh")
        list(APPEND mode_defines CONFIG_DUST_ECAT_INPUT_LATE_REFRESH=1)
    endif()
    add_executable(ssc_contracts_${mode}
        "${SRC_DIR}/ecat/gateway.cpp" "${SRC_DIR}/ecat/gateway_objects.cpp"
        "${SRC_DIR}/ecat/gateway_ssc.cpp" "${SRC_DIR}/protocol/layout.cpp"
        "${SRC_DIR}/protocol/core.cpp" "${SRC_DIR}/protocol/pdo_dictionary.cpp"
        "${FAKES_DIR}/ssc_native/io_stub.cpp"
        "${UNIT_DIR}/ethercat_ssc_contracts.cpp"
        $<TARGET_OBJECTS:ssc_upstream>)
    add_dependencies(ssc_contracts_${mode} dust_ssc_stage dust_generated_gateway)
    target_include_directories(ssc_contracts_${mode} PRIVATE
        "${SSC_STAGE_DIR}" "${GEN_DIR}" "${FAKES_DIR}/ssc_native" "${SRC_DIR}")
    target_compile_definitions(ssc_contracts_${mode} PRIVATE ${mode_defines})
    target_compile_options(ssc_contracts_${mode} PRIVATE
        -fno-exceptions -ffunction-sections -fdata-sections
        ${DUST_SANITIZER} -fno-omit-frame-pointer
        "-include${FAKES_DIR}/ssc_native/platform.h"
        "-include${GATEWAY_CONFIG}")
    target_link_options(ssc_contracts_${mode} PRIVATE ${DUST_SANITIZER} ${SSC_GC_FLAGS})
    dust_add_test(ssc_contracts_${mode} ssc_contracts_${mode})
endforeach()

# ---------------------------------------------------------------------------
# python 契约 / CAN profile / heater
# ---------------------------------------------------------------------------
dust_add_test(python_can_profile_generator
    "${Python3_EXECUTABLE}" "${UNIT_DIR}/test_can_profile_generator.py")
dust_add_test(python_phy_link_contracts
    "${Python3_EXECUTABLE}" "${UNIT_DIR}/phy_link_contracts.py")
dust_add_test(python_ethercat_esi_contracts
    "${Python3_EXECUTABLE}" "${UNIT_DIR}/ethercat_esi_contracts.py")

add_executable(can_profile_contracts "${UNIT_DIR}/can_profile_contracts.cpp")
target_include_directories(can_profile_contracts PRIVATE "${SRC_DIR}" "${GEN_DIR}")
target_compile_options(can_profile_contracts PRIVATE -fno-exceptions)
dust_test_flags(can_profile_contracts)
add_dependencies(can_profile_contracts dust_generated_can_profile)
dust_add_test(can_profile_contracts can_profile_contracts)

add_executable(heater_contracts "${UNIT_DIR}/heater_contracts.cpp")
target_include_directories(heater_contracts PRIVATE "${SRC_DIR}")
target_compile_options(heater_contracts PRIVATE -fno-exceptions)
dust_test_flags(heater_contracts)
dust_add_test(heater_contracts heater_contracts)

# ---------------------------------------------------------------------------
# 可选主机 crosscheck：未设置 DUST_HOST_ROOT 时跳过（不视为失败）。
# ---------------------------------------------------------------------------
if(DUST_HOST_ROOT)
    if(NOT EXISTS "${DUST_HOST_ROOT}/src/hardware/ethercat/pdo_config.cpp")
        message(FATAL_ERROR
            "DUST_HOST_ROOT=${DUST_HOST_ROOT} 缺少 src/hardware/ethercat/pdo_config.cpp")
    endif()
    add_executable(host_layout_crosscheck
        "${SRC_DIR}/protocol/layout.cpp" "${SRC_DIR}/protocol/pdo_dictionary.cpp"
        "${DUST_HOST_ROOT}/src/hardware/ethercat/pdo_config.cpp"
        "${UNIT_DIR}/ethercat_layout_crosscheck.cpp")
    target_include_directories(host_layout_crosscheck PRIVATE
        "${SRC_DIR}" "${DUST_HOST_ROOT}/src")
    target_compile_options(host_layout_crosscheck PRIVATE
        -std=c++20 -Wall -Wextra -Werror ${DUST_SANITIZER} -fno-omit-frame-pointer)
    target_link_options(host_layout_crosscheck PRIVATE ${DUST_SANITIZER})
    target_link_libraries(host_layout_crosscheck PRIVATE yaml-cpp)
    dust_add_test(host_layout_crosscheck host_layout_crosscheck)
else()
    message(STATUS
        "DUST_HOST_ROOT 未设置：跳过 host layout crosscheck（设置后启用）。")
endif()
