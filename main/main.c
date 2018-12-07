#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/unistd.h>
#include <sys/stat.h>
#include <stdint.h>
#include <stddef.h>
#include <_ansi.h>

#include "driver/gpio.h"
#include "sdkconfig.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"

#include "esp_wifi.h"
#include "esp_wifi_types.h"
#include "esp_event.h"
#include "esp_event_loop.h"
#include "esp_system.h"

#include "esp_log.h"
#include "esp_spiffs.h"
#include "nvs_flash.h"

#include "apps/sntp/sntp.h"

#include "md5.h"
#include "mqtt_client.h"
#include "lwip/sockets.h"
#include "lwip/dns.h"
#include "lwip/netdb.h"

/* LED RED ON: ESP32 turned on
 * LED BLUE FAST BLINK: startup phase
 * LED BLUE ON: ESP32 connected to the wifi, but not to the MQTT broker
 * LED BLUE BLINK: ESP32 connected to the broker */

/* --- LED variables --- */
#define BLINK_GPIO 2 //LED pin definition
#define BLINK_MODE 0
#define ON_MODE 1
#define OFF_MODE 2
#define STARTUP_MODE 3
static int BLINK_TIME_ON = 5; //LED blink time init on
static int BLINK_TIME_OFF = 1000; //LED blink time init off

 /* --- Some configurations --- */
#define SSID_MAX_LEN (32+1) //max length of a SSID
#define MD5_LEN (32+1) //length of md5 hash
#define BUFFSIZE 1024 //size of buffer used to send data to the server
#define NROWS 11 //max rows that buffer can have inside send_data, it can be changed modifying BUFFSIZE
#define MAX_FILES 3 //max number of files in SPIFFS partition

/* TAG of ESP32 for I/O operation */
static const char *TAG = "ETS";
/* Always set as true, when a fatal error occurs in task the variable will be set as false */
static bool RUNNING = true;
/* Only used in startup: if time_init() can't set current time for the first time -> reboot() */
static bool ONCE = true;
/* True if ESP is connected to the wifi, false otherwise */
static bool WIFI_CONNECTED = false;
/* True if ESP is connected to the MQTT broker, false otherwise */
static bool MQTT_CONNECTED = false;
/* If the variable is true the sniffer_task() will write on FILENAME1, otherwise on FILENAME2
 * The value of this variable is changed only by the function send_data() */
static bool WHICH_FILE = false;
 /* True when the wifi-task lock a file (to be send) and set the other file for the sniffer-task*/
static bool FILE_CHANGED = true;
/* Lock used for mutual exclusion for I/O operation in the files */
static _lock_t lck_file;
/* Lock used for MQTT connection to access to the MQTT_CONNECTED variable */
static _lock_t lck_mqtt;

/* Handle for blink task */
static TaskHandle_t xHandle_led = NULL;
/* Handle for sniff task */
static TaskHandle_t xHandle_sniff = NULL;
/* Handle for wifi task */
static TaskHandle_t xHandle_wifi = NULL;
/* Client variable for MQTT connection */
static esp_mqtt_client_handle_t client;
/* FreeRTOS event group to signal when we are connected & ready to make a request */
static EventGroupHandle_t wifi_event_group;

typedef struct {
	int16_t fctl; //frame control
	int16_t duration; //duration id
	uint8_t da[6]; //receiver address
	uint8_t sa[6]; //sender address
	uint8_t bssid[6]; //filtering address
	int16_t seqctl; //sequence control
	unsigned char payload[]; //network data
} __attribute__((packed)) wifi_mgmt_hdr;

static esp_err_t event_handler(void *ctx, system_event_t *event);
static esp_err_t mqtt_event_handler(esp_mqtt_event_handle_t event);

static void vfs_spiffs_init(void);
static void time_init(void);
static void initialize_sntp(void);
static void obtain_time(void);

static void blink_task(void *pvParameter);
static void set_blink_led(int state);

