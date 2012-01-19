/*
 * Copyright (C) 2010-2011 Red Hat, Inc.
 * Copyright (C) 2010 IBM Corporation
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
 * Authors:
 *     Stefan Berger <stefanb@us.ibm.com>
 *
 * Notes:
 * netlink: http://lovezutto.googlepages.com/netlink.pdf
 *          iproute2 package
 *
 */

#include <config.h>


#include "virnetdevmacvlan.h"
#include "util.h"
#include "virterror_internal.h"

#define VIR_FROM_THIS VIR_FROM_NET

#define virNetDevError(code, ...)                                       \
    virReportErrorHelper(VIR_FROM_NET, code, __FILE__,                  \
                         __FUNCTION__, __LINE__, __VA_ARGS__)


VIR_ENUM_IMPL(virNetDevMacVLanMode, VIR_NETDEV_MACVLAN_MODE_LAST,
              "vepa",
              "private",
              "bridge",
              "passthrough")

#if WITH_MACVTAP

# include <stdint.h>
# include <stdio.h>
# include <errno.h>
# include <fcntl.h>
# include <sys/socket.h>
# include <sys/ioctl.h>

# include <linux/if.h>
# include <linux/if_tun.h>

/* Older kernels lacked this enum value.  */
# if !HAVE_DECL_MACVLAN_MODE_PASSTHRU
#  define MACVLAN_MODE_PASSTHRU 8
# endif

# include "memory.h"
# include "logging.h"
# include "uuid.h"
# include "virfile.h"
# include "netlink.h"
# include "virnetdev.h"

# define MACVTAP_NAME_PREFIX	"macvtap"
# define MACVTAP_NAME_PATTERN	"macvtap%d"

# define MACVLAN_NAME_PREFIX	"macvlan"
# define MACVLAN_NAME_PATTERN	"macvlan%d"

/**
 * virNetDevMacVLanCreate:
 *
 * @ifname: The name the interface is supposed to have; optional parameter
 * @type: The type of device, i.e., "macvtap", "macvlan"
 * @macaddress: The MAC address of the device
 * @srcdev: The name of the 'link' device
 * @macvlan_mode: The macvlan mode to use
 * @retry: Pointer to integer that will be '1' upon return if an interface
 *         with the same name already exists and it is worth to try
 *         again with a different name
 *
 * Create a macvtap device with the given properties.
 *
 * Returns 0 on success, -1 on fatal error.
 */
int
virNetDevMacVLanCreate(const char *ifname,
                       const char *type,
                       const unsigned char *macaddress,
                       const char *srcdev,
                       uint32_t macvlan_mode,
                       int *retry)
{
    int rc = 0;
    struct nlmsghdr *resp;
    struct nlmsgerr *err;
    struct ifinfomsg ifinfo = { .ifi_family = AF_UNSPEC };
    int ifindex;
    unsigned char *recvbuf = NULL;
    unsigned int recvbuflen;
    struct nl_msg *nl_msg;
    struct nlattr *linkinfo, *info_data;

    if (virNetDevGetIndex(srcdev, &ifindex) < 0)
        return -1;

    *retry = 0;

    nl_msg = nlmsg_alloc_simple(RTM_NEWLINK,
                                NLM_F_REQUEST | NLM_F_CREATE | NLM_F_EXCL);
    if (!nl_msg) {
        virReportOOMError();
        return -1;
    }

    if (nlmsg_append(nl_msg,  &ifinfo, sizeof(ifinfo), NLMSG_ALIGNTO) < 0)
        goto buffer_too_small;

    if (nla_put_u32(nl_msg, IFLA_LINK, ifindex) < 0)
        goto buffer_too_small;

    if (nla_put(nl_msg, IFLA_ADDRESS, VIR_MAC_BUFLEN, macaddress) < 0)
        goto buffer_too_small;

    if (ifname &&
        nla_put(nl_msg, IFLA_IFNAME, strlen(ifname)+1, ifname) < 0)
        goto buffer_too_small;

    if (!(linkinfo = nla_nest_start(nl_msg, IFLA_LINKINFO)))
        goto buffer_too_small;

    if (nla_put(nl_msg, IFLA_INFO_KIND, strlen(type), type) < 0)
        goto buffer_too_small;

    if (macvlan_mode > 0) {
        if (!(info_data = nla_nest_start(nl_msg, IFLA_INFO_DATA)))
            goto buffer_too_small;

        if (nla_put(nl_msg, IFLA_MACVLAN_MODE, sizeof(macvlan_mode),
                    &macvlan_mode) < 0)
            goto buffer_too_small;

        nla_nest_end(nl_msg, info_data);
    }

    nla_nest_end(nl_msg, linkinfo);

    if (nlComm(nl_msg, &recvbuf, &recvbuflen, 0) < 0) {
        rc = -1;
        goto cleanup;
    }

    if (recvbuflen < NLMSG_LENGTH(0) || recvbuf == NULL)
        goto malformed_resp;

    resp = (struct nlmsghdr *)recvbuf;

    switch (resp->nlmsg_type) {
    case NLMSG_ERROR:
        err = (struct nlmsgerr *)NLMSG_DATA(resp);
        if (resp->nlmsg_len < NLMSG_LENGTH(sizeof(*err)))
            goto malformed_resp;

        switch (err->error) {

        case 0:
            break;

        case -EEXIST:
            *retry = 1;
            rc = -1;
            break;

        default:
            virReportSystemError(-err->error,
                                 _("error creating %s type of interface"),
                                 type);
            rc = -1;
        }
        break;

    case NLMSG_DONE:
        break;

    default:
        goto malformed_resp;
    }

cleanup:
    nlmsg_free(nl_msg);

    VIR_FREE(recvbuf);

    return rc;

malformed_resp:
    nlmsg_free(nl_msg);

    virNetDevError(VIR_ERR_INTERNAL_ERROR, "%s",
                   _("malformed netlink response message"));
    VIR_FREE(recvbuf);
    return -1;

buffer_too_small:
    nlmsg_free(nl_msg);

    virNetDevError(VIR_ERR_INTERNAL_ERROR, "%s",
                   _("allocated netlink buffer is too small"));
    return -1;
}

