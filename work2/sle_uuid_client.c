/**
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
 * limitations under the License. *
 * Description: SLE private service register sample of client.
 */
#include "securec.h"
#include <string.h>
#include "test_suite_uart.h"
#include "soc_osal.h"
#include "app_init.h"
#include "sle_device_discovery.h"
#include "sle_connection_manager.h"
#include "sle_ssap_client.h"
#include "sle_uuid_client.h"
#include "iot_gpio.h"
#include "pinctrl.h"

#undef THIS_FILE_ID
#define THIS_FILE_ID BTH_GLE_SAMPLE_UUID_CLIENT

#define SLE_MTU_SIZE_DEFAULT        300
#define SLE_SEEK_INTERVAL_DEFAULT   100
#define SLE_SEEK_WINDOW_DEFAULT     100
#define UUID_16BIT_LEN 2
#define UUID_128BIT_LEN 16
#define SLE_TARGET_SERVICE_UUID 0xABCD
#define SLE_TARGET_DEVICE_NAME "MacroPad"

#define KEY1_GPIO 10  /* Enter */
#define KEY2_GPIO 11  /* Backspace */
#define KEY3_GPIO 12  /* Voice: down=3D, up=3U */

/* Claude Code status LEDs (active high, LED cathode -> GND). */
#define LED_RUNNING_GPIO   0  /* Yellow: Claude Code is running. */
#define LED_DONE_GPIO      1  /* Green: Claude Code is idle/done. */
#define LED_ATTENTION_GPIO 2  /* Red: confirmation/input/error required. */
#define LED_ON  IOT_GPIO_VALUE1
#define LED_OFF IOT_GPIO_VALUE0

#define KEY_TASK_STACK_SIZE 0x1000
#define RECONNECT_TASK_STACK_SIZE 0x800
#define RECONNECT_DELAY_MS 500

sle_announce_seek_callbacks_t g_seek_cbk = {0};
sle_connection_callbacks_t    g_connect_cbk = {0};
ssapc_callbacks_t             g_ssapc_cbk = {0};
sle_addr_t                    g_remote_addr = {0};
uint16_t                      g_conn_id = 0;
ssapc_find_service_result_t   g_find_service_result = {0};
static uint8_t g_sle_ready = 0;
static uint8_t g_target_device_found = 0;
static uint8_t g_target_service_found = 0;

static void reconnect_task(void *arg)
{
    (void)arg;
    osal_msleep(RECONNECT_DELAY_MS);
    test_suite_uart_sendf("[ssap client] reconnect delay done, restart scan\r\n");
    sle_start_scan();
}

static void reconnect_task_start(void)
{
    osal_task *task_handle = osal_kthread_create((osal_kthread_handler)reconnect_task, 0,
        "sle_reconnect", RECONNECT_TASK_STACK_SIZE);
    if (task_handle != NULL) {
        osal_kfree(task_handle);
    }
}

static void status_led_set(uint32_t running, uint32_t done, uint32_t attention)
{
    IoTGpioSetOutputVal(LED_RUNNING_GPIO, running ? LED_ON : LED_OFF);
    IoTGpioSetOutputVal(LED_DONE_GPIO, done ? LED_ON : LED_OFF);
    IoTGpioSetOutputVal(LED_ATTENTION_GPIO, attention ? LED_ON : LED_OFF);
}

static void status_led_init(void)
{
    const pin_t leds[] = {LED_RUNNING_GPIO, LED_DONE_GPIO, LED_ATTENTION_GPIO};

    for (uint32_t i = 0; i < sizeof(leds) / sizeof(leds[0]); i++) {
        IoTGpioInit(leds[i]);
        IoTGpioSetDir(leds[i], IOT_GPIO_DIR_OUT);
        IoTGpioSetOutputVal(leds[i], LED_OFF);
    }
    test_suite_uart_sendf("[led] GPIO0=RUNNING GPIO1=DONE GPIO2=ATTENTION\r\n");
}

static void status_led_handle_command(const uint8_t *data, uint16_t len)
{
    char command[32] = {0};
    uint16_t copy_len;

    if (data == NULL || len == 0) {
        return;
    }
    copy_len = (len < sizeof(command) - 1) ? len : sizeof(command) - 1;
    (void)memcpy_s(command, sizeof(command), data, copy_len);

    if (strcmp(command, "@LED:RUNNING") == 0) {
        status_led_set(1, 0, 0);
    } else if (strcmp(command, "@LED:DONE") == 0) {
        status_led_set(0, 1, 0);
    } else if (strcmp(command, "@LED:ATTENTION") == 0) {
        status_led_set(0, 0, 1);
    } else {
        test_suite_uart_sendf("[led] ignore command:%s\r\n", command);
        return;
    }
    test_suite_uart_sendf("[led] state:%s\r\n", command);
}

