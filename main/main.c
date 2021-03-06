/**
 * Copyright (c) 2017, Łukasz Marcin Podkalicki <lpodkalicki@gmail.com>
 * ESP32/016
 * WiFi Sniffer.
 */

#include "freertos/FreeRTOS.h"
#include "esp_wifi.h"
#include "esp_wifi_types.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_event_loop.h"
#include "nvs_flash.h"
#include "driver/gpio.h"

#include "freertos/task.h"
#include "freertos/event_groups.h"
//#include "esp_system.h"
#include "esp_log.h"
//#include "nvs_flash.h"
#include "bt.h"
#include "bta_api.h"
#include "esp_gap_ble_api.h"
#include "esp_gatts_api.h"
#include "esp_bt_defs.h"
#include "esp_bt_main.h"
#include "esp_bt_main.h"
#include "sdkconfig.h"
#pragma GCC diagnostic pop
#pragma GCC diagnostic push
#pragma GCC diagnostic warning "-fpermissive"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define	LED_GPIO_PIN			GPIO_NUM_4
#define	WIFI_CHANNEL_MAX		(13)
#define	WIFI_CHANNEL_SWITCH_INTERVAL	(500)

uint8_t midi_count=0;
uint8_t pre_vel=0;


static wifi_country_t wifi_country = {.cc="CN", .schan=1, .nchan=13, .policy=WIFI_COUNTRY_POLICY_AUTO};

typedef struct {
	unsigned frame_ctrl:16;
	unsigned duration_id:16;
	uint8_t addr1[6]; /* receiver address */
	uint8_t addr2[6]; /* sender address */
	uint8_t addr3[6]; /* filtering address */
	unsigned sequence_ctrl:16;
	uint8_t addr4[6]; /* optional */
} wifi_ieee80211_mac_hdr_t;

typedef struct {
	wifi_ieee80211_mac_hdr_t hdr;
	uint8_t payload[0]; /* network data ended with 4 bytes csum (CRC32) */
} wifi_ieee80211_packet_t;

static esp_err_t event_handler(void *ctx, system_event_t *event);
static void wifi_sniffer_init(void);
static void wifi_sniffer_set_channel(uint8_t channel);
static const char *wifi_sniffer_packet_type2str(wifi_promiscuous_pkt_type_t type);
static void wifi_sniffer_packet_handler(void *buff, wifi_promiscuous_pkt_type_t type);

//BLE midi merge start
/* this function will be invoked to handle incomming events */
static void gatts_profile_event_handler(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if, esp_ble_gatts_cb_param_t *param);

#define TEST_DEVICE_NAME            "MiDiBoyBLE!"

/* maximum value of a characteristic */
#define GATTS_DEMO_CHAR_VAL_LEN_MAX 0xFF

/* value range of a attribute (characteristic) */
uint8_t attr_str[] = {0x00};
esp_attr_value_t gatts_attr_val =
{
    .attr_max_len = GATTS_DEMO_CHAR_VAL_LEN_MAX,
    .attr_len     = sizeof(attr_str),
    .attr_value   = attr_str,
};

/* service uuid */
static uint8_t service_uuid128[32] = {
    0x00, 0xC7, 0xC4, 0x4E, 0xE3, 0x6C, 0x51, 0xA7, 0x33, 0x4B, 0xE8, 0xED, 0x5A, 0x0E, 0xB8, 0x03,
    0xF3, 0x6B, 0x10, 0x9D, 0x66, 0xF2, 0xA9, 0xA1, 0x12, 0x41, 0x68, 0x38, 0xDB, 0xE5, 0x72, 0x77
};

static uint8_t raw_adv_data[] = {
        0x02, 0x01, 0x06,
        0x11, 0x07, 0x00, 0xC7, 0xC4, 0x4E, 0xE3, 0x6C, 0x51, 0xA7, 0x33, 0x4B, 0xE8, 0xED, 0x5A, 0x0E, 0xB8, 0x03,
};
static uint8_t raw_scan_rsp_data[] = {
        0x0c, 0x09, 'M','i','D','i','B','o','y','B','L','E','!'
};

esp_ble_adv_params_t test_adv_params;

#define PROFILE_ON_APP_ID 0
#define CHAR_NUM 1

struct gatts_characteristic_inst{
    esp_bt_uuid_t char_uuid;
    esp_bt_uuid_t descr_uuid;
    uint16_t char_handle;
    esp_gatt_perm_t perm;
    esp_gatt_char_prop_t property;
    uint16_t descr_handle;
};

struct gatts_profile_inst {
    esp_gatts_cb_t gatts_cb;
    uint16_t gatts_if;
    uint16_t app_id;
    uint16_t conn_id;
    uint16_t service_handle;
    esp_gatt_srvc_id_t service_id;
    struct gatts_characteristic_inst chars[CHAR_NUM];
};

