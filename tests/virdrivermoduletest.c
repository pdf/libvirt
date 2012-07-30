/*
 * Copyright (C) 2012 Red Hat, Inc.
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
 * Author: Daniel P. Berrange <berrange@redhat.com>
 */

#include <config.h>

#include "testutils.h"
#include "util.h"
#include "virterror_internal.h"
#include "memory.h"
#include "logging.h"
#include "driver.h"

#define VIR_FROM_THIS VIR_FROM_NONE

struct testDriverData {
    const char *name;
    const char *dep1;
};


static int testDriverModule(const void *args)
{
    const struct testDriverData *data = args;

    if (data->dep1 &&
        !virDriverLoadModule(data->dep1))
        return -1;

    if (!virDriverLoadModule(data->name))
        return -1;

    return 0;
}


static int
mymain(void)
{
    int ret = 0;

#define TEST(name, dep1)                                                \
    do  {                                                               \
        const struct testDriverData data = { name, dep1 };              \
        if (virtTestRun("Test driver " # name,  1, testDriverModule, &data) < 0) \
            ret = -1;                                                   \
    } while (0)

    virDriverModuleInitialize(abs_builddir "/../src/.libs");

#ifdef WITH_NETWORK
    TEST("network", NULL);
#endif
#ifdef WITH_STORAGE
    TEST("storage", NULL);
#endif
#ifdef WITH_NODE_DEVICES
    TEST("nodedev", NULL);
#endif
#ifdef WITH_SECRETS
    TEST("secret", NULL);
#endif
#ifdef WITH_NWFILTER
    TEST("nwfilter", NULL);
#endif
#ifdef WITH_NETCF
    TEST("interface", NULL);
#endif
#ifdef WITH_QEMU
    TEST("qemu", "network");
#endif
#ifdef WITH_LXC
    TEST("lxc", "network");
#endif
#ifdef WITH_UML
    TEST("uml", NULL);
#endif
#ifdef WITH_XEN
    TEST("xen", NULL);
#endif
#ifdef WITH_LIBXL
    TEST("libxl", NULL);
#endif

    return ret==0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

VIRT_TEST_MAIN(mymain)
