// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "ravl_sev_snp.h"

#include "ravl.h"
#include "ravl_crypto.h"
#include "ravl_crypto_openssl.h"
#include "ravl_requests.h"

#include <stdexcept>

#define FMT_HEADER_ONLY
#include <fmt/format.h>

using namespace ravl::crypto;

namespace ravl
{
  namespace sev_snp
  {
    // Based on the SEV-SNP ABI Spec document at
    // https://www.amd.com/system/files/TechDocs/56860.pdf

    // ARK = AMD Root Key
    // ASK = AMD SEV Signing Key (intermediate)
    // VCEK = Versioned Chip Endorsement Key (VCEK) (leaf)

    static const std::string kds_url = "https://kdsintf.amd.com";

    static constexpr size_t attestation_report_data_size = 64;
    using attestation_report_data =
      std::array<uint8_t, attestation_report_data_size>;
    static constexpr size_t attestation_measurement_size = 48;
    using attestation_measurement =
      std::array<uint8_t, attestation_measurement_size>;

    namespace snp
    {

      // From https://developer.amd.com/sev/
      static const std::string amd_milan_root_signing_public_key =
        R"(-----BEGIN PUBLIC KEY-----
MIICIjANBgkqhkiG9w0BAQEFAAOCAg8AMIICCgKCAgEA0Ld52RJOdeiJlqK2JdsV
mD7FktuotWwX1fNgW41XY9Xz1HEhSUmhLz9Cu9DHRlvgJSNxbeYYsnJfvyjx1MfU
0V5tkKiU1EesNFta1kTA0szNisdYc9isqk7mXT5+KfGRbfc4V/9zRIcE8jlHN61S
1ju8X93+6dxDUrG2SzxqJ4BhqyYmUDruPXJSX4vUc01P7j98MpqOS95rORdGHeI5
2Naz5m2B+O+vjsC060d37jY9LFeuOP4Meri8qgfi2S5kKqg/aF6aPtuAZQVR7u3K
FYXP59XmJgtcog05gmI0T/OitLhuzVvpZcLph0odh/1IPXqx3+MnjD97A7fXpqGd
/y8KxX7jksTEzAOgbKAeam3lm+3yKIcTYMlsRMXPcjNbIvmsBykD//xSniusuHBk
gnlENEWx1UcbQQrs+gVDkuVPhsnzIRNgYvM48Y+7LGiJYnrmE8xcrexekBxrva2V
9TJQqnN3Q53kt5viQi3+gCfmkwC0F0tirIZbLkXPrPwzZ0M9eNxhIySb2npJfgnq
z55I0u33wh4r0ZNQeTGfw03MBUtyuzGesGkcw+loqMaq1qR4tjGbPYxCvpCq7+Og
pCCoMNit2uLo9M18fHz10lOMT8nWAUvRZFzteXCm+7PHdYPlmQwUw3LvenJ/ILXo
QPHfbkH0CyPfhl1jWhJFZasCAwEAAQ==
-----END PUBLIC KEY-----
)";

      // Table 3
#pragma pack(push, 1)
      struct TcbVersion
      {
        uint8_t boot_loader;
        uint8_t tee;
        uint8_t reserved[4];
        uint8_t snp;
        uint8_t microcode;
      };
#pragma pack(pop)
      static_assert(
        sizeof(TcbVersion) == sizeof(uint64_t),
        "Can't cast TcbVersion to uint64_t");

#pragma pack(push, 1)
      struct Signature
      {
        uint8_t r[72];
        uint8_t s[72];
        uint8_t reserved[512 - 144];
      };
#pragma pack(pop)

