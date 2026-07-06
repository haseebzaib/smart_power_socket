#include "device_status.hpp"

#include <cstdio>

#include <zephyr/kernel.h>

namespace device_status
{
    static constexpr int32_t utilityVoltagePresentThresholdMv = 50000;
    static constexpr int32_t outletLoadPresentThresholdMa = 5;
    static constexpr int64_t millisecondsPerDay = 24LL * 60LL * 60LL * 1000LL;

    bool utility_on(std::span<const sensors::hlw811x::measurements, outletCount> measurements)
    {
        for (const sensors::hlw811x::measurements &measurement : measurements)
        {
            if (measurement.mV >= utilityVoltagePresentThresholdMv)
            {
                return true;
            }
        }

        return false;
    }

    bool outlet_on(hardware::gpioCon &relay)
    {
        return relay.get() > 0;
    }

    bool outlet_load_connected(const sensors::hlw811x::measurements &measurement)
    {
        return measurement.mA >= outletLoadPresentThresholdMa;
    }

    uint32_t uptime_days_milli(int64_t bootTimeMs)
    {
        const int64_t elapsedMs = k_uptime_get() - bootTimeMs;
        if (elapsedMs <= 0)
        {
            return 0;
        }

        return static_cast<uint32_t>((elapsedMs * 1000LL) / millisecondsPerDay);
    }

    snapshot make_snapshot(const cellular::sim7080::modemInformation &modemInformation,
                           std::span<hardware::gpioCon *, outletCount> relays,
                           std::span<const sensors::hlw811x::measurements, outletCount> measurements,
                           int64_t bootTimeMs,
                           uint32_t heartBeatDaysMilli)
    {
        snapshot status{};

        status.utilityOn = utility_on(measurements);
        status.rssiDbm = modemInformation.networkQuality;
        status.latitude = modemInformation.latitude.data();
        status.longitude = modemInformation.longitude.data();
        status.upDaysMilli = uptime_days_milli(bootTimeMs);
        status.heartBeatDaysMilli = heartBeatDaysMilli;
        status.batteryCentivolts = 0;

        for (std::size_t i = 0; i < outletCount; ++i)
        {
            status.outletOn[i] = relays[i] != nullptr && outlet_on(*relays[i]);
            status.outletLoadConnected[i] = outlet_load_connected(measurements[i]);
        }

        return status;
    }

    int format_milli(char *buffer, std::size_t bufferSize, uint32_t valueMilli)
    {
        return std::snprintf(buffer,
                             bufferSize,
                             "%u.%03u",
                             static_cast<unsigned int>(valueMilli / 1000U),
                             static_cast<unsigned int>(valueMilli % 1000U));
    }

    int format_status_message(const snapshot &status, char *buffer, std::size_t bufferSize)
    {
        char upDays[16]{};
        char heartBeatDays[16]{};

        format_milli(upDays, sizeof(upDays), status.upDaysMilli);
        format_milli(heartBeatDays, sizeof(heartBeatDays), status.heartBeatDaysMilli);

        return std::snprintf(buffer,
                             bufferSize,
                             "Utility=%s\n"
                             "Device=%d%d%d%d\n"
                             "Relay=%d%d%d%d\n"
                             "RSL=%d\n"
                             "Batt=%u.%02u\n"
                             "Lat=%s\n"
                             "Lon=%s\n"
                             "FW=%.*s\n"
                             "HBD=%s\n"
                             "UpDays=%s",
                             status.utilityOn ? "ON" : "OFF",
                             status.outletLoadConnected[0] ? 1 : 0,
                             status.outletLoadConnected[1] ? 1 : 0,
                             status.outletLoadConnected[2] ? 1 : 0,
                             status.outletLoadConnected[3] ? 1 : 0,
                             status.outletOn[0] ? 1 : 0,
                             status.outletOn[1] ? 1 : 0,
                             status.outletOn[2] ? 1 : 0,
                             status.outletOn[3] ? 1 : 0,
                             status.rssiDbm,
                             static_cast<unsigned int>(status.batteryCentivolts / 100U),
                             static_cast<unsigned int>(status.batteryCentivolts % 100U),
                             status.latitude,
                             status.longitude,
                             static_cast<int>(firmwareVersion.size()),
                             firmwareVersion.data(),
                             heartBeatDays,
                             upDays);
    }

}
