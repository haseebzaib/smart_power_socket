#include "app_config.hpp"

#include <cerrno>

#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>

LOG_MODULE_REGISTER(app_config, LOG_LEVEL_INF);

namespace app_config
{
    namespace
    {
        struct calibrationStorage
        {
            uint32_t magic;
            uint8_t version;
            uint8_t validMask;
            uint16_t reserved;
            std::array<int32_t, device_status::outletCount> k2Micro;
        };

        struct relayStorage
        {
            uint32_t magic;
            uint8_t version;
            uint8_t valid;
            uint8_t onMask;
            uint8_t reserved;
        };

        constexpr uint32_t calibrationMagic = 0x53504341U; // SPCA
        constexpr uint32_t relayMagic = 0x5350524cU;       // SPRL
        constexpr uint8_t storageVersion = 1;
        constexpr int32_t defaultK2Micro = static_cast<int32_t>(defaultK2 * ratioScale + 0.5f);

        calibrationStorage calibration = {
            .magic = calibrationMagic,
            .version = storageVersion,
            .validMask = 0,
            .reserved = 0,
            .k2Micro = {defaultK2Micro, defaultK2Micro, defaultK2Micro, defaultK2Micro},
        };

        relayStorage relayState = {
            .magic = relayMagic,
            .version = storageVersion,
            .valid = 0,
            .onMask = 0,
            .reserved = 0,
        };

        bool settingsReady = false;

        bool valid_outlet(std::size_t outletIndex)
        {
            return outletIndex < device_status::outletCount;
        }

        void sanitize_calibration()
        {
            if (calibration.magic != calibrationMagic || calibration.version != storageVersion)
            {
                calibration.magic = calibrationMagic;
                calibration.version = storageVersion;
                calibration.validMask = 0;
            }

            for (std::size_t i = 0; i < calibration.k2Micro.size(); ++i)
            {
                if (calibration.k2Micro[i] < 500000 || calibration.k2Micro[i] > 2000000)
                {
                    calibration.k2Micro[i] = defaultK2Micro;
                    calibration.validMask &= static_cast<uint8_t>(~(1U << i));
                }
            }
        }

        void sanitize_relay_state()
        {
            if (relayState.magic != relayMagic || relayState.version != storageVersion)
            {
                relayState.magic = relayMagic;
                relayState.version = storageVersion;
                relayState.valid = 0;
                relayState.onMask = 0;
            }

            relayState.onMask &= 0x0f;
        }

        int settings_set(const char *key, size_t len, settings_read_cb read_cb, void *cb_arg)
        {
            const char *next = nullptr;

            if (settings_name_steq(key, "cal", &next) && next == nullptr)
            {
                if (len != sizeof(calibration))
                {
                    return -EINVAL;
                }

                const ssize_t read = read_cb(cb_arg, &calibration, sizeof(calibration));
                if (read != sizeof(calibration))
                {
                    return -EIO;
                }

                sanitize_calibration();
                return 0;
            }

            if (settings_name_steq(key, "relay", &next) && next == nullptr)
            {
                if (len != sizeof(relayState))
                {
                    return -EINVAL;
                }

                const ssize_t read = read_cb(cb_arg, &relayState, sizeof(relayState));
                if (read != sizeof(relayState))
                {
                    return -EIO;
                }

                sanitize_relay_state();
                return 0;
            }

            return -ENOENT;
        }

        settings_handler configHandler = {
            .name = "sps",
            .cprio = 0,
            .h_get = nullptr,
            .h_set = settings_set,
            .h_commit = nullptr,
            .h_export = nullptr,
        };
    }

    int init()
    {
        int ret = settings_subsys_init();
        if (ret != 0 && ret != -EALREADY)
        {
            LOG_ERR("settings init failed: %d", ret);
            return ret;
        }

        ret = settings_register(&configHandler);
        if (ret != 0)
        {
            LOG_ERR("settings register failed: %d", ret);
            return ret;
        }

        ret = settings_load();
        if (ret != 0)
        {
            LOG_ERR("settings load failed: %d", ret);
            return ret;
        }

        settingsReady = true;
        LOG_INF("config loaded: relay valid=%u mask=0x%02x",
                static_cast<unsigned int>(relayState.valid),
                static_cast<unsigned int>(relayState.onMask));
        return 0;
    }

    hlw811x_resistor_ratio ratio_for_outlet(std::size_t outletIndex)
    {
        return {
            .K1_A = defaultK1A,
            .K1_B = defaultK1B,
            .K2 = k2_for_outlet(outletIndex),
        };
    }

    int32_t k2_micro_for_outlet(std::size_t outletIndex)
    {
        if (!valid_outlet(outletIndex))
        {
            return defaultK2Micro;
        }

        return calibration.k2Micro[outletIndex];
    }

    float k2_for_outlet(std::size_t outletIndex)
    {
        return static_cast<float>(k2_micro_for_outlet(outletIndex)) / static_cast<float>(ratioScale);
    }

    int save_k2_micro(std::size_t outletIndex, int32_t k2Micro)
    {
        if (!valid_outlet(outletIndex) || k2Micro < 500000 || k2Micro > 2000000)
        {
            return -EINVAL;
        }

        calibration.magic = calibrationMagic;
        calibration.version = storageVersion;
        calibration.k2Micro[outletIndex] = k2Micro;
        calibration.validMask |= static_cast<uint8_t>(1U << outletIndex);

        if (!settingsReady)
        {
            return 0;
        }

        return settings_save_one("sps/cal", &calibration, sizeof(calibration));
    }

    bool relay_state_valid()
    {
        return relayState.valid != 0;
    }

    uint8_t relay_on_mask()
    {
        return relayState.onMask & 0x0f;
    }

    int save_relay_on_mask(uint8_t onMask)
    {
        relayState.magic = relayMagic;
        relayState.version = storageVersion;
        relayState.valid = 1;
        relayState.onMask = onMask & 0x0f;

        if (!settingsReady)
        {
            return 0;
        }

        return settings_save_one("sps/relay", &relayState, sizeof(relayState));
    }

}
