#pragma once
#include <cstdint>
#include <zephyr/storage/flash_map.h>
// Verifies the MCUboot SHA-512/Ed25519 image using the configured boot key.
// MCUboot independently authenticates the image again during installation.
int verify_update_signature(const flash_area *area, uint32_t signedFileSize);
