// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "ravl_sgx.h"

#include "crypto_wrappers.h"
#include "ravl_requests.h"
#include "ravl_util.h"

#include <dlfcn.h>
#include <iostream>
#include <nlohmann/json.hpp>
#include <sgx_quote_3.h>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_set>

#define FMT_HEADER_ONLY
#include <fmt/format.h>

#define SGX_QUOTE_VERSION 3

using namespace crypto::OpenSSL;

// All of this is inspired by Open Enclave's SGX verification, especially
// https://github.com/openenclave/openenclave/blob/master/common/sgx/quote.c

// Intel Provisioning Spec:
// https://api.portal.trustedservices.intel.com/documentation

// PCK Crl from cache:
// 'https://global.acccache.azure.net/sgx/certification/v3/pckcrl?uri=https%253a%252f%252fcertificates.trustedservices.intel.com%252fintelsgxpckprocessor.crl&clientid=production_client&api-version=2020-02-12-preview'.
// Root CA Crl from cache:
// 'https://global.acccache.azure.net/sgx/certification/v3/pckcrl?uri=https%253a%252f%252fcertificates.trustedservices.intel.com%252fintelsgxrootca.crl&clientid=production_client&api-version=2020-02-12-preview'.
// TCB Info from cache:
// 'https://global.acccache.azure.net/sgx/certification/v3/tcb?fmspc=00906ed50000&clientid=production_client&api-version=2018-10-01-preview'.
// QE Identity from cache:
// 'https://global.acccache.azure.net/sgx/certification/v3/qe/identity?clientid=production_client&api-version=2018-10-01-preview'

namespace ravl
{
  namespace sgx
  {
    static const std::string pck_cert_common_name = "Intel SGX PCK Certificate";

    static const std::string intel_root_public_key_pem =
      "-----BEGIN PUBLIC KEY-----\n"
      "MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEC6nEwMDIYZOj/iPWsCzaEKi71OiO\n"
      "SLRFhWGjbnBVJfVnkY4u3IjkDYYL0MxO4mqsyYjlBalTVYxFP2sJBK5zlA==\n"
      "-----END PUBLIC KEY-----\n";

    static const char* datetime_format = "%Y-%m-%dT%H:%M:%SZ";
    static const char* sgx_earliest_tcb_crl_date = "2017-03-17T00:00:00Z";

    class QL_QVE_Collateral // ~ sgx_ql_qve_collateral_t
    {
    public:
      QL_QVE_Collateral() {}

      QL_QVE_Collateral(const std::vector<uint8_t>& data)
      {
        std::vector<uint8_t>::size_type pos = 0, n = 0;

        major_version = get<uint16_t>(data, pos);
        minor_version = get<uint16_t>(data, pos);
        tee_type = get<uint32_t>(data, pos);

        n = get<uint64_t>(data, pos);
        pck_crl_issuer_chain = get_n(data, n, pos);

        n = get<uint64_t>(data, pos);
        root_ca_crl = get_n(data, n, pos);

        n = get<uint64_t>(data, pos);
        pck_crl = get_n(data, n, pos);

        n = get<uint64_t>(data, pos);
        tcb_info_issuer_chain = get_n(data, n, pos);

        n = get<uint64_t>(data, pos);
        tcb_info = get_n(data, n, pos);

        n = get<uint64_t>(data, pos);
        qe_identity_issuer_chain = get_n(data, n, pos);

        n = get<uint64_t>(data, pos);
        qe_identity = get_n(data, n, pos);

        // TODO: Investigate why there are sometimes extra null bytes
        // if (pos != data.size())
        //   throw std::runtime_error("excess collateral data");
      }

      uint16_t major_version;
      uint16_t minor_version;
      uint32_t tee_type;

      std::vector<uint8_t> pck_crl_issuer_chain;
      std::vector<uint8_t> root_ca_crl;
      std::vector<uint8_t> pck_crl;
      std::vector<uint8_t> tcb_info_issuer_chain;
      std::vector<uint8_t> tcb_info;
      std::vector<uint8_t> qe_identity_issuer_chain;
      std::vector<uint8_t> qe_identity;

