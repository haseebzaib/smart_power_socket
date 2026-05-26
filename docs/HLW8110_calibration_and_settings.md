# HLW8110 Calibration And Settings Notes

This note is for the Smart Power Socket board using HLW8110 metering ICs through
the UART mux.

Board assumptions:

- Device: HLW8110
- UART mode
- One current channel is used: channel A
- Current shunt: `3 mOhm`
- Current channel B is not used for metering
- Four HLW8110 devices are selected through the mux

The C library is named `hlw811x`, because it supports the HLW8110/HLW8112
family. Treat any channel B API carefully, because this board is a single
current-channel design.

## Bring-Up Goal

During bring-up, first prove these things in order:

1. UART TX/RX works.
2. Mux selects the correct HLW8110.
3. Coefficients can be read.
4. Configuration registers can be written and read back.
5. RMS voltage/current registers produce nonzero sane values.
6. Power, energy, frequency, power factor, and phase readings become sane after
   the signal path is configured and enough settling time has passed.

Do not start calibration until raw communication and basic register reads are
stable.

## Recommended HLW8110 Setup For This Board

Use channel A and voltage U only:

```cpp
hlw811x_set_resistor_ratio(device, &(const hlw811x_resistor_ratio) {
    .K1_A = 3.0f,
    .K1_B = 1.0f,
    .K2 = YOUR_VOLTAGE_RATIO,
});

hlw811x_set_pga(device, &(const hlw811x_pga) {
    .A = HLW811X_PGA_GAIN_16,
    .B = HLW811X_PGA_GAIN_1,
    .U = HLW811X_PGA_GAIN_1,
});

hlw811x_set_channel_b_mode(device, HLW811X_B_MODE_TEMPERATURE);
hlw811x_set_active_power_calc_mode(device,
    HLW811X_ACTIVE_POWER_MODE_POS_NEG_ALGEBRAIC);
hlw811x_set_rms_calc_mode(device, HLW811X_RMS_MODE_AC);
hlw811x_set_data_update_frequency(device, HLW811X_DATA_UPDATE_FREQ_HZ_3_4);

hlw811x_select_channel(device, HLW811X_CHANNEL_A);
hlw811x_enable_channel(device, HLW811X_CHANNEL_A | HLW811X_CHANNEL_U);
hlw811x_disable_channel(device, HLW811X_CHANNEL_B);

hlw811x_enable_power_factor(device);
hlw811x_enable_waveform(device);
hlw811x_enable_zerocrossing(device);
hlw811x_enable_pulse(device, HLW811X_CHANNEL_A);
```

For a first stable measurement loop, read:

```cpp
hlw811x_get_rms(device, HLW811X_CHANNEL_U, &mV);
hlw811x_get_rms(device, HLW811X_CHANNEL_A, &mA);
hlw811x_get_power(device, HLW811X_CHANNEL_A, &mW);
hlw811x_get_power(device, HLW811X_CHANNEL_U, &apparent_mW);
hlw811x_get_energy(device, HLW811X_CHANNEL_A, &Wh);
hlw811x_get_power_factor(device, &pf_centi);
```

Frequency and phase angle need waveform and zero-crossing enabled, then some
settling time:

```cpp
hlw811x_enable_waveform(device);
hlw811x_enable_zerocrossing(device);
k_msleep(1000);

hlw811x_get_frequency(device, &centiHz);
hlw811x_get_phase_angle(device, &centidegree, HLW811X_LINE_FREQ_60HZ);
```

Use `HLW811X_LINE_FREQ_50HZ` or `HLW811X_LINE_FREQ_60HZ` according to the
actual mains input.

## Resistor Ratio Settings

The library uses:

```c
struct hlw811x_resistor_ratio {
    float K1_A;
    float K1_B;
    float K2;
};
```

### K1_A

From the datasheet, current shunt scaling is relative to `1 mOhm`.

Formula:

```text
K1_A = actual_shunt_mohm / 1 mOhm
```

For this board:

```text
Rshunt = 3 mOhm
K1_A = 3.0
```

Use:

```cpp
.K1_A = 3.0f
```

### K1_B

Channel B is not used for current metering on this board.

Use:

```cpp
.K1_B = 1.0f
```

Do not use channel B RMS/current/power values for application logic.

### K2

`K2` is the voltage divider scaling, relative to the datasheet nominal divider:

```text
K2 = actual_low_side_resistor_kohm / 1 kohm
```

