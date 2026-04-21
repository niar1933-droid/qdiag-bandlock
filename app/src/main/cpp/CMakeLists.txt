cmake_minimum_required(VERSION 3.22.1)
project(qdiag_native C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_library(qdiag_native SHARED
        jni_bridge.c
        diag.c
        hdlc.c
        qmi_nas.c
        efs2.c
        sniffer.c
        qrtr.c
)

target_include_directories(qdiag_native PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

find_library(log-lib log)
target_link_libraries(qdiag_native ${log-lib})

target_compile_options(qdiag_native PRIVATE -Wall -Wextra -Wno-unused-parameter)
