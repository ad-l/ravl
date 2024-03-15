// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#pragma once

#include "crypto.h"
#include "http_client.h"
#include "json.h"
#include "sev_snp.h"
#include "aci.h"
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
      std::cout << "Calling SEV-SNP attestation verifier";
      return sev_snp::Attestation::verify(options, http_responses);
    }
  }
}