static void sniffer_task(void *pvParameter);
static void wifi_sniffer_init(void);
static void wifi_sniffer_deinit(void);
static void wifi_sniffer_packet_handler(void *buff, wifi_promiscuous_pkt_type_t type);
static void get_hash(unsigned char *data, int len_res, char hash[MD5_LEN]);
static void get_ssid(unsigned char *data, char ssid[SSID_MAX_LEN], uint8_t ssid_len);
static int get_sn(unsigned char *data);
static void get_ht_capabilites_info(unsigned char *data, char htci[5], int pkt_len, int ssid_len);
static void dumb(unsigned char *data, int len);
static void save_pkt_info(uint8_t address[6], char *ssid, time_t timestamp, char *hash, int8_t rssi, int sn, char htci[5]);
static int get_start_timestamp(void);

static void wifi_task(void *pvParameter);
static void wifi_connect_init(void);
static void wifi_connect_deinit(void);
static void mqtt_app_start(void);
static int set_waiting_time(void);
static void send_data(void);
static void file_init(char *filename);

static void reboot(char *msg_err); //called only by main thread

void app_main(void)
{
	ESP_LOGI(TAG, "[+] Startup...");

	ESP_ERROR_CHECK(nvs_flash_init()); //initializing NVS (Non-Volatile Storage)
	ESP_ERROR_CHECK(esp_event_loop_init(event_handler, NULL)); //initialize (wifi) event handler

	ESP_LOGI(TAG, "[!] Starting blink task...");
	xTaskCreate(&blink_task, "blink_task", configMINIMAL_STACK_SIZE, NULL, 5, &xHandle_led);
	if(xHandle_led == NULL)
		reboot("Impossible to create LED task");

	set_blink_led(STARTUP_MODE);

    wifi_connect_init(); //both soft-AP and station

    if(CONFIG_VERBOSE){
    	tcpip_adapter_ip_info_t ip_info;
        uint8_t l_Mac[6];

        esp_wifi_get_mac(ESP_IF_WIFI_STA, l_Mac);
        ESP_LOGI(TAG, "MAC Address: %02x:%02x:%02x:%02x:%02x:%02x", l_Mac[0], l_Mac[1], l_Mac[2], l_Mac[3], l_Mac[4], l_Mac[5]);

    	ESP_ERROR_CHECK(tcpip_adapter_get_ip_info(TCPIP_ADAPTER_IF_STA, &ip_info));
    	ESP_LOGI(TAG, "IP Address:  %s", ip4addr_ntoa(&ip_info.ip));
    	ESP_LOGI(TAG, "Subnet mask: %s", ip4addr_ntoa(&ip_info.netmask));
    	ESP_LOGI(TAG, "Gateway:     %s", ip4addr_ntoa(&ip_info.gw));

    	ESP_LOGI(TAG, "Free memory: %d bytes", esp_get_free_heap_size());
    	ESP_LOGI(TAG, "IDF version: %s", esp_get_idf_version());

    	esp_log_level_set("*", ESP_LOG_INFO);
    	esp_log_level_set("MQTT_CLIENT", ESP_LOG_VERBOSE);
    	esp_log_level_set("TRANSPORT_TCP", ESP_LOG_VERBOSE);
    	esp_log_level_set("TRANSPORT", ESP_LOG_VERBOSE);
    	esp_log_level_set("OUTBOX", ESP_LOG_VERBOSE);

    	//Easter egg
    	printf("[---] Malnati please give us 9 points [---]\n");
    }

	vfs_spiffs_init(); //initializing virtual file system (SPI Flash File System)
	time_init(); //initializing time (current data time)

	_lock_init(&lck_file);
	_lock_init(&lck_mqtt);
	file_init(CONFIG_FILENAME1);
	file_init(CONFIG_FILENAME2);

	ESP_LOGI(TAG, "[!] Starting sniffing task...");
	xTaskCreate(&sniffer_task, "sniffig_task", 10000, NULL, 1, &xHandle_sniff);
	if(xHandle_sniff == NULL)
		reboot("Impossible to create sniffing task");

	ESP_LOGI(TAG, "[!] Starting Wi-Fi task...");
	xTaskCreate(&wifi_task, "wifi_task", 10000, NULL, 1, &xHandle_wifi);
	if(xHandle_wifi == NULL)
		reboot("Impossible to create Wi-Fi task");

	while(RUNNING){ //every 0.5s check if fatal error occurred
		vTaskDelay(500 / portTICK_PERIOD_MS);
	}

	ESP_LOGW(TAG, "Deleting led task...");
	vTaskDelete(xHandle_led);
	ESP_LOGW(TAG, "Deleting sniffing task...");
	vTaskDelete(xHandle_sniff);
	ESP_LOGW(TAG, "Deleting Wi-Fi task...");
	vTaskDelete(xHandle_wifi);

	ESP_LOGW(TAG, "Unmounting SPIFFS");
	esp_vfs_spiffs_unregister(NULL); //SPIFFS unmounted

	ESP_LOGW(TAG, "Stopping sniffing mode...");
	wifi_sniffer_deinit();
	ESP_LOGI(TAG, "Stopped");

	ESP_LOGW(TAG, "Disconnecting from %s...", CONFIG_WIFI_SSID);
	wifi_connect_deinit();

	reboot("Rebooting: Fatal error occurred in a task");
}

