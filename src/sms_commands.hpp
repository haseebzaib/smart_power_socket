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
        std::span<bool, device_status::outletCount> monitoredLoadConnected;
        std::span<bool, device_status::outletCount> monitoredLoadStateValid;
        int64_t bootTimeMs;
        uint32_t &heartBeatDaysMilli;
        int64_t &nextHeartbeatMs;
        uint32_t batteryMillivolts;
        bool inputAcPresent = false;
        std::span<uint32_t, device_status::outletCount> relayChangeSequence;
        uint32_t bootResetCause = 0;
        bool bootResetCauseValid = false;
    };

    void handle(const context &ctx);
    // GetOutletData and calibration-all each produce at most one reply per outlet.
    constexpr std::size_t maxCommandReplyMessages = device_status::outletCount;
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
