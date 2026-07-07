/*
 * Copyright (c) 2023, Meta
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <array>
#include <cstring>
#include <iostream>
#include <span>
#include <string_view>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/logging/log.h>

#include "hardware/gpio_con.hpp"
#include "hardware/uart_con.hpp"
#include "hardware/adc_con.hpp"
#include "sensors/hlw811x.hpp"
#include "cellular/at_engine.hpp"
#include "cellular/sim7080.hpp"
#include "sms_commands.hpp"

LOG_MODULE_REGISTER(mainCpp, LOG_LEVEL_INF);

hardware::gpioCon utilityPwrLed(GPIO_DT_SPEC_GET(DT_ALIAS(utility_pwr_led), gpios), GPIO_OUTPUT_HIGH);
hardware::gpioCon cellularLed(GPIO_DT_SPEC_GET(DT_ALIAS(cellular_led), gpios), GPIO_OUTPUT_HIGH);
hardware::gpioCon relay1(GPIO_DT_SPEC_GET(DT_ALIAS(relay1), gpios), GPIO_OUTPUT_INACTIVE);
hardware::gpioCon relay2(GPIO_DT_SPEC_GET(DT_ALIAS(relay2), gpios), GPIO_OUTPUT_INACTIVE);
hardware::gpioCon relay3(GPIO_DT_SPEC_GET(DT_ALIAS(relay3), gpios), GPIO_OUTPUT_INACTIVE);
hardware::gpioCon relay4(GPIO_DT_SPEC_GET(DT_ALIAS(relay4), gpios), GPIO_OUTPUT_INACTIVE);

hardware::adcCon batteryAdc(HARDWARE_ADC_CHANNEL_DT_SPEC_GET(DT_ALIAS(batt_volt_adc)), 110000, 110000);

cellular::sim7080 modemSim7080(DEVICE_DT_GET(DT_ALIAS(gsm_uart)),
							   GPIO_DT_SPEC_GET(DT_ALIAS(gsm_pwrkey), gpios), GPIO_OUTPUT_INACTIVE);

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

const hlw811x_resistor_ratio baselineRatio = {
	.K1_A = 3.0f,
	.K1_B = 1.0f,
	.K2 = 1.064f,
};

std::array<int, 4> energyMeterErr;

std::array<sensors::hlw811x::measurements, 4> acMeasurements;
std::array<hardware::gpioCon *, device_status::outletCount> outletRelays = {{
	&relay1,
	&relay2,
	&relay3,
	&relay4,
}};

std::string_view alert_number = "1234567890";
uint32_t heartBeatDaysMilli = 0;
uint16_t batteryCentivolts = 0;

bool sms_network_ready(const cellular::sim7080::modemInformation &modemInfo)
{
	return modemInfo.networkRegistration == 1 || modemInfo.networkRegistration == 5;
}

int main(void)
{
	const int64_t bootTimeMs = k_uptime_get();

	utilityPwrLed.init();
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
		energyMeterErr[meter - 1] = energyMeters.configureIndividual(meter, baselineRatio, baselinePga);
	}

	relay1.set(0);
	relay2.set(0);
	relay3.set(0);
	relay4.set(0);

	// k_msleep(5000);

	modemSim7080.init();

	cellular::sim7080::modemInformation sim7080Information{};
	bool startupStatusSent = false;

	while (1)
	{

		for (uint8_t meter = 1; meter <= 4; ++meter)
		{
			if (energyMeterErr[meter - 1] != 0)
			{
				energyMeters.configureIndividual(meter, baselineRatio, baselinePga);
			}
			else
			{
				int ret = energyMeters.measurement(meter, acMeasurements[meter - 1]);
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

		int32_t batteryMillivolts = 0;
		int batteryRet = batteryAdc.read_divider_millivolts(batteryMillivolts);
		if (batteryRet == 0 && batteryMillivolts >= 0)
		{
			batteryCentivolts = static_cast<uint16_t>((batteryMillivolts + 5) / 10);
		}
		else
		{
			LOG_ERR("Battery ADC read failed: %d", batteryRet);
		}
		LOG_INF("battery mV %d", batteryMillivolts);

		modemSim7080.loop(sim7080Information);

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
			.relays = std::span<hardware::gpioCon *, device_status::outletCount>{outletRelays},
			.measurements = std::span<const sensors::hlw811x::measurements, device_status::outletCount>{acMeasurements},
			.bootTimeMs = bootTimeMs,
			.heartBeatDaysMilli = heartBeatDaysMilli,
			.batteryCentivolts = batteryCentivolts,
		};

		if (!startupStatusSent && sms_network_ready(sim7080Information))
		{
			startupStatusSent = sms_commands::send_status(smsContext, alert_number, "SPS is starting");
		}

		if (sim7080Information.smsReceived)
		{
			LOG_INF("smsNumber: %s", sim7080Information.smsNumber.data());
			LOG_INF("smsBody: %s", sim7080Information.smsBody.data());
			sms_commands::handle(smsContext);
		}
		LOG_INF("#########END#########");

		k_msleep(500);
	}
	return 0;
}
