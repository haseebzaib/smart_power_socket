
#include "sim7080.hpp"

LOG_MODULE_REGISTER(sim7080, LOG_LEVEL_INF);

namespace cellular
{

    sim7080::sim7080(const device *uart, const gpio_dt_spec pwrKey, const gpio_flags_t pwrKeyFlags) : atEngine_(uart), pwrKey_(pwrKey, pwrKeyFlags)
    {
    }

    sim7080::~sim7080()
    {
    }

    int sim7080::init()
    {

        pwrKey_.init();
        atEngine_.init();

        if (atEngine_.send_command(atAT, 1000) != cellular::atEngine::atResult::OK)
        {
            LOG_DBG("GSM not responding, PWR Key pulse");
            pwr_key_pulse();
            k_msleep(5000);
            atEngine_.processRx();
        }

        
         atEngine_.send_command(atATE0, 1000);

        send_with_retry(atSetATCREG, ipCommandRetries, 1000);
        send_with_retry(atSetATCOPS, ipCommandRetries, 1000);
        send_with_retry(atSetATCOPSFmt, ipCommandRetries, 1000);
        send_with_retry(atSetATCGATT, ipCommandRetries, 1000);

        std::array<char, 64> cmd{};
        int len = std::snprintf(cmd.data(),
                                cmd.size(),
                                atSetATCGDCONT,
                                pdpContext);
        send_with_retry(std::string_view{cmd.data(), len}, ipCommandRetries, 1000);

        int len1 = std::snprintf(cmd.data(),
                                 cmd.size(),
                                 atSetATCNCFG,
                                 pdpContext);
        send_with_retry(std::string_view{cmd.data(), len1}, ipCommandRetries, 1000);

        send_with_retry(atSetATCNMP, ipCommandRetries, 1000);
        send_with_retry(atSetATCMNB, ipCommandRetries, 1000);
        send_with_retry(atSetATCNACT, ipCommandRetries, 1000);

        return 0;
    }

    void sim7080::loop(modemInformation &modemInfo)
    {

           atEngine_.processRx();
        get_network();
        get_network_quality();
        get_model_identification();
        get_pin_status();
        get_carrier();
        //k_msleep(1000);
        get_imei();
        //k_msleep(1000);
        get_simId();
        if (modemInformation_.networkRegistration == 1 || modemInformation_.networkRegistration == 5)
        {
            get_location();
            ensure_data_connection();
        }
        else
        {
            modemInformation_.dataConnected = 0;
            copy_string_view_to_array("0.0.0.0", modemInformation_.ipAddress);
        }

        modemInfo = modemInformation_;
    }

