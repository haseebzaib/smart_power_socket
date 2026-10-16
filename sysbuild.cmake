# Apply the carrier wiring to MCUboot too; do not use the DVK's flash pins.
set(mcuboot_DTC_OVERLAY_FILE "${APP_DIR}/boards/bl54l15_dvk_nrf54l15_cpuapp.overlay" CACHE STRING "")
set(mcuboot_EXTRA_ZEPHYR_MODULES "${APP_DIR}/modules/sps_boot" CACHE STRING "")
