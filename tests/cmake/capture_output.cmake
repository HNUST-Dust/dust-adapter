# SPDX-License-Identifier: Apache-2.0
# 运行可执行文件并把 stdout 写入 OUT；供 reference/optimized 字节等价测试使用。
if(NOT DEFINED EXE OR NOT DEFINED OUT)
    message(FATAL_ERROR "capture_output.cmake requires -DEXE=<path> -DOUT=<path>")
endif()

execute_process(
    COMMAND "${EXE}"
    OUTPUT_FILE "${OUT}"
    ERROR_VARIABLE errors
    RESULT_VARIABLE result)

if(NOT result EQUAL 0)
    message(FATAL_ERROR "${EXE} failed with exit code ${result}\n${errors}")
endif()
