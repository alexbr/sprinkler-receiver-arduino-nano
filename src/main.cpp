 /********************************************************

  Alex's sprinkler control

 *********************************************************/

#include "main.h"

#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>
#include <NTPClient.h>
#include <TimeLib.h>
#include <Timezone.h>
#include <WiFiNINA.h>
#include <WiFiServer.h>
#include <WiFiUdp.h>
#include <Wire.h>
#include <string.h>

#include "ButtonHook.h"
#include "secrets.h"

// Sprinkler state
enum SprinklerState { ON, OFF, UNKNOWN };
SprinklerState sprinklerState = UNKNOWN;
unsigned long sprinklerStart = 0;
unsigned long manualOverride = 0;
time_t sprinklerStartTime = 0;
char sprinklerStartDisplay[50] = "Hello\n";
char sprinklerDurationDisplay[50] = "Rodriguez Fam!\n";

time_t lastRunStartTime = 0;
unsigned long lastRunDurationMs = 0;

// Internet setup
WiFiServer server(80);
unsigned long checkServerTime = 0;
unsigned long lastHeapLog = 0;

// Display
Adafruit_SH1107 display = Adafruit_SH1107(64, 128, &Wire);
unsigned long turnDisplayOff = 0;
static int16_t displayScrollOffset = 0;
static int16_t displayMaxScroll = 0;
static unsigned long lastDisplayScroll = 0;
static unsigned long displayScrollEndTime = 0;

// Button handling
ButtonHook buttonHook(LONG_PRESS);

// NTP
IPAddress timeServer(129, 6, 15, 28);
WiFiUDP ntpUdp;
NTPClient ntpClient(ntpUdp, timeServer);
unsigned long checkTimeAfter = 0;
TimeChangeRule myPST = {"PST", Second, Sun, Mar,
                        2,     -7 * 60};
TimeChangeRule myPDT = {"PDT", First,  Sun, Nov,
                        2,     -8 * 60};
Timezone myTZ(myPST, myPDT);

static void setSprinklerOn();
static void setSprinklerOff();
static void pulseLatch(int pin);
static void scrollDisplayText(const char* str);
static void sendJsonResponse(WiFiClient& client, const char* body);
static void handleHttpRequests();

void setup() {
  Serial.begin(115200);

  pinMode(TRIGGER_PIN_ON, OUTPUT);
  pinMode(TRIGGER_PIN_OFF, OUTPUT);
  pulseLatch(TRIGGER_PIN_OFF);

  pinMode(BUTTON_A, INPUT_PULLUP);
  pinMode(BUTTON_B, INPUT_PULLUP);
  pinMode(BUTTON_C, INPUT_PULLUP);

  buttonHook.registerButton(BUTTON_A, &buttonAPressed);
  buttonHook.registerButton(BUTTON_B, &buttonBPressed);
  buttonHook.registerButton(BUTTON_C, &buttonCPressed);

  Wire.begin();
  display.begin(0x3C, true);
  display.setRotation(1);
  Serial.println("OLED begun");

  displayOnWithTimeout();
  display.setTextSize(2);
  display.setTextWrap(false);
  display.setTextColor(SH110X_WHITE);
  printDisplay("connecting internet...");

  const char* ssid = SECRET_SSID;
  const char* passkey = SECRET_PASSKEY;

  printStringLn("connecting internet");
  WiFi.begin(ssid, passkey);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    printString(".");
  }
  printStringLn("");

  WiFi.setAutoReconnect(true);

  printStringLn("internet initialized");
  char ipBuffer[32];
  snprintf(ipBuffer, sizeof(ipBuffer), "IP: %s",
           WiFi.localIP().toString().c_str());
  printDisplay(ipBuffer);

  server.begin();
  printStringLn("HTTP server started");

  ntpClient.begin();
  Serial.println("ntp started");
  setSyncProvider(getTime);
  Serial.println("ntp done");
}

