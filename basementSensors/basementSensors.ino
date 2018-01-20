#include <FS.h>
#include <WiFiManager.h>          //https://github.com/tzapu/WiFiManager
#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <Wire.h>
#include "esp8266OTA.h"

#include <ArduinoJson.h>          //https://github.com/bblanchon/ArduinoJson

/*
 * Configure these to get different behavior 
 */
//const int GANG_COUNT = 1;
//const bool TOGGLE[2] = { true, false };
const char* AVAILABILITY_FORMAT = "home/%s/availability";
const char* RESET_FORMAT = "home/%s/reset";
//const char* SET_FORMAT = "%s/set";

/*
 * You MAY need to change these, but hopefully not
 */
const int PIN_ULTRASOUND_TRIGGER = D1;
const int PIN_ULTRASOUND_ECHO = D2;

/*
 * You SHOULDN'T need to change things below here
 */

const char* JSON_MQTT_SERVER = "mqtt_server";
const char* JSON_DEVICE_NAME = "device_name";
const char* JSON_ULTRASOUND_STATE = "ultrasound";

const char* JSON_FILE = "/config.json";

//These are default values, if there are different values in config.json, they are overwritten.
char mqtt_server[20] = "192.168.125.2";
char device_name[50] = "esp8266Switch1";
char ultrasound_state[75] = "home/floor/room/ultrasound";
//char switchCommands[2][75] = {"home/floor/room/lights/set", "home/floor/room2/lights/set"};
//these get automatically set based on the device name to the format set above
char availabilityTopic[75] = "home/deviceName/availability";
char resetTopic[75] = "home/deviceName/reset";

unsigned long lastAvailabliltySent = 0;

//bool relayState[2] = { false, false };
//bool lastSwitchState[2] = { false, false };

char buf[256];

WiFiServer server(23);
WiFiClient serverClient;
WiFiClient client;
PubSubClient mqttClient(client);
WiFiManager wifiManager;

bool shouldSaveConfig = false;

void setup()
{
  pinMode(PIN_ULTRASOUND_ECHO, INPUT);
  pinMode(PIN_ULTRASOUND_TRIGGER, OUTPUT);
  
  Serial.begin(115200);
  Serial.println("Booted Up :)");

  if(readConfig() == false)
  {
    //if we couldn't read a setting we need to reset the wifi to get the right settings anyways.
    wifiManager.resetSettings();
  }
  
  wifiManager.setSaveConfigCallback(saveConfigCallback);
  
  //sets timeout (seconds) until configuration portal gets turned off
  wifiManager.setTimeout(180);
  
  WiFiManagerParameter custom_mqtt_server("server", "mqtt server", mqtt_server, 20);
  WiFiManagerParameter custom_device("device", "device", device_name, 50);
  WiFiManagerParameter custom_ultrasound_state("ultrasoundState", "ultrasound state", ultrasound_state, 75);

  wifiManager.addParameter(&custom_mqtt_server);
  wifiManager.addParameter(&custom_device);
  wifiManager.addParameter(&custom_ultrasound_state);
  
//  wifiManager.startConfigPortal("OnDemandAP");
  if(!wifiManager.autoConnect())
  {
    Serial.println("failed to connect and hit timeout");
    ESP.reset();
  }
  
  //if you get here you have connected to the WiFi
  Serial.println("WiFi connected");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());

  //read updated parameters
  strcpy(mqtt_server, custom_mqtt_server.getValue());
  strcpy(device_name, custom_device.getValue());
  strcpy(ultrasound_state, custom_ultrasound_state.getValue());
  sprintf(availabilityTopic, AVAILABILITY_FORMAT, device_name);
  sprintf(resetTopic, RESET_FORMAT, device_name);

  //save the custom parameters to FS
  if (shouldSaveConfig) {
    saveConfig();
  }
  
  Serial.println("Settings");
  Serial.println(mqtt_server);
  Serial.println(device_name);
  Serial.println(ultrasound_state);
  Serial.println(availabilityTopic);

  mqttClient.setServer(mqtt_server, 1883);
  mqttClient.setCallback(callback);
  
  server.begin();
  server.setNoDelay(true);
  
  setupOTA(device_name);
}

bool readConfig()
{
    //read configuration from FS json
  Serial.println("mounting FS...");

  if (SPIFFS.begin()) {
    Serial.println("mounted file system");
    if (SPIFFS.exists("/config.json")) {
      //file exists, reading and loading
      Serial.println("reading config file");
      File configFile = SPIFFS.open(JSON_FILE, "r");
      if (configFile) {
        Serial.println("opened config file");
        size_t size = configFile.size();
        // Allocate a buffer to store contents of the file.
        std::unique_ptr<char[]> buf(new char[size]);

        configFile.readBytes(buf.get(), size);
        DynamicJsonBuffer jsonBuffer;
        JsonObject& json = jsonBuffer.parseObject(buf.get());
        json.printTo(Serial);
        if (json.success()) {
          Serial.println("\nparsed json");

          const char* mqttServer = json[JSON_MQTT_SERVER];
          if(mqttServer)
          {
            strcpy(mqtt_server, mqttServer);
          }
          else
          {
            Serial.println("json missing mqtt_server");
            return false;
          }
          const char* deviceName = json[JSON_DEVICE_NAME];
          if(deviceName)
          {
            strcpy(device_name, deviceName);
          }
          else
          {
            Serial.println("json missing device_name");
            return false;
          }
          const char* ultrasoundState = json[JSON_ULTRASOUND_STATE];
          if(ultrasoundState)
          {
            strcpy(ultrasound_state, ultrasoundState);
          }
          else
          {
            Serial.print("json missing ");
            Serial.println(JSON_ULTRASOUND_STATE);
            return false;
          }
          return true;
        } else {
          Serial.println("failed to load json config");
        }
      }
    }
  } else {
    Serial.println("failed to mount FS");
  }
  return false;
}

