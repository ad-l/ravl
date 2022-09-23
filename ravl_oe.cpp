// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "ravl_oe.h"

#include "ravl_sgx.h"
#include "ravl_sgx_defs.h"
#include "ravl_url_requests.h"

#include <memory>

// By defining USE_OE_VERIFIER, all requests are simply forwarded to Open
// Enclave. Without this, we support only a subset of attestation formats for
// which we can extract a raw SGX quote, which is verified by ravl::sgx::verify.

using namespace ravl::sgx;

#ifdef USE_OE_VERIFIER
#  ifndef HAVE_OPEN_ENCLAVE
#    error Open Enclave Verifier requires Open Enclave library
#  endif
#  include <openenclave/attestation/sgx/evidence.h>
#  include <openenclave/attestation/verifier.h>
#  include <openenclave/bits/attestation.h>
#  include <openenclave/bits/evidence.h>
#  include <openenclave/bits/result.h>
#else
#  define OE_UUID_SIZE 16
#  define OE_ENUM_MAX 0xffffffff
#  define OE_ATTESTATION_HEADER_VERSION 3
#  define OE_SGX_ENDORSEMENTS_VERSION 1
#  define OE_FORMAT_UUID_SGX_ECDSA \
    { \
      0xa3, 0xa2, 0x1e, 0x87, 0x1b, 0x4d, 0x40, 0x14, 0xb7, 0x0a, 0xa1, 0x25, \
        0xd2, 0xfb, 0xcd, 0x8c \
    }

enum oe_enclave_type_t
{

  OE_ENCLAVE_TYPE_AUTO = 1,
  OE_ENCLAVE_TYPE_SGX = 2,
  OE_ENCLAVE_TYPE_OPTEE = 3,
  __OE_ENCLAVE_TYPE_MAX = OE_ENUM_MAX,
};

enum oe_sgx_endorsements_fields_t
{
  OE_SGX_ENDORSEMENT_FIELD_VERSION,
  OE_SGX_ENDORSEMENT_FIELD_TCB_INFO,
  OE_SGX_ENDORSEMENT_FIELD_TCB_ISSUER_CHAIN,
  OE_SGX_ENDORSEMENT_FIELD_CRL_PCK_CERT,
  OE_SGX_ENDORSEMENT_FIELD_CRL_PCK_PROC_CA,
  OE_SGX_ENDORSEMENT_FIELD_CRL_ISSUER_CHAIN_PCK_CERT,
  OE_SGX_ENDORSEMENT_FIELD_QE_ID_INFO,
  OE_SGX_ENDORSEMENT_FIELD_QE_ID_ISSUER_CHAIN,
  OE_SGX_ENDORSEMENT_FIELD_CREATION_DATETIME,
  OE_SGX_ENDORSEMENT_COUNT
};

#  pragma pack(push, 1)

struct oe_uuid_t
{
  uint8_t b[OE_UUID_SIZE];
};

struct oe_attestation_header_t
{
  uint32_t version;
  oe_uuid_t format_id;
  uint64_t data_size;
#  ifdef _MSC_VER
#    pragma warning(push)
#    pragma warning(disable : 4200)
#  endif
  uint8_t data[];
#  ifdef _MSC_VER
#    pragma warning(pop)
#  endif
};

struct oe_endorsements_t
{
  uint32_t version;
  uint32_t enclave_type;
  uint32_t buffer_size;
  uint32_t num_elements;
#  ifdef _MSC_VER
#    pragma warning(push)
#    pragma warning(disable : 4200)
#  endif
  uint8_t buffer[];
#  ifdef _MSC_VER
#    pragma warning(pop)
#  endif
};

struct oe_sgx_endorsement_item
{
  uint8_t* data;
  uint32_t size;
};

struct oe_sgx_endorsements_t
{
  oe_sgx_endorsement_item items[OE_SGX_ENDORSEMENT_COUNT];
};

#  pragma pack(pop)

#endif

#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>

static oe_uuid_t sgx_remote_uuid = {OE_FORMAT_UUID_SGX_ECDSA};

// From SGX SDK/library headers
typedef struct _sgx_ql_qve_collateral_t
{
  union
  {
    uint32_t version;
    struct
    {
      uint16_t major_version;
      uint16_t minor_version;
    };
  };
  uint32_t tee_type;
  char* pck_crl_issuer_chain;
  uint32_t pck_crl_issuer_chain_size;
  char* root_ca_crl;
  uint32_t root_ca_crl_size;
  char* pck_crl;
  uint32_t pck_crl_size;
  char* tcb_info_issuer_chain;
  uint32_t tcb_info_issuer_chain_size;
  char* tcb_info;
  uint32_t tcb_info_size;
  char* qe_identity_issuer_chain;
  uint32_t qe_identity_issuer_chain_size;
  char* qe_identity;
  uint32_t qe_identity_size;
} sgx_ql_qve_collateral_t;

namespace ravl
{
  namespace oe
  {
    template <typename T>
    void put(const T& t, const uint8_t* data, size_t& pos)
    {
      for (size_t i = 0; i < sizeof(T); i++)
        data[pos + i] = (t >> (8 * (sizeof(T) - i - 1))) & 0xFF;
      pos += sizeof(T);
    }