      std::string to_string(uint32_t verbosity, size_t indent = 0) const
      {
        std::string ins(indent + 2, ' ');
        std::stringstream ss;

        ss << std::string(indent, ' ') << "- SGX Collateral" << std::endl;

        ss << ins
           << fmt::format("- Version: {}.{}", major_version, minor_version)
           << std::endl;
        ss << ins << fmt::format("- TEE type: 0x{:08x}", tee_type) << std::endl;

        if (verbosity > 0)
        {
          Unique_X509_CRL crl(root_ca_crl);
          ss << ins << "- Root CA CRL:" << std::endl;
          ss << crl.to_string(indent + 4) << std::endl;
        }
        if (verbosity > 1)
          ss << ins << fmt::format("  - PEM:\n{}", vec2str(root_ca_crl, 8))
             << std::endl;

        if (verbosity > 0)
        {
          Unique_STACK_OF_X509 st(pck_crl_issuer_chain);
          ss << ins << "- PCK CRL issuer chain:" << std::endl;
          ss << st.to_string_short(indent + 4) << std::endl;
        }
        if (verbosity > 1)
          ss << ins << "  - PEM:" << std::endl
             << vec2str(pck_crl_issuer_chain, 8) << std::endl;

        if (verbosity > 0)
        {
          Unique_X509_CRL crl(pck_crl);
          ss << ins << "- PCK CRL:" << std::endl;
          ss << crl.to_string(indent + 4) << std::endl;
        }
        if (verbosity > 1)
          ss << ins << "  - PEM:" << std::endl
             << vec2str(pck_crl, 8) << std::endl;

        if (verbosity > 0)
        {
          Unique_STACK_OF_X509 st(tcb_info_issuer_chain);
          ss << ins << "- TCB info issuer chain:" << std::endl;
          ss << st.to_string_short(indent + 4) << std::endl;
        }
        if (verbosity > 1)
          ss << ins << "  - PEM:" << std::endl
             << vec2str(tcb_info_issuer_chain, 8) << std::endl;

        if (verbosity > 1)
          ss << ins << fmt::format("- TCB info: {}", vec2str(tcb_info))
             << std::endl;

        if (verbosity > 0)
        {
          Unique_STACK_OF_X509 st(qe_identity_issuer_chain);
          ss << ins << "- QE identity issuer chain:" << std::endl;
          ss << st.to_string_short(indent + 4) << std::endl;
        }
        if (verbosity > 1)
          ss << ins << "  - PEM:" << std::endl
             << vec2str(qe_identity_issuer_chain, 8) << std::endl;

        if (verbosity > 0)
          ss << ins << fmt::format("- QE identity: {}", vec2str(qe_identity));
        return ss.str();
      }
    };

    static bool verify_signature(
      const Unique_EVP_PKEY& pkey,
      const std::span<const uint8_t>& message,
      const std::span<const uint8_t>& signature)
    {
      SHA256_CTX ctx;
      SHA256_Init(&ctx);

      SHA256_Update(&ctx, message.data(), message.size());

      std::vector<uint8_t> hash(ctx.md_len, 0);
      SHA256_Final(hash.data(), &ctx);

      auto signature_der = convert_signature_to_der(signature);

      Unique_EVP_PKEY_CTX pctx(pkey);
      CHECK1(EVP_PKEY_verify_init(pctx));
      int rc = EVP_PKEY_verify(
        pctx,
        signature_der.data(),
        signature_der.size(),
        hash.data(),
        hash.size());

      return rc == 1;
    }

    static bool verify_signature(
      const Unique_EC_KEY& eckey,
      const std::span<const uint8_t>& message,
      const std::span<const uint8_t>& signature)
    {
      return verify_signature(Unique_EVP_PKEY(eckey), message, signature);
    }

    static bool verify_signature(
      const std::span<const uint8_t>& public_key,
      const std::span<const uint8_t>& message,
      const std::span<const uint8_t>& signature)
    {
      return verify_signature(
        Unique_EC_KEY_P256(public_key), message, signature);
    }

    static bool verify_hash_match(
      const std::vector<std::span<const uint8_t>>& inputs,
      const std::span<const uint8_t>& expected)
    {
      SHA256_CTX sha256_ctx;
      CHECK1(SHA256_Init(&sha256_ctx));
      for (const auto& input : inputs)
        if (input.size() > 0)
          CHECK1(SHA256_Update(&sha256_ctx, input.data(), input.size()));
      std::vector<uint8_t> hash(sha256_ctx.md_len, 0);
      CHECK1(SHA256_Final(hash.data(), &sha256_ctx));
      if (hash.size() != expected.size())
        return false;
      for (size_t i = 0; i < hash.size(); i++)
        if (hash[i] != expected[i])
          return false;
      return true;
    }

    static const std::string root_ca_url =
      "https://certificates.trustedservices.intel.com/"
      "Intel_SGX_Provisioning_Certification_RootCA.pem";
    static const std::string root_crl_url =
      "https://certificates.trustedservices.intel.com/IntelSGXRootCA.crl";
    static const std::string api_base_url =
      "https://api.trustedservices.intel.com/sgx/certification/v3";
    static const std::string tcb_url = api_base_url + "/tcb";
    static const std::string pck_crl_url = api_base_url + "/pckcrl";
    static const std::string qe_identity_url = api_base_url + "/qe/identity";
    static const std::string qve_identity_url = api_base_url + "/qve/identity";

    std::vector<uint8_t> download_root_ca_pem(
      std::shared_ptr<RequestTracker> tracker = nullptr)
    {
      if (!tracker)
        tracker = std::make_shared<SynchronousRequestTracker>();

      std::vector<uint8_t> r;
      auto response = tracker->when_completed(
        {Request(root_ca_url)}, [&r](std::vector<Response>&& response_set) {
          if (response_set.size() != 1)
            return false;
          r = str2vec(response_set.at(0).body);
          return true;
        });

      if (r.empty())
        throw std::runtime_error("download of root CA certificate failed");

      return r;
    }