static uint8_t sle_uuid16_match(const sle_uuid_t *uuid, uint16_t expected)
{
    if (uuid == NULL || uuid->len != UUID_16BIT_LEN) {
        return 0;
    }
    return (uuid->uuid[14] == (uint8_t)expected &&
        uuid->uuid[15] == (uint8_t)(expected >> 8)) ? 1 : 0;
}

static void sle_client_send_event(const char *event)
{
    if (g_sle_ready == 0) {
        test_suite_uart_sendf("[ssap client] not ready, ignore event:%s\r\n", event);
        return;
    }

    ssapc_write_param_t param = {0};
    param.handle = g_find_service_result.start_hdl;
    param.type = SSAP_PROPERTY_TYPE_VALUE;
    param.data_len = strlen(event);
    param.data = (uint8_t *)event;

    test_suite_uart_sendf("[ssap client] send event: %s\r\n", event);

    ssapc_write_req(0, g_conn_id, &param);
}

typedef struct {
    pin_t gpio;
    const char *down_event;
    const char *up_event;
    uint8_t pressed;
} key_item_t;

static key_item_t g_keys[] = {
    {KEY1_GPIO, "1",  NULL, 0},  /* GPIO10 -> Enter */
    {KEY2_GPIO, "2",  NULL, 0},  /* GPIO11 -> Backspace */
    {KEY3_GPIO, "3D", "3U", 0},  /* GPIO12 -> Voice: down=3D, up=3U */
};

static uint32_t read_key_gpio(uint32_t gpio)
{
    uint32_t value = IOT_GPIO_VALUE0;
    IoTGpioGetInputVal(gpio, &value);
    return value;
}

static void key_task(void *arg)
{
    (void)arg;

    test_suite_uart_sendf("[key] 3-key task start\r\n");

    for (uint32_t i = 0; i < sizeof(g_keys) / sizeof(g_keys[0]); i++) {
        IoTGpioInit(g_keys[i].gpio);
        IoTGpioSetDir(g_keys[i].gpio, IOT_GPIO_DIR_IN);
        uapi_pin_set_pull(g_keys[i].gpio, PIN_PULL_TYPE_DOWN);
        test_suite_uart_sendf("[key] GPIO%d pull-down\r\n", g_keys[i].gpio);
    }

    while (1) {
        for (uint32_t i = 0; i < sizeof(g_keys) / sizeof(g_keys[0]); i++) {
            uint32_t value = read_key_gpio(g_keys[i].gpio);

            if (value == IOT_GPIO_VALUE1 && g_keys[i].pressed == 0) {
                osal_msleep(20);

                if (read_key_gpio(g_keys[i].gpio) == IOT_GPIO_VALUE1) {
                    g_keys[i].pressed = 1;
                    test_suite_uart_sendf("[key] GPIO%d down\r\n", g_keys[i].gpio);
                    sle_client_send_event(g_keys[i].down_event);
                }
            }

            if (value == IOT_GPIO_VALUE0 && g_keys[i].pressed == 1) {
                osal_msleep(20);

                if (read_key_gpio(g_keys[i].gpio) == IOT_GPIO_VALUE0) {
                    g_keys[i].pressed = 0;
                    test_suite_uart_sendf("[key] GPIO%d up\r\n", g_keys[i].gpio);

                    if (g_keys[i].up_event != NULL) {
                        sle_client_send_event(g_keys[i].up_event);
                    }
                }
            }
        }

        osal_msleep(10);
    }
}

static void key_task_start(void)
{
    osal_task *task_handle = NULL;

    task_handle = osal_kthread_create((osal_kthread_handler)key_task, 0,
        "key_task", KEY_TASK_STACK_SIZE);

    if (task_handle != NULL) {
        osal_kfree(task_handle);
    }
}

void sle_sample_sle_enable_cbk(errcode_t status)
{
    test_suite_uart_sendf("[ssap client] sle enable cbk status:%x\r\n", status);

    if (status == 0) {
        test_suite_uart_sendf("[ssap client] start scan\r\n");
        sle_start_scan();
    }
}

