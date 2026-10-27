// Production writer + signature verifier, with NOR/journal APIs simulated in RAM.
#include "ota/firmware_update.hpp"
#include "ota/image_verify.hpp"
#include "sps_boot.h"
#include <zephyr/app_version.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>
#include <psa/crypto.h>
#include <algorithm>
#include <array>
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>
#include <string_view>
static std::array<std::vector<uint8_t>,3> nor;
static std::array<flash_area,3> areas;
static sps_boot_state state{};
static sps_boot_history history{};
static int64_t now;
static int pending;
static std::vector<char> events;
static bool failBackupCopy;
int64_t k_uptime_get() {return now;}
int flash_area_open(int id,const flash_area **a) {*a=&areas[id];return 0;}
void flash_area_close(const flash_area *) {}
int flash_area_read(const flash_area *a,uint32_t pos,void *out,size_t n) {if(pos>a->fa_size||n>a->fa_size-pos)return -1;memcpy(out,a->data+pos,n);return 0;}
int flash_area_write(const flash_area *a,uint32_t pos,const void *bytes,size_t n) {if(pos>a->fa_size||n>a->fa_size-pos)return -1;memcpy(const_cast<uint8_t *>(a->data)+pos,bytes,n);return 0;}
int flash_area_check_int_sha256(const flash_area *a,const flash_area_check *check) {
    std::array<uint8_t,32> h;size_t n;
    if(psa_hash_compute(PSA_ALG_SHA_256,a->data,check->clen,h.data(),h.size(),&n))return -EIO;
    return !memcmp(h.data(),check->match,32)?0:-EIO;
}
int boot_request_upgrade(int permanent) {assert(permanent==1);++pending;return 0;}
int boot_write_img_confirmed() {return 0;}
extern "C" {
int sps_boot_state_load(sps_boot_state *s) {*s=state;return 0;}
int sps_boot_state_save(sps_boot_state *s) {state=*s;return 0;}
int sps_boot_history_load(sps_boot_history *s) {*s=history;return 0;}
int sps_area_crc(const flash_area *a,uint32_t n,uint32_t *crc) {*crc=crc32_ieee_update(0,a->data,n);return 0;}
int sps_image_length(const flash_area *a,uint32_t *n) {
    if(sys_get_le32(a->data)!=0x96f3b83d)return -EBADMSG;
    *n=sys_get_le16(a->data+8)+sys_get_le32(a->data+12)+sys_get_le16(a->data+10);
    if(*n+4>a->fa_size)return -EBADMSG;*n+=sys_get_le16(a->data+*n+2);return 0;
}
int sps_backup_verify(const sps_boot_state *s) {return s->backup_valid && crc32_ieee_update(0,areas[2].data,s->backup_length)==s->backup_crc?0:-EIO;}
int sps_area_erase(const flash_area *a) {events.push_back('A');memset(const_cast<uint8_t *>(a->data),0xff,a->fa_size);return 0;}
int sps_image_copy(const flash_area *a,const flash_area *b,uint32_t n,uint32_t) {events.push_back('B');if(failBackupCopy)return -EIO;memcpy(const_cast<uint8_t *>(b->data),a->data,n);return 0;}
}
int main(int argc,char **argv) {
    assert(argc==2 || argc==3);std::ifstream f(argv[1],std::ios::binary);assert(f);
    std::vector<uint8_t> image{std::istreambuf_iterator<char>(f),{}};
    for(int i=0;i<3;++i) {nor[i].assign(0x100000,0xff);areas[i]={nor[i].data(),nor[i].size()};}
    std::copy(image.begin(),image.end(),nor[0].begin());
    std::vector<uint8_t> oldBackup=image;
    if(argc==3) {
        std::ifstream old(argv[2],std::ios::binary);assert(old);
        oldBackup={std::istreambuf_iterator<char>(old),{}};
    } else oldBackup[0x840]^=1;
    std::copy(oldBackup.begin(),oldBackup.end(),nor[2].begin());
    state.backup_valid=1;state.backup_length=oldBackup.size();
    state.backup_crc=crc32_ieee_update(0,nor[2].data(),oldBackup.size());
    bool trustedBackup=verify_update_signature(&areas[2],oldBackup.size())==0;
    state.failures=2;
    assert(!firmware_update::init());
#if defined(CONFIG_SPS_OTA_HEALTH_CHECK)
    assert(firmware_update::ready_for_update()==-EAGAIN);
    firmware_update::poll_health(true);now=60000;firmware_update::poll_health(true);
    assert(!firmware_update::ready_for_update());
    if(trustedBackup) {
        assert(state.backup_length==oldBackup.size() && !memcmp(nor[2].data(),oldBackup.data(),oldBackup.size()));
        puts("PASS: a valid backup signed by the alternate project key is preserved during health checks");
    } else {
        assert(state.backup_length==image.size() && !memcmp(nor[2].data(),image.data(),image.size()));
        puts("PASS: an intact untrusted/SDK-signed backup is reprovisioned from the healthy project-signed primary");
    }
#else
    assert(!firmware_update::ready_for_update() && !state.healthy && state.failures==2);
    // Disabling the startup gate must not bypass the backup prerequisite for
    // erasing A or clear consecutive watchdog failures at application startup.
    testPsaImportStatus=PSA_ERROR_INSUFFICIENT_MEMORY;
    assert(firmware_update::begin(image.size())==-ENOMEM);
    assert(events.empty() && !pending && state.backup_valid);
    assert(!memcmp(nor[2].data(),oldBackup.data(),oldBackup.size()));
    testPsaImportStatus=PSA_SUCCESS;
    failBackupCopy=true;
    assert(firmware_update::begin(image.size())==-EIO);
    assert(events.size()==1 && events[0]=='B' && !pending);
    failBackupCopy=false;
    assert(!firmware_update::begin(image.size()) && !state.healthy);
    assert(!memcmp(nor[2].data(),image.data(),image.size()));
    firmware_update::abort();
    const auto eventsBefore=events;
    firmware_update::poll_health(false);now=60000;firmware_update::poll_health(false);
    assert(events==eventsBefore && state.failures==0);
    puts("PASS: health gate off accepts OTA immediately; backup failures still prevent erasing A; progress clears watchdog streak independently of settings/backup checks");
#endif
    std::array<char,161> info{};
    int infoLength=firmware_update::format_info(info);
    assert(infoLength>0 && infoLength<=160);
    assert(std::string_view(info.data()).find("FW=" APP_VERSION_TWEAK_STRING)!=std::string_view::npos);
#if !defined(CONFIG_SPS_OTA_HEALTH_CHECK)
    assert(std::string_view(info.data()).find("Health=Off")!=std::string_view::npos);
#endif
    history.app_boots=history.watchdog_resets=history.recoveries=UINT32_MAX;
    history.image_origin=SPS_ORIGIN_BACKUP_B;history.last_reason=SPS_REASON_WATCHDOG;
    state.recovery=SPS_RECOVERY_REQUEST;state.failures=3;state.healthy=0;
    infoLength=firmware_update::format_info(info);
    assert(infoLength>0 && infoLength<=160);
    const std::string_view text(info.data());
    assert(text.find("FW=" APP_VERSION_TWEAK_STRING)!=text.npos && text.find("4294967295")!=text.npos);
    state.recovery=SPS_RECOVERY_IDLE;state.failures=0;state.healthy=1;
    puts("PASS: OTA SMS includes full running version and exact maximum counters within 160 characters");
    auto hash=[](const std::vector<uint8_t> &b) {std::array<uint8_t,32> h{};size_t n;assert(!psa_hash_compute(256,b.data(),b.size(),h.data(),h.size(),&n));return h;};
    auto feed=[](const std::vector<uint8_t> &b) {for(size_t pos=0;pos<b.size();pos+=1024)assert(!firmware_update::write({b.data()+pos,std::min<size_t>(1024,b.size()-pos)}));};
    assert(!firmware_update::begin(image.size()));feed(image);
    const auto firstBackup=std::find(events.begin(),events.end(),'B');
    const auto firstErase=std::find(events.begin(),events.end(),'A');
    assert(firstBackup<firstErase && firstErase!=events.end() && events.back()=='A');
    assert(!memcmp(nor[2].data(),image.data(),image.size()));
    assert(firmware_update::finish()<0 && !pending);firmware_update::abort();
    puts("PASS: production writer archives healthy primary to B before erasing A; missing final hash cannot arm update");
    auto corrupt=image;corrupt.back()^=1;
    assert(!firmware_update::begin(corrupt.size()));feed(corrupt);
    assert(firmware_update::finish(hash(corrupt))<0 && !pending);firmware_update::abort();
    assert(!firmware_update::begin(image.size(),hash(image)));feed(image);nor[1][0x840]^=1;
    assert(firmware_update::finish()<0 && !pending);firmware_update::abort();
    puts("PASS: invalid signature with correct file hash and corrupted flash readback both fail before pending marker");
    assert(!firmware_update::begin(image.size()));feed(image);assert(!firmware_update::finish(hash(image)) && pending==1);
    assert(firmware_update::ready_for_update()==-EBUSY);
    puts("PASS: complete authenticated image arms exactly one permanent MCUboot update");
}
