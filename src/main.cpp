/*
 * Copyright (c) 2023, Meta
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <iostream>
#include <zephyr/kernel.h>
#include "hardware/gpio_con.hpp"


hardware::gpioCon utilityPwrLed(GPIO_DT_SPEC_GET(DT_ALIAS(utility_pwr_led), gpios),GPIO_OUTPUT);
hardware::gpioCon cellularLed(GPIO_DT_SPEC_GET(DT_ALIAS(cellular_led), gpios),GPIO_OUTPUT);


int main(void)
{
	std::cout << "Hello, C++ world! " << CONFIG_BOARD << std::endl;

	utilityPwrLed.init();
cellularLed.init();

	while(1)
	{
		utilityPwrLed.toggle();
		cellularLed.toggle();

	k_msleep(1000);

	}
	return 0;
}

