/* CoAP client Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

/*
 * WARNING
 * libcoap is not multi-thread safe, so only this thread must make any coap_*()
 * calls.  Any external (to this thread) data transmitted in/out via libcoap
 * therefore has to be passed in/out by xQueue*() via this thread.
 */

#include <string.h>
#include <sys/socket.h>
#include <sys/param.h>
#include <netdb.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "esp_event.h"

#include "coap.h"

/* Note: ESP32 don't support temperature sensor */
#include "driver/temp_sensor.h"

#define ESP_WIFI_SSID      		""		// Please insert your SSID
#define ESP_WIFI_PASS      		""		// Please insert your password
#define ESP_WIFI_AUTH_MODE		WIFI_AUTH_WPA2_PSK // See esp_wifi_types.h
#define ESP_WIFI_MAX_RETRY 		5U

#define THETHINGSIO_TOKEN_ID 	""		// Please insert your TOKEN ID
#define THETHINGSIO_COAP_HOST	"coap://coap.thethings.io"
#define THETHINGSIO_COAP_PATH 	"v2/things/" THETHINGSIO_TOKEN_ID

#define TEMP_SENSOR_TASK_DELAY	1000U	// In milliseconds
#define COAP_POST_DELAY			300000U	// In milliseconds
#define COAP_DEFAULT_TIME 		5000U	// In milliseconds

#define COAP_BUFFER_SIZE 		((uint8_t)64)

/* FreeRTOS event group to signal when we are connected*/
static EventGroupHandle_t s_wifi_event_group;

/* The event group allows multiple bits for each event, but we only care about two events:
 * - we are connected to the AP with an IP
 * - we failed to connect after the maximum amount of retries */
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static ip_event_got_ip_t* event = NULL;
static uint8_t u8RetryCounter = 0U;

const static char *pcTAG = "TTIO_COAP_CLIENT";

static float fTemperature = 0.0f;

static bool resp_wait = true;
static coap_optlist_t *optlist = NULL;
static int wait_ms = 0U;

static void WIFI_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{

	if (event_base == WIFI_EVENT)
	{
		switch (event_id)
		{
			case WIFI_EVENT_STA_START:
				esp_wifi_connect();
				break;
			case WIFI_EVENT_STA_DISCONNECTED:
				if (u8RetryCounter < ESP_WIFI_MAX_RETRY)
				{
					esp_wifi_connect();
					u8RetryCounter++;
					ESP_LOGI(pcTAG, "Retry to connect to the access point");
				}
				else
				{
					xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
					ESP_LOGI(pcTAG,"Connect to the access point fail");
				}
				break;
			default:
				// Do nothing (see WiFi event declarations in the esp_wifi_types.h)
				break;
		}
	}
	else if (event_base == IP_EVENT)
	{
		switch (event_id)
		{
			case IP_EVENT_STA_GOT_IP:
				event = (ip_event_got_ip_t*) event_data;
				u8RetryCounter = 0U;
				xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
				ESP_LOGI(pcTAG, "Got IP:" IPSTR, IP2STR(&event->ip_info.ip));
				break;
			default:
				// Do nothing (see WiFi event declarations in the esp_netif_types.h)
				break;
		}
	}

}

