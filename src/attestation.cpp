// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "ravl/attestation.h"

#include "ravl/crypto.h"
#include "ravl/json.h"
#include "ravl/oe.h"
#include "ravl/sev_snp.h"
#include "ravl/sgx.h"
#include "ravl/aci.h"

namespace ravl
{
  using namespace crypto;

  NLOHMANN_JSON_SERIALIZE_ENUM(
    Source,
    {
      {Source::SGX, "sgx"},
      {Source::SEV_SNP, "sevsnp"},
      {Source::OPEN_ENCLAVE, "openenclave"},
      {Source::ACI, "aci"}
    })

  std::string to_string(Source src)
  {
    ravl::json j;
    to_json(j, src);
    return j.dump();
  }

  static ravl::json attestation_json(const Attestation& a, bool base64 = true)
  {
    ravl::json j;
    j["source"] = a.source;
    if (base64)
      j["evidence"] = to_base64(a.evidence);
    else
      j["evidence"] = a.evidence;
    if (!a.endorsements.empty())
    {
      if (base64)
        j["endorsements"] = to_base64(a.endorsements);
      else
        j["endorsements"] = a.endorsements;
    }
    return j;
  }

  Attestation::operator std::string() const
  {
    return attestation_json(*this).dump();
  }

  static std::shared_ptr<Attestation> parse(
    const ravl::json& j, bool base64 = true)
  {
    try
    {
      std::shared_ptr<Attestation> r = nullptr;
      auto source = j.at("source").get<Source>();
      std::vector<uint8_t> evidence;
      if (base64)
        evidence = from_base64(j.at("evidence").get<std::string>());
      else
        evidence = j.at("evidence").get<std::vector<uint8_t>>();
      std::vector<uint8_t> endorsements;

      if (j.contains("endorsements"))
      {
        if (base64)
          endorsements = from_base64(j.at("endorsements").get<std::string>());
        else
          endorsements = j.at("endorsements").get<std::vector<uint8_t>>();
      }

      std::vector<uint8_t> uvm_endorsements;
      if (j.contains("uvm_endorsements"))
      {
        if (base64) 
          uvm_endorsements = from_base64(j.at("uvm_endorsements").get<std::string>());
        else 
          uvm_endorsements = j.at("uvm_endorsements").get<std::vector<uint8_t>>();
      }

      switch (source)
      {
        case Source::SGX:
          r = std::make_shared<sgx::Attestation>(evidence, endorsements);
          break;
        case Source::SEV_SNP:
          r = std::make_shared<sev_snp::Attestation>(evidence, endorsements);
          break;
        case Source::OPEN_ENCLAVE:
          r = std::make_shared<oe::Attestation>(evidence, endorsements);
          break;
        case Source::ACI:
          r = std::make_shared<aci::Attestation>(evidence, endorsements, uvm_endorsements);
          break;
        default:
          throw std::runtime_error(
            "unsupported attestation source '" +
            std::to_string((unsigned)source) + "'");
          break;
      };

      return r;
    }
    catch (std::exception& ex)
    {
      throw std::runtime_error(
        fmt::format("attestation parsing failed: {}", ex.what()));
    }

    return nullptr;
  }

  std::shared_ptr<Attestation> parse_attestation(const std::string& json_string)
  {
    return parse(json::parse(json_string));
  }

  std::vector<uint8_t> Attestation::cbor()
  {
    auto aj = attestation_json(*this, false);
    return ravl::json::to_cbor(aj);
  }

  std::shared_ptr<Attestation> parse_attestation_cbor(
    const std::vector<uint8_t>& cbor)
  {
    return parse(ravl::json::from_cbor(cbor), false);
  }

  /*
    // Add binary encoding along these lines?

    std::shared_ptr<Attestation> parse_attestation_bin(
      const std::vector<uint8_t>& data)
    {
      size_t pos = 0;

      Source source = static_cast<Source>(get<uint8_t>(data, pos));

      size_t evidence_size = get<uint32_t>(data, pos);
      auto evidence = get_n(data, evidence_size, pos);

      size_t endorsements_size = get<uint32_t>(data, pos);
      auto endorsements = get_n(data, endorsements_size, pos);

      std::shared_ptr<Attestation> r = nullptr;

      switch (source)
      {
        case Source::SGX:
          r = std::make_shared<sgx::Attestation>(evidence, endorsements);
          break;
        case Source::SEV_SNP:
          r = std::make_shared<sev_snp::Attestation>(evidence, endorsements);
          break;
        case Source::OPEN_ENCLAVE:
          r = std::make_shared<oe::Attestation>(evidence, endorsements);
          break;
        default:
          throw std::runtime_error(
            "unsupported attestation source '" +
            std::to_string((unsigned)source) + "'");
          break;
      };

      return r;
    }
    */
}