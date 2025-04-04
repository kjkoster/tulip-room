/**
 * This code was (heavily) adapted from the demo code that Shenzhen Xin Yuan
 * kindly provided as example sketch for their Liligo 4.7" epaper system.
 *
 * Unfortunately, the code is based on an older version of the ESP32S3 core
 * libraries from Expressif. It makes use of an unofficial GPIO interface,
 * which locks you into the old version. You have to downgrade your core
 * libraries to version 2.x, because using 3.x will not compile.
 *
 * @copyright Copyright (c) 2024  Shenzhen Xin Yuan Electronic Technology Co., Ltd
 * @date      2024-04-05
 * @note      Arduino Setting
 *            Tools ->
 *                  Board:"ESP32S3 Dev Module"
 *                  USB CDC On Boot:"Enable"
 *                  USB DFU On Boot:"Disable"
 *                  Flash Size : "16MB(128Mb)"
 *                  Flash Mode"QIO 80MHz
 *                  Partition Scheme:"16M Flash(3M APP/9.9MB FATFS)"
 *                  PSRAM:"OPI PSRAM"
 *                  Upload Mode:"UART0/Hardware CDC"
 *                  USB Mode:"Hardware CDC and JTAG"
 *  
 */

// XXX put constant strings as const char * for memory efficiency and type safely. Same with int/float values, actually.

#ifndef BOARD_HAS_PSRAM
#error "Please enable PSRAM, Arduino IDE -> tools -> PSRAM -> OPI !!!"
#endif

#include <WiFi.h>
#include <HTTPClient.h>
#include <Preferences.h>

#include "epd_driver.h"
#include "esp_adc_cal.h"
#include "utilities.h"

// icon files
#include "battery_low.h"
#include "room_free.h"
#include "connection_lost.h"
#include "room_booked.h"
#include "tulip_room_qr.h"

// font files
#include "QuicksandBold24.h"
#include "QuicksandRegular12.h"

// user defined constants
#define WIFI_SSID "YOUR_WIFI_NETWORK"
#define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"
#define PROXY_URL_PREFIX "YOUR_PROXY_URL_PREFIX"

// sleep time between cycles, 10 seconds in microseconds
const uint64_t SLEEP_uS = 10 * 1000000;
// when the battery is dead, we sleep for 15 minutes, hoping to get recharged.
const uint64_t SLEEP_BATTERY_DEAD_uS = 15 * 60 * 1000000;

// Persistent storage that survives deep sleep mode
Preferences persistent_storage;
const char *PERSISTENT_STORAGE = "storage";
const char *STORAGE_MESSAGE = "message";
const char *STORAGE_VOLTAGE = "voltage";

const float BATTERY_LOW = 3.4;   // volts
const float BATTERY_DEAD = 3.0;  // volts

// used for wifi, connection and read timeouts
const uint16_t TIMEOUT_MS = 5000;
const unsigned long WIFI_RECONNECT_TIMOUT_MS = 300;

const uint8_t COLOUR_WHITE = 0xff;
const uint8_t COLOUR_BLACK = 0x00;

// The example system is mounted in a wooden frame. The `VIEWPORT_*` values
// define the bounds of the visible area. The screen itself has a weird, mobile
// phone-like aspect ratio that really does not look good in a classical wooden
// frame.
//
// From the viewport, we calculate the layout of the screen elements; positions
// for the title, icon and QR code. This makes the layout dynamic and we mostly
// don't have to worry about string lengths and other details.

const int32_t VIEWPORT_X = 16;
const int32_t VIEWPORT_Y = 9;
const int32_t VIEWPORT_WIDTH = 800;
const int32_t VIEWPORT_HEIGHT = 520;
const int32_t VIEWPORT_CENTER_X = VIEWPORT_X + VIEWPORT_WIDTH / 2;
const int32_t VIEWPORT_CENTER_Y = VIEWPORT_Y + VIEWPORT_HEIGHT / 2;

const int32_t ICON_WIDTH = 300;
const int32_t H_PADDING = (VIEWPORT_WIDTH - (ICON_WIDTH * 2)) / 3;

