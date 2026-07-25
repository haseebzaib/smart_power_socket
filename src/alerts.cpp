#include "alerts.hpp"

#include <cstddef>

#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(alerts, LOG_LEVEL_INF);

namespace alerts
{
    void devicePowerMonitor::outletTracker::reset()
    {
        currentMa.fill(0);
        activePowerMw.fill(0);
        nextSample = 0;
        sampleCount = 0;
        currentSum = 0;
        activePowerSum = 0;
        lastState = state::Unknown;
        candidateState = state::Unknown;
        candidateCount = 0;
    }

    void devicePowerMonitor::outletTracker::add(int32_t current, int32_t activePower)
    {
        const int32_t normalizedCurrent = current > 0 ? current : 0;
        const int32_t normalizedPower = activePower > 0 ? activePower : 0;

        if (sampleCount == sampleWindow)
        {
            currentSum -= currentMa[nextSample];
            activePowerSum -= activePowerMw[nextSample];
        }
        else
        {
            sampleCount++;
        }

        currentMa[nextSample] = normalizedCurrent;
        activePowerMw[nextSample] = normalizedPower;
        currentSum += normalizedCurrent;
        activePowerSum += normalizedPower;
        nextSample = (nextSample + 1U) % sampleWindow;
    }

    bool devicePowerMonitor::outletTracker::ready() const
    {
        return sampleCount == sampleWindow;
    }

    int32_t devicePowerMonitor::outletTracker::average_current_ma() const
    {
        return sampleCount == 0 ? 0 : static_cast<int32_t>(currentSum / sampleCount);
    }

    int32_t devicePowerMonitor::outletTracker::average_active_power_mw() const
    {
        return sampleCount == 0 ? 0 : static_cast<int32_t>(activePowerSum / sampleCount);
    }

    bool utilityPowerMonitor::process(const sms_commands::context &ctx, std::string_view recipient)
    {
        const bool utilityOn = device_status::utility_on(ctx.measurements);

        if (lastState_ == state::Unknown)
        {
            if (utilityOn)
            {
                lastState_ = state::On;
                return false;
            }

            bool sent = sms_commands::send_alert(ctx,
                                                 recipient,
                                                 "Utility power lost",
                                                 false);
            if (sent)
            {
                lastState_ = state::Off;
            }
            return sent;
        }

        if (lastState_ == state::On && !utilityOn)
        {
            bool sent = sms_commands::send_alert(ctx,
                                                 recipient,
                                                 "Utility power lost",
                                                 false);
            if (sent)
            {
                lastState_ = state::Off;
            }
            return sent;
        }

        if (lastState_ == state::Off && utilityOn)
        {
            bool sent = sms_commands::send_alert(ctx,
                                                 recipient,
                                                 "Utility power restored",
                                                 true);
            if (sent)
            {
                lastState_ = state::On;
            }
            return sent;
        }

        return false;
    }

    bool devicePowerMonitor::process(const sms_commands::context &ctx, std::string_view recipient)
    {
        if (!device_status::utility_on(ctx.measurements))
        {
            for (outletTracker &outlet : outlets_)
            {
                outlet.reset();
            }
            return false;
        }

        bool anySent = false;

        for (std::size_t i = 0; i < device_status::outletCount; ++i)
        {
            const bool relayOn =
                i < ctx.relays.size() &&
                ctx.relays[i] != nullptr &&
                device_status::outlet_on(*ctx.relays[i]);

            // An unpowered outlet cannot have an unexpected device power loss.
            // Reset its baseline so an intentional DeviceOff/DeviceOn cycle
            // does not generate either a false "lost" or "restored" alert.
            if (!relayOn)
            {
                outlets_[i].reset();
                continue;
            }

            outletTracker &outlet = outlets_[i];
            outlet.add(ctx.measurements[i].mA, ctx.measurements[i].mW);

            if (!outlet.ready())
            {
                continue;
            }

            const int32_t averageCurrentMa = outlet.average_current_ma();
            const int32_t averageActivePowerMw = outlet.average_active_power_mw();

            if (outlet.lastState == state::Unknown)
            {
                outlet.lastState =
                    device_status::outlet_load_connected(averageCurrentMa, averageActivePowerMw)
                        ? state::Connected
                        : state::Disconnected;
                continue;
            }

            state candidate = state::Unknown;
            if (outlet.lastState == state::Connected &&
                device_status::outlet_load_clearly_disconnected(averageCurrentMa,
                                                                averageActivePowerMw))
            {
                candidate = state::Disconnected;
            }
            else if (outlet.lastState == state::Disconnected &&
                     device_status::outlet_load_connected(averageCurrentMa,
                                                          averageActivePowerMw))
            {
                candidate = state::Connected;
            }

            if (candidate == state::Unknown)
            {
                outlet.candidateState = state::Unknown;
                outlet.candidateCount = 0;
                continue;
            }

            if (outlet.candidateState != candidate)
            {
                outlet.candidateState = candidate;
                outlet.candidateCount = 1;
            }
            else if (outlet.candidateCount < confirmationSamples)
            {
                outlet.candidateCount++;
            }

            if (outlet.candidateCount < confirmationSamples)
            {
                continue;
            }

            const char *alert =
                candidate == state::Connected ? "Device power restored" : "Device power lost";
            bool sent = sms_commands::send_device_alert(ctx, recipient, alert, i);
            if (sent)
            {
                outlet.lastState = candidate;
                outlet.candidateState = state::Unknown;
                outlet.candidateCount = 0;
                anySent = true;
            }
        }

        return anySent;
    }