    void sim7080::get_network_quality()
    {
        cellular::atEngine::atResponse atResponse_;
        std::string_view prefix = "+CSQ:";

        atResponse_ = atEngine_.send_command(
            atGetATCSQ,
            prefix,
            std::span<uint8_t>(data_.data(), data_.size()),
            1000);

        if (atResponse_.result != cellular::atEngine::atResult::OK)
        {
            modemInformation_.networkQuality = -1;
            return;
        }

        LOG_DBG("CSQ: Inside okay result");

        std::string_view line{
            reinterpret_cast<const char *>(data_.data()),
            atResponse_.responseLength};

        line = trim(line);

        LOG_DBG("CSQ: Line Data %.*s",
                static_cast<int>(line.size()),
                line.data());

        if (!line.starts_with(prefix))
        {
            LOG_ERR("CSQ: Prefix not found");
            modemInformation_.networkQuality = -1;
            return;
        }

        std::string_view data = line.substr(prefix.size());
        data = trim(data);

        LOG_DBG("CSQ: Data %.*s",
                static_cast<int>(data.size()),
                data.data());

        std::size_t commaPos = data.find(',');

        if (commaPos == std::string_view::npos)
        {
            LOG_ERR("CSQ: comma not found");
            modemInformation_.networkQuality = -1;
            return;
        }

        std::string_view rssiText = data.substr(0, commaPos);
        std::string_view berText = data.substr(commaPos + 1);

        rssiText = trim(rssiText);
        berText = trim(berText);

        LOG_DBG("CSQ: RssiText %.*s",
                static_cast<int>(rssiText.size()),
                rssiText.data());

        LOG_DBG("CSQ: BerText %.*s",
                static_cast<int>(berText.size()),
                berText.data());

        int rssi = 0;
        int ber = 0;

        if (!parse_int(rssiText, rssi) || !parse_int(berText, ber))
        {
            LOG_ERR("CSQ: parse failed");
            modemInformation_.networkQuality = -1;
            return;
        }

        if (rssi >= 99)
        {
            modemInformation_.networkQuality = 0;
        }
        else
        {
            modemInformation_.networkQuality = (2 * rssi) - 113;
        }

        LOG_DBG("RSSI raw: %d, RSSI dBm: %d, BER: %d",
                rssi,
                modemInformation_.networkQuality,
                ber);
    }
    void sim7080::get_network()
    {
        cellular::atEngine::atResponse atResponse_{};
        std::string_view prefix = "+CREG:";

        atResponse_ = atEngine_.send_command(
            atGetATCREG,
            prefix,
            std::span<uint8_t>(data_.data(), data_.size()),
            1000);

        if (atResponse_.result != cellular::atEngine::atResult::OK)
        {
            modemInformation_.networkRegistration = -1;
            LOG_ERR("CREG: command failed");
            return;
        }

        std::string_view line{
            reinterpret_cast<const char *>(data_.data()),
            atResponse_.responseLength};

        line = trim(line);

        LOG_DBG("CREG Line: %.*s",
                static_cast<int>(line.size()),
                line.data());

        if (!line.starts_with(prefix))
        {
            modemInformation_.networkRegistration = -1;
            LOG_ERR("CREG: prefix not found");
            return;
        }

        std::string_view data = line.substr(prefix.size());
        data = trim(data);

        LOG_DBG("CREG Data: %.*s",
                static_cast<int>(data.size()),
                data.data());

        std::size_t commaPos = data.find(',');

        if (commaPos == std::string_view::npos)
        {
            modemInformation_.networkRegistration = -1;
            LOG_ERR("CREG: comma not found");
            return;
        }

        std::string_view nText = data.substr(0, commaPos);
        std::string_view statText = data.substr(commaPos + 1);

        nText = trim(nText);
        statText = trim(statText);

        LOG_DBG("CREG nText: %.*s",
                static_cast<int>(nText.size()),
                nText.data());

        LOG_DBG("CREG statText: %.*s",
                static_cast<int>(statText.size()),
                statText.data());

        int n = 0;
        int stat = 0;

        if (!parse_int(nText, n) || !parse_int(statText, stat))
        {
            modemInformation_.networkRegistration = -1;
            LOG_ERR("CREG: parse failed");
            return;
        }

        modemInformation_.networkRegistration = stat;

        LOG_DBG("CREG n: %d, stat: %d", n, stat);

        if (stat >= 0 && stat < networkRegistrationStringCount)
        {
            LOG_DBG("Network Status: %s", networkRegistrationString[stat]);
        }
        else
        {
            LOG_DBG("Network Status: unknown stat %d", stat);
        }
    }

    void sim7080::get_model_identification()
    {

        copy_string_view_to_array("SIM7080_LTD", modemInformation_.modelIdentification);
    }
    void sim7080::get_pin_status()
    {
        cellular::atEngine::atResponse atResponse_;
        std::string_view prefix = "+CPIN:";

        atResponse_ = atEngine_.send_command(
            atGetATCPIN,
            prefix,
            std::span<uint8_t>(data_.data(), data_.size()),
            1000);

        if (atResponse_.result != cellular::atEngine::atResult::OK)
        {
            copy_string_view_to_array("Error", modemInformation_.pin);
            return;
        }

        LOG_DBG("CPIN: Inside okay result");

        std::string_view line{
            reinterpret_cast<const char *>(data_.data()),
            atResponse_.responseLength};

        line = trim(line);

        LOG_DBG("CPIN: Line Data %.*s",
                static_cast<int>(line.size()),
                line.data());

        if (!line.starts_with(prefix))
        {
            LOG_ERR("CPIN: Prefix not found");
            modemInformation_.networkQuality = -1;
            return;
        }

        std::string_view data = line.substr(prefix.size());
        data = trim(data);

        LOG_DBG("CPIN: Data %.*s",
                static_cast<int>(data.size()),
                data.data());

        copy_string_view_to_array(data, modemInformation_.pin);
    }
    cellular::atEngine::atResult sim7080::send_with_retry(std::string_view command,
                                                          int retries,
                                                          uint32_t timeoutMs)
    {
        cellular::atEngine::atResult result = cellular::atEngine::atResult::Timeout;

        for (int attempt = 0; attempt < retries; ++attempt)
        {
            result = atEngine_.send_command(command, timeoutMs);

            if (result == cellular::atEngine::atResult::OK)
            {
                return result;
            }

            LOG_WRN("Cmd failed (attempt %d/%d), retrying", attempt + 1, retries);
            k_msleep(500);
        }

        return result;
    }

