#pragma once

#include <cstdint>
#include <cstddef>
#include <span>
#include <string_view>

#include "cellular/sim7080.hpp"
#include "device_status.hpp"
#include "hardware/gpio_con.hpp"
#include "sensors/hlw811x.hpp"

namespace sms_commands
{

    struct context
    {
        cellular::sim7080 &modem;
        const cellular::sim7080::modemInformation &modemInformation;
        sensors::hlw811x *energyMeters;
        const hlw811x_pga *meterPga;
        std::span<hardware::gpioCon *, device_status::outletCount> relays;
        std::span<const sensors::hlw811x::measurements, device_status::outletCount> measurements;
        int64_t bootTimeMs;
        uint32_t &heartBeatDaysMilli;
        uint32_t batteryMillivolts;
    };

    void handle(const context &ctx);
    bool send_status(const context &ctx, std::string_view recipient, std::string_view heading = {});
    bool send_alert(const context &ctx,
                    std::string_view recipient,
                    std::string_view alert,
                    bool includeOutletReport);
    bool send_device_alert(const context &ctx,
                           std::string_view recipient,
                           std::string_view alert,
                           std::size_t outletIndex);
    bool send_frequency_alert(const context &ctx,
                              std::string_view recipient,
                              std::string_view alert,
                              int32_t frequencyCentihz);
    bool send_power_factor_alert(const context &ctx,
                                 std::string_view recipient,
                                 std::string_view alert,
                                 std::size_t outletIndex);

}
