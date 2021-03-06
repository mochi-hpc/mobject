/*
 * (C) 2018 The University of Chicago
 *
 * See COPYRIGHT in top-level directory.
 */
#include <map>
#include <cstring>
#include <string>
#include <iostream>
#include <limits>
#include <bake-client.h>
#include "src/server/visitor-args.h"
#include "src/io-chain/write-op-visitor.h"

#define ENTERING margo_trace(mid, "[mobject] Entering function %s", __func__);
#define LEAVING  margo_trace(mid, "[mobject] Leaving function %s", __func__);

static void write_op_exec_begin(void*);
static void write_op_exec_end(void*);
static void write_op_exec_create(void*, int);
static void write_op_exec_write(void*, buffer_u, size_t, uint64_t);
static void write_op_exec_write_full(void*, buffer_u, size_t);
static void write_op_exec_writesame(void*, buffer_u, size_t, size_t, uint64_t);
static void write_op_exec_append(void*, buffer_u, size_t);
static void write_op_exec_remove(void*);
static void write_op_exec_truncate(void*, uint64_t);
static void write_op_exec_zero(void*, uint64_t, uint64_t);
static void write_op_exec_omap_set(
    void*, char const* const*, char const* const*, const size_t*, size_t);
static void write_op_exec_omap_rm_keys(void*, char const* const*, size_t);

static oid_t get_or_create_oid(struct mobject_provider* provider,
                               sdskv_provider_handle_t  ph,
                               sdskv_database_id_t      name_db_id,
                               sdskv_database_id_t      oid_db_id,
                               const char*              object_name);

static void insert_region_log_entry(struct mobject_provider* provider,
                                    oid_t                    oid,
                                    uint64_t                 offset,
                                    uint64_t                 len,
                                    bake_region_id_t*        region,
                                    time_t                   ts = 0);

static void insert_small_region_log_entry(struct mobject_provider* provider,
                                          oid_t                    oid,
                                          uint64_t                 offset,
                                          uint64_t                 len,
                                          const char*              data,
                                          time_t                   ts = 0);

static void insert_zero_log_entry(struct mobject_provider* provider,
                                  oid_t                    oid,
                                  uint64_t                 offset,
                                  uint64_t                 len,
                                  time_t                   ts = 0);

static void insert_punch_log_entry(struct mobject_provider* provider,
                                   oid_t                    oid,
                                   uint64_t                 offset,
                                   time_t                   ts = 0);

uint64_t mobject_compute_object_size(struct mobject_provider* provider,
                                     sdskv_provider_handle_t  ph,
                                     sdskv_database_id_t      seg_db_id,
                                     oid_t                    oid,
                                     time_t                   ts);

static struct write_op_visitor write_op_exec
    = {.visit_begin        = write_op_exec_begin,
       .visit_create       = write_op_exec_create,
       .visit_write        = write_op_exec_write,
       .visit_write_full   = write_op_exec_write_full,
       .visit_writesame    = write_op_exec_writesame,
       .visit_append       = write_op_exec_append,
       .visit_remove       = write_op_exec_remove,
       .visit_truncate     = write_op_exec_truncate,
       .visit_zero         = write_op_exec_zero,
       .visit_omap_set     = write_op_exec_omap_set,
       .visit_omap_rm_keys = write_op_exec_omap_rm_keys,
       .visit_end          = write_op_exec_end};

extern "C" void core_write_op(mobject_store_write_op_t write_op,
                              server_visitor_args_t    vargs)
{
    /* Execute the operation chain */
    execute_write_op_visitor(&write_op_exec, write_op, (void*)vargs);
}

void write_op_exec_begin(void* u)
{
    auto                    vargs      = static_cast<server_visitor_args_t>(u);
    sdskv_provider_handle_t sdskv_ph   = vargs->provider->sdskv_ph;
    sdskv_database_id_t     name_db_id = vargs->provider->name_db_id;
    sdskv_database_id_t     oid_db_id  = vargs->provider->oid_db_id;
    oid_t oid  = get_or_create_oid(vargs->provider, sdskv_ph, name_db_id,
                                  oid_db_id, vargs->object_name);
    vargs->oid = oid;
}

void write_op_exec_end(void* u)
{
    auto vargs = static_cast<server_visitor_args_t>(u);
}

