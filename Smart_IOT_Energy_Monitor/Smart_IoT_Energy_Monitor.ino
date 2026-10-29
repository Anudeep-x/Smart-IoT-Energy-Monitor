/*
  SMART IoT ENERGY MONITOR
  ESP32 + ZMPT101B + ACS712 20A + SSD1306 OLED + BUZZER
  Cloud: Blynk + ThingSpeak
  MLX90640: NOT USED

  Measurements:
  - Estimated AC voltage and current RMS
  - Apparent power (VA)
  - Apparent energy (kVAh)
  - Apparent power change rate (VA/s)
  - High-current buzzer alarm

  IMPORTANT:
  Calibration constants are placeholders.
  This does not measure true real power (W) or kWh.
*/

#define BLYNK_TEMPLATE_ID   "YOUR_BLYNK_TEMPLATE_ID"
#define BLYNK_TEMPLATE_NAME "Smart Energy Monitor"
#define BLYNK_AUTH_TOKEN    "YOUR_BLYNK_AUTH_TOKEN"



#include <WiFi.h>
#include <BlynkSimpleEsp32.h>
#include <ThingSpeak.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <math.h>

// =============== WIFI SETTINGS ===============
char ssid[] = WIFI_SSID;
char pass[] = WIFI_PASSWORD;

// =============== THINGSPEAK SETTINGS ===============
unsigned long THINGSPEAK_CHANNEL_ID = 12345;
const char *tsWriteKey = THINGSPEAK_WRITE_API_KEY;

// =============== SENSOR PINS ===============
const int VOLTAGE_PIN = 34;  // ZMPT101B OUT
const int CURRENT_PIN = 35;  // ACS712 OUT
const int BUZZER_PIN  = 25;  // Active buzzer signal

// =============== CALIBRATION ===============
// Starting placeholders. Calibrate using a trusted reference meter.
float VOLTAGE_CAL_V_PER_ADC_V = 100.0;
float CURRENT_CAL_A_PER_ADC_V = 20.0;

// =============== OLED SETTINGS ===============
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_ADDRESS 0x3C

Adafruit_SSD1306 display(
    SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

bool oledAvailable = false;

// =============== CLOUD CONNECTIONS ===============
WiFiClient client;
BlynkTimer timer;

// =============== MEASUREMENT SETTINGS ===============
const int SAMPLE_COUNT = 1200;
const int SAMPLE_DELAY_US = 200;

const unsigned long MEASURE_INTERVAL_MS = 2000;
const unsigned long CLOUD_INTERVAL_MS = 20000;

// =============== MEASURED VALUES ===============
float voltageRMS = 0.0;
float currentRMS = 0.0;
float apparentPowerVA = 0.0;
float apparentEnergyKVAh = 0.0;
float powerChangeRate = 0.0;

float previousPowerVA = 0.0;
float currentThresholdA = 1.0;

unsigned long previousEnergyMs = 0;
unsigned long previousPowerMs = 0;

// =============== READ AC RIPPLE RMS ===============
float readACRippleRMSVolts(int pin) {
  double sum = 0.0;
  double sumSquares = 0.0;

  for (int i = 0; i < SAMPLE_COUNT; i++) {
    float sampleV =
        analogReadMilliVolts(pin) / 1000.0f;

    sum += sampleV;
    sumSquares += (double)sampleV * sampleV;

    delayMicroseconds(SAMPLE_DELAY_US);
  }

  double mean = sum / SAMPLE_COUNT;

  double variance =
      (sumSquares / SAMPLE_COUNT) - (mean * mean);

  if (variance < 0.0) {
    variance = 0.0;
  }

  return sqrt(variance);
}

// =============== BUZZER CONTROL ===============
void updateBuzzer() {
  if (currentRMS > currentThresholdA) {
    digitalWrite(BUZZER_PIN, HIGH);
  } else {
    digitalWrite(BUZZER_PIN, LOW);
  }
}

// =============== OLED DISPLAY ===============
void updateOLED() {
  if (!oledAvailable) {
    return;
  }

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);

  display.println("SMART ENERGY MON");
  display.printf("Voltage: %.1f V\n", voltageRMS);
  display.printf("Current: %.3f A\n", currentRMS);
  display.printf("Power:   %.1f VA\n", apparentPowerVA);
  display.printf("Energy:  %.5f kVAh\n",
                 apparentEnergyKVAh);

  display.printf("WiFi: %s\n",
      WiFi.status() == WL_CONNECTED
          ? "Connected" : "Offline");

  display.printf("Blynk: %s\n",
      Blynk.connected()
          ? "Connected" : "Offline");

  display.display();
}

// =============== BLYNK UPLOAD ===============
void updateBlynk() {
  if (!Blynk.connected()) {
    return;
  }

  Blynk.virtualWrite(V0, voltageRMS);
  Blynk.virtualWrite(V1, currentRMS);
  Blynk.virtualWrite(V2, apparentPowerVA);
  Blynk.virtualWrite(V3, apparentEnergyKVAh);
  Blynk.virtualWrite(V4, powerChangeRate);

  if (currentRMS > currentThresholdA) {
    Blynk.virtualWrite(V7, "HIGH CURRENT");
  } else {
    Blynk.virtualWrite(V7, "NORMAL");
  }
}

