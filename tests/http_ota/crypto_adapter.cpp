// Host adapter runs real SHA-256/SHA-512/Ed25519 via OpenSSL, not crypto mocks.
#include <psa/crypto.h>
psa_status_t testPsaImportStatus=PSA_SUCCESS;
psa_status_t testPsaVerifyStatus=PSA_SUCCESS;
int psa_hash_setup(psa_hash_operation_t *op,int alg) {
    op->ctx=EVP_MD_CTX_new();
    return op->ctx && EVP_DigestInit_ex(op->ctx,alg==256?EVP_sha256():EVP_sha512(),nullptr)==1?0:-1;
}
int psa_hash_update(psa_hash_operation_t *op,const uint8_t *p,size_t n) {return EVP_DigestUpdate(op->ctx,p,n)==1?0:-1;}
int psa_hash_abort(psa_hash_operation_t *op) {EVP_MD_CTX_free(op->ctx);op->ctx=nullptr;return 0;}
int psa_hash_finish(psa_hash_operation_t *op,uint8_t *out,size_t cap,size_t *n) {
    if(cap<static_cast<size_t>(EVP_MD_CTX_get_size(op->ctx))) return -1;
    unsigned size=0;int rc=EVP_DigestFinal_ex(op->ctx,out,&size);*n=size;psa_hash_abort(op);return rc==1?0:-1;
}
int psa_hash_compute(int alg,const uint8_t *p,size_t n,uint8_t *out,size_t cap,size_t *size) {
    psa_hash_operation_t op=PSA_HASH_OPERATION_INIT;
    int rc=psa_hash_setup(&op,alg);
    if(!rc) rc=psa_hash_update(&op,p,n);
    if(!rc) rc=psa_hash_finish(&op,out,cap,size);
    psa_hash_abort(&op);return rc;
}
int psa_import_key(psa_key_attributes_t *,const uint8_t *p,size_t n,psa_key_id_t *id) {
    if (testPsaImportStatus!=PSA_SUCCESS) return testPsaImportStatus;
    auto key=EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519,nullptr,p,n);
    *id=reinterpret_cast<uintptr_t>(key);return key?0:-1;
}
int psa_verify_message(psa_key_id_t id,int,const uint8_t *p,size_t n,const uint8_t *sig,size_t len) {
    if (testPsaVerifyStatus!=PSA_SUCCESS) return testPsaVerifyStatus;
    auto ctx=EVP_MD_CTX_new();
    int rc=EVP_DigestVerifyInit(ctx,nullptr,nullptr,nullptr,reinterpret_cast<EVP_PKEY *>(id));
    if(rc==1) rc=EVP_DigestVerify(ctx,sig,len,p,n);
    EVP_MD_CTX_free(ctx);return rc==1?PSA_SUCCESS:PSA_ERROR_INVALID_SIGNATURE;
}
int psa_destroy_key(psa_key_id_t id) {EVP_PKEY_free(reinterpret_cast<EVP_PKEY *>(id));return 0;}
