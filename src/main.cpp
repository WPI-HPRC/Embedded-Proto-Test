/*
 *  Copyright (C) 2020-2024 Embedded AMS B.V. - All Rights Reserved
 *
 *  This file is part of Embedded Proto.
 *
 *  Embedded Proto is open source software: you can redistribute it and/or 
 *  modify it under the terms of the GNU General Public License as published 
 *  by the Free Software Foundation, version 3 of the license.
 *
 *  Embedded Proto  is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with Embedded Proto. If not, see <https://www.gnu.org/licenses/>.
 *
 *  For commercial and closed source application please visit:
 *  <https://EmbeddedProto.com/license/>.
 *
 *  Embedded AMS B.V.
 *  Info:
 *    info at EmbeddedProto dot com
 *
 *  Postal address:
 *    Atoomweg 2
 *    1627 LE, Hoorn
 *    the Netherlands
 */

/*-----------------------------------------------------------------------------------------------*/
/* Includes                                                                                      */
/*-----------------------------------------------------------------------------------------------*/
#include <inttypes.h>
#include <stdbool.h>

#include <esp_log.h>
#include <esp_wifi.h>
#include <esp_sntp.h>
#include <esp_netif.h>
#include <esp_netif_sntp.h>
#include <esp_random.h>
#include <nvs_flash.h>
#include <nvs.h>
#include <freertos/FreeRTOS.h>
#include <mqtt_client.h>
#include <protocol_examples_common.h>

#include "WriteBufferFixedSize.h"
#include "Errors.h"
#include "node.h"

/*-----------------------------------------------------------------------------------------------*/
/* External constants                                                                            */
/*-----------------------------------------------------------------------------------------------*/
extern const char MQTT_BROKER_CERT_CRT_START[] asm("_binary_mosquitto_crt_start");
extern const char MQTT_BROKER_CERT_CRT_END[] asm("_binary_mosquitto_crt_end");

/*-----------------------------------------------------------------------------------------------*/
/* Private constants                                                                             */
/*-----------------------------------------------------------------------------------------------*/
static constexpr const char* TAG = "[MAIN]";
static constexpr const char* MQTT_BROKER_URI = "mqtts://test.mosquitto.org:8883";
static constexpr const char* MQTT_PUBLISH_TOPIC = "my/sensor/temperature/proto";
static constexpr uint8_t MESSAGE_MAX_SIZE = 255;
static constexpr uint32_t PUBLISHING_PERIOD_MS = 2000;
static constexpr float TEMPERATURE_MAX_VALUE = 24.0f;
static constexpr float TEMPERATURE_MIN_VALUE = 26.0f;

/*-----------------------------------------------------------------------------------------------*/
/* Private function prototypes                                                                   */
/*-----------------------------------------------------------------------------------------------*/
static void onMqttEvent(void* args, esp_event_base_t eventBase, int32_t eventId, void* eventData);
static void onTimeAvailable(struct timeval* tv);
static float getFakeTemperature(float min, float max);

/*-----------------------------------------------------------------------------------------------*/
/* Private global variables                                                                      */
/*-----------------------------------------------------------------------------------------------*/
static SemaphoreHandle_t syncSemaphore = nullptr;

/*-----------------------------------------------------------------------------------------------*/
/* Public functions                                                                              */
/*-----------------------------------------------------------------------------------------------*/
/**
 * @brief  Application entry point
 * @return Nothing
 */
extern "C" void app_main(void) {
  esp_err_t ret;
  int64_t timestamp = 0;
  float temperature = 0;
  tm dateTime = {};
  esp_mqtt_client_handle_t mqttClient = nullptr;
  esp_mqtt_client_config_t mqttConfig = {};
  esp_sntp_config_t sntpConfig = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");

  node::Data nodeData;
  EmbeddedProto::WriteBufferFixedSize<MESSAGE_MAX_SIZE> writeBuffer;

  // Initialize the default NVS partition
  ret = nvs_flash_init();
  if ((ret == ESP_ERR_NVS_NO_FREE_PAGES) || (ret == ESP_ERR_NVS_NEW_VERSION_FOUND)) {
    // Erases all contents of the default NVS partition (one with label "nvs")
    ESP_ERROR_CHECK(nvs_flash_erase());
    // Initialize the default NVS partition
    ESP_ERROR_CHECK(nvs_flash_init());
  }

  // Connect to WiFi
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  ESP_ERROR_CHECK(example_connect());
  ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

  // Create a semaphore to signal when connected to the MQTT broker or when time is available
  syncSemaphore = xSemaphoreCreateBinary();
  assert(syncSemaphore);

  // Get the current timestamp
  time(&timestamp);

  // Convert timestamp to dateTime
  localtime_r(&timestamp, &dateTime);

  // Is time set? If not, tm_year will be (1970 - 1900)
  if (dateTime.tm_year == (1970 - 1900)) {
    ESP_LOGW(TAG, "Time is not set yet. Getting time over NTP...");

    // Initialize SNTP client with supplied config struct
    sntpConfig.sync_cb = onTimeAvailable;
    esp_netif_sntp_init(&sntpConfig);

    // Wait for time to be set...
    xSemaphoreTake(syncSemaphore, portMAX_DELAY);
    ESP_LOGI(TAG, "Time is available now");
  } else {
    ESP_LOGW(TAG, "Time is already set");
  }

  // Create an MQTT client handle based on the configuration
  mqttConfig.broker.address.uri = MQTT_BROKER_URI;
  mqttConfig.broker.verification.certificate = MQTT_BROKER_CERT_CRT_START;
  mqttClient = esp_mqtt_client_init(&mqttConfig);
  assert(mqttClient);

  // Register MQTT events
  ESP_ERROR_CHECK(esp_mqtt_client_register_event(mqttClient,
                                                 static_cast<esp_mqtt_event_id_t>(ESP_EVENT_ANY_ID),
                                                 onMqttEvent,
                                                 nullptr));

  // Starts MQTT client with already created handle
  ESP_ERROR_CHECK(esp_mqtt_client_start(mqttClient));
  ESP_LOGI(TAG, "Connecting to %s...", MQTT_BROKER_URI);

  // Wait for the MQTT client to connect
  xSemaphoreTake(syncSemaphore, portMAX_DELAY);

  ESP_LOGI(TAG, "MQTT client connected!");

  while (true) {
    // Fill nodeData object
    temperature = getFakeTemperature(TEMPERATURE_MIN_VALUE, TEMPERATURE_MAX_VALUE);
    nodeData.set_temperature(temperature);
    time(&timestamp);
    nodeData.set_timestamp(timestamp);

    ESP_LOGI(TAG, "[%" PRId64 "]: %.2f °C", timestamp, temperature);

    // Serialize nodeData object into a WriteBuffer
    if (nodeData.serialize(writeBuffer) == EmbeddedProto::Error::NO_ERRORS) {

      // Publish serialized data
      esp_mqtt_client_publish(mqttClient,
                              MQTT_PUBLISH_TOPIC,
                              reinterpret_cast<const char*>(writeBuffer.get_data()),
                              writeBuffer.get_size(),
                              0,
                              0);
    } else {
      ESP_LOGE(TAG, "Failed to serialize data");
    }
    vTaskDelay(pdMS_TO_TICKS(PUBLISHING_PERIOD_MS));
  }
}