void sle_sample_seek_enable_cbk(errcode_t status)
{
    test_suite_uart_sendf("[ssap client] seek enable cbk status:%x\r\n", status);
}

void sle_sample_seek_disable_cbk(errcode_t status)
{
    test_suite_uart_sendf("[ssap client] seek disable cbk status:%x\r\n", status);

    if (status == 0 && g_target_device_found != 0) {
        test_suite_uart_sendf("[ssap client] connect remote device\r\n");
        sle_connect_remote_device(&g_remote_addr);
    }
}

void sle_sample_seek_result_info_cbk(sle_seek_result_info_t *seek_result_data)
{
    test_suite_uart_sendf("[ssap client] seek result cbk\r\n");

    if (seek_result_data != NULL && seek_result_data->data != NULL &&
        strstr((const char *)seek_result_data->data, SLE_TARGET_DEVICE_NAME) != NULL) {
        test_suite_uart_sendf("[ssap client] target device found, stop seek\r\n");

        (void)memcpy_s(&g_remote_addr, sizeof(sle_addr_t),
            &seek_result_data->addr, sizeof(sle_addr_t));

        g_target_device_found = 1;
        sle_stop_seek();
    }
}

void sle_sample_seek_cbk_register(void)
{
    g_seek_cbk.sle_enable_cb = sle_sample_sle_enable_cbk;
    g_seek_cbk.seek_enable_cb = sle_sample_seek_enable_cbk;
    g_seek_cbk.seek_disable_cb = sle_sample_seek_disable_cbk;
    g_seek_cbk.seek_result_cb = sle_sample_seek_result_info_cbk;
}

void sle_sample_connect_state_changed_cbk(uint16_t conn_id, const sle_addr_t *addr,
    sle_acb_state_t conn_state, sle_pair_state_t pair_state, sle_disc_reason_t disc_reason)
{
    errcode_t pair_ret;
    (void)addr;

    test_suite_uart_sendf("[ssap client] connect state changed conn_id:%x, conn_state:%x, pair_state:%x, reason:%x\r\n",
        conn_id, conn_state, pair_state, disc_reason);

    if (conn_state == SLE_ACB_STATE_CONNECTED) {
        g_conn_id = conn_id;
        g_sle_ready = 0;
        g_target_service_found = 0;
        (void)memset_s(&g_find_service_result, sizeof(g_find_service_result), 0,
            sizeof(g_find_service_result));
        pair_ret = sle_pair_remote_device(&g_remote_addr);
        test_suite_uart_sendf("[ssap client] connected, start pair ret:%x\r\n", pair_ret);
    } else if (conn_state == SLE_ACB_STATE_DISCONNECTED) {
        g_sle_ready = 0;
        g_target_device_found = 0;
        g_target_service_found = 0;
        g_conn_id = 0;
        (void)memset_s(&g_find_service_result, sizeof(g_find_service_result), 0,
            sizeof(g_find_service_result));
        test_suite_uart_sendf("[ssap client] disconnected, restart scan after %d ms\r\n",
            RECONNECT_DELAY_MS);
        reconnect_task_start();
    }
}

void sle_sample_pair_complete_cbk(uint16_t conn_id, const sle_addr_t *addr, errcode_t status)
{
    test_suite_uart_sendf("[ssap client] pair complete conn_id:%d status:%x, addr:%02x***%02x%02x\n",
        conn_id, status, addr->addr[0], addr->addr[4], addr->addr[5]); /* 0 4 5: addr index */
    if (status == 0) {
        ssap_exchange_info_t info = {0};
        info.mtu_size = SLE_MTU_SIZE_DEFAULT;
        info.version = 1;
        ssapc_exchange_info_req(1, g_conn_id, &info);
    }
}

void sle_sample_connect_cbk_register(void)
{
    g_connect_cbk.connect_state_changed_cb = sle_sample_connect_state_changed_cbk;
    g_connect_cbk.pair_complete_cb = sle_sample_pair_complete_cbk;
}

void sle_sample_exchange_info_cbk(uint8_t client_id, uint16_t conn_id, ssap_exchange_info_t *param,
    errcode_t status)
{
    test_suite_uart_sendf("[ssap client] pair complete client id:%d status:%d\n", client_id, status);
    if (status != 0 || param == NULL) {
        test_suite_uart_sendf("[ssap client] exchange info failed\r\n");
        return;
    }
    test_suite_uart_sendf("[ssap client] exchange mtu, mtu size: %d, version: %d.\n",
        param->mtu_size, param->version);

    ssapc_find_structure_param_t find_param = {0};
    find_param.type = SSAP_FIND_TYPE_PRIMARY_SERVICE;
    find_param.start_hdl = 1;
    find_param.end_hdl = 0xFFFF;
    ssapc_find_structure(0, conn_id, &find_param);
}

