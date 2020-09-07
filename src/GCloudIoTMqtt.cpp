/******************************************************************************
 * Copyright 2019 Google
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *****************************************************************************/
#include "GCloudIoTMqtt.h"
#include "CloudIoTCore.h"

#ifdef ESP8266
#include <ESP8266WiFi.h>
#endif

#ifdef ESP32 
#include <WiFi.h>
#endif

// connection exponential backoff settings
// see: https://cloud.google.com/iot/docs/how-tos/exponential-backoff
#define EXP_BACKOFF_FACTOR    2
#define EXP_BACKOFF_MIN_MS    1000
#define EXP_BACKOFF_MAX_MS    32000
#define EXP_BACKOFF_JITTER_MS 500

// Certificates for SSL on the Google Cloud IOT LTS server
const char* gciot_primary_ca = CLOUD_IOT_CORE_LTS_PRIMARY_CA;
const char* gciot_backup_ca = CLOUD_IOT_CORE_LTS_BACKUP_CA;



class MQTTClientWithCookie : public MQTTClient {
public:
  MQTTClientWithCookie(int bufSize, void * cookie)
    : MQTTClient(bufSize) { _cookie_ = cookie; }

  void * _cookie_;
};


void gciot_onMessageAdv(MQTTClient *client, char topic[], char bytes[], int length) {
  
  GCloudIoTMqtt * gcmqtt = (GCloudIoTMqtt*)((MQTTClientWithCookie *)client)->_cookie_;
  String topic_str = String(topic);
  String payload_str = String((const char *)bytes);

  if (gcmqtt != NULL) {
    gcmqtt->onMessageReceived(topic_str, payload_str);
  }
}

///////////////////////////////
// MQTT common functions
///////////////////////////////

GCloudIoTMqtt::GCloudIoTMqtt(CloudIoTCoreDevice * device)
{
  this->device = device;
}

GCloudIoTMqtt::~GCloudIoTMqtt()
{
  cleanup();
}	

bool GCloudIoTMqtt::setup(int bufsize, int keepAlive_sec, int timeout_ms)
{ 
  // ESP8266 WiFi setup
  this->netClient = new BearSSL::WiFiClientSecure();
  this->certList = new BearSSL::X509List();
  
  // ESP8266 WiFi secure initialization
  // Set CA cert on wifi client
  this->certList->append(gciot_primary_ca);
  this->certList->append(gciot_backup_ca);
  this->netClient->setTrustAnchors(this->certList);
  
  this->mqttClient = new MQTTClientWithCookie(bufsize, this);
  this->mqttClient->setOptions(keepAlive_sec, true, timeout_ms);
  
  this->backoff_until_millis = 0;
  this->backoff_ms = 0;
  this->useLts = true;

  if (this->useLts) {
    this->mqttClient->begin(CLOUD_IOT_CORE_MQTT_HOST_LTS, CLOUD_IOT_CORE_MQTT_PORT, *netClient);
  } else {
    this->mqttClient->begin(CLOUD_IOT_CORE_MQTT_HOST, CLOUD_IOT_CORE_MQTT_PORT, *netClient);
  }

  this->mqttClient->onMessageAdvanced(gciot_onMessageAdv);
  
  return true;
}

void GCloudIoTMqtt::cleanup() {
	
  if (this->mqttClient != NULL) {
    this->mqttClient->disconnect();
    delete this->mqttClient;
    this->mqttClient = NULL;
  }

  if (this->netClient != NULL) {
    delete this->netClient;
    this->netClient = NULL;
  }

  if (this->certList != NULL) {
    delete this->certList;
    this->certList = NULL;
  }

}

