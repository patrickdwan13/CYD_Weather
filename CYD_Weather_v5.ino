// This is a Weather program for the ESP32-3248S035 CYD
//
#include <TFT_eSPI.h>     // Graphics library for the display (ST7796)
#include <TAMC_GT911.h>   // Library for the capacitive touch controller (GT911)
#include <WiFi.h>         // Library to connect over WiFi
#include "time.h"         // ESP32 time library
#include <HTTPClient.h>   // Library to read data from web API
#include <ArduinoJson.h>  // Library to extract data received from web API
#include "secret.h"       // Library with WiFi credentials and GPS location data

// DISPLAY AND TOUCH OBJECTS ---
TFT_eSPI tft = TFT_eSPI();
TAMC_GT911 ts = TAMC_GT911(33, 32, 21, 25, 480, 320);

// Screen dimensions (will be swapped to Landscape if rotation is 1 or 3)
#define SCREEN_W TFT_WIDTH
#define SCREEN_H TFT_HEIGHT

// --- Screen State ---
const int NAV_HEIGHT = 30; // Height for navigation buttons at the bottom
const int STATUS_HEIGHT = 20; // Height for status bar at the top

// --- TOUCH PARAMETERS ---
unsigned long lastTouchTime = 0;
bool prevTouched = false;

// --- BACKLIGHT CONTROL PARAMETERS ---
#define BL_PIN 27                             // Backlight control pin
const unsigned long backlightTimeout = 60000; // 60 second inactivity timeout
bool backlightOn = false;

// --- WIFI CREDENTIALS ---
const char* ssid = WIFI_SSID;
const char* password = WIFI_PASSWORD;

// --- WEATHER API DATA kept in secret.h ---
const char* apiKey = WEATHER_API_KEY;
const char* lat = MY_LAT;
const char* lon = MY_LON;
const char* weatherUrl = NWS_WEATHER_URL;

// --- WEATHER RETRIEVAL PARAMETERS ---
unsigned long lastWeatherFetch = 0;
const unsigned long weatherInterval = 10 * 60 * 1000UL; // 10 minutes

// --- STRUCTURE TO STORE CURRENT WEATHER API DATA ---
struct WeatherData {
  unsigned long dt;       // Curent date (UNIX code)
  float temperature;      // Current temperature (F)
  String condition;       // Current weather condition
  int humidity;           // Current humidity (%)
  float windSpeed;        // Current wind speed (mph)
  int windDeg;            // Current wind direction (deg)
  float pressure;         // Current atmospheric pressure
  String forecast;        // Current forecast
  unsigned long sunrise;  // Current day sunrise
  unsigned long sunset;   // Current day sunset
} weatherData;

// --- STRUCTURE TO STORE NW WEATHER FORECAST API DATA ---
#define MAX_FORECAST_PERIODS 7 
struct ForecastPeriod {
  char name[30];
  int temperature;
  char tempUnit[5];
  char shortForecast[64];
  char detailedForecast[128];
  int windSpeed;
  char windDir[5];
};

ForecastPeriod dailyForecast[MAX_FORECAST_PERIODS];
int periodCount = 0;
bool dataReady = false;
bool isFetching = false;

// --- DEFINE USER INTERFACE MODES --- UI mode: 0=current weather, 1=forecast
int uiMode = 0;

// --- FUNCTION FORWARD DECLARATIONS ---
void fetchWeatherData();
void fetchNWSData();
void parseNWSData(String json);
void drawCurrentWeather();
void drawNWSForecast();
void drawSunRiseSet();
void drawTime();
void drawUIButtons();
String degToCompass(int deg);
String unixTo12HourString(time_t unixTime);
bool isDST(struct tm *timeinfo);

