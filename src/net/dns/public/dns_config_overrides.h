// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef NET_DNS_PUBLIC_DNS_CONFIG_OVERRIDES_H_
#define NET_DNS_PUBLIC_DNS_CONFIG_OVERRIDES_H_

#include <string>
#include <vector>

#include "base/time/time.h"
#include "net/base/ip_endpoint.h"
#include "net/base/net_export.h"
#include "net/dns/public/dns_over_https_server_config.h"
#include "net/dns/public/secure_dns_mode.h"
#include "third_party/abseil-cpp/absl/types/optional.h"

namespace net {

struct DnsConfig;

// Overriding values to be applied over a DnsConfig struct.
struct NET_EXPORT DnsConfigOverrides {
  DnsConfigOverrides();
  DnsConfigOverrides(const DnsConfigOverrides& other);
  DnsConfigOverrides(DnsConfigOverrides&& other);
  ~DnsConfigOverrides();

  DnsConfigOverrides& operator=(const DnsConfigOverrides& other);
  DnsConfigOverrides& operator=(DnsConfigOverrides&& other);

  bool operator==(const DnsConfigOverrides& other) const;
  bool operator!=(const DnsConfigOverrides& other) const;

  // Creation method that initializes all values with the defaults from
  // DnsConfig. Guarantees the result of OverridesEverything() will be |true|.
  static DnsConfigOverrides CreateOverridingEverythingWithDefaults();

  // Creates a new DnsConfig where any field with an overriding value in |this|
  // is replaced with that overriding value. Any field without an overriding
  // value (|absl::nullopt|) will be copied as-is from |config|.
  DnsConfig ApplyOverrides(const DnsConfig& config) const;

  // Returns |true| if the overriding configuration is comprehensive and would
  // override everything in a base DnsConfig. This is the case if all Optional
  // fields have a value.
  bool OverridesEverything() const;

  // Overriding values. See same-named fields in DnsConfig for explanations.
  absl::optional<std::vector<IPEndPoint>> nameservers;
  absl::optional<std::vector<std::string>> search;
  absl::optional<bool> append_to_multi_label_name;
  absl::optional<int> ndots;
  absl::optional<base::TimeDelta> fallback_period;
  absl::optional<int> attempts;
  absl::optional<int> doh_attempts;
  absl::optional<bool> rotate;
  absl::optional<bool> use_local_ipv6;
  absl::optional<std::vector<DnsOverHttpsServerConfig>> dns_over_https_servers;
  absl::optional<SecureDnsMode> secure_dns_mode;
  absl::optional<bool> allow_dns_over_https_upgrade;
  absl::optional<std::vector<std::string>> disabled_upgrade_providers;

  // |hosts| is not supported for overriding except to clear it.
  bool clear_hosts = false;

  // Note no overriding value for |unhandled_options|. It is meta-configuration,
  // and there should be no reason to override it.
};

}  // namespace net

#endif  // NET_DNS_PUBLIC_DNS_CONFIG_OVERRIDES_H_
