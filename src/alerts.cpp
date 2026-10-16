#include "alerts.hpp"

#include <cstddef>

#include <zephyr/logging/log.h>

#include "app_config.hpp"

LOG_MODULE_REGISTER(alerts, LOG_LEVEL_INF);

namespace alerts
{
    void devicePowerMonitor::outletTracker::reset()
    {
        // Relay-change identity/settling survives sample resets during an outage.
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

            const bool queued = sms_commands::send_alert(ctx,
                                                        recipient,
                                                        "Utility power restored",
                                                        true);
            if (queued)
            {
                lastState_ = state::On;
            }
            return queued;
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

            const bool queued = sms_commands::send_alert(ctx,
                                                        recipient,
                                                        "Utility power lost - fuse probably blown",
                                                        false);
            if (queued)
            {
                lastState_ = state::FuseSuspected;
            }
            return queued;
        }

        fuseCandidateCount_ = 0;
        if (lastState_ == state::Off || lastState_ == state::FuseSuspected)
        {
            // Do not repeat a loss SMS until socket voltage is restored.
            return false;
        }

        const bool queued = sms_commands::send_alert(ctx,
                                                    recipient,
                                                    "Utility power lost",
                                                    false);
        if (queued)
        {
            lastState_ = state::Off;
        }
        return queued;
    }

    bool devicePowerMonitor::process(const sms_commands::context &ctx, std::string_view recipient)
    {
        for (std::size_t i = 0; i < outlets_.size(); ++i)
        {
            auto &outlet = outlets_[i];
            if (outlet.observedRelaySequence != ctx.relayChangeSequence[i])
            {
                outlet.reset();
                outlet.observedRelaySequence = ctx.relayChangeSequence[i];
                outlet.settleIntentionalRelayChange = true;
                ctx.monitoredLoadStateValid[i] = false;
            }
        }
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

        bool anyQueued = false;

        for (std::size_t i = 0; i < device_status::outletCount; ++i)
        {
            const bool relayOn =
                i < ctx.relays.size() &&
                ctx.relays[i] != nullptr &&
                device_status::outlet_on(*ctx.relays[i]);

            // An unpowered outlet cannot have an unexpected device power loss.
            // Intentional relay changes must not masquerade as load events.
            if (!relayOn)
            {
                outlets_[i].reset();
                outlets_[i].settleIntentionalRelayChange = true;
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

            if (outlet.settleIntentionalRelayChange)
            {
                outlet.lastState = averageActivePowerMw > static_cast<int32_t>(app_config::on_threshold_mw())
                    ? state::Connected : state::Disconnected;
                outlet.settleIntentionalRelayChange = false;
                ctx.monitoredLoadConnected[i] = outlet.lastState == state::Connected;
                ctx.monitoredLoadStateValid[i] = true;
                LOG_INF("Load monitor outlet %u settled after intentional relay change; load alert suppressed",
                        static_cast<unsigned>(i + 1));
                continue;
            }

            // Empty startup state needs no "lost" notification. A present load
            // follows the same threshold/confirmation path as later connections;
            // never silently absorb it as an initial connected baseline.
            if (outlet.lastState == state::Unknown &&
                averageActivePowerMw <= static_cast<int32_t>(app_config::on_threshold_mw()))
            {
                outlet.lastState = state::Disconnected;
                ctx.monitoredLoadConnected[i] = false;
                ctx.monitoredLoadStateValid[i] = true;
            }

            state candidate = state::Unknown;
            if (outlet.lastState == state::Connected &&
                averageActivePowerMw < static_cast<int32_t>(app_config::off_threshold_mw()))
            {
                candidate = state::Disconnected;
            }
            else if ((outlet.lastState == state::Disconnected || outlet.lastState == state::Unknown) &&
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
            const bool sendAvailable = ctx.modem.can_queue_sms();
            if (sendAvailable)
                LOG_INF("Load monitor outlet %u queue confirmed %s: average=%d mW on=%u off=%u mW",
                        static_cast<unsigned>(i + 1), alert, averageActivePowerMw,
                        app_config::on_threshold_mw(), app_config::off_threshold_mw());
            bool queued = sms_commands::send_device_alert(ctx, recipient, alert, i);
            if (!queued && sendAvailable)
                LOG_WRN("Load monitor outlet %u alert could not be queued; confirmed transition will be retried while it remains present",
                        static_cast<unsigned>(i + 1));
            if (queued)
            {
                outlet.lastState = candidate;
                ctx.monitoredLoadConnected[i] = candidate == state::Connected;
                ctx.monitoredLoadStateValid[i] = true;
                outlet.candidateState = state::Unknown;
                outlet.candidateCount = 0;
                anyQueued = true;
            }
        }

        return anyQueued;
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

            bool queued = sms_commands::send_frequency_alert(ctx,
                                                           recipient,
                                                           "Frequency bad",
                                                           frequencyCentihz);
            if (queued)
            {
                lastState_ = state::Bad;
            }
            return queued;
        }

        if (lastState_ == state::Good && bad)
        {
            bool queued = sms_commands::send_frequency_alert(ctx,
                                                           recipient,
                                                           "Frequency bad",
                                                           frequencyCentihz);
            if (queued)
            {
                lastState_ = state::Bad;
            }
            return queued;
        }

        if (lastState_ == state::Bad && !bad)
        {
            bool queued = sms_commands::send_frequency_alert(ctx,
                                                           recipient,
                                                           "Frequency restored",
                                                           frequencyCentihz);
            if (queued)
            {
                lastState_ = state::Good;
            }
            return queued;
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

        bool anyQueued = false;

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

                bool queued = sms_commands::send_power_factor_alert(ctx,
                                                                  recipient,
                                                                  "Power factor bad",
                                                                  i);
                if (queued)
                {
                    lastState_[i] = state::Bad;
                    anyQueued = true;
                }
                continue;
            }

            if (lastState_[i] == state::Good && bad)
            {
                bool queued = sms_commands::send_power_factor_alert(ctx,
                                                                  recipient,
                                                                  "Power factor bad",
                                                                  i);
                if (queued)
                {
                    lastState_[i] = state::Bad;
                    anyQueued = true;
                }
                continue;
            }

            if (lastState_[i] == state::Bad && restored)
            {
                bool queued = sms_commands::send_power_factor_alert(ctx,
                                                                  recipient,
                                                                  "Power factor restored",
                                                                  i);
                if (queued)
                {
                    lastState_[i] = state::Good;
                    anyQueued = true;
                }
            }
        }

        return anyQueued;
    }

    void powerFactorMonitor::reset()
    {
        lastState_.fill(state::Unknown);
    }

}
