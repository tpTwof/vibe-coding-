/*
 * Copyright (c) 2020 HiSilicon (Shanghai) Technologies CO., LIMITED.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 * Description: sle uuid server sample.
 */
#include <stddef.h>
#include <string.h>
#include "securec.h"
#include "errcode.h"
#include "osal_addr.h"
#include "soc_osal.h"
#include "app_init.h"
#include "sle_common.h"
#include "test_suite_uart.h"
#include "sle_errcode.h"
#include "sle_ssap_server.h"
#include "sle_connection_manager.h"
#include "sle_device_discovery.h"
#include "sle_server_adv.h"
#include "sle_uuid_server.h"
#include "at.h"

#define OCTET_BIT_LEN 8
#define UUID_LEN_2     2
#define BT_INDEX_4     4
#define BT_INDEX_5     5
#define BT_INDEX_0     0

#define encode2byte_little(_ptr, data) \
    do { \
        *(uint8_t *)((_ptr) + 1) = (uint8_t)((data) >> 8); \
        *(uint8_t *)(_ptr) = (uint8_t)(data); \
    } while (0)

/* sle server app uuid for test */
char g_sle_uuid_app_uuid[UUID_LEN_2] = {0x0, 0x0};
/* server notify property uuid for test */
char g_sle_property_value[OCTET_BIT_LEN] = {0x0, 0x0, 0x0, 0x0, 0x0, 0x0};
/* sle server handle */
uint8_t g_server_id = 0;
/* sle service handle */
uint16_t g_service_handle = 0;
/* sle ntf property handle */
uint16_t g_property_handle = 0;

#define sample_at_log_print(fmt, args...) test_suite_uart_sendf(fmt, ##args)

/* 多连接管理 */
#define MAX_SLE_CONNECTIONS 1
static uint16_t g_conn_handles[MAX_SLE_CONNECTIONS];
static uint8_t g_conn_count = 0;
static uint8_t g_conn_ready[MAX_SLE_CONNECTIONS];
static char g_led_state[32] = "@LED:DONE";

static void sle_uuid_server_send_led_state(uint16_t conn_id)
{
    (void)sle_uuid_server_send_report_by_uuid(conn_id, (const uint8_t *)g_led_state, strlen(g_led_state));
}

static void sle_uuid_server_broadcast_led_state(void)
{
    for (uint8_t i = 0; i < g_conn_count; i++) {
        if (g_conn_ready[i] != 0) {
            sle_uuid_server_send_led_state(g_conn_handles[i]);
        }
    }
}

static void conn_mark_ready(uint16_t conn_id)
{
    for (uint8_t i = 0; i < g_conn_count; i++) {
        if (g_conn_handles[i] == conn_id) {
            g_conn_ready[i] = 1;
            sample_at_log_print("[led] conn:%d ready\r\n", conn_id);
            return;
        }
    }
}

typedef struct {
    uint32_t para_map;
    const uint8_t *state;
} led_at_args_t;

static at_ret_t at_led_set(const led_at_args_t *args)
{
    const char *payload;

    if (args == NULL || args->state == NULL) {
        return AT_RET_SYNTAX_ERROR;
    }

    if (strcmp((const char *)args->state, "RUNNING") == 0) {
        payload = "@LED:RUNNING";
    } else if (strcmp((const char *)args->state, "DONE") == 0) {
        payload = "@LED:DONE";
    } else if (strcmp((const char *)args->state, "ATTENTION") == 0) {
        payload = "@LED:ATTENTION";
    } else {
        return AT_RET_SYNTAX_ERROR;
    }

    if (memcpy_s(g_led_state, sizeof(g_led_state), payload, strlen(payload) + 1) != EOK) {
        return AT_RET_MEM_API_ERROR;
    }
    sample_at_log_print("[led] AT state:%s\r\n", g_led_state);
    sle_uuid_server_broadcast_led_state();
    return AT_RET_OK;
}

