# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/prepare.cmake)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(dust_usb_gateway)
set(generated ${CMAKE_CURRENT_BINARY_DIR}/generated)
set(message_names booster_state chassismotors_feedback_raw chassismotors_send_raw
 gimbal_state hi91_imu_sample pc_auto_aim_command remote_input_state usb_raw_frame_queue)
foreach(name IN LISTS message_names)
 list(APPEND message_inputs ${DUST_ADAPTER_ROOT}/interfaces/${name}.msg)
 list(APPEND message_headers ${generated}/msg/${name}.hpp)
endforeach()
add_custom_command(OUTPUT ${generated}/msg/messages_generated.cpp ${message_headers}
 COMMAND ${PYTHON_EXECUTABLE} ${DUST_ADAPTER_ROOT}/tools/generate_messages.py
 --output-dir ${generated}/msg --source-output ${generated}/msg/messages_generated.cpp ${message_inputs}
 DEPENDS ${DUST_ADAPTER_ROOT}/tools/generate_messages.py ${message_inputs}
 VERBATIM)
add_custom_command(OUTPUT ${generated}/params/motors_params_generated.h
 COMMAND ${PYTHON_EXECUTABLE} ${DUST_ADAPTER_ROOT}/tools/generate_params.py
 --input ${DUST_ADAPTER_ROOT}/config/motors_params.yaml
 --schema ${DUST_ADAPTER_ROOT}/config/motors_params.schema.yaml
 --namespace platform::motors_params --output ${generated}/params/motors_params_generated.h
 DEPENDS ${DUST_ADAPTER_ROOT}/tools/generate_params.py
 ${DUST_ADAPTER_ROOT}/config/motors_params.yaml ${DUST_ADAPTER_ROOT}/config/motors_params.schema.yaml
 VERBATIM)
target_sources(app PRIVATE src/main.cpp src/peripherals.cpp
 ${DUST_ADAPTER_ROOT}/drivers/can/can_dispatch.cpp
 ${DUST_ADAPTER_ROOT}/drivers/usb/usb_session.cpp
 ${DUST_ADAPTER_ROOT}/drivers/imu/onboard_imu.cpp
 ${DUST_ADAPTER_ROOT}/protocols/motors/dm_motor_protocol.cpp
 ${generated}/msg/messages_generated.cpp ${message_headers}
 ${generated}/params/motors_params_generated.h)
target_sources_ifdef(CONFIG_WBR_UART10_LINE_IDLE app PRIVATE src/uart10_line_idle.cpp)
target_include_directories(app PRIVATE ${DUST_ADAPTER_ROOT}/include ${generated} ${generated}/params)
target_compile_definitions(app PRIVATE WBR_USB_BULK_TRANSPORT=1 WBR_USB_RAW_OUT_BYTES=128 WBR_CAN_FORWARDING=1)
zephyr_include_directories(${DUST_HPM_SUPPORT_DIR}/samples/cherryusb/config)
