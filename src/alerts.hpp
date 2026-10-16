#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
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
            FuseSuspected,
        };

        static constexpr uint8_t fuseConfirmationSamples = 3;
        uint8_t fuseCandidateCount_ = 0;
        state lastState_ = state::Unknown;
    };

    class devicePowerMonitor
    {
    public:
        bool process(const sms_commands::context &ctx, std::string_view recipient);

    private:
        static constexpr std::size_t sampleWindow = 5;
        static constexpr uint8_t confirmationSamples = 3;

        enum class state
        {
            Unknown,
            Connected,
            Disconnected,
        };

        struct outletTracker
        {
            std::array<int32_t, sampleWindow> activePowerMw{};
            std::size_t nextSample = 0;
            std::size_t sampleCount = 0;
            int64_t activePowerSum = 0;
            state lastState = state::Unknown;
            state candidateState = state::Unknown;
            uint8_t candidateCount = 0;
            uint32_t observedRelaySequence = 0;
            bool settleIntentionalRelayChange = false;

            void reset();
            void add(int32_t activePower);
            bool ready() const;
            int32_t average_active_power_mw() const;
        };

        std::array<outletTracker, device_status::outletCount> outlets_{};
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
        void reset();

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