      // Table. 105
      enum class SignatureAlgorithm : uint32_t
      {
        invalid = 0,
        ecdsa_p384_sha384 = 1
      };

#pragma pack(push, 1)
      // Table 21
      struct Attestation
      {
        uint32_t version; /* 0x000 */
        uint32_t guest_svn; /* 0x004 */
        uint64_t policy; /* 0x008 */
        uint8_t family_id[16]; /* 0x010 */
        uint8_t image_id[16]; /* 0x020 */
        uint32_t vmpl; /* 0x030 */
        SignatureAlgorithm signature_algo; /* 0x034 */
        struct TcbVersion platform_version; /* 0x038 */
        uint64_t platform_info; /* 0x040 */
        uint32_t flags; /* 0x048 */
        uint32_t reserved0; /* 0x04C */
        uint8_t report_data[attestation_report_data_size]; /* 0x050 */
        uint8_t measurement[attestation_measurement_size]; /* 0x090 */
        uint8_t host_data[32]; /* 0x0C0 */
        uint8_t id_key_digest[48]; /* 0x0E0 */
        uint8_t author_key_digest[48]; /* 0x110 */
        uint8_t report_id[32]; /* 0x140 */
        uint8_t report_id_ma[32]; /* 0x160 */
        struct TcbVersion reported_tcb; /* 0x180 */
        uint8_t reserved1[24]; /* 0x188 */
        uint8_t chip_id[64]; /* 0x1A0 */
        struct TcbVersion committed_tcb; /* 0x1E0 */
        uint8_t current_minor; /* 0x1E8 */
        uint8_t current_build; /* 0x1E9 */
        uint8_t current_major; /* 0x1EA */
        uint8_t reserved2; /* 0x1EB */
        uint8_t committed_build; /* 0x1EC */
        uint8_t committed_minor; /* 0x1ED */
        uint8_t committed_major; /* 0x1EE */
        uint8_t reserved3; /* 0x1EF */
        struct TcbVersion launch_tcb; /* 0x1F0 */
        uint8_t reserved4[168]; /* 0x1F8 */
        struct Signature signature; /* 0x2A0 */
      };
#pragma pack(pop)

      // Table 20
      struct AttestationReq
      {
        uint8_t report_data[attestation_report_data_size];
        uint32_t vmpl;
        uint8_t reserved[28];
      };

      // Table 23
#pragma pack(push, 1)
      struct AttestationResp
      {
        uint32_t status;
        uint32_t report_size;
        uint8_t reserved[0x20 - 0x8];
        struct Attestation report;
        uint8_t padding[64];
        // padding to the size of SEV_SNP_REPORT_RSP_BUF_SZ (i.e., 1280 bytes)
      };
#pragma pack(pop)

      struct GuestRequest
      {
        uint8_t req_msg_type;
        uint8_t rsp_msg_type;
        uint8_t msg_version;
        uint16_t request_len;
        uint64_t request_uaddr;
        uint16_t response_len;
        uint64_t response_uaddr;
        uint32_t error; /* firmware error code on failure (see psp-sev.h) */
      };

      // Table 99
      enum MsgType
      {
        MSG_TYPE_INVALID = 0,
        MSG_CPUID_REQ,
        MSG_CPUID_RSP,
        MSG_KEY_REQ,
        MSG_KEY_RSP,
        MSG_REPORT_REQ,
        MSG_REPORT_RSP,
        MSG_EXPORT_REQ,
        MSG_EXPORT_RSP,
        MSG_IMPORT_REQ,
        MSG_IMPORT_RSP,
        MSG_ABSORB_REQ,
        MSG_ABSORB_RSP,
        MSG_VMRK_REQ,
        MSG_VMRK_RSP,
        MSG_TYPE_MAX
      };

      // Changes on 5.19+ kernel
      constexpr auto DEVICE = "/dev/sev";
    }

#define SEV_GUEST_IOC_TYPE 'S'
#define SEV_SNP_GUEST_MSG_REPORT \
  _IOWR(SEV_GUEST_IOC_TYPE, 0x1, struct snp::GuestRequest)

