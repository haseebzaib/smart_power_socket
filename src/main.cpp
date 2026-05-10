/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include <stddef.h>
#include <stdint.h>

#define GSM_UART_NODE DT_ALIAS(gsm_uart)
#define HLW_UART_NODE DT_ALIAS(hlw_uart)

static const gpio_dt_spec gsm_pwrkey =
	GPIO_DT_SPEC_GET(DT_ALIAS(gsm_pwrkey), gpios);
static const gpio_dt_spec gsm_dtr =
	GPIO_DT_SPEC_GET(DT_ALIAS(gsm_dtr), gpios);
static const gpio_dt_spec relay4 =
	GPIO_DT_SPEC_GET(DT_ALIAS(relay4), gpios);
static const gpio_dt_spec hlw_mux_a =
	GPIO_DT_SPEC_GET(DT_ALIAS(energy_metering_mux_a), gpios);
static const gpio_dt_spec hlw_mux_b =
	GPIO_DT_SPEC_GET(DT_ALIAS(energy_metering_mux_b), gpios);

struct async_uart_test {
	const device *dev;
	const char *name;
	uint8_t rx_buf[2][64];
	uint8_t next_buf;
	bool ready;
	volatile bool tx_busy;
};

static async_uart_test gsm_uart = {
	.dev = DEVICE_DT_GET(GSM_UART_NODE),
	.name = "GSM",
};

static async_uart_test hlw_uart = {
	.dev = DEVICE_DT_GET(HLW_UART_NODE),
	.name = "HLW",
};

static void print_bytes(const char *name, const uint8_t *data, size_t len)
{
	printk("%s RX len=%u text=\"", name, static_cast<unsigned int>(len));

	for (size_t i = 0; i < len; ++i) {
		const uint8_t c = data[i];

		if (c >= 0x20 && c <= 0x7e) {
			printk("%c", c);
		} else if (c == '\r') {
			printk("\\r");
		} else if (c == '\n') {
			printk("\\n");
		} else {
			printk("\\x%02x", c);
		}
	}

	printk("\" hex=");
	for (size_t i = 0; i < len; ++i) {
		printk("%02x ", data[i]);
	}
	printk("\n");
}

static void uart_cb(const device *dev, uart_event *evt, void *user_data)
{
	async_uart_test *ctx = static_cast<async_uart_test *>(user_data);

	switch (evt->type) {
	case UART_TX_DONE:
		ctx->tx_busy = false;
		printk("%s TX done len=%u\n", ctx->name,
		       static_cast<unsigned int>(evt->data.tx.len));
		break;

	case UART_TX_ABORTED:
		ctx->tx_busy = false;
		printk("%s TX aborted\n", ctx->name);
		break;

	case UART_RX_RDY:
		print_bytes(ctx->name,
			    evt->data.rx.buf + evt->data.rx.offset,
			    evt->data.rx.len);
		break;

	case UART_RX_BUF_REQUEST: {
		uint8_t index = ctx->next_buf;
		ctx->next_buf = ctx->next_buf ? 0 : 1;
		int ret = uart_rx_buf_rsp(dev, ctx->rx_buf[index],
					  sizeof(ctx->rx_buf[index]));
		if (ret != 0) {
			printk("%s RX buf rsp failed %d\n", ctx->name, ret);
		}
		break;
	}

	case UART_RX_BUF_RELEASED:
		break;

	case UART_RX_DISABLED:
		printk("%s RX disabled\n", ctx->name);
		break;

	case UART_RX_STOPPED:
		printk("%s RX stopped reason=%d\n", ctx->name,
		       evt->data.rx_stop.reason);
		break;

	default:
		printk("%s UART event %d\n", ctx->name, evt->type);
		break;
	}
}

