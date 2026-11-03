# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

# ==============================================================================
# 1. Project Initialization
# ==============================================================================

include(${CMAKE_CURRENT_LIST_DIR}/cmake/prepare.cmake)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(dust_ethercat_slave)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ==============================================================================
# 2. Paths and Dependencies
# ==============================================================================

set(ADAPTER_SSC_DIR
    "${ADAPTER_ROOT}/src/ssc_generated"
    CACHE PATH "Licensed SSC 5.13 generated source directory"
)

set(HPM_PHY_DIR
    "${HPM_SUPPORT_DIR}/samples/ecat/Middleware/ESC"
)
set(HPM_PHY_BOARD_DIR
    "${HPM_SUPPORT_DIR}/samples/ecat/EtherCAT/bsp"
)

set(GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(SSC_GATEWAY_DIR "${CMAKE_CURRENT_BINARY_DIR}/ssc_gateway")
set(ADAPTER_CAN_CONFIG
    "${ADAPTER_ROOT}/config/can_devices.yaml"
    CACHE FILEPATH "MCU CAN device/protection configuration"
)
set(PDO_SCHEMA "${ADAPTER_ROOT}/config/pdo_schema.yaml")

find_package(Python3 COMPONENTS Interpreter REQUIRED)

# ==============================================================================
# 3. Dependency Validation
# ==============================================================================

set(SSC_REQUIRED_FILES
    ecat_def.h
    ecatslv.c
    ecatappl.c
    sampleappl.c
    sampleappl.h
    applInterface.h
    eeprom.h
)
foreach(required_file IN LISTS SSC_REQUIRED_FILES)
    if(NOT EXISTS "${ADAPTER_SSC_DIR}/${required_file}")
        message(FATAL_ERROR
            "SSC source missing: ${required_file}. "
            "Import your licensed configured sources with tools/import_ssc.py; "
            "see docs/ethercat.md")
    endif()
endforeach()

file(READ "${ADAPTER_SSC_DIR}/ecat_def.h" SSC_CONFIG_CONTENT)
if(SSC_CONFIG_CONTENT MATCHES "#define[ \t]+DC_SUPPORTED[ \t]+1")
    message(FATAL_ERROR "FreeRun requires DC_SUPPORTED=0 SSC sources")
endif()

if(NOT EXISTS "${HPM_PHY_DIR}/hpm5e31_phy.c")
    message(FATAL_ERROR
        "HPM5E31 PHY implementation missing; update hpm_support to west.yml "
        "revision")
endif()

# ==============================================================================
# 4. Code Generation（构建期生成，生成物是常量唯一来源）
# ==============================================================================

file(MAKE_DIRECTORY "${GENERATED_DIR}" "${SSC_GATEWAY_DIR}")

set(PDO_SCHEMA_HEADER "${GENERATED_DIR}/pdo_schema.hpp")
add_custom_command(
    OUTPUT "${PDO_SCHEMA_HEADER}"
    COMMAND ${Python3_EXECUTABLE} "${ADAPTER_ROOT}/tools/generate_pdo_schema.py"
            --schema "${PDO_SCHEMA}" --output "${PDO_SCHEMA_HEADER}"
    DEPENDS "${ADAPTER_ROOT}/tools/generate_pdo_schema.py" "${PDO_SCHEMA}"
    COMMENT "Generating pdo_schema.hpp"
    VERBATIM)

set(CAN_PROFILE_HEADER "${GENERATED_DIR}/can_profile_generated.hpp")
add_custom_command(
    OUTPUT "${CAN_PROFILE_HEADER}"
    COMMAND ${Python3_EXECUTABLE} "${ADAPTER_ROOT}/tools/generate_can_profile.py"
            --input "${ADAPTER_CAN_CONFIG}" --output "${CAN_PROFILE_HEADER}"
    DEPENDS "${ADAPTER_ROOT}/tools/generate_can_profile.py"
            "${ADAPTER_CAN_CONFIG}"
    COMMENT "Generating can_profile_generated.hpp"
    VERBATIM)

add_custom_target(dust_pdo_schema DEPENDS "${PDO_SCHEMA_HEADER}")
add_custom_target(dust_can_profile DEPENDS "${CAN_PROFILE_HEADER}")

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${ADAPTER_ROOT}/config/ethercat_gateway.json"
    "${ADAPTER_ROOT}/tools/generate_gateway_esi.py")
