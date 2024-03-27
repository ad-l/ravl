// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the Apache 2.0 License.
#pragma once

#include "json.h"

#include <string>
namespace ravl::crypto
{
  // SNIPPET_START: supported_curves
  enum class CurveID
  {
    /// No curve
    NONE = 0,
    /// The SECP384R1 curve
    SECP384R1,
    /// The SECP256R1 curve
    SECP256R1,
    /// The SECP256K1 curve
    SECP256K1,
    /// The CURVE25519 curve
    CURVE25519,
    X25519
  };

  enum class JsonWebKeyType
  {
    EC = 0,
    RSA = 1,
    OKP = 2
  };

  struct JsonWebKey
  {
    JsonWebKeyType kty;
    std::optional<std::string> kid = std::nullopt;
    std::optional<std::vector<std::string>> x5c = std::nullopt;

    bool operator==(const JsonWebKey&) const = default;
  };

  enum class JsonWebKeyECCurve
  {
    P256 = 0,
    P256K1 = 1,
    P384 = 2,
    P521 = 3
  };

  static JsonWebKeyECCurve curve_id_to_jwk_curve(CurveID curve_id)
  {
    switch (curve_id)
    {
      case CurveID::SECP384R1:
        return JsonWebKeyECCurve::P384;
      case CurveID::SECP256R1:
        return JsonWebKeyECCurve::P256;
      case CurveID::SECP256K1:
        return JsonWebKeyECCurve::P256K1;
      default:
        throw std::logic_error(fmt::format("Unknown curve"));
    }
  }

  static CurveID jwk_curve_to_curve_id(JsonWebKeyECCurve jwk_curve)
  {
    switch (jwk_curve)
    {
      case JsonWebKeyECCurve::P384:
        return CurveID::SECP384R1;
      case JsonWebKeyECCurve::P256:
        return CurveID::SECP256R1;
      case JsonWebKeyECCurve::P256K1:
        return CurveID::SECP256K1;
      default:
        throw std::logic_error(fmt::format("Unknown JWK curve"));
    }
  }

  enum class JsonWebKeyEdDSACurve
  {
    ED25519 = 0,
    X25519 = 1
  };

  static JsonWebKeyEdDSACurve curve_id_to_jwk_eddsa_curve(CurveID curve_id)
  {
    switch (curve_id)
    {
      case CurveID::CURVE25519:
        return JsonWebKeyEdDSACurve::ED25519;
      case CurveID::X25519:
        return JsonWebKeyEdDSACurve::X25519;
      default:
        throw std::logic_error(fmt::format("Unknown EdDSA curve"));
    }
  }

  struct JsonWebKeyECPublic : JsonWebKey
  {
    JsonWebKeyECCurve crv;
    std::string x; // base64url
    std::string y; // base64url

    bool operator==(const JsonWebKeyECPublic&) const = default;
  };

  struct JsonWebKeyECPrivate : JsonWebKeyECPublic
  {
    std::string d; // base64url

    bool operator==(const JsonWebKeyECPrivate&) const = default;
  };

  struct JsonWebKeyRSAPublic : JsonWebKey
  {
    std::string n; // base64url
    std::string e; // base64url

    bool operator==(const JsonWebKeyRSAPublic&) const = default;
  };

  struct JsonWebKeyRSAPrivate : JsonWebKeyRSAPublic
  {
    std::string d; // base64url
    std::string p; // base64url
    std::string q; // base64url
    std::string dp; // base64url
    std::string dq; // base64url
    std::string qi; // base64url

    bool operator==(const JsonWebKeyRSAPrivate&) const = default;
  };

  struct JsonWebKeyEdDSAPublic : JsonWebKey
  {
    JsonWebKeyEdDSACurve crv;
    std::string x; // base64url

    bool operator==(const JsonWebKeyEdDSAPublic&) const = default;
  };

  struct JsonWebKeyEdDSAPrivate : JsonWebKeyEdDSAPublic
  {
    std::string d; // base64url

    bool operator==(const JsonWebKeyEdDSAPrivate&) const = default;
  };
}

namespace ravl {
  template <>
  struct ravl_json_serializer<crypto::JsonWebKeyRSAPublic>
  {
    inline static void to_json(
      ravl::json& j, const crypto::JsonWebKeyRSAPublic& x)
    {
    }

    inline static void from_json(
      const ravl::json& j, crypto::JsonWebKeyRSAPublic& x)
    {
      std::string kty;
      j.at("kty").get_to(kty);
      if (kty == "RSA") {
        x.kty = crypto::JsonWebKeyType::RSA;
      } 
      else if (kty == "EC") {
        x.kty = crypto::JsonWebKeyType::EC;
      }
      else if (kty == "OKP") {
        x.kty == crypto::JsonWebKeyType::OKP;
      }
      else {
        throw std::logic_error("Unsupported Json Web Key Type");
      }

      j.at("n").get_to(x.n);
      j.at("e").get_to(x.e);
    }
  };
}