static const at_para_parse_syntax_t g_led_at_syntax[] = {
    {
        .type = AT_SYNTAX_TYPE_STRING,
        .last = true,
        .attribute = AT_SYNTAX_ATTR_MAX_LENGTH,
        .offset = offsetof(led_at_args_t, state),
        .entry.string.max_length = 9,
    },
};

static const at_cmd_entry_t g_led_at_cmd[] = {
    {
        .name = "LED",
        .cmd_id = 0x4C45,
        .attribute = 0,
        .syntax = g_led_at_syntax,
        .cmd = NULL,
        .set = (at_set_func_t)at_led_set,
        .read = NULL,
        .test = NULL,
    },
};

static void at_led_command_register(void)
{
    errcode_t ret = uapi_at_cmd_table_register(g_led_at_cmd,
        sizeof(g_led_at_cmd) / sizeof(g_led_at_cmd[0]), sizeof(led_at_args_t));
    sample_at_log_print("[server] AT+LED register ret:%x\r\n", ret);
}

static void conn_add(uint16_t conn_id)
{
    for (uint8_t i = 0; i < g_conn_count; i++) {
        if (g_conn_handles[i] == conn_id) {
            return;
        }
    }
    if (g_conn_count < MAX_SLE_CONNECTIONS) {
        g_conn_handles[g_conn_count] = conn_id;
        g_conn_ready[g_conn_count] = 0;
        g_conn_count++;
        sample_at_log_print("[uuid server] conn added, total:%d\r\n", g_conn_count);
    }
}

static void conn_remove(uint16_t conn_id)
{
    for (uint8_t i = 0; i < g_conn_count; i++) {
        if (g_conn_handles[i] == conn_id) {
            g_conn_count--;
            g_conn_handles[i] = g_conn_handles[g_conn_count];
            g_conn_ready[i] = g_conn_ready[g_conn_count];
            g_conn_ready[g_conn_count] = 0;
            sample_at_log_print("[uuid server] conn removed, total:%d\r\n", g_conn_count);
            return;
        }
    }
}

static uint8_t sle_uuid_base[] = { 0x37, 0xBE, 0xA8, 0x80, 0xFC, 0x70, 0x11, 0xEA, \
    0xB7, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };

static void sle_uuid_set_base(sle_uuid_t *out)
{
    (void)memcpy_s(out->uuid, SLE_UUID_LEN, sle_uuid_base, SLE_UUID_LEN);
    out->len = UUID_LEN_2;
}

static void sle_uuid_setu2(uint16_t u2, sle_uuid_t *out)
{
    sle_uuid_set_base(out);
    out->len = UUID_LEN_2;
    encode2byte_little(&out->uuid[14], u2);
}

static void ssaps_read_request_cbk(uint8_t server_id, uint16_t conn_id, ssaps_req_read_cb_t *read_cb_para,
    errcode_t status)
{
    sample_at_log_print("[uuid server] ssaps read request cbk server_id:%x, conn_id:%x, handle:%x, status:%x\r\n",
        server_id, conn_id, read_cb_para->handle, status);
}

static void ssaps_write_request_cbk(uint8_t server_id, uint16_t conn_id, ssaps_req_write_cb_t *write_cb_para,
    errcode_t status)
{
    (void)server_id;
    (void)status;

    if (write_cb_para == NULL || write_cb_para->value == NULL || write_cb_para->length == 0) {
        return;
    }

    char buf[64] = {0};
    uint16_t copy_len = (write_cb_para->length < sizeof(buf) - 1) ?
        write_cb_para->length : sizeof(buf) - 1;
    (void)memcpy_s(buf, sizeof(buf), write_cb_para->value, copy_len);
    buf[copy_len] = '\0';

    if (strcmp(buf, "@LED:SYNC") == 0) {
        conn_mark_ready(conn_id);
        sample_at_log_print("[led] sync conn:%d state:%s\r\n", conn_id, g_led_state);
        sle_uuid_server_send_led_state(conn_id);
        return;
    }

    /* All regular SLE key events are forwarded to the PC serial port unchanged. */
    sample_at_log_print("%s\r\n", buf);
}

