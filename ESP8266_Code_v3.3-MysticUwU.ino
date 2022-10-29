#pragma GCC optimize("-Ofast")
#include <ArduinoJson.h>
#include <bearssl/bearssl.h>
#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClient.h>
#include <Ticker.h>
#include <ESP8266WebServer.h>


#include "SSD1306Wire.h"
#include "OLEDDisplayUi.h"
#include "Wire.h"

#define RED 14
#define GREEN 12
#define BLUE 15
// Display Settings
const int I2C_DISPLAY_ADDRESS = 0x3c;

const int SDA_PIN = 4;
const int SDC_PIN = 5;

SSD1306Wire display(I2C_DISPLAY_ADDRESS, SDA_PIN, SDC_PIN);
OLEDDisplayUi ui(&display);


namespace {
// Change the part in brackets to your Duino-Coin username
const char *DUCO_USER = "MysticUwU";
// Change the part in brackets to your mining key (if you have enabled it in the wallet)
const char *MINER_KEY = "2022";
// Change the part in brackets to your WiFi name
const char *SSID = "PocoX2";
// Change the part in brackets to your WiFi password
const char *PASSWORD = "12341234";
// Change the part in brackets if you want to set a custom miner name (use Auto to autogenerate, None for no name)
const char *RIG_IDENTIFIER = "None";
// Set to true to use the 160 MHz overclock mode (and not get the first share rejected)
const bool USE_HIGHER_DIFF = true;
// Set to true if you want to host the dashboard page (available on ESPs IP address)
const bool WEB_DASHBOARD = false;
// Set to true if you want to update hashrate in browser without reloading the page
const bool WEB_HASH_UPDATER = false;
// Set to false if you want to disable the onboard led blinking when finding shares
const bool LED_BLINKING = true;

/* Do not change the lines below. These lines are static and dynamic variables
   that will be used by the program for counters and measurements. */
const char *DEVICE = "ESP8266";
const char *POOLPICKER_URL[] = { "https://server.duinocoin.com/getPool" };
const char *MINER_BANNER = "Official ESP8266 Miner";
const char *MINER_VER = "3.3";
unsigned int share_count = 0;
unsigned int port = 0;
unsigned int difficulty = 0;
float hashrate = 0;
String AutoRigName = "";
String host = "";
String node_id = "";
double balance;
int stakeAmount;

ESP8266WebServer server(80);

void hashupdater() {  //update hashrate every 3 sec in browser without reloading page
  server.send(200, "text/plain", String(hashrate / 1000));
  Serial.println("Update hashrate on page");
};
void setRGB(int r, int g, int b) {
  analogWrite(RED, r);
  analogWrite(GREEN, g);
  analogWrite(BLUE, b);
}

void UpdateHostPort(String input) {
  // Thanks @ricaun for the code
  DynamicJsonDocument doc(256);
  deserializeJson(doc, input);
  const char *name = doc["name"];

  host = String((const char *)doc["ip"]);
  port = int(doc["port"]);
  node_id = String(name);

  Serial.println("Poolpicker selected the best mining node: " + node_id);
}

String httpGetString(String URL) {
  String payload = "";
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;

  if (http.begin(client, URL)) {
    int httpCode = http.GET();

    if (httpCode == HTTP_CODE_OK) payload = http.getString();
    else Serial.printf("Error fetching node from poolpicker: %s\n", http.errorToString(httpCode).c_str());

    http.end();
  }
  return payload;
}

void fetchAPI() {
  String input = httpGetString("http://server.duinocoin.com/users/MysticUwU");
  StaticJsonDocument<80> filter;

  JsonObject filter_result = filter.createNestedObject("result");

  JsonObject filter_result_balance = filter_result.createNestedObject("balance");
  filter_result_balance["balance"] = true;
  filter_result_balance["stake_amount"] = true;
  filter_result_balance["stake_date"] = 1668816000;
  filter_result["miners"] = true;

  StaticJsonDocument<192> doc;

  DeserializationError error = deserializeJson(doc, input, DeserializationOption::Filter(filter));

  if (error) {
    Serial.print(F("deserializeJson() failed: "));
    Serial.println(error.f_str());
    return;
  }

  balance = doc["result"]["balance"]["balance"];
  stakeAmount = doc["result"]["balance"]["stake_amount"];
}


void drawOtaProgress(unsigned int progress, unsigned int total) {
  setRGB(5, 0, 0);
  display.clear();
  display.flipScreenVertically();
  display.setTextAlignment(TEXT_ALIGN_CENTER);
  display.setFont(ArialMT_Plain_10);
  display.drawString(64, 10, "OTA Update");
  display.drawProgressBar(2, 28, 124, 10, progress / (total / 100));
  display.display();
  setRGB(0, 0, 0);
}
void UpdatePool() {
  String input = "";
  int waitTime = 1;
  int poolIndex = 0;
  int poolSize = sizeof(POOLPICKER_URL) / sizeof(char *);

  while (input == "") {
    Serial.println("Fetching mining node from the poolpicker in " + String(waitTime) + "s");
    input = httpGetString(POOLPICKER_URL[poolIndex]);
    poolIndex += 1;

    // Check if pool index needs to roll over
    if (poolIndex >= poolSize) {
      poolIndex %= poolSize;
      delay(waitTime * 1000);

      // Increase wait time till a maximum of 32 seconds (addresses: Limit connection requests on failure in ESP boards #1041)
      waitTime *= 2;
      if (waitTime > 10)
        waitTime = 10;
    }
  }

  // Setup pool with new input
  UpdateHostPort(input);
}

WiFiClient client;
String client_buffer = "";
String chipID = "";
String START_DIFF = "";

// Loop WDT... please don't feed me...
// See lwdtcb() and lwdtFeed() below
Ticker lwdTimer;
#define LWD_TIMEOUT 20000

unsigned long lwdCurrentMillis = 0;
unsigned long lwdTimeOutMillis = LWD_TIMEOUT;

#define END_TOKEN '\n'
#define SEP_TOKEN ','

#define LED_BUILTIN 2

#define BLINK_SETUP_COMPLETE 2
#define BLINK_CLIENT_CONNECT 3
#define BLINK_RESET_DEVICE 5

void SetupWifi() {
  Serial.println("Connecting to: " + String(SSID));
  WiFi.mode(WIFI_STA);  // Setup ESP in client mode
  WiFi.setSleepMode(WIFI_NONE_SLEEP);
  WiFi.begin(SSID, PASSWORD);

  int wait_passes = 0;
  while (WiFi.waitForConnectResult() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    if (++wait_passes >= 10) {
      WiFi.begin(SSID, PASSWORD);
      wait_passes = 0;
    }
  }

  Serial.println("\n\nnSuccessfully connected to WiFi");
  Serial.println("Local IP address: " + WiFi.localIP().toString());
  Serial.println("Rig name: " + String(RIG_IDENTIFIER));
  Serial.println();

  UpdatePool();
}

void SetupOTA() {
  // Prepare OTA handler
  ArduinoOTA.onStart([]() {
    Serial.println("Start");
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("\nEnd");
  });
  ArduinoOTA.onProgress(drawOtaProgress);
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
    else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
    else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
    else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
    else if (error == OTA_END_ERROR) Serial.println("End Failed");
  });

  ArduinoOTA.setHostname(RIG_IDENTIFIER);  // Give port a name not just address
  ArduinoOTA.begin();
}

