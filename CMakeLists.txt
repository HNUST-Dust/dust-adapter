
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

# Licensed EtherCAT Slave Stack Code (SSC 5.13)
set(ADAPTER_SSC_DIR
    "${ADAPTER_ROOT}/src/ecat/generated"
    CACHE PATH "Licensed SSC 5.13 generated source directory"
)

# HPMicro EtherCAT PHY support
set(HPM_PHY_DIR
    "${HPM_SUPPORT_DIR}/samples/ecat/Middleware/ESC"
)
set(HPM_PHY_BOARD_DIR
    "${HPM_SUPPORT_DIR}/samples/ecat/EtherCAT/bsp"
)

# Generated files
set(GENERATED_DIR
    "${CMAKE_CURRENT_BINARY_DIR}/generated"
)
set(SSC_GATEWAY_DIR
    "${CMAKE_CURRENT_BINARY_DIR}/ssc_gateway"
)

# CAN configuration
set(ADAPTER_CAN_CONFIG
    "${ADAPTER_ROOT}/config/can_devices.yaml"
    CACHE FILEPATH "MCU CAN device/protection configuration"
)

# ==============================================================================
# 3. Dependency Validation
# ==============================================================================

# Check required licensed SSC files
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
            "Import your licensed configured sources with "
            "tools/import_ssc.py; see docs/ethercat.md"
        )
    endif()
endforeach()

# FreeRun mode requires DC_SUPPORTED=0
file(READ
    "${ADAPTER_SSC_DIR}/ecat_def.h"
    SSC_CONFIG_CONTENT
)

if(SSC_CONFIG_CONTENT MATCHES "#define[ \t]+DC_SUPPORTED[ \t]+1")
    message(FATAL_ERROR
        "FreeRun requires DC_SUPPORTED=0 SSC sources"
    )
endif()

# Reuse HPMicro PHY initialization and software link detection.
# Do not import the vendor example's SSC/PDO implementation.
if(NOT EXISTS "${HPM_PHY_DIR}/hpm5e31_phy.c")
    message(FATAL_ERROR
        "HPM5E31 PHY implementation missing; "
        "update hpm_support to west.yml revision"
    )
endif()

# ==============================================================================
# 4. Code Generation
# ==============================================================================

# 4.1 Generate CAN device profile
set(CAN_PROFILE_HEADER
    "${GENERATED_DIR}/can_profile_generated.hpp"
)

add_custom_command(
    OUTPUT "${CAN_PROFILE_HEADER}"

    COMMAND
        ${Python3_EXECUTABLE}
        "${ADAPTER_ROOT}/tools/generate_can_profile.py"
        --input "${ADAPTER_CAN_CONFIG}"
        --output "${CAN_PROFILE_HEADER}"

    DEPENDS
        "${ADAPTER_ROOT}/tools/generate_can_profile.py"
        "${ADAPTER_CAN_CONFIG}"

    VERBATIM
)

# 4.2 Generate EtherCAT gateway configuration
file(MAKE_DIRECTORY "${SSC_GATEWAY_DIR}")

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${ADAPTER_ROOT}/config/ethercat_gateway.json"
    "${ADAPTER_ROOT}/tools/generate_gateway_esi.py"
)

set(GATEWAY_ESI_OPTIONS)

execute_process(
    COMMAND
        ${Python3_EXECUTABLE}
        "${ADAPTER_ROOT}/tools/generate_gateway_esi.py"
        --output "${SSC_GATEWAY_DIR}"
        ${GATEWAY_ESI_OPTIONS}

    COMMAND_ERROR_IS_FATAL ANY
)

# ==============================================================================
# 5. EtherCAT Protocol Library
# ==============================================================================

# Keep the protocol layer independent of the licensed SSC.
# This library can also be reused by native cross-platform tests.
add_library(ecat_protocol STATIC
    "${ADAPTER_ROOT}/src/protocol/pdo_dictionary.cpp"
    "${ADAPTER_ROOT}/src/protocol/layout.cpp"
    "${ADAPTER_ROOT}/src/protocol/core.cpp"
)

target_include_directories(ecat_protocol
    PUBLIC
        "${ADAPTER_ROOT}/src"
)

target_compile_features(ecat_protocol
    PUBLIC cxx_std_17
)

target_compile_options(ecat_protocol
    PRIVATE
        -fno-exceptions
        -fno-rtti
)

target_link_libraries(ecat_protocol
    PRIVATE zephyr_interface
)

# ==============================================================================
# 6. SSC Source Preparation
# ==============================================================================