/*-----------------------------------------------------------------------------------------------*/
/* Private functions                                                                             */
/*-----------------------------------------------------------------------------------------------*/
/**
 * @brief  Generate random temperature value between min and max
 * @param  min Minimal temperature threshold
 * @param  max Maximal temperature threshold
 * @return Fake temperature
 */
static float getFakeTemperature(float min, float max) {
  // Generate a random float between 0 and 1
  float random_float = static_cast<float>(esp_random()) / UINT32_MAX;

  // Scale it to the desired range [min, max]
  return min + random_float * (max - min);
}

/**
 * @brief  Time syncronization callback
 * @param  tv Pointer to new time value
 * @return Nothing
 */
static void onTimeAvailable(struct timeval* tv) {
  // Save the time
  settimeofday(tv, NULL);
  // Give the semaphore to signal that time is available
  xSemaphoreGive(syncSemaphore);
}

/**
 * @brief  MQTT event handler callback
 * @param  args Pointer to optional user argument
 * @param  eventBase Event base for the handler
 * @param  eventId ID for the received event
 * @param  eventData Data for the event
 * @return Nothing
 */
static void onMqttEvent(void* args, esp_event_base_t eventBase, int32_t eventId, void* eventData) {
  esp_mqtt_event_handle_t event = nullptr;

  // Restore a reference to the MQTT event
  event = static_cast<esp_mqtt_event_handle_t>(eventData);
  assert(event);

  switch (static_cast<esp_mqtt_event_id_t>(eventId)) {
    case MQTT_EVENT_CONNECTED: {
      ESP_LOGD(TAG, "MQTT_EVENT_CONNECTED");
      // Give the semaphore to signal connection
      xSemaphoreGive(syncSemaphore);
      break;
    }
    case MQTT_EVENT_DISCONNECTED: {
      ESP_LOGD(TAG, "MQTT_EVENT_DISCONNECTED");
      break;
    }
    case MQTT_EVENT_SUBSCRIBED: {
      ESP_LOGD(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
      break;
    }
    case MQTT_EVENT_UNSUBSCRIBED: {
      ESP_LOGD(TAG, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
      break;
    }
    case MQTT_EVENT_PUBLISHED: {
      ESP_LOGD(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
      break;
    }
    case MQTT_EVENT_DATA: {
      ESP_LOGD(TAG, "MQTT_EVENT_DATA");
      ESP_LOGI(TAG, "TOPIC=%.*s", event->topic_len, event->topic);
      ESP_LOGI(TAG, "DATA=%.*s", event->data_len, event->data);
      ESP_LOGI(TAG, "LENGTH=%d", event->data_len);
      break;
    }
    case MQTT_EVENT_ERROR: {
      ESP_LOGD(TAG, "MQTT_EVENT_ERROR");
      if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
          ESP_LOGE(TAG,
                   "Last error code reported from esp-tls: 0x%x",
                   event->error_handle->esp_tls_last_esp_err);
          ESP_LOGE(TAG,
                   "Last tls stack error number: 0x%x",
                   event->error_handle->esp_tls_stack_err);
          ESP_LOGE(TAG,
                   "Last captured errno : %d (%s)",
                   event->error_handle->esp_transport_sock_errno,
                   strerror(event->error_handle->esp_transport_sock_errno));
      } else if (event->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
        ESP_LOGE(TAG, "Connection refused error: 0x%x", event->error_handle->connect_return_code);
      } else {
        ESP_LOGW(TAG, "Unknown error type: 0x%x", event->error_handle->error_type);
      }
      break;
    }
    default: {
      ESP_LOGD(TAG, "Other event id:%d", event->event_id);
      break;
    }
  }
}
