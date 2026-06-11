#include "uart_con.hpp"

namespace hardware
{

    uartCon::uartCon(const device *uart) : uart_(uart), callBack_(nullptr), userData_(nullptr)
    {
    }
    uartCon::uartCon(const device *uart, callBack _callBack, void *_userData)
        : uart_(uart), callBack_(_callBack), userData_(_userData)
    {
    }

    uartCon::~uartCon()
    {
    }

    int uartCon::init()
    {
        int ret = 0;
        if (uart_ == nullptr)
        {
            return -ENODEV;
        }

        if (!device_is_ready(uart_))
        {
            return -1;
        }

        ring_buf_init(&txRing_, sizeof(txBuf_), txBuf_);

        /* Always install our own trampoline so interrupt TX works. The trampoline
         * forwards RX events to the user callback and drains the TX ring buffer. */
        ret = uart_irq_callback_user_data_set(uart_, &uartCon::irqDispatch, this);
        if (ret != 0)
        {
            return ret;
        }

        if (callBack_ != nullptr)
        {
            uart_irq_rx_enable(uart_);
        }

        return ret;
    }

    void uartCon::irqDispatch(const struct device *_dev, void *_ctx)
    {
        static_cast<uartCon *>(_ctx)->handleIrq();
    }

    void uartCon::handleIrq()
    {
        while (uart_irq_update(uart_) && uart_irq_is_pending(uart_))
        {
            if (uart_irq_rx_ready(uart_) && callBack_ != nullptr)
            {
                /* User callback drains the RX FIFO (e.g. into its own ring buffer). */
                callBack_(uart_, userData_);
            }

            if (uart_irq_tx_ready(uart_))
            {
                handleTx();
            }
        }
    }

    void uartCon::handleTx()
    {
        uint8_t *data = nullptr;
        uint32_t claimed = ring_buf_get_claim(&txRing_, &data, sizeof(txBuf_));

        if (claimed == 0)
        {
            /* Nothing left to send; stop generating TX interrupts. */
            uart_irq_tx_disable(uart_);
            return;
        }

        int sent = uart_fifo_fill(uart_, data, static_cast<int>(claimed));
        ring_buf_get_finish(&txRing_, sent < 0 ? 0 : static_cast<uint32_t>(sent));
    }

    int uartCon::configureBaud(uint32_t baudrate)
    {
        if (uart_ == nullptr)
        {
            return -ENODEV;
        }

        struct uart_config config;
        int ret = uart_config_get(uart_, &config);
        if (ret != 0)
        {
            return ret;
        }

        config.baudrate = baudrate;
        return uart_configure(uart_, &config);
    }

    int uartCon::flushRx()
    {
        if (uart_ == nullptr)
        {
            return -ENODEV;
        }

        uint8_t dummy;
        int count = 0;

        while (uart_poll_in(uart_, &dummy) == 0)
        {
            ++count;
        }

        return count;
    }
    int uartCon::read(std::span<uint8_t> buffer, uint32_t timeOutMs)
    {
        if (uart_ == nullptr)
        {
            return -ENODEV;
        }
        if (buffer.empty())
        {
            return 0;
        }

        size_t count = 0;
        int64_t start = k_uptime_get();

        while (count < buffer.size())
        {
            int ret = uart_poll_in(uart_, &buffer[count]);

            if (ret == 0)
            {
                ++count;
                continue;
            }

            if ((k_uptime_get() - start) >= timeOutMs)
            {
                break;
            }
            k_sleep(K_MSEC(1));
        }

        return static_cast<int>(count);
    }
    int uartCon::readIntr()
    {
        if (uart_ == nullptr)
        {
            return -ENODEV;
        }

        return -ENOSYS;
    }
    int uartCon::write(std::span<const uint8_t> data)
    {
        if (uart_ == nullptr)
        {
            return -ENODEV;
        }
        if (data.empty())
        {
            return 0;
        }

        for (uint8_t byte : data)
        {
            uart_poll_out(uart_, byte);
        }

        return static_cast<int>(data.size());
    }

    int uartCon::writeIntr(std::span<const uint8_t> data)
    {
        if (uart_ == nullptr)
        {
            return -ENODEV;
        }
        if (data.empty())
        {
            return 0;
        }

        uint32_t queued = ring_buf_put(&txRing_, data.data(),
                                       static_cast<uint32_t>(data.size()));

        /* Kick off transmission; the ISR drains txRing_ until empty. */
        uart_irq_tx_enable(uart_);

        return static_cast<int>(queued);
    }

}