# Copy licensed SSC files into the build directory.
# Replace the sample application header with our own implementation.
# Never modify the imported third-party sources.

file(GLOB SSC_INPUTS CONFIGURE_DEPENDS
    "${ADAPTER_SSC_DIR}/*.c"
    "${ADAPTER_SSC_DIR}/*.h"
)

set(SSC_SOURCES)

set(SSC_EXCLUDED_FILES
    sampleappl.c
    sampleappl.h
    eeprom.h
)

foreach(input_file IN LISTS SSC_INPUTS)
    get_filename_component(file_name "${input_file}" NAME)

    if(file_name IN_LIST SSC_EXCLUDED_FILES)
        continue()
    endif()

    configure_file(
        "${input_file}"
        "${SSC_GATEWAY_DIR}/${file_name}"
        COPYONLY
    )

    if(file_name MATCHES "\\.c$")
        list(APPEND SSC_SOURCES
            "${SSC_GATEWAY_DIR}/${file_name}"
        )
    endif()
endforeach()

# Use the project's custom SSC application interface
configure_file(
    "${ADAPTER_ROOT}/src/ecat/sampleappl.h"
    "${SSC_GATEWAY_DIR}/sampleappl.h"
    COPYONLY
)

# ==============================================================================
# 7. Application Sources
# ==============================================================================

target_sources(app PRIVATE
    # Application entry point
    "${ADAPTER_ROOT}/src/main.c"

    # EtherCAT slave implementation
    "${ADAPTER_ROOT}/src/ecat/gateway.cpp"
    "${ADAPTER_ROOT}/src/ecat/boot_identity.cpp"
    "${ADAPTER_ROOT}/src/io/io_backend.cpp"
    "${ADAPTER_ROOT}/src/io/can_adapter.cpp"
    "${ADAPTER_ROOT}/src/io/uart_adapter.cpp"

    # Hardware drivers
    "${ADAPTER_ROOT}/src/drivers/uart_receive.cpp"
    "${ADAPTER_ROOT}/src/drivers/imu_sensor.cpp"
    "${ADAPTER_ROOT}/src/drivers/can_driver.cpp"
    "${ADAPTER_ROOT}/src/drivers/uart_driver.cpp"

    # EtherCAT ESC and PHY
    "${ADAPTER_ROOT}/src/ecat/esc/esc_regs.c"
    "${ADAPTER_ROOT}/src/ecat/esc/esc_boot.c"
    "${ADAPTER_ROOT}/src/ecat/esc/sii.c"
    "${ADAPTER_ROOT}/src/ecat/runtime.c"
    "${HPM_PHY_DIR}/hpm5e31_phy.c"

    # Generated CAN profile
    "${CAN_PROFILE_HEADER}"

    # Licensed SSC sources
    ${SSC_SOURCES}
)

# ==============================================================================
# 8. Optional Board Features
# ==============================================================================

# Shared PWM driver
if(CONFIG_BUZZER OR CONFIG_BOARD_OUTPUTS)
    target_sources(app PRIVATE
        "${ADAPTER_ROOT}/src/drivers/board_pwm.c"
    )
endif()

# Buzzer
if(CONFIG_BUZZER)
    target_sources(app PRIVATE
        "${ADAPTER_ROOT}/src/drivers/buzzer.c"
    )
endif()

# IMU heater
if(CONFIG_BOARD_OUTPUTS)
    target_sources(app PRIVATE
        "${ADAPTER_ROOT}/src/drivers/imu_heater.cpp"
    )
endif()

# ==============================================================================
# 9. Include Directories and Linking
# ==============================================================================

target_include_directories(app PRIVATE
    "${ADAPTER_ROOT}/src"
    "${SSC_GATEWAY_DIR}"
    "${GENERATED_DIR}"
    "${HPM_PHY_DIR}"
    "${HPM_PHY_BOARD_DIR}"
)

target_link_libraries(app
    PRIVATE ecat_protocol
)

# Force-include the generated gateway configuration
target_compile_options(app PRIVATE
    "-include${SSC_GATEWAY_DIR}/gateway_config.h"
)

# ==============================================================================
# 10. Third-Party Compiler Warnings
# ==============================================================================

# Suppress selected warnings only for licensed SSC sources.
set_source_files_properties(${SSC_SOURCES}
    PROPERTIES
        COMPILE_OPTIONS
            "-Wno-unused-parameter;-Wno-sign-compare;-Wno-implicit-fallthrough"
)
