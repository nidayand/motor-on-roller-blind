#include <Stepper_28BYJ_48.h>
#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <ESP8266mDNS.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include <WiFiManager.h>

#include <ArduinoJson.h>
#include "FS.h"

#define mqtt_server "192.168.2.195" //MQTT server IP
#define mqtt_port 1883              //MQTT server port

//WIFI and MQTT
WiFiClient espClient;
PubSubClient client(espClient);
int ledPin = 2;                     //PIN used for the onboard led

String action;                      //Action manual/auto
int path = 0;                       //Direction of blind (1 = down, 0 = stop, -1 = up)

//MQTT topics
const char* mqttclientid;         //Generated MQTT client id

//Stored data
long currentPosition = 0;           //Current position of the blind
long maxPosition = 2000000;         //Max position of the blind
boolean loadDataSuccess = false;
boolean saveItNow = false;          //If true will store positions to SPIFFS

Stepper_28BYJ_48 small_stepper(D1, D3, D2, D4);

bool loadConfig() {
  File configFile = SPIFFS.open("/config.json", "r");
  if (!configFile) {
    Serial.println("Failed to open config file");
    return false;
  }

  size_t size = configFile.size();
  if (size > 1024) {
    Serial.println("Config file size is too large");
    return false;
  }

  // Allocate a buffer to store contents of the file.
  std::unique_ptr<char[]> buf(new char[size]);

  // We don't use String here because ArduinoJson library requires the input
  // buffer to be mutable. If you don't use ArduinoJson, you may as well
  // use configFile.readString instead.
  configFile.readBytes(buf.get(), size);

  StaticJsonBuffer<200> jsonBuffer;
  JsonObject& json = jsonBuffer.parseObject(buf.get());

  if (!json.success()) {
    Serial.println("Failed to parse config file");
    return false;
  }
  json.printTo(Serial);
  Serial.println();

  currentPosition = long(json["currentPosition"]);
  maxPosition = long(json["maxPosition"]);

  // Real world application would store these values in some variables for
  // later use.

  Serial.print("Loaded currentPosition: ");
  Serial.println(currentPosition);
  Serial.print("Loaded maxPosition: ");
  Serial.println(maxPosition);
  return true;
}

bool saveConfig() {
  StaticJsonBuffer<200> jsonBuffer;
  JsonObject& json = jsonBuffer.createObject();
  json["currentPosition"] = currentPosition;
  json["maxPosition"] = maxPosition;

  File configFile = SPIFFS.open("/config.json", "w");
  if (!configFile) {
    Serial.println("Failed to open config file for writing");
    return false;
  }

  json.printTo(configFile);
  json.printTo(Serial);
  Serial.println();

  Serial.println("Saved JSON to SPIFFS");
  return true;
}

/*
 * Setup WIFI connection and connect the MQTT client to the
 * MQTT server
 */
void reconnect() {
  // Loop until we're reconnected
  while (!client.connected()) {
    Serial.print("Attempting MQTT connection...");
    // Attempt to connect
    if (client.connect(mqttclientid)) {
      Serial.println("connected");

      //Send register MQTT message with JSON of chipid and ip-address
      sendmsg("/raw/esp8266/register","{ \"id\": \""+String(ESP.getChipId())+"\", \"ip\":"+WiFi.localIP().toString()+"\"}");

      //Setup subscription
      client.subscribe(("/raw/esp8266/"+String(ESP.getChipId())+"/in").c_str());

    } else {
      Serial.print("failed, rc=");
      Serial.print(client.state());
      Serial.println(" try again in 5 seconds");
      // Wait 5 seconds before retrying
      ESP.wdtFeed();
      delay(5000);
    }
  }
}

/*
 * Blink the onboard led
 */
void qblink(){
    digitalWrite(ledPin, LOW); //turn on led
    delay(50);
    digitalWrite(ledPin, HIGH);
    delay(50);
}

