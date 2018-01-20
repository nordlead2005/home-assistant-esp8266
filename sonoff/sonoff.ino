#include <FS.h>
#include <WiFiManager.h>          //https://github.com/tzapu/WiFiManager
#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include "esp8266OTA.h"

#include <ArduinoJson.h>          //https://github.com/bblanchon/ArduinoJson

#define PIN_SWITCH 12
#define PIN_TOGGLE 14

const char* STATE_ON = "ON";
const char* STATE_OFF = "OFF";

//define your default values here, if there are different values in config.json, they are overwritten.
char mqtt_server[20] = "192.168.125.2";
char device_name[20] = "esp8266ChangeThis";
char extension_state[50] = "home/changeThis";
char extension_set[50] = "home/changeThis/set";

bool switchState = false;
bool lastToggleState = false;

char buf[256];

WiFiServer server(23);
WiFiClient serverClient;
WiFiClient client;
PubSubClient mqttClient(client);

bool shouldSaveConfig = false;

void setup() {
  pinMode(PIN_SWITCH, OUTPUT);
  pinMode(PIN_TOGGLE, INPUT_PULLUP);
  
  Serial.begin(115200);
  Serial.println("Booted Up :)");
  
  digitalWrite(PIN_SWITCH, false);
  lastToggleState = digitalRead(PIN_TOGGLE);

  readConfig();
  
  WiFiManager wifiManager;
  wifiManager.setSaveConfigCallback(saveConfigCallback);
  
  //reset settings - for testing
//  wifiManager.resetSettings();
  
  //sets timeout (seconds) until configuration portal gets turned off
  wifiManager.setTimeout(180);
  
  WiFiManagerParameter custom_mqtt_server("server", "mqtt server", mqtt_server, 20);
  WiFiManagerParameter custom_device("device", "device", device_name, 20);
  WiFiManagerParameter custom_extension_state("mqttState", "mqtt state", extension_state, 50);
  WiFiManagerParameter custom_extension_set("mqttSet", "mqtt set", extension_set, 50);

  wifiManager.addParameter(&custom_mqtt_server);
  wifiManager.addParameter(&custom_device);
  wifiManager.addParameter(&custom_extension_state);
  wifiManager.addParameter(&custom_extension_set);
  
//  wifiManager.startConfigPortal("OnDemandAP");
  if(!wifiManager.autoConnect())
  {
    Serial.println("failed to connect and hit timeout");
    ESP.reset();
  }
  
  //if you get here you have connected to the WiFi
  Serial.println("connected...yeey :)");

  //read updated parameters
  strcpy(mqtt_server, custom_mqtt_server.getValue());
  strcpy(device_name, custom_device.getValue());
  strcpy(extension_state, custom_extension_state.getValue());
  strcpy(extension_set, custom_extension_set.getValue());

    //save the custom parameters to FS
  if (shouldSaveConfig) {
    saveConfig();
  }
  
  Serial.println();
  Serial.print("connecting to ");
//  Serial.println(ssid);
//  WiFi.mode(WIFI_STA);
//  WiFi.begin(ssid, password);
//  while (WiFi.status() != WL_CONNECTED) {
//    delay(500);
//    Serial.print(".");
//  }
  Serial.println("");
  Serial.println("WiFi connected");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());

  
  Serial.println("Settings");
  Serial.println(mqtt_server);
  Serial.println(device_name);
  Serial.println(extension_state);
  Serial.println(extension_set);
  
  
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
      File configFile = SPIFFS.open("/config.json", "r");
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

          strcpy(mqtt_server, json["mqtt_server"]);
          strcpy(device_name, json["device_name"]);
          strcpy(extension_state, json["extension_state"]);
          strcpy(extension_set, json["extension_set"]);

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
    json["mqtt_server"] = mqtt_server;
    json["device_name"] = device_name;
    json["extension_state"] = extension_state;
    json["extension_set"] = extension_set;

    File configFile = SPIFFS.open("/config.json", "w");
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

void reconnect() {
  // Loop until we're reconnected
  while (!mqttClient.connected()) {
    logger("Attempting MQTT connection...");
    // Attempt to connect
    if (mqttClient.connect(device_name)) {
      logger("connected\r\n");
      // update levels ...
      reportTopic(extension_state, (switchState) ? STATE_ON : STATE_OFF);
      // ... and resubscribe
      mqttClient.subscribe(extension_set);
    } else {
      sprintf(buf, "failed, rc=%d try again in 5 seconds\r\n", mqttClient.state());
      // Wait 5 seconds before retrying
      delay(5000);
    }
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
    if (!mqttClient.connected())
    {
      reconnect();
    }
    mqttClient.loop();
    checkServer();

    checkToggle();
//    int waterLevel = analogRead(PIN_CHRISTMAS_TREE_WATER_LEVEL);
//    if(waterLevel < lastChristmasTreeWaterLevel - 10 || lastChristmasTreeWaterLevel + 10 < waterLevel)
//    {
//      reportTopic(CHRISTMAS_TREE_WATER_LEVEL, waterLevel);
//      lastChristmasTreeWaterLevel = waterLevel;
//    }
    
  }
  delay(100);
  ArduinoOTA.handle();
}

void checkToggle()
{
  bool toggleState = digitalRead(PIN_TOGGLE);
  if(toggleState != lastToggleState)
  {
    lastToggleState = toggleState;
    setRelay(!switchState);
  }
}


void setRelay(bool state)
{
  //printf("Button: %d\tstate: %b\tpin:%d\r\n", button, state, capRelayPin[button]);
//  switchCountdown[button] = 200;
  switchState = state;
  digitalWrite(PIN_SWITCH, state);
  reportTopic(extension_state, (state) ? STATE_ON : STATE_OFF);
}

void callback(char* topic, byte* payload, unsigned int length)
{
  sprintf(buf, "Received topic %s, payload %s\r\n", topic, payload);
  logger(buf);
  if(strncmp(topic, extension_set, strlen(extension_set)) == 0)
  {
    if ((char)payload[1] == 'N') //on
    {
      setRelay(true);
    }
    else if((char)payload[1] == 'F') //close
    {
      setRelay(false);
    }
  }
}