execute_process(
    COMMAND ${Python3_EXECUTABLE} "${ADAPTER_ROOT}/tools/generate_gateway_esi.py"
            --output "${SSC_GATEWAY_DIR}"
    COMMAND_ERROR_IS_FATAL ANY)

# ==============================================================================
# 5. SSC Source Staging
# ==============================================================================

file(GLOB SSC_INPUTS CONFIGURE_DEPENDS
    "${ADAPTER_SSC_DIR}/*.c" "${ADAPTER_SSC_DIR}/*.h")
set(SSC_SOURCES)
set(SSC_EXCLUDED_FILES sampleappl.c sampleappl.h eeprom.h)
foreach(input_file IN LISTS SSC_INPUTS)
    get_filename_component(file_name "${input_file}" NAME)
    if(file_name IN_LIST SSC_EXCLUDED_FILES)
        continue()
    endif()
    configure_file("${input_file}" "${SSC_GATEWAY_DIR}/${file_name}" COPYONLY)
    if(file_name MATCHES "\\.c$")
        list(APPEND SSC_SOURCES "${SSC_GATEWAY_DIR}/${file_name}")
    endif()
endforeach()
configure_file("${ADAPTER_ROOT}/src/adapters/ethercat_ssc/sampleappl.h"
               "${SSC_GATEWAY_DIR}/sampleappl.h" COPYONLY)

# ==============================================================================
# 6. Layered Static Libraries（依赖方向由链接图强制）
# ==============================================================================

# 6.1 domain: 协议/策略逻辑，链接 Zephyr 接口以统一 libc/stdint 配置。
add_library(dust_domain OBJECT
    "${ADAPTER_ROOT}/src/domain/protocol/wire.cpp"
    "${ADAPTER_ROOT}/src/domain/protocol/can_channel.cpp"
    "${ADAPTER_ROOT}/src/domain/protocol/uart_channel.cpp"
    "${ADAPTER_ROOT}/src/domain/protocol/codec.cpp"
    "${ADAPTER_ROOT}/src/domain/pdo/layout.cpp"
    "${ADAPTER_ROOT}/src/domain/pdo/pdo_dictionary.cpp"
    "${ADAPTER_ROOT}/src/domain/protocol/session.cpp"
    "${ADAPTER_ROOT}/src/domain/can/can_service.cpp"
    "${ADAPTER_ROOT}/src/domain/uart/uart_service.cpp"
    "${ADAPTER_ROOT}/src/domain/protection/protection.cpp"
)
target_include_directories(dust_domain
    PUBLIC "${ADAPTER_ROOT}/src"
    PRIVATE "${GENERATED_DIR}")
target_compile_features(dust_domain PUBLIC cxx_std_17)
target_compile_options(dust_domain PRIVATE -fno-exceptions -fno-rtti)
# 统一 libc/stdint 配置：与 app/adapters 使用同一套 stdint 定义。
target_link_libraries(dust_domain PRIVATE zephyr_interface)
add_dependencies(dust_domain dust_pdo_schema)

# 6.2 drivers: 窄硬件原语 + CAN/UART/IMU 设备类。
add_library(dust_drivers OBJECT
    "${ADAPTER_ROOT}/src/drivers/can/can_driver.cpp"
    "${ADAPTER_ROOT}/src/drivers/uart/uart_driver.cpp"
    "${ADAPTER_ROOT}/src/drivers/uart/uart_receive.cpp"
    "${ADAPTER_ROOT}/src/drivers/spi_imu/imu_sensor.cpp"
    "${ADAPTER_ROOT}/src/drivers/spi_imu/imu_source.cpp"
    "${ADAPTER_ROOT}/src/drivers/can/can_bus.cpp"
    "${ADAPTER_ROOT}/src/drivers/uart/uart_port.cpp"
    "${ADAPTER_ROOT}/src/drivers/pwm/outputs.cpp"
)
if(CONFIG_BUZZER OR CONFIG_BOARD_OUTPUTS)
    target_sources(dust_drivers PRIVATE
        "${ADAPTER_ROOT}/src/drivers/pwm/board_pwm.c")
