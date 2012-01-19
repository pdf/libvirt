#include <config.h>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include <sys/types.h>
#include <fcntl.h>

#include "internal.h"
#include "testutils.h"
#include "storage_conf.h"
#include "testutilsqemu.h"

static int
testCompareXMLToXMLFiles(const char *inxml, const char *outxml)
{
    char *inXmlData = NULL;
    char *outXmlData = NULL;
    char *actual = NULL;
    int ret = -1;
    virStoragePoolDefPtr dev = NULL;

    if (virtTestLoadFile(inxml, &inXmlData) < 0)
        goto fail;
    if (virtTestLoadFile(outxml, &outXmlData) < 0)
        goto fail;

    if (!(dev = virStoragePoolDefParseString(inXmlData)))
        goto fail;

    if (!(actual = virStoragePoolDefFormat(dev)))
        goto fail;

    if (STRNEQ(outXmlData, actual)) {
        virtTestDifference(stderr, outXmlData, actual);
        goto fail;
    }

    ret = 0;

 fail:
    free(inXmlData);
    free(outXmlData);
    free(actual);
    virStoragePoolDefFree(dev);
    return ret;
}

static int
testCompareXMLToXMLHelper(const void *data)
{
    int result = -1;
    char *inxml = NULL;
    char *outxml = NULL;

    if (virAsprintf(&inxml, "%s/storagepoolxml2xmlin/%s.xml",
                    abs_srcdir, (const char*)data) < 0 ||
        virAsprintf(&outxml, "%s/storagepoolxml2xmlout/%s.xml",
                    abs_srcdir, (const char*)data) < 0) {
        goto cleanup;
    }

    result = testCompareXMLToXMLFiles(inxml, outxml);

cleanup:
    free(inxml);
    free(outxml);

    return result;
}

static int
mymain(void)
{
    int ret = 0;

#define DO_TEST(name) \
    if (virtTestRun("Storage Pool XML-2-XML " name, \
                    1, testCompareXMLToXMLHelper, (name)) < 0) \
        ret = -1

    DO_TEST("pool-dir");
    DO_TEST("pool-fs");
    DO_TEST("pool-logical");
    DO_TEST("pool-logical-create");
    DO_TEST("pool-disk");
    DO_TEST("pool-iscsi");
    DO_TEST("pool-iscsi-auth");
    DO_TEST("pool-netfs");
    DO_TEST("pool-scsi");
    DO_TEST("pool-mpath");
    DO_TEST("pool-iscsi-multiiqn");
    DO_TEST("pool-iscsi-vendor-product");

    return (ret==0 ? EXIT_SUCCESS : EXIT_FAILURE);
}

VIRT_TEST_MAIN(mymain)