    std::shared_ptr<QL_QVE_Collateral> download_collateral(
      const std::string& ca,
      const std::string& fmspc,
      bool qve = false,
      std::shared_ptr<RequestTracker> tracker = nullptr)
    {
      auto r = std::make_shared<QL_QVE_Collateral>();

      r->major_version = 3;
      r->minor_version = 1;
      r->tee_type = 0;

      std::vector<Request> request_set;

      // Root CRL
      request_set.emplace_back(root_crl_url);

      // TCB info
      // https://api.portal.trustedservices.intel.com/documentation#pcs-tcb-info-v3
      request_set.emplace_back(tcb_url + "?fmspc=" + fmspc);

      // PCK CRL
      // https://api.portal.trustedservices.intel.com/documentation#pcs-revocation-v3
      request_set.emplace_back(pck_crl_url + "?ca=" + ca + "&encoding=pem");

      if (!qve)
      {
        // QE Identity
        // https://api.portal.trustedservices.intel.com/documentation#pcs-qe-identity-v3
        request_set.emplace_back(qe_identity_url);
      }
      else
      {
        // QVE Identity
        // https://api.portal.trustedservices.intel.com/documentation#pcs-qve-identity-v3
        request_set.emplace_back(qve_identity_url);
      }

      if (!tracker)
        tracker = std::make_shared<SynchronousRequestTracker>();

      bool tr = tracker->when_completed(
        std::move(request_set),
        [&r, qve](std::vector<Response>&& response_set) {
          if (response_set.size() != 4)
            return false;

          r->root_ca_crl = str2vec(response_set[0].body);

          r->tcb_info = str2vec(response_set[1].body);
          r->tcb_info_issuer_chain =
            response_set[1].get_header_data("SGX-TCB-Info-Issuer-Chain", true);

          r->pck_crl = str2vec(response_set[2].body);
          r->pck_crl_issuer_chain =
            response_set[2].get_header_data("SGX-PCK-CRL-Issuer-Chain", true);

          if (!qve)
          {
            auto response = response_set[3];
            r->qe_identity = str2vec(response.body);
            r->qe_identity_issuer_chain = response.get_header_data(
              "SGX-Enclave-Identity-Issuer-Chain", true);
          }
          else
          {
            auto response = response_set[3];
            r->qe_identity = str2vec(response.body);
            r->qe_identity_issuer_chain = response.get_header_data(
              "SGX-Enclave-Identity-Issuer-Chain", true);
          }

          return true;
        });

      if (!tr)
        throw std::runtime_error("collateral download request set failed");

      return r;
    }

    class CertificateExtension
    {
    public:
      const std::string sgx_ext_oid = "1.2.840.113741.1.13.1";
      const std::string sgx_ext_ppid_oid = sgx_ext_oid + ".1";
      const std::string sgx_ext_tcb_oid = sgx_ext_oid + ".2";
      const std::string sgx_ext_pceid_oid = sgx_ext_oid + ".3";
      const std::string sgx_ext_fmspc_oid = sgx_ext_oid + ".4";
      const std::string sgx_ext_type_oid = sgx_ext_oid + ".5";
      const std::string sgx_ext_platform_instance_oid = sgx_ext_oid + ".6";
      const std::string sgx_ext_configuration_oid = sgx_ext_oid + ".7";
      const std::string sgx_ext_configuration_dynamic_platform_oid =
        sgx_ext_configuration_oid + ".1";
      const std::string sgx_ext_configuration_cached_keys_oid =
        sgx_ext_configuration_oid + ".2";
      const std::string sgx_ext_configuration_smt_enabled_oid =
        sgx_ext_configuration_oid + ".3";

      CertificateExtension(const Unique_X509& certificate)
      {
        // See
        // https://api.trustedservices.intel.com/documents/Intel_SGX_PCK_Certificate_CRL_Spec-1.4.pdf

        static constexpr size_t processor_num_extensions = 5;
        static constexpr size_t platform_num_extensions = 7;
        static constexpr size_t platform_num_config_extensions = 3;

        auto sgx_ext = certificate.extension(sgx_ext_oid);

        if (!sgx_ext)
          throw std::runtime_error(
            "PCK certificate does not contain the SGX extension");

        Unique_ASN1_SEQUENCE seq(X509_EXTENSION_get_data(sgx_ext));

        int seq_sz = seq.size();
        if (
          seq_sz != processor_num_extensions &&
          seq_sz != platform_num_extensions)
          throw std::runtime_error(
            "SGX X509 extension sequence has invalid size");

        size_t i = 0;
        ppid = seq.get_octet_string(i++, sgx_ext_ppid_oid);
        tcb = get_tcb_ext(seq, i++, sgx_ext_tcb_oid);
        pceid = seq.get_octet_string(i++, sgx_ext_pceid_oid);
        fmspc = seq.get_octet_string(i++, sgx_ext_fmspc_oid);
        sgx_type = seq.get_enum(i++, sgx_ext_type_oid) != 0;

        if (seq_sz > processor_num_extensions)
        {
          platform_instance_id =
            seq.get_octet_string(i++, sgx_ext_platform_instance_oid);

          // Platform-CA certificates come with these extensions, but only
          // existence and order is verified here.
          auto config_seq = seq.get_seq(i++, sgx_ext_configuration_oid);
          if (config_seq.size() != platform_num_config_extensions)
            throw std::runtime_error(
              "SGX X509 extension configuration sequence has invalid size");

          size_t j = 0;
          auto dyn_platform = config_seq.get_bool(
            j++, sgx_ext_configuration_dynamic_platform_oid);
          auto cached_keys =
            config_seq.get_bool(j++, sgx_ext_configuration_cached_keys_oid);
          auto smt_enabled =
            config_seq.get_bool(j++, sgx_ext_configuration_smt_enabled_oid);

          configuration = CertificateExtension::Configuration{
            .dynamic_platform = dyn_platform,
            .cached_keys = cached_keys,
            .smt_enabled = smt_enabled};
        }
      }

