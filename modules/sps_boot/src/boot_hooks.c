#include "sps_boot.h"
#include <errno.h>
#include <bootutil/boot_hooks.h>
#include <bootutil/bootutil_public.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/reboot.h>
#include <pm_config.h>
LOG_MODULE_REGISTER(sps_recovery, LOG_LEVEL_INF);

static uint32_t increment(uint32_t value)
{
    return value == UINT32_MAX ? value : value + 1U;
}
static bool save_history(struct sps_boot_history *history)
{
    int rc = sps_boot_history_save(history);
    if (rc) LOG_ERR("Recovery history save failed: %d", rc);
    return rc == 0;
}
static bool record_recovery_request(struct sps_boot_history *history, uint32_t reason)
{
    history->pending_reason = reason;
    history->recovery_recorded = 0;
    return save_history(history);
}

static int stage_backup(struct sps_boot_state *state)
{
    int rc = sps_backup_verify(state);
    if (rc) return rc;
    const struct flash_area *backup, *staging;
    rc = flash_area_open(PM_OTA_IMAGE_B_ID, &backup);
    if (rc) return rc;
    rc = flash_area_open(PM_MCUBOOT_SECONDARY_ID, &staging);
    if (!rc) {
        rc = sps_image_copy(backup, staging, state->backup_length, state->backup_crc);
        flash_area_close(staging);
    }
    flash_area_close(backup);
    if (rc) return rc;
    /* No native swap may start before ARMED is durable. A reset before the
     * journal commit leaves COPY and safely restarts staging from untouched B. */
    rc = boot_set_pending(1);
    if (!rc) {
        state->recovery = SPS_RECOVERY_ARMED;
        rc = sps_boot_state_save(state);
    }
    return rc;
}

