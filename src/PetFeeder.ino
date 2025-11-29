#include <WiFi.h>
#include <PubSubClient.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <ESP32Servo.h>
#include "HX711.h"
#include "time.h"
#include <vector>

// --- KONFIGURACIJA ---
const char* wifi_ssid = "***";
const char* wifi_password = "***";

const char* mqtt_server = "broker.hivemq.com";
const int mqtt_port = 1883;
const char* mqtt_topic_feed = "kaciukas/feed";
const char* mqtt_topic_distance = "kaciukas/distance";
const char* mqtt_topic_weight = "kaciukas/weight";
const char* mqtt_topic_status = "kaciukas/status";
const char* mqtt_topic_mode = "kaciukas/mode"; // SET: kaciukas/mode/set
const char* mqtt_topic_schedule = "kaciukas/schedule/set"; // Format: "13:00,16:00,19:00"

// PINAI
const int TRIG_PIN = 14;
const int ECHO_PIN = 27;
const int SERVO_PIN = 13;
const int LED_RED_PIN = 33;
const int LED_GREEN_PIN = 32;
const int LOADCELL_DOUT_PIN = 16;
const int LOADCELL_SCK_PIN = 4;

// OBJEKTAI
WiFiClient espClient;
PubSubClient mqtt(espClient);
LiquidCrystal_I2C lcd(0x27, 16, 2);
Servo servo;
HX711 scale;

// KINTAMIEJI
long duration;
float distance = 0;
float weight = 0;
String currentMode = "MANUAL"; // MANUAL, AUTO, SCHEDULE
unsigned long lastMqttSend = 0;
unsigned long lastFeedTime = 0;
const unsigned long FEED_COOLDOWN = 10000; // Sumazinam iki 10s testavimui
const float BOWL_FULL_WEIGHT = 50.0; // g (Pakeista i 50g)
int feedCount = 0; // Serimu skaicius (automatiniam rezimui)

// LAIKAS (NTP)
const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = 7200; // GMT+2 (Lietuva)
const int   daylightOffset_sec = 3600; // Vasaros laikas

// GRAFIKAS
struct FeedTime {
  int hour;
  int minute;
  bool fedToday;
};
std::vector<FeedTime> schedule;

void feedCat(String source);

void parseSchedule(String schedStr) {
  schedule.clear();
  int start = 0;
  int end = schedStr.indexOf(',');
  while (end != -1) {
    String timeStr = schedStr.substring(start, end);
    int split = timeStr.indexOf(':');
    if (split != -1) {
      FeedTime ft;
      ft.hour = timeStr.substring(0, split).toInt();
      ft.minute = timeStr.substring(split + 1).toInt();
      ft.fedToday = false;
      schedule.push_back(ft);
    }
    start = end + 1;
    end = schedStr.indexOf(',', start);
  }
  // Paskutinis elementas
  String timeStr = schedStr.substring(start);
  int split = timeStr.indexOf(':');
  if (split != -1) {
    FeedTime ft;
    ft.hour = timeStr.substring(0, split).toInt();
    ft.minute = timeStr.substring(split + 1).toInt();
    ft.fedToday = false;
    schedule.push_back(ft);
  }
  Serial.println("Grafikas atnaujintas! Elementu: " + String(schedule.size()));
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String message = "";
  for (int i = 0; i < length; i++) {
    message += (char)payload[i];
  }
  
  Serial.print("MQTT [");
  Serial.print(topic);
  Serial.print("]: ");
  Serial.println(message);
  
  if (String(topic) == mqtt_topic_feed) {
    if (message == "FEED") {
      feedCat("Web Manual");
    }
  } else if (String(topic) == "kaciukas/mode/set") {
    currentMode = message;
    mqtt.publish(mqtt_topic_mode, currentMode.c_str());
    Serial.println("Rezimas pakeistas i: " + currentMode);
    feedCount = 0; 
  } else if (String(topic) == mqtt_topic_schedule) {
    parseSchedule(message);
    mqtt.publish(mqtt_topic_status, "Grafikas atnaujintas!");
  }
}

