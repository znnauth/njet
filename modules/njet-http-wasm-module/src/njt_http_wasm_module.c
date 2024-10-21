#include <njt_config.h>
#include <njt_core.h>
#include <njt_http.h>
#include <njt_stream.h>
#include <njt_json_api.h>
#include <math.h>
#include <njt_http_kv_module.h>
#include <njt_http_util.h>
#include <njt_str_util.h>
#include "njt_http_api_register_module.h"
#include <njt_log.h>

#include "njt_string.h"

#include <sys/types.h>
#include <sys/socket.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>
#include <netdb.h>
#include <netinet/in.h>

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
} njt_http_wasm_loc_conf_t;


static    WasmEdge_VMContext *vm;
static    WasmEdge_MemoryInstanceContext *memory;
static void
njt_http_wasm_read_data(njt_http_request_t *r);

static njt_int_t
njt_http_wasm_handler(njt_http_request_t *r);

static njt_int_t
njt_http_wasm_init_worker(njt_cycle_t *cycle);

static void *
njt_http_wasm_create_loc_conf(njt_conf_t *cf);

static char *njt_http_wasm_merge_loc_conf(njt_conf_t *cf,
                                          void *parent, void *child);

static void *
njt_http_wasm_create_main_conf(njt_conf_t *cf);

static void njt_http_wasm_exit(njt_cycle_t *cycle);

static njt_int_t
njt_http_wasm_init(njt_conf_t *cf);
static njt_int_t
load_wasm_instance(njt_http_wasm_loc_conf_t *conf, const char *path);

static char *
njt_http_wasm_plugin(njt_conf_t *cf, njt_command_t *cmd, void *conf);

uint32_t ptr_offset = 10240;

static njt_command_t njt_http_wasm_commands[] = {
    {njt_string("wasm_plugin"),
     NJT_HTTP_MAIN_CONF | NJT_HTTP_SRV_CONF | NJT_HTTP_LOC_CONF | NJT_CONF_ANY,
     njt_http_wasm_plugin,
     NJT_HTTP_LOC_CONF_OFFSET,
     offsetof(njt_http_wasm_loc_conf_t, wasm_enable),
     NULL},
    {njt_string("wasm_func_name"),
     NJT_HTTP_MAIN_CONF | NJT_HTTP_SRV_CONF | NJT_HTTP_LOC_CONF | NJT_CONF_ANY,
     njt_conf_set_str_slot,
     NJT_HTTP_LOC_CONF_OFFSET,
     offsetof(njt_http_wasm_loc_conf_t, func_name),
     NULL},
    {njt_string("wasm_plugin_path"),
     NJT_HTTP_MAIN_CONF | NJT_HTTP_SRV_CONF | NJT_HTTP_LOC_CONF | NJT_CONF_ANY,
     njt_conf_set_str_slot,
     NJT_HTTP_LOC_CONF_OFFSET,
     offsetof(njt_http_wasm_loc_conf_t, plugin_path),
     NULL},
    njt_null_command};

static njt_http_module_t njt_http_wasm_module_ctx = {
    NULL,
    njt_http_wasm_init,
    njt_http_wasm_create_main_conf,
    NULL,
    NULL,
    NULL,
    njt_http_wasm_create_loc_conf,
    njt_http_wasm_merge_loc_conf};

njt_module_t njt_http_wasm_module = {
    NJT_MODULE_V1,
    &njt_http_wasm_module_ctx,
    njt_http_wasm_commands,
    NJT_HTTP_MODULE,
    NULL,
    NULL,
    njt_http_wasm_init_worker,
    NULL,
    NULL,
    njt_http_wasm_exit,
    NULL,
    NJT_MODULE_V1_PADDING};

static char *
njt_http_wasm_plugin(njt_conf_t *cf, njt_command_t *cmd, void *conf)
{

    njt_http_wasm_loc_conf_t *clcf = conf;
    clcf->wasm_enable = 1;
    
    njt_http_core_loc_conf_t *core_conf;
    core_conf = njt_http_conf_get_module_loc_conf(cf, njt_http_core_module);
    core_conf->handler = njt_http_wasm_handler;

    return NJT_CONF_OK;
}

