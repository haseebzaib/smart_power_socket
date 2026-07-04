#pragma once

#include "array"
#include "span"
#include "optional"
#include "cstdint"
#include "cstddef"
#include "string_view"
#include "atomic"

#include "hardware/uart_con.hpp"

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/devicetree.h>
#include <zephyr/device.h>
#include <zephyr/logging/log.h>



namespace cellular
{


    class atEngine
    {

    public:
        enum class atResult
        {
            OK,
            Error,
            Timeout,
            Busy,
            InvalidResponse,
            BufferOverflow,
            URCRecv,
            Unknown
        }; 
        enum class urcResult
        {
            Available,
            NotAvailable,
            BufferTooSmall,
            Invalid
        };
        struct atResponse
        {
            atResult result;
            size_t responseLength;
            bool hasResponse;
        };

        struct urcMessage
        {
            urcResult result;
            size_t length;

            bool available() const
            {
               return result == urcResult::Available;
            }

        };

        atEngine(const device *uart);
        ~atEngine();
        atEngine(const atEngine &) = delete;
        atEngine &operator=(const atEngine &) = delete;

        int init();

        void processRx();
        void onRxByte(uint8_t byte);
        /**
         * Parses upto Okay
         */
        atResult send_command(std::string_view command, uint32_t timeoutMs);
        /**
         * Checks for URC and parses that
         */
        atResponse send_command(std::string_view command, std::string_view expectedURC, std::span<uint8_t> expectedResponse,
                               uint32_t timeoutMs);
        

    private:
        bool save_response(std::string_view line);
        void handle_line(std::string_view line);
        static std::string_view strip_line_ending(std::string_view text);
  
        
        

        int available() const;
        int push_single(const uint8_t &value);
        int pop_single( uint8_t &value);
        int push_buf(std::span<const uint8_t> buf);
        int pop_buf(std::span<uint8_t> buf);
        bool pop_range(uint32_t offset,uint32_t length);

        static void rxCallback(const device *dev, void *userData);



        struct pendingCommand {
            bool active = false;
            bool done = false;
            bool responseSeen = false;
            bool collectingResponse = false;

            std::string_view expectedPrefix{};
            std::string_view commandText{};
            atResult result = atResult::Timeout;
        };

        pendingCommand currentCommand{};

        std::array<uint8_t, 4096> rxBuffer;
        std::array<uint8_t,1024> commandResponse;
        uint32_t commandResponseLength;
        std::array<uint8_t,1024> lineBuffer;
        uint32_t lineBufferLength;
        std::atomic<uint32_t> rd_index ;
        std::atomic<uint32_t> wr_index ;
        hardware::uartCon uart_;
    };

}