fih_ret boot_go_hook(struct boot_rsp *rsp)
{
    struct sps_boot_state state;
    struct sps_boot_history history;
    bool history_ok = sps_boot_history_load(&history) == 0;
    if (!history_ok) LOG_ERR("Recovery history unavailable; boot policy remains independent");
    int rc = sps_boot_state_load(&state);
    bool policy_ok = rc == 0;
    if (policy_ok) {
        uint32_t cause;
        rc = hwinfo_get_reset_cause(&cause);
        if (!rc) cause = sps_watchdog_classify_reset(cause);
        if (!rc && history_ok && (cause & RESET_WATCHDOG)) {
            history.watchdog_resets = increment(history.watchdog_resets);
            history_ok = save_history(&history);
        }
        if (!rc && state.recovery == SPS_RECOVERY_IDLE) {
            state.failures = (cause & RESET_WATCHDOG) ?
                MIN(state.failures + 1U, SPS_WATCHDOG_LIMIT) : 0;
        }
        state.healthy = 0;
        if (!rc) {
            state.reset_cause = cause;
            rc = sps_boot_state_save(&state);
        }
        // RESETREAS accumulates until cleared, including before main(). Save
        // the captured reason for application diagnostics before clearing it.
        if (!rc) rc = hwinfo_clear_reset_cause();
        if (rc) {
            LOG_ERR("Boot policy unavailable: %d", rc);
            policy_ok = false;
        } else {
            LOG_INF("Watchdog failure streak: %u/3; backup=%s", state.failures,
                    state.backup_valid ? "available" : "not provisioned");
        }
    } else LOG_ERR("Boot journal unavailable: %d; no recovery writes", rc);

    if (policy_ok && state.recovery == SPS_RECOVERY_IDLE &&
        state.failures >= SPS_WATCHDOG_LIMIT) {
        rc = sps_backup_verify(&state);
        if (!rc) {
            if (history_ok) history_ok = record_recovery_request(&history, SPS_REASON_WATCHDOG);
            state.recovery = SPS_RECOVERY_REQUEST;
            rc = sps_boot_state_save(&state);
        }
        if (rc) LOG_ERR("Known-good recovery unavailable: %d", rc);
    }
    if (policy_ok && state.recovery == SPS_RECOVERY_COPY) {
        LOG_WRN("Restoring known-good backup B through MCUboot slot A");
        rc = stage_backup(&state);
        if (rc) {
            /* No swap has started; refuse to boot a partially staged image. */
            LOG_ERR("Recovery staging failed: %d; retry on next boot", rc);
            sys_reboot(SYS_REBOOT_COLD);
        }
    }

    fih_ret result;
    FIH_CALL(boot_go, result, rsp); /* Always preserve MCUboot authentication. */
    if (policy_ok && FIH_EQ(result, FIH_SUCCESS) && state.recovery == SPS_RECOVERY_ARMED) {
        const struct flash_area *primary;
        rc = flash_area_open(PM_MCUBOOT_PRIMARY_ID, &primary);
        if (!rc) {
            uint32_t crc;
            rc = sps_area_crc(primary, state.backup_length, &crc);
            if (!rc && crc != state.backup_crc) rc = -EIO;
            flash_area_close(primary);
        }
        if (!rc) {
            if (history_ok && !history.recovery_recorded) {
                history.recoveries = increment(history.recoveries);
                history.last_reason = history.pending_reason;
                history.recovery_recorded = 1;
                history.image_origin = SPS_ORIGIN_BACKUP_B;
                history.image_length = state.backup_length;
                history.image_crc = state.backup_crc;
                // Commit history before clearing ARMED. If power is lost in
                // between, recovery_recorded prevents counting this twice.
                history_ok = save_history(&history);
            }
            state.recovery = SPS_RECOVERY_IDLE;
            state.failures = 0;
            rc = sps_boot_state_save(&state);
            LOG_WRN("Known-good firmware restored; journal result=%d", rc);
        } else LOG_ERR("Recovery not complete: %d", rc);
    }
    /* Always let native MCUboot finish/resume any interrupted swap before
     * repurposing A. REQUEST survives power loss during that native operation. */
    if (policy_ok && (state.recovery == SPS_RECOVERY_REQUEST ||
        (state.recovery == SPS_RECOVERY_IDLE && FIH_NOT_EQ(result, FIH_SUCCESS)))) {
        struct boot_swap_state primary;
        rc = boot_read_swap_state_by_id(PM_MCUBOOT_PRIMARY_ID, &primary);
        bool completed = FIH_EQ(result, FIH_SUCCESS);
        /* A confirmed or untouched primary with failed authentication can be
         * repaired from B. Unfinished native swap metadata must be preserved. */
        bool idle_primary = !rc && (primary.magic == BOOT_MAGIC_UNSET ||
            primary.image_ok == BOOT_FLAG_SET || primary.copy_done == BOOT_FLAG_SET);
        if ((completed || idle_primary) && !sps_backup_verify(&state)) {
            if (history_ok && state.recovery == SPS_RECOVERY_IDLE)
                history_ok = record_recovery_request(&history, SPS_REASON_IMAGE);
            state.recovery = SPS_RECOVERY_COPY;
            if (!sps_boot_state_save(&state)) sys_reboot(SYS_REBOOT_COLD);
        }
    }
    if (history_ok && FIH_EQ(result, FIH_SUCCESS)) {
        const struct flash_area *primary;
        rc = flash_area_open(PM_MCUBOOT_PRIMARY_ID, &primary);
        if (!rc) {
            uint32_t length, crc;
            rc = sps_image_length(primary, &length);
            if (!rc) rc = sps_area_crc(primary, length, &crc);
            if (!rc) {
                if (history.image_length == 0) history.image_origin = SPS_ORIGIN_FACTORY;
                else if (history.image_length != length || history.image_crc != crc)
                    history.image_origin = SPS_ORIGIN_UPDATE_A;
                history.image_length = length;
                history.image_crc = crc;
            }
            flash_area_close(primary);
        }
        if (rc) history.image_origin = SPS_ORIGIN_UNKNOWN;
        // Count only handoffs to the application, excluding the recovery
        // housekeeping reboots above. Counters saturate instead of wrapping.
        history.app_boots = increment(history.app_boots);
        (void)save_history(&history);
    }
    sps_watchdog_feed();
    FIH_RET(result);
}
