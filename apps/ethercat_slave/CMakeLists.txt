# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/prepare.cmake)
set(DUST_ADAPTER_SSC_DIR "${DUST_ADAPTER_ROOT}/external/ssc" CACHE PATH "Licensed SSC 5.13 generated source directory")
foreach(required ecat_def.h ecatslv.c ecatappl.c sampleappl.c sampleappl.h applInterface.h eeprom.h)
 if(NOT EXISTS "${DUST_ADAPTER_SSC_DIR}/${required}")
  message(FATAL_ERROR "SSC source missing: ${required}. Import your licensed configured sources with tools/import_ssc.py; see docs/ethercat.md")
 endif()
endforeach()
# 复用锁定版本中新板的 PHY 初始化与软件链路检测，不引入示例的 SSC/PDO。
set(hpm_phy_dir "${DUST_HPM_SUPPORT_DIR}/samples/ecat/Middleware/ESC")
set(hpm_phy_board_dir "${DUST_HPM_SUPPORT_DIR}/samples/ecat/EtherCAT/bsp")
if(NOT EXISTS "${hpm_phy_dir}/hpm5e31_phy.c")
 message(FATAL_ERROR "HPM5E31 PHY implementation missing; update hpm_support to west.yml revision")
endif()
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(dust_ethercat_slave)
set(generated "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(DUST_ADAPTER_CAN_CONFIG "${DUST_ADAPTER_ROOT}/config/can_devices.yaml" CACHE FILEPATH "MCU CAN device/protection configuration")
add_custom_command(OUTPUT ${generated}/can_profile_generated.hpp
 COMMAND ${Python3_EXECUTABLE} ${DUST_ADAPTER_ROOT}/tools/generate_can_profile.py
 --input ${DUST_ADAPTER_CAN_CONFIG} --output ${generated}/can_profile_generated.hpp
 DEPENDS ${DUST_ADAPTER_ROOT}/tools/generate_can_profile.py ${DUST_ADAPTER_CAN_CONFIG}
 VERBATIM)
# 协议层仍独立于 SSC，供原生交叉测试复用。
add_library(dust_ecat_protocol STATIC
 ${DUST_ADAPTER_ROOT}/src/ethercat/pdo_dictionary.cpp
 ${DUST_ADAPTER_ROOT}/src/ethercat/layout.cpp
 ${DUST_ADAPTER_ROOT}/src/ethercat/core.cpp)
target_include_directories(dust_ecat_protocol PUBLIC ${DUST_ADAPTER_ROOT}/src)
target_compile_features(dust_ecat_protocol PUBLIC cxx_std_17)
target_compile_options(dust_ecat_protocol PRIVATE -fno-exceptions -fno-rtti)
target_link_libraries(dust_ecat_protocol PRIVATE zephyr_interface)
# 在构建目录组合 licensed SSC 与自有应用头，避免改写导入的第三方文件。
set(gateway_generated "${CMAKE_CURRENT_BINARY_DIR}/ssc_gateway")
file(MAKE_DIRECTORY "${gateway_generated}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
 ${CMAKE_CURRENT_LIST_DIR}/gateway.json
 ${DUST_ADAPTER_ROOT}/tools/generate_gateway_esi.py)
execute_process(COMMAND ${Python3_EXECUTABLE}
 ${DUST_ADAPTER_ROOT}/tools/generate_gateway_esi.py --output ${gateway_generated}
 COMMAND_ERROR_IS_FATAL ANY)
file(GLOB ssc_inputs CONFIGURE_DEPENDS ${DUST_ADAPTER_SSC_DIR}/*.c ${DUST_ADAPTER_SSC_DIR}/*.h)
set(ssc_sources)
foreach(input IN LISTS ssc_inputs)
 get_filename_component(name "${input}" NAME)
 if(name STREQUAL "sampleappl.c" OR name STREQUAL "sampleappl.h" OR name STREQUAL "eeprom.h")
  continue()
 endif()
 configure_file("${input}" "${gateway_generated}/${name}" COPYONLY)
 if(name MATCHES "\\.c$")
  list(APPEND ssc_sources "${gateway_generated}/${name}")
 endif()
endforeach()
configure_file(${CMAKE_CURRENT_LIST_DIR}/src/ssc/sampleappl.h
 ${gateway_generated}/sampleappl.h COPYONLY)
target_sources(app PRIVATE
 ${CMAKE_CURRENT_LIST_DIR}/src/main.c
 ${CMAKE_CURRENT_LIST_DIR}/src/rtt_trace.cpp
 ${CMAKE_CURRENT_LIST_DIR}/src/gateway.cpp
 ${CMAKE_CURRENT_LIST_DIR}/src/io_backend.cpp
 ${CMAKE_CURRENT_LIST_DIR}/src/boot_identity.cpp
 ${DUST_ADAPTER_ROOT}/src/drivers/uart_receive.cpp
 ${generated}/can_profile_generated.hpp
 ${CMAKE_CURRENT_LIST_DIR}/src/hpm5e_esc_hw.c
 ${hpm_phy_dir}/hpm5e31_phy.c
 ${ssc_sources})
target_include_directories(app PRIVATE
 ${CMAKE_CURRENT_LIST_DIR}/src ${gateway_generated} ${generated}
 ${hpm_phy_dir} ${hpm_phy_board_dir})
target_link_libraries(app PRIVATE dust_ecat_protocol)
target_compile_options(app PRIVATE "-include${gateway_generated}/gateway_config.h")
set_source_files_properties(${ssc_sources} PROPERTIES
 COMPILE_OPTIONS "-Wno-unused-parameter;-Wno-sign-compare;-Wno-implicit-fallthrough")