void write_op_exec_create(void* u, int exclusive)
{
    auto              vargs = static_cast<server_visitor_args_t>(u);
    oid_t             oid   = vargs->oid;
    margo_instance_id mid   = vargs->provider->mid;
    ENTERING;
    if (oid == 0) {
        margo_error(mid, "[mobject] %s:%d: oid == 0", __func__, __LINE__);
    }
    /* nothing to do, the object is actually created in write_op_exec_begin
       if it did not exist before */
    LEAVING;
}

void write_op_exec_write(void* u, buffer_u buf, size_t len, uint64_t offset)
{
    auto              vargs = static_cast<server_visitor_args_t>(u);
    margo_instance_id mid   = vargs->provider->mid;
    ENTERING;
    oid_t oid = vargs->oid;
    if (oid == 0) {
        margo_error(mid, "[mobject] %s:%d: oid == 0", __func__, __LINE__);
        LEAVING;
        return;
    }

    struct mobject_provider* provider = vargs->provider;
    bake_provider_handle_t   bake_ph  = provider->bake_ph;
    bake_target_id_t         bti      = provider->bake_tid;
    bake_region_id_t         rid;
    hg_bulk_t                remote_bulk     = vargs->bulk_handle;
    const char*              remote_addr_str = vargs->client_addr_str;
    hg_addr_t                remote_addr     = vargs->client_addr;
    double                   wr_start, wr_end;

    int ret;

    ABT_mutex_lock(provider->stats_mutex);
    wr_start = ABT_get_wtime();
    if ((provider->last_wr_start > 0)
        && (provider->last_wr_start >= provider->last_wr_end)) {
        provider->total_seg_wr_duration += (wr_start - provider->last_wr_start);
    }
    provider->last_wr_start = wr_start;
    ABT_mutex_unlock(provider->stats_mutex);

    if (len > SMALL_REGION_THRESHOLD) {
        ret = bake_create(bake_ph, bti, len, &rid);
        if (ret != 0) {
            margo_error(mid, "[mobject] %s:%d: bake_create returned %d",
                        __func__, __LINE__, ret);
            LEAVING;
            return;
        }
        ret = bake_proxy_write(bake_ph, bti, rid, 0, remote_bulk, buf.as_offset,
                               remote_addr_str, len);
        if (ret != 0) {
            margo_error(mid, "[mobject] %s:%d: bake_proxy_write returned %d",
                        __func__, __LINE__, ret);
            LEAVING;
            return;
        }
        ret = bake_persist(bake_ph, bti, rid, 0, len);
        if (ret != 0) {
            margo_error(mid, "[mobject] %s:%d: bake_persist returned %d",
                        __func__, __LINE__, ret);
            LEAVING;
            return;
        }

        insert_region_log_entry(provider, oid, offset, len, &rid);
    } else {
        margo_instance_id mid = vargs->provider->mid;
        char              data[SMALL_REGION_THRESHOLD];
        void*             buf_ptrs[1]  = {(void*)(&data[0])};
        hg_size_t         buf_sizes[1] = {len};
        hg_bulk_t         handle;
        ret = margo_bulk_create(mid, 1, buf_ptrs, buf_sizes, HG_BULK_WRITE_ONLY,
                                &handle);
        if (ret != 0) {
            margo_error(mid, "[mobject] %s:%d: margo_bulk_create returned %d",
                        __func__, __LINE__, ret);
            LEAVING;
            return;
        }
        ret = margo_bulk_transfer(mid, HG_BULK_PULL, remote_addr, remote_bulk,
                                  buf.as_offset, handle, 0, len);
        if (ret != 0) {
            margo_error(mid, "[mobject] %s:%d: margo_bulk_transfer returned %d",
                        __func__, __LINE__, ret);
            margo_bulk_free(handle);
            LEAVING;
            return;
        }
        margo_bulk_free(handle);

        insert_small_region_log_entry(provider, oid, offset, len, data);
    }

    ABT_mutex_lock(provider->stats_mutex);
    wr_end = ABT_get_wtime();
    provider->segs++;
    provider->total_seg_size += len;
    if (provider->last_wr_start > provider->last_wr_end) {
        provider->total_seg_wr_duration += (wr_end - provider->last_wr_start);
    } else {
        provider->total_seg_wr_duration += (wr_end - provider->last_wr_end);
    }
    provider->last_wr_end = wr_end;
    ABT_mutex_unlock(provider->stats_mutex);
    LEAVING;
}

