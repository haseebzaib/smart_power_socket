#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
namespace firmware_update {
// Shared signed-image writer for HTTPS now and BLE later.
int init();
// Formats one <=160-character SMS. Returns length, or a negative errno.
int format_info(std::span<char> output);
// Optional startup trial; with the health gate off, only clears the watchdog
// streak after sustained main-loop progress, without backup or crypto checks.
void poll_health(bool settingsReady);
int ready_for_update();
int begin(std::size_t signedFileSize, std::span<const uint8_t, 32> expectedSha256);
// Standard DFU manifests lack a file hash. This form requires finish(hash)
// with the hash accumulated while receiving; signature checks still apply.
int begin(std::size_t signedFileSize);
int write(std::span<const uint8_t> data);
// Verify the complete file and mark it pending. Caller reboots after any reply.
// Both the application and MCUboot authenticate its signature.
int finish();
int finish(std::span<const uint8_t, 32> receivedSha256);
void abort();
}
