#include "ota/http_update.hpp"
#include "ota/firmware_update.hpp"
#include "cellular/sim7080.hpp"
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
static int64_t now;
static int begins,aborts,reboots,finishError;
static bool staged;
static size_t expected;
static std::vector<uint8_t> written;
int64_t k_uptime_get() {return now;}
void sys_reboot(int) {++reboots;}
namespace firmware_update {
int ready_for_update() {return 0;}
int begin(size_t size) {++begins;expected=size;written.clear();staged=true;return 0;}
int begin(size_t size,std::span<const uint8_t,32>) {return begin(size);}
int write(std::span<const uint8_t> bytes) {assert(staged);written.insert(written.end(),bytes.begin(),bytes.end());return 0;}
void abort() {++aborts;staged=false;}
int finish(std::span<const uint8_t,32>) {assert(written.size()==expected);if(finishError)return finishError;staged=false;return 0;}
}
static bool optional(const cellular::sim7080::message &message) {return message.text.starts_with("OTA 1.0.1+0:");}
static void tick(cellular::sim7080 &m) {
    now+=1000;http_update::process(m);
    assert(std::count_if(m.messages.begin(),m.messages.end(),[](auto &message) {return message.pending && optional(message);})<=1);
}
static void settle(cellular::sim7080 &m) {
    for(int i=0;i<1000 && http_update::busy();++i)tick(m);
    assert(!http_update::busy() && !reboots);
}
int main(int argc,char **argv) {
    assert(argc==3);
    auto read=[](const char *p) {std::ifstream f(p,std::ios::binary);assert(f);return std::string(std::istreambuf_iterator<char>(f),{});};
    cellular::sim7080 initial;
    initial.manifest=read(argv[1]);initial.image=read(argv[2]);
    auto release=[](std::string text,const char *value) {
        auto field=text.find("version_MCUBOOT");assert(field!=text.npos);
        auto start=text.find('"',text.find(':',field))+1;auto end=text.find('"',start);
        text.replace(start,end-start,value);return text;
    };
    initial.manifest=release(initial.manifest,"1.0.1+0");
    initial.image[20]=1;initial.image[21]=0;initial.image[22]=1;
    for(int i=23;i<28;++i)initial.image[i]=0;
    cellular::sim7080 m=initial;
    assert(http_update::request(m,"123","1.0.0")==-EALREADY);
    assert(http_update::request(m,"123","1")==-EINVAL);
    auto beginRequest=[&] {
        assert(!http_update::request(m,"123","1.0.1"));
        assert(http_update::request(m,"123","1.0.2")==-EBUSY);
        auto count=m.urls.size();tick(m);assert(m.urls.size()==count);
        m.submit(m.last_sms_id());
    };
    // Requested version differs from the manifest: no image request or erase.
    m.manifest=release(initial.manifest,"1.0.2+0");beginRequest();settle(m);assert(begins==0 && m.urls.size()==1);
    // HTTP failures do not erase A or restart the application.
    m=initial;m.httpCode=404;beginRequest();settle(m);assert(begins==0);
    // Manifest/image header mismatch is rejected before starting the writer.
    m=initial;m.image[22]=0;beginRequest();settle(m);assert(begins==0);
    // A status read failure is retried; a permanent failure remains bounded.
    m=initial;m.statusFailures=3;beginRequest();settle(m);assert(begins==0);
    m=initial;m.advertisedExtra=1;beginRequest();settle(m);assert(begins==0);
    assert(m.messages.back().text.find("manifest file size")!=std::string::npos);
    m=initial;m.progressSize=5000;beginRequest();settle(m);assert(begins==0);
    m=initial;m.stalled=true;beginRequest();auto started=now;settle(m);assert(begins==0 && now-started<200000);
    assert(m.messages.back().text.find("download stalled")!=std::string::npos);
    m=initial;m.missingCompletion=true;beginRequest();started=now;settle(m);assert(begins==0 && now-started<30000);
    assert(m.messages.back().text.find("HTTP completion missing")!=std::string::npos);
    puts("PASS: permanent query failures, size discrepancies, stalled downloads and missing completion have bounded failure paths");
    // A native retry can restart its byte counter without stalling the job.
    m=initial;m.manifest=release(initial.manifest,"1.0.2+0");
    const auto manifestSize=static_cast<uint32_t>(m.manifest.size());assert(manifestSize>200);
    m.statusScript.push_back({0,0,200,manifestSize,false,true});
    for(unsigned i=1;i<=100;++i)m.statusScript.push_back({0,0,i,manifestSize,false,true});
    beginRequest();settle(m);assert(begins==0);
    assert(m.messages.back().text.find("manifest version mismatch")!=std::string::npos);
    puts("PASS: advancing byte counts after a native retry avoid a false stall");
    // The accepted job waits through native-engine recovery/re-registration;
    // it must not fail immediately with setup busy or replay the HTTP start.
    for(int error:{-EAGAIN,-ENETDOWN,-ENETUNREACH}) {
        m=initial;m.prepareWaits=5;m.prepareWaitError=error;
        m.manifest=release(initial.manifest,"1.0.2+0");
        beginRequest();settle(m);
        assert(begins==0 && m.urls.size()==1);
        assert(m.messages.back().text.find("manifest version mismatch")!=std::string::npos);
        assert(std::count_if(m.messages.begin(),m.messages.end(),[](auto &message) {
            return message.text.find("Recovering modem connection")!=std::string::npos;
        })==1);
    }
    m=initial;m.imagePrepareWaits=5;m.image[22]=0;
    beginRequest();settle(m);assert(begins==0 && m.urls.size()==2);
    m=initial;m.prepareWaits=10000;
    beginRequest();started=now;settle(m);
    assert(begins==0 && m.urls.empty() && now-started<190000);
    assert(m.messages.back().text.find("modem/network recovery timeout")!=std::string::npos);
    puts("PASS: manifest/image setup waits for modem recovery without replaying downloads; unavailable networks have a bounded deadline");
    // A complete transfer that fails signature/readback verification is aborted.
    m=initial;finishError=-EACCES;beginRequest();settle(m);assert(begins==1 && aborts==1 && !staged);
    puts("PASS: controller rejects version/header/HTTP errors before erasing A; verification failure aborts without reboot");
    // Slow download with a temporary data pause: milestones and heartbeat are
    // delivered without misleading 100% before the final HTTP result.
    m=initial;finishError=-EACCES;
    m.statusScript.push_back({200,static_cast<uint32_t>(m.manifest.size()),0,0,true,false});
    const auto imageSize=static_cast<uint32_t>(m.image.size());
    for(unsigned percent=0;percent<=100;++percent) {
        auto received=static_cast<uint32_t>(static_cast<uint64_t>(imageSize)*percent/100);
        m.statusScript.push_back({0,0,received,imageSize,false,true});
        if(percent==25)for(unsigned pause=0;pause<40;++pause)m.statusScript.push_back({0,0,received,imageSize,false,true});
    }
    for(unsigned hold=0;hold<12;++hold)m.statusScript.push_back({0,0,imageSize,imageSize,false,true});
    beginRequest();
    for(int i=0;i<1500 && http_update::busy();++i) {
        for(auto &message:m.messages)if(optional(message) && message.pending)m.submit(message.id);
        tick(m);
    }
    assert(!http_update::busy() && !reboots);
    int64_t lastPercentAt=-1;unsigned percentMessages=0;bool download=false,waiting=false,saving=false,checking=false;
    for(const auto &message:m.messages) {
        assert(message.text.size()<=160);
        download|=message.text.find("Downloading firmware...")!=std::string::npos;
        waiting|=message.text.find("Waiting for download data")!=std::string::npos;
        saving|=message.text.find("Download complete; saving firmware")!=std::string::npos;
        checking|=message.text.find("Firmware saved; checking signature")!=std::string::npos;
        if(message.text.find('%')!=std::string::npos) {
            assert(message.text.find("100%") == std::string::npos);
            if(lastPercentAt>=0)assert(message.queuedAt-lastPercentAt>=30000);
            lastPercentAt=message.queuedAt;++percentMessages;
        }
    }
    assert(download && waiting && saving && checking && percentMessages>=3 && percentMessages<=12);
    assert(m.messages.back().text.starts_with("ERR OTA"));
    puts("PASS: short phase/progress/waiting SMS notices are rate-limited and never claim premature completion");

    // A full command-reply backlog suppresses optional notices but preserves
    // room for the error response; none of those other replies is cancelled.
    m=initial;m.stalled=true;beginRequest();
    for(unsigned i=0;i<20;++i)assert(m.send_sms("456","other command reply"));
    settle(m);
    for(const auto &message:m.messages) {
        assert(!optional(message));
        if(message.text=="other command reply")assert(message.pending && !message.cancelled);
    }
    assert(m.messages.back().text.starts_with("ERR OTA"));
    puts("PASS: optional OTA notices cannot exhaust reply capacity or cancel unrelated messages");

    // Successful request waits for its own acknowledgement/completion SMS IDs.
    m=initial;finishError=0;m.statusFailures=2;beginRequest();
    for(int i=0;i<1000 && std::string_view(http_update::phase_name())!="Pending";++i)tick(m);
    assert(std::string_view(http_update::phase_name())=="Pending" && !reboots);
    m.full=true;tick(m);assert(!reboots);m.full=false;tick(m);
    const auto completion=m.last_sms_id();
    for(const auto &message:m.messages)if(optional(message))assert(!message.pending);
    assert(m.messages.back().text.find("verified; rebooting to install")!=std::string::npos);
    for(int i=0;i<10;++i)tick(m);
    assert(!reboots && m.sms_pending(completion));
    assert(m.send_sms("456","unrelated message"));
    m.submit(completion);tick(m);assert(reboots==1);
    assert(written.size()==m.image.size() && !memcmp(written.data(),m.image.data(),written.size()));
    assert(m.urls.size()==2 && m.urls[0].ends_with("/manifest.json") && m.urls[1].ends_with("/smart_power_socket.signed.bin"));
    puts("PASS: exact binary staged; reboot waits for completion SMS submission, independently of other queued messages");
}