void blink(uint8_t count, uint8_t pin = LED_BUILTIN) {
  if (LED_BLINKING) {
    uint8_t state = HIGH;

    for (int x = 0; x < (count << 1); ++x) {
      digitalWrite(pin, state ^= HIGH);
      delay(50);
    }
  } else {
    digitalWrite(LED_BUILTIN, HIGH);
  }
}

void RestartESP(String msg) {
  Serial.println(msg);
  Serial.println("Restarting ESP...");
  blink(BLINK_RESET_DEVICE);
  ESP.reset();
}

// Our new WDT to help prevent freezes
// code concept taken from https://sigmdel.ca/michel/program/esp8266/arduino/watchdogs2_en.html
void ICACHE_RAM_ATTR lwdtcb(void) {
  if ((millis() - lwdCurrentMillis > LWD_TIMEOUT) || (lwdTimeOutMillis - lwdCurrentMillis != LWD_TIMEOUT))
    RestartESP("Loop WDT Failed!");
}

void lwdtFeed(void) {
  lwdCurrentMillis = millis();
  lwdTimeOutMillis = lwdCurrentMillis + LWD_TIMEOUT;
}

void VerifyWifi() {
  while (WiFi.status() != WL_CONNECTED || WiFi.localIP() == IPAddress(0, 0, 0, 0))
    WiFi.reconnect();
}

void handleSystemEvents(void) {
  VerifyWifi();
  ArduinoOTA.handle();
  yield();
}

// https://stackoverflow.com/questions/9072320/split-string-into-string-array
String getValue(String data, char separator, int index) {
  int found = 0;
  int strIndex[] = { 0, -1 };
  int max_index = data.length() - 1;

  for (int i = 0; i <= max_index && found <= index; i++) {
    if (data.charAt(i) == separator || i == max_index) {
      found++;
      strIndex[0] = strIndex[1] + 1;
      strIndex[1] = (i == max_index) ? i + 1 : i;
    }
  }
  return found > index ? data.substring(strIndex[0], strIndex[1]) : "";
}

