#ifndef SPS_MOCK_H
#define SPS_MOCK_H
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>
#define MIN(a,b) ((a)<(b)?(a):(b))
#define PM_MCUBOOT_PRIMARY_ID 0
#define PM_MCUBOOT_SECONDARY_ID 1
#define PM_OTA_IMAGE_B_ID 2
#define PM_NVS_STORAGE_ID 3
#define RESET_WATCHDOG 1
#define RESET_SOFTWARE 2
#define SYS_REBOOT_COLD 0
#define LOG_LEVEL_INF 3
#define LOG_MODULE_REGISTER(...)
#define LOG_INF(...)
#define LOG_WRN(...)
#define LOG_ERR(...)
typedef int fih_ret;
#define FIH_SUCCESS 0
#define FIH_FAILURE 1
#define FIH_EQ(a,b) ((a)==(b))
#define FIH_NOT_EQ(a,b) ((a)!=(b))
#define FIH_CALL(f,r,...) ((r)=f(__VA_ARGS__))
#define FIH_RET(r) return (r)
#define BOOT_MAGIC_UNSET 3
#define BOOT_MAGIC_GOOD 1
#define BOOT_FLAG_SET 1
struct boot_rsp { int unused; };
struct boot_swap_state { unsigned magic, image_ok, copy_done; };
struct flash_area { unsigned id; size_t fa_size; uintptr_t fa_off; void *fa_dev; };
struct nvs_fs { void *flash_device; uintptr_t offset; unsigned sector_size, sector_count; };
int flash_area_open(unsigned, const struct flash_area **);
void flash_area_close(const struct flash_area *);
int flash_area_read(const struct flash_area *, size_t, void *, size_t);
int flash_area_write(const struct flash_area *, size_t, const void *, size_t);
int flash_area_erase(const struct flash_area *, size_t, size_t);
int nvs_mount(struct nvs_fs *);
ssize_t nvs_read(struct nvs_fs *, unsigned, void *, size_t);
ssize_t nvs_write(struct nvs_fs *, unsigned, const void *, size_t);
uint16_t sys_get_le16(const uint8_t *);
uint32_t sys_get_le32(const uint8_t *);
uint32_t crc32_ieee(const uint8_t *, size_t);
uint32_t crc32_ieee_update(uint32_t, const uint8_t *, size_t);
int hwinfo_get_reset_cause(uint32_t *);
int hwinfo_clear_reset_cause(void);
_Noreturn void sys_reboot(int);
int boot_set_pending(int);
int boot_read_swap_state_by_id(unsigned, struct boot_swap_state *);
fih_ret boot_go(struct boot_rsp *);
fih_ret boot_go_hook(struct boot_rsp *);
#endif
