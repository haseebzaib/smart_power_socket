#include "sim7080.hpp"
#include "system/watchdog.hpp"

#include <cerrno>
#include <charconv>
#include <cstdio>

LOG_MODULE_REGISTER(sim7080_https, LOG_LEVEL_INF);

namespace {
constexpr uint32_t commandTimeoutMs = 5000;
constexpr std::string_view completionPrefix = "+HTTPTOFS:";
constexpr std::string_view progressPrefix = "+HTTPTOFSRL:";

bool number(std::string_view &text, uint32_t &out, bool last)
{
    while (!text.empty() && text.front() == ' ') text.remove_prefix(1);
    if (text.empty()) return false;
    auto result = std::from_chars(text.data(), text.data() + text.size(), out);
    if (result.ec != std::errc{}) return false;
    text.remove_prefix(result.ptr - text.data());
    while (!text.empty() && (text.front() == ' ' || text.front() == '\r' || text.front() == '\n'))
        text.remove_prefix(1);
    if (last) return text.empty();
    if (text.empty() || text.front() != ',') return false;
    text.remove_prefix(1);
    return true;
}

bool alphanumeric(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

bool safe_host(std::string_view host)
{
    if (host.empty() || host.size() > 100 || !alphanumeric(host.front()) ||
        !alphanumeric(host.back()) || host.find("..") != host.npos) return false;
    for (char c : host) if (!alphanumeric(c) && c != '-' && c != '.') return false;
    return true;
}

bool safe_file(std::string_view file)
{
    if (file.empty() || file.size() > 32 || !alphanumeric(file.front()) ||
        file.find("..") != file.npos) return false;
    for (char c : file) if (!alphanumeric(c) && c != '-' && c != '.' && c != '_') return false;
    return true;
}

int command_error(cellular::atEngine::atResult result)
{
    using result_t = cellular::atEngine::atResult;
    switch (result) {
    case result_t::OK: return 0;
    case result_t::Timeout: return -ETIMEDOUT;
    case result_t::Busy: return -EBUSY;
    case result_t::InvalidResponse: case result_t::BufferOverflow: return -EBADMSG;
    default: return -EIO;
    }
}

bool progress(std::string_view line, cellular::sim7080::http_result &result)
{
    // The manual's example uses HTTPTOFS for this three-field query reply.
    if (line.starts_with(progressPrefix)) line.remove_prefix(progressPrefix.size());
    else if (line.starts_with(completionPrefix)) line.remove_prefix(completionPrefix.size());
    else return false;
    uint32_t state, received, total;
    if (!number(line, state, false) || !number(line, received, false) ||
        !number(line, total, true) || state > 1 || (total && received > total)) return false;
    result.downloading = state == 1;
    result.received = received;
    result.total = total;
    return true;
}
}

namespace cellular {
bool sim7080::http_notification(std::string_view line, void *self)
{
    if (!line.starts_with(completionPrefix)) return false;
    auto values = line.substr(completionPrefix.size());
    uint32_t status, bytes;
    if (!number(values, status, false) || !number(values, bytes, true)) return false;
    if (status < 100 || status > 65535) return true;
    auto &modem = *static_cast<sim7080 *>(self);
    LOG_INF("HTTPTOFS completion: status=%u bytes=%u tracked=%u", status, bytes,
            static_cast<unsigned>(modem.httpActive_));
    if (modem.httpActive_ && !modem.httpResult_.finished) {
        modem.httpResult_.code = static_cast<int>(status);
        modem.httpResult_.bytes = bytes;
        modem.httpResult_.finished = true;
        modem.httpResult_.downloading = false;
        modem.httpMayBeRunning_ = false;
    }
    // Consume stale/duplicate completion URCs too, rather than adding them to
    // another command's response. Three-field progress replies pass through.
    return true;
}

int sim7080::https_prepare(std::string_view host)
{
    if (!safe_host(host)) return -EINVAL;
    if (httpActive_) return -EBUSY;
    int recovery = service_http_recovery();
    if (recovery) return recovery;
    httpHost_.fill(0);
    if (modemInformation_.networkRegistration != 1 && modemInformation_.networkRegistration != 5)
        return -ENETDOWN;
    ensure_data_connection();
    if (!modemInformation_.dataConnected) return -ENETUNREACH;
    int rc = command_error(atEngine_.send_command("AT+CSSLCFG=\"SSLVERSION\",0,3\r\n", commandTimeoutMs));
    if (rc) return rc;
    std::array<char, 160> command{};
    int n = std::snprintf(command.data(), command.size(), "AT+CSSLCFG=\"SNI\",0,\"%.*s\"\r\n",
        static_cast<int>(host.size()), host.data());
    if (n < 0 || static_cast<size_t>(n) >= command.size()) return -ENAMETOOLONG;
    rc = command_error(atEngine_.send_command({command.data(), static_cast<size_t>(n)}, commandTimeoutMs));
    if (!rc) std::copy(host.begin(), host.end(), httpHost_.begin());
    return rc;
}

int sim7080::file_open()
{
    if (httpMayBeRunning_ || httpActive_) return -EBUSY;
    if (fileOpen_) return 0;
    if (fileNeedsSync_) {
        // A modem/MCU reset or failed CFSTERM can leave the CFS session unknown.
        // Termination may legitimately return ERROR if no session was open.
        auto reset = atEngine_.send_command("AT+CFSTERM\r\n", commandTimeoutMs);
        if (reset != atEngine::atResult::OK && reset != atEngine::atResult::Error)
            return command_error(reset);
    }
    int rc = command_error(atEngine_.send_command("AT+CFSINIT\r\n", commandTimeoutMs));
    if (rc) return rc;
    fileOpen_ = true;
    fileNeedsSync_ = false;
    return 0;
}

int sim7080::file_close()
{
    if (!fileOpen_) return 0;
    int rc = command_error(atEngine_.send_command("AT+CFSTERM\r\n", commandTimeoutMs));
    fileOpen_ = false;
    fileNeedsSync_ = rc != 0;
    return rc;
}

int sim7080::file_size(std::string_view file, uint32_t &size)
{
    if (!fileOpen_ || !safe_file(file)) return -EINVAL;
    std::array<char, 96> command{}, response{};
    int n = std::snprintf(command.data(), command.size(), "AT+CFSGFIS=0,\"%.*s\"\r\n",
        static_cast<int>(file.size()), file.data());
    auto result = atEngine_.send_command({command.data(), static_cast<size_t>(n)}, "+CFSGFIS:",
        {reinterpret_cast<uint8_t *>(response.data()), response.size()}, commandTimeoutMs);
    int rc = command_error(result.result);
    if (rc) return rc;
    std::string_view value{response.data(), result.responseLength};
    if (!value.starts_with("+CFSGFIS:")) return -EBADMSG;
    value.remove_prefix(9);
    return number(value, size, true) ? 0 : -EBADMSG;
}

int sim7080::file_read(std::string_view file, uint32_t offset, std::span<uint8_t> output)
{
    if (!fileOpen_ || !safe_file(file) || output.empty() || output.size() > 1024 ||
        offset > UINT32_MAX - output.size()) return -EINVAL;
    std::array<char, 128> command{};
    int n = std::snprintf(command.data(), command.size(), "AT+CFSRFILE=0,\"%.*s\",1,%u,%u\r\n",
        static_cast<int>(file.size()), file.data(), static_cast<unsigned>(output.size()), static_cast<unsigned>(offset));
    auto result = atEngine_.send_binary_command({command.data(), static_cast<size_t>(n)}, "+CFSRFILE:", output, commandTimeoutMs);
    int rc = command_error(result.result);
    return rc ? rc : result.responseLength == output.size() ? 0 : -EBADMSG;
}

int sim7080::read_http_progress(http_result &result)
{
    atEngine_.processRx();
    if (httpActive_ && httpResult_.finished) { result = httpResult_; return 0; }
    std::array<char, 128> response{};
    auto reply = atEngine_.send_command("AT+HTTPTOFSRL?\r\n", "+HTTPTOFS",
        {reinterpret_cast<uint8_t *>(response.data()), response.size()}, commandTimeoutMs);
    // A completion URC is authoritative even if the overlapping query fails.
    if (httpActive_ && httpResult_.finished) { result = httpResult_; return 0; }
    int rc = command_error(reply.result);
    if (rc) return rc;
    result = {};
    const std::string_view raw{response.data(), reply.responseLength};
    if (!progress(raw, result)) {
        LOG_ERR("Invalid HTTP progress response: %.*s", static_cast<int>(raw.size()), raw.data());
        return -EBADMSG;
    }
    if (!httpActive_ || k_uptime_get()>=nextHttpRawLog_ || result.received!=httpResult_.received ||
        result.downloading!=httpResult_.downloading) {
        LOG_INF("HTTP progress response: %.*s", static_cast<int>(raw.size()), raw.data());
        nextHttpRawLog_=k_uptime_get()+30000;
    }
    return 0;
}

bool sim7080::https_recovering() const
{
    return httpRecovery_!=http_recovery::none && httpRecovery_!=http_recovery::failed;
}

void sim7080::start_http_recovery()
{
    if (httpRecovery_!=http_recovery::none) return;
    httpActive_=false;
    httpResult_={};
    httpMayBeRunning_=true;
    int rc=command_error(atEngine_.send_command("AT+CNACT=0,0\r\n",commandTimeoutMs));
    modemInformation_.dataConnected=0;
    modemInformation_.ipAddress.fill(0);
    copy_string_view_to_array("0.0.0.0",modemInformation_.ipAddress);
    nextDataConnectionRetryMs_=k_uptime_get()+dataConnectionRetryIntervalMs;
    httpRecovery_=http_recovery::wait_idle;
    httpRecoveryDeadline_=k_uptime_get()+10000;
    httpRecoveryNextPoll_=k_uptime_get()+1000;
    LOG_WRN("HTTP cancellation requested through PDP deactivation: %d; checking idle for 10 s",rc);
}

int sim7080::service_http_recovery()
{
    if (httpRecovery_==http_recovery::none) return 0;
    if (httpRecovery_==http_recovery::failed) return httpRecoveryError_;
    if (k_uptime_get()<httpRecoveryNextPoll_) return -EAGAIN;
    if (httpRecovery_==http_recovery::wait_idle) {
        http_result native;
        const int rc=read_http_progress(native);
        if (!rc && !native.downloading) {
            httpMayBeRunning_=false;
            fileNeedsSync_=true;
            httpRecovery_=http_recovery::none;
            LOG_INF("HTTP cancellation verified: native engine idle");
            return 0;
        }
        if (k_uptime_get()<httpRecoveryDeadline_) {
            httpRecoveryNextPoll_=k_uptime_get()+1000;
            return -EAGAIN;
        }
        LOG_WRN("HTTP engine remains busy after PDP cancellation; restarting SIM7080 once");
        // A missing OK can mean the reset has already executed. Do not resend
        // the state-changing command; probe AT and verify native idle instead.
        const auto reset=atEngine_.send_command("AT+CFUN=1,1\r\n",10000);
        if (reset!=atEngine::atResult::OK && reset!=atEngine::atResult::Timeout) {
            httpRecoveryError_=command_error(reset);
            httpRecovery_=http_recovery::failed;
            LOG_ERR("SIM7080 HTTP recovery reset rejected: %d",httpRecoveryError_);
            return httpRecoveryError_;
        }
        modemInformation_.networkRegistration=0;
        modemInformation_.networkQuality=-1;
        modemInformation_.signalValid=false;
        modemInformation_.dataConnected=0;
        fileOpen_=false;
        fileNeedsSync_=true;
        httpHost_.fill(0);
        httpRecovery_=http_recovery::wait_restart;
        httpRecoveryDeadline_=k_uptime_get()+60000;
        httpRecoveryNextPoll_=k_uptime_get()+3000;
        return -EAGAIN;
    }
    if (httpRecovery_==http_recovery::wait_restart) {
        if (k_uptime_get()>=httpRecoveryDeadline_) {
            httpRecoveryError_=-ETIMEDOUT;
            httpRecovery_=http_recovery::failed;
            LOG_ERR("SIM7080 HTTP recovery: modem did not become responsive within 60 s");
            return httpRecoveryError_;
        }
        if (atEngine_.send_command("AT\r\n",readinessProbeTimeoutMs)!=atEngine::atResult::OK) {
            httpRecoveryNextPoll_=k_uptime_get()+1000;
            return -EAGAIN;
        }
        httpRecovery_=http_recovery::configure;
        httpRecoveryNextPoll_=0;
        return -EAGAIN;
    }
    // Each configuration command is attempted once. Its 5 s timeout and the
    // following status query fit within this finite task-watchdog allowance.
    app_watchdog::operation allowance{90000,"HTTP modem reconfiguration"};
    const bool configured=configure_modem(1);
    http_result native;
    const int rc=configured?read_http_progress(native):-EIO;
    if (rc || native.downloading) {
        httpRecoveryError_=rc?rc:-EBUSY;
        httpRecovery_=http_recovery::failed;
        LOG_ERR("SIM7080 HTTP recovery failed: %d; destination files remain protected",httpRecoveryError_);
        return httpRecoveryError_;
    }
    httpMayBeRunning_=false;
    httpRecovery_=http_recovery::none;
    nextDataConnectionRetryMs_=0;
    LOG_INF("SIM7080 HTTP recovery complete; native engine idle, awaiting network reconnect; SMS outbox retained");
    return 0;
}

int sim7080::https_start(std::string_view url, std::string_view file, uint32_t maxBytes)
{
    if (httpActive_) return -EBUSY;
    if (https_recovering()) return -EAGAIN;
    if (httpRecovery_==http_recovery::failed) return httpRecoveryError_;
    if (!safe_file(file) || !maxBytes || !url.starts_with("https://") || url.size() > 255) return -EINVAL;
    auto slash = url.find('/', 8);
    if (slash == url.npos || httpHost_[0] == 0 || url.substr(8, slash - 8) != httpHost_.data()) return -EINVAL;
    for (unsigned char c : url) if (c <= 32 || c >= 127 || c == '"' || c == '\\') return -EINVAL;

    // The modem stays powered across MCU resets. Never delete a file still
    // being written by a transfer from a previous application boot.
    http_result previous;
    int rc = read_http_progress(previous);
    if (rc) return rc;
    httpMayBeRunning_ = previous.downloading;
    if (httpMayBeRunning_) {
        start_http_recovery();
        return -EAGAIN;
    }

    rc = file_open();
    if (rc) return rc;
    uint32_t oldSize;
    rc = file_size(file, oldSize);
    if (!rc) {
        std::array<char, 96> remove{};
        int n = std::snprintf(remove.data(), remove.size(), "AT+CFSDFILE=0,\"%.*s\"\r\n",
            static_cast<int>(file.size()), file.data());
        rc = command_error(atEngine_.send_command({remove.data(), static_cast<size_t>(n)}, commandTimeoutMs));
        if (rc) return rc;
    } else if (rc != -EIO) {
        // A rejected size query is normal for a nonexistent file. A timeout
        // or malformed reply is not evidence that the old file is absent.
        return rc;
    }
    rc = file_close();
    if (rc) return rc;
    std::array<char, 400> command{};
    // Keep slow but advancing downloads viable. The independent no-progress
    // deadline cancels/restarts a stuck engine without waiting for this timeout.
    int n = std::snprintf(command.data(), command.size(), "AT+HTTPTOFS=\"%.*s\",\"/custapp/%.*s\",1000\r\n",
        static_cast<int>(url.size()), url.data(), static_cast<int>(file.size()), file.data());
    if (n < 0 || static_cast<size_t>(n) >= command.size()) return -ENAMETOOLONG;
    httpResult_ = {};
    httpMaxBytes_ = maxBytes;
    httpActive_ = true;
    httpMayBeRunning_ = true;
    rc = command_error(atEngine_.send_command({command.data(), static_cast<size_t>(n)}, commandTimeoutMs));
    if (rc && !httpResult_.finished) {
        // A missing OK does not establish whether HTTPTOFS started. Cleanup
        // reconciles native modem state before cancelling a possible request.
        httpActive_ = false;
        return rc;
    }
    return 0;
}

int sim7080::https_poll(http_result &result)
{
    if (!httpActive_) return -EINVAL;
    int rc = read_http_progress(result);
    if (rc) return rc;
    if (result.bytes > httpMaxBytes_ || result.received > httpMaxBytes_ || result.total > httpMaxBytes_)
        return -EFBIG;
    httpResult_ = result;
    if (result.finished) httpActive_ = false;
    return 0;
}

void sim7080::https_cleanup(bool cancel)
{
    if (file_close()) LOG_WRN("CFS close failed; session will be resynchronized before reuse");
    if (cancel && httpMayBeRunning_ && httpRecovery_==http_recovery::none) {
        http_result native;
        int rc = read_http_progress(native);
        if (!rc && (native.finished || !native.downloading)) {
            httpMayBeRunning_ = false;
        } else start_http_recovery();
    }
    httpActive_ = false;
    httpResult_ = {};
}
}