void waitForClientData(void) {
  client_buffer = "";

  while (client.connected()) {
    if (client.available()) {
      client_buffer = client.readStringUntil(END_TOKEN);
      if (client_buffer.length() == 1 && client_buffer[0] == END_TOKEN)
        client_buffer = "???\n";  // NOTE: Should never happen

      break;
    }
    handleSystemEvents();
  }
}

void ConnectToServer() {
  if (client.connected())
    return;

  Serial.println("\n\nConnecting to the Duino-Coin server...");
  while (!client.connect(host.c_str(), port))
    ;

  waitForClientData();
  Serial.println("Connected to the server. Server version: " + client_buffer);
  blink(BLINK_CLIENT_CONNECT);  // Sucessfull connection with the server
}

bool max_micros_elapsed(unsigned long current, unsigned long max_elapsed) {
  static unsigned long _start = 0;

  if ((current - _start) > max_elapsed) {
    _start = current;
    return true;
  }
  return false;
}

}  // namespace

// https://github.com/esp8266/Arduino/blob/master/cores/esp8266/TypeConversion.cpp
const char base36Chars[36] PROGMEM = { '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M', 'N', 'O', 'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z' };
const uint8_t base36CharValues[75] PROGMEM{
  0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 0, 0, 0, 0, 0, 0, 0,                                                                         // 0 to 9
  10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 0, 0, 0, 0, 0, 0,  // Upper case letters
  10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35                     // Lower case letters
};
uint8_t *hexStringToUint8Array(const String &hexString, uint8_t *uint8Array, const uint32_t arrayLength) {
  assert(hexString.length() >= arrayLength * 2);
  for (uint32_t i = 0; i < arrayLength; ++i) {
    uint8Array[i] = (pgm_read_byte(base36CharValues + hexString.charAt(i * 2) - '0') << 4) + pgm_read_byte(base36CharValues + hexString.charAt(i * 2 + 1) - '0');
  }
  return uint8Array;
}


void drawFrame1(OLEDDisplay *display, OLEDDisplayUiState *state, int16_t x, int16_t y) {
  // draw an xbm image.
  // Please note that everything that should be transitioned
  // needs to be drawn relative to x and y
}

void drawFrame2(OLEDDisplay *display, OLEDDisplayUiState *state, int16_t x, int16_t y) {
  // Demonstrates the 3 included default sizes. The fonts come from SSD1306Fonts.h file
  // Besides the default fonts there will be a program to convert TrueType fonts into this format
  display->setTextAlignment(TEXT_ALIGN_LEFT);
  display->setFont(ArialMT_Plain_10);
  display->drawString(0 + x, 10 + y, "Arial 10");

  display->setFont(ArialMT_Plain_16);
  display->drawString(0 + x, 20 + y, "Arial 16");

  display->setFont(ArialMT_Plain_24);
  display->drawString(0 + x, 34 + y, "Arial 24");
}

void drawFrame3(OLEDDisplay *display, OLEDDisplayUiState *state, int16_t x, int16_t y) {
  // Text alignment demo
  display->setFont(ArialMT_Plain_10);

  // The coordinates define the left starting point of the text
  display->setTextAlignment(TEXT_ALIGN_LEFT);
  display->drawString(0 + x, 11 + y, "Left aligned (0,10)");

  // The coordinates define the center of the text
  display->setTextAlignment(TEXT_ALIGN_CENTER);
  display->drawString(64 + x, 22 + y, "Center aligned (64,22)");

  // The coordinates define the right end of the text
  display->setTextAlignment(TEXT_ALIGN_RIGHT);
  display->drawString(128 + x, 33 + y, "Right aligned (128,33)");
}

void drawFrame4(OLEDDisplay *display, OLEDDisplayUiState *state, int16_t x, int16_t y) {
  // Demo for drawStringMaxWidth:
  // with the third parameter you can define the width after which words will be wrapped.
  // Currently only spaces and "-" are allowed for wrapping
  display->setTextAlignment(TEXT_ALIGN_LEFT);
  display->setFont(ArialMT_Plain_10);
  display->drawStringMaxWidth(0 + x, 10 + y, 128, "Lorem ipsum\n dolor sit amet, consetetur sadipscing elitr, sed diam nonumy eirmod tempor invidunt ut labore.");
}

void drawFrame5(OLEDDisplay *display, OLEDDisplayUiState *state, int16_t x, int16_t y) {
}


FrameCallback frames[] = { drawFrame1, drawFrame2, drawFrame3, drawFrame4, drawFrame5 };
int frameCount = 5;
OverlayCallback overlays[] = { msOverlay };
int overlaysCount = 1;