void sle_sample_find_structure_cbk(uint8_t client_id, uint16_t conn_id, ssapc_find_service_result_t *service,
    errcode_t status)
{
    test_suite_uart_sendf("[ssap client] find structure cbk client: %d conn_id:%d status: %d \n",
        client_id, conn_id, status);
    if (status != 0 || service == NULL) {
        return;
    }
    test_suite_uart_sendf("[ssap client] find structure start_hdl:[0x%02x], end_hdl:[0x%02x], uuid len:%d\r\n",
        service->start_hdl, service->end_hdl, service->uuid.len);
    if (service->uuid.len == UUID_16BIT_LEN) {
        test_suite_uart_sendf("[ssap client] structure uuid:[0x%02x][0x%02x]\r\n",
            service->uuid.uuid[14], service->uuid.uuid[15]); /* 14 15: uuid index */
    } else {
        for (uint8_t idx = 0; idx < UUID_128BIT_LEN; idx++) {
            test_suite_uart_sendf("[ssap client] structure uuid[%d]:[0x%02x]\r\n", idx, service->uuid.uuid[idx]);
        }
    }
    if (sle_uuid16_match(&service->uuid, SLE_TARGET_SERVICE_UUID) != 0) {
        g_find_service_result.start_hdl = service->start_hdl;
        g_find_service_result.end_hdl = service->end_hdl;
        (void)memcpy_s(&g_find_service_result.uuid, sizeof(sle_uuid_t), &service->uuid, sizeof(sle_uuid_t));
        g_target_service_found = 1;
        test_suite_uart_sendf("[ssap client] target service found\r\n");
    }
}

void sle_sample_find_structure_cmp_cbk(uint8_t client_id, uint16_t conn_id,
    ssapc_find_structure_result_t *structure_result, errcode_t status)
{
    if (structure_result == NULL) {
        test_suite_uart_sendf("[ssap client] find structure result is NULL\r\n");
        return;
    }
    test_suite_uart_sendf("[ssap client] find structure cmp cbk client id:%d status:%d type:%d uuid len:%d \r\n",
        client_id, status, structure_result->type, structure_result->uuid.len);
    if (structure_result->uuid.len == UUID_16BIT_LEN) {
        test_suite_uart_sendf("[ssap client] find structure cmp cbk structure uuid:[0x%02x][0x%02x]\r\n",
            structure_result->uuid.uuid[14], structure_result->uuid.uuid[15]); /* 14 15: uuid index */
    } else {
        for (uint8_t idx = 0; idx < UUID_128BIT_LEN; idx++) {
            test_suite_uart_sendf("[ssap client] find structure cmp cbk structure uuid[%d]:[0x%02x]\r\n", idx,
                structure_result->uuid.uuid[idx]);
        }
    }
    (void)conn_id;

    if (status == 0 && g_target_service_found != 0) {
        g_sle_ready = 1;
        test_suite_uart_sendf("[ssap client] sle ready, wait key\r\n");
        sle_client_send_event("@LED:SYNC");
    } else {
        g_sle_ready = 0;
        test_suite_uart_sendf("[ssap client] target service not found\r\n");
    }
}

void sle_sample_find_property_cbk(uint8_t client_id, uint16_t conn_id,
    ssapc_find_property_result_t *property, errcode_t status)
{
    test_suite_uart_sendf("[ssap client] find property cbk, client id: %d, conn id: %d, operate ind: %d, "
        "descriptors count: %d status:%d.\n", client_id, conn_id, property->operate_indication,
        property->descriptors_count, status);
    for (uint16_t idx = 0; idx < property->descriptors_count; idx++) {
        test_suite_uart_sendf("[ssap client] find property cbk, descriptors type [%d]: 0x%02x.\n",
            idx, property->descriptors_type[idx]);
    }
    if (property->uuid.len == UUID_16BIT_LEN) {
        test_suite_uart_sendf("[ssap client] find property cbk, uuid: %02x %02x.\n",
            property->uuid.uuid[14], property->uuid.uuid[15]); /* 14 15: uuid index */
    } else if (property->uuid.len == UUID_128BIT_LEN) {
        for (uint16_t idx = 0; idx < UUID_128BIT_LEN; idx++) {
            test_suite_uart_sendf("[ssap client] find property cbk, uuid [%d]: %02x.\n",
                idx, property->uuid.uuid[idx]);
        }
    }
}