void loop() {
  buttonHook.checkButtons();

  if (millis() >= turnDisplayOff) {
    display.clearDisplay();
    display.display();
  }

  if (manualOverride != 0 && (long)(millis() - manualOverride) >= 0) {
    manualOverride = 0;
    setSprinklerOff();
  }

  if (millis() > checkServerTime) {
    handleHttpRequests();
    checkServerTime = millis() + CHECK_SERVER_DELAY;
  }

  if (sprinklerState != OFF && (millis() - sprinklerStart) > SPRINKLER_ON_MAX) {
    printStringLn("sprinkler on past max threshold, turning off");
    setSprinklerOff();
  }

  if (sprinklerState == ON) {
    char buffer[64];
    unsigned long duration = millis() - sprinklerStart;
    if (manualOverride != 0) {
      unsigned long remaining = manualOverride - millis();
      snprintf(buffer, sizeof(buffer), "Running:\n%.1f mins\nRemaining:\n%.1f mins",
               duration / 60000.0, remaining / 60000.0);
    } else {
      snprintf(buffer, sizeof(buffer), "Running:\n%.1f mins",
               duration / 60000.0);
    }
    printDisplay(buffer);
  } else {
    char buffer[sizeof(sprinklerStartDisplay) +
                sizeof(sprinklerDurationDisplay) + 2];
    snprintf(buffer, sizeof(buffer), "%s\n%s", sprinklerStartDisplay,
             sprinklerDurationDisplay);
    printDisplay(buffer);
  }

  if (millis() - lastHeapLog >= 60000UL) {
    lastHeapLog = millis();
    Serial.println("heartbeat");
  }
}

void buttonAPressed(bool longPress) { sprinklerOnButtonHandler(longPress); }
void buttonBPressed(bool longPress) { sprinklerOffButtonHandler(); }
void buttonCPressed(bool longPress) { displayOnWithTimeout(); }

void sprinklerOnButtonHandler(bool longPress) {
  if (longPress) {
    printStringLn("running sprinkler for 60 mins");
    manualOverride = millis() + MANUAL_OVERRIDE_60;
  } else {
    printStringLn("running sprinkler for 30 mins");
    manualOverride = millis() + MANUAL_OVERRIDE_30;
  }

  setSprinklerOn();
  displayOnWithTimeout();
}

void sprinklerOffButtonHandler() {
  manualOverride = 0;
  setSprinklerOff();
  displayOnWithTimeout();
}

static void sendJsonResponse(WiFiClient& client, const char* body) {
  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: application/json");
  client.println("Connection: close");
  client.println();
  client.print(body);
}

static void handleHttpRequests() {
  WiFiClient client = server.available();
  if (!client) {
    return;
  }

  String request = client.readStringUntil('\r');

  if (request.indexOf("GET /on") >= 0) {
    sprinklerOnWebHandler(client);
  } else if (request.indexOf("GET /off") >= 0) {
    sprinklerOffWebHandler(client);
  } else if (request.indexOf("GET /") >= 0) {
    infoHandler(client);
  }

  client.stop();
}

void sprinklerOnWebHandler(WiFiClient& client) {
  if (manualOverride != 0) {
    sendJsonResponse(client, "{\"status\":\"override\"}");
    return;
  }

  setSprinklerOn();
  sendJsonResponse(client, "{\"status\":\"on\"}");
}

void sprinklerOffWebHandler(WiFiClient& client) {
  if (manualOverride != 0) {
    sendJsonResponse(client, "{\"status\":\"override\"}");
    return;
  }

  setSprinklerOff();
  sendJsonResponse(client, "{\"status\":\"off\"}");
}

static void pulseLatch(int pin) {
  digitalWrite(pin, HIGH);
  delay(12);
  digitalWrite(pin, LOW);
}

static void setSprinklerOn() {
  if (sprinklerState == ON) {
    printStringLn("sprinkler already on");
    return;
  }

  printStringLn("turning sprinkler on");
  sprinklerState = ON;
  sprinklerStart = millis();
  time_t t = myTZ.toLocal(now());
  lastRunStartTime = t;

  snprintf(sprinklerStartDisplay, sizeof(sprinklerStartDisplay),
           "Start:\n%d-%02d-%02d %02d:%02d", year(t), month(t), day(t), hour(t),
           minute(t));
  pulseLatch(TRIGGER_PIN_ON);
}

static void setSprinklerOff() {
  printStringLn("turning sprinkler off");

  if (sprinklerState == ON) {
    unsigned long duration = millis() - sprinklerStart;
    lastRunDurationMs = duration;
    sprinklerState = OFF;
    sprinklerStart = 0;

    snprintf(sprinklerDurationDisplay, sizeof(sprinklerDurationDisplay),
             "Duration:\n%.1f mins", duration / 60000.0);
  }

  pulseLatch(TRIGGER_PIN_OFF);
}