bool GCloudIoTMqtt::connect(bool auto_reconnect, bool skip) {
  this->autoReconnect = true;

  // regenerate JWT if expiring
  if ((millis() + 60000) > device->getExpMillis()) {
    // reconnecting before JWT expiration
    GCIOT_DEBUG_LOG("cloudiotmqtt: JWT expired, regenerating...\n");
	  device->createJWT(); // Regenerate JWT using device function
  }

  bool result =
      this->mqttClient->connect(
          device->getClientId().c_str(),
          "unused",
          device->getJWT().c_str(),
          skip);

  GCIOT_DEBUG_LOG("cloudiotmqtt: connect rc=%s [%d], errcode=%s [%d]\n",
      getLastConnectReturnCodeAsString().c_str(), mqttClient->returnCode(),
      getLastErrorCodeAsString().c_str(), getLastErrorCode());
		  
  if (result && this->mqttClient->connected()) {
    this->backoff_ms = 0;
    // Set QoS to 1 (ack) for configuration messages
    this->mqttClient->subscribe(device->getConfigTopic(), 1);
    // QoS 0 (no ack) for commands
    this->mqttClient->subscribe(device->getCommandsTopic(), 0);

    onConnect(); 

    return true;
  }

  switch(mqttClient->returnCode()) {
    case (LWMQTT_BAD_USERNAME_OR_PASSWORD):
    case (LWMQTT_NOT_AUTHORIZED):
      GCIOT_DEBUG_LOG("cloudiotmqtt: auth failed: regenerating JWT token\n");
      device->createJWT(); // Regenerate JWT using device function
    default:
      break;
  }

  // See https://cloud.google.com/iot/docs/how-tos/exponential-backoff
  if (this->backoff_ms < EXP_BACKOFF_MIN_MS) {
    this->backoff_ms = EXP_BACKOFF_MIN_MS + random(EXP_BACKOFF_JITTER_MS);
  } else if (this->backoff_ms < EXP_BACKOFF_MAX_MS) {
    this->backoff_ms = this->backoff_ms * EXP_BACKOFF_FACTOR + random(EXP_BACKOFF_JITTER_MS);
  }

	this->backoff_until_millis = millis() + this->backoff_ms;

  return false;
}

bool GCloudIoTMqtt::connected()
{
  return this->mqttClient->connected();
}

bool GCloudIoTMqtt::disconnect()
{
  this->autoReconnect = false;
  return mqttClient->disconnect();
}

void GCloudIoTMqtt::loop() {
  
  if (mqttClient->connected() && (millis() + 60000) > device->getExpMillis()) {
    // reconnecting before JWT expiration
    GCIOT_DEBUG_LOG("cloudiotmqtt: JWT expiring, disconnecting to regenerate...\n");
    mqttClient->disconnect();
    connect(autoReconnect, false); // TODO: should we skip closing connection
  } else if (autoReconnect && !mqttClient->connected() && millis() > this->backoff_until_millis) {
    if (isNetworkConnected()) {
      // attempt to reconnect only if network is connected
      GCIOT_DEBUG_LOG("cloudiotmqtt: reconnecting...\n");
      connect(true);
    }
  }

  this->mqttClient->loop();
}


bool GCloudIoTMqtt::publishTelemetry(String data) {
  return this->mqttClient->publish(device->getEventsTopic(), data);
}

bool GCloudIoTMqtt::publishTelemetry(String data, int qos) {
  return this->mqttClient->publish(device->getEventsTopic(), data, false, qos);
}

bool GCloudIoTMqtt::publishTelemetry(const char* data, int length) {
  return this->mqttClient->publish(device->getEventsTopic().c_str(), data, length);
}

bool GCloudIoTMqtt::publishTelemetry(String subtopic, String data) {
  return this->mqttClient->publish(device->getEventsTopic() + subtopic, data);
}

bool GCloudIoTMqtt::publishTelemetry(String subtopic, String data, int qos) {
  return this->mqttClient->publish(device->getEventsTopic() + subtopic, data, false, qos);
}

bool GCloudIoTMqtt::publishTelemetry(String subtopic, const char* data, int length) {
  return this->mqttClient->publish(String(device->getEventsTopic() + subtopic).c_str(), data, length);
}

// Helper that just sends default sensor
bool GCloudIoTMqtt::publishState(String data) {
  return this->mqttClient->publish(device->getStateTopic(), data);
}

bool GCloudIoTMqtt::publishState(const char* data, int length) {
  return this->mqttClient->publish(device->getStateTopic().c_str(), data, length);
}


void GCloudIoTMqtt::onConnect() {
  if (logConnect) {
    publishState("connected");
    publishTelemetry("/events", device->getDeviceId() + String("-connected"));
  }
}