void write_op_exec_write_full(void* u, buffer_u buf, size_t len)
{
    auto              vargs = static_cast<server_visitor_args_t>(u);
    margo_instance_id mid   = vargs->provider->mid;
    ENTERING;
    // truncate to 0 then write
    write_op_exec_truncate(u, 0);
    write_op_exec_write(u, buf, len, 0);
    LEAVING;
}

void write_op_exec_writesame(
    void* u, buffer_u buf, size_t data_len, size_t write_len, uint64_t offset)
{
    auto              vargs = static_cast<server_visitor_args_t>(u);
    margo_instance_id mid   = vargs->provider->mid;
    ENTERING;
    oid_t oid = vargs->oid;
    if (oid == 0) {
        margo_error(mid, "[mobject] %s:%d: oid == 0", __func__, __LINE__);
        LEAVING;
        return;
    }

    hg_bulk_t   remote_bulk     = vargs->bulk_handle;
    const char* remote_addr_str = vargs->client_addr_str;
    hg_addr_t   remote_addr     = vargs->client_addr;
    int         ret;

    if (data_len > SMALL_REGION_THRESHOLD) {

        bake_provider_handle_t bph = vargs->provider->bake_ph;
        bake_target_id_t       bti = vargs->provider->bake_tid;
        bake_region_id_t       rid;

        ret = bake_create(bph, bti, data_len, &rid);
        if (ret != 0) {
            margo_error(mid, "[mobject] %s:%d: bake_create returned %d",
                        __func__, __LINE__, ret);
            LEAVING;
            return;
        }
        ret = bake_proxy_write(bph, bti, rid, 0, remote_bulk, buf.as_offset,
                               remote_addr_str, data_len);
        if (ret != 0) {
            margo_error(mid, "[mobject] %s:%d: bake_proxy_write returned %d",
                        __func__, __LINE__, ret);
            LEAVING;
            return;
        }
        ret = bake_persist(bph, bti, rid, 0, data_len);
        if (ret != 0) {
            margo_error(mid, "[mobject] %s:%d: bake_persist returned %d",
                        __func__, __LINE__, ret);
            LEAVING;
            return;
        }

        size_t i;

        // time_t ts = time(NULL);
        for (i = 0; i < write_len; i += data_len) {
            // TODO normally we should have the same timestamps but right now it
            // bugs...
            insert_region_log_entry(vargs->provider, oid, offset + i,
                                    std::min(data_len, write_len - i),
                                    &rid); //, ts);
        }

    } else {

        margo_instance_id mid = vargs->provider->mid;
        char              data[SMALL_REGION_THRESHOLD];
        void*             buf_ptrs[1]  = {(void*)(&data[0])};
        hg_size_t         buf_sizes[1] = {data_len};
        hg_bulk_t         handle;
        ret = margo_bulk_create(mid, 1, buf_ptrs, buf_sizes, HG_BULK_WRITE_ONLY,
                                &handle);
        if (ret != 0) {
            margo_error(mid, "[mobject] %s:%d: margo_bulk_create returned %d",
                        __func__, __LINE__, ret);
            LEAVING;
            return;
        }
        ret = margo_bulk_transfer(mid, HG_BULK_PULL, remote_addr, remote_bulk,
                                  buf.as_offset, handle, 0, data_len);
        if (ret != 0) {
            margo_error(mid, "[mobject] %s:%d: margo_bulk_transfer returned %d",
                        __func__, __LINE__, ret);
            margo_bulk_free(handle);
            LEAVING;
            return;
        }
        margo_bulk_free(handle);

        size_t i;
        for (i = 0; i < write_len; i += data_len) {
            insert_small_region_log_entry(vargs->provider, oid, offset + i,
                                          std::min(data_len, write_len - i),
                                          data);
        }
    }
    LEAVING;
}

