#ifndef APP_HTTPD_H
#define APP_HTTPD_H

#include <Arduino.h>
#include <WiFi.h>
#include "esp_http_server.h"
#include <ArduinoWebsockets.h>

using namespace websockets;

// === External WebSocket server instance (defined in app_httpd.cpp)
extern WebsocketsServer wsServer;

extern String ProfileData;

// === LED Flash Configuration
extern const int flashPin;
extern const int flashChannel;
extern const int freq;
extern const int pwmResolution;

// Core 0 HTTP Task
extern QueueHandle_t googleRequestQueue;
void sendGoogleRequestTask(void *parameter);

// === Global face detection flag (set in app_httpd.cpp, read in main loop)
extern bool face_ready_to_vote;
extern bool clientViewingStream;
extern bool shouldEnrollFingerprint;
extern bool shouldEnrollProfile;
extern bool shouldDeleteFinger;
extern bool shouldDeleteProfile;

// === Public function declarations
void startCameraServer(IPAddress ip);         // Starts the camera + HTTP server
void startWebSocketServer(IPAddress ip);      // Starts the WebSocket server
void pollWebSocket();
void runFaceDetection();
void enrollFingerprint();
void enrollProfile(const String& data);
void deleteFingerprint();
void deleteProfile(const String& data);
void vote();
esp_err_t cmd_handler(httpd_req_t *req);      // Handles LED toggle and commands

#endif // APP_HTTPD_H