      virtual ~CertificateExtension() = default;

      struct TCB
      {
        std::array<uint8_t, 16> comp_svn;
        uint16_t pce_svn;
        std::array<uint8_t, 16> cpu_svn;
      };

      struct Configuration
      {
        bool dynamic_platform;
        bool cached_keys;
        bool smt_enabled;
      };

      std::vector<uint8_t> ppid;
      TCB tcb;
      std::vector<uint8_t> pceid;
      std::vector<uint8_t> fmspc;
      uint8_t sgx_type;

      std::optional<std::vector<uint8_t>> platform_instance_id = std::nullopt;
      std::optional<Configuration> configuration = std::nullopt;

    protected:
      TCB get_tcb_ext(
        const Unique_ASN1_SEQUENCE& seq,
        int index,
        const std::string& expected_oid)
      {
        static constexpr size_t x509_tcb_seq_size = 18;

        TCB r;

        auto sss = seq.get_seq(index, expected_oid);

        int n = sss.size();
        if (n != x509_tcb_seq_size)
          throw std::runtime_error(
            "SGX X509 TCB extension: sequence of invalid length");

        size_t num_comp_svns = r.comp_svn.size();

        for (int i = 0; i < n; i++)
        {
          std::string expected_oid_i =
            std::string(sgx_ext_tcb_oid) + "." + std::to_string(i + 1);

          if (i < num_comp_svns)
            r.comp_svn[i] = sss.get_uint8(i, expected_oid_i);
          else if (i == num_comp_svns)
            r.pce_svn = sss.get_uint16(i, expected_oid_i);
          else if (i == x509_tcb_seq_size - 1)
          {
            auto t = sss.get_octet_string(i, expected_oid_i);
            if (t.size() != r.cpu_svn.size())
              throw std::runtime_error(
                "SGX X509 TCB extension: ASN.1 octet string of invalid size");
            for (size_t i = 0; i < r.cpu_svn.size(); i++)
              r.cpu_svn.at(i) = t.at(i);
          }
          else
            throw std::runtime_error("unreachable");
        }

        return r;
      }
    };

    static bool has_intel_public_key(const Unique_X509& certificate)
    {
      Unique_EVP_PKEY pubkey(certificate);
      Unique_BIO bio(intel_root_public_key_pem);
      Unique_EVP_PKEY intel_pubkey(bio, true);
      return pubkey == intel_pubkey;
    }

    static bool json_vector_eq(
      const nlohmann::json& tcbinfo_j,
      const std::string& key,
      const std::vector<uint8_t>& ref,
      bool optional = false)
    {
      auto vj = tcbinfo_j[key];
      if (vj.is_null())
      {
        if (optional)
          return true;
        else
          throw std::runtime_error("missing json object");
      }

      auto vv = vj.get<std::string>();
      return from_hex(vv) == ref;
    }

    struct TCBLevel
    {
      std::array<uint8_t, 16> comp_svn = {0};
      uint16_t pce_svn = 0;
      std::string status = "";
      std::string date = "";
      std::vector<std::string> advisory_ids = {};
    };

    static void check_datetime(
      const std::string& date_s, const std::string& name)
    {
      auto earliest_permitted =
        parse_time_point(sgx_earliest_tcb_crl_date, datetime_format);
      auto issue_timepoint = parse_time_point(date_s, datetime_format);
      if (issue_timepoint < earliest_permitted)
        throw std::runtime_error(name + " earlier than permitted");
    }

