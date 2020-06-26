#include <Arduino.h>
#include <LiquidCrystal_I2C.h>
#include <analogWrite.h>
#include <DHT.h>
#include <WiFi.h>
#include <ESP32_FTPClient.h>
#include <esp_log.h>
#include <virtuabotixRTC.h>
#include <time.h>

// SENSORS - Peripherals for getting the knowledge from environment
#define DHTTYPE DHT11
#define DHTPIN 33
#define WATER_VCC_PIN 32
#define WATER_DATA_PIN 35
#define LDR_PIN 34

// WATER PUMP & LIGHT
#define PUMP_ENA 5
#define PUMP_IN2 18
#define LIGHT_IN3 19

// INDICATORS
#define WATER_LED_G 14
#define WATER_LED_R 12
#define HEAT_LED_R 27
#define HEAT_LED_G 26
#define WIFI_LED 0

// I2C PINS for LCD
#define LCD_SDA 21
#define LCD_SDC 22
#define LCD_LED 25

//RTC pins
#define RTC_CLK 17
#define RTC_DAT 16
#define RTC_RST 4

// Settings
#define WATER_LIMIT 1400
#define LIGHT_THRESHOLD 2600
#define WIFI_SSID   "Ally-Bros-T85E7"
#define WIFI_PASS   "vW0e0jgt"
#define FTP_SERVER  "192.168.1.1"
#define FTP_UNAME   "admin"
#define FTP_PASS    "dxr32"

// Define components
DHT dht(DHTPIN, DHTTYPE);
LiquidCrystal_I2C lcd(0x27,16,2);
ESP32_FTPClient ftp(FTP_SERVER, FTP_UNAME, FTP_PASS, 5000, 2);
virtuabotixRTC gardenRTC(RTC_CLK, RTC_DAT, RTC_RST);

// Global sensor data
static int solidMoisture = 0;
static float solidDrought = 0;
static float airHeatIndex = 0;
static float airHumidityIndex = 0;
static int lightIndex = 0;

unsigned int totalWateringCount = 0;
unsigned int dailyWateringCount = 0;
unsigned int failedWateringCount = 0;

// Status flags
bool lightOn = false;
bool autoLightEnabled = true;
bool network = false;
bool watered = false;
bool dhtError;
bool wateringError;

//Indicators
void setWaterIndicator(boolean alert);
void setHeatIndicator(boolean alert);

//Commands
std::vector<String> commandQueue;
std::vector<String> responseQueue;
void waterPlants();
void setBootTime();
void setLight(bool on);
void saveLog(String tag, String data);
String getStatus();
String getUpTime();
String getTimeStamp();

//Read Water procedure
void updateSensorData(boolean updateMoisture);
float readMoisture();

/**
* Wait for connection. Only one client allowed.
**/
WiFiServer wifiServer(1919);
TaskHandle_t serverTaskHandle;
void serverTask(void *parameters){
  wifiServer.begin();
  while(true) {
    WiFiClient client = wifiServer.available();
    if (client) {
      Serial.println("WifiServer: Client connected.");
      //Read data
      while (client.connected()) {
        bool incomingMsg = false;
        String msg = "";

        /* Write pending responses */
        if (!responseQueue.empty()) {
          String response = responseQueue.back();
          responseQueue.pop_back();
          // Create a byte buffer
          char responseBuf[response.length()];
          response.toCharArray(responseBuf, response.length());
          // Send data
          client.write(responseBuf);
        }

        // Read if client sent data
        while (client.available() > 0) {
          digitalWrite(2, LOW);
          incomingMsg = true;
          char c = client.read();
          msg+=c;
          digitalWrite(2, HIGH);
        }

        if (incomingMsg) {
          Serial.print("Incoming Command: ");
          Serial.println(msg);
          commandQueue.push_back(msg);
        }

        vTaskDelay(50 / portTICK_PERIOD_MS);
      }
      client.stop();
      Serial.println("WifiServer: Client disconnected");
    }
    vTaskDelay(50 / portTICK_PERIOD_MS);
  }
}