void sle_sample_write_cfm_cbk(uint8_t client_id, uint16_t conn_id, ssapc_write_result_t *write_result,
    errcode_t status)
{
    test_suite_uart_sendf("[ssap client] write cfm cbk, client id: %d status:%d.\n", client_id, status);
    ssapc_read_req(0, conn_id, write_result->handle, write_result->type);
}

void sle_sample_read_cfm_cbk(uint8_t client_id, uint16_t conn_id, ssapc_handle_value_t *read_data,
    errcode_t status)
{
    test_suite_uart_sendf("[ssap client] read cfm cbk client id: %d conn id: %d status: %d\n",
        client_id, conn_id, status);
    test_suite_uart_sendf("[ssap client] read cfm cbk handle: %d, type: %d , len: %d\n",
        read_data->handle, read_data->type, read_data->data_len);
    for (uint16_t idx = 0; idx < read_data->data_len; idx++) {
        test_suite_uart_sendf("[ssap client] read cfm cbk[%d] 0x%02x\r\n", idx, read_data->data[idx]);
    }
}

void sle_sample_notification_cbk(uint8_t client_id, uint16_t conn_id, ssapc_handle_value_t *data,
    errcode_t status)
{
    test_suite_uart_sendf("[ssap client] notification client:%d conn:%d status:%d\r\n",
        client_id, conn_id, status);
    if (status == 0 && data != NULL) {
        status_led_handle_command(data->data, data->data_len);
    }
}

void sle_sample_ssapc_cbk_register(void)
{
    g_ssapc_cbk.exchange_info_cb = sle_sample_exchange_info_cbk;
    g_ssapc_cbk.find_structure_cb = sle_sample_find_structure_cbk;
    g_ssapc_cbk.find_structure_cmp_cb = sle_sample_find_structure_cmp_cbk;
    g_ssapc_cbk.ssapc_find_property_cbk = sle_sample_find_property_cbk;
    g_ssapc_cbk.write_cfm_cb = sle_sample_write_cfm_cbk;
    g_ssapc_cbk.read_cfm_cb = sle_sample_read_cfm_cbk;
    g_ssapc_cbk.notification_cb = sle_sample_notification_cbk;
}

void sle_client_init()
{
    uapi_pin_init();
    status_led_init();
    sle_sample_seek_cbk_register();
    sle_sample_connect_cbk_register();
    sle_sample_ssapc_cbk_register();
    sle_announce_seek_register_callbacks(&g_seek_cbk);
    sle_connection_register_callbacks(&g_connect_cbk);
    ssapc_register_callbacks(&g_ssapc_cbk);

    key_task_start();

    enable_sle();
}

void sle_start_scan()
{
    test_suite_uart_sendf("[ssap client] sle_start_scan in\r\n");

    sle_seek_param_t param = {0};
    param.own_addr_type = 0;
    param.filter_duplicates = 0;
    param.seek_filter_policy = 0;
    param.seek_phys = 1;
    param.seek_type[0] = SLE_SEEK_ACTIVE;
    param.seek_interval[0] = SLE_SEEK_INTERVAL_DEFAULT;
    param.seek_window[0] = SLE_SEEK_WINDOW_DEFAULT;

    sle_set_seek_param(&param);

    test_suite_uart_sendf("[ssap client] call sle_start_seek\r\n");
    sle_start_seek();
}

#define SLE_UUID_CLIENT_TASK_PRIO 26
#define SLE_UUID_CLIENT_STACK_SIZE 0x2000

static void sle_uuid_client_entry(void)
{
    osal_task *task_handle = NULL;
    osal_kthread_lock();
    task_handle= osal_kthread_create((osal_kthread_handler)sle_client_init, 0, "sle_gatt_client",
        SLE_UUID_CLIENT_STACK_SIZE);
    if (task_handle != NULL) {
        osal_kthread_set_priority(task_handle, SLE_UUID_CLIENT_TASK_PRIO);
        osal_kfree(task_handle);
    }
    osal_kthread_unlock();
}

/* Run the app entry. */
app_run(sle_uuid_client_entry);