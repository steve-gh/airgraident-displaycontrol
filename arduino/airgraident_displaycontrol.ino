/**
 * This sketch connects an AirGradient DIY Pro sensor to a WiFi network
 * with an HTTP server to provide prometheus-style metrics and to allow the
 * display OLED display to be programmed remotely.
 *
 * Urls:
 * GET /metrics - prometheus data
 * POST /setscreen2 - upload data for the 2nd screen. Vars:
 *   watt / wattavg / water / watertoday / garage / garagetime - values for the 2nd screen
 *   oledInterval - the ms between oled updates
 * POST /uploadscreen - upload additional screens. Vars:
 *   d0 ... d19 - up to 20 different screens, XBM format / 1024 chars, each bit is a pixel
 *   Recommend 1 screen updated at a time to keep byte count low.
 *
 * This work is based heavily on:
 *  Airgradient code: https://github.com/airgradienthq/arduino/blob/master/examples/DIY_PRO_V4_2/
 *  Jeff's airgraident-prometheus: https://github.com/geerlingguy/airgradient-prometheus
 *
 * Written for a AirGradient DIY PRO 4.2 with a SGP41 TVOC module.
 */

#include <AirGradient.h>
#include <ESP8266WebServer.h>
#include <ESP8266WiFi.h>

#include <NOxGasIndexAlgorithm.h>
#include <SensirionI2CSgp41.h>
#include <VOCGasIndexAlgorithm.h>

#include <U8g2lib.h>
#include <WiFiClient.h>

AirGradient ag = AirGradient();

SensirionI2CSgp41 sgp41;
NOxGasIndexAlgorithm nox_algorithm;
VOCGasIndexAlgorithm voc_algorithm;
// time in seconds needed for NOx conditioning
uint16_t conditioning_s = 10;

// Display bottom right
U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, /* reset=*/U8X8_PIN_NONE);

// Replace above if you have display on top left
// U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R2, /* reset=*/ U8X8_PIN_NONE);

// Config ----------------------------------------------------------------------

// Optional.
const char *deviceId = "";

// Hardware options for AirGradient DIY sensor.
const bool hasPM = true;
const bool hasCO2 = true;
const bool hasSHT = true;
const bool hasSGP41 = true;

// WiFi and IP connection info.
const char *ssid = "";
const char *password = "";
const int port = 9926;

// Uncomment the line below to configure a static IP address.
// #define staticip
#ifdef staticip
IPAddress static_ip(192, 168, 0, 0);
IPAddress gateway(192, 168, 0, 0);
IPAddress subnet(255, 255, 255, 0);
#endif

// Max age in ms that a custom display will show up without changes
const int customDisplayMaxAge = 60 * 60 * 1000; // 1 hour

// Constants for the Arduino-built screens
// Width of the text characters
#define WIDTH_TEXT 6
// Width of the numbers
#define WIDTH_NUM 11
// Width of the last row
#define WIDTH_GARAGE 9
// Additional offset for the text to give it a bit more space
#define TEXT_OFFSET 2
// 3 rows - the start of each row
#define ROW1_Y 21
#define ROW2_Y 42
#define ROW3_Y 63

// Config End ------------------------------------------------------------------

unsigned long currentMillis = 0;
unsigned long display_last_set = 0;

// not const, this can be overridden via console.
unsigned int oledInterval = 5000;
unsigned long previousOled = 0;

const int tvocInterval = 1000;
unsigned long previousTVOC = 0;
int TVOC = 0;
int NOX = 0;

const int co2Interval = 5000;
unsigned long previousCo2 = 0;
int Co2 = 0;

const int pm25Interval = 5000;
unsigned long previousPm25 = 0;
int pm25 = 0;

const int tempHumInterval = 2500;
unsigned long previousTempHum = 0;
float tempc = 0;
float tempf = 0;
int hum = 0;

// PM2.5 in US AQI (default ug/m3)
boolean inUSAQI = true;

// Fields set by an HTTP client
String custom_watt = "";
String custom_wattavg = "";
String custom_water = "";
String custom_watertoday = "";
String custom_garage = "";
String custom_garagetime = "";

ESP8266WebServer server(port);

// Size of the OLED display
#define OLED_WIDTH 128
#define OLED_HEIGHT 64

// OLED is 16-bytes (128 bits) x 64 rows = 1024 bytes
#define DISPLAY_IMG_BYTE_LEN 1024

struct display_image {
  uint8_t display[DISPLAY_IMG_BYTE_LEN];
  bool active;
  long last_updated;
};

