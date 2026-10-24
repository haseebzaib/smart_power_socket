#include "manifest.hpp"
#include <algorithm>
#include <charconv>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <zephyr/data/json.h>
namespace ota_manifest {
bool parse_version(std::string_view text, version &out)
{
    version value;
    uint32_t *fields[] = {&value.major, &value.minor, &value.patch, &value.build};
    for (unsigned i = 0; i < 3; ++i) {
        auto end = text.find(i == 2 ? '+' : '.');
        auto part = text.substr(0, end);
        if (part.empty()) return false;
        auto r = std::from_chars(part.data(), part.data() + part.size(), *fields[i]);
        if (r.ec != std::errc{} || r.ptr != part.data() + part.size()) return false;
        if (i < 2 && end == text.npos) return false;
        if (end == text.npos) { text = {}; break; }
        text.remove_prefix(end + 1);
        if (i == 2) {
            if (text.empty()) return false;
            auto b = std::from_chars(text.data(), text.data() + text.size(), value.build);
            if (b.ec != std::errc{} || b.ptr != text.data() + text.size()) return false;
            text = {};
        }
    }
    if (!text.empty() || value.major > 255 || value.minor > 255 || value.patch > 65535) return false;
    out = value;
    return true;
}
int format_version(const version &v, std::span<char> output)
{
    int n = std::snprintf(output.data(), output.size(), "%u.%u.%u+%u",
        static_cast<unsigned>(v.major), static_cast<unsigned>(v.minor),
        static_cast<unsigned>(v.patch), static_cast<unsigned>(v.build));
    return n < 0 || static_cast<size_t>(n) >= output.size() ? -ENOSPC : n;
}
namespace {
struct entry {
    char *type{}, *board{}, *soc{}, *filename{}, *version_text{}, *index{}, *primary{}, *secondary{}, *sha{};
    int32_t address{}, size{};
};
struct package { int32_t format{}; char *name{}; entry files[2]{}; size_t count{}; };
const json_obj_descr entry_descr[] = {
    JSON_OBJ_DESCR_PRIM(entry, type, JSON_TOK_STRING),
    JSON_OBJ_DESCR_PRIM(entry, board, JSON_TOK_STRING),
    JSON_OBJ_DESCR_PRIM(entry, soc, JSON_TOK_STRING),
    JSON_OBJ_DESCR_PRIM_NAMED(entry, "file", filename, JSON_TOK_STRING),
    JSON_OBJ_DESCR_PRIM_NAMED(entry, "version_MCUBOOT", version_text, JSON_TOK_STRING),
    JSON_OBJ_DESCR_PRIM_NAMED(entry, "load_address", address, JSON_TOK_NUMBER),
    JSON_OBJ_DESCR_PRIM(entry, size, JSON_TOK_NUMBER),
    JSON_OBJ_DESCR_PRIM_NAMED(entry, "image_index", index, JSON_TOK_STRING),
    JSON_OBJ_DESCR_PRIM_NAMED(entry, "slot_index_primary", primary, JSON_TOK_STRING),
    JSON_OBJ_DESCR_PRIM_NAMED(entry, "slot_index_secondary", secondary, JSON_TOK_STRING),
    JSON_OBJ_DESCR_PRIM_NAMED(entry, "sha256", sha, JSON_TOK_STRING),
};
const json_obj_descr package_descr[] = {
    JSON_OBJ_DESCR_PRIM_NAMED(package, "format-version", format, JSON_TOK_NUMBER),
    JSON_OBJ_DESCR_PRIM(package, name, JSON_TOK_STRING),
    JSON_OBJ_DESCR_OBJ_ARRAY_NAMED(package, "files", files, 2, count, entry_descr, ARRAY_SIZE(entry_descr)),
};
bool equal(const char *value, std::string_view expected) { return value && expected == value; }
uint32_t le32(const uint8_t *p) { return p[0] | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
int hex(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c-'a'+10 : c >= 'A' && c <= 'F' ? c-'A'+10 : -1; }
}
int parse(std::span<char> json, const target &hardware, const version &requested, image &out)
{
    package p;
    if (json.empty() || json_obj_parse(json.data(), json.size(), package_descr, ARRAY_SIZE(package_descr), &p) != 7 ||
        p.format != 1 || !equal(p.name, hardware.name) || p.count != 1) return -EINVAL;
    const auto &f = p.files[0];
    if (!equal(f.type, "application") || !equal(f.board, hardware.board) || !equal(f.soc, hardware.soc) ||
        f.address < 0 || static_cast<uint32_t>(f.address) != hardware.address || !equal(f.index, "0") ||
        !equal(f.primary, "1") || !equal(f.secondary, "2")) return -ENOEXEC;
    image result;
    if (!f.version_text || !parse_version(f.version_text, result.release) || result.release != requested) return -ESTALE;
    if (f.size <= 0 || static_cast<uint32_t>(f.size) > hardware.max_size) return -EFBIG;
    result.size = f.size;
    if (!f.filename) return -EINVAL;
    std::string_view filename{f.filename};
    if (filename.empty() || filename.size() >= result.file.size() || !filename.ends_with(".bin") || filename.front() == '.') return -EINVAL;
    for (char c : filename) if (!(c >= 'a' && c <= 'z') && !(c >= 'A' && c <= 'Z') &&
        !(c >= '0' && c <= '9') && c != '_' && c != '-' && c != '.') return -EINVAL;
    std::copy(filename.begin(), filename.end(), result.file.begin());
    if (f.sha) {
        if (std::strlen(f.sha) != 64) return -EINVAL;
        for (unsigned i = 0; i < 32; ++i) {
            int a = hex(f.sha[i*2]), b = hex(f.sha[i*2+1]);
            if (a < 0 || b < 0) return -EINVAL;
            result.sha256[i] = (a << 4) | b;
        }
        result.has_sha256 = true;
    }
    out = result;
    return 0;
}
bool header_matches(std::span<const uint8_t> h, const image &expected)
{
    if (h.size() < 32 || le32(h.data()) != 0x96f3b83dU || le32(h.data()+4) != 0 ||
        (h[8] | uint32_t(h[9]) << 8) != 0x800 || le32(h.data()+16) != 0) return false;
    version v{h[20],h[21],h[22] | uint32_t(h[23]) << 8,le32(h.data()+24)};
    return v == expected.release && le32(h.data()+12) > 0 && le32(h.data()+12) < expected.size;
}
bool valid_base_url(std::string_view url)
{
    if (!url.starts_with("https://") || url.size() > 220) return false;
    auto tail = url.substr(8);
    auto slash = tail.find('/');
    if (slash == tail.npos || slash == 0 || slash > 100 || tail.find("..") != tail.npos) return false;
    for (size_t i = 0; i < tail.size(); ++i) {
        char c = tail[i];
        bool common = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.';
        if (!common && !(i >= slash && (c == '/' || c == '_'))) return false;
    }
    return true;
}
}
