// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#pragma once

#include "crypto.h"
#include "http_client.h"
#include "json.h"
#include "sev_snp.h"
#include "aci.h"
#include "uvm_endorsements.h"
#include "util.h"
#include "visibility.h"

#include <span>

#define FMT_HEADER_ONLY
#include <fmt/format.h>

namespace ravl
{
  namespace aci
  {
    RAVL_VISIBILITY std::shared_ptr<ravl::Claims> Attestation::verify(
      const Options& options,
      const std::optional<std::vector<HTTPResponse>>& http_responses) const
    {
      auto generic_snp_claims = sev_snp::Attestation::verify(options, http_responses);
      
      auto snp_claims = std::dynamic_pointer_cast<sev_snp::Claims>(generic_snp_claims);
      std::vector<uint8_t> measurement(begin(snp_claims->measurement), end(snp_claims->measurement));
      verify_uvm_endorsements(uvm_endorsements, measurement);
      
      return snp_claims;
    }
  }
}