/*
 * Copyright (c) 2023, Meta
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <iostream>
#include <cstring>
#include <zephyr/kernel.h>
#include "hardware/gpio_con.hpp"
#include "hardware/uart_con.hpp"
#include "sensors/hlw811x.hpp"

hardware::gpioCon utilityPwrLed(GPIO_DT_SPEC_GET(DT_ALIAS(utility_pwr_led), gpios), GPIO_OUTPUT_HIGH);
hardware::gpioCon cellularLed(GPIO_DT_SPEC_GET(DT_ALIAS(cellular_led), gpios), GPIO_OUTPUT_HIGH);
hardware::gpioCon relay4(GPIO_DT_SPEC_GET(DT_ALIAS(relay4), gpios), GPIO_OUTPUT_INACTIVE);
hardware::gpioCon gsmDtr(GPIO_DT_SPEC_GET(DT_ALIAS(gsm_dtr), gpios), GPIO_OUTPUT_INACTIVE);
hardware::gpioCon gsmPwrKey(GPIO_DT_SPEC_GET(DT_ALIAS(gsm_pwrkey), gpios), GPIO_OUTPUT_INACTIVE);
hardware::uartCon gsmUart(DEVICE_DT_GET(DT_ALIAS(gsm_uart)));
sensors::hlw811x energyMeters(
	DEVICE_DT_GET(DT_ALIAS(hlw_uart)),
	GPIO_DT_SPEC_GET(DT_ALIAS(energy_metering_mux_a), gpios),
	GPIO_DT_SPEC_GET(DT_ALIAS(energy_metering_mux_b), gpios),
	4);

static bool bufferContains(const uint8_t *buffer, int length, const char *needle)
{
	const int needleLength = static_cast<int>(std::strlen(needle));

	if (needleLength == 0 || length < needleLength) {
		return false;
	}

	for (int i = 0; i <= length - needleLength; ++i) {
		bool matched = true;

		for (int j = 0; j < needleLength; ++j) {
			if (buffer[i + j] != static_cast<uint8_t>(needle[j])) {
				matched = false;
				break;
			}
		}

		if (matched) {
			return true;
		}
	}

	return false;
}

static void printRx(const char *label, const uint8_t *buffer, int length)
{
	std::cout << label << " rx=" << length << " ";

	for (int i = 0; i < length; ++i) {
		const uint8_t c = buffer[i];

		if (c >= 0x20 && c <= 0x7e) {
			std::cout << static_cast<char>(c);
		} else if (c == '\r') {
			std::cout << "\\r";
		} else if (c == '\n') {
			std::cout << "\\n";
		} else {
			std::cout << "\\x" << std::hex << static_cast<int>(c) << std::dec;
		}
	}

	std::cout << std::endl;
}

static bool gsmCommand(const char *command, uint32_t timeoutMs, const char *expect = "OK")
{
	uint8_t rx[256] = {};

	gsmUart.flushRx();
	int tx = gsmUart.write(std::span<const uint8_t>(
		reinterpret_cast<const uint8_t *>(command), std::strlen(command)));
	k_msleep(50);
	int rxLength = gsmUart.read(std::span<uint8_t>(rx, sizeof(rx)), timeoutMs);

	std::cout << "GSM cmd tx=" << tx << " cmd=" << command;
	printRx("GSM", rx, rxLength);

	return bufferContains(rx, rxLength, expect);
}

static bool gsmTryAt()
{
	if (gsmCommand("AT\r\n", 8000)) {
		return true;
	}

	k_msleep(200);
	return gsmCommand("AT\r\n", 8000);
}

static bool gsmFindBaud(uint32_t &baudrate)
{
	const uint32_t baudrates[] = {115200, 9600, 57600, 38400, 19200};

	for (uint32_t baud : baudrates) {
		int ret = gsmUart.configureBaud(baud);
		std::cout << "GSM baud " << baud << " configure ret=" << ret << std::endl;

		if (ret == 0 && gsmTryAt()) {
			baudrate = baud;
			return true;
		}
	}

	return false;
}

static void gsmPowerPulse()
{
	gsmPwrKey.set(1);
	k_msleep(4000);
	gsmPwrKey.set(0);
	k_msleep(10000);
}

static void gsmBasicInit()
{
	uint32_t detectedBaud = 0;

	// if (!gsmFindBaud(detectedBaud)) {
	// 	std::cout << "GSM no AT response, pulsing PWRKEY" << std::endl;
	// 	gsmPowerPulse();
	// 	k_msleep(10000);

	// 	if (!gsmFindBaud(detectedBaud)) {
	// 		std::cout << "GSM still no AT response after PWRKEY" << std::endl;
	// 		return;
	// 	}
	// }

	gsmPowerPulse();

	std::cout << "GSM AT OK at baud " << detectedBaud << std::endl;
	gsmCommand("AT+IPR?\r\n", 5000);

	if (detectedBaud != 115200) {
		if (gsmCommand("AT+IPR=115200\r\n", 8000)) {
			k_msleep(200);
			gsmUart.configureBaud(115200);
			gsmTryAt();
		}
	}

	gsmCommand("ATE1\r\n", 8000);
}

int main(void)
{
	

	utilityPwrLed.init();
	cellularLed.init();
	relay4.init();
	gsmDtr.init();
	gsmPwrKey.init();
	int gsmUartInit = gsmUart.init();
	std::cout << "GSM UART init: " << gsmUartInit << std::endl;
	// gsmBasicInit();
	gsmPowerPulse();
	int ret = energyMeters.init();
	std::cout << "HLW811x init: " << ret << std::endl;

	const hlw811x_resistor_ratio baselineRatio = {
		.K1_A = 3.0f,
		.K1_B = 1.0f,
		.K2 = 1.0f,
	};

	k_msleep(5000);
	while (1) {
		// utilityPwrLed.toggle();
		// cellularLed.toggle();
		// relay4.toggle();

		for (uint8_t meter = 1; meter <= 4; ++meter) {
			int baselineRet = energyMeters.printBaseline(meter, baselineRatio);
			std::cout << "HLW811x meter " << static_cast<int>(meter)
				  << " baseline ret=" << baselineRet << std::endl;
			k_msleep(5000);
		}

		k_msleep(5000);
	}
	return 0;
}