/**
* Keep network alive
**/
TaskHandle_t networkTaskHandle;
void networkTask(void *parameters){
  networkBegin:
  // Set WiFi to station mode and disconnect from an AP if it was previously connected
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  vTaskDelay(100 / portTICK_PERIOD_MS);

  // Try to connect network
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.println("Connecting...");

  while (WiFi.status() != WL_CONNECTED) {
    vTaskDelay(100 / portTICK_PERIOD_MS);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWifi Connected.");
    Serial.print(" IP Addres: ");
    Serial.println(WiFi.localIP());
    network = true;
    digitalWrite(2, HIGH);

    //Start server
    xTaskCreatePinnedToCore(
      serverTask,
      "serverTask",
      4066,
      NULL,
      1,
      &serverTaskHandle,
      0);
  }

  //keep wifi alive
  while(network) {
    if (WiFi.status() != WL_CONNECTED) {
      network = false;
      digitalWrite(2, LOW);
    }
    else {
      network = true;
      digitalWrite(2, HIGH);
    }
    vTaskDelay(6000 / portTICK_PERIOD_MS);
  }
  goto networkBegin;
}

/**
* Event scheduler
**/
TaskHandle_t schedulerTaskHandle;
void schedulerTask(void *parameters){
  while (true) {
    gardenRTC.updateTime();
    // TODO: Make cron type scheduler
    // Every day when hour is 21
    if (gardenRTC.hours >= 21 && gardenRTC.minutes == 0) {
      // Check enough watering today
      if (dailyWateringCount <= 2) {
        commandQueue.push_back("water-plants");
        saveLog("actions.log", "{\"action\": \"Watered plants.\", \"cause\": \"Not enough daily waterings.\"}");
      }
    }

    if (gardenRTC.hours >= 19 && gardenRTC.minutes == 0) {
      autoLightEnabled = false;
      setLight(false);
      saveLog("actions.log", "{\"action\": \"Disabled autolight.\", \"cause\": \"Scheduled event.\"}");
    }
    else if (gardenRTC.hours >= 11 && gardenRTC.minutes == 0) {
      autoLightEnabled = true;
      saveLog("actions.log", "{\"action\": \"Enabled autolight.\", \"cause\": \"Scheduled event.\"}");
    }

    if (gardenRTC.hours == 6 && gardenRTC.minutes == 0) {
      // New day, reset daily waterings.
      dailyWateringCount = 0;
    }

    // Each 20 mins
    if (gardenRTC.minutes % 20 == 0) {
      saveLog("status.log", getStatus());
    }

    vTaskDelay(60000 / portTICK_PERIOD_MS);
  }
}


// Set up
void setup() {
  Serial.begin(115200);
  Serial.println("Automata Garden v.0.1");
  Serial.println("Main task on core: ");
  Serial.print(xPortGetCoreID()+"\n");
  pinMode(2, OUTPUT);

  // Set Pin Modes
  pinMode(WATER_LED_R, OUTPUT);
  pinMode(WATER_LED_G, OUTPUT);
  pinMode(HEAT_LED_R, OUTPUT);
  pinMode(HEAT_LED_G, OUTPUT);
  pinMode(PUMP_IN2, OUTPUT);
  pinMode(PUMP_ENA, OUTPUT);
  pinMode(LIGHT_IN3, OUTPUT);
  pinMode(WATER_VCC_PIN, OUTPUT);
  pinMode(WATER_DATA_PIN, INPUT);
  pinMode(LDR_PIN, INPUT);

  // Set sensors
  dht.begin();
  lcd.init();
  lcd.backlight();
  digitalWrite(WATER_VCC_PIN, HIGH);
  setBootTime();
  // Welcome
  lcd.setCursor(0,0);
  lcd.printf("Automata Garden");
  lcd.setCursor(0,1);
  lcd.printf("Starting...");

  //Start network task
  xTaskCreatePinnedToCore(networkTask, "networkTask" , 8192, NULL, 1, &networkTaskHandle, 0);
  //Start scheduler task
  xTaskCreatePinnedToCore(schedulerTask, "schedulerTask", 4096, NULL, 1, &schedulerTaskHandle, 0);

  vTaskDelay(2000 / portTICK_PERIOD_MS);

  if (network) {
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.printf("WiFi connected.");
    lcd.setCursor(0, 1);
    lcd.print(WiFi.localIP().toString());
  }

  vTaskDelay(2000 / portTICK_PERIOD_MS);
}


