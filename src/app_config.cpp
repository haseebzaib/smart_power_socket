#include "app_config.hpp"

#include <cerrno>
#include <cstring>

#include <zephyr/drivers/flash.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/storage/flash_map.h>

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

        struct alertStorage
        {
            uint32_t magic;
            uint32_t onThresholdMw;
            uint32_t offThresholdMw;
            uint8_t version;
            uint8_t powerFactorAlertsEnabled;
            uint16_t reserved;
        };

        struct heartbeatStorage
        {
            uint32_t magic;
            uint32_t daysMilli;
            uint8_t version;
            uint8_t reserved[3];
        };

        constexpr uint32_t calibrationMagic = 0x53504341U; // SPCA
        constexpr uint32_t relayMagic = 0x5350524cU;       // SPRL
        constexpr uint32_t alertMagic = 0x5350414cU;       // SPAL
        constexpr uint32_t heartbeatMagic = 0x53504842U;   // SPHB
        constexpr uint8_t storageVersion = 1;
        constexpr uint32_t maxDeviceThresholdMw = 20000000U;
        constexpr uint32_t maxHeartbeatDaysMilli = 999999U;
        constexpr uint32_t defaultHeartbeatDaysMilli = 30000U; // 30 days.
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

        alertStorage alertSettings = {
            .magic = alertMagic,
            .onThresholdMw = defaultOnThresholdMw,
            .offThresholdMw = defaultOffThresholdMw,
            .version = storageVersion,
            .powerFactorAlertsEnabled = 0,
            .reserved = 0,
        };

        heartbeatStorage heartbeatSettings = {
            .magic = heartbeatMagic,
            .daysMilli = defaultHeartbeatDaysMilli,
            .version = storageVersion,
            .reserved = {0, 0, 0},
        };

        bool settingsReady = false;
        uint8_t loadedSettings = 0;

        int save_setting(const char *key, const void *value, size_t size)
        {
            if (!settingsReady)
            {
                LOG_ERR("Settings save %s failed: storage unavailable", key);
                return -ENODEV;
            }
            std::array<uint8_t, sizeof(calibrationStorage)> readback{};
            static_assert(sizeof(relayStorage) <= sizeof(calibrationStorage));
            static_assert(sizeof(alertStorage) <= sizeof(calibrationStorage));
            static_assert(sizeof(heartbeatStorage) <= sizeof(calibrationStorage));
            if (size > readback.size()) return -E2BIG;
            int ret = settings_save_one(key, value, size);
            if (ret != 0)
            {
                LOG_ERR("Settings save %s failed: %d", key, ret);
                return ret;
            }
            const ssize_t length = settings_load_one(key, readback.data(), readback.size());
            if (length != static_cast<ssize_t>(size) || std::memcmp(readback.data(), value, size))
            {
                ret = length < 0 ? static_cast<int>(length) : -EIO;
                LOG_ERR("Settings readback %s failed: %d", key, ret);
                return ret;
            }
            LOG_INF("Settings persisted and verified: %s (%u bytes)", key, static_cast<unsigned>(size));
            return 0;
        }

        [[maybe_unused]] void log_reserved_partition(uint8_t id, const char *name)
        {
            const flash_area *area = nullptr;
            const int ret = flash_area_open(id, &area);
            if (ret != 0)
            {
                LOG_ERR("Reserved partition %s unavailable: %d", name, ret);
                return;
            }
            LOG_INF("Reserved %s: device=%s offset=0x%lx size=0x%lx", name,
                    area->fa_dev->name, static_cast<unsigned long>(area->fa_off),
                    static_cast<unsigned long>(area->fa_size));
            flash_area_close(area);
        }

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

        void sanitize_alert_settings()
        {
            if (alertSettings.magic != alertMagic ||
                alertSettings.version != storageVersion ||
                alertSettings.onThresholdMw > maxDeviceThresholdMw ||
                alertSettings.offThresholdMw >= alertSettings.onThresholdMw)
            {
                alertSettings.magic = alertMagic;
                alertSettings.onThresholdMw = defaultOnThresholdMw;
                alertSettings.offThresholdMw = defaultOffThresholdMw;
                alertSettings.version = storageVersion;
                alertSettings.powerFactorAlertsEnabled = 0;
                alertSettings.reserved = 0;
                return;
            }

            alertSettings.powerFactorAlertsEnabled =
                alertSettings.powerFactorAlertsEnabled != 0 ? 1 : 0;
        }

        void sanitize_heartbeat_settings()
        {
            if (heartbeatSettings.magic != heartbeatMagic ||
                heartbeatSettings.version != storageVersion ||
                heartbeatSettings.daysMilli > maxHeartbeatDaysMilli)
            {
                heartbeatSettings.magic = heartbeatMagic;
                heartbeatSettings.daysMilli = defaultHeartbeatDaysMilli;
                heartbeatSettings.version = storageVersion;
                heartbeatSettings.reserved[0] = 0;
                heartbeatSettings.reserved[1] = 0;
                heartbeatSettings.reserved[2] = 0;
            }
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
                loadedSettings |= 1U;
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
                loadedSettings |= 2U;
                return 0;
            }

            if (settings_name_steq(key, "alerts", &next) && next == nullptr)
            {
                if (len != sizeof(alertSettings))
                {
                    return -EINVAL;
                }

                const ssize_t read = read_cb(cb_arg, &alertSettings, sizeof(alertSettings));
                if (read != sizeof(alertSettings))
                {
                    return -EIO;
                }

                sanitize_alert_settings();
                loadedSettings |= 4U;
                return 0;
            }

            if (settings_name_steq(key, "heartbeat", &next) && next == nullptr)
            {
                if (len != sizeof(heartbeatSettings))
                {
                    return -EINVAL;
                }

                const ssize_t read = read_cb(cb_arg, &heartbeatSettings, sizeof(heartbeatSettings));
                if (read != sizeof(heartbeatSettings))
                {
                    return -EIO;
                }

                sanitize_heartbeat_settings();
                loadedSettings |= 8U;
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
        const flash_area *area = nullptr;
        int ret = flash_area_open(FIXED_PARTITION_ID(storage_partition), &area);
        if (ret != 0)
        {
            LOG_ERR("Settings partition unavailable: %d", ret);
            return ret;
        }
        if (!device_is_ready(area->fa_dev))
        {
            LOG_ERR("Settings flash %s is not ready", area->fa_dev->name);
            flash_area_close(area);
            return -ENODEV;
        }
#if DT_NODE_HAS_STATUS(DT_ALIAS(spi_flash), okay)
        if (area->fa_dev != DEVICE_DT_GET(DT_ALIAS(spi_flash)))
        {
            LOG_ERR("Settings are not on external flash; check the BL54L15 partition configuration");
            flash_area_close(area);
            return -EINVAL;
        }
#if defined(CONFIG_FLASH_JESD216_API)
        std::array<uint8_t, 3> jedec{};
        ret = flash_read_jedec_id(area->fa_dev, jedec.data());
        if (ret != 0 || jedec != std::array<uint8_t, 3>{0xc2, 0x28, 0x17})
        {
            LOG_ERR("External flash identity failed: ret=%d JEDEC=%02x %02x %02x", ret,
                    jedec[0], jedec[1], jedec[2]);
            flash_area_close(area);
            return ret != 0 ? ret : -ENODEV;
        }
        LOG_INF("External flash: MX25R6435FZNIL0, JEDEC=c2 28 17, capacity=8388608 bytes (8 MiB)");
#endif
#endif
        LOG_INF("Settings storage: device=%s offset=0x%lx size=0x%lx", area->fa_dev->name,
                static_cast<unsigned long>(area->fa_off), static_cast<unsigned long>(area->fa_size));
        flash_pages_info page{};
        ret = flash_get_page_info_by_offs(area->fa_dev, area->fa_off, &page);
        if (ret != 0)
        {
            LOG_ERR("Settings flash layout query failed: %d", ret);
            flash_area_close(area);
            return ret;
        }
        const size_t sectorSize = page.size * CONFIG_SETTINGS_NVS_SECTOR_SIZE_MULT;
        LOG_INF("Settings NVS layout: page=%u bytes sector=%u bytes configured sectors=%u",
                static_cast<unsigned>(page.size), static_cast<unsigned>(sectorSize),
                static_cast<unsigned>(CONFIG_SETTINGS_NVS_SECTOR_COUNT));
        if (sectorSize > UINT16_MAX)
        {
            LOG_ERR("Settings sector exceeds NVS limit; include boards/bl54l15_dvk_nrf54l15_cpuapp.conf (4 KiB pages)");
            flash_area_close(area);
            return -EDOM;
        }
        flash_area_close(area);
#if FIXED_PARTITION_EXISTS(ota_image_a)
        log_reserved_partition(FIXED_PARTITION_ID(ota_image_a), "OTA image A");
#endif
#if FIXED_PARTITION_EXISTS(ota_image_b)
        log_reserved_partition(FIXED_PARTITION_ID(ota_image_b), "OTA image B");
#endif
#if FIXED_PARTITION_EXISTS(data_storage)
        log_reserved_partition(FIXED_PARTITION_ID(data_storage), "future data");
#endif
        ret = settings_subsys_init();
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
        LOG_INF("Settings ready: loaded mask=0x%02x calibration mask=0x%02x",
                static_cast<unsigned>(loadedSettings), static_cast<unsigned>(calibration.validMask));
        LOG_INF("config loaded: relay valid=%u mask=0x%02x, on=%u mW off=%u mW pf=%u hbd=%u",
                static_cast<unsigned int>(relayState.valid),
                static_cast<unsigned int>(relayState.onMask),
                static_cast<unsigned int>(alertSettings.onThresholdMw),
                static_cast<unsigned int>(alertSettings.offThresholdMw),
                static_cast<unsigned int>(alertSettings.powerFactorAlertsEnabled),
                static_cast<unsigned int>(heartbeatSettings.daysMilli));
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

        return save_setting("sps/cal", &calibration, sizeof(calibration));
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

        return save_setting("sps/relay", &relayState, sizeof(relayState));
    }

    uint32_t on_threshold_mw()
    {
        return alertSettings.onThresholdMw;
    }

    uint32_t off_threshold_mw()
    {
        return alertSettings.offThresholdMw;
    }

    int save_on_threshold_mw(uint32_t thresholdMw)
    {
        if (thresholdMw > maxDeviceThresholdMw || thresholdMw <= alertSettings.offThresholdMw)
        {
            return -EINVAL;
        }

        alertSettings.onThresholdMw = thresholdMw;
        return save_setting("sps/alerts", &alertSettings, sizeof(alertSettings));
    }

    int save_off_threshold_mw(uint32_t thresholdMw)
    {
        if (thresholdMw >= alertSettings.onThresholdMw)
        {
            return -EINVAL;
        }

        alertSettings.offThresholdMw = thresholdMw;
        return save_setting("sps/alerts", &alertSettings, sizeof(alertSettings));
    }

    bool power_factor_alerts_enabled()
    {
        return alertSettings.powerFactorAlertsEnabled != 0;
    }

    int save_power_factor_alerts_enabled(bool enabled)
    {
        alertSettings.powerFactorAlertsEnabled = enabled ? 1 : 0;
        return save_setting("sps/alerts", &alertSettings, sizeof(alertSettings));
    }

    uint32_t heartbeat_days_milli()
    {
        return heartbeatSettings.daysMilli;
    }

    int save_heartbeat_days_milli(uint32_t daysMilli)
    {
        if (daysMilli > maxHeartbeatDaysMilli)
        {
            return -EINVAL;
        }

        heartbeatSettings.daysMilli = daysMilli;
        return save_setting("sps/heartbeat", &heartbeatSettings, sizeof(heartbeatSettings));
    }

}
