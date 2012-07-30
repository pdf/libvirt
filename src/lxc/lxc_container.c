/*
 * Copyright (C) 2008-2012 Red Hat, Inc.
 * Copyright (C) 2008 IBM Corp.
 *
 * lxc_container.c: file description
 *
 * Authors:
 *  David L. Leskovec <dlesko at linux.vnet.ibm.com>
 *  Daniel P. Berrange <berrange@redhat.com>
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

#include <config.h>

#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <unistd.h>
#include <mntent.h>
#include <dirent.h>
#include <sys/reboot.h>
#include <linux/reboot.h>

/* Yes, we want linux private one, for _syscall2() macro */
#include <linux/unistd.h>

/* For MS_MOVE */
#include <linux/fs.h>

#if HAVE_CAPNG
# include <cap-ng.h>
#endif

#if HAVE_LIBBLKID
# include <blkid/blkid.h>
#endif

#include "virterror_internal.h"
#include "logging.h"
#include "lxc_container.h"
#include "util.h"
#include "memory.h"
#include "virnetdevveth.h"
#include "uuid.h"
#include "virfile.h"
#include "command.h"
#include "virnetdev.h"

#define VIR_FROM_THIS VIR_FROM_LXC

/*
 * GLibc headers are behind the kernel, so we define these
 * constants if they're not present already.
 */

#ifndef CLONE_NEWPID
# define CLONE_NEWPID  0x20000000
#endif
#ifndef CLONE_NEWUTS
# define CLONE_NEWUTS  0x04000000
#endif
#ifndef CLONE_NEWUSER
# define CLONE_NEWUSER 0x10000000
#endif
#ifndef CLONE_NEWIPC
# define CLONE_NEWIPC  0x08000000
#endif
#ifndef CLONE_NEWNET
# define CLONE_NEWNET  0x40000000 /* New network namespace */
#endif

/* messages between parent and container */
typedef char lxc_message_t;
#define LXC_CONTINUE_MSG 'c'

typedef struct __lxc_child_argv lxc_child_argv_t;
struct __lxc_child_argv {
    virDomainDefPtr config;
    virSecurityManagerPtr securityDriver;
    size_t nveths;
    char **veths;
    int monitor;
    char **ttyPaths;
    size_t nttyPaths;
    int handshakefd;
};


/*
 * reboot(LINUX_REBOOT_CMD_CAD_ON) will return -EINVAL
 * in a child pid namespace if container reboot support exists.
 * Otherwise, it will either succeed or return -EPERM.
 */
ATTRIBUTE_NORETURN static int
lxcContainerRebootChild(void *argv)
{
    int *cmd = argv;
    int ret;

    ret = reboot(*cmd);
    if (ret == -1 && errno == EINVAL)
        _exit(1);
    _exit(0);
}


static
int lxcContainerHasReboot(void)
{
    int flags = CLONE_NEWPID|CLONE_NEWNS|CLONE_NEWUTS|
        CLONE_NEWIPC|SIGCHLD;
    int cpid;
    char *childStack;
    char *stack;
    char *buf;
    int cmd, v;
    int status;
    char *tmp;

    if (virFileReadAll("/proc/sys/kernel/ctrl-alt-del", 10, &buf) < 0)
        return -1;

    if ((tmp = strchr(buf, '\n')))
        *tmp = '\0';

    if (virStrToLong_i(buf, NULL, 10, &v) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Malformed ctrl-alt-del setting '%s'"), buf);
        VIR_FREE(buf);
        return -1;
    }
    VIR_FREE(buf);
    cmd = v ? LINUX_REBOOT_CMD_CAD_ON : LINUX_REBOOT_CMD_CAD_OFF;

    if (VIR_ALLOC_N(stack, getpagesize() * 4) < 0) {
        virReportOOMError();
        return -1;
    }

    childStack = stack + (getpagesize() * 4);

    cpid = clone(lxcContainerRebootChild, childStack, flags, &cmd);
    VIR_FREE(stack);
    if (cpid < 0) {
        virReportSystemError(errno, "%s",
                             _("Unable to clone to check reboot support"));
        return -1;
    } else if (virPidWait(cpid, &status) < 0) {
        return -1;
    }

    if (WEXITSTATUS(status) != 1) {
        VIR_DEBUG("Containerized reboot support is missing "
                  "(kernel probably too old < 3.4)");
        return 0;
    }

    VIR_DEBUG("Containerized reboot support is available");
    return 1;
}


/**
 * lxcContainerBuildInitCmd:
 * @vmDef: pointer to vm definition structure
 *
 * Build a virCommandPtr for launching the container 'init' process
 *
 * Returns a virCommandPtr
 */
static virCommandPtr lxcContainerBuildInitCmd(virDomainDefPtr vmDef)
{
    char uuidstr[VIR_UUID_STRING_BUFLEN];
    virCommandPtr cmd;

    virUUIDFormat(vmDef->uuid, uuidstr);

    cmd = virCommandNew(vmDef->os.init);

    if (vmDef->os.initargv && vmDef->os.initargv[0])
        virCommandAddArgSet(cmd, (const char **)vmDef->os.initargv);

    virCommandAddEnvString(cmd, "PATH=/bin:/sbin");
    virCommandAddEnvString(cmd, "TERM=linux");
    virCommandAddEnvString(cmd, "container=lxc-libvirt");
    virCommandAddEnvPair(cmd, "container_uuid", uuidstr);
    virCommandAddEnvPair(cmd, "LIBVIRT_LXC_UUID", uuidstr);
    virCommandAddEnvPair(cmd, "LIBVIRT_LXC_NAME", vmDef->name);
    if (vmDef->os.cmdline)
        virCommandAddEnvPair(cmd, "LIBVIRT_LXC_CMDLINE", vmDef->os.cmdline);

    return cmd;
}

/**
 * lxcContainerSetStdio:
 * @control: control FD from parent
 * @ttyfd: FD of tty to set as the container console
 *
 * Sets the given tty as the primary conosole for the container as well as
 * stdout, stdin and stderr.
 *
 * Returns 0 on success or -1 in case of error
 */
static int lxcContainerSetStdio(int control, int ttyfd, int handshakefd)
{
    int rc = -1;
    int open_max, i;

    if (setsid() < 0) {
        virReportSystemError(errno, "%s",
                             _("setsid failed"));
        goto cleanup;
    }

    if (ioctl(ttyfd, TIOCSCTTY, NULL) < 0) {
        virReportSystemError(errno, "%s",
                             _("ioctl(TIOCSTTY) failed"));
        goto cleanup;
    }

    /* Just in case someone forget to set FD_CLOEXEC, explicitly
     * close all FDs before executing the container */
    open_max = sysconf (_SC_OPEN_MAX);
    for (i = 0; i < open_max; i++)
        if (i != ttyfd && i != control && i != handshakefd) {
            int tmpfd = i;
            VIR_FORCE_CLOSE(tmpfd);
        }

    if (dup2(ttyfd, 0) < 0) {
        virReportSystemError(errno, "%s",
                             _("dup2(stdin) failed"));
        goto cleanup;
    }

    if (dup2(ttyfd, 1) < 0) {
        virReportSystemError(errno, "%s",
                             _("dup2(stdout) failed"));
        goto cleanup;
    }

    if (dup2(ttyfd, 2) < 0) {
        virReportSystemError(errno, "%s",
                             _("dup2(stderr) failed"));
        goto cleanup;
    }

    rc = 0;

cleanup:
    VIR_DEBUG("rc=%d", rc);
    return rc;
}

