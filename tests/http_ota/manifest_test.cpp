#include "ota/manifest.hpp"
#include "ota/image_verify.hpp"
#include <zephyr/sys/byteorder.h>
#include <psa/crypto.h>
#include <cerrno>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
#include <optional>
static std::string file(const char *p) {std::ifstream f(p,std::ios::binary);assert(f);return {std::istreambuf_iterator<char>(f),{}};}
int flash_area_read(const flash_area *a,uint32_t off,void *out,size_t n) {
    if(off>a->fa_size || n>a->fa_size-off) return -1;
    memcpy(out,a->data+off,n);return 0;
}
static std::string replace(std::string s,const char *before,const char *after) {
    auto p=s.find(before);assert(p!=s.npos);s.replace(p,strlen(before),after);return s;
}
int main(int argc,char **argv) {
    assert(argc>=3 && argc<=5);
    using namespace ota_manifest;
    version v;
    for(auto text:{"1.0.0","1.0.0+0","255.255.65535+4294967295"}) assert(parse_version(text,v));
    for(auto text:{"1","1.0","1.2.3+","1.2.3.4","1.2.3+-1","-1.0.0","256.0.0","1.256.0","1.0.65536","1.0.0+4294967296","1.0.0\r\nAT"}) assert(!parse_version(text,v));
    assert(parse_version("1.0.0",v));
    assert(valid_base_url("https://raw.githubusercontent.com/haseebzaib/test/main/dfu_application/"));
    for(auto url:{"http://github.com/x","https://github.com?x", "https://user@host/x", "https://host/../x", "https://host/\"\r\nAT", "https://host:443/x"}) assert(!valid_base_url(url));
    target t{"smart_power_socket","bl54l15_dvk","nrf54l15",0x20800,0xfc000};
    std::string bytes=file(argv[2]);
    const auto *header=reinterpret_cast<const uint8_t *>(bytes.data());
    v={header[20],header[21],sys_get_le16(header+22),sys_get_le32(header+24)};
    const std::string original=file(argv[1]);
    image metadata;
    auto parseText=[&](std::string text,std::optional<version> wanted=std::nullopt) {
        return parse({text.data(),text.size()},t,wanted.value_or(v),metadata);
    };
    assert(parseText(original)==0);
    assert(parseText(original,version{v.major^1U,v.minor,v.patch,v.build})<0);
    assert(parseText(replace(original,"bl54l15_dvk","wrong_board"))<0);
    assert(parseText(replace(original,"nrf54l15","wrong_soc"))<0);
    assert(parseText(replace(original,"smart_power_socket.signed.bin","../image.bin"))<0);
    assert(parseText(replace(original,"smart_power_socket.signed.bin","image.hex"))<0);
    assert(parseText(replace(original,"133120","0"))<0);
    assert(parseText(replace(original,"\"image_index\": \"0\"","\"image_index\": \"1\""))<0);
    assert(parseText("{}")<0 && parseText("not json")<0);
    std::string duplicate=original;auto a=duplicate.find("{",duplicate.find("["));auto b=duplicate.find("}",a);
    duplicate.insert(b+1,","+duplicate.substr(a,b-a+1));assert(parseText(duplicate)<0);
    std::string extra=original;auto entryEnd=extra.find("}",extra.find("["));
    extra.insert(entryEnd,",\"sha256\":\""+std::string(64,'a')+"\"");assert(!parseText(extra)&&metadata.has_sha256);
    assert(parseText(replace(extra,std::string(64,'a').c_str(),"bad"))<0);
    assert(!parseText(original));
    assert(header_matches({reinterpret_cast<const uint8_t *>(bytes.data()),32},metadata));
    bytes[20]^=1;assert(!header_matches({reinterpret_cast<const uint8_t *>(bytes.data()),32},metadata));bytes[20]^=1;
    puts("PASS: actual SDK manifest/version/header accepted; wrong target/version/slot/path and malformed manifests rejected");
    flash_area area{reinterpret_cast<const uint8_t *>(bytes.data()),bytes.size()};
    assert(!verify_update_signature(&area,bytes.size()));
    testPsaImportStatus=PSA_ERROR_INSUFFICIENT_MEMORY;
    assert(verify_update_signature(&area,bytes.size())==-ENOMEM);
    testPsaImportStatus=PSA_ERROR_HARDWARE_FAILURE;
    assert(verify_update_signature(&area,bytes.size())==-EIO);
    testPsaImportStatus=PSA_ERROR_NOT_SUPPORTED;
    assert(verify_update_signature(&area,bytes.size())==-ENOTSUP);
    testPsaImportStatus=PSA_SUCCESS;
    testPsaVerifyStatus=PSA_ERROR_INSUFFICIENT_MEMORY;
    assert(verify_update_signature(&area,bytes.size())==-ENOMEM);
    testPsaVerifyStatus=PSA_ERROR_HARDWARE_FAILURE;
    assert(verify_update_signature(&area,bytes.size())==-EIO);
    testPsaVerifyStatus=PSA_SUCCESS;
    assert(!verify_update_signature(&area,bytes.size()));
    puts("PASS: PSA key-allocation/resource failures remain distinct from invalid signatures and flash I/O");
    bytes[0x840]^=1;assert(verify_update_signature(&area,bytes.size())<0);bytes[0x840]^=1;
    bytes.back()^=1;assert(verify_update_signature(&area,bytes.size())<0);bytes.back()^=1;
    auto pos=0x800+sys_get_le32(area.data+12)+sys_get_le16(area.data+10)+4;
    bytes[pos+4]^=1;assert(verify_update_signature(&area,bytes.size())<0);bytes[pos+4]^=1;
    assert(verify_update_signature(&area,bytes.size()-1)<0);
    assert(!verify_update_signature(&area,bytes.size()));
    puts("PASS: production pre-reboot verifier accepts real signature; rejects payload/signature/key-hash tampering and truncation");
    if(argc>=4) {
        std::string other=file(argv[3]);
        flash_area recovery{reinterpret_cast<const uint8_t *>(other.data()),other.size()};
        assert(!verify_update_signature(&recovery,other.size()));
        puts("PASS: the same application verifier accepts a real image signed by the second project key");
    }
    if(argc>=5) {
        std::string other=file(argv[4]);
        flash_area untrusted{reinterpret_cast<const uint8_t *>(other.data()),other.size()};
        assert(verify_update_signature(&untrusted,other.size())<0);
        puts("PASS: an authentic SDK-example-key image is rejected by the project trust set");
    }
}