static njt_int_t
njt_http_wasm_init(njt_conf_t *cf)
{
    return NJT_OK;
}

static void *
njt_http_wasm_create_loc_conf(njt_conf_t *cf)
{
    njt_http_wasm_loc_conf_t *uclcf;
    uclcf = njt_pcalloc(cf->pool, sizeof(njt_http_wasm_loc_conf_t));
    if (uclcf == NULL)
    {
        njt_log_error(NJT_LOG_ERR, cf->log, 0, "malloc uclcf eror");
        return NULL;
    }
    uclcf->wasm_enable = NJT_CONF_UNSET;
    uclcf->func_name.len = 0;
    uclcf->func_name.data = NULL;
    uclcf->plugin_path.len = 0;
    uclcf->plugin_path.data = NULL;
    return uclcf;
}

static void *
njt_http_wasm_create_main_conf(njt_conf_t *cf)
{
    njt_http_wasm_main_conf_t *uclcf;

    uclcf = njt_pcalloc(cf->pool, sizeof(njt_http_wasm_main_conf_t));
    if (uclcf == NULL)
    {
        njt_log_error(NJT_LOG_ERR, cf->log, 0, "malloc njt_http_wasm_main_conf_t eror");
        return NULL;
    }
    uclcf->size = NJT_CONF_UNSET;
    return uclcf;
}

static njt_int_t load_wasm_instance(njt_http_wasm_loc_conf_t *conf, const char *path)
{
    WasmEdge_ConfigureContext *ConfCxt = WasmEdge_ConfigureCreate();
    WasmEdge_ConfigureAddHostRegistration(ConfCxt, WasmEdge_HostRegistration_Wasi);

    vm = WasmEdge_VMCreate(ConfCxt, NULL);
    WasmEdge_VMContext *VMCxt = vm;
    WasmEdge_ConfigureDelete(ConfCxt);

    WasmEdge_ModuleInstanceContext *WasiCxt =
        WasmEdge_VMGetImportModuleContext(VMCxt, WasmEdge_HostRegistration_Wasi);
    WasmEdge_ModuleInstanceInitWASI(WasiCxt, NULL, 0, NULL, 3, NULL, 0);

    WasmEdge_Result Res;

    Res = WasmEdge_VMLoadWasmFromFile(VMCxt, path);
    if (!WasmEdge_ResultOK(Res))
    {
        njt_log_error(NJT_LOG_EMERG, njt_cycle->log, 0, "Load WASM failed. Error message: %s\n",
                      WasmEdge_ResultGetMessage(Res));
        return NJT_ERROR;
    }
    Res = WasmEdge_VMValidate(VMCxt);

    if (!WasmEdge_ResultOK(Res))
    {
        njt_log_error(NJT_LOG_EMERG, njt_cycle->log, 0, "Validate WASM failed. Error message: %s\n",
                      WasmEdge_ResultGetMessage(Res));
        return NJT_ERROR;
    }
    Res = WasmEdge_VMInstantiate(VMCxt);

    if (!WasmEdge_ResultOK(Res))
    {
        njt_log_error(NJT_LOG_EMERG, njt_cycle->log, 0, "Instantiate WASM failed. Error message: %s\n",
                      WasmEdge_ResultGetMessage(Res));
        return NJT_ERROR;
    }

    const WasmEdge_ModuleInstanceContext *mod_inst = WasmEdge_VMGetActiveModule(VMCxt);
    memory = WasmEdge_ModuleInstanceFindMemory(mod_inst, WasmEdge_StringCreateByCString("memory"));
    if (!memory)
    {
        njt_log_error(NJT_LOG_EMERG, njt_cycle->log, 0, "Failed to find memory instance.",
                      WasmEdge_ResultGetMessage(Res));
        return NJT_ERROR;
    }
    return NJT_OK;
}