/**
 * lxcContainerSendContinue:
 * @control: control FD to child
 *
 * Sends the continue message via the socket pair stored in the vm
 * structure.
 *
 * Returns 0 on success or -1 in case of error
 */
int lxcContainerSendContinue(int control)
{
    int rc = -1;
    lxc_message_t msg = LXC_CONTINUE_MSG;
    int writeCount = 0;

    writeCount = safewrite(control, &msg, sizeof(msg));
    if (writeCount != sizeof(msg)) {
        goto error_out;
    }

    rc = 0;
error_out:
    return rc;
}

/**
 * lxcContainerWaitForContinue:
 * @control: Control FD from parent
 *
 * This function will wait for the container continue message from the
 * parent process.  It will send this message on the socket pair stored in
 * the vm structure once it has completed the post clone container setup.
 *
 * Returns 0 on success or -1 in case of error
 */
int lxcContainerWaitForContinue(int control)
{
    lxc_message_t msg;
    int readLen;

    readLen = saferead(control, &msg, sizeof(msg));
    if (readLen != sizeof(msg)) {
        if (readLen >= 0)
            errno = EIO;
        return -1;
    }
    if (msg != LXC_CONTINUE_MSG) {
        errno = EINVAL;
        return -1;
    }

    return 0;
}


/**
 * lxcContainerRenameAndEnableInterfaces:
 * @nveths: number of interfaces
 * @veths: interface names
 *
 * This function will rename the interfaces to ethN
 * with id ascending order from zero and enable the
 * renamed interfaces for this container.
 *
 * Returns 0 on success or nonzero in case of error
 */
static int lxcContainerRenameAndEnableInterfaces(bool privNet,
                                                 size_t nveths,
                                                 char **veths)
{
    int rc = 0;
    size_t i;
    char *newname = NULL;

    for (i = 0 ; i < nveths ; i++) {
        if (virAsprintf(&newname, "eth%zu", i) < 0) {
            virReportOOMError();
            rc = -1;
            goto error_out;
        }

        VIR_DEBUG("Renaming %s to %s", veths[i], newname);
        rc = virNetDevSetName(veths[i], newname);
        if (rc < 0)
            goto error_out;

        VIR_DEBUG("Enabling %s", newname);
        rc = virNetDevSetOnline(newname, true);
        if (rc < 0)
            goto error_out;

        VIR_FREE(newname);
    }

    /* enable lo device only if there were other net devices */
    if (veths || privNet)
        rc = virNetDevSetOnline("lo", true);

error_out:
    VIR_FREE(newname);
    return rc;
}


/*_syscall2(int, pivot_root, char *, newroot, const char *, oldroot)*/
extern int pivot_root(const char * new_root,const char * put_old);

static int lxcContainerChildMountSort(const void *a, const void *b)
{
  const char **sa = (const char**)a;
  const char **sb = (const char**)b;

  /* Deliberately reversed args - we need to unmount deepest
     children first */
  return strcmp(*sb, *sa);
}

#ifndef MS_REC
# define MS_REC          16384
#endif

#ifndef MNT_DETACH
# define MNT_DETACH      0x00000002
#endif

#ifndef MS_PRIVATE
# define MS_PRIVATE              (1<<18)
#endif

#ifndef MS_SLAVE
# define MS_SLAVE                (1<<19)
#endif

static int lxcContainerPivotRoot(virDomainFSDefPtr root)
{
    int ret;
    char *oldroot = NULL, *newroot = NULL;

    ret = -1;

    VIR_DEBUG("Pivot via %s", root->src);

    /* root->parent must be private, so make / private. */
    if (mount("", "/", NULL, MS_PRIVATE|MS_REC, NULL) < 0) {
        virReportSystemError(errno, "%s",
                             _("Failed to make root private"));
        goto err;
    }

    if (virAsprintf(&oldroot, "%s/.oldroot", root->src) < 0) {
        virReportOOMError();
        goto err;
    }

    if (virFileMakePath(oldroot) < 0) {
        virReportSystemError(errno,
                             _("Failed to create %s"),
                             oldroot);
        goto err;
    }

    /* Create a tmpfs root since old and new roots must be
     * on separate filesystems */
    if (mount("tmprootfs", oldroot, "tmpfs", 0, NULL) < 0) {
        virReportSystemError(errno,
                             _("Failed to mount empty tmpfs at %s"),
                             oldroot);
        goto err;
    }

    /* Create a directory called 'new' in tmpfs */
    if (virAsprintf(&newroot, "%s/new", oldroot) < 0) {
        virReportOOMError();
        goto err;
    }

    if (virFileMakePath(newroot) < 0) {
        virReportSystemError(errno,
                             _("Failed to create %s"),
                             newroot);
        goto err;
    }

    /* ... and mount our root onto it */
    if (mount(root->src, newroot, NULL, MS_BIND|MS_REC, NULL) < 0) {
        virReportSystemError(errno,
                             _("Failed to bind new root %s into tmpfs"),
                             root->src);
        goto err;
    }

    if (root->readonly) {
        if (mount(root->src, newroot, NULL, MS_BIND|MS_REC|MS_RDONLY|MS_REMOUNT, NULL) < 0) {
            virReportSystemError(errno,
                                 _("Failed to make new root %s readonly"),
                                 root->src);
            goto err;
        }
    }

    /* Now we chroot into the tmpfs, then pivot into the
     * root->src bind-mounted onto '/new' */
    if (chdir(newroot) < 0) {
        virReportSystemError(errno,
                             _("Failed to chroot into %s"), newroot);
        goto err;
    }

    /* The old root directory will live at /.oldroot after
     * this and will soon be unmounted completely */
    if (pivot_root(".", ".oldroot") < 0) {
        virReportSystemError(errno, "%s",
                             _("Failed to pivot root"));
        goto err;
    }

    /* CWD is undefined after pivot_root, so go to / */
    if (chdir("/") < 0)
        goto err;

    ret = 0;

err:
    VIR_FREE(oldroot);
    VIR_FREE(newroot);

    return ret;
}


