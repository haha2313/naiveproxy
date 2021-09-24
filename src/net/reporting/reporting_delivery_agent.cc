// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/reporting/reporting_delivery_agent.h"

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "base/bind.h"
#include "base/check.h"
#include "base/containers/contains.h"
#include "base/json/json_writer.h"
#include "base/time/tick_clock.h"
#include "base/timer/timer.h"
#include "base/values.h"
#include "net/base/network_isolation_key.h"
#include "net/base/url_util.h"
#include "net/reporting/reporting_cache.h"
#include "net/reporting/reporting_cache_observer.h"
#include "net/reporting/reporting_context.h"
#include "net/reporting/reporting_delegate.h"
#include "net/reporting/reporting_endpoint_manager.h"
#include "net/reporting/reporting_report.h"
#include "net/reporting/reporting_uploader.h"
#include "url/gurl.h"
#include "url/origin.h"

namespace net {

namespace {

using ReportList = std::vector<const ReportingReport*>;

std::string SerializeReports(const ReportList& reports, base::TimeTicks now) {
  base::ListValue reports_value;

  for (const ReportingReport* report : reports) {
    std::unique_ptr<base::DictionaryValue> report_value =
        std::make_unique<base::DictionaryValue>();

    report_value->SetInteger("age", (now - report->queued).InMilliseconds());
    report_value->SetString("type", report->type);
    report_value->SetString("url", report->url.spec());
    report_value->SetString("user_agent", report->user_agent);
    report_value->SetKey("body", report->body->Clone());

    reports_value.Append(std::move(report_value));
  }

  std::string json_out;
  bool json_written = base::JSONWriter::Write(reports_value, &json_out);
  DCHECK(json_written);
  return json_out;
}

bool CompareReportGroupKeys(const ReportingReport* lhs,
                            const ReportingReport* rhs) {
  return lhs->GetGroupKey() < rhs->GetGroupKey();
}

// Each Delivery corresponds to one upload URLRequest.
class Delivery {
 public:
  // The target of a delivery. All reports uploaded together must share the
  // same values for these parameters.
  // Note that |origin| here (which matches the report's |origin|) is not
  // necessarily the same as the |origin| of the ReportingEndpoint's group key
  // (if the endpoint is configured to include subdomains). Reports with
  // different group keys can be in the same delivery, as long as the NIK and
  // report origin are the same, and they all get assigned to the same endpoint
  // URL.
  struct Target {
    Target(const NetworkIsolationKey& network_isolation_key,
           const url::Origin& origin,
           const GURL& endpoint_url)
        : network_isolation_key(network_isolation_key),
          origin(origin),
          endpoint_url(endpoint_url) {}

    ~Target() = default;

    bool operator<(const Target& other) const {
      return std::tie(network_isolation_key, origin, endpoint_url) <
             std::tie(other.network_isolation_key, other.origin,
                      other.endpoint_url);
    }

    NetworkIsolationKey network_isolation_key;
    url::Origin origin;
    GURL endpoint_url;
  };

  explicit Delivery(const Target& target) : target_(target) {}

  ~Delivery() = default;

  // Add the reports in [reports_begin, reports_end) into this delivery.
  // Modify the report counter for the |endpoint| to which this delivery is
  // destined.
  void AddReports(const ReportingEndpoint& endpoint,
                  const ReportList::const_iterator reports_begin,
                  const ReportList::const_iterator reports_end) {
    DCHECK(reports_begin != reports_end);
    DCHECK_EQ(endpoint.group_key.network_isolation_key,
              network_isolation_key());
    DCHECK(IsSubdomainOf(target_.origin.host() /* subdomain */,
                         endpoint.group_key.origin.host() /* superdomain */));
    for (auto it = reports_begin; it != reports_end; ++it) {
      DCHECK_EQ((*reports_begin)->GetGroupKey(), (*it)->GetGroupKey());
      DCHECK_EQ((*it)->network_isolation_key, network_isolation_key());
      DCHECK_EQ(url::Origin::Create((*it)->url), target_.origin);
      DCHECK_EQ((*it)->group, endpoint.group_key.group_name);
      // Report origin is equal to, or a subdomain of, the endpoint
      // configuration's origin.
      DCHECK(IsSubdomainOf((*it)->url.host_piece() /* subdomain */,
                           endpoint.group_key.origin.host() /* superdomain */));
    }

    reports_per_group_[endpoint.group_key] +=
        std::distance(reports_begin, reports_end);
    reports_.insert(reports_.end(), reports_begin, reports_end);
  }

