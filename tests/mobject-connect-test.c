/*
 * (C) 2017 The University of Chicago
 * 
 * See COPYRIGHT in top-level directory.
 */

#include "mobject-store-config.h"

#include <stdio.h>
#include <assert.h>

#ifdef USE_RADOS
#include <rados/librados.h>
#else
#include <librados-mobject-store.h>
#endif

int main(int argc, char *argv[])
{
    rados_t cluster;
    int ret;

    ret = rados_create(&cluster, "admin");
    assert(ret == 0);

    ret = rados_connect(cluster);
    assert(ret == 0);

    rados_shutdown(cluster);

    return(0);
}
