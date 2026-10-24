# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(smart_power_socket)

target_sources(app PRIVATE
	src/main.cpp
	src/alerts.cpp
	src/sms_commands.cpp
	src/device_status.cpp
	src/app_config.cpp
	src/hardware/gpio_con.cpp
	src/hardware/uart_con.cpp
	src/hardware/adc_con.cpp
	src/sensors/hlw811x.cpp
	src/sensors/externalLibs/hlw811x/hlw811x.c
	src/cellular/at_engine.cpp
	src/cellular/sim7080.cpp
)

target_include_directories(app PRIVATE
	src
	src/hardware
	src/sensors
	src/sensors/externalLibs/hlw811x
	src/cellular/
)

if(CONFIG_BOOTLOADER_MCUBOOT)
  if(NOT CONFIG_TASK_WDT)
    message(FATAL_ERROR "The MCUboot carrier profile requires CONFIG_TASK_WDT. Include boards/bl54l15_dvk_nrf54l15_cpuapp.conf with prj.conf.")
  endif()
  target_sources(app PRIVATE
    src/ota/firmware_update.cpp
    src/ota/image_verify.cpp
    modules/sps_boot/src/boot_storage.c
    modules/sps_boot/src/watchdog_reset.c
    src/system/watchdog.cpp
  )
  if(NOT CONFIG_MCUBOOT_BOOTLOADER_SIGNATURE_TYPE_ED25519 OR CONFIG_MCUBOOT_BOOTLOADER_SIGNATURE_TYPE_PURE)
    message(FATAL_ERROR "SPS OTA requires MCUboot SHA-512/Ed25519 hash signing")
  endif()
  set(ota_key_header ${CMAKE_CURRENT_BINARY_DIR}/ota_generated/ota_public_key.hpp)
  add_custom_command(OUTPUT ${ota_key_header}
    COMMAND ${PYTHON_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/export_ota_key.py
      --key ${CONFIG_MCUBOOT_SIGNATURE_KEY_FILE} --output ${ota_key_header}
    DEPENDS ${CONFIG_MCUBOOT_SIGNATURE_KEY_FILE} tools/export_ota_key.py
    VERBATIM)
  target_sources(app PRIVATE ${ota_key_header})
  target_include_directories(app PRIVATE modules/sps_boot/src ${CMAKE_CURRENT_BINARY_DIR}/ota_generated)
endif()

if(CONFIG_SPS_HTTP_OTA)
  target_sources(app PRIVATE src/ota/manifest.cpp src/ota/http_update.cpp src/cellular/sim7080_https.cpp)
endif()
