
#include "sim7080.hpp"


LOG_MODULE_REGISTER(sim7080, LOG_LEVEL_INF);

namespace cellular
{

    sim7080::sim7080(const device *uart, const gpio_dt_spec pwrKey, const gpio_flags_t pwrKeyFlags) : atEngine_(uart), pwrKey_(pwrKey, pwrKeyFlags)
    {
    }

    sim7080::~sim7080()
    {
    }

    int sim7080::init()
    {

        pwrKey_.init();
        atEngine_.init();

        if (atEngine_.send_command(atAT, 1000) != cellular::atEngine::atResult::OK)
        {
            LOG_INF("GSM not responding, PWR Key pulse");
            pwrKeyPulse();
        }

        atEngine_.send_command(atSetATCREG, 1000);
        atEngine_.send_command(atSetATCOPS, 1000);
        atEngine_.send_command(atSetATCGATT, 1000);
        atEngine_.send_command(atSetATCNMP, 1000);
        atEngine_.send_command(atSetATCMNB, 1000);

        return 0;
    }



    void sim7080::loop()
    {
        
    }

    void sim7080::pwrKeyPulse()
    {

        pwrKey_.set(1);
        k_msleep(4000);
        pwrKey_.set(0);
        k_msleep(2000);
    }

}