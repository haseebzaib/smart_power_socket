#include "alerts.hpp"

#include <cstddef>

#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(alerts, LOG_LEVEL_INF);

namespace alerts
{

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
            return false;
        }

        bool anySent = false;

        for (std::size_t i = 0; i < device_status::outletCount; ++i)
        {
            const bool connected = device_status::outlet_load_connected(ctx.measurements[i]);

            if (lastState_[i] == state::Unknown)
            {
                lastState_[i] = connected ? state::Connected : state::Disconnected;
                continue;
            }

            if (lastState_[i] == state::Connected && !connected)
            {
                bool sent = sms_commands::send_device_alert(ctx,
                                                            recipient,
                                                            "Device power lost",
                                                            i);
                if (sent)
                {
                    lastState_[i] = state::Disconnected;
                    anySent = true;
                }
                continue;
            }

            if (lastState_[i] == state::Disconnected && connected)
            {
                bool sent = sms_commands::send_device_alert(ctx,
                                                            recipient,
                                                            "Device power restored",
                                                            i);
                if (sent)
                {
                    lastState_[i] = state::Connected;
                    anySent = true;
                }
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
