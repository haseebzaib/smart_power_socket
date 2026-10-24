#include "image_verify.hpp"
#include "ota_public_key.hpp"
#include <array>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <psa/crypto.h>
#include <zephyr/sys/byteorder.h>

int verify_update_signature(const flash_area *area, uint32_t length)
{
    std::array<uint8_t,32> header{};
    int rc = flash_area_read(area,0,header.data(),header.size());
    if (rc) return rc;
    uint32_t h = sys_get_le16(header.data()+8), body = sys_get_le32(header.data()+12), prot = sys_get_le16(header.data()+10);
    if (sys_get_le32(header.data()) != 0x96f3b83dU || h != 0x800 || !body ||
        h > length || body > length-h || prot > length-h-body) return -EBADMSG;
    uint32_t protectedEnd = h + body + prot;
    if (length-protectedEnd < 4) return -EBADMSG;
    uint8_t tlv[4];
    rc = flash_area_read(area,protectedEnd,tlv,4);
    if (rc) return rc;
    if (sys_get_le16(tlv)!=0x6907 || sys_get_le16(tlv+2)!=length-protectedEnd) return -EBADMSG;
    std::array<uint8_t,64> expectedDigest{}, signature{}, actualDigest{};
    std::array<uint8_t,64> keyHash{}, actualKeyHash{};
    bool gotHash=false,gotSignature=false,gotKey=false;
    for (uint32_t pos=protectedEnd+4;pos<length;) {
        if (length-pos<4) return -EBADMSG;
        if ((rc=flash_area_read(area,pos,tlv,4))) return rc;
        uint16_t type=sys_get_le16(tlv), size=sys_get_le16(tlv+2);
        pos+=4;
        if (size>length-pos) return -EBADMSG;
        if (type==0x12) {
            if (gotHash || size!=expectedDigest.size()) return -EBADMSG;
            if ((rc=flash_area_read(area,pos,expectedDigest.data(),size))) return rc;
            gotHash=true;
        } else if (type==0x24) {
            if (gotSignature || size!=signature.size()) return -EBADMSG;
            if ((rc=flash_area_read(area,pos,signature.data(),size))) return rc;
            gotSignature=true;
        } else if (type==0x01) {
            if (gotKey || size!=keyHash.size()) return -EBADMSG;
            if ((rc=flash_area_read(area,pos,keyHash.data(),size))) return rc;
            gotKey=true;
        } else if (type==0x10 || type==0x11 || type==0x25 || type==0x40) {
            // This single-app profile supports SHA-512 hash signing only,
            // not pure-signature, alternate hashes or dependent images.
            return -ENOTSUP;
        }
        pos+=size;
    }
    if (!gotHash || !gotSignature || !gotKey) return -EBADMSG;
    size_t n=0;
    if (psa_hash_compute(PSA_ALG_SHA_512,ota_public_key::der.data(),ota_public_key::der.size(),actualKeyHash.data(),actualKeyHash.size(),&n)!=PSA_SUCCESS ||
        n!=keyHash.size() || actualKeyHash!=keyHash) return -EACCES;
    psa_hash_operation_t hash=PSA_HASH_OPERATION_INIT;
    if (psa_hash_setup(&hash,PSA_ALG_SHA_512)!=PSA_SUCCESS) return -EIO;
    std::array<uint8_t,1024> chunk{};
    for (uint32_t pos=0;pos<protectedEnd;) {
        size_t count=std::min<size_t>(chunk.size(),protectedEnd-pos);
        rc=flash_area_read(area,pos,chunk.data(),count);
        if (!rc && psa_hash_update(&hash,chunk.data(),count)!=PSA_SUCCESS) rc=-EIO;
        if (rc) { psa_hash_abort(&hash); return rc; }
        pos+=count;
    }
    if (psa_hash_finish(&hash,actualDigest.data(),actualDigest.size(),&n)!=PSA_SUCCESS || n!=actualDigest.size()) {
        psa_hash_abort(&hash); return -EIO;
    }
    if (actualDigest!=expectedDigest) return -EACCES;
    psa_key_attributes_t attributes=PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attributes,PSA_KEY_TYPE_ECC_PUBLIC_KEY(PSA_ECC_FAMILY_TWISTED_EDWARDS));
    psa_set_key_usage_flags(&attributes,PSA_KEY_USAGE_VERIFY_MESSAGE);
    psa_set_key_algorithm(&attributes,PSA_ALG_PURE_EDDSA);
    psa_key_id_t key;
    auto status=psa_import_key(&attributes,ota_public_key::raw.data(),ota_public_key::raw.size(),&key);
    psa_reset_key_attributes(&attributes);
    if (status!=PSA_SUCCESS) return -EIO;
    status=psa_verify_message(key,PSA_ALG_PURE_EDDSA,actualDigest.data(),actualDigest.size(),signature.data(),signature.size());
    auto destroyed=psa_destroy_key(key);
    return status==PSA_SUCCESS && destroyed==PSA_SUCCESS ? 0 : -EACCES;
}
