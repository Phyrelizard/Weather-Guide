#include <Arduino.h>
#include <math.h>
// Firmware version
#define FIRMWARE_VERSION "v2.3.2"
/** * Changelog:
 *  v2.3.2 - address http -11 error and 5th day forecast showing up with high and low temps the same.
 *  v2.3.1 - Minor bug fixes and improvements - fixed occasional reboots when backlight 
 *  turns on after PIR trigger, added more debug logging around PIR state changes
 * 
 *  v2.3.0 - Added SR602 PIR module to gopio06 for motion detection for backlight control
 * 
 *  v2.2.3 - rain animation drop overlays auto-center to rain icon width (works better with smaller rain.c assets)
 *  v2.2.2 - OTA update prep now turns backlight OFF after splash/blackout to eliminate visible flicker during upload
 *  v2.2.1 - animated rain icon overlays + forecast daily high/low aggregation fix
 *  v2.2.0 - OTA pre-upload splash/blackout screen + boot continue countdown auto-advance
 *  v2.1.2 - adjustments to gui layout and fonts, added more weather details to main screen, added precipitation % to forecast panels
 *  v2.1.1 - Minor bug fixes and improvements - added weather icons
 * 
 * v2.1.0 - OpenWeatherMap integration with current weather and 5-day forecast
 * v2.0.7 - gave up on have seconds display updating correctly, just hours and minutes
 * 
 * Weather Guide for Waveshare ESP32-S3 LCD 4.3"
 * 
 * Features:
 * - WiFiManager captive portal for easy WiFi setup
 * - Web interface for timezone configuration
 * - OTA firmware updates
 * - Displays clock in upper right corner
 * - OpenWeatherMap current weather and 5-day forecast
 * - Full screen weather display with icons
 *cd 
 * First boot: Connect to "WeatherGuide_Setup" network to configure WiFi
 * Configure weather at http://weatherguide.local/weather
 */

#include <lvgl.h>
#include <ESP_Panel_Library.h>
#include <ESP_IOExpander_Library.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <WebServer.h>
#include <Update.h>
#include <ESPmDNS.h>
#include <time.h>
#include <Preferences.h>
#include <nvs_flash.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include "driver/gpio.h"

// Weather icons (LVGL C-array images)
#include "weather_icons.h"

// Icon scaling (256 = 1.0x). Adjust if your icon bitmaps are large.
#define MAIN_ICON_ZOOM     256   // Current conditions icon
#define FORECAST_ICON_ZOOM 192   // 80px -> ~60px


// mDNS hostname
const char* mdnsName = "weatherguide";

// NTP server settings
const char* ntpServer = "pool.ntp.org";
long gmtOffset_sec = 0;
int daylightOffset_sec = 0;

// Clock format (12 or 24 hour)
bool use24HourFormat = true;

// Preferences for storing timezone settings
Preferences preferences;

// ============== WEATHER CONFIGURATION ==============
#define WEATHER_UPDATE_INTERVAL_MS 600000  // 10 minutes
String weatherApiKey = "";
float weatherLat = 0.0;
float weatherLon = 0.0;
bool useMetricUnits = false;
unsigned long lastWeatherUpdate = 0;
String weatherLocationName = "";

// Weather data structure
struct WeatherData {
    float temperature;
    float feelsLike;
    int humidity;
    float pressure;
    float windSpeed;
    int windDirection;
    String condition;
    String description;
    String icon;
    int clouds;
    time_t sunrise;
    time_t sunset;
    bool valid;
};

struct ForecastDay {
    time_t dt;
    float tempHigh;
    float tempLow;
    String condition;
    String icon;
    int pop;
};

WeatherData currentWeather = {0, 0, 0, 0, 0, 0, "", "", "", 0, 0, 0, false};
ForecastDay forecast[5];

// Web server
WebServer server(80);

// UI elements - Header
static lv_obj_t *clock_label = NULL;
static lv_obj_t *ampm_label = NULL;
static lv_obj_t *title_label = NULL;
static lv_obj_t *backlight_btn = NULL;

// UI elements - Setup screen
static lv_obj_t *serial_output = NULL;
static lv_obj_t *continue_btn = NULL;
static lv_obj_t *continue_countdown_label = NULL;

// OTA overlay
static lv_obj_t *ota_overlay = NULL;
static lv_obj_t *ota_label = NULL;
static lv_obj_t *ota_sub_label = NULL;

// UI elements - Weather screen
static lv_obj_t *weather_container = NULL;
static lv_obj_t *main_icon_img = NULL;        // Large weather icon image for current
static lv_obj_t *temp_label = NULL;           // Large temperature
static lv_obj_t *feels_like_label = NULL;
static lv_obj_t *condition_label = NULL;
static lv_obj_t *location_label = NULL;
static lv_obj_t *humidity_label = NULL;
static lv_obj_t *wind_label = NULL;
static lv_obj_t *pressure_label = NULL;
static lv_obj_t *last_update_label = NULL;

// Forecast panels and their children
static lv_obj_t *forecast_panels[5] = {NULL};
static lv_obj_t *forecast_day_labels[5] = {NULL};
static lv_obj_t *forecast_icon_imgs[5] = {NULL};
static lv_obj_t *forecast_high_labels[5] = {NULL};
static lv_obj_t *forecast_low_labels[5] = {NULL};
static lv_obj_t *forecast_pop_labels[5] = {NULL};

// Animated rain overlays (drawn on top of rain icons)
#define MAIN_RAIN_DROP_COUNT 6
#define FORECAST_RAIN_DROP_COUNT 4
static lv_obj_t *main_rain_overlay = NULL;
static lv_obj_t *main_rain_drops[MAIN_RAIN_DROP_COUNT] = {NULL};
static lv_point_t main_rain_drop_points[MAIN_RAIN_DROP_COUNT][2];
static lv_obj_t *forecast_rain_overlays[5] = {NULL};
static lv_obj_t *forecast_rain_drops[5][FORECAST_RAIN_DROP_COUNT] = {{NULL}};
static lv_point_t forecast_rain_drop_points[5][FORECAST_RAIN_DROP_COUNT][2];
static bool g_mainRainActive = false;
static bool g_forecastRainActive[5] = {false, false, false, false, false};
static uint8_t g_rainAnimPhase = 0;
static unsigned long g_lastRainAnimTick = 0;
// Rain drop alignment nudges (fine tune if you swap icon art again)
static const int MAIN_RAIN_DROP_X_NUDGE = -18; //prev was -4
static const int FORECAST_RAIN_DROP_X_NUDGE = -18; //was -6

static constexpr gpio_num_t PIR_GPIO = GPIO_NUM_6;
static constexpr unsigned long PIR_HOLD_MS = 15000;
static constexpr unsigned long PIR_DEBOUNCE_MS = 50;      // debounce chatter
static constexpr unsigned long PIR_BOOT_IGNORE_MS = 2000; // ignore PIR right after boot

static unsigned long pirLastRawChangeMs = 0;
static int pirRawLast = 0;
static int pirStable = 0;

static unsigned long pirLastHighMs = 0;
static bool pirKeepingBacklightOn = false;
static unsigned long pirBootMs = 0;

static constexpr unsigned long BACKLIGHT_TOGGLE_GUARD_MS = 1500;
static unsigned long backlightLastToggleMs = 0;

// UI state
enum UIState {
    UI_SETUP,
    UI_MAIN
};
UIState currentUIState = UI_SETUP;

// Sleep/wake state
bool isBacklightOff = false;
ESP_IOExpander *expander = NULL;

// Serial output buffer
#define MAX_SERIAL_LINES 15
#define MAX_LINE_LENGTH 80
String serialLines[MAX_SERIAL_LINES];
int currentLine = 0;

// Update tracking
static unsigned long lastUpdate = 0;
const unsigned long UPDATE_INTERVAL_MS = 1000;
bool g_rebootRequired = false;

// Boot continue auto-advance countdown
bool g_continueCountdownActive = false;
int g_continueCountdownSeconds = 10;
unsigned long g_lastContinueCountdownTick = 0;

// OTA display freeze / blackout
bool g_otaDisplayFreeze = false;
bool g_otaUploadStarted = false;
bool g_otaBacklightWasOff = false;
bool g_otaBacklightForcedOff = false;

// Extend IO Pin define
#define TP_RST 1
#define LCD_BL 2
#define LCD_RST 3
#define SD_CS 4
#define USB_SEL 5

// I2C Pin define
#define I2C_MASTER_NUM 0
#define I2C_MASTER_SDA_IO 8
#define I2C_MASTER_SCL_IO 9

/* LVGL porting configurations */
#define LVGL_TICK_PERIOD_MS     (2)
#define LVGL_TASK_MAX_DELAY_MS  (500)
#define LVGL_TASK_MIN_DELAY_MS  (1)
#define LVGL_TASK_STACK_SIZE    (4 * 1024)
#define LVGL_TASK_PRIORITY      (2)
#define LVGL_BUF_SIZE           (ESP_PANEL_LCD_H_RES * 20)

ESP_Panel *panel = NULL;
SemaphoreHandle_t lvgl_mux = NULL;

// Forward declare LVGL lock helpers used by weather animation helpers
void lvgl_port_lock(int timeout_ms);
void lvgl_port_unlock(void);

// ============== WEATHER ICON HELPERS ==============

// Get weather icon text (emoji-style text representation)
const char* getWeatherIconText(String iconCode) {
    if (iconCode.startsWith("01")) return "SUNNY";
    if (iconCode.startsWith("02")) return "PARTLY\nCLOUDY";
    if (iconCode.startsWith("03")) return "CLOUDY";
    if (iconCode.startsWith("04")) return "OVER\nCAST";
    if (iconCode.startsWith("09")) return "SHOW\nERS";
    if (iconCode.startsWith("10")) return "RAINY";
    if (iconCode.startsWith("11")) return "STORM";
    if (iconCode.startsWith("13")) return "SNOW";
    if (iconCode.startsWith("50")) return "FOGGY";
    return "---";
}


