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
# 核心暂不绑定 SSC/硬件；独立库同时验证目标工具链兼容性。
add_library(dust_ecat_protocol STATIC
 ${DUST_ADAPTER_ROOT}/src/ethercat/pdo_dictionary.cpp
 ${DUST_ADAPTER_ROOT}/src/ethercat/layout.cpp
 ${DUST_ADAPTER_ROOT}/src/ethercat/core.cpp)
target_include_directories(dust_ecat_protocol PUBLIC ${DUST_ADAPTER_ROOT}/src)
target_compile_features(dust_ecat_protocol PUBLIC cxx_std_17)
target_compile_options(dust_ecat_protocol PRIVATE -fno-exceptions -fno-rtti)
target_link_libraries(dust_ecat_protocol PRIVATE zephyr_interface)
file(GLOB ssc_sources CONFIGURE_DEPENDS ${DUST_ADAPTER_SSC_DIR}/*.c)
target_sources(app PRIVATE
 ${CMAKE_CURRENT_LIST_DIR}/src/main.c
 ${CMAKE_CURRENT_LIST_DIR}/src/hpm5e_esc_hw.c
 ${hpm_phy_dir}/hpm5e31_phy.c
 ${ssc_sources})
target_include_directories(app PRIVATE
 ${CMAKE_CURRENT_LIST_DIR}/src ${DUST_ADAPTER_SSC_DIR}
 ${hpm_phy_dir} ${hpm_phy_board_dir})
target_link_libraries(app PRIVATE dust_ecat_protocol)
set_source_files_properties(${ssc_sources} PROPERTIES
 COMPILE_OPTIONS "-Wno-unused-parameter;-Wno-sign-compare;-Wno-implicit-fallthrough")
