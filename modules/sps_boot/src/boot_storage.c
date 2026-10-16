#include "sps_boot.h"
#include <errno.h>
#include <string.h>
#include <zephyr/fs/nvs.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>
#include <zephyr/logging/log.h>
#include <pm_config.h>
LOG_MODULE_REGISTER(boot_storage, LOG_LEVEL_INF);
static struct nvs_fs journal;
static bool mounted;
static int mount_journal(void)
{
    if (mounted) return 0;
    const struct flash_area *area;
    int rc = flash_area_open(PM_NVS_STORAGE_ID, &area);
    if (rc) return rc;
    journal.flash_device = area->fa_dev;
    journal.offset = area->fa_off;
    journal.sector_size = 4096;
    journal.sector_count = area->fa_size / 4096;
    rc = nvs_mount(&journal);
    flash_area_close(area);
    if (!rc) mounted = true;
    return rc;
}
int sps_boot_state_load(struct sps_boot_state *state)
{
    int rc = mount_journal();
    if (rc) return rc;
    memset(state, 0, sizeof(*state));
    ssize_t n = nvs_read(&journal, 1, state, sizeof(*state));
    if (n == -ENOENT) {
        state->magic = SPS_BOOT_MAGIC;
        state->version = SPS_BOOT_VERSION;
        return 0;
    }
    if (n < 0) return (int)n;
    if (n != sizeof(*state) || state->magic != SPS_BOOT_MAGIC ||
        state->version != SPS_BOOT_VERSION || state->failures > SPS_WATCHDOG_LIMIT || state->recovery > SPS_RECOVERY_ARMED ||
        state->crc != crc32_ieee((const uint8_t *)state, offsetof(struct sps_boot_state, crc)))
        return -EBADMSG;
    return 0;
}
int sps_boot_state_save(struct sps_boot_state *state)
{
    int rc = mount_journal();
    if (rc) return rc;
    state->crc = crc32_ieee((const uint8_t *)state, offsetof(struct sps_boot_state, crc));
    ssize_t n = nvs_write(&journal, 1, state, sizeof(*state));
    if (n < 0) return (int)n;
    struct sps_boot_state check;
    n = nvs_read(&journal, 1, &check, sizeof(check));
    return n == sizeof(check) && !memcmp(state, &check, sizeof(check)) ? 0 : -EIO;
}
int sps_boot_history_load(struct sps_boot_history *history)
{
    int rc = mount_journal();
    if (rc) return rc;
    memset(history, 0, sizeof(*history));
    ssize_t n = nvs_read(&journal, 2, history, sizeof(*history));
    if (n == -ENOENT) {
        history->magic = SPS_HISTORY_MAGIC;
        history->version = 1;
        return 0;
    }
    if (n < 0) return (int)n;
    if (n != sizeof(*history) || history->magic != SPS_HISTORY_MAGIC || history->version != 1 ||
        history->image_origin > SPS_ORIGIN_BACKUP_B || history->last_reason > SPS_REASON_IMAGE ||
        history->pending_reason > SPS_REASON_IMAGE ||
        history->crc != crc32_ieee((const uint8_t *)history, offsetof(struct sps_boot_history, crc)))
        return -EBADMSG;
    return 0;
}
int sps_boot_history_save(struct sps_boot_history *history)
{
    int rc = mount_journal();
    if (rc) return rc;
    history->crc = crc32_ieee((const uint8_t *)history, offsetof(struct sps_boot_history, crc));
    ssize_t n = nvs_write(&journal, 2, history, sizeof(*history));
    if (n < 0) return (int)n;
    struct sps_boot_history check;
    n = nvs_read(&journal, 2, &check, sizeof(check));
    return n == sizeof(check) && !memcmp(history, &check, sizeof(check)) ? 0 : -EIO;
}
/* Parse the signed image extent, including protected and unprotected TLVs.
 * This is bounds checking, not authentication. MCUboot verifies signatures. */