// Blynk V6 slider sets the current threshold.
BLYNK_WRITE(V6) {
  currentThresholdA = param.asFloat();

  Serial.printf(
      "Current threshold: %.2f A\n",
      currentThresholdA);

  updateBuzzer();
}

// =============== SENSOR MEASUREMENTS ===============
void updateMeasurements() {
  float voltageADC =
      readACRippleRMSVolts(VOLTAGE_PIN);

  float currentADC =
      readACRippleRMSVolts(CURRENT_PIN);

  voltageRMS =
      voltageADC * VOLTAGE_CAL_V_PER_ADC_V;

  currentRMS =
      currentADC * CURRENT_CAL_A_PER_ADC_V;

  // Apparent power estimate, not true real power.
  apparentPowerVA = voltageRMS * currentRMS;

  unsigned long now = millis();

  // Accumulate apparent energy in kVAh.
  if (previousEnergyMs != 0) {
    float elapsedHours =
        (now - previousEnergyMs) / 3600000.0f;

    apparentEnergyKVAh +=
        (apparentPowerVA * elapsedHours) / 1000.0f;
  }

  previousEnergyMs = now;

  // Rate of change of apparent power, VA/s.
  if (previousPowerMs != 0) {
    float elapsedSeconds =
        (now - previousPowerMs) / 1000.0f;

    if (elapsedSeconds > 0.0f) {
      powerChangeRate =
          (apparentPowerVA - previousPowerVA)
          / elapsedSeconds;
    }
  }

  previousPowerVA = apparentPowerVA;
  previousPowerMs = now;

  // Activate buzzer if current exceeds threshold.
  updateBuzzer();

  Serial.println("------ ENERGY MONITOR ------");

  Serial.printf("Voltage: %.2f V\n", voltageRMS);
  Serial.printf("Current: %.3f A\n", currentRMS);

  Serial.printf("Apparent power: %.2f VA\n",
                apparentPowerVA);

  Serial.printf("Apparent energy: %.6f kVAh\n",
                apparentEnergyKVAh);

  Serial.printf("Power change: %.2f VA/s\n",
                powerChangeRate);

  Serial.printf("Current threshold: %.2f A\n",
                currentThresholdA);

  if (currentRMS > currentThresholdA) {
    Serial.println("ALARM: HIGH CURRENT - BUZZER ON");
  } else {
    Serial.println("Status: NORMAL - BUZZER OFF");
  }

  updateOLED();
  updateBlynk();
}

// =============== THINGSPEAK UPLOAD ===============
void uploadThingSpeak() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("ThingSpeak skipped: Wi-Fi offline");
    return;
  }

  ThingSpeak.setField(1, voltageRMS);
  ThingSpeak.setField(2, currentRMS);
  ThingSpeak.setField(3, apparentPowerVA);
  ThingSpeak.setField(4, apparentEnergyKVAh);
  ThingSpeak.setField(5, powerChangeRate);

  int result = ThingSpeak.writeFields(
      THINGSPEAK_CHANNEL_ID,
      tsWriteKey);

  if (result == 200) {
    Serial.println("ThingSpeak upload successful");
  } else {
    Serial.printf(
        "ThingSpeak upload failed: HTTP %d\n",
        result);
  }
}

// =============== SETUP ===============
void setup() {
  Serial.begin(115200);
  delay(500);

  // Keep buzzer off at startup.
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  analogReadResolution(12);
  analogSetPinAttenuation(VOLTAGE_PIN, ADC_11db);
  analogSetPinAttenuation(CURRENT_PIN, ADC_11db);

  // OLED: SDA GPIO21, SCL GPIO22.
  Wire.begin(21, 22);
  Wire.setClock(100000);

  oledAvailable = display.begin(
      SSD1306_SWITCHCAPVCC,
      OLED_ADDRESS);

  if (oledAvailable) {
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.setTextSize(1);
    display.setCursor(0, 0);
    display.println("Starting monitor...");
    display.display();
  } else {
    Serial.println(
        "OLED not detected. Check wiring/address.");
  }

  // Connect Wi-Fi.
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, pass);

  Serial.print("Connecting to Wi-Fi...");

  unsigned long startTime = millis();

  while (WiFi.status() != WL_CONNECTED &&
         millis() - startTime < 20000) {
    delay(300);
    Serial.print(".");
  }

  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("Wi-Fi connected");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());

    ThingSpeak.begin(client);
  } else {
    Serial.println(
        "Wi-Fi unavailable. Check local credentials.");
  }

  // Configure Blynk.
  Blynk.config(BLYNK_AUTH_TOKEN);

  if (WiFi.status() == WL_CONNECTED) {
    Blynk.connect(5000);
  }

  previousEnergyMs = millis();
  previousPowerMs = millis();

  timer.setInterval(
      MEASURE_INTERVAL_MS,
      updateMeasurements);

  timer.setInterval(
      CLOUD_INTERVAL_MS,
      uploadThingSpeak);

  updateMeasurements();
}

// =============== MAIN LOOP ===============
void loop() {
  Blynk.run();
  timer.run();
}