    void sim7080::ensure_data_connection()
    {
        cellular::atEngine::atResponse atResponse_{};
        std::string_view prefix = "+CNACT:";

        atResponse_ = atEngine_.send_command(
            atGetATCNACT,
            prefix,
            std::span<uint8_t>(data_.data(), data_.size()),
            1000);

        if (atResponse_.result != cellular::atEngine::atResult::OK)
        {
            LOG_ERR("CNACT?: query failed");
            modemInformation_.dataConnected = 0;
            copy_string_view_to_array("0.0.0.0", modemInformation_.ipAddress);
            return;
        }

        std::string_view line = trim(std::string_view{
            reinterpret_cast<const char *>(data_.data()),
            atResponse_.responseLength});

        if (!line.starts_with(prefix))
        {
            LOG_ERR("CNACT?: prefix not found");
            modemInformation_.dataConnected = 0;
            copy_string_view_to_array("0.0.0.0", modemInformation_.ipAddress);
            return;
        }

        // +CNACT: <pdpidx>,<status>,<address>  -> status is the field after the first comma
        std::string_view data = trim(line.substr(prefix.size()));

        std::size_t firstComma = data.find(',');
        if (firstComma == std::string_view::npos)
        {
            LOG_ERR("CNACT?: unexpected format");
            return;
        }

        std::string_view rest = trim(data.substr(firstComma + 1));
        std::size_t secondComma = rest.find(',');
        std::string_view statusText =
            trim(secondComma == std::string_view::npos ? rest : rest.substr(0, secondComma));

        int status = -1;
        if (!parse_int(statusText, status))
        {
            LOG_ERR("CNACT?: status parse failed");
            return;
        }

        LOG_DBG("CNACT status: %d", status);

        // Extract the quoted IP address, if present
        std::size_t firstQuote = line.find('"');
        std::size_t lastQuote = line.rfind('"');

        if (firstQuote != std::string_view::npos &&
            lastQuote != std::string_view::npos &&
            lastQuote > firstQuote + 1)
        {
            std::string_view ip =
                line.substr(firstQuote + 1, lastQuote - firstQuote - 1);
            copy_string_view_to_array(ip, modemInformation_.ipAddress);
        }
        else
        {
            copy_string_view_to_array("0.0.0.0", modemInformation_.ipAddress);
        }

        // 1 = activated, 2 = in operation -> connection is up
        if (status == 1 || status == 2)
        {
            modemInformation_.dataConnected = 1;
            return;
        }

        // Deactivated -> re-run activation
        modemInformation_.dataConnected = 0;
        LOG_WRN("Data context down, re-activating");

        std::array<char, 64> cmd{};
        int len = std::snprintf(cmd.data(),
                                cmd.size(),
                                atSetATCNCFG,
                                pdpContext);
        send_with_retry(std::string_view{cmd.data(), len}, ipCommandRetries, 1000);
        send_with_retry(atSetATCNACT, ipCommandRetries, 1000);
    }