void msOverlay(OLEDDisplay *display, OLEDDisplayUiState *state) {
  display->setTextAlignment(TEXT_ALIGN_RIGHT);
  display->setFont(ArialMT_Plain_10);
  display->drawString(128, 0, "Balance: " + String(balance) + "DUCOp");
}

void setup() {
  Serial.begin(115200);
  Serial.println("\nDuino-Coin " + String(MINER_VER));
  pinMode(LED_BUILTIN, OUTPUT);

  pinMode(RED, HIGH);
  pinMode(GREEN, HIGH);
  pinMode(BLUE, HIGH);

  setRGB(100, 100, 100);
  display.init();
  display.clear();
  display.display();

  display.setFont(ArialMT_Plain_10);
  display.setTextAlignment(TEXT_ALIGN_CENTER);
  display.setContrast(255);

  ui.setTargetFPS(30);
  ui.setFrameAnimation(SLIDE_LEFT);
  ui.setFrames(frames, frameCount);
  ui.setOverlays(overlays, overlaysCount);

  ui.init();
  // Autogenerate ID if required
  chipID = String(ESP.getChipId(), HEX);

  if (strcmp(RIG_IDENTIFIER, "Auto") == 0) {
    AutoRigName = "ESP8266-" + chipID;
    AutoRigName.toUpperCase();
    RIG_IDENTIFIER = AutoRigName.c_str();
  }

  SetupWifi();
  SetupOTA();

  lwdtFeed();
  lwdTimer.attach_ms(LWD_TIMEOUT, lwdtcb);
  if (USE_HIGHER_DIFF) START_DIFF = "ESP8266NH";
  else START_DIFF = "ESP8266N";

  blink(BLINK_SETUP_COMPLETE);
}

void loop() {

  ui.update();


  br_sha1_context sha1_ctx, sha1_ctx_base;
  uint8_t hashArray[20];
  String duco_numeric_result_str;

  // 1 minute watchdog
  lwdtFeed();

  // OTA handlers
  VerifyWifi();
  ArduinoOTA.handle();

  ConnectToServer();
  Serial.println("Asking for a new job for user: " + String(DUCO_USER));

  waitForClientData();
  String last_block_hash = getValue(client_buffer, SEP_TOKEN, 0);
  String expected_hash_str = getValue(client_buffer, SEP_TOKEN, 1);
  difficulty = getValue(client_buffer, SEP_TOKEN, 2).toInt() * 100 + 1;

  if (USE_HIGHER_DIFF) system_update_cpu_freq(160);

  int job_len = last_block_hash.length() + expected_hash_str.length() + String(difficulty).length();

  Serial.println("Received job with size of " + String(job_len) + " bytes: " + last_block_hash + " " + expected_hash_str + " " + difficulty);

  uint8_t expected_hash[20];
  hexStringToUint8Array(expected_hash_str, expected_hash, 20);

  br_sha1_init(&sha1_ctx_base);
  br_sha1_update(&sha1_ctx_base, last_block_hash.c_str(), last_block_hash.length());

  float start_time = micros();
  max_micros_elapsed(start_time, 0);

  String result = "";
  if (LED_BLINKING) digitalWrite(LED_BUILTIN, LOW);
  for (unsigned int duco_numeric_result = 0; duco_numeric_result < difficulty; duco_numeric_result++) {
    // Difficulty loop
    sha1_ctx = sha1_ctx_base;
    duco_numeric_result_str = String(duco_numeric_result);

    br_sha1_update(&sha1_ctx, duco_numeric_result_str.c_str(), duco_numeric_result_str.length());
    br_sha1_out(&sha1_ctx, hashArray);

    if (memcmp(expected_hash, hashArray, 20) == 0) {
      // If result is found
      if (LED_BLINKING) digitalWrite(LED_BUILTIN, HIGH);
      unsigned long elapsed_time = micros() - start_time;
      float elapsed_time_s = elapsed_time * .000001f;
      hashrate = duco_numeric_result / elapsed_time_s;
      share_count++;
      client.print(String(duco_numeric_result)
                   + ","
                   + String(hashrate)
                   + ","
                   + String(MINER_BANNER)
                   + " "
                   + String(MINER_VER)
                   + ","
                   + String(RIG_IDENTIFIER)
                   + ",DUCOID"
                   + String(chipID)
                   + "\n");

      waitForClientData();
      Serial.println(client_buffer
                     + " share #"
                     + String(share_count)
                     + " (" + String(duco_numeric_result) + ")"
                     + " hashrate: "
                     + String(hashrate / 1000, 2)
                     + " kH/s ("
                     + String(elapsed_time_s)
                     + "s)");
      break;
    }
    if (max_micros_elapsed(micros(), 500000)) {
      handleSystemEvents();
    }
  }
}