    template <typename T>
    void put(const T& t, std::vector<uint8_t>& data)
    {
      for (size_t i = 0; i < sizeof(T); i++)
        data.push_back((t >> (8 * (sizeof(T) - i - 1))) & 0xFF);
    }

#ifndef USE_OE_VERIFIER
    std::shared_ptr<sgx::Attestation> extract_sgx_attestation(
      const Attestation& a, const Options& options)
    {
      if (a.evidence.empty())
        throw std::runtime_error("No evidence to verify");

      bool with_plugin_header = false;

      if (with_plugin_header)
      {
        const oe_attestation_header_t* evidence_header =
          (oe_attestation_header_t*)a.evidence.data();
        const oe_attestation_header_t* endorsements_header = nullptr;

        if (a.evidence.size() < sizeof(oe_attestation_header_t))
          throw std::runtime_error(
            "Unknown evidence format: too small to contain attestation format "
            "header");
        if (evidence_header->version != OE_ATTESTATION_HEADER_VERSION)
          throw std::runtime_error("Unsupported evidence format version");
        if (
          a.evidence.size() >
          (evidence_header->data_size + sizeof(oe_attestation_header_t)))
          throw std::runtime_error(
            "Unsupported evidence format: excess evidence data");
        if (
          memcmp(
            &evidence_header->format_id, &sgx_remote_uuid, sizeof(oe_uuid_t)) !=
          0)
          throw std::runtime_error(
            "Unsupported evidence format: only OE_FORMAT_UUID_SGX_ECDSA is "
            "supported");

        if (a.endorsements.size() > 0)
        {
          endorsements_header = (oe_attestation_header_t*)a.endorsements.data();

          if (a.endorsements.size() < sizeof(oe_attestation_header_t))
            throw std::runtime_error(
              "Unknown endorsements format: too small to contain attestation "
              "format header");

          if (endorsements_header->version != OE_ATTESTATION_HEADER_VERSION)
            throw std::runtime_error("Unsupported endorsements format version");
          if (
            a.endorsements.size() >
            (endorsements_header->data_size + sizeof(oe_attestation_header_t)))
            throw std::runtime_error(
              "Unsupported endorsements format: excess data");

          if (
            memcmp(
              &evidence_header->format_id,
              &sgx_remote_uuid,
              sizeof(oe_uuid_t)) != 0)
            throw std::runtime_error(
              "Unsupported endorsements format: only OE_FORMAT_UUID_SGX_ECDSA "
              "is supported");
        }

        return std::make_shared<sgx::Attestation>(
          std::vector<uint8_t>(
            evidence_header->data,
            evidence_header->data + evidence_header->data_size),
          std::vector<uint8_t>(
            endorsements_header->data,
            endorsements_header->data + endorsements_header->data_size));
      }
      else
      {
        const sgx_quote_t* quote = (sgx_quote_t*)a.evidence.data();
        const uint8_t* custom_claims = NULL;

        if (a.evidence.size() < sizeof(sgx_quote_t))
          throw std::runtime_error(
            "Unknown evidence format: too small to contain SGX quote");

        auto squote = a.evidence;

        if (squote.size() > (sizeof(sgx_quote_t) + quote->signature_len))
        {
          custom_claims = quote->signature + quote->signature_len;
          squote.resize(sizeof(sgx_quote_t) + quote->signature_len);
        }

        std::vector<uint8_t> scollateral;

        if (!a.endorsements.empty())
        {
          if (
            a.endorsements.size() < sizeof(oe_endorsements_t) ||
            a.endorsements.size() < sizeof(oe_sgx_endorsements_t))
            throw std::runtime_error(
              "Unknown endorsements format: too small to contain OE/SGX "
              "endorsements");

          const oe_endorsements_t* oeendo =
            (oe_endorsements_t*)a.endorsements.data();

          if (oeendo->version != OE_SGX_ENDORSEMENTS_VERSION)
            throw std::runtime_error(
              "unsupported version of OE endorsements data structure");

          if (oeendo->enclave_type != OE_ENCLAVE_TYPE_SGX)
            throw std::runtime_error(
              "unsupported enclave type in OE endorsements");

          sgx_ql_qve_collateral_t sgxcol = {0};
          sgxcol.major_version = 3;
          sgxcol.minor_version = 1;
          sgxcol.tee_type = 0; // 0 = SGX, 0x81 = TDX

          const uint32_t* offsets = (uint32_t*)oeendo->buffer;
          size_t offsets_size = oeendo->num_elements * sizeof(uint32_t);
          size_t data_size = oeendo->buffer_size - offsets_size;
          const uint8_t* data = oeendo->buffer + offsets_size;

          for (size_t i = 0; i < oeendo->num_elements; i++)
          {
            auto offset = offsets[i];
            if (offset >= data_size)
              throw std::runtime_error("invalid endorsement item offset");

            const uint8_t* item = &data[offset];
            size_t item_size = 0;

            if (i < oeendo->num_elements - 1)
              item_size = offsets[i + 1] - offsets[i];
            else
              item_size = data_size - offsets[i];

            switch (i)
            {
              case 0:
                // OE_SGX_ENDORSEMENT_FIELD_VERSION
                if (item_size != 4 || *((uint32_t*)item) != 1)
                  throw std::runtime_error(
                    "unsupported version of OE endorsements data structure");
                break;
              case 1:
                // OE_SGX_ENDORSEMENT_FIELD_TCB_INFO
                sgxcol.tcb_info = (char*)item;
                sgxcol.tcb_info_size = item_size;
                break;
              case 2:
                // OE_SGX_ENDORSEMENT_FIELD_TCB_ISSUER_CHAIN
                sgxcol.tcb_info_issuer_chain = (char*)item;
                sgxcol.tcb_info_issuer_chain_size = item_size;
                break;
              case 3:
                // OE_SGX_ENDORSEMENT_FIELD_CRL_PCK_CERT
                sgxcol.pck_crl = (char*)item;
                sgxcol.pck_crl_size = item_size;
                break;
              case 4:
                // OE_SGX_ENDORSEMENT_FIELD_CRL_PCK_PROC_CA
                sgxcol.root_ca_crl = (char*)item;
                sgxcol.root_ca_crl_size = item_size;
                break;
              case 5:
                // OE_SGX_ENDORSEMENT_FIELD_CRL_ISSUER_CHAIN_PCK_CERT
                sgxcol.pck_crl_issuer_chain = (char*)item;
                sgxcol.pck_crl_issuer_chain_size = item_size;

                break;
              case 6:
                // OE_SGX_ENDORSEMENT_FIELD_QE_ID_INFO
                sgxcol.qe_identity = (char*)item;
                sgxcol.qe_identity_size = item_size;
                break;
              case 7:
                // OE_SGX_ENDORSEMENT_FIELD_QE_ID_ISSUER_CHAIN
                sgxcol.qe_identity_issuer_chain = (char*)item;
                sgxcol.qe_identity_issuer_chain_size = item_size;
                break;
              case 8:
                // OE_SGX_ENDORSEMENT_FIELD_CREATION_DATETIME
                // Ignore
                break;
              default:
                throw std::runtime_error(
                  "excess elements in OE endorsements data");
            }
          }

          put(sgxcol.major_version, scollateral);
          put(sgxcol.minor_version, scollateral);
          put(sgxcol.tee_type, scollateral);

          for (const auto& [d, sz] : std::vector<std::pair<void*, size_t>>{
                 {sgxcol.pck_crl_issuer_chain,
                  sgxcol.pck_crl_issuer_chain_size},
                 {sgxcol.root_ca_crl, sgxcol.root_ca_crl_size},
                 {sgxcol.pck_crl, sgxcol.pck_crl_size},
                 {sgxcol.tcb_info_issuer_chain,
                  sgxcol.tcb_info_issuer_chain_size},
                 {sgxcol.tcb_info, sgxcol.tcb_info_size},
                 {sgxcol.qe_identity_issuer_chain,
                  sgxcol.qe_identity_issuer_chain_size},
                 {sgxcol.qe_identity, sgxcol.qe_identity_size}})
          {
            put(sz, scollateral);
            for (size_t i = 0; i < sz; i++)
              scollateral.push_back(((uint8_t*)d)[i]);
          }
        }

        return std::make_shared<sgx::Attestation>(squote, scollateral);
      }
    }
#endif