void infoHandler(WiFiClient& client) {
  printStringLn("returning info");
  if (lastRunStartTime == 0) {
    sendJsonResponse(client, "{}");
    return;
  }

  time_t t = lastRunStartTime;
  char timebuf[32];
  snprintf(timebuf, sizeof(timebuf), "%04d-%02d-%02d %02d:%02d", year(t),
           month(t), day(t), hour(t), minute(t));

  char statusBuf[9];

  if (manualOverride != 0) {
    sprintf(statusBuf, "override");
  } else if (sprinklerState == ON) {
    sprintf(statusBuf, "on");
  } else {
    sprintf(statusBuf, "off");
  }

  unsigned long duration;
  if (sprinklerState == ON) {
    duration = millis() - sprinklerStart;
  } else {
    duration = lastRunDurationMs;
  }

  char jsonbuf[128];
  snprintf(jsonbuf, sizeof(jsonbuf),
           "{\"start\":\"%s\",\"duration_ms\":%lu,\"status\":\"%s\"}", timebuf,
           duration, statusBuf);

  sendJsonResponse(client, jsonbuf);
}

void displayOnWithTimeout() { turnDisplayOff = millis() + DISPLAY_TIMEOUT; }

void printDisplay(const char* str) {
  display.clearDisplay();

  if (turnDisplayOff <= millis()) {
    display.display();
    return;
  }

  scrollDisplayText(str);
  display.display();
  display.setCursor(0, 0);
}

static void scrollDisplayText(const char* str) {
  const char* lineStart = str;
  int16_t y = 0;
  int16_t maxScroll = 0;

  while (lineStart != nullptr && y < display.height()) {
    const char* lineEnd = strchr(lineStart, '\n');
    size_t lineLength = lineEnd ? (size_t)(lineEnd - lineStart)
                                : strlen(lineStart);
    char line[64];
    if (lineLength >= sizeof(line)) {
      lineLength = sizeof(line) - 1;
    }
    memcpy(line, lineStart, lineLength);
    line[lineLength] = '\0';

    int16_t boundsX;
    int16_t boundsY;
    uint16_t textWidth;
    uint16_t textHeight;
    display.getTextBounds(line, 0, y, &boundsX, &boundsY, &textWidth,
                          &textHeight);
    if (textWidth > display.width() &&
        textWidth - display.width() > maxScroll) {
      maxScroll = textWidth - display.width();
    }

    if (!lineEnd) {
      lineStart = nullptr;
    } else {
      lineStart = lineEnd + 1;
    }
    y += 16;
  }

  const unsigned long nowMs = millis();
  if (maxScroll == 0) {
    displayScrollOffset = 0;
    displayScrollEndTime = 0;
  } else if (maxScroll != displayMaxScroll) {
    displayScrollOffset = min(displayScrollOffset, maxScroll);
    displayScrollEndTime = displayScrollOffset == maxScroll ? nowMs : 0;
  }
  displayMaxScroll = maxScroll;

  if (maxScroll > 0) {
    if (displayScrollOffset < maxScroll && nowMs - lastDisplayScroll >= 35) {
      ++displayScrollOffset;
      lastDisplayScroll = nowMs;
      if (displayScrollOffset == maxScroll) {
        displayScrollEndTime = nowMs;
      }
    } else if (displayScrollOffset == maxScroll &&
               nowMs - displayScrollEndTime >= 1000) {
      displayScrollOffset = 0;
      displayScrollEndTime = 0;
      lastDisplayScroll = nowMs;
    }
  }

  lineStart = str;
  y = 0;
  while (lineStart != nullptr && y < display.height()) {
    const char* lineEnd = strchr(lineStart, '\n');
    size_t lineLength = lineEnd ? (size_t)(lineEnd - lineStart)
                                : strlen(lineStart);
    char line[64];
    if (lineLength >= sizeof(line)) {
      lineLength = sizeof(line) - 1;
    }
    memcpy(line, lineStart, lineLength);
    line[lineLength] = '\0';

    int16_t boundsX;
    int16_t boundsY;
    uint16_t textWidth;
    uint16_t textHeight;
    display.getTextBounds(line, 0, y, &boundsX, &boundsY, &textWidth,
                          &textHeight);
    int16_t lineScroll = textWidth > display.width()
                             ? textWidth - display.width()
                             : 0;
    int16_t offset = min(displayScrollOffset, lineScroll);
    display.setCursor(-offset, y);
    display.print(line);

    if (!lineEnd) {
      lineStart = nullptr;
    } else {
      lineStart = lineEnd + 1;
    }
    y += 16;
  }
}

time_t getTime() { return ntpClient.getTime(); }

void printString(const char* str) {
  const char* p = str;
  while (*p) {
    Serial.print(*p);
    p++;
  }
}

void printStringLn(const char* str) {
  printString(str);
  Serial.print('\n');
}