static esp_err_t event_handler(void *ctx, system_event_t *event)
{
	switch(event->event_id){
		case SYSTEM_EVENT_STA_START:
	    	ESP_LOGI(TAG, "[WI-FI] Connecting to %s", CONFIG_WIFI_SSID);
	    	ESP_ERROR_CHECK(esp_wifi_connect());
			break;

		case SYSTEM_EVENT_STA_GOT_IP: //wifi connected
			ESP_LOGI(TAG, "[WI-FI] Connected");
			WIFI_CONNECTED = true;
			set_blink_led(ON_MODE);
			xEventGroupSetBits(wifi_event_group, BIT0);
			break;

		case SYSTEM_EVENT_STA_DISCONNECTED: //wifi lost connection
			ESP_LOGI(TAG, "[WI-FI] Disconnected");
			if(WIFI_CONNECTED == false)
				ESP_LOGW(TAG, "[WI-FI] Impossible to connect to wifi: wrong password and/or SSID or Wi-Fi down");
			WIFI_CONNECTED = false;
			set_blink_led(OFF_MODE);
			if(RUNNING){
				ESP_ERROR_CHECK(esp_wifi_connect());
			}
			else
				xEventGroupClearBits(wifi_event_group, BIT0);
			break;

		default:
			break;
	}

	return ESP_OK;
}

static esp_err_t mqtt_event_handler(esp_mqtt_event_handle_t event)
{
    client = event->client;

    //your_context_t *context = event->context;

    switch (event->event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "[MQTT] Connected");

        	_lock_acquire(&lck_mqtt);
            MQTT_CONNECTED = true;
        	_lock_release(&lck_mqtt);

			set_blink_led(BLINK_MODE);
            break;

        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "[MQTT] Disconnected");

            _lock_acquire(&lck_mqtt);
            MQTT_CONNECTED = false;
        	_lock_release(&lck_mqtt);

        	set_blink_led(ON_MODE);
            break;

        case MQTT_EVENT_SUBSCRIBED:
            ESP_LOGI(TAG, "[MQTT] EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
            break;

        case MQTT_EVENT_UNSUBSCRIBED:
            ESP_LOGI(TAG, "[MQTT] EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
            break;

        case MQTT_EVENT_PUBLISHED:
            ESP_LOGI(TAG, "[MQTT] EVENT_PUBLISHED, msg_id=%d", event->msg_id);
            break;

        case MQTT_EVENT_DATA:
            ESP_LOGI(TAG, "[MQTT] EVENT_DATA");
            ESP_LOGI(TAG, "[MQTT] TOPIC=%.*s\r\n", event->topic_len, event->topic);
            ESP_LOGI(TAG, "[MQTT] DATA=%.*s\r\n", event->data_len, event->data);
            break;

        case MQTT_EVENT_ERROR:
            ESP_LOGI(TAG, "[MQTT] MQTT_EVENT_ERROR");
            break;
    }

    return ESP_OK;
}