    void sim7080::get_carrier()
    {
        cellular::atEngine::atResponse atResponse_{};
        std::string_view prefix = "+COPS:";

        atResponse_ = atEngine_.send_command(
            atGetATCOPS,
            prefix,
            std::span<uint8_t>(data_.data(), data_.size()),
            1000);

        if (atResponse_.result != cellular::atEngine::atResult::OK)
        {
            copy_string_view_to_array("Error", modemInformation_.carrier);
            return;
        }

        std::string_view line = trim(std::string_view{
            reinterpret_cast<const char *>(data_.data()),
            atResponse_.responseLength});

        LOG_DBG("COPS: Line Data %.*s",
                static_cast<int>(line.size()),
                line.data());

        if (!line.starts_with(prefix))
        {
            copy_string_view_to_array("Unknown", modemInformation_.carrier);
            return;
        }

        // +COPS: <mode>,<format>,"<oper>",<AcT> -> take the quoted operator name
        std::size_t firstQuote = line.find('"');
        std::size_t lastQuote = line.rfind('"');

        if (firstQuote == std::string_view::npos ||
            lastQuote == std::string_view::npos ||
            lastQuote <= firstQuote + 1)
        {
            // Not registered or name not available
            copy_string_view_to_array("Unknown", modemInformation_.carrier);
            return;
        }

        std::string_view carrier =
            line.substr(firstQuote + 1, lastQuote - firstQuote - 1);

        LOG_DBG("COPS: carrier %.*s",
                static_cast<int>(carrier.size()),
                carrier.data());

        copy_string_view_to_array(carrier, modemInformation_.carrier);
    }

    void sim7080::get_imei()
    {
        cellular::atEngine::atResponse atResponse_;
        std::string_view prefix = "";

        atResponse_ = atEngine_.send_command(
            atGetATCGSN,
            prefix,
            std::span<uint8_t>(data_.data(), data_.size()),
            1000);

        if (atResponse_.result != cellular::atEngine::atResult::OK)
        {
            copy_string_view_to_array("Error", modemInformation_.imei);
            return;
        }

        LOG_DBG("CGSN: Inside okay result");

        std::string_view line{
            reinterpret_cast<const char *>(data_.data()),
            atResponse_.responseLength};

        line = trim(line);

        LOG_DBG("CGSN: Line Data %.*s",
                static_cast<int>(line.size()),
                line.data());

        copy_string_view_to_array(line, modemInformation_.imei);
    }
    void sim7080::get_simId()
    {
        cellular::atEngine::atResponse atResponse_;
        std::string_view prefix = "";

        atResponse_ = atEngine_.send_command(
            atGetATCCID,
            prefix,
            std::span<uint8_t>(data_.data(), data_.size()),
            1000);

        if (atResponse_.result != cellular::atEngine::atResult::OK)
        {
            copy_string_view_to_array("Error", modemInformation_.simId);
            return;
        }

        LOG_DBG("CCID: Inside okay result");

        std::string_view line{
            reinterpret_cast<const char *>(data_.data()),
            atResponse_.responseLength};

        line = trim(line);

        LOG_DBG("CCID: Line Data %.*s",
                static_cast<int>(line.size()),
                line.data());

        copy_string_view_to_array(line, modemInformation_.simId);
    }