void write_op_exec_append(void* u, buffer_u buf, size_t len)
{
    auto              vargs = static_cast<server_visitor_args_t>(u);
    margo_instance_id mid   = vargs->provider->mid;
    ENTERING;
    oid_t oid = vargs->oid;
    if (oid == 0) {
        margo_error(mid, "[mobject] %s:%d: oid == 0", __func__, __LINE__);
        LEAVING;
        return;
    }

    hg_bulk_t   remote_bulk     = vargs->bulk_handle;
    const char* remote_addr_str = vargs->client_addr_str;
    hg_addr_t   remote_addr     = vargs->client_addr;
    int         ret;

    // find out the current length of the object
    time_t   ts     = time(NULL);
    uint64_t offset = mobject_compute_object_size(
        vargs->provider, vargs->provider->sdskv_ph,
        vargs->provider->segment_db_id, oid, ts);

    if (len > SMALL_REGION_THRESHOLD) {

        bake_provider_handle_t bph = vargs->provider->bake_ph;
        bake_target_id_t       bti = vargs->provider->bake_tid;
        bake_region_id_t       rid;

        ret = bake_create(bph, bti, len, &rid);
        if (ret != 0) {
            margo_error(mid, "[mobject] %s:%d: bake_create returned %d",
                        __func__, __LINE__, ret);
            LEAVING;
            return;
        }
        ret = bake_proxy_write(bph, bti, rid, 0, remote_bulk, buf.as_offset,
                               remote_addr_str, len);
        if (ret != 0) {
            margo_error(mid, "[mobject] %s:%d: bake_proxy_write returned %d",
                        __func__, __LINE__, ret);
            LEAVING;
            return;
        }
        ret = bake_persist(bph, bti, rid, 0, len);
        if (ret != 0) {
            margo_error(mid, "[mobject] %s:%d: bake_persist returned %d",
                        __func__, __LINE__, ret);
            LEAVING;
            return;
        }

        insert_region_log_entry(vargs->provider, oid, offset, len, &rid, ts);

    } else {

        margo_instance_id mid = vargs->provider->mid;
        char              data[SMALL_REGION_THRESHOLD];
        void*             buf_ptrs[1]  = {(void*)(&data[0])};
        hg_size_t         buf_sizes[1] = {len};
        hg_bulk_t         handle;
        ret = margo_bulk_create(mid, 1, buf_ptrs, buf_sizes, HG_BULK_WRITE_ONLY,
                                &handle);
        if (ret != 0) {
            margo_error(mid, "[mobject] %s:%d: margo_bulk_create returned %d",
                        __func__, __LINE__, ret);
            LEAVING;
            return;
        }
        ret = margo_bulk_transfer(mid, HG_BULK_PULL, remote_addr, remote_bulk,
                                  buf.as_offset, handle, 0, len);
        if (ret != 0) {
            margo_error(mid, "[mobject] %s:%d: margo_bulk_transfer returned %d",
                        __func__, __LINE__, ret);
            margo_bulk_free(handle);
            LEAVING;
            return;
        }
        margo_bulk_free(handle);

        insert_small_region_log_entry(vargs->provider, oid, offset, len, data);
    }
    LEAVING;
}