static void blink_task(void *pvParameter)
{
    gpio_pad_select_gpio(BLINK_GPIO);

    /* Set the GPIO as a push/pull output */
    gpio_set_direction(BLINK_GPIO, GPIO_MODE_OUTPUT);

    while(true){
        /* Blink off (output low) */
        gpio_set_level(BLINK_GPIO, 0);
        vTaskDelay(BLINK_TIME_OFF / portTICK_PERIOD_MS);

        /* Blink on (output high) */
        gpio_set_level(BLINK_GPIO, 1);
        vTaskDelay(BLINK_TIME_ON / portTICK_PERIOD_MS);
    }
}

static void set_blink_led(int state)
{
	switch(state){
		case BLINK_MODE: //blink
			BLINK_TIME_OFF = 1000;
			BLINK_TIME_ON = 1000;
			break;
		case ON_MODE: //always on
			BLINK_TIME_OFF = 5;
			BLINK_TIME_ON = 2000;
			break;
		case OFF_MODE: //always off
			BLINK_TIME_OFF = 2000;
			BLINK_TIME_ON = 5;
			break;
		case STARTUP_MODE: //fast blink
			BLINK_TIME_OFF = 100;
			BLINK_TIME_ON = 100;
			break;
		default:
			break;
	}
}

static void vfs_spiffs_init()
{
    esp_vfs_spiffs_conf_t conf = {
    		.base_path = "/spiffs",
			.partition_label = NULL,
			.max_files = MAX_FILES,
			.format_if_mount_failed = true
    };

    //esp_vfs_spiffs_register() is an all-in-one convenience function
    esp_err_t ret = esp_vfs_spiffs_register(&conf);

    if(ret != ESP_OK){
        if(ret == ESP_FAIL){
            ESP_LOGE(TAG, "Failed to mount or format filesystem");
        }
        else if(ret == ESP_ERR_NOT_FOUND){
            ESP_LOGE(TAG, "Failed to find SPIFFS partition");
        }
        else{
            ESP_LOGE(TAG, "Failed to initialize SPIFFS (%s)", esp_err_to_name(ret));
        }
        reboot("Fatal error SPIFFS");
    }

    size_t total = 0, used = 0;
    ret = esp_spiffs_info(NULL, &total, &used);
    if(ret != ESP_OK){
        ESP_LOGE(TAG, "Failed to get SPIFFS partition information (%s)", esp_err_to_name(ret));
    }
    else{
        ESP_LOGI(TAG, "[SPIFFS] Partition size: total: %d, used: %d", total, used);
    }
}

static void time_init()
{
	time_t now;
    struct tm timeinfo;
    char strftime_buf[64];

	ESP_LOGI(TAG, "Connecting to WiFi and getting time over NTP.");
	obtain_time();
	time(&now);  //update 'now' variable with current time

    //setting timezone to Greenwich
    setenv("TZ", "GMT0BST,M3.5.0/1,M10.5.0", 1);
    tzset();
    localtime_r(&now, &timeinfo);
    strftime(strftime_buf, sizeof(strftime_buf), "%c", &timeinfo);
    ESP_LOGI(TAG, "TIME INFO: The Greenwich date/time is: %s", strftime_buf);
}

