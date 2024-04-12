// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the Apache 2.0 License.
#pragma once

#include "jwk.h"
#include "json.h"
#include "cose_verifier.h"
#include "cose_verifier_impl.h"
#include "cose_common.h"
#include "did.h"

#include <didx509cpp/didx509cpp.h>
#include <nlohmann/json.hpp>
#include <qcbor/qcbor.h>
#include <qcbor/qcbor_spiffy_decode.h>
#include <span>
#include <t_cose/t_cose_sign1_verify.h>

namespace ravl
{
  using DID = std::string;
  using Feed = std::string;

  struct UVMEndorsements
  {
    DID did;
    Feed feed;
    std::string svn;

    bool operator==(const UVMEndorsements&) const = default;
  };

  struct UVMEndorsementsPayload
  {
    std::string sevsnpvm_guest_svn;
    std::string sevsnpvm_launch_measurement;
  };

  template <>
  struct ravl_json_serializer<UVMEndorsementsPayload>
  {
    inline static void to_json(
      ravl::json& j, const UVMEndorsementsPayload& x)
    {
    }

    inline static void from_json(
      const ravl::json& j, UVMEndorsementsPayload& x)
    {
      j.at("x-ms-sevsnpvm-guestsvn").get_to(x.sevsnpvm_guest_svn);
      j.at("x-ms-sevsnpvm-launchmeasurement").get_to(x.sevsnpvm_launch_measurement);
    }
  };

  struct UvmEndorsementsProtectedHeader
  {
    int64_t alg;
    std::string content_type;
    std::vector<std::vector<uint8_t>> x5_chain;
    std::string iss;
    std::string feed;
  };

  // Roots of trust for UVM endorsements/measurement in AMD SEV-SNP attestations
  static std::vector<UVMEndorsements> uvm_roots_of_trust = {
    // Confidential Azure Kubertnetes Service (AKS)
    {"did:x509:0:sha256:I__iuL25oXEVFdTP_aBLx_eT1RPHbCQ_ECBQfYZpt9s::eku:1.3.6."
     "1.4.1.311.76.59.1.2",
     "ContainerPlat-AMD-UVM",
     "0"},
    // Confidential Azure Container Instances (ACI)
    {"did:x509:0:sha256:I__iuL25oXEVFdTP_aBLx_eT1RPHbCQ_ECBQfYZpt9s::eku:1.3.6."
     "1.4.1.311.76.59.1.5",
     "ConfAKS-AMD-UVM",
     "0"}};

  bool inline matches_uvm_roots_of_trust(const UVMEndorsements& endorsements)
  {
    for (const auto& uvm_root_of_trust : uvm_roots_of_trust)
    {
      if (
        uvm_root_of_trust.did == endorsements.did &&
        uvm_root_of_trust.feed == endorsements.feed &&
        uvm_root_of_trust.svn <= endorsements.svn)
      {
        return true;
      }
    }
    return false;
  }

  namespace cose
  {
    static constexpr auto HEADER_PARAM_ISSUER = "iss";
    static constexpr auto HEADER_PARAM_FEED = "feed";

    static std::vector<std::vector<uint8_t>> decode_x5chain(
      QCBORDecodeContext& ctx, const QCBORItem& x5chain)
    {
      std::vector<std::vector<uint8_t>> parsed;

      if (x5chain.uDataType == QCBOR_TYPE_ARRAY)
      {
        QCBORDecode_EnterArrayFromMapN(&ctx, headers::PARAM_X5CHAIN);
        while (true)
        {
          QCBORItem item;
          auto result = QCBORDecode_GetNext(&ctx, &item);
          if (result == QCBOR_ERR_NO_MORE_ITEMS)
          {
            break;
          }
          if (result != QCBOR_SUCCESS)
          {
            throw COSEDecodeError("Item in x5chain is not well-formed");
          }
          if (item.uDataType == QCBOR_TYPE_BYTE_STRING)
          {
            parsed.push_back(qcbor_buf_to_byte_vector(item.val.string));
          }
          else
          {
            throw COSEDecodeError(
              "Next item in x5chain was not of type byte string");
          }
        }
        QCBORDecode_ExitArray(&ctx);
        if (parsed.empty())
        {
          throw COSEDecodeError("x5chain array length was 0 in COSE header");
        }
      }
      else if (x5chain.uDataType == QCBOR_TYPE_BYTE_STRING)
      {
        parsed.push_back(qcbor_buf_to_byte_vector(x5chain.val.string));
      }
      else
      {
        throw COSEDecodeError(fmt::format(
          "Value type {} of x5chain in COSE header is not array or byte string",
          x5chain.uDataType));
      }

      return parsed;
    }

