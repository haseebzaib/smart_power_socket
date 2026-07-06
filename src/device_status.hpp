#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

#include "hardware/gpio_con.hpp"
#include "sensors/hlw811x.hpp"
#include "cellular/sim7080.hpp"

namespace device_status
{

    static constexpr std::size_t outletCount = 4;
    static constexpr std::string_view firmwareVersion = "001.00";

    struct snapshot
    {
        bool utilityOn;
        std::array<bool, outletCount> outletOn;
        std::array<bool, outletCount> outletLoadConnected;
        int rssiDbm;
        const char *latitude;
        const char *longitude;
        uint32_t upDaysMilli;
        uint32_t heartBeatDaysMilli;
        uint16_t batteryCentivolts;
    };

    bool utility_on(std::span<const sensors::hlw811x::measurements, outletCount> measurements);
    bool outlet_on(hardware::gpioCon &relay);
    bool outlet_load_connected(const sensors::hlw811x::measurements &measurement);
    uint32_t uptime_days_milli(int64_t bootTimeMs);

    snapshot make_snapshot(const cellular::sim7080::modemInformation &modemInformation,
                           std::span<hardware::gpioCon *, outletCount> relays,
                           std::span<const sensors::hlw811x::measurements, outletCount> measurements,
                           int64_t bootTimeMs,
                           uint32_t heartBeatDaysMilli);

    int format_status_message(const snapshot &status, char *buffer, std::size_t bufferSize);

}
