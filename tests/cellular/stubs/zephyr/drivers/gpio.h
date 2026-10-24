#pragma once
#include <mock.hpp>
using gpio_flags_t=uint32_t;
struct gpio_dt_spec {const device *port=nullptr;uint32_t pin=0;gpio_flags_t dt_flags=0;};
