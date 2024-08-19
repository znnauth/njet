#ifndef NJT_HTTP_HELLO_MODULE_H
#define NJT_HTTP_HELLO_MODULE_H

#include <njt_config.h>
#include <njt_core.h>
#include <njt_http.h>
#include <njt_stream.h>
#include <njt_json_api.h>
#include <math.h>
#include <njt_http_kv_module.h>
#include <wasmedge/wasmedge.h>

typedef struct njt_http_wasm_main_conf_s
{
    njt_http_request_t **reqs;
    njt_int_t size;
} njt_http_wasm_main_conf_t;

typedef struct
{
    njt_http_request_t *req;
    njt_int_t index;
    njt_http_wasm_main_conf_t *dlmcf;
} njt_http_wasm_rpc_ctx_t;

typedef struct njt_http_wasm_loc_conf_s
{
    njt_flag_t wasm_enable;
    njt_str_t func_name;
    njt_str_t plugin_path;
    WasmEdge_VMContext *vm;
    WasmEdge_MemoryInstanceContext *memory;
} njt_http_wasm_loc_conf_t;

#endif