void write_op_exec_remove(void* u)
{
    auto              vargs = static_cast<server_visitor_args_t>(u);
    margo_instance_id mid   = vargs->provider->mid;
    ENTERING;
    const char*             object_name = vargs->object_name;
    oid_t                   oid         = vargs->oid;
    bake_provider_handle_t  bake_ph     = vargs->provider->bake_ph;
    bake_target_id_t        bti         = vargs->provider->bake_tid;
    sdskv_provider_handle_t sdskv_ph    = vargs->provider->sdskv_ph;
    sdskv_database_id_t     name_db_id  = vargs->provider->name_db_id;
    sdskv_database_id_t     oid_db_id   = vargs->provider->oid_db_id;
    sdskv_database_id_t     seg_db_id   = vargs->provider->segment_db_id;
    int                     ret;

    /* remove name->OID entry to make object no longer visible to clients */
    ret = sdskv_erase(sdskv_ph, name_db_id, (const void*)object_name,
                      strlen(object_name) + 1);
    if (ret != SDSKV_SUCCESS) {
        margo_error(mid, "[mobject] %s:%d: sdskv_erase returned %d", __func__,
                    __LINE__, ret);
        LEAVING;
        return;
    }

    /* TODO bg thread for everything beyond this point */

    ret = sdskv_erase(sdskv_ph, oid_db_id, &oid, sizeof(oid));
    if (ret != SDSKV_SUCCESS) {
        margo_error(mid, "[mobject] %s:%d: sdskv_erase returned %d", __func__,
                    __LINE__, ret);
        LEAVING;
        return;
    }

    segment_key_t lb;
    lb.oid       = oid;
    lb.timestamp = time(NULL);
    lb.seq_id    = MOBJECT_SEQ_ID_MAX;

    size_t        max_segments = 128; // XXX this is a pretty arbitrary number
    segment_key_t segment_keys[max_segments];
    void*         segment_keys_addrs[max_segments];
    hg_size_t     segment_keys_size[max_segments];
    bake_region_id_t segment_data[max_segments];
    void*            segment_data_addrs[max_segments];
    hg_size_t        segment_data_size[max_segments];
    for (auto i = 0; i < max_segments; i++) {
        segment_keys_addrs[i] = (void*)(&segment_keys[i]);
        segment_keys_size[i]  = sizeof(segment_key_t);
        segment_data_addrs[i] = (void*)(&segment_data[i]);
        segment_data_size[i]  = sizeof(bake_region_id_t);
    }

    /* iterate over and remove all segments for this oid */
    bool done          = false;
    int  seg_start_ndx = 0;
    while (!done) {
        size_t num_segments = max_segments;

        ret = sdskv_list_keyvals(sdskv_ph, seg_db_id, (const void*)&lb,
                                 sizeof(lb), segment_keys_addrs,
                                 segment_keys_size, segment_data_addrs,
                                 segment_data_size, &num_segments);

        if (ret != SDSKV_SUCCESS) {
            margo_error(mid, "[mobject] %s:%d: sdskv_list_keyvals returned %d",
                        __func__, __LINE__, ret);
            LEAVING;
            return;
        }

        size_t i;
        for (i = seg_start_ndx; i < num_segments; i++) {
            const segment_key_t&    seg    = segment_keys[i];
            const bake_region_id_t& region = segment_data[i];

            if (seg.oid != oid) {
                done = true;
                break;
            }

            if (seg.type == seg_type_t::BAKE_REGION) {
                ret = bake_remove(bake_ph, bti, region);
                if (ret != BAKE_SUCCESS) {
                    margo_error(mid, "[mobject] %s:%d: bake_remove returned %d",
                                __func__, __LINE__, ret);
                    /* XXX should save the error and keep removing */
                    LEAVING;
                    return;
                }
            }
            ret = sdskv_erase(sdskv_ph, seg_db_id, &seg, sizeof(seg));
            if (ret != SDSKV_SUCCESS) {
                margo_error(mid, "[mobject] %s:%d: sdskv_erase returned %d",
                            __func__, __LINE__, ret);
                LEAVING;
                return;
            }
        }
        if (num_segments != max_segments) { done = true; }
        seg_start_ndx = 1;
    }

    LEAVING;
}

void write_op_exec_truncate(void* u, uint64_t offset)
{
    auto              vargs = static_cast<server_visitor_args_t>(u);
    margo_instance_id mid   = vargs->provider->mid;
    ENTERING;
    oid_t oid = vargs->oid;
    if (oid == 0) {
        margo_error(mid, "[mobject] %s:%d: oid == 0", __func__, __LINE__);
        LEAVING;
        return;
    }

    insert_punch_log_entry(vargs->provider, oid, offset);
    LEAVING;
}

void write_op_exec_zero(void* u, uint64_t offset, uint64_t len)
{
    auto              vargs = static_cast<server_visitor_args_t>(u);
    margo_instance_id mid   = vargs->provider->mid;
    ENTERING;
    oid_t oid = vargs->oid;
    if (oid == 0) {
        margo_error(mid, "[mobject] %s:%d: oid == 0", __func__, __LINE__);
        LEAVING;
        return;
    }

    insert_zero_log_entry(vargs->provider, oid, offset, len);
    LEAVING;
}