/**
 * virNetDevMacVLanDelete:
 *
 * @ifname: Name of the interface
 *
 * Tear down an interface with the given name.
 *
 * Returns 0 on success, -1 on fatal error.
 */
int virNetDevMacVLanDelete(const char *ifname)
{
    int rc = 0;
    struct nlmsghdr *resp;
    struct nlmsgerr *err;
    struct ifinfomsg ifinfo = { .ifi_family = AF_UNSPEC };
    unsigned char *recvbuf = NULL;
    unsigned int recvbuflen;
    struct nl_msg *nl_msg;

    nl_msg = nlmsg_alloc_simple(RTM_DELLINK,
                                NLM_F_REQUEST | NLM_F_CREATE | NLM_F_EXCL);
    if (!nl_msg) {
        virReportOOMError();
        return -1;
    }

    if (nlmsg_append(nl_msg,  &ifinfo, sizeof(ifinfo), NLMSG_ALIGNTO) < 0)
        goto buffer_too_small;

    if (nla_put(nl_msg, IFLA_IFNAME, strlen(ifname)+1, ifname) < 0)
        goto buffer_too_small;

    if (nlComm(nl_msg, &recvbuf, &recvbuflen, 0) < 0) {
        rc = -1;
        goto cleanup;
    }

    if (recvbuflen < NLMSG_LENGTH(0) || recvbuf == NULL)
        goto malformed_resp;

    resp = (struct nlmsghdr *)recvbuf;

    switch (resp->nlmsg_type) {
    case NLMSG_ERROR:
        err = (struct nlmsgerr *)NLMSG_DATA(resp);
        if (resp->nlmsg_len < NLMSG_LENGTH(sizeof(*err)))
            goto malformed_resp;

        if (err->error) {
            virReportSystemError(-err->error,
                                 _("error destroying %s interface"),
                                 ifname);
            rc = -1;
        }
        break;

    case NLMSG_DONE:
        break;

    default:
        goto malformed_resp;
    }

cleanup:
    nlmsg_free(nl_msg);

    VIR_FREE(recvbuf);

    return rc;

malformed_resp:
    nlmsg_free(nl_msg);

    virNetDevError(VIR_ERR_INTERNAL_ERROR, "%s",
                   _("malformed netlink response message"));
    VIR_FREE(recvbuf);
    return -1;

buffer_too_small:
    nlmsg_free(nl_msg);

    virNetDevError(VIR_ERR_INTERNAL_ERROR, "%s",
                   _("allocated netlink buffer is too small"));
    return -1;
}


/**
 * virNetDevMacVLanTapOpen:
 * Open the macvtap's tap device.
 * @ifname: Name of the macvtap interface
 * @retries : Number of retries in case udev for example may need to be
 *            waited for to create the tap chardev
 * Returns negative value in case of error, the file descriptor otherwise.
 */