// --- SETUP ---
void setup() {
  Serial.begin(115200);

// --- INITIALIZE TFT SCREEN ---
  pinMode(BL_PIN, OUTPUT);       // Setup the backlight
  digitalWrite(BL_PIN, LOW);
  tft.init();
  tft.setRotation(1);            // Critical for this ESP32-3248S035 CYD
  tft.fillScreen(TFT_BLACK);


// --- INITIALIZE THE TOUCH CONTROLLER ---
  ts.begin();
  ts.setRotation(2);             // Critical for this ESP32-3248S035 CYD and GT911 Touch

// --- DISPLAY SCREEN CONNECTING WIFI ---
  tft.fillScreen(TFT_BLACK);
  tft.setTextSize(2);
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.drawCentreString("Connecting WiFi...", 240, 150, 4);
  digitalWrite(BL_PIN, HIGH);
  backlightOn = true;
  Serial.print("Connecting to WiFi...");

// --- CONNECT TO WIFI ---
  WiFi.begin(ssid, password);
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts++ < 40) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();

// --- DISPLAY SCREEN WIFI CONNECTION SUCCESSFUL ---
  if (WiFi.status() == WL_CONNECTED) {
    tft.fillScreen(TFT_BLACK);
        tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.drawCentreString("WiFi Connected", 240, 150, 4);
    digitalWrite(BL_PIN, HIGH);
    backlightOn = true;
    Serial.print("WiFi connected - IP: ");
    Serial.println(WiFi.localIP());
  } else {

// --- DISPLAY SCREEN WIFI CONNECTION FAILED ---
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.drawCentreString("WiFi FAILED", 240, 150, 4);
    digitalWrite(BL_PIN, HIGH); // Still turn on backlight on failure for message
    backlightOn = true;
    Serial.println("WiFi connection FAILED");
}
  delay(500);

  fetchWeatherData(); // First fetch immediately
  fetchNWSData();
  drawCurrentWeather();
}

// --- MAIN LOOP ---
void loop() {

// --- CHECK FOR SCREEN TOUCH ---
  ts.read();
  bool currentTouched = ts.isTouched;

  if (currentTouched && !prevTouched) {
    lastTouchTime = millis();

// --- READ AND PRINT TOUCH COORDINATES ---
    int16_t x = ts.points[0].x;
    int16_t y = ts.points[0].y;
    Serial.printf("Touched: %d,%d\n", x, y);

// --- TURN ON BACKLIGHT ---
    digitalWrite(BL_PIN, HIGH);
    backlightOn = true;

// --- IF USER TOUCHED BUTTON AREA, TOGGLE UI MODE ---
    if (y > 280) {            // Buttons area near bottom of screen
      if (x < 160) {
        uiMode = 0;           // Current weather
        drawCurrentWeather();
      } else if (x < 320) {
        uiMode = 1;           // Forecast
        drawNWSForecast();
      } else {
        // Clear screen or add another future MODE here
        drawSunRiseSet();
      }
    }
  }

  if (currentTouched) {
    lastTouchTime = millis(); // Refresh backlight timer
  }

  // --- BACKLIGHT TIMEOUT ---
  if (backlightOn && millis() - lastTouchTime > backlightTimeout) {
    digitalWrite(BL_PIN, LOW);
    backlightOn = false;
    Serial.println("Backlight off due to inactivity");
  }

  prevTouched = currentTouched;

  // --- FETCH WEATHER AT DEFINED INTERVAL weatherInterval ---
  if (millis() - lastWeatherFetch > weatherInterval) {
    fetchWeatherData();
    if (uiMode == 0) {
      drawCurrentWeather();
    } else {
      fetchNWSData();
      drawNWSForecast();
    }
  }

  delay(100);
}

