#include "adc_con.hpp"

#include <errno.h>

#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(adc_con, LOG_LEVEL_INF);

namespace hardware
{

    adcCon::adcCon(const adc_dt_spec adcSpec,
                   uint32_t dividerTopOhms,
                   uint32_t dividerBottomOhms)
        : adcSpec_(adcSpec),
          dividerTopOhms_(dividerTopOhms),
          dividerBottomOhms_(dividerBottomOhms),
          sample_(0),
          initialized_(false)
    {
    }

    adcCon::~adcCon()
    {
    }

    int adcCon::init()
    {
        if (!adc_is_ready_dt(&adcSpec_))
        {
            LOG_ERR("ADC device is not ready");
            return -ENODEV;
        }

        int ret = adc_channel_setup_dt(&adcSpec_);
        if (ret != 0)
        {
            LOG_ERR("ADC channel setup failed: %d", ret);
            initialized_ = false;
            return ret;
        }

        initialized_ = true;
        LOG_INF("ADC channel %u configured", adcSpec_.channel_id);
        return 0;
    }

    int adcCon::read_input_millivolts(int32_t &millivolts)
    {
        if (!initialized_)
        {
            int ret = init();
            if (ret != 0)
            {
                return ret;
            }
        }

        sample_ = 0;

        adc_sequence sequence{};
        sequence.buffer = &sample_;
        sequence.buffer_size = sizeof(sample_);

        int ret = adc_sequence_init_dt(&adcSpec_, &sequence);
        if (ret != 0)
        {
            return ret;
        }

        ret = adc_read_dt(&adcSpec_, &sequence);
        if (ret != 0)
        {
            return ret;
        }

        int32_t value = sample_;
        ret = adc_raw_to_millivolts_dt(&adcSpec_, &value);
        if (ret != 0)
        {
            return ret;
        }

        millivolts = value;
        return 0;
    }

    int adcCon::read_divider_millivolts(int32_t &millivolts)
    {
        if (dividerBottomOhms_ == 0)
        {
            return -EINVAL;
        }

        int32_t inputMillivolts = 0;
        int ret = read_input_millivolts(inputMillivolts);
        if (ret != 0)
        {
            return ret;
        }

        const int64_t numerator =
            static_cast<int64_t>(inputMillivolts) *
            static_cast<int64_t>(dividerTopOhms_ + dividerBottomOhms_);

        millivolts = static_cast<int32_t>(numerator / dividerBottomOhms_);
        return 0;
    }

}
