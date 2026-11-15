cmake_minimum_required(VERSION 3.13)

# Host-side unit tests (no Pico SDK required):
#   cmake -S . -B build-host -DBUILD_HOST_TESTS=ON && cmake --build build-host && ./build-host/test_engine
if (BUILD_HOST_TESTS)
  project(workshop_tracker_tests LANGUAGES CXX)
  set(CMAKE_CXX_STANDARD 17)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  add_executable(test_engine tests/test_engine.cpp)
  target_include_directories(test_engine PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_compile_options(test_engine PRIVATE -O2 -Wall -Wextra -Werror)
  # Helper for web/test/xcheck.test.mjs (JS protocol vs. firmware parser).
  add_executable(xcheck tests/xcheck.cpp)
  target_include_directories(xcheck PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_compile_options(xcheck PRIVATE -O2 -Wall -Wextra -Werror)
  return()
endif()

include(pico_sdk_import.cmake)
project(workshop_tracker C CXX ASM)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
pico_sdk_init()

add_executable(workshop_tracker src/main.cpp src/usb_descriptors.c)

target_include_directories(workshop_tracker PUBLIC
  ${CMAKE_CURRENT_LIST_DIR}/src
  ${CMAKE_CURRENT_LIST_DIR}/vendor/ComputerCard
)

target_compile_options(workshop_tracker PRIVATE
  $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra -Wdouble-promotion -Wfloat-conversion>
  -O2
)

target_compile_definitions(workshop_tracker PRIVATE
  PICO_XOSC_STARTUP_DELAY_MULTIPLIER=64
)

# Whole image runs from RAM: the audio interrupt (core 0) never touches XIP
# while core 1 erases/programs song slots.
pico_set_binary_type(workshop_tracker copy_to_ram)

target_link_libraries(workshop_tracker
  pico_unique_id
  pico_stdlib
  pico_multicore
  tinyusb_device
  tinyusb_board
  hardware_flash
  hardware_dma
  hardware_i2c
  hardware_pwm
  hardware_adc
  hardware_spi
)

target_link_options(workshop_tracker PRIVATE -Wl,--print-memory-usage)

pico_add_extra_outputs(workshop_tracker)
pico_enable_stdio_usb(workshop_tracker 0)
pico_enable_stdio_uart(workshop_tracker 0)