assuming the high side is the nominal `1 Mohm` style divider described by the
datasheet.

If the calculated HLW voltage does not match a trusted multimeter, calibrate
`K2` with:

```text
new_K2 = old_K2 * hlw_voltage_reading / reference_voltage
```

Example:

```text
HLW reading = 128.7 V
Multimeter = 121.0 V
old_K2 = 1.0

new_K2 = 1.0 * 128.7 / 121.0 = 1.0636
```

Use:

```cpp
.K2 = 1.064f
```

Repeat with averaged readings until voltage matches the reference meter.

## PGA Gain Selection

The HLW8110 analog input full-scale peak is approximately:

```text
input_peak_max = 800 mV / PGA
```

For a shunt resistor:

```text
Vshunt_rms = Irms * Rshunt
Vshunt_peak = Vshunt_rms * sqrt(2)
Imax_rms = (800 mV / PGA) / sqrt(2) / Rshunt
```

For `Rshunt = 3 mOhm`:

| PGA | Approx Max RMS Current Before Full Scale |
| --- | ---: |
| 1   | 188.6 A |
| 2   | 94.3 A |
| 4   | 47.1 A |
| 8   | 23.6 A |
| 16  | 11.8 A |

For a 5 A load:

```text
Vshunt_rms = 5 A * 0.003 ohm = 15 mV RMS
Vshunt_peak = 21.2 mV peak
PGA16 input limit = 800 mV / 16 = 50 mV peak
```

So `PGA_GAIN_16` is safe for 5 A and gives the best low-current resolution.

For 10 mA:

```text
Vshunt_rms = 0.01 A * 0.003 ohm = 30 uV RMS
Vshunt_peak = 42.4 uV peak
```

This is very small. `PGA_GAIN_16` is still the best choice, but precision at
10 mA will be limited by noise, offset, PCB layout, and RMS offset calibration.
Use averaging and expect low-current readings to be less stable than high-load
readings.

Recommended current PGA for this board:

```cpp
.A = HLW811X_PGA_GAIN_16
```

Only reduce current PGA if the design must measure currents above about `11 A`
RMS with the same `3 mOhm` shunt.

Voltage PGA:

```cpp
.U = HLW811X_PGA_GAIN_1
```

This is the normal starting point for mains voltage divider inputs.

Channel B PGA:

```cpp
.B = HLW811X_PGA_GAIN_1
```

Channel B is unused on this board.

## AC Measurement Theory And Datasheet Math

This section explains the electrical calculations behind the settings. The goal
is to make the HLW8110 numbers feel predictable instead of magic.

### RMS, Peak, And Peak-To-Peak

For a sine wave:

```text
Vpeak = Vrms * sqrt(2)
Vrms = Vpeak / sqrt(2)
Vpp = 2 * Vpeak
```

Example for a `120 V RMS` mains waveform:

```text
Vpeak = 120 * 1.414 = 169.7 V
Vpp = 339.4 V
```

The HLW8110 analog pins do not see the full mains voltage. The voltage channel
sees a divided-down copy, and the current channel sees only the small voltage
across the shunt resistor.

### Current Measurement With A Shunt

The current channel measures differential voltage across the shunt:

```text
Vshunt_rms = Irms * Rshunt
Vshunt_peak = Vshunt_rms * sqrt(2)
```

For this board:

```text
Rshunt = 3 mOhm = 0.003 ohm
```

At `5 A RMS`:

```text
Vshunt_rms = 5 * 0.003 = 0.015 V = 15 mV RMS
Vshunt_peak = 15 mV * 1.414 = 21.2 mV peak
```

At `10 mA RMS`:

```text
Vshunt_rms = 0.010 * 0.003 = 0.000030 V = 30 uV RMS
Vshunt_peak = 30 uV * 1.414 = 42.4 uV peak
```

This is why low-current accuracy is difficult. The signal is real, but it is
very small compared with noise, PCB pickup, offset, and ADC quantization.

### PGA Gain And Analog Headroom

The datasheet describes the differential analog input limit as approximately:

```text
input_peak_max = 800 mV / PGA
```

So the physical input range gets smaller as PGA increases:

| PGA | Max Input Peak | Max Input RMS |
| ---: | ---: | ---: |
| 1  | 800 mV | 565.7 mV |
| 2  | 400 mV | 282.8 mV |
| 4  | 200 mV | 141.4 mV |
| 8  | 100 mV | 70.7 mV |
| 16 | 50 mV | 35.4 mV |

