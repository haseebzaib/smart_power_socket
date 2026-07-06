#pragma once

#include "array"
#include "span"
#include "optional"
#include "cstdint"
#include "cstddef"
#include <cstdio>
#include "string_view"
#include "atomic"
#include <charconv>
#include <system_error>
#include <algorithm>

#include "at_engine.hpp"
#include "hardware/uart_con.hpp"
#include "hardware/gpio_con.hpp"

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/devicetree.h>
#include <zephyr/device.h>

namespace cellular
{

    class sim7080
    {
    public:
        struct modemInformation
        {
            std::array<char, 25> modelIdentification;
            std::array<char, 25> pin;
            std::array<char, 25> carrier;
            std::array<char, 25> imei;
            std::array<char, 25> simId;
            std::array<char, 25> longitude;
            std::array<char, 25> latitude;
            std::array<char, 25> ipAddress;
            std::array<char, 32> smsNumber;
            std::array<char, 300> smsBody;
            int8_t networkRegistration;
            int8_t networkQuality;
            int8_t dataConnected;
            int8_t smsReceived;
        };

        sim7080(const device *uart, const gpio_dt_spec pwrKey, const gpio_flags_t pwrKeyFlags);
        ~sim7080();
        sim7080(const sim7080 &) = delete;
        sim7080 &operator=(const sim7080 &) = delete;

        struct smsMessage
        {
            std::array<char, 32> number;
            std::array<char, 300> body;
            bool valid;
        };

        int init();
        void loop(modemInformation &modemInfo);

        // Reads the oldest stored SMS into out, then deletes it from storage.
        // Returns true if a message was read, false if the queue is empty.
        bool read_next_sms(smsMessage &out);

        // Sends an SMS; automatically switches to long/concatenated send
        // (CMGSEX) when body exceeds a single SMS.
        bool send_sms(std::string_view number, std::string_view body);

    private:
        bool send_sms_single(std::string_view number, std::string_view body);
        bool send_sms_long(std::string_view number, std::string_view body);

        static constexpr std::size_t smsSingleMaxChars = 160;
        static constexpr std::size_t smsSegmentChars = 153;

        void get_network_quality();
        void get_network();
        void get_model_identification();
        void get_pin_status();
        void get_carrier();
        void get_imei();
        void get_simId();
        void get_location();
        void ensure_data_connection();

        static constexpr int ipCommandRetries = 3;
        static constexpr uint32_t defaultCommandTimeoutMs = 5000;
        static constexpr uint32_t locationCommandTimeoutMs = 30000;
        static constexpr uint32_t smsSubmitTimeoutMs = 60000;
        cellular::atEngine::atResult send_with_retry(std::string_view command,
                                                     int retries,
                                                     uint32_t timeoutMs);


        template <std::size_t N>
        bool copy_string_view_to_array(std::string_view src, std::array<char, N> &dst)
        {
            dst.fill('\0');

            if (src.size() >= dst.size())
            {
                return false; // not enough space for text + null terminator
            }

            std::copy_n(src.data(), src.size(), dst.data());

            dst[src.size()] = '\0';

            return true;
        }

        std::string_view trim(std::string_view text);
        bool parse_int(std::string_view text, int &value);
        void pwr_key_pulse();

        static constexpr const char *pdpContext = "iot.1nce.net";

        /**set AT commands */
        const std::string_view atAT = "AT\r\n";
        const std::string_view atATE0 = "ATE0\r\n";
        const std::string_view atSetATCREG = "AT+CREG=1\r\n";
        const std::string_view atSetATCOPS = "AT+COPS=0\r\n";
        const std::string_view atSetATCOPSFmt = "AT+COPS=3,0\r\n";
        const std::string_view atSetATCGATT = "AT+CGATT=1\r\n";
        static constexpr const char *atSetATCGDCONT = "AT+CGDCONT=1,\"IP\",\"%s\"\r\n";
        static constexpr const char *atSetATCNCFG = "AT+CNCFG=0,1,\"%s\"\r\n";
        const std::string_view atSetATCNMP = "AT+CNMP=38\r\n";
        const std::string_view atSetATCMNB = "AT+CMNB=1\r\n";
        const std::string_view atSetATCNACT = "AT+CNACT=0,2\r\n";

        /**SMS AT commands */
        const std::string_view atSetATCMGF = "AT+CMGF=1\r\n";
        const std::string_view atSetATCSMS = "AT+CSMS=1\r\n";
        const std::string_view atSetATCPMS = "AT+CPMS=\"SM\",\"SM\",\"SM\"\r\n";
        const std::string_view atGetATCMGLAll = "AT+CMGL=\"ALL\"\r\n";
        static constexpr const char *atSetATCMGD = "AT+CMGD=%d\r\n";
        static constexpr const char *atSetATCMGS = "AT+CMGS=\"%.*s\"\r\n";
        static constexpr const char *atSetATCMGSEX = "AT+CMGSEX=\"%.*s\",%d,%d,%d,%d\r\n";

        /**get AT commands */
        const std::string_view atGetATCSQ = "AT+CSQ\r\n";
        const std::string_view atGetATCOPS = "AT+COPS?\r\n";
        const std::string_view atGetATCREG = "AT+CREG?\r\n";
        const std::string_view atGetATCLBS = "AT+CLBS=4,0\r\n";
        const std::string_view atGetATCNACT = "AT+CNACT?\r\n";
        const std::string_view atGetATCPIN = "AT+CPIN?\r\n";
        const std::string_view atGetATCGSN = "AT+CGSN\r\n";
        const std::string_view atGetATCCID = "AT+CCID\r\n";

        static constexpr const char *networkRegistrationString[] = {
            "Not registered",
            "Registered home network",
            "Searching",
            "Registration denied",
            "Unknown",
            "Registered roaming"};

        static constexpr int networkRegistrationStringCount =
            sizeof(networkRegistrationString) / sizeof(networkRegistrationString[0]);

        modemInformation modemInformation_{};

        std::array<uint8_t, 1024> data_;
        uint32_t dataLength_;

        atEngine atEngine_;
        hardware::gpioCon pwrKey_;
    };

}
