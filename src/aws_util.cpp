#include "aws_util.h"

#include "c_completion.h"
#include "fetch_error.h"
#include "tui.h"
#include "util.h"

#include "aws/auth/auth.h"
#include "aws/auth/credentials.h"
#include "aws/auth/private/credentials_utils.h"
#include "aws/auth/private/sso_token_utils.h"
#include "aws/auth/signing_config.h"
#include "aws/common/date_time.h"
#include "aws/common/error.h"
#include "aws/common/thread.h"
#include "aws/common/uri.h"
#include "aws/http/connection_manager.h"
#include "aws/http/proxy.h"
#include "aws/http/request_response.h"
#include "aws/io/channel_bootstrap.h"
#include "aws/io/event_loop.h"
#include "aws/io/host_resolver.h"
#include "aws/io/retry_strategy.h"
#include "aws/io/tls_channel_handler.h"
#include "aws/s3/s3.h"
#include "aws/s3/s3_client.h"
#include "aws/sdkutils/aws_profile.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace envy {
namespace {

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

// The advice a failed S3 request earns, by S3 error code and HTTP status. denied replaces
// the generic 401/403 advice when envy knows more, such as an expired SSO session.
void append_hint(std::ostringstream &msg,
                 std::string_view name,
                 int http_code,
                 bool writing,
                 std::string_view denied,
                 std::string_view bucket_region) {
  if (name == "NoSuchBucket") {
    msg << "\n  Hint: the bucket does not exist. envy never creates buckets.";
  } else if (http_code == 404 && name.empty()) {  // a HEAD's 404 carries no S3 code
    msg << "\n  Hint: no such object, or no such bucket.";
  } else if (name == "PermanentRedirect" || name == "AuthorizationHeaderMalformed" ||
             http_code == 301 || (http_code == 400 && !bucket_region.empty())) {
    // S3 names the bucket's region in x-amz-bucket-region, even on a bodiless HEAD.
    if (bucket_region.empty()) {
      msg << "\n  Hint: the bucket is in a different region. Set AWS_DEFAULT_REGION (or a"
             " profile region); envy defaults to us-east-1.";
    } else {
      msg << "\n  Hint: the bucket is in " << bucket_region << ". Set AWS_DEFAULT_REGION="
          << bucket_region << " and retry.";
    }
  } else if ((http_code == 401 || http_code == 403) && !denied.empty()) {
    msg << "\n  Hint: " << denied;
  } else if (http_code == 401 || http_code == 403) {
    msg << (writing ? "\n  Hint: these credentials lack s3:PutObject on this prefix."
                      " Run 'aws sso login' or check the bucket policy."
                    : "\n  Hint: check that the bucket policy allows public access,"
                      " or run 'aws sso login' to authenticate.");
  } else if (http_code == 501 && writing) {
    // An unsigned write draws this, but so does any endpoint asked for an operation it
    // lacks -- S3-compatible ones included. State the possibility, not a cause.
    msg << "\n  Hint: S3 answers unsigned writes with NotImplemented. If the session"
           " expired mid-run, run 'aws sso login' and retry.";
  }
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

// --- aws-c-s3 ------------------------------------------------------------------------

// Downloads use small ranges: aws-c-s3 reports progress only as each range completes.
constexpr std::uint64_t kDownloadPartSize{ 2 * 1024 * 1024 };
constexpr std::uint32_t kConnectionsPerTransfer{ 8 };  // of the client's 16: no hogging

aws_byte_cursor to_cursor(std::string_view s) {
  return aws_byte_cursor_from_array(s.data(), s.size());
}

std::string_view to_view(aws_byte_cursor c) {
  return { reinterpret_cast<char const *>(c.ptr), c.len };
}

std::string_view env_value(char const *name) {  // unset and empty read alike
  char const *const value{ std::getenv(name) };
  return value ? std::string_view{ value } : std::string_view{};
}

[[noreturn]] void throw_aws_error(std::string_view op, std::string_view what) {
  throw_runtime_error({ op, ": ", what, ": ", aws_error_str(aws_last_error()) });
}

// Owning handles: each aws-c-* object envy holds is released exactly once, throw or not.
template <auto Release>
struct aws_release {
  template <typename T>
  void operator()(T *p) const noexcept {
    Release(p);
  }
};
template <typename T, auto Release>
using aws_ptr = std::unique_ptr<T, aws_release<Release>>;
using provider_ptr = aws_ptr<aws_credentials_provider, &aws_credentials_provider_release>;
using string_ptr = aws_ptr<aws_string, &aws_string_destroy>;

// aws-c-auth's credential HTTP bounds the connect but not the response, so a proxy that
// accepts and never answers would stall signing forever; every provider builds here.
aws_http_connection_manager *bounded_connection_manager(
    aws_allocator *alloc,
    aws_http_connection_manager_options const *options) noexcept {
  auto bounded{ *options };
  if (!bounded.response_first_byte_timeout_ms) {
    bounded.response_first_byte_timeout_ms = 10'000;
  }
  return aws_http_connection_manager_new(alloc, &bounded);
}

aws_auth_http_system_vtable *auth_http() {  // static: providers shut down after envy lets go
  static aws_auth_http_system_vtable table{ [] {
    auto t{ *g_aws_credentials_provider_http_function_table };
    t.aws_http_connection_manager_new = bounded_connection_manager;
    return t;
  }() };
  return &table;
}

struct credentials_diagnosis {
  std::vector<std::string> failures;  // "<source>: <why>"
  std::string sso_expired_at;         // set when the SSO token has lapsed
};

// The aws-c-* stack behind S3, built on first use: one event loop group, resolver,
// bootstrap, TLS context, credential chain and client serve every transfer.
struct crt_stack : unmovable {
  aws_allocator *alloc{ aws_default_allocator() };
  std::string profile;         // AWS_PROFILE, AWS_DEFAULT_PROFILE, else "default"
  std::string region;          // for requests that name none
  std::string endpoint;        // AWS_ENDPOINT_URL_S3, AWS_ENDPOINT_URL, profile; else AWS
  std::string sso_expired_at;  // the profile's cached SSO token had lapsed at startup
  proxy_env_var_settings proxy{ .env_var_type = AWS_HPEV_ENABLE };  // HTTPS_PROXY et al.
  aws_ptr<aws_event_loop_group, &aws_event_loop_group_release> elg;  // members release
  aws_ptr<aws_host_resolver, &aws_host_resolver_release> resolver;  // bottom-up; aws-c-*
  aws_ptr<aws_client_bootstrap, &aws_client_bootstrap_release> bootstrap;  // winds down
  aws_ptr<aws_tls_ctx, &aws_tls_ctx_release> tls;  // on its own threads
  provider_ptr credentials;
  aws_ptr<aws_s3_client, &aws_s3_client_release> client;
  std::once_flag diagnosed;  // credential failures are diagnosed once per run
  credentials_diagnosis diagnosis;

  void build();
};

// A property of the active profile in the config file, or empty.
std::string profile_property(aws_allocator *alloc,
                             std::string const &profile,
                             char const *property) {
  string_ptr const path{ aws_get_config_file_path(alloc, nullptr) };
  aws_ptr<aws_profile_collection, &aws_profile_collection_release> const config{
    path ? aws_profile_collection_new_from_file(alloc, path.get(), AWS_PST_CONFIG)
         : nullptr
  };
  string_ptr const name{ aws_string_new_from_c_str(alloc, profile.c_str()) };
  string_ptr const key{ aws_string_new_from_c_str(alloc, property) };
  auto const *const entry{
    config && name ? aws_profile_collection_get_profile(config.get(), name.get()) : nullptr
  };
  auto const *const value{ entry && key ? aws_profile_get_property(entry, key.get())
                                        : nullptr };
  return value ? aws_string_c_str(aws_profile_property_get_value(value)) : "";
}

// The lapsed expiry (ISO 8601) of the profile's cached SSO token, else empty. Read here:
// aws-c-auth dials the SSO portal before checking it, so offline it reads as a DNS error.
std::string lapsed_sso_token(aws_allocator *alloc, std::string const &profile) {
  auto const key{ [&] {  // what names the cache file
    auto session{ profile_property(alloc, profile, "sso_session") };
    return session.empty() ? profile_property(alloc, profile, "sso_start_url") : session;
  }() };
  if (key.empty()) { return {}; }
  string_ptr const input{ aws_string_new_from_c_str(alloc, key.c_str()) };
  string_ptr const path{ input ? aws_construct_sso_token_path(alloc, input.get())
                               : nullptr };
  aws_ptr<aws_sso_token, &aws_sso_token_destroy> const token{
    path ? aws_sso_token_new_from_file(alloc, path.get()) : nullptr
  };
  aws_date_time now;
  aws_date_time_init_now(&now);
  if (!token || aws_date_time_diff(&token->expiration, &now) > 0) { return {}; }
  std::uint8_t text[AWS_DATE_TIME_STR_MAX_LEN]{};
  auto buf{ aws_byte_buf_from_empty_array(text, sizeof text) };
  return aws_date_time_to_utc_time_str(&token->expiration, AWS_DATE_FORMAT_ISO_8601, &buf)
             ? std::string{ "an unknown time" }
             : std::string{ to_view(aws_byte_cursor_from_buf(&buf)) };
}

// A credential source, by the name its failure is reported under.
struct named_provider {
  char const *name;
  provider_ptr provider;
};

// The AWS default chain minus IMDS and ECS, which blackhole off AWS. The profile source
// runs credential_process itself; SSO is explicit, as the C profile provider skips it.
std::vector<named_provider> credential_sources(crt_stack &s) {
  std::vector<named_provider> out;
  auto const add{ [&](char const *name, aws_credentials_provider *p) {
    provider_ptr owned{ p };  // owned before push_back can throw
    if (owned) { out.push_back({ name, std::move(owned) }); }
  } };
  auto const profile_name{ to_cursor(s.profile) };
  aws_credentials_provider_environment_options const env{};
  add("environment", aws_credentials_provider_new_environment(s.alloc, &env));
  aws_credentials_provider_profile_options profile{};
  profile.profile_name_override = profile_name;
  profile.bootstrap = s.bootstrap.get();
  profile.tls_ctx = s.tls.get();
  profile.function_table = auth_http();
  profile.proxy_ev_settings = &s.proxy;
  add("profile", aws_credentials_provider_new_profile(s.alloc, &profile));
  aws_credentials_provider_sts_web_identity_options web{};
  web.bootstrap = s.bootstrap.get();
  web.tls_ctx = s.tls.get();
  web.proxy_ev_settings = &s.proxy;
  web.function_table = auth_http();
  web.profile_name_override = profile_name;
  web.region = to_cursor(s.region);
  add("web identity", aws_credentials_provider_new_sts_web_identity(s.alloc, &web));
  if (s.sso_expired_at.empty()) {  // a lapsed token would only send it to the portal
    aws_credentials_provider_sso_options sso{};
    sso.profile_name_override = profile_name;
    sso.bootstrap = s.bootstrap.get();
    sso.tls_ctx = s.tls.get();
    sso.proxy_ev_settings = &s.proxy;
    sso.function_table = auth_http();
    add("sso", aws_credentials_provider_new_sso(s.alloc, &sso));
  }
  return out;
}

void crt_stack::build() {
  constexpr std::string_view kOp{ "aws" };
  auto const first_set{ [](std::initializer_list<char const *> vars) {
    for (char const *var : vars) {
      if (auto const v{ env_value(var) }; !v.empty()) { return std::string{ v }; }
    }
    return std::string{};
  } };

  profile = first_set({ "AWS_PROFILE", "AWS_DEFAULT_PROFILE" });
  if (profile.empty()) { profile = "default"; }
  region = first_set({ "AWS_DEFAULT_REGION", "AWS_REGION" });  // the SDK's order, no IMDS
  if (region.empty()) { region = profile_property(alloc, profile, "region"); }
  if (region.empty()) { region = "us-east-1"; }
  if (auto const ignore{ env_value("AWS_IGNORE_CONFIGURED_ENDPOINT_URLS") };
      !(ignore.size() == 4 && istarts_with(ignore, "true"))) {
    endpoint = first_set({ "AWS_ENDPOINT_URL_S3", "AWS_ENDPOINT_URL" });
    if (endpoint.empty()) { endpoint = profile_property(alloc, profile, "endpoint_url"); }
  }
  sso_expired_at = lapsed_sso_token(alloc, profile);

  aws_event_loop_group_options const elg_options{ .loop_count = 4 };  // for 16 connections
  elg.reset(aws_event_loop_group_new(alloc, &elg_options));
  if (!elg) { throw_aws_error(kOp, "event loop group"); }
  aws_host_resolver_default_options const resolver_options{ .max_entries = 16,
                                                            .el_group = elg.get() };
  resolver.reset(aws_host_resolver_new_default(alloc, &resolver_options));
  if (!resolver) { throw_aws_error(kOp, "host resolver"); }
  aws_client_bootstrap_options const bootstrap_options{ .event_loop_group = elg.get(),
                                                        .host_resolver = resolver.get() };
  bootstrap.reset(aws_client_bootstrap_new(alloc, &bootstrap_options));
  if (!bootstrap) { throw_aws_error(kOp, "client bootstrap"); }
  aws_tls_ctx_options tls_options;
  aws_tls_ctx_options_init_default_client(&tls_options, alloc);
  tls.reset(aws_tls_client_ctx_new(alloc, &tls_options));
  aws_tls_ctx_options_clean_up(&tls_options);
  if (!tls) { throw_aws_error(kOp, "TLS context"); }

  // Anonymous last: with nothing configured, requests go unsigned and public objects
  // still download. The chain acquires each member; `chain` releases envy's refs.
  auto chain{ credential_sources(*this) };
  aws_credentials_provider_shutdown_options const no_shutdown{};
  chain.push_back(
      { "anonymous",
        provider_ptr{ aws_credentials_provider_new_anonymous(alloc, &no_shutdown) } });
  std::vector<aws_credentials_provider *> members;
  for (auto const &source : chain) {
    if (source.provider) { members.push_back(source.provider.get()); }
  }
  aws_credentials_provider_chain_options const chain_options{ .providers = members.data(),
                                                              .provider_count =
                                                                  members.size() };
  provider_ptr const linked{ aws_credentials_provider_new_chain(alloc, &chain_options) };
  if (!linked) { throw_aws_error(kOp, "credential chain"); }
  // Cached, so the chain runs per expiry, not per request; the refresh bounds how long a
  // transient failure (cached as anonymous) leaves requests unsigned.
  aws_credentials_provider_cached_options const cached_options{
    .source = linked.get(), .refresh_time_in_milliseconds = 60'000
  };
  credentials.reset(aws_credentials_provider_new_cached(alloc, &cached_options));
  if (!credentials) { throw_aws_error(kOp, "credential cache"); }

  // AWS_MAX_ATTEMPTS as the SDK reads it; aws-c-io caps retries at 63.
  aws_ptr<aws_retry_strategy, &aws_retry_strategy_release> const retry{
    [&]() -> aws_retry_strategy * {
      auto const text{ env_value("AWS_MAX_ATTEMPTS") };
      std::size_t attempts{ 0 };
      std::from_chars(text.data(), text.data() + text.size(), attempts);
      if (attempts == 0) { return nullptr; }  // aws-c-s3's default: 5 retries
      aws_no_retry_options const no_retry{};
      aws_standard_retry_options standard{};
      standard.backoff_retry_options.el_group = elg.get();
      standard.backoff_retry_options.max_retries = std::min<std::size_t>(attempts - 1, 63);
      auto *const strategy{ attempts == 1 ? aws_retry_strategy_new_no_retry(alloc, &no_retry)
                                          : aws_retry_strategy_new_standard(alloc, &standard) };
      if (!strategy) { throw_aws_error(kOp, "retry strategy"); }
      return strategy;
    }()
  };

  aws_tls_connection_options tls_connection;
  aws_tls_connection_options_init_from_ctx(&tls_connection, tls.get());
  aws_signing_config_aws signing;
  aws_s3_init_default_signing_config(&signing, to_cursor(region), credentials.get());
  aws_s3_client_config client_config{};
  client_config.region = to_cursor(region);
  client_config.client_bootstrap = bootstrap.get();
  client_config.tls_mode = AWS_MR_TLS_ENABLED;
  client_config.tls_connection_options = &tls_connection;
  client_config.signing_config = &signing;
  client_config.retry_strategy = retry.get();
  // Uploads take 8 MiB parts (S3's floor is 5). Left alone, aws-c-s3 sizes ranges for
  // 10 Gbps; capped, at most 16 connections x 4 in flight x part size are buffered.
  client_config.part_size = 8 * 1024 * 1024;
  client_config.max_active_connections_override = 2 * kConnectionsPerTransfer;
  client_config.proxy_ev_settings = &proxy;
  client.reset(aws_s3_client_new(alloc, &client_config));
  aws_tls_connection_options_clean_up(&tls_connection);
  if (!client) { throw_aws_error(kOp, "S3 client"); }
}

std::mutex g_crt_mutex;
std::unique_ptr<crt_stack> g_crt;  // Built on first use; reset by aws_shutdown.
bool g_library_live{ false };

crt_stack &crt_get() {
  std::lock_guard const lock{ g_crt_mutex };
  if (!g_crt) {
    if (!std::exchange(g_library_live, true)) {
      aws_s3_library_init(aws_default_allocator());
    }
    auto stack{ std::make_unique<crt_stack>() };
    stack->build();  // A throw here releases what was built.
    g_crt = std::move(stack);
  }
  return *g_crt;
}

struct resolved_credentials {
  int error{ AWS_ERROR_SUCCESS };
  bool anonymous{ false };
  bool expired{ false };  // a cache or credential_process can return lapsed ones
};

// One provider's answer, waited for.
resolved_credentials resolve_credentials(aws_credentials_provider *provider) {
  struct waiter {
    c_completion done;
    resolved_credentials result;
  } w;
  auto const on_credentials{ +[](aws_credentials *credentials,
                                 int error,
                                 void *user) noexcept {
    auto &w{ *static_cast<waiter *>(user) };
    auto const expiry{ credentials ? aws_credentials_get_expiration_timepoint_seconds(
                                         credentials)
                                   : UINT64_MAX };
    auto const now{ static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count()) };
    w.result = !credentials ? resolved_credentials{ error ? error
                                                          : AWS_AUTH_SIGNING_NO_CREDENTIALS }
               : expiry <= now
                   ? resolved_credentials{ AWS_AUTH_SIGNING_INVALID_CREDENTIALS, false, true }
                   : resolved_credentials{ AWS_ERROR_SUCCESS,
                                           aws_credentials_is_anonymous(credentials) };
    w.done.finish();
  } };
  if (aws_credentials_provider_get_credentials(provider, on_credentials, &w)) {
    return { aws_last_error() };
  }
  w.done.wait();
  return w.result;
}

// Why no source produced credentials, source by source: the chain reports only its last
// member's error. Empty when one works -- then credentials aren't the problem.
credentials_diagnosis diagnose_credentials(crt_stack &s) {
  auto const sources{ credential_sources(s) };
  bool const sso_profile{ !s.sso_expired_at.empty() ||
                          std::ranges::any_of(sources, [](named_provider const &p) {
                            return std::string_view{ p.name } == "sso";
                          }) };
  credentials_diagnosis out{ .sso_expired_at = s.sso_expired_at };
  for (auto const &[name, provider] : sources) {
    std::string_view const source{ name };
    auto const result{ resolve_credentials(provider.get()) };
    if (result.error == AWS_ERROR_SUCCESS) { return {}; }
    // Quote only sources meant to answer: environment always constructs, and an SSO
    // profile's static-key lookup fails by design.
    if ((source == "environment" && env_value("AWS_ACCESS_KEY_ID").empty()) ||
        (source == "profile" && sso_profile)) {
      continue;
    }
    out.failures.push_back(std::string{ source } + ": " +
                           (result.expired ? "credentials have expired"
                                           : aws_error_str(result.error)));
  }
  return out;
}

credentials_diagnosis const &diagnosis_of(crt_stack &s) {
  std::call_once(s.diagnosed, [&] { s.diagnosis = diagnose_credentials(s); });
  return s.diagnosis;
}

std::string sso_expired_hint(crt_stack const &s, std::string const &expired_at) {
  return "the cached AWS SSO token for profile '" + s.profile + "' expired at " +
         expired_at + ". Run 'aws sso login' and retry.";
}

// What a 401/403 should say when the request went unsigned for want of credentials;
// empty when it was signed, since then the policy, not the credentials, refused it.
std::string denied_hint(crt_stack &s) {
  if (auto const r{ resolve_credentials(s.credentials.get()) };
      r.error == AWS_ERROR_SUCCESS && !r.anonymous) {
    return {};
  }
  auto const &diagnosis{ diagnosis_of(s) };
  if (!diagnosis.sso_expired_at.empty()) {
    return sso_expired_hint(s, diagnosis.sso_expired_at);
  }
  if (diagnosis.failures.empty()) { return {}; }
  std::string out{ "no usable AWS credentials, so the request went unsigned:" };
  for (auto const &failure : diagnosis.failures) { out.append("\n    ").append(failure); }
  return out.append(
      "\n  Run 'aws sso login', or set AWS_ACCESS_KEY_ID and"
      " AWS_SECRET_ACCESS_KEY.");
}

// How a meta request ended, as its finish callback reported it.
struct s3_outcome {
  int error_code{ AWS_ERROR_SUCCESS };
  int status{ 0 };
  std::string body;           // S3's XML error, when it sent one
  std::string bucket_region;  // x-amz-bucket-region: where a misrouted bucket lives
};

[[noreturn]] void throw_s3_failure(crt_stack &crt,
                                   std::string_view op,
                                   bool writing,
                                   s3_outcome const &outcome,
                                   std::string_view url,
                                   std::string_view local) {
  auto const error{ aws_s3_parse_error(outcome.body) };
  std::ostringstream msg;
  msg << op << ": transfer failed";
  if (outcome.error_code == AWS_ERROR_S3_OBJECT_MODIFIED) {
    msg << ": the object changed during the download; retry";
  } else if (outcome.error_code == AWS_OP_ERR || outcome.error_code == AWS_ERROR_UNKNOWN) {
    // aws-c-s3 reports a failed fwrite/fread as a bare -1.
    msg << ": I/O failed on " << local << (writing ? "" : " (disk full, or not writable?)");
  } else {
    if (!error.code.empty()) { msg << ": " << error.code; }
    if (!error.message.empty() && error.message != error.code) {
      msg << ": " << error.message;
    } else if (error.code.empty() &&
               outcome.error_code != AWS_ERROR_S3_INVALID_RESPONSE_STATUS) {
      msg << ": " << aws_error_str(outcome.error_code);  // no response at all: say why
    }
  }
  if (outcome.status > 0) {
    msg << " (HTTP " << outcome.status << ")";
  } else {
    msg << " (" << url << ")";
  }
  bool const denied{ outcome.status == 401 || outcome.status == 403 };
  append_hint(msg,
              error.code,
              outcome.status,
              writing,
              denied ? denied_hint(crt) : std::string{},
              outcome.bucket_region);
  // What aws-c-s3 won't retry but a server means as "later": envy's fetch policy will.
  if (outcome.status == 429 || outcome.status == 502 || outcome.status == 504) {
    throw fetch_error(fetch_error_kind::HTTP_STATUS, msg.str(), outcome.status);
  }
  throw_runtime_error({ msg.str() });
}

// One meta request, start to shutdown. Callbacks run on event-loop threads inside
// c_completion's guard; run() returns after the shutdown callback, the last of them.
class s3_transfer : unmovable {
 public:
  s3_transfer(fetch_progress_cb_t const &progress, std::string_view op)
      : progress_{ progress }, op_{ op } {}