With a `3 mOhm` shunt, max RMS current before analog full scale is:

```text
Imax_rms = input_rms_max / Rshunt
```

For `PGA = 16`:

```text
Imax_rms = 35.4 mV / 0.003 ohm = 11.8 A RMS
```

For a target max of `5 A`, `PGA 16` has enough headroom:

```text
5 A signal = 15 mV RMS
PGA16 limit = 35.4 mV RMS
headroom = 35.4 / 15 = 2.36x
```

That is a good bring-up setting. It improves low-current resolution while still
leaving room above 5 A. If the product must measure much above `10 A` with this
same `3 mOhm` shunt, reduce PGA to `8`.

Important library note: the current C library stores the PGA setting, but the
conversion functions do not directly use `param.pga` in the final math. That
means you should not casually change PGA and expect the same calibration-free
coefficients to stay accurate. For this board, keep the datasheet-style baseline
of:

```cpp
.A = HLW811X_PGA_GAIN_16
.U = HLW811X_PGA_GAIN_1
.B = HLW811X_PGA_GAIN_1
```

Then calibrate around that fixed setup.

### Voltage Divider Math

The voltage channel measures a divided-down version of mains:

```text
Vhlw_rms = Vmains_rms * Rlow / (Rhigh + Rlow)
```

The datasheet's calibration-free formula uses a nominal divider ratio of about:

```text
Rnom_low / Rnom_high = 1 kOhm / 1 MOhm
```

The library setting `K2` is the board's voltage divider ratio relative to that
nominal divider.

General formula:

```text
actual_ratio = Rlow / (Rhigh + Rlow)
nominal_ratio = 1 kOhm / (1 MOhm + 1 kOhm)
K2 = actual_ratio / nominal_ratio
```

If your high-side network is effectively `1 MOhm` and your low-side resistor is
near `1 kOhm`, this simplifies roughly to:

```text
K2 ~= actual_low_side_kohm / 1 kOhm
```

Example with nominal `1 MOhm + 1 kOhm` at `120 V RMS`:

```text
actual_ratio = 1000 / 1001000 = 0.000999
Vhlw_rms = 120 * 0.000999 = 119.9 mV RMS
Vhlw_peak = 169.5 mV peak
```

This is safe for voltage PGA `1`, because PGA1 allows about `800 mV peak`.

### Why K1 And K2 Divide The Result

The HLW8110 coefficient formulas are based on reference hardware values:

```text
current reference shunt = 1 mOhm
voltage reference divider ~= 1 kOhm / 1 MOhm
```

If your shunt is larger, the chip sees more voltage for the same current. A
`3 mOhm` shunt produces 3x the voltage of a `1 mOhm` shunt at the same current,
so the raw RMS current register is about 3x larger. To convert back to real
current, the library divides by `K1_A = 3`.

That is why the current correction formula is:

```text
K1_A = actual_shunt_mohm / 1 mOhm
```

And why the converted current is effectively:

```text
current_mA ~= raw_RmsIA * RmsIAC / (K1_A * 2^23)
```

Same idea for voltage:

```text
voltage_mV ~= raw_RmsU * RmsUC * 10 / (K2 * 2^22)
```

If the divider feeds more voltage into the HLW8110 than the nominal divider,
`K2` is larger, and the final calculated mains voltage is divided down.

### Library Conversion Formulas

The C library reads the chip coefficients with `hlw811x_read_coeff()`, stores
your board scaling with `hlw811x_set_resistor_ratio()`, and then applies
formulas like these.

For current A:

```text
current_mA = RmsIA * RmsIAC / (K1_A * 2^23)
```

For voltage U:

```text
voltage_mV = RmsU * RmsUC * 10 / (K2 * 2^22)
```

For active power A:

```text
active_power_mW = PowerPA * PowerPAC * 1000 / (K1_A * K2 * 2^31)
```

For apparent power:

```text
apparent_power_mW = PowerS * PowerSC * 1000 / (K1_selected * K2 * 2^31)
```

For energy, the chip accumulates pulse/count style energy internally. The
library combines:

```text
EnergyPA register
EnergyAC coefficient
HFConst
K1_A
K2
```

The important practical point is that energy depends on both current scaling and
voltage scaling. If voltage or current calibration is wrong, energy will also be
wrong.

### Active Power, Apparent Power, And Power Factor

