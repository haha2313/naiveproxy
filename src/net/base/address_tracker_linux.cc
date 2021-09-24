// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/base/address_tracker_linux.h"

#include <errno.h>
#include <linux/if.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <utility>

#include "base/callback_helpers.h"
#include "base/check.h"
#include "base/files/scoped_file.h"
#include "base/logging.h"
#include "base/posix/eintr_wrapper.h"
#include "base/task/current_thread.h"
#include "base/threading/scoped_blocking_call.h"
#include "build/build_config.h"
#include "net/base/network_interfaces_linux.h"
#include "third_party/abseil-cpp/absl/types/optional.h"

#if defined(OS_ANDROID)
#include "base/android/build_info.h"
#endif

namespace net {
namespace internal {

namespace {

// Some kernel functions such as wireless_send_event and rtnetlink_ifinfo_prep
// may send spurious messages over rtnetlink. RTM_NEWLINK messages where
// ifi_change == 0 and rta_type == IFLA_WIRELESS should be ignored.
bool IgnoreWirelessChange(const struct ifinfomsg* msg, int length) {
  for (const struct rtattr* attr = IFLA_RTA(msg); RTA_OK(attr, length);
       attr = RTA_NEXT(attr, length)) {
    if (attr->rta_type == IFLA_WIRELESS && msg->ifi_change == 0)
      return true;
  }
  return false;
}

// Retrieves address from NETLINK address message.
// Sets |really_deprecated| for IPv6 addresses with preferred lifetimes of 0.
// Precondition: |header| must already be validated with NLMSG_OK.
bool GetAddress(const struct nlmsghdr* header,
                int header_length,
                IPAddress* out,
                bool* really_deprecated) {
  if (really_deprecated)
    *really_deprecated = false;

  // Extract the message and update |header_length| to be the number of
  // remaining bytes.
  const struct ifaddrmsg* msg =
      reinterpret_cast<const struct ifaddrmsg*>(NLMSG_DATA(header));
  header_length -= NLMSG_HDRLEN;

  size_t address_length = 0;
  switch (msg->ifa_family) {
    case AF_INET:
      address_length = IPAddress::kIPv4AddressSize;
      break;
    case AF_INET6:
      address_length = IPAddress::kIPv6AddressSize;
      break;
    default:
      // Unknown family.
      return false;
  }
  // Use IFA_ADDRESS unless IFA_LOCAL is present. This behavior here is based on
  // getaddrinfo in glibc (check_pf.c). Judging from kernel implementation of
  // NETLINK, IPv4 addresses have only the IFA_ADDRESS attribute, while IPv6
  // have the IFA_LOCAL attribute.
  uint8_t* address = NULL;
  uint8_t* local = NULL;
  int length = IFA_PAYLOAD(header);
  if (length > header_length) {
    LOG(ERROR) << "ifaddrmsg length exceeds bounds";
    return false;
  }
  for (const struct rtattr* attr =
           reinterpret_cast<const struct rtattr*>(IFA_RTA(msg));
       RTA_OK(attr, length); attr = RTA_NEXT(attr, length)) {
    switch (attr->rta_type) {
      case IFA_ADDRESS:
        if (RTA_PAYLOAD(attr) < address_length) {
          LOG(ERROR) << "attr does not have enough bytes to read an address";
          return false;
        }
        address = reinterpret_cast<uint8_t*>(RTA_DATA(attr));
        break;
      case IFA_LOCAL:
        if (RTA_PAYLOAD(attr) < address_length) {
          LOG(ERROR) << "attr does not have enough bytes to read an address";
          return false;
        }
        local = reinterpret_cast<uint8_t*>(RTA_DATA(attr));
        break;
      case IFA_CACHEINFO: {
        if (RTA_PAYLOAD(attr) < sizeof(struct ifa_cacheinfo)) {
          LOG(ERROR)
              << "attr does not have enough bytes to read an ifa_cacheinfo";
          return false;
        }
        const struct ifa_cacheinfo* cache_info =
            reinterpret_cast<const struct ifa_cacheinfo*>(RTA_DATA(attr));
        if (really_deprecated)
          *really_deprecated = (cache_info->ifa_prefered == 0);
      } break;
      default:
        break;
    }
  }
  if (local)
    address = local;
  if (!address)
    return false;
  *out = IPAddress(address, address_length);
  return true;
}

// SafelyCastNetlinkMsgData<T> performs a bounds check before casting |header|'s
// data to a |T*|. When the bounds check fails, returns nullptr.
template <typename T>
T* SafelyCastNetlinkMsgData(const struct nlmsghdr* header, int length) {
  DCHECK(NLMSG_OK(header, static_cast<__u32>(length)));
  if (length <= 0 || static_cast<size_t>(length) < NLMSG_HDRLEN + sizeof(T))
    return nullptr;
  return reinterpret_cast<const T*>(NLMSG_DATA(header));
}

}  // namespace

// static
char* AddressTrackerLinux::GetInterfaceName(int interface_index, char* buf) {
  memset(buf, 0, IFNAMSIZ);
  base::ScopedFD ioctl_socket = GetSocketForIoctl();
  if (!ioctl_socket.is_valid())
    return buf;

  struct ifreq ifr = {};
  ifr.ifr_ifindex = interface_index;

  if (ioctl(ioctl_socket.get(), SIOCGIFNAME, &ifr) == 0)
    strncpy(buf, ifr.ifr_name, IFNAMSIZ - 1);
  return buf;
}

AddressTrackerLinux::AddressTrackerLinux()
    : get_interface_name_(GetInterfaceName),
      address_callback_(base::DoNothing()),
      link_callback_(base::DoNothing()),
      tunnel_callback_(base::DoNothing()),
      ignored_interfaces_(),
      connection_type_initialized_(false),
      connection_type_initialized_cv_(&connection_type_lock_),
      current_connection_type_(NetworkChangeNotifier::CONNECTION_NONE),
      tracking_(false),
      threads_waiting_for_connection_type_initialization_(0) {}

AddressTrackerLinux::AddressTrackerLinux(
    const base::RepeatingClosure& address_callback,
    const base::RepeatingClosure& link_callback,
    const base::RepeatingClosure& tunnel_callback,
    const std::unordered_set<std::string>& ignored_interfaces)
    : get_interface_name_(GetInterfaceName),
      address_callback_(address_callback),
      link_callback_(link_callback),
      tunnel_callback_(tunnel_callback),
      ignored_interfaces_(ignored_interfaces),
      connection_type_initialized_(false),
      connection_type_initialized_cv_(&connection_type_lock_),
      current_connection_type_(NetworkChangeNotifier::CONNECTION_NONE),
      tracking_(true),
      threads_waiting_for_connection_type_initialization_(0) {
  DCHECK(!address_callback.is_null());
  DCHECK(!link_callback.is_null());
}

AddressTrackerLinux::~AddressTrackerLinux() = default;

void AddressTrackerLinux::Init() {
#if defined(OS_ANDROID)
  // RTM_GETLINK stopped working in Android 11 (see
  // https://developer.android.com/preview/privacy/mac-address),
  // so AddressTrackerLinux should not be used in later versions
  // of Android.  Chromium code doesn't need it past Android P.
  DCHECK_LT(base::android::BuildInfo::GetInstance()->sdk_int(),
            base::android::SDK_VERSION_P);
#endif
  netlink_fd_.reset(socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE));
  if (!netlink_fd_.is_valid()) {
    PLOG(ERROR) << "Could not create NETLINK socket";
    AbortAndForceOnline();
    return;
  }