static void obtain_time()
{
    time_t now = 0;
    struct tm timeinfo = { 0 };
    int retry = 0;
    const int retry_count = 15;

    initialize_sntp();

    //wait for time to be set
    while(timeinfo.tm_year < (2016 - 1900) && ++retry < retry_count) {
        ESP_LOGI(TAG, "Waiting for system time to be set... (%d/%d)", retry, retry_count);
        vTaskDelay(2000 / portTICK_PERIOD_MS);
        time(&now);
        localtime_r(&now, &timeinfo);
    }

    if(retry >= retry_count){ //can't set time
    	if(ONCE) //if it is first time -> reboot: no reason to sniff with wrong time
    		reboot("No response from server after several time. Impossible to set current time");
    }
    else{
    	ONCE = false;
    }
}

static void initialize_sntp()
{
    sntp_setoperatingmode(SNTP_OPMODE_POLL); //automatically request time after 1h
    sntp_setservername(0, "pool.ntp.org");
    sntp_init();
}

static void file_init(char *filename)
{
	FILE *fp = fopen(filename, "wb");

	if(fp == NULL){
		RUNNING = false;
		ESP_LOGE(TAG, "Error creating or initializing file %s", filename);
		return;
	}

	ESP_LOGI(TAG, "File %s initialized", filename);

	fclose(fp);
}

static void wifi_task(void *pvParameter)
{
	int st = CONFIG_SNIFFING_TIME*1000;

	ESP_LOGI(TAG, "[WIFI] Wi-Fi task created");

	mqtt_app_start();

	while(true){
		st = set_waiting_time(); //wait until the current minute ends
		vTaskDelay(st / portTICK_PERIOD_MS);

		_lock_acquire(&lck_mqtt);
		if(MQTT_CONNECTED)
			send_data();
		else
			ESP_LOGW(TAG, "[WI-FI] Impossible send data to %s. ESP32 is not connected to the broker", CONFIG_BROKER_ADDR);
		_lock_release(&lck_mqtt);
	}
}

static int set_waiting_time()
{
	int st;
	time_t t;

	time(&t);
	st = (CONFIG_SNIFFING_TIME - (int)t % CONFIG_SNIFFING_TIME) * 1000;

	return st;
}

static void wifi_connect_init()
{
	esp_log_level_set("wifi", ESP_LOG_NONE); //disable the default wifi logging

	tcpip_adapter_init();
	wifi_event_group = xEventGroupCreate(); //create the event group to handle wifi events

	wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
	ESP_ERROR_CHECK(esp_wifi_init(&cfg));
	ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
	ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA)); //create soft-AP and station control block

	wifi_config_t wifi_config = {
		.sta = {
			.ssid = CONFIG_WIFI_SSID,
			.password = CONFIG_WIFI_PSW,
			//.bssid_set = false,
		},
	};
	ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));
	ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Waiting for connection to the WiFi network...");
    xEventGroupWaitBits(wifi_event_group, BIT0, false, true, portMAX_DELAY);
}

static void wifi_connect_deinit()
{
	ESP_ERROR_CHECK(esp_wifi_disconnect()); //disconnect the ESP32 WiFi station from the AP
	ESP_ERROR_CHECK(esp_wifi_stop()); //it stop station and free station control block
	ESP_ERROR_CHECK(esp_wifi_deinit()); //free all resource allocated in esp_wifi_init and stop WiFi task
}

static void mqtt_app_start()
{
	//MQTT client will reconnect automatically to the server after 10s (when disconnect/error occurs)
    const esp_mqtt_client_config_t mqtt_cfg = {
    	.uri = CONFIG_BROKER_ADDR,
		.port = CONFIG_BROKER_PORT,
		.transport = MQTT_TRANSPORT_OVER_TCP,
        //.username = "",
        //.transport = MQTT_TRANSPORT_OVER_WS,
		.client_id = CONFIG_ESP32_ID,
		.keepalive = 120,
		.buffer_size = BUFFSIZE,
        .event_handle = mqtt_event_handler,
        //.user_context = (void *)your_context
    };

    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_start(client);

    ESP_LOGI(TAG, "[MQTT] Connecting to %s:%d", CONFIG_BROKER_ADDR, CONFIG_BROKER_PORT);
}

