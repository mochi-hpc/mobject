/*
 * (C) 2017 The University of Chicago
 * 
 * See COPYRIGHT in top-level directory.
 */
#ifndef __SERVER_MOBJECT_CONTEXT_H
#define __SERVER_MOBJECT_CONTEXT_H

#include <margo.h>
//#include <sds-keyval.h>
#include <bake-server.h>
#include <bake-client.h>
#include <ssg-mpi.h>

#ifdef __cplusplus
extern "C" {
#endif

struct mobject_server_context
{
    /* margo, bake, sds-keyval, ssg state */
    margo_instance_id mid;
    uint8_t mplex_id;
    ABT_pool pool;
    /* ssg-related data */
    ssg_group_id_t gid;
    /* bake-related data */
    bake_provider_handle_t bake_ph;
    bake_target_id_t bake_tid;
    /* other data */
    int ref_count;
};

#ifdef __cplusplus
}
#endif

#endif