// Get LVGL image descriptor for the OpenWeather icon code (e.g. "01d", "10n")
const lv_img_dsc_t* getWeatherIconDsc(const String& iconCode) {
    if (iconCode.startsWith("01")) return &sunny;
    if (iconCode.startsWith("02")) return &partly_cloudy;
    if (iconCode.startsWith("03")) return &cloudy;
    if (iconCode.startsWith("04")) return &cloudy;
    if (iconCode.startsWith("09")) return &rain;
    if (iconCode.startsWith("10")) return &rain;
    if (iconCode.startsWith("11")) return &storm;
    if (iconCode.startsWith("13")) return &snow;
    if (iconCode.startsWith("50")) return &fog;
    return NULL;
}

bool isRainIconCode(const String& iconCode) {
    return iconCode.startsWith("09") || iconCode.startsWith("10");
}

void createRainOverlay(lv_obj_t* parent, lv_obj_t** overlayOut, lv_obj_t** drops, int dropCount, uint16_t w, uint16_t h, uint8_t lineWidth) {
    if (!parent || !overlayOut) return;

    lv_obj_t* overlay = lv_obj_create(parent);
    lv_obj_set_size(overlay, w, h);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(overlay, 0, 0);
    lv_obj_set_style_pad_left(overlay, 0, 0);
    lv_obj_set_style_pad_right(overlay, 0, 0);
    lv_obj_set_style_pad_top(overlay, 0, 0);
    lv_obj_set_style_pad_bottom(overlay, 0, 0);
    lv_obj_set_style_radius(overlay, 0, 0);
    lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_HIDDEN);
    // Don't block touches on the screen
    lv_obj_clear_flag(overlay, LV_OBJ_FLAG_CLICKABLE);

    for (int i = 0; i < dropCount; i++) {
        drops[i] = lv_line_create(overlay);
        lv_obj_set_style_line_color(drops[i], lv_color_hex(0x66CCFF), 0);
        lv_obj_set_style_line_width(drops[i], lineWidth, 0);
        lv_obj_set_style_line_rounded(drops[i], true, 0);
        lv_obj_add_flag(drops[i], LV_OBJ_FLAG_HIDDEN);
    }

    *overlayOut = overlay;
}