static
int virNetDevMacVLanTapOpen(const char *ifname,
                            int retries)
{
    FILE *file;
    char path[64];
    int ifindex;
    char tapname[50];
    int tapfd;

    if (snprintf(path, sizeof(path),
                 "/sys/class/net/%s/ifindex", ifname) >= sizeof(path)) {
        virReportSystemError(errno,
                             "%s",
                             _("buffer for ifindex path is too small"));
        return -1;
    }

    file = fopen(path, "r");

    if (!file) {
        virReportSystemError(errno,
                             _("cannot open macvtap file %s to determine "
                               "interface index"), path);
        return -1;
    }

    if (fscanf(file, "%d", &ifindex) != 1) {
        virReportSystemError(errno,
                             "%s",_("cannot determine macvtap's tap device "
                             "interface index"));
        VIR_FORCE_FCLOSE(file);
        return -1;
    }

    VIR_FORCE_FCLOSE(file);

    if (snprintf(tapname, sizeof(tapname),
                 "/dev/tap%d", ifindex) >= sizeof(tapname)) {
        virReportSystemError(errno,
                             "%s",
                             _("internal buffer for tap device is too small"));
        return -1;
    }

    while (1) {
        /* may need to wait for udev to be done */
        tapfd = open(tapname, O_RDWR);
        if (tapfd < 0 && retries > 0) {
            retries--;
            usleep(20000);
            continue;
        }
        break;
    }

    if (tapfd < 0)
        virReportSystemError(errno,
                             _("cannot open macvtap tap device %s"),
                             tapname);

    return tapfd;
}


/**
 * virNetDevMacVLanTapSetup:
 * @tapfd: file descriptor of the macvtap tap
 * @vnet_hdr: 1 to enable IFF_VNET_HDR, 0 to disable it
 *
 * Returns 0 on success, -1 in case of fatal error, error code otherwise.
 *
 * Turn the IFF_VNET_HDR flag, if requested and available, make sure
 * it's off in the other cases.
 * A fatal error is defined as the VNET_HDR flag being set but it cannot
 * be turned off for some reason. This is reported with -1. Other fatal
 * error is not being able to read the interface flags. In that case the
 * macvtap device should not be used.
 */
static int
virNetDevMacVLanTapSetup(int tapfd, int vnet_hdr)
{
    unsigned int features;
    struct ifreq ifreq;
    short new_flags = 0;
    int rc_on_fail = 0;
    const char *errmsg = NULL;

    memset(&ifreq, 0, sizeof(ifreq));

    if (ioctl(tapfd, TUNGETIFF, &ifreq) < 0) {
        virReportSystemError(errno, "%s",
                             _("cannot get interface flags on macvtap tap"));
        return -1;
    }

    new_flags = ifreq.ifr_flags;

    if ((ifreq.ifr_flags & IFF_VNET_HDR) && !vnet_hdr) {
        new_flags = ifreq.ifr_flags & ~IFF_VNET_HDR;
        rc_on_fail = -1;
        errmsg = _("cannot clean IFF_VNET_HDR flag on macvtap tap");
    } else if ((ifreq.ifr_flags & IFF_VNET_HDR) == 0 && vnet_hdr) {
        if (ioctl(tapfd, TUNGETFEATURES, &features) < 0) {
            virReportSystemError(errno, "%s",
                   _("cannot get feature flags on macvtap tap"));
            return -1;
        }
        if ((features & IFF_VNET_HDR)) {
            new_flags = ifreq.ifr_flags | IFF_VNET_HDR;
            errmsg = _("cannot set IFF_VNET_HDR flag on macvtap tap");
        }
    }

    if (new_flags != ifreq.ifr_flags) {
        ifreq.ifr_flags = new_flags;
        if (ioctl(tapfd, TUNSETIFF, &ifreq) < 0) {
            virReportSystemError(errno, "%s", errmsg);
            return rc_on_fail;
        }
    }

    return 0;
}


static const uint32_t modeMap[VIR_NETDEV_MACVLAN_MODE_LAST] = {
    [VIR_NETDEV_MACVLAN_MODE_VEPA] = MACVLAN_MODE_VEPA,
    [VIR_NETDEV_MACVLAN_MODE_PRIVATE] = MACVLAN_MODE_PRIVATE,
    [VIR_NETDEV_MACVLAN_MODE_BRIDGE] = MACVLAN_MODE_BRIDGE,
    [VIR_NETDEV_MACVLAN_MODE_PASSTHRU] = MACVLAN_MODE_PASSTHRU,
};