    TCBLevel verify_tcb_json(
      const std::span<uint8_t>& tcb_info,
      const CertificateExtension& pck_ext,
      const Unique_EVP_PKEY& signer_pubkey)
    {
      TCBLevel platform_tcb_level = {0};

      std::vector<uint8_t> signature;

      std::string tcb_info_s(
        tcb_info.data(), tcb_info.data() + tcb_info.size());

      try
      {
        auto col_tcb_info_j = nlohmann::json::parse(tcb_info_s);
        auto tcbinfo_j = col_tcb_info_j["tcbInfo"];

        if (
          tcbinfo_j.find("version") == tcbinfo_j.end() ||
          tcbinfo_j["version"].get<uint32_t>() != 2)
          throw std::runtime_error("unsupported tcbInfo version");

        auto id = tcbinfo_j["issueDate"].get<std::string>();
        check_datetime(id, "TCB issue date");
        auto nu = tcbinfo_j["nextUpdate"].get<std::string>();
        check_datetime(nu, "TCB next update");

        // TODO: advisory IDs?

        if (!json_vector_eq(tcbinfo_j, "fmspc", pck_ext.fmspc))
          throw std::runtime_error("fmspc mismatch");

        if (!json_vector_eq(tcbinfo_j, "pceId", pck_ext.pceid))
          throw std::runtime_error("pceid mismatch");

        uint64_t tcb_type = tcbinfo_j["tcbType"].get<uint64_t>();
        if (tcb_type != 0)
          throw std::runtime_error("tcbType not supported");

        // This is the TCB recovery event number, monotinically increasing.
        // Report as result?
        uint64_t tcb_eval_data_number =
          tcbinfo_j["tcbEvaluationDataNumber"].get<uint64_t>();

        for (const auto& tcb_level_j : tcbinfo_j["tcbLevels"])
        {
          std::string tcb_date = tcb_level_j["tcbDate"].get<std::string>();
          std::string tcb_status = tcb_level_j["tcbStatus"].get<std::string>();
          const auto& tcb = tcb_level_j["tcb"];

          size_t comp_svn_size = pck_ext.tcb.comp_svn.size();
          if (comp_svn_size != 16)
            throw std::runtime_error("unexpected comp_svn size");

          std::array<uint8_t, 16> tcb_level_comp_svn;
          for (size_t i = 0; i < comp_svn_size; i++)
          {
            std::string svn_name = fmt::format("sgxtcbcomp{:02d}svn", i + 1);
            tcb_level_comp_svn[i] = tcb[svn_name].get<uint8_t>();
          }
          uint16_t tcb_level_pce_svn = tcb["pcesvn"].get<uint16_t>();

          // optional advisoryIDs?

          if (platform_tcb_level.status.empty())
          {
            // See
            // https://github.com/openenclave/openenclave/blob/master/common/sgx/tcbinfo.c#L398
            // "Choose the first tcb level for which all of the platform's comp
            // svn values and pcesvn values are greater than or equal to
            // corresponding values of the tcb level."
            bool good = true;
            for (size_t i = 0; i < comp_svn_size && good; i++)
              good = good && pck_ext.tcb.comp_svn[i] >= tcb_level_comp_svn[i];
            good = good && pck_ext.tcb.pce_svn >= tcb_level_pce_svn;
            if (good)
            {
              platform_tcb_level = {
                tcb_level_comp_svn,
                tcb_level_pce_svn,
                tcb_status,
                tcb_date,
                std::vector<std::string>()};
            }
          }
        }

        if (platform_tcb_level.status.empty())
          throw std::runtime_error("no matching TCB level found");

        auto sig_j = col_tcb_info_j["signature"];
        signature = from_hex(sig_j.get<std::string>());
      }
      catch (const std::exception& ex)
      {
        throw std::runtime_error(
          std::string("incorrectly formatted SGX TCB: ") + ex.what());
      }
      catch (...)
      {
        throw std::runtime_error("incorrectly formatted SGX TCB");
      }

      // find the part of the json that was signed
      static const std::string pre = "{\"tcbInfo\":";
      static const std::string post = ",\"signature\"";

      auto l = tcb_info_s.find(pre);
      auto r = tcb_info_s.rfind(post);
      if (l == std::string::npos || r == std::string::npos)
        throw std::runtime_error("tcbInfo does not contain signature");

      std::span signed_msg = {
        (uint8_t*)tcb_info_s.data() + l + pre.size(),
        (uint8_t*)tcb_info_s.data() + r};

      if (!verify_signature(signer_pubkey, signed_msg, signature))
        throw std::runtime_error("tcbInfo signature verification failed");

      return platform_tcb_level;
    }

    TCBLevel verify_tcb(
      const std::span<uint8_t>& tcb_info_issuer_chain,
      const std::span<uint8_t>& tcb_info,
      const CertificateExtension& pck_ext,
      const Unique_X509_STORE& store,
      const Options& options,
      size_t indent = 0)
    {
      if (options.verbosity > 0)
      {
        std::string ins(indent, ' ');
        log(ins + "- TCB info verification");
        log(ins + "  - TCB info issuer certificate chain verification");
      }
      auto tcb_issuer_chain = verify_certificate_chain(
        tcb_info_issuer_chain,
        store,
        options.certificate_validation,
        false,
        options.verbosity > 0,
        indent + 4);

      auto tcb_issuer_leaf = tcb_issuer_chain.front();
      auto tcb_issuer_root = tcb_issuer_chain.back();

      Unique_EVP_PKEY tcb_issuer_leaf_pubkey(tcb_issuer_leaf);

      if (!has_intel_public_key(tcb_issuer_root))
        throw std::runtime_error(
          "TCB issuer root certificate does not use the expected Intel SGX "
          "public key");

      return verify_tcb_json(tcb_info, pck_ext, tcb_issuer_leaf_pubkey);
    }