static void send_data()
{
	FILE *fp = NULL;
	int msg_id, tid;
	int sending = true, reading = true, tot_read = 0, n = 1;
	char *topic, buffer[BUFFSIZE], last_pkt = 'F';
	ssize_t len = strlen(CONFIG_ETS)+strlen(CONFIG_ROOM)+strlen(CONFIG_ESP32_ID)+1;

	_lock_acquire(&lck_file);
	if(WHICH_FILE){
		WHICH_FILE = false;
		FILE_CHANGED = true;
		fp = fopen(CONFIG_FILENAME1, "r");
		if(fp == NULL){
			RUNNING = false;
			ESP_LOGE(TAG, "[WI-FI] Impossible to open file %s and read information", CONFIG_FILENAME1);
			return;
		}
	}
	else{
		WHICH_FILE = true;
		FILE_CHANGED = true;
		fp = fopen(CONFIG_FILENAME2, "r");
		if(fp == NULL){
			RUNNING = false;
			ESP_LOGE(TAG, "[WI-FI] Impossible to open file %s and read information", CONFIG_FILENAME2);
			return;
		}
	}
	_lock_release(&lck_file);

	topic = malloc(len*sizeof(char));
	memset(topic, '\0', len);
	strcpy(topic, CONFIG_ETS);
	strcat(topic, "/");
	strcat(topic, CONFIG_ROOM);
	strcat(topic, "/");
	strcat(topic, CONFIG_ESP32_ID);

	fscanf(fp, "%d", &tid);

	ESP_LOGI(TAG, "[WI-FI] Sending information about sniffed packets to %s:%d", CONFIG_BROKER_ADDR, CONFIG_BROKER_PORT);
	while(sending){
		n = 1;
		tot_read = 0;
		memset(buffer, '\0', BUFFSIZE);

		sprintf(buffer, "%c %d\n", last_pkt, tid); //sending also information about if it is last packet or not
		tot_read = strlen(buffer);

		reading = true;
		while(fgets(buffer+tot_read, BUFFSIZE, fp) != NULL && reading){
			n++;
			tot_read = strlen(buffer);
			if(n >= NROWS)
				reading = false; //stop reading because buffer is full
		}

		if(reading){ //finished to read file
			last_pkt = 'T'; //set as TRUE last packet -> next send is the last
			buffer[0] = last_pkt;
			sending = false;
		}
		//else -> i need at least one more cycle to finish to read the file

		msg_id = esp_mqtt_client_publish(client, topic, buffer, strlen(buffer), 0, 0);
		ESP_LOGI(TAG, "[WI-FI] Sent publish successful on topic=%s, msg_id=%d", topic, msg_id);
	}

	_lock_acquire(&lck_file);
	if(WHICH_FILE){
		fclose(fp);
		file_init(CONFIG_FILENAME2);
	}
	else{
		fclose(fp);
		file_init(CONFIG_FILENAME1);
	}
	_lock_release(&lck_file);

	free(topic);
}

static void sniffer_task(void *pvParameter)
{
	int sleep_time = CONFIG_SNIFFING_TIME*1000;

	ESP_LOGI(TAG, "[SNIFFER] Sniffer task created");

	ESP_LOGI(TAG, "[SNIFFER] Starting sniffing mode...");
	wifi_sniffer_init();
	ESP_LOGI(TAG, "[SNIFFER] Started. Sniffing on channel %d", CONFIG_CHANNEL);

	while(true){
		vTaskDelay(sleep_time / portTICK_PERIOD_MS);
    	_lock_acquire(&lck_mqtt);
		/* if ESP is not connected to the broker
		 * -> need to reset file after 1 minutes, because wifi-task will not ever send it and initialize it */
		if(!MQTT_CONNECTED){
			ESP_LOGW(TAG, "[SNIFFER] Initializing file...");
			_lock_acquire(&lck_file);
			if(WHICH_FILE)
				file_init(CONFIG_FILENAME1);
			else
				file_init(CONFIG_FILENAME2);
			_lock_release(&lck_file);
		}
    	_lock_release(&lck_mqtt);
	}
}

