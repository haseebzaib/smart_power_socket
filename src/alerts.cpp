#include "alerts.hpp"

#include <cstddef>

#include <zephyr/logging/log.h>

#include "app_config.hpp"

LOG_MODULE_REGISTER(alerts, LOG_LEVEL_INF);

namespace alerts
{
    void devicePowerMonitor::outletTracker::reset()
    {
        activePowerMw.fill(0);
        nextSample = 0;
        sampleCount = 0;
        activePowerSum = 0;
        lastState = state::Unknown;
        candidateState = state::Unknown;
        candidateCount = 0;
    }

    void devicePowerMonitor::outletTracker::add(int32_t activePower)
    {
        const int32_t normalizedPower = activePower > 0 ? activePower : 0;

        if (sampleCount == sampleWindow)
        {
            activePowerSum -= activePowerMw[nextSample];
        }
        else
        {
            sampleCount++;
        }

        activePowerMw[nextSample] = normalizedPower;
        activePowerSum += normalizedPower;
        nextSample = (nextSample + 1U) % sampleWindow;
    }

    bool devicePowerMonitor::outletTracker::ready() const
    {
        return sampleCount == sampleWindow;
    }

    int32_t devicePowerMonitor::outletTracker::average_active_power_mw() const
    {
        return sampleCount == 0 ? 0 : static_cast<int32_t>(activePowerSum / sampleCount);
    }

    bool utilityPowerMonitor::process(const sms_commands::context &ctx, std::string_view recipient)
    {
        const bool utilityOn = device_status::utility_on(ctx.measurements);

        if (utilityOn)
        {
            fuseCandidateCount_ = 0;
            if (lastState_ == state::Unknown)
            {
                lastState_ = state::On;
                return false;
            }

            if (lastState_ == state::On)
            {
                return false;
            }

            const bool sent = sms_commands::send_alert(ctx,
                                                        recipient,
                                                        "Utility power restored",
                                                        true);
            if (sent)
            {
                lastState_ = state::On;
            }
            return sent;
        }

        // The optocoupler observes AC before the fuse; metering observes
        // the sockets after it. Confirm the mismatch across multiple polls.
        if (ctx.inputAcPresent)
        {
            if (fuseCandidateCount_ < fuseConfirmationSamples)
            {
                ++fuseCandidateCount_;
            }
            if (fuseCandidateCount_ < fuseConfirmationSamples ||
                lastState_ == state::FuseSuspected)
            {
                return false;
            }

            const bool sent = sms_commands::send_alert(ctx,
                                                        recipient,
                                                        "Utility power lost - fuse probably blown",
                                                        false);
            if (sent)
            {
                lastState_ = state::FuseSuspected;
            }
            return sent;
        }

        fuseCandidateCount_ = 0;
        if (lastState_ == state::Off || lastState_ == state::FuseSuspected)
        {
            // Do not repeat a loss SMS until socket voltage is restored.
            return false;
        }

        const bool sent = sms_commands::send_alert(ctx,
                                                    recipient,
                                                    "Utility power lost",
                                                    false);
        if (sent)
        {
            lastState_ = state::Off;
        }
        return sent;
    }

    bool devicePowerMonitor::process(const sms_commands::context &ctx, std::string_view recipient)
    {
        if (!device_status::utility_on(ctx.measurements))
        {
            for (std::size_t i = 0; i < outlets_.size(); ++i)
            {
                outlets_[i].reset();
                ctx.monitoredLoadConnected[i] = false;
                ctx.monitoredLoadStateValid[i] = false;
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
                ctx.monitoredLoadConnected[i] = false;
                ctx.monitoredLoadStateValid[i] = true;
                continue;
            }

            outletTracker &outlet = outlets_[i];
            if (outlet.lastState == state::Unknown && outlet.sampleCount == 0)
            {
                ctx.monitoredLoadStateValid[i] = false;
            }
            outlet.add(ctx.measurements[i].mW);

            if (!outlet.ready())
            {
                continue;
            }

            const int32_t averageActivePowerMw = outlet.average_active_power_mw();

            if (outlet.lastState == state::Unknown)
            {
                outlet.lastState =
                    averageActivePowerMw >= static_cast<int32_t>(app_config::startupThresholdMw)
                        ? state::Connected
                        : state::Disconnected;
                ctx.monitoredLoadConnected[i] = outlet.lastState == state::Connected;
                ctx.monitoredLoadStateValid[i] = true;
                continue;
            }

            state candidate = state::Unknown;
            if (outlet.lastState == state::Connected &&
                averageActivePowerMw < static_cast<int32_t>(app_config::off_threshold_mw()))
            {
                candidate = state::Disconnected;
            }
            else if (outlet.lastState == state::Disconnected &&
                     averageActivePowerMw > static_cast<int32_t>(app_config::on_threshold_mw()))
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
                ctx.monitoredLoadConnected[i] = candidate == state::Connected;
                ctx.monitoredLoadStateValid[i] = true;
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
            reset();
            return false;
        }

        bool anySent = false;

        for (std::size_t i = 0; i < device_status::outletCount; ++i)
        {
            const bool relayOn =
                i < ctx.relays.size() &&
                ctx.relays[i] != nullptr &&
                device_status::outlet_on(*ctx.relays[i]);
            const bool loadConnected = ctx.monitoredLoadStateValid[i]
                                           ? ctx.monitoredLoadConnected[i]
                                           : device_status::outlet_load_connected(ctx.measurements[i]);
            if (!relayOn || !loadConnected)
            {
                lastState_[i] = state::Unknown;
                continue;
            }

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

    void powerFactorMonitor::reset()
    {
        lastState_.fill(state::Unknown);
    }

}
