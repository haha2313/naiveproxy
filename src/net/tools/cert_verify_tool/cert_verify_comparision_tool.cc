// Copyright 2021 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Tool used to do batch comparisons of cert verification results between
// the platform verifier and the builtin verifier. Currently only tested on
// Windows.
#include <iostream>

#include "base/at_exit.h"
#include "base/bind.h"
#include "base/callback_helpers.h"
#include "base/command_line.h"
#include "base/containers/span.h"
#include "base/logging.h"
#include "base/message_loop/message_pump_type.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/synchronization/waitable_event.h"
#include "base/task/thread_pool/thread_pool_instance.h"
#include "base/threading/thread.h"
#include "build/build_config.h"
#include "net/cert/cert_net_fetcher.h"
#include "net/cert/cert_verify_proc.h"
#include "net/cert/cert_verify_proc_builtin.h"
#include "net/cert/crl_set.h"
#include "net/cert/internal/system_trust_store.h"
#include "net/cert/trial_comparison_cert_verifier_util.h"
#include "net/cert/x509_certificate.h"
#include "net/cert_net/cert_net_fetcher_url_request.h"
#include "net/tools/cert_verify_tool/cert_verify_tool_util.h"
#include "net/tools/cert_verify_tool/dumper.pb.h"
#include "net/tools/cert_verify_tool/verify_using_cert_verify_proc.h"
#include "net/url_request/url_request_context.h"
#include "net/url_request/url_request_context_builder.h"
#include "net/url_request/url_request_context_getter.h"

#if defined(OS_LINUX) || defined(OS_CHROMEOS)
#include "net/proxy_resolution/proxy_config.h"
#include "net/proxy_resolution/proxy_config_service_fixed.h"
#endif

namespace {
std::string GetUserAgent() {
  return "cert_verify_comparison_tool/0.1";
}

void SetUpOnNetworkThread(
    std::unique_ptr<net::URLRequestContext>* context,
    scoped_refptr<net::CertNetFetcherURLRequest>* cert_net_fetcher,
    base::WaitableEvent* initialization_complete_event) {
  net::URLRequestContextBuilder url_request_context_builder;
  url_request_context_builder.set_user_agent(GetUserAgent());
#if defined(OS_LINUX) || defined(OS_CHROMEOS)
  // On Linux, use a fixed ProxyConfigService, since the default one
  // depends on glib.
  //
  // TODO(akalin): Remove this once http://crbug.com/146421 is fixed.
  url_request_context_builder.set_proxy_config_service(
      std::make_unique<net::ProxyConfigServiceFixed>(
          net::ProxyConfigWithAnnotation()));
#endif
  *context = url_request_context_builder.Build();

  // TODO(mattm): add command line flag to configure using
  // CertNetFetcher
  *cert_net_fetcher = base::MakeRefCounted<net::CertNetFetcherURLRequest>();
  (*cert_net_fetcher)->SetURLRequestContext(context->get());
  initialization_complete_event->Signal();
}

void ShutdownOnNetworkThread(
    std::unique_ptr<net::URLRequestContext>* context,
    scoped_refptr<net::CertNetFetcherURLRequest>* cert_net_fetcher) {
  (*cert_net_fetcher)->Shutdown();
  cert_net_fetcher->reset();
  context->reset();
}

// Runs certificate verification using a particular CertVerifyProc.
class CertVerifyImpl {
 public:
  CertVerifyImpl(const std::string& name,
                 scoped_refptr<net::CertVerifyProc> proc)
      : name_(name), proc_(std::move(proc)) {}

  virtual ~CertVerifyImpl() = default;
  std::string GetName() const { return name_; }

  // Does certificate verification.
  bool VerifyCert(net::X509Certificate& x509_target_and_intermediates,
                  const std::string& hostname,
                  net::CertVerifyResult* result,
                  int* error) {
    if (hostname.empty()) {
      std::cerr << "ERROR: --hostname is required for " << GetName()
                << ", skipping\n";
      return true;  // "skipping" is considered a successful return.
    }

    scoped_refptr<net::CRLSet> crl_set = net::CRLSet::BuiltinCRLSet();
    // TODO(mattm): add command line flags to configure VerifyFlags.
    int flags = 0;
    // Don't add any additional trust anchors.
    net::CertificateList x509_additional_trust_anchors;

    // TODO(crbug.com/634484): use a real netlog and print the results?
    *error = proc_->Verify(&x509_target_and_intermediates, hostname,
                           /*ocsp_response=*/std::string(),
                           /*sct_list=*/std::string(), flags, crl_set.get(),
                           x509_additional_trust_anchors, result,
                           net::NetLogWithSource());

    return *error == net::OK;
  }