    bool verify_qe_id(
      const std::span<const uint8_t>& qe_identity_issuer_chain,
      const std::span<const uint8_t>& qe_identity,
      const std::span<const uint8_t>& qe_report_body_s,
      const TCBLevel& platform_tcb_level,
      const CertificateExtension& pck_ext,
      const Unique_X509_STORE& store,
      const Options& options,
      size_t indent = 0)
    {
      const sgx_report_body_t& qe_report_body =
        *(sgx_report_body_t*)qe_report_body_s.data();

      if (options.verbosity > 0)
      {
        std::string ins(indent, ' ');
        log(ins + "- QE identity verification");
        log(ins + "  - QE identity issuer certificate chain verification");
      }
      auto qe_id_issuer_chain = verify_certificate_chain(
        qe_identity_issuer_chain,
        store,
        options.certificate_validation,
        false,
        options.verbosity > 0,
        indent + 4);

      auto qe_id_issuer_leaf = qe_id_issuer_chain.at(0);
      auto qe_id_issuer_root =
        qe_id_issuer_chain.at(qe_id_issuer_chain.size() - 1);

      Unique_EVP_PKEY qe_id_issuer_leaf_pubkey(qe_id_issuer_leaf);

      if (!has_intel_public_key(qe_id_issuer_root))
        throw std::runtime_error(
          "QE identity issuer root certificate does not use the expected Intel "
          "SGX public key");

      std::string qe_identity_s = {
        (char*)qe_identity.data(), qe_identity.size()};
      std::vector<uint8_t> signature;

      try
      {
        std::string qe_tcb_level_status = "";
        std::string qe_tcb_date = "";
        uint16_t qe_tcb_level_isv_svn = 0;

        auto qe_id_j = nlohmann::json::parse(qe_identity_s);
        auto enclave_identity = qe_id_j["enclaveIdentity"];

        auto version = enclave_identity["version"].get<uint64_t>();
        if (version != 2)
          throw std::runtime_error("enclaveIdentity version not supported");

        auto eid_id = enclave_identity["id"].get<std::string>();
        if (eid_id != "QE" && eid_id != "QVE")
          throw std::runtime_error("QE identity type not supported");

        for (const auto& tcb_level : enclave_identity["tcbLevels"])
        {
          auto tcb_j = tcb_level["tcb"];
          uint16_t tcb_level_isv_svn = tcb_j["isvsvn"].get<uint16_t>();
          auto tcb_date = tcb_level["tcbDate"];
          auto tcb_status = tcb_level["tcbStatus"].get<std::string>();

          if (qe_tcb_level_status.empty())
          {
            // See
            // https://github.com/openenclave/openenclave/blob/master/common/sgx/tcbinfo.c#L1023
            // "Choose the first tcb level for which all of the platform's isv
            // svn values are greater than or equal to corresponding values of
            // the tcb level."
            if (qe_report_body.isv_svn >= tcb_level_isv_svn)
            {
              qe_tcb_level_status = tcb_status;
              qe_tcb_date = tcb_date;
              qe_tcb_level_isv_svn = tcb_level_isv_svn;

              // TODO: optional advisories?
            }
          }
        }

        if (qe_tcb_level_status.empty())
          throw std::runtime_error("no matching QE TCB level found");

        auto id = enclave_identity["issueDate"].get<std::string>();
        check_datetime(id, "QE TCB issue date");
        auto nu = enclave_identity["nextUpdate"].get<std::string>();
        check_datetime(nu, "QE TCB next update");

        std::vector<uint8_t> reported_mrsigner = {
          &qe_report_body.mr_signer.m[0],
          &qe_report_body.mr_signer.m[0] + sizeof(qe_report_body.mr_signer.m)};

        if (
          from_hex(enclave_identity["mrsigner"].get<std::string>()) !=
          reported_mrsigner)
          throw std::runtime_error("QE mrsigner mismatch");

        if (
          enclave_identity["isvprodid"].get<uint16_t>() !=
          qe_report_body.isv_prod_id)
          throw std::runtime_error("QE isv prod id mismatch");

        if (qe_tcb_level_isv_svn >= qe_report_body.isv_svn)
          throw std::runtime_error("QE isv svn too small");

        uint32_t msel_mask = from_hex_t<uint32_t>(
          enclave_identity["miscselectMask"].get<std::string>());
        uint32_t msel = from_hex_t<uint32_t>(
          enclave_identity["miscselect"].get<std::string>());
        if ((qe_report_body.misc_select & msel_mask) != msel)
          throw std::runtime_error("misc select mismatch");

        auto attribute_flags_xfrm_s =
          enclave_identity["attributes"].get<std::string>();
        auto attribute_flags_xfrm_mask_s =
          enclave_identity["attributesMask"].get<std::string>();

        if (
          attribute_flags_xfrm_s.size() != 32 ||
          attribute_flags_xfrm_mask_s.size() != 32)
          throw std::runtime_error("unexpected attribute value sizes");

        auto flags_s = attribute_flags_xfrm_s.substr(0, 16);
        auto xfrm_s = attribute_flags_xfrm_s.substr(16);
        auto flags_mask_s = attribute_flags_xfrm_mask_s.substr(0, 16);
        auto xfrm_mask_s = attribute_flags_xfrm_mask_s.substr(16);

        uint64_t flags = from_hex_t<uint64_t>(flags_s);
        uint64_t xfrm = from_hex_t<uint64_t>(xfrm_s);
        uint64_t flags_mask = from_hex_t<uint64_t>(flags_mask_s);
        uint64_t xfrm_mask = from_hex_t<uint64_t>(xfrm_mask_s);

        if ((qe_report_body.attributes.flags & flags_mask) != flags)
          throw std::runtime_error("attribute flags mismatch");

        if ((qe_report_body.attributes.xfrm & xfrm_mask) != xfrm)
          throw std::runtime_error("attribute xfrm mismatch");

        if (qe_report_body.attributes.flags & SGX_FLAGS_DEBUG)
          throw std::runtime_error("report purported to be from debug QE");

        auto sig_j = qe_id_j["signature"];
        signature = from_hex(sig_j.get<std::string>());
      }
      catch (const std::exception& ex)
      {
        throw std::runtime_error(
          std::string("incorrectly formatted SGX QE ID: ") + ex.what());
      }
      catch (...)
      {
        throw std::runtime_error("incorrectly formatted SGX QE ID");
      }

      // find the part of the json that was signed
      static const std::string& pre = "\"enclaveIdentity\":";
      static const std::string& post = ",\"signature\":\"";

      auto l = qe_identity_s.find(pre);
      auto r = qe_identity_s.rfind(post);
      if (l == std::string::npos || r == std::string::npos)
        throw std::runtime_error("QE identity does not contain signature");

      std::span signed_msg = {
        (uint8_t*)qe_identity_s.data() + l + pre.size(),
        (uint8_t*)qe_identity_s.data() + r};

      if (!verify_signature(qe_id_issuer_leaf_pubkey, signed_msg, signature))
        throw std::runtime_error("QE identity signature verification failed");

      return true;
    }

