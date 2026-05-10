#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>

#include "hardware/gpio_con.hpp"
#include "hardware/uart_con.hpp"

extern "C" {
#include "externalLibs/hlw811x/hlw811x.h"
}

namespace sensors {

class hlw811x {
public:
	static constexpr uint8_t maxDevices = 4;

	struct LowLevelContext {
		hlw811x *owner;
		uint8_t deviceNumber;
	};

	hlw811x(const device *uart, gpio_dt_spec muxA, gpio_dt_spec muxB,
		uint8_t deviceCount);
	~hlw811x();

	hlw811x(const hlw811x &) = delete;
	hlw811x &operator=(const hlw811x &) = delete;

	int init();
	int selectDevice(uint8_t deviceNumber);
	hlw811x_error_t readReg(uint8_t deviceNumber, hlw811x_reg_addr_t reg,
		uint8_t *buffer, size_t length);
	hlw811x_error_t readSysStatus(uint8_t deviceNumber, uint16_t &status);

	int llWrite(uint8_t deviceNumber, const uint8_t *data, size_t length);
	int llRead(uint8_t deviceNumber, uint8_t *buffer, size_t length);

private:
	static constexpr uint32_t muxSettleUs = 1000;
	static constexpr uint32_t defaultTimeoutMs = 2000;

	bool validDevice(uint8_t deviceNumber) const;

	hardware::uartCon uart_;
	hardware::gpioCon muxA_;
	hardware::gpioCon muxB_;
	uint8_t deviceCount_;
	bool initialized_;
	std::array<struct ::hlw811x *, maxDevices> devices_;
	std::array<LowLevelContext, maxDevices> contexts_;
};

} // namespace sensors
