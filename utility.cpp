// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. 

#include "HTS221Sensor.h"
#include "AzureIotHub.h"
#include "Arduino.h"
#include "parson.h"
#include "config.h"
#include "RGB_LED.h"
#include "Sensor.h"
#include "AZ3166WiFi.h"
#include "Sensor.h"
#include "DevKitMQTTClient.h"


static const char* iot_event = "{\"topic\":\"iot\"}"; // The #hashtag is 'iot' here, you can change to other keyword you want

#define RGB_LED_BRIGHTNESS 32

DevI2C *i2c;
HTS221Sensor *sensor;
LPS22HBSensor *lp_sensor;
static LSM6DSLSensor *acc_gyro;
static RGB_LED rgbLed;
static int interval = INTERVAL;
static int app_status;
static int shake_progress;
static int oldSteps = 0;

// The interval time of heart beat
static uint64_t hb_interval_ms;
// The timeout for retrieving the tweet
static uint64_t tweet_timeout_ms;

int getInterval()
{
    return interval;
}

void blinkLED()
{
    rgbLed.turnOff();
    rgbLed.setColor(RGB_LED_BRIGHTNESS, 0, 0);
    delay(500);
    rgbLed.turnOff();
}

void blinkSendConfirmation()
{
    rgbLed.turnOff();
    rgbLed.setColor(0, 0, RGB_LED_BRIGHTNESS);
    delay(500);
    rgbLed.turnOff();
}

// char* getMacAddress(mac) {
//     char mac[12];
//     WiFi.macAddress(mac);
//     return mac;
//     // ;
//     // String cMac = "";
//     // for (int i = 0; i < 6; ++i) {
//     // cMac += String(mac[i],HEX);
//     // if(i<5)
//     // cMac += "-";
//     // }
//     // cMac.toUpperCase();
//     // return cMac.c_str();
// }


void parseTwinMessage(DEVICE_TWIN_UPDATE_STATE updateState, const char *message)
{
    JSON_Value *root_value;
    root_value = json_parse_string(message);
    if (json_value_get_type(root_value) != JSONObject)
    {
        if (root_value != NULL)
        {
            json_value_free(root_value);
        }
        LogError("parse %s failed", message);
        return;
    }
    JSON_Object *root_object = json_value_get_object(root_value);

    double val = 0;
    if (updateState == DEVICE_TWIN_UPDATE_COMPLETE)
    {
        JSON_Object *desired_object = json_object_get_object(root_object, "desired");
        if (desired_object != NULL)
        {
            val = json_object_get_number(desired_object, "interval");
        }
    }
    else
    {
        val = json_object_get_number(root_object, "interval");
    }
    if (val > 500)
    {
        interval = (int)val;
        LogInfo(">>>Device twin updated: set interval to %d", interval);
    }
    json_value_free(root_value);
}

void SensorInit()
{
    i2c = new DevI2C(D14, D15);
    sensor = new HTS221Sensor(*i2c);
    sensor->init(NULL);

    lp_sensor= new LPS22HBSensor(*i2c);
    lp_sensor->init(NULL);

    acc_gyro = new LSM6DSLSensor(*i2c, D4, D5);
    acc_gyro->init(NULL); 
    acc_gyro->enableAccelerator();
    acc_gyro->enableGyroscope();
    acc_gyro->enablePedometer();
    acc_gyro->setPedometerThreshold(LSM6DSL_PEDOMETER_THRESHOLD_MID_LOW);
}

float readTemperature()
{
    sensor->reset();

    float temperature = 0;
    sensor->getTemperature(&temperature);

    return temperature;
}

float readHumidity()
{
    sensor->reset();

    float humidity = 0;
    sensor->getHumidity(&humidity);

    return humidity;
}

float readPressure()
{
    sensor->reset();

    float pressure = 0;
    lp_sensor->getPressure(&pressure);

    return pressure;
}

void DoShake(void)
{
  int steps = 0;
  acc_gyro->getStepCounter(&steps);
  if (oldSteps == 0) oldSteps = steps;
  if (steps - oldSteps > 2)
  {
    oldSteps = steps;
    LogInfo("Doing Shake! (Steps %d)", steps);
    hb_interval_ms = SystemTickCounterRead();
    
    // Enter the do work mode
    app_status = 2;
    // Shake detected
    shake_progress = 1;

    // LED
    DigitalOut LedUser(LED_BUILTIN);
    LedUser = 1;
    // Set RGB LED to red
    rgbLed.setColor(RGB_LED_BRIGHTNESS, 0, 0);
    // Update the screen
    //ShowShakeProgress();
    // Send to IoT hub
    if (DevKitMQTTClient_SendEvent(iot_event))
    {
      LogInfo("Shake Sent");
      if (shake_progress < 2)
      {
        // IoT hub has got the message
        // The tweet may return in the TwitterMessageCallback.
        // So check the shake_progress to avoid set the wrong value.
        shake_progress = 2;
      }
      // Update the screen
      //ShowShakeProgress();
      // Start retrieving tweet timeout clock
      tweet_timeout_ms = SystemTickCounterRead();
    }
    else
    {
      // Failed to send message to IoT hub
      //NoTweets();
    }
    LedUser = 0;
  }
  else
  {
    // Draw the animation
    //DrawShakeAnimation();
  }
}

bool readMessage(int messageId, char *payload)
{
    JSON_Value *root_value = json_value_init_object();
    JSON_Object *root_object = json_value_get_object(root_value);
    char *serialized_string = NULL;

    json_object_set_string(root_object, "deviceId", DEVICE_ID);
    json_object_set_number(root_object, "messageId", messageId);

    float temperature = readTemperature();
    float humidity = readHumidity();
    float pressure = readPressure();
    LogInfo("Telemetry: temperature=%f, humidity=%f, pressure=%f", temperature, humidity, pressure);
    bool temperatureAlert = false;
    if(temperature != temperature)
    {
        json_object_set_null(root_object, "temperature");
    }
    else
    {
        json_object_set_number(root_object, "temperature", temperature);
        if(temperature > TEMPERATURE_ALERT)
        {
            temperatureAlert = true;
        }
    }

    if(humidity != humidity)
    {
        json_object_set_null(root_object, "humidity");
    }
    else
    {
        json_object_set_number(root_object, "humidity", humidity);
    }
    
    json_object_set_number(root_object, "pressure", pressure);

    serialized_string = json_serialize_to_string_pretty(root_value);

    snprintf(payload, MESSAGE_MAX_LEN, "%s", serialized_string);
    json_free_serialized_string(serialized_string);
    json_value_free(root_value);
    return temperatureAlert;
}