const int32_t TITLE_HEIGHT = 48;
const int32_t ICON_HEIGHT = 300;
const int32_t V_PADDING = (VIEWPORT_HEIGHT - TITLE_HEIGHT - ICON_HEIGHT) / 2.5;

const int32_t TITLE_Y = VIEWPORT_Y + V_PADDING + TITLE_HEIGHT;
const int32_t ICON_X = VIEWPORT_X + H_PADDING;
const int32_t ICON_Y = VIEWPORT_Y + TITLE_HEIGHT + (V_PADDING * 2);
const int32_t QR_X = VIEWPORT_X + ICON_WIDTH + (H_PADDING * 2);
const int32_t QR_Y = ICON_Y;
const int32_t DEBUG_Y = VIEWPORT_Y + VIEWPORT_HEIGHT - 4;

const Rect_t icon_area = {
  .x = ICON_X,
  .y = ICON_Y,
  .width = ICON_WIDTH,
  .height = ICON_HEIGHT,
};
const Rect_t qr_area = {
  .x = QR_X,
  .y = QR_Y,
  .width = ICON_WIDTH,
  .height = ICON_HEIGHT,
};

const bool DRAW_GUIDES = false;

// We test for wifi in a very tight loop, to minimise the time needed for the
// ESP to be out of sleep more. This works nicely in most cases, but the wifi
// connection sometimes fails to connect and we end up in status 4:
// `WL_CONNECT_FAILED`. Restarting wifi negotiation does help, but not if we
// call that every 10 ms. Thus, we allow one reconnect attempt every
// `WIFI_RECONNECT_TIMOUT_MS`. This seems to give us quite decent connection
// reliability.
void await_wifi_connection() {
  const unsigned long start = millis();
  unsigned long last_reconnect = start;
  while (WiFi.status() != WL_CONNECTED && (millis() - start) < TIMEOUT_MS) {
    if (WiFi.status() == WL_CONNECT_FAILED && (millis() - last_reconnect) > WIFI_RECONNECT_TIMOUT_MS) {
      Serial.printf("wifi status %d, reconnecting...\n", WiFi.status());
      last_reconnect = millis();
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    }
    delay(10);
  }
  Serial.printf("%sconnected as %s to %s after %lums, wifi status %d, %d dBm.\n",
                (WiFi.status() == WL_CONNECTED ? "" : "not "),
                WiFi.localIP().toString().c_str(),
                WIFI_SSID, (millis() - start), WiFi.status(),
                WiFi.RSSI());
}

int fetch_room_availbility(const float battery_v, bool *busy, String *message) {
  HTTPClient http;
  http.setConnectTimeout(TIMEOUT_MS);
  const String room_url = PROXY_URL_PREFIX + WiFi.macAddress() + "?battery=" + String(battery_v);
  Serial.printf("fetching room availability from: %s.\n", room_url.c_str());

  int httpCode = -255;
  const unsigned long start = millis();
  while (httpCode != HTTP_CODE_OK && millis() < start + TIMEOUT_MS) {
    http.begin(room_url);
    http.setTimeout(TIMEOUT_MS);
    httpCode = http.GET();
    Serial.printf("room proxy HTTP code: %d.\n", httpCode);
  }

  if (httpCode == HTTP_CODE_OK) {
    const String payload = http.getString();
    Serial.printf("room proxy HTTP response: '%s'.\n", payload.c_str());

    const int firstNL = payload.indexOf('\n');
    const int secondNL = payload.indexOf('\n', firstNL + 1);
    if (firstNL != -1 && secondNL != -1) {
      *busy = (payload.substring(firstNL + 1, secondNL) == "busy");
      *message = payload.substring(0, firstNL) + ", " + payload.substring(secondNL + 1);
      message->trim();
    } else {
      *message = "bad response '" + payload + "'";
    }
  } else {
    *message = "http " + String(httpCode) + " " + http.errorToString(httpCode);
  }
  return httpCode;
}

