#pragma once
#include <array>
#include <cstdint>
#include <span>
#include <string_view>
namespace ota_manifest {
struct version {
    uint32_t major{}, minor{}, patch{}, build{};
    bool operator==(const version &) const = default;
};
bool parse_version(std::string_view text, version &out);
int format_version(const version &value, std::span<char> output);
struct target {
    std::string_view name, board, soc;
    uint32_t address, max_size;
};
struct image {
    version release{};
    uint32_t size{};
    std::array<char, 128> file{};
    bool has_sha256{};
    std::array<uint8_t, 32> sha256{};
};
// Parser mutates JSON strings in place. Metadata owns all retained values.
int parse(std::span<char> json, const target &hardware, const version &requested, image &out);
bool header_matches(std::span<const uint8_t> header, const image &expected);
bool valid_base_url(std::string_view url);
}