bool saveConfig()
{
    Serial.println("saving config");
    DynamicJsonBuffer jsonBuffer;
    JsonObject& json = jsonBuffer.createObject();
    json[JSON_MQTT_SERVER] = mqtt_server;
    json[JSON_DEVICE_NAME] = device_name;
    json[JSON_ULTRASOUND_STATE] = ultrasoundState;

    File configFile = SPIFFS.open(JSON_FILE, "w");
    if (!configFile) {
      Serial.println("failed to open config file for writing");
    }

    json.printTo(Serial);
    json.printTo(configFile);
    configFile.close();
    //end save
}

void saveConfigCallback()
{
  shouldSaveConfig = true;
}


void logger(const char* msg)
{
  Serial.print(msg);
  int len = strlen(msg);
  uint8_t sbuf[len];
  memcpy(sbuf, msg, len);
  if (serverClient && serverClient.connected())
  {
    serverClient.write(sbuf, len);
  }
}

void checkMqtt()
{
  if(!mqttClient.connected())
  {
    logger("Attempting MQTT connection...");
    if (mqttClient.connect(device_name, availabilityTopic, 0, 0, "offline"))
    {
      logger("connected\r\n");
      // update levels ...
      reportTopic(ultrasound_state, ultrasound_distance);
      reportTopic(availabilityTopic, "online");
      lastAvailabliltySent = millis();
      
      // ... and resubscribe
      mqttClient.subscribe(resetTopic);
    }
  }
  unsigned long now = millis();
  if((now - lastAvailabliltySent) > (15000))
  {
    reportTopic(availabilityTopic, "online");
    lastAvailabliltySent = now;
  }
}

void checkServer()
{
  if (server.hasClient()){
    serverClient.stop();
    serverClient = server.available();
    sprintf(buf, "New client\r\n");
    logger(buf);
  }
}

void reportTopic(const char* topic, const char* payload)
{
  mqttClient.publish(topic, payload);
  sprintf(buf, "%s: %s\r\n", topic, payload);
  logger(buf);
}

void reportTopic(const char* topic, int payload)
{
  sprintf(buf, "%d", payload);
  mqttClient.publish(topic, buf);
  sprintf(buf, "%s: %d\r\n", topic, payload);
  logger(buf);
}

void loop() {
  // put your main code here, to run repeatedly:
  if(WiFi.status() == WL_CONNECTED)
  {
    checkMqtt();
    mqttClient.loop();
    checkServer();  
  }

  for(int i = 0; i < GANG_COUNT; ++i)
  {
    if(TOGGLE[i])
    {
      checkToggle(i);
    }
    else
    {
      checkPushButton(i);
    }
  }
  delay(50);
  ArduinoOTA.handle();
}

void checkToggle(int channel)
{
  bool toggleState = digitalRead(PIN_SWITCH[channel]);
//  sprintf(buf, "Channel %d\tswitch: %d\tPrevious %d\tPin: %d\r\n", channel, toggleState, lastSwitchState[channel], PIN_SWITCH[channel]);
//  logger(buf);
  if(toggleState != lastSwitchState[channel])
  {
    lastSwitchState[channel] = toggleState;
    setRelay(!relayState[channel], channel);
  }
}

void checkPushButton(int channel)
{
  bool switchState = digitalRead(PIN_SWITCH[channel]);
  delayMicroseconds(500);
  bool switchState2 = digitalRead(PIN_SWITCH[channel]);
  if(switchState == switchState2)
  {
//    sprintf(buf, "Channel %d: switch %d, previous %d\r\n", channel, switchState, lastSwitchState[channel]);
//    logger(buf);
    if(switchState == false && lastSwitchState[channel] == true)
    {
      setRelay(!relayState[channel], channel);
    }
    lastSwitchState[channel] = switchState;
  }
}


//void setRelay(bool state, int channel)
//{
//  sprintf(buf, "Set Channel: %d\tPin: %d\tState: %d\r\n", channel, PIN_RELAY[channel], state);
//  logger(buf);
//  relayState[channel] = state;
//  digitalWrite(PIN_RELAY[channel], state);
//  reportTopic(switchStates[channel], (state) ? STATE_ON : STATE_OFF);
//}

void callback(char* topic, byte* payload, unsigned int length)
{
  sprintf(buf, "Received topic %s, payload %s\r\n", topic, payload);
  logger(buf);
  if(strncmp(topic, resetTopic, strlen(resetTopic)) == 0)
  {
    //reset settings and reboot ESP
    wifiManager.resetSettings();
    ESP.reset();
  }
  else
  {
    int channel = (strncmp(topic, switchCommands[0], strlen(switchCommands[0])) == 0) ? 0 : 1;
    
    if ((char)payload[1] == 'N') //on
    {
      setRelay(true, channel);
    }
    else if((char)payload[1] == 'F') //close
    {
      setRelay(false, channel);
    }
  }
}