int sps_image_length(const struct flash_area *area, uint32_t *length)
{
    uint8_t header[32], tlv[4];
    int rc = flash_area_read(area, 0, header, sizeof(header));
    if (rc) return rc;
    uint32_t h = sys_get_le16(header + 8);
    uint32_t protected_size = sys_get_le16(header + 10);
    uint32_t body = sys_get_le32(header + 12);
    uint32_t flags = sys_get_le32(header + 16);
    /* Only plain, internal-execution images are supported by this carrier. */
    if (sys_get_le32(header) != 0x96f3b83dU || h != 0x800 || body == 0 || flags != 0 ||
        h > area->fa_size || body > area->fa_size - h)
        return -EBADMSG;
    uint32_t pos = h + body;
    if (protected_size) {
        if (protected_size < 4 || protected_size > area->fa_size - pos) return -EBADMSG;
        rc = flash_area_read(area, pos, tlv, sizeof(tlv));
        if (rc) return rc;
        if (sys_get_le16(tlv) != 0x6908 || sys_get_le16(tlv + 2) != protected_size)
            return -EBADMSG;
        pos += protected_size;
    }
    if (area->fa_size - pos < sizeof(tlv)) return -EBADMSG;
    rc = flash_area_read(area, pos, tlv, sizeof(tlv));
    if (rc) return rc;
    uint32_t total = sys_get_le16(tlv + 2);
    if (sys_get_le16(tlv) != 0x6907 || total < 4 || total > area->fa_size - pos)
        return -EBADMSG;
    *length = pos + total;
    /* Reserve 16 KiB for scratch-swap trailers (256 sectors, max alignment).
     * Keep this conservative limit consistent in app and bootloader. */
    return *length <= area->fa_size - 0x4000 ? 0 : -EFBIG;
}
int sps_area_crc(const struct flash_area *area, uint32_t length, uint32_t *crc)
{
    if (!length || length > area->fa_size) return -EINVAL;
    uint8_t buf[1024];
    uint32_t value = 0;
    for (uint32_t off = 0; off < length;) {
        size_t n = MIN(sizeof(buf), length - off);
        int rc = flash_area_read(area, off, buf, n);
        if (rc) return rc;
        value = crc32_ieee_update(value, buf, n);
        off += n;
#if defined(CONFIG_MCUBOOT)
        sps_watchdog_feed();
#endif
    }
    *crc = value;
    return 0;
}
int sps_area_erase(const struct flash_area *area)
{
    for (uint32_t off = 0; off < area->fa_size; off += 4096) {
        int rc = flash_area_erase(area, off, 4096);
        if (rc) return rc;
#if defined(CONFIG_MCUBOOT)
        sps_watchdog_feed();
#endif
    }
    return 0;
}
int sps_image_copy(const struct flash_area *source, const struct flash_area *dest,
                   uint32_t length, uint32_t expected_crc)
{
    uint32_t actual;
    int rc = sps_area_crc(source, length, &actual);
    if (rc || actual != expected_crc) return rc ? rc : -EBADMSG;
    if (length > dest->fa_size - 0x4000) return -EFBIG;
    rc = sps_area_erase(dest);
    if (rc) return rc;
    uint8_t buf[1024];
    for (uint32_t off = 0; off < length;) {
        size_t n = MIN(sizeof(buf), length - off);
        rc = flash_area_read(source, off, buf, n);
        if (rc) return rc;
        rc = flash_area_write(dest, off, buf, n);
        if (rc) return rc;
        off += n;
#if defined(CONFIG_MCUBOOT)
        sps_watchdog_feed();
#endif
    }
    rc = sps_area_crc(dest, length, &actual);
    return rc ? rc : actual == expected_crc ? 0 : -EIO;
}
int sps_backup_verify(const struct sps_boot_state *state)
{
    if (!state->backup_valid) return -ENOENT;
    const struct flash_area *area;
    int rc = flash_area_open(PM_OTA_IMAGE_B_ID, &area);
    if (rc) return rc;
    uint32_t length, crc;
    rc = sps_image_length(area, &length);
    if (!rc && length != state->backup_length) rc = -EBADMSG;
    if (!rc) rc = sps_area_crc(area, length, &crc);
    if (!rc && crc != state->backup_crc) rc = -EBADMSG;
    flash_area_close(area);
    return rc;
}
