// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the Apache 2.0 License.
#pragma once

#include <string>
#include <vector>

namespace ravl::did
{
  // From https://www.w3.org/TR/did-core.
  // Note that the types defined in this file do not exhaustively cover
  // all fields and types from the spec.
  struct DIDDocumentVerificationMethod
  {
    std::string id;
    std::string type;
    std::string controller;
    std::optional<crypto::JsonWebKeyRSAPublic> public_key_jwk =
      std::nullopt; // Note: Only supports RSA for now

    bool operator==(const DIDDocumentVerificationMethod&) const = default;
  };

  struct DIDDocument
  {
    std::string id;
    std::string context;
    std::string type;
    std::vector<DIDDocumentVerificationMethod> verification_method = {};
    nlohmann::json assertion_method = {};

    bool operator==(const DIDDocument&) const = default;
  };
}