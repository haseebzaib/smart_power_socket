# HLW811x driver

Minimal C driver for HLW811x energy metering ICs – no external dependencies, no RTOS required.

Provides direct access to voltage, current, power, energy, frequency, and phase angle through low-level register interaction.

## Highlights

- No external dependencies (pure C99)
- Suitable for STM32, ESP32, or bare-metal systems
- Minimal heap usage — dynamic memory used only once at initialization

## Limitations

This driver currently supports only a single HLW811x instance.
Support for multiple devices (via dependency injection of I/O functions) is under consideration.
Let us know if this is important for your use case.

## Used in Production
This driver is used in the [Pazzk open-source EV charger project](https://github.com/pazzk-labs/evse) to monitor real-time AC input parameters such as voltage, current, and power.
See [`hlw811x_meter.c`](https://github.com/pazzk-labs/evse/blob/main/src/metering/adapter/hlw8112.c) for a working integration on ESP32.

## Quick Start

```c
struct hlw811x_coeff coeff;

hlw811x_init(HLW811X_UART);
hlw811x_reset();
sleep_ms(60);

hlw811x_read_coeff(&coeff);

hlw811x_set_resistor_ratio(&(const struct hlw811x_resistor_ratio) {
    .K1_A = 1,
    .K1_B = 1,
    .K2 = 1,
});
hlw811x_set_pga(&(const struct hlw811x_pga) {
    .A = HLW811X_PGA_GAIN_16,
    .B = HLW811X_PGA_GAIN_2,
    .U = HLW811X_PGA_GAIN_1,
});
hlw811x_set_channel_b_mode(HLW811X_B_MODE_NORMAL);
hlw811x_set_active_power_calc_mode(HLW811X_ACTIVE_POWER_MODE_POS_NEG_ALGEBRAIC);
hlw811x_set_rms_calc_mode(HLW811X_RMS_MODE_AC);
hlw811x_set_data_update_frequency(HLW811X_DATA_UPDATE_FREQ_HZ_3_4);
hlw811x_set_zerocrossing_mode(HLW811X_ZEROCROSSING_MODE_POSITIVE);
hlw811x_set_interrupt_mode(HLW811X_INTR_PULSE_OUT_A, HLW811X_INTR_B_LEAKAGE);
hlw811x_enable_waveform();
hlw811x_enable_zerocrossing();
hlw811x_enable_powerfactor();
hlw811x_enable_b_channel_comparator();
hlw811x_enable_interrupt(HLW811X_INTR_PULSE_OUT_A | HLW811X_INTR_B_LEAKAGE);
hlw811x_enable_channel(HLW811X_CHANNEL_ALL);
hlw811x_enable_pulse(HLW811X_CHANNEL_ALL);

hlw811x_select_channel(HLW811X_CHANNEL_A);

int32_t mV, mA, mW, watt, centiHz, pf_centi, centidegree;

hlw811x_get_rms(HLW811X_CHANNEL_U, &mV);
hlw811x_get_rms(HLW811X_CHANNEL_A, &mA);
hlw811x_get_power(&mW);
hlw811x_get_energy(HLW811X_CHANNEL_A, &watt);
hlw811x_get_frequency(&centiHz);
hlw811x_get_power_factor(&pf_centi);
hlw811x_get_phase_angle(&centidegree, HLW811X_LINE_FREQ_60HZ);
```

## Contributing
Contributions are welcome.

Feel free to open issues or PRs	to improve the driver, fix bugs, or add features.

## License
This project is licensed under the MIT License.
