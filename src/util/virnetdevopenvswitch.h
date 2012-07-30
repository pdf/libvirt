/*
 * Copyright (C) 2012 Nicira, Inc.
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
 *
 * Authors:
 *     Dan Wendlandt <dan@nicira.com>
 *     Kyle Mestery <kmestery@cisco.com>
 *     Ansis Atteka <aatteka@nicira.com>
 */

#ifndef __VIR_NETDEV_OPENVSWITCH_H__
# define __VIR_NETDEV_OPENVSWITCH_H__

# include "internal.h"
# include "util.h"
# include "virnetdevvportprofile.h"


int virNetDevOpenvswitchAddPort(const char *brname,
                                const char *ifname,
                                const virMacAddrPtr macaddr,
                                const unsigned char *vmuuid,
                                virNetDevVPortProfilePtr ovsport)
    ATTRIBUTE_NONNULL(1) ATTRIBUTE_NONNULL(2) ATTRIBUTE_NONNULL(3)
    ATTRIBUTE_RETURN_CHECK;

int virNetDevOpenvswitchRemovePort(const char *brname, const char *ifname)
    ATTRIBUTE_NONNULL(2) ATTRIBUTE_RETURN_CHECK;

#endif /* __VIR_NETDEV_OPENVSWITCH_H__ */
