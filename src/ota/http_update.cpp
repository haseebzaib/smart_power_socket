#include "http_update.hpp"
#include "firmware_update.hpp"
#include "manifest.hpp"
#include "cellular/sim7080.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <psa/crypto.h>
#include <zephyr/app_version.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/reboot.h>
#include <pm_config.h>
LOG_MODULE_REGISTER(http_ota, LOG_LEVEL_INF);
namespace {
using modem_t=cellular::sim7080;
enum class phase { idle, accepted, prepare, manifest_wait, manifest_read, image_start, image_wait,
                   stage_prepare, copy, verify, notify_ready, ready_wait, notify_failure };
phase state=phase::idle;
std::array<char,32> recipient{}, release{};
std::array<char,160> failure{};
std::array<char,256> url{};
std::array<char,4097> manifest{};
std::array<uint8_t,1024> chunk{};
ota_manifest::version requested;
ota_manifest::image metadata;
psa_hash_operation_t hash=PSA_HASH_OPERATION_INIT;
bool hashActive=false, writerActive=false;
uint32_t offset=0, downloadedBytes=0, lastReceived=0;
unsigned pollFailures=0;
int64_t lastProgress=0, idleSince=-1, nextProgressLog=0;
constexpr int64_t stalledTimeoutMs=180000, completionGraceMs=10000;
constexpr unsigned maxPollFailures=3;
int64_t deadline=0, nextPoll=0;
uint64_t notificationId=0;
struct progress_notices {
    uint64_t queuedId=0;
    int64_t lastQueued=0;
    unsigned downloadPercent=0, storagePercent=0;
    std::array<char,96> pendingPhase{};
} notices;
constexpr int64_t progressIntervalMs=60000, checkpointIntervalMs=30000;

bool queue_progress(modem_t &modem, const char *detail)
{
    // One optional notice at a time. Leave five free slots after inserting it
    // for command replies and the essential OTA completion/error reply.
    if (modem.sms_pending(notices.queuedId) || !modem.can_queue_sms(6)) return false;
    char message[160];
    int n=std::snprintf(message,sizeof(message),"OTA %s: %s",release.data(),detail);
    if (n<0 || static_cast<size_t>(n)>=sizeof(message) || !modem.send_sms(recipient.data(),message)) return false;
    notices.queuedId=modem.last_sms_id();
    notices.lastQueued=k_uptime_get();
    return true;
}
void flush_progress(modem_t &modem)
{
    if (notices.pendingPhase[0] && queue_progress(modem,notices.pendingPhase.data()))
        notices.pendingPhase[0]=0;
}
void announce_progress(modem_t &modem, const char *detail)
{
    // Supersede an unsent older stage, keeping the latest stage when busy.
    modem.cancel_queued_sms(notices.queuedId);
    notices.queuedId=0;
    std::snprintf(notices.pendingPhase.data(),notices.pendingPhase.size(),"%s",detail);
    flush_progress(modem);
}
void stop_progress(modem_t &modem)
{
    modem.cancel_queued_sms(notices.queuedId);
    notices.queuedId=0;
    notices.pendingPhase[0]=0;
}
void periodic_progress(modem_t &modem, const char *detail, unsigned percent, unsigned &lastPercent)
{
    if (notices.pendingPhase[0]) return;
    const int64_t elapsed=k_uptime_get()-notices.lastQueued;
    const bool checkpoint=percent/25>lastPercent/25 && elapsed>=checkpointIntervalMs;
    if (elapsed<progressIntervalMs && !checkpoint) return;
    char text[96];
    if (state==phase::manifest_wait) std::snprintf(text,sizeof(text),"Checking release information...");
    else std::snprintf(text,sizeof(text),"%s: %u%%",detail,percent);
    if (queue_progress(modem,text)) lastPercent=percent;
}
constexpr std::string_view metaFile="sps-ota.json", imageFile="sps-ota.bin";
constexpr std::string_view base=CONFIG_SPS_OTA_BASE_URL;
constexpr ota_manifest::target target{"smart_power_socket","bl54l15_dvk","nrf54l15",PM_APP_ADDRESS,PM_MCUBOOT_SECONDARY_SIZE-0x4000};
void fail(modem_t &modem,const char *step,int error)
{
    stop_progress(modem);
    LOG_ERR("OTA failed at %s: %d; current application retained",step,error);
    if (writerActive) firmware_update::abort();
    writerActive=false;
    if(hashActive) psa_hash_abort(&hash);
    hashActive=false;
    modem.https_cleanup(true);
    std::snprintf(failure.data(),failure.size(),"ERR OTA %s: %s (%d)",release.data(),step,error);
    state=phase::notify_failure;
}
bool make_url(std::string_view file)
{
    int n=std::snprintf(url.data(),url.size(),"%.*s%s%.*s",static_cast<int>(base.size()),base.data(),
        base.ends_with('/')?"":"/",static_cast<int>(file.size()),file.data());
    return n>0 && static_cast<size_t>(n)<url.size();
}
bool queue(modem_t &modem,std::string_view message)
{
    if (!modem.send_sms(recipient.data(),message)) return false;
    notificationId=modem.last_sms_id();
    return true;
}
int begin_fetch(modem_t &modem,std::string_view file,std::string_view local)
{
    if (!make_url(file)) return -ENAMETOOLONG;
    const uint32_t limit=local==metaFile?static_cast<uint32_t>(manifest.size()-1):metadata.size;
    int rc=modem.https_start(url.data(),local,limit);
    if (!rc) {
        const int64_t now=k_uptime_get();
        deadline=now+30LL*60LL*1000LL;
        lastProgress=nextProgressLog=now;
        nextPoll=pollFailures=lastReceived=downloadedBytes=0;
        idleSince=-1;
    }
    return rc;
}
int read_block(modem_t &modem)
{
    const size_t count=std::min<size_t>(chunk.size(),metadata.size-offset);
    int rc=modem.file_read(imageFile,offset,{chunk.data(),count});
    if (!rc && offset==0 && !ota_manifest::header_matches({chunk.data(),count},metadata)) rc=-ENOEXEC;
    if (!rc && psa_hash_update(&hash,chunk.data(),count)!=PSA_SUCCESS) rc=-EIO;
    if (!rc) rc=firmware_update::write({chunk.data(),count});
    if (!rc) offset+=count;
    return rc;
}
}
bool http_update::busy() { return state!=phase::idle; }
const char *http_update::phase_name()
{
    switch(state) {
    case phase::idle:return "Idle";
    case phase::accepted:return "Checking";
    case phase::prepare:case phase::manifest_wait:case phase::manifest_read:return "Manifest";
    case phase::image_start:case phase::image_wait:return "Download";
    case phase::stage_prepare:case phase::copy:return "Stage";
    case phase::verify:return "Verify";
    case phase::notify_ready:case phase::ready_wait:return "Pending";
    case phase::notify_failure:return "Failed";
    }
    return "Unknown";
}
const char *http_update::request_error(int code)
{
    switch(code) {
    case -EINVAL:return "ERR OTA use ota=1.0.1 or ota=1.0.1+0";
    case -ENOTCONN:return "ERR OTA URL not configured";
    case -EADDRNOTAVAIL:return "ERR OTA URL configuration invalid";
    case -EAGAIN:return "ERR OTA not ready; wait for Health=OK in getotainfo";
    case -EBUSY:return "ERR OTA another update is active or pending";
    case -EALREADY:return "ERR OTA requested version is already running";
    default:return "ERR OTA could not queue request";
    }
}
int http_update::request(modem_t &modem,std::string_view sender,std::string_view version)
{
    if (busy()) return -EBUSY;
    if (base.empty()) return -ENOTCONN;
    if (!ota_manifest::valid_base_url(base)) return -EADDRNOTAVAIL;
    if (!ota_manifest::parse_version(version,requested) || sender.empty() || sender.size()>=recipient.size()) return -EINVAL;
    ota_manifest::version current;
    if (ota_manifest::parse_version(APP_VERSION_TWEAK_STRING,current) && current==requested) return -EALREADY;
    int rc=firmware_update::ready_for_update();
    if (rc) return rc;
    recipient.fill(0);std::copy(sender.begin(),sender.end(),recipient.begin());
    ota_manifest::format_version(requested,release);
    char ack[100];std::snprintf(ack,sizeof(ack),"OTA %s accepted; checking manifest",release.data());
    if (!queue(modem,ack)) return -ENOBUFS;
    notices={};notices.lastQueued=k_uptime_get();
    state=phase::accepted;deadline=k_uptime_get()+300000;
    LOG_INF("OTA %s requested; current image %s",release.data(),APP_VERSION_TWEAK_STRING);
    return 0;
}
void http_update::process(modem_t &modem)
{
    if (state!=phase::idle && state!=phase::accepted && state!=phase::notify_ready &&
        state!=phase::ready_wait && state!=phase::notify_failure) flush_progress(modem);
    int rc=0;
    switch(state) {
    case phase::idle:return;
    case phase::accepted:
        if (modem.sms_pending(notificationId)) {
            if(k_uptime_get()>deadline) fail(modem,"acceptance SMS not submitted",-ETIMEDOUT);
            return;
        }
        notices.lastQueued=k_uptime_get();
        state=phase::prepare;return;
    case phase::prepare: {
        auto slash=base.find('/',8);
        rc=modem.https_prepare(base.substr(8,slash-8));
        if(!rc) rc=begin_fetch(modem,"manifest.json",metaFile);
        if(rc) {fail(modem,"manifest HTTPS setup",rc);return;}
        state=phase::manifest_wait;return;
    }
    case phase::manifest_wait:case phase::image_wait: {
        if(k_uptime_get()>deadline) {fail(modem,"download timeout",-ETIMEDOUT);return;}
        if(k_uptime_get()<nextPoll) return;
        nextPoll=k_uptime_get()+2000;
        modem_t::http_result status;
        rc=modem.https_poll(status);
        if(rc) {
            // Retry only a read-only status query. Never blindly replay the
            // request that starts a download, nor a partially read binary chunk.
            if ((rc==-EIO || rc==-ETIMEDOUT || rc==-EAGAIN) && ++pollFailures<maxPollFailures) {
                LOG_WRN("HTTPS status query failed: %d; retry %u/%u",rc,pollFailures,maxPollFailures);
                nextPoll=k_uptime_get()+5000;
                return;
            }
            fail(modem,"download status",rc);return;
        }
        pollFailures=0;
        const int64_t now=k_uptime_get();
        const uint32_t limit=state==phase::manifest_wait?static_cast<uint32_t>(manifest.size()-1):metadata.size;
        if(status.received>limit || status.total>limit) {fail(modem,"download exceeds size limit",-EFBIG);return;}
        if(!status.finished) {
            // HTTPTOFS can retry from byte zero. Compare with the previous
            // sample so useful progress after a restart is not called a stall.
            if(status.received>lastReceived) lastProgress=now;
            lastReceived=status.received;
            if(now>=nextProgressLog) {
                LOG_INF("HTTPS %s: %u/%u bytes, modem %s",state==phase::manifest_wait?"manifest":"firmware",
                    status.received,status.total,status.downloading?"downloading":"idle; awaiting HTTP result");
                nextProgressLog=now+30000;
            }
            if(status.downloading) idleSince=-1;
            else if(idleSince<0) idleSince=now;
            if(idleSince>=0 && now-idleSince>=completionGraceMs) {
                fail(modem,"HTTP completion missing",-ENOMSG);return;
            }
            if(now-lastProgress>=stalledTimeoutMs) {fail(modem,"download stalled",-ETIMEDOUT);return;}
            if (state==phase::manifest_wait) {
                periodic_progress(modem,"",0,notices.downloadPercent);
            } else {
                const unsigned percent=static_cast<unsigned>(std::min<uint64_t>(99,100ULL*status.received/metadata.size));
                const char *detail=!status.downloading?"Waiting for download confirmation":
                    now-lastProgress>=progressIntervalMs?"Waiting for download data":"Downloading firmware";
                periodic_progress(modem,detail,percent,notices.downloadPercent);
            }
            return;
        }
        LOG_INF("HTTPS %s completed: HTTP %d, %u bytes",
            state==phase::manifest_wait?"manifest":"firmware",status.code,status.bytes);
        if(status.code!=200) {fail(modem,"HTTP download status",status.code);return;}
        uint32_t expected=state==phase::manifest_wait?0:metadata.size;
        if(!status.bytes || (expected && status.bytes!=expected) || (!expected && status.bytes>=manifest.size())) {
            fail(modem,"download length",-EMSGSIZE);return;
        }
        downloadedBytes=status.bytes;
        if (state==phase::image_wait) announce_progress(modem,"Download complete; saving firmware...");
        state=state==phase::manifest_wait?phase::manifest_read:phase::stage_prepare;
        return;
    }
    case phase::manifest_read: {
        uint32_t size;
        rc=modem.file_open();
        if(!rc) rc=modem.file_size(metaFile,size);
        if(rc || !size || size>=manifest.size() || size!=downloadedBytes) {
            fail(modem,"manifest file size",rc?rc:-EMSGSIZE);return;
        }
        manifest.fill(0);
        for(uint32_t pos=0;pos<size;pos+=chunk.size()) {
            size_t count=std::min<size_t>(chunk.size(),size-pos);
            rc=modem.file_read(metaFile,pos,{reinterpret_cast<uint8_t *>(manifest.data()+pos),count});
            if(rc) break;
        }
        if(!rc) rc=ota_manifest::parse({manifest.data(),size},target,requested,metadata);
        if(!rc) rc=modem.file_close();
        if(rc) {fail(modem,rc==-ESTALE?"manifest version mismatch":"manifest validation",rc);return;}
        LOG_INF("Manifest accepted: %s, version %s, %u bytes",metadata.file.data(),release.data(),metadata.size);
        state=phase::image_start;return;
    }
    case phase::image_start:
        rc=begin_fetch(modem,metadata.file.data(),imageFile);
        if(rc) {fail(modem,"firmware HTTPS setup",rc);return;}
        announce_progress(modem,"Downloading firmware...");
        state=phase::image_wait;return;
    case phase::stage_prepare: {
        uint32_t size;
        rc=modem.file_open();
        if(!rc) rc=modem.file_size(imageFile,size);
        if(rc || size!=metadata.size) {fail(modem,"firmware file size",rc?rc:-EMSGSIZE);return;}
        size_t count=std::min<size_t>(chunk.size(),metadata.size);
        rc=modem.file_read(imageFile,0,{chunk.data(),count});
        if(!rc && !ota_manifest::header_matches({chunk.data(),count},metadata)) rc=-ENOEXEC;
        if(rc) {fail(modem,"firmware version/header mismatch",rc);return;}
        hash=PSA_HASH_OPERATION_INIT;
        if(psa_hash_setup(&hash,PSA_ALG_SHA_256)!=PSA_SUCCESS) {fail(modem,"SHA-256 setup",-EIO);return;}
        hashActive=true;
        rc=metadata.has_sha256?firmware_update::begin(metadata.size,metadata.sha256):firmware_update::begin(metadata.size);
        if(rc) {fail(modem,"prepare staging A",rc);return;}
        writerActive=true;offset=0;
        state=phase::copy;
        return;
    }
    case phase::copy: {
        int64_t started=k_uptime_get();
        unsigned oldPercent=static_cast<unsigned>(100ULL*offset/metadata.size);
        for(unsigned packets=0;offset<metadata.size && packets<8 && k_uptime_get()-started<1000;++packets) {
            rc=read_block(modem);
            if(rc) {fail(modem,"binary transfer to staging A",rc);return;}
        }
        unsigned percent=static_cast<unsigned>(100ULL*offset/metadata.size);
        if(percent/10!=oldPercent/10) LOG_INF("OTA staging: %u%% (%u/%u bytes)",percent,offset,metadata.size);
        if(offset==metadata.size) {
            announce_progress(modem,"Firmware saved; checking signature...");
            state=phase::verify;
        } else periodic_progress(modem,"Saving firmware",percent,notices.storagePercent);
        return;
    }
    case phase::verify: {
        std::array<uint8_t,32> digest{};size_t count;
        if(psa_hash_finish(&hash,digest.data(),digest.size(),&count)!=PSA_SUCCESS || count!=digest.size()) {
            fail(modem,"SHA-256 finish",-EIO);return;
        }
        hashActive=false;
        // Finish checks readback/hash and the Ed25519 signature before arming.
        rc=firmware_update::finish(digest);
        if(rc) {fail(modem,"firmware verification",rc);return;}
        writerActive=false;
        modem.https_cleanup(false);
        // An unsent optional update must not hold up the final reply/reboot.
        stop_progress(modem);
        state=phase::notify_ready;
        return;
    }
    case phase::notify_ready: {
        char ready[120];std::snprintf(ready,sizeof(ready),"OTA %s verified; rebooting to install",release.data());
        if(queue(modem,ready)) state=phase::ready_wait;
        return;
    }
    case phase::ready_wait:
        // A queued SMS is not yet submitted. Wait for this specific message,
        // letting the shared outbox retry normally; no blind timed reboot.
        if(!modem.sms_pending(notificationId)) {
            LOG_INF("OTA completion SMS accepted; rebooting for installation");
            sys_reboot(SYS_REBOOT_COLD);
        }
        return;
    case phase::notify_failure:
        if(queue(modem,failure.data())) state=phase::idle;
        return;
    }
}
