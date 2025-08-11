#define CAMERA_MODEL_AI_THINKER

#include "camera_pins.h"
#include "esp_camera.h"
#include <WiFi.h>
#include "Arduino.h"
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
#include "app_httpd.h"
#include "esp_sleep.h"
#include "driver/uart.h"

// 💤 WAKEUP pin
#define WAKEUP_PIN GPIO_NUM_2


// Use your Wi-Fi name and password here!
const char* ssid = ****;
const char* password = ****;


// WiFi Access Point Configuration
// const char* ssid = "Voters-Net";
// const char* password = "voters32";
// IPAddress local_ip(192, 168, 1, 1);
// IPAddress gateway(192, 168, 1, 1);
// IPAddress subnet(255, 255, 255, 0);

// Flash Led
const int flashPin = 4;         // GPIO 4 controls the white flash LED
const int flashChannel = 7;     // Pick any unused channel 0–15
const int freq = 5000;          // PWM frequency
const int pwmResolution = 8;    // 8-bit: duty from 0 to 255

static unsigned long last_vote_time = -12500;
bool prevClientViewing = false;

void setup() {
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);  // Disable brownout detector

  Serial.begin(115200);
  delay(1000);
  Serial.println();
  Serial.setDebugOutput(false);

  // === Camera configuration ===
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sscb_sda = SIOD_GPIO_NUM;
  config.pin_sscb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size = FRAMESIZE_CIF; //FRAMESIZE_QVGA;
  config.jpeg_quality = 10;
  config.fb_count = 2;

  if (esp_camera_init(&config) != ESP_OK) {
    Serial.println("Camera init failed");
    return;
  }

  //esp_log_level_set("*", ESP_LOG_WARN);
  esp_log_level_set("*", ESP_LOG_NONE); // Disable all logging

  // Flash LED
  ledcSetup(flashChannel, freq, pwmResolution);
  ledcAttachPin(flashPin, flashChannel);
  ledcWrite(flashChannel, 0);  // Turn off flash LED

  // pinMode(4, OUTPUT);  // Flash LED
  // digitalWrite(4, LOW);

  // GPIO 2 for wake input
  pinMode(WAKEUP_PIN, INPUT_PULLDOWN);

  // === Wi-Fi Access Point Mode ===
  // WiFi.softAP(ssid, password);
  // WiFi.softAPConfig(local_ip, gateway, subnet);

  // startCameraServer(local_ip);
  // startWebSocketServer(local_ip);

  // === Wi-Fi Station Mode ===
  WiFi.begin(ssid, password);
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    attempts++;
  }

  if (WiFi.status() != WL_CONNECTED) {
    //Serial.println("Wi-Fi failed to connect!");
    // Optional: restart or fallback to AP mode here
    ESP.restart();
    return;
  }
  Serial.println();

  startCameraServer(WiFi.localIP());
  startWebSocketServer(WiFi.localIP());

  //Google Spreadsheet HTTP Request
  xTaskCreatePinnedToCore(
    sendGoogleRequestTask,
    "GoogleRequestTask",
    8192,
    NULL,
    1,
    NULL,
    0  // Run on Core 0
  );

  delay(2500);
  Serial.println("WIFI CONNECTED: http://" + WiFi.localIP().toString() + "/");
}


void loop() {
  pollWebSocket();

  // === ADMIN MODE LOGIC ===
  if (clientViewingStream && !prevClientViewing) {
    Serial.println("ADMIN MODE");
  } else if (!clientViewingStream && prevClientViewing) {
    Serial.println("EXIT ADMIN");
  }
  prevClientViewing = clientViewingStream;

  if (clientViewingStream) {
    if (shouldEnrollFingerprint) {
      enrollFingerprint();
      shouldEnrollFingerprint = false;
    } else if (shouldEnrollProfile) {
      enrollProfile(ProfileData);
      shouldEnrollProfile = false;
    } else if (shouldDeleteFinger) {
      deleteFingerprint();
      shouldDeleteFinger = false;
    } else if (shouldDeleteProfile) {
      deleteProfile(ProfileData);
      shouldDeleteProfile = false;
    }
    return;  // Don't continue if in ADMIN mode
  }

  // === FACE DETECTION ===
  unsigned long now = millis();
  bool shouldDetect = !face_ready_to_vote && (now - last_vote_time >= 25000);

  if (shouldDetect) {
    ledcWrite(flashChannel, 200);  // Turn on flash LED
    runFaceDetection();
  }

  // === VOTING ===
  if (face_ready_to_vote) {
    ledcWrite(flashChannel, 0);
    vote();
    face_ready_to_vote = false;
    last_vote_time = millis();
    delay(100);
  }

  // === SERIAL COMMANDS ===
  String incoming = Serial.readStringUntil('\n');
  incoming.trim();
  if (incoming == "SLEEP") {
    Serial.println("SLEEPING");
    delay(100);
    ledcWrite(flashChannel, 0);
    esp_sleep_enable_ext0_wakeup(WAKEUP_PIN, 1);
    esp_deep_sleep_start();
  }

  delay(100);
}




