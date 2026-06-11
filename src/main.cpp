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

/* Ring buffer for bytes received from the GSM module's UART, filled from the RX ISR. */
RING_BUF_DECLARE(gsmRxRing, 256);

/* Called from uartCon's ISR trampoline when RX data is ready. The trampoline
 * already handles uart_irq_update()/pending looping, so we just drain the FIFO. */
static void gsmUartIsr(const device *dev, void *userData)
{
	uint8_t buf[64];
	int len = uart_fifo_read(dev, buf, sizeof(buf));

	if (len > 0)
	{
		ring_buf_put(&gsmRxRing, buf, static_cast<uint32_t>(len));
	}
}

hardware::uartCon gsmUart(DEVICE_DT_GET(DT_ALIAS(gsm_uart)), gsmUartIsr, nullptr);
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

static int gsmSendAt(const char *command)
{
	int ret = gsmUart.writeIntr(std::span<const uint8_t>(
		reinterpret_cast<const uint8_t *>(command), std::strlen(command)));
	printk("GSM TX \"%s\" ret=%d\n", command, ret);
	return ret;
}

/* Drains whatever the RX ISR has stashed in gsmRxRing and prints it. Call this
 * regularly from the main loop so nothing is missed while we are busy elsewhere. */
static void gsmDrainRx()
{
	uint8_t buf[64];
	uint32_t len = ring_buf_get(&gsmRxRing, buf, sizeof(buf));

	if (len > 0)
	{
		printGsmResponse(std::span<const uint8_t>(buf, len));
	}
}


static void gsmPowerPulse()
{
	gsmPwrKey.set(1);
	k_msleep(4000);
	gsmPwrKey.set(0);
	k_msleep(2000);
}

/* Enable command echo, then hammer AT a few times. If board-TX actually reaches
 * the module's RX, ATE1 makes the module echo our bytes straight back, so we
 * should see them in the RX drain even before any "OK" reply arrives. */
static void gsmStartupProbe()
{
	gsmSendAt("ATE1\r\n");
	k_msleep(300);
	gsmDrainRx();

	for (int i = 0; i < 5; ++i)
	{
		gsmSendAt("AT\r\n");
		k_msleep(300);
		gsmDrainRx();
	}
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

		gsmDtr.set(0);
	gsmPowerPulse();

	gsmStartupProbe();

	int64_t nextGsmTx = k_uptime_get();

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

			gsmDrainRx();
			k_msleep(300);
		}

		if (k_uptime_get() >= nextGsmTx)
		{
			gsmSendAt("AT\r\n");
			nextGsmTx = k_uptime_get() + 3000;
		}

		gsmDrainRx();
		k_msleep(200);
	}
	return 0;
}
