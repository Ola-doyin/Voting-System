#include "esp_camera.h"
#include "img_converters.h"
#include "Arduino.h"
#include "fb_gfx.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoWebsockets.h>
#include "app_httpd.h"
#include "voters_html_gz.h"
#include "human_face_detect_msr01.hpp"
#include "human_face_detect_mnp01.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

//Replace your google script web app link here
const char *GOOGLE_SCRIPT_URL = "https://script.google.com/macros/s/****/exec";

using namespace websockets;

// === Public flags ===
bool face_ready_to_vote = false;
bool clientViewingStream = false;  // <-- ADDED FLAG
static bool adminModeSent = false;
bool shouldEnrollFingerprint = false;
bool shouldEnrollProfile = false;
bool shouldDeleteFinger = false;
bool shouldDeleteProfile = false;


String ProfileData = "";
String ProfileID = "";

// Internal state
static unsigned long face_detected_start = 0;
static bool vote_triggered = false;

// Websocket and server handles
WebsocketsServer wsServer;
WebsocketsClient *client = nullptr;

httpd_handle_t face_stream_httpd = NULL;
httpd_handle_t index_httpd = NULL;
httpd_handle_t camera_stream_httpd = NULL;

typedef struct {
  httpd_req_t *req;
  size_t len;
} jpg_chunking_t;



//TaskHandle_t googleRequestTaskHandle = NULL;
typedef struct {
  String url;
  String response;         // Filled by Core 0 after request
  SemaphoreHandle_t done;  // To notify the waiting task when done
} GoogleRequest;

//QueueHandle_t googleRequestQueue = xQueueCreate(5, sizeof(GoogleRequest));
QueueHandle_t googleRequestQueue = xQueueCreate(5, sizeof(GoogleRequest *));




#define PART_BOUNDARY "123456789000000000000987654321"
static const char *_STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char *_STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char *_STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";


// === Face detection models
HumanFaceDetectMSR01 detector_1(0.1F, 0.5F, 10, 0.2F);
HumanFaceDetectMNP01 detector_2(0.5F, 0.3F, 5);



// === WebSocket
void startWebSocketServer(IPAddress ip) {
  wsServer.listen(88);
  //Serial.println("✅ WebSocket server started at ws://" + ip.toString() + ":88");
}

void pollWebSocket() {
  if (wsServer.poll()) {
    if (client != nullptr) {
      client->close();
      delete client;
      client = nullptr;
    }

    client = new WebsocketsClient(wsServer.accept());

    if (client->available()) {
      client->onMessage([](WebsocketsMessage message) {
        String msg = message.data();

        if (msg == "LED_ON") {
          delay(50);
          //digitalWrite(4, HIGH);
          ledcWrite(flashChannel, 128);  // Turn off flash LED
        } else if (msg == "LED_OFF") {
          delay(50);
          //digitalWrite(4, LOW);
          ledcWrite(flashChannel, 0);  // Turn off flash LED
        } else if (msg == "ENROL_FINGER") {
          shouldEnrollFingerprint = true;
        } else if (msg.startsWith("ENROL_PROFILE ")) {
          ProfileData = "";
          ProfileData = msg.substring(strlen("ENROL_PROFILE "));
          // You can now split data into id, first name, last name
          //Serial.println("Profile Received: " + ProfileData);
          shouldEnrollProfile = true;
        } else if (msg.startsWith("DELETE_FINGER ")) {
          ProfileID = "";
          ProfileID = msg.substring(strlen("DELETE_FINGER "));
          shouldDeleteFinger = true;
        } else if (msg.startsWith("DELETE_PROFILE ")) {
          ProfileData = "";
          ProfileData = msg.substring(strlen("DELETE_PROFILE "));
          shouldDeleteProfile = true;
        }
      });
    }
  }

  if (client && client->available()) {
    client->poll();
  }
}


// === HTTP Handlers
static esp_err_t index_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html");
  httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
  return httpd_resp_send(req, (const char *)voters_html_gz, voters_html_gz_len);
}

static size_t jpg_encode_stream(void *arg, size_t index, const void *data, size_t len) {
  jpg_chunking_t *j = (jpg_chunking_t *)arg;
  if (!index) j->len = 0;
  if (httpd_resp_send_chunk(j->req, (const char *)data, len) != ESP_OK) return 0;
  j->len += len;
  return len;
}