  int rv;

  if (tracking_) {
    // Request notifications.
    struct sockaddr_nl addr = {};
    addr.nl_family = AF_NETLINK;
    addr.nl_pid = 0;  // Let the kernel select a unique value.
    // TODO(szym): Track RTMGRP_LINK as well for ifi_type,
    // http://crbug.com/113993
    addr.nl_groups =
        RTMGRP_IPV4_IFADDR | RTMGRP_IPV6_IFADDR | RTMGRP_NOTIFY | RTMGRP_LINK;
    rv = bind(netlink_fd_.get(), reinterpret_cast<struct sockaddr*>(&addr),
              sizeof(addr));
    if (rv < 0) {
      PLOG(ERROR) << "Could not bind NETLINK socket";
      AbortAndForceOnline();
      return;
    }
  }

  // Request dump of addresses.
  struct sockaddr_nl peer = {};
  peer.nl_family = AF_NETLINK;

  struct {
    struct nlmsghdr header;
    struct rtgenmsg msg;
  } request = {};

  request.header.nlmsg_len = NLMSG_LENGTH(sizeof(request.msg));
  request.header.nlmsg_type = RTM_GETADDR;
  request.header.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
  request.header.nlmsg_pid = 0;  // This field is opaque to netlink.
  request.msg.rtgen_family = AF_UNSPEC;

