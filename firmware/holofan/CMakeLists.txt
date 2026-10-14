# HoloFan firmware for the fan's ESP32-C2. Build with ./build.sh (ESP-IDF at
# commit 11edebdbbe, the release/v5.4 build the stock firmware used).
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(holofan)
# SagMeter.cpp restores the brownout threshold before a panic restart (IDF's
# documented hook; IDF 5.0 migration guide "target_link_libraries with project_elf").
target_link_libraries(${project_elf} PRIVATE "-Wl,--wrap=esp_panic_handler")
