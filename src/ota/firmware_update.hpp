#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
namespace firmware_update {
// No HTTP/BLE transport or SMS update command yet. Both transports will use
// this same sequential writer. Supply the SHA-256 of the complete signed file.
int init();
// Formats one <=160-character SMS. Returns length, or a negative errno.
int format_info(std::span<char> output);
void poll_health(bool settingsReady);
int begin(std::size_t signedFileSize, std::span<const uint8_t, 32> expectedSha256);
int write(std::span<const uint8_t> data);
// Verify the complete file and mark it pending. Caller reboots after any reply.
// MCUboot authenticates its signature before installing/executing it.
int finish();
void abort();
}