void GCloudIoTMqtt::onMessageReceived(String &topic, String &payload) {
  if (device->getCommandsTopic().startsWith(topic)) {
    if (commandCB != NULL)
      commandCB(topic, payload);
  } else if (device->getConfigTopic().startsWith(topic)) {
    if (configCB != NULL)
      configCB(topic, payload);
  } else if (messageCB != NULL) {
	  messageCB(topic, payload);
  }
}

bool GCloudIoTMqtt::isNetworkConnected() {
  return WiFi.status() == WL_CONNECTED;
}

void GCloudIoTMqtt::setLogConnect(bool enabled) {
  this->logConnect = enabled;
}

void GCloudIoTMqtt::setUseLts(bool enabled) {
  this->useLts = enabled;
}

void GCloudIoTMqtt::setCommandCallback(MQTTClientCallbackSimple cb) {
  this->commandCB = cb;
}

void GCloudIoTMqtt::setConfigCallback(MQTTClientCallbackSimple cb) {
  this->configCB = cb;
}

void GCloudIoTMqtt::setMessageCallback(MQTTClientCallbackSimple cb) {
  this->messageCB = cb;
}

int GCloudIoTMqtt::getLastErrorCode()
{
  return this->mqttClient->lastError();
}

String GCloudIoTMqtt::getLastErrorCodeAsString()
{
  switch(mqttClient->lastError()) {
    case (LWMQTT_BUFFER_TOO_SHORT):
      return String("LWMQTT_BUFFER_TOO_SHORT");
    case (LWMQTT_VARNUM_OVERFLOW):
      return String("LWMQTT_VARNUM_OVERFLOW");
    case (LWMQTT_NETWORK_FAILED_CONNECT):
      return String("LWMQTT_NETWORK_FAILED_CONNECT");
    case (LWMQTT_NETWORK_TIMEOUT):
      return String("LWMQTT_NETWORK_TIMEOUT");
    case (LWMQTT_NETWORK_FAILED_READ):
      return String("LWMQTT_NETWORK_FAILED_READ");
    case (LWMQTT_NETWORK_FAILED_WRITE):
      return String("LWMQTT_NETWORK_FAILED_WRITE");
    case (LWMQTT_REMAINING_LENGTH_OVERFLOW):
      return String("LWMQTT_REMAINING_LENGTH_OVERFLOW");
    case (LWMQTT_REMAINING_LENGTH_MISMATCH):
      return String("LWMQTT_REMAINING_LENGTH_MISMATCH");
    case (LWMQTT_MISSING_OR_WRONG_PACKET):
      return String("LWMQTT_MISSING_OR_WRONG_PACKET");
    case (LWMQTT_CONNECTION_DENIED):
      return String("LWMQTT_CONNECTION_DENIED");
    case (LWMQTT_FAILED_SUBSCRIPTION):
      return String("LWMQTT_FAILED_SUBSCRIPTION");
    case (LWMQTT_SUBACK_ARRAY_OVERFLOW):
      return String("LWMQTT_SUBACK_ARRAY_OVERFLOW");
    case (LWMQTT_PONG_TIMEOUT):
      return String("LWMQTT_PONG_TIMEOUT");
    default:
      return String("Unknown error");
  }
}

int GCloudIoTMqtt::getLastConnectReturnCode()
{
  return this->mqttClient->returnCode();
}

String GCloudIoTMqtt::getLastConnectReturnCodeAsString()
{
  switch(mqttClient->returnCode()) {
    case (LWMQTT_CONNECTION_ACCEPTED):
      return String("OK");
    case (LWMQTT_UNACCEPTABLE_PROTOCOL):
      return String("LWMQTT_UNACCEPTABLE_PROTOCOLL");
    case (LWMQTT_IDENTIFIER_REJECTED):
      return String("LWMQTT_IDENTIFIER_REJECTED");
    case (LWMQTT_SERVER_UNAVAILABLE):
      return String("LWMQTT_SERVER_UNAVAILABLE");
    case (LWMQTT_BAD_USERNAME_OR_PASSWORD):
      return String("LWMQTT_BAD_USERNAME_OR_PASSWORD");
    case (LWMQTT_NOT_AUTHORIZED):
      return String("LWMQTT_NOT_AUTHORIZED");
    case (LWMQTT_UNKNOWN_RETURN_CODE):
      return String("LWMQTT_UNKNOWN_RETURN_CODE");
    default:
      return String("Unknown return code.");
  }
}
