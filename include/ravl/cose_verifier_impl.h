// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the Apache 2.0 License.

#include "cose_verifier.h"

#include <openssl/evp.h>
#include <openssl/ossl_typ.h>
#include <openssl/x509.h>
#include <openssl/x509_vfy.h>
#include <t_cose/t_cose_sign1_verify.h>

namespace ravl::crypto
{
  using namespace OpenSSL;

  COSEVerifier_OpenSSL::COSEVerifier_OpenSSL()
  {
  }

  COSEVerifier_OpenSSL::~COSEVerifier_OpenSSL() 
  {
  }

  bool COSEVerifier_OpenSSL::verify(
    const JsonWebKeyRSAPublic& pubk,
    const std::span<const uint8_t>& buf,
    std::span<uint8_t>& authned_content) const
  {
    UqEVP_PKEY_RSA rsa_key(pubk);
    t_cose_key cose_key;
    cose_key.crypto_lib = T_COSE_CRYPTO_LIB_OPENSSL;
    cose_key.k.key_ptr = rsa_key;

    t_cose_sign1_verify_ctx verify_ctx;
    t_cose_sign1_verify_init(&verify_ctx, T_COSE_OPT_TAG_REQUIRED);
    t_cose_sign1_set_verification_key(&verify_ctx, cose_key);

    q_useful_buf_c buf_;
    buf_.ptr = buf.data();
    buf_.len = buf.size();

    q_useful_buf_c authned_content_;

    t_cose_err_t error =
      t_cose_sign1_verify(&verify_ctx, buf_, &authned_content_, nullptr);
    if (error == T_COSE_SUCCESS)
    {
      authned_content = {(uint8_t*)authned_content_.ptr, authned_content_.len};
      return true;
    }

    return false;
  }

  COSEVerifierUniquePtr make_cose_verifier()
  {
    return std::make_unique<COSEVerifier_OpenSSL>();
  }
}