static void wifi_sniffer_init()
{
	tcpip_adapter_init();

	wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
	ESP_ERROR_CHECK(esp_wifi_init(&cfg)); //allocate resource for WiFi driver

	const wifi_country_t wifi_country = {
			.cc = "CN",
			.schan = 1,
			.nchan = 13,
			.policy = WIFI_COUNTRY_POLICY_AUTO
	};
	ESP_ERROR_CHECK(esp_wifi_set_country(&wifi_country)); //set country for channel range [1, 13]
	ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
	ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_NULL));
	ESP_ERROR_CHECK(esp_wifi_start());

	const wifi_promiscuous_filter_t filt = {
			.filter_mask = WIFI_EVENT_MASK_AP_PROBEREQRECVED
	};
	ESP_ERROR_CHECK(esp_wifi_set_promiscuous_filter(&filt)); //set filter mask
	ESP_ERROR_CHECK(esp_wifi_set_promiscuous_rx_cb(wifi_sniffer_packet_handler)); //callback function
	ESP_ERROR_CHECK(esp_wifi_set_promiscuous(true)); //set 'true' the promiscuous mode

	esp_wifi_set_channel(CONFIG_CHANNEL, WIFI_SECOND_CHAN_NONE); //only set the primary channel
}

static void wifi_sniffer_deinit()
{
	ESP_ERROR_CHECK(esp_wifi_set_promiscuous(false)); //set as 'false' the promiscuous mode
	ESP_ERROR_CHECK(esp_wifi_stop()); //it stop soft-AP and free soft-AP control block
	ESP_ERROR_CHECK(esp_wifi_deinit()); //free all resource allocated in esp_wifi_init() and stop WiFi task
}

static void wifi_sniffer_packet_handler(void* buff, wifi_promiscuous_pkt_type_t type)
{
	int pkt_len, fc, sn=0;
	char ssid[SSID_MAX_LEN] = "\0", hash[MD5_LEN] = "\0", htci[5] = "\0";
	uint8_t ssid_len;
	time_t ts;

	wifi_promiscuous_pkt_t *pkt = (wifi_promiscuous_pkt_t *)buff;
	wifi_mgmt_hdr *mgmt = (wifi_mgmt_hdr *)pkt->payload;

	fc = ntohs(mgmt->fctl);

	if((fc & 0xFF00) == 0x4000){ //only look for probe request packets
		time(&ts);

		ssid_len = pkt->payload[25];
		if(ssid_len > 0)
			get_ssid(pkt->payload, ssid, ssid_len);

		pkt_len = pkt->rx_ctrl.sig_len;
		get_hash(pkt->payload, pkt_len-4, hash);

		if(CONFIG_VERBOSE){
			ESP_LOGI(TAG, "Dump");
			dumb(pkt->payload, pkt_len);
		}

		sn = get_sn(pkt->payload);

		get_ht_capabilites_info(pkt->payload, htci, pkt_len, ssid_len);

		ESP_LOGI(TAG, "ADDR=%02x:%02x:%02x:%02x:%02x:%02x, "
				"SSID=%s, "
				"TIMESTAMP=%d, "
				"HASH=%s, "
				"RSSI=%02d, "
				"SN=%d, "
				"HT CAP. INFO=%s",
				mgmt->sa[0], mgmt->sa[1], mgmt->sa[2], mgmt->sa[3], mgmt->sa[4], mgmt->sa[5],
				ssid,
				(int)ts,
				hash,
				pkt->rx_ctrl.rssi,
				sn,
				htci);

		save_pkt_info(mgmt->sa, ssid, ts, hash, pkt->rx_ctrl.rssi, sn, htci);
	}
}