static int lxcContainerMountBasicFS(bool pivotRoot,
                                    char *sec_mount_options)
{
    const struct {
        const char *src;
        const char *dst;
        const char *type;
        const char *opts;
        int mflags;
    } mnts[] = {
        /* When we want to make a bind mount readonly, for unknown reasons,
         * it is currently necessary to bind it once, and then remount the
         * bind with the readonly flag. If this is not done, then the original
         * mount point in the main OS becomes readonly too which is not what
         * we want. Hence some things have two entries here.
         */
        { "proc", "/proc", "proc", NULL, MS_NOSUID|MS_NOEXEC|MS_NODEV },
        { "/proc/sys", "/proc/sys", NULL, NULL, MS_BIND },
        { "/proc/sys", "/proc/sys", NULL, NULL, MS_BIND|MS_REMOUNT|MS_RDONLY },
        { "sysfs", "/sys", "sysfs", NULL, MS_NOSUID|MS_NOEXEC|MS_NODEV },
        { "sysfs", "/sys", "sysfs", NULL, MS_BIND|MS_REMOUNT|MS_RDONLY },
#if HAVE_SELINUX
        { SELINUX_MOUNT, SELINUX_MOUNT, "selinuxfs", NULL, MS_NOSUID|MS_NOEXEC|MS_NODEV },
        { SELINUX_MOUNT, SELINUX_MOUNT, NULL, NULL, MS_BIND|MS_REMOUNT|MS_RDONLY },
#endif
    };
    int i, rc = -1;
    char *opts = NULL;

    VIR_DEBUG("Mounting basic filesystems pivotRoot=%d", pivotRoot);

    for (i = 0 ; i < ARRAY_CARDINALITY(mnts) ; i++) {
        const char *srcpath = NULL;

        VIR_DEBUG("Processing %s -> %s",
                  mnts[i].src, mnts[i].dst);

        if (virFileMakePath(mnts[i].dst) < 0) {
            virReportSystemError(errno,
                                 _("Failed to mkdir %s"),
                                 mnts[i].src);
            goto cleanup;
        }

        srcpath = mnts[i].src;

        /* Skip if mount doesn't exist in source */
        if ((srcpath[0] == '/') &&
            (access(srcpath, R_OK) < 0))
            continue;

        VIR_DEBUG("Mount %s on %s type=%s flags=%x, opts=%s",
                  srcpath, mnts[i].dst, mnts[i].type, mnts[i].mflags, mnts[i].opts);
        if (mount(srcpath, mnts[i].dst, mnts[i].type, mnts[i].mflags, mnts[i].opts) < 0) {
            virReportSystemError(errno,
                                 _("Failed to mount %s on %s type %s"),
                                 mnts[i].src, mnts[i].dst, NULLSTR(mnts[i].type));
            goto cleanup;
        }
    }

    if (pivotRoot) {
        /*
         * tmpfs is limited to 64kb, since we only have device nodes in there
         * and don't want to DOS the entire OS RAM usage
         */

        ignore_value(virAsprintf(&opts,
                                 "mode=755,size=65536%s",(sec_mount_options ? sec_mount_options : "")));
        if (!opts) {
            virReportOOMError();
            goto cleanup;
        }

        VIR_DEBUG("Mount devfs on /dev type=tmpfs flags=%x, opts=%s",
                  MS_NOSUID, opts);
        if (mount("devfs", "/dev", "tmpfs", MS_NOSUID, opts) < 0) {
            virReportSystemError(errno,
                                 _("Failed to mount %s on %s type %s"),
                                 "devfs", "/dev", "tmpfs");
            goto cleanup;
        }
    }

    rc = 0;

cleanup:
    VIR_DEBUG("rc=%d", rc);
    VIR_FREE(opts);
    return rc;
}


static int lxcContainerMountFSDevPTS(virDomainFSDefPtr root)
{
    char *devpts = NULL;
    int rc = -1;

    if (virAsprintf(&devpts, "/.oldroot%s/dev/pts", root->src) < 0) {
        virReportOOMError();
        goto cleanup;
    }

    if (virFileMakePath("/dev/pts") < 0) {
        virReportSystemError(errno, "%s",
                             _("Cannot create /dev/pts"));
        goto cleanup;
    }

    VIR_DEBUG("Trying to move %s to %s", devpts, "/dev/pts");
    if ((rc = mount(devpts, "/dev/pts", NULL, MS_MOVE, NULL)) < 0) {
        virReportSystemError(errno, "%s",
                             _("Failed to mount /dev/pts in container"));
        goto cleanup;
    }

    rc = 0;

 cleanup:
    VIR_FREE(devpts);

    return rc;
}

static int lxcContainerPopulateDevices(char **ttyPaths, size_t nttyPaths)
{
    size_t i;
    const struct {
        int maj;
        int min;
        mode_t mode;
        const char *path;
    } devs[] = {
        { LXC_DEV_MAJ_MEMORY, LXC_DEV_MIN_NULL, 0666, "/dev/null" },
        { LXC_DEV_MAJ_MEMORY, LXC_DEV_MIN_ZERO, 0666, "/dev/zero" },
        { LXC_DEV_MAJ_MEMORY, LXC_DEV_MIN_FULL, 0666, "/dev/full" },
        { LXC_DEV_MAJ_MEMORY, LXC_DEV_MIN_RANDOM, 0666, "/dev/random" },
        { LXC_DEV_MAJ_MEMORY, LXC_DEV_MIN_URANDOM, 0666, "/dev/urandom" },
    };
    const struct {
        const char *src;
        const char *dst;
    } links[] = {
        { "/proc/self/fd/0", "/dev/stdin" },
        { "/proc/self/fd/1", "/dev/stdout" },
        { "/proc/self/fd/2", "/dev/stderr" },
        { "/proc/self/fd", "/dev/fd" },
    };

    /* Populate /dev/ with a few important bits */
    for (i = 0 ; i < ARRAY_CARDINALITY(devs) ; i++) {
        dev_t dev = makedev(devs[i].maj, devs[i].min);
        if (mknod(devs[i].path, S_IFCHR, dev) < 0 ||
            chmod(devs[i].path, devs[i].mode)) {
            virReportSystemError(errno,
                                 _("Failed to make device %s"),
                                 devs[i].path);
            return -1;
        }
    }

    for (i = 0 ; i < ARRAY_CARDINALITY(links) ; i++) {
        if (symlink(links[i].src, links[i].dst) < 0) {
            virReportSystemError(errno,
                                 _("Failed to symlink device %s to %s"),
                                 links[i].dst, links[i].src);
            return -1;
        }
    }

    if (access("/dev/pts/ptmx", W_OK) == 0) {
        /* We have private devpts capability, so bind that */
        if (virFileTouch("/dev/ptmx", 0666) < 0)
            return -1;

        if (mount("/dev/pts/ptmx", "/dev/ptmx", "ptmx", MS_BIND, NULL) < 0) {
            virReportSystemError(errno, "%s",
                                 _("Failed to bind /dev/pts/ptmx on to /dev/ptmx"));
            return -1;
        }
    } else {
        /* Legacy devpts, so we need to just use shared one */
        dev_t dev = makedev(LXC_DEV_MAJ_TTY, LXC_DEV_MIN_PTMX);
        if (mknod("/dev/ptmx", S_IFCHR, dev) < 0 ||
            chmod("/dev/ptmx", 0666)) {
            virReportSystemError(errno, "%s",
                                 _("Failed to make device /dev/ptmx"));
            return -1;
        }
    }

    for (i = 0 ; i < nttyPaths ; i++) {
        char *tty;
        if (virAsprintf(&tty, "/dev/tty%zu", i+1) < 0) {
            virReportOOMError();
            return -1;
        }
        if (symlink(ttyPaths[i], tty) < 0) {
            VIR_FREE(tty);
            virReportSystemError(errno,
                                 _("Failed to symlink %s to %s"),
                                 ttyPaths[i], tty);
            return -1;
        }
        VIR_FREE(tty);
        if (i == 0 &&
            symlink(ttyPaths[i], "/dev/console") < 0) {
            virReportSystemError(errno,
                                 _("Failed to symlink %s to /dev/console"),
                                 ttyPaths[i]);
            return -1;
        }
    }
    return 0;
}