// === Face Detection Stream Handler
static esp_err_t fd_stream_handler(httpd_req_t *req) {
  camera_fb_t *fb = NULL;
  esp_err_t res = ESP_OK;
  size_t jpg_buf_len = 0;
  uint8_t *jpg_buf = NULL;
  char part_buf[64];

  res = httpd_resp_set_type(req, _STREAM_CONTENT_TYPE);
  if (res != ESP_OK) return res;

  clientViewingStream = true;

  while (true) {
    fb = esp_camera_fb_get();
    if (!fb || fb->format != PIXFORMAT_JPEG || fb->len == 0) {
      if (fb) esp_camera_fb_return(fb);
      continue;
    }

    size_t rgb_buf_size = fb->width * fb->height * 3;
    uint8_t *rgb_buf = (uint8_t *)malloc(rgb_buf_size);
    if (!rgb_buf) {
      esp_camera_fb_return(fb);
      continue;
    }

    if (!fmt2rgb888(fb->buf, fb->len, fb->format, rgb_buf)) {
      free(rgb_buf);
      esp_camera_fb_return(fb);
      continue;
    }

    std::vector<int> shape = { fb->height, fb->width, 3 };
    auto results = detector_1.infer<uint8_t>(rgb_buf, shape);
    results = detector_2.infer<uint8_t>(rgb_buf, shape, results);

    fb_data_t fb_data = {
      .width = fb->width,
      .height = fb->height,
      .bytes_per_pixel = 3,
      .format = FB_RGB888,
      .data = rgb_buf
    };

    uint32_t red = 0xFF0000;
    for (auto &face : results) {
      int x = face.box[0], y = face.box[1];
      int w = face.box[2] - x, h = face.box[3] - y;
      fb_gfx_drawFastHLine(&fb_data, x, y, w, red);
      fb_gfx_drawFastHLine(&fb_data, x, y + h, w, red);
      fb_gfx_drawFastVLine(&fb_data, x, y, h, red);
      fb_gfx_drawFastVLine(&fb_data, x + w, y, h, red);
    }

    if (!fmt2jpg(rgb_buf, rgb_buf_size, fb->width, fb->height, PIXFORMAT_RGB888, 80, &jpg_buf, &jpg_buf_len)) {
      free(rgb_buf);
      esp_camera_fb_return(fb);
      continue;
    }

    size_t hlen = snprintf(part_buf, sizeof(part_buf), _STREAM_PART, jpg_buf_len);
    res = httpd_resp_send_chunk(req, part_buf, hlen);
    if (res == ESP_OK) res = httpd_resp_send_chunk(req, (const char *)jpg_buf, jpg_buf_len);
    if (res == ESP_OK) res = httpd_resp_send_chunk(req, _STREAM_BOUNDARY, strlen(_STREAM_BOUNDARY));

    free(rgb_buf);
    free(jpg_buf);
    esp_camera_fb_return(fb);

    if (res != ESP_OK) break;
  }

  clientViewingStream = false;
  return res;
}


// === Google Spreadsheet HTTP Task ===

void sendGoogleRequestTask(void *parameter) {
  GoogleRequest *request;

  while (true) {
    if (xQueueReceive(googleRequestQueue, &request, portMAX_DELAY) == pdTRUE) {
      HTTPClient https;
      https.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
      https.setTimeout(15000); // 15 seconds
      https.begin(request->url);

      int httpCode = https.GET();
      if (httpCode > 0) {
        request->response = https.getString();
      } else {
        request->response = "HTTP Error: " + String(httpCode);
      }

      https.end();

      xSemaphoreGive(request->done);  // Notify main task
    }
  }
}

String sendGoogleRequest(String url) {
  GoogleRequest *request = new GoogleRequest;
  request->url = url;
  request->response = "";
  request->done = xSemaphoreCreateBinary();

  if (googleRequestQueue != NULL) {
    xQueueSend(googleRequestQueue, &request, portMAX_DELAY);
    xSemaphoreTake(request->done, portMAX_DELAY);  // Wait for response
  }

  String result = request->response;

  vSemaphoreDelete(request->done);
  delete request;

  return result;
}


