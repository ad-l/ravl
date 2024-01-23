// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include <chrono>
#include <emscripten.h>
#include <ostream>
#include <ravl/attestation.h>
#include <ravl/http_client.h>
#include <ravl/ravl.h>
#include <thread>
#include <emscripten/bind.h>

using namespace emscripten;
using namespace ravl;

std::string check(const std::string& a, Options* opt)
{
  std::vector<uint8_t> att(a.begin(), a.end());
  auto http_client = std::make_shared<AsynchronousHTTPClient>(0, 5, true);
  auto att_tracker = std::make_shared<AttestationRequestTracker>();
  auto att_without = parse_attestation_cbor(att);
  bool keep_waiting = true;
  std::shared_ptr<Claims> claims;

  auto id = att_tracker->submit(
    *opt,
    att_without,
    http_client,
    [att_tracker, &keep_waiting, &claims](
      AttestationRequestTracker::RequestID id) {
      claims = att_tracker->result(id);
      printf("without endorsements: %d\n", claims != nullptr);
      att_tracker->erase(id);
      keep_waiting = false;
    });

  printf("\nwaiting for verification result...\n");

  while (keep_waiting)
  {
    emscripten_sleep(20);
  }

  return claims->to_json();
}

EMSCRIPTEN_KEEPALIVE
std::string check_attestation(std::string att)
{
  printf("Checking attestation...\n");
  Options options = { .verbosity = 1, .certificate_verification = {.ignore_time = true}};
  try
  {
    auto claims = check(att, &options);
    return claims;
  }
  catch (const std::exception& ex)
  {
    printf("Exception: %s\n", ex.what());
    return "";
  }
  catch (...)
  {
    printf("Unknown exception caught.\n");
    return "";
  }
}

EMSCRIPTEN_KEEPALIVE
std::string check_partial_attestation(std::string att)
{
  printf("Checking partial attestation....\n");
  Options options = { .verbosity = 1, .certificate_verification = {.ignore_time = true}, .partial = 1};

  try
  {
    auto claims = check(att, &options);
    return claims;
  }
  catch (const std::exception& ex)
  {
    printf("Exception: %s\n", ex.what());
    return "";
  }
  catch (...)
  {
    printf("Unknown exception caught.\n");
    return "";
  }
}

EMSCRIPTEN_BINDINGS(my_module) {
    function("check_attestation", &check_attestation);
    function("check_partial_attestation", &check_partial_attestation);
}