static int lxcContainerMountFSBind(virDomainFSDefPtr fs,
                                   const char *srcprefix)
{
    char *src = NULL;
    int ret = -1;
    struct stat st;

    if (virAsprintf(&src, "%s%s", srcprefix, fs->src) < 0) {
        virReportOOMError();
        goto cleanup;
    }

    if (stat(fs->dst, &st) < 0) {
        if (errno != ENOENT) {
            virReportSystemError(errno, _("Unable to stat bind target %s"),
                                 fs->dst);
            goto cleanup;
        }
        /* ENOENT => create the target dir or file */
        if (stat(src, &st) < 0) {
            virReportSystemError(errno, _("Unable to stat bind source %s"),
                                 src);
            goto cleanup;
        }
        if (S_ISDIR(st.st_mode)) {
            if (virFileMakePath(fs->dst) < 0) {
                virReportSystemError(errno,
                                     _("Failed to create %s"),
                                     fs->dst);
                goto cleanup;
            }
        } else {
            /* Create Empty file for target mount point */
            int fd = open(fs->dst, O_WRONLY|O_CREAT|O_NOCTTY|O_NONBLOCK, 0666);
            if (fd < 0) {
                if (errno != EEXIST) {
                    virReportSystemError(errno,
                                         _("Failed to create bind target %s"),
                                         fs->dst);
                    goto cleanup;
                }
            }
            if (VIR_CLOSE(fd) < 0) {
                virReportSystemError(errno,
                                     _("Failed to close bind target %s"),
                                     fs->dst);
                goto cleanup;
            }
        }
    }

    if (mount(src, fs->dst, NULL, MS_BIND, NULL) < 0) {
        virReportSystemError(errno,
                             _("Failed to bind mount directory %s to %s"),
                             src, fs->dst);
        goto cleanup;
    }

    if (fs->readonly) {
        VIR_DEBUG("Binding %s readonly", fs->dst);
        if (mount(src, fs->dst, NULL, MS_BIND|MS_REMOUNT|MS_RDONLY, NULL) < 0) {
            virReportSystemError(errno,
                                 _("Failed to make directory %s readonly"),
                                 fs->dst);
        }
    }

    ret = 0;

cleanup:
    VIR_FREE(src);
    return ret;
}


#ifdef HAVE_LIBBLKID
static int
lxcContainerMountDetectFilesystem(const char *src, char **type)
{
    int fd;
    int ret = -1;
    int rc;
    const char *data = NULL;
    blkid_probe blkid = NULL;

    *type = NULL;

    if ((fd = open(src, O_RDONLY)) < 0) {
        virReportSystemError(errno,
                             _("Unable to open filesystem %s"), src);
        return -1;
    }

    if (!(blkid = blkid_new_probe())) {
        virReportSystemError(errno, "%s",
                             _("Unable to create blkid library handle"));
        goto cleanup;
    }
    if (blkid_probe_set_device(blkid, fd, 0, 0) < 0) {
        virReportSystemError(EINVAL,
                             _("Unable to associate device %s with blkid library"),
                             src);
        goto cleanup;
    }

    blkid_probe_enable_superblocks(blkid, 1);

    blkid_probe_set_superblocks_flags(blkid, BLKID_SUBLKS_TYPE);

    rc = blkid_do_safeprobe(blkid);
    if (rc != 0) {
        if (rc == 1) /* Nothing found, return success with *type == NULL */
            goto done;

        if (rc == -2) {
            virReportSystemError(EINVAL,
                                 _("Too many filesystems detected for %s"),
                                 src);
        } else {
            virReportSystemError(errno,
                                 _("Unable to detect filesystem for %s"),
                                 src);
        }
        goto cleanup;
    }

    if (blkid_probe_lookup_value(blkid, "TYPE", &data, NULL) < 0) {
        virReportSystemError(ENOENT,
                             _("Unable to find filesystem type for %s"),
                             src);
        goto cleanup;
    }

    if (!(*type = strdup(data))) {
        virReportOOMError();
        goto cleanup;
    }

done:
    ret = 0;
cleanup:
    VIR_FORCE_CLOSE(fd);
    if (blkid)
        blkid_free_probe(blkid);
    return ret;
}
#else /* ! HAVE_LIBBLKID */
static int
lxcContainerMountDetectFilesystem(const char *src ATTRIBUTE_UNUSED,
                                  char **type)
{
    /* No libblkid, so just return success with no detected type */
    *type = NULL;
    return 0;
}
#endif /* ! HAVE_LIBBLKID */

/*
 * This functions attempts to do automatic detection of filesystem
 * type following the same rules as the util-linux 'mount' binary.
 *
 * The main difference is that we don't (currently) try to use
 * libblkid to detect the format first. We go straight to using
 * /etc/filesystems, and then /proc/filesystems
 */
static int lxcContainerMountFSBlockAuto(virDomainFSDefPtr fs,
                                        int fsflags,
                                        const char *src,
                                        const char *srcprefix)
{
    FILE *fp = NULL;
    int ret = -1;
    bool tryProc = false;
    bool gotStar = false;
    char *fslist = NULL;
    char *line = NULL;
    const char *type;

    VIR_DEBUG("src=%s srcprefix=%s dst=%s", src, srcprefix, fs->dst);

    /* First time around we use /etc/filesystems */
retry:
    if (virAsprintf(&fslist, "%s%s",
                    srcprefix, tryProc ? "/proc/filesystems" : "/etc/filesystems") < 0) {
        virReportOOMError();
        goto cleanup;
    }

    VIR_DEBUG("Open fslist %s", fslist);
    if (!(fp = fopen(fslist, "r"))) {
        /* If /etc/filesystems does not exist, then we need to retry
         * with /proc/filesystems next
         */
        if (errno == ENOENT &&
            !tryProc) {
            tryProc = true;
            VIR_FREE(fslist);
            goto retry;
        }

        virReportSystemError(errno,
                             _("Unable to read %s"),
                             fslist);
        goto cleanup;
    }

    while (!feof(fp)) {
        size_t n;
        VIR_FREE(line);
        if (getline(&line, &n, fp) <= 0) {
            if (feof(fp))
                break;

            goto cleanup;
        }

        if (strstr(line, "nodev"))
            continue;

        type = strchr(line, '\n');
        if (type)
            line[type-line] = '\0';

        type = line;
        virSkipSpaces(&type);

        /*
         * /etc/filesystems is only allowed to contain '*' on the last line
         */
        if (gotStar && !tryProc) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("%s has unexpected '*' before last line"),
                           fslist);
            goto cleanup;
        }

        /* An '*' on the last line in /etc/filesystems
         * means try /proc/filesystems next. We don't
         * jump immediately though, since we need to see
         * if any more lines follow
         */
        if (!tryProc &&
            STREQ(type, "*"))
            gotStar = true;

        VIR_DEBUG("Trying mount %s with %s", src, type);
        if (mount(src, fs->dst, type, fsflags, NULL) < 0) {
            /* These errnos indicate a bogus filesystem type for
             * the image we have, so skip to the next type
             */
            if (errno == EINVAL || errno == ENODEV)
                continue;

            virReportSystemError(errno,
                                 _("Failed to mount device %s to %s"),
                                 src, fs->dst);
            goto cleanup;
        }

        ret = 0;
        break;
    }

    /* We've got to the end of /etc/filesystems and saw
     * a '*', so we must try /proc/filesystems next
     */
    if (ret != 0 &&
        !tryProc &&
        gotStar) {
        tryProc = true;
        VIR_FREE(fslist);
        VIR_FORCE_FCLOSE(fp);
        goto retry;
    }

    if (ret != 0) {
        virReportSystemError(ENODEV,
                             _("Failed to mount device %s to %s, unable to detect filesystem"),
                             src, fs->dst);
    }

    VIR_DEBUG("Done mounting filesystem ret=%d tryProc=%d", ret, tryProc);

cleanup:
    VIR_FREE(line);
    VIR_FREE(fslist);
    VIR_FORCE_FCLOSE(fp);
    return ret;
}


/*
 * Mount a block device 'src' on fs->dst, automatically
 * probing for filesystem type
 */
