#include "ssl_certs_macos.h"

#include "util.h"

#import <Foundation/Foundation.h>
#import <Security/Security.h>

#include <stdexcept>

namespace {

// Convert single certificate to PEM format
void append_cert_pem(SecCertificateRef cert, std::vector<unsigned char> &out) {
  NSData *der_data = (__bridge_transfer NSData *)SecCertificateCopyData(cert);
  if (!der_data) { return; }

  NSString *base64 = [der_data base64EncodedStringWithOptions:0];

  // PEM header
  static char const kHeader[] = "-----BEGIN CERTIFICATE-----\n";
  out.insert(out.end(), kHeader, kHeader + sizeof(kHeader) - 1);

  // Base64 data with line breaks every 64 characters
  char const *b64 = [base64 UTF8String];
  size_t len = [base64 lengthOfBytesUsingEncoding:NSUTF8StringEncoding];
  for (size_t i = 0; i < len; i += 64) {
    size_t chunk = std::min(size_t{ 64 }, len - i);
    out.insert(out.end(), b64 + i, b64 + i + chunk);
    out.push_back('\n');
  }

  // PEM footer
  static char const kFooter[] = "-----END CERTIFICATE-----\n";
  out.insert(out.end(), kFooter, kFooter + sizeof(kFooter) - 1);
}

// Extract certificates from a specific trust domain
void extract_from_domain(SecTrustSettingsDomain domain, std::vector<unsigned char> &out) {
  CFArrayRef certs_cf = nullptr;
  if (SecTrustSettingsCopyCertificates(domain, &certs_cf) != errSecSuccess || !certs_cf) {
    return;
  }
  NSArray *certs = (__bridge_transfer NSArray *)certs_cf;

  for (id cert_id in certs) {
    SecCertificateRef cert = (__bridge SecCertificateRef)cert_id;

    CFArrayRef trust_settings_cf = nullptr;
    OSStatus ts_status =
        SecTrustSettingsCopyTrustSettings(cert, domain, &trust_settings_cf);

    bool trusted = false;
    if (ts_status == errSecSuccess && trust_settings_cf) {
      NSArray *trust_settings = (__bridge_transfer NSArray *)trust_settings_cf;

      // Empty array = trust for all purposes
      if (trust_settings.count == 0) {
        trusted = true;
      } else {
        bool dominated = false;  // True if an explicit result was found
        for (NSDictionary *setting in trust_settings) {
          NSNumber *result_num = setting[(__bridge NSString *)kSecTrustSettingsResult];
          if (result_num) {
            SInt32 result = [result_num intValue];
            if (result == kSecTrustSettingsResultDeny) {
              trusted = false;
              dominated = true;
              break;
            }
            if (result == kSecTrustSettingsResultTrustRoot ||
                result == kSecTrustSettingsResultTrustAsRoot) {
              trusted = true;
              dominated = true;
              break;
            }
          }
        }
        // No explicit result found: treat as trusted (default behavior)
        if (!dominated) { trusted = true; }
      }
    } else if (ts_status == errSecItemNotFound) {
      // No specific settings = trust for all purposes (for system domain)
      if (domain == kSecTrustSettingsDomainSystem) { trusted = true; }
    }

    if (trusted) { append_cert_pem(cert, out); }
  }
}

}  // namespace

namespace envy {

std::vector<unsigned char> extract_system_ca_certs() {
  @autoreleasepool {
    std::vector<unsigned char> pem_bundle;
    pem_bundle.reserve(256 * 1024);  // ~200 certs typical

    // System domain first (Apple's trusted root CAs)
    extract_from_domain(kSecTrustSettingsDomainSystem, pem_bundle);

    // Then admin domain (org-installed certs)
    extract_from_domain(kSecTrustSettingsDomainAdmin, pem_bundle);

    // User domain last (user-installed certs)
    extract_from_domain(kSecTrustSettingsDomainUser, pem_bundle);

    if (pem_bundle.empty()) {
      throw_runtime_error({ "No trusted CA certificates found in system keychain" });
    }

    return pem_bundle;
  }
}

}  // namespace envy