static void CoAP_event_handler(coap_context_t *psCtx, coap_session_t *psSession, coap_pdu_t *psPduSent, coap_pdu_t *psPduReceived,
                            const coap_tid_t id)
{

    unsigned char *pcBuffer = NULL, acBuffer[4];
    size_t BufferLen;
    coap_pdu_t *psPdu = NULL;
    coap_opt_t *sBlockOpt;
    coap_opt_iterator_t sOptIter;
    coap_optlist_t *sOption;
    coap_tid_t sTid;

    if (COAP_RESPONSE_CLASS(psPduReceived->code) == 2)
    {
        /* Need to see if blocked response */
        sBlockOpt = coap_check_option(psPduReceived, COAP_OPTION_BLOCK2, &sOptIter);

        if (sBlockOpt)
        {
            uint16_t blktype = sOptIter.type;

            if (coap_opt_block_num(sBlockOpt) == 0)
            {
                printf("Received:\n");
            }

            if (coap_get_data(psPduReceived, &BufferLen, &pcBuffer))
            {
                printf("%.*s", (int)BufferLen, pcBuffer);
            }

            if (COAP_OPT_BLOCK_MORE(sBlockOpt))
            {
                /* more bit is set */

                /* create pdu with request for next block */
                psPdu = coap_new_pdu(psSession);

                if (!psPdu)
                {
                    ESP_LOGE(pcTAG, "coap_new_pdu() failed");
                    goto clean_up;
                }
                psPdu->type = COAP_MESSAGE_CON;
                psPdu->tid = coap_new_message_id(psSession);
                psPdu->code = COAP_REQUEST_GET;

                /* add URI components from optlist */
                for (sOption = optlist; sOption; sOption = sOption->next )
                {
                    switch (sOption->number)
                    {
						case COAP_OPTION_URI_HOST :
						case COAP_OPTION_URI_PORT :
						case COAP_OPTION_URI_PATH :
						case COAP_OPTION_URI_QUERY :
							coap_add_option(psPdu, sOption->number, sOption->length,
											sOption->data);
							break;
						default:
							;     /* skip other options */
                    }
                }

                /* finally add updated block option from response, clear M bit */
                /* blocknr = (blocknr & 0xfffffff7) + 0x10; */
                coap_add_option(psPdu, blktype, coap_encode_var_safe(acBuffer, sizeof(acBuffer),
                                                     	 	 	 	 ((coap_opt_block_num(sBlockOpt) + 1) << 4) |
																	 COAP_OPT_BLOCK_SZX(sBlockOpt)), acBuffer);

                sTid = coap_send(psSession, psPdu);

                if (sTid != COAP_INVALID_TID)
                {
                    resp_wait = true;
                    wait_ms = COAP_DEFAULT_TIME;
                    return;
                }
            }
            printf("\n");
        }
        else
        {
            if (coap_get_data(psPduReceived, &BufferLen, &pcBuffer))
            {
                printf("Received: %.*s\n", (int)BufferLen, pcBuffer);
            }
        }
    }

clean_up:

    resp_wait = false;

}

void wifi_init_sta(void)
{

	wifi_init_config_t sWifiInitCfg = WIFI_INIT_CONFIG_DEFAULT();
	esp_event_handler_instance_t instance_any_id;
	esp_event_handler_instance_t instance_got_ip;

	EventBits_t WifiEventBits = 0U;

    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());

    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    ESP_ERROR_CHECK(esp_wifi_init(&sWifiInitCfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &WIFI_event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &WIFI_event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    wifi_config_t sWifiConfig = {
        .sta = {
            .ssid = ESP_WIFI_SSID,
            .password = ESP_WIFI_PASS,
            /* Setting a password implies station will connect to all security modes including WEP/WPA.
             * However these modes are deprecated and not advisable to be used. Incase your Access point
             * doesn't support WPA2, these mode can be enabled by commenting below line */
	     .threshold.authmode = ESP_WIFI_AUTH_MODE,

            .pmf_cfg = {
                .capable = true,
                .required = false
            },
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &sWifiConfig));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(pcTAG, "Wi-Fi initializated");

    /* Waiting until either the connection is established (WIFI_CONNECTED_BIT) or connection failed for the maximum
     * number of re-tries (WIFI_FAIL_BIT). The bits are set by wifi_event_handler() (see above) */
    WifiEventBits = xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE, portMAX_DELAY);

    /* xEventGroupWaitBits() returns the bits before the call returned, hence we can test which event actually
     * happened. */
    if (WifiEventBits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(pcTAG, "Connected to access point SSID: %s, Password: %s",
                 ESP_WIFI_SSID, ESP_WIFI_PASS);
    } else if (WifiEventBits & WIFI_FAIL_BIT) {
        ESP_LOGI(pcTAG, "Failed to connect to SSID: %s, Password: %s",
                 ESP_WIFI_SSID, ESP_WIFI_PASS);
    } else {
        ESP_LOGE(pcTAG, "UNEXPECTED EVENT");
    }

    /* The event will not be processed after unregister */
    ESP_ERROR_CHECK(esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, instance_got_ip));
    ESP_ERROR_CHECK(esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, instance_any_id));

    vEventGroupDelete(s_wifi_event_group);

}

