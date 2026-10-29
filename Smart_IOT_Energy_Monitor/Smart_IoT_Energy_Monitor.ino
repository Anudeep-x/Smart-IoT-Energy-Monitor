/*
  SMART IoT ENERGY MONITOR
  ESP32 + ZMPT101B + ACS712 20A + SSD1306 OLED + BUZZER
  Cloud: Blynk + ThingSpeak
  MLX90640 thermal camera: NOT USED

  Measures estimated RMS voltage/current, apparent power (VA),
  and apparent energy (kVAh). Not true real power (W).
  Calibrate both sensors before relying on readings.
*/

#define BLYNK_TEMPLATE_ID   "TMPL3BFAlBC-G"
#define BLYNK_TEMPLATE_NAME "Smart Energy Monitor"
#define BLYNK_AUTH_TOKEN    "rnTk1rdVnp06L7AXv7-qDEGSOPg-1vsr"

#include <WiFi.h>
#include <BlynkSimpleEsp32.h>
#include <ThingSpeak.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <math.h>

// =============== WIFI SETTINGS ===============
char ssid[] = "ACT-ai_102824320875";
char pass[] = "41613770";

// =============== THINGSPEAK SETTINGS ===============
unsigned long THINGSPEAK_CHANNEL_ID = 3548012;
const char *THINGSPEAK_WRITE_API_KEY =
    "HZGCEG93ZXPW7W49";

// =============== SENSOR AND BUZZER PINS ===============
const int VOLTAGE_PIN = 34;
const int CURRENT_PIN = 35;
const int BUZZER_PIN = 25;

// =============== CALIBRATION ===============
// Starting placeholders; calibrate with a reference meter.
float VOLTAGE_CAL_V_PER_ADC_V = 100.0;
float CURRENT_CAL_A_PER_ADC_V = 20.0;

// =============== OLED SETTINGS ===============
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_ADDRESS 0x3C

Adafruit_SSD1306 display(
    SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

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
    float sampleV = analogReadMilliVolts(pin) / 1000.0f;

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
      WiFi.status() == WL_CONNECTED ? "Connected" : "Offline");

  display.printf("Blynk: %s\n",
      Blynk.connected() ? "Connected" : "Offline");

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

// Blynk V6 slider sets the current alarm threshold.
BLYNK_WRITE(V6) {
  currentThresholdA = param.asFloat();

  Serial.printf(
      "Current threshold: %.2f A\n",
      currentThresholdA);

  // Apply the new threshold immediately.
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

  // Apparent power estimate, not real power.
  apparentPowerVA = voltageRMS * currentRMS;

  unsigned long now = millis();

  // Integrate apparent energy in kVAh.
  if (previousEnergyMs != 0) {
    float elapsedHours =
        (now - previousEnergyMs) / 3600000.0f;

    apparentEnergyKVAh +=
        (apparentPowerVA * elapsedHours) / 1000.0f;
  }

  previousEnergyMs = now;

  // Rate of change of apparent power in VA/s.
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

  // Trigger or clear the buzzer alarm.
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

  if (currentRMS > currentThresholdA) {
    Serial.println("WARNING: HIGH CURRENT - BUZZER ON");
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
      THINGSPEAK_WRITE_API_KEY);

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

  // Set buzzer output and keep it OFF at startup.
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  analogReadResolution(12);
  analogSetPinAttenuation(VOLTAGE_PIN, ADC_11db);
  analogSetPinAttenuation(CURRENT_PIN, ADC_11db);

  // OLED I2C: SDA = GPIO21, SCL = GPIO22.
  Wire.begin(21, 22);
  Wire.setClock(100000);

  if (display.begin(
          SSD1306_SWITCHCAPVCC,
          OLED_ADDRESS)) {
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

  // Connect to Wi-Fi.
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, pass);

  Serial.print("Connecting to Wi-Fi");
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
        "Wi-Fi connection failed. Check credentials.");
  }

  // Configure and connect Blynk.
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