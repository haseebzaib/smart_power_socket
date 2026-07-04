#include "at_engine.hpp"

LOG_MODULE_REGISTER(at_engine, LOG_LEVEL_INF);

namespace cellular
{

    atEngine::atEngine(const device *uart) : lineBufferLength(0), rd_index(0), wr_index(0), uart_(uart, atEngine::rxCallback, this)
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

    void atEngine::processRx()
    {
        uint8_t byte{};

        while (pop_single(byte) == 0)
        {
            LOG_INF("processRx: %c", static_cast<char>(byte));
            onRxByte(byte);
        }
    }

    void atEngine::onRxByte(uint8_t byte)
    {
        if (byte == '\r')
        {
            return;
        }

        if (byte == '\n')
        {
            if (lineBufferLength > 0)
            {
                handle_line(std::string_view{
                    reinterpret_cast<char *>(lineBuffer.data()),
                    lineBufferLength});

                lineBufferLength = 0;
            }
            return;
        }

        if (lineBufferLength < lineBuffer.size())
        {
            lineBuffer[lineBufferLength++] = byte;
        }
        else
        {
            lineBufferLength = 0;
        }
    }

    atEngine::atResult atEngine::send_command(std::string_view command, uint32_t timeoutMs)
    {
        currentCommand.active = true;
        currentCommand.done = false;
        currentCommand.responseSeen = false;
        currentCommand.expectedPrefix = nullptr;
        currentCommand.commandText = strip_line_ending(command);
        currentCommand.result = atResult::Timeout;

        LOG_INF("SendCmd: %s", command.data());

        uint32_t tick = 0;
        std::span<const uint8_t> bytes = std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(command.data()), command.size());

        uart_.writeIntr(bytes);

        while (tick < timeoutMs)
        {
            processRx(); // process the response

            if (currentCommand.done)
            {
                break;
            }

            tick++;
            k_msleep(1);
        }

        currentCommand.active = false;
        return currentCommand.result;
    }

    atEngine::atResponse atEngine::send_command(std::string_view command, std::string_view expectedURC, std::span<uint8_t> expectedResponse,
                                                uint32_t timeoutMs)
    {
        currentCommand.active = true;
        currentCommand.done = false;
        currentCommand.responseSeen = false;
        currentCommand.collectingResponse = false;
        currentCommand.expectedPrefix = expectedURC;
        currentCommand.commandText = strip_line_ending(command);
        currentCommand.result = atResult::Timeout;

        commandResponse.fill(0);
        commandResponseLength = 0;

        LOG_INF("SendCmd: %s", command.data());

        uint32_t tick = 0;
        std::span<const uint8_t> bytes = std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(command.data()), command.size());

        uart_.writeIntr(bytes);

        while (tick < timeoutMs)
        {
            processRx(); // process the response

            if (currentCommand.done)
            {
                break;
            }

            tick++;
            k_msleep(1);
        }

        atResponse response{};

        response.result = currentCommand.done ? currentCommand.result : atResult::Timeout;

        if (response.result == atResult::OK && commandResponseLength > 0)
        {
            const uint32_t copyLen = std::min(expectedResponse.size(), commandResponseLength);

            for (std::size_t i = 0; i < copyLen; ++i)
            {
                expectedResponse[i] = commandResponse[i];
            }

            response.responseLength = copyLen;
        }

        currentCommand.active = false;

        return response;
    }

    bool atEngine::save_response(std::string_view line)
    {
        constexpr std::string_view lineEnding = "\r\n";
        const std::size_t needed = line.size() + lineEnding.size();

        if ((commandResponseLength + needed) > commandResponse.size())
        {

            return false;
        }

        for (char ch : line)
        {
            commandResponse[commandResponseLength++] = static_cast<std::uint8_t>(ch);
        }

        for (char ch : lineEnding)
        {
            commandResponse[commandResponseLength++] = static_cast<std::uint8_t>(ch);
        }

        return true;
    }

    std::string_view atEngine::strip_line_ending(std::string_view text)
    {
        while (!text.empty() && (text.back() == '\r' || text.back() == '\n'))
        {
            text.remove_suffix(1);
        }
        return text;
    }

    void atEngine::handle_line(std::string_view line)
    {
        if (line.empty())
        {
            return; // nothing to process basically
        }

        if (currentCommand.active)
        {
            // Drop the echoed command line (modem echo still on, e.g. before ATE0
            // takes effect on a fresh power-up). Otherwise it would be saved as a
            // bogus response line for prefix-less getters like AT+CGSN / AT+CCID.
            if (!currentCommand.commandText.empty() &&
                line == currentCommand.commandText)
            {
                return;
            }

            if (line == "OK")
            {
                currentCommand.result = atResult::OK;
                currentCommand.done = true;
                currentCommand.active = false;
                return;
            }

            if (line == "ERROR" ||
                line.starts_with("+CME ERROR:") ||
                line.starts_with("+CMS ERROR:"))
            {
                currentCommand.result = atResult::Error;
                currentCommand.done = true;
                currentCommand.active = false;
                return;
            }

            if (!currentCommand.expectedPrefix.empty())
            {
                if (line.starts_with(currentCommand.expectedPrefix))
                {
                    save_response(line);

                    currentCommand.responseSeen = true;
                    currentCommand.collectingResponse = true;
                    return;
                }
                else if (currentCommand.collectingResponse /*&& !isURC()*/)
                {
                    save_response(line);
                    return;
                }
            }
            else
            {
                // save raw responses here not handling notifications urc currently
                save_response(line);
                currentCommand.responseSeen = true;
                return;
            }
        }
    }

    int atEngine::push_single(const uint8_t &value)
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

    int atEngine::pop_single(uint8_t &value)
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

    int atEngine::push_buf(std::span<const uint8_t> buf)
    {
        for (uint8_t byte : buf)
        {
            if (push_single(byte) != 0)
            {
                return 1;
            }
        }

        return 0;
    }

    int atEngine::pop_buf(std::span<uint8_t> buf)
    {
        uint32_t popped = 0;

        while (popped < buf.size())
        {
            if (pop_single(buf[popped]) != 0)
            {
                break;
            }
            popped++;
        }

        return popped;
    }

    bool atEngine::pop_range(uint32_t offset, uint32_t length)
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
            self->push_buf(std::span<const std::uint8_t>{
                buf,
                static_cast<std::size_t>(len)});
        }
    }
}