static int lxcContainerMountFSBlockHelper(virDomainFSDefPtr fs,
                                          const char *src,
                                          const char *srcprefix)
{
    int fsflags = 0;
    int ret = -1;
    char *format = NULL;

    if (fs->readonly)
        fsflags |= MS_RDONLY;

    if (virFileMakePath(fs->dst) < 0) {
        virReportSystemError(errno,
                             _("Failed to create %s"),
                             fs->dst);
        goto cleanup;
    }

    if (lxcContainerMountDetectFilesystem(src, &format) < 0)
        goto cleanup;

    if (format) {
        VIR_DEBUG("Mount %s with detected format %s", src, format);
        if (mount(src, fs->dst, format, fsflags, NULL) < 0) {
            virReportSystemError(errno,
                                 _("Failed to mount device %s to %s as %s"),
                                 src, fs->dst, format);
            goto cleanup;
        }
        ret = 0;
    } else {
        ret = lxcContainerMountFSBlockAuto(fs, fsflags, src, srcprefix);
    }

cleanup:
    VIR_FREE(format);
    return ret;
}


static int lxcContainerMountFSBlock(virDomainFSDefPtr fs,
                                    const char *srcprefix)
{
    char *src = NULL;
    int ret = -1;

    if (virAsprintf(&src, "%s%s", srcprefix, fs->src) < 0) {
        virReportOOMError();
        goto cleanup;
    }

    ret = lxcContainerMountFSBlockHelper(fs, src, srcprefix);

    VIR_DEBUG("Done mounting filesystem ret=%d", ret);

cleanup:
    VIR_FREE(src);
    return ret;
}


static int lxcContainerMountFSTmpfs(virDomainFSDefPtr fs,
                                    char *sec_mount_options)
{
    int ret = -1;
    char *data = NULL;

    if (virAsprintf(&data,
                    "size=%lldk%s", fs->usage, (sec_mount_options ? sec_mount_options : "")) < 0) {
        virReportOOMError();
        goto cleanup;
    }

    if (virFileMakePath(fs->dst) < 0) {
        virReportSystemError(errno,
                             _("Failed to create %s"),
                             fs->dst);
        goto cleanup;
    }

    if (mount("tmpfs", fs->dst, "tmpfs", MS_NOSUID|MS_NODEV, data) < 0) {
        virReportSystemError(errno,
                             _("Failed to mount directory %s as tmpfs"),
                             fs->dst);
        goto cleanup;
    }

    if (fs->readonly) {
        VIR_DEBUG("Binding %s readonly", fs->dst);
        if (mount(fs->dst, fs->dst, NULL, MS_BIND|MS_REMOUNT|MS_RDONLY, NULL) < 0) {
            virReportSystemError(errno,
                                 _("Failed to make directory %s readonly"),
                                 fs->dst);
        }
    }

    ret = 0;

cleanup:
    VIR_FREE(data);
    return ret;
}


static int lxcContainerMountFS(virDomainFSDefPtr fs,
                               const char *srcprefix,
                               char *sec_mount_options)
{
    switch (fs->type) {
    case VIR_DOMAIN_FS_TYPE_MOUNT:
        if (lxcContainerMountFSBind(fs, srcprefix) < 0)
            return -1;
        break;
    case VIR_DOMAIN_FS_TYPE_BLOCK:
        if (lxcContainerMountFSBlock(fs, srcprefix) < 0)
            return -1;
        break;
    case VIR_DOMAIN_FS_TYPE_RAM:
        if (lxcContainerMountFSTmpfs(fs, sec_mount_options) < 0)
            return -1;
        break;
    case VIR_DOMAIN_FS_TYPE_BIND:
        if (lxcContainerMountFSBind(fs, "") < 0)
            return -1;
        break;
    case VIR_DOMAIN_FS_TYPE_FILE:
        /* We do actually support this, but the lxc controller
         * should have associated the file with a loopback
         * device and changed this to TYPE_BLOCK for us */
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Unexpected filesystem type %s"),
                       virDomainFSTypeToString(fs->type));
        break;
    default:
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("Cannot mount filesystem type %s"),
                       virDomainFSTypeToString(fs->type));
        break;
    }
    return 0;
}


static int lxcContainerMountAllFS(virDomainDefPtr vmDef,
                                  const char *dstprefix,
                                  bool skipRoot,
                                  char *sec_mount_options)
{
    size_t i;
    VIR_DEBUG("Mounting %s %d", dstprefix, skipRoot);

    /* Pull in rest of container's mounts */
    for (i = 0 ; i < vmDef->nfss ; i++) {
        if (skipRoot &&
            STREQ(vmDef->fss[i]->dst, "/"))
            continue;

        if (lxcContainerMountFS(vmDef->fss[i], dstprefix, sec_mount_options) < 0)
            return -1;
    }

    VIR_DEBUG("Mounted all filesystems");
    return 0;
}


static int lxcContainerGetSubtree(const char *prefix,
                                  char ***mountsret,
                                  size_t *nmountsret)
{
    FILE *procmnt;
    struct mntent mntent;
    char mntbuf[1024];
    int ret = -1;
    char **mounts = NULL;
    size_t nmounts = 0;

    *mountsret = NULL;
    *nmountsret = 0;

    if (!(procmnt = setmntent("/proc/mounts", "r"))) {
        virReportSystemError(errno, "%s",
                             _("Failed to read /proc/mounts"));
        return -1;
    }

    while (getmntent_r(procmnt, &mntent, mntbuf, sizeof(mntbuf)) != NULL) {
        VIR_DEBUG("Got %s", mntent.mnt_dir);
        if (!STRPREFIX(mntent.mnt_dir, prefix))
            continue;

        if (VIR_REALLOC_N(mounts, nmounts+1) < 0) {
            virReportOOMError();
            goto cleanup;
        }
        if (!(mounts[nmounts] = strdup(mntent.mnt_dir))) {
            virReportOOMError();
            goto cleanup;
        }
        nmounts++;
        VIR_DEBUG("Grabbed %s", mntent.mnt_dir);
    }

    if (mounts)
        qsort(mounts, nmounts, sizeof(mounts[0]),
              lxcContainerChildMountSort);

    ret = 0;
cleanup:
    *mountsret = mounts;
    *nmountsret = nmounts;
    endmntent(procmnt);
    return ret;
}

static int lxcContainerUnmountSubtree(const char *prefix,
                                      bool isOldRootFS)
{
    char **mounts = NULL;
    size_t nmounts = 0;
    size_t i;
    int saveErrno;
    const char *failedUmount = NULL;
    int ret = -1;

    VIR_DEBUG("Unmount subtreee from %s", prefix);

    if (lxcContainerGetSubtree(prefix, &mounts, &nmounts) < 0)
        goto cleanup;
    for (i = 0 ; i < nmounts ; i++) {
        VIR_DEBUG("Umount %s", mounts[i]);
        if (umount(mounts[i]) < 0) {
            char ebuf[1024];
            failedUmount = mounts[i];
            saveErrno = errno;
            VIR_WARN("Failed to unmount '%s', trying to detach subtree '%s': %s",
                     failedUmount, mounts[nmounts-1],
                     virStrerror(errno, ebuf, sizeof(ebuf)));
            break;
        }
    }

    if (failedUmount) {
        /* This detaches the subtree */
        if (umount2(mounts[nmounts-1], MNT_DETACH) < 0) {
            virReportSystemError(saveErrno,
                                 _("Failed to unmount '%s' and could not detach subtree '%s'"),
                                 failedUmount, mounts[nmounts-1]);
            goto cleanup;
        }
        /* This unmounts the tmpfs on which the old root filesystem was hosted */
        if (isOldRootFS &&
            umount(mounts[nmounts-1]) < 0) {
            virReportSystemError(saveErrno,
                                 _("Failed to unmount '%s' and could not unmount old root '%s'"),
                                 failedUmount, mounts[nmounts-1]);
            goto cleanup;
        }
    }

    ret = 0;

cleanup:
    for (i = 0 ; i < nmounts ; i++)
        VIR_FREE(mounts[i]);
    VIR_FREE(mounts);

    return ret;
}