/*
 * Common function to turn on WIFI radio, connect and get an IP address,
 * connect to MQTT server and publish a message on the bus.
 * Finally, close down the connection and radio
 */
void sendmsg(String topic, String payload){
    //Blink
    qblink();

    //Send status to MQTT bus if connected
    if (client.connected()){
      client.publish(topic.c_str(), payload.c_str());
      Serial.println("Published MQTT message");
    }
}

void callback(char* topic, byte* payload, unsigned int length) {
  /*
   * Possible input
   * - start. Will set existing position as 0
   * - max Will set the max position
   * - -1 / 0 / 1 . Will steer the blinds up/stop/down
   */
  Serial.print("Message arrived [");
  Serial.print(topic);
  Serial.print("] ");
  String res ="";
  for (int i=0;i<length;i++) {
    res+= String((char)payload[i]);
  }

  /*
   * Check if calibration is running and if stop is received. Store the location
   */
  if (action == "set" && res == "0"){
    maxPosition = currentPosition;
    saveItNow = true;
  }

  /*
   * Below are new actions
   */
  if (res == "start"){
    currentPosition = 0;
    path = 0;
    saveItNow = true;
    action = "manual";
  } else if (res == "max"){
    maxPosition = currentPosition;
    path = 0;
    saveItNow = true;
    action = "manual";
  } else if (res == "0"){
    path = 0;
    saveItNow = true;
    action = "manual";
  } else if (res == "1"){
    path = 1;
    action = "manual";
  } else if (res == "-1"){
    path = -1;
    action = "manual";
  } else if (res == "close"){
    path = 1;
    action = "auto";
  } else if (res == "open"){
    path = -1;
    action = "auto";
  }

  Serial.println(res);
  Serial.println();
}

void setup()
{
  Serial.begin(115200);
  delay(100);
  Serial.print("Starting now\n");

  action = "";

  //Setup MQTT Client ID
  mqttclientid = ("ESPClient-"+String(ESP.getChipId())).c_str();
  Serial.print("MQTT Client ID: ");
  Serial.println(mqttclientid);

  //Setup WIFI Manager
  WiFiManager wifiManager;
  wifiManager.autoConnect("AutoConnectAP", "mypassword");

  //Setup connections
  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(callback);

  //Load config upon start
  if (!SPIFFS.begin()) {
    Serial.println("Failed to mount file system");
    return;
  }
  loadDataSuccess = loadConfig();
  if (!loadDataSuccess){
    currentPosition = 0;
    maxPosition = 2000000;
  }

  //Setup OTA
  {

    // Authentication to avoid unauthorized updates
    //ArduinoOTA.setPassword((const char *)"mypassword");

    ArduinoOTA.onStart([]() {
      Serial.println("Start");
    });
    ArduinoOTA.onEnd([]() {
      Serial.println("\nEnd");
    });
    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
      Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
    });
    ArduinoOTA.onError([](ota_error_t error) {
      Serial.printf("Error[%u]: ", error);
      if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
      else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
      else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
      else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
      else if (error == OTA_END_ERROR) Serial.println("End Failed");
    });
    ArduinoOTA.begin();
  }
}

void loop(){
  //OTA client code
  ArduinoOTA.handle();

  if (!client.connected()) {
    reconnect();
  } else {
    client.loop();

    if (saveItNow){
      saveConfig();
      saveItNow = false;
    }

    if (action == "auto"){
      switch (path) {
        case -1:
            if (currentPosition > 0){
              small_stepper.step(path);
              currentPosition = currentPosition + path;
            } else {
              path = 0;
              Serial.println("Stopped. Reached top position");
              saveItNow = true;
            }
            break;
        case 1:
          if (currentPosition < maxPosition){
              small_stepper.step(path);
              currentPosition = currentPosition + path;
          } else {
            path = 0;
            Serial.println("Stopped. Reached max position");
            saveItNow = true;
          }
      }
    } else if (action == "manual" && path != 0) {
      small_stepper.step(path);
      currentPosition = currentPosition + path;
    }
  }
}