    std::string download_root_ca_pem(
      const std::string& product_name,
      std::shared_ptr<RequestTracker> tracker = nullptr)
    {
      if (!tracker)
        tracker = std::make_shared<SynchronousRequestTracker>();

      std::string r;

      std::vector<Request> request_set;

      auto vcek_issuer_chain_url =
        fmt::format("{}/vcek/v1/{}/cert_chain", kds_url, product_name);

      request_set.emplace_back(vcek_issuer_chain_url);

      if (!tracker)
        tracker = std::make_shared<SynchronousRequestTracker>();

      bool tr = tracker->when_completed(
        std::move(request_set), [&r](std::vector<Response>&& response_set) {
          if (response_set.size() != 1)
            return false;
          auto issuer_chain = response_set[0].body;
          Unique_STACK_OF_X509 stack(issuer_chain);
          if (stack.size() != 2)
            return false;
          r = stack.at(1).pem();
          return true;
        });

      if (!tr)
        throw std::runtime_error("endorsement download request set failed");

      return r;
    }

    struct EndorsementsEtc
    {
      std::string root_ca_certificate;
      std::string vcek_certificate_chain;
      std::string vcek_issuer_chain_crl;
    };

    EndorsementsEtc download_endorsements(
      const std::string& product_name,
      const std::span<const uint8_t>& chip_id,
      const snp::TcbVersion& tcb_version,
      std::shared_ptr<RequestTracker> tracker = nullptr)
    {
      if (!tracker)
        tracker = std::make_shared<SynchronousRequestTracker>();

      EndorsementsEtc r;

      std::vector<Request> request_set;

      // https://www.amd.com/system/files/TechDocs/57230.pdf Chapter 4
      auto hwid = fmt::format("{:02x}", fmt::join(chip_id, ""));
      auto tcb_parameters = fmt::format(
        "blSPL={}&teeSPL={}&snpSPL={}&ucodeSPL={}",
        tcb_version.boot_loader,
        tcb_version.tee,
        tcb_version.snp,
        tcb_version.microcode);
      auto vcek_url = fmt::format(
        "{}/vcek/v1/{}/{}?{}", kds_url, product_name, hwid, tcb_parameters);
      auto vcek_issuer_chain_url =
        fmt::format("{}/vcek/v1/{}/cert_chain", kds_url, product_name);
      auto vcek_issuer_crl_url =
        fmt::format("{}/vcek/v1/{}/crl", kds_url, product_name);

      request_set.emplace_back(vcek_url);
      request_set.emplace_back(vcek_issuer_chain_url);
      request_set.emplace_back(vcek_issuer_crl_url);

      if (!tracker)
        tracker = std::make_shared<SynchronousRequestTracker>();

      bool tr = tracker->when_completed(
        std::move(request_set), [&r](std::vector<Response>&& response_set) {
          if (response_set.size() != 3)
            return false;

          auto issuer_chain = response_set[1].body;

          Unique_STACK_OF_X509 stack(issuer_chain);
          if (stack.size() != 2)
            return false;
          r.root_ca_certificate = stack.at(1).pem();

          auto vcek_cert = str2vec(response_set[0].body);
          r.vcek_certificate_chain = Unique_X509(vcek_cert, false).pem();
          r.vcek_certificate_chain += issuer_chain;

          auto issuer_crl_der = std::span<const uint8_t>(
            (uint8_t*)response_set[2].body.data(), response_set[2].body.size());
          r.vcek_issuer_chain_crl =
            Unique_X509_CRL(issuer_crl_der, false).pem();

          return true;
        });

      if (!tr)
        throw std::runtime_error("endorsement download request set failed");

      return r;
    }