    std::span<const uint8_t> parse_quote(const Attestation& a)
    {
      static constexpr size_t sgx_quote_t_signed_size =
        sizeof(sgx_quote_t) - sizeof(uint32_t); // (minus signature_len)

      // TODO: Endianness, e.g. for sizes?

      const sgx_quote_t* quote = (sgx_quote_t*)a.evidence.data();

      if (a.evidence.size() < (sizeof(sgx_quote_t) + quote->signature_len))
        throw std::runtime_error(
          "Unknown evidence format: too small to contain an sgx_quote_t");

      std::span r = {(uint8_t*)quote, sgx_quote_t_signed_size};
      verify_within(r, a.evidence);

      if (quote->version != SGX_QUOTE_VERSION)
        throw std::runtime_error(
          "Unknown evidence format: unsupported quote version");

      if (quote->sign_type != SGX_QL_ALG_ECDSA_P256)
        throw std::runtime_error(
          "Unknown evidence format: unsupported signing type");

      // if (a.evidence.size() > (sizeof(sgx_quote_t) + quote->signature_len))
      //   throw std::runtime_error(
      //     "Unsupported evidence format: excess evidence data");

      return r;
    }

    class SignatureData // ~ _sgx_ql_ecdsa_sig_data_t
    {
    public:
      SignatureData(const std::span<const uint8_t>& quote, const Attestation& a)
      {
        const sgx_ql_ecdsa_sig_data_t* sig_data =
          (sgx_ql_ecdsa_sig_data_t*)((const sgx_quote_t*)quote.data())
            ->signature;

        if (sig_data == NULL)
          throw std::runtime_error("missing signature data");

        std::span sig_data_span = {(uint8_t*)sig_data, sizeof(*sig_data)};
        verify_within(sig_data_span, a.evidence);

        report = {(uint8_t*)&sig_data->qe_report, sizeof(sig_data->qe_report)};
        verify_within(report, a.evidence);

        report_signature = {
          sig_data->qe_report_sig, sizeof(sig_data->qe_report_sig)};
        verify_within(report_signature, a.evidence);

        quote_signature = {sig_data->sig, sizeof(sig_data->sig)};
        verify_within(quote_signature, a.evidence);

        public_key = {
          sig_data->attest_pub_key, sizeof(sig_data->attest_pub_key)};
        verify_within(public_key, a.evidence);

        report_data = {
          (uint8_t*)&sig_data->qe_report.report_data,
          sizeof(sig_data->qe_report.report_data)};
        verify_within(report_data, a.evidence);

        const sgx_ql_auth_data_t* ad_raw =
          (sgx_ql_auth_data_t*)sig_data->auth_certification_data;

        auth_data = {ad_raw->auth_data, ad_raw->size};
        verify_within(auth_data, a.evidence);

        if (ad_raw == NULL || ad_raw->size == 0)
          throw std::runtime_error("missing authentication data");

        const sgx_ql_certification_data_t* cd_raw =
        (sgx_ql_certification_data_t*)(sig_data->auth_certification_data + sizeof(sgx_ql_auth_data_t) + ad_raw->size);

        certification_data = {cd_raw->certification_data, cd_raw->size};
        verify_within(certification_data, a.evidence);

        if (cd_raw == NULL || cd_raw->size == 0)
          throw std::runtime_error("missing certification data");

        if (cd_raw->cert_key_type != PCK_CERT_CHAIN)
          throw std::runtime_error("unsupported certification data key type");
      }

      ~SignatureData() = default;

      std::span<const uint8_t> quote_signature;
      std::span<const uint8_t> public_key;
      std::span<const uint8_t> report;
      std::span<const uint8_t> report_signature;
      std::span<const uint8_t> report_data;
      std::span<const uint8_t> auth_data;
      std::span<const uint8_t> certification_data;
    };