static void ssaps_mtu_changed_cbk(uint8_t server_id, uint16_t conn_id,  ssap_exchange_info_t *mtu_size,
    errcode_t status)
{
    sample_at_log_print("[uuid server] ssaps write request cbk server_id:%x, conn_id:%x, mtu_size:%x, status:%x\r\n",
        server_id, conn_id, mtu_size->mtu_size, status);
}

static void ssaps_start_service_cbk(uint8_t server_id, uint16_t handle, errcode_t status)
{
    sample_at_log_print("[uuid server] start service cbk server_id:%x, handle:%x, status:%x\r\n",
        server_id, handle, status);
}

static void sle_ssaps_register_cbks(void)
{
    ssaps_callbacks_t ssaps_cbk = {0};
    ssaps_cbk.start_service_cb = ssaps_start_service_cbk;
    ssaps_cbk.mtu_changed_cb = ssaps_mtu_changed_cbk;
    ssaps_cbk.read_request_cb = ssaps_read_request_cbk;
    ssaps_cbk.write_request_cb = ssaps_write_request_cbk;
    ssaps_register_callbacks(&ssaps_cbk);
}

static errcode_t sle_uuid_server_service_add(void)
{
    errcode_t ret;
    sle_uuid_t service_uuid = {0};
    sle_uuid_setu2(SLE_UUID_SERVER_SERVICE, &service_uuid);
    ret = ssaps_add_service_sync(g_server_id, &service_uuid, 1, &g_service_handle);
    if (ret != ERRCODE_SLE_SUCCESS) {
        sample_at_log_print("[uuid server] sle uuid add service fail, ret:%x\r\n", ret);
        return ERRCODE_SLE_FAIL;
    }
    return ERRCODE_SLE_SUCCESS;
}

static errcode_t sle_uuid_server_property_add(void)
{
    errcode_t ret;
    ssaps_property_info_t property = {0};
    ssaps_desc_info_t descriptor = {0};
    uint8_t ntf_value[] = {0x01, 0x0};

    property.permissions = SLE_UUID_TEST_PROPERTIES;
    sle_uuid_setu2(SLE_UUID_SERVER_NTF_REPORT, &property.uuid);
    property.value = osal_vmalloc(sizeof(g_sle_property_value));
    if (property.value == NULL) {
        sample_at_log_print("[uuid server] sle property mem fail\r\n");
        return ERRCODE_SLE_FAIL;
    }
    if (memcpy_s(property.value, sizeof(g_sle_property_value), g_sle_property_value,
        sizeof(g_sle_property_value)) != EOK) {
        osal_vfree(property.value);
        sample_at_log_print("[uuid server] sle property mem cpy fail\r\n");
        return ERRCODE_SLE_FAIL;
    }
    ret = ssaps_add_property_sync(g_server_id, g_service_handle, &property,  &g_property_handle);
    if (ret != ERRCODE_SLE_SUCCESS) {
        sample_at_log_print("[uuid server] sle uuid add property fail, ret:%x\r\n", ret);
        osal_vfree(property.value);
        return ERRCODE_SLE_FAIL;
    }
    descriptor.permissions = SLE_UUID_TEST_DESCRIPTOR;
    descriptor.value = osal_vmalloc(sizeof(ntf_value));
    if (descriptor.value == NULL) {
        sample_at_log_print("[uuid server] sle descriptor mem fail\r\n");
        osal_vfree(property.value);
        return ERRCODE_SLE_FAIL;
    }
    if (memcpy_s(descriptor.value, sizeof(ntf_value), ntf_value, sizeof(ntf_value)) != EOK) {
        sample_at_log_print("[uuid server] sle descriptor mem cpy fail\r\n");
        osal_vfree(property.value);
        osal_vfree(descriptor.value);
        return ERRCODE_SLE_FAIL;
    }
    ret = ssaps_add_descriptor_sync(g_server_id, g_service_handle, g_property_handle, &descriptor);
    if (ret != ERRCODE_SLE_SUCCESS) {
        sample_at_log_print("[uuid server] sle uuid add descriptor fail, ret:%x\r\n", ret);
        osal_vfree(property.value);
        osal_vfree(descriptor.value);
        return ERRCODE_SLE_FAIL;
    }
    osal_vfree(property.value);
    osal_vfree(descriptor.value);
    return ERRCODE_SLE_SUCCESS;
}