static int async_uart_init(async_uart_test *ctx)
{
	if (!device_is_ready(ctx->dev)) {
		printk("%s UART not ready\n", ctx->name);
		return -ENODEV;
	}

	int ret = uart_callback_set(ctx->dev, uart_cb, ctx);
	if (ret != 0) {
		printk("%s uart_callback_set failed %d\n", ctx->name, ret);
		return ret;
	}

	ctx->next_buf = 1;
	ctx->ready = false;
	ctx->tx_busy = false;

	ret = uart_rx_enable(ctx->dev, ctx->rx_buf[0], sizeof(ctx->rx_buf[0]),
			     10000);
	if (ret != 0) {
		printk("%s uart_rx_enable failed %d\n", ctx->name, ret);
		return ret;
	}

	ctx->ready = true;
	return 0;
}

static int async_uart_send(async_uart_test *ctx, const uint8_t *data, size_t len)
{
	if (!ctx->ready) {
		printk("%s TX skipped, UART not ready\n", ctx->name);
		return -ENODEV;
	}

	if (ctx->tx_busy) {
		printk("%s TX busy\n", ctx->name);
		return -EBUSY;
	}

	ctx->tx_busy = true;
	int ret = uart_tx(ctx->dev, data, len, SYS_FOREVER_US);
	if (ret != 0) {
		ctx->tx_busy = false;
		printk("%s uart_tx failed %d\n", ctx->name, ret);
	}

	return ret;
}

static int gpio_init_output(const gpio_dt_spec *gpio, gpio_flags_t flags)
{
	if (!gpio_is_ready_dt(gpio)) {
		return -ENODEV;
	}

	return gpio_pin_configure_dt(gpio, flags);
}

static void gsm_power_pulse()
{
	gpio_pin_set_dt(&gsm_pwrkey, 1);
	k_msleep(4000);
	gpio_pin_set_dt(&gsm_pwrkey, 0);
	k_msleep(10000);
}

static int hlw_select_meter(uint8_t meter)
{
	if (meter < 1 || meter > 4) {
		return -EINVAL;
	}

	const uint8_t index = meter - 1;
	int ret = gpio_pin_set_dt(&hlw_mux_a, index & 0x01);
	if (ret != 0) {
		return ret;
	}

	ret = gpio_pin_set_dt(&hlw_mux_b, (index >> 1) & 0x01);
	if (ret != 0) {
		return ret;
	}

	k_busy_wait(10);
	return 0;
}

int main(void)
{
	printk("Async UART test on %s\n", CONFIG_BOARD);

	int ret = gpio_init_output(&gsm_pwrkey, GPIO_OUTPUT_INACTIVE);
	printk("GSM PWRKEY init %d\n", ret);

	ret = gpio_init_output(&gsm_dtr, GPIO_OUTPUT_INACTIVE);
	printk("GSM DTR init %d\n", ret);

	ret = gpio_init_output(&relay4, GPIO_OUTPUT_INACTIVE);
	printk("Relay4 init %d\n", ret);

	ret = gpio_init_output(&hlw_mux_a, GPIO_OUTPUT_INACTIVE);
	printk("HLW mux A init %d\n", ret);

	ret = gpio_init_output(&hlw_mux_b, GPIO_OUTPUT_INACTIVE);
	printk("HLW mux B init %d\n", ret);

	gsm_power_pulse();

	ret = async_uart_init(&gsm_uart);
	printk("GSM async UART init %d\n", ret);

	ret = async_uart_init(&hlw_uart);
	printk("HLW async UART init %d\n", ret);

	const uint8_t gsm_at[] = {'A', 'T', '\r', '\n'};
	const uint8_t hlw_sys_status[] = {0xa5, 0x43};

	while (true) {
		gpio_pin_toggle_dt(&relay4);

		printk("Sending GSM AT\n");
		async_uart_send(&gsm_uart, gsm_at, sizeof(gsm_at));

		hlw_select_meter(4);
		printk("Sending HLW SYS_STATUS read to meter 4\n");
		async_uart_send(&hlw_uart, hlw_sys_status, sizeof(hlw_sys_status));

		k_sleep(K_SECONDS(3));
	}
}