static char *njt_http_wasm_merge_loc_conf(njt_conf_t *cf,
                                          void *parent, void *child)
{
    njt_http_wasm_loc_conf_t *prev = parent;
    njt_http_wasm_loc_conf_t *conf = child;

    njt_conf_merge_value(conf->wasm_enable, prev->wasm_enable, 0);
    njt_conf_merge_str_value(conf->func_name, prev->func_name, "");
    njt_conf_merge_str_value(conf->plugin_path, prev->plugin_path, "");
    return NJT_CONF_OK;
}

static njt_int_t
njt_http_wasm_handler(njt_http_request_t *r)
{
    njt_int_t rc = NJT_OK;

    njt_http_core_loc_conf_t *loc;
    njt_http_wasm_loc_conf_t *wasm_clcf;

    loc = njt_http_get_module_loc_conf(r, njt_http_core_module);
    wasm_clcf = njt_http_get_module_loc_conf(r, njt_http_wasm_module);
    if (wasm_clcf && wasm_clcf->wasm_enable && loc)
    {
    }
    else
    {
        return NJT_DECLINED;
    }

    njt_log_debug0(NJT_LOG_DEBUG_ALLOC, r->pool->log, 0, "1 read_client_request_body start +++++++++++++++");
    rc = njt_http_read_client_request_body(r, njt_http_wasm_read_data);
    if (rc >= NJT_HTTP_SPECIAL_RESPONSE)
    {
        return rc;
    }

    return NJT_DONE;
}

static njt_int_t
njt_http_wasm_init_worker(njt_cycle_t *cycle)
{

    return NJT_OK;
}

static int njt_http_wasm_request_output(njt_http_request_t *r, njt_int_t code, njt_str_t *msg)
{
    njt_int_t rc;
    njt_buf_t *buf;
    njt_chain_t out;

    if (code == NJT_OK)
    {
        if (msg == NULL || msg->len == 0)
        {
            r->headers_out.status = NJT_HTTP_NO_CONTENT;
        }
        else
        {
            r->headers_out.status = NJT_HTTP_OK;
        }
    }
    else
    {
        r->headers_out.status = code;
    }
    r->headers_out.content_length_n = 0;
    if (msg != NULL && msg->len > 0)
    {
        njt_str_t type = njt_string("text/plain");
        r->headers_out.content_type = type;
        r->headers_out.content_length_n = msg->len;
    }
    if (r->headers_out.content_length)
    {
        r->headers_out.content_length->hash = 0;
        r->headers_out.content_length = NULL;
    }
    rc = njt_http_send_header(r);
    if (rc == NJT_ERROR || rc > NJT_OK || r->header_only || msg == NULL || msg->len < 1)
    {
        return rc;
    }
    buf = njt_create_temp_buf(r->pool, msg->len);
    if (buf == NULL)
    {
        return NJT_ERROR;
    }
    njt_memcpy(buf->pos, msg->data, msg->len);
    buf->last = buf->pos + msg->len;
    buf->last_buf = 1;
    out.buf = buf;
    out.next = NULL;
    return njt_http_output_filter(r, &out);
}

static void add_obj_to_json(char *str, char *key, char *value)
{
    strcat(str, ",\"");
    strcat(str, key);
    strcat(str, "\":\"");
    strcat(str, value);
    strcat(str, "\"");
}

static char *loop_headers(njt_http_request_t *r)
{
    char *res = njt_palloc(r->pool, 1000);
    memset(res, 0, 1000);
    strcat(res, "{\"headers\": { \"proxy-from\": \"wasm-module\"");
    njt_list_part_t *part = &r->headers_in.headers.part;
    njt_table_elt_t *data = part->elts;
    njt_uint_t i = 0;
    for (i = 0 ; /* void */ ; i++) {
        if (i >= part->nelts) {
            if (part->next == NULL) {
                break;
            }

            part = part->next;
            data = part->elts;
            i = 0;
        }
        add_obj_to_json(res, (char *) data[i].key.data, (char *) data[i].value.data);
    }
    strcat(res, "}");
    return res;
}

