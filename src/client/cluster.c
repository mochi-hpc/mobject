/*
 * (C) 2017 The University of Chicago
 * 
 * See COPYRIGHT in top-level directory.
 */

#include "mobject-store-config.h"

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

#include <margo.h>
#include <ssg.h>

#include "libmobject-store.h"
#include "src/client/cluster.h"
#include "src/io-chain/prepare-write-op.h"
#include "src/io-chain/prepare-read-op.h"
#include "src/rpc-types/write-op.h"
#include "src/rpc-types/read-op.h"
#include "src/util/log.h"

static unsigned long sdbm_hash(const char* str);

static int mobject_store_shutdown_servers(struct mobject_store_handle *cluster_handle);

int mobject_store_create(mobject_store_t *cluster, const char * const id)
{
    struct mobject_store_handle *cluster_handle;
    char *cluster_file;
    int ret;

    (void)id; /* XXX: id unused in mobject */

    printf("***SDS MOBJECT IN RR MODE\n");

    /* allocate a new cluster handle and set some fields */
    cluster_handle = (struct mobject_store_handle*)calloc(1,sizeof(*cluster_handle));
    if (!cluster_handle)
        return -1;

    /* use env variable to determine how to connect to the cluster */
    /* NOTE: this is the _only_ method for specifying a cluster for now... */
    cluster_file = getenv(MOBJECT_CLUSTER_FILE_ENV);
    if (!cluster_file)
    {
        fprintf(stderr, "Error: %s env variable must point to mobject cluster file\n",
                MOBJECT_CLUSTER_FILE_ENV);
        free(cluster_handle);
        return -1;
    }

    ret = ssg_group_id_load(cluster_file, &cluster_handle->gid);
    if (ret != 0)
    {
        fprintf(stderr, "Error: Unable to load mobject cluster info from file %s\n",
                cluster_file);
        free(cluster_handle);
        return -1;
    }

    /* set the returned cluster handle */
    *cluster = cluster_handle;

    return 0;
}

int mobject_store_connect(mobject_store_t cluster)
{
    struct mobject_store_handle *cluster_handle;
    char *svr_addr_str;
    char proto[24] = {0};
    int i;
    int ret;

    cluster_handle = (struct mobject_store_handle *)cluster;
    assert(cluster_handle);

    if (cluster_handle->connected)
        return 0;

    /* figure out protocol to connect with using address information 
     * associated with the SSG group ID
     */
    svr_addr_str = ssg_group_id_get_addr_str(cluster_handle->gid);
    if (!svr_addr_str)
    {
        fprintf(stderr, "Error: Unable to obtain cluster group server address\n");
        ssg_group_id_free(cluster_handle->gid);
        free(cluster_handle);
        return -1;
    }
    /* we only need to get the proto portion of the address to initialize */
    for(i=0; i<24 && svr_addr_str[i] != '\0' && svr_addr_str[i] != ':'; i++)
        proto[i] = svr_addr_str[i];

    /* intialize margo */
    /* XXX: probably want to expose some way of tweaking threading parameters */
    margo_instance_id mid = margo_init(proto, MARGO_SERVER_MODE, 0, -1);
    if (mid == MARGO_INSTANCE_NULL)
    {
        fprintf(stderr, "Error: Unable to initialize margo\n");
        free(svr_addr_str);
        ssg_group_id_free(cluster_handle->gid);
        free(cluster_handle);
        return -1;
    }
    cluster_handle->mid = mid;

    /* initialize ssg */
    /* XXX: we need to think about how to do this once per-client... clients could connect to mult. clusters */
    ret = ssg_init(cluster_handle->mid);
    if (ret != SSG_SUCCESS)
    {
        fprintf(stderr, "Error: Unable to initialize SSG\n");
        margo_finalize(cluster_handle->mid);
        free(svr_addr_str);
        ssg_group_id_free(cluster_handle->gid);
        free(cluster_handle);
        return -1;
    }

    /* attach to the cluster group */
    ret = ssg_group_attach(cluster_handle->gid);
    if (ret != SSG_SUCCESS)
    {
        fprintf(stderr, "Error: Unable to attach to the mobject cluster group\n");
        ssg_finalize();
        margo_finalize(cluster_handle->mid);
        free(svr_addr_str);
        ssg_group_id_free(cluster_handle->gid);
        free(cluster_handle);
        return -1;
    }
    cluster_handle->connected = 1;

    // get number of servers
    int gsize = ssg_get_group_size(cluster_handle->gid);
    if(gsize == 0)
    {
        fprintf(stderr, "Error: Unable to get SSG group size\n");
        ssg_finalize();
        margo_finalize(cluster_handle->mid);
        free(svr_addr_str);
        ssg_group_id_free(cluster_handle->gid);
        free(cluster_handle);
        return -1;
    }

    // initialize ch-placement
    cluster_handle->ch_instance = 
        ch_placement_initialize("static_modulo", gsize, 0, 0);
    if(!cluster_handle->ch_instance)
    {
        fprintf(stderr, "Error: Unable to initialize ch-placement instance\n");
        ssg_finalize();
        margo_finalize(cluster_handle->mid);
        free(svr_addr_str);
        ssg_group_id_free(cluster_handle->gid);
        free(cluster_handle);
        return -1;
    }

    // initialize mobject client
    ret = mobject_client_init(mid, &(cluster_handle->mobject_clt));
    if(ret != 0)
    {
        fprintf(stderr, "Error: Unable to create a mobject client\n");
        free(cluster_handle);
        return -1;
    }

    free(svr_addr_str);

    return 0;
}

