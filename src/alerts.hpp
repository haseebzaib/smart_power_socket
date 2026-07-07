#pragma once

#include <array>
#include <string_view>

#include "sms_commands.hpp"

namespace alerts
{

    class utilityPowerMonitor
    {
    public:
        bool process(const sms_commands::context &ctx, std::string_view recipient);

    private:
        enum class state
        {
            Unknown,
            On,
            Off,
        };

        state lastState_ = state::Unknown;
    };

    class devicePowerMonitor
    {
    public:
        bool process(const sms_commands::context &ctx, std::string_view recipient);

    private:
        enum class state
        {
            Unknown,
            Connected,
            Disconnected,
        };

        std::array<state, device_status::outletCount> lastState_{};
    };

    class frequencyMonitor
    {
    public:
        bool process(const sms_commands::context &ctx, std::string_view recipient);

    private:
        enum class state
        {
            Unknown,
            Good,
            Bad,
        };

        state lastState_ = state::Unknown;
    };

    class powerFactorMonitor
    {
    public:
        bool process(const sms_commands::context &ctx, std::string_view recipient);

    private:
        enum class state
        {
            Unknown,
            Good,
            Bad,
        };

        std::array<state, device_status::outletCount> lastState_{};
    };

}