  // Returns the object's length as aws-c-s3 reported it, or 0 when it never said.
  std::uint64_t run(aws_s3_client *client,
                    aws_s3_meta_request_options options,
                    function_ref<void(s3_outcome const &)> fail) {
    options.user_data = this;
    options.finish_callback = on_finish;
    options.shutdown_callback = on_shutdown;
    options.progress_callback = on_progress;
    if (!aws_s3_client_make_meta_request(client, &options)) {  // no callbacks follow
      fail({ .error_code = aws_last_error() });  // throws
    }
    done_.wait();

    auto const [cancelled, outcome, last, total]{ [&] {
      std::lock_guard const lock{ mutex_ };  // uncontended: every callback has returned
      return std::tuple{ cancelled_, outcome_, snapshot(), total_ };
    }() };
    if (cancelled) {
      throw_runtime_error({ op_, ": transfer cancelled by progress callback" });
    }
    if (outcome.error_code != AWS_ERROR_SUCCESS) { fail(outcome); }
    if (progress_) { progress_(last); }  // Final callback so the bar reaches 100%.
    return total;
  }

 private:
  fetch_progress_t snapshot() const {
    return fetch_progress_t{
      std::in_place_type<fetch_transfer_progress>,
      fetch_transfer_progress{ .transferred = transferred_,
                               .total = total_ ? std::optional{ total_ } : std::nullopt }
    };
  }