void setupWifi() {
  delay(10);
  Serial.println();
  Serial.print("Jungiamasi prie ");
  Serial.println(wifi_ssid);
  
  WiFi.begin(wifi_ssid, wifi_password);
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("");
    Serial.println("WiFi prisijungta");
    Serial.println("IP adresas: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("WiFi nepavyko prisijungti");
  }
}


void connectMQTT() {
  while (!mqtt.connected()) {
    Serial.print("Jungiamasi prie MQTT...");
    String clientId = "ESP32Kaciukas-" + String(random(0xffff), HEX);
    
    if (mqtt.connect(clientId.c_str())) {
      Serial.println(" Prisijungta!");
      mqtt.subscribe(mqtt_topic_feed);
      mqtt.subscribe("kaciukas/mode/set");
      mqtt.subscribe(mqtt_topic_schedule);
      mqtt.publish(mqtt_topic_status, "Online");
      mqtt.publish(mqtt_topic_mode, currentMode.c_str());
    } else {
      Serial.print(" Nepavyko, rc=");
      Serial.print(mqtt.state());
      Serial.println(" bandoma is naujo uz 5s");
      delay(5000);
    }
  }
}

void setup() {
  Serial.begin(115200);

  // Pinai
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  pinMode(LED_RED_PIN, OUTPUT);
  pinMode(LED_GREEN_PIN, OUTPUT);
  
  digitalWrite(LED_RED_PIN, HIGH); // Raudona - kraunasi
  digitalWrite(LED_GREEN_PIN, LOW);

  // LCD
  // LCD
  // SVARBU: SCL laidas turi buti perkeltas i D22 pina!
  // RX0 (D3) naudojamas Serial komunikacijai, todel pjaunasi.
  Wire.begin(21, 22); // SDA=21, SCL=22 (Standartiniai)
  // Wire.begin(21, 3); // SDA, SCL
  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print("Sistema");
  lcd.setCursor(0, 1);
  lcd.print("Kraunasi...");

  // Servo
  servo.attach(SERVO_PIN);
  servo.write(0);

  // HX711
  Serial.println("HX711 Init...");
  scale.begin(LOADCELL_DOUT_PIN, LOADCELL_SCK_PIN);
  // Laukiame
  unsigned long start = millis();
  while (!scale.is_ready() && millis() - start < 2000) {
    delay(10);
  }
  if (scale.is_ready()) {
    scale.set_scale(2246.f);
    scale.tare(); 
    Serial.println("HX711 Ready");
  } else {
    Serial.println("HX711 Timeout");
  }

  setupWifi();
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);

  mqtt.setServer(mqtt_server, mqtt_port);
  mqtt.setCallback(mqttCallback);
  
  digitalWrite(LED_RED_PIN, LOW);
  digitalWrite(LED_GREEN_PIN, HIGH); // Zalia - OK
  lcd.clear();
  
  // Numatytasis grafikas (pvz 13:00)
  FeedTime ft; ft.hour = 13; ft.minute = 0; ft.fedToday = false;
  schedule.push_back(ft);
}

void updateSensors() {
  // Ultragarso sensorius
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);
  duration = pulseIn(ECHO_PIN, HIGH);
  distance = duration * 0.034 / 2;

  // Svoris - optimizuota, tik 1 matavimas kad neblokuotu
  if (scale.is_ready()) {
    float reading = scale.get_units(1); 
    // Paprastas filtras
    weight = 0.7 * weight + 0.3 * reading;
    if (weight < 0) weight = 0;
  }
}

