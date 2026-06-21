#include "at_engine.hpp"

namespace cellular
{

    atEngine::atEngine(const device *uart) : uart_(uart, atEngine::rxCallback, this)
    {
    }

    int atEngine::init()
    {

        uart_.init();

        return 0;
    }

    atEngine::~atEngine()
    {
        rxBuffer.fill(0);
    }

    atEngine::atResult atEngine::sendCommand(std::string_view command, uint32_t timeout)
    {
    }

    atEngine::atResponse atEngine::sendCommand(std::string_view command, std::string_view expectedURC, std::span<uint8_t> expectedResponse,
                                               uint32_t timeout)
    {
    }

    void atEngine::rxCallback(const device *dev, void *userData)
    {
        auto *self = static_cast<atEngine *>(userData);

        if (self == nullptr)
        {
            return;
        }

        std::uint8_t buf[128];
        int len = uart_fifo_read(dev, buf, sizeof(buf));

        if (len > 0)
        {
            self->pushBuf(std::span<const std::uint8_t>{
                buf,
                static_cast<std::size_t>(len)});
        }
    }

    int atEngine::pushBuf(std::span<const uint8_t> buf)
    {
        for (uint8_t byte : buf)
        {
            const size_t next = (wr_index + 1) & (rxBuffer.size() - 1);
            if (next == rd_index)
            {
                return 1;
            }
            rxBuffer[wr_index] = byte;
            wr_index = next;
        }

        return 0;
    }

    int atEngine::popBuf(std::span<uint8_t> buf)
    {
        int i = 0;
        if (rd_index == wr_index)
        {
            return 1;
        }
        while (rd_index != wr_index && i < buf.size())
        {
            buf[i++] = rxBuffer[rd_index];
            const size_t next = (rd_index + 1) & (rxBuffer.size() - 1);
            rd_index = next;
        }

        return 0;
    }

}