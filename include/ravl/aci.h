// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#pragma once

#include "attestation.h"

#include <array>
#include <memory>
#include <optional>

namespace ravl
{
  class HTTPClient;

  namespace aci
  {
    struct UvmEndorsements
    {
    };

    class Claims : public ravl::sev_snp::Claims
    {
    public:
      Claims() : ravl::sev_snp::Claims() {}

      virtual ~Claims() = default;

      UvmEndorsements uvm_endorsements;
    };

    class Attestation : public ravl::sev_snp::Attestation 
    {
    public:
      /// UVM endorsements
      std::vector<uint8_t> uvm_endorsements;

      Attestation(
        const std::vector<uint8_t>& evidence_,
        const std::vector<uint8_t>& endorsements_,
        const std::vector<uint8_t>& uvm_endorsements_) :
        ravl::sev_snp::Attestation(Source::ACI, evidence_, endorsements_)
      {
        uvm_endorsements = uvm_endorsements_;
      }

      virtual ~Attestation() = default;

      virtual std::shared_ptr<ravl::Claims> verify(
        const Options& options = {},
        const std::optional<HTTPResponses>& http_responses = {}) const override;
    };
  }
}