// FETCH WEATHER DATA
void fetchWeatherData() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi not connected. Skipping fetch.");
    return;
  }
  
  String url = String("http://api.openweathermap.org/data/2.5/weather?lat=") + lat + "&lon=" + lon + "&appid=" + apiKey + "&units=imperial";

  HTTPClient http;
  http.begin(url);
  int httpCode = http.GET();

  if (httpCode == 200) {
    String payload = http.getString();

    DynamicJsonDocument doc(2048);
    auto error = deserializeJson(doc, payload);

  // EXTRACT WEATHER DATA FROM JSON RETURN
    if (!error) {
      weatherData.temperature = doc["main"]["temp"].as<float>();
      weatherData.condition = doc["weather"][0]["main"].as<String>();
      weatherData.humidity = doc["main"]["humidity"];
      weatherData.windSpeed = doc["wind"]["speed"].as<float>();
      weatherData.windDeg = doc["wind"]["deg"];
      weatherData.dt = doc["dt"];
      weatherData.sunrise = doc["sys"]["sunrise"];
      weatherData.sunset = doc["sys"]["sunset"];
      weatherData.pressure = doc["main"]["pressure"].as<float>();

      Serial.println("Weather data updated.");
      Serial.println(payload);
    } else {
      Serial.print("Failed to parse JSON: ");
      Serial.println(error.c_str());
    }
  } else {
    Serial.print("HTTP error code: ");
    Serial.println(httpCode);
  }

  http.end();
  lastWeatherFetch = millis();
}
void fetchNWSData() {
  HTTPClient httpNWS;
  httpNWS.begin(weatherUrl);
   
  int httpCodeNWS = httpNWS.GET();
  
  if (httpCodeNWS == HTTP_CODE_OK) {
    String payloadNWS = httpNWS.getString();
    parseNWSData(payloadNWS);
  } else {
    Serial.printf("HTTP NWS Error: %d\n", httpCodeNWS);
    char statusMsg[40];
    snprintf(statusMsg, sizeof(statusMsg), "HTTP NWS Error: %d", httpCodeNWS);
  }
  
  httpNWS.end();
}

void parseNWSData(String jsonNWS) {
  JsonDocument docNWS;
  // Use DeserializationOption::NestingLimit(20) for newer ArduinoJson versions
  DeserializationError error = deserializeJson(docNWS, jsonNWS, DeserializationOption::NestingLimit(20));
 
  if (error) {
    Serial.print("JSON NWS parsing failed: ");
    Serial.println(error.c_str());
    return;
  }
  
  JsonArray periods = docNWS["properties"]["periods"].as<JsonArray>();
  
  if (periods.isNull() || periods.size() == 0) {
    Serial.println("No NWS forecast data available");
    return;
  }
  
  periodCount = 0;
  for (JsonObject period : periods) {
    if (periodCount >= MAX_FORECAST_PERIODS) break;
    
    ForecastPeriod& fp = dailyForecast[periodCount];
    
    // Name (e.g., "Tonight", "Monday")
    strncpy(fp.name, period["name"] | "N/A", sizeof(fp.name) - 1);
    Serial.print(fp.name);
    Serial.print(" ");

    // Temperature
    fp.temperature = period["temperature"] | 0;
    strncpy(fp.tempUnit, period["temperatureUnit"] | "F", sizeof(fp.tempUnit) - 1);
    Serial.print(fp.temperature);
    Serial.print(fp.tempUnit);
    Serial.print(" ");
    
    // Short Forecast
    strncpy(fp.shortForecast, period["shortForecast"] | "Unknown", sizeof(fp.shortForecast) - 1);
    Serial.print(fp.shortForecast);
    Serial.print(" ");

    // Detailed Forecast
    strncpy(fp.detailedForecast, period["detailedForecast"] | "Unknown", sizeof(fp.detailedForecast) - 1);
    Serial.print(fp.detailedForecast);
    Serial.print(" ");
    Serial.println();

    // Wind Speed/Direction
    String windStr = period["windSpeed"].as<String>();
    fp.windSpeed = windStr.toInt(); 
    strncpy(fp.windDir, period["windDirection"] | "N/A", sizeof(fp.windDir) - 1);

    periodCount++;
  }
  
  Serial.printf("Parsed %d forecast periods.\n", periodCount);
  dataReady = true;
//  updateAllViews(); // Update UI after successful data fetch
}

