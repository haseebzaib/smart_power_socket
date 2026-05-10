#pragma once

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include "stddef.h"
#include "stdint.h"
#include <zephyr/devicetree.h>
#include <zephyr/device.h>
#include <errno.h>
#include <span>
#include <cstdint>



namespace hardware {

    class uartCon {

        public:
        using callBack = void (*)(const struct device *_dev, void *_userData);
        uartCon(const device *uart);
        uartCon(const device *uart,callBack _callBack,void *_userData);
        ~uartCon();
        uartCon(const uartCon &) = delete;
        uartCon &operator=(const uartCon &)=delete;

        
        int init();
        int configureBaud(uint32_t baudrate);
        int read(std::span<uint8_t>buffer, uint32_t timeOutMs);
        int flushRx();
        int readIntr();
        int write(std::span <const uint8_t> data);
        int writeIntr();


        private:
        const device *uart_;
        callBack callBack_;
        void *userData_;



    };




}