void mobject_store_shutdown(mobject_store_t cluster)
{
    struct mobject_store_handle *cluster_handle;
    char *svr_kill_env_str;
    int ret;

    cluster_handle = (struct mobject_store_handle *)cluster;
    assert(cluster_handle != NULL);

    if (!cluster_handle->connected)
        return;

    svr_kill_env_str = getenv(MOBJECT_CLUSTER_SHUTDOWN_KILL_ENV);
    if (svr_kill_env_str && !strcmp(svr_kill_env_str, "true"))
    {
        /* kill server cluster if requested */
        ret = mobject_store_shutdown_servers(cluster_handle);
        if (ret != 0)
        {
            fprintf(stderr, "Warning: Unable to send shutdown signal \
                    to mobject server cluster\n");
        }
    }

    mobject_client_finalize(cluster_handle->mobject_clt);
    ssg_group_detach(cluster_handle->gid);
    ssg_finalize();
    margo_finalize(cluster_handle->mid);
    ssg_group_id_free(cluster_handle->gid);
    ch_placement_finalize(cluster_handle->ch_instance);
    free(cluster_handle);

    return;
}

int mobject_store_pool_create(mobject_store_t cluster, const char * pool_name)
{
    /* XXX: this is a NOOP -- we don't implement pools currently */
    (void)cluster;
    (void)pool_name;
    return 0;
}

int mobject_store_pool_delete(mobject_store_t cluster, const char * pool_name)
{
    /* XXX: this is a NOOP -- we don't implement pools currently */
    (void)cluster;
    (void)pool_name;
    return 0;
}

int mobject_store_ioctx_create(
        mobject_store_t cluster,
        const char * pool_name,
        mobject_store_ioctx_t *ioctx)
{
    *ioctx = (mobject_store_ioctx_t)calloc(1, sizeof(**ioctx));
    (*ioctx)->pool_name = strdup(pool_name);
    (*ioctx)->cluster   = cluster;
    return 0;
}

void mobject_store_ioctx_destroy(mobject_store_ioctx_t ioctx)
{
    if(ioctx) free(ioctx->pool_name);
    free(ioctx);
}

mobject_store_write_op_t mobject_store_create_write_op(void)
{
    return mobject_create_write_op();
}

void mobject_store_release_write_op(mobject_store_write_op_t write_op)
{
    mobject_release_write_op(write_op);
}

void mobject_store_write_op_create(mobject_store_write_op_t write_op,
        int exclusive,
        const char* category)
{
    mobject_write_op_create(write_op, exclusive, category);
}

void mobject_store_write_op_write(mobject_store_write_op_t write_op,
        const char *buffer,
        size_t len,
        uint64_t offset)
{
    // fields are exchanged in the mobject-client API, it's normal
    mobject_write_op_write(write_op, buffer, offset, len);
}

void mobject_store_write_op_write_full(mobject_store_write_op_t write_op,
        const char *buffer,
        size_t len)
{
    return mobject_write_op_write_full(write_op, buffer, len);
}

void mobject_store_write_op_writesame(mobject_store_write_op_t write_op,
        const char *buffer,
        size_t data_len,
        size_t write_len,
        uint64_t offset)
{
    // fields are exchanged in the mobject-client API, it's normal
    mobject_write_op_write_same(write_op, buffer, offset, data_len, write_len);
}

void mobject_store_write_op_append(mobject_store_write_op_t write_op,
        const char *buffer,
        size_t len)
{
    mobject_write_op_append(write_op, buffer, len);
}

void mobject_store_write_op_remove(mobject_store_write_op_t write_op)
{
    mobject_write_op_remove(write_op);
}

void mobject_store_write_op_truncate(mobject_store_write_op_t write_op,
        uint64_t offset)
{
    mobject_write_op_truncate(write_op, offset);
}

void mobject_store_write_op_zero(mobject_store_write_op_t write_op,
        uint64_t offset,
        uint64_t len)
{
    mobject_write_op_zero(write_op, offset, len);
}

void mobject_store_write_op_omap_set(mobject_store_write_op_t write_op,
        char const* const* keys,
        char const* const* vals,
        const size_t *lens,
        size_t num)
{
    mobject_write_op_omap_set(write_op, keys, vals, lens, num);
}