/* One gatt-based profile one app_id and one gatts_if, this array will store the gatts_if returned by ESP_GATTS_REG_EVT */
static struct gatts_profile_inst test_profile;

/* this callback will handle process of advertising BLE info */
static void gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT:
        esp_ble_gap_start_advertising(&test_adv_params);
        break;
    case ESP_GAP_BLE_ADV_DATA_RAW_SET_COMPLETE_EVT:
        esp_ble_gap_start_advertising(&test_adv_params);
        break;
    case ESP_GAP_BLE_SCAN_RSP_DATA_RAW_SET_COMPLETE_EVT:
        esp_ble_gap_start_advertising(&test_adv_params);
        break;
    case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
        //advertising start complete event to indicate advertising start successfully or failed
        //if (param->adv_start_cmpl.status != ESP_BT_STATUS_SUCCESS) Serial.println("Advertising start failed\n");
        break;
    case ESP_GAP_BLE_ADV_STOP_COMPLETE_EVT:
        //if (param->adv_stop_cmpl.status != ESP_BT_STATUS_SUCCESS) Serial.println("Advertising stop failed\n");
        //else Serial.println("Stop adv successfully\n");
        break;
    default:
        break;
    }
}

/* this callback handle BLE profile such as registering services and characteristics, send response to central device */
static void gatts_profile_event_handler(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if, esp_ble_gatts_cb_param_t *param) {
    switch (event) {
    /* create service event */
    case ESP_GATTS_REG_EVT:
        printf("REGISTER_APP_EVT, status %d, app_id %d\n", param->reg.status, param->reg.app_id);
        test_profile.service_id.is_primary = true;
        test_profile.service_id.id.inst_id = 0x00;
        test_profile.service_id.id.uuid.len = ESP_UUID_LEN_128;
        for(int i=0; i<16; i++) test_profile.service_id.id.uuid.uuid.uuid128[i] = service_uuid128[i];
        esp_ble_gap_set_device_name(TEST_DEVICE_NAME);
        esp_ble_gap_config_adv_data_raw(raw_adv_data, sizeof(raw_adv_data));
        esp_ble_gap_config_scan_rsp_data_raw(raw_scan_rsp_data, sizeof(raw_scan_rsp_data));
        esp_ble_gatts_create_service(gatts_if, &test_profile.service_id, 4);
        break;
    /* when central device request info from this device, this event will be invoked and respond */
    case ESP_GATTS_READ_EVT: {
        printf("ESP_GATTS_READ_EVT, conn_id %d, trans_id %d, handle %d\n", param->read.conn_id, param->read.trans_id, param->read.handle);
        esp_gatt_rsp_t rsp;
        memset(&rsp, 0, sizeof(esp_gatt_rsp_t));
        rsp.attr_value.handle = param->read.handle;
        rsp.attr_value.len = 1;
        rsp.attr_value.value[0] = 0;
        esp_ble_gatts_send_response(gatts_if, param->read.conn_id, param->read.trans_id, ESP_GATT_OK, &rsp);
        break;
    }
    /* when central device send data to this device, this event will be invoked */
    case ESP_GATTS_WRITE_EVT: {
        printf("ESP_GATTS_WRITE_EVT, conn_id %d, trans_id %d, handle %d\n", param->write.conn_id, param->write.trans_id, param->write.handle);
        printf("value len %d, value %08x\n", param->write.len, *(uint8_t *)param->write.value);
        break;
    }
    /* start service and add characterstic event */
    case ESP_GATTS_CREATE_EVT:
        printf("status %d,  service_handle %d\n", param->create.status, param->create.service_handle);
        test_profile.service_handle = param->create.service_handle; 
        esp_ble_gatts_add_char(test_profile.service_handle, &test_profile.chars[0].char_uuid,
                               ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE,
                               ESP_GATT_CHAR_PROP_BIT_READ | ESP_GATT_CHAR_PROP_BIT_WRITE_NR | ESP_GATT_CHAR_PROP_BIT_NOTIFY,
                               &gatts_attr_val, NULL);
        esp_ble_gatts_start_service(test_profile.service_handle);
        break;
    case ESP_GATTS_ADD_CHAR_EVT: {
        printf("ADD_CHAR_EVT, status %d,  attr_handle %d, service_handle %d (%x)\n",
                param->add_char.status, param->add_char.attr_handle, param->add_char.service_handle, param->add_char.char_uuid.uuid.uuid128[0]);
        if(param->add_char.char_uuid.uuid.uuid128[0] == 0xf3){
            test_profile.chars[0].char_handle = param->add_char.attr_handle;
        }
        break;
    }
    case ESP_GATTS_ADD_CHAR_DESCR_EVT:
        printf("ESP_GATTS_ADD_CHAR_DESCR_EVT, status %d, attr_handle %d, service_handle %d\n",
                 param->add_char.status, param->add_char.attr_handle, param->add_char.service_handle);
        break;
    case ESP_GATTS_DISCONNECT_EVT:
        esp_ble_gap_start_advertising(&test_adv_params);
//        wb32_fillRect(20,120,120,15,0);
        break;
    case ESP_GATTS_CONNECT_EVT: {
        printf("ESP_GATTS_CONNECT_EVT\n");
        esp_ble_conn_update_params_t conn_params = {0};
        memcpy(conn_params.bda, param->connect.remote_bda, sizeof(esp_bd_addr_t));
        /* For the IOS system, please reference the apple official documents about the ble connection parameters restrictions. */
        conn_params.latency = 0;
        conn_params.max_int = 0x30;
        conn_params.min_int = 0x20;
        conn_params.timeout = 500;
        printf("ESP_GATTS_CONNECT_EVT, conn_id %d, remote %02x:%02x:%02x:%02x:%02x:%02x:, \n",
                 param->connect.conn_id,
                 param->connect.remote_bda[0], param->connect.remote_bda[1], param->connect.remote_bda[2],
                 param->connect.remote_bda[3], param->connect.remote_bda[4], param->connect.remote_bda[5]//,
                 //param->connect.is_connected
                 );
        test_profile.conn_id = param->connect.conn_id;
        //start sent the update connection parameters to the peer device.
        esp_ble_gap_update_conn_params(&conn_params);
//        wb32_drawString("Connected!", 20, 120, 1, 2);
        break;
    }
    default:
        break;
    }
}