// Max number of images to store
#define MAX_DISPLAY_IMG 20
struct display_image dimg[MAX_DISPLAY_IMG];
// which custom display is active
int displayActive = 0;
// 0 = display summary. 1 = displayActive. 2 = summary #2 (if does not exist, go
// back to 0).  3 = displayActive
int displayCycle = 0;

void updateTVOC() {
  uint16_t error;
  char errorMessage[256];
  uint16_t defaultRh = 0x8000;
  uint16_t defaultT = 0x6666;
  uint16_t srawVoc = 0;
  uint16_t srawNox = 0;
  uint16_t defaultCompenstaionRh = 0x8000; // in ticks as defined by SGP41
  uint16_t defaultCompenstaionT = 0x6666;  // in ticks as defined by SGP41
  uint16_t compensationRh = 0;             // in ticks as defined by SGP41
  uint16_t compensationT = 0;              // in ticks as defined by SGP41

  // Per sgp41_i2c.h
  // relative humidity in ticks (ticks = %RH * 65535 / 100)
  // temperature in ticks (ticks = (degC + 45) * 65535 / 175)
  compensationT = static_cast<uint16_t>((tempc + 45) * 65535 / 175);
  compensationRh = static_cast<uint16_t>(hum * 65535 / 100);

  if (currentMillis - previousTVOC >= tvocInterval) {
    previousTVOC += tvocInterval;
    if (conditioning_s > 0) {
      Serial.print("TVOC Conditioning: " + conditioning_s);
      error = sgp41.executeConditioning(compensationRh, compensationT, srawVoc);
      if (error) {
        Serial.print("Error trying to execute executeConditioning(): ");
        errorToString(error, errorMessage, 256);
        Serial.println(errorMessage);
      }
      conditioning_s--;
      return;
    }
    error = sgp41.measureRawSignals(compensationRh, compensationT, srawVoc,
                                    srawNox);
    if (error) {
      Serial.print("Error trying to execute measureRawSignals(): ");
      errorToString(error, errorMessage, 256);
      Serial.println(errorMessage);
    } else {
      TVOC = voc_algorithm.process(srawVoc);
      NOX = nox_algorithm.process(srawNox);
    }
  }
}

void updateCo2() {
  if (currentMillis - previousCo2 >= co2Interval) {
    previousCo2 += co2Interval;
    Co2 = ag.getCO2_Raw();
  }
}

void updatePm25() {
  if (currentMillis - previousPm25 >= pm25Interval) {
    previousPm25 += pm25Interval;
    pm25 = ag.getPM2_Raw();
  }
}

void updateTempHum() {
  if (currentMillis - previousTempHum >= tempHumInterval) {
    previousTempHum += tempHumInterval;
    TMP_RH result = ag.periodicFetchData();
    tempc = result.t;
    // convert from Celcius to Fahrenheit
    tempf = (tempc * 9 / 5) + 32;
    hum = result.rh;
  }
}

void drawScreen1() {
  String s_aqi = String(PM_TO_AQI_US(pm25));
  String s_co2 = String(Co2);
  String s_tvoc = String(TVOC);
  String s_nox = String(NOX);
  String s_tempf = String(tempf, 1);
  String s_hum = String(hum) + "%";

  u8g2.firstPage();
  do {
    int aqi_x = 60 - (WIDTH_TEXT * 3);
    int co2_x = 128 - (WIDTH_TEXT * 3);
    int tvoc_x = 4 + 64 - (WIDTH_TEXT * 4); // add a bit more offset
    int nox_x = 128 - (WIDTH_TEXT * 3);
    int f_x = 64 - (WIDTH_TEXT * 2); // add a bit more offset
    int h_x = 128 - (WIDTH_TEXT * 3);
    u8g2.setFont(u8g2_font_t0_11_tf);
    u8g2.drawStr(aqi_x, ROW1_Y, "AQI");
    u8g2.drawStr(co2_x, ROW1_Y, "CO2");

    u8g2.drawStr(tvoc_x, ROW2_Y, "TVoC");
    u8g2.drawStr(nox_x, ROW2_Y, "NOX");

    u8g2.drawStr(f_x - 4, ROW3_Y - 8, "o");
    u8g2.drawStr(f_x, ROW3_Y, "F");
    u8g2.drawStr(h_x, ROW3_Y, "H2O");

    u8g2.setFont(u8g2_font_t0_22b_tr);

    u8g2.drawStr(aqi_x - TEXT_OFFSET - (WIDTH_NUM * s_aqi.length()), ROW1_Y,
                 s_aqi.c_str());
    u8g2.drawStr(co2_x - TEXT_OFFSET - (WIDTH_NUM * s_co2.length()), ROW1_Y,
                 s_co2.c_str());
    u8g2.drawStr(tvoc_x - TEXT_OFFSET - (WIDTH_NUM * s_tvoc.length()), ROW2_Y,
                 s_tvoc.c_str());
    u8g2.drawStr(nox_x - TEXT_OFFSET - (WIDTH_NUM * s_nox.length()), ROW2_Y,
                 s_nox.c_str());
    u8g2.drawStr(f_x - TEXT_OFFSET - 4 - (WIDTH_NUM * s_tempf.length()), ROW3_Y,
                 s_tempf.c_str());
    u8g2.drawStr(h_x - TEXT_OFFSET - (WIDTH_NUM * s_hum.length()), ROW3_Y,
                 s_hum.c_str());

  } while (u8g2.nextPage());
}

