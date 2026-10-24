// Production SIM7080 HTTP/file methods and AT parser against a UART transcript.
#include "cellular/sim7080.hpp"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
static device dev;
static int64_t now;
static std::string rx,pending,lastCommand,stored;
static hardware::uartCon::callBack receive;
static void *context;
static bool instantCompletion,alternateProgress;
static void inject(std::string text) {rx+=text;while(!rx.empty())receive(&dev,context);}
int64_t k_uptime_get() {return now;}
void k_msleep(int32_t ms) {now+=ms;if(!pending.empty()) {auto p=pending;pending.clear();inject(p);}}
int uart_fifo_read(const device *,uint8_t *out,int max) {int n=std::min<int>(max,rx.size());memcpy(out,rx.data(),n);rx.erase(0,n);return n;}
namespace hardware {
uartCon::uartCon(const device *d,callBack cb,void *c):uart_(d),callBack_(cb),userData_(c) {receive=cb;context=c;}
uartCon::~uartCon() {}
int uartCon::init() {return 0;}
int uartCon::writeIntr(std::span<const uint8_t> bytes) {
    std::string cmd(reinterpret_cast<const char *>(bytes.data()),bytes.size());lastCommand=cmd;
    pending="\r\nOK\r\n";
    if(cmd.starts_with("AT+CFSGFIS"))pending="\r\n+CFSGFIS: "+std::to_string(stored.size())+"\r\nOK\r\n";
    if(cmd.starts_with("AT+HTTPTOFS=")) {
        if(instantCompletion)pending="\r\n+HTTPTOFS: 200,1024\r\nOK\r\n";
    }
    if(cmd.starts_with("AT+HTTPTOFSRL"))pending=std::string("\r\n")+(alternateProgress?"+HTTPTOFS:":"+HTTPTOFSRL:")+" 1,10,1024\r\nOK\r\n";
    if(cmd.starts_with("AT+CFSRFILE")) {
        unsigned count,offset;
        assert(sscanf(cmd.c_str(),"AT+CFSRFILE=0,\"x.bin\",1,%u,%u",&count,&offset)==2);
        assert(offset+count<=stored.size());
        pending="\r\n+CFSRFILE: "+std::to_string(count)+"\r\n"+stored.substr(offset,count)+"\r\nOK\r\n";
    }
    return bytes.size();
}
gpioCon::gpioCon(gpio_dt_spec spec,gpio_flags_t flags):gpio_(spec),flags_(flags) {}
gpioCon::~gpioCon() {}
}
namespace cellular {
sim7080::sim7080(const device *d,gpio_dt_spec p,gpio_flags_t pf,gpio_dt_spec t,gpio_flags_t tf):atEngine_(d),pwrKey_(p,pf),dtr_(t,tf) {
    atEngine_.init();atEngine_.set_notification_handler(http_notification,this);modemInformation_.networkRegistration=5;modemInformation_.dataConnected=1;
}
sim7080::~sim7080() {}
void sim7080::ensure_data_connection() {}
}
int main() {
    cellular::sim7080 modem(&dev,{},0,{},0);
    using result=cellular::sim7080::http_result;
    assert(!modem.https_prepare("raw.githubusercontent.com"));
    assert(modem.https_prepare("bad\"\r\nAT")<0);
    assert(modem.https_start("https://host/\"bad","x.bin")<0);
    assert(modem.https_start("http://host/x.bin","x.bin")<0);
    assert(!modem.https_start("https://host/x.bin","x.bin"));
    result r;assert(!modem.https_poll(r) && !r.finished && r.received==10 && r.total==1024);
    alternateProgress=true;assert(!modem.https_poll(r) && !r.finished && r.total==1024);
    inject("\r\n+HTTPTOFS: 200,1024\r\n");
    assert(!modem.https_poll(r) && r.finished && r.code==200 && r.bytes==1024);
    instantCompletion=true;
    assert(!modem.https_start("https://host/x.bin","x.bin"));
    assert(!modem.https_poll(r) && r.finished && r.code==200);
    puts("PASS: production HTTP methods parse progress variants and async completion before/after initial OK");
    for(int i=0;i<1024;++i)stored.push_back(i%256);
    assert(!modem.file_open());uint32_t size;assert(!modem.file_size("x.bin",size) && size==1024);
    std::array<uint8_t,1024> bytes{};assert(!modem.file_read("x.bin",0,bytes));
    assert(!memcmp(bytes.data(),stored.data(),1024));
    assert(modem.file_read("../x.bin",0,bytes)<0);
    assert(!modem.file_close());modem.https_cleanup(false);
    puts("PASS: production modem file APIs read all 256 byte values without corruption and reject unsafe names");
}