void write_op_exec_omap_set(void*              u,
                            char const* const* keys,
                            char const* const* vals,
                            const size_t*      lens,
                            size_t             num)
{
    int               ret;
    auto              vargs = static_cast<server_visitor_args_t>(u);
    margo_instance_id mid   = vargs->provider->mid;
    ENTERING;
    sdskv_provider_handle_t sdskv_ph   = vargs->provider->sdskv_ph;
    sdskv_database_id_t     name_db_id = vargs->provider->name_db_id;
    sdskv_database_id_t     oid_db_id  = vargs->provider->oid_db_id;
    sdskv_database_id_t     omap_db_id = vargs->provider->omap_db_id;
    oid_t                   oid        = vargs->oid;
    if (oid == 0) {
        margo_error(mid, "[mobject] %s:%d: oid == 0", __func__, __LINE__);
        LEAVING;
        return;
    }

    /* find out the max key length */
    size_t max_k_len = 0;
    for (auto i = 0; i < num; i++) {
        size_t s  = strlen(keys[i]);
        max_k_len = max_k_len < s ? s : max_k_len;
    }

    /* create an omap key of the right size */
    omap_key_t* k = (omap_key_t*)calloc(1, max_k_len + sizeof(omap_key_t));

    for (auto i = 0; i < num; i++) {
        size_t k_len = strlen(keys[i]) + sizeof(omap_key_t);
        memset(k, 0, max_k_len + sizeof(omap_key_t));
        k->oid = oid;
        strcpy(k->key, keys[i]);
        ret = sdskv_put(sdskv_ph, omap_db_id, (const void*)k, k_len,
                        (const void*)vals[i], lens[i]);
        if (ret != SDSKV_SUCCESS) {
            margo_error(mid, "[mobject] %s:%d: sdskv_put returned %d", __func__,
                        __LINE__, ret);
        }
    }
    free(k);
    LEAVING;
}

void write_op_exec_omap_rm_keys(void*              u,
                                char const* const* keys,
                                size_t             num_keys)
{
    int               ret;
    auto              vargs = static_cast<server_visitor_args_t>(u);
    margo_instance_id mid   = vargs->provider->mid;
    ENTERING;
    sdskv_provider_handle_t sdskv_ph   = vargs->provider->sdskv_ph;
    sdskv_database_id_t     name_db_id = vargs->provider->name_db_id;
    sdskv_database_id_t     oid_db_id  = vargs->provider->oid_db_id;
    sdskv_database_id_t     omap_db_id = vargs->provider->omap_db_id;
    oid_t                   oid        = vargs->oid;
    if (oid == 0) {
        margo_error(mid, "[mobject] %s:%d: oid == 0", __func__, __LINE__);
        LEAVING;
        return;
    }

    for (auto i = 0; i < num_keys; i++) {
        ret = sdskv_erase(sdskv_ph, omap_db_id, (const void*)keys[i],
                          strlen(keys[i]) + 1);
        if (ret != SDSKV_SUCCESS)
            margo_error(mid, "[mobject] %s:%d: sdskv_erase returned %d",
                        __func__, __LINE__, ret);
    }
    LEAVING;
}

static oid_t get_or_create_oid(struct mobject_provider* provider,
                               sdskv_provider_handle_t  ph,
                               sdskv_database_id_t      name_db_id,
                               sdskv_database_id_t      oid_db_id,
                               const char*              object_name)
{
    margo_instance_id mid = provider->mid;
    ENTERING;
    oid_t     oid = 0;
    hg_size_t s;
    int       ret;

    s   = sizeof(oid);
    ret = sdskv_get(ph, name_db_id, (const void*)object_name,
                    strlen(object_name) + 1, &oid, &s);
    if (SDSKV_ERR_UNKNOWN_KEY == ret) {
        std::hash<std::string> hash_fn;
        oid              = hash_fn(std::string(object_name));
        s                = strlen(object_name) + 1;
        char* name_check = (char*)malloc(s);
        if (!name_check) {
            LEAVING;
            return 0;
        }
        while (1) {
            /* avoid hash collisions by checking this oid mapping */
            ret = sdskv_get(ph, oid_db_id, (const void*)&oid, sizeof(oid),
                            (void*)name_check, &s);
            if (ret == SDSKV_SUCCESS) {
                if (strncmp(object_name, name_check, s) == 0) {
                    /* the object has been created by someone else in the
                     * meantime...  */
                    free(name_check);
                    LEAVING;
                    return oid;
                }
                oid++;
                continue;
            }
            break;
        }
        free(name_check);
        // we make sure we stopped at an unknown key (not another SDSKV error)
        if (ret != SDSKV_ERR_UNKNOWN_KEY) {
            margo_error(mid, "[mobject] %s:%d: ret != SDSKV_ERR_UNKONW_KEY",
                        __func__, __LINE__);
            LEAVING;
            return 0;
        }
        // set name => oid
        ret = sdskv_put(ph, name_db_id, (const void*)object_name,
                        strlen(object_name) + 1, &oid, sizeof(oid));
        if (ret != SDSKV_SUCCESS) {
            margo_error(mid, "[mobject] %s:%d: sdskv_put returned %d", __func__,
                        __LINE__, ret);
            LEAVING;
            return 0;
        }
        // set oid => name
        ret = sdskv_put(ph, oid_db_id, &oid, sizeof(oid),
                        (const void*)object_name, strlen(object_name) + 1);
        if (ret != SDSKV_SUCCESS) {
            margo_error(mid, "[mobject] %s:%d: sdskv_put returned %d", __func__,
                        __LINE__, ret);
            LEAVING;
            return 0;
        }
    }
    LEAVING;
    return oid;
}

