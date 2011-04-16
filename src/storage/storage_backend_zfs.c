/*
 * storage_backend_zfs.c: storage backend for ZFS volume handling
 *
 * Copyright (C) 2011 Wikstrom Telephone Company
 * Copyright (C) 2007-2009 Red Hat, Inc.
 * Copyright (C) 2007-2008 Daniel P. Berrange
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
 * Author: Richard Laager <rlaager@wiktel.com>
 */

#include <config.h>

#include <sys/wait.h>
#include <sys/stat.h>
#include <stdio.h>
#include <regex.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#include "virterror_internal.h"
#include "storage_backend_zfs.h"
#include "storage_conf.h"
#include "util.h"
#include "memory.h"
#include "logging.h"
#include "files.h"

#define VIR_FROM_THIS VIR_FROM_STORAGE


static int
virStorageBackendZFSCheckPool(virConnectPtr conn ATTRIBUTE_UNUSED,
                              virStoragePoolObjPtr pool,
                              bool *isActive)
{
    const char *name = pool->def->source.name;
    const char *cmdargv[] = {
        ZPOOL, "status", name, NULL
    };
    const char *slash;
    char *pool_name = NULL;
    int status;

    if ((slash = (const char *)strchr(name, '/')) != NULL) {
        pool_name = strdup(name);
        if (pool_name == NULL) {
            virReportOOMError();
            return -1;
        }
        *(pool_name + (slash - name)) = '\0';
        cmdargv[2] = pool_name;
    }

    if (virRun(cmdargv, &status) < 0)
        goto cleanup;
    VIR_FREE(pool_name);

    *isActive = (status == 0);
    return 0;

 cleanup:
    VIR_FREE(pool_name);
    return -1;
}


static int
virStorageBackendZFSSetActive(virStoragePoolObjPtr pool,
                              int on)
{
    const char *name = pool->def->source.name;
    const char *cmdargv[] = {
        ZPOOL, "status", name, NULL
    };
    const char *slash;
    char *pool_name = NULL;
    int status;

    if ((slash = (const char *)strchr(name, '/')) != NULL) {
        pool_name = strdup(name);
        if (pool_name == NULL) {
            virReportOOMError();
            return -1;
        }
        *(pool_name + (slash - name)) = '\0';
        cmdargv[2] = pool_name;
    }

    if (virRun(cmdargv, &status) < 0)
        goto cleanup;

    /* If the pool is already in the correct state, exit.  Otherwise, the
     * import or export will fail.
     */
    if ((status == 0) == (on != 0))
        goto success;

    cmdargv[1] = on ? "import" : "export";

    if (virRun(cmdargv, NULL) < 0)
        goto cleanup;

 success:
    VIR_FREE(pool_name);
    return 0;

 cleanup:
    VIR_FREE(pool_name);
    return -1;
}


static int
virStorageBackendZFSMakeVol(virStoragePoolObjPtr pool,
                                char **const groups,
                                void *data)
{
    virStorageVolDefPtr vol = NULL;
    unsigned long long offset, size, length;

    /* See if we're only looking for a specific volume */
    if (data != NULL) {
        vol = data;
        if (STRNEQ(vol->name, groups[0]))
            return 0;
    }

    /* Or filling in more data on an existing volume */
    if (vol == NULL)
        vol = virStorageVolDefFindByName(pool, groups[0]);

    /* Or a completely new volume */
    if (vol == NULL) {
        if (VIR_ALLOC(vol) < 0) {
            virReportOOMError();
            return -1;
        }

        vol->type = VIR_STORAGE_VOL_BLOCK;

        if ((vol->name = strdup(groups[0])) == NULL) {
            virReportOOMError();
            virStorageVolDefFree(vol);
            return -1;
        }

        if (VIR_REALLOC_N(pool->volumes.objs,
                          pool->volumes.count + 1)) {
            virReportOOMError();
            virStorageVolDefFree(vol);
            return -1;
        }
        pool->volumes.objs[pool->volumes.count++] = vol;
    }

    if (vol->target.path == NULL) {
        if (virAsprintf(&vol->target.path, "%s/%s",
                        pool->def->target.path, vol->name) < 0) {
            virReportOOMError();
            virStorageVolDefFree(vol);
            return -1;
        }
    }

    if (groups[1] && !STREQ(groups[1], "")) {
        if (virAsprintf(&vol->backingStore.path, "%s/%s",
                        pool->def->target.path, groups[1]) < 0) {
            virReportOOMError();
            virStorageVolDefFree(vol);
            return -1;
        }

        vol->backingStore.format = VIR_STORAGE_POOL_LOGICAL_LVM2;
    }

    if (vol->key == NULL &&
        (vol->key = strdup(groups[2])) == NULL) {
        virReportOOMError();
        return -1;
    }

    if (virStorageBackendUpdateVolInfo(vol, 1) < 0)
        return -1;


    /* Finally fill in extents information */
    if (VIR_REALLOC_N(vol->source.extents,
                      vol->source.nextent + 1) < 0) {
        virReportOOMError();
        return -1;
    }

    if ((vol->source.extents[vol->source.nextent].path =
         strdup(groups[3])) == NULL) {
        virReportOOMError();
        return -1;
    }

    if (virStrToLong_ull(groups[4], NULL, 10, &offset) < 0) {
        virStorageReportError(VIR_ERR_INTERNAL_ERROR,
                              "%s", _("malformed volume extent offset value"));
        return -1;
    }
    if (virStrToLong_ull(groups[5], NULL, 10, &length) < 0) {
        virStorageReportError(VIR_ERR_INTERNAL_ERROR,
                              "%s", _("malformed volume extent length value"));
        return -1;
    }
    if (virStrToLong_ull(groups[6], NULL, 10, &size) < 0) {
        virStorageReportError(VIR_ERR_INTERNAL_ERROR,
                              "%s", _("malformed volume extent size value"));
        return -1;
    }

    vol->source.extents[vol->source.nextent].start = offset * size;
    vol->source.extents[vol->source.nextent].end = (offset * size) + length;
    vol->source.nextent++;

    return 0;
}