    bool frequencyMonitor::process(const sms_commands::context &ctx, std::string_view recipient)
    {
        if (!device_status::utility_on(ctx.measurements))
        {
            return false;
        }

        int32_t frequencyCentihz = 0;
        if (!device_status::average_frequency_centihz(ctx.measurements, frequencyCentihz))
        {
            return false;
        }

        const bool bad = device_status::frequency_bad(frequencyCentihz);

        if (lastState_ == state::Unknown)
        {
            if (!bad)
            {
                lastState_ = state::Good;
                return false;
            }

            bool sent = sms_commands::send_frequency_alert(ctx,
                                                           recipient,
                                                           "Frequency bad",
                                                           frequencyCentihz);
            if (sent)
            {
                lastState_ = state::Bad;
            }
            return sent;
        }

        if (lastState_ == state::Good && bad)
        {
            bool sent = sms_commands::send_frequency_alert(ctx,
                                                           recipient,
                                                           "Frequency bad",
                                                           frequencyCentihz);
            if (sent)
            {
                lastState_ = state::Bad;
            }
            return sent;
        }

        if (lastState_ == state::Bad && !bad)
        {
            bool sent = sms_commands::send_frequency_alert(ctx,
                                                           recipient,
                                                           "Frequency restored",
                                                           frequencyCentihz);
            if (sent)
            {
                lastState_ = state::Good;
            }
            return sent;
        }

        return false;
    }

    bool powerFactorMonitor::process(const sms_commands::context &ctx, std::string_view recipient)
    {
        if (!device_status::utility_on(ctx.measurements))
        {
            return false;
        }

        bool anySent = false;

        for (std::size_t i = 0; i < device_status::outletCount; ++i)
        {
            const bool bad = device_status::power_factor_bad(ctx.measurements[i]);
            const bool restored = device_status::power_factor_restored(ctx.measurements[i]);

            if (lastState_[i] == state::Unknown)
            {
                if (!bad)
                {
                    lastState_[i] = state::Good;
                    continue;
                }

                bool sent = sms_commands::send_power_factor_alert(ctx,
                                                                  recipient,
                                                                  "Power factor bad",
                                                                  i);
                if (sent)
                {
                    lastState_[i] = state::Bad;
                    anySent = true;
                }
                continue;
            }

            if (lastState_[i] == state::Good && bad)
            {
                bool sent = sms_commands::send_power_factor_alert(ctx,
                                                                  recipient,
                                                                  "Power factor bad",
                                                                  i);
                if (sent)
                {
                    lastState_[i] = state::Bad;
                    anySent = true;
                }
                continue;
            }

            if (lastState_[i] == state::Bad && restored)
            {
                bool sent = sms_commands::send_power_factor_alert(ctx,
                                                                  recipient,
                                                                  "Power factor restored",
                                                                  i);
                if (sent)
                {
                    lastState_[i] = state::Good;
                    anySent = true;
                }
            }
        }

        return anySent;
    }

}
