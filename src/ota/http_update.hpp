#pragma once
#include <string_view>
namespace cellular { class sim7080; }
namespace http_update {
// Returns zero after the acceptance SMS has been queued; negative errno otherwise.
int request(cellular::sim7080 &modem, std::string_view sender, std::string_view version);
void process(cellular::sim7080 &modem);
bool busy();
const char *phase_name();
const char *request_error(int code);
}
