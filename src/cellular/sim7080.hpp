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
            std::array<char, 25> serviceProvider;
            std::array<char, 25> imei;
            std::array<char, 25> simId;
            std::array<char, 25> longitude;
            std::array<char, 25> latitude;
            int8_t networkRegistration;
            int8_t networkQuality;
        };

        sim7080(const device *uart, const gpio_dt_spec pwrKey, const gpio_flags_t pwrKeyFlags);
        ~sim7080();
        sim7080(const sim7080 &) = delete;
        sim7080 &operator=(const sim7080 &) = delete;

        int init();
        void loop(modemInformation &modemInfo);

    private:
        void get_network_quality();
        void get_network();
        void get_model_identification();
        void get_pin_status();
        void get_carrier();
        void get_service_provider();
        void get_imei();
        void get_simId();
        void get_location();
        void ensure_data_connection();

        static constexpr int ipCommandRetries = 3;
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

        /**get AT commands */
        const std::string_view atGetATCSQ = "AT+CSQ\r\n";
        const std::string_view atGetATCOPS = "AT+COPS?\r\n";
        const std::string_view atGetATCSPN = "AT+CSPN?\r\n";
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