/* Draw a custom screen provided remotely (via http) */
void drawCustomScreen(int index) {
  u8g2.firstPage();
  do {
    u8g2.drawXBM(0, 0, OLED_WIDTH, OLED_HEIGHT, dimg[index].display);
  } while (u8g2.nextPage());
}

void drawScreen2() {
  int watt_x = 64 - (WIDTH_TEXT * 4);
  int wattavg_x = 128 - (WIDTH_TEXT * 3);
  int water_x = 64 - (WIDTH_TEXT * 4);
  int watertoday_x = 128 - (WIDTH_TEXT * 3);
  int garage_x = (WIDTH_TEXT * 7);
  int garagetime_x = 128 - (WIDTH_TEXT * 4);
  u8g2.firstPage();
  do {
    u8g2.setFont(u8g2_font_t0_11_tf);
    u8g2.drawStr(watt_x, ROW1_Y, "Watt");
    u8g2.drawStr(wattavg_x, ROW1_Y, "Avg");

    u8g2.drawStr(water_x, ROW2_Y, "Gal/H");
    u8g2.drawStr(watertoday_x, ROW2_Y, "TDY");

    u8g2.drawStr(0, ROW3_Y, "Garage");

    u8g2.setFont(u8g2_font_t0_22b_tr);

    u8g2.drawStr(max((int)-2, (int)(watt_x - TEXT_OFFSET -
                                    (WIDTH_NUM * custom_watt.length()))),
                 ROW1_Y, custom_watt.c_str());
    u8g2.drawStr(wattavg_x - TEXT_OFFSET -
                     (WIDTH_NUM * custom_wattavg.length()),
                 ROW1_Y, custom_wattavg.c_str());
    u8g2.drawStr(max((int)-2, (int)(water_x - TEXT_OFFSET -
                                    (WIDTH_NUM * custom_water.length()))),
                 ROW2_Y, custom_water.c_str());
    u8g2.drawStr(watertoday_x - TEXT_OFFSET -
                     (WIDTH_NUM * custom_watertoday.length()),
                 ROW2_Y, custom_watertoday.c_str());
    u8g2.setFont(u8g2_font_t0_18b_tr);
    u8g2.drawStr(garage_x + TEXT_OFFSET, ROW3_Y, custom_garage.c_str());
    u8g2.setFont(u8g2_font_t0_11_tf);
    u8g2.drawStr(garage_x + 2 + TEXT_OFFSET +
                     (WIDTH_GARAGE * custom_garage.length()),
                 ROW3_Y, custom_garagetime.c_str());

  } while (u8g2.nextPage());
}

/* Update the OLED display */
void updateOLED() {
  if (currentMillis - previousOled >= oledInterval) {
    previousOled += oledInterval;
    displayCycle = (displayCycle + 1) % 4;
    Serial.println("Updating Screen: " + String(displayCycle));
    // 0 = display summary. 1 = displayActive. 2 = summary #2 (if does not
    // exist, go back to 0).  3 = displayActive
    if (displayCycle == 1 || displayCycle == 3) {
      bool drew_screen = false;
      for (int i = 0; i < MAX_DISPLAY_IMG; i++) {
        displayActive = (displayActive + 1) % MAX_DISPLAY_IMG;
        if (dimg[displayActive].active &&
            (currentMillis - dimg[displayActive].last_updated <
             customDisplayMaxAge)) {
          drawCustomScreen(displayActive);
          drew_screen = true;
          break;
        }
      }
      if (!drew_screen) {
        // no valid screens, go to the next screen
        displayCycle = (displayCycle + 1) % 4;
      }
    }
    if (displayCycle == 2) {
      if ((display_last_set > 0) &&
          (currentMillis - -display_last_set < customDisplayMaxAge)) {
        drawScreen2();
      } else {
        displayCycle = 0; // no screen,  so jump back to 0.
      }
    }
    if (displayCycle == 0) {
      drawScreen1();
    }
  }
}