// === Camera Web Server
void startCameraServer(IPAddress ip) {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;

  httpd_uri_t index_uri = {
    .uri = "/",
    .method = HTTP_GET,
    .handler = index_handler,
    .user_ctx = NULL
  };

  httpd_uri_t face_stream_uri = {
    .uri = "/face_stream",
    .method = HTTP_GET,
    .handler = fd_stream_handler,
    .user_ctx = NULL
  };

  if (httpd_start(&index_httpd, &config) == ESP_OK) {
    httpd_register_uri_handler(index_httpd, &index_uri);
    httpd_register_uri_handler(index_httpd, &face_stream_uri);
    //Serial.println("Web server:   http://" + ip.toString() + "/");
    //Serial.println("Face stream:  http://" + ip.toString() + "/face_stream");
  } else {
    //Serial.println("❌ Failed to start HTTP server!");
  }
}

// === Background Face Detection (for use in loop)
void runFaceDetection() {
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb || fb->format != PIXFORMAT_JPEG || fb->len == 0) {
    if (fb) esp_camera_fb_return(fb);
    return;
  }

  size_t rgb_buf_size = fb->width * fb->height * 3;
  uint8_t *rgb_buf = (uint8_t *)malloc(rgb_buf_size);
  if (!rgb_buf) {
    esp_camera_fb_return(fb);
    return;
  }

  if (!fmt2rgb888(fb->buf, fb->len, fb->format, rgb_buf)) {
    free(rgb_buf);
    esp_camera_fb_return(fb);
    return;
  }

  std::vector<int> shape = { fb->height, fb->width, 3 };
  auto results = detector_1.infer<uint8_t>(rgb_buf, shape);
  results = detector_2.infer<uint8_t>(rgb_buf, shape, results);

  if (!results.empty()) {
    if (face_detected_start == 0) {
      face_detected_start = millis();
    } else if (!vote_triggered && millis() - face_detected_start >= 1500) {
      face_ready_to_vote = true;
      vote_triggered = true;
    }
  } else {
    face_detected_start = 0;
    vote_triggered = false;
  }

  free(rgb_buf);
  esp_camera_fb_return(fb);
}



void vote() {
  Serial.println("VOTE");
  String incoming = "";
  unsigned long votestart = millis();  // fixed here

  while (!clientViewingStream && (millis() - votestart) < 120000) {
    pollWebSocket();
    while (Serial.available()) {
      char ch = Serial.read();
      incoming += ch;

      if (ch == '\n' || ch == '\r') {
        incoming.trim();

        // Expecting format: VOTE,<id>,<pres>,<gub>,<sen>
        if (incoming.startsWith("VOTE,")) {
          int firstComma  = incoming.indexOf(',');
          int secondComma = incoming.indexOf(',', firstComma + 1);
          int thirdComma  = incoming.indexOf(',', secondComma + 1);
          int fourthComma = incoming.indexOf(',', thirdComma + 1);

          if (firstComma > 0 && secondComma > firstComma && thirdComma > secondComma && fourthComma > thirdComma) {
            String id   = incoming.substring(firstComma + 1, secondComma);
            String pres = incoming.substring(secondComma + 1, thirdComma);
            String gub  = incoming.substring(thirdComma + 1, fourthComma);
            String sen  = incoming.substring(fourthComma + 1);

            // GOOGLE SPREADSHEET LOGGING
            String url = String(GOOGLE_SCRIPT_URL) + "?action=vote&id=" + id + "&pres=" + pres + "&gub=" + gub + "&sen=" + sen;
            String response = sendGoogleRequest(url);

            if (response == "Vote logged") {
              Serial.println("VOTED");
            } else if (response == "Previously voted") {
              Serial.println("DOUBLE VOTE");
            } else if (response == "Unregistered ID") {
              Serial.println("UNREGISTERED");
            } else {
              Serial.println("POOR NETWORK");
            }

            return; // Exit after handling vote
          }
        } else if (incoming == "VOTING FAILED") {
          return;
        }

        incoming = ""; // Reset for next read
      }
    }
    delay(100);
  }

}


