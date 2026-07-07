#pragma once

#include <array>
#include <cstddef>
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
        std::array<sensors::hlw811x::measurements, outletCount> outletMeasurements;
        int rssiDbm;
        const char *latitude;
        const char *longitude;
        uint32_t upDaysMilli;
        uint32_t heartBeatDaysMilli;
        uint32_t batteryMillivolts;
    };

    bool utility_on(std::span<const sensors::hlw811x::measurements, outletCount> measurements);
    bool outlet_on(hardware::gpioCon &relay);
    bool outlet_load_connected(const sensors::hlw811x::measurements &measurement);
    bool average_frequency_centihz(std::span<const sensors::hlw811x::measurements, outletCount> measurements,
                                   int32_t &frequencyCentihz);
    bool frequency_bad(int32_t frequencyCentihz);
    bool power_factor_bad(const sensors::hlw811x::measurements &measurement);
    bool power_factor_restored(const sensors::hlw811x::measurements &measurement);
    uint32_t uptime_days_milli(int64_t bootTimeMs);

    snapshot make_snapshot(const cellular::sim7080::modemInformation &modemInformation,
                           std::span<hardware::gpioCon *, outletCount> relays,
                           std::span<const sensors::hlw811x::measurements, outletCount> measurements,
                           int64_t bootTimeMs,
                           uint32_t heartBeatDaysMilli,
                           uint32_t batteryMillivolts);

    int format_status_message(const snapshot &status, char *buffer, std::size_t bufferSize);
    int format_alert_message(const snapshot &status,
                             std::string_view alert,
                             char *buffer,
                             std::size_t bufferSize);
    int format_outlet_report(const snapshot &status, char *buffer, std::size_t bufferSize);
    int format_single_outlet_report(const snapshot &status,
                                    std::size_t outletIndex,
                                    char *buffer,
                                    std::size_t bufferSize);
    int format_outlet_detail_report(const snapshot &status,
                                    std::size_t outletIndex,
                                    char *buffer,
                                    std::size_t bufferSize);
    int format_frequency_alert(std::string_view alert,
                               int32_t frequencyCentihz,
                               char *buffer,
                               std::size_t bufferSize);
    int format_power_factor_alert(const snapshot &status,
                                  std::string_view alert,
                                  std::size_t outletIndex,
                                  char *buffer,
                                  std::size_t bufferSize);

}
