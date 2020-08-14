#!/bin/sh
# Copyright 2018 The Chromium OS Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

set -- "$@" --log-level="${SHILL_LOG_LEVEL}" --log-scopes="${SHILL_LOG_SCOPES}"

if [ -n "${SHILL_LOG_VMODULES}" ]; then
  set -- "$@" --vmodule="${SHILL_LOG_VMODULES}"
fi

if [ -n "${BLOCKED_DEVICES}" ] && [ -n "${SHILL_TEST_DEVICES}" ]; then
  set -- "$@" --devices-blocked="${BLOCKED_DEVICES},${SHILL_TEST_DEVICES}"
elif [ -n "${BLOCKED_DEVICES}" ]; then
  set -- "$@" --devices-blocked="${BLOCKED_DEVICES}"
elif [ -n "${SHILL_TEST_DEVICES}" ]; then
  set -- "$@" --devices-blocked="${SHILL_TEST_DEVICES}"
fi

if [ -n "${ALLOWED_DEVICES}" ]; then
  set -- "$@" --devices-allowed="${ALLOWED_DEVICES}"
fi

if [ -n "${SHILL_PASSIVE_MODE}" ]; then
  set -- "$@" --passive-mode
fi

if [ -n "${SHILL_PREPEND_DNS_SERVERS}" ]; then
  set -- "$@" --prepend-dns-servers="${SHILL_PREPEND_DNS_SERVERS}"
fi

if [ -n "${SHILL_ACCEPT_HOSTNAME_FROM}" ]; then
  set -- "$@" --accept-hostname-from="${SHILL_ACCEPT_HOSTNAME_FROM}"
fi

if [ -n "${SHILL_MINIMUM_MTU}" ]; then
  set -- "$@" --minimum-mtu="${SHILL_MINIMUM_MTU}"
fi

if [ -n "${DHCPV6_ENABLED_DEVICES}" ]; then
  set -- "$@" --dhcpv6-enabled-devices="${DHCPV6_ENABLED_DEVICES}"
fi

# If OOBE has not completed (i.e. EULA not agreed to), do not run
# portal checks
if [ ! -f /home/chronos/.oobe_completed ]; then
  set -- "$@" --portal-list=
fi

# Run shill as shill user/group in a minijail:
#   -G so shill programs can inherit supplementary groups.
#   -n to run shill with no_new_privs.
#   -B 20 to avoid locking SECURE_KEEP_CAPS flag.
#   -c for runtime capabilities:
#     CAP_WAKE_ALARM | CAP_NET_RAW | CAP_NET_ADMIN | CAP_NET_BROADCAST |
#     CAP_NET_BIND_SERVICE | CAP_SETPCAP | CAP_SETUID | CAP_SETGID | CAP_KILL
#   --ambient so child processes can inherit runtime capabilities.
#   -i to lose the dangling minijail0 process.
exec /sbin/minijail0 -u shill -g shill -G -n -B 20 -c 800003de0 --ambient -i \
     -- /usr/bin/shill "$@"
