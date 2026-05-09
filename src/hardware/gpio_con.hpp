#pragma once

#include "stddef.h"
#include "stdint.h"
#include <zephyr/devicetree.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <errno.h>


namespace hardware 
{

    class gpioCon {

        public:
        gpioCon(const gpio_dt_spec gpio,const gpio_flags_t control_flags);
        ~gpioCon();
        gpioCon(const gpioCon &) = delete;
        gpioCon &operator=(const gpioCon &)=delete;

        int init();
        int toggle();

        int set(uint8_t set);
        int get();


        private:
        gpio_dt_spec gpio_;
        gpio_flags_t flags_;

    };



}