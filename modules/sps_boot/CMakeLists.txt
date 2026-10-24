zephyr_library()
zephyr_library_sources(src/boot_hooks.c src/boot_storage.c src/watchdog.c src/watchdog_reset.c)

# Replace only the SDK's default single-key provider, without editing the SDK.
# Source properties are applied in MCUboot's own directory where keys.c is added.
if(NOT CONFIG_BOOT_SIGNATURE_TYPE_ED25519 OR CONFIG_BOOT_HW_KEY)
  message(FATAL_ERROR "SPS MCUboot requires software Ed25519 trust-key selection")
endif()
get_filename_component(sps_project_dir "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set_source_files_properties("${APPLICATION_SOURCE_DIR}/keys.c"
  DIRECTORY "${APPLICATION_SOURCE_DIR}" PROPERTIES HEADER_FILE_ONLY TRUE)
set(sps_boot_keys "${CMAKE_BINARY_DIR}/sps_keys/trusted_keys.c")
set(sps_boot_key_metadata "${CMAKE_BINARY_DIR}/sps_keys/trusted_keys.json")
add_custom_command(OUTPUT "${sps_boot_keys}" "${sps_boot_key_metadata}"
  COMMAND "${PYTHON_EXECUTABLE}" "${sps_project_dir}/tools/export_ota_key.py"
    --key "${sps_project_dir}/keys/ota/public/primary-ed25519.pem"
    --key "${sps_project_dir}/keys/ota/public/recovery-ed25519.pem"
    --output "${sps_boot_keys}" --metadata "${sps_boot_key_metadata}" --format mcuboot
  DEPENDS "${sps_project_dir}/tools/export_ota_key.py"
    "${sps_project_dir}/keys/ota/public/primary-ed25519.pem"
    "${sps_project_dir}/keys/ota/public/recovery-ed25519.pem"
  VERBATIM)
zephyr_library_sources("${sps_boot_keys}")