  rv = HANDLE_EINTR(
      sendto(netlink_fd_.get(), &request, request.header.nlmsg_len, 0,
             reinterpret_cast<struct sockaddr*>(&peer), sizeof(peer)));
  if (rv < 0) {
    PLOG(ERROR) << "Could not send NETLINK request";
    AbortAndForceOnline();
    return;
  }

  // Consume pending message to populate the AddressMap, but don't notify.
  // Sending another request without first reading responses results in EBUSY.
  bool address_changed;
  bool link_changed;
  bool tunnel_changed;
  ReadMessages(&address_changed, &link_changed, &tunnel_changed);

  // Request dump of link state
  request.header.nlmsg_type = RTM_GETLINK;

  rv = HANDLE_EINTR(
      sendto(netlink_fd_.get(), &request, request.header.nlmsg_len, 0,
             reinterpret_cast<struct sockaddr*>(&peer), sizeof(peer)));
  if (rv < 0) {
    PLOG(ERROR) << "Could not send NETLINK request";
    AbortAndForceOnline();
    return;
  }

  // Consume pending message to populate links_online_, but don't notify.
  ReadMessages(&address_changed, &link_changed, &tunnel_changed);
  {
    AddressTrackerAutoLock lock(*this, connection_type_lock_);
    connection_type_initialized_ = true;
    connection_type_initialized_cv_.Broadcast();
  }

