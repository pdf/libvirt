/*
 * virnodesuspend.h: Support for suspending a node (host machine)
 *
 * Copyright (C) 2011 Srivatsa S. Bhat <srivatsa.bhat@linux.vnet.ibm.com>
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
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307  USA
 *
 */


#ifndef __VIR_NODE_SUSPEND_H__
# define __VIR_NODE_SUSPEND_H__

# include "internal.h"

int nodeSuspendForDuration(virConnectPtr conn,
                           unsigned int target,
                           unsigned long long duration,
                           unsigned int flags);

int virNodeSuspendInit(void);
int virNodeSuspendGetTargetMask(unsigned int *bitmask);

#endif /* __VIR_NODE_SUSPEND_H__ */