static int
virStorageBackendZFSFindVolumes(virStoragePoolObjPtr pool,
                                virStorageVolDefPtr vol)
{
    /*
     * # zfs get -rHp type POOL
     * POOL	type	filesystem	-
     * POOL/vol	type	volume	-
     * POOL/sub/vol	type	volume	-
     * POOL/sub/fs	type	filesystem	-
     */
    const char *regexes[] = {
        "^(\\S+)	type	volume"
    };
    int vars[] = {
        1
    };
    const char *const prog[] = {
        ZFS, "get", "-rHp", "type",
        pool->def->source.name, NULL
    };
    int exitstatus;

    if (virStorageBackendRunProgRegex(pool, prog, 1, regexes, vars,
                                      virStorageBackendZFSMakeVol,
                                      vol, &exitstatus) < 0) {
        // TODO: Do we need this here? If so, add it elsewhere?
        virStorageReportError(VIR_ERR_INTERNAL_ERROR,
                              "%s", _("zfs command failed"));
        return -1;
    }

    if (exitstatus != 0) {
        virStorageReportError(VIR_ERR_INTERNAL_ERROR,
                              _("zfs command failed with exitstatus %d"),
                              exitstatus);
        return -1;
    }

    return 0;
}

static int
virStorageBackendZFSRefreshPoolFunc(virStoragePoolObjPtr pool ATTRIBUTE_UNUSED,
                                    char **const groups,
                                    void *data ATTRIBUTE_UNUSED)
{
    if (STREQ(groups[0], "used")) {
        if (virStrToLong_ull(groups[1], NULL, 10, &pool->def->allocation) < 0)
            return -1;
    }
    if (STREQ(groups[0], "available")) {
        if (virStrToLong_ull(groups[1], NULL, 10, &pool->def->available) < 0)
            return -1;
        /* We asked for "used,available", so this is the second line. */
        pool->def->capacity = pool->def->allocation + pool->def->available;
    }

    return 0;
}


static int
virStorageBackendZFSFindPoolSourcesFunc(virStoragePoolObjPtr pool ATTRIBUTE_UNUSED,
                                        char **const groups,
                                        void *data)
{
    virStoragePoolSourceListPtr sourceList = data;
    char *name;
    char *slash;
    int format;
    int i;
    virStoragePoolSource *thisSource;

    name = strdup(groups[0]);
    if (name == NULL) {
        virReportOOMError();
        return -1;
    }

    /* Truncate to the last slash. */
    if ((slash = strrchr(name, '/')) != NULL) {
        format = VIR_STORAGE_POOL_ZFS_ZPL;
        *slash = '\0';
    } else
        format = VIR_STORAGE_POOL_ZFS_ZPOOL;

    /* If this pool/dataset has already been found, exit. */
    for (i = 0 ; i < sourceList->nsources; i++) {
        if (STREQ(sourceList->sources[i].name, name)) {
            VIR_FREE(name);
            return 0;
        }
    }

    if (!(thisSource = virStoragePoolSourceListNewSource(sourceList))) {
        virReportOOMError();
        VIR_FREE(name);
        return -1;
    }

    thisSource->name = name;
    thisSource->format = format;

    return 0;
}

/* Find all pools and also datasets that currently have one or more volumes as
 * direct children.
 */