 private:
  const std::string name_;
  scoped_refptr<net::CertVerifyProc> proc_;
};

// Creates an subclass of CertVerifyImpl based on its name, or returns nullptr.
std::unique_ptr<CertVerifyImpl> CreateCertVerifyImplFromName(
    base::StringPiece impl_name,
    scoped_refptr<net::CertNetFetcher> cert_net_fetcher) {
#if !(defined(OS_FUCHSIA) || defined(OS_LINUX) || defined(OS_CHROMEOS))
  if (impl_name == "platform") {
    return std::make_unique<CertVerifyImpl>(
        "CertVerifyProc (system)", net::CertVerifyProc::CreateSystemVerifyProc(
                                       std::move(cert_net_fetcher)));
  }
#endif

  if (impl_name == "builtin") {
    return std::make_unique<CertVerifyImpl>(
        "CertVerifyProcBuiltin",
        net::CreateCertVerifyProcBuiltin(
            std::move(cert_net_fetcher),
            net::CreateSslSystemTrustStoreChromeRoot()));
  }

  std::cerr << "WARNING: Unrecognized impl: " << impl_name << "\n";
  return nullptr;
}

const char kUsage[] =
    " --input=<file>\n"
    "\n"
    " <file> is a file containing serialized protos from trawler. Format \n"
    " of the file is a uint32 size, followed by that many bytes of a\n"
    " serialized proto message of type \n"
    " cert_verify_tool::CertChain. The path to the file must not\n"
    " contain any dot(.) characters."
    "\n";

void PrintUsage(const char* argv0) {
  std::cerr << "Usage: " << argv0 << kUsage;
}

// Returns -1 if an error occurred.
int RunCert(base::File* input_file,
            const std::unique_ptr<CertVerifyImpl>& platform_proc,
            const std::unique_ptr<CertVerifyImpl>& builtin_proc) {
  // read 4 bytes, convert to uint32_t
  std::vector<char> size_bytes(4);
  int size_bytes_read = input_file->ReadAtCurrentPos(size_bytes.data(), 4);
  if (size_bytes_read != 4) {
    std::cerr << "Couldn't read 4 byte size field, read only "
              << size_bytes_read << "\n";
    return -1;
  }

  uint32_t proto_size;
  proto_size = (static_cast<uint8_t>(size_bytes[3]) << 24) +
               (static_cast<uint8_t>(size_bytes[2]) << 16) +
               (static_cast<uint8_t>(size_bytes[1]) << 8) +
               (static_cast<uint8_t>(size_bytes[0]));

  // read proto_size bytes, parse to proto.
  std::vector<char> proto_bytes(proto_size);
  int proto_bytes_read =
      input_file->ReadAtCurrentPos(proto_bytes.data(), proto_size);

  if ((proto_bytes_read - proto_size) != 0) {
    std::cerr << "Couldn't read expected proto of size " << proto_size
              << "read only " << proto_bytes_read << "\n";
    return -1;
  }

  cert_verify_tool::CertChain cert_chain;

  if (!cert_chain.ParseFromArray(proto_bytes.data(), proto_bytes_read)) {
    std::cerr << "Proto parse error for proto of size " << proto_size << ", "
              << proto_bytes_read << " proto bytes read total, "
              << size_bytes_read << " size bytes read\n\n\n";
    return -1;
  }

  std::vector<base::StringPiece> der_cert_chain;
  for (int i = 0; i < cert_chain.der_certs_size(); i++) {
    der_cert_chain.push_back(cert_chain.der_certs(i));
  }

  scoped_refptr<net::X509Certificate> x509_target_and_intermediates =
      net::X509Certificate::CreateFromDERCertChain(der_cert_chain);
  if (!x509_target_and_intermediates) {
    std::cerr << "X509Certificate::CreateFromDERCertChain failed for host"
              << cert_chain.host() << "\n\n\n";

    // We try to continue here; its possible that the cert chain contained
    // invalid certs for some reason so we don't bail out entirely.
    return 0;
  }

  net::CertVerifyResult platform_result;
  int platform_error;
  net::CertVerifyResult builtin_result;
  int builtin_error;

  platform_proc->VerifyCert(*x509_target_and_intermediates, cert_chain.host(),
                            &platform_result, &platform_error);
  builtin_proc->VerifyCert(*x509_target_and_intermediates, cert_chain.host(),
                           &builtin_result, &builtin_error);

  if (net::CertVerifyResultEqual(platform_result, builtin_result) &&
      platform_error == builtin_error) {
    std::cerr << "Host " << cert_chain.host() << " has equal verify results!\n";
  } else {
    // Much of the below code is lifted from
    // TrialComparisonCertVerifier::Job::OnTrialJobCompleted as it wasn't
    // obvious how to easily refactor the code here to prevent copying this
    // section of code.
    const bool chains_equal =
        platform_result.verified_cert->EqualsIncludingChain(
            builtin_result.verified_cert.get());

    // Chains built were different with either builtin being OK or both not OK.
    // Pass builtin chain to platform, see if platform comes back the same.
    if (!chains_equal &&
        (builtin_error == net::OK || platform_error != net::OK)) {
      net::CertVerifyResult platform_reverification_result;
      int platform_reverification_error;

      platform_proc->VerifyCert(
          *builtin_result.verified_cert, cert_chain.host(),
          &platform_reverification_result, &platform_reverification_error);
      if (net::CertVerifyResultEqual(platform_reverification_result,
                                     builtin_result) &&
          platform_reverification_error == builtin_error) {
        std::cerr << "Host " << cert_chain.host()
                  << " has equal reverify results!\n";
        return 0;
      }
    }

    net::TrialComparisonResult result = net::IsSynchronouslyIgnorableDifference(
        platform_error, platform_result, builtin_error, builtin_result,
        /*sha1_local_anchors_enabled=*/false);

    // TODO(hchao): gather stats on result values.

    if (result != net::TrialComparisonResult::kInvalid) {
      std::cerr << "Host " << cert_chain.host()
                << " has ignorable verify results!\n";
      return 0;
    }

    std::cerr << "Host " << cert_chain.host()
              << " has different verify results!\n";

    // TODO(hchao): print input chain.

    std::cerr << "Platform: (error = "
              << net::ErrorToShortString(platform_error) << ")\n";
    PrintCertVerifyResult(platform_result);
    std::cerr << "Builtin:  (error = " << net::ErrorToShortString(builtin_error)
              << ")\n";
    PrintCertVerifyResult(builtin_result);
  }

  std::cerr << "\n\n";

  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  base::AtExitManager at_exit_manager;
  if (!base::CommandLine::Init(argc, argv)) {
    std::cerr << "ERROR in CommandLine::Init\n";
    return 1;
  }

  base::ThreadPoolInstance::CreateAndStartWithDefaultParams(
      "cert_verify_comparison_tool");
  base::CommandLine& command_line = *base::CommandLine::ForCurrentProcess();
  logging::LoggingSettings settings;
  settings.logging_dest =
      logging::LOG_TO_SYSTEM_DEBUG_LOG | logging::LOG_TO_STDERR;
  logging::InitLogging(settings);

  base::CommandLine::StringVector args = command_line.GetArgs();
  if (command_line.HasSwitch("help")) {
    PrintUsage(argv[0]);
    return 1;
  }

  base::FilePath input_path = command_line.GetSwitchValuePath("input");
  if (input_path.empty()) {
    std::cerr << "Error: --input is required\n";
    return 1;
  }

  int flags = base::File::FLAG_OPEN | base::File::FLAG_READ;
  std::unique_ptr<base::File> input_file =
      std::make_unique<base::File>(input_path, flags);

  if (!input_file->IsValid()) {
    std::cerr << "Error: --dump file " << input_path.MaybeAsASCII()
              << " is not valid \n";
    return 1;
  }

  // Create a network thread to be used for AIA fetches, and wait for a
  // CertNetFetcher to be constructed on that thread.
  base::Thread::Options options(base::MessagePumpType::IO, 0);
  base::Thread thread("network_thread");
  CHECK(thread.StartWithOptions(std::move(options)));
  // Owned by this thread, but initialized, used, and shutdown on the network
  // thread.
  std::unique_ptr<net::URLRequestContext> context;
  scoped_refptr<net::CertNetFetcherURLRequest> cert_net_fetcher;
  base::WaitableEvent initialization_complete_event(
      base::WaitableEvent::ResetPolicy::MANUAL,
      base::WaitableEvent::InitialState::NOT_SIGNALED);
  thread.task_runner()->PostTask(
      FROM_HERE,
      base::BindOnce(&SetUpOnNetworkThread, &context, &cert_net_fetcher,
                     &initialization_complete_event));
  initialization_complete_event.Wait();

  // Initialize verifiers; platform and builtin.
  std::vector<std::unique_ptr<CertVerifyImpl>> impls;
  std::unique_ptr<CertVerifyImpl> platform_proc =
      CreateCertVerifyImplFromName("platform", cert_net_fetcher);
  if (!platform_proc) {
    std::cerr << "Error platform proc not sucessfully created";
    return 1;
  }
  std::unique_ptr<CertVerifyImpl> builtin_proc =
      CreateCertVerifyImplFromName("builtin", cert_net_fetcher);
  if (!builtin_proc) {
    std::cerr << "Error builtin proc not sucessfully created";
    return 1;
  }

  // Read file and process cert chains.
  while (RunCert(input_file.get(), platform_proc, builtin_proc) != -1) {
  }

  // Clean up on the network thread and stop it (which waits for the clean up
  // task to run).
  thread.task_runner()->PostTask(
      FROM_HERE,
      base::BindOnce(&ShutdownOnNetworkThread, &context, &cert_net_fetcher));
  thread.Stop();
}