void enrollFingerprint() {
  Serial.println("ENROL");

  String incoming = "";
  unsigned long startTime = millis();
  const unsigned long timeout = 30000;  // 30 seconds

  while (millis() - startTime < timeout) {
    while (Serial.available()) {
      char ch = Serial.read();
      incoming += ch;

      if (ch == '\n' || ch == '\r') {
        incoming.trim();

        if (incoming.startsWith("ENROLLED ID: ")) {
          int id = incoming.substring(13).toInt();
          if (id > 0 && client && client->available()) {
            client->send("ID: " + String(id));
          }
          return;
        }

        else if (incoming == "NOT ENROLLED") {
          if (client && client->available()) {
            client->send("ENROLLMENT FAILED!");
          }
          return;
        }

        incoming = ""; // reset
      }

      if (incoming.length() > 100) incoming = ""; // prevent memory issues
    }
    delay(10); // avoid tight CPU loop
  }

  // Timeout response
  if (client && client->available()) {
    client->send("ENROLLMENT TIMEOUT!");
  }
}


void enrollProfile(const String &data) {
  // Parse the data
  int sep1 = data.indexOf('|');
  int sep2 = data.indexOf('|', sep1 + 1);

  if (sep1 == -1 || sep2 == -1) {
    if (client && client->available()) {
      client->send("[Profile] Error: Invalid data format.");
    }
    return;
  }

  String id = data.substring(0, sep1);
  String firstName = data.substring(sep1 + 1, sep2);
  String lastName = data.substring(sep2 + 1);

  //GOOGLE SPREADSHEET LOGGING
  String url = String(GOOGLE_SCRIPT_URL) + "?action=enroll&id=" + id + "&first=" + firstName + "&last=" + lastName;
  String response = sendGoogleRequest(url);
  //Serial.println(response);

  if (response == "Profile enrolled") {
    if (client && client->available()) {
      client->send("Profile Enrollment successful for ID: " + id);
    }
  } else {
    if (client && client->available()) {
      client->send("Profile Enrollment Failed!");
      client->send("GoogleSheet Response: " + response);
    }
  }

  ProfileData = "";
}


void deleteFingerprint() {
  if (ProfileID == "") return;

  Serial.println("DELETE ID: " + ProfileID);

  String incoming = "";
  //unsigned long startTime = millis();
  //while (millis() - startTime < 5000) {

  while (true) {
    while (Serial.available()) {
      char ch = Serial.read();
      incoming += ch;

      if (ch == '\n' || ch == '\r') {
        incoming.trim();

        // Check if deletion successful
        if (incoming == "DELETED ID: " + ProfileID) {
          String url = String(GOOGLE_SCRIPT_URL) + "?action=retrieve&id=" + ProfileID;
          String payload = sendGoogleRequest(url);

          // Expecting payload format: 123,John,Doe
          int firstComma = payload.indexOf(',');
          int secondComma = payload.indexOf(',', firstComma + 1);

          String firstName = payload.substring(firstComma + 1, secondComma);
          String lastName = payload.substring(secondComma + 1);

          if (client && client->available()) {
            client->send("PROFILE: " + firstName + "|" + lastName);
            client->send("Fingerprint " + ProfileID + " deleted");
          }

          ProfileID = "";
          return;
        }

        // Deletion failed
        if (incoming == "NOT DELETED ID: " + ProfileID) {
          //Serial.println("Fingerprint not deleted: " + ProfileID);
          if (client && client->available()) {
            client->send("Fingerprint " + ProfileID + " not deleted");
          }

          ProfileID = "";
          return;
        }

        incoming = "";
      }
    }
  }

  //Timeout
  if (client && client->available()) {
    client->send("Fingerprint Delete Timeout for ID: " + ProfileID);
  }
  ProfileID = "";
}


void deleteProfile(const String &data) {
  // Parse the data
  int sep1 = data.indexOf('|');
  int sep2 = data.indexOf('|', sep1 + 1);

  if (sep1 == -1 || sep2 == -1) {
    if (client && client->available()) {
      client->send("[Profile] Error: Invalid data format.");
    }
    return;
  }

  String id = data.substring(0, sep1);
  String firstName = data.substring(sep1 + 1, sep2);
  String lastName = data.substring(sep2 + 1);

  //GOOGLE SPREADSHEET LOGGING
  String url = String(GOOGLE_SCRIPT_URL) + "?action=delete&id=" + id + "&first=" + firstName + "&last=" + lastName;
  String response = sendGoogleRequest(url);
  //Serial.println(response);

  if (response == "Profile deleted") {
    if (client && client->available()) {
      client->send("Profile deleted: " + id);
    }
  } else {
    if (client && client->available()) {
      client->send("Profile Deletion Failed!");
      client->send("GoogleSheet Response: " + response);
    }
  }

  ProfileData = "";
}