  // Once per completed part, which is what bounds aws-c-s3's progress granularity.
  void report(aws_s3_meta_request *request, aws_s3_meta_request_progress const &p) {
    auto const now{ [&] {
      std::lock_guard const lock{ mutex_ };
      transferred_ += p.bytes_transferred;
      if (p.content_length) { total_ = p.content_length; }
      return snapshot();
    }() };
    if (progress_ && !progress_(now)) {
      {
        std::lock_guard const lock{ mutex_ };
        cancelled_ = true;
      }
      aws_s3_meta_request_cancel(request);
    }
  }

  static void on_progress(aws_s3_meta_request *request,
                          aws_s3_meta_request_progress const *progress,
                          void *user) noexcept {
    auto &self{ *static_cast<s3_transfer *>(user) };
    self.done_.guard([&] { self.report(request, *progress); },
                     [&] { aws_s3_meta_request_cancel(request); });
  }

  static void on_finish(aws_s3_meta_request *request,
                        aws_s3_meta_request_result const *result,
                        void *user) noexcept {
    auto &self{ *static_cast<s3_transfer *>(user) };
    self.done_.guard(
        [&] {
          std::lock_guard const lock{ self.mutex_ };
          self.outcome_.error_code = result->error_code;
          self.outcome_.status = result->response_status;
          if (result->error_response_body) {
            self.outcome_.body = to_view(aws_byte_cursor_from_buf(result->error_response_body));
          }
          if (aws_byte_cursor region{}; result->error_response_headers &&
                                      !aws_http_headers_get(result->error_response_headers,
                                                            to_cursor("x-amz-bucket-region"),
                                                            &region)) {
            self.outcome_.bucket_region = to_view(region);
          }
        },
        [] {});
    aws_s3_meta_request_release(request);  // make_meta_request's, as aws-c-s3's samples do
  }