static char *
virStorageBackendZFSFindPoolSources(virConnectPtr conn ATTRIBUTE_UNUSED,
                                    const char *srcSpec ATTRIBUTE_UNUSED,
                                    unsigned int flags ATTRIBUTE_UNUSED)
{
    /*
     * # zfs get -Hp type
     * POOL	type	filesystem	-
     * POOL/vol	type	volume	-
     * POOL/sub/vol	type	volume	-
     * POOL/sub/fs	type	filesystem	-
     *
     * # zpool list -H -o name
     * POOL
     */
    const char *regexes[] = {
        "^(\\S+)	type	volume"
    };
    int vars[] = {
        1
    };
    const char *const zfsargv[] = { ZFS, "get", "-Hp", "type", NULL };
    const char *const zpoolargv[] = { ZPOOL, "list", "-Ho", "name", NULL };
    int exitstatus;
    char *retval = NULL;
    virStoragePoolSourceList sourceList;
    int i;

    memset(&sourceList, 0, sizeof(sourceList));
    sourceList.type = VIR_STORAGE_POOL_ZFS;

    /* Find all volumes.  The callback will grab their parent pool/dataset. */
    if (virStorageBackendRunProgRegex(NULL, zfsargv, 1, regexes, vars,
                                      virStorageBackendZFSFindPoolSourcesFunc,
                                      &sourceList,
                                      &exitstatus) < 0 || exitstatus != 0)
        goto cleanup;

    /* Find all pools. */
    regexes[0] = "^(\\S+)";
    if (virStorageBackendRunProgRegex(NULL, zpoolargv, 1, regexes, vars,
                                      virStorageBackendZFSFindPoolSourcesFunc,
                                      &sourceList,
                                      &exitstatus) < 0 || exitstatus != 0)
        goto cleanup;

    retval = virStoragePoolSourceListFormat(&sourceList);
    if (retval == NULL) {
        virStorageReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                              _("failed to get source from sourceList"));
        goto cleanup;
    }

 cleanup:
    for (i = 0; i < sourceList.nsources; i++)
        virStoragePoolSourceFree(&sourceList.sources[i]);

    VIR_FREE(sourceList.sources);

    return retval;
}


static int
virStorageBackendZFSStartPool(virConnectPtr conn ATTRIBUTE_UNUSED,
                              virStoragePoolObjPtr pool)
{
    if (virStorageBackendZFSSetActive(pool, 1) < 0)
        return -1;

    return 0;
}


static int
virStorageBackendZFSRefreshPool(virConnectPtr conn ATTRIBUTE_UNUSED,
                                virStoragePoolObjPtr pool)
{
    /*
     * # zfs get -Hp used,available POOL
     * POOL	used	32076800	-
     * POOL	available	34507776	-
     */
    const char *regexes[] = {
        "^\\S+	(\\S+)	(\\S+)"
    };
    int vars[] = {
        2
    };
    const char *prog[] = {
        ZFS, "get", "-Hp", "used,available",
        pool->def->source.name, NULL
    };
    int exitstatus;

    /* I'm not sure if this is necessary for ZFS. */
    virFileWaitForDevices();

    /* Get list of all logical volumes */
    if (virStorageBackendZFSFindVolumes(pool, NULL) < 0) {
        virStoragePoolObjClearVols(pool);
        return -1;
    }

    if (virStorageBackendRunProgRegex(pool, prog, 1, regexes, vars,
                                      virStorageBackendZFSRefreshPoolFunc,
                                      NULL,
                                      &exitstatus) < 0 || exitstatus != 0) {
        virStoragePoolObjClearVols(pool);
        return -1;
    }

    return 0;
}


/*
 * This is actually relatively safe; if you happen to try to "stop" the
 * pool that your / is on, for instance, you will get failure like:
 * "cannot export 'tank': pool is busy"
 */
static int
virStorageBackendZFSStopPool(virConnectPtr conn ATTRIBUTE_UNUSED,
                                 virStoragePoolObjPtr pool)
{
    if (virStorageBackendZFSSetActive(pool, 0) < 0)
        return -1;

    return 0;
}


static int
virStorageBackendZFSDeletePool(virConnectPtr conn ATTRIBUTE_UNUSED,
                               virStoragePoolObjPtr pool,
                               unsigned int flags ATTRIBUTE_UNUSED)
{
    const char *name = pool->def->source.name;
    const char *cmdargv[] = {
        NULL, "destroy", name, NULL
    };

    if (strchr(name, '/') != NULL)
        cmdargv[0] = ZFS;
    else {
        cmdargv[0] = ZPOOL;
        /* zpools must be imported to be destroyed.  However, the libvirt API
         * requires pools to be inactive before it'll call deletePool.  So, we
         * reactivate the pool here. */ 
        if (virStorageBackendZFSSetActive(pool, 1) < 0)
            return -1;
    }

    if (virRun(cmdargv, NULL) < 0)
        return -1;

    return 0;
}