void refreshRainAnimationVisibilityLocked() {
    bool mainRain = currentWeather.valid && isRainIconCode(currentWeather.icon);
    g_mainRainActive = mainRain;
    if (main_rain_overlay) {
        if (mainRain) lv_obj_clear_flag(main_rain_overlay, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(main_rain_overlay, LV_OBJ_FLAG_HIDDEN);
    }

    for (int i = 0; i < 5; i++) {
        bool active = (forecast[i].dt != 0) && isRainIconCode(forecast[i].icon);
        g_forecastRainActive[i] = active;
        if (forecast_rain_overlays[i]) {
            if (active) lv_obj_clear_flag(forecast_rain_overlays[i], LV_OBJ_FLAG_HIDDEN);
            else lv_obj_add_flag(forecast_rain_overlays[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void updateRainAnimationFrameLocked() {
    // Main 80x80 icon overlay
    if (g_mainRainActive && main_rain_overlay) {
    // Auto-center the drop pattern based on the actual rain icon width (helps when rain.c is smaller)
    static const uint8_t xPct[MAIN_RAIN_DROP_COUNT] = {18, 34, 50, 66, 82, 58};
    int overlayW = (int)lv_obj_get_width(main_rain_overlay);
    int rainDispW = (int)((rain.header.w * (uint32_t)MAIN_ICON_ZOOM + 128U) / 256U);
    if (rainDispW <= 0 || rainDispW > overlayW) rainDispW = overlayW;
    int rainLeft = (overlayW - rainDispW) / 2;
    for (int i = 0; i < MAIN_RAIN_DROP_COUNT; i++) {
        int y = 38 + ((int)g_rainAnimPhase * 4 + i * 7) % 30;
        int x = rainLeft + (int)((rainDispW * xPct[i]) / 100) + MAIN_RAIN_DROP_X_NUDGE;
        if (x < 2) x = 2;
        if (x > overlayW - 3) x = overlayW - 3;
        main_rain_drop_points[i][0].x = x;

            main_rain_drop_points[i][0].y = y;
            main_rain_drop_points[i][1].x = x - 2;
            main_rain_drop_points[i][1].y = y + 10;
            if (main_rain_drops[i]) {
                lv_line_set_points(main_rain_drops[i], main_rain_drop_points[i], 2);
                if (((g_rainAnimPhase + i) % 5) == 0) lv_obj_add_flag(main_rain_drops[i], LV_OBJ_FLAG_HIDDEN);
                else lv_obj_clear_flag(main_rain_drops[i], LV_OBJ_FLAG_HIDDEN);
            }
        }
    } else {
        for (int i = 0; i < MAIN_RAIN_DROP_COUNT; i++) {
            if (main_rain_drops[i]) lv_obj_add_flag(main_rain_drops[i], LV_OBJ_FLAG_HIDDEN);
        }
    }

    // Forecast 60x60 icon overlays
    for (int p = 0; p < 5; p++) {
        if (g_forecastRainActive[p] && forecast_rain_overlays[p]) {
    static const uint8_t xPctSmall[FORECAST_RAIN_DROP_COUNT] = {22, 42, 62, 80};
    int overlayW = (int)lv_obj_get_width(forecast_rain_overlays[p]);
    int rainDispW = (int)((rain.header.w * (uint32_t)FORECAST_ICON_ZOOM + 128U) / 256U);
    if (rainDispW <= 0 || rainDispW > overlayW) rainDispW = overlayW;
    int rainLeft = (overlayW - rainDispW) / 2;
    for (int i = 0; i < FORECAST_RAIN_DROP_COUNT; i++) {
        int y = 25 + ((int)g_rainAnimPhase * 3 + i * 5 + p) % 18;
        int x = rainLeft + (int)((rainDispW * xPctSmall[i]) / 100) + FORECAST_RAIN_DROP_X_NUDGE;
        if (x < 2) x = 2;
        if (x > overlayW - 3) x = overlayW - 3;
        forecast_rain_drop_points[p][i][0].x = x;

                forecast_rain_drop_points[p][i][0].y = y;
                forecast_rain_drop_points[p][i][1].x = x - 2;
                forecast_rain_drop_points[p][i][1].y = y + 7;
                if (forecast_rain_drops[p][i]) {
                    lv_line_set_points(forecast_rain_drops[p][i], forecast_rain_drop_points[p][i], 2);
                    if (((g_rainAnimPhase + i + p) % 4) == 0) lv_obj_add_flag(forecast_rain_drops[p][i], LV_OBJ_FLAG_HIDDEN);
                    else lv_obj_clear_flag(forecast_rain_drops[p][i], LV_OBJ_FLAG_HIDDEN);
                }
            }
        } else {
            for (int i = 0; i < FORECAST_RAIN_DROP_COUNT; i++) {
                if (forecast_rain_drops[p][i]) lv_obj_add_flag(forecast_rain_drops[p][i], LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
}

void tickRainAnimation(unsigned long nowMs) {
    if (g_otaDisplayFreeze || currentUIState != UI_MAIN) return;
    if (nowMs - g_lastRainAnimTick < 140) return;
    g_lastRainAnimTick = nowMs;
    g_rainAnimPhase = (uint8_t)((g_rainAnimPhase + 1) % 20);

    lvgl_port_lock(-1);
    updateRainAnimationFrameLocked();
    lvgl_port_unlock();
}

// Get short weather text for forecast panels
const char* getWeatherShortText(String iconCode) {
    if (iconCode.startsWith("01")) return "Sun";
    if (iconCode.startsWith("02")) return "P.Cld";
    if (iconCode.startsWith("03")) return "Cloud";
    if (iconCode.startsWith("04")) return "Ovcst";
    if (iconCode.startsWith("09")) return "Shwr";
    if (iconCode.startsWith("10")) return "Rain";
    if (iconCode.startsWith("11")) return "Storm";
    if (iconCode.startsWith("13")) return "Snow";
    if (iconCode.startsWith("50")) return "Fog";
    return "---";
}

// Get weather icon color based on condition
uint32_t getWeatherColor(String iconCode) {
    if (iconCode.startsWith("01")) return 0xFFD700;  // Gold - sunny
    if (iconCode.startsWith("02")) return 0x87CEEB;  // Sky blue - partly cloudy
    if (iconCode.startsWith("03")) return 0xB0C4DE;  // Light steel blue - cloudy
    if (iconCode.startsWith("04")) return 0x778899;  // Light slate gray - overcast
    if (iconCode.startsWith("09")) return 0x4682B4;  // Steel blue - showers
    if (iconCode.startsWith("10")) return 0x1E90FF;  // Dodger blue - rain
    if (iconCode.startsWith("11")) return 0x9400D3;  // Dark violet - storm
    if (iconCode.startsWith("13")) return 0xFFFFFF;  // White - snow
    if (iconCode.startsWith("50")) return 0xDCDCDC;  // Gainsboro - fog
    return 0xFFFFFF;
}

// ============== LVGL PORT FUNCTIONS ==============

#if ESP_PANEL_LCD_BUS_TYPE == ESP_PANEL_BUS_TYPE_RGB
void lvgl_port_disp_flush(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p)
{
    panel->getLcd()->drawBitmap(area->x1, area->y1, area->x2 + 1, area->y2 + 1, color_p);
    lv_disp_flush_ready(disp);
}
#else
void lvgl_port_disp_flush(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p)
{
    panel->getLcd()->drawBitmap(area->x1, area->y1, area->x2 + 1, area->y2 + 1, color_p);
}

bool notify_lvgl_flush_ready(void *user_ctx)
{
    lv_disp_drv_t *disp_driver = (lv_disp_drv_t *)user_ctx;
    lv_disp_flush_ready(disp_driver);
    return false;
}
#endif

#if ESP_PANEL_USE_LCD_TOUCH
void lvgl_port_tp_read(lv_indev_drv_t * indev, lv_indev_data_t * data)
{
    panel->getLcdTouch()->readData();
    bool touched = panel->getLcdTouch()->getTouchState();
    if(!touched) {
        data->state = LV_INDEV_STATE_REL;
    } else {
        TouchPoint point = panel->getLcdTouch()->getPoint();
        data->state = LV_INDEV_STATE_PR;
        data->point.x = point.x;
        data->point.y = point.y;
    }
}
#endif

void lvgl_port_lock(int timeout_ms)
{
    const TickType_t timeout_ticks = (timeout_ms < 0) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    xSemaphoreTakeRecursive(lvgl_mux, timeout_ticks);
}

void lvgl_port_unlock(void)
{
    xSemaphoreGiveRecursive(lvgl_mux);
}

void lvgl_port_task(void *arg)
{
    uint32_t task_delay_ms = LVGL_TASK_MAX_DELAY_MS;
    while (1) {
        lvgl_port_lock(-1);
        task_delay_ms = lv_timer_handler();
        lvgl_port_unlock();
        if (task_delay_ms > LVGL_TASK_MAX_DELAY_MS) {
            task_delay_ms = LVGL_TASK_MAX_DELAY_MS;
        } else if (task_delay_ms < LVGL_TASK_MIN_DELAY_MS) {
            task_delay_ms = LVGL_TASK_MIN_DELAY_MS;
        }
        vTaskDelay(pdMS_TO_TICKS(task_delay_ms));
    }
}

// ============== SERIAL OUTPUT ==============

void addSerialLine(String line) {
    if (line.length() > MAX_LINE_LENGTH) {
        line = line.substring(0, MAX_LINE_LENGTH);
    }
    
    serialLines[currentLine] = line;
    currentLine = (currentLine + 1) % MAX_SERIAL_LINES;
    
    if (serial_output != NULL && currentUIState == UI_SETUP) {
        String fullText = "";
        for (int i = 0; i < MAX_SERIAL_LINES; i++) {
            int idx = (currentLine + i) % MAX_SERIAL_LINES;
            if (serialLines[idx].length() > 0) {
                fullText += serialLines[idx] + "\n";
            }
        }
        
        lvgl_port_lock(-1);
        lv_label_set_text(serial_output, fullText.c_str());
        lvgl_port_unlock();
    }
}

class DisplaySerial : public Print {
public:
    size_t write(uint8_t c) override {
        Serial.write(c);
        
        static String lineBuffer = "";
        if (c == '\n') {
            addSerialLine(lineBuffer);
            lineBuffer = "";
        } else if (c != '\r') {
            lineBuffer += (char)c;
        }
        return 1;
    }
    
    size_t write(const uint8_t *buffer, size_t size) override {
        for (size_t i = 0; i < size; i++) {
            write(buffer[i]);
        }
        return size;
    }
};

DisplaySerial DisplayLog;

// ============== SETTINGS FUNCTIONS ==============

void loadTimezoneSettings() {
    preferences.begin("clock", false);
    gmtOffset_sec = preferences.getLong("gmtOffset", 0);
    daylightOffset_sec = preferences.getInt("dstOffset", 0);
    use24HourFormat = preferences.getBool("format24", true);
    preferences.end();
    DisplayLog.printf("Timezone: GMT=%ld, DST=%d\n", gmtOffset_sec, daylightOffset_sec);
}

void saveTimezoneSettings() {
    preferences.begin("clock", false);
    preferences.putLong("gmtOffset", gmtOffset_sec);
    preferences.putInt("dstOffset", daylightOffset_sec);
    preferences.putBool("format24", use24HourFormat);
    preferences.end();
}

void loadWeatherSettings() {
    preferences.begin("weather", true);
    weatherApiKey = preferences.getString("apikey", "");
    weatherLat = preferences.getFloat("lat", 0.0);
    weatherLon = preferences.getFloat("lon", 0.0);
    useMetricUnits = preferences.getBool("metric", false);
    preferences.end();
    
    if (weatherApiKey.length() > 0) {
        DisplayLog.printf("Weather: %.4f, %.4f\n", weatherLat, weatherLon);
    } else {
        DisplayLog.println("Weather not configured");
    }
}

void saveWeatherSettings() {
    preferences.begin("weather", false);
    preferences.putString("apikey", weatherApiKey);
    preferences.putFloat("lat", weatherLat);
    preferences.putFloat("lon", weatherLon);
    preferences.putBool("metric", useMetricUnits);
    preferences.end();
    DisplayLog.println("Weather settings saved");
}

// ============== WEATHER FUNCTIONS ==============

String getWindDirection(int degrees) {
    const char* directions[] = {"N", "NNE", "NE", "ENE", "E", "ESE", "SE", "SSE", 
                                 "S", "SSW", "SW", "WSW", "W", "WNW", "NW", "NNW"};
    int index = (int)((degrees + 11.25) / 22.5) % 16;
    return String(directions[index]);
}

// Forward declaration
void updateWeatherUI();
void updateContinueCountdownLabel();
void beginContinueCountdown();
void stopContinueCountdown();

void showOTAPrepareMessageScreen();
void showOTABlackoutScreen();
void backlightOff();
void backlightOn();
void otaCutBacklightForUpdate();
void otaRestoreBacklightAfterFailedUpdate();
void initPirInput();
void updatePirBacklightControl(unsigned long nowMs);


bool isRainIconCode(const String& iconCode);
void createRainOverlay(lv_obj_t* parent, lv_obj_t** overlayOut, lv_obj_t** drops, int dropCount, uint16_t w, uint16_t h, uint8_t lineWidth);
void refreshRainAnimationVisibilityLocked();
void updateRainAnimationFrameLocked();
void tickRainAnimation(unsigned long nowMs);

bool fetchCurrentWeather() {
    if (WiFi.status() != WL_CONNECTED) {
        DisplayLog.println("WiFi not connected");
        return false;
    }
    
    if (weatherApiKey.length() == 0 || (weatherLat == 0.0 && weatherLon == 0.0)) {
        DisplayLog.println("Weather not configured");
        return false;
    }
    
    HTTPClient http;
    WiFiClientSecure client;
    client.setInsecure();
    
    String units = useMetricUnits ? "metric" : "imperial";
    String url = "https://api.openweathermap.org/data/2.5/weather?";
    url += "lat=" + String(weatherLat, 6);
    url += "&lon=" + String(weatherLon, 6);
    url += "&appid=" + weatherApiKey;
    url += "&units=" + units;
    
    DisplayLog.println("Fetching weather...");
    
    http.begin(client, url);
    http.setTimeout(10000);
    int httpCode = http.GET();
    
    if (httpCode == HTTP_CODE_OK) {
        String payload = http.getString();
        
        DynamicJsonDocument doc(2048);
        DeserializationError error = deserializeJson(doc, payload);
        
        if (error) {
            DisplayLog.printf("JSON error: %s\n", error.c_str());
            http.end();
            return false;
        }
        
        currentWeather.temperature = doc["main"]["temp"];
        currentWeather.feelsLike = doc["main"]["feels_like"];
        currentWeather.humidity = doc["main"]["humidity"];
        currentWeather.pressure = doc["main"]["pressure"];
        currentWeather.windSpeed = doc["wind"]["speed"];
        currentWeather.windDirection = doc["wind"]["deg"] | 0;
        currentWeather.condition = doc["weather"][0]["main"].as<String>();
        currentWeather.description = doc["weather"][0]["description"].as<String>();
        currentWeather.icon = doc["weather"][0]["icon"].as<String>();
        currentWeather.clouds = doc["clouds"]["all"] | 0;
        currentWeather.sunrise = doc["sys"]["sunrise"];
        currentWeather.sunset = doc["sys"]["sunset"];
        weatherLocationName = doc["name"].as<String>();
        currentWeather.valid = true;
        
        DisplayLog.printf("Weather: %.0f%s %s\n", 
                          currentWeather.temperature, 
                          useMetricUnits ? "C" : "F",
                          currentWeather.condition.c_str());
        
        http.end();
        return true;
    } else {
        DisplayLog.printf("HTTP error: %d\n", httpCode);
        http.end();
        return false;
    }
}

bool fetchForecast() {
    if (WiFi.status() != WL_CONNECTED || weatherApiKey.length() == 0) {
        DisplayLog.println("Forecast: WiFi or API missing");
        return false;
    }

    HTTPClient http;
    WiFiClientSecure client;
    client.setInsecure();

    String units = useMetricUnits ? "metric" : "imperial";
    String url = "https://api.openweathermap.org/data/2.5/forecast?";
    url += "lat=" + String(weatherLat, 6);
    url += "&lon=" + String(weatherLon, 6);
    url += "&appid=" + weatherApiKey;
    url += "&units=" + units;

    DisplayLog.println("Fetching forecast...");

    http.begin(client, url);
    http.setTimeout(20000);

    int httpCode = http.GET();
    if (httpCode == -11) {                 // retry once on timeout
        DisplayLog.println("Forecast timeout (-11), retrying once...");
        delay(250);
        http.end();
        http.begin(client, url);
        http.setTimeout(20000);
        httpCode = http.GET();
    }

    if (httpCode == HTTP_CODE_OK) {
        String payload = http.getString();

        DynamicJsonDocument doc(24576);
        DeserializationError error = deserializeJson(doc, payload);

        if (error) {
            DisplayLog.printf("Forecast JSON error: %s\n", error.c_str());
            http.end();
            return false;
        }

        JsonArray list = doc["list"].as<JsonArray>();
        if (list.isNull() || list.size() == 0) {
            DisplayLog.println("Forecast: empty list");
            http.end();
            return false;
        }

        int dayIndex = 0;
        int lastDayKey = -1;

        // Determine "today" key from first entry local date,
        // then SKIP that partial day to avoid fake Day-5 equal high/low.
        time_t firstDt = list[0]["dt"].as<long>();
        struct tm* firstTm = localtime(&firstDt);
        int firstDayKey = firstTm
            ? (((firstTm->tm_year + 1900) * 1000) + firstTm->tm_yday)
            : -1;

        // point counter per bucket so you can see if a day had only one sample
        int pointsPerDay[5] = {0, 0, 0, 0, 0};

        for (int i = 0; i < 5; i++) {
            forecast[i].dt = 0;
            forecast[i].tempHigh = 0;
            forecast[i].tempLow = 0;
            forecast[i].condition = "";
            forecast[i].icon = "";
            forecast[i].pop = 0;
        }

        for (JsonObject item : list) {
            if (dayIndex >= 5) break;

            time_t dt = item["dt"].as<long>();
            struct tm* timeinfo = localtime(&dt);
            if (!timeinfo) continue;

            int dayKey = ((timeinfo->tm_year + 1900) * 1000) + timeinfo->tm_yday;

            // Skip current/partial day
            //if (dayKey == firstDayKey) continue;

            float tMax = item["main"]["temp_max"].as<float>();
            float tMin = item["main"]["temp_min"].as<float>();
            String cond = item["weather"][0]["main"].as<String>();
            String icon = item["weather"][0]["icon"].as<String>();
            int popPct = (int)(item["pop"].as<float>() * 100.0f + 0.5f);

            if (dayKey != lastDayKey) {
                forecast[dayIndex].dt = dt;
                forecast[dayIndex].tempHigh = tMax;
                forecast[dayIndex].tempLow = tMin;
                forecast[dayIndex].condition = cond;
                forecast[dayIndex].icon = icon;
                forecast[dayIndex].pop = popPct;

                pointsPerDay[dayIndex] = 1;
                lastDayKey = dayKey;
                dayIndex++;
            } else {
                int idx = dayIndex - 1;
                if (idx < 0 || idx >= 5) continue;

                if (tMax > forecast[idx].tempHigh) forecast[idx].tempHigh = tMax;
                if (tMin < forecast[idx].tempLow)  forecast[idx].tempLow  = tMin;

                if (popPct >= forecast[idx].pop) {
                    forecast[idx].pop = popPct;
                    forecast[idx].condition = cond;
                    forecast[idx].icon = icon;
                }

                pointsPerDay[idx]++;
            }
        }

        for (int i = 0; i < dayIndex; i++) {
        const char* partialTag = (pointsPerDay[i] < 8) ? " (partial)" : "";
        DisplayLog.printf("Day %d%s: %s %.1f/%.1f  pop=%d%%  points=%d\n",
        i + 1,
        partialTag,
        forecast[i].condition.c_str(),
        forecast[i].tempHigh,
        forecast[i].tempLow,
        forecast[i].pop,
        pointsPerDay[i]);
        }

        DisplayLog.printf("Forecast: %d days loaded\n", dayIndex);
        http.end();
        return (dayIndex > 0);
    } else {
        DisplayLog.printf("Forecast HTTP error: %d\n", httpCode);
        http.end();
        return false;
    }
}

void updateWeather() {
    if (fetchCurrentWeather()) {
        fetchForecast();
        lastWeatherUpdate = millis();
        updateWeatherUI();
    }
}

// ============== BOOT COUNTDOWN / OTA OVERLAY HELPERS ==============

void updateContinueCountdownLabel() {
    if (!continue_countdown_label) return;

    char buf[32];
    snprintf(buf, sizeof(buf), "Auto %ds", g_continueCountdownSeconds);

    lvgl_port_lock(-1);
    lv_label_set_text(continue_countdown_label, buf);
    if (continue_btn) {
        lv_obj_align_to(continue_countdown_label, continue_btn, LV_ALIGN_OUT_LEFT_MID, -12, 0);
    }
    lvgl_port_unlock();
}

void beginContinueCountdown() {
    g_continueCountdownSeconds = 10;
    g_lastContinueCountdownTick = millis();
    g_continueCountdownActive = true;
    updateContinueCountdownLabel();

    lvgl_port_lock(-1);
    if (continue_countdown_label) {
        lv_obj_clear_flag(continue_countdown_label, LV_OBJ_FLAG_HIDDEN);
        if (continue_btn) lv_obj_align_to(continue_countdown_label, continue_btn, LV_ALIGN_OUT_LEFT_MID, -12, 0);
    }
    lvgl_port_unlock();
}

void stopContinueCountdown() {
    g_continueCountdownActive = false;
    lvgl_port_lock(-1);
    if (continue_countdown_label) lv_obj_add_flag(continue_countdown_label, LV_OBJ_FLAG_HIDDEN);
    lvgl_port_unlock();
}

void showOTAPrepareMessageScreen() {
    g_otaDisplayFreeze = true;

    lvgl_port_lock(-1);
    if (ota_overlay) {
        lv_obj_move_foreground(ota_overlay);
        lv_obj_clear_flag(ota_overlay, LV_OBJ_FLAG_HIDDEN);
    }
    if (ota_label) {
        lv_label_set_text(ota_label, "Firmware Updating...");
        lv_obj_clear_flag(ota_label, LV_OBJ_FLAG_HIDDEN);
    }
    if (ota_sub_label) {
        lv_label_set_text(ota_sub_label, "Device will reboot upon completion");
        lv_obj_clear_flag(ota_sub_label, LV_OBJ_FLAG_HIDDEN);
    }
    lvgl_port_unlock();
}

void showOTABlackoutScreen() {
    g_otaDisplayFreeze = true;

    lvgl_port_lock(-1);
    if (ota_overlay) {
        lv_obj_move_foreground(ota_overlay);
        lv_obj_clear_flag(ota_overlay, LV_OBJ_FLAG_HIDDEN);
    }
    if (ota_label) lv_obj_add_flag(ota_label, LV_OBJ_FLAG_HIDDEN);
    if (ota_sub_label) lv_obj_add_flag(ota_sub_label, LV_OBJ_FLAG_HIDDEN);
    lvgl_port_unlock();
}

// ============== WEB SERVER HTML ==============

const char update_html[] PROGMEM = 
"<!DOCTYPE html><html><head>"
"<title>Weather Guide OTA</title>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<meta charset='UTF-8'>"
"<script src='https://ajax.googleapis.com/ajax/libs/jquery/3.2.1/jquery.min.js'></script>"
"<style>"
"*{margin:0;padding:0;box-sizing:border-box}"
"body{font-family:'Courier New',monospace;background:linear-gradient(135deg,#0a0a0a 0%,#1a0033 50%,#330066 100%);color:#0ff;min-height:100vh;display:flex;align-items:center;justify-content:center;padding:20px}"
".container{background:rgba(0,0,0,0.8);border:2px solid #0ff;border-radius:15px;padding:30px;max-width:500px;width:100%}"
"h1{text-align:center;font-size:2em;margin-bottom:10px;text-shadow:0 0 10px #0ff}"
".info{background:rgba(0,255,255,0.1);border:1px solid #0ff;border-radius:8px;padding:15px;margin:20px 0;text-align:center}"
"strong{color:#fff}"
".radio-group{margin:20px 0;padding:15px;background:rgba(0,255,255,0.05);border-radius:8px}"
".radio-option{display:flex;align-items:center;padding:10px;margin:5px 0;background:rgba(0,0,0,0.3);border:1px solid #0ff;border-radius:5px;cursor:pointer}"
"#file-label{display:block;padding:15px;background:rgba(0,255,255,0.1);border:2px dashed #0ff;border-radius:8px;text-align:center;cursor:pointer;margin:20px 0}"
"#file-input{position:absolute;left:-9999px}"
"button{width:100%;padding:15px;margin:10px 0;background:linear-gradient(135deg,#0ff 0%,#00aaaa 100%);color:#000;border:none;border-radius:8px;font-size:1.1em;font-weight:bold;cursor:pointer}"
"button:disabled{background:#555;color:#999}"
"#progress-container{display:none;margin:20px 0}"
"#progress-bar-bg{width:100%;height:30px;background:rgba(0,50,50,0.5);border-radius:5px}"
"#progress-bar{width:0%;height:100%;background:linear-gradient(90deg,#0ff,#00ff88);border-radius:5px}"
"#status{text-align:center;margin:15px 0;font-size:1.2em}"
"</style></head><body>"
"<div class='container'>"
"<h1>FIRMWARE UPDATE</h1>"
"<div class='info'><p><strong>Version:</strong> " FIRMWARE_VERSION "</p></div>"
"<form id='upload-form' enctype='multipart/form-data'>"
"<div class='radio-group'>"
"<label class='radio-option'><input type='radio' name='cmd' value='0' checked> FIRMWARE (.bin)</label>"
"<label class='radio-option'><input type='radio' name='cmd' value='100'> FILESYSTEM</label>"
"</div>"
"<label id='file-label' for='file-input'>SELECT FILE</label>"
"<input type='file' id='file-input' name='update' accept='.bin'>"
"<button type='submit' id='submit-btn' disabled>UPLOAD</button>"
"</form>"
"<div id='progress-container'><div id='progress-bar-bg'><div id='progress-bar'></div></div><div id='status'></div></div>"
"<button onclick='location.href=\"/\"' style='background:#666'>BACK</button>"
"</div>"
"<script>"
"var fi=document.getElementById('file-input'),fl=document.getElementById('file-label'),sb=document.getElementById('submit-btn');"
"fi.onchange=function(){if(this.files.length>0){fl.textContent=this.files[0].name;sb.disabled=false;}};"
"document.getElementById('upload-form').onsubmit=function(e){"
"e.preventDefault();var fd=new FormData(this),cmd=document.querySelector('input[name=cmd]:checked').value;"
"document.getElementById('progress-container').style.display='block';sb.disabled=true;"
"document.getElementById('status').textContent='Preparing device screen...';"
"var prep=new XMLHttpRequest();"
"prep.onload=function(){if(prep.status!=200){document.getElementById('status').textContent='Prep failed';sb.disabled=false;return;}"
"document.getElementById('status').textContent='Uploading...';"
"var xhr=new XMLHttpRequest();"
"xhr.upload.onprogress=function(e){if(e.lengthComputable){var p=Math.round(e.loaded/e.total*100);document.getElementById('progress-bar').style.width=p+'%';document.getElementById('status').textContent=p+'%';}};"
"xhr.onload=function(){document.getElementById('status').textContent=xhr.responseText=='OK'?'SUCCESS! Rebooting...':'FAILED';if(xhr.responseText=='OK')setTimeout(function(){location.href='/';},5000);else sb.disabled=false;};"
"xhr.open('POST','/update?cmd='+cmd);xhr.send(fd);};"
"prep.onerror=function(){document.getElementById('status').textContent='Prep request failed';sb.disabled=false;};"
"prep.open('GET','/ota-prep');prep.send();};"
"</script></body></html>";

const char timezone_html[] PROGMEM = 
"<!DOCTYPE html><html><head>"
"<title>Weather Guide</title>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<style>"
"body{font-family:Arial;background:#1a1a1a;color:#fff;padding:20px;text-align:center}"
"h1{color:#0ff}"
".container{max-width:600px;margin:auto;background:#2a2a2a;padding:20px;border-radius:10px}"
".info{background:#3a3a3a;padding:15px;margin:20px 0;border-radius:5px}"
"select,button{font-size:18px;padding:12px;margin:10px;width:90%;border-radius:5px}"
"select{background:#2a2a2a;color:#fff;border:2px solid #0ff}"
"button{background:#0ff;border:none;color:#000;font-weight:bold;cursor:pointer}"
".nav-btn{background:#666;margin-top:10px}"
".radio-group{margin:15px 0;padding:10px;background:#3a3a3a;border-radius:5px}"
".radio-option{display:inline-block;margin:10px 15px}"
"</style></head><body>"
"<div class='container'>"
"<h1>Weather Guide</h1>"
"<div class='info'><p><strong>Firmware:</strong> " FIRMWARE_VERSION "</p><p><strong>IP:</strong> %IP%</p></div>"
"<form action='/settings' method='POST'>"
"<label style='color:#0ff'>Timezone:</label>"
"<select name='timezone' id='tz'>"
"<option value='-36000,0'>UTC-10 Hawaii</option>"
"<option value='-28800,0'>UTC-8 PST</option>"
"<option value='-25200,0'>UTC-7 MST</option>"
"<option value='-21600,0'>UTC-6 CST</option>"
"<option value='-18000,0'>UTC-5 EST</option>"
"<option value='0,0'>UTC+0 GMT</option>"
"<option value='3600,0'>UTC+1 CET</option>"
"</select>"
"<div class='radio-group'>"
"<label class='radio-option'><input type='radio' name='format' value='12' %F12%> 12 Hour</label>"
"<label class='radio-option'><input type='radio' name='format' value='24' %F24%> 24 Hour</label>"
"</div>"
"<button type='submit'>Save</button>"
"</form>"
"<button class='nav-btn' onclick='location.href=\"/weather\"'>Weather Config</button>"
"<button class='nav-btn' onclick='location.href=\"/update\"'>OTA Update</button>"
"<button class='nav-btn' onclick='location.href=\"/resetwifi\"'>Reset WiFi</button>"
"</div>"
"<script>var c='%GMT%,%DST%',s=document.getElementById('tz');for(var i=0;i<s.options.length;i++)if(s.options[i].value===c)s.selectedIndex=i;</script>"
"</body></html>";

// ============== WEB SERVER SETUP ==============

void setupWebServer() {
    server.on("/", HTTP_GET, [](){
        String html = String(timezone_html);
        html.replace("%IP%", WiFi.localIP().toString());
        html.replace("%GMT%", String(gmtOffset_sec));
        html.replace("%DST%", String(daylightOffset_sec));
        html.replace("%F12%", use24HourFormat ? "" : "checked");
        html.replace("%F24%", use24HourFormat ? "checked" : "");
        server.send(200, "text/html", html);
    });
    
    server.on("/update", HTTP_GET, [](){
        server.send_P(200, "text/html", update_html);
    });
    
    server.on("/ota-prep", HTTP_GET, [](){
        showOTAPrepareMessageScreen();
        delay(4000);                 // keep message visible before upload begins
        showOTABlackoutScreen();     // black screen during OTA upload to avoid flicker
        otaCutBacklightForUpdate();   // physically blank panel before upload begins
        g_otaUploadStarted = false;  // set true in actual upload callback
        server.send(200, "text/plain", "READY");
    });

    server.on("/settings", HTTP_POST, [](){
        if(server.hasArg("timezone")) {
            String tz = server.arg("timezone");
            int idx = tz.indexOf(',');
            if(idx > 0) {
                gmtOffset_sec = tz.substring(0, idx).toInt();
                daylightOffset_sec = tz.substring(idx + 1).toInt();
            }
        }
        if(server.hasArg("format")) {
            use24HourFormat = (server.arg("format") == "24");
        }
        saveTimezoneSettings();
        configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
        server.sendHeader("Location", "/");
        server.send(303);
    });
    
    server.on("/resetwifi", HTTP_GET, [](){
        server.send(200, "text/html", "<html><body style='background:#000;color:#0ff;text-align:center;padding:50px'><h1>WiFi Reset</h1><p>Rebooting...</p></body></html>");
        delay(2000);
        WiFiManager wm;
        wm.resetSettings();
        ESP.restart();
    });
    
    server.on("/update", HTTP_POST, 
        []() {
            server.sendHeader("Connection", "close");
            bool ok = !Update.hasError();
            server.send(200, "text/plain", ok ? "OK" : "FAIL");
            if (!ok) {
                g_otaDisplayFreeze = false;
                otaRestoreBacklightAfterFailedUpdate();
                lvgl_port_lock(-1);
                if (ota_overlay) lv_obj_add_flag(ota_overlay, LV_OBJ_FLAG_HIDDEN);
                if (ota_label) lv_obj_clear_flag(ota_label, LV_OBJ_FLAG_HIDDEN);
                if (ota_sub_label) lv_obj_clear_flag(ota_sub_label, LV_OBJ_FLAG_HIDDEN);
                lvgl_port_unlock();
            }
            g_rebootRequired = ok;
        },
        []() {
            HTTPUpload& upload = server.upload();
            if (upload.status == UPLOAD_FILE_START) {
                g_otaUploadStarted = true;
                g_otaDisplayFreeze = true;
                showOTABlackoutScreen();  // ensure black screen stays visible during upload
                otaCutBacklightForUpdate();   // fallback in case /ota-prep was skipped
                int cmd = server.arg("cmd") == "100" ? U_SPIFFS : U_FLASH;
                Update.begin(UPDATE_SIZE_UNKNOWN, cmd);
            } else if (upload.status == UPLOAD_FILE_WRITE) {
                Update.write(upload.buf, upload.currentSize);
            } else if (upload.status == UPLOAD_FILE_END) {
                Update.end(true);
            }
        }
    );
    
    // Weather configuration page
    server.on("/weather", HTTP_GET, [](){
        String html = F("<!DOCTYPE html><html><head>"
            "<title>Weather Config</title>"
            "<meta name='viewport' content='width=device-width,initial-scale=1'>"
            "<style>"
            "body{font-family:Arial;background:#1a1a1a;color:#fff;padding:20px;text-align:center}"
            "h1{color:#0ff}"
            ".container{max-width:500px;margin:auto;background:#2a2a2a;padding:20px;border-radius:10px}"
            "input,select{width:90%;padding:12px;margin:10px 0;background:#3a3a3a;border:2px solid #0ff;color:#fff;border-radius:5px}"
            "button{width:90%;padding:12px;margin:10px 0;background:#0ff;border:none;color:#000;font-weight:bold;border-radius:5px;cursor:pointer}"
            ".nav-btn{background:#666}"
            "label{display:block;margin-top:15px;color:#0ff}"
            ".info{font-size:12px;color:#888}"
            "</style></head><body>"
            "<div class='container'>"
            "<h1>Weather Settings</h1>"
            "<form action='/save-weather' method='POST'>"
            "<label>OpenWeatherMap API Key</label>"
            "<input type='password' name='apikey' value='");
        html += (weatherApiKey.length() > 0 ? "********" : "");
        html += F("' placeholder='Enter API key'>"
            "<p class='info'>Get free key at openweathermap.org/api</p>"
            "<label>Latitude</label>"
            "<input type='text' name='lat' value='");
        html += String(weatherLat, 6);
        html += F("' placeholder='e.g. 40.7128'>"
            "<label>Longitude</label>"
            "<input type='text' name='lon' value='");
        html += String(weatherLon, 6);
        html += F("' placeholder='e.g. -74.0060'>"
            "<p class='info'>Find coords at latlong.net</p>"
            "<label>Units</label>"
            "<select name='units'>"
            "<option value='imperial'");
        html += (useMetricUnits ? "" : " selected");
        html += F(">Fahrenheit</option><option value='metric'");
        html += (useMetricUnits ? " selected" : "");
        html += F(">Celsius</option></select>"
            "<button type='submit'>Save</button>"
            "</form>"
            "<button class='nav-btn' onclick='location.href=\"/\"'>Back</button>"
            "</div></body></html>");
        server.send(200, "text/html", html);
    });
    
    server.on("/save-weather", HTTP_POST, [](){
        if (server.hasArg("apikey") && server.arg("apikey") != "********" && server.arg("apikey").length() > 0) {
            weatherApiKey = server.arg("apikey");
        }
        if (server.hasArg("lat")) weatherLat = server.arg("lat").toFloat();
        if (server.hasArg("lon")) weatherLon = server.arg("lon").toFloat();
        if (server.hasArg("units")) useMetricUnits = (server.arg("units") == "metric");
        
        saveWeatherSettings();
        updateWeather();
        server.sendHeader("Location", "/weather");
        server.send(303);
    });
    
    server.on("/refresh-weather", HTTP_GET, [](){
        updateWeather();
        server.sendHeader("Location", "/");
        server.send(303);
    });
    
    server.begin();
    DisplayLog.println("Web server started");
}

// ============== WIFI ==============

void initWiFi() {
    WiFiManager wm;
    wm.setConfigPortalTimeout(180);
    wm.setConnectTimeout(20);
    
    DisplayLog.println("Starting WiFi...");
    
    if (WiFi.SSID() == "") {
        DisplayLog.println("=== WIFI SETUP ===");
        DisplayLog.println("Connect to: WeatherGuide_Setup");
        DisplayLog.println("Password: weather123");
    }
    
    if(!wm.autoConnect("WeatherGuide_Setup", "weather123")) {
        DisplayLog.println("WiFi failed, starting portal...");
        if (!wm.startConfigPortal("WeatherGuide_Setup", "weather123")) {
            ESP.restart();
        }
    }
    
    DisplayLog.println("WiFi connected!");
    DisplayLog.print("IP: ");
    DisplayLog.println(WiFi.localIP());
    
    if (MDNS.begin(mdnsName)) {
        MDNS.addService("http", "tcp", 80);
    }
}

void initTime() {
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
    DisplayLog.println("Syncing time...");
    
    struct tm timeinfo;
    int attempts = 0;
    while (!getLocalTime(&timeinfo) && attempts < 10) {
        delay(1000);
        attempts++;
    }
    
    if (getLocalTime(&timeinfo)) {
        DisplayLog.println("Time synced!");
    }
}

// ============== BACKLIGHT ==============

void backlightOff() {
    unsigned long now = millis();
    if (now - backlightLastToggleMs < BACKLIGHT_TOGGLE_GUARD_MS) return;

    if (expander && !isBacklightOff) {
        expander->digitalWrite(LCD_BL, LOW);
        isBacklightOff = true;
        backlightLastToggleMs = now;
    }
}

void initPirInput() {
    gpio_config_t cfg = {};
    cfg.pin_bit_mask = (1ULL << PIR_GPIO);
    cfg.mode = GPIO_MODE_INPUT;
    cfg.pull_up_en = GPIO_PULLUP_DISABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_ENABLE; // PIR output is active HIGH, so pull down to ensure LOW when idle
    cfg.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&cfg);

    pirBootMs = millis();
    pirRawLast = gpio_get_level(PIR_GPIO);
    pirStable = pirRawLast;
    pirLastRawChangeMs = pirBootMs;
    pirLastHighMs = pirBootMs;
}

void updatePirBacklightControl(unsigned long nowMs) {
    // Ignore PIR during early boot settling
    if (nowMs - pirBootMs < PIR_BOOT_IGNORE_MS) {
        return;
    }

    int raw = gpio_get_level(PIR_GPIO);

    // Track raw transitions
    if (raw != pirRawLast) {
        pirRawLast = raw;
        pirLastRawChangeMs = nowMs;
    }

    // Debounce: accept new stable state only after PIR_DEBOUNCE_MS
    if ((nowMs - pirLastRawChangeMs) >= PIR_DEBOUNCE_MS) {
        pirStable = pirRawLast;
    }

    if (pirStable == 1) {
        pirLastHighMs = nowMs;
        if (!pirKeepingBacklightOn) {
            backlightOn();                 // one-shot write only on edge
            pirKeepingBacklightOn = true;
        }
        return;
    }

    // stable LOW: hold for PIR_HOLD_MS, then turn off once
    if (pirKeepingBacklightOn && (nowMs - pirLastHighMs >= PIR_HOLD_MS)) {
        backlightOff();                    // one-shot write only on edge
        pirKeepingBacklightOn = false;
    }
}

void backlightOn() {
    unsigned long now = millis();
    if (now - backlightLastToggleMs < BACKLIGHT_TOGGLE_GUARD_MS) return;

    if (expander && isBacklightOff) {
        expander->digitalWrite(LCD_BL, HIGH);
        isBacklightOff = false;
        backlightLastToggleMs = now;
    }
}

void otaCutBacklightForUpdate() {
    g_otaBacklightWasOff = isBacklightOff;
    if (!isBacklightOff) {
        backlightOff();
        g_otaBacklightForcedOff = true;
    } else {
        g_otaBacklightForcedOff = false;
    }
}

void otaRestoreBacklightAfterFailedUpdate() {
    if (g_otaBacklightForcedOff && !g_otaBacklightWasOff) {
        backlightOn();
    }
    g_otaBacklightForcedOff = false;
}

void toggleBacklight() {
    if (isBacklightOff) backlightOn();
    else backlightOff();
}

// ============== UI STATE ==============

void showContinueButton() {
    if (continue_btn && currentUIState == UI_SETUP) {
        lvgl_port_lock(-1);
        lv_obj_clear_flag(continue_btn, LV_OBJ_FLAG_HIDDEN);
        lvgl_port_unlock();
        beginContinueCountdown();
    }
}

void showMainScreen() {
    currentUIState = UI_MAIN;
    stopContinueCountdown();
    g_lastRainAnimTick = 0;
    
    lvgl_port_lock(-1);
    
    if (serial_output) lv_obj_add_flag(serial_output, LV_OBJ_FLAG_HIDDEN);
    if (continue_btn) lv_obj_add_flag(continue_btn, LV_OBJ_FLAG_HIDDEN);
    if (continue_countdown_label) lv_obj_add_flag(continue_countdown_label, LV_OBJ_FLAG_HIDDEN);
    if (weather_container) lv_obj_clear_flag(weather_container, LV_OBJ_FLAG_HIDDEN);
    
    lvgl_port_unlock();
    
    if (weatherApiKey.length() > 0) {
        updateWeather();
    }
}

void screen_touch_handler(lv_event_t * e) {
    if (isBacklightOff) backlightOn();
}

// ============== CREATE UI ==============

void createUI() {
    lv_obj_t *scr = lv_scr_act();
    
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0a0a1a), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_add_event_cb(scr, screen_touch_handler, LV_EVENT_PRESSED, NULL);
    
    // Title
    title_label = lv_label_create(scr);
    lv_obj_set_style_text_font(title_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(title_label, lv_color_hex(0x00FFFF), 0);
    lv_label_set_text(title_label, "Weather Guide " FIRMWARE_VERSION);
    lv_obj_align(title_label, LV_ALIGN_TOP_LEFT, 10, 8);
    
    // Backlight button
    backlight_btn = lv_btn_create(scr);
    lv_obj_set_size(backlight_btn, 80, 28);
    lv_obj_align(backlight_btn, LV_ALIGN_TOP_MID, 0, 5);
    lv_obj_set_style_bg_color(backlight_btn, lv_color_hex(0xFF6600), 0);
    lv_obj_t *bl_lbl = lv_label_create(backlight_btn);
    lv_label_set_text(bl_lbl, "Light");
    lv_obj_center(bl_lbl);
    lv_obj_add_event_cb(backlight_btn, [](lv_event_t * e) {
        if (lv_event_get_code(e) == LV_EVENT_CLICKED) toggleBacklight();
    }, LV_EVENT_CLICKED, NULL);
    
    // Clock
    clock_label = lv_label_create(scr);
    lv_obj_set_style_text_font(clock_label, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(clock_label, lv_color_hex(0xFFFF00), 0);
    lv_label_set_text(clock_label, "00:00");
    lv_obj_align(clock_label, LV_ALIGN_TOP_RIGHT, -50, 5);
    
    ampm_label = lv_label_create(scr);
    lv_obj_set_style_text_font(ampm_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(ampm_label, lv_color_hex(0xFFFF00), 0);
    lv_label_set_text(ampm_label, "");
    lv_obj_align(ampm_label, LV_ALIGN_TOP_RIGHT, -5, 8);
    
    // Serial output (setup screen)
    serial_output = lv_label_create(scr);
    lv_obj_set_style_text_font(serial_output, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(serial_output, lv_color_hex(0x00FF00), 0);
    lv_label_set_long_mode(serial_output, LV_LABEL_LONG_WRAP);
    lv_obj_set_size(serial_output, 780, 350);
    lv_obj_align(serial_output, LV_ALIGN_TOP_LEFT, 10, 40);
    lv_label_set_text(serial_output, "Starting...\n");
    
    // Continue button
    continue_btn = lv_btn_create(scr);
    lv_obj_set_size(continue_btn, 140, 50);
    lv_obj_align(continue_btn, LV_ALIGN_BOTTOM_RIGHT, -10, -10);
    lv_obj_set_style_bg_color(continue_btn, lv_color_hex(0x00FFFF), 0);
    lv_obj_add_flag(continue_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *cont_lbl = lv_label_create(continue_btn);
    lv_label_set_text(cont_lbl, "Continue");
    lv_obj_set_style_text_color(cont_lbl, lv_color_hex(0x000000), 0);
    lv_obj_center(cont_lbl);
    lv_obj_add_event_cb(continue_btn, [](lv_event_t * e) {
        if (lv_event_get_code(e) == LV_EVENT_CLICKED) showMainScreen();
    }, LV_EVENT_CLICKED, NULL);

    // Countdown label (auto-continue after 10s if not touched)
    continue_countdown_label = lv_label_create(scr);
    lv_obj_set_style_text_font(continue_countdown_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(continue_countdown_label, lv_color_hex(0xFFAA00), 0);
    lv_label_set_text(continue_countdown_label, "Auto 10s");
    lv_obj_align_to(continue_countdown_label, continue_btn, LV_ALIGN_OUT_LEFT_MID, -12, 0);
    lv_obj_add_flag(continue_countdown_label, LV_OBJ_FLAG_HIDDEN);

    // Full-screen OTA overlay (blackout + centered status text)
    ota_overlay = lv_obj_create(scr);
    lv_obj_set_size(ota_overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(ota_overlay, 0, 0);
    lv_obj_set_style_bg_color(ota_overlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(ota_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(ota_overlay, 0, 0);
    lv_obj_clear_flag(ota_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(ota_overlay, LV_OBJ_FLAG_HIDDEN);

    ota_label = lv_label_create(ota_overlay);
    lv_obj_set_style_text_font(ota_label, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(ota_label, lv_color_hex(0xFF8800), 0);
    lv_obj_set_style_text_align(ota_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(ota_label, "Firmware Updating...");
    lv_obj_align(ota_label, LV_ALIGN_CENTER, 0, -18);

    ota_sub_label = lv_label_create(ota_overlay);
    lv_obj_set_style_text_font(ota_sub_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(ota_sub_label, lv_color_hex(0xFFAA33), 0);
    lv_obj_set_style_text_align(ota_sub_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(ota_sub_label, "Device will reboot upon completion");
    lv_obj_align(ota_sub_label, LV_ALIGN_CENTER, 0, 18);
}

// ============== CREATE WEATHER UI (FULL SCREEN) ==============

void createWeatherUI() {
    lv_obj_t *scr = lv_scr_act();
    
    // Weather container - full screen below header
    weather_container = lv_obj_create(scr);

    // Use the full screen height (minus the top header row) — no reserved footer area on Weather screen
    lv_disp_t *disp = lv_disp_get_default();
    int scrW = (int)lv_disp_get_hor_res(disp);
    int scrH = (int)lv_disp_get_ver_res(disp);
    const int HEADER_H = 40;

    lv_obj_set_size(weather_container, scrW, scrH - HEADER_H);
    lv_obj_set_pos(weather_container, 0, HEADER_H);

    lv_obj_set_style_bg_color(weather_container, lv_color_hex(0x0a0a1a), 0);
    lv_obj_set_style_border_width(weather_container, 0, 0);

    // Keep a small margin on left/right/top, but let content go to the bottom edge
    lv_obj_set_style_pad_left(weather_container, 5, 0);
    lv_obj_set_style_pad_right(weather_container, 5, 0);
    lv_obj_set_style_pad_top(weather_container, 5, 0);
    lv_obj_set_style_pad_bottom(weather_container, 0, 0);
lv_obj_add_flag(weather_container, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(weather_container, LV_OBJ_FLAG_SCROLLABLE);
    
    // ===== LEFT SECTION: Current Weather Icon =====
    main_icon_img = lv_img_create(weather_container);
    lv_img_set_src(main_icon_img, &sunny);  // placeholder until first update
    lv_obj_set_pos(main_icon_img, 35, 15);
    lv_img_set_zoom(main_icon_img, MAIN_ICON_ZOOM);

    // Animated rain overlay for main icon (80x80 icon region)
    createRainOverlay(weather_container, &main_rain_overlay, main_rain_drops, MAIN_RAIN_DROP_COUNT, 80, 80, 3);
    lv_obj_set_pos(main_rain_overlay, 35, 15);

    // ===== CENTER SECTION: Temperature & Details =====
    
    // Large temperature
    temp_label = lv_label_create(weather_container);
    lv_obj_set_style_text_font(temp_label, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(temp_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(temp_label, LV_ALIGN_TOP_MID, 0, 10);
    lv_label_set_text(temp_label, "--°F");
    
    // Feels like
    feels_like_label = lv_label_create(weather_container);
    lv_obj_set_style_text_font(feels_like_label, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_color(feels_like_label, lv_color_hex(0xAAAAAA), 0);
    lv_obj_align(feels_like_label, LV_ALIGN_TOP_MID, 0, 70);
    lv_label_set_text(feels_like_label, "Feels like --°");
    
    // Condition description
    condition_label = lv_label_create(weather_container);
    lv_obj_set_style_text_font(condition_label, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(condition_label, lv_color_hex(0x00FFFF), 0);
    lv_obj_align(condition_label, LV_ALIGN_TOP_MID, 0, 100);
    lv_label_set_text(condition_label, "---");
    
    // Location
    location_label = lv_label_create(weather_container);
    lv_obj_set_style_text_font(location_label, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_color(location_label, lv_color_hex(0x888888), 0);
    lv_obj_align(location_label, LV_ALIGN_TOP_MID, 0, 130);
    lv_label_set_text(location_label, "---");
    
    // ===== RIGHT SECTION: Details =====
    
    // Humidity
    humidity_label = lv_label_create(weather_container);
    lv_obj_set_style_text_font(humidity_label, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(humidity_label, lv_color_hex(0x66CCFF), 0);
    lv_obj_align(humidity_label, LV_ALIGN_TOP_RIGHT, -20, 20);
    lv_label_set_text(humidity_label, "Humidity: --%");
    
    // Wind
    wind_label = lv_label_create(weather_container);
    lv_obj_set_style_text_font(wind_label, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(wind_label, lv_color_hex(0x99FF99), 0);
    lv_obj_align(wind_label, LV_ALIGN_TOP_RIGHT, -20, 55);
    lv_label_set_text(wind_label, "Wind: -- mph");
    
    // Pressure
    pressure_label = lv_label_create(weather_container);
    lv_obj_set_style_text_font(pressure_label, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(pressure_label, lv_color_hex(0xFFCC66), 0);
    lv_obj_align(pressure_label, LV_ALIGN_TOP_RIGHT, -20, 90);
    lv_label_set_text(pressure_label, "Pressure: -- hPa");
    
    // Last update
    last_update_label = lv_label_create(weather_container);
    lv_obj_set_style_text_font(last_update_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(last_update_label, lv_color_hex(0x666666), 0);
    lv_obj_align(last_update_label, LV_ALIGN_TOP_RIGHT, -20, 125);
    lv_label_set_text(last_update_label, "Updated: --:--");
    
    // ===== BOTTOM SECTION: 5-Day Forecast =====
    
    int panelWidth = 145;
    int panelSpacing = 155;
    int startX = 15;
    int startY = 170;

    // Make forecast panels reach the bottom of the weather container (including any area
    // that was previously reserved for a footer on other screens)
    int containerH = lv_obj_get_height(weather_container);
    int padTop = (int)lv_obj_get_style_pad_top(weather_container, LV_PART_MAIN);
    int padBottom = (int)lv_obj_get_style_pad_bottom(weather_container, LV_PART_MAIN);
    int contentH = containerH - padTop - padBottom;

    int panelHeight = contentH - startY;   // go all the way down
    if (panelHeight < 200) panelHeight = 200;
    
    for (int i = 0; i < 5; i++) {
        // Panel background
        lv_obj_t *panel = lv_obj_create(weather_container);
        lv_obj_set_size(panel, panelWidth, panelHeight + 40);
        lv_obj_set_pos(panel, startX + (i * panelSpacing), startY);
        lv_obj_set_style_bg_color(panel, lv_color_hex(0x1a1a2e), 0);
        lv_obj_set_style_border_color(panel, lv_color_hex(0x333355), 0);
        lv_obj_set_style_border_width(panel, 2, 0);
        lv_obj_set_style_radius(panel, 10, 0);
        lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
        forecast_panels[i] = panel;
        
        // Day name
        lv_obj_t *day_lbl = lv_label_create(panel);
        lv_obj_set_style_text_font(day_lbl, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(day_lbl, lv_color_hex(0x00FFFF), 0);
        lv_obj_align(day_lbl, LV_ALIGN_TOP_MID, 0, 5);
        lv_label_set_text(day_lbl, "---");
        forecast_day_labels[i] = day_lbl;
        
        // Weather icon
        lv_obj_t *icon_img = lv_img_create(panel);
        lv_img_set_src(icon_img, &cloudy);  // placeholder
        lv_obj_align(icon_img, LV_ALIGN_TOP_MID, 0, 30);
        lv_img_set_zoom(icon_img, FORECAST_ICON_ZOOM);
        forecast_icon_imgs[i] = icon_img;

        // Animated rain overlay over the scaled forecast icon (~60x60 visual area)
        createRainOverlay(panel, &forecast_rain_overlays[i], forecast_rain_drops[i], FORECAST_RAIN_DROP_COUNT, 60, 60, 2);
        lv_obj_set_pos(forecast_rain_overlays[i], (panelWidth - 60) / 2, 40);

        // High temp
        lv_obj_t *high_lbl = lv_label_create(panel);
        lv_obj_set_style_text_font(high_lbl, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(high_lbl, lv_color_hex(0xFF6666), 0);
        lv_obj_align(high_lbl, LV_ALIGN_TOP_MID, 0, 110);
        lv_label_set_text(high_lbl, "--°");
        forecast_high_labels[i] = high_lbl;
        
        // Low temp
        lv_obj_t *low_lbl = lv_label_create(panel);
        lv_obj_set_style_text_font(low_lbl, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(low_lbl, lv_color_hex(0x6699FF), 0);
        lv_obj_align(low_lbl, LV_ALIGN_TOP_MID, 0, 140);
        lv_label_set_text(low_lbl, "--°");
        forecast_low_labels[i] = low_lbl;
        
        // Precipitation %
        lv_obj_t *pop_lbl = lv_label_create(panel);
        lv_obj_set_style_text_font(pop_lbl, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(pop_lbl, lv_color_hex(0x66CCFF), 0);
        lv_obj_align(pop_lbl, LV_ALIGN_TOP_MID, 0, 170);
        lv_label_set_text(pop_lbl, "-- %");
        forecast_pop_labels[i] = pop_lbl;
    }
}

// ============== UPDATE WEATHER UI ==============

void updateWeatherUI() {
    if (!currentWeather.valid) return;
    
    lvgl_port_lock(-1);
    
    const char* unit = useMetricUnits ? "C" : "F";
    const char* speedUnit = useMetricUnits ? "m/s" : "mph";
    char buf[64];
    
    // Update main weather icon image
    const lv_img_dsc_t *mainIcon = getWeatherIconDsc(currentWeather.icon);
    if (main_icon_img && mainIcon) {
        lv_img_set_src(main_icon_img, mainIcon);
        lv_obj_clear_flag(main_icon_img, LV_OBJ_FLAG_HIDDEN);
    } else if (main_icon_img) {
        lv_obj_add_flag(main_icon_img, LV_OBJ_FLAG_HIDDEN);
    }
    
    // Temperature
    snprintf(buf, sizeof(buf), "%.0f°%s", currentWeather.temperature, unit);
    lv_label_set_text(temp_label, buf);
    
    // Feels like
    snprintf(buf, sizeof(buf), "Feels like %.0f°%s", currentWeather.feelsLike, unit);
    lv_label_set_text(feels_like_label, buf);
    
    // Condition
    lv_label_set_text(condition_label, currentWeather.description.c_str());
    
    // Location
    if (weatherLocationName.length() > 0) {
        lv_label_set_text(location_label, weatherLocationName.c_str());
    }
    
    // Details
    snprintf(buf, sizeof(buf), "Humidity: %d%%", currentWeather.humidity);
    lv_label_set_text(humidity_label, buf);
    
    snprintf(buf, sizeof(buf), "Wind: %.0f %s %s", currentWeather.windSpeed, speedUnit, getWindDirection(currentWeather.windDirection).c_str());
    lv_label_set_text(wind_label, buf);
    
    snprintf(buf, sizeof(buf), "Pressure: %d hPa", (int)currentWeather.pressure);
    lv_label_set_text(pressure_label, buf);
    
    // Last update time
    struct tm timeinfo;
    if (getLocalTime(&timeinfo)) {
        strftime(buf, sizeof(buf), "Updated: %H:%M", &timeinfo);
        lv_label_set_text(last_update_label, buf);
    }
    
    // Update forecast panels
    const char* dayNames[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    
    for (int i = 0; i < 5; i++) {
        if (forecast[i].dt == 0) {
            lv_label_set_text(forecast_day_labels[i], "---");
            if (forecast_icon_imgs[i]) lv_obj_add_flag(forecast_icon_imgs[i], LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(forecast_high_labels[i], "--°");
            lv_label_set_text(forecast_low_labels[i], "--°");
            lv_label_set_text(forecast_pop_labels[i], "-- %");
            if (forecast_rain_overlays[i]) lv_obj_add_flag(forecast_rain_overlays[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            struct tm* fc_time = localtime(&forecast[i].dt);
            
            // Day name
            lv_label_set_text(forecast_day_labels[i], dayNames[fc_time->tm_wday]);
            
            // Weather icon image
            const lv_img_dsc_t *fIcon = getWeatherIconDsc(forecast[i].icon);
            if (forecast_icon_imgs[i] && fIcon) {
                lv_img_set_src(forecast_icon_imgs[i], fIcon);
                lv_obj_clear_flag(forecast_icon_imgs[i], LV_OBJ_FLAG_HIDDEN);
            } else if (forecast_icon_imgs[i]) {
                lv_obj_add_flag(forecast_icon_imgs[i], LV_OBJ_FLAG_HIDDEN);
            }
            
            // High/Low temp (avoid same displayed integer when raw values differ but rounding collapses them)
            int highDisp = (int)lroundf(forecast[i].tempHigh);
            int lowDisp = (int)lroundf(forecast[i].tempLow);
            if (highDisp == lowDisp && (forecast[i].tempHigh - forecast[i].tempLow) > 0.01f) {
                highDisp = (int)ceilf(forecast[i].tempHigh);
                lowDisp = (int)floorf(forecast[i].tempLow);
            }

            snprintf(buf, sizeof(buf), "%d°", highDisp);
            lv_label_set_text(forecast_high_labels[i], buf);

            snprintf(buf, sizeof(buf), "%d°", lowDisp);
            lv_label_set_text(forecast_low_labels[i], buf);

            // Precipitation
            snprintf(buf, sizeof(buf), "%d%%", forecast[i].pop);
            lv_label_set_text(forecast_pop_labels[i], buf);
        }
    }

    // Refresh rain overlay visibility and advance one frame immediately after new data/icons arrive
    refreshRainAnimationVisibilityLocked();
    updateRainAnimationFrameLocked();

    lvgl_port_unlock();
}

// ============== UPDATE CLOCK ==============

void updateClock() {
    if (g_otaDisplayFreeze) return;
    struct tm timeinfo;
    if (getLocalTime(&timeinfo)) {
        char timeStr[16];
        char ampmStr[4] = "";
        
        if (use24HourFormat) {
            strftime(timeStr, sizeof(timeStr), "%H:%M", &timeinfo);
        } else {
            strftime(timeStr, sizeof(timeStr), "%I:%M", &timeinfo);
            strftime(ampmStr, sizeof(ampmStr), "%p", &timeinfo);
        }
        
        lvgl_port_lock(-1);
        if (clock_label) lv_label_set_text(clock_label, timeStr);
        if (ampm_label) lv_label_set_text(ampm_label, ampmStr);
        lvgl_port_unlock();
    }
}

// ============== SETUP ==============

void setup()
{
    Serial.begin(115200);
    
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    Serial.println("Weather Guide " FIRMWARE_VERSION);

    loadTimezoneSettings();
    loadWeatherSettings();

    panel = new ESP_Panel();
    lv_init();

    static lv_disp_draw_buf_t draw_buf;
    uint8_t *buf = (uint8_t *)heap_caps_calloc(1, LVGL_BUF_SIZE * sizeof(lv_color_t), MALLOC_CAP_INTERNAL);
    assert(buf);
    lv_disp_draw_buf_init(&draw_buf, buf, NULL, LVGL_BUF_SIZE);

    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = ESP_PANEL_LCD_H_RES;
    disp_drv.ver_res = ESP_PANEL_LCD_V_RES;
    disp_drv.flush_cb = lvgl_port_disp_flush;
    disp_drv.draw_buf = &draw_buf;
    lv_disp_drv_register(&disp_drv);

#if ESP_PANEL_USE_LCD_TOUCH
    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = lvgl_port_tp_read;
    lv_indev_drv_register(&indev_drv);
#endif

    panel->init();
#if ESP_PANEL_LCD_BUS_TYPE != ESP_PANEL_BUS_TYPE_RGB
    panel->getLcd()->setCallback(notify_lvgl_flush_ready, &disp_drv);
#endif

    DisplayLog.println("Init IO expander");
    expander = new ESP_IOExpander_CH422G((i2c_port_t)I2C_MASTER_NUM, ESP_IO_EXPANDER_I2C_CH422G_ADDRESS_000);
    expander->init();
    expander->begin();
    expander->multiPinMode(TP_RST | LCD_BL | LCD_RST | SD_CS | USB_SEL, OUTPUT);
    expander->multiDigitalWrite(TP_RST | LCD_BL | LCD_RST | SD_CS, HIGH);
    expander->digitalWrite(USB_SEL, LOW);
    panel->addIOExpander(expander);
  
    expander->digitalWrite(LCD_BL, HIGH);
    isBacklightOff = false;

    panel->begin();
    delay(100);
    expander->digitalWrite(LCD_BL, HIGH);

    initPirInput();

    lvgl_mux = xSemaphoreCreateRecursiveMutex();
    xTaskCreate(lvgl_port_task, "lvgl", LVGL_TASK_STACK_SIZE, NULL, LVGL_TASK_PRIORITY, NULL);

    lvgl_port_lock(-1);
    createUI();
    createWeatherUI();
    lvgl_port_unlock();

    initWiFi();
    initTime();
    setupWebServer();

    DisplayLog.println("Setup complete!");
    DisplayLog.printf("http://%s.local\n", mdnsName);
    
    if (weatherApiKey.length() > 0) {
        DisplayLog.println("Fetching weather...");
        updateWeather();
    } else {
        DisplayLog.println("Configure weather at /weather");
    }
    
    showContinueButton();
}

// ============== LOOP ==============

void loop()
{
    server.handleClient();
    
    if (g_rebootRequired) {
        delay(2000);
        ESP.restart();
    }
    
unsigned long currentMillis = millis();
tickRainAnimation(currentMillis);
updatePirBacklightControl(currentMillis);

    // Boot screen auto-continue countdown (10s) if button is not pressed
    if (g_continueCountdownActive && currentUIState == UI_SETUP && !g_otaDisplayFreeze) {
        if (currentMillis - g_lastContinueCountdownTick >= 1000) {
            g_lastContinueCountdownTick += 1000;
            if (g_continueCountdownSeconds > 0) {
                g_continueCountdownSeconds--;
                updateContinueCountdownLabel();
            }
            if (g_continueCountdownSeconds <= 0) {
                stopContinueCountdown();
                showMainScreen();
            }
        }
    }

    if (!g_otaDisplayFreeze && (currentMillis - lastUpdate >= UPDATE_INTERVAL_MS)) {
        lastUpdate = currentMillis;
        updateClock();
    }

    if (!g_otaDisplayFreeze && currentUIState == UI_MAIN && weatherApiKey.length() > 0) {
        if (currentMillis - lastWeatherUpdate >= WEATHER_UPDATE_INTERVAL_MS) {
            updateWeather();
        }
    }
}