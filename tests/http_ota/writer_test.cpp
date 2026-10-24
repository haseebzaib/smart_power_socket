// Production writer + signature verifier, with NOR/journal APIs simulated in RAM.
#include "ota/firmware_update.hpp"
#include "sps_boot.h"
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>
#include <psa/crypto.h>
#include <array>
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>
static std::array<std::vector<uint8_t>,3> nor;
static std::array<flash_area,3> areas;
static sps_boot_state state{};
static int64_t now;
static int pending;
static std::vector<char> events;
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
int sps_boot_history_load(sps_boot_history *s) {*s={};return 0;}
int sps_area_crc(const flash_area *a,uint32_t n,uint32_t *crc) {*crc=crc32_ieee_update(0,a->data,n);return 0;}
int sps_image_length(const flash_area *a,uint32_t *n) {
    if(sys_get_le32(a->data)!=0x96f3b83d)return -EBADMSG;
    *n=sys_get_le16(a->data+8)+sys_get_le32(a->data+12)+sys_get_le16(a->data+10);
    if(*n+4>a->fa_size)return -EBADMSG;*n+=sys_get_le16(a->data+*n+2);return 0;
}
int sps_backup_verify(const sps_boot_state *s) {return s->backup_valid && crc32_ieee_update(0,areas[2].data,s->backup_length)==s->backup_crc?0:-EIO;}
int sps_area_erase(const flash_area *a) {events.push_back('A');memset(const_cast<uint8_t *>(a->data),0xff,a->fa_size);return 0;}
int sps_image_copy(const flash_area *a,const flash_area *b,uint32_t n,uint32_t) {events.push_back('B');memcpy(const_cast<uint8_t *>(b->data),a->data,n);return 0;}
}
int main(int argc,char **argv) {
    assert(argc==2);std::ifstream f(argv[1],std::ios::binary);assert(f);
    std::vector<uint8_t> image{std::istreambuf_iterator<char>(f),{}};
    for(int i=0;i<3;++i) {nor[i].assign(0x100000,0xff);areas[i]={nor[i].data(),nor[i].size()};}
    std::copy(image.begin(),image.end(),nor[0].begin());
    // Simulate a distinct recorded B so begin() must archive the healthy primary.
    std::copy(image.begin(),image.end(),nor[2].begin());nor[2][0x840]^=1;
    state.backup_valid=1;state.backup_length=image.size();state.backup_crc=crc32_ieee_update(0,nor[2].data(),image.size());
    assert(!firmware_update::init());assert(firmware_update::ready_for_update()==-EAGAIN);
    firmware_update::poll_health(true);now=60000;firmware_update::poll_health(true);
    assert(!firmware_update::ready_for_update());
    auto hash=[](const std::vector<uint8_t> &b) {std::array<uint8_t,32> h{};size_t n;assert(!psa_hash_compute(256,b.data(),b.size(),h.data(),h.size(),&n));return h;};
    auto feed=[](const std::vector<uint8_t> &b) {for(size_t pos=0;pos<b.size();pos+=1024)assert(!firmware_update::write({b.data()+pos,std::min<size_t>(1024,b.size()-pos)}));};
    assert(!firmware_update::begin(image.size()));feed(image);
    assert(events.size()==2 && events[0]=='B' && events[1]=='A');
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
