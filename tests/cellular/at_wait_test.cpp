// Execute production AT parsing and the common wait helper against a fake UART.
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include "cellular/at_engine.hpp"
static device uartDevice;
static int64_t now, due;
static int sleepStep=1;
static std::string rx, pendingRx, writes;
static hardware::uartCon::callBack receive;
static void *receiveContext;
enum mode { normal, data, error, no_response, sms, no_prompt, sms_error, sms_timeout };
static mode behavior;
static bool smsPayload;
static std::string binaryFrame;
int64_t k_uptime_get() { return now; }
void k_msleep(int32_t ms) {
    now += ms*sleepStep;
    if (!pendingRx.empty() && now>=due) {
        rx+=pendingRx; pendingRx.clear(); while (!rx.empty()) receive(&uartDevice,receiveContext);
    }
}
int uart_fifo_read(const device *,uint8_t *buf,int max) {
    int n=std::min<int>(max,rx.size()); memcpy(buf,rx.data(),n); rx.erase(0,n); return n;
}
namespace hardware {
uartCon::uartCon(const device *d,callBack cb,void *ctx):uart_(d),callBack_(cb),userData_(ctx) {
    receive=cb; receiveContext=ctx;
}
uartCon::~uartCon() {}
int uartCon::init() { return 0; }
int uartCon::writeIntr(std::span<const uint8_t> bytes) {
    writes.append(reinterpret_cast<const char *>(bytes.data()),bytes.size());
    std::string text(reinterpret_cast<const char *>(bytes.data()),bytes.size());
    if(text.starts_with("AT+CFSRFILE")) pendingRx=binaryFrame;
    else if(text=="AT+CSQ\r\n") pendingRx="AT+CSQ\r\n+CSQ: 24,99\r\nOK\r\n";
    else if(text.starts_with("AT+CMGS")) {
        smsPayload=true;
        if(behavior!=no_prompt) pendingRx="> ";
    } else if(bytes.size()==1 && bytes[0]==0x1a) {
        if(behavior==sms) pendingRx="\r\n+CMGS: 1\r\nOK\r\n";
        else if(behavior==sms_error) pendingRx="\r\n+CMS ERROR: 500\r\n";
    } else if (!smsPayload && behavior==normal) pendingRx="AT\r\nOK\r\n";
    else if (!smsPayload && behavior==error) pendingRx="\r\nERROR\r\n";
    due=now+2;
    return bytes.size();
}
}
static void setup(mode m) {
    now=0; due=0; sleepStep=1; rx.clear(); pendingRx.clear(); writes.clear(); behavior=m; smsPayload=false;
}
int main() {
    using engine=cellular::atEngine;
    engine modem(&uartDevice); assert(!modem.init());
    setup(normal); assert(modem.send_command("AT\r\n",100)==engine::atResult::OK);
    setup(error); assert(modem.send_command("AT\r\n",100)==engine::atResult::Error);
    puts("PASS: shared wait preserves ordinary AT OK/error responses");
    setup(data); std::array<uint8_t,100> response{};
    auto result=modem.send_command("AT+CSQ\r\n","+CSQ:",response,100);
    assert(result.result==engine::atResult::OK && result.responseLength>0);
    assert(std::string_view(reinterpret_cast<char *>(response.data()),result.responseLength).starts_with("+CSQ: 24,99"));
    puts("PASS: prefixed AT response collection and echo filtering preserved");
    setup(no_response); sleepStep=17;
    assert(modem.send_command("AT\r\n",50)==engine::atResult::Timeout);
    assert(now>=50 && now<67);
    puts("PASS: timeout uses elapsed wall time instead of poll iteration count");
    setup(sms);
    assert(modem.send_prompt_command("AT+CMGS=\"123\"\r\n","hello",100,60000)==engine::atResult::OK);
    assert(writes.find("hello")!=std::string::npos && writes.back()==0x1a);
    puts("PASS: SMS prompt, payload, Ctrl-Z and successful submission preserved");
    setup(no_prompt);
    assert(modem.send_prompt_command("AT+CMGS=\"123\"\r\n","hello",50,60000)==engine::atResult::Timeout);
    assert(writes.back()==0x1b && writes.find("hello")==std::string::npos);
    puts("PASS: missing SMS prompt sends ESC and never submits payload");
    setup(sms_error);
    assert(modem.send_prompt_command("AT+CMGS=\"123\"\r\n","hello",100,60000)==engine::atResult::Error);
    puts("PASS: SMS modem error still returns failure to the generic retry queue");
    setup(sms_timeout); sleepStep=13;
    assert(modem.send_prompt_command("AT+CMGS=\"123\"\r\n","hello",100,60000)==engine::atResult::Timeout);
    assert(now>=60000 && now<60040);
    puts("PASS: full 60 s SMS submission timeout remains available");
    const char raw[]="\0\r\nOK\r\nERROR\r\n+HTTPTOFS: 200,123\r\n>\x1a";
    const std::string payload(raw,sizeof(raw)-1);

    std::array<uint8_t,sizeof(raw)-1> binary{};
    auto readBinary = [&] {
        return modem.send_binary_command("AT+CFSRFILE=0,\"x.bin\",1,32,0\r\n", "+CFSRFILE:",binary,100);
    };
    setup(data);
    binaryFrame="\r\n+CFSRFILE: "+std::to_string(payload.size())+"\r\n"+payload+"\r\nOK\r\n";
    result=readBinary();
    assert(result.result==engine::atResult::OK && result.responseLength==payload.size());
    assert(!memcmp(binary.data(),payload.data(),payload.size()));
    puts("PASS: binary bytes bypass text OK/ERROR, NUL, CR/LF, prompt and notification parsing");
    setup(data); binaryFrame="\r\n+CFSRFILE: 38\r\n"+payload+"\r\nOK\r\n";
    assert(readBinary().result==engine::atResult::InvalidResponse);
    setup(data); binaryFrame="\r\n+CFSRFILE: "+std::to_string(payload.size())+"\r\nshort";
    assert(readBinary().result==engine::atResult::Timeout);
    setup(data); binaryFrame="\r\nERROR\r\n";
    assert(readBinary().result==engine::atResult::Error);
    setup(normal); assert(modem.send_command("AT\r\n",100)==engine::atResult::OK);
    puts("PASS: oversized, truncated and rejected binary frames fail and text commands recover");
    int notifications=0;
    modem.set_notification_handler([](std::string_view line,void *ctx) {
        if(!line.starts_with("+HTTPTOFS:")) return false;
        ++*static_cast<int *>(ctx);return true;
    },&notifications);
    setup(data); binaryFrame="\r\n+HTTPTOFS: 200,999\r\n+CFSRFILE: "+std::to_string(payload.size())+"\r\n"+payload+"\r\nOK\r\n";
    assert(readBinary().result==engine::atResult::OK && notifications==1);
    setup(sms); rx="\r\n+HTTPTOFS: 200,456\r\n";
    while(!rx.empty()) receive(&uartDevice,receiveContext);
    assert(modem.send_prompt_command("AT+CMGS=\"123\"\r\n","hello",100,60000)==engine::atResult::OK);
    assert(notifications==2);
    puts("PASS: asynchronous HTTP completion survives binary commands and SMS prompt setup");

}