// Note well: measuring battery power will only work when the epaper display is
// powered up.
float measure_battery_voltage() {
  // XXX review implementation and redo it? Also: move it into measuring the battery stats...
  // Correct the ADC reference voltage
  esp_adc_cal_characteristics_t adc_chars;
  esp_adc_cal_value_t val_type = esp_adc_cal_characterize(
    ADC_UNIT_2,
    ADC_ATTEN_DB_12,
    ADC_WIDTH_BIT_12,
    1100,
    &adc_chars);
  Serial.printf("--- val_type: %d.\n", val_type);  // XXX

  uint32_t vref = 1100;
  if (val_type == ESP_ADC_CAL_VAL_EFUSE_VREF) {
    Serial.printf("eFuse Vref: %umV\r\n", adc_chars.vref);
    vref = adc_chars.vref;
  }
  Serial.printf("ADC reference voltage: %umV.\n", vref);

  delay(10);  // Make adc measurement slightly more accurate
  const uint16_t raw = analogRead(BATT_PIN);
  const float v = (((float)raw / 4095.0) * 2.0 * 3.3 * ((float)vref / 1000.0)) - 0.37;
  Serial.printf("battery voltage: %.2fV.\n", v);

  return v;
}

// Draw a centered text at a given y-position
void draw_centered_text(const String text, const GFXfont *font, const int32_t y, uint8_t *framebuffer) {
  int32_t cursor_x = 0;
  int32_t cursor_y = y;
  int32_t bounds_x, bounds_y, bounds_w, bounds_h;
  get_text_bounds(font, (char *)text.c_str(), &cursor_x, &cursor_y, &bounds_x, &bounds_y, &bounds_w, &bounds_h, NULL);

  cursor_x = VIEWPORT_CENTER_X - (bounds_w / 2);
  cursor_y = y;
  if (DRAW_GUIDES) {
    epd_draw_rect(cursor_x, y - bounds_h, bounds_w, bounds_h, COLOUR_BLACK, framebuffer);
  }
  writeln(font, (char *)text.c_str(), &cursor_x, &cursor_y, framebuffer);
}

