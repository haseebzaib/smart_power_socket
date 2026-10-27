#pragma once
#include <algorithm>
#include <cstdint>
#include <cerrno>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <zephyr/kernel.h>
void advance_file_read_time(int64_t ms);
namespace cellular {
class sim7080 {
public:
    struct http_result {int code=200;uint32_t bytes=0,received=0,total=0;bool finished=true;bool downloading=false;};
    struct message {uint64_t id;std::string text;bool pending=true;int64_t queuedAt=0;bool cancelled=false;};
    std::vector<message> messages;
    std::vector<std::string> urls;
    std::vector<http_result> statusScript;
    size_t statusCursor=0;
    std::string manifest,image;
    int httpCode=200,readError=0;
    bool full=false, stalled=false, missingCompletion=false;
    unsigned statusFailures=0;
    unsigned prepareWaits=0,imagePrepareWaits=0;
    int prepareWaitError=-EAGAIN;
    uint32_t advertisedExtra=0, progressSize=0;
    int64_t fileReadDelayMs=0;
    uint64_t next=1;
    std::string active;
    bool send_sms(std::string_view,std::string_view body) {if(!can_queue_sms())return false;messages.push_back({next++,std::string(body),true,k_uptime_get(),false});return true;}
    bool can_queue_sms(size_t count=1) const {
        const auto pending=std::count_if(messages.begin(),messages.end(),[](auto &m) {return m.pending;});
        return !full && count<=24-static_cast<size_t>(pending);
    }
    bool cancel_queued_sms(uint64_t id) {
        for(auto &m:messages)if(m.id==id && m.pending) {m.pending=false;m.cancelled=true;return true;}
        return false;
    }
    uint64_t last_sms_id() const {return next-1;}
    bool sms_pending(uint64_t id) const {for(auto &m:messages)if(m.id==id)return m.pending;return false;}
    int https_prepare(std::string_view host) {
        if(prepareWaits) {--prepareWaits;return prepareWaitError;}
        if(active=="sps-ota.json" && imagePrepareWaits) {--imagePrepareWaits;return prepareWaitError;}
        return host=="raw.githubusercontent.com"?0:-1;
    }
    int https_start(std::string_view url,std::string_view name,uint32_t) {urls.emplace_back(url);active=name;return 0;}
    int https_poll(http_result &r) {
        if(statusFailures) {--statusFailures;return -5;}
        if(statusCursor<statusScript.size()) {r=statusScript[statusCursor++];return 0;}
        const auto size=static_cast<uint32_t>(active=="sps-ota.json"?manifest.size():image.size());
        r={httpCode,size+advertisedExtra,0,0,true,false};
        if(stalled || missingCompletion || progressSize) r={0,0,0,progressSize,false,!missingCompletion};
        return 0;
    }
    void https_cleanup(bool) {}
    int file_open() {return 0;}
    int file_close() {return 0;}
    int file_size(std::string_view name,uint32_t &n) {n=(name=="sps-ota.json"?manifest:image).size();return 0;}
    int file_read(std::string_view name,uint32_t off,std::span<uint8_t> out) {
        advance_file_read_time(fileReadDelayMs);
        if(readError)return readError;const auto &bytes=name=="sps-ota.json"?manifest:image;
        if(off>bytes.size()||out.size()>bytes.size()-off)return -1;
        std::copy_n(bytes.data()+off,out.size(),out.data());return 0;
    }
    void submit(uint64_t id) {for(auto &m:messages)if(m.id==id)m.pending=false;}
};
}