    static UvmEndorsementsProtectedHeader decode_protected_header(
      const std::vector<uint8_t>& uvm_endorsements_raw)
    {
      UsefulBufC msg{uvm_endorsements_raw.data(), uvm_endorsements_raw.size()};

      QCBORError qcbor_result;

      QCBORDecodeContext ctx;
      QCBORDecode_Init(&ctx, msg, QCBOR_DECODE_MODE_NORMAL);

      QCBORDecode_EnterArray(&ctx, nullptr);
      qcbor_result = QCBORDecode_GetError(&ctx);
      if (qcbor_result != QCBOR_SUCCESS)
      {
        throw COSEDecodeError("Failed to parse COSE_Sign1 outer array");
      }

      uint64_t tag = QCBORDecode_GetNthTagOfLast(&ctx, 0);
      if (tag != CBOR_TAG_COSE_SIGN1)
      {
        throw COSEDecodeError("Failed to parse COSE_Sign1 tag");
      }

      struct q_useful_buf_c protected_parameters;
      QCBORDecode_EnterBstrWrapped(
        &ctx, QCBOR_TAG_REQUIREMENT_NOT_A_TAG, &protected_parameters);
      QCBORDecode_EnterMap(&ctx, NULL);

      enum
      {
        ALG_INDEX,
        CONTENT_TYPE_INDEX,
        X5_CHAIN_INDEX,
        ISS_INDEX,
        FEED_INDEX,
        END_INDEX
      };
      QCBORItem header_items[END_INDEX + 1];

      header_items[ALG_INDEX].label.int64 = headers::PARAM_ALG;
      header_items[ALG_INDEX].uLabelType = QCBOR_TYPE_INT64;
      header_items[ALG_INDEX].uDataType = QCBOR_TYPE_INT64;

      header_items[CONTENT_TYPE_INDEX].label.int64 =
        headers::PARAM_CONTENT_TYPE;
      header_items[CONTENT_TYPE_INDEX].uLabelType = QCBOR_TYPE_INT64;
      header_items[CONTENT_TYPE_INDEX].uDataType = QCBOR_TYPE_TEXT_STRING;

      header_items[X5_CHAIN_INDEX].label.int64 = headers::PARAM_X5CHAIN;
      header_items[X5_CHAIN_INDEX].uLabelType = QCBOR_TYPE_INT64;
      header_items[X5_CHAIN_INDEX].uDataType = QCBOR_TYPE_ANY;

      header_items[ISS_INDEX].label.string =
        UsefulBuf_FromSZ(HEADER_PARAM_ISSUER);
      header_items[ISS_INDEX].uLabelType = QCBOR_TYPE_TEXT_STRING;
      header_items[ISS_INDEX].uDataType = QCBOR_TYPE_TEXT_STRING;

      header_items[FEED_INDEX].label.string =
        UsefulBuf_FromSZ(HEADER_PARAM_FEED);
      header_items[FEED_INDEX].uLabelType = QCBOR_TYPE_TEXT_STRING;
      header_items[FEED_INDEX].uDataType = QCBOR_TYPE_TEXT_STRING;

      header_items[END_INDEX].uLabelType = QCBOR_TYPE_NONE;

      QCBORDecode_GetItemsInMap(&ctx, header_items);
      qcbor_result = QCBORDecode_GetError(&ctx);
      if (qcbor_result != QCBOR_SUCCESS)
      {
        throw COSEDecodeError("Failed to decode protected header");
      }

      UvmEndorsementsProtectedHeader phdr = {};

      if (header_items[ALG_INDEX].uDataType != QCBOR_TYPE_NONE)
      {
        phdr.alg = header_items[ALG_INDEX].val.int64;
      }

      if (header_items[CONTENT_TYPE_INDEX].uDataType != QCBOR_TYPE_NONE)
      {
        phdr.content_type =
          qcbor_buf_to_string(header_items[CONTENT_TYPE_INDEX].val.string);
      }

      if (header_items[X5_CHAIN_INDEX].uDataType != QCBOR_TYPE_NONE)
      {
        phdr.x5_chain = decode_x5chain(ctx, header_items[X5_CHAIN_INDEX]);
      }

      if (header_items[ISS_INDEX].uDataType != QCBOR_TYPE_NONE)
      {
        phdr.iss = qcbor_buf_to_string(header_items[ISS_INDEX].val.string);
      }

      if (header_items[FEED_INDEX].uDataType != QCBOR_TYPE_NONE)
      {
        phdr.feed = qcbor_buf_to_string(header_items[FEED_INDEX].val.string);
      }

      QCBORDecode_ExitMap(&ctx);
      QCBORDecode_ExitBstrWrapped(&ctx);

      qcbor_result = QCBORDecode_GetError(&ctx);
      if (qcbor_result != QCBOR_SUCCESS)
      {
        throw COSEDecodeError(
          fmt::format("Failed to decode protected header"));
      }

      return phdr;
    }
  }