void draw_guides(uint8_t *framebuffer) {
  // Big cross over the whole viewport, so we can see that the edges are tucked in properly
  epd_draw_circle(VIEWPORT_X, VIEWPORT_Y, 6, COLOUR_BLACK, framebuffer);
  epd_draw_circle(VIEWPORT_X, VIEWPORT_Y, 12, COLOUR_BLACK, framebuffer);
  epd_draw_circle(VIEWPORT_X, VIEWPORT_Y + VIEWPORT_HEIGHT, 6, COLOUR_BLACK, framebuffer);
  epd_draw_circle(VIEWPORT_X, VIEWPORT_Y + VIEWPORT_HEIGHT, 12, COLOUR_BLACK, framebuffer);
  epd_draw_circle(VIEWPORT_X + VIEWPORT_WIDTH, VIEWPORT_Y, 6, COLOUR_BLACK, framebuffer);
  epd_draw_circle(VIEWPORT_X + VIEWPORT_WIDTH, VIEWPORT_Y, 12, COLOUR_BLACK, framebuffer);
  epd_draw_circle(VIEWPORT_X + VIEWPORT_WIDTH, VIEWPORT_Y + VIEWPORT_HEIGHT, 6, COLOUR_BLACK, framebuffer);
  epd_draw_circle(VIEWPORT_X + VIEWPORT_WIDTH, VIEWPORT_Y + VIEWPORT_HEIGHT, 12, COLOUR_BLACK, framebuffer);
  epd_draw_line(VIEWPORT_X, VIEWPORT_Y, VIEWPORT_X + VIEWPORT_WIDTH, VIEWPORT_Y + VIEWPORT_HEIGHT, COLOUR_BLACK, framebuffer);
  epd_draw_line(VIEWPORT_X, VIEWPORT_Y + VIEWPORT_HEIGHT, VIEWPORT_X + VIEWPORT_WIDTH, VIEWPORT_Y, COLOUR_BLACK, framebuffer);

  // vertical and horizontal cross, for centering
  epd_draw_circle(VIEWPORT_CENTER_X, VIEWPORT_CENTER_Y, 6, COLOUR_BLACK, framebuffer);
  epd_draw_circle(VIEWPORT_CENTER_X, VIEWPORT_CENTER_Y, 12, COLOUR_BLACK, framebuffer);

  epd_draw_circle(VIEWPORT_CENTER_X, VIEWPORT_Y, 6, COLOUR_BLACK, framebuffer);
  epd_draw_circle(VIEWPORT_CENTER_X, VIEWPORT_Y, 12, COLOUR_BLACK, framebuffer);
  epd_draw_circle(VIEWPORT_CENTER_X, VIEWPORT_Y + VIEWPORT_HEIGHT, 6, COLOUR_BLACK, framebuffer);
  epd_draw_circle(VIEWPORT_CENTER_X, VIEWPORT_Y + VIEWPORT_HEIGHT, 12, COLOUR_BLACK, framebuffer);
  epd_draw_line(VIEWPORT_CENTER_X, VIEWPORT_Y, VIEWPORT_CENTER_X, VIEWPORT_Y + VIEWPORT_HEIGHT, COLOUR_BLACK, framebuffer);

  epd_draw_circle(VIEWPORT_X, VIEWPORT_CENTER_Y, 6, COLOUR_BLACK, framebuffer);
  epd_draw_circle(VIEWPORT_X, VIEWPORT_CENTER_Y, 12, COLOUR_BLACK, framebuffer);
  epd_draw_circle(VIEWPORT_X + VIEWPORT_WIDTH, VIEWPORT_CENTER_Y, 6, COLOUR_BLACK, framebuffer);
  epd_draw_circle(VIEWPORT_X + VIEWPORT_WIDTH, VIEWPORT_CENTER_Y, 12, COLOUR_BLACK, framebuffer);
  epd_draw_line(VIEWPORT_X, VIEWPORT_CENTER_Y, VIEWPORT_X + VIEWPORT_WIDTH, VIEWPORT_CENTER_Y, COLOUR_BLACK, framebuffer);

  // reference line for the title
  epd_draw_line(VIEWPORT_X, TITLE_Y, VIEWPORT_X + VIEWPORT_WIDTH, TITLE_Y, COLOUR_BLACK, framebuffer);

  // icon and qr code corners
  epd_fill_circle(ICON_X, ICON_Y, 12, COLOUR_BLACK, framebuffer);
  epd_fill_circle(ICON_X + ICON_WIDTH, ICON_Y, 12, COLOUR_BLACK, framebuffer);
  epd_fill_circle(ICON_X, ICON_Y + ICON_HEIGHT, 12, COLOUR_BLACK, framebuffer);
  epd_fill_circle(ICON_X + ICON_WIDTH, QR_Y + ICON_HEIGHT, 12, COLOUR_BLACK, framebuffer);
  epd_fill_circle(QR_X, QR_Y, 12, COLOUR_BLACK, framebuffer);
  epd_fill_circle(QR_X + ICON_WIDTH, QR_Y, 12, COLOUR_BLACK, framebuffer);
  epd_fill_circle(QR_X, QR_Y + ICON_HEIGHT, 12, COLOUR_BLACK, framebuffer);
  epd_fill_circle(QR_X + ICON_WIDTH, QR_Y + ICON_HEIGHT, 12, COLOUR_BLACK, framebuffer);
}

void full_screen_update(const String message, const uint8_t *icon_data, const uint8_t *qr_data,
                        const bool show_network_details, const esp_sleep_wakeup_cause_t wakeup_reason, const float battery_voltage) {
  uint8_t *framebuffer = (uint8_t *)ps_calloc(sizeof(uint8_t), EPD_WIDTH * EPD_HEIGHT / 2);
  if (!framebuffer) {
    Serial.println("framebuffer memory allocation failed, halting");
    while (1)
      ;
  }
  // start with a fresh image buffer
  memset(framebuffer, COLOUR_WHITE, EPD_WIDTH * EPD_HEIGHT / 2);
  // clear any ghosting
  epd_clear();

  if (DRAW_GUIDES) {
    draw_guides(framebuffer);
  }

  draw_centered_text(message, &QuicksandBold24, TITLE_Y, framebuffer);
  epd_copy_to_framebuffer(icon_area, (uint8_t *)icon_data, framebuffer);
  epd_copy_to_framebuffer(qr_area, (uint8_t *)qr_data, framebuffer);

  if (show_network_details) {
    const String network_details = "boot " + String(wakeup_reason) + " - wifi "
                                   + String(WiFi.status()) + " "
                                   + String(WiFi.RSSI()) + " dBm - "
                                   + WiFi.macAddress() + " - "
                                   + WiFi.localIP().toString() + " - "
                                   + String(battery_voltage) + "V";
    draw_centered_text(network_details, &QuicksandRegular12, DEBUG_Y, framebuffer);
  }

  // send the framebuffer to the screen
  epd_draw_grayscale_image(epd_full_screen(), framebuffer);
}