void TempSensor_Task(void *pvParams)
{

	temp_sensor_config_t sTemperatureInitCfg = TSENS_CONFIG_DEFAULT();

    // Initialize touch pad peripheral, it will start a timer to run a filter
    ESP_LOGI(pcTAG, "Initializing Temperature sensor");

    temp_sensor_get_config(&sTemperatureInitCfg);
    ESP_LOGI(pcTAG, "Default offset: %d, clk_div: %d", sTemperatureInitCfg.dac_offset, sTemperatureInitCfg.clk_div);
    temp_sensor_set_config(sTemperatureInitCfg);
    temp_sensor_start();
    ESP_LOGI(pcTAG, "Temperature sensor started");

    vTaskDelay(1000U / portTICK_RATE_MS);

    while (1)
	{
        temp_sensor_read_celsius(&fTemperature);
        ESP_LOGI(pcTAG, "Temperature out Celsius: %2.2f", fTemperature);
        vTaskDelay(TEMP_SENSOR_TASK_DELAY / portTICK_RATE_MS);
    }

    ESP_LOGI(pcTAG, "Finish temperature sensor");
    vTaskDelete(NULL);

}

static void CoAP_Task(void *p)
{

	const char acThingUri[] = THETHINGSIO_COAP_HOST "/v2/things/" THETHINGSIO_TOKEN_ID;
	const uint8_t acThingData[] = "{\"values\":[{\"key\":\"Temperature\",\"value\":\"25.00\"}]}";

    struct hostent *psHostent;
    coap_address_t  dst_addr;
    static coap_uri_t sUri;
    char *pcHostName = NULL, acTemporalBuffer[INET6_ADDRSTRLEN];
    unsigned char *pcBuffer, acBuffer[COAP_BUFFER_SIZE];
	size_t BufferLen;
	int res = 0U;
	coap_context_t *psCtx = NULL;
	coap_session_t *psSession = NULL;
	coap_pdu_t *psRequest = NULL;

	coap_set_log_level(LOG_DEBUG);

    vTaskDelay(1100U / portTICK_PERIOD_MS);

    while (1)
    {
        optlist = NULL;

        if (coap_split_uri((const uint8_t *)acThingUri, strlen(acThingUri), &sUri) == -1)
        {
            ESP_LOGE(pcTAG, "CoAP server uri error");
            break;
        }

        pcHostName = (char *)calloc(1, sUri.host.length + 1);


        if (pcHostName == NULL)
        {
            ESP_LOGE(pcTAG, "calloc failed");
            break;
        }

        memcpy(pcHostName, sUri.host.s, sUri.host.length);
        psHostent = gethostbyname(pcHostName);
        free(pcHostName);

        if (psHostent == NULL)
        {
            ESP_LOGE(pcTAG, "DNS lookup failed");
            vTaskDelay(1000 / portTICK_PERIOD_MS);
            free(pcHostName);
            continue;
        }

        coap_address_init(&dst_addr);

        switch (psHostent->h_addrtype)
        {
            case AF_INET:
                dst_addr.addr.sin.sin_family      = AF_INET;
                dst_addr.addr.sin.sin_port        = htons(sUri.port);
                memcpy(&dst_addr.addr.sin.sin_addr, psHostent->h_addr, sizeof(dst_addr.addr.sin.sin_addr));
                inet_ntop(AF_INET, &dst_addr.addr.sin.sin_addr, acTemporalBuffer, sizeof(acTemporalBuffer));
                ESP_LOGI(pcTAG, "DNS lookup succeeded. IP=%s", acTemporalBuffer);
                break;
            case AF_INET6:
                dst_addr.addr.sin6.sin6_family      = AF_INET6;
                dst_addr.addr.sin6.sin6_port        = htons(sUri.port);
                memcpy(&dst_addr.addr.sin6.sin6_addr, psHostent->h_addr, sizeof(dst_addr.addr.sin6.sin6_addr));
                inet_ntop(AF_INET6, &dst_addr.addr.sin6.sin6_addr, acTemporalBuffer, sizeof(acTemporalBuffer));
                ESP_LOGI(pcTAG, "DNS lookup succeeded. IP=%s", acTemporalBuffer);
                break;
            default:
                ESP_LOGE(pcTAG, "DNS lookup response failed");
                goto clean_up;
        }

        if (sUri.path.length)
        {
            BufferLen = COAP_BUFFER_SIZE;
            pcBuffer = acBuffer;

            res = coap_split_path(sUri.path.s, sUri.path.length, pcBuffer, &BufferLen);

            while (res--)
            {
                coap_insert_optlist(&optlist, coap_new_optlist(COAP_OPTION_URI_PATH, coap_opt_length(pcBuffer), coap_opt_value(pcBuffer)));

                pcBuffer += coap_opt_size(pcBuffer);
            }
        }

        if (sUri.query.length)
        {
            BufferLen = COAP_BUFFER_SIZE;
            pcBuffer = acBuffer;

            res = coap_split_query(sUri.query.s, sUri.query.length, pcBuffer, &BufferLen);

            while (res--)
            {
                coap_insert_optlist(&optlist, coap_new_optlist(COAP_OPTION_URI_QUERY, coap_opt_length(pcBuffer), coap_opt_value(pcBuffer)));

                pcBuffer += coap_opt_size(pcBuffer);
            }
        }

        psCtx = coap_new_context(NULL);

        if (!psCtx)
        {
            ESP_LOGE(pcTAG, "coap_new_context() failed");
            goto clean_up;
        }

        psSession = coap_new_client_session(psCtx, NULL, &dst_addr, COAP_PROTO_UDP);

        if (!psSession)
        {
            ESP_LOGE(pcTAG, "coap_new_client_session() failed");
            goto clean_up;
        }

        coap_register_response_handler(psCtx, CoAP_event_handler);

        psRequest = coap_new_pdu(psSession);

        if (!psRequest)
        {
            ESP_LOGE(pcTAG, "coap_new_pdu() failed");
            goto clean_up;
        }
        psRequest->type = COAP_MESSAGE_CON;
        psRequest->tid = coap_new_message_id(psSession);
        psRequest->code = COAP_REQUEST_POST;
        coap_add_optlist_pdu(psRequest, &optlist);

        sprintf((char *)acThingData, "{\"values\":[{\"key\":\"Temperature\",\"value\":\"%2.2f\"}]}", fTemperature);
        ESP_LOGE(pcTAG, "Message sent: %s", acThingData);
        coap_add_data(psRequest, sizeof(acThingData) - 1, acThingData);

        coap_send(psSession, psRequest);

        resp_wait = false;
        wait_ms = COAP_DEFAULT_TIME;

        while (resp_wait)
        {
            int result = coap_run_once(psCtx, wait_ms > 1000U ? 1000U : wait_ms);

            if (result >= 0)
            {
                if (result >= wait_ms)
                {
                    ESP_LOGE(pcTAG, "Select timeout");
                    break;
                }
                else
                {
                    wait_ms -= result;
                }
            }
    	}

clean_up:

        if (optlist)
        {
            coap_delete_optlist(optlist);
            optlist = NULL;
        }

        if (psSession)
        {
            coap_session_release(psSession);
        }

        if (psCtx)
        {
            coap_free_context(psCtx);
        }

        coap_cleanup();

        /*
         * change the following line to something like sleep(2)
         * if you want the request to continually be sent
         */
        vTaskDelay(COAP_POST_DELAY / portTICK_PERIOD_MS);
    }

    vTaskDelete(NULL);
}

void app_main(void)
{

	// Initialize NVS
	esp_err_t ret = nvs_flash_init();

	if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
	{
	  ESP_ERROR_CHECK(nvs_flash_erase());
	  ret = nvs_flash_init();
	}

	ESP_ERROR_CHECK(ret);

    // Initialize station mode
	wifi_init_sta();

    xTaskCreate(TempSensor_Task, "tempsensor_task", 2048U, NULL, 5U, NULL);
    xTaskCreate(CoAP_Task, "coap_task", 8192U, NULL, 5, NULL);

}
