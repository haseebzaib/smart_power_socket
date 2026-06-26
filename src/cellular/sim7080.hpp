#pragma once

#include "array"
#include "span"
#include "optional"
#include "cstdint"
#include "cstddef"
#include "string_view"
#include "atomic"

#include "at_engine.hpp"
#include "hardware/uart_con.hpp"
#include "hardware/gpio_con.hpp"

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/devicetree.h>
#include <zephyr/device.h>


namespace cellular {

    class sim7080 {
        public:

        struct modemInformation {
             std::array<char,25> modelIdentification;
             std::array<char,25> pin;
             std::array<char,25> carrier;
             std::array<char,25> imei;
             std::array<char,25> simId;
             std::array<char,25> longitude;
             std::array<char,25> latitude;
             uint8_t networkRegistration;
             int8_t networkQuality;

        };


        sim7080(const device *uart,const gpio_dt_spec pwrKey,const gpio_flags_t pwrKeyFlags);
        ~sim7080();
        sim7080(const sim7080 &) = delete;
        sim7080 &operator=(const sim7080 &) = delete;


        int init();
        void loop();
        
        
        private:

        void pwrKeyPulse();




        const std::string_view pdpContext = "iot.1nce.net";
        
        /**set AT commands */
        const std::string_view atAT = "AT\r\n";
        const std::string_view atSetATCREG = "AT+CREG=1\r\n";
        const std::string_view atSetATCOPS = "AT+COPS=0\r\n";
        const std::string_view atSetATCGATT = "AT+CGATT=1\r\n";
        const std::string_view atSetATCNMP = "AT+CNMP=38\r\n";
        const std::string_view atSetATCMNB = "AT+CMNB=1\r\n";


        std::array<uint8_t, 1024> data_;
        uint32_t dataLength_;

        atEngine atEngine_;
        hardware::gpioCon pwrKey_;







    };






}

