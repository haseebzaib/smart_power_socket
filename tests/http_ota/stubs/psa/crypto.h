#pragma once
#include <cstdint>
#include <cstddef>
#include <openssl/evp.h>
using psa_status_t=int;
using psa_key_id_t=uintptr_t;
struct psa_key_attributes_t {int unused;};
struct psa_hash_operation_t {EVP_MD_CTX *ctx;};
#define PSA_HASH_OPERATION_INIT {nullptr}
#define PSA_KEY_ATTRIBUTES_INIT {0}
#define PSA_SUCCESS 0
#define PSA_ERROR_INSUFFICIENT_MEMORY (-141)
#define PSA_ERROR_NOT_SUPPORTED (-134)
#define PSA_ERROR_HARDWARE_FAILURE (-147)
#define PSA_ERROR_INVALID_SIGNATURE (-149)
extern psa_status_t testPsaImportStatus;
extern psa_status_t testPsaVerifyStatus;
#define PSA_ALG_SHA_256 256
#define PSA_ALG_SHA_512 512
#define PSA_ALG_PURE_EDDSA 1
#define PSA_KEY_TYPE_ECC_PUBLIC_KEY(x) (x)
#define PSA_ECC_FAMILY_TWISTED_EDWARDS 1
#define PSA_KEY_USAGE_VERIFY_MESSAGE 1
int psa_hash_setup(psa_hash_operation_t *,int);
int psa_hash_update(psa_hash_operation_t *,const uint8_t *,size_t);
int psa_hash_finish(psa_hash_operation_t *,uint8_t *,size_t,size_t *);
int psa_hash_abort(psa_hash_operation_t *);
int psa_hash_compute(int,const uint8_t *,size_t,uint8_t *,size_t,size_t *);
inline void psa_set_key_type(psa_key_attributes_t *,int) {}
inline void psa_set_key_usage_flags(psa_key_attributes_t *,int) {}
inline void psa_set_key_algorithm(psa_key_attributes_t *,int) {}
inline void psa_reset_key_attributes(psa_key_attributes_t *) {}
int psa_import_key(psa_key_attributes_t *,const uint8_t *,size_t,psa_key_id_t *);
int psa_verify_message(psa_key_id_t,int,const uint8_t *,size_t,const uint8_t *,size_t);
int psa_destroy_key(psa_key_id_t);