    static bool verify_signature(
      const Unique_EVP_PKEY& pkey,
      const std::span<const uint8_t>& message,
      const snp::Signature& signature)
    {
      SHA512_CTX ctx;
      SHA384_Init(&ctx);
      SHA384_Update(&ctx, message.data(), message.size());
      std::vector<uint8_t> hash(ctx.md_len, 0);
      SHA384_Final(hash.data(), &ctx);

      auto signature_der =
        convert_signature_to_der(signature.r, signature.s, true);

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

    bool verify(
      const Attestation& a,
      const Options& options,
      std::shared_ptr<RequestTracker> tracker)
    {
      size_t indent = 0;

      const auto& quote =
        *reinterpret_cast<const ravl::sev_snp::snp::Attestation*>(
          a.evidence.data());

      if (quote.version != 2)
        throw std::runtime_error("unsupported attestation format version");

      Unique_X509_STORE store;

      EndorsementsEtc endorsements = {};

      if (!a.endorsements.empty() && !options.fresh_endorsements)
      {
        endorsements.vcek_certificate_chain = vec2str(a.endorsements);

        if (options.root_ca_certificate)
          endorsements.root_ca_certificate = *options.root_ca_certificate;
        else if (options.fresh_root_ca_certificate)
          endorsements.root_ca_certificate =
            download_root_ca_pem("Milan", tracker);
      }
      else
      {
        endorsements = download_endorsements(
          "Milan", quote.chip_id, quote.reported_tcb, tracker);

        if (options.root_ca_certificate)
          endorsements.root_ca_certificate = *options.root_ca_certificate;
      }

      if (options.verbosity > 0)
      {
        std::stringstream ss;
        ss << std::string(indent + 2, ' ') << "- Endorsements" << std::endl;

        std::string ins(indent + 4, ' ');
        Unique_STACK_OF_X509 st(endorsements.vcek_certificate_chain);
        ss << ins << "- VCEK certificate chain:" << std::endl;
        ss << st.to_string_short(indent + 4) << std::endl;

        if (options.verbosity > 1)
          ss << ins << "  - PEM:" << std::endl
             << indentate(endorsements.vcek_certificate_chain, 8) << std::endl;

        ss << ins << "- VCEK issuer CRL: ";
        if (endorsements.vcek_issuer_chain_crl.empty())
          ss << "none";
        else
        {
          Unique_X509_CRL vcek_issuer_crl(endorsements.vcek_issuer_chain_crl);
          ss << std::endl
             << vcek_issuer_crl.to_string_short(indent + 6) << std::endl;
          if (options.verbosity > 1)
            ss << ins << "  - PEM:" << std::endl
               << indentate(endorsements.vcek_issuer_chain_crl, 8);
        }

        log(ss.str());
      }

      store.set_flags(X509_V_FLAG_CRL_CHECK | X509_V_FLAG_CRL_CHECK_ALL);
      store.add_crl(
        endorsements.vcek_issuer_chain_crl); // TODO: is this for ARK or ASK?

      bool trusted_root = false;

      if (!endorsements.root_ca_certificate.empty())
        store.add(endorsements.root_ca_certificate);
      else
        trusted_root = true;

      if (options.verbosity > 0)
        log("- VCEK issuer certificate chain verification", indent + 2);
      auto chain = crypto::verify_certificate_chain(
        endorsements.vcek_certificate_chain,
        store,
        options.certificate_verification,
        trusted_root,
        options.verbosity,
        indent + 4);

      if (chain.size() != 3)
        throw std::runtime_error("unexpected certificate chain length");

      auto vcek_certificate = chain.at(0);
      auto ask_certificate = chain.at(1);
      auto ark_certificate = chain.at(2);

      if (!ark_certificate.has_public_key(
            snp::amd_milan_root_signing_public_key))
        throw std::runtime_error(
          "Root CA certificate does not have the expected AMD Milan public "
          "key");

      if (!ark_certificate.is_ca())
        throw std::runtime_error("Root CA certificate is not a CA");

      if (quote.signature_algo != snp::SignatureAlgorithm::ecdsa_p384_sha384)
        throw std::runtime_error("unexpected signature algorithm");

      std::span msg(
        a.evidence.data(), a.evidence.size() - sizeof(quote.signature));

      Unique_EVP_PKEY vcek_pk(vcek_certificate);
      if (!verify_signature(vcek_pk, msg, quote.signature))
        throw std::runtime_error("invalid VCEK signature");

      // https://www.amd.com/system/files/TechDocs/55766_SEV-KM_API_Specification.pdf
      // Appendix C defines a custom (?!) certificate format and steps to verify
      // the various certificates.

      return true;
    }
  }
}