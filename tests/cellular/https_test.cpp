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
    if (cmd == "AT+CNACT=0,0\r\n") nativeActive = false;
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
}
namespace cellular {
sim7080::sim7080(const device *d, gpio_dt_spec p, gpio_flags_t pf, gpio_dt_spec t, gpio_flags_t tf)
    : atEngine_(d), pwrKey_(p, pf), dtr_(t, tf)
{
    atEngine_.init(); atEngine_.set_notification_handler(http_notification, this);
    modemInformation_.networkRegistration = 5; modemInformation_.dataConnected = 1;
}
sim7080::~sim7080() {}
void sim7080::ensure_data_connection() { modemInformation_.dataConnected = 1; }
}
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
    assert(modem.https_start(url, "x.bin") == -EBUSY);
    assert(count("AT+CFSDFILE") == deletes && count("AT+HTTPTOFS=") == starts);
    modem.https_cleanup(true); assert(!nativeActive);
    assert(modem.file_open() == -EBUSY); // An accepted PDP command is not native-idle proof.
    assert(commands.back() == "AT+CNACT=0,0\r\n");
    puts("PASS: a transfer surviving MCU reset is cancelled before a later start can touch its file");

    startReply = start_reply::timeout;
    assert(modem.https_start(url, "x.bin") == -ETIMEDOUT && nativeActive);
    modem.https_cleanup(true); assert(!nativeActive);
    startReply = start_reply::ok;
    puts("PASS: a lost HTTPTOFS OK still cancels the possibly running native request");

    assert(!modem.https_start(url, "x.bin", 16));
    assert(modem.https_poll(r) == -EFBIG);
    modem.https_cleanup(true); assert(!nativeActive);
    assert(!modem.https_start(url, "x.bin"));
    malformedProgress = true;
    assert(modem.https_poll(r) == -EBADMSG);
    malformedProgress = false; modem.https_cleanup(true);
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
}
