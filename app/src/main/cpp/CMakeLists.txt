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
        qmux.c
)

target_include_directories(qdiag_native PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

find_library(log-lib log)
target_link_libraries(qdiag_native ${log-lib})

target_compile_options(qdiag_native PRIVATE -Wall -Wextra -Wno-unused-parameter)

# ------------------------------------------------------------------
# qdiag_helper — standalone ELF executable. We rename its output to
# libqdiag_helper_exec.so so the Android Gradle Plugin packages it as
# if it were a shared library (AGP only packages lib*.so from CMake).
# At runtime we copy it out of nativeLibraryDir and exec it under `su`.
# ------------------------------------------------------------------
add_executable(qdiag_helper helper_main.c)
set_target_properties(qdiag_helper PROPERTIES
        OUTPUT_NAME "qdiag_helper_exec"
        PREFIX "lib"
        SUFFIX ".so")
target_link_libraries(qdiag_helper ${log-lib} dl)
target_compile_options(qdiag_helper PRIVATE -Wall -Wextra -Wno-unused-parameter)
