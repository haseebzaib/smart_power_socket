# Apply the carrier wiring to MCUboot too; do not use the DVK's flash pins.
set(mcuboot_DTC_OVERLAY_FILE "${APP_DIR}/boards/bl54l15_dvk_nrf54l15_cpuapp.overlay" CACHE STRING "")
set(mcuboot_EXTRA_ZEPHYR_MODULES "${APP_DIR}/modules/sps_boot" CACHE STRING "")

if(SB_CONFIG_BOOTLOADER_MCUBOOT AND SB_CONFIG_DFU_ZIP_APP)
  # VERSION affects both the application's signature and sysbuild's ZIP
  # metadata. Reconfigure the parent too when it changes.
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${APP_DIR}/VERSION")

  # Refresh the publishable folder on every normal sysbuild, including when
  # it was deleted or edited without changing the signed application.
  add_custom_target(sps_ota_release ALL
    COMMAND "${PYTHON_EXECUTABLE}" "${APP_DIR}/tools/prepare_ota_folder.py"
      "${CMAKE_BINARY_DIR}" --version-file "${APP_DIR}/VERSION"
    BYPRODUCTS
      "${CMAKE_BINARY_DIR}/dfu_application/manifest.json"
      "${CMAKE_BINARY_DIR}/dfu_application/smart_power_socket.signed.bin"
    COMMENT "Validating and refreshing the SPS OTA release folder"
    VERBATIM)
  add_dependencies(sps_ota_release dfu_application_zip)
endif()
