#include "sms_commands.hpp"

#include <array>
#include <cstddef>
#include <cstdio>
#include <string_view>

#include <zephyr/kernel.h>
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
    void handle_device_on(const context &ctx,
                          std::string_view sender,
                          std::string_view body);
    void handle_device_off(const context &ctx,
                           std::string_view sender,
                           std::string_view body);
    void handle_device_reboot(const context &ctx,
                              std::string_view sender,
                              std::string_view body);
    void handle_reboot(const context &ctx,
                       std::string_view sender,
                       std::string_view body);
    void handle_get_outlet_data(const context &ctx,
                                std::string_view sender,
                                std::string_view body);

    constexpr std::array<commandDefinition, 8> commands = {{
        {"getStatus", handle_get_status},
        {"SetHeartBeatDays", handle_set_heart_beat_days},
        {"DeviceOn", handle_device_on},
        {"DeviceOff", handle_device_off},
        {"DeviceReboot", handle_device_reboot},
        {"Reboot", handle_reboot},
        {"GetOutletData", handle_get_outlet_data},
        {"OutletData", handle_get_outlet_data},
    }};

    constexpr uint32_t relayRebootDelayMs = 1000;

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
        const std::size_t end = body.find_first_of("= \t\r\n");
        return end == std::string_view::npos ? body : body.substr(0, end);
    }

    std::string_view command_argument(std::string_view body)
    {
        body = trim(body);
        const std::size_t equal = body.find('=');
        if (equal == std::string_view::npos)
        {
            return {};
        }

        return trim(body.substr(equal + 1));
    }

    bool parse_outlet_mask(std::string_view body, std::array<bool, device_status::outletCount> &mask)
    {
        const std::string_view arg = command_argument(body);
        if (arg.size() < device_status::outletCount)
        {
            return false;
        }

        for (std::size_t i = 0; i < device_status::outletCount; ++i)
        {
            if (arg[i] == '0')
            {
                mask[i] = false;
            }
            else if (arg[i] == '1')
            {
                mask[i] = true;
            }
            else
            {
                return false;
            }
        }

        return true;
    }

    bool parse_outlet_number(std::string_view body, std::size_t &outletIndex)
    {
        body = trim(body);
        std::string_view arg = trim(body.substr(command_token(body).size()));
        if (!arg.empty() && arg.front() == '=')
        {
            arg.remove_prefix(1);
            arg = trim(arg);
        }

        if (arg.size() != 1 || arg.front() < '1' || arg.front() > '4')
        {
            return false;
        }

        outletIndex = static_cast<std::size_t>(arg.front() - '1');
        return true;
    }

    bool parse_optional_outlet_number(std::string_view body,
                                      bool &allOutlets,
                                      std::size_t &outletIndex)
    {
        body = trim(body);
        std::string_view arg = trim(body.substr(command_token(body).size()));
        if (!arg.empty() && arg.front() == '=')
        {
            arg.remove_prefix(1);
            arg = trim(arg);
        }

        if (arg.empty() || ascii_equals_ignore_case(arg, "all"))
        {
            allOutlets = true;
            return true;
        }

        if (arg.size() != 1 || arg.front() < '1' || arg.front() > '4')
        {
            return false;
        }

        allOutlets = false;
        outletIndex = static_cast<std::size_t>(arg.front() - '1');
        return true;
    }

    void set_outlet_relay(const context &ctx, std::size_t outletIndex, bool on)
    {
        if (outletIndex >= ctx.relays.size() || ctx.relays[outletIndex] == nullptr)
        {
            return;
        }

        ctx.relays[outletIndex]->set(on ? 0 : 1);
    }

    bool outlet_relay_on(const context &ctx, std::size_t outletIndex)
    {
        if (outletIndex >= ctx.relays.size() || ctx.relays[outletIndex] == nullptr)
        {
            return false;
        }

        return device_status::outlet_on(*ctx.relays[outletIndex]);
    }

    bool send_command_ack(const context &ctx, std::string_view recipient, std::string_view text)
    {
        return ctx.modem.send_sms(recipient, text);
    }

    void reboot_outlet(const context &ctx, std::size_t outletIndex)
    {
        if (!outlet_relay_on(ctx, outletIndex))
        {
            set_outlet_relay(ctx, outletIndex, true);
            k_msleep(relayRebootDelayMs);
        }

        set_outlet_relay(ctx, outletIndex, false);
        k_msleep(relayRebootDelayMs);
        set_outlet_relay(ctx, outletIndex, true);
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

    device_status::snapshot make_status_snapshot(const context &ctx)
    {
        return device_status::make_snapshot(ctx.modemInformation,
                                            ctx.relays,
                                            ctx.measurements,
                                            ctx.bootTimeMs,
                                            ctx.heartBeatDaysMilli,
                                            ctx.batteryMillivolts);
    }

    bool send_outlet_report(const context &ctx, std::string_view recipient, const device_status::snapshot &status)
    {
        std::array<char, 160> outletReport{};
        int outletLen = device_status::format_outlet_report(status, outletReport.data(), outletReport.size());
        if (outletLen <= 0 || static_cast<std::size_t>(outletLen) >= outletReport.size())
        {
            LOG_ERR("outlet report formatting failed");
            return false;
        }

        return ctx.modem.send_sms(recipient,
                                  std::string_view{outletReport.data(), static_cast<std::size_t>(outletLen)});
    }

    bool send_status(const context &ctx, std::string_view recipient, std::string_view heading)
    {
        device_status::snapshot status = make_status_snapshot(ctx);

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

            bool outletSent = send_outlet_report(ctx, recipient, status);
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

        bool outletSent = send_outlet_report(ctx, recipient, status);
        return statusSent && outletSent;
    }

    bool send_alert(const context &ctx,
                    std::string_view recipient,
                    std::string_view alert,
                    bool includeOutletReport)
    {
        device_status::snapshot status = make_status_snapshot(ctx);

        std::array<char, 192> alertText{};
        int alertLen = device_status::format_alert_message(status,
                                                           alert,
                                                           alertText.data(),
                                                           alertText.size());

        if (alertLen <= 0 || static_cast<std::size_t>(alertLen) >= alertText.size())
        {
            LOG_ERR("alert formatting failed");
            return false;
        }

        bool alertSent = ctx.modem.send_sms(recipient,
                                            std::string_view{alertText.data(), static_cast<std::size_t>(alertLen)});

        if (!includeOutletReport)
        {
            return alertSent;
        }

        bool outletSent = send_outlet_report(ctx, recipient, status);
        return alertSent && outletSent;
    }

    bool send_device_alert(const context &ctx,
                           std::string_view recipient,
                           std::string_view alert,
                           std::size_t outletIndex)
    {
        device_status::snapshot status = make_status_snapshot(ctx);

        std::array<char, 96> outletText{};
        int outletLen = device_status::format_single_outlet_report(status,
                                                                   outletIndex,
                                                                   outletText.data(),
                                                                   outletText.size());
        if (outletLen <= 0 || static_cast<std::size_t>(outletLen) >= outletText.size())
        {
            LOG_ERR("device alert outlet formatting failed");
            return false;
        }

        std::array<char, 160> alertText{};
        int alertLen = std::snprintf(alertText.data(),
                                     alertText.size(),
                                     "Alert=%.*s\n%.*s",
                                     static_cast<int>(alert.size()),
                                     alert.data(),
                                     outletLen,
                                     outletText.data());
        if (alertLen <= 0 || static_cast<std::size_t>(alertLen) >= alertText.size())
        {
            LOG_ERR("device alert formatting failed");
            return false;
        }

        return ctx.modem.send_sms(recipient,
                                  std::string_view{alertText.data(), static_cast<std::size_t>(alertLen)});
    }

    bool send_frequency_alert(const context &ctx,
                              std::string_view recipient,
                              std::string_view alert,
                              int32_t frequencyCentihz)
    {
        std::array<char, 80> alertText{};
        int alertLen = device_status::format_frequency_alert(alert,
                                                             frequencyCentihz,
                                                             alertText.data(),
                                                             alertText.size());
        if (alertLen <= 0 || static_cast<std::size_t>(alertLen) >= alertText.size())
        {
            LOG_ERR("frequency alert formatting failed");
            return false;
        }

        return ctx.modem.send_sms(recipient,
                                  std::string_view{alertText.data(), static_cast<std::size_t>(alertLen)});
    }

    bool send_outlet_detail(const context &ctx,
                            std::string_view recipient,
                            const device_status::snapshot &status,
                            std::size_t outletIndex)
    {
        std::array<char, 160> outletText{};
        int outletLen = device_status::format_outlet_detail_report(status,
                                                                   outletIndex,
                                                                   outletText.data(),
                                                                   outletText.size());
        if (outletLen <= 0 || static_cast<std::size_t>(outletLen) >= outletText.size())
        {
            LOG_ERR("outlet detail formatting failed");
            return false;
        }

        return ctx.modem.send_sms(recipient,
                                  std::string_view{outletText.data(), static_cast<std::size_t>(outletLen)});
    }

    bool send_power_factor_alert(const context &ctx,
                                 std::string_view recipient,
                                 std::string_view alert,
                                 std::size_t outletIndex)
    {
        device_status::snapshot status = make_status_snapshot(ctx);

        std::array<char, 160> alertText{};
        int alertLen = device_status::format_power_factor_alert(status,
                                                                alert,
                                                                outletIndex,
                                                                alertText.data(),
                                                                alertText.size());
        if (alertLen <= 0 || static_cast<std::size_t>(alertLen) >= alertText.size())
        {
            LOG_ERR("power factor alert formatting failed");
            return false;
        }

        return ctx.modem.send_sms(recipient,
                                  std::string_view{alertText.data(), static_cast<std::size_t>(alertLen)});
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

    void handle_device_on(const context &ctx,
                          std::string_view sender,
                          std::string_view body)
    {
        std::array<bool, device_status::outletCount> mask{};
        if (!parse_outlet_mask(body, mask))
        {
            send_command_ack(ctx, sender, "ERR DeviceOn");
            return;
        }

        for (std::size_t i = 0; i < mask.size(); ++i)
        {
            if (mask[i])
            {
                set_outlet_relay(ctx, i, true);
            }
        }

        send_command_ack(ctx, sender, "OK DeviceOn");
    }

    void handle_device_off(const context &ctx,
                           std::string_view sender,
                           std::string_view body)
    {
        std::array<bool, device_status::outletCount> mask{};
        if (!parse_outlet_mask(body, mask))
        {
            send_command_ack(ctx, sender, "ERR DeviceOff");
            return;
        }

        for (std::size_t i = 0; i < mask.size(); ++i)
        {
            if (mask[i])
            {
                set_outlet_relay(ctx, i, false);
            }
        }

        send_command_ack(ctx, sender, "OK DeviceOff");
    }

    void handle_device_reboot(const context &ctx,
                              std::string_view sender,
                              std::string_view body)
    {
        std::array<bool, device_status::outletCount> mask{};
        if (!parse_outlet_mask(body, mask))
        {
            send_command_ack(ctx, sender, "ERR DeviceReboot");
            return;
        }

        for (std::size_t i = 0; i < mask.size(); ++i)
        {
            if (!mask[i])
            {
                continue;
            }

            reboot_outlet(ctx, i);
        }

        send_command_ack(ctx, sender, "OK DeviceReboot");
    }

    void handle_reboot(const context &ctx,
                       std::string_view sender,
                       std::string_view body)
    {
        std::size_t outletIndex = 0;
        if (!parse_outlet_number(body, outletIndex))
        {
            send_command_ack(ctx, sender, "ERR Reboot");
            return;
        }

        reboot_outlet(ctx, outletIndex);
        send_command_ack(ctx, sender, "OK Reboot");
    }

    void handle_get_outlet_data(const context &ctx,
                                std::string_view sender,
                                std::string_view body)
    {
        bool allOutlets = true;
        std::size_t outletIndex = 0;
        if (!parse_optional_outlet_number(body, allOutlets, outletIndex))
        {
            send_command_ack(ctx, sender, "ERR GetOutletData");
            return;
        }

        device_status::snapshot status = make_status_snapshot(ctx);
        if (!allOutlets)
        {
            send_outlet_detail(ctx, sender, status, outletIndex);
            return;
        }

        for (std::size_t i = 0; i < device_status::outletCount; ++i)
        {
            send_outlet_detail(ctx, sender, status, i);
        }
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
