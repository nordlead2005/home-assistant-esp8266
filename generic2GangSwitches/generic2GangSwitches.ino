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
const int GANG_COUNT = 1;
const bool TOGGLE[2] = { true, false };
const char* AVAILABILITY_FORMAT = "home/%s/availability";
const char* RESET_FORMAT = "home/%s/reset";
const char* SET_FORMAT = "%s/set";

/*
 * You MAY need to change these, but hopefully not
 */
const int PIN_SWITCH[2] = { D5, D2 };
const int PIN_SWITCH_TYPE[2] { INPUT_PULLUP, INPUT_PULLUP };
const int PIN_RELAY[2] = { D1, D6 };

/*
 * You SHOULDN'T need to change things below here
 */
const char* STATE_ON = "ON";
const char* STATE_OFF = "OFF";

const char* JSON_MQTT_SERVER = "mqtt_server";
const char* JSON_DEVICE_NAME = "device_name";
const char* JSON_SWITCH_STATE[2] = {"switch_state_1", "switch_state_2"};

const char* JSON_FILE = "/config.json";

//These are default values, if there are different values in config.json, they are overwritten.
char mqtt_server[20] = "192.168.125.2";
char device_name[50] = "esp8266Switch1";
char switchStates[2][75] = {"home/floor/room/lights", "home/floor/room2/lights"};
char switchCommands[2][75] = {"home/floor/room/lights/set", "home/floor/room2/lights/set"};
//these get automatically set based on the device name to the format set above
char availabilityTopic[75] = "home/deviceName/availability";
char resetTopic[75] = "home/deviceName/reset";

unsigned long lastAvailabliltySent = 0;

bool relayState[2] = { false, false };
bool lastSwitchState[2] = { false, false };

char buf[256];

WiFiServer server(23);
WiFiClient serverClient;
WiFiClient client;
PubSubClient mqttClient(client);
WiFiManager wifiManager;

bool shouldSaveConfig = false;

void setup()
{
  for(int i = 0; i < GANG_COUNT; ++i)
  {
    pinMode(PIN_RELAY[i], OUTPUT);
    pinMode(PIN_SWITCH[i], PIN_SWITCH_TYPE[i]);
    digitalWrite(PIN_RELAY[i], relayState[i]);
    lastSwitchState[i] = digitalRead(PIN_SWITCH[i]);
  }
  
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
  WiFiManagerParameter custom_switch_state_1("mqttState1", "mqtt state 1", switchStates[0], 75);
  WiFiManagerParameter custom_switch_state_2("mqttState2", "mqtt state 2", switchStates[1], 75);

  wifiManager.addParameter(&custom_mqtt_server);
  wifiManager.addParameter(&custom_device);
  wifiManager.addParameter(&custom_switch_state_1);
  if(GANG_COUNT > 1)
  {
    wifiManager.addParameter(&custom_switch_state_2);
  }
  
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
  strcpy(switchStates[0], custom_switch_state_1.getValue());
  sprintf(switchCommands[0], SET_FORMAT, custom_switch_state_1.getValue());
  if(GANG_COUNT > 1)
  {
    strcpy(switchStates[1], custom_switch_state_2.getValue());
    sprintf(switchCommands[1], SET_FORMAT, custom_switch_state_2.getValue());
  }
  sprintf(availabilityTopic, AVAILABILITY_FORMAT, device_name);
  sprintf(resetTopic, RESET_FORMAT, device_name);

  //save the custom parameters to FS
  if (shouldSaveConfig) {
    saveConfig();
  }
  
  Serial.println("Settings");
  Serial.println(mqtt_server);
  Serial.println(device_name);
  Serial.println(switchStates[0]);
  Serial.println(switchCommands[0]);
  Serial.println(switchStates[1]);
  Serial.println(switchCommands[1]);
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
          for(int i = 0; i < GANG_COUNT; ++i)
          {
            const char* switchState = json[JSON_SWITCH_STATE[i]];
            if(switchState)
            {
              strcpy(switchStates[i], switchState);
            }
            else
            {
              Serial.print("json missing ");
              Serial.println(JSON_SWITCH_STATE[i]);
              return false;
            }
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
    for(int i = 0; i < GANG_COUNT; i++)
    {
      json[JSON_SWITCH_STATE[i]] = switchStates[i];
    }

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
      for(int i = 0; i < GANG_COUNT; ++i)
      {
        reportTopic(switchStates[i], (relayState[i]) ? STATE_ON : STATE_OFF);
      }
      reportTopic(availabilityTopic, "online");
      lastAvailabliltySent = millis();
      
      // ... and resubscribe
      for(int i = 0; i < GANG_COUNT; ++i)
      {
        mqttClient.subscribe(switchCommands[i]);
      }
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


void setRelay(bool state, int channel)
{
  sprintf(buf, "Set Channel: %d\tPin: %d\tState: %d\r\n", channel, PIN_RELAY[channel], state);
  logger(buf);
  relayState[channel] = state;
  digitalWrite(PIN_RELAY[channel], state);
  reportTopic(switchStates[channel], (state) ? STATE_ON : STATE_OFF);
}

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