    bool verify(
      const Attestation& a,
      const Options& options,
      std::shared_ptr<RequestTracker> tracker)
    {
      std::span quote = parse_quote(a);
      SignatureData signature_data(quote, a);

      Unique_X509_STORE store;
      std::shared_ptr<QL_QVE_Collateral> collateral = nullptr;
      std::vector<uint8_t> root_ca_pem = {};

      if (!a.endorsements.empty() && !options.fresh_endorsements)
      {
        collateral = std::make_shared<QL_QVE_Collateral>(a.endorsements);

        if (options.root_ca_certificate_pem)
          root_ca_pem = *options.root_ca_certificate_pem;
        else if (options.fresh_root_ca_certificate)
          root_ca_pem = download_root_ca_pem(tracker);
      }
      else
      {
        // Get X509 extensions from the PCK cert to find CA type and fmspc. The
        // cert chain is still unverified at this point.
        auto pck_pem = extract_pem(signature_data.certification_data);
        Unique_X509 pck_leaf(Unique_BIO(pck_pem), true);
        CertificateExtension pck_ext(pck_leaf);

        bool have_pid = pck_ext.platform_instance_id &&
          !is_all_zero(*pck_ext.platform_instance_id);
        auto ca_type = have_pid ? "platform" : "processor";
        auto fmspc_hex = fmt::format("{:02x}", fmt::join(pck_ext.fmspc, ""));
        collateral = download_collateral(ca_type, fmspc_hex, false, tracker);

        if (options.root_ca_certificate_pem)
          root_ca_pem = *options.root_ca_certificate_pem;
        else
          root_ca_pem = download_root_ca_pem(tracker);
      }

      if (options.verbosity > 0)
        log(collateral->to_string(options.verbosity, 2));

      // These flags also check that we have a CRL for each CA.
      store.set_flags(X509_V_FLAG_CRL_CHECK | X509_V_FLAG_CRL_CHECK_ALL);
      store.add_crl(collateral->root_ca_crl);
      store.add_crl(collateral->pck_crl);

      bool trusted_root = false;

      if (!root_ca_pem.empty())
        store.add(root_ca_pem);
      else
        trusted_root = true;

      // Validate PCK certificate and it's issuer chain. We trust the root CA
      // certificate in the endorsements if no other one is provided, but check
      // that it has Intel's public key afterwards.
      if (options.verbosity > 0)
        log("  - PCK CRL issuer certificate chain verification");
      auto pck_crl_issuer_chain = verify_certificate_chain(
        collateral->pck_crl_issuer_chain,
        store,
        options.certificate_validation,
        trusted_root,
        options.verbosity > 0,
        4);

      if (options.verbosity > 0)
      {
        if (trusted_root)
        {
          log("  - Root CA Certificate (auto-trusted):");
          log(pck_crl_issuer_chain.back().to_string_short(4));
        }
        else
        {
          Unique_X509 root(root_ca_pem, true);
          log("  - Root CA Certificate:");
          log(root.to_string_short(4));
        }
      }
      if (options.verbosity > 1)
      {
        log("    - PEM:");
        if (trusted_root)
        {
          std::string rs = pck_crl_issuer_chain.back().to_string();
          indentate(rs, 6);
          log(rs);
        }
        else
          log(vec2str(root_ca_pem, 6));
      }

      if (options.verbosity > 0)
        log("  - PCK certificate chain verification");
      auto pck_cert_chain = verify_certificate_chain(
        signature_data.certification_data,
        store,
        options.certificate_validation,
        trusted_root,
        options.verbosity > 0,
        4);

      auto pck_leaf = pck_cert_chain.front();
      auto pck_root = pck_cert_chain.back();

      if (!pck_leaf.has_common_name(pck_cert_common_name))
        throw std::runtime_error(
          "PCK certificate does not have expected common name");

      if (!has_intel_public_key(pck_root))
        throw std::runtime_error(
          "root certificate does not have the expected Intel SGX public key");

      if (!pck_root.is_ca())
        throw std::runtime_error("root certificate is not from a CA");

      // Verify QE and quote signatures and the authentication hash
      Unique_EVP_PKEY qe_leaf_pubkey(pck_leaf);

      bool qe_sig_ok = verify_signature(
        qe_leaf_pubkey, signature_data.report, signature_data.report_signature);
      if (!qe_sig_ok)
        throw std::runtime_error("QE signature verification failed");

      bool quote_sig_ok = verify_signature(
        signature_data.public_key, quote, signature_data.quote_signature);
      if (!quote_sig_ok)
        throw std::runtime_error("quote signature verification failed");

      // TODO: This hash check seems open-enclave specific?
      bool pk_auth_hash_matches = verify_hash_match(
        {signature_data.public_key, signature_data.auth_data},
        signature_data.report_data.subspan(0, 32));
      if (!pk_auth_hash_matches)
        throw std::runtime_error("QE authentication message hash mismatch");

      // Verify TCB info
      CertificateExtension pck_x509_ext(pck_leaf);
      auto platform_tcb_level = verify_tcb(
        collateral->tcb_info_issuer_chain,
        collateral->tcb_info,
        pck_x509_ext,
        store,
        options,
        2);

      // Verify the QE identity
      bool qe_id_ok = verify_qe_id(
        collateral->qe_identity_issuer_chain,
        collateral->qe_identity,
        signature_data.report,
        platform_tcb_level,
        pck_x509_ext,
        store,
        options,
        2);

      return pck_cert_chain && qe_sig_ok && pk_auth_hash_matches &&
        quote_sig_ok && qe_id_ok;
    }
  }
}