struct lxcContainerCGroup {
    const char *dir;
    const char *linkDest;
};


static void lxcContainerCGroupFree(struct lxcContainerCGroup *mounts,
                                   size_t nmounts)
{
    size_t i;

    if (!mounts)
        return;

    for (i = 0 ; i < nmounts ; i++) {
        VIR_FREE(mounts[i].dir);
        VIR_FREE(mounts[i].linkDest);
    }
    VIR_FREE(mounts);
}


static int lxcContainerIdentifyCGroups(struct lxcContainerCGroup **mountsret,
                                       size_t *nmountsret)
{
    FILE *procmnt = NULL;
    struct mntent mntent;
    struct dirent *dent;
    char mntbuf[1024];
    int ret = -1;
    struct lxcContainerCGroup *mounts = NULL;
    size_t nmounts = 0;
    DIR *dh = NULL;
    char *path = NULL;

    *mountsret = NULL;
    *nmountsret = 0;

    VIR_DEBUG("Finding cgroups mount points under %s", VIR_CGROUP_SYSFS_MOUNT);

    if (!(procmnt = setmntent("/proc/mounts", "r"))) {
        virReportSystemError(errno, "%s",
                             _("Failed to read /proc/mounts"));
        return -1;
    }

    while (getmntent_r(procmnt, &mntent, mntbuf, sizeof(mntbuf)) != NULL) {
        VIR_DEBUG("Got %s", mntent.mnt_dir);
        if (STRNEQ(mntent.mnt_type, "cgroup") ||
            !STRPREFIX(mntent.mnt_dir, VIR_CGROUP_SYSFS_MOUNT))
            continue;

        /* Skip named mounts with no controller since they're
         * for application use only ie systemd */
        if (strstr(mntent.mnt_opts, "name="))
            continue;

        if (VIR_EXPAND_N(mounts, nmounts, 1) < 0) {
            virReportOOMError();
            goto cleanup;
        }
        if (!(mounts[nmounts-1].dir = strdup(mntent.mnt_dir))) {
            virReportOOMError();
            goto cleanup;
        }
        VIR_DEBUG("Grabbed %s", mntent.mnt_dir);
    }

    VIR_DEBUG("Checking for symlinks in %s", VIR_CGROUP_SYSFS_MOUNT);
    if (!(dh = opendir(VIR_CGROUP_SYSFS_MOUNT))) {
        virReportSystemError(errno,
                             _("Unable to read directory %s"),
                             VIR_CGROUP_SYSFS_MOUNT);
        goto cleanup;
    }

    while ((dent = readdir(dh)) != NULL) {
        ssize_t rv;
        /* The cgroups links are just relative to the local
         * dir so we don't need a large buf */
        char linkbuf[100];

        if (dent->d_name[0] == '.')
            continue;

        VIR_DEBUG("Checking entry %s", dent->d_name);
        if (virAsprintf(&path, "%s/%s", VIR_CGROUP_SYSFS_MOUNT, dent->d_name) < 0) {
            virReportOOMError();
            goto cleanup;
        }

        if ((rv = readlink(path, linkbuf, sizeof(linkbuf)-1)) < 0) {
            if (errno != EINVAL) {
                virReportSystemError(errno,
                                     _("Unable to resolve link %s"),
                                     path);
                VIR_FREE(path);
                goto cleanup;
            }
            /* Ok not a link */
            VIR_FREE(path);
        } else {
            linkbuf[rv] = '\0';
            VIR_DEBUG("Got a link %s to %s", path, linkbuf);
            if (VIR_EXPAND_N(mounts, nmounts, 1) < 0) {
                virReportOOMError();
                goto cleanup;
            }
            if (!(mounts[nmounts-1].linkDest = strdup(linkbuf))) {
                virReportOOMError();
                goto cleanup;
            }
            mounts[nmounts-1].dir = path;
            path = NULL;
        }
    }

    *mountsret = mounts;
    *nmountsret = nmounts;
    ret = 0;

cleanup:
    closedir(dh);
    endmntent(procmnt);
    VIR_FREE(path);

    if (ret < 0)
        lxcContainerCGroupFree(mounts, nmounts);
    return ret;
}


static int lxcContainerMountCGroups(struct lxcContainerCGroup *mounts,
                                    size_t nmounts,
                                    char * sec_mount_options)
{
    size_t i;
    char *opts = NULL;

    VIR_DEBUG("Mounting cgroups at '%s'", VIR_CGROUP_SYSFS_MOUNT);

    if (virFileMakePath(VIR_CGROUP_SYSFS_MOUNT) < 0) {
        virReportSystemError(errno,
                             _("Unable to create directory %s"),
                             VIR_CGROUP_SYSFS_MOUNT);
        return -1;
    }

    if (virAsprintf(&opts,
                    "mode=755,size=65536%s",(sec_mount_options ? sec_mount_options : "")) < 0 ) {
        virReportOOMError();
        return -1;
    }

    if (mount("tmpfs", VIR_CGROUP_SYSFS_MOUNT, "tmpfs", MS_NOSUID|MS_NODEV|MS_NOEXEC, opts) < 0) {
        VIR_FREE(opts);
        virReportSystemError(errno,
                             _("Failed to mount %s on %s type %s"),
                             "tmpfs", VIR_CGROUP_SYSFS_MOUNT, "tmpfs");
        return -1;
    }
    VIR_FREE(opts);

    for (i = 0 ; i < nmounts ; i++) {
        if (mounts[i].linkDest) {
            VIR_DEBUG("Link mount point '%s' to '%s'",
                      mounts[i].dir, mounts[i].linkDest);
            if (symlink(mounts[i].linkDest, mounts[i].dir) < 0) {
                virReportSystemError(errno,
                                     _("Unable to symlink directory %s to %s"),
                                     mounts[i].dir, mounts[i].linkDest);
                return -1;
            }
        } else {
            VIR_DEBUG("Create mount point '%s'", mounts[i].dir);
            if (virFileMakePath(mounts[i].dir) < 0) {
                virReportSystemError(errno,
                                     _("Unable to create directory %s"),
                                     mounts[i].dir);
                return -1;
            }

            if (mount("cgroup", mounts[i].dir, "cgroup",
                      0, mounts[i].dir + strlen(VIR_CGROUP_SYSFS_MOUNT) + 1) < 0) {
                virReportSystemError(errno,
                                     _("Failed to mount %s on %s"),
                                     "cgroup", mounts[i].dir);
                return -1;
            }
        }
    }

    return 0;
}


/* Got a FS mapped to /, we're going the pivot_root
 * approach to do a better-chroot-than-chroot
 * this is based on this thread http://lkml.org/lkml/2008/3/5/29
 */