/**
 * virNetDevMacVLanCreateWithVPortProfile:
 * Create an instance of a macvtap device and open its tap character
 * device.
 * @tgifname: Interface name that the macvtap is supposed to have. May
 *    be NULL if this function is supposed to choose a name
 * @macaddress: The MAC address for the macvtap device
 * @linkdev: The interface name of the NIC to connect to the external bridge
 * @mode: int describing the mode for 'bridge', 'vepa', 'private' or 'passthru'.
 * @vnet_hdr: 1 to enable IFF_VNET_HDR, 0 to disable it
 * @vmuuid: The UUID of the VM the macvtap belongs to
 * @virtPortProfile: pointer to object holding the virtual port profile data
 * @res_ifname: Pointer to a string pointer where the actual name of the
 *     interface will be stored into if everything succeeded. It is up
 *     to the caller to free the string.
 *
 * Returns file descriptor of the tap device in case of success with @withTap,
 * otherwise returns 0; returns -1 on error.
 */
int virNetDevMacVLanCreateWithVPortProfile(const char *tgifname,
                                           const unsigned char *macaddress,
                                           const char *linkdev,
                                           enum virNetDevMacVLanMode mode,
                                           bool withTap,
                                           int vnet_hdr,
                                           const unsigned char *vmuuid,
                                           virNetDevVPortProfilePtr virtPortProfile,
                                           char **res_ifname,
                                           enum virNetDevVPortProfileOp vmOp,
                                           char *stateDir,
                                           virNetDevBandwidthPtr bandwidth)
{
    const char *type = withTap ? "macvtap" : "macvlan";
    const char *prefix = withTap ? MACVTAP_NAME_PREFIX : MACVLAN_NAME_PREFIX;
    const char *pattern = withTap ? MACVTAP_NAME_PATTERN : MACVLAN_NAME_PATTERN;
    int c, rc;
    char ifname[IFNAMSIZ];
    int retries, do_retry = 0;
    uint32_t macvtapMode;
    const char *cr_ifname;
    int ret;

    macvtapMode = modeMap[mode];

    *res_ifname = NULL;

    VIR_DEBUG("%s: VM OPERATION: %s", __FUNCTION__, virNetDevVPortProfileOpTypeToString(vmOp));

    /** Note: When using PASSTHROUGH mode with MACVTAP devices the link
     * device's MAC address must be set to the VMs MAC address. In
     * order to not confuse the first switch or bridge in line this MAC
     * address must be reset when the VM is shut down.
     * This is especially important when using SRIOV capable cards that
     * emulate their switch in firmware.
     */
    if (mode == VIR_NETDEV_MACVLAN_MODE_PASSTHRU) {
        if (virNetDevReplaceMacAddress(linkdev, macaddress, stateDir) < 0)
            return -1;
    }

    if (tgifname) {
        if ((ret = virNetDevExists(tgifname)) < 0)
            return -1;

        if (ret) {
            if (STRPREFIX(tgifname, prefix)) {
                goto create_name;
            }
            virReportSystemError(EEXIST,
                                 _("Unable to create macvlan device %s"), tgifname);
            return -1;
        }
        cr_ifname = tgifname;
        rc = virNetDevMacVLanCreate(tgifname, type, macaddress, linkdev,
                                    macvtapMode, &do_retry);
        if (rc < 0)
            return -1;
    } else {
create_name:
        retries = 5;
        for (c = 0; c < 8192; c++) {
            snprintf(ifname, sizeof(ifname), pattern, c);
            if ((ret = virNetDevExists(ifname)) < 0)
                return -1;
            if (!ret) {
                rc = virNetDevMacVLanCreate(ifname, type, macaddress, linkdev,
                                            macvtapMode, &do_retry);
                if (rc == 0)
                    break;

                if (do_retry && --retries)
                    continue;
                return -1;
            }
        }
        cr_ifname = ifname;
    }

    if (virNetDevVPortProfileAssociate(cr_ifname,
                                       virtPortProfile,
                                       macaddress,
                                       linkdev,
                                       vmuuid, vmOp) < 0) {
        rc = -1;
        goto link_del_exit;
    }

    if (virNetDevSetOnline(cr_ifname, true) < 0) {
        rc = -1;
        goto disassociate_exit;
    }

    if (withTap) {
        if ((rc = virNetDevMacVLanTapOpen(cr_ifname, 10)) < 0)
            goto disassociate_exit;

        if (virNetDevMacVLanTapSetup(rc, vnet_hdr) < 0) {
            VIR_FORCE_CLOSE(rc); /* sets rc to -1 */
            goto disassociate_exit;
        }
        if (!(*res_ifname = strdup(cr_ifname))) {
            VIR_FORCE_CLOSE(rc); /* sets rc to -1 */
            virReportOOMError();
            goto disassociate_exit;
        }
    } else {
        if (!(*res_ifname = strdup(cr_ifname))) {
            virReportOOMError();
            goto disassociate_exit;
        }
        rc = 0;
    }

    if (virNetDevBandwidthSet(cr_ifname, bandwidth) < 0) {
        virNetDevError(VIR_ERR_INTERNAL_ERROR,
                       _("cannot set bandwidth limits on %s"),
                       cr_ifname);
        if (withTap)
            VIR_FORCE_CLOSE(rc); /* sets rc to -1 */
        else
            rc = -1;
        goto disassociate_exit;
    }


    return rc;

disassociate_exit:
    ignore_value(virNetDevVPortProfileDisassociate(cr_ifname,
                                                   virtPortProfile,
                                                   macaddress,
                                                   linkdev,
                                                   vmOp));

link_del_exit:
    ignore_value(virNetDevMacVLanDelete(cr_ifname));

    return rc;
}


