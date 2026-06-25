#include "at_engine.hpp"

namespace cellular
{

    atEngine::atEngine(const device *uart) :  rd_index(0), wr_index(0),uart_(uart, atEngine::rxCallback, this)
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

    atEngine::atResult atEngine::sendCommand(std::string_view command, uint32_t timeoutMs)
    {
        uint32_t tick = 0;
        std::span<const uint8_t> bytes = std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(command.data()), command.size());

        uart_.writeIntr(bytes);

        while (tick < timeoutMs)
        {
            if (uart_.writeIntrFinished() == true)
            {
                break;
            }

            tick++;
            k_msleep(1);
        }

        k_msleep(100);

        printk("GSM RX: \"%s\" \r\n", rxBuffer.data());

        return parseOkayError();
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
            printk("GSM INTR RX: \"%s\" \r\n", buf);
            self->pushBuf(std::span<const std::uint8_t>{
                buf,
                static_cast<std::size_t>(len)});
        }
    }

    atEngine::atResult atEngine::parseOkayError()
    {
        atResult atResult_ = atResult::Unknown;
        uint32_t getOffset = 0;
        std::string_view okay = "OK\r\n";
        std::string_view error = "ERROR\r\n";

        if (contains(okay, &getOffset) == true)
        {
            printk("Contains Okay: %d \r\n",getOffset);
            popRange(getOffset, error.size());
            atResult_ = atResult::OK;
        }
        else
        {
            if (contains(error, &getOffset) == true)
            {
                printk("Contains Error: %d \r\n",getOffset);
                popRange(getOffset, error.size());
                atResult_ = atResult::Error;
            }
        }

        return atResult_;
    }

    int atEngine::available() const
    {
        const uint32_t wr = wr_index.load(std::memory_order_acquire);
        const uint32_t rd = rd_index.load(std::memory_order_acquire);
        return (wr - rd) & (rxBuffer.size() - 1);
    }

    bool atEngine::contains(std::string_view pattern, uint32_t *getOffset)
    {
        const uint32_t wr = wr_index.load(std::memory_order_acquire);
        const uint32_t rd = rd_index.load(std::memory_order_acquire);
        if (pattern.empty())
        {
            return true;
        }
        const uint32_t count = (wr - rd) & (rxBuffer.size() - 1);

        if (pattern.size() > count)
        {
            return false;
        }

        const uint32_t mask = rxBuffer.size() - 1;

        uint32_t rd_index_ = 0;
        uint32_t offset = 0;
        for (offset = 0; offset <= count - pattern.size(); ++offset)
        {

            bool matched = true;
            for (uint32_t i = 0; i < pattern.size(); ++i)
            {
                rd_index_ = (rd + offset + i) & mask;

                if (rxBuffer[rd_index_] != static_cast<uint8_t>(pattern[i]))
                {
                    matched = false;
                    break;
                }
            }

            if (matched)
            {
                return true;
            }
        }

        return false;
    }

    int atEngine::pushsingle(const uint8_t &value)
    {
        const uint32_t wr = wr_index.load(std::memory_order_relaxed);
        const uint32_t rd = rd_index.load(std::memory_order_acquire);

        const uint32_t next = (wr + 1) & (rxBuffer.size() - 1);

        if (next == rd)
        {
            return 1; // full
        }

        rxBuffer[wr] = value;
        wr_index.store(next, std::memory_order_release);
        return 0;
    }

    int atEngine::popsingle(uint8_t &value)
    {

        const uint32_t rd = rd_index.load(std::memory_order_relaxed);
        const uint32_t wr = wr_index.load(std::memory_order_acquire);

        if (rd == wr)
        {
            return 1; // empty
        }

        value = rxBuffer[rd];

        const uint32_t next = (rd + 1) & (rxBuffer.size() - 1);
        rd_index.store(next, std::memory_order_release);

        return 0;
    }

    int atEngine::pushBuf(std::span<const uint8_t> buf)
    {
        for (uint8_t byte : buf)
        {
            if (pushsingle(byte) != 0)
            {
                return 1;
            }
        }

        return 0;
    }

    int atEngine::popBuf(std::span<uint8_t> buf)
    {
        uint32_t popped = 0;

        while (popped < buf.size())
        {
            if (popsingle(buf[popped]) != 0)
            {
                break;
            }
            popped++;
        }

        return popped;
    }

    bool atEngine::popRange(uint32_t offset, uint32_t length)
    {
        unsigned int key = irq_lock();
        const uint32_t wr = wr_index.load(std::memory_order_acquire);
        const uint32_t rd = rd_index.load(std::memory_order_acquire);

        const uint32_t m = rxBuffer.size() - 1;
        const uint32_t count = (wr - rd) & m;

        if (offset > count)
        {
            return false;
        }

        if (length > (count - offset))
        {
            return false;
        }

        if (length == 0)
        {
            return true;
        }

        const uint32_t tailCount = count - offset - length;

        for (uint32_t i = 0; i < tailCount; ++i)
        {
            const uint32_t src = (rd + offset + length + i) & m;
            const uint32_t dst = (rd + offset + i) & m;

            rxBuffer[dst] = rxBuffer[src];
        }

        wr_index.store((wr - length) & m, std::memory_order_release);
        irq_unlock(key);

        return true;
    }

}