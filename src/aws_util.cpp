#include "aws_util.h"
#include "platform.h"

#include "aws/core/Aws.h"
#include "aws/core/auth/AWSCredentialsProvider.h"
#include "aws/core/auth/AWSCredentialsProviderChain.h"
#include "aws/core/auth/SSOCredentialsProvider.h"
#include "aws/core/auth/STSCredentialsProvider.h"
#include "aws/core/client/ClientConfiguration.h"
#include "aws/core/utils/logging/CRTLogSystem.h"
#include "aws/core/utils/logging/LogLevel.h"
#include "aws/core/utils/logging/LogSystemInterface.h"
#include "aws/core/utils/memory/stl/AWSMap.h"
#include "aws/core/utils/threading/PooledThreadExecutor.h"
#include "aws/s3/S3Client.h"
#include "aws/transfer/TransferHandle.h"
#include "aws/transfer/TransferManager.h"

#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace envy {
namespace {

constexpr char const *kAllocationTag{ "envy-aws-util" };

Aws::SDKOptions g_options;
std::once_flag g_init_once;
std::mutex g_state_mutex;
bool g_initialized{ false };

// A credential failure explains itself only through the SDK's logger -- token expiry, an
// unreadable cache file -- so keep those lines. Nothing here is ever printed on its own.
constexpr size_t kCaptureMax{ 4 };
std::mutex g_capture_mutex;
std::vector<std::string> g_capture;

void capture_push(char const *tag, std::string_view text) {
  if (text.empty()) { return; }
  std::string line{ tag ? tag : "aws" };  // Tagged: several providers fail per resolution.
  line.append(": ").append(text);
  std::lock_guard<std::mutex> lock{ g_capture_mutex };
  // Deduplicated: a provider that fails is re-consulted for every credential lookup a
  // client makes while starting up, and reports the same reason each time.
  if (std::ranges::find(g_capture, line) != g_capture.end()) { return; }
  if (g_capture.size() == kCaptureMax) { g_capture.erase(g_capture.begin()); }
  g_capture.push_back(std::move(line));
}

std::vector<std::string> capture_take() {
  std::lock_guard<std::mutex> lock{ g_capture_mutex };
  return std::exchange(g_capture, std::vector<std::string>{});
}

class capture_log_system final : public Aws::Utils::Logging::LogSystemInterface {
 public:
  Aws::Utils::Logging::LogLevel GetLogLevel() const override {
    return Aws::Utils::Logging::LogLevel::Error;
  }

  void Log(Aws::Utils::Logging::LogLevel level,
           char const *tag,
           char const *format,
           ...) override {
    va_list args;
    va_start(args, format);
    vaLog(level, tag, format, args);
    va_end(args);
  }

  void vaLog(Aws::Utils::Logging::LogLevel,
             char const *tag,
             char const *format,
             va_list args) override {
    char buffer[512];
    if (std::vsnprintf(buffer, sizeof(buffer), format, args) > 0) {
      capture_push(tag, buffer);
    }
  }

  void LogStream(Aws::Utils::Logging::LogLevel,
                 char const *tag,
                 Aws::OStringStream const &stream) override {
    capture_push(tag, stream.str().c_str());
  }

  void Flush() override {}
};

// CRT logs are file- and socket-plumbing detail, never credential policy. Unset, the SDK
// redirects them into the capture above, crowding out the provider reports that matter.
class discard_crt_log_system final : public Aws::Utils::Logging::CRTLogSystemInterface {
 public:
  Aws::Utils::Logging::LogLevel GetLogLevel() const override {
    return Aws::Utils::Logging::LogLevel::Off;
  }

  void SetLogLevel(Aws::Utils::Logging::LogLevel) override {}

