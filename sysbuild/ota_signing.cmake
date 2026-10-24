# Applied after the SDK's MAIN defaults: resolve the selected PEM against the
# project checkout, allowing identical relative paths on Linux and Windows.
get_filename_component(sps_signing_key "${SB_CONFIG_BOOT_SIGNATURE_KEY_FILE}"
  ABSOLUTE BASE_DIR "${APP_DIR}")
if(NOT EXISTS "${sps_signing_key}")
  message(FATAL_ERROR "SPS signing key missing: ${sps_signing_key}. Restore the existing PEM or select the recovery PEM; no key will be generated automatically.")
endif()
execute_process(
  COMMAND "${PYTHON_EXECUTABLE}" "${APP_DIR}/tools/ota_key_info.py"
    --check-signing-key "${sps_signing_key}"
    --trusted-key "${APP_DIR}/keys/ota/public/primary-ed25519.pem"
    --trusted-key "${APP_DIR}/keys/ota/public/recovery-ed25519.pem"
  RESULT_VARIABLE sps_key_check
  OUTPUT_VARIABLE sps_key_info
  ERROR_VARIABLE sps_key_error)
if(NOT sps_key_check EQUAL 0)
  message(FATAL_ERROR "SPS signer does not match the project trust set: ${sps_key_error}")
endif()
message(STATUS "${sps_key_info}")
set_config_string(${ZCMAKE_APPLICATION} CONFIG_MCUBOOT_SIGNATURE_KEY_FILE "${sps_signing_key}")