static errcode_t sle_uuid_server_add(void)
{
    errcode_t ret;
    sle_uuid_t app_uuid = {0};

    sample_at_log_print("[uuid server] sle uuid add service in\r\n");
    app_uuid.len = sizeof(g_sle_uuid_app_uuid);
    if (memcpy_s(app_uuid.uuid, app_uuid.len, g_sle_uuid_app_uuid, sizeof(g_sle_uuid_app_uuid)) != EOK) {
        return ERRCODE_SLE_FAIL;
    }
    ssaps_register_server(&app_uuid, &g_server_id);

    if (sle_uuid_server_service_add() != ERRCODE_SLE_SUCCESS) {
        ssaps_unregister_server(g_server_id);
        return ERRCODE_SLE_FAIL;
    }

    if (sle_uuid_server_property_add() != ERRCODE_SLE_SUCCESS) {
        ssaps_unregister_server(g_server_id);
        return ERRCODE_SLE_FAIL;
    }
    sample_at_log_print("[uuid server] sle uuid add service, server_id:%x, service_handle:%x, property_handle:%x\r\n",
        g_server_id, g_service_handle, g_property_handle);
    ret = ssaps_start_service(g_server_id, g_service_handle);
    if (ret != ERRCODE_SLE_SUCCESS) {
        sample_at_log_print("[uuid server] sle uuid add service fail, ret:%x\r\n", ret);
        return ERRCODE_SLE_FAIL;
    }
    sample_at_log_print("[uuid server] sle uuid add service out\r\n");
    return ERRCODE_SLE_SUCCESS;
}

/* device通过uuid向指定连接发送数据：report */
errcode_t sle_uuid_server_send_report_by_uuid(uint16_t conn_id, const uint8_t *data, uint16_t len)
{
    ssaps_ntf_ind_by_uuid_t param = {0};
    param.type = SSAP_PROPERTY_TYPE_VALUE;
    param.start_handle = g_service_handle;
    param.end_handle = g_property_handle;
    param.value_len = len;
    param.value = osal_vmalloc(len);
    if (param.value == NULL) {
        sample_at_log_print("[uuid server] send report new fail\r\n");
        return ERRCODE_SLE_FAIL;
    }
    if (memcpy_s(param.value, param.value_len, data, len) != EOK) {
        sample_at_log_print("[uuid server] send input report memcpy fail\r\n");
        osal_vfree(param.value);
        return ERRCODE_SLE_FAIL;
    }
    sle_uuid_setu2(SLE_UUID_SERVER_NTF_REPORT, &param.uuid);
    ssaps_notify_indicate_by_uuid(g_server_id, conn_id, &param);
    osal_vfree(param.value);
    return ERRCODE_SLE_SUCCESS;
}