// Main program
const int cycleLen = 500 / portTICK_PERIOD_MS; // Time between refresh UI
static byte cycle;
void loop() {
  //Constantly update ui
  cycle++;
  vTaskDelay(cycleLen);
  byte displayMod = cycle % 45;
  //Update sensor data
  updateSensorData(displayMod <= 15);
  Serial.printf("----- dm: %d\n", displayMod);
  Serial.printf("\rMoisture: %d\n", solidMoisture);
  Serial.printf("\rDrought: %.2f%%\n", solidDrought);
  Serial.printf("\rAir temp: %.2f\n", airHeatIndex);
  Serial.printf("\rHumidity: %.2f%%\n", airHumidityIndex);
  Serial.printf("\rLight: %d\n", lightIndex);

  //Check command queue
  if (!commandQueue.empty()) {
    String command = (String) commandQueue.back();
    commandQueue.pop_back();

    if (command.equals("water-plants")) {
      lcd.clear();
      lcd.setCursor(0,0);
      lcd.printf("Water command:");
      lcd.setCursor(0,1);
      lcd.print("Watering plants");
      responseQueue.push_back("watering?Watering plants, engine started.$");
      waterPlants();
      lcd.setCursor(0,1);
      lcd.print("Watering done.   ");
      responseQueue.push_back("watering?Completed watering, engine stopped.$");
      vTaskDelay(3000 / portTICK_PERIOD_MS);
    }
    else if (command.equals("status")){
      String statusResponse = getStatus();
      responseQueue.push_back(statusResponse + "$");
    }
    else if (command.equals("uptime")) {
      String uptimeResponse = "uptime?" + getUpTime() + "$";
      responseQueue.push_back(uptimeResponse);
    }
    else if (command.equals("systime")) {
      String systimeResponse = "systime?" + getTimeStamp() + "$";
      responseQueue.push_back(systimeResponse);
    }
    else if (command.equals("toggle-light")) {
      if (lightOn) {
        setLight(false);
        responseQueue.push_back("light?Light turned off.$");
      }
      else {
        setLight(true);
        responseQueue.push_back("light?Light turned on.$");
      }
    }
    else if (command.equals("toggle-autolight")) {
      if (autoLightEnabled) {
        autoLightEnabled = false;
        responseQueue.push_back("autolight?Auto light disabled.$");
      } else {
        autoLightEnabled = true;
        responseQueue.push_back("autolight?Auto light enabled.$");
      }
    }
    else {
      lcd.clear();
      lcd.setCursor(0,0);
      lcd.printf("Received:");
      lcd.setCursor(0,1);
      lcd.print(command);
      responseQueue.push_back("response?Invalid command.$");
      vTaskDelay(3000 / portTICK_PERIOD_MS);
    }
  }

  if (displayMod <= 2) {
    //Error displayMod
    if (dhtError) {
      Serial.printf("E: DHT Sensor failure!\n");
      lcd.setCursor(0,0);
      lcd.printf("Error: DHT       ");
      lcd.setCursor(0,1);
      lcd.printf("sensor failure!  ");
      dhtError = false;
      vTaskDelay(3000 / portTICK_PERIOD_MS);
      return;
    }

    if (wateringError){
      lcd.setCursor(0,0);
      lcd.printf("Error: Watering   ");
      lcd.setCursor(0,1);
      lcd.printf("failures: %d     ", failedWateringCount);
      wateringError = false;
      vTaskDelay(3000 / portTICK_PERIOD_MS);
      return;
    }

    if (!network){
      lcd.setCursor(0,0);
      lcd.printf("Error: WiFi     ");
      lcd.setCursor(0,1);
      lcd.printf("not connected!   ");
      wateringError = false;
      vTaskDelay(3000 / portTICK_PERIOD_MS);
      return;
    }
  }

  if (displayMod <= 15) {
    // Solid data
    lcd.setCursor(0,0);
    lcd.printf("Moisture: %d     ", (int) solidMoisture);
    lcd.setCursor(0,1);
    lcd.printf("Drought: %.1f %%      ", solidDrought);
  }

  else if (displayMod <= 30) {
    // DHT data
    digitalWrite(WATER_VCC_PIN, LOW);
    lcd.setCursor(0,0);
    lcd.printf("Humidity: %.1f %%    ", airHumidityIndex);
    lcd.setCursor(0,1);
    lcd.printf("Temp: %.2f C       ", airHeatIndex);
  }

  else {
    String time = getTimeStamp();
    lcd.setCursor(0,0);
    lcd.printf("%s", time.c_str());
    lcd.setCursor(0,1);
    lcd.printf("Light: %d       ", lightIndex);
  }

  // Take actions

  if (solidMoisture > WATER_LIMIT){
    // Soil drought high
    if (dailyWateringCount <= 2 && cycle == 127) {
      failedWateringCount++;
      watered = false;
    }
    else if (!watered) {
      // Water plants
      watered = true;
      lcd.setCursor(0,0);
      lcd.printf("High drought:   ");
      lcd.setCursor(0,1);
      lcd.printf("Watering plants ");
      waterPlants();
      lcd.printf("Watering done.  ");
      // Save log
      saveLog("actions", "{\"action\": \"Watered plants.\", \"cause\": \"High drought.\"}");
      vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
    setWaterIndicator(true);
  } else {
    wateringError = false;
    failedWateringCount = 0;
    setWaterIndicator(false);
  }

  //Decide light state
  if (autoLightEnabled) setLight(lightIndex <= 2600);

}


void updateSensorData(boolean updateMoisture){
  // Read moisture
  if (updateMoisture) {
    digitalWrite(WATER_VCC_PIN, HIGH);
    solidMoisture = readMoisture();
    solidDrought = (solidMoisture / 4095.0) * 100;
  }

  // Read light
  lightIndex = analogRead(LDR_PIN);

  //Read temp.
  float t = dht.readTemperature();
  airHumidityIndex = dht.readHumidity();
  airHeatIndex = dht.computeHeatIndex(t, airHumidityIndex, false); // In celcius

  // Error check
  if (isnan(airHumidityIndex) || isnan(t)) {
    Serial.println("E: DHT Sensor failure!");
    dhtError = true;
  } else {
    dhtError = false;
  }
}

/**
 * Read water index
 */
float readMoisture(){
  float avgData = 0;
  avgData = analogRead(WATER_DATA_PIN);
  return avgData;
}

/*
* Set water Indicator alert mode
*/
void setWaterIndicator(bool alert) {
  if (alert) {
    digitalWrite(WATER_LED_G, LOW);
    digitalWrite(WATER_LED_R, HIGH);
  } else {
    digitalWrite(WATER_LED_R, LOW);
    digitalWrite(WATER_LED_G, HIGH);
  }
}

/**
 * Set heat indicator alert mode
 */
void setHeatIndicator(bool alert) {
  if (alert) {
    digitalWrite(HEAT_LED_G, LOW);
    digitalWrite(HEAT_LED_R, HIGH);
  } else {
    digitalWrite(HEAT_LED_R, LOW);
    digitalWrite(HEAT_LED_G, HIGH);
  }
}

/*
* Run pump for watering, Watering command
*/
void waterPlants(){
  Serial.printf("Watering plants... (Total waterings: %d)\n", totalWateringCount);
  totalWateringCount++;
  dailyWateringCount++;
  //Close LED
  digitalWrite(LIGHT_IN3, LOW);
  // Turn on PUMP
  analogWrite(PUMP_ENA, 0);
  digitalWrite(PUMP_IN2, HIGH);
  // Soft Launch
  analogWrite(PUMP_ENA, 20);
  delay(250);
  analogWrite(PUMP_ENA, 30);
  delay(250);
  analogWrite(PUMP_ENA, 40);
  delay(250);
  analogWrite(PUMP_ENA, 50);
  delay(250);
  analogWrite(PUMP_ENA, 55);

  delay(7750);

  // Stop PUMP
  analogWrite(PUMP_ENA, 50);
  delay(250);
  analogWrite(PUMP_ENA, 40);
  delay(250);
  analogWrite(PUMP_ENA, 30);
  delay(250);
  analogWrite(PUMP_ENA, 0);
  digitalWrite(PUMP_IN2, LOW);
  delay(250);

  setLight(lightOn);

}

/**
* Turn additional ligting on if needed
**/
void setLight(bool on) {
  if (on) {
    digitalWrite(LIGHT_IN3, HIGH);
    lightOn = true;
  } else {
    digitalWrite(LIGHT_IN3, LOW);
    lightOn = false;
  }
}

/**
* Return status as string
**/
String getStatus(){
  String status = "{";
  status += "soilMoisture: " + (String) solidMoisture;
  status += ", soilDrought: " + (String) solidDrought;
  status += ", airHeat: " + (String) airHeatIndex;
  status += ", airHumidity: " + (String) airHumidityIndex;
  status += ", dayLight: " + (String) lightIndex;
  status += ", ledLightOn: " + (String) lightOn;
  status += ", totalWaterings: " + (String) totalWateringCount;
  status +="}";
  return status;
}

/**
* Records a log to the given file, with time stamp
**/
void saveLog(String tag, String data){
  char tagBuf[50];
  char dataBuf[50 + data.length()];
  String timeStamp = getTimeStamp();

  sprintf(dataBuf, "[%s] %s\n", timeStamp.c_str(), data.c_str());
  tag.toCharArray(tagBuf, tag.length()+1);

  ftp.OpenConnection();
  ftp.ChangeWorkDir("home/garden");

  //Write and close file
  ftp.InitFile("Type A");
  ftp.AppendFile(tagBuf);
  ftp.Write(dataBuf);
  ftp.CloseFile();
  ftp.CloseConnection();
}

/**
* Returns the uptime of the system
**/
String getUpTime(){
  unsigned long currentMillis = millis();
	int days = 0;
	int hours = 0;
	int mins = 0;
	int secs = 0;

	char timeStamp[32];
	secs = currentMillis/1000; //convect milliseconds to seconds
	mins = secs / 60; //convert seconds to minutes
	hours = mins / 60; //convert minutes to hours
	days = hours / 24; //convert hours to days

	secs = secs-(mins*60);
	mins = mins-(hours*60);
	hours = hours-(days*24);

	sprintf(timeStamp, "%d-%02d:%02d:%02d", days, hours, mins, secs);
	Serial.printf("Milis: %d, System is up for: %s", currentMillis, timeStamp);
	return timeStamp;
}

/**
* Returns the string time stamp from rtc
**/
String getTimeStamp(){
  gardenRTC.updateTime();
  int year = gardenRTC.year;
  int dayofmonth = gardenRTC.dayofmonth;
  int month = gardenRTC.month;
  int hours = gardenRTC.hours;
  int mins = gardenRTC.minutes;

  char timeStamp[32];
  sprintf(timeStamp, "%02d/%02d/%d %02d:%02d", dayofmonth, month, year, hours, mins);
  Serial.printf("%s\n", timeStamp);
  return String(timeStamp);
}