Instantaneous power is:

```text
p(t) = v(t) * i(t)
```

Active power is the average of instantaneous power over time:

```text
P = average(v(t) * i(t))
```

This is the real work-producing power in watts. For a pure resistive load,
voltage and current are in phase, so active power is close to:

```text
P = Vrms * Irms
```

Apparent power is:

```text
S = Vrms * Irms
```

Its unit is VA, but the library returns it through the power API in `mW` style
units. Treat it as `mVA` conceptually.

Power factor is:

```text
PF = P / S
```

For a clean sine wave load:

```text
PF = cos(phase_angle)
```

For real loads such as phone chargers or switching supplies, PF also includes
distortion, so it is not only phase angle.

Examples:

```text
120 V, 5 A, resistive:
S = 120 * 5 = 600 VA
PF ~= 1
P ~= 600 W

120 V, 5 A, PF 0.5:
S = 600 VA
P = 600 * 0.5 = 300 W
```

### Energy

Energy is active power integrated over time:

```text
Wh = W * hours
kWh = Wh / 1000
```

Examples:

```text
100 W for 1 hour = 100 Wh = 0.1 kWh
600 W for 10 minutes = 600 * (10 / 60) = 100 Wh = 0.1 kWh
```

Energy accuracy depends on active power accuracy. Calibrate RMS first, then
active power, then trust energy.

### Phase Angle

The chip can measure angle between voltage and selected current channel. The
datasheet uses these scale factors:

```text
phase_deg_50Hz = AngleReg * 0.0805
phase_deg_60Hz = AngleReg * 0.0965
```

The library returns centidegrees:

```text
centidegree_50Hz = AngleReg * 8.05
centidegree_60Hz = AngleReg * 9.65
```

Phase readings need:

```cpp
hlw811x_select_channel(device, HLW811X_CHANNEL_A);
hlw811x_enable_waveform(device);
hlw811x_enable_zerocrossing(device);
k_msleep(1000);
```

Do not judge phase from a no-load circuit. Use a known load.

### Frequency

Frequency is measured from the voltage channel zero crossing. The library uses:

```text
frequency_centiHz = MCLK * 100 / 8 / Ufreq
```

With the nominal HLW8110 clock, a valid `Ufreq` value near the datasheet example
gives about `50 Hz`. If frequency reads zero, usually one of these is true:

- voltage waveform is missing or too small
- waveform/zero-crossing was not enabled
- not enough settling time has passed
- the read is happening immediately after reconfiguration

### Calibration Order

Do not calibrate every feature at once. Use this order:

1. Fix UART, parity, mux, and checksums.
2. Read coefficients from every HLW8110.
3. Set PGA and keep it fixed.
4. Set initial `K1_A = 3.0f` for the `3 mOhm` shunt.
5. Calculate initial `K2` from your voltage divider.
6. Apply a known AC voltage and calibrate `K2`.
7. Apply a known current/load and verify or fine-tune `K1_A`.
8. Calibrate RMS current offset at no load.
9. Calibrate active power gain with a known resistive load.
10. Calibrate active power offset at small load if needed.
11. Calibrate apparent power and phase only after RMS and active power are good.

### Voltage Calibration Example

Suppose:

```text
K2 = 1.000
HLW voltage reading = 128.7 V
multimeter voltage = 121.0 V
```

The HLW reading is high. Since voltage is divided by `K2`, increase `K2`:

```text
new_K2 = old_K2 * hlw_reading / reference
new_K2 = 1.000 * 128.7 / 121.0
new_K2 = 1.0636
```

Use:

```cpp
.K2 = 1.064f
```

### Current Calibration Example

Suppose:

```text
K1_A = 3.000
HLW current reading = 5.20 A
reference current = 5.00 A
```

The HLW reading is high. Since current is divided by `K1_A`, increase `K1_A`:

```text
new_K1_A = old_K1_A * hlw_reading / reference
new_K1_A = 3.000 * 5.20 / 5.00
new_K1_A = 3.120
```

Use:

```cpp
.K1_A = 3.120f
```

If your shunt tolerance is 1%, a small correction around `3.0` is normal. A very
large correction means the resistor value, analog routing, PGA setting, or
coefficient usage needs review.

### Overcurrent And Sag Thresholds

For protection thresholds, the datasheet gives theoretical formulas, but it
also recommends deriving thresholds from real measured register values when
possible. The practical workflow is:

