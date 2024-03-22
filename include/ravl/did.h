// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the Apache 2.0 License.
#pragma once

#include "json.h"
#include <string>
#include <vector>

namespace ravl
{
  namespace did 
  {
    // From https://www.w3.org/TR/did-core.
    // Note that the types defined in this file do not exhaustively cover
    // all fields and types from the spec.
    struct DIDDocumentVerificationMethod
    {
      std::string id;
      std::string type;
      std::string controller;
      crypto::JsonWebKeyRSAPublic publicKeyJwk;

      bool operator==(const DIDDocumentVerificationMethod&) const = default;
    };

    struct DIDDocument
    {
      std::string id;
      std::string context;
      std::vector<DIDDocumentVerificationMethod> verificationMethod = {};
      std::string assertionMethod;

      bool operator==(const DIDDocument&) const = default;
    };
  }

  RAVL_JSON_DEFINE_TYPE_NON_INTRUSIVE(did::DIDDocumentVerificationMethod, 
    id, 
    type, 
    controller, 
    publicKeyJwk);

  RAVL_JSON_DEFINE_TYPE_NON_INTRUSIVE(did::DIDDocument, 
    id, 
    verificationMethod, 
    assertionMethod);

}