static void
njt_http_wasm_read_data(njt_http_request_t *r)
{
    njt_int_t rc;

    njt_http_wasm_loc_conf_t *wasm_clcf = njt_http_get_module_loc_conf(r, njt_http_wasm_module);
    if (vm == NULL)
    {
        njt_str_t path = wasm_clcf->plugin_path;
        // check wasm plugin path config
        if(path.len == 0) {
            njt_str_t err = njt_string("no wasm plugin_path config!");
            rc = njt_http_wasm_request_output(r, NJT_HTTP_INTERNAL_SERVER_ERROR, &err);
            njt_http_finalize_request(r, rc);
            return;
        }

        u_char *wasmPath = path.data;
        if (path.data[path.len - 1] != '\0')
        {
            u_char *new_data = njt_palloc(r->pool, path.len + 1);
            if (new_data == NULL)
            {
                return;
            }
            njt_memcpy(new_data, path.data, path.len);
            new_data[path.len] = '\0';
            wasmPath = new_data;
        }
        njt_int_t load_result = load_wasm_instance(wasm_clcf, (const char *)wasmPath);
        
        if(load_result != NJT_OK) {
            njt_str_t err = njt_string("load wasm plugin error!");
            rc = njt_http_wasm_request_output(r, NJT_HTTP_INTERNAL_SERVER_ERROR, &err);
            njt_http_finalize_request(r, rc);
            return;
        }
    }

    njt_str_t response_data = njt_string("wasm world!");
    njt_str_t no_json_body = njt_string("\"no body content\"");
    njt_str_t json_body;
    rc = njt_http_util_read_request_body(r, &json_body, 2, 5242880);
    if (rc != NJT_OK)
    {
        json_body = no_json_body;
    }

    char *request_str = loop_headers(r);

    strcat(request_str, ",\"body\": ");
    strcat(request_str, (char *)(char *)json_body.data);
    strcat(request_str, "}");

    const char *input = request_str;
    size_t input_len = strlen(input);
    WasmEdge_Result Res;

    WasmEdge_MemoryInstanceSetData(memory, (const uint8_t *)input, ptr_offset, input_len);

    WasmEdge_Value Params[2] = {WasmEdge_ValueGenI32(ptr_offset),
                                WasmEdge_ValueGenI32(input_len)};
    ptr_offset += input_len;

    WasmEdge_Value Returns[1] = {};
    WasmEdge_String FuncName = WasmEdge_StringCreateByCString((char *)wasm_clcf->func_name.data);
    Res = WasmEdge_VMExecute(vm, FuncName, Params, 2, Returns, 1);
    if (!WasmEdge_ResultOK(Res))
    {
        njt_str_t err = njt_string("wasm execute error!");
        rc = njt_http_wasm_request_output(r, NJT_HTTP_OK, &err);
        njt_http_finalize_request(r, rc);
        return;
    }

    uint32_t ptr = WasmEdge_ValueGetI32(Returns[0]);

    uint32_t *data = (uint32_t *)WasmEdge_MemoryInstanceGetPointer(memory, ptr, 2 * sizeof(uint32_t));

    uint32_t str_ptr = data[0];
    uint32_t str_len = data[1];

    u_char *c_string = (u_char *)WasmEdge_MemoryInstanceGetPointer(memory, str_ptr, str_len);

    njt_int_t plugin_response_status = 200;
    if (str_len > 3)
    {
        char tmp[4];
        njt_memcpy(tmp, c_string, 3);
        tmp[3] = '\0';
        plugin_response_status = atoi(tmp);
        if (plugin_response_status == 0)
        {
            c_string = (u_char *)"插件返回格式出错";
            str_len = 24;

        }
        else
        {
            c_string = c_string + 3;
            str_len = str_len - 3;
        }
    }
    else
    {
        c_string = (u_char *)"插件返回格式出错";
        str_len = 24;
    }

    njt_str_t tmp_str;
    tmp_str.data = c_string;
    tmp_str.len = str_len;

    response_data = tmp_str;
    rc = njt_http_wasm_request_output(r, plugin_response_status, &response_data);
    njt_http_finalize_request(r, rc);
    return;
}


static void njt_http_wasm_exit(njt_cycle_t *cycle)
{
    WasmEdge_VMDelete(vm);
    WasmEdge_MemoryInstanceDelete(memory);
	return;
}