  if (tracking_) {
    watcher_ = base::FileDescriptorWatcher::WatchReadable(
        netlink_fd_.get(),
        base::BindRepeating(&AddressTrackerLinux::OnFileCanReadWithoutBlocking,
                            base::Unretained(this)));
  }
}

bool AddressTrackerLinux::DidTrackingInitSucceedForTesting() const {
  CHECK(tracking_);
  return watcher_ != nullptr;
}

void AddressTrackerLinux::AbortAndForceOnline() {
  watcher_.reset();
  netlink_fd_.reset();
  AddressTrackerAutoLock lock(*this, connection_type_lock_);
  current_connection_type_ = NetworkChangeNotifier::CONNECTION_UNKNOWN;
  connection_type_initialized_ = true;
  connection_type_initialized_cv_.Broadcast();
}

AddressTrackerLinux::AddressMap AddressTrackerLinux::GetAddressMap() const {
  AddressTrackerAutoLock lock(*this, address_map_lock_);
  return address_map_;
}

std::unordered_set<int> AddressTrackerLinux::GetOnlineLinks() const {
  AddressTrackerAutoLock lock(*this, online_links_lock_);
  return online_links_;
}

bool AddressTrackerLinux::IsInterfaceIgnored(int interface_index) const {
  if (ignored_interfaces_.empty())
    return false;

  char buf[IFNAMSIZ] = {0};
  const char* interface_name = get_interface_name_(interface_index, buf);
  return ignored_interfaces_.find(interface_name) != ignored_interfaces_.end();
}

NetworkChangeNotifier::ConnectionType
AddressTrackerLinux::GetCurrentConnectionType() {
  // http://crbug.com/125097
  base::ScopedAllowBaseSyncPrimitivesOutsideBlockingScope allow_wait;
  AddressTrackerAutoLock lock(*this, connection_type_lock_);
  // Make sure the initial connection type is set before returning.
  threads_waiting_for_connection_type_initialization_++;
  while (!connection_type_initialized_) {
    connection_type_initialized_cv_.Wait();
  }
  threads_waiting_for_connection_type_initialization_--;
  return current_connection_type_;
}

void AddressTrackerLinux::ReadMessages(bool* address_changed,
                                       bool* link_changed,
                                       bool* tunnel_changed) {
  *address_changed = false;
  *link_changed = false;
  *tunnel_changed = false;
  char buffer[4096];
  bool first_loop = true;
  {
    absl::optional<base::ScopedBlockingCall> blocking_call;
    if (tracking_) {
      // If the loop below takes a long time to run, a new thread should added
      // to the current thread pool to ensure forward progress of all tasks.
      blocking_call.emplace(FROM_HERE, base::BlockingType::MAY_BLOCK);
    }

    for (;;) {
      int rv = HANDLE_EINTR(recv(netlink_fd_.get(), buffer, sizeof(buffer),
                                 // Block the first time through loop.
                                 first_loop ? 0 : MSG_DONTWAIT));
      first_loop = false;
      if (rv == 0) {
        LOG(ERROR) << "Unexpected shutdown of NETLINK socket.";
        return;
      }
      if (rv < 0) {
        if ((errno == EAGAIN) || (errno == EWOULDBLOCK))
          break;
        PLOG(ERROR) << "Failed to recv from netlink socket";
        return;
      }
      HandleMessage(buffer, rv, address_changed, link_changed, tunnel_changed);
    }
  }
  if (*link_changed || *address_changed)
    UpdateCurrentConnectionType();
}

void AddressTrackerLinux::HandleMessage(const char* buffer,
                                        int length,
                                        bool* address_changed,
                                        bool* link_changed,
                                        bool* tunnel_changed) {
  DCHECK(buffer);
  // Note that NLMSG_NEXT decrements |length| to reflect the number of bytes
  // remaining in |buffer|.
  for (const struct nlmsghdr* header =
           reinterpret_cast<const struct nlmsghdr*>(buffer);
       length >= 0 && NLMSG_OK(header, static_cast<__u32>(length));
       header = NLMSG_NEXT(header, length)) {
    // The |header| pointer should never precede |buffer|.
    DCHECK_LE(buffer, reinterpret_cast<const char*>(header));
    switch (header->nlmsg_type) {
      case NLMSG_DONE:
        return;
      case NLMSG_ERROR: {
        const struct nlmsgerr* msg =
            SafelyCastNetlinkMsgData<const struct nlmsgerr>(header, length);
        if (msg == nullptr)
          return;
        LOG(ERROR) << "Unexpected netlink error " << msg->error << ".";
      } return;
      case RTM_NEWADDR: {
        IPAddress address;
        bool really_deprecated;
        const struct ifaddrmsg* msg =
            SafelyCastNetlinkMsgData<const struct ifaddrmsg>(header, length);
        if (msg == nullptr)
          return;
        if (IsInterfaceIgnored(msg->ifa_index))
          break;
        if (GetAddress(header, length, &address, &really_deprecated)) {
          struct ifaddrmsg msg_copy = *msg;
          AddressTrackerAutoLock lock(*this, address_map_lock_);
          // Routers may frequently (every few seconds) output the IPv6 ULA
          // prefix which can cause the linux kernel to frequently output two
          // back-to-back messages, one without the deprecated flag and one with
          // the deprecated flag but both with preferred lifetimes of 0. Avoid
          // interpretting this as an actual change by canonicalizing the two
          // messages by setting the deprecated flag based on the preferred
          // lifetime also.  http://crbug.com/268042
          if (really_deprecated)
            msg_copy.ifa_flags |= IFA_F_DEPRECATED;
          // Only indicate change if the address is new or ifaddrmsg info has
          // changed.
          auto it = address_map_.find(address);
          if (it == address_map_.end()) {
            address_map_.insert(it, std::make_pair(address, msg_copy));
            *address_changed = true;
          } else if (memcmp(&it->second, &msg_copy, sizeof(msg_copy))) {
            it->second = msg_copy;
            *address_changed = true;
          }
        }
      } break;
      case RTM_DELADDR: {
        IPAddress address;
        const struct ifaddrmsg* msg =
            SafelyCastNetlinkMsgData<const struct ifaddrmsg>(header, length);
        if (msg == nullptr)
          return;
        if (IsInterfaceIgnored(msg->ifa_index))
          break;
        if (GetAddress(header, length, &address, nullptr)) {
          AddressTrackerAutoLock lock(*this, address_map_lock_);
          if (address_map_.erase(address))
            *address_changed = true;
        }
      } break;
      case RTM_NEWLINK: {
        const struct ifinfomsg* msg =
            SafelyCastNetlinkMsgData<const struct ifinfomsg>(header, length);
        if (msg == nullptr)
          return;
        if (IsInterfaceIgnored(msg->ifi_index))
          break;
        if (IgnoreWirelessChange(msg, IFLA_PAYLOAD(header))) {
          VLOG(2) << "Ignoring RTM_NEWLINK message";
          break;
        }
        if (!(msg->ifi_flags & IFF_LOOPBACK) && (msg->ifi_flags & IFF_UP) &&
            (msg->ifi_flags & IFF_LOWER_UP) && (msg->ifi_flags & IFF_RUNNING)) {
          AddressTrackerAutoLock lock(*this, online_links_lock_);
          if (online_links_.insert(msg->ifi_index).second) {
            *link_changed = true;
            if (IsTunnelInterface(msg->ifi_index))
              *tunnel_changed = true;
          }
        } else {
          AddressTrackerAutoLock lock(*this, online_links_lock_);
          if (online_links_.erase(msg->ifi_index)) {
            *link_changed = true;
            if (IsTunnelInterface(msg->ifi_index))
              *tunnel_changed = true;
          }
        }
      } break;
      case RTM_DELLINK: {
        const struct ifinfomsg* msg =
            SafelyCastNetlinkMsgData<const struct ifinfomsg>(header, length);
        if (msg == nullptr)
          return;
        if (IsInterfaceIgnored(msg->ifi_index))
          break;
        AddressTrackerAutoLock lock(*this, online_links_lock_);
        if (online_links_.erase(msg->ifi_index)) {
          *link_changed = true;
          if (IsTunnelInterface(msg->ifi_index))
            *tunnel_changed = true;
        }
      } break;
      default:
        break;
    }
  }
}

void AddressTrackerLinux::OnFileCanReadWithoutBlocking() {
  bool address_changed;
  bool link_changed;
  bool tunnel_changed;
  ReadMessages(&address_changed, &link_changed, &tunnel_changed);
  if (address_changed)
    address_callback_.Run();
  if (link_changed)
    link_callback_.Run();
  if (tunnel_changed)
    tunnel_callback_.Run();
}

bool AddressTrackerLinux::IsTunnelInterface(int interface_index) const {
  char buf[IFNAMSIZ] = {0};
  return IsTunnelInterfaceName(get_interface_name_(interface_index, buf));
}

// static
bool AddressTrackerLinux::IsTunnelInterfaceName(const char* name) {
  // Linux kernel drivers/net/tun.c uses "tun" name prefix.
  return strncmp(name, "tun", 3) == 0;
}

void AddressTrackerLinux::UpdateCurrentConnectionType() {
  AddressTrackerLinux::AddressMap address_map = GetAddressMap();
  std::unordered_set<int> online_links = GetOnlineLinks();

  // Strip out tunnel interfaces from online_links
  for (auto it = online_links.cbegin(); it != online_links.cend();) {
    if (IsTunnelInterface(*it)) {
      it = online_links.erase(it);
    } else {
      ++it;
    }
  }

  NetworkInterfaceList networks;
  NetworkChangeNotifier::ConnectionType type =
      NetworkChangeNotifier::CONNECTION_NONE;
  if (GetNetworkListImpl(&networks, 0, online_links, address_map,
                         get_interface_name_)) {
    type = NetworkChangeNotifier::ConnectionTypeFromInterfaceList(networks);
  } else {
    type = online_links.empty() ? NetworkChangeNotifier::CONNECTION_NONE
                                : NetworkChangeNotifier::CONNECTION_UNKNOWN;
  }

  AddressTrackerAutoLock lock(*this, connection_type_lock_);
  current_connection_type_ = type;
}

int AddressTrackerLinux::GetThreadsWaitingForConnectionTypeInitForTesting() {
  AddressTrackerAutoLock lock(*this, connection_type_lock_);
  return threads_waiting_for_connection_type_initialization_;
}

AddressTrackerLinux::AddressTrackerAutoLock::AddressTrackerAutoLock(
    const AddressTrackerLinux& tracker,
    base::Lock& lock)
    : tracker_(tracker), lock_(lock) {
  if (tracker_.tracking_) {
    lock_.Acquire();
  } else {
    DCHECK(tracker_.thread_checker_.CalledOnValidThread());
  }
}

AddressTrackerLinux::AddressTrackerAutoLock::~AddressTrackerAutoLock() {
  if (tracker_.tracking_) {
    lock_.AssertAcquired();
    lock_.Release();
  }
}

}  // namespace internal
}  // namespace net