// DRAW CURRENT WEATHER SCREEN ---
void drawCurrentWeather() {
  tft.fillScreen(TFT_BLACK);
  tft.setTextSize(1);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawCentreString("Current Weather", 240, 10, 4);

// --- TEMPERATURE ---
  float tempF = weatherData.temperature;
  String tempStr = String(tempF, 0) + " F";
  tft.setTextSize(2);
  tft.drawCentreString(tempStr, 240, 50, 4);

// --- WEATHER CONDITION ---
  tft.setTextSize(1);
  tft.drawCentreString(weatherData.condition, 240, 100, 4);

// --- HUMIDITY ---
  String humidityStr = "Humidity: " + String(weatherData.humidity) + "%";
  tft.drawCentreString(humidityStr, 240, 140, 4);

// --- WIND SPEED AND DIRECTION---
  float windMph = weatherData.windSpeed;
  String windStr = "Wind: " + String(windMph, 0) + " mph";
  String windDir = " from the " + degToCompass(weatherData.windDeg);
  String dspwind = windStr + windDir;
  if (windMph == 0) {
    dspwind = "Calm";
  }
  tft.drawCentreString(dspwind, 240, 180, 4);

// --- BAROMETRIC PRESSURE ---
  float barohPA = weatherData.pressure;
  float baroIn = barohPA * 0.02953;
  String baroStr = "Barometric Pressure: " + String(baroIn, 1) + " inHg";
  tft.drawCentreString(baroStr, 240, 220, 4);

  drawTime();
  drawUIButtons();
}

// DETERMINE WIND DIRECTION FROM windDeg
String degToCompass(int deg) {
  const char* dirs[] = {"N", "NNE", "NE", "ENE", "E", "ESE", "SE", "SSE", 
                        "S", "SSW", "SW", "WSW", "W", "WNW", "NW", "NNW"};
  int index = ((deg + 11) / 22) % 16;
  return String(dirs[index]);
}

// --- DRAW FORECAST SCREEN ---
void drawNWSForecast() {
  tft.fillScreen(TFT_BLACK);
  tft.setTextSize(1);
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);

  tft.drawCentreString("Forecast", 240, 10, 4);

// --- WEATHER CONDITION AND TEMPERATURE ---
  const int START_Y = STATUS_HEIGHT + 30;
  const int ITEM_HEIGHT = 30;
  const int MAX_ITEMS = (SCREEN_H - STATUS_HEIGHT - NAV_HEIGHT) / ITEM_HEIGHT;
  
  // Use a max of 7 periods (usually a week's forecast)
  int displayCount = min((int)periodCount, MAX_ITEMS); 

  tft.setTextFont(2);
  tft.setTextDatum(ML_DATUM); // Middle Left Datum

  for (int i = 0; i < displayCount; i++) {
    ForecastPeriod& fp = dailyForecast[i];
    int currentY = START_Y + i * ITEM_HEIGHT;
    
    // Draw background card for the item

    // 1. Day Name (Left)
    tft.setTextColor(TFT_YELLOW, TFT_BLACK);
    tft.drawString(fp.name, 15, currentY + ITEM_HEIGHT / 2);
    
    // 2. Short Forecast (Center)
    tft.setTextDatum(MC_DATUM); // Middle Center
    // Adjust x position slightly past the center towards the right
    tft.drawString(fp.shortForecast, 240, currentY + ITEM_HEIGHT / 2);

    // 3. Temperature (Right)
    char tempStr[32];
    snprintf(tempStr, sizeof(tempStr), "%d °%s", fp.temperature, fp.tempUnit);
    tft.setTextDatum(MR_DATUM); // Middle Right
    tft.drawString(tempStr, 465, currentY + ITEM_HEIGHT / 2);

    tft.setTextDatum(ML_DATUM); // Reset datum for safety
  }
  drawTime();
  drawUIButtons();
}

// DRAW SUNRISE SUNSET SCREEN ---
void drawSunRiseSet() {
  tft.fillScreen(TFT_BLACK);
  tft.setTextSize(1);
  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.drawCentreString("Daylight", 240, 10, 4);

// --- SUNRISE ---
  String srise = unixTo12HourString(weatherData.sunrise);
  tft.drawCentreString("Sunrise: " + srise, 240, 100, 4);

// --- SUNSET ---
  String sset = unixTo12HourString(weatherData.sunset);
  tft.drawCentreString("Sunset:  " + sset, 240, 140, 4);
  
  float daylight = (weatherData.sunset - weatherData.sunrise)/3600.0;
  String dlight = String(daylight, 1);
  tft.drawCentreString("Daylight: " + dlight + " hours", 240, 180, 4);

  drawTime();
  drawUIButtons();
}