  void Log(Aws::Utils::Logging::LogLevel, char const *, char const *, va_list) override {}
};

// Per-region TransferManager cache. The provider is kept so a credential probe resolves
// through the very chain the transfers sign with, and warms its cache for them.
struct transfer_context {
  std::shared_ptr<Aws::Auth::AWSCredentialsProviderChain> provider;
  std::shared_ptr<Aws::S3::S3Client> client;
  std::shared_ptr<Aws::Transfer::TransferManager> manager;
};

std::mutex g_transfer_mutex;
std::unordered_map<std::string, transfer_context> g_transfer_contexts;

struct progress_entry {  // Per-transfer progress state, keyed by local file path.
  fetch_progress_cb_t cb;
  std::uint64_t last_reported{ 0 };
  bool cancelled{ false };
};

std::mutex g_progress_mutex;
std::unordered_map<std::string, progress_entry> g_progress_map;

constexpr std::uint64_t kProgressInterval{ 1 << 17 };  // 128 KB

void configure_options(Aws::SDKOptions &options) {
  // Error, not Off: Off skips logger installation entirely, and the SDK's error log is the
  // only place credential resolution says why it failed. capture_log_system buffers.
  options.loggingOptions.logLevel = Aws::Utils::Logging::LogLevel::Error;
  options.loggingOptions.logger_create_fn = [] {
    return Aws::MakeShared<capture_log_system>(kAllocationTag);
  };
  options.loggingOptions.crt_logger_create_fn = [] {
    return Aws::MakeShared<discard_crt_log_system>(kAllocationTag);
  };
}

// Serves both directions: TransferHandle::GetTargetFilePath() is the *local* file either
// way (download destination, upload source), so one map keyed on that path covers both.
void on_transfer_progress(
    Aws::Transfer::TransferManager const *,
    std::shared_ptr<Aws::Transfer::TransferHandle const> const &handle) {
  auto const local{ std::string(handle->GetTargetFilePath().c_str()) };
  auto const total{ handle->GetBytesTotalSize() };

  fetch_progress_cb_t cb;
  std::uint64_t transferred{};
  {
    std::lock_guard<std::mutex> lock{ g_progress_mutex };
    auto const it{ g_progress_map.find(local) };
    if (it == g_progress_map.end()) { return; }
    // Read under the lock: multipart uploads report from several threads, and a stale
    // reader winning the lock second would drive last_reported backwards (and
    // unsigned-wrap the interval check).
    transferred = handle->GetBytesTransferred();
    if (transferred <= it->second.last_reported ||
        transferred - it->second.last_reported < kProgressInterval) {
      return;
    }
    it->second.last_reported = transferred;
    cb = it->second.cb;
  }

  std::optional<std::uint64_t> content_length;
  if (total > 0) { content_length = total; }
  fetch_progress_t payload{ std::in_place_type<fetch_transfer_progress>,
                            fetch_transfer_progress{ transferred, content_length } };
  if (!cb(payload)) {
    std::lock_guard<std::mutex> lock{ g_progress_mutex };
    auto const it{ g_progress_map.find(local) };
    if (it != g_progress_map.end()) { it->second.cancelled = true; }
  }
}

// AWS default chain minus the EC2 IMDS provider, which blackholes off-EC2
// (169.254.169.254 routes nowhere). Empty creds -> signer sends unsigned (public
// buckets); real creds -> signed (private objects).
class non_imds_credentials_chain : public Aws::Auth::AWSCredentialsProviderChain {
 public:
  non_imds_credentials_chain() {
    using namespace Aws::Auth;
    AddProvider(Aws::MakeShared<EnvironmentAWSCredentialsProvider>(kAllocationTag));
    AddProvider(Aws::MakeShared<ProfileConfigFileAWSCredentialsProvider>(kAllocationTag));
    AddProvider(Aws::MakeShared<ProcessCredentialsProvider>(kAllocationTag));
    AddProvider(
        Aws::MakeShared<STSAssumeRoleWebIdentityCredentialsProvider>(kAllocationTag));
    AddProvider(Aws::MakeShared<SSOCredentialsProvider>(kAllocationTag));
  }
};

transfer_context &get_transfer_context(std::string const &region) {
  std::lock_guard<std::mutex> lock{ g_transfer_mutex };
  auto const it{ g_transfer_contexts.find(region) };
  if (it != g_transfer_contexts.end()) { return it->second; }

  // Disable IMDS so the ctor never probes 169.254.169.254 for region when none is
  // configured (env/profile); that blackholes off-EC2 and stalls until timeout.
  Aws::Client::ClientConfiguration config{ Aws::Client::ClientConfigurationInitValues{
      .shouldDisableIMDS = true } };
  if (!region.empty()) { config.region = Aws::String(region.c_str()); }

  auto provider{ Aws::MakeShared<non_imds_credentials_chain>(kAllocationTag) };

  auto client{ Aws::MakeShared<Aws::S3::S3Client>(
      kAllocationTag,
      provider,
      config,
      Aws::Client::AWSAuthV4Signer::PayloadSigningPolicy::Never,
      /*useVirtualAddressing=*/true) };

  // Both callbacks must be set here: TransferManagerConfiguration is copied into the
  // manager at Create() and the manager is memoized per region for process lifetime, so
  // there is no later opportunity to add one.
  Aws::Transfer::TransferManagerConfiguration tm_config(nullptr);
  tm_config.s3Client = client;
  tm_config.downloadProgressCallback = on_transfer_progress;
  tm_config.uploadProgressCallback = on_transfer_progress;
  tm_config.executorCreateFn = [] {
    return Aws::MakeShared<Aws::Utils::Threading::PooledThreadExecutor>(kAllocationTag, 8);
  };
  auto manager{ Aws::Transfer::TransferManager::Create(tm_config) };

  auto [inserted, _] = g_transfer_contexts.emplace(
      region,
      transfer_context{ std::move(provider), std::move(client), std::move(manager) });
  return inserted->second;
}

// prefix must already be lowercase.
bool istarts_with(std::string_view value, std::string_view prefix) {
  if (prefix.size() > value.size()) { return false; }
  for (size_t i{ 0 }; i < prefix.size(); ++i) {
    char const c{ (value[i] >= 'A' && value[i] <= 'Z')
                      ? static_cast<char>(value[i] - 'A' + 'a')
                      : value[i] };
    if (c != prefix[i]) { return false; }
  }
  return true;
}

// Formats a TransferHandle failure. Shared by both directions so the AWS-specific hints
// live in one place.
[[noreturn]] void throw_transfer_error(
    std::shared_ptr<Aws::Transfer::TransferHandle> const &handle,
    std::string_view op,
    bool writing) {
  auto const &error{ handle->GetLastError() };
  auto const http_code{ static_cast<int>(error.GetResponseCode()) };
  auto const &name{ error.GetExceptionName() };

  // A body the marshaller cannot map arrives wrapped as "Unable to parse ExceptionName:
  // <name> Message: <text>". Unwrap it; the code is unrecognized, not one envy diagnosed.
  constexpr std::string_view kUnparsed{ "Unable to parse ExceptionName: " };
  std::string_view const body{ error.GetMessage().c_str() };
  bool const unrecognized{ body.starts_with(kUnparsed) };
  auto const detail{ [&]() -> std::string_view {
    if (!unrecognized) { return body == "No response body." ? std::string_view{} : body; }
    constexpr std::string_view kText{ " Message: " };
    auto const pos{ body.find(kText, kUnparsed.size()) };
    return pos == std::string_view::npos ? body : body.substr(pos + kText.size());
  }() };

  std::ostringstream msg;
  msg << op << ": transfer failed";
  if (!name.empty()) {
    msg << (unrecognized ? ": unrecognized S3 error code " : ": ") << name;
  }
  if (!detail.empty()) { msg << ": " << detail; }
  if (http_code > 0) { msg << " (HTTP " << http_code << ")"; }

  if (name == "NoSuchBucket") {
    msg << "\n  Hint: the bucket does not exist. envy never creates buckets.";
  } else if (name == "PermanentRedirect" || name == "AuthorizationHeaderMalformed" ||
             http_code == 301) {
    // The SDK's ClientConfiguration falls back to us-east-1 when no region is configured,
    // so a bucket elsewhere fails as a redirect rather than as "region not set".
    msg << "\n  Hint: the bucket is in a different region. Set AWS_REGION (or a profile"
           " region); envy uses the AWS SDK's own resolution, which defaults to"
           " us-east-1.";
  } else if (http_code == 401 || http_code == 403) {
    msg << (writing ? "\n  Hint: these credentials lack s3:PutObject on this prefix."
                      " Run 'aws sso login' or check the bucket policy."
                    : "\n  Hint: check that the bucket policy allows public access,"
                      " or run 'aws sso login' to authenticate.");
  } else if (http_code == 501 && writing) {
    // S3 answers an unsigned write carrying x-amz-* headers with NotImplemented. envy
    // sends one only when the credential chain resolved nothing.
    msg << "\n  Hint: S3 rejects unsigned writes. envy signs only when AWS credentials"
           " resolve, so this means none did -- run 'aws sso login'.";
  }

  throw std::runtime_error(msg.str());
}

std::filesystem::path absolute_normalized(std::filesystem::path const &p) {
  auto out{ p.is_absolute() ? p : std::filesystem::absolute(p) };
  return out.lexically_normal();
}

char const *content_type_for(std::filesystem::path const &p) {
  auto const name{ p.filename().string() };
  if (name.ends_with(".zip")) { return "application/zip"; }
  if (name.ends_with(".tar.gz")) { return "application/gzip"; }
  return "application/octet-stream";
}

// Drives a TransferManager operation to completion: registers progress under the local
// file path, waits, honors cancellation, maps failure, reports 100%. Shared by both
// directions so the lifecycle exists once.
void run_transfer(
    std::string const &local_path,
    fetch_progress_cb_t const &progress,
    std::string_view op,
    bool writing,
    std::function<std::shared_ptr<Aws::Transfer::TransferHandle>()> const &start) {
  aws_init();

  if (progress) {  // Must be registered before the transfer starts.
    std::lock_guard<std::mutex> lock{ g_progress_mutex };
    g_progress_map.insert_or_assign(local_path, progress_entry{ progress });
  }

  auto handle{ start() };
  handle->WaitUntilFinished();

  if (auto const was_cancelled{ [&] {  // Deregister callback and check cancellation.
        std::lock_guard<std::mutex> lock{ g_progress_mutex };
        auto const it{ g_progress_map.find(local_path) };
        if (it == g_progress_map.end()) { return false; }
        bool const c{ it->second.cancelled };
        g_progress_map.erase(it);
        return c;
      }() }) {
    handle->Cancel();
    throw std::runtime_error(std::string{ op } +
                             ": transfer cancelled by progress callback");
  }

  switch (handle->GetStatus()) {
    case Aws::Transfer::TransferStatus::FAILED:
    case Aws::Transfer::TransferStatus::CANCELED:
    case Aws::Transfer::TransferStatus::ABORTED: throw_transfer_error(handle, op, writing);
    default: break;
  }

  if (progress) {  // Final callback so the bar reaches 100%.
    auto const total{ handle->GetBytesTotalSize() };
    std::optional<std::uint64_t> content_length;
    if (total > 0) { content_length = total; }
    fetch_progress_t payload{ std::in_place_type<fetch_transfer_progress>,
                              fetch_transfer_progress{ handle->GetBytesTransferred(),
                                                       content_length } };
    progress(payload);
  }
}

}  // namespace