1. Apply a known safe voltage/current.
2. Read the RMS or peak register several times.
3. Average the register value.
4. Scale that register value to the desired trip point.

This removes errors from resistor tolerance, PGA gain tolerance, and reference
voltage tolerance.

Example idea for overcurrent:

```text
RmsIA_at_5A = averaged raw or converted value
desired_trip = 10 A
threshold ~= RmsIA_at_5A * desired_trip / 5
```

For production, do this with raw register values if you are writing raw
threshold registers.

## Calibration Features In The C Library

### `hlw811x_read_coeff`

Reads factory/OTP conversion coefficients:

- `hfconst`
- RMS current A coefficient
- RMS current B coefficient
- RMS voltage coefficient
- active power A coefficient
- active power B coefficient
- apparent power coefficient
- energy A coefficient
- energy B coefficient

Call this before using converted RMS, power, or energy values.

For HLW8110 single-channel use, expect channel B coefficients to be unused or
`0xffff`.

### `hlw811x_set_resistor_ratio`

Sets board-level scaling:

- `K1_A` for current channel A shunt
- `K1_B` for current channel B shunt/CT, unused here
- `K2` for voltage divider

This does not write the HLW8110 chip. It updates the library-side conversion
math.

### `hlw811x_set_pga`

Writes analog PGA settings to the chip.

Use this before reading RMS/current/power values. Changing PGA changes the
analog input range and affects interpretation of ADC results. Keep coefficients,
ratio settings, and PGA assumptions consistent.

Recommended:

```cpp
A = 16
U = 1
B = 1
```

### `hlw811x_set_active_power_calc_mode`

Recommended:

```cpp
HLW811X_ACTIVE_POWER_MODE_POS_NEG_ALGEBRAIC
```

This preserves sign, so reverse power can be detected rather than silently
turned positive.

### `hlw811x_set_rms_calc_mode`

Recommended for AC mains:

```cpp
HLW811X_RMS_MODE_AC
```

Use DC mode only if intentionally measuring DC and you understand the high-pass
filter behavior.

### `hlw811x_set_data_update_frequency`

Options:

- `HLW811X_DATA_UPDATE_FREQ_HZ_3_4`
- `HLW811X_DATA_UPDATE_FREQ_HZ_6_8`
- `HLW811X_DATA_UPDATE_FREQ_HZ_13_65`
- `HLW811X_DATA_UPDATE_FREQ_HZ_27_3`

Recommended starting point:

```cpp
HLW811X_DATA_UPDATE_FREQ_HZ_3_4
```

Lower update rate gives slower but more stable values. Higher update rate gives
faster but noisier values.

### `hlw811x_set_channel_b_mode`

For this board, channel B is not used for current.

Recommended:

```cpp
HLW811X_B_MODE_TEMPERATURE
```

or simply avoid using channel B measurements.

Do not use B-channel comparator/leakage features unless the hardware is designed
for that use case.

### `hlw811x_select_channel`

This selects which current channel is used for apparent power, power factor,
phase angle, and some instantaneous calculations.

For this board:

```cpp
hlw811x_select_channel(device, HLW811X_CHANNEL_A);
```

### `hlw811x_enable_channel`

Enable only the physical channels in use:

```cpp
hlw811x_enable_channel(device, HLW811X_CHANNEL_A | HLW811X_CHANNEL_U);
hlw811x_disable_channel(device, HLW811X_CHANNEL_B);
```

### `hlw811x_enable_power_factor`

Required before power factor results are meaningful.

### `hlw811x_enable_waveform`

Required for zero-crossing, phase angle, frequency, waveform, peak, sag,
over-voltage, and over-current related features.

### `hlw811x_enable_zerocrossing`

Required before frequency and phase angle are meaningful.

After enabling waveform and zero-crossing, wait before reading frequency/phase:

```cpp
k_msleep(1000);
```

### `hlw811x_enable_pulse`

For this board:

```cpp
hlw811x_enable_pulse(device, HLW811X_CHANNEL_A);
```

Do not enable pulse for channel B unless channel B is actually connected and
used.

## Features To Avoid For This Board

Avoid these in normal socket metering code:

### Channel B metering

Do not use:

```cpp
hlw811x_get_rms(device, HLW811X_CHANNEL_B, ...)
hlw811x_get_power(device, HLW811X_CHANNEL_B, ...)
hlw811x_get_energy(device, HLW811X_CHANNEL_B, ...)
```