  // Records statistics for reports after an upload has completed.
  // Either removes successfully delivered reports, or increments the failure
  // counter if delivery was unsuccessful.
  void ProcessOutcome(ReportingCache* cache, bool success) {
    for (const auto& group_name_and_count : reports_per_group_) {
      cache->IncrementEndpointDeliveries(group_name_and_count.first,
                                         target_.endpoint_url,
                                         group_name_and_count.second, success);
    }
    if (success) {
      cache->RemoveReports(reports_);
    } else {
      cache->IncrementReportsAttempts(reports_);
    }
  }

  const NetworkIsolationKey& network_isolation_key() const {
    return target_.network_isolation_key;
  }
  const GURL& endpoint_url() const { return target_.endpoint_url; }
  const ReportList& reports() const { return reports_; }

 private:
  const Target target_;
  ReportList reports_;

  // Used to track statistics for each ReportingEndpoint.
  // The endpoint is uniquely identified by the key in conjunction with
  // |target_.endpoint_url|. See ProcessOutcome().
  std::map<ReportingEndpointGroupKey, int> reports_per_group_;
};

class ReportingDeliveryAgentImpl : public ReportingDeliveryAgent,
                                   public ReportingCacheObserver {
 public:
  ReportingDeliveryAgentImpl(ReportingContext* context,
                             const RandIntCallback& rand_callback)
      : context_(context),
        timer_(std::make_unique<base::OneShotTimer>()),
        endpoint_manager_(
            ReportingEndpointManager::Create(&context->policy(),
                                             &context->tick_clock(),
                                             context->delegate(),
                                             context->cache(),
                                             rand_callback)) {
    context_->AddCacheObserver(this);
  }

  // ReportingDeliveryAgent implementation:

  ~ReportingDeliveryAgentImpl() override {
    context_->RemoveCacheObserver(this);
  }

  void SetTimerForTesting(std::unique_ptr<base::OneShotTimer> timer) override {
    DCHECK(!timer_->IsRunning());
    timer_ = std::move(timer);
  }

  // ReportingCacheObserver implementation:
  void OnReportsUpdated() override {
    if (CacheHasReports() && !timer_->IsRunning()) {
      SendReports();
      StartTimer();
    }
  }

 private:
  bool CacheHasReports() {
    ReportList reports;
    context_->cache()->GetReports(&reports);
    return !reports.empty();
  }

  void StartTimer() {
    timer_->Start(FROM_HERE, policy().delivery_interval,
                  base::BindOnce(&ReportingDeliveryAgentImpl::OnTimerFired,
                                 base::Unretained(this)));
  }

  void OnTimerFired() {
    if (CacheHasReports()) {
      SendReports();
      StartTimer();
    }
  }

  void SendReports() {
    ReportList reports = cache()->GetReportsToDeliver();
    if (reports.empty())
      return;

    // First determine which origins we're allowed to upload reports about.
    std::set<url::Origin> report_origins;
    for (const ReportingReport* report : reports) {
      report_origins.insert(url::Origin::Create(report->url));
    }
    delegate()->CanSendReports(
        std::move(report_origins),
        base::BindOnce(&ReportingDeliveryAgentImpl::OnSendPermissionsChecked,
                       weak_factory_.GetWeakPtr(), std::move(reports)));
  }

  void OnSendPermissionsChecked(ReportList reports,
                                std::set<url::Origin> allowed_report_origins) {
    DCHECK(!reports.empty());
    std::map<Delivery::Target, std::unique_ptr<Delivery>> deliveries;

    // Sort by group key
    std::sort(reports.begin(), reports.end(), &CompareReportGroupKeys);

    // Iterate over "buckets" of reports with the same group key.
    for (auto bucket_it = reports.begin(); bucket_it != reports.end();) {
      auto bucket_start = bucket_it;
      // Set the iterator to the beginning of the next group bucket.
      bucket_it = std::upper_bound(bucket_it, reports.end(), *bucket_it,
                                   &CompareReportGroupKeys);

      // Skip this group if we don't have origin permissions for this origin.
      const ReportingEndpointGroupKey& report_group_key =
          (*bucket_start)->GetGroupKey();
      if (!base::Contains(allowed_report_origins, report_group_key.origin))
        continue;

      // Skip this group if there is already a pending upload for it.
      // We don't allow multiple concurrent uploads for the same group.
      if (base::Contains(pending_groups_, report_group_key))
        continue;

      // Find an endpoint to deliver these reports to.
      const ReportingEndpoint endpoint =
          endpoint_manager_->FindEndpointForDelivery(report_group_key);
      // TODO(chlily): Remove reports for which there are no valid delivery
      // endpoints.
      if (!endpoint)
        continue;

      pending_groups_.insert(report_group_key);

      // Add the reports to the appropriate delivery.
      Delivery::Target target(report_group_key.network_isolation_key,
                              report_group_key.origin, endpoint.info.url);
      auto delivery_it = deliveries.find(target);
      if (delivery_it == deliveries.end()) {
        bool inserted;
        auto new_delivery = std::make_unique<Delivery>(target);
        std::tie(delivery_it, inserted) = deliveries.insert(
            std::make_pair(std::move(target), std::move(new_delivery)));
        DCHECK(inserted);
      }
      delivery_it->second->AddReports(endpoint, bucket_start, bucket_it);
    }

    // Keep track of which of these reports we don't queue for delivery; we'll
    // need to mark them as not-pending.
    std::set<const ReportingReport*> undelivered_reports(reports.begin(),
                                                         reports.end());

    // Start an upload for each delivery.
    for (auto& target_and_delivery : deliveries) {
      const Delivery::Target& target = target_and_delivery.first;
      std::unique_ptr<Delivery>& delivery = target_and_delivery.second;

      int max_depth = 0;
      for (const ReportingReport* report : delivery->reports()) {
        undelivered_reports.erase(report);
        max_depth = std::max(report->depth, max_depth);
      }

      std::string upload_data =
          SerializeReports(delivery->reports(), tick_clock().NowTicks());

      // TODO: Calculate actual max depth.
      uploader()->StartUpload(
          target.origin, target.endpoint_url, target.network_isolation_key,
          upload_data, max_depth,
          base::BindOnce(&ReportingDeliveryAgentImpl::OnUploadComplete,
                         weak_factory_.GetWeakPtr(), std::move(delivery)));
    }

    cache()->ClearReportsPending(
        {undelivered_reports.begin(), undelivered_reports.end()});
  }

  void OnUploadComplete(std::unique_ptr<Delivery> delivery,
                        ReportingUploader::Outcome outcome) {
    bool success = outcome == ReportingUploader::Outcome::SUCCESS;
    delivery->ProcessOutcome(cache(), success);

    endpoint_manager_->InformOfEndpointRequest(
        delivery->network_isolation_key(), delivery->endpoint_url(), success);

    // TODO(chlily): This leaks information across NIKs. If the endpoint URL is
    // configured for both NIK1 and NIK2, and it responds with a 410 on a NIK1
    // connection, then the change in configuration will be detectable on a NIK2
    // connection.
    if (outcome == ReportingUploader::Outcome::REMOVE_ENDPOINT)
      cache()->RemoveEndpointsForUrl(delivery->endpoint_url());

    for (const ReportingReport* report : delivery->reports()) {
      pending_groups_.erase(report->GetGroupKey());
    }

    cache()->ClearReportsPending(delivery->reports());
  }

  const ReportingPolicy& policy() const { return context_->policy(); }
  const base::TickClock& tick_clock() const { return context_->tick_clock(); }
  ReportingDelegate* delegate() { return context_->delegate(); }
  ReportingCache* cache() { return context_->cache(); }
  ReportingUploader* uploader() { return context_->uploader(); }

  ReportingContext* context_;

  std::unique_ptr<base::OneShotTimer> timer_;

  // Tracks endpoint groups for which there is a pending delivery running.
  std::set<ReportingEndpointGroupKey> pending_groups_;

  std::unique_ptr<ReportingEndpointManager> endpoint_manager_;

  base::WeakPtrFactory<ReportingDeliveryAgentImpl> weak_factory_{this};

  DISALLOW_COPY_AND_ASSIGN(ReportingDeliveryAgentImpl);
};

}  // namespace

// static
std::unique_ptr<ReportingDeliveryAgent> ReportingDeliveryAgent::Create(
    ReportingContext* context,
    const RandIntCallback& rand_callback) {
  return std::make_unique<ReportingDeliveryAgentImpl>(context, rand_callback);
}

ReportingDeliveryAgent::~ReportingDeliveryAgent() = default;

}  // namespace net
