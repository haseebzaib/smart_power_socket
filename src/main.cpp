/*
 * Copyright (c) 2023, Meta
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <iostream>
#include <zephyr/kernel.h>
#include "hardware/gpio_con.hpp"
#include "sensors/hlw811x.hpp"

hardware::gpioCon utilityPwrLed(GPIO_DT_SPEC_GET(DT_ALIAS(utility_pwr_led), gpios), GPIO_OUTPUT);
hardware::gpioCon cellularLed(GPIO_DT_SPEC_GET(DT_ALIAS(cellular_led), gpios), GPIO_OUTPUT);
sensors::hlw811x energyMeters(
	DEVICE_DT_GET(DT_ALIAS(hlw_uart)),
	GPIO_DT_SPEC_GET(DT_ALIAS(energy_metering_mux_a), gpios),
	GPIO_DT_SPEC_GET(DT_ALIAS(energy_metering_mux_b), gpios),
	4);

int main(void)
{
	std::cout << "Hello, C++ world! " << CONFIG_BOARD << std::endl;

	utilityPwrLed.init();
	cellularLed.init();
	int ret = energyMeters.init();
	std::cout << "HLW811x init: " << ret << std::endl;

	uint16_t sysStatus = 0;

	while (1)
	{
		utilityPwrLed.toggle();
		cellularLed.toggle();
		hlw811x_error_t err = energyMeters.readSysStatus(1, sysStatus);
		std::cout << "HLW811x meter 1 sys status err=" << err
				  << " value=0x" << std::hex << sysStatus << std::dec << std::endl;

		k_msleep(1000);
	}
	return 0;
}