/* Update the OLED with 3 rows of text */
void updateOLED2(String ln1, String ln2, String ln3) {
  // char buf[9];
  u8g2.firstPage();
  // u8g2.firstPage();
  do {
    u8g2.setFont(u8g2_font_t0_16_tf);
    u8g2.drawStr(1, 10, String(ln1).c_str());
    u8g2.drawStr(1, 30, String(ln2).c_str());
    u8g2.drawStr(1, 50, String(ln3).c_str());
  } while (u8g2.nextPage());
}

void setup() {
  Serial.begin(115200);
  Serial.println("Hello");
  u8g2.setBusClock(100000);
  u8g2.begin();

  // Init Display.
  updateOLED2("Warming up the", "sensors.", "");

  // Enable enabled sensors.
  if (hasSGP41)
    sgp41.begin(Wire);
  if (hasPM)
    ag.PMS_Init();
  if (hasCO2)
    ag.CO2_Init();
  if (hasSHT)
    ag.TMP_RH_Init(0x44);

// Set static IP address if configured.
#ifdef staticip
  WiFi.config(static_ip, gateway, subnet);
#endif

  // Set WiFi mode to client (without this it may try to act as an AP).
  WiFi.mode(WIFI_STA);

  // Configure Hostname
  if ((deviceId != NULL) && (deviceId[0] == '\0')) {
    Serial.printf("No Device ID is Defined, Defaulting to board defaults");
  } else {
    wifi_station_set_hostname(deviceId);
    WiFi.setHostname(deviceId);
  }

  // Setup and wait for WiFi.
  WiFi.begin(ssid, password);
  Serial.println("");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    updateOLED2("Trying to", "connect to:", ssid);
    Serial.print(".");
  }

  Serial.println("");
  Serial.print("Connected to ");
  Serial.println(ssid);
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());
  Serial.print("MAC address: ");
  Serial.println(WiFi.macAddress());
  Serial.print("Hostname: ");
  Serial.println(WiFi.hostname());
  server.on("/", HTTP_GET, HandleRoot);
  server.on("/metrics", HTTP_GET, HandleRoot);
  server.on("/setscreen2", HTTP_POST, HandleDisplaySet);
  server.on("/uploadscreen", HTTP_POST, HandleDisplayUpload);
  server.onNotFound(HandleNotFound);

  server.begin();
  Serial.println("HTTP server started at ip " + WiFi.localIP().toString() +
                 ":" + String(port));
  updateOLED2("Listening To", WiFi.localIP().toString() + ":" + String(port),
              "");
  delay(1000);
}

void loop() {
  currentMillis = millis();
  updateTVOC();
  updateCo2();
  updatePm25();
  updateTempHum();
  updateOLED();
  server.handleClient();
}

String GenerateMetrics() {
  String message = "";
  String idString = "{id=\"" + String(deviceId) + "\",mac=\"" +
                    WiFi.macAddress().c_str() + "\"} ";

  if (hasPM && pm25 >= 0) {
    message += "# HELP aqi Particulate Matter AQI value\n";
    message += "# TYPE aqi gauge\n";
    message += "aqi";
    message += idString;
    message += String(PM_TO_AQI_US(pm25));
    message += "\n";

    message += "# HELP pm025 Particulate Matter PM2.5 value\n";
    message += "# TYPE pm025 gauge\n";
    message += "pm025";
    message += idString;
    message += String(pm25);
    message += "\n";
  }

  if (hasSGP41 && NOX >= 0) {
    message += "# HELP nox Nitrogen Oxides (NOx) value\n";
    message += "# TYPE nox gauge\n";
    message += "nox";
    message += idString;
    message += String(NOX);
    message += "\n";
  }

  if (hasSGP41 && TVOC >= 0) {
    message += "# HELP tvoc Total volatile organic compounds (TVOC) value\n";
    message += "# TYPE tvoc gauge\n";
    message += "tvoc";
    message += idString;
    message += String(TVOC);
    message += "\n";
  }

  if (hasCO2 && Co2 >= 0) {
    message += "# HELP rco2 CO2 value, in ppm\n";
    message += "# TYPE rco2 gauge\n";
    message += "rco2";
    message += idString;
    message += String(Co2);
    message += "\n";
  }

  if (hasSHT && tempf >= 0) {
    message += "# HELP atmp Temperature, in degrees Fahrenheit\n";
    message += "# TYPE atmp gauge\n";
    message += "atmp";
    message += idString;
    message += String(tempf);
    message += "\n";
  }

  if (hasSHT && hum >= 0) {
    message += "# HELP rhum Relative humidity, in percent\n";
    message += "# TYPE rhum gauge\n";
    message += "rhum";
    message += idString;
    message += String(hum);
    message += "\n";
  }

  // provide current uptime, validates system isn't rebooting constantly
  message += "# HELP uptimesec Seconds since boot\n";
  message += "# TYPE uptimesec gauge\n";
  message += "uptimesec";
  message += idString;
  message += String(currentMillis / 1000);
  message += "\n";

  return message;
}