s3_uri_parts aws_s3_parse_uri(std::string_view uri, std::string_view op) {
  constexpr std::string_view kPrefix{ "s3://" };
  if (!istarts_with(uri, kPrefix)) {
    throw std::invalid_argument(std::string{ op } + ": URI must start with s3://");
  }

  std::string_view remainder{ uri.substr(kPrefix.size()) };
  auto const slash{ remainder.find('/') };
  if (slash == 0) {
    throw std::invalid_argument(std::string{ op } + ": URI must include a bucket");
  }

  auto const bucket{ remainder.substr(0, slash) };  // npos slash -> whole remainder
  if (bucket.empty()) {
    throw std::invalid_argument(std::string{ op } + ": URI must include a bucket");
  }

  std::string_view key{};
  if (slash != std::string_view::npos) {
    key = remainder.substr(slash + 1);
    while (key.ends_with('/')) { key.remove_suffix(1); }
  }

  return s3_uri_parts{ .bucket = std::string{ bucket }, .key = std::string{ key } };
}

void aws_init() {
  std::call_once(g_init_once, [] {
    platform::env_var_set("AWS_SDK_LOAD_CONFIG", "1");
    configure_options(g_options);
    Aws::InitAPI(g_options);
    std::lock_guard<std::mutex> lock{ g_state_mutex };
    g_initialized = true;
  });
}