static int
virStorageBackendZFSDeleteVol(virConnectPtr conn,
                                  virStoragePoolObjPtr pool,
                                  virStorageVolDefPtr vol,
                                  unsigned int flags);


static int
virStorageBackendZFSCreateVol(virConnectPtr conn,
                                  virStoragePoolObjPtr pool,
                                  virStorageVolDefPtr vol)
{
    int fdret, fd = -1;
    char size[100];
    const char *cmdargvnew[] = {
        LVCREATE, "--name", vol->name, "-L", size,
        pool->def->target.path, NULL
    };
    const char *cmdargvsnap[] = {
        LVCREATE, "--name", vol->name, "-L", size,
        "-s", vol->backingStore.path, NULL
    };
    const char **cmdargv = cmdargvnew;

    if (vol->target.encryption != NULL) {
        virStorageReportError(VIR_ERR_NO_SUPPORT,
                              "%s", _("storage pool does not support encrypted "
                                      "volumes"));
        return -1;
    }

    if (vol->backingStore.path) {
        cmdargv = cmdargvsnap;
    }

    snprintf(size, sizeof(size)-1, "%lluK", VIR_DIV_UP(vol->capacity, 1024));
    size[sizeof(size)-1] = '\0';

    vol->type = VIR_STORAGE_VOL_BLOCK;

    if (vol->target.path != NULL) {
        /* A target path passed to CreateVol has no meaning */
        VIR_FREE(vol->target.path);
    }

    if (virAsprintf(&vol->target.path, "%s/%s",
                    pool->def->target.path,
                    vol->name) == -1) {
        virReportOOMError();
        return -1;
    }

    if (virRun(cmdargv, NULL) < 0)
        return -1;

    if ((fdret = virStorageBackendVolOpen(vol->target.path)) < 0)
        goto cleanup;
    fd = fdret;

    /* We can only chown/grp if root */
    if (getuid() == 0) {
        if (fchown(fd, vol->target.perms.uid, vol->target.perms.gid) < 0) {
            virReportSystemError(errno,
                                 _("cannot set file owner '%s'"),
                                 vol->target.path);
            goto cleanup;
        }
    }
    if (fchmod(fd, vol->target.perms.mode) < 0) {
        virReportSystemError(errno,
                             _("cannot set file mode '%s'"),
                             vol->target.path);
        goto cleanup;
    }

    if (VIR_CLOSE(fd) < 0) {
        virReportSystemError(errno,
                             _("cannot close file '%s'"),
                             vol->target.path);
        goto cleanup;
    }
    fd = -1;

    /* Fill in data about this new vol */
    if (virStorageBackendZFSFindLVs(pool, vol) < 0) {
        virReportSystemError(errno,
                             _("cannot find newly created volume '%s'"),
                             vol->target.path);
        goto cleanup;
    }

    return 0;

 cleanup:
    VIR_FORCE_CLOSE(fd);
    virStorageBackendZFSDeleteVol(conn, pool, vol, 0);
    return -1;
}

static int
virStorageBackendZFSBuildVolFrom(virConnectPtr conn,
                                     virStoragePoolObjPtr pool,
                                     virStorageVolDefPtr vol,
                                     virStorageVolDefPtr inputvol,
                                     unsigned int flags)
{
    virStorageBackendBuildVolFrom build_func;

    build_func = virStorageBackendGetBuildVolFromFunction(vol, inputvol);
    if (!build_func)
        return -1;

    return build_func(conn, pool, vol, inputvol, flags);
}

static int
virStorageBackendZFSDeleteVol(virConnectPtr conn ATTRIBUTE_UNUSED,
                                  virStoragePoolObjPtr pool ATTRIBUTE_UNUSED,
                                  virStorageVolDefPtr vol,
                                  unsigned int flags ATTRIBUTE_UNUSED)
{
    const char *cmdargv[] = {
        LVREMOVE, "-f", vol->target.path, NULL
    };

    if (virRun(cmdargv, NULL) < 0)
        return -1;

    return 0;
}


virStorageBackend virStorageBackendZFS = {
    .type = VIR_STORAGE_POOL_ZFS,

    .findPoolSources = virStorageBackendZFSFindPoolSources,
    .checkPool = virStorageBackendZFSCheckPool,
    .startPool = virStorageBackendZFSStartPool,
    .refreshPool = virStorageBackendZFSRefreshPool,
    .stopPool = virStorageBackendZFSStopPool,
    .deletePool = virStorageBackendZFSDeletePool,
//    .buildVol = NULL,
//    .buildVolFrom = virStorageBackendZFSBuildVolFrom,
//    .createVol = virStorageBackendZFSCreateVol,
//    .deleteVol = virStorageBackendZFSDeleteVol,
};