/* device通过handle向指定连接发送数据：report */
errcode_t sle_uuid_server_send_report_by_handle(uint16_t conn_id, const uint8_t *data, uint8_t len)
{
    ssaps_ntf_ind_t param = {0};

    param.handle = g_property_handle;
    param.type = SSAP_PROPERTY_TYPE_VALUE;
    param.value = osal_vmalloc(len);
    param.value_len = len;
    if (param.value == NULL) {
        sample_at_log_print("[uuid server] send report new fail\r\n");
        return ERRCODE_SLE_FAIL;
    }
    if (memcpy_s(param.value, param.value_len, data, len) != EOK) {
        sample_at_log_print("[uuid server] send input report memcpy fail\r\n");
        osal_vfree(param.value);
        return ERRCODE_SLE_FAIL;
    }
    ssaps_notify_indicate(g_server_id, conn_id, &param);
    osal_vfree(param.value);
    return ERRCODE_SLE_SUCCESS;
}

static void sle_connect_state_changed_cbk(uint16_t conn_id, const sle_addr_t *addr,
    sle_acb_state_t conn_state, sle_pair_state_t pair_state, sle_disc_reason_t disc_reason)
{
    sample_at_log_print("[uuid server] connect state changed conn_id:0x%02x, conn_state:0x%x, pair_state:0x%x, \
        disc_reason:0x%x\r\n", conn_id, conn_state, pair_state, disc_reason);
    sample_at_log_print("[uuid server] connect state changed addr:%02x:**:**:**:%02x:%02x\r\n",
        addr->addr[BT_INDEX_0], addr->addr[BT_INDEX_4], addr->addr[BT_INDEX_5]);

    if (conn_state == SLE_ACB_STATE_CONNECTED) {
        conn_add(conn_id);
        sample_at_log_print("[uuid server] connected, total:%d, max:%d\r\n", g_conn_count, MAX_SLE_CONNECTIONS);
    } else if (conn_state == SLE_ACB_STATE_DISCONNECTED) {
        conn_remove(conn_id);
        sample_at_log_print("[uuid server] disconnected, remaining:%d\r\n", g_conn_count);
        sle_uuid_server_adv_restart();
    }
}

static void sle_pair_complete_cbk(uint16_t conn_id, const sle_addr_t *addr, errcode_t status)
{
    sample_at_log_print("[uuid server] pair complete conn_id:%02x, status:%x\r\n",
        conn_id, status);
    sample_at_log_print("[uuid server] pair complete addr:%02x:**:**:**:%02x:%02x\r\n",
        addr->addr[BT_INDEX_0], addr->addr[BT_INDEX_4], addr->addr[BT_INDEX_5]);
    if (status == 0) {
        sample_at_log_print("[uuid server] pair done, waiting for client sync\r\n");
    }
}

static void sle_conn_register_cbks(void)
{
    sle_connection_callbacks_t conn_cbks = {0};
    conn_cbks.connect_state_changed_cb = sle_connect_state_changed_cbk;
    conn_cbks.pair_complete_cb = sle_pair_complete_cbk;
    sle_connection_register_callbacks(&conn_cbks);
}

/* 初始化uuid server */
errcode_t sle_uuid_server_init(void)
{
    enable_sle();
    sle_conn_register_cbks();
    sle_ssaps_register_cbks();
    sle_uuid_server_add();
    sle_uuid_server_adv_init();
    sample_at_log_print("[uuid server] init ok\r\n");
    return ERRCODE_SLE_SUCCESS;
}

#define SLE_SERVER_TASK_STACK_SIZE 0x2000
#define SLE_SERVER_STARTUP_DELAY_MS 1000

static void sle_server_task(void *arg)
{
    (void)arg;
    osal_msleep(SLE_SERVER_STARTUP_DELAY_MS);
    sample_at_log_print("[server] starting SLE...\r\n");
    sle_uuid_server_init();
    at_led_command_register();
}

static void sle_uuid_server_entry(void)
{
    osal_task *task_handle = NULL;

    osal_kthread_lock();
    task_handle = osal_kthread_create((osal_kthread_handler)sle_server_task, 0,
        "sle_server", SLE_SERVER_TASK_STACK_SIZE);
    if (task_handle != NULL) {
        osal_kfree(task_handle);
    }
    osal_kthread_unlock();
}

/* Run the app entry. */
app_run(sle_uuid_server_entry);