  static std::span<const uint8_t> verify_uvm_endorsements_signature(
    const crypto::JsonWebKeyRSAPublic& pubkey,
    const std::vector<uint8_t>& uvm_endorsements_raw)
  {
    auto verifier = crypto::make_cose_verifier();

    std::span<uint8_t> payload;
    if (!verifier->verify(pubkey, uvm_endorsements_raw, payload))
    {
      throw cose::COSESignatureValidationError("Signature verification failed");
    }

    return payload;
  }

  static UVMEndorsements verify_uvm_endorsements(
    const std::vector<uint8_t>& uvm_endorsements_raw,
    const std::vector<uint8_t>& uvm_measurement)
  {
    auto phdr = cose::decode_protected_header(uvm_endorsements_raw);

    if (phdr.content_type != cose::headers::CONTENT_TYPE_APPLICATION_JSON_VALUE)
    {
      throw std::logic_error(fmt::format(
        "Unexpected payload content type {}, expected {}",
        phdr.content_type,
        cose::headers::CONTENT_TYPE_APPLICATION_JSON_VALUE));
    }

    if (!cose::is_rsa_alg(phdr.alg))
    {
      throw std::logic_error(
        fmt::format("Signature algorithm {} is not expected RSA", phdr.alg));
    }

    std::string pem_chain;
    for (auto const& c : phdr.x5_chain)
    {
      pem_chain += crypto::cert_der_to_pem(c);
    }

    const auto& did = phdr.iss;
    auto did_document_str = didx509::resolve(pem_chain, did);
    std::cout << did_document_str << "\n";

    did::DIDDocument did_document;
    try 
    {
      auto j = ravl::json::parse(did_document_str);
      ravl::ravl_json_serializer<did::DIDDocument>::from_json(j, did_document);
    }
    catch(std::exception& ex)
    {
      std::cout << ex.what();
      throw std::runtime_error(ex.what());
    }

    if (did_document.verificationMethod.empty())
    {
      throw std::logic_error(fmt::format(
        "Could not find verification method for DID document: {}",
        did_document_str));
    }

    std::optional<crypto::JsonWebKeyRSAPublic> pubk = std::nullopt;
    bool found = false;
    for (auto const& vm : did_document.verificationMethod)
    {
      if (vm.controller == did)
      {
        pubk.emplace(vm.publicKeyJwk);
        break;
      }
    }

    if (!pubk.has_value())
    {
      throw std::logic_error(fmt::format(
        "Could not find matching public key for DID {} in {}",
        did,
        did_document_str));
    }

    auto raw_payload =
      verify_uvm_endorsements_signature(pubk.value(), uvm_endorsements_raw);

    UVMEndorsementsPayload payload = ravl::json::parse(raw_payload);
    auto uvm_measurement_hex = to_hex(uvm_measurement);
    if (payload.sevsnpvm_launch_measurement != uvm_measurement_hex)
    {
      throw std::logic_error(fmt::format(
        "Launch measurement in UVM endorsements payload {} is not equal "
        "to UVM attestation measurement {}",
        payload.sevsnpvm_launch_measurement,
        uvm_measurement_hex));
    }

    UVMEndorsements end{did, phdr.feed, payload.sevsnpvm_guest_svn};

    if (!matches_uvm_roots_of_trust(end))
    {
      throw std::logic_error(fmt::format(
        "UVM endorsements did {}, feed {}, svn {} "
        "do not match any of the known UVM roots of trust",
        end.did,
        end.feed,
        end.svn));
    }

    return end;
  }
}