Channel B is not part of this board's current measurement path.

### B-channel comparator

Avoid:

```cpp
hlw811x_enable_b_channel_comparator()
```

The library notes that comparator mode conflicts with B-channel current
measurement and has special hardware behavior. It is not needed for this board.

### B leakage interrupt

Avoid using `HLW811X_INTR_B_LEAKAGE` unless the board is explicitly wired and
validated for leakage detection.

### `hlw811x_calc_current_gain_b`

This is for matching channel B to channel A. Not useful on this board.

## Calibration Workflow

### 1. Communication Baseline

Read:

- `SYS_STATUS`
- coefficient registers
- voltage RMS
- current A RMS

All should return `err=0`.

### 2. Voltage Ratio Calibration

Use a trusted multimeter.

Formula:

```text
new_K2 = old_K2 * hlw_voltage_reading / reference_voltage
```

Update `.K2`.

### 3. Current Ratio Check

With a known load or current source:

```text
new_K1_A = old_K1_A * hlw_current_reading / reference_current
```

For the hardware shunt value, start with:

```text
K1_A = 3.0
```

Small correction from the exact shunt tolerance is normal.

### 4. RMS Offset Calibration

Use when no load is connected and current reading is not close enough to zero.

The library provides:

```cpp
hlw811x_calc_rms_offset(device, HLW811X_CHANNEL_A, &rms_offset);
```

The calculated value must be written into calibration storage/registers using
`hlw811x_apply_calibration()` or a specific register write flow. Do not blindly
apply it until the no-load setup is stable.

### 5. Active Power Gain Calibration

Use a known real load at power factor close to 1.

The library provides:

```cpp
hlw811x_calc_active_power_gain(device, error_pct, &pa_gain);
```

This calculates a gain correction from known measurement error. The value must
then be written as part of calibration.

### 6. Active Power Offset Calibration

Use a small known load, typically around 5% of nominal current.

The library provides:

```cpp
hlw811x_calc_active_power_offset(device, HLW811X_CHANNEL_A, error_pct,
    &pa_offset);
```

### 7. Apparent Power Calibration

Use after RMS and active power are reasonable.

The library provides:

```cpp
hlw811x_calc_apparent_power_gain(device, &ps_gain);
hlw811x_calc_apparent_power_offset(device, &ps_offset);
```

### 8. Phase Calibration

Use a known power-factor load if phase accuracy matters.

The calibration struct includes:

```cpp
phase_a
```

Do not tune phase from random loads.

## Known Library Issues / Cautions

These are issues observed while integrating the library:

### 1-byte UART register replies

Some HLW8110 registers return:

```text
1 data byte + checksum
```

The external library originally rejected UART RX frames shorter than 3 bytes.
For `SYS_STATUS`, the valid frame may be only 2 bytes total.

Keep the local fix that allows `rx_len >= 2`.

### `hlw811x_read_current_channel`

The library reads `METER_STATUS` as a 16-bit value but shifts by 21 bits. That is
not valid for a 16-bit value. Do not trust this function until corrected.

Since this board always uses channel A, explicitly call:

```cpp
hlw811x_select_channel(device, HLW811X_CHANNEL_A);
```

and treat channel A as selected.

### `hlw811x_set_zerocrossing_mode`

Review before relying on it. The implementation reads one control register and
writes another. Use the simpler enable path first:

```cpp
hlw811x_enable_waveform(device);
hlw811x_enable_zerocrossing(device);
```

Then verify frequency/phase on real AC.

### Submodule edits

`src/sensors/externalLibs/hlw811x` is a submodule. If modifying the external C
library, commit the change inside the submodule or vendor the library into the
main repository intentionally.

## Practical Bring-Up Interpretation

Good signs:

```text
read_coeff err=0
set_pga err=0
enable_channels err=0
rms_voltage err=0
rms_current_a err=0
active_power_a err=0
```

Expected with no load:

```text
current near 0
active power near 0
energy 0 or slowly changing
power factor may be meaningless
phase angle may be meaningless
```

Frequency showing zero usually means one of these:

- zero-crossing was just enabled and has not settled
- waveform/zero-crossing path is not enabled correctly
- no valid AC voltage waveform is present
- the library function/setting sequence needs review

For production code, separate configuration from measurement:

1. Configure once after boot/reset.
2. Wait for data update settling.
3. Read measurements periodically.
4. Do not rewrite configuration every measurement loop.
