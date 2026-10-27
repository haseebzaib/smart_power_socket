// Production SIM7080 HTTP/file methods and AT parser against a UART transcript.
#include "cellular/sim7080.hpp"
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static device dev;
static int64_t now;
static std::string rx, pending, stored;
static std::vector<std::string> commands;
static hardware::uartCon::callBack receive;
static void *context;
static bool nativeActive, fileSession, instantCompletion, alternateProgress;
static bool completionInQuery, queryError, malformedProgress, sizeTimeout;
static unsigned closeFailures;
static bool stuckPdp, stuckReset, lostResetOk, responsive=true, configurationOk=true;
static unsigned configurations;
enum class start_reply { ok, error, timeout };
static start_reply startReply;

static void inject(std::string text)
{
    rx += text;
    while (!rx.empty()) receive(&dev, context);
}
int64_t k_uptime_get() { return now; }
void k_msleep(int32_t ms)
{
    now += ms;
    if (!pending.empty()) { auto p = pending; pending.clear(); inject(p); }
}
int uart_fifo_read(const device *, uint8_t *out, int max)
{
    int n = std::min<int>(max, rx.size());
    memcpy(out, rx.data(), n); rx.erase(0, n); return n;
}
static size_t count(std::string_view prefix)
{
    return std::count_if(commands.begin(), commands.end(), [prefix](auto &c) { return c.starts_with(prefix); });
}
namespace hardware {
uartCon::uartCon(const device *d, callBack cb, void *c) : uart_(d), callBack_(cb), userData_(c)
{ receive = cb; context = c; }
uartCon::~uartCon() {}
int uartCon::init() { return 0; }
int uartCon::writeIntr(std::span<const uint8_t> bytes)
{
    std::string cmd(reinterpret_cast<const char *>(bytes.data()), bytes.size());
    commands.push_back(cmd);
    pending = "\r\nOK\r\n";
    if (cmd == "AT+CFSINIT\r\n") fileSession = true;
    if (cmd == "AT+CFSTERM\r\n") {
        if (closeFailures || !fileSession) {
            pending = "\r\nERROR\r\n";
            if (closeFailures) --closeFailures;
        } else fileSession = false;
    }
    if (cmd.starts_with("AT+CFSGFIS")) {
        pending = sizeTimeout ? "" : "\r\n+CFSGFIS: " + std::to_string(stored.size()) + "\r\nOK\r\n";
    }
    if (cmd.starts_with("AT+HTTPTOFS=")) {
        nativeActive = true;
        if (startReply == start_reply::timeout) pending.clear();
        if (startReply == start_reply::error) { nativeActive = false; pending = "\r\nERROR\r\n"; }
        if (instantCompletion) { nativeActive = false; pending = "\r\n+HTTPTOFS: 200,1024\r\nOK\r\n"; }
    }
    if (cmd.starts_with("AT+HTTPTOFSRL")) {
        pending = std::string("\r\n") + (alternateProgress ? "+HTTPTOFS:" : "+HTTPTOFSRL:") +
            (nativeActive ? " 1,10,1024\r\n" : " 0,0,0\r\n") + (queryError ? "ERROR\r\n" : "OK\r\n");
        if (malformedProgress) pending = "\r\n+HTTPTOFSRL: 1,20,10\r\nOK\r\n";
        if (completionInQuery) { nativeActive = false; pending = "\r\n+HTTPTOFS: 200,1024\r\n" + pending; }
    }
    if (cmd == "AT+CNACT=0,0\r\n" && !stuckPdp) nativeActive = false;
    if (cmd == "AT+CFUN=1,1\r\n") {
        if (!stuckReset) nativeActive=false;
        if (lostResetOk) pending.clear();
    }
    if (cmd == "AT\r\n" && !responsive) pending.clear();
    if (cmd.starts_with("AT+CFSRFILE")) {
        unsigned count, offset;
        assert(sscanf(cmd.c_str(), "AT+CFSRFILE=0,\"x.bin\",1,%u,%u", &count, &offset) == 2);
        assert(offset + count <= stored.size());
        pending = "\r\n+CFSRFILE: " + std::to_string(count) + "\r\n" + stored.substr(offset, count) + "\r\nOK\r\n";
    }
    return bytes.size();
}
gpioCon::gpioCon(gpio_dt_spec spec, gpio_flags_t flags) : gpio_(spec), flags_(flags) {}
gpioCon::~gpioCon() {}
int gpioCon::init() {return 0;}
int gpioCon::set(uint8_t) {return 0;}
}
#if !defined(SPS_TEST_STARTUP_INIT)
namespace cellular {
sim7080::sim7080(const device *d, gpio_dt_spec p, gpio_flags_t pf, gpio_dt_spec t, gpio_flags_t tf)
    : atEngine_(d), pwrKey_(p, pf), dtr_(t, tf)
{
    atEngine_.init(); atEngine_.set_notification_handler(http_notification, this);
    modemInformation_.networkRegistration = 5; modemInformation_.dataConnected = 1;
}
sim7080::~sim7080() {}
void sim7080::ensure_data_connection() { modemInformation_.dataConnected = 1; }
bool sim7080::configure_modem(int retries) {
    assert(retries==1);++configurations;
    if(configurationOk)modemInformation_.networkRegistration=5;
    return configurationOk;
}
}
#endif
static int recover(cellular::sim7080 &modem) {
    int result=0;
    for(unsigned i=0;i<100 && modem.https_recovering();++i) {
        k_msleep(1000);result=modem.https_prepare("raw.githubusercontent.com");
    }
    assert(!modem.https_recovering());return result;
}
#if !defined(SPS_TEST_STARTUP_INIT)
int main()
{
    cellular::sim7080 modem(&dev, {}, 0, {}, 0);
    using result = cellular::sim7080::http_result;
    constexpr auto url = "https://raw.githubusercontent.com/test/x.bin";
    for (int i = 0; i < 1024; ++i) stored.push_back(i % 256);
    assert(!modem.https_prepare("raw.githubusercontent.com"));
    assert(modem.https_start("https://other.host/x.bin", "x.bin") == -EINVAL);
    assert(modem.https_start("https://raw.githubusercontent.com/\"bad", "x.bin") == -EINVAL);
    assert(modem.https_start(url, "..") == -EINVAL);
    assert(!modem.https_start(url, "x.bin"));
    assert(commands.back().ends_with(",1000\r\n"));
    assert(modem.file_open() == -EBUSY);
    result r;
    assert(!modem.https_poll(r) && !r.finished && r.downloading && r.received == 10 && r.total == 1024);
    alternateProgress = true;
    assert(!modem.https_poll(r) && !r.finished && r.total == 1024);
    nativeActive = false;
    inject("\r\n+HTTPTOFS: 200,1024\r\n+HTTPTOFS: 500,0\r\n");
    assert(!modem.https_poll(r) && r.finished && r.code == 200 && r.bytes == 1024);
    instantCompletion = true;
    assert(!modem.https_start(url, "x.bin"));
    assert(!modem.https_poll(r) && r.finished && r.code == 200);
    instantCompletion = false;
    puts("PASS: progress variants, early completion and duplicate URCs preserve the first HTTP result");

    assert(!modem.https_start(url, "x.bin"));
    completionInQuery = queryError = true;
    assert(!modem.https_poll(r) && r.finished && r.code == 200);
    completionInQuery = queryError = false;
    puts("PASS: HTTP completion survives an overlapping status-query ERROR");

    // Reconcile native state before deleting or opening a previous destination.
    nativeActive = true;
    auto deletes = count("AT+CFSDFILE"), starts = count("AT+HTTPTOFS=");
    assert(modem.https_start(url, "x.bin") == -EAGAIN);
    assert(count("AT+CFSDFILE") == deletes && count("AT+HTTPTOFS=") == starts);
    modem.https_cleanup(true); assert(!nativeActive);
    assert(modem.file_open() == -EBUSY); // An accepted PDP command is not native-idle proof.
    assert(commands.back() == "AT+CNACT=0,0\r\n");
    assert(!recover(modem) && count("AT+CFUN=1,1")==0);
    puts("PASS: a transfer surviving MCU reset is cancelled before a later start can touch its file");

    startReply = start_reply::timeout;
    assert(modem.https_start(url, "x.bin") == -ETIMEDOUT && nativeActive);
    modem.https_cleanup(true); assert(!nativeActive);
    assert(!recover(modem));
    startReply = start_reply::ok;
    puts("PASS: a lost HTTPTOFS OK still cancels the possibly running native request");

    assert(!modem.https_start(url, "x.bin", 16));
    assert(modem.https_poll(r) == -EFBIG);
    modem.https_cleanup(true); assert(!nativeActive);
    assert(!recover(modem));
    assert(!modem.https_start(url, "x.bin"));
    malformedProgress = true;
    assert(modem.https_poll(r) == -EBADMSG);
    malformedProgress = false; modem.https_cleanup(true);assert(!recover(modem));
    puts("PASS: oversized downloads and inconsistent progress counts are rejected");

    starts = count("AT+HTTPTOFS="); deletes = count("AT+CFSDFILE");
    sizeTimeout = true;
    assert(modem.https_start(url, "x.bin") == -ETIMEDOUT);
    assert(count("AT+HTTPTOFS=") == starts && count("AT+CFSDFILE") == deletes);
    sizeTimeout = false; modem.https_cleanup(true);
    puts("PASS: a timed-out file-size query is not treated as a missing destination");

    assert(!modem.file_open());
    uint32_t size; assert(!modem.file_size("x.bin", size) && size == 1024);
    std::array<uint8_t, 1024> bytes{};
    assert(!modem.file_read("x.bin", 0, bytes) && !memcmp(bytes.data(), stored.data(), 1024));
    assert(modem.file_read("x.bin", UINT32_MAX, bytes) == -EINVAL);
    closeFailures = 1; assert(modem.file_close() == -EIO);
    auto inits = count("AT+CFSINIT");
    assert(!modem.file_open() && count("AT+CFSINIT") == inits + 1);
    assert(!modem.file_close());
    assert(modem.https_prepare("bad\"\r\nAT") == -EINVAL);
    puts("PASS: binary file reads, offset bounds and CFS-session recovery after failed close");

    // Transcript reported by the carrier: PDP deactivation OK, but the native
    // HTTP engine remains active. No new file may be touched until reset+idle.
    nativeActive=true;stuckPdp=true;
    starts=count("AT+HTTPTOFS=");deletes=count("AT+CFSDFILE");
    const auto resets=count("AT+CFUN=1,1");
    assert(modem.https_start(url,"x.bin")==-EAGAIN);
    assert(modem.file_open()==-EBUSY && modem.https_recovering());
    assert(!recover(modem) && !nativeActive && configurations==1);
    assert(count("AT+CFUN=1,1")==resets+1);
    assert(count("AT+HTTPTOFS=")==starts && count("AT+CFSDFILE")==deletes);
    assert(!modem.https_start(url,"x.bin"));
    stuckPdp=false;modem.https_cleanup(true);assert(!recover(modem));
    puts("PASS: stuck native download after accepted PDP cancellation triggers exactly one modem reset before file reuse");

    nativeActive=stuckPdp=lostResetOk=true;
    const auto beforeLostOk=count("AT+CFUN=1,1");
    assert(modem.https_start(url,"x.bin")==-EAGAIN && !recover(modem));
    assert(count("AT+CFUN=1,1")==beforeLostOk+1 && !nativeActive);
    lostResetOk=false;
    puts("PASS: lost reset OK is reconciled through responsiveness and native idle; reset is never blindly replayed");

    // A reset that does not clear the HTTP state must fail closed, not cause
    // a reboot loop or delete the destination still owned by the native task.
    cellular::sim7080 broken(&dev,{},0,{},0);
    nativeActive=stuckPdp=stuckReset=true;
    const auto beforeBadReset=count("AT+CFUN=1,1");
    deletes=count("AT+CFSDFILE");starts=count("AT+HTTPTOFS=");
    assert(broken.https_start(url,"x.bin")==-EINVAL); // Host not configured yet.
    assert(!broken.https_prepare("raw.githubusercontent.com"));
    assert(broken.https_start(url,"x.bin")==-EAGAIN && recover(broken)==-EBUSY);
    broken.https_cleanup(true);
    for(int i=0;i<10;++i)assert(broken.https_prepare("raw.githubusercontent.com")==-EBUSY);
    assert(count("AT+CFUN=1,1")==beforeBadReset+1);
    assert(count("AT+CFSDFILE")==deletes && count("AT+HTTPTOFS=")==starts && broken.file_open()==-EBUSY);
    puts("PASS: persistent HTTP state after reset stays blocked without repeated resets or destination access");

    cellular::sim7080 dead(&dev,{},0,{},0);
    assert(!dead.https_prepare("raw.githubusercontent.com"));
    nativeActive=stuckPdp=true;stuckReset=false;responsive=false;
    const auto beforeDeadReset=count("AT+CFUN=1,1");
    const auto began=now;
    assert(dead.https_start(url,"x.bin")==-EAGAIN && recover(dead)==-ETIMEDOUT);
    assert(count("AT+CFUN=1,1")==beforeDeadReset+1 && now-began<90000);
    puts("PASS: unresponsive modem after reset has a bounded recovery timeout");

    cellular::sim7080 misconfigured(&dev,{},0,{},0);
    responsive=true;configurationOk=false;nativeActive=true;
    assert(!misconfigured.https_prepare("raw.githubusercontent.com"));
    const auto beforeConfiguration=count("AT+CFUN=1,1");
    assert(misconfigured.https_start(url,"x.bin")==-EAGAIN && recover(misconfigured)==-EIO);
    assert(count("AT+CFUN=1,1")==beforeConfiguration+1 && misconfigured.file_open()==-EBUSY);
    puts("PASS: failed modem reconfiguration cannot enable destination reuse or a reset loop");
}
#else
int main()
{
    // Exercise the actual init()/loop()/RAM outbox, not constructor or init mocks.
    cellular::sim7080 idle(&dev,{},0,{},0);
    assert(!idle.init() && !idle.https_recovering());
    assert(count("AT+HTTPTOFSRL?")==1 && count("AT+CNACT=0,0")==0 && count("AT+CFUN=1,1")==0);
    const auto idleChecks=count("AT+HTTPTOFSRL?");
    assert(!idle.init() && count("AT+HTTPTOFSRL?")==idleChecks);
    puts("PASS: real modem init checks native HTTP once per MCU boot; an idle modem is not reset");

    cellular::sim7080 stale(&dev,{},0,{},0);
    assert(stale.send_sms("123","pending command reply"));
    const auto reply=stale.last_sms_id();
    nativeActive=stuckPdp=true;
    const auto starts=count("AT+HTTPTOFS="),deletes=count("AT+CFSDFILE"),resets=count("AT+CFUN=1,1");
    assert(!stale.init() && stale.https_recovering());
    assert(stale.file_open()==-EBUSY && !stale.sms_send_ready());
    cellular::sim7080::modemInformation info{};
    for(unsigned i=0;i<100 && stale.https_recovering();++i) {
        k_msleep(1000);stale.loop(info,false,false);stale.process_sms_outbox();
    }
    assert(!stale.https_recovering() && !nativeActive);
    assert(count("AT+CFUN=1,1")==resets+1 && stale.sms_pending(reply) && stale.sms_pending_count()==1);
    assert(count("AT+HTTPTOFS=")==starts && count("AT+CFSDFILE")==deletes);
    assert(count("AT+CMGS")==0);
    puts("PASS: real startup releases a stale HTTP worker without an OTA request, resets once if PDP cancellation sticks, and retains queued SMS");

    cellular::sim7080 cancelled(&dev,{},0,{},0);
    nativeActive=true;stuckPdp=false;
    const auto beforeCancel=count("AT+CFUN=1,1");
    assert(!cancelled.init() && cancelled.https_recovering());
    for(unsigned i=0;i<20 && cancelled.https_recovering();++i) {
        k_msleep(1000);cancelled.loop(info,false,false);
    }
    assert(!cancelled.https_recovering() && !nativeActive && count("AT+CFUN=1,1")==beforeCancel);
    puts("PASS: successful startup PDP cancellation is verified idle without a modem reset");

    cellular::sim7080 unknown(&dev,{},0,{},0);
    queryError=true;
    assert(!unknown.init() && unknown.https_recovering() && unknown.file_open()==-EBUSY);
    queryError=false;
    for(unsigned i=0;i<20 && unknown.https_recovering();++i) {
        k_msleep(1000);unknown.loop(info,false,false);
    }
    assert(!unknown.https_recovering());
    puts("PASS: an unknown HTTP state at startup is treated as unverified ownership and recovered before file reuse");
}
#endif