static int lxcContainerSetupPivotRoot(virDomainDefPtr vmDef,
                                      virDomainFSDefPtr root,
                                      char **ttyPaths,
                                      size_t nttyPaths,
                                      char *sec_mount_options)
{
    struct lxcContainerCGroup *mounts = NULL;
    size_t nmounts = 0;
    int ret = -1;

    /* Before pivoting we need to identify any
     * cgroups controllers that are mounted */
    if (lxcContainerIdentifyCGroups(&mounts, &nmounts) < 0)
        return -1;

    /* Gives us a private root, leaving all parent OS mounts on /.oldroot */
    if (lxcContainerPivotRoot(root) < 0)
        goto cleanup;

    /* If we have the root source being '/', then we need to
     * get rid of any existing stuff under /proc, /sys & /tmp.
     * We need new namespace aware versions of those. We must
     * do /proc last otherwise we won't find /proc/mounts :-) */
    if (STREQ(root->src, "/") &&
        (lxcContainerUnmountSubtree("/sys", false) < 0 ||
         lxcContainerUnmountSubtree("/dev", false) < 0 ||
         lxcContainerUnmountSubtree("/proc", false) < 0))
        goto cleanup;

    /* Mounts the core /proc, /sys, etc filesystems */
    if (lxcContainerMountBasicFS(true, sec_mount_options) < 0)
        goto cleanup;

    /* Now we can re-mount the cgroups controllers in the
     * same configuration as before */
    if (lxcContainerMountCGroups(mounts, nmounts, sec_mount_options) < 0)
        goto cleanup;

    /* Mounts /dev/pts */
    if (lxcContainerMountFSDevPTS(root) < 0)
        goto cleanup;

    /* Populates device nodes in /dev/ */
    if (lxcContainerPopulateDevices(ttyPaths, nttyPaths) < 0)
        goto cleanup;

    /* Sets up any non-root mounts from guest config */
    if (lxcContainerMountAllFS(vmDef, "/.oldroot", true, sec_mount_options) < 0)
        goto cleanup;

    /* Gets rid of all remaining mounts from host OS, including /.oldroot itself */
    if (lxcContainerUnmountSubtree("/.oldroot", true) < 0)
        goto cleanup;

    ret = 0;

cleanup:
    lxcContainerCGroupFree(mounts, nmounts);
    return ret;
}


/* Nothing mapped to /, we're using the main root,
   but with extra stuff mapped in */
static int lxcContainerSetupExtraMounts(virDomainDefPtr vmDef,
                                        virDomainFSDefPtr root,
                                        char *sec_mount_options)
{
    int ret = -1;
    struct lxcContainerCGroup *mounts = NULL;
    size_t nmounts = 0;

    VIR_DEBUG("def=%p", vmDef);
    /*
     * This makes sure that any new filesystems in the
     * host OS propagate to the container, but any
     * changes in the container are private
     */
    if (mount("", "/", NULL, MS_SLAVE|MS_REC, NULL) < 0) {
        virReportSystemError(errno, "%s",
                             _("Failed to make / slave"));
        return -1;
    }

    if (root && root->readonly) {
        if (mount("", "/", NULL, MS_BIND|MS_REC|MS_RDONLY|MS_REMOUNT, NULL) < 0) {
            virReportSystemError(errno, "%s",
                                 _("Failed to make root readonly"));
            return -1;
        }
    }

    VIR_DEBUG("Mounting config FS");
    if (lxcContainerMountAllFS(vmDef, "", false, sec_mount_options) < 0)
        return -1;

    /* Before replacing /sys we need to identify any
     * cgroups controllers that are mounted */
    if (lxcContainerIdentifyCGroups(&mounts, &nmounts) < 0)
        return -1;

    /* Gets rid of any existing stuff under /proc, since we need new
     * namespace aware versions of those. We must do /proc second
     * otherwise we won't find /proc/mounts :-) */
    if (lxcContainerUnmountSubtree("/sys", false) < 0 ||
        lxcContainerUnmountSubtree("/proc", false) < 0)
        goto cleanup;

    /* Mounts the core /proc, /sys, etc filesystems */
    if (lxcContainerMountBasicFS(false, sec_mount_options) < 0)
        goto cleanup;

    /* Now we can re-mount the cgroups controllers in the
     * same configuration as before */
    if (lxcContainerMountCGroups(mounts, nmounts, sec_mount_options) < 0)
        goto cleanup;

    VIR_DEBUG("Mounting completed");

    ret = 0;

cleanup:
    lxcContainerCGroupFree(mounts, nmounts);
    return ret;
}


static int lxcContainerResolveSymlinks(virDomainDefPtr vmDef)
{
    char *newroot;
    size_t i;

    for (i = 0 ; i < vmDef->nfss ; i++) {
        virDomainFSDefPtr fs = vmDef->fss[i];
        if (!fs->src)
            continue;
        if (virFileResolveAllLinks(fs->src, &newroot) < 0)
            return -1;

        VIR_DEBUG("Resolved '%s' to %s", fs->src, newroot);

        VIR_FREE(fs->src);
        fs->src = newroot;
    }

    return 0;
}

static int lxcContainerSetupMounts(virDomainDefPtr vmDef,
                                   virDomainFSDefPtr root,
                                   char **ttyPaths,
                                   size_t nttyPaths,
                                   virSecurityManagerPtr securityDriver)
{
    int rc = -1;
    char *sec_mount_options = NULL;
    if (lxcContainerResolveSymlinks(vmDef) < 0)
        return -1;

    sec_mount_options = virSecurityManagerGetMountOptions(securityDriver, vmDef);
    if (root && root->src)
        rc =  lxcContainerSetupPivotRoot(vmDef, root, ttyPaths, nttyPaths, sec_mount_options);
    else
        rc = lxcContainerSetupExtraMounts(vmDef, root, sec_mount_options);

    VIR_FREE(sec_mount_options);
    return rc;
}


/*
 * This is running as the 'init' process insid the container.
 * It removes some capabilities that could be dangerous to
 * host system, since they are not currently "containerized"
 */
static int lxcContainerDropCapabilities(bool keepReboot)
{
#if HAVE_CAPNG
    int ret;

    capng_get_caps_process();

    if ((ret = capng_updatev(CAPNG_DROP,
                             CAPNG_EFFECTIVE | CAPNG_PERMITTED |
                             CAPNG_INHERITABLE | CAPNG_BOUNDING_SET,
                             CAP_SYS_MODULE, /* No kernel module loading */
                             CAP_SYS_TIME, /* No changing the clock */
                             CAP_AUDIT_CONTROL, /* No messing with auditing status */
                             CAP_MAC_ADMIN, /* No messing with LSM config */
                             keepReboot ? -1 : CAP_SYS_BOOT, /* No use of reboot */
                             -1 /* sentinal */)) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Failed to remove capabilities: %d"), ret);
        return -1;
    }

    if ((ret = capng_apply(CAPNG_SELECT_BOTH)) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Failed to apply capabilities: %d"), ret);
        return -1;
    }

    /* We do not need to call capng_lock() in this case. The bounding
     * set restriction will prevent them reacquiring sys_boot/module/time,
     * etc which is all that matters for the container. Once inside the
     * container it is fine for SECURE_NOROOT / SECURE_NO_SETUID_FIXUP to
     * be unmasked  - they can never escape the bounding set. */

#else
    VIR_WARN("libcap-ng support not compiled in, unable to clear capabilities");
#endif
    return 0;
}


/**
 * lxcContainerChild:
 * @data: pointer to container arguments
 *
 * This function is run in the process clone()'d in lxcStartContainer.
 * Perform a number of container setup tasks:
 *     Setup container file system
 *     mount container /proca
 * Then exec's the container init
 *
 * Returns 0 on success or -1 in case of error
 */