void aws_shutdown() {
  std::lock_guard<std::mutex> lock{ g_state_mutex };
  if (!g_initialized) { return; }
  {
    std::lock_guard<std::mutex> tl{ g_transfer_mutex };
    // TransferManager's destructor only drains buffers; the SDK requires an explicit wait
    // before ShutdownAPI. Multipart uploads make this reachable: the final part's callback
    // marks the main handle COMPLETED (releasing WaitUntilFinished) before its own
    // RemoveTask runs, so tasks can still be in flight here.
    constexpr std::int64_t kDrainTimeoutMs{ 30'000 };
    for (auto &[_, ctx] : g_transfer_contexts) {
      ctx.manager->WaitUntilAllFinished(kDrainTimeoutMs);
    }
    g_transfer_contexts.clear();
  }
  g_initialized = false;
  Aws::ShutdownAPI(g_options);
}

aws_shutdown_guard::~aws_shutdown_guard() { aws_shutdown(); }

std::filesystem::path aws_s3_download(s3_download_request const &request) {
  constexpr std::string_view kOp{ "aws_s3_download" };

  if (request.destination.empty()) {
    throw std::invalid_argument("aws_s3_download: destination path is empty");
  }

  auto const local{ absolute_normalized(request.destination) };
  auto const parts{ aws_s3_parse_uri(request.uri, kOp) };
  if (parts.key.empty()) {
    throw std::invalid_argument("aws_s3_download: URI must include a key");
  }

  auto const parent{ local.parent_path() };
  if (!parent.empty()) {
    std::error_code ec;
    std::filesystem::create_directories(parent, ec);
    if (ec) {
      throw std::runtime_error(
          "aws_s3_download: failed to create destination directories: " + ec.message());
    }
  }

  auto const local_str{ local.string() };
  run_transfer(local_str, request.progress, kOp, /*writing=*/false, [&] {
    return get_transfer_context(request.region.value_or(""))
        .manager->DownloadFile(Aws::String(parts.bucket.c_str()),
                               Aws::String(parts.key.c_str()),
                               Aws::String(local_str.c_str()));
  });

  return local;
}

void aws_credentials_require(std::optional<std::string> const &region,
                             std::string_view op) {
  aws_init();
  capture_take();  // Drop anything logged earlier, so only this resolution is quoted.

  auto const provider{ get_transfer_context(region.value_or("")).provider };
  if (!provider->GetAWSCredentials().IsExpiredOrEmpty()) { return; }

  std::ostringstream msg;
  msg << op << ": no usable AWS credentials";
  for (auto const &line : capture_take()) { msg << "\n  " << line; }
  msg << "\n  Hint: run 'aws sso login' (honoring AWS_PROFILE), or set"
         " AWS_ACCESS_KEY_ID and AWS_SECRET_ACCESS_KEY.";
  throw std::runtime_error(msg.str());
}

void aws_s3_upload(s3_upload_request const &request) {
  constexpr std::string_view kOp{ "aws_s3_upload" };

  if (request.source.empty()) {
    throw std::invalid_argument("aws_s3_upload: source path is empty");
  }

  auto const local{ absolute_normalized(request.source) };
  if (!std::filesystem::is_regular_file(local)) {
    throw std::runtime_error("aws_s3_upload: source is not a regular file: " +
                             local.string());
  }

  auto const parts{ aws_s3_parse_uri(request.uri, kOp) };
  if (parts.key.empty()) {
    throw std::invalid_argument("aws_s3_upload: URI must include a key");
  }

  auto const local_str{ local.string() };
  run_transfer(local_str, request.progress, kOp, /*writing=*/true, [&] {
    return get_transfer_context(request.region.value_or(""))
        .manager->UploadFile(Aws::String(local_str.c_str()),
                             Aws::String(parts.bucket.c_str()),
                             Aws::String(parts.key.c_str()),
                             Aws::String(content_type_for(local)),
                             Aws::Map<Aws::String, Aws::String>{});
  });
}

}  // namespace envy