// --- DRAW UI BUTTONS ---
void drawUIButtons() {
  tft.fillRect(0, 280, 160, 40, TFT_DARKGREY);
  tft.fillRect(160, 280, 160, 40, TFT_DARKGREY);
  tft.fillRect(320, 280, 160, 40, TFT_DARKGREY);

  tft.setTextSize(1);   // smaller text size for buttons
  tft.setTextColor(TFT_WHITE, TFT_DARKGREY);
  tft.drawCentreString("Current", 80, 290, 4);
  tft.setTextColor(TFT_YELLOW, TFT_DARKGREY);
  tft.drawCentreString("Forecast", 240, 290, 4);
  tft.setTextColor(TFT_GREEN, TFT_DARKGREY);
  tft.drawCentreString("Daylight", 400, 290, 4);
}

// --- DRAW TIME OF DATA RETREIVAL --
void drawTime() {
  int rectX = 380;
  int rectY = 10;
  int rectW = 100;
  int rectH = 30;

  tft.fillRect(rectX, rectY, rectW, rectH, TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(1);  // Adjust as needed for font size
  String polltime = unixTo12HourString(weatherData.dt);
  tft.drawCentreString(polltime, rectX + rectW / 2, rectY + rectH / 2 - 8, 2);
}

// CONVERT UNIX TIME TO HH:MM EST with or without DST
String unixTo12HourString(time_t unixTime) {
  // Convert UNIX time (UTC) to tm struct
  struct tm *utcTime = gmtime(&unixTime);

  // Check if DST is active based on UTC time
  bool dstActive = isDST(utcTime);

  // Calculate offset hours: EDT = UTC-4, EST = UTC-5
  int offsetHours = dstActive ? 4 : 5;

  // Adjust unixTime by subtracting offset to get EST/EDT time (still in epoch)
  unixTime -= offsetHours * 3600;

  // Convert adjusted unixTime to localtime struct tm
  struct tm *estTime = gmtime(&unixTime);

  // Extract hour, minute for formatting
  int hour = estTime->tm_hour;
  int minute = estTime->tm_min;
  bool isPM = false;

  // Convert 24h to 12h format without zero-padded hour
  if (hour == 0) {
    hour = 12;  // Midnight is 12 AM
  } else if (hour == 12) {
    isPM = true;  // Noon is 12 PM
  } else if (hour > 12) {
    hour -= 12;
    isPM = true;
  }

  // Build string result
  String result = String(hour) + ":";

  // Zero-pad minutes
  if (minute < 10)
    result += "0";

  result += String(minute) + (isPM ? " PM" : " AM");

  return result;
}


// --- IS IT DAYLIGHT SAVINGS TIME ---
bool isDST(struct tm *timeinfo) {
  int month = timeinfo->tm_mon + 1; // tm_mon is 0-11, add 1 for 1-12
  int day = timeinfo->tm_mday;      // Day of the month
  int wday = timeinfo->tm_wday;     // Day of the week (0=Sun, 1=Mon,...)

  // DST applies only between March and November
  if (month < 3 || month > 11) {
    return false;
  }
  
  // April through October are fully in DST
  if (month > 3 && month < 11) {
    return true;
  }

  // Calculate second Sunday in March
  if (month == 3) {
    // Find first Sunday in March
    int firstSunday = 1 + ((7 - (timeinfo->tm_wday + (1 - day)) % 7) % 7);
    int secondSunday = firstSunday + 7;

    if (day > secondSunday) {
      return true;  // After second Sunday in March
    } else if (day == secondSunday) {
      // If day is second Sunday, DST starts at 2:00 AM - assume all day after is DST
      if (timeinfo->tm_hour >= 2) {
        return true;
      }
    }
    return false;
  }

  // Calculate first Sunday in November
  if (month == 11) {
    int firstSunday = 1 + ((7 - (wday + (1 - day)) % 7) % 7);

    if (day < firstSunday) {
      return true;  // Before first Sunday in November
    } else if (day == firstSunday) {
      // DST ends at 2:00 AM, so before 2:00 AM is still DST
      if (timeinfo->tm_hour < 2) {
        return true;
      }
    }
    return false;
  }
  
  return false; // Fallback
}  