/**
 * virNetDevMacVLanDeleteWithVPortProfile:
 * @ifname : The name of the macvtap interface
 * @linkdev: The interface name of the NIC to connect to the external bridge
 * @virtPortProfile: pointer to object holding the virtual port profile data
 *
 * Delete an interface given its name. Disassociate
 * it with the switch if port profile parameters
 * were provided.
 */
int virNetDevMacVLanDeleteWithVPortProfile(const char *ifname,
                                           const unsigned char *macaddr,
                                           const char *linkdev,
                                           int mode,
                                           virNetDevVPortProfilePtr virtPortProfile,
                                           char *stateDir)
{
    int ret = 0;
    if (mode == VIR_NETDEV_MACVLAN_MODE_PASSTHRU) {
        ignore_value(virNetDevRestoreMacAddress(linkdev, stateDir));
    }

    if (ifname) {
        if (virNetDevVPortProfileDisassociate(ifname,
                                              virtPortProfile,
                                              macaddr,
                                              linkdev,
                                              VIR_NETDEV_VPORT_PROFILE_OP_DESTROY) < 0)
            ret = -1;
        if (virNetDevMacVLanDelete(ifname) < 0)
            ret = -1;
    }
    return ret;
}

#else /* ! WITH_MACVTAP */
int virNetDevMacVLanCreate(const char *ifname ATTRIBUTE_UNUSED,
                           const char *type ATTRIBUTE_UNUSED,
                           const unsigned char *macaddress ATTRIBUTE_UNUSED,
                           const char *srcdev ATTRIBUTE_UNUSED,
                           uint32_t macvlan_mode ATTRIBUTE_UNUSED,
                           int *retry ATTRIBUTE_UNUSED)
{
    virReportSystemError(ENOSYS, "%s",
                         _("Cannot create macvlan devices on this platform"));
    return -1;
}

int virNetDevMacVLanDelete(const char *ifname ATTRIBUTE_UNUSED)
{
    virReportSystemError(ENOSYS, "%s",
                         _("Cannot create macvlan devices on this platform"));
    return -1;
}

int virNetDevMacVLanCreateWithVPortProfile(const char *ifname ATTRIBUTE_UNUSED,
                                           const unsigned char *macaddress ATTRIBUTE_UNUSED,
                                           const char *linkdev ATTRIBUTE_UNUSED,
                                           enum virNetDevMacVLanMode mode ATTRIBUTE_UNUSED,
                                           bool withTap ATTRIBUTE_UNUSED,
                                           int vnet_hdr ATTRIBUTE_UNUSED,
                                           const unsigned char *vmuuid ATTRIBUTE_UNUSED,
                                           virNetDevVPortProfilePtr virtPortProfile ATTRIBUTE_UNUSED,
                                           char **res_ifname ATTRIBUTE_UNUSED,
                                           enum virNetDevVPortProfileOp vmop ATTRIBUTE_UNUSED,
                                           char *stateDir ATTRIBUTE_UNUSED,
                                           virNetDevBandwidthPtr bandwidth ATTRIBUTE_UNUSED)
{
    virReportSystemError(ENOSYS, "%s",
                         _("Cannot create macvlan devices on this platform"));
    return -1;
}

int virNetDevMacVLanDeleteWithVPortProfile(const char *ifname ATTRIBUTE_UNUSED,
                                           const unsigned char *macaddress ATTRIBUTE_UNUSED,
                                           const char *linkdev ATTRIBUTE_UNUSED,
                                           int mode ATTRIBUTE_UNUSED,
                                           virNetDevVPortProfilePtr virtPortProfile ATTRIBUTE_UNUSED,
                                           char *stateDir ATTRIBUTE_UNUSED)
{
    virReportSystemError(ENOSYS, "%s",
                         _("Cannot create macvlan devices on this platform"));
    return -1;
}
#endif /* ! WITH_MACVTAP */