void feedCat(String source) {
  // Pirmiausia tikrinam svori!
  if (weight >= BOWL_FULL_WEIGHT) {
    Serial.println("Dubuo pilnas (" + String(weight) + "g), nemaitinama.");
    mqtt.publish(mqtt_topic_status, "Dubuo pilnas!");
    return;
  }

  if (millis() - lastFeedTime < FEED_COOLDOWN) {
    Serial.println("Cooldown aktyvus!");
    return;
  }
  
  Serial.println("Maitinama... Saltinis: " + source);
  mqtt.publish(mqtt_topic_status, ("Maitinama: " + source).c_str());
  
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("MAITINAMA!");
  lcd.setCursor(0, 1);
  lcd.print(source);

  servo.write(180); // Atidaryti
  delay(3000);      // 3 sekundes byra (PAILGINTA)
  servo.write(0);   // Uzdaryti
  
  lastFeedTime = millis();
  feedCount++;
  
  delay(1000);
  lcd.clear();
}

void handleLogic() {
  struct tm timeinfo;
  bool timeSynced = getLocalTime(&timeinfo);

  if (currentMode == "AUTO") {
    // Jei kate priartejo (< 15cm) ir dar neserta 3 kartus
    if (distance > 0 && distance < 15) {
      if (feedCount < 3) {
        feedCat("Auto Sensorius");
      }
    }
  } 
  else if (currentMode == "SCHEDULE") {
    if (timeSynced) {
      for (auto &ft : schedule) {
        if (timeinfo.tm_hour == ft.hour && timeinfo.tm_min == ft.minute) {
          if (!ft.fedToday) {
            feedCat("Tvarkarastis " + String(ft.hour) + ":" + String(ft.minute));
            ft.fedToday = true;
          }
        } else if (timeinfo.tm_hour > ft.hour || (timeinfo.tm_hour == ft.hour && timeinfo.tm_min > ft.minute)) {
           // Jei laikas praejo, bet nebuvo pamaitinta (pvz buvo isjungta), cia galima ideti logika
           // Bet kol kas tiesiog pazymim kad praejo, kad nebutu double feed rytoj
           // Arba tiesiog paliekam false, ir resetinam vidurnakti
        }
        
        // Resetinam vidurnakti
        if (timeinfo.tm_hour == 0 && timeinfo.tm_min == 0) {
           ft.fedToday = false;
        }
      }
    }
  }
}

void loop() {
  if (!mqtt.connected()) connectMQTT();
  mqtt.loop();

  updateSensors();
  handleLogic();
  
  // LCD atnaujinimas
  static unsigned long lastLcdUpdate = 0;
  if (millis() - lastLcdUpdate > 1000) {
    // 1 eilutė: M:AUTO D:15cm
    lcd.setCursor(0, 0);
    lcd.print("                "); // Išvalome visą eilutę
    lcd.setCursor(0, 0);
    lcd.print("M:");
    lcd.print(currentMode.substring(0, 4)); // MANU, AUTO, SCHE
    lcd.print(" D:");
    lcd.print((int)distance);
    lcd.print("cm");
    
    // 2 eilutė: W:25g    21:51
    lcd.setCursor(0, 1);
    lcd.print("                "); // Išvalome visą eilutę
    lcd.setCursor(0, 1);
    lcd.print("W:");
    lcd.print((int)weight);
    lcd.print("g    ");
    
    struct tm timeinfo;
    if(getLocalTime(&timeinfo)){
      char timeStringBuff[6];
      strftime(timeStringBuff, sizeof(timeStringBuff), "%H:%M", &timeinfo);
      lcd.print(timeStringBuff);
    }
    
    lastLcdUpdate = millis();
  }

  // MQTT atnaujinimas
  if (millis() - lastMqttSend > 2000) {
    mqtt.publish(mqtt_topic_distance, String(distance, 1).c_str());
    mqtt.publish(mqtt_topic_weight, String(weight, 1).c_str());
    mqtt.publish(mqtt_topic_mode, currentMode.c_str());
    lastMqttSend = millis();
  }
  
  delay(50); // Mazesnis delay greitesniam reagavimui
}
