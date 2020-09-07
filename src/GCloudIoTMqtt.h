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
#ifndef GCLOUD_IOT_MQTT_H
#define GCLOUD_IOT_MQTT_H

#include <Arduino.h>

#include "CloudIoTCore.h"
#include "CloudIoTCoreDevice.h"

#include "WiFiClientSecureBearSSL.h"
#include <MQTTClient.h>

// error returned when wait backoff is not exceeded
#define GCIOT_BACKOFF_WAIT_NOT_EXCEEDED -100

class GCloudIoTMqtt {
  public:
    GCloudIoTMqtt(CloudIoTCoreDevice * device);
    virtual ~GCloudIoTMqtt();
	
    bool setup(int bufsize = 512, int keep_alive_sec = 180, int timeout_ms = 1000);
    void cleanup();

    bool connect(bool auto_reconnect = true, bool skip=false);
    bool connected();
    bool disconnect();

    void loop();
    
    bool publishTelemetry(String data);
    bool publishTelemetry(String data, int qos);
    bool publishTelemetry(const char* data, int length);
    bool publishTelemetry(String subtopic, String data);
    bool publishTelemetry(String subtopic, String data, int qos);
    bool publishTelemetry(String subtopic, const char* data, int length);
    bool publishState(String data);
    bool publishState(const char* data, int length);

    void setLogConnect(bool enabled);
    void setUseLts(bool enabled);

    void setMessageCallback(MQTTClientCallbackSimple cb);
    void setCommandCallback(MQTTClientCallbackSimple cb);
    void setConfigCallback(MQTTClientCallbackSimple cb);

    int getLastConnectReturnCode();
    String getLastConnectReturnCodeAsString();

    int getLastErrorCode();
    String getLastErrorCodeAsString();

    virtual void onMessageReceived(String &topic, String &payload);
    
    virtual bool isNetworkConnected();

  protected:
    void onConnect();
    
    void logConfiguration(bool showJWT);
    void logError();
    void logReturnCode();
	
  private: 
    int backoff_ms = 0; // current backoff, milliseconds
    unsigned long backoff_until_millis = 0; // time to wait to until next attempt
    bool logConnect = true;
    bool useLts = true;
    bool autoReconnect = false;
    MQTTClient * mqttClient = NULL;
    BearSSL::X509List * certList = NULL;
    BearSSL::WiFiClientSecure * netClient = NULL;
    CloudIoTCoreDevice *device = NULL;
    MQTTClientCallbackSimple commandCB = NULL;
    MQTTClientCallbackSimple configCB = NULL;
    MQTTClientCallbackSimple messageCB = NULL;
};
#endif // __CLOUDIOTCORE_MQTT_H__
