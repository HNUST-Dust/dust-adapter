# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
# Export compiler flags and include paths for clangd.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
include(${CMAKE_CURRENT_LIST_DIR}/cmake/prepare.cmake)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(dust_usb_gateway)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(generated ${CMAKE_CURRENT_BINARY_DIR}/generated)
set(DUST_ADAPTER_CAN_CONFIG "${DUST_ADAPTER_ROOT}/config/can_devices.yaml" CACHE FILEPATH "MCU CAN device/protection configuration")
add_custom_command(OUTPUT ${generated}/can_profile_generated.hpp
 COMMAND ${PYTHON_EXECUTABLE} ${DUST_ADAPTER_ROOT}/tools/generate_can_profile.py
 --input ${DUST_ADAPTER_CAN_CONFIG}
 --output ${generated}/can_profile_generated.hpp
 DEPENDS ${DUST_ADAPTER_ROOT}/tools/generate_can_profile.py
 ${DUST_ADAPTER_CAN_CONFIG}
 VERBATIM)
target_sources(app PRIVATE
 src/main.cpp
 src/gateway_runtime.cpp
 src/usb_reports.cpp
 src/imu_reports.cpp
 src/drivers/uart_port.cpp
 src/drivers/board_indicators.cpp
 src/drivers/imu_heater.cpp
 src/drivers/uart_receive.cpp
 src/drivers/can_port.cpp
 src/drivers/usb_identity.cpp
 src/drivers/usb_transport.cpp
 src/drivers/imu_sensor.cpp
 ${generated}/can_profile_generated.hpp)
target_include_directories(app PRIVATE ${DUST_ADAPTER_ROOT}/src ${generated})
target_compile_definitions(app PRIVATE DUST_USB_OUT_BUFFER_BYTES=128)
if(CONFIG_CHERRYUSB_DEVICE_HPM)
  zephyr_include_directories(${DUST_HPM_SUPPORT_DIR}/samples/cherryusb/config)
endif()
