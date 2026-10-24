#include "firmware_update.hpp"
#include "image_verify.hpp"
#if defined(CONFIG_SPS_HTTP_OTA)
#include "http_update.hpp"
#endif
#include "sps_boot.h"
#include "system/watchdog.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <zephyr/app_version.h>
#include <zephyr/dfu/mcuboot.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <pm_config.h>
#include <zephyr/sys/crc.h>
LOG_MODULE_REGISTER(firmware_update, LOG_LEVEL_INF);
namespace {
K_MUTEX_DEFINE(updateMutex);
bool ready, active, healthy, scheduled, expectedHashKnown;
int64_t healthySince = -1;
std::size_t expectedSize, receivedSize;
std::array<uint8_t, 32> expectedHash{};
uint32_t downloadCrc;
const flash_area *staging;

int backup_current(sps_boot_state &state)
{
    const flash_area *primary, *backup;
    int rc = flash_area_open(PM_MCUBOOT_PRIMARY_ID, &primary);
    if (rc) return rc;
    rc = flash_area_open(PM_OTA_IMAGE_B_ID, &backup);
    if (!rc) {
        uint32_t length, crc;
        rc = sps_image_length(primary, &length);
        if (!rc) rc = sps_area_crc(primary, length, &crc);
        // Do not erase B if it already contains this exact healthy image.
        if (!rc && state.backup_valid && state.backup_length == length &&
            state.backup_crc == crc && sps_backup_verify(&state) == 0) {
            flash_area_close(backup);
            flash_area_close(primary);
            return 0;
        }
        if (!rc) {
            state.backup_valid = 0;
            rc = sps_boot_state_save(&state);
        }
        if (!rc) {
            LOG_INF("Saving known-good internal firmware to backup B (%u bytes)", length);
            rc = sps_image_copy(primary, backup, length, crc);
        }
        if (!rc) {
            state.backup_length = length;
            state.backup_crc = crc;
            state.backup_valid = 1;
            rc = sps_boot_state_save(&state);
        }
        flash_area_close(backup);
    }
    flash_area_close(primary);
    if (!rc) LOG_INF("Known-good backup B verified and committed");
    return rc;
}
struct lock {
    lock() { k_mutex_lock(&updateMutex, K_FOREVER); }
    ~lock() { k_mutex_unlock(&updateMutex); }
};
}
int firmware_update::init()
{
    lock guard;
    sps_boot_state state;
    int rc = sps_boot_state_load(&state);
    ready = rc == 0;
    if (!rc) {
        LOG_INF("MCUboot: internal primary=1 MiB; external A=staging, B=known-good, each 1 MiB");
        LOG_INF("Recovery: %u/3 watchdog failures; healthy after %u s of main-loop progress; backup=%s",
                state.failures, static_cast<unsigned>(SPS_HEALTHY_MS / 1000U),
                state.backup_valid ? "available" : "not provisioned");
    } else LOG_ERR("OTA journal unavailable: %d; updates disabled", rc);
    return rc;
}
int firmware_update::format_info(std::span<char> output)
{
    lock guard;
    sps_boot_state state;
    int rc = sps_boot_state_load(&state);
    if (rc) return rc;
    sps_boot_history history;
    rc = sps_boot_history_load(&history);
    if (rc) return rc;
    const char *source = history.image_origin == SPS_ORIGIN_BACKUP_B ? "Backup B" :
        history.image_origin == SPS_ORIGIN_UPDATE_A ? "Updated" :
        history.image_origin == SPS_ORIGIN_FACTORY ? "Initial" : "Unknown";
    const char *backup = !state.backup_valid ? "None" :
        sps_backup_verify(&state) == 0 ? "Ready" : "Bad";
    const char *reason = history.last_reason == SPS_REASON_WATCHDOG ? "Watchdog" :
        history.last_reason == SPS_REASON_IMAGE ? "Image" : "None";
    const char *update = state.recovery != SPS_RECOVERY_IDLE ? "Recovery" :
        scheduled ? "Pending" : active ? "Stage" : "Idle";
#if defined(CONFIG_SPS_HTTP_OTA)
    if (state.recovery == SPS_RECOVERY_IDLE && http_update::busy()) update = http_update::phase_name();
#endif
    int n = std::snprintf(output.data(), output.size(),
        "OTA\nFW=%s\nRun=Internal\nFrom=%s\nBackup=%s\nHealth=%s/%us\nBoots=%u WDT=%u\n"
        "Streak=%u/3\nRecoveries=%u\nLast=%s\nUpdate=%s",
        APP_VERSION_TWEAK_STRING, source, backup, state.healthy ? "OK" : "Wait",
        static_cast<unsigned>(SPS_HEALTHY_MS / 1000U),
        static_cast<unsigned>(history.app_boots), static_cast<unsigned>(history.watchdog_resets),
        static_cast<unsigned>(state.failures), static_cast<unsigned>(history.recoveries), reason, update);
    if (n > 160) {
        // Preserve the full image version and exact counters even when long
        // versions/cumulative counts need compact labels to fit one SMS.
        n = std::snprintf(output.data(), output.size(),
            "FW=%s\nRun=Int\nFrom=%s\nB=%s\nHealth=%s/%us\nBoots=%u WDT=%u\n"
            "Streak=%u/3\nRec=%u\nLast=%s\nUpd=%s",
            APP_VERSION_TWEAK_STRING, source, backup, state.healthy ? "OK" : "Wait",
            static_cast<unsigned>(SPS_HEALTHY_MS / 1000U),
            static_cast<unsigned>(history.app_boots), static_cast<unsigned>(history.watchdog_resets),
            static_cast<unsigned>(state.failures), static_cast<unsigned>(history.recoveries), reason, update);
    }
    return n < 0 || n > 160 || static_cast<std::size_t>(n) >= output.size() ? -ENOSPC : n;
}
void firmware_update::poll_health(bool settingsReady)
{
    if (k_mutex_lock(&updateMutex, K_NO_WAIT)) return;
    if (!ready || healthy || active || scheduled || !settingsReady) {
        if (!settingsReady) healthySince = -1;
        k_mutex_unlock(&updateMutex);
        return;
    }
    const int64_t now = k_uptime_get();
    if (healthySince < 0) healthySince = now;
    if (now - healthySince >= SPS_HEALTHY_MS) {
        app_watchdog::operation allowance{app_watchdog::flashTimeoutMs, "firmware health/backup"};
        sps_boot_state state;
        int rc = sps_boot_state_load(&state);
        if (!rc && state.recovery != SPS_RECOVERY_IDLE) rc = -EBUSY;
        // Provision the first factory backup only. After an update, preserve
        // the previous version in B until the NEXT download begins.
        if (!rc && !state.backup_valid) rc = backup_current(state);
        if (!rc) rc = sps_backup_verify(&state);
        if (!rc) rc = boot_write_img_confirmed();
        if (!rc) {
            state.failures = 0;
            state.healthy = 1;
            rc = sps_boot_state_save(&state);
        }
        if (!rc) {
            healthy = true;
            LOG_INF("Firmware healthy: watchdog streak cleared; known-good backup B retained");
        } else {
            LOG_ERR("Firmware health commit failed: %d; retry in 30 s", rc);
            healthySince = now - SPS_HEALTHY_MS + 30000;
        }
    }
    k_mutex_unlock(&updateMutex);
}
static int prepare_download(std::size_t size, const uint8_t *sha256)
{
    app_watchdog::operation allowance{app_watchdog::flashTimeoutMs, "OTA prepare"};
    lock guard;
    if (!ready || !healthy) return -EAGAIN;
    if (active || scheduled) return -EBUSY;
    if (!size || size > PM_MCUBOOT_SECONDARY_SIZE - 0x4000) return -EFBIG;
    sps_boot_state state;
    int rc = sps_boot_state_load(&state);
    if (!rc && (!state.healthy || state.recovery != SPS_RECOVERY_IDLE)) rc = -EBUSY;
    // Save the running healthy version BEFORE allowing A to be overwritten.
    if (!rc) rc = backup_current(state);
    if (!rc) rc = flash_area_open(PM_MCUBOOT_SECONDARY_ID, &staging);
    if (rc) return rc;
    rc = sps_area_erase(staging);
    if (rc) {
        flash_area_close(staging);
        staging = nullptr;
        return rc;
    }
    expectedHashKnown = sha256 != nullptr;
    if (sha256) std::copy_n(sha256, expectedHash.size(), expectedHash.begin());
    expectedSize = size;
    receivedSize = 0;
    downloadCrc = 0;
    active = true;
    LOG_INF("OTA ready: signed file %u bytes, staging A; backup B protected", static_cast<unsigned>(size));
    return 0;
}
int firmware_update::write(std::span<const uint8_t> data)
{
    app_watchdog::operation allowance{app_watchdog::flashTimeoutMs, "OTA write"};
    lock guard;
    if (!active) return -EINVAL;
    if (data.empty() || data.size() > expectedSize - receivedSize) return -EINVAL;
    int rc = flash_area_write(staging, receivedSize, data.data(), data.size());
    if (rc) return rc;
    downloadCrc = crc32_ieee_update(downloadCrc, data.data(), data.size());
    receivedSize += data.size();
    return 0;
}
static int finish_download(const uint8_t *receivedHash)
{
    app_watchdog::operation allowance{app_watchdog::flashTimeoutMs, "OTA verify"};
    lock guard;
    if (!active || receivedSize != expectedSize) return -EINVAL;
    if (receivedHash) {
        if (expectedHashKnown && !std::equal(expectedHash.begin(), expectedHash.end(), receivedHash)) return -EILSEQ;
        std::copy_n(receivedHash, expectedHash.size(), expectedHash.begin());
        expectedHashKnown = true;
    }
    if (!expectedHashKnown) return -EINVAL;
    uint32_t length, crc;
    int rc = sps_image_length(staging, &length);
    if (!rc && length != expectedSize) rc = -EBADMSG;
    if (!rc) rc = sps_area_crc(staging, length, &crc);
    if (!rc && crc != downloadCrc) rc = -EIO;
    std::array<uint8_t, 1024> buf{};
    flash_area_check check{};
    check.match = expectedHash.data();
    check.clen = expectedSize;
    check.rbuf = buf.data();
    check.rblen = buf.size();
    if (!rc) rc = flash_area_check_int_sha256(staging, &check);
    if (!rc) rc = verify_update_signature(staging, length);
    if (!rc) rc = boot_request_upgrade(BOOT_UPGRADE_PERMANENT);
    // Permanent here disables MCUboot's one-reset trial policy. Lifetime
    // rollback is handled independently by the boot journal and backup B.
    if (!rc) {
        scheduled = true;
        active = false;
        flash_area_close(staging);
        staging = nullptr;
        LOG_INF("OTA SHA-256, readback and Ed25519 signature verified; reboot ready");
    } else LOG_ERR("OTA finish failed: %d; image not scheduled", rc);
    return rc;
}
void firmware_update::abort()
{
    lock guard;
    if (active) {
        flash_area_close(staging);
        staging = nullptr;
        active = false;
        LOG_WRN("OTA download abandoned; running firmware and backup B retained");
    }
}

int firmware_update::ready_for_update()
{
    lock guard;
    if (!ready || !healthy) return -EAGAIN;
    if (active || scheduled) return -EBUSY;
    return 0;
}
int firmware_update::begin(std::size_t size, std::span<const uint8_t,32> sha256) { return prepare_download(size,sha256.data()); }
int firmware_update::begin(std::size_t size) { return prepare_download(size,nullptr); }
int firmware_update::finish() { return finish_download(nullptr); }
int firmware_update::finish(std::span<const uint8_t,32> sha256) { return finish_download(sha256.data()); }
