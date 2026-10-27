/* Execute the production recovery hook and storage parser against a simulated
 * NOR/journal. Native MCUboot swap/authentication is a mock here; real signing
 * is checked separately with imgtool and board power-cut tests remain required. */
#include <assert.h>
#include <errno.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sps_boot.h"
#include <mock.h>
static uint8_t flash[4][0x100000];
static const struct flash_area areas[] = {
    {0,0x100000,0,NULL}, {1,0x100000,0,NULL},
    {2,0x100000,0,NULL}, {3,0x2000,0,NULL}
};
static struct sps_boot_state durable;
static struct sps_boot_history durable_history;
static bool history_exists, cut_complete;
static bool journal_exists, pending, native_busy, cut_copy, cut_arm, cut_native, fail_journal;
static unsigned staging_erases, watchdog_feeds;
static unsigned block_erases,sector_erases,erase_attempts;
static int erase_error_offset=-1;
static uint32_t cause;
static bool task_watchdog_marker;
static jmp_buf reboot_target;
void sps_watchdog_start(void) {}
void sps_task_watchdog_mark_reset(void) { task_watchdog_marker=true; }
uint32_t sps_watchdog_classify_reset(uint32_t flags) {
    if (task_watchdog_marker && (flags&RESET_SOFTWARE)) flags|=RESET_WATCHDOG;
    task_watchdog_marker=false;
    return flags;
}
void sps_watchdog_feed(void) { ++watchdog_feeds; }
int flash_area_open(unsigned id, const struct flash_area **a) { *a=&areas[id]; return 0; }
void flash_area_close(const struct flash_area *a) { (void)a; }
int flash_area_read(const struct flash_area *a,size_t off,void *buf,size_t n) {
    if (off>a->fa_size || n>a->fa_size-off) return -EINVAL;
    memcpy(buf,flash[a->id]+off,n); return 0;
}
int flash_area_write(const struct flash_area *a,size_t off,const void *buf,size_t n) {
    assert(a->id != 0); // Recovery must never bypass MCUboot to program RRAM.
    if (off>a->fa_size || n>a->fa_size-off) return -EINVAL;
    if (cut_copy && a->id==1) { cut_copy=false; longjmp(reboot_target,2); }
    for(size_t i=0;i<n;i++) {
        uint8_t value=((const uint8_t *)buf)[i];
        assert((flash[a->id][off+i]&value)==value);
        flash[a->id][off+i]=value;
    }
    return 0;
}
int flash_area_erase(const struct flash_area *a,size_t off,size_t n) {
    assert(a->id != 0);
    assert((n==4096 || n==65536) && (a->fa_off+off)%n==0 && off+n<=a->fa_size);
    ++erase_attempts;
    if((int)off==erase_error_offset)return -EIO;
    if(n==65536)++block_erases;else ++sector_erases;
    if(a->id==1) { assert(!native_busy); ++staging_erases; }
    memset(flash[a->id]+off,255,n); return 0;
}
int nvs_mount(struct nvs_fs *fs) { assert(fs->sector_count==2); return 0; }
ssize_t nvs_read(struct nvs_fs *fs,unsigned id,void *out,size_t n) {
    (void)fs; assert(id==1 || id==2);
    if (id==1) {
        if (!journal_exists) return -ENOENT;
        assert(n==sizeof(durable)); memcpy(out,&durable,n);
    } else {
        if (!history_exists) return -ENOENT;
        assert(n==sizeof(durable_history)); memcpy(out,&durable_history,n);
    }
    return n;
}
ssize_t nvs_write(struct nvs_fs *fs,unsigned id,const void *data,size_t n) {
    (void)fs; assert(id==1 || id==2);
    if(fail_journal) return -EIO;
    if (id==2) {
        assert(n==sizeof(durable_history));
        memcpy(&durable_history,data,n); history_exists=true; return n;
    }
    assert(n==sizeof(durable));
    const struct sps_boot_state *s=data;
    if(cut_arm && s->recovery==SPS_RECOVERY_ARMED) {
        cut_arm=false; longjmp(reboot_target,2);
    }
    if(cut_complete && durable.recovery==SPS_RECOVERY_ARMED && s->recovery==SPS_RECOVERY_IDLE) {
        cut_complete=false; longjmp(reboot_target,2);
    }
    memcpy(&durable,data,n); journal_exists=true; return n;
}
uint16_t sys_get_le16(const uint8_t *p) { return p[0]|p[1]<<8; }
uint32_t sys_get_le32(const uint8_t *p) { return p[0]|p[1]<<8|p[2]<<16|(uint32_t)p[3]<<24; }
uint32_t crc32_ieee_update(uint32_t c,const uint8_t *p,size_t n) {
    c=~c; while(n--) { c^=*p++; for(int j=0;j<8;j++) c=(c>>1)^(0xedb88320U&-(c&1)); }
    return ~c;
}
uint32_t crc32_ieee(const uint8_t *p,size_t n) { return crc32_ieee_update(0,p,n); }
int hwinfo_get_reset_cause(uint32_t *out) { *out=cause; return 0; }
int hwinfo_clear_reset_cause(void) { cause=0; return 0; }
_Noreturn void sys_reboot(int type) { (void)type; longjmp(reboot_target,1); }
int boot_set_pending(int permanent) { assert(permanent==1); pending=true; return 0; }
int boot_read_swap_state_by_id(unsigned id,struct boot_swap_state *s) {
    (void)id; s->magic=BOOT_MAGIC_GOOD; s->image_ok=BOOT_FLAG_SET; s->copy_done=BOOT_FLAG_SET; return 0;
}
fih_ret boot_go(struct boot_rsp *rsp) {
    (void)rsp;
    if(pending) {
        if(cut_native) { native_busy=true; cut_native=false; longjmp(reboot_target,2); }
        memcpy(flash[0],flash[1],sizeof(flash[0]));
        pending=false; native_busy=false;
    }
    return FIH_SUCCESS;
}
static void put16(uint8_t *p,uint16_t n) { p[0]=n; p[1]=n>>8; }
static void put32(uint8_t *p,uint32_t n) { for(int i=0;i<4;i++) p[i]=n>>(i*8); }
static void image(unsigned id,uint8_t tag) {
    memset(flash[id],255,sizeof(flash[id]));
    memset(flash[id],0,32);
    put32(flash[id],0x96f3b83d);
    put16(flash[id]+8,0x800); put32(flash[id]+12,32);
    memset(flash[id]+0x800,tag,32);
    put16(flash[id]+0x820,0x6907); put16(flash[id]+0x822,4);
}
static void setup(void) {
    memset(flash,255,sizeof(flash));
    memset(&durable,0,sizeof(durable));
    journal_exists=pending=native_busy=cut_copy=cut_arm=cut_native=fail_journal=false;
    history_exists=cut_complete=task_watchdog_marker=false;
    memset(&durable_history,0,sizeof(durable_history));
    cause=0; staging_erases=0;
    erase_error_offset=-1;block_erases=sector_erases=erase_attempts=0;
    image(0,42); image(2,17);
    struct sps_boot_state s;
    assert(sps_boot_state_load(&s)==0);
    s.backup_valid=1;
    assert(sps_image_length(&areas[2],&s.backup_length)==0);
    assert(sps_area_crc(&areas[2],s.backup_length,&s.backup_crc)==0);
    assert(sps_boot_state_save(&s)==0);
}
static int boot(uint32_t reason) {
    cause=reason;
    int jumped=setjmp(reboot_target);
    if(jumped) return jumped;
    struct boot_rsp rsp;
    assert(boot_go_hook(&rsp)==FIH_SUCCESS);
    return 0;
}
static void trigger_three(void) {
    assert(boot(RESET_WATCHDOG)==0 && durable.failures==1 && flash[0][0x800]==42);
    assert(boot(RESET_WATCHDOG)==0 && durable.failures==2 && staging_erases==0);
    assert(boot(RESET_WATCHDOG)==1 && durable.recovery==SPS_RECOVERY_COPY);
    assert(staging_erases==0); // Native swap/resume must finish first.
}
int main(int argc, char **argv) {
    assert(crc32_ieee((const uint8_t *)"123456789",9)==0xcbf43926U);
    setup(); trigger_three();
    assert(cause==0 && durable.reset_cause==RESET_WATCHDOG);
    assert(boot(0)==0 && durable.failures==0 && durable.recovery==0 && flash[0][0x800]==17);
    puts("PASS: lifetime rollback on third watchdog reset, first two retain current image");
    assert(durable_history.app_boots==3 && durable_history.watchdog_resets==3);
    assert(durable_history.recoveries==1 && durable_history.last_reason==SPS_REASON_WATCHDOG);
    assert(durable_history.image_origin==SPS_ORIGIN_BACKUP_B);
    assert(!boot(0));
    assert(durable_history.recoveries==1 && durable_history.image_origin==SPS_ORIGIN_BACKUP_B);
    puts("PASS: persistent boot/watchdog/recovery counts and recovered image origin survive reboot");
    setup(); trigger_three(); cut_complete=true;
    assert(boot(0)==2 && durable_history.recoveries==1 && durable.recovery==SPS_RECOVERY_ARMED);
    assert(!boot(0) && durable_history.recoveries==1);
    puts("PASS: interrupted recovery completion does not double-count recovery");
    setup(); assert(!boot(0)); image(0,99); assert(!boot(0));
    assert(durable_history.image_origin==SPS_ORIGIN_UPDATE_A);
    puts("PASS: newly observed firmware replaces stale backup-origin label");
    setup();
    sps_task_watchdog_mark_reset(); assert(!boot(RESET_SOFTWARE) && durable.failures==1);
    sps_task_watchdog_mark_reset(); assert(!boot(RESET_SOFTWARE) && durable.failures==2);
    sps_task_watchdog_mark_reset(); assert(boot(RESET_SOFTWARE)==1);
    assert(!boot(0) && flash[0][0x800]==17 && durable_history.watchdog_resets==3);
    assert(durable_history.recoveries==1);
    puts("PASS: three Zephyr task-watchdog software resets trigger the same lifetime recovery");
    setup(); sps_task_watchdog_mark_reset(); assert(!boot(4) && durable.failures==0);
    assert(!boot(RESET_SOFTWARE) && durable.failures==0);
    puts("PASS: stale task marker cannot classify a pin or later normal software reset as watchdog");
    setup(); assert(!boot(RESET_WATCHDOG)); assert(!boot(0)); assert(durable.failures==0);
    assert(!boot(RESET_WATCHDOG)); assert(durable.failures==1);
    puts("PASS: non-watchdog reboot breaks consecutive failure streak");
    setup(); trigger_three(); cut_copy=true;
    assert(boot(0)==2 && durable.recovery==SPS_RECOVERY_COPY);
    assert(flash[0][0x800]==42 && flash[2][0x800]==17);
    assert(boot(0)==0 && flash[0][0x800]==17);
    puts("PASS: interrupted recovery copy restarts from untouched backup");
    setup(); trigger_three(); cut_arm=true;
    assert(boot(0)==2 && pending && durable.recovery==SPS_RECOVERY_COPY);
    assert(boot(0)==0 && flash[0][0x800]==17);
    puts("PASS: interrupted pending/journal commit safely restages before native swap");
    setup(); trigger_three(); cut_native=true;
    assert(boot(0)==2 && durable.recovery==SPS_RECOVERY_ARMED && native_busy);
    unsigned erased=staging_erases;
    assert(boot(0)==0 && staging_erases==erased && flash[0][0x800]==17);
    puts("PASS: interrupted recovery swap resumes without erasing A");
    setup(); assert(!boot(RESET_WATCHDOG)); assert(!boot(RESET_WATCHDOG));
    image(1,99); pending=true; cut_native=true;
    assert(boot(RESET_WATCHDOG)==2 && durable.recovery==SPS_RECOVERY_REQUEST && native_busy);
    assert(boot(0)==1 && !native_busy && staging_erases==0);
    assert(boot(0)==0 && flash[0][0x800]==17);
    puts("PASS: pre-existing interrupted update completes before recovery can repurpose A");
    setup(); flash[2][0x805]^=1;
    assert(!boot(RESET_WATCHDOG)); assert(!boot(RESET_WATCHDOG)); assert(!boot(RESET_WATCHDOG));
    assert(staging_erases==0 && flash[0][0x800]==42);
    puts("PASS: corrupt backup cannot erase running firmware or staging");
    setup(); fail_journal=true; assert(!boot(RESET_WATCHDOG)); assert(staging_erases==0);
    puts("PASS: failed journal disables recovery writes and preserves normal boot");
    setup(); durable.crc^=1; assert(!boot(RESET_WATCHDOG)); assert(staging_erases==0);
    puts("PASS: corrupt journal disables recovery writes");
    setup(); assert(!boot(0)); durable_history.crc ^= 1;
    assert(!boot(RESET_WATCHDOG) && durable.failures==1);
    assert(sps_boot_history_load(&durable_history)==-EBADMSG);
    puts("PASS: corrupt telemetry cannot disable the independent rollback policy");
    setup(); uint32_t length; put32(flash[2]+12,0xfffffff0U);
    assert(sps_image_length(&areas[2],&length)==-EBADMSG);
    image(2,17); put16(flash[2]+0x822,3); assert(sps_image_length(&areas[2],&length)==-EBADMSG);
    puts("PASS: truncated/overflowing image metadata rejected before copy");
    // Block batching must respect exact partition boundaries and feed between
    // requests, including non-block-aligned areas and a 4 KiB tail.
    setup();
    struct flash_area subarea=areas[2];subarea.fa_off=0x100000;subarea.fa_size=0x21000;
    memset(flash[2],0x3c,sizeof(flash[2]));
    unsigned feeds=watchdog_feeds;
    assert(sps_area_erase(&subarea)==0 && block_erases==2 && sector_erases==1 && erase_attempts==3);
    assert(watchdog_feeds-feeds==3);
    for(size_t i=0;i<subarea.fa_size;++i)assert(flash[2][i]==0xff);
    for(size_t i=subarea.fa_size;i<sizeof(flash[2]);++i)assert(flash[2][i]==0x3c);
    subarea.fa_off+=4096;block_erases=sector_erases=erase_attempts=0;
    assert(!sps_area_erase(&subarea) && block_erases==1 && sector_erases==17);
    subarea.fa_size=4097;erase_attempts=0;
    assert(sps_area_erase(&subarea)==-EINVAL && erase_attempts==0);
    subarea.fa_size=0x21000;subarea.fa_off=0x100000;
    erase_error_offset=65536;erase_attempts=0;
    assert(sps_area_erase(&subarea)==-EIO && erase_attempts==2);
    erase_error_offset=-1;
    puts("PASS: 64 KiB erase batching preserves boundaries, sector-aligned edges, watchdog feeds and error propagation");
    if (argc > 1) {
        FILE *file=fopen(argv[1],"rb"); assert(file);
        memset(flash[0],255,sizeof(flash[0]));
        size_t n=fread(flash[0],1,sizeof(flash[0]),file);
        assert(!ferror(file) && feof(file)); fclose(file);
        uint32_t parsed, crc;
        assert(sps_image_length(&areas[0],&parsed)==0 && parsed==n);
        assert(sps_area_crc(&areas[0],parsed,&crc)==0);
        assert(sps_image_copy(&areas[0],&areas[2],parsed,crc)==0);
        assert(!memcmp(flash[0],flash[2],n));
        puts("PASS: production parser and backup copy accept the actual signed build artifact");
    }
    assert(watchdog_feeds>0);
    return 0;
}