void setup() {
  // give the wireless hardware a head start
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.begin(115200);

  persistent_storage.begin(PERSISTENT_STORAGE, true);
  // remember what message that is actually on the screen. If that does not
  // change, there is no real need to update the screen. That saves power and
  // reduces flicker.
  String current_message = persistent_storage.getString(STORAGE_MESSAGE, "");
  // remember the voltage we measured, because we do not measure voltage on
  // every cycle.
  float battery_voltage = persistent_storage.getFloat(STORAGE_VOLTAGE, 5.0);
  persistent_storage.end();

  const esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();
  if (wakeup_reason == ESP_SLEEP_WAKEUP_TIMER) {
    Serial.println("woke up from timer.");
  } else {
    Serial.printf("woke up from power-on or other cause: %d.\n", wakeup_reason);
    current_message = "";  // force screen refresh
  }

  // We need an icon, a QR code and a message to draw everything on the screen.
  // The icon may be overridden to show the low battery status. See below.
  uint8_t *icon_data;
  String message;

  await_wifi_connection();
  if (WiFi.status() != WL_CONNECTED) {
    // no need to reconnect, we are going to sleep anyway
    icon_data = (uint8_t *)connection_lost_data;
    message = "no wifi connection";
  } else {
    bool busy;
    const int httpCode = fetch_room_availbility(battery_voltage, &busy, &message);

    if (httpCode != HTTP_CODE_OK) {
      icon_data = (uint8_t *)connection_lost_data;
      message = message;
    } else {
      if (busy) {
        icon_data = (uint8_t *)room_booked_data;
        message = message;
      } else {
        icon_data = (uint8_t *)room_free_data;
        message = message;
      }
    }
  }

  if (current_message != message) {
    Serial.printf("message change '%s' -> '%s'.\n", current_message.c_str(), message.c_str());
    current_message = message;

    epd_init();
    epd_poweron();

    // yes, we read the battery voltage only when we power on the screen. This
    // is so we don't have to power on the screen every cycle, but only when
    // something changed. As a side effect, we will not read the battery
    // voltage every cycle and readings are really only used in the next cycle.
    // Not perfect, but more battery efficient.
    //
    // If we no do not check the room calendar, we no do not get booking
    // updates. Without booking updates we never refresh the screen, which in
    // turn means we never measure the battery voltage to return from
    // low-battery mode. To avoid getting stuck in low-battery state, we
    // override only the icon in low battery mode, so that normal changes in
    // room availability will still trigger screen refreshes and thus battery
    // voltage measurements.
    battery_voltage = measure_battery_voltage();
    if (battery_voltage < BATTERY_LOW) {
      icon_data = (uint8_t *)battery_low_data;
    }
    if (battery_voltage < BATTERY_DEAD) {
      message = "battery critical, please recharge";
    }

    const bool show_network_details = wakeup_reason != ESP_SLEEP_WAKEUP_TIMER
                                      || WiFi.status() != WL_CONNECTED
                                      || battery_voltage < BATTERY_LOW;
    full_screen_update(message, icon_data, tulip_room_qr_data,
                       show_network_details, wakeup_reason, battery_voltage);

    epd_poweroff_all();

    persistent_storage.begin(PERSISTENT_STORAGE, false);
    persistent_storage.putString(STORAGE_MESSAGE, message);
    persistent_storage.putFloat(STORAGE_VOLTAGE, battery_voltage);
    persistent_storage.end();
  } else {
    Serial.println("no change.");
  }

  esp_sleep_enable_timer_wakeup(battery_voltage < BATTERY_DEAD ? SLEEP_BATTERY_DEAD_uS : SLEEP_uS);
  esp_deep_sleep_start();
}

void loop() {
  /* not used */
}
