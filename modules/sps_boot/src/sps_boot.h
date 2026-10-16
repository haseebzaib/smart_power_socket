#ifndef SPS_BOOT_H
#define SPS_BOOT_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <zephyr/storage/flash_map.h>
#ifdef __cplusplus
extern "C" {
#endif
#define SPS_BOOT_MAGIC 0x53505342U
#define SPS_BOOT_VERSION 1U
#define SPS_WATCHDOG_MS 20000U
/* Testing: one minute. Use 300000U for a five-minute production trial. */
#define SPS_HEALTHY_MS 60000U
#define SPS_WATCHDOG_LIMIT 3U
#define SPS_RECOVERY_IDLE 0U
#define SPS_RECOVERY_REQUEST 1U
#define SPS_RECOVERY_COPY 2U
#define SPS_RECOVERY_ARMED 3U
/* Fixed-width on-flash ABI, shared by MCUboot and every application release. */
struct sps_boot_state {
    uint32_t magic;
    uint32_t version;
    uint32_t failures;
    uint32_t healthy;
    uint32_t backup_valid;
    uint32_t backup_length;
    uint32_t backup_crc;
    uint32_t recovery;
    uint32_t reset_cause;
    uint32_t crc;
};
/* Separate NVS record: keep the original boot-policy ABI unchanged. */
#define SPS_HISTORY_MAGIC 0x53505348U
#define SPS_REASON_NONE 0U
#define SPS_REASON_WATCHDOG 1U
#define SPS_REASON_IMAGE 2U
#define SPS_ORIGIN_UNKNOWN 0U
#define SPS_ORIGIN_FACTORY 1U
#define SPS_ORIGIN_UPDATE_A 2U
#define SPS_ORIGIN_BACKUP_B 3U
struct sps_boot_history {
    uint32_t magic;
    uint32_t version;
    uint32_t app_boots;
    uint32_t watchdog_resets;
    uint32_t recoveries;
    uint32_t last_reason;
    uint32_t pending_reason;
    uint32_t recovery_recorded;
    uint32_t image_origin;
    uint32_t image_length;
    uint32_t image_crc;
    uint32_t crc;
};
int sps_boot_history_load(struct sps_boot_history *history);
int sps_boot_history_save(struct sps_boot_history *history);
void sps_task_watchdog_mark_reset(void);
uint32_t sps_watchdog_classify_reset(uint32_t cause);
void sps_watchdog_start(void);
void sps_watchdog_feed(void);
int sps_boot_state_load(struct sps_boot_state *state);
int sps_boot_state_save(struct sps_boot_state *state);
int sps_image_length(const struct flash_area *area, uint32_t *length);
int sps_area_crc(const struct flash_area *area, uint32_t length, uint32_t *crc);
int sps_area_erase(const struct flash_area *area);
int sps_image_copy(const struct flash_area *source, const struct flash_area *dest,
                   uint32_t length, uint32_t expected_crc);
int sps_backup_verify(const struct sps_boot_state *state);
#ifdef __cplusplus
}
#endif
#endif
