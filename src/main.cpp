/*
 * Copyright (c) 2023, Meta
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <array>
#include <cstring>
#include <iostream>
#include <span>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/logging/log.h>

#include "hardware/gpio_con.hpp"
#include "hardware/uart_con.hpp"
#include "sensors/hlw811x.hpp"
#include "cellular/at_engine.hpp"
#include "cellular/sim7080.hpp"

LOG_MODULE_REGISTER(mainCpp, LOG_LEVEL_INF);

hardware::gpioCon utilityPwrLed(GPIO_DT_SPEC_GET(DT_ALIAS(utility_pwr_led), gpios), GPIO_OUTPUT_HIGH);
hardware::gpioCon cellularLed(GPIO_DT_SPEC_GET(DT_ALIAS(cellular_led), gpios), GPIO_OUTPUT_HIGH);
hardware::gpioCon relay1(GPIO_DT_SPEC_GET(DT_ALIAS(relay1), gpios), GPIO_OUTPUT_INACTIVE);
hardware::gpioCon relay2(GPIO_DT_SPEC_GET(DT_ALIAS(relay2), gpios), GPIO_OUTPUT_INACTIVE);
hardware::gpioCon relay3(GPIO_DT_SPEC_GET(DT_ALIAS(relay3), gpios), GPIO_OUTPUT_INACTIVE);
hardware::gpioCon relay4(GPIO_DT_SPEC_GET(DT_ALIAS(relay4), gpios), GPIO_OUTPUT_INACTIVE);
// hardware::gpioCon gsmDtr(GPIO_DT_SPEC_GET(DT_ALIAS(gsm_dtr), gpios), GPIO_OUTPUT_INACTIVE);
// hardware::gpioCon gsmPwrKey(GPIO_DT_SPEC_GET(DT_ALIAS(gsm_pwrkey), gpios), GPIO_OUTPUT_INACTIVE);

// cellular::atEngine atEngine_(DEVICE_DT_GET(DT_ALIAS(gsm_uart)));


cellular::sim7080 modemSim7080(DEVICE_DT_GET(DT_ALIAS(gsm_uart)), 
                                GPIO_DT_SPEC_GET(DT_ALIAS(gsm_pwrkey), gpios) 
							,GPIO_OUTPUT_INACTIVE);

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


int main(void)
{

	utilityPwrLed.init();
	cellularLed.init();
	relay1.init();
	relay2.init();
	relay3.init();
	relay4.init();

	int ret = energyMeters.init();


	for (uint8_t meter = 1; meter <= 4; ++meter)
	{
		energyMeterErr[meter - 1] = energyMeters.configureIndividual(meter, baselineRatio, baselinePga);
	}

	relay1.set(1);
	relay2.set(1);
	relay3.set(1);
	relay4.set(1);

	k_msleep(5000);

	modemSim7080.init();



	int64_t nextGsmTx = k_uptime_get();

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

				// std::cout << "HLW811x meter " << static_cast<int>(meter)
				// 		  << " ret=" << ret
				// 		  << " mV " << acMeasurements[meter - 1].mV
				// 		  << " mA " << acMeasurements[meter - 1].mA
				// 		  << " mW " << acMeasurements[meter - 1].mW
				// 		  << " apparentmW " << acMeasurements[meter - 1].apparentmW
				// 		  << " wH " << acMeasurements[meter - 1].wH
				// 		  << " hZ " << acMeasurements[meter - 1].hZ
				// 		  << " pF " << acMeasurements[meter - 1].pF
				// 		  << std::endl;
				LOG_INF("HLW811x meter %d", static_cast<int>(meter));
				LOG_INF("ret= %d", ret);
				LOG_INF("mV %d",  acMeasurements[meter - 1].mV);
				LOG_INF("mA %d", acMeasurements[meter - 1].mA);
				LOG_INF("mW %d", acMeasurements[meter - 1].mW);
				LOG_INF("apparentmW %d", acMeasurements[meter - 1].apparentmW);
				LOG_INF("wH %d", acMeasurements[meter - 1].wH);
				LOG_INF("hZ %d", acMeasurements[meter - 1].hZ);
				LOG_INF("pF %d", acMeasurements[meter - 1].pF);
				
			}

			k_msleep(300);
		}

	

		k_msleep(1000);
	}
	return 0;
}
