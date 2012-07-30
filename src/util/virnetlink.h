/*
 * Copyright (C) 2010-2012 Red Hat, Inc.
 * Copyright (C) 2010-2012 IBM Corporation
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library;  If not, see
 * <http://www.gnu.org/licenses/>.
 */

#ifndef __VIR_NETLINK_H__
# define __VIR_NETLINK_H__

# include "config.h"
# include "internal.h"
# include "virmacaddr.h"

# include <stdint.h>

# if defined(__linux__) && defined(HAVE_LIBNL)

#  include <netlink/msg.h>

# else

struct nl_msg;
struct sockaddr_nl;
struct nlattr;

# endif /* __linux__ */

int virNetlinkStartup(void);
void virNetlinkShutdown(void);

int virNetlinkCommand(struct nl_msg *nl_msg,
                      unsigned char **respbuf, unsigned int *respbuflen,
                      uint32_t src_port, uint32_t dst_port);

typedef void (*virNetlinkEventHandleCallback)(unsigned char *msg, int length, struct sockaddr_nl *peer, bool *handled, void *opaque);

typedef void (*virNetlinkEventRemoveCallback)(int watch, const virMacAddrPtr macaddr, void *opaque);

/**
 * stopNetlinkEventServer: stop the monitor to receive netlink messages for libvirtd
 */
int virNetlinkEventServiceStop(void);

/**
 * startNetlinkEventServer: start a monitor to receive netlink messages for libvirtd
 */
int virNetlinkEventServiceStart(void);

/**
 * virNetlinkEventServiceIsRunning: returns if the netlink event service is running.
 */
bool virNetlinkEventServiceIsRunning(void);

/**
 * virNetlinkEventServiceLocalPid: returns nl_pid used to bind() netlink socket
 */
int virNetlinkEventServiceLocalPid(void);

/**
 * virNetlinkEventAddClient: register a callback for handling of netlink messages
 */
int virNetlinkEventAddClient(virNetlinkEventHandleCallback handleCB,
                             virNetlinkEventRemoveCallback removeCB,
                             void *opaque, const virMacAddrPtr macaddr);

/**
 * virNetlinkEventRemoveClient: unregister a callback from a netlink monitor
 */
int virNetlinkEventRemoveClient(int watch, const virMacAddrPtr macaddr);

#endif /* __VIR_NETLINK_H__ */
