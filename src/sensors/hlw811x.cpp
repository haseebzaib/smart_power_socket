#include "sensors/hlw811x.hpp"

#include <errno.h>

#include <zephyr/kernel.h>

namespace sensors {

hlw811x::hlw811x(const device *uart, gpio_dt_spec muxA, gpio_dt_spec muxB,
	uint8_t deviceCount)
	: uart_(uart),
	  muxA_(muxA, GPIO_OUTPUT_INACTIVE),
	  muxB_(muxB, GPIO_OUTPUT_INACTIVE),
	  deviceCount_(deviceCount),
	  initialized_(false),
	  devices_{},
	  contexts_{}
{
}

hlw811x::~hlw811x()
{
	for (auto *device : devices_) {
		if (device != nullptr) {
			hlw811x_destroy(device);
		}
	}
}

int hlw811x::init()
{
	if (deviceCount_ == 0 || deviceCount_ > maxDevices) {
		return -EINVAL;
	}

	int ret = uart_.init();
	if (ret != 0) {
		return ret;
	}

	ret = muxA_.init();
	if (ret != 0) {
		return ret;
	}

	ret = muxB_.init();
	if (ret != 0) {
		return ret;
	}

	for (uint8_t i = 0; i < deviceCount_; ++i) {
		contexts_[i] = LowLevelContext{
			.owner = this,
			.deviceNumber = static_cast<uint8_t>(i + 1),
		};

		devices_[i] = hlw811x_create(HLW811X_UART, &contexts_[i]);
		if (devices_[i] == nullptr) {
			return -ENOMEM;
		}
	}

	initialized_ = true;
	return selectDevice(1);
}

bool hlw811x::validDevice(uint8_t deviceNumber) const
{
	return deviceNumber >= 1 && deviceNumber <= deviceCount_;
}

int hlw811x::selectDevice(uint8_t deviceNumber)
{
	if (!validDevice(deviceNumber)) {
		return -EINVAL;
	}

	const uint8_t muxIndex = deviceNumber - 1;

	int ret = muxA_.set(muxIndex & 0x01);
	if (ret != 0) {
		return ret;
	}

	ret = muxB_.set((muxIndex >> 1) & 0x01);
	if (ret != 0) {
		return ret;
	}

	k_busy_wait(muxSettleUs);
	return 0;
}

hlw811x_error_t hlw811x::readReg(uint8_t deviceNumber, hlw811x_reg_addr_t reg,
	uint8_t *buffer, size_t length)
{
	if (!initialized_ || !validDevice(deviceNumber) || buffer == nullptr) {
		return HLW811X_INVALID_PARAM;
	}

	const size_t index = deviceNumber - 1;
	return hlw811x_read_reg(devices_[index], reg, buffer, length);
}

hlw811x_error_t hlw811x::readSysStatus(uint8_t deviceNumber, uint16_t &status)
{
	uint8_t buffer[2] = {};
	hlw811x_error_t err = readReg(deviceNumber, HLW811X_REG_SYS_STATUS,
		buffer, sizeof(buffer));

	if (err == HLW811X_ERROR_NONE) {
		status = (static_cast<uint16_t>(buffer[0]) << 8) | buffer[1];
	}

	return err;
}

int hlw811x::llWrite(uint8_t deviceNumber, const uint8_t *data, size_t length)
{
	if (!validDevice(deviceNumber)) {
		return -EINVAL;
	}

	int ret = selectDevice(deviceNumber);
	if (ret != 0) {
		return ret;
	}

	uart_.flushRx();
	return uart_.write(std::span<const uint8_t>(data, length));
}

int hlw811x::llRead(uint8_t deviceNumber, uint8_t *buffer, size_t length)
{
	if (!validDevice(deviceNumber)) {
		return -EINVAL;
	}

	int ret = selectDevice(deviceNumber);
	if (ret != 0) {
		return ret;
	}

	return uart_.read(std::span<uint8_t>(buffer, length), defaultTimeoutMs);
}

} // namespace sensors

extern "C" int hlw811x_ll_write(const uint8_t *data, size_t datalen, void *ctx)
{
	if (ctx == nullptr) {
		return -EINVAL;
	}

	auto *context = static_cast<sensors::hlw811x::LowLevelContext *>(ctx);
	return context->owner->llWrite(context->deviceNumber, data, datalen);
}

extern "C" int hlw811x_ll_read(uint8_t *buffer, size_t bufferSize, void *ctx)
{
	if (ctx == nullptr) {
		return -EINVAL;
	}

	auto *context = static_cast<sensors::hlw811x::LowLevelContext *>(ctx);
	return context->owner->llRead(context->deviceNumber, buffer, bufferSize);
}