  static void on_shutdown(void *user) noexcept {
    static_cast<s3_transfer *>(user)->done_.finish();
  }

  fetch_progress_cb_t const &progress_;
  std::string_view op_;
  c_completion done_;
  std::mutex mutex_;
  std::uint64_t transferred_{ 0 };
  std::uint64_t total_{ 0 };
  bool cancelled_{ false };
  s3_outcome outcome_;
};

// One operation on s3://bucket/key: endpoint, Host and signing region from the URI and
// environment; setup adds the operation's own headers and files. local names the file
// in messages. Returns the object length aws-c-s3 reported, or 0.
std::uint64_t s3_run(
    aws_byte_cursor method,
    s3_uri_parts const &parts,
    std::optional<std::string> const &region,
    fetch_progress_cb_t const &progress,
    std::string_view op,
    bool writing,
    std::string_view local,
    function_ref<void(aws_s3_meta_request_options &, aws_http_message *)> setup) {
  auto &crt{ crt_get() };
  std::string const signing_region{ region && !region->empty() ? *region : crt.region };
  auto const endpoint{ aws_s3_endpoint(parts, signing_region, crt.endpoint) };
  std::string const url{ endpoint.scheme + "://" + endpoint.authority };
  tui::debug("s3: %.*s %s%s (region %s)",
             static_cast<int>(method.len),
             reinterpret_cast<char const *>(method.ptr),
             url.c_str(),
             endpoint.path.c_str(),
             signing_region.c_str());

  aws_ptr<aws_http_message, &aws_http_message_release> const message{
    aws_http_message_new_request(crt.alloc)
  };
  struct uri_holder : unmovable {
    aws_uri uri{};
    ~uri_holder() { aws_uri_clean_up(&uri); }
  } uri;
  auto const url_cursor{ to_cursor(url) };
  if (!message || aws_http_message_set_request_method(message.get(), method) ||
      aws_http_message_set_request_path(message.get(), to_cursor(endpoint.path)) ||
      aws_http_message_add_header(
          message.get(),
          aws_http_header{ .name = to_cursor("Host"),
                           .value = to_cursor(endpoint.authority) }) ||
      aws_uri_init_parse(&uri.uri, crt.alloc, &url_cursor)) {
    throw_aws_error(op, "cannot build the request");
  }

  aws_signing_config_aws signing;
  aws_s3_init_default_signing_config(&signing,
                                     to_cursor(signing_region),
                                     crt.credentials.get());
  aws_s3_meta_request_options options{};
  options.signing_config = &signing;
  options.message = message.get();
  options.endpoint = &uri.uri;
  options.max_active_connections_override = kConnectionsPerTransfer;
  setup(options, message.get());
  return s3_transfer{ progress, op }.run(crt.client.get(), options, [&](s3_outcome const &o) {
    throw_s3_failure(crt, op, writing, o, url, local);
  });
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

namespace {

bool is_unreserved(char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
         c == '-' || c == '.' || c == '_' || c == '~';
}

// RFC 3986 percent-encoding with '/' kept: S3 keys are paths, signed as sent.
void append_uri_path(std::string &out, std::string_view raw) {
  constexpr char kHex[]{ "0123456789ABCDEF" };
  for (char const c : raw) {
    if (is_unreserved(c) || c == '/') {
      out.push_back(c);
    } else {
      auto const b{ static_cast<unsigned char>(c) };
      out.append({ '%', kHex[b >> 4], kHex[b & 0xF] });
    }
  }
}

// S3's DNS-compatible bucket rule without dots, which break TLS wildcard certificates.
bool virtual_hostable(std::string_view bucket) {
  if (bucket.size() < 3 || bucket.size() > 63) { return false; }
  auto const alnum{ [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
  } };
  return alnum(bucket.front()) && alnum(bucket.back()) &&
         std::ranges::all_of(bucket, [&](char c) { return alnum(c) || c == '-'; });
}

// Path-style hosts: IP literals and localhost, as S3-compatible test servers listen.
bool path_style_host(std::string_view host) {
  if (host.starts_with('[') || host == "localhost") { return true; }
  return !host.empty() && std::ranges::all_of(host, [](char c) {
    return (c >= '0' && c <= '9') || c == '.';
  });
}

std::string xml_unescape(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  constexpr std::pair<std::string_view, char> kEntities[]{ { "&amp;", '&' },
                                                           { "&lt;", '<' },
                                                           { "&gt;", '>' },
                                                           { "&quot;", '"' },
                                                           { "&apos;", '\'' } };
  for (size_t i{ 0 }; i < s.size();) {
    auto const entity{ std::ranges::find_if(kEntities, [&](auto const &e) {
      return s.substr(i).starts_with(e.first);
    }) };
    if (entity == std::end(kEntities)) {
      out.push_back(s[i++]);
    } else {
      out.push_back(entity->second);
      i += entity->first.size();
    }
  }
  return out;
}

}  // namespace

s3_endpoint aws_s3_endpoint(s3_uri_parts const &parts,
                            std::string_view region,
                            std::string_view override_url) {
  s3_endpoint out;
  std::string_view prefix;  // an override's own path, e.g. a gateway's mount point
  if (override_url.empty()) {
    out.scheme = "https";
    out.authority.append("s3.").append(region).append(
        region.starts_with("cn-") ? ".amazonaws.com.cn" : ".amazonaws.com");
  } else {
    auto const scheme_end{ override_url.find("://") };
    auto const rest{ scheme_end == std::string_view::npos
                         ? override_url
                         : override_url.substr(scheme_end + 3) };
    out.scheme = scheme_end == std::string_view::npos ? "https"
                                                      : override_url.substr(0, scheme_end);
    auto const slash{ rest.find('/') };
    out.authority = rest.substr(0, slash);
    if (slash != std::string_view::npos) { prefix = rest.substr(slash); }
    while (prefix.ends_with('/')) { prefix.remove_suffix(1); }
  }

  auto const host{ [&] {
    std::string_view const a{ out.authority };
    if (a.starts_with('[')) { return a.substr(0, a.find(']') + 1); }
    return a.substr(0, a.rfind(':'));
  }() };
  bool const path_style{ !virtual_hostable(parts.bucket) || path_style_host(host) };
  if (!path_style) { out.authority.insert(0, parts.bucket + "."); }

  out.path.assign(prefix);
  out.path.push_back('/');
  if (path_style) {
    append_uri_path(out.path, parts.bucket);
    out.path.push_back('/');
  }
  append_uri_path(out.path, parts.key);
  return out;
}

s3_error aws_s3_parse_error(std::string_view xml) {
  auto const element{ [&](std::string_view tag) -> std::string {
    std::string const open{ "<" + std::string{ tag } + ">" };
    std::string const close{ "</" + std::string{ tag } + ">" };
    auto const begin{ xml.find(open) };
    if (begin == std::string_view::npos) { return {}; }
    auto const start{ begin + open.size() };
    auto const end{ xml.find(close, start) };
    return end == std::string_view::npos ? std::string{}
                                         : xml_unescape(xml.substr(start, end - start));
  } };
  return { .code = element("Code"), .message = element("Message") };
}

void aws_shutdown() {
  std::lock_guard const lock{ g_crt_mutex };
  g_crt.reset();
  if (std::exchange(g_library_live, false)) {
    // aws-c-* winds down on its own threads; a resolver stuck in getaddrinfo can't be
    // joined, so bound the wait, and leave the libraries up if one is still running.
    aws_thread_set_managed_join_timeout_ns(5'000'000'000);
    if (aws_thread_join_all_managed() == AWS_OP_SUCCESS) { aws_s3_library_clean_up(); }
  }
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
      throw_runtime_error(
          { "aws_s3_download: failed to create destination directories: ", ec.message() });
    }
  }

  auto const local_utf8{ util_path_to_utf8(local) };  // aws-c-* paths are UTF-8
  // Validated against the checksum S3 stored, if any, which the HEAD it implies returns.
  aws_s3_checksum_config const checksums{ .validate_response_checksum = true };
  try {
    auto const expected{ s3_run(aws_http_method_get,
                                parts,
                                request.region,
                                request.progress,
                                kOp,
                                /*writing=*/false,
                                local_utf8,
                                [&](aws_s3_meta_request_options &options, aws_http_message *) {
                                  options.type = AWS_S3_META_REQUEST_TYPE_GET_OBJECT;
                                  options.part_size = kDownloadPartSize;
                                  options.recv_filepath = to_cursor(local_utf8);
                                  options.recv_file_option = AWS_S3_RECV_FILE_CREATE_OR_REPLACE;
                                  options.recv_file_delete_on_failure = true;
                                  options.checksum_config = &checksums;
                                }) };
    // aws-c-s3 never checks fclose, so a disk that fills on the last flush goes unnoticed.
    if (std::error_code ec; expected && std::filesystem::file_size(local, ec) != expected) {
      throw_runtime_error({ kOp, ": ", local, " is short of ", expected, " bytes (disk full?)" });
    }
  } catch (...) {  // A checksum mismatch is decided after aws-c-s3's own cleanup.
    std::error_code ec;
    std::filesystem::remove(local, ec);
    throw;
  }

  return local;
}

void aws_credentials_require(std::string_view op) {
  auto &crt{ crt_get() };
  if (auto const resolved{ resolve_credentials(crt.credentials.get()) };
      resolved.error == AWS_ERROR_SUCCESS && !resolved.anonymous) {
    return;
  }
  auto const &diagnosis{ diagnosis_of(crt) };
  std::ostringstream msg;
  msg << op << ": no usable AWS credentials";
  if (!diagnosis.sso_expired_at.empty()) {
    msg << "\n  Hint: " << sso_expired_hint(crt, diagnosis.sso_expired_at);
  } else {
    for (auto const &failure : diagnosis.failures) { msg << "\n  " << failure; }
    msg << "\n  Hint: run 'aws sso login' (honoring AWS_PROFILE), or set"
           " AWS_ACCESS_KEY_ID and AWS_SECRET_ACCESS_KEY.";
  }
  throw_runtime_error({ msg.str() });
}

void aws_s3_upload(s3_upload_request const &request) {
  constexpr std::string_view kOp{ "aws_s3_upload" };

  if (request.source.empty()) {
    throw std::invalid_argument("aws_s3_upload: source path is empty");
  }

  auto const local{ absolute_normalized(request.source) };
  if (!std::filesystem::is_regular_file(local)) {
    throw_runtime_error({ "aws_s3_upload: source is not a regular file: ", local });
  }

  auto const parts{ aws_s3_parse_uri(request.uri, kOp) };
  if (parts.key.empty()) {
    throw std::invalid_argument("aws_s3_upload: URI must include a key");
  }

  auto const local_utf8{ util_path_to_utf8(local) };  // aws-c-* paths are UTF-8
  // CRC64NVME in a trailer, as the SDK sent it: S3 checks the bytes it stores.
  aws_s3_checksum_config const checksums{ .location = AWS_SCL_TRAILER,
                                          .checksum_algorithm = AWS_SCA_CRC64NVME };
  s3_run(aws_http_method_put,
         parts,
         request.region,
         request.progress,
         kOp,
         /*writing=*/true,
         local_utf8,
         [&](aws_s3_meta_request_options &options, aws_http_message *message) {
           options.type = AWS_S3_META_REQUEST_TYPE_PUT_OBJECT;
           options.send_filepath = to_cursor(local_utf8);
           options.checksum_config = &checksums;
           if (aws_http_message_add_header(
                   message,
                   aws_http_header{ .name = to_cursor("Content-Type"),
                                    .value = to_cursor(content_type_for(local)) })) {
             throw_aws_error(kOp, "cannot build the request");
           }
         });
}

}  // namespace envy
