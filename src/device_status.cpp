#include "device_status.hpp"

#include <cstdio>

#include <zephyr/kernel.h>

namespace device_status
{
    static constexpr int32_t utilityVoltagePresentThresholdMv = 50000;
    static constexpr int32_t outletLoadPresentThresholdMa = 5;
    static constexpr int32_t outletLoadPresentThresholdMw = 500;
    static constexpr int32_t frequencyLowBadCentihz = 5800;
    static constexpr int32_t frequencyHighBadCentihz = 6100;
    static constexpr int32_t powerFactorBadCenti = 80;
    static constexpr int32_t powerFactorRestoredCenti = 85;
    static constexpr int64_t millisecondsPerDay = 24LL * 60LL * 60LL * 1000LL;

    int32_t abs_i32(int32_t value)
    {
        if (value >= 0)
        {
            return value;
        }

        return static_cast<int32_t>(-static_cast<int64_t>(value));
    }

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
        return relay.get() == 0;
    }

    bool outlet_load_connected(const sensors::hlw811x::measurements &measurement)
    {
        return abs_i32(measurement.mA) >= outletLoadPresentThresholdMa ||
               abs_i32(measurement.apparentmW) >= outletLoadPresentThresholdMw;
    }

    bool average_frequency_centihz(std::span<const sensors::hlw811x::measurements, outletCount> measurements,
                                   int32_t &frequencyCentihz)
    {
        int64_t total = 0;
        uint32_t count = 0;

        for (const sensors::hlw811x::measurements &measurement : measurements)
        {
            if (measurement.mV >= utilityVoltagePresentThresholdMv && measurement.hZ > 0)
            {
                total += measurement.hZ;
                count++;
            }
        }

        if (count == 0)
        {
            return false;
        }

        frequencyCentihz = static_cast<int32_t>(total / count);
        return true;
    }

    bool frequency_bad(int32_t frequencyCentihz)
    {
        return frequencyCentihz < frequencyLowBadCentihz ||
               frequencyCentihz > frequencyHighBadCentihz;
    }

    bool power_factor_bad(const sensors::hlw811x::measurements &measurement)
    {
        return outlet_load_connected(measurement) &&
               abs_i32(measurement.pF) < powerFactorBadCenti;
    }

    bool power_factor_restored(const sensors::hlw811x::measurements &measurement)
    {
        return !outlet_load_connected(measurement) ||
               abs_i32(measurement.pF) >= powerFactorRestoredCenti;
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
                           uint32_t heartBeatDaysMilli,
                           uint32_t batteryMillivolts)
    {
        snapshot status{};

        status.utilityOn = utility_on(measurements);
        status.rssiDbm = modemInformation.networkQuality;
        status.latitude = modemInformation.latitude.data();
        status.longitude = modemInformation.longitude.data();
        status.upDaysMilli = uptime_days_milli(bootTimeMs);
        status.heartBeatDaysMilli = heartBeatDaysMilli;
        status.batteryMillivolts = batteryMillivolts;

        for (std::size_t i = 0; i < outletCount; ++i)
        {
            status.outletOn[i] = relays[i] != nullptr && outlet_on(*relays[i]);
            status.outletLoadConnected[i] = outlet_load_connected(measurements[i]);
            status.outletMeasurements[i] = measurements[i];
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
        const uint32_t batteryCentivolts = (status.batteryMillivolts + 5U) / 10U;

        return std::snprintf(buffer,
                             bufferSize,
                             "Utility=%s\n"
                             "Device=%d%d%d%d\n"
                             "Outlet=%d%d%d%d\n"
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
                             static_cast<unsigned int>(batteryCentivolts / 100U),
                             static_cast<unsigned int>(batteryCentivolts % 100U),
                             status.latitude,
                             status.longitude,
                             static_cast<int>(firmwareVersion.size()),
                             firmwareVersion.data(),
                             heartBeatDays,
                             upDays);
    }

    int format_alert_message(const snapshot &status,
                             std::string_view alert,
                             char *buffer,
                             std::size_t bufferSize)
    {
        char heartBeatDays[16]{};

        format_milli(heartBeatDays, sizeof(heartBeatDays), status.heartBeatDaysMilli);
        const uint32_t batteryCentivolts = (status.batteryMillivolts + 5U) / 10U;

        return std::snprintf(buffer,
                             bufferSize,
                             "Alert=%.*s\n"
                             "Utility=%s\n"
                             "Device=%d%d%d%d\n"
                             "Outlet=%d%d%d%d\n"
                             "RSL=%d\n"
                             "Batt=%u.%02u\n"
                             "Lat=%s\n"
                             "Lon=%s\n"
                             "FW=%.*s\n"
                             "HBD=%s",
                             static_cast<int>(alert.size()),
                             alert.data(),
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
                             static_cast<unsigned int>(batteryCentivolts / 100U),
                             static_cast<unsigned int>(batteryCentivolts % 100U),
                             status.latitude,
                             status.longitude,
                             static_cast<int>(firmwareVersion.size()),
                             firmwareVersion.data(),
                             heartBeatDays);
    }

    int format_signed_milli(char *buffer, std::size_t bufferSize, int32_t valueMilli)
    {
        const bool negative = valueMilli < 0;
        const uint32_t magnitude = negative
                                       ? static_cast<uint32_t>(-static_cast<int64_t>(valueMilli))
                                       : static_cast<uint32_t>(valueMilli);

        return std::snprintf(buffer,
                             bufferSize,
                             "%s%u.%03u",
                             negative ? "-" : "",
                             static_cast<unsigned int>(magnitude / 1000U),
                             static_cast<unsigned int>(magnitude % 1000U));
    }

    int format_signed_centi(char *buffer, std::size_t bufferSize, int32_t valueCenti)
    {
        const bool negative = valueCenti < 0;
        const uint32_t magnitude = negative
                                       ? static_cast<uint32_t>(-static_cast<int64_t>(valueCenti))
                                       : static_cast<uint32_t>(valueCenti);

        return std::snprintf(buffer,
                             bufferSize,
                             "%s%u.%02u",
                             negative ? "-" : "",
                             static_cast<unsigned int>(magnitude / 100U),
                             static_cast<unsigned int>(magnitude % 100U));
    }

    int format_outlet_report(const snapshot &status, char *buffer, std::size_t bufferSize)
    {
        char voltage[outletCount][16]{};
        char current[outletCount][16]{};
        char power[outletCount][16]{};

        for (std::size_t i = 0; i < outletCount; ++i)
        {
            format_signed_milli(voltage[i], sizeof(voltage[i]), status.outletMeasurements[i].mV);
            format_signed_milli(current[i], sizeof(current[i]), status.outletMeasurements[i].mA);
            format_signed_milli(power[i], sizeof(power[i]), status.outletMeasurements[i].mW);
        }

        return std::snprintf(buffer,
                             bufferSize,
                             "Outlet=%d%d%d%d\n"
                             "1=%sV,%sA,%sW\n"
                             "2=%sV,%sA,%sW\n"
                             "3=%sV,%sA,%sW\n"
                             "4=%sV,%sA,%sW",
                             status.outletOn[0] ? 1 : 0,
                             status.outletOn[1] ? 1 : 0,
                             status.outletOn[2] ? 1 : 0,
                             status.outletOn[3] ? 1 : 0,
                             voltage[0],
                             current[0],
                             power[0],
                             voltage[1],
                             current[1],
                             power[1],
                             voltage[2],
                             current[2],
                             power[2],
                             voltage[3],
                             current[3],
                             power[3]);
    }

    int format_single_outlet_report(const snapshot &status,
                                    std::size_t outletIndex,
                                    char *buffer,
                                    std::size_t bufferSize)
    {
        if (outletIndex >= outletCount)
        {
            return -1;
        }

        char voltage[16]{};
        char current[16]{};
        char power[16]{};

        format_signed_milli(voltage, sizeof(voltage), status.outletMeasurements[outletIndex].mV);
        format_signed_milli(current, sizeof(current), status.outletMeasurements[outletIndex].mA);
        format_signed_milli(power, sizeof(power), status.outletMeasurements[outletIndex].mW);

        return std::snprintf(buffer,
                             bufferSize,
                             "Outlet=%u\n"
                             "%u=%sV,%sA,%sW",
                             static_cast<unsigned int>(outletIndex + 1U),
                             static_cast<unsigned int>(outletIndex + 1U),
                             voltage,
                             current,
                             power);
    }

    int format_frequency_alert(std::string_view alert,
                               int32_t frequencyCentihz,
                               char *buffer,
                               std::size_t bufferSize)
    {
        char frequency[16]{};
        format_signed_centi(frequency, sizeof(frequency), frequencyCentihz);

        return std::snprintf(buffer,
                             bufferSize,
                             "Alert=%.*s\n"
                             "Freq=%sHz",
                             static_cast<int>(alert.size()),
                             alert.data(),
                             frequency);
    }

    int format_power_factor_alert(const snapshot &status,
                                  std::string_view alert,
                                  std::size_t outletIndex,
                                  char *buffer,
                                  std::size_t bufferSize)
    {
        if (outletIndex >= outletCount)
        {
            return -1;
        }

        char pf[16]{};
        char outlet[96]{};

        format_signed_centi(pf, sizeof(pf), status.outletMeasurements[outletIndex].pF);
        int outletLen = format_single_outlet_report(status, outletIndex, outlet, sizeof(outlet));
        if (outletLen <= 0 || static_cast<std::size_t>(outletLen) >= sizeof(outlet))
        {
            return -1;
        }

        return std::snprintf(buffer,
                             bufferSize,
                             "Alert=%.*s\n"
                             "PF=%s\n"
                             "%.*s",
                             static_cast<int>(alert.size()),
                             alert.data(),
                             pf,
                             outletLen,
                             outlet);
    }

}
