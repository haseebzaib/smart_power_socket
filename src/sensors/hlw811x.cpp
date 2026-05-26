#include "sensors/hlw811x.hpp"

#include <errno.h>

#include <zephyr/kernel.h>

namespace sensors
{

#define RETURN_IF_ERROR(expr)    \
	do                           \
	{                            \
		const int ret_ = (expr); \
		if (ret_ != 0)           \
		{                        \
			return ret_;         \
		}                        \
	} while (false)

	static int countError(hlw811x_error_t err)
	{
		return err == HLW811X_ERROR_NONE ? 0 : 1;
	}

	static void printErr(const char *label, hlw811x_error_t err)
	{
		printk("  %s err=%d\n", label, err);
	}

	static void printU16(const char *label, hlw811x_error_t err, uint16_t value)
	{
		printk("  %s err=%d value=0x%04x\n", label, err, value);
	}

	static void printI32(const char *label, hlw811x_error_t err, int32_t value,
						 const char *unit)
	{
		printk("  %s err=%d value=%d %s\n", label, err, value, unit);
	}

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
		for (auto *device : devices_)
		{
			if (device != nullptr)
			{
				hlw811x_destroy(device);
			}
		}
	}

	int hlw811x::init()
	{
		if (deviceCount_ == 0 || deviceCount_ > maxDevices)
		{
			return -EINVAL;
		}

		int ret = uart_.init();
		if (ret != 0)
		{
			return ret;
		}

		ret = muxA_.init();
		if (ret != 0)
		{
			return ret;
		}

		ret = muxB_.init();
		if (ret != 0)
		{
			return ret;
		}

		for (uint8_t i = 0; i < deviceCount_; ++i)
		{
			contexts_[i] = LowLevelContext{
				.owner = this,
				.deviceNumber = static_cast<uint8_t>(i + 1),
			};

			devices_[i] = hlw811x_create(HLW811X_UART, &contexts_[i]);
			if (devices_[i] == nullptr)
			{
				return -ENOMEM;
			}
		}

		initialized_ = true;
		return selectDevice(1);
	}

	int hlw811x::configureIndividual(uint8_t deviceNumber, const hlw811x_resistor_ratio &ratio, const hlw811x_pga &baselinePga)
	{
		const size_t index = deviceNumber - 1;
		struct ::hlw811x *device = devices_[index];
		if (device == nullptr)
		{
			return -ENODEV;
		}
		RETURN_IF_ERROR(selectDevice(deviceNumber));
		hlw811x_set_resistor_ratio(device, &ratio);

		hlw811x_coeff coeff = {};
		RETURN_IF_ERROR(static_cast<int>(hlw811x_read_coeff(device, &coeff)));

		RETURN_IF_ERROR(static_cast<int>(hlw811x_set_pga(device, &baselinePga)));
		RETURN_IF_ERROR(static_cast<int>(hlw811x_set_channel_b_mode(device, HLW811X_B_MODE_TEMPERATURE)));
		RETURN_IF_ERROR(static_cast<int>(hlw811x_set_active_power_calc_mode(device,
																			HLW811X_ACTIVE_POWER_MODE_POS_NEG_ALGEBRAIC)));
		RETURN_IF_ERROR(static_cast<int>(hlw811x_set_rms_calc_mode(device, HLW811X_RMS_MODE_AC)));
		RETURN_IF_ERROR(static_cast<int>(hlw811x_set_data_update_frequency(device, HLW811X_DATA_UPDATE_FREQ_HZ_3_4)));

		RETURN_IF_ERROR(static_cast<int>(hlw811x_select_channel(device, HLW811X_CHANNEL_A)));
		RETURN_IF_ERROR(static_cast<int>(hlw811x_enable_channel(device, HLW811X_CHANNEL_A | HLW811X_CHANNEL_U)));
		RETURN_IF_ERROR(static_cast<int>(hlw811x_disable_channel(device, HLW811X_CHANNEL_B)));

		RETURN_IF_ERROR(static_cast<int>(hlw811x_enable_power_factor(device)));
		RETURN_IF_ERROR(static_cast<int>(hlw811x_enable_waveform(device)));
		RETURN_IF_ERROR(static_cast<int>(hlw811x_enable_zerocrossing(device)));
		RETURN_IF_ERROR(static_cast<int>(hlw811x_enable_pulse(device, HLW811X_CHANNEL_A)));

		k_msleep(1000);

		return 0;
	}

	bool hlw811x::validDevice(uint8_t deviceNumber) const
	{
		return deviceNumber >= 1 && deviceNumber <= deviceCount_;
	}

	int hlw811x::selectDevice(uint8_t deviceNumber)
	{
		if (!validDevice(deviceNumber))
		{
			return -EINVAL;
		}

		const uint8_t muxIndex = deviceNumber - 1;

		int ret = muxA_.set(muxIndex & 0x01);
		if (ret != 0)
		{
			return ret;
		}

		ret = muxB_.set((muxIndex >> 1) & 0x01);
		if (ret != 0)
		{
			return ret;
		}

		k_busy_wait(muxSettleUs);
		return 0;
	}

	hlw811x_error_t hlw811x::readReg(uint8_t deviceNumber, hlw811x_reg_addr_t reg,
									 uint8_t *buffer, size_t length)
	{
		if (!initialized_ || !validDevice(deviceNumber) || buffer == nullptr)
		{
			return HLW811X_INVALID_PARAM;
		}

		const size_t index = deviceNumber - 1;
		return hlw811x_read_reg(devices_[index], reg, buffer, length);
	}

	hlw811x_error_t hlw811x::readSysStatus(uint8_t deviceNumber, uint16_t &status)
	{
		uint8_t buffer = 0;
		hlw811x_error_t err = readReg(deviceNumber, HLW811X_REG_SYS_STATUS,
									  &buffer, sizeof(buffer));

		if (err == HLW811X_ERROR_NONE)
		{
			status = buffer;
		}

		return err;
	}

	int hlw811x::printBaseline(uint8_t deviceNumber,
							   const hlw811x_resistor_ratio &ratio)
	{
		if (!initialized_ || !validDevice(deviceNumber))
		{
			return -EINVAL;
		}

		const size_t index = deviceNumber - 1;
		struct ::hlw811x *device = devices_[index];
		if (device == nullptr)
		{
			return -ENODEV;
		}

		int failures = 0;
		uint16_t sysStatus = 0;
		hlw811x_error_t err = readSysStatus(deviceNumber, sysStatus);

		printk("HLW811x meter %u baseline\n", deviceNumber);
		printU16("sys_status", err, sysStatus);
		failures += countError(err);

		hlw811x_set_resistor_ratio(device, &ratio);

		hlw811x_coeff coeff = {};
		err = hlw811x_read_coeff(device, &coeff);
		printErr("read_coeff", err);
		if (err == HLW811X_ERROR_NONE)
		{
			printk("  coeff hfconst=0x%04x rmsA=0x%04x rmsB=0x%04x rmsU=0x%04x\n",
				   coeff.hfconst, coeff.rms.A, coeff.rms.B, coeff.rms.U);
			printk("  coeff pA=0x%04x pB=0x%04x pS=0x%04x eA=0x%04x eB=0x%04x\n",
				   coeff.power.A, coeff.power.B, coeff.power.S,
				   coeff.energy.A, coeff.energy.B);
		}
		failures += countError(err);

		hlw811x_pga pga = {};
		const hlw811x_pga baselinePga = {
			.A = HLW811X_PGA_GAIN_16,
			.B = HLW811X_PGA_GAIN_1,
			.U = HLW811X_PGA_GAIN_1,
		};

		err = hlw811x_set_pga(device, &baselinePga);
		printErr("set_pga", err);
		failures += countError(err);

		err = hlw811x_get_pga(device, &pga);
		printk("  %s err=%d A=%d B=%d U=%d\n", "pga", err, pga.A, pga.B,
			   pga.U);
		failures += countError(err);

		err = hlw811x_set_channel_b_mode(device, HLW811X_B_MODE_TEMPERATURE);
		printErr("set_b_temp_mode", err);
		failures += countError(err);

		err = hlw811x_set_active_power_calc_mode(device,
												 HLW811X_ACTIVE_POWER_MODE_POS_NEG_ALGEBRAIC);
		printErr("set_power_mode", err);
		failures += countError(err);

		hlw811x_active_power_mode_t activePowerMode =
			HLW811X_ACTIVE_POWER_MODE_POS_NEG_ALGEBRAIC;
		err = hlw811x_get_active_power_calc_mode(device, &activePowerMode);
		printk("  %s err=%d mode=%d\n", "active_power_mode", err,
			   activePowerMode);
		failures += countError(err);

		err = hlw811x_set_rms_calc_mode(device, HLW811X_RMS_MODE_AC);
		printErr("set_rms_mode", err);
		failures += countError(err);

		hlw811x_rms_mode_t rmsMode = HLW811X_RMS_MODE_AC;
		err = hlw811x_get_rms_calc_mode(device, &rmsMode);
		printk("  %s err=%d mode=%d\n", "rms_mode", err, rmsMode);
		failures += countError(err);

		err = hlw811x_set_data_update_frequency(device,
												HLW811X_DATA_UPDATE_FREQ_HZ_3_4);
		printErr("set_update_freq", err);
		failures += countError(err);

		hlw811x_data_update_freq_t updateFreq = HLW811X_DATA_UPDATE_FREQ_HZ_3_4;
		err = hlw811x_get_data_update_frequency(device, &updateFreq);
		printk("  %s err=%d mode=%d\n", "update_freq", err, updateFreq);
		failures += countError(err);

		hlw811x_channel_b_mode_t bMode = HLW811X_B_MODE_TEMPERATURE;
		err = hlw811x_get_channel_b_mode(device, &bMode);
		printk("  %s err=%d mode=%d\n", "channel_b_mode", err, bMode);
		failures += countError(err);

		hlw811x_channel_t currentChannel = HLW811X_CHANNEL_A;
		err = hlw811x_read_current_channel(device, &currentChannel);
		printk("  %s err=%d channel=0x%02x\n", "current_channel", err,
			   currentChannel);
		failures += countError(err);

		hlw811x_intr_t interrupts = static_cast<hlw811x_intr_t>(0);
		err = hlw811x_get_interrupt(device, &interrupts);
		printk("  %s err=%d mask=0x%04x\n", "interrupts", err,
			   interrupts);
		failures += countError(err);

		err = hlw811x_select_channel(device, HLW811X_CHANNEL_A);
		printErr("select_channel_a", err);
		failures += countError(err);

		err = hlw811x_enable_channel(device, HLW811X_CHANNEL_A | HLW811X_CHANNEL_U);
		printErr("enable_channels", err);
		failures += countError(err);

		err = hlw811x_disable_channel(device, HLW811X_CHANNEL_B);
		printErr("disable_channel_b", err);
		failures += countError(err);

		err = hlw811x_enable_power_factor(device);
		printErr("enable_pf", err);
		failures += countError(err);

		err = hlw811x_enable_waveform(device);
		printErr("enable_waveform", err);
		failures += countError(err);

		err = hlw811x_enable_zerocrossing(device);
		printErr("enable_zc", err);
		failures += countError(err);

		err = hlw811x_enable_pulse(device, HLW811X_CHANNEL_A);
		printErr("enable_pulse_a", err);
		failures += countError(err);

		int32_t value = 0;
		err = hlw811x_get_rms(device, HLW811X_CHANNEL_U, &value);
		printI32("rms_voltage", err, value, "mV");
		failures += countError(err);

		err = hlw811x_get_rms(device, HLW811X_CHANNEL_A, &value);
		printI32("rms_current_a", err, value, "mA");
		failures += countError(err);

		err = hlw811x_get_power(device, HLW811X_CHANNEL_A, &value);
		printI32("active_power_a", err, value, "mW");
		failures += countError(err);

		err = hlw811x_get_power(device, HLW811X_CHANNEL_U, &value);
		printI32("apparent_power", err, value, "mW");
		failures += countError(err);

		err = hlw811x_get_energy(device, HLW811X_CHANNEL_A, &value);
		printI32("energy_a", err, value, "Wh");
		failures += countError(err);

		err = hlw811x_get_frequency(device, &value);
		printI32("frequency", err, value, "centiHz");
		failures += countError(err);

		err = hlw811x_get_power_factor(device, &value);
		printI32("power_factor", err, value, "centi");
		failures += countError(err);

		err = hlw811x_get_phase_angle(device, &value, HLW811X_LINE_FREQ_50HZ);
		printI32("phase_angle", err, value, "centideg");
		failures += countError(err);

		printk("HLW811x meter %u baseline failures=%d\n", deviceNumber,
			   failures);

		return failures == 0 ? 0 : -EIO;
	}

	int hlw811x::measurement(uint8_t deviceNumber, struct measurements &measOut)
	{
		const size_t index = deviceNumber - 1;
		struct ::hlw811x *device = devices_[index];
		if (device == nullptr)
		{
			return -ENODEV;
		}
		RETURN_IF_ERROR(selectDevice(deviceNumber));

		RETURN_IF_ERROR(static_cast<int>(hlw811x_get_rms(device, HLW811X_CHANNEL_U, &measOut.mV)));
		RETURN_IF_ERROR(static_cast<int>(hlw811x_get_rms(device, HLW811X_CHANNEL_A, &measOut.mA)));
		RETURN_IF_ERROR(static_cast<int>(hlw811x_get_power(device, HLW811X_CHANNEL_A, &measOut.mW)));
		RETURN_IF_ERROR(static_cast<int>(hlw811x_get_power(device, HLW811X_CHANNEL_U, &measOut.apparentmW)));
		RETURN_IF_ERROR(static_cast<int>(hlw811x_get_energy(device, HLW811X_CHANNEL_A, &measOut.wH)));
		RETURN_IF_ERROR(static_cast<int>(hlw811x_get_power_factor(device, &measOut.pF)));
		RETURN_IF_ERROR(static_cast<int>(hlw811x_get_frequency(device, &measOut.hZ)));


		return 0;
	}

	int hlw811x::llWrite(uint8_t deviceNumber, const uint8_t *data, size_t length)
	{
		if (!validDevice(deviceNumber))
		{
			return -EINVAL;
		}

		int ret = selectDevice(deviceNumber);
		if (ret != 0)
		{
			return ret;
		}

		uart_.flushRx();
		ret = uart_.write(std::span<const uint8_t>(data, length));

		// printk("HLW811x meter %u tx=%d:", deviceNumber, ret);
		// for (size_t i = 0; i < length; ++i)
		// {
		// 	printk(" %02x", data[i]);
		// }
		// printk("\n");

		return ret;
	}

	int hlw811x::llRead(uint8_t deviceNumber, uint8_t *buffer, size_t length)
	{
		if (!validDevice(deviceNumber))
		{
			return -EINVAL;
		}

		int ret = selectDevice(deviceNumber);
		if (ret != 0)
		{
			return ret;
		}

		ret = uart_.read(std::span<uint8_t>(buffer, length), defaultTimeoutMs);

		// printk("HLW811x meter %u rx=%d:", deviceNumber, ret);
		// if (ret > 0)
		// {
		// 	for (int i = 0; i < ret; ++i)
		// 	{
		// 		printk(" %02x", buffer[i]);
		// 	}
		// }
		// printk("\n");

		return ret;
	}

} // namespace sensors

extern "C" int hlw811x_ll_write(const uint8_t *data, size_t datalen, void *ctx)
{
	if (ctx == nullptr)
	{
		return -EINVAL;
	}

	auto *context = static_cast<sensors::hlw811x::LowLevelContext *>(ctx);
	return context->owner->llWrite(context->deviceNumber, data, datalen);
}

extern "C" int hlw811x_ll_read(uint8_t *buffer, size_t bufferSize, void *ctx)
{
	if (ctx == nullptr)
	{
		return -EINVAL;
	}

	auto *context = static_cast<sensors::hlw811x::LowLevelContext *>(ctx);
	return context->owner->llRead(context->deviceNumber, buffer, bufferSize);
}