static void gatts_event_handler(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if, esp_ble_gatts_cb_param_t *param)
{
    /* If event is register event, store the gatts_if for each profile */
    if (event == ESP_GATTS_REG_EVT) {
        if (param->reg.status == ESP_GATT_OK) test_profile.gatts_if = gatts_if;
        else {
            printf("Reg app failed, app_id %04x, status %d\n", param->reg.app_id, param->reg.status);
            return;
        }
    }
    /* here call each profile's callback */
    if (gatts_if == ESP_GATT_IF_NONE || gatts_if == test_profile.gatts_if)
        if (test_profile.gatts_cb) test_profile.gatts_cb(event, gatts_if, param);
}

void setup()
{
//    wb32_init();
//    wb32_setTextColor(wbYELLOW, wbYELLOW);
//    wb32_drawString("MiDiBoy on WiFiBoy", 20, 20, 1, 3);
//    wb32_setTextColor(wbWHITE, wbWHITE);    
//    wb32_drawString("BLE-MIDI Test", 20, 70, 1, 2);
//    wb32_setTextColor(wbGREEN, wbGREEN); 
//    wb32_drawString("(C)2017 WiFiBoy.Org & WiFiBoy.Club", 20, 300, 2, 1);
//    for(int i=0; i<7; i++) wb32_fillRect(i*31+13, 160, 28, 120, 0xffff);
//    for(int i=0; i<2; i++) wb32_fillRect(i*31+31, 160, 23, 75, 0);
//    for(int i=3; i<6; i++) wb32_fillRect(i*31+31, 160, 23, 75, 0);
    //Serial.begin(115200);
    test_adv_params.adv_int_min        = 0x20;
    test_adv_params.adv_int_max        = 0x30;
    test_adv_params.adv_type           = ADV_TYPE_IND;
    test_adv_params.own_addr_type      = BLE_ADDR_TYPE_PUBLIC;
    test_adv_params.channel_map        = ADV_CHNL_ALL;
    test_adv_params.adv_filter_policy  = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY;
    test_profile.gatts_cb = gatts_profile_event_handler;
    test_profile.gatts_if = ESP_GATT_IF_NONE; /* Not get the gatt_if, so initial is ESP_GATT_IF_NONE */
    test_profile.chars[0].char_uuid.len = ESP_UUID_LEN_128;
    for(int i=0; i<16; i++) test_profile.chars[0].char_uuid.uuid.uuid128[i] = service_uuid128[i+16];
    test_profile.chars[0].perm = ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE_ENCRYPTED;
    test_profile.chars[0].property = ESP_GATT_CHAR_PROP_BIT_READ | ESP_GATT_CHAR_PROP_BIT_WRITE_NR | ESP_GATT_CHAR_PROP_BIT_NOTIFY;
    
    //btStart();
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    esp_bt_controller_init(&bt_cfg);
    esp_bt_controller_enable(ESP_BT_MODE_BLE);
    esp_bluedroid_init();
    esp_bluedroid_enable();
    esp_ble_gatts_register_callback(gatts_event_handler);
    esp_ble_gap_register_callback(gap_event_handler);
    esp_ble_gatts_app_register(0);
    
    //pinMode(33,INPUT); pinMode(17,INPUT); pinMode(27,INPUT);
    //pinMode(32,INPUT); pinMode(34,INPUT); pinMode(35,INPUT);
}

