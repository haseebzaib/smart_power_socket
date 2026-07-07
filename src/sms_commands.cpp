#include "sms_commands.hpp"

#include <array>
#include <cstddef>
#include <cstdio>
#include <string_view>

#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(sms_commands, LOG_LEVEL_INF);

namespace sms_commands
{
    using commandCallback = void (*)(const context &ctx,
                                    std::string_view sender,
                                    std::string_view body);

    struct commandDefinition
    {
        std::string_view name;
        commandCallback callback;
    };

    void handle_get_status(const context &ctx,
                           std::string_view sender,
                           std::string_view body);
    void handle_set_heart_beat_days(const context &ctx,
                                    std::string_view sender,
                                    std::string_view body);

    constexpr std::array<commandDefinition, 2> commands = {{
        {"getStatus", handle_get_status},
        {"SetHeartBeatDays", handle_set_heart_beat_days},
    }};

    char ascii_lower(char ch)
    {
        if (ch >= 'A' && ch <= 'Z')
        {
            return static_cast<char>(ch - 'A' + 'a');
        }
        return ch;
    }

    bool ascii_equals_ignore_case(std::string_view lhs, std::string_view rhs)
    {
        if (lhs.size() != rhs.size())
        {
            return false;
        }

        for (std::size_t i = 0; i < lhs.size(); ++i)
        {
            if (ascii_lower(lhs[i]) != ascii_lower(rhs[i]))
            {
                return false;
            }
        }

        return true;
    }

    bool is_space(char ch)
    {
        return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
    }

    std::string_view trim(std::string_view text)
    {
        while (!text.empty() && is_space(text.front()))
        {
            text.remove_prefix(1);
        }

        while (!text.empty() && is_space(text.back()))
        {
            text.remove_suffix(1);
        }

        return text;
    }

    std::string_view command_token(std::string_view body)
    {
        body = trim(body);
        const std::size_t end = body.find_first_of(" \t\r\n");
        return end == std::string_view::npos ? body : body.substr(0, end);
    }

    const commandDefinition *find_command(std::string_view body)
    {
        const std::string_view command = command_token(body);

        for (const commandDefinition &definition : commands)
        {
            if (ascii_equals_ignore_case(command, definition.name))
            {
                return &definition;
            }
        }

        return nullptr;
    }

    bool send_status(const context &ctx, std::string_view recipient, std::string_view heading)
    {
        device_status::snapshot status = device_status::make_snapshot(ctx.modemInformation,
                                                                       ctx.relays,
                                                                       ctx.measurements,
                                                                       ctx.bootTimeMs,
                                                                       ctx.heartBeatDaysMilli,
                                                                       ctx.batteryMillivolts);

        std::array<char, 192> statusText{};
        int statusLen = device_status::format_status_message(status, statusText.data(), statusText.size());

        if (statusLen <= 0 || static_cast<std::size_t>(statusLen) >= statusText.size())
        {
            LOG_ERR("status response formatting failed");
            return false;
        }

        LOG_INF("status battery mV %u, payload: %.*s",
                static_cast<unsigned int>(ctx.batteryMillivolts),
                statusLen,
                statusText.data());

        if (heading.empty())
        {
            bool statusSent = ctx.modem.send_sms(recipient,
                                                 std::string_view{statusText.data(), static_cast<std::size_t>(statusLen)});

            std::array<char, 160> outletReport{};
            int outletLen = device_status::format_outlet_report(status, outletReport.data(), outletReport.size());
            if (outletLen <= 0 || static_cast<std::size_t>(outletLen) >= outletReport.size())
            {
                LOG_ERR("outlet report formatting failed");
                return false;
            }

            bool outletSent = ctx.modem.send_sms(recipient,
                                                 std::string_view{outletReport.data(), static_cast<std::size_t>(outletLen)});
            return statusSent && outletSent;
        }

        std::array<char, 192> response{};
        int responseLen = std::snprintf(response.data(),
                                        response.size(),
                                        "%.*s\n%.*s",
                                        static_cast<int>(heading.size()),
                                        heading.data(),
                                        statusLen,
                                        statusText.data());

        if (responseLen <= 0 || static_cast<std::size_t>(responseLen) >= response.size())
        {
            LOG_ERR("headed status response formatting failed");
            return false;
        }

        bool statusSent = ctx.modem.send_sms(recipient,
                                             std::string_view{response.data(), static_cast<std::size_t>(responseLen)});

        std::array<char, 160> outletReport{};
        int outletLen = device_status::format_outlet_report(status, outletReport.data(), outletReport.size());
        if (outletLen <= 0 || static_cast<std::size_t>(outletLen) >= outletReport.size())
        {
            LOG_ERR("outlet report formatting failed");
            return false;
        }

        bool outletSent = ctx.modem.send_sms(recipient,
                                             std::string_view{outletReport.data(), static_cast<std::size_t>(outletLen)});
        return statusSent && outletSent;
    }

    void handle_get_status(const context &ctx,
                           std::string_view sender,
                           std::string_view)
    {
        LOG_INF("SMS command getStatus from %.*s",
                static_cast<int>(sender.size()),
                sender.data());

        send_status(ctx, sender);
    }

    void handle_set_heart_beat_days(const context &,
                                    std::string_view sender,
                                    std::string_view)
    {
        LOG_INF("SMS command SetHeartBeatDays from %.*s",
                static_cast<int>(sender.size()),
                sender.data());
    }

    void handle(const context &ctx)
    {
        const std::string_view sender{ctx.modemInformation.smsNumber.data()};
        const std::string_view body{ctx.modemInformation.smsBody.data()};
        const commandDefinition *command = find_command(body);

        if (command != nullptr)
        {
            command->callback(ctx, sender, body);
            return;
        }

        LOG_WRN("Unknown SMS command from %.*s: %.*s",
                static_cast<int>(sender.size()),
                sender.data(),
                static_cast<int>(body.size()),
                body.data());
    }

}