endif()
if(CONFIG_BUZZER)
    target_sources(dust_drivers PRIVATE
        "${ADAPTER_ROOT}/src/drivers/pwm/buzzer.c")
endif()
target_include_directories(dust_drivers PUBLIC "${ADAPTER_ROOT}/src")
target_link_libraries(dust_drivers PRIVATE zephyr_interface)

# 6.3 adapters: ESC/SSC/board 适配。
add_library(dust_adapters OBJECT
    "${ADAPTER_ROOT}/src/adapters/esc/esc_regs.c"
    "${ADAPTER_ROOT}/src/adapters/esc/esc_boot.c"
    "${ADAPTER_ROOT}/src/adapters/esc/sii.c"
    "${ADAPTER_ROOT}/src/adapters/ethercat_ssc/ssc_stack.cpp"
    "${ADAPTER_ROOT}/src/adapters/ethercat_ssc/ssc_callbacks.cpp"
    "${ADAPTER_ROOT}/src/adapters/ethercat_ssc/ssc_dictionary.cpp"
    "${ADAPTER_ROOT}/src/adapters/ethercat_ssc/io_coordinator.cpp"
    "${ADAPTER_ROOT}/src/adapters/ethercat_ssc/feedback.cpp"
    "${ADAPTER_ROOT}/src/adapters/ethercat_ssc/layout_service.cpp"
    "${HPM_PHY_DIR}/hpm5e31_phy.c"
    ${SSC_SOURCES}
)
target_include_directories(dust_adapters
    PUBLIC "${ADAPTER_ROOT}/src"
    PRIVATE
        "${GENERATED_DIR}"
        "${SSC_GATEWAY_DIR}"
        "${HPM_PHY_DIR}"
        "${HPM_PHY_BOARD_DIR}"
)
add_dependencies(dust_adapters dust_can_profile dust_pdo_schema)
target_compile_options(dust_adapters PRIVATE
    "-include${SSC_GATEWAY_DIR}/gateway_config.h")
set_source_files_properties(${SSC_SOURCES}
    PROPERTIES COMPILE_OPTIONS
        "-Wno-unused-parameter;-Wno-sign-compare;-Wno-implicit-fallthrough")
target_link_libraries(dust_adapters
    PUBLIC dust_domain
    PRIVATE dust_drivers zephyr_interface)

# ==============================================================================
# 7. Application
# ==============================================================================

target_sources(app PRIVATE
    "${ADAPTER_ROOT}/src/main.cpp"
    "${ADAPTER_ROOT}/src/composition.cpp"
    "${ADAPTER_ROOT}/src/identity.cpp"
    "${ADAPTER_ROOT}/src/lifecycle.cpp"
    "${ADAPTER_ROOT}/src/scheduler.cpp"
)
target_include_directories(app PRIVATE
    "${ADAPTER_ROOT}/src"
    "${GENERATED_DIR}"
    "${SSC_GATEWAY_DIR}")
target_sources(app PRIVATE
    $<TARGET_OBJECTS:dust_domain>
    $<TARGET_OBJECTS:dust_drivers>
    $<TARGET_OBJECTS:dust_adapters>)


# 库目标也要等待 Zephyr 生成头（offsets/version 等），否则会在
# zephyr_generated_headers 完成前编译 .obj。
add_dependencies(dust_drivers zephyr_generated_headers)
add_dependencies(dust_adapters zephyr_generated_headers)
add_dependencies(app zephyr_generated_headers dust_can_profile)
