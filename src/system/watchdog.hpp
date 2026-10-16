#pragma once
#include <cstdint>

namespace app_watchdog {
inline constexpr uint32_t normalTimeoutMs = 30000;
inline constexpr uint32_t flashTimeoutMs = 180000;
inline constexpr uint32_t commandMarginMs = 5000;

// One heartbeat at the main-loop boundary. Zephyr owns hardware feeding.
void progress();

// A bounded allowance for an operation which can legitimately exceed 30 s.
// Destruction restores the enclosing deadline (or the normal 30 s period).
// The operation name must remain valid until scope exit.
// This monitors the main application thread; other workers need their own
// task_wdt channel. Their activity must never refresh the main channel.
class operation {
public:
#if defined(CONFIG_TASK_WDT)
    explicit operation(uint64_t timeoutMs, const char *name);
    ~operation();
private:
    friend void progress();
    operation *previous_ = nullptr;
    int64_t deadline_ = 0;
    const char *name_ = nullptr;
    bool owned_ = false;
#else
    explicit operation(uint64_t, const char *) {}
#endif
public:
    operation(const operation &) = delete;
    operation &operator=(const operation &) = delete;
};
#if !defined(CONFIG_TASK_WDT)
inline void progress() {}
#endif
}
