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

        if (callBack_ != nullptr)
        {
            ret = uart_irq_callback_user_data_set(uart_, callBack_, userData_);
            if (ret != 0)
            {
                return ret;
            }
            uart_irq_rx_enable(uart_);
        }

        return ret;
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

    int uartCon::writeIntr()
    {
        if (uart_ == nullptr)
        {
            return -ENODEV;
        }

        return -ENOSYS;
    }

}
