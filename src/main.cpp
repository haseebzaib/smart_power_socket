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
#include "hardware/gpio_con.hpp"
#include "hardware/uart_con.hpp"
#include "sensors/hlw811x.hpp"

hardware::gpioCon utilityPwrLed(GPIO_DT_SPEC_GET(DT_ALIAS(utility_pwr_led), gpios), GPIO_OUTPUT_HIGH);
hardware::gpioCon cellularLed(GPIO_DT_SPEC_GET(DT_ALIAS(cellular_led), gpios), GPIO_OUTPUT_HIGH);
hardware::gpioCon relay1(GPIO_DT_SPEC_GET(DT_ALIAS(relay1), gpios), GPIO_OUTPUT_INACTIVE);
hardware::gpioCon relay2(GPIO_DT_SPEC_GET(DT_ALIAS(relay2), gpios), GPIO_OUTPUT_INACTIVE);
hardware::gpioCon relay3(GPIO_DT_SPEC_GET(DT_ALIAS(relay3), gpios), GPIO_OUTPUT_INACTIVE);
hardware::gpioCon relay4(GPIO_DT_SPEC_GET(DT_ALIAS(relay4), gpios), GPIO_OUTPUT_INACTIVE);
hardware::gpioCon gsmDtr(GPIO_DT_SPEC_GET(DT_ALIAS(gsm_dtr), gpios), GPIO_OUTPUT_INACTIVE);
hardware::gpioCon gsmPwrKey(GPIO_DT_SPEC_GET(DT_ALIAS(gsm_pwrkey), gpios), GPIO_OUTPUT_INACTIVE);
hardware::uartCon gsmUart(DEVICE_DT_GET(DT_ALIAS(gsm_uart)));
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

std::array<int,4> energyMeterErr;

std::array<sensors::hlw811x::measurements,4> acMeasurements;

static void printGsmResponse(std::span<const uint8_t> data)
{
	printk("GSM RX len=%u text=\"", static_cast<unsigned int>(data.size()));
	for (uint8_t byte : data)
	{
		if (byte >= 0x20 && byte <= 0x7e)
		{
			printk("%c", byte);
		}
		else if (byte == '\r')
		{
			printk("\\r");
		}
		else if (byte == '\n')
		{
			printk("\\n");
		}
		else
		{
			printk("\\x%02x", byte);
		}
	}

	printk("\" hex=");
	for (uint8_t byte : data)
	{
		printk("%02x ", byte);
	}
	printk("\n");
}

static int gsmAtExchange(const char *command, uint32_t totalTimeoutMs)
{
	std::array<uint8_t, 160> response{};
	size_t responseLen = 0;
	int64_t deadline = k_uptime_get() + totalTimeoutMs;

	gsmUart.flushRx();

	int ret = gsmUart.write(std::span<const uint8_t>(
		reinterpret_cast<const uint8_t *>(command), std::strlen(command)));
	printk("GSM TX \"%s\" ret=%d\n", command, ret);
	if (ret < 0)
	{
		return ret;
	}

	while (k_uptime_get() < deadline && responseLen < response.size())
	{
		uint8_t byte = 0;
		ret = gsmUart.read(std::span<uint8_t>(&byte, 1), 50);
		if (ret == 1)
		{
			response[responseLen++] = byte;

			if (responseLen >= 4 &&
			    std::memcmp(response.data() + responseLen - 4, "OK\r\n", 4) == 0)
			{
				break;
			}
			if (responseLen >= 7 &&
			    std::memcmp(response.data() + responseLen - 7, "ERROR\r\n", 7) == 0)
			{
				break;
			}
		}
	}

	printGsmResponse(std::span<const uint8_t>(response.data(), responseLen));
	return responseLen > 0 ? static_cast<int>(responseLen) : -ETIMEDOUT;
}

static void gsmProbe()
{
	gsmDtr.set(0);
	int ret = gsmUart.configureBaud(115200);
	printk("GSM UART force 115200 ret=%d\n", ret);

	k_msleep(200);
	ret = gsmAtExchange("AT\r\n", 2000);
	printk("GSM AT probe ret=%d\n", ret);
}


static void gsmPowerPulse()
{
	gsmPwrKey.set(1);
	k_msleep(4000);
	gsmPwrKey.set(0);
	k_msleep(10000);
}

int main(void)
{

	utilityPwrLed.init();
	cellularLed.init();
	relay1.init();
	relay2.init();
	relay3.init();
	relay4.init();
	gsmDtr.init();
	gsmPwrKey.init();
	int gsmUartInit = gsmUart.init();
	std::cout << "GSM UART init: " << gsmUartInit << std::endl;
	gsmProbe();

	int ret = energyMeters.init();
	std::cout << "HLW811x init: " << ret << std::endl;

	for(uint8_t meter = 1;meter <=4; ++meter)
	{
		energyMeterErr[meter - 1] = energyMeters.configureIndividual(meter,baselineRatio,baselinePga);
	}

	relay1.set(1);
	relay2.set(1);
	relay3.set(1);
	relay4.set(1);


	k_msleep(5000);
	while (1)
	{



		 for (uint8_t meter = 1; meter <= 4; ++meter)
		{
			if(energyMeterErr[meter - 1] != 0)
			{
				energyMeters.configureIndividual(meter,baselineRatio,baselinePga);
			}
			else
			{
               int ret = energyMeters.measurement(meter,acMeasurements[meter - 1]);

			   	std::cout << "HLW811x meter " << static_cast<int>(meter)
					      << " ret=" << ret 
					      << " mV "   << acMeasurements[meter - 1].mV 
						  << " mA "   << acMeasurements[meter - 1].mA
						  << " mW "   << acMeasurements[meter - 1].mW 
						  << " apparentmW "   << acMeasurements[meter - 1].apparentmW
						  << " wH "   << acMeasurements[meter - 1].wH 
						  << " hZ "   << acMeasurements[meter - 1].hZ
						  << " pF "   << acMeasurements[meter - 1].pF 
						  << std::endl;
			}

			k_msleep(300);
		}

		gsmAtExchange("AT\r\n", 1000);
		k_msleep(5000);
	}
	return 0;
}