void mobject_store_write_op_omap_rm_keys(mobject_store_write_op_t write_op,
        char const* const* keys,
        size_t keys_len)
{
    mobject_write_op_omap_rm_keys(write_op, keys, keys_len);
}

int mobject_store_write_op_operate(mobject_store_write_op_t write_op,
        mobject_store_ioctx_t io,
        const char *oid,
        time_t *mtime,
        int flags)
{
    mobject_provider_handle_t mph = MOBJECT_PROVIDER_HANDLE_NULL;
    uint64_t oid_hash = sdbm_hash(oid);

    const char *sep = strchr(oid, '.');
    const char *str_rank = ++sep;
    int rank = atoi(str_rank);
    int gsize = ssg_get_group_size(io->cluster->gid);
    unsigned long server_idx = rank % gsize;

    // XXX multiple providers may be in the same node (with distinct mplex ids)
    hg_addr_t svr_addr = ssg_get_addr(io->cluster->gid, server_idx);
    // TODO for now multiplex id is hard-coded as 1
    int r = mobject_provider_handle_create(io->cluster->mobject_clt, svr_addr, 1, &mph);
    if(r != 0) return r;

    r = mobject_write_op_operate(mph, write_op, io->pool_name, oid, mtime, flags);
    mobject_provider_handle_release(mph);
    return r;
}

mobject_store_read_op_t mobject_store_create_read_op(void)
{
    return mobject_create_read_op();
}

void mobject_store_release_read_op(mobject_store_read_op_t read_op)
{
    mobject_release_read_op(read_op);
}

void mobject_store_read_op_stat(mobject_store_read_op_t read_op,
        uint64_t *psize,
        time_t *pmtime,
        int *prval)
{
    mobject_read_op_stat(read_op, psize, pmtime, prval);
}

void mobject_store_read_op_read(mobject_store_read_op_t read_op,
        uint64_t offset,
        size_t len,
        char *buffer,
        size_t *bytes_read,
        int *prval)
{
    mobject_read_op_read(read_op, buffer, offset, len, bytes_read, prval);
}

void mobject_store_read_op_omap_get_keys(mobject_store_read_op_t read_op,
        const char *start_after,
        uint64_t max_return,
        mobject_store_omap_iter_t *iter,
        int *prval)
{
    mobject_read_op_omap_get_keys(read_op, start_after, max_return, iter, prval);
}

void mobject_store_read_op_omap_get_vals(mobject_store_read_op_t read_op,
        const char *start_after,
        const char *filter_prefix,
        uint64_t max_return,
        mobject_store_omap_iter_t *iter,
        int *prval)
{
    mobject_read_op_omap_get_vals(read_op, start_after, filter_prefix, max_return, iter, prval);
}

void mobject_store_read_op_omap_get_vals_by_keys(mobject_store_read_op_t read_op,
        char const* const* keys,
        size_t keys_len,
        mobject_store_omap_iter_t *iter,
        int *prval)
{
    mobject_read_op_omap_get_vals_by_keys(read_op, keys, keys_len, iter, prval);
}

int mobject_store_read_op_operate(mobject_store_read_op_t read_op,
        mobject_store_ioctx_t ioctx,
        const char *oid,
        int flags)
{
    mobject_provider_handle_t mph = MOBJECT_PROVIDER_HANDLE_NULL;
    uint64_t oid_hash = sdbm_hash(oid);

    const char *sep = strchr(oid, '.');
    const char *str_rank = ++sep;
    int rank = atoi(str_rank);
    int gsize = ssg_get_group_size(ioctx->cluster->gid);
    unsigned long server_idx = rank % gsize;

    // XXX multiple providers may be in the same node (with distinct mplex ids)
    hg_addr_t svr_addr = ssg_get_addr(ioctx->cluster->gid, server_idx);
    // TODO for now multiplex id is hard-coded as 1
    int r = mobject_provider_handle_create(ioctx->cluster->mobject_clt, svr_addr, 1, &mph);
    if(r != 0) return r;

    r = mobject_read_op_operate(mph,read_op, ioctx->pool_name, oid, flags);
    mobject_provider_handle_release(mph);
    return r;
}

// send a shutdown signal to a server cluster
static int mobject_store_shutdown_servers(struct mobject_store_handle *cluster_handle)
{
    hg_addr_t svr_addr;

    /* get the address of the first server */
    svr_addr = ssg_get_addr(cluster_handle->gid, 0);
    if (svr_addr == HG_ADDR_NULL)
    {
        fprintf(stderr, "Error: Unable to obtain address for mobject server\n");
        return -1;
    }
    // TODO we should actually call that for all the members of the group
    return mobject_shutdown(cluster_handle->mobject_clt, svr_addr);
}

static unsigned long sdbm_hash(const char* str)
{
    unsigned long hash = 0;
    int c;

    while (c = *str++)
        hash = c + (hash << 6) + (hash << 16) - hash;

    return hash;
}
