// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the Apache 2.0 License.
#pragma once

#include "crypto.h"
#include <chrono>

namespace ravl::crypto
{
  class COSEVerifier
  {
  public:
    virtual bool verify(
      const JsonWebKeyRSAPublic& pubk,
      const std::span<const uint8_t>& buf,
      std::span<uint8_t>& authned_content) const = 0;
    virtual ~COSEVerifier() = default;
  };

  using COSEVerifierUniquePtr = std::unique_ptr<COSEVerifier>;

  COSEVerifierUniquePtr make_cose_verifier(const JsonWebKeyRSAPublic& pubk);

  class COSEVerifier_OpenSSL : public COSEVerifier
  {
  public:
    COSEVerifier_OpenSSL();
    virtual ~COSEVerifier_OpenSSL() override;
    virtual bool verify(
      const JsonWebKeyRSAPublic& pubk,
      const std::span<const uint8_t>& buf,
      std::span<uint8_t>& authned_content) const override;
  };  
}
