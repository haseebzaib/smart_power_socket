#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "device_status.hpp"
#include "sensors/hlw811x.hpp"

namespace app_config
{

    static constexpr float defaultK1A = 3.0f;
    static constexpr float defaultK1B = 1.0f;
    static constexpr float defaultK2 = 1.064f;
    static constexpr int32_t ratioScale = 1000000;

    int init();

    hlw811x_resistor_ratio ratio_for_outlet(std::size_t outletIndex);

    int32_t k2_micro_for_outlet(std::size_t outletIndex);
    float k2_for_outlet(std::size_t outletIndex);
    int save_k2_micro(std::size_t outletIndex, int32_t k2Micro);

    bool relay_state_valid();
    uint8_t relay_on_mask();
    int save_relay_on_mask(uint8_t onMask);

}
