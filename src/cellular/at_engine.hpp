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
        /**
         * Parses upto Okay
         */
        atResult sendCommand(std::string_view command, uint32_t timeoutMs);
        /**
         * Checks for URC and parses that
         */
        atResponse sendCommand(std::string_view command, std::string_view expectedURC, std::span<uint8_t> expectedResponse,
                               uint32_t timeoutMs);

        void sendCommandAsync(std::string_view command);

        atResponse process(std::span<uint8_t> recvCmdResp);

        urcMessage peekUrc(std::span<uint8_t> recvURC);

    private:
        static void rxCallback(const device *dev, void *userData);

        atResult parseOkayError();
        
        int available() const;
        bool contains(std::string_view pattern,uint32_t *getOffset);

        int pushsingle(const uint8_t &value);
        int popsingle( uint8_t &value);
        int pushBuf(std::span<const uint8_t> buf);
        int popBuf(std::span<uint8_t> buf);
        bool popRange(uint32_t offset,uint32_t length);

        std::array<uint8_t, 4096> rxBuffer;
        std::atomic<uint32_t> rd_index ;
        std::atomic<uint32_t> wr_index ;
        hardware::uartCon uart_;
    };

}