    std::optional<URLRequests> Attestation::prepare_endorsements(
      const Options& options, std::shared_ptr<URLRequestTracker> tracker) const
    {
#ifdef USE_OE_VERIFIER
      return std::nullopt;
#else
      sgx_attestation = extract_sgx_attestation(*this, options);
      return sgx_attestation->prepare_endorsements(options, tracker);
#endif
    }

    bool Attestation::verify(
      const Options& options,
      const std::optional<std::vector<URLResponse>>& url_response_set) const
    {
#ifdef USE_OE_VERIFIER
      if (oe_verifier_initialize() != OE_OK)
        throw std::runtime_error("failed to initialize Open Enclave verifier");

      std::vector<oe_policy_t> policies;

      oe_claim_t* claims = nullptr;
      size_t claims_size = 0;

      oe_result_t r = oe_verify_evidence(
        &sgx_remote_uuid,
        evidence.data(),
        evidence.size(),
        endorsements.size() > 0 ? endorsements.data() : nullptr,
        endorsements.size(),
        policies.data(),
        policies.size(),
        &claims,
        &claims_size);

      if (oe_free_claims(claims, claims_size) != OE_OK)
        throw std::runtime_error("failed to free Open Enclave claims");

      if (oe_verifier_shutdown() != OE_OK)
        throw std::runtime_error("failed to initialize Open Enclave verifier");

      return r == OE_OK;
#else
      if (!sgx_attestation)
        throw std::runtime_error("missing underlying SGX attestation");
      // std::string sat = sgx_attestation;
      // printf("%s\n", sat.c_str());
      return sgx_attestation->verify(options, url_response_set);
#endif
    }
  }
}