static int lxcContainerChild( void *data )
{
    lxc_child_argv_t *argv = data;
    virDomainDefPtr vmDef = argv->config;
    int ttyfd = -1;
    int ret = -1;
    char *ttyPath = NULL;
    virDomainFSDefPtr root;
    virCommandPtr cmd = NULL;
    int hasReboot;

    if (NULL == vmDef) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       "%s", _("lxcChild() passed invalid vm definition"));
        goto cleanup;
    }

    if ((hasReboot = lxcContainerHasReboot()) < 0)
        goto cleanup;

    cmd = lxcContainerBuildInitCmd(vmDef);
    virCommandWriteArgLog(cmd, 1);

    root = virDomainGetRootFilesystem(vmDef);

    if (argv->nttyPaths) {
        if (root) {
            if (virAsprintf(&ttyPath, "%s%s", root->src, argv->ttyPaths[0]) < 0) {
                virReportOOMError();
                goto cleanup;
            }
        } else {
            if (!(ttyPath = strdup(argv->ttyPaths[0]))) {
                virReportOOMError();
                goto cleanup;
            }
        }
    } else {
        if (!(ttyPath = strdup("/dev/null"))) {
            virReportOOMError();
            goto cleanup;
        }
    }

    VIR_DEBUG("Container TTY path: %s", ttyPath);

    ttyfd = open(ttyPath, O_RDWR|O_NOCTTY);
    if (ttyfd < 0) {
        virReportSystemError(errno,
                             _("Failed to open tty %s"),
                             ttyPath);
        goto cleanup;
    }

    if (lxcContainerSetupMounts(vmDef, root,
                                argv->ttyPaths, argv->nttyPaths,
                                argv->securityDriver) < 0)
        goto cleanup;

    if (!virFileExists(vmDef->os.init)) {
        virReportSystemError(errno,
                    _("cannot find init path '%s' relative to container root"),
                    vmDef->os.init);
        goto cleanup;
    }

    /* Wait for interface devices to show up */
    if (lxcContainerWaitForContinue(argv->monitor) < 0) {
        virReportSystemError(errno, "%s",
                             _("Failed to read the container continue message"));
        goto cleanup;
    }
    VIR_DEBUG("Received container continue message");

    /* rename and enable interfaces */
    if (lxcContainerRenameAndEnableInterfaces(!!(vmDef->features &
                                                 (1 << VIR_DOMAIN_FEATURE_PRIVNET)),
                                              argv->nveths,
                                              argv->veths) < 0) {
        goto cleanup;
    }

    /* drop a set of root capabilities */
    if (lxcContainerDropCapabilities(!!hasReboot) < 0)
        goto cleanup;

    if (lxcContainerSendContinue(argv->handshakefd) < 0) {
        virReportSystemError(errno, "%s",
                            _("failed to send continue signal to controller"));
        goto cleanup;
    }

    VIR_DEBUG("Setting up security labeling");
    if (virSecurityManagerSetProcessLabel(argv->securityDriver, vmDef) < 0)
        goto cleanup;

    if (lxcContainerSetStdio(argv->monitor, ttyfd, argv->handshakefd) < 0) {
        goto cleanup;
    }

    ret = 0;
cleanup:
    VIR_FREE(ttyPath);
    VIR_FORCE_CLOSE(ttyfd);
    VIR_FORCE_CLOSE(argv->monitor);
    VIR_FORCE_CLOSE(argv->handshakefd);

    if (ret == 0) {
        /* this function will only return if an error occurred */
        ret = virCommandExec(cmd);
    }

    virCommandFree(cmd);
    return ret;
}

static int userns_supported(void)
{
#if 1
    /*
     * put off using userns until uid mapping is implemented
     */
    return 0;
#else
    return lxcContainerAvailable(LXC_CONTAINER_FEATURE_USER) == 0;
#endif
}

const char *lxcContainerGetAlt32bitArch(const char *arch)
{
    /* Any Linux 64bit arch which has a 32bit
     * personality available should be listed here */
    if (STREQ(arch, "x86_64"))
        return "i686";
    if (STREQ(arch, "s390x"))
        return "s390";
    if (STREQ(arch, "ppc64"))
        return "ppc";
    if (STREQ(arch, "parisc64"))
        return "parisc";
    if (STREQ(arch, "sparc64"))
        return "sparc";
    if (STREQ(arch, "mips64"))
        return "mips";

    return NULL;
}


/**
 * lxcContainerStart:
 * @def: pointer to virtual machine structure
 * @nveths: number of interfaces
 * @veths: interface names
 * @control: control FD to the container
 * @ttyPath: path of tty to set as the container console
 *
 * Starts a container process by calling clone() with the namespace flags
 *
 * Returns PID of container on success or -1 in case of error
 */
int lxcContainerStart(virDomainDefPtr def,
                      virSecurityManagerPtr securityDriver,
                      size_t nveths,
                      char **veths,
                      int control,
                      int handshakefd,
                      char **ttyPaths,
                      size_t nttyPaths)
{
    pid_t pid;
    int cflags;
    int stacksize = getpagesize() * 4;
    char *stack, *stacktop;
    lxc_child_argv_t args = { def, securityDriver,
                              nveths, veths, control,
                              ttyPaths, nttyPaths, handshakefd};

    /* allocate a stack for the container */
    if (VIR_ALLOC_N(stack, stacksize) < 0) {
        virReportOOMError();
        return -1;
    }
    stacktop = stack + stacksize;

    cflags = CLONE_NEWPID|CLONE_NEWNS|CLONE_NEWUTS|CLONE_NEWIPC|SIGCHLD;

    if (userns_supported()) {
        VIR_DEBUG("Enable user namespaces");
        cflags |= CLONE_NEWUSER;
    }

    if (def->nets != NULL ||
        (def->features & (1 << VIR_DOMAIN_FEATURE_PRIVNET))) {
        VIR_DEBUG("Enable network namespaces");
        cflags |= CLONE_NEWNET;
    }

    pid = clone(lxcContainerChild, stacktop, cflags, &args);
    VIR_FREE(stack);
    VIR_DEBUG("clone() completed, new container PID is %d", pid);

    if (pid < 0) {
        virReportSystemError(errno, "%s",
                             _("Failed to run clone container"));
        return -1;
    }

    return pid;
}

ATTRIBUTE_NORETURN static int
lxcContainerDummyChild(void *argv ATTRIBUTE_UNUSED)
{
    _exit(0);
}

int lxcContainerAvailable(int features)
{
    int flags = CLONE_NEWPID|CLONE_NEWNS|CLONE_NEWUTS|
        CLONE_NEWIPC|SIGCHLD;
    int cpid;
    char *childStack;
    char *stack;

    if (features & LXC_CONTAINER_FEATURE_USER)
        flags |= CLONE_NEWUSER;

    if (features & LXC_CONTAINER_FEATURE_NET)
        flags |= CLONE_NEWNET;

    if (VIR_ALLOC_N(stack, getpagesize() * 4) < 0) {
        VIR_DEBUG("Unable to allocate stack");
        return -1;
    }

    childStack = stack + (getpagesize() * 4);

    cpid = clone(lxcContainerDummyChild, childStack, flags, NULL);
    VIR_FREE(stack);
    if (cpid < 0) {
        char ebuf[1024] ATTRIBUTE_UNUSED;
        VIR_DEBUG("clone call returned %s, container support is not enabled",
                  virStrerror(errno, ebuf, sizeof(ebuf)));
        return -1;
    } else if (virPidWait(cpid, NULL) < 0) {
        return -1;
    }

    VIR_DEBUG("container support is enabled");
    return 0;
}
