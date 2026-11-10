#ifndef main_h_
#define main_h_

#include <Arduino.h>
#include <TimeLib.h>
#include <WiFiNINA.h>

#define TRIGGER_PIN_ON 8  // 24V relay pulse, Nano 33 IoT-safe digital output
#define TRIGGER_PIN_OFF 9 // 24V relay pulse, Nano 33 IoT-safe digital output

#define BUTTON_A 2
#define BUTTON_B 3
#define BUTTON_C 4

const int CHECK_TIME_DELAY_MS = 5 * 60 * 1000; // 5 mins
const int CHECK_SERVER_DELAY = 500;
const int SPRINKLER_ON_MAX = 60 * 60000; // 1 hour
const int DISPLAY_TIMEOUT = 60 * 1000; // 1 min
const int LONG_PRESS = 1000; // 1s
const int MANUAL_OVERRIDE_30 = 30 * 60 * 1000; // 30 mins
const int MANUAL_OVERRIDE_60 = 60 * 60 * 1000; // 60 mins

// Helpers
void sprinklerOnWebHandler(WiFiClient& client);
void sprinklerOffWebHandler(WiFiClient& client);
void sprinklerOnButtonHandler(bool longPress);
void sprinklerOffButtonHandler();
void infoHandler(WiFiClient& client);
void toggleOverride();
time_t getTime();
void buttonAPressed(bool);
void buttonBPressed(bool);
void buttonCPressed(bool);
void displayOnWithTimeout();
void printDisplay(const char *str);
void printString(const char *str);
void printStringLn(const char *str);

#endif