static void insert_region_log_entry(struct mobject_provider* provider,
                                    oid_t                    oid,
                                    uint64_t                 offset,
                                    uint64_t                 len,
                                    bake_region_id_t*        region,
                                    time_t                   ts)
{
    margo_instance_id mid = provider->mid;
    ENTERING;
    sdskv_provider_handle_t sdskv_ph  = provider->sdskv_ph;
    sdskv_database_id_t     seg_db_id = provider->segment_db_id;
    segment_key_t           seg;

    seg.oid         = oid;
    seg.timestamp   = ts == 0 ? time(NULL) : ts;
    seg.start_index = offset;
    seg.end_index   = offset + len;
    seg.type        = seg_type_t::BAKE_REGION;
    ABT_mutex_lock(provider->mutex);
    seg.seq_id = provider->seq_id++;
    ABT_mutex_unlock(provider->mutex);
    int ret = sdskv_put(sdskv_ph, seg_db_id, (const void*)&seg, sizeof(seg),
                        (const void*)region, sizeof(*region));
    if (ret != SDSKV_SUCCESS) {
        margo_error(mid, "[mobject] %s:%d: sdskv_put returned %d", __func__,
                    __LINE__, ret);
    }
    LEAVING;
}

static void insert_small_region_log_entry(struct mobject_provider* provider,
                                          oid_t                    oid,
                                          uint64_t                 offset,
                                          uint64_t                 len,
                                          const char*              data,
                                          time_t                   ts)
{
    margo_instance_id mid = provider->mid;
    ENTERING;
    sdskv_provider_handle_t sdskv_ph  = provider->sdskv_ph;
    sdskv_database_id_t     seg_db_id = provider->segment_db_id;
    segment_key_t           seg;

    seg.oid         = oid;
    seg.timestamp   = ts == 0 ? time(NULL) : ts;
    seg.start_index = offset;
    seg.end_index   = offset + len;
    seg.type        = seg_type_t::SMALL_REGION;
    ABT_mutex_lock(provider->mutex);
    seg.seq_id = provider->seq_id++;
    ABT_mutex_unlock(provider->mutex);
    int ret = sdskv_put(sdskv_ph, seg_db_id, (const void*)&seg, sizeof(seg),
                        (const void*)data, len);
    if (ret != SDSKV_SUCCESS) {
        margo_error(mid, "[mobject] %s:%d: sdskv_put returned %d", __func__,
                    __LINE__, ret);
    }
    LEAVING;
}