int lastkey1=0, lastkey2=0, lastkey3=0, lastkey4=0, lastkey5=0, lastkey6=0;
uint8_t  mididata1[]={0x82, 0x84, 0x90, 0x30, 0x50};
//BLE midi merge end

void
app_main(void)
{
	uint8_t level = 0, channel = 1;

	/* setup */
	wifi_sniffer_init();
	gpio_set_direction(LED_GPIO_PIN, GPIO_MODE_OUTPUT);
	setup();
	vTaskDelay(20000 / portTICK_PERIOD_MS);

	/* loop */
	while (true) {
		gpio_set_level(LED_GPIO_PIN, level ^= 1);
		vTaskDelay(WIFI_CHANNEL_SWITCH_INTERVAL / portTICK_PERIOD_MS);
		wifi_sniffer_set_channel(channel);
		channel = (channel % WIFI_CHANNEL_MAX) + 1;
    	}
}

esp_err_t
event_handler(void *ctx, system_event_t *event)
{
	
	return ESP_OK;
}

void
wifi_sniffer_init(void)
{

	nvs_flash_init();
    	tcpip_adapter_init();
    	ESP_ERROR_CHECK( esp_event_loop_init(event_handler, NULL) );
    	wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
	ESP_ERROR_CHECK( esp_wifi_init(&cfg) );
	ESP_ERROR_CHECK( esp_wifi_set_country(&wifi_country) ); /* set country for channel range [1, 13] */
	ESP_ERROR_CHECK( esp_wifi_set_storage(WIFI_STORAGE_RAM) );
    	ESP_ERROR_CHECK( esp_wifi_set_mode(WIFI_MODE_NULL) );
    	ESP_ERROR_CHECK( esp_wifi_start() );
	esp_wifi_set_promiscuous(true);
	esp_wifi_set_promiscuous_rx_cb(&wifi_sniffer_packet_handler);
}

void
wifi_sniffer_set_channel(uint8_t channel)
{
	
	esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
}

const char *
wifi_sniffer_packet_type2str(wifi_promiscuous_pkt_type_t type)
{
	switch(type) {
	case WIFI_PKT_MGMT: return "MGMT";
	case WIFI_PKT_DATA: return "DATA";
	default:	
	case WIFI_PKT_MISC: return "MISC";
	}
}

void
wifi_sniffer_packet_handler(void* buff, wifi_promiscuous_pkt_type_t type)
{

	if (type != WIFI_PKT_MGMT)
		return;

	const wifi_promiscuous_pkt_t *ppkt = (wifi_promiscuous_pkt_t *)buff;
	const wifi_ieee80211_packet_t *ipkt = (wifi_ieee80211_packet_t *)ppkt->payload;
	const wifi_ieee80211_mac_hdr_t *hdr = &ipkt->hdr;

	printf("PACKET TYPE=%s, CHAN=%02d, RSSI=%02d,"
		" ADDR1=%02x:%02x:%02x:%02x:%02x:%02x,"
		" ADDR2=%02x:%02x:%02x:%02x:%02x:%02x,"
		" ADDR3=%02x:%02x:%02x:%02x:%02x:%02x\n",
		wifi_sniffer_packet_type2str(type),
		ppkt->rx_ctrl.channel,
		ppkt->rx_ctrl.rssi,
		/* ADDR1 */
		hdr->addr1[0],hdr->addr1[1],hdr->addr1[2],
		hdr->addr1[3],hdr->addr1[4],hdr->addr1[5],
		/* ADDR2 */
		hdr->addr2[0],hdr->addr2[1],hdr->addr2[2],
		hdr->addr2[3],hdr->addr2[4],hdr->addr2[5],
		/* ADDR3 */
		hdr->addr3[0],hdr->addr3[1],hdr->addr3[2],
		hdr->addr3[3],hdr->addr3[4],hdr->addr3[5]
		
	);
	if (midi_count == 2){
		mididata1[4]=ppkt->rx_ctrl.rssi;
		if(mididata1[4]==pre_vel)
		{
			mididata1[4]++;
		}
		pre_vel=mididata1[4];
        esp_ble_gatts_send_indicate(test_profile.gatts_if, test_profile.conn_id, 
                test_profile.chars[0].char_handle, sizeof(mididata1), mididata1, false);
	    midi_count = 0;
	}
	midi_count++;
}