    void sim7080::get_location()
    {
        cellular::atEngine::atResponse atResponse_;
        std::string_view prefix = "+CNACT:";
        std::string_view prefixLoc = "+CLBS:";

        atResponse_ = atEngine_.send_command(
            atGetATCNACT,
            prefix,
            std::span<uint8_t>(data_.data(), data_.size()),
            1000);

        if (atResponse_.result != cellular::atEngine::atResult::OK)
        {
            return;
        }

        std::string_view line{
            reinterpret_cast<const char *>(data_.data()),
            atResponse_.responseLength};

        line = trim(line);

        LOG_DBG("CNACT: Line Data %.*s",
                static_cast<int>(line.size()),
                line.data());

        if (!line.starts_with(prefix))
        {
            LOG_ERR("CNACT: Prefix not found");
            return;
        }

        std::string_view data = line.substr(prefix.size());
        data = trim(data);

        LOG_DBG("CNACT: Data %.*s",
                static_cast<int>(data.size()),
                data.data());

        std::size_t commaPos = data.find(',');

        if (commaPos == std::string_view::npos)
        {
            LOG_ERR("CNACT: comma not found");
            return;
        }

        std::string_view pdpidx = data.substr(0, commaPos);
        std::string_view nextcomma = data.substr(commaPos + 1);

        std::size_t commaPosNext = nextcomma.find(',');

        std::string_view statusx = nextcomma.substr(0, commaPosNext);

        pdpidx = trim(pdpidx);
        statusx = trim(statusx);

        LOG_DBG("CNACT: pdpidx %.*s",
                static_cast<int>(pdpidx.size()),
                pdpidx.data());

        LOG_DBG("CNACT: statusx %.*s",
                static_cast<int>(statusx.size()),
                statusx.data());

        int pdpidxData = 0;
        int statuxData = 0;

        if (!parse_int(pdpidx, pdpidxData) || !parse_int(statusx, statuxData))
        {

            LOG_ERR("CNACT: parse failed");
            return;
        }

        if (pdpidxData == 0 && statuxData == 1)
        {

            atResponse_ = atEngine_.send_command(
                atGetATCLBS,
                prefixLoc,
                std::span<uint8_t>(data_.data(), data_.size()),
                1000);

            if (atResponse_.result != cellular::atEngine::atResult::OK)
            {
                return;
            }

            std::string_view line1{
                reinterpret_cast<const char *>(data_.data()),
                atResponse_.responseLength};

            line1 = trim(line1);

            LOG_DBG("CLBS: Line Data %.*s",
                    static_cast<int>(line1.size()),
                    line1.data());

            if (!line1.starts_with(prefixLoc))
            {
                LOG_ERR("CLBS: Prefix not found");
                return;
            }

            std::string_view data1 = line1.substr(prefixLoc.size());
            data1 = trim(data1);

            LOG_DBG("CLBS: Data %.*s",
                    static_cast<int>(data1.size()),
                    data1.data());

            std::size_t commaPos1 = data1.find(',');

            if (commaPos1 == std::string_view::npos)
            {
                LOG_ERR("CLBS: comma not found");
                return;
            }
            std::string_view data2 = data1.substr(commaPos1 + 1);

            std::size_t commaPos2 = data2.find(',');

            std::string_view longitudeText = data2.substr(0, commaPos2);
            std::string_view data3 = data2.substr(commaPos2 + 1);

            std::size_t commaPos3 = data3.find(',');
            std::string_view latitudeText = data3.substr(0, commaPos3);

            longitudeText = trim(longitudeText);
            latitudeText = trim(latitudeText);
            LOG_DBG("CLBS: longitudeText %.*s",
                    static_cast<int>(longitudeText.size()),
                    longitudeText.data());

            LOG_DBG("CLBS: latitudeText %.*s",
                    static_cast<int>(latitudeText.size()),
                    latitudeText.data());

            if (!copy_string_view_to_array(longitudeText, modemInformation_.longitude))
            {
                LOG_WRN("Longitude buffer too small");
            }

            if (!copy_string_view_to_array(latitudeText, modemInformation_.latitude))
            {
                LOG_WRN("Latitude buffer too small");
            }

            LOG_DBG("Longitude: %s", modemInformation_.longitude.data());
            LOG_DBG("Latitude: %s", modemInformation_.latitude.data());
        }
    }

    std::string_view sim7080::trim(std::string_view text)
    {
        while (!text.empty() &&
               (text.front() == ' ' || text.front() == '\r' || text.front() == '\n' || text.front() == '\t'))
        {
            text.remove_prefix(1);
        }

        while (!text.empty() &&
               (text.back() == ' ' || text.back() == '\r' || text.back() == '\n' || text.back() == '\t'))
        {
            text.remove_suffix(1);
        }

        return text;
    }

    bool sim7080::parse_int(std::string_view text, int &value)
    {
        text = trim(text);

        const char *begin = text.data();
        const char *end = text.data() + text.size();

        std::from_chars_result result = std::from_chars(begin, end, value);

        LOG_DBG("parse_int text: %.*s",
                static_cast<int>(text.size()),
                text.data());

        LOG_DBG("parse_int ec: %d, consumed: %d/%d",
                static_cast<int>(result.ec),
                static_cast<int>(result.ptr - begin),
                static_cast<int>(text.size()));

        return result.ec == std::errc{} && result.ptr == end;
    }

    void sim7080::pwr_key_pulse()
    {

        pwrKey_.set(1);
        k_msleep(4000);
        pwrKey_.set(0);
        k_msleep(2000);
    }

}