static void insert_zero_log_entry(struct mobject_provider* provider,
                                  oid_t                    oid,
                                  uint64_t                 offset,
                                  uint64_t                 len,
                                  time_t                   ts)
{
    margo_instance_id mid = provider->mid;
    ENTERING;
    sdskv_provider_handle_t sdskv_ph  = provider->sdskv_ph;
    sdskv_database_id_t     seg_db_id = provider->segment_db_id;
    segment_key_t           seg;

    seg.oid         = oid;
    seg.timestamp   = ts == 0 ? time(NULL) : ts;
    seg.start_index = offset;
    seg.end_index   = offset + len;
    seg.type        = seg_type_t::ZERO;
    ABT_mutex_lock(provider->mutex);
    seg.seq_id = provider->seq_id++;
    ABT_mutex_unlock(provider->mutex);
    int ret = sdskv_put(sdskv_ph, seg_db_id, (const void*)&seg, sizeof(seg),
                        (const void*)nullptr, 0);
    if (ret != SDSKV_SUCCESS) {
        margo_error(mid, "[mobject] %s:%d: sdskv_put returned %d", __func__,
                    __LINE__, ret);
    }
    LEAVING;
}

static void insert_punch_log_entry(struct mobject_provider* provider,
                                   oid_t                    oid,
                                   uint64_t                 offset,
                                   time_t                   ts)
{
    margo_instance_id mid = provider->mid;
    ENTERING;
    sdskv_provider_handle_t sdskv_ph  = provider->sdskv_ph;
    sdskv_database_id_t     seg_db_id = provider->segment_db_id;
    segment_key_t           seg;

    seg.oid         = oid;
    seg.timestamp   = ts == 0 ? time(NULL) : ts;
    seg.start_index = offset;
    seg.end_index   = std::numeric_limits<uint64_t>::max();
    seg.type        = seg_type_t::TOMBSTONE;
    ABT_mutex_lock(provider->mutex);
    seg.seq_id = provider->seq_id++;
    ABT_mutex_unlock(provider->mutex);
    int ret = sdskv_put(sdskv_ph, seg_db_id, (const void*)&seg, sizeof(seg),
                        (const void*)nullptr, 0);
    if (ret != SDSKV_SUCCESS) {
        margo_error(mid, "[mobject] %s:%d: sdskv_put returned %d", __func__,
                    __LINE__, ret);
    }
    LEAVING;
}

uint64_t mobject_compute_object_size(struct mobject_provider* provider,
                                     sdskv_provider_handle_t  ph,
                                     sdskv_database_id_t      seg_db_id,
                                     oid_t                    oid,
                                     time_t                   ts)
{
    margo_instance_id mid = provider->mid;
    ENTERING;
    segment_key_t lb;
    lb.oid       = oid;
    lb.timestamp = ts;
    lb.seq_id    = MOBJECT_SEQ_ID_MAX;

    uint64_t size     = 0; // current assumed size
    uint64_t max_size = std::numeric_limits<uint64_t>::max();

    size_t        max_segments = 128;
    segment_key_t segment_keys[max_segments];
    void*         segment_keys_addrs[max_segments];
    hg_size_t     segment_keys_size[max_segments];

    for (auto i = 0; i < max_segments; i++) {
        segment_keys_addrs[i] = (void*)&segment_keys[i];
        segment_keys_size[i]  = sizeof(segment_key_t);
    }

    bool done          = false;
    int  seg_start_ndx = 0;
    while (!done) {

        size_t num_items = max_segments;

        int ret = sdskv_list_keys(ph, seg_db_id, (const void*)&lb, sizeof(lb),
                                  segment_keys_addrs, segment_keys_size,
                                  &num_items);

        if (ret != SDSKV_SUCCESS) {
            margo_error(mid, "[mobject] %s:%d: sdskv_list_keys returned %d",
                        __func__, __LINE__, ret);
            LEAVING;
            return 0;
        }

        size_t i;
        for (i = seg_start_ndx; i < num_items; i++) {
            if (segment_keys[i].oid != oid) {
                done = true;
                break;
            }
            auto& seg = segment_keys[i];
            if (seg.type < seg_type_t::TOMBSTONE) {
                if (size < seg.end_index) {
                    size = std::min(seg.end_index, max_size);
                }
            } else if (seg.type == seg_type_t::TOMBSTONE) {
                if (max_size > seg.start_index) { max_size = seg.start_index; }
                if (size < seg.start_index) { size = seg.start_index; }
                done = true;
                break;
            }
            lb.timestamp = seg.timestamp;
            lb.seq_id    = seg.seq_id;
        }
        if (num_items != max_segments) { done = true; }
        seg_start_ndx = 1;
    }
    LEAVING;
    return size;
}