void HandleDisplayUpload() {
  String retval = "Screen Update Results:\n";
  String argVal;
  for (int i = 0; i < MAX_DISPLAY_IMG; i++) {
    String argName = "d" + String(i);
    if (!server.hasArg(argName)) {
      continue;
    }
    if (server.arg(argName).length() == 0) {
      dimg[i].active = false;
      retval += "SUCCESS: " + argName + " emptied.\n";
    } else if (server.arg(argName).length() != DISPLAY_IMG_BYTE_LEN) {
      retval += "ERROR: " + argName + " invalid length.\n";
      dimg[i].active = false;
    } else {
      String argVal = server.arg(argName);
      memcpy(dimg[i].display, argVal.c_str(), DISPLAY_IMG_BYTE_LEN);
      dimg[i].active = true;
      dimg[i].last_updated = currentMillis;
      retval += "SUCCESS: " + argName + " updated.\n";
    }
  }
  server.send(200, "text/html", retval);
}

void HandleDisplaySet() {
  display_last_set = currentMillis;
  if (server.hasArg("watt")) {
    custom_watt = server.arg("watt");
  }
  if (server.hasArg("wattavg")) {
    custom_wattavg = server.arg("wattavg");
  }
  if (server.hasArg("water")) {
    custom_water = server.arg("water");
  }
  if (server.hasArg("watertoday")) {
    custom_watertoday = server.arg("watertoday");
  }
  if (server.hasArg("garage")) {
    custom_garage = server.arg("garage");
  }
  if (server.hasArg("garagetime")) {
    custom_garagetime = server.arg("garagetime");
  }
  if (server.hasArg("oledInterval")) {
    oledInterval = atoi(server.arg("oledInterval").c_str());
  }
  server.send(200, "text/html", "Thank you.\n");
}

void HandleRoot() { server.send(200, "text/plain", GenerateMetrics()); }

void HandleNotFound() {
  String message = "File Not Found\n\n";
  message += "URI: ";
  message += server.uri();
  message += "\nMethod: ";
  message += (server.method() == HTTP_GET) ? "GET" : "POST";
  message += "\nArguments: ";
  message += server.args();
  message += "\n";
  for (uint i = 0; i < server.args(); i++) {
    message += " " + server.argName(i) + ": " + server.arg(i) + "\n";
  }
  server.send(404, "text/html", message);
}

// Calculate PM2.5 US AQI
int PM_TO_AQI_US(int pm) {
  /*                                  AQI         RAW PM2.5
    Good                               0 - 50   |   0.0 – 12.0
    Moderate                          51 - 100  |  12.1 – 35.4
    Unhealthy for Sensitive Groups   101 – 150  |  35.5 – 55.4
    Unhealthy                        151 – 200  |  55.5 – 150.4
    Very Unhealthy                   201 – 300  |  150.5 – 250.4
    Hazardous                        301 – 400  |  250.5 – 350.4
    Hazardous                        401 – 500  |  350.5 – 500.4
    */
  if (pm < 0)
    return pm;
  if (pm > 1000) {
    return 500;
  }
  if (pm > 350.5) {
    return calcAQI(pm, 500, 401, 500.4, 350.5); // Hazardous
  } else if (pm > 250.5) {
    return calcAQI(pm, 400, 301, 350.4, 250.5); // Hazardous
  } else if (pm > 150.5) {
    return calcAQI(pm, 300, 201, 250.4, 150.5); // Very Unhealthy
  } else if (pm > 55.5) {
    return calcAQI(pm, 200, 151, 150.4, 55.5); // Unhealthy
  } else if (pm > 35.5) {
    return calcAQI(pm, 150, 101, 55.4, 35.5); // Unhealthy for Sensitive Groups
  } else if (pm > 12.1) {
    return calcAQI(pm, 100, 51, 35.4, 12.1); // Moderate
  } else {
    return calcAQI(pm, 50, 0, 12, 0); // Good
  }
}

int calcAQI(int Cp, int Ih, int Il, float BPh, float BPl) {
  float a = (Ih - Il);
  float b = (BPh - BPl);
  float c = (Cp - BPl);
  return int(round(a / b) * c + Il);
}
