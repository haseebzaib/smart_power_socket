#include "gpio_con.hpp"

namespace hardware
{

    gpioCon::gpioCon(const gpio_dt_spec gpio, const gpio_flags_t control_flags) : gpio_(gpio), flags_(control_flags)
    {
    }

    gpioCon::~gpioCon()
    {
    }

    int gpioCon::init()
    {
        if (!gpio_is_ready_dt(&gpio_))
        {
            return -ENODEV;
        }

        return gpio_pin_configure_dt(&gpio_, flags_);
    }

    int gpioCon::toggle()
    {

        return gpio_pin_toggle_dt(&gpio_);
    }
    int gpioCon::set(uint8_t set)
    {
        return gpio_pin_set_dt(&gpio_, set);
    }
    int gpioCon::get()
    {

        return gpio_pin_get_dt(&gpio_);
    }

}