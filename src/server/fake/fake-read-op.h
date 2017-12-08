#ifndef __FAKE_READ_OP_H
#define __FAKE_READ_OP_H

#include <margo.h>
#include "libmobject-store.h"
#include "src/server/visitor-args.h"

#ifdef __cplusplus
extern "C" {
#endif

void fake_read_op(mobject_store_read_op_t read_op, server_visitor_args_t vargs);

#ifdef __cplusplus
}
#endif
#endif