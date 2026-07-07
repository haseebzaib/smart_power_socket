#pragma once

#include <cstdint>

#include <zephyr/devicetree.h>
#include <zephyr/drivers/adc.h>

#define HARDWARE_ADC_CHANNEL_DT_SPEC_GET(node_id)             \
    {                                                         \
        .dev = DEVICE_DT_GET(DT_PARENT(node_id)),             \
        .channel_id = DT_REG_ADDR(node_id),                   \
        .channel_cfg_dt_node_exists = true,                   \
        .channel_cfg = ADC_CHANNEL_CFG_DT(node_id),           \
        .vref_mv = DT_PROP_OR(node_id, zephyr_vref_mv, 0),    \
        .resolution = DT_PROP_OR(node_id, zephyr_resolution, 0), \
        .oversampling = DT_PROP_OR(node_id, zephyr_oversampling, 0), \
    }

namespace hardware
{

    class adcCon
    {
    public:
        adcCon(const adc_dt_spec adcSpec,
               uint32_t dividerTopOhms,
               uint32_t dividerBottomOhms);
        ~adcCon();

        adcCon(const adcCon &) = delete;
        adcCon &operator=(const adcCon &) = delete;

        int init();
        int read_input_millivolts(int32_t &millivolts);
        int read_divider_millivolts(int32_t &millivolts);

    private:
        adc_dt_spec adcSpec_;
        uint32_t dividerTopOhms_;
        uint32_t dividerBottomOhms_;
        int16_t sample_;
        bool initialized_;
    };

}