static void get_hash(unsigned char *data, int len_res, char hash[MD5_LEN])
{
	uint8_t pkt_hash[16];

	md5((uint8_t *)data, len_res, pkt_hash);

	sprintf(hash, "%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x",
			pkt_hash[0], pkt_hash[1], pkt_hash[2], pkt_hash[3], pkt_hash[4], pkt_hash[5],
			pkt_hash[6], pkt_hash[7], pkt_hash[8], pkt_hash[9], pkt_hash[10], pkt_hash[11],
			pkt_hash[12], pkt_hash[13], pkt_hash[14], pkt_hash[15]);
}

static void get_ssid(unsigned char *data, char ssid[SSID_MAX_LEN], uint8_t ssid_len)
{
	int i, j;

	for(i=26, j=0; i<26+ssid_len; i++, j++){
		ssid[j] = data[i];
	}

	ssid[j] = '\0';
}

static int get_sn(unsigned char *data)
{
	int sn;
    char num[5] = "\0";

	sprintf(num, "%02x%02x", data[22], data[23]);
    sscanf(num, "%x", &sn);

    return sn;
}

static void get_ht_capabilites_info(unsigned char *data, char htci[5], int pkt_len, int ssid_len)
{
	int ht_start = 25+ssid_len+19;

	/* 1) data[ht_start-1] is the byte that says if HT Capabilities is present or not (tag length).
	 * 2) I need to check also that i'm not outside the payload: if HT Capabilities is not present in the packet,
	 * for this reason i'm considering the ht_start must be lower than the total length of the packet less the last 4 bytes of FCS */

	if(data[ht_start-1]>0 && ht_start<pkt_len-4){ //HT capabilities is present
		if(data[ht_start-4] == 1) //DSSS parameter is set -> need to shift of three bytes
			sprintf(htci, "%02x%02x", data[ht_start+3], data[ht_start+1+3]);
		else
			sprintf(htci, "%02x%02x", data[ht_start], data[ht_start+1]);
	}
}

static void dumb(unsigned char *data, int len)
{
	unsigned char i, j, byte;

	for(i=0; i<len; i++){
		byte = data[i];
		printf("%02x ", data[i]);

		if(((i%16)==15) || (i==len-1)){
			for(j=0; j<15-(i%16); j++)
				printf(" ");
			printf("| ");
			for(j=(i-(i%16)); j<=i; j++){
				byte = data[j];
				if((byte>31) && (byte<127))
					printf("%c", byte);
				else
					printf(".");
			}
			printf("\n");
		}
	}
}

static void save_pkt_info(uint8_t address[6], char *ssid, time_t timestamp, char *hash, int8_t rssi, int sn, char htci[5])
{
	FILE *fp = NULL;
	int stime;

	_lock_acquire(&lck_file);
	if(WHICH_FILE)
		fp = fopen(CONFIG_FILENAME1, "a");
	else
		fp = fopen(CONFIG_FILENAME2, "a");
	_lock_release(&lck_file);

	if(fp == NULL){
		ESP_LOGE(TAG, "[SNIFFER] Impossible to open file and save information about sniffed packets");
	}

	if(FILE_CHANGED){
		FILE_CHANGED = false;
		stime = get_start_timestamp();
		fprintf(fp, "%d\n", stime);
	}

	fprintf(fp, "%02x:%02x:%02x:%02x:%02x:%02x %s %d %s %02d %d %s\n",
			address[0], address[1], address[2], address[3], address[4], address[5],
			ssid,
			(int)timestamp,
			hash,
			rssi,
			sn,
			htci);

	fclose(fp);
}

static int get_start_timestamp()
{
	int stime;
	time_t clk;

	time(&clk);
	stime = (int)clk - (int)clk % CONFIG_SNIFFING_TIME;

	return stime;
}

static void reboot(char *msg_err)
{
	int i;

	ESP_LOGE(TAG, "%s", msg_err);
    for(i=3; i>=0; i--){
        ESP_LOGW(TAG, "Restarting in %d seconds...", i);
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }

    ESP_LOGW(TAG, "Restarting now");
    fflush(stdout);

    esp_restart();
}
