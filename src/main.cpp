/*
 * Copyright (c) 2023, Meta
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <array>
#include <iostream>
#include <span>
#include <string_view>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/logging/log.h>

#include "hardware/gpio_con.hpp"
#include "hardware/uart_con.hpp"
#include "hardware/adc_con.hpp"
#include "sensors/hlw811x.hpp"
#include "cellular/at_engine.hpp"
#include "cellular/sim7080.hpp"
#include "sms_commands.hpp"
#include "alerts.hpp"
#include "app_config.hpp"
#include "system/watchdog.hpp"
#if defined(CONFIG_BOOTLOADER_MCUBOOT)
#include "ota/firmware_update.hpp"
#include "sps_boot.h"
#endif

LOG_MODULE_REGISTER(mainCpp, LOG_LEVEL_INF);

hardware::gpioCon utilityPwrLed(GPIO_DT_SPEC_GET(DT_ALIAS(utility_pwr_led), gpios), GPIO_OUTPUT_HIGH);
#if DT_NODE_EXISTS(DT_ALIAS(fuse_detection))
hardware::gpioCon fuseDetection(GPIO_DT_SPEC_GET(DT_ALIAS(fuse_detection), gpios), GPIO_INPUT);
bool fuseDetectionReady = false;
namespace
{
	const gpio_dt_spec fuseDetectionGpio = GPIO_DT_SPEC_GET(DT_ALIAS(fuse_detection), gpios);
	gpio_callback fuseDetectionCallback{};
	k_spinlock fusePulseLock{};
	std::array<uint32_t, 8> fusePeriodsUs{};
	uint32_t fusePeriodSumUs = 0;
	uint32_t fusePreviousCycle = 0;
	uint32_t fuseEdgeCount = 0;
	uint32_t fuseRejectedIntervals = 0;
	uint32_t fuseLastPeriodUs = 0;
	int64_t fuseLastEdgeMs = -1;
	size_t fusePeriodCount = 0;
	size_t fusePeriodIndex = 0;
	bool fuseFrequencyReady = false;

	void fuse_detection_edge(const device *, gpio_callback *, gpio_port_pins_t)
	{
		const uint32_t cycle = k_cycle_get_32();
		const int64_t nowMs = k_uptime_get();
		const auto key = k_spin_lock(&fusePulseLock);
		const uint32_t periodUs = static_cast<uint32_t>(
			k_cyc_to_us_floor64(static_cast<uint32_t>(cycle - fusePreviousCycle)));
		++fuseEdgeCount;
		fuseLastPeriodUs = fuseLastEdgeMs >= 0 ? periodUs : 0;
		// Two active pulses per mains cycle. Reject glitches and missing cycles;
		// 6-15 ms covers the expected 50/60 Hz half-cycle interval.
		if (fuseLastEdgeMs >= 0 && nowMs - fuseLastEdgeMs <= 40 &&
			periodUs >= 6000 && periodUs <= 15000)
		{
			if (fusePeriodCount == fusePeriodsUs.size())
				fusePeriodSumUs -= fusePeriodsUs[fusePeriodIndex];
			else
				++fusePeriodCount;
			fusePeriodsUs[fusePeriodIndex] = periodUs;
			fusePeriodSumUs += periodUs;
			fusePeriodIndex = (fusePeriodIndex + 1) % fusePeriodsUs.size();
		}
		else
		{
			if (fuseLastEdgeMs >= 0) ++fuseRejectedIntervals;
			fusePeriodCount = 0;
			fusePeriodIndex = 0;
			fusePeriodSumUs = 0;
		}
		fusePreviousCycle = cycle;
		fuseLastEdgeMs = nowMs;
		k_spin_unlock(&fusePulseLock, key);
	}

	void log_fuse_detection(bool acPresent)
	{
		const int64_t nowMs = k_uptime_get();
		const auto key = k_spin_lock(&fusePulseLock);
		const bool frequencyValid = fuseFrequencyReady && acPresent &&
			fuseLastEdgeMs >= 0 && nowMs - fuseLastEdgeMs <= 40 && fusePeriodCount >= 3;
		const uint32_t frequencyCentiHz = frequencyValid ?
			static_cast<uint32_t>(50000000ULL * fusePeriodCount / fusePeriodSumUs) : 0;
		const uint32_t edges = fuseEdgeCount;
		const uint32_t rejected = fuseRejectedIntervals;
		const uint32_t lastPeriodUs = fuseLastPeriodUs;
		const unsigned validPeriods = static_cast<unsigned>(fusePeriodCount);
		const int64_t lastEdgeAgeMs = fuseLastEdgeMs >= 0 ? nowMs - fuseLastEdgeMs : -1;
		k_spin_unlock(&fusePulseLock, key);

		static int64_t nextLogMs = 0;
		static bool lastPresent = false;
		static bool lastFrequencyValid = false;
		if (nowMs < nextLogMs && acPresent == lastPresent &&
			frequencyValid == lastFrequencyValid) return;
		if (!acPresent)
			LOG_INF("fuse_detection: AC unavailable, freq unavailable");
		else if (frequencyValid)
			LOG_INF("fuse_detection: AC available, freq ~%u.%02u Hz (pulse %u.%02u Hz)",
					frequencyCentiHz / 100U, frequencyCentiHz % 100U,
					frequencyCentiHz * 2U / 100U, frequencyCentiHz * 2U % 100U);
		else
		{
			const char *reason = !fuseFrequencyReady ? "pulse timing disabled" :
				edges == 0 ? "no active edges observed" :
				lastEdgeAgeMs > 40 ? "no recent active edges" : "insufficient valid intervals";
			LOG_INF("fuse_detection: AC available, freq unavailable (%s)", reason);
			LOG_INF("fuse_detection: raw=%d edges=%u valid_intervals=%u rejected=%u last_period_us=%u edge_age_ms=%lld",
					gpio_pin_get_raw(fuseDetectionGpio.port, fuseDetectionGpio.pin),
					edges, validPeriods, rejected, lastPeriodUs,
					static_cast<long long>(lastEdgeAgeMs));
		}
		lastPresent = acPresent;
		lastFrequencyValid = frequencyValid;
		nextLogMs = nowMs + 10000;
	}
}
#endif

bool input_ac_present()
{
#if DT_NODE_EXISTS(DT_ALIAS(fuse_detection))
	if (!fuseDetectionReady)
	{
		return false;
	}

	// AC optocoupler output pulses low. Observe at least two 50 Hz cycles
	// rather than treating a high sample near a zero crossing as no AC.
	const int64_t deadlineMs = k_uptime_get() + 40;
	do
	{
		const int value = fuseDetection.get();
		if (value < 0)
		{
			LOG_ERR("Fuse detection GPIO read failed: %d", value);
			return false;
		}
		if (value == 1) // GPIO_ACTIVE_LOW: a low level means input AC present.
		{
			log_fuse_detection(true);
			return true;
		}
		k_msleep(1);
	} while (k_uptime_get() < deadlineMs);
	log_fuse_detection(false);
#endif
	return false;
}

hardware::gpioCon cellularLed(GPIO_DT_SPEC_GET(DT_ALIAS(cellular_led), gpios), GPIO_OUTPUT_HIGH);
// State reporting/persistence reads the pin; keep the input buffer connected
// while driving each relay output. Nordic GPIO disconnects it for output-only.
hardware::gpioCon relay1(GPIO_DT_SPEC_GET(DT_ALIAS(relay1), gpios), GPIO_OUTPUT_INACTIVE | GPIO_INPUT);
hardware::gpioCon relay2(GPIO_DT_SPEC_GET(DT_ALIAS(relay2), gpios), GPIO_OUTPUT_INACTIVE | GPIO_INPUT);
hardware::gpioCon relay3(GPIO_DT_SPEC_GET(DT_ALIAS(relay3), gpios), GPIO_OUTPUT_INACTIVE | GPIO_INPUT);
hardware::gpioCon relay4(GPIO_DT_SPEC_GET(DT_ALIAS(relay4), gpios), GPIO_OUTPUT_INACTIVE | GPIO_INPUT);

hardware::adcCon batteryAdc(HARDWARE_ADC_CHANNEL_DT_SPEC_GET(DT_ALIAS(batt_volt_adc)), 110000, 110000);

cellular::sim7080 modemSim7080(DEVICE_DT_GET(DT_ALIAS(gsm_uart)),
							   GPIO_DT_SPEC_GET(DT_ALIAS(gsm_pwrkey), gpios), GPIO_OUTPUT_INACTIVE,
							   GPIO_DT_SPEC_GET(DT_ALIAS(gsm_dtr), gpios), GPIO_OUTPUT_INACTIVE);

sensors::hlw811x energyMeters(
	DEVICE_DT_GET(DT_ALIAS(hlw_uart)),
	GPIO_DT_SPEC_GET(DT_ALIAS(energy_metering_mux_a), gpios),
	GPIO_DT_SPEC_GET(DT_ALIAS(energy_metering_mux_b), gpios),
	4);

const hlw811x_pga baselinePga = {
	.A = HLW811X_PGA_GAIN_16,
	.B = HLW811X_PGA_GAIN_1,
	.U = HLW811X_PGA_GAIN_1,
};

std::array<int, 4> energyMeterErr;
std::array<int64_t, 4> nextEnergyMeterRetryMs;

std::array<sensors::hlw811x::measurements, 4> acMeasurements;
std::array<bool, device_status::outletCount> monitoredLoadConnected{};
std::array<bool, device_status::outletCount> monitoredLoadStateValid{};
std::array<uint32_t, device_status::outletCount> relayChangeSequence{};
std::array<hardware::gpioCon *, device_status::outletCount> outletRelays = {{
	&relay1,
	&relay2,
	&relay3,
	&relay4,
}};

std::string_view alert_number = "1234567890";
uint32_t heartBeatDaysMilli = 0;
uint32_t batteryMillivolts = 0;
alerts::utilityPowerMonitor utilityPowerAlerts;
alerts::devicePowerMonitor devicePowerAlerts;
alerts::frequencyMonitor frequencyAlerts;
alerts::powerFactorMonitor powerFactorAlerts;

constexpr int64_t energyMeterRetryIntervalMs = 30000;
constexpr int64_t modemInitRetryIntervalMs = 30000;
constexpr int64_t startupStatusRetryIntervalMs = 5LL * 60LL * 1000LL;

bool sms_network_ready(const cellular::sim7080::modemInformation &modemInfo)
{
	return modemInfo.networkRegistration == 1 || modemInfo.networkRegistration == 5;
}

int main(void)
{
	const int64_t bootTimeMs = k_uptime_get();
	uint32_t resetCause = 0;
	int resetRet = hwinfo_get_reset_cause(&resetCause);
#if defined(CONFIG_BOOTLOADER_MCUBOOT)
    sps_boot_state bootState{};
    if (sps_boot_state_load(&bootState) == 0)
    {
        resetCause = bootState.reset_cause;
        resetRet = 0;
    }
#endif
	if (resetRet == 0)
	{
		LOG_INF("Boot reset cause: 0x%08x pin=%u software=%u watchdog=%u lockup=%u debug=%u",
				resetCause, !!(resetCause & RESET_PIN), !!(resetCause & RESET_SOFTWARE),
				!!(resetCause & RESET_WATCHDOG), !!(resetCause & RESET_CPU_LOCKUP),
				!!(resetCause & RESET_DEBUG));
		if (resetCause == 0)
			LOG_INF("No classified reset cause; power-on/supply interruption remains possible");
		const int clearRet = hwinfo_clear_reset_cause();
		if (clearRet != 0) LOG_WRN("Reset cause clear failed: %d", clearRet);
	}
	else
		LOG_WRN("Reset cause unavailable: %d", resetRet);
#if defined(CONFIG_BOOTLOADER_MCUBOOT)
	const int firmwareUpdateRet = firmware_update::init();
	LOG_INF("OTA init ret: %d", firmwareUpdateRet);
#endif
	const int appConfigRet = app_config::init();
	LOG_INF("App config init ret: %d", appConfigRet);
	heartBeatDaysMilli = app_config::heartbeat_days_milli();
	int64_t nextHeartbeatMs = device_status::next_heartbeat_uptime_ms(bootTimeMs,
													 heartBeatDaysMilli,
													 k_uptime_get());

	utilityPwrLed.init();
#if DT_NODE_EXISTS(DT_ALIAS(fuse_detection))
	const int fuseDetectionRet = fuseDetection.init();
	fuseDetectionReady = fuseDetectionRet == 0;
	if (!fuseDetectionReady)
	{
		LOG_ERR("Fuse detection GPIO init failed: %d", fuseDetectionRet);
	}
	else
	{
		gpio_init_callback(&fuseDetectionCallback, fuse_detection_edge,
						   BIT(fuseDetectionGpio.pin));
		int ret = gpio_add_callback(fuseDetectionGpio.port, &fuseDetectionCallback);
		if (ret == 0)
			ret = gpio_pin_interrupt_configure_dt(&fuseDetectionGpio, GPIO_INT_EDGE_TO_ACTIVE);
		fuseFrequencyReady = ret == 0;
		if (!fuseFrequencyReady)
			LOG_WRN("fuse_detection: pulse timing init failed: %d; AC presence still sampled", ret);
		else
			LOG_INF("fuse_detection: pulse timing enabled on P1.12; two pulses per AC cycle");
	}
#endif
	cellularLed.init();
	relay1.init();
	relay2.init();
	relay3.init();
	relay4.init();
	int batteryAdcInitRet = batteryAdc.init();
	LOG_INF("Battery ADC init ret: %d", batteryAdcInitRet);

	energyMeters.init();

	for (uint8_t meter = 1; meter <= 4; ++meter)
	{
		const hlw811x_resistor_ratio ratio = app_config::ratio_for_outlet(meter - 1);
		LOG_INF("Configuring HLW811x meter %u", static_cast<unsigned int>(meter));
		energyMeterErr[meter - 1] = energyMeters.configureIndividual(meter, ratio, baselinePga);
		LOG_INF("HLW811x meter %u configure ret: %d",
				static_cast<unsigned int>(meter),
				energyMeterErr[meter - 1]);
		if (energyMeterErr[meter - 1] != 0)
		{
			nextEnergyMeterRetryMs[meter - 1] = k_uptime_get() + energyMeterRetryIntervalMs;
			LOG_WRN("HLW811x meter %u initialization failed; retry due in 30 s when main loop runs",
					static_cast<unsigned>(meter));
		}
	}

	const uint8_t startupRelayMask = app_config::relay_state_valid() ? app_config::relay_on_mask() : 0x0f;
	for (std::size_t i = 0; i < outletRelays.size(); ++i)
	{
		outletRelays[i]->set((startupRelayMask & (1U << i)) != 0U ? 0 : 1);
	}

	int modemInitRet = modemSim7080.init();
	bool modemReady = modemInitRet == 0;
	int64_t nextModemInitRetryMs =
		modemReady ? 0 : k_uptime_get() + modemInitRetryIntervalMs;
	LOG_INF("Modem init ret: %d", modemInitRet);

	cellular::sim7080::modemInformation sim7080Information{};
	bool startupStatusQueued = false;
	int64_t nextStartupStatusAttemptMs = 0;

	while (1)
	{
		app_watchdog::progress();

		for (uint8_t meter = 1; meter <= 4; ++meter)
		{
			if (energyMeterErr[meter - 1] != 0)
			{
				const int64_t nowMs = k_uptime_get();
				if (nowMs < nextEnergyMeterRetryMs[meter - 1])
				{
					continue;
				}

				const hlw811x_resistor_ratio ratio = app_config::ratio_for_outlet(meter - 1);
				LOG_INF("HLW811x meter %u configure retry begin (previous error=%d)",
						static_cast<unsigned>(meter), energyMeterErr[meter - 1]);
				energyMeterErr[meter - 1] = energyMeters.configureIndividual(meter, ratio, baselinePga);
				if (energyMeterErr[meter - 1] != 0)
				{
					nextEnergyMeterRetryMs[meter - 1] = k_uptime_get() + energyMeterRetryIntervalMs;
					LOG_WRN("HLW811x meter %u configure retry failed: %d; retry due in 30 s",
							static_cast<unsigned int>(meter),
							energyMeterErr[meter - 1]);
				}
				else
				{
					nextEnergyMeterRetryMs[meter - 1] = 0;
					LOG_INF("HLW811x meter %u configure retry succeeded; measurements resume next loop",
							static_cast<unsigned>(meter));
				}
			}
			else
			{
				int ret = energyMeters.measurement(meter, acMeasurements[meter - 1]);
				if(ret != 0)
				{   
                         acMeasurements[meter - 1] = sensors::hlw811x::measurements{};
				}
				LOG_INF("#########START#########");
				LOG_INF("HLW811x meter %d", static_cast<int>(meter));
				LOG_INF("ret= %d", ret);
				LOG_INF("mV %d", acMeasurements[meter - 1].mV);
				LOG_INF("mA %d", acMeasurements[meter - 1].mA);
				LOG_INF("mW %d", acMeasurements[meter - 1].mW);
				LOG_INF("apparentmW %d", acMeasurements[meter - 1].apparentmW);
				LOG_INF("wH %d", acMeasurements[meter - 1].wH);
				LOG_INF("hZ %d", acMeasurements[meter - 1].hZ);
				LOG_INF("pF %d", acMeasurements[meter - 1].pF);
				LOG_INF("#########END#########");
			}

			k_msleep(300);
		}

		int32_t batteryReadingMillivolts = 0;
		int batteryRet = batteryAdc.read_divider_millivolts(batteryReadingMillivolts);
		if (batteryRet == 0 && batteryReadingMillivolts >= 0)
		{
			batteryMillivolts = static_cast<uint32_t>(batteryReadingMillivolts);
		}
		else
		{
			LOG_ERR("Battery ADC read failed: %d", batteryRet);
		}
		LOG_INF("battery mV %u", static_cast<unsigned int>(batteryMillivolts));

		if (!modemReady)
		{
			const int64_t nowMs = k_uptime_get();
			if (nowMs >= nextModemInitRetryMs)
			{
				LOG_INF("Retrying modem initialization");
				modemInitRet = modemSim7080.init();
				modemReady = modemInitRet == 0;
				LOG_INF("Modem init retry ret: %d", modemInitRet);
				if (!modemReady)
				{
					nextModemInitRetryMs = k_uptime_get() + modemInitRetryIntervalMs;
				}
			}

			if (!modemReady)
			{
#if defined(CONFIG_BOOTLOADER_MCUBOOT)
				firmware_update::poll_health(appConfigRet == 0);
#endif
				k_msleep(500);
				continue;
			}
		}

		modemSim7080.loop(sim7080Information, modemSim7080.can_queue_sms(sms_commands::maxCommandReplyMessages));

		LOG_INF("#########Sim7080Information#########");
		LOG_INF("ModelIdentification: %s", sim7080Information.modelIdentification.data());
		LOG_INF("Pin status: %s", sim7080Information.pin.data());
		LOG_INF("carrier= %s", sim7080Information.carrier.data());
		LOG_INF("imei: %s", sim7080Information.imei.data());
		LOG_INF("simId: %s", sim7080Information.simId.data());
		LOG_INF("longitude: %s", sim7080Information.longitude.data());
		LOG_INF("latitude: %s", sim7080Information.latitude.data());
		LOG_INF("networkRegistration: %d", sim7080Information.networkRegistration);
		LOG_INF("networkQuality: %d", sim7080Information.networkQuality);
		LOG_INF("dataConnected: %d", sim7080Information.dataConnected);
		LOG_INF("ipAddress: %s", sim7080Information.ipAddress.data());
		LOG_INF("smsReceived: %d", sim7080Information.smsReceived);

		sms_commands::context smsContext{
			.modem = modemSim7080,
			.modemInformation = sim7080Information,
			.energyMeters = &energyMeters,
			.meterPga = &baselinePga,
			.relays = std::span<hardware::gpioCon *, device_status::outletCount>{outletRelays},
			.measurements = std::span<const sensors::hlw811x::measurements, device_status::outletCount>{acMeasurements},
			.monitoredLoadConnected = std::span<bool, device_status::outletCount>{monitoredLoadConnected},
			.monitoredLoadStateValid = std::span<bool, device_status::outletCount>{monitoredLoadStateValid},
			.bootTimeMs = bootTimeMs,
			.heartBeatDaysMilli = heartBeatDaysMilli,
			.nextHeartbeatMs = nextHeartbeatMs,
			.batteryMillivolts = batteryMillivolts,
			.inputAcPresent = input_ac_present(),
			.relayChangeSequence = std::span<uint32_t, device_status::outletCount>{relayChangeSequence},
			.bootResetCause = resetCause,
			.bootResetCauseValid = resetRet == 0,
		};

		// Command admission reserved enough free slots for its entire reply group.
		// Execute once before background producers can use those slots.
		if (sim7080Information.smsReceived)
		{
			LOG_INF("smsNumber: %s", sim7080Information.smsNumber.data());
			LOG_INF("smsBody: %s", sim7080Information.smsBody.data());
			sms_commands::handle(smsContext);
		}
		modemSim7080.process_sms_outbox();

		if (!startupStatusQueued &&
			sms_network_ready(sim7080Information) &&
			k_uptime_get() >= nextStartupStatusAttemptMs)
		{
			startupStatusQueued = sms_commands::send_status(smsContext, alert_number, "SPS is starting");
			if (!startupStatusQueued)
			{
				nextStartupStatusAttemptMs = k_uptime_get() + startupStatusRetryIntervalMs;
				LOG_WRN("Startup status could not be queued; retrying in %lld seconds",
						static_cast<long long>(startupStatusRetryIntervalMs / 1000));
			}
		}

		// Detect events even while unregistered; the shared outbox waits for
		// network recovery before attempting submission.
		utilityPowerAlerts.process(smsContext, alert_number);
		devicePowerAlerts.process(smsContext, alert_number);
		frequencyAlerts.process(smsContext, alert_number);
		if (app_config::power_factor_alerts_enabled())
		{
			powerFactorAlerts.process(smsContext, alert_number);
		}
		else
		{
			powerFactorAlerts.reset();
		}

		if (startupStatusQueued &&
			sms_network_ready(sim7080Information) &&
			heartBeatDaysMilli > 0U &&
			nextHeartbeatMs > 0 &&
			k_uptime_get() >= nextHeartbeatMs)
		{
			if (sms_commands::send_status(smsContext, alert_number, "SPS Heartbeat"))
			{
				nextHeartbeatMs = device_status::next_heartbeat_uptime_ms(bootTimeMs,
														 heartBeatDaysMilli,
														 k_uptime_get());
			}
		}

		modemSim7080.process_sms_outbox();
		LOG_INF("#########END#########");

#if defined(CONFIG_BOOTLOADER_MCUBOOT)
		firmware_update::poll_health(appConfigRet == 0);
#endif
		k_msleep(500);
	}
	return 0;
}
