/*
 * cpu_s390.c: CPU driver for s390(x) CPUs
 *
 * Copyright IBM Corp. 2012
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
 *      Thang Pham <thang.pham@us.ibm.com>
 */

#include <config.h>

#include "memory.h"
#include "cpu.h"


#define VIR_FROM_THIS VIR_FROM_CPU

static const char *archs[] = { "s390", "s390x" };

static union cpuData *
s390NodeData(void)
{
    union cpuData *data;

    if (VIR_ALLOC(data) < 0) {
        virReportOOMError();
        return NULL;
    }

    return data;
}


static int
s390Decode(virCPUDefPtr cpu ATTRIBUTE_UNUSED,
           const union cpuData *data ATTRIBUTE_UNUSED,
           const char **models ATTRIBUTE_UNUSED,
           unsigned int nmodels ATTRIBUTE_UNUSED,
           const char *preferred ATTRIBUTE_UNUSED)
{
    return 0;
}

static void
s390DataFree(union cpuData *data)
{
    VIR_FREE(data);
}

struct cpuArchDriver cpuDriverS390 = {
    .name = "s390",
    .arch = archs,
    .narch = ARRAY_CARDINALITY(archs),
    .compare    = NULL,
    .decode     = s390Decode,
    .encode     = NULL,
    .free       = s390DataFree,
    .nodeData   = s390NodeData,
    .guestData  = NULL,
    .baseline   = NULL,
    .update     = NULL,
    .hasFeature = NULL,
};
