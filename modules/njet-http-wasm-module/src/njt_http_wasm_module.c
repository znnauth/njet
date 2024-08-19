#include <njt_config.h>
#include <njt_core.h>
#include <njt_http.h>
#include <njt_stream.h>
#include <njt_json_api.h>
#include <math.h>
#include <njt_http_kv_module.h>
#include <njt_http_wasm_module.h>
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

static njt_int_t
njt_http_wasm_init(njt_conf_t *cf);
static void
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
    {njt_string("wasm_runtime"),
     NJT_HTTP_MAIN_CONF | NJT_HTTP_SRV_CONF | NJT_HTTP_LOC_CONF | NJT_CONF_ANY,
     njt_conf_set_str_slot,
     NJT_HTTP_LOC_CONF_OFFSET,
     offsetof(njt_http_wasm_loc_conf_t, runtime),
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
    NULL,
    NULL,
    NJT_MODULE_V1_PADDING};

static char *
njt_http_wasm_plugin(njt_conf_t *cf, njt_command_t *cmd, void *conf)
{

    njt_http_wasm_loc_conf_t *clcf = conf;
    clcf->wasm_enable = 1;
    return NJT_CONF_OK;
}

static njt_int_t
njt_http_wasm_init(njt_conf_t *cf)
{
    njt_http_core_main_conf_t *cmcf;
    njt_http_handler_pt *h;
    njt_http_wasm_main_conf_t *dlmcf;

    dlmcf = njt_http_conf_get_module_main_conf(cf, njt_http_wasm_module);
    if (dlmcf->size == NJT_CONF_UNSET)
    {
        dlmcf->size = 500;
    }
    dlmcf->reqs = njt_pcalloc(cf->pool, sizeof(njt_http_request_t *) * dlmcf->size);
    if (dlmcf->reqs == NULL)
    {
        njt_log_error(NJT_LOG_EMERG, njt_cycle->log, 0, "njt_http_wasm_postconfiguration alloc mem error");
        return NJT_ERROR;
    }

    cmcf = njt_http_conf_get_module_main_conf(cf, njt_http_core_module);
    if (cmcf == NULL)
    {
        return NJT_ERROR;
    }
    h = njt_array_push(&cmcf->phases[NJT_HTTP_CONTENT_PHASE].handlers);
    if (h == NULL)
    {
        return NJT_ERROR;
    }

    *h = njt_http_wasm_handler;
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
    uclcf->runtime.len = 0;
    uclcf->runtime.data = NULL;
    uclcf->plugin_path.len = 0;
    uclcf->plugin_path.data = NULL;
    uclcf->memory = NULL;
    uclcf->vm = NULL;
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

static void load_wasm_instance(njt_http_wasm_loc_conf_t *conf, const char *path)
{
    WasmEdge_ConfigureContext *ConfCxt = WasmEdge_ConfigureCreate();
    WasmEdge_ConfigureAddHostRegistration(ConfCxt, WasmEdge_HostRegistration_Wasi);

    conf->vm = WasmEdge_VMCreate(ConfCxt, NULL);
    WasmEdge_VMContext *VMCxt = conf->vm;
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
        return;
    }
    Res = WasmEdge_VMValidate(VMCxt);

    if (!WasmEdge_ResultOK(Res))
    {
        njt_log_error(NJT_LOG_EMERG, njt_cycle->log, 0, "Validate WASM failed. Error message: %s\n",
                      WasmEdge_ResultGetMessage(Res));
        return;
    }
    Res = WasmEdge_VMInstantiate(VMCxt);

    if (!WasmEdge_ResultOK(Res))
    {
        njt_log_error(NJT_LOG_EMERG, njt_cycle->log, 0, "Instantiate WASM failed. Error message: %s\n",
                      WasmEdge_ResultGetMessage(Res));
        return;
    }

    const WasmEdge_ModuleInstanceContext *mod_inst = WasmEdge_VMGetActiveModule(VMCxt);
    WasmEdge_MemoryInstanceContext *memory = WasmEdge_ModuleInstanceFindMemory(mod_inst, WasmEdge_StringCreateByCString("memory"));
    if (!memory)
    {
        njt_log_error(NJT_LOG_EMERG, njt_cycle->log, 0, "Failed to find memory instance.",
                      WasmEdge_ResultGetMessage(Res));
        return;
    }
    conf->memory = memory;
    return;
}

static char *njt_http_wasm_merge_loc_conf(njt_conf_t *cf,
                                          void *parent, void *child)
{
    njt_http_wasm_loc_conf_t *prev = parent;
    njt_http_wasm_loc_conf_t *conf = child;

    njt_conf_merge_value(conf->wasm_enable, prev->wasm_enable, 0);
    njt_conf_merge_str_value(conf->runtime, prev->runtime, "");
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
        /* error */
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
    strcat(res, "{\"headers\": ");
    njt_str_t host = r->headers_in.host->value;
    strcat(res, "{\"host\":\"");

    strcat(res, (char *)(char *)host.data);
    strcat(res, "\"");
    if (r->headers_in.connection)
    {
        njt_str_t connection = r->headers_in.connection->value;
        add_obj_to_json(res, "connection", (char *)(char *)connection.data);
    }

    if (r->headers_in.authorization)
    {
        njt_str_t authorization = r->headers_in.authorization->value;
        add_obj_to_json(res, "authorization", (char *)(char *)authorization.data);
    }
    strcat(res, "}");
    return res;
}

static void
njt_http_wasm_read_data(njt_http_request_t *r)
{
    njt_http_wasm_loc_conf_t *wasm_clcf = njt_http_get_module_loc_conf(r, njt_http_wasm_module);
    if (wasm_clcf->vm == NULL)
    {
        njt_str_t path = wasm_clcf->plugin_path;
        u_char *wasmPath = path.data;
        if (path.data[path.len] != '\0')
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
        load_wasm_instance(wasm_clcf, (const char *)wasmPath);
    }

    njt_str_t response_data = njt_string("wasm world!");
    njt_str_t no_json_body = njt_string("no body content");
    njt_str_t json_body;
    njt_int_t rc = njt_http_util_read_request_body(r, &json_body, 2, 5242880);
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

    WasmEdge_MemoryInstanceContext *memory = wasm_clcf->memory;
    WasmEdge_VMContext *vm = wasm_clcf->vm;

    WasmEdge_MemoryInstanceSetData(memory, (const uint8_t *)input, ptr_offset, input_len);

    WasmEdge_Value Params[2] = {WasmEdge_ValueGenI32(ptr_offset),
                                WasmEdge_ValueGenI32(input_len)};

    WasmEdge_Value Returns[1] = {};
    WasmEdge_String FuncName = WasmEdge_StringCreateByCString((char *)wasm_clcf->runtime.data);
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

    njt_str_t tmp_str;
    tmp_str.data = c_string;
    tmp_str.len = str_len;

    response_data = tmp_str;
    rc = njt_http_wasm_request_output(r, NJT_HTTP_OK, &response_data);
    njt_http_finalize_request(r, rc);
    return;
}