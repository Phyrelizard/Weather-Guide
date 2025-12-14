#include <Arduino.h>

/**
 * v2.0.7 - gave up on have seconds display updating correctly , just hours and minutes
 * v.2.0.6 - permanently assign x values for the clock so it sits still
 * v2.0.5 - make clock stationary, fix serial output scrolling, title font size larger
 * v2.0.4 - make clock on one line, bigger serial font,
 * v2.0.3 - fix backlight not turning on after reboot
 * v2.0.2 -  add continue button and coming soon message
 * v2.0.1 - minor bug fixes
 * v2.0.0 - major update web interface and OTA
 * 
 * * Weather Guide for Waveshare ESP32-S3 LCD 4.3"
 * 
 * Features:
 * - WiFiManager captive portal for easy WiFi setup
 * - Web interface for timezone configuration
 * - OTA firmware updates
 * - Displays clock in upper right corner
 * - Shows serial output on display
 * 
 * First boot:   Connect to "WeatherGuide_Setup" network to configure WiFi
 * Then access http://<device-ip>/ for timezone and OTA updates
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

// Firmware version
#define FIRMWARE_VERSION "v2.0.7"

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

// Web server
WebServer server(80);

// UI elements
static lv_obj_t *clock_label = NULL;
static lv_obj_t *ampm_label = NULL; 
static lv_obj_t *serial_output = NULL;
static lv_obj_t *title_label = NULL;
static lv_obj_t *continue_btn = NULL;
static lv_obj_t *coming_soon_label = NULL;
static lv_obj_t *backlight_btn = NULL;

// UI state
enum UIState {
    UI_SETUP,
    UI_MAIN
};
UIState currentUIState = UI_SETUP;

// Sleep/wake state
bool isBacklightOff = false;
ESP_IOExpander *expander = NULL;  // Make global

// Serial output buffer
#define MAX_SERIAL_LINES 20
#define MAX_LINE_LENGTH 100
String serialLines[MAX_SERIAL_LINES];
int currentLine = 0;

// Time string buffer
#define TIME_STRING_BUFFER_SIZE 16

// Update tracking
static unsigned long lastUpdate = 0;
const unsigned long UPDATE_INTERVAL_MS = 1000;
bool g_rebootRequired = false;

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

// Add line to serial output display
void addSerialLine(String line) {
    if (line.length() > MAX_LINE_LENGTH) {
        line = line.substring(0, MAX_LINE_LENGTH);
    }
    
    serialLines[currentLine] = line;
    currentLine = (currentLine + 1) % MAX_SERIAL_LINES;
    
    // Update display
    if (serial_output != NULL) {
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

// Custom Serial class to capture output
class DisplaySerial :  public Print {
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

// Load timezone settings
void loadTimezoneSettings() {
    preferences.begin("clock", false);
    gmtOffset_sec = preferences.getLong("gmtOffset", 0);
    daylightOffset_sec = preferences.getInt("dstOffset", 0);
    use24HourFormat = preferences.getBool("format24", true);
    preferences.end();
    DisplayLog.printf("Loaded timezone:  GMT=%ld, DST=%d, Format=%s\n", 
                     gmtOffset_sec, daylightOffset_sec, 
                     use24HourFormat ? "24h" : "12h");
}

// Save timezone settings
void saveTimezoneSettings() {
    preferences.begin("clock", false);
    preferences.putLong("gmtOffset", gmtOffset_sec);
    preferences.putInt("dstOffset", daylightOffset_sec);
    preferences.putBool("format24", use24HourFormat);
    preferences.end();
    DisplayLog.printf("Saved timezone: GMT=%ld, DST=%d, Format=%s\n", 
                     gmtOffset_sec, daylightOffset_sec,
                     use24HourFormat ? "24h" : "12h");
}

// OTA Update HTML (Cyberpunk theme) - WITH BETTER FEEDBACK
const char update_html[] PROGMEM = 
"<!DOCTYPE html><html><head>"
"<title>Weather Guide OTA</title>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<meta charset='UTF-8'>"
"<script src='https://ajax.googleapis.com/ajax/libs/jquery/3.2.1/jquery.min.js'></script>"
"<style>"
"*{margin:0;padding:0;box-sizing:border-box}"
"body{font-family:'Courier New',monospace;background: linear-gradient(135deg,#0a0a0a 0%,#1a0033 50%,#330066 100%);color:#0ff;min-height:100vh;display: flex;align-items:center;justify-content:center;padding:20px}"
". container{background:rgba(0,0,0,0.8);border:2px solid #0ff;border-radius:15px;padding:30px;max-width:500px;width:100%;box-shadow:0 0 30px rgba(0,255,255,0.5),inset 0 0 20px rgba(0,255,255,0.1)}"
"h1{text-align:center;font-size:2em;margin-bottom:10px;text-shadow:0 0 10px #0ff,0 0 20px #0ff;animation:glow 2s ease-in-out infinite}"
"@keyframes glow{0%,100%{text-shadow:0 0 10px #0ff,0 0 20px #0ff}50%{text-shadow:0 0 20px #0ff,0 0 30px #0ff,0 0 40px #0ff}}"
".info{background: rgba(0,255,255,0.1);border: 1px solid #0ff;border-radius:8px;padding:15px;margin:20px 0;text-align:center}"
". info p{margin:5px 0;font-size: 0.9em}"
"strong{color:#fff}"
". radio-group{margin:20px 0;padding:15px;background:rgba(0,255,255,0.05);border-radius:8px}"
".radio-option{display:flex;align-items:center;padding:10px;margin:5px 0;background:rgba(0,0,0,0.3);border:1px solid #0ff;border-radius:5px;cursor:pointer;transition:all 0.3s}"
".radio-option:hover{background:rgba(0,255,255,0.2);box-shadow:0 0 10px rgba(0,255,255,0.3)}"
".radio-option input{margin-right:10px;cursor:pointer}"
".file-input-wrapper{position:relative;overflow:hidden;display:block;margin:20px 0}"
"#file-label{display:block;padding:15px;background:rgba(0,255,255,0.1);border: 2px dashed #0ff;border-radius:8px;text-align:center;cursor:pointer;transition:all 0.3s}"
"#file-label:hover{background:rgba(0,255,255,0.2);box-shadow:0 0 15px rgba(0,255,255,0.3)}"
"#file-input{position:absolute;left:-9999px}"
"button{width:100%;padding:15px;margin:10px 0;background: linear-gradient(135deg,#0ff 0%,#00aaaa 100%);color:#000;border:none;border-radius:8px;font-size: 1.1em;font-weight:bold;cursor:pointer;transition:all 0.3s;text-transform:uppercase;box-shadow:0 4px 15px rgba(0,255,255,0.4)}"
"button:hover:not(:disabled){background:linear-gradient(135deg,#00ffff 0%,#00dddd 100%);box-shadow:0 6px 20px rgba(0,255,255,0.6);transform:translateY(-2px)}"
"button:disabled{background:#555;color:#999;cursor:not-allowed;box-shadow:none}"
"#progress-container{display:none;margin:20px 0;background:rgba(0,0,0,0.5);padding:10px;border-radius:8px;border:1px solid #0ff}"
"#progress-bar-bg{width:100%;height:30px;background: rgba(0,50,50,0.5);border-radius:5px;overflow:hidden;position:relative}"
"#progress-bar{width:0%;height:100%;background: linear-gradient(90deg,#0ff 0%,#00ff88 100%);border-radius:5px;transition: width 0.3s;box-shadow:0 0 10px rgba(0,255,255,0.8);position:relative}"
"#progress-bar::after{content:'';position: absolute;top: 0;left:0;bottom:0;right:0;background:linear-gradient(90deg,transparent,rgba(255,255,255,0.3),transparent);animation:shimmer 2s infinite}"
"@keyframes shimmer{0%{transform:translateX(-100%)}100%{transform:translateX(100%)}}"
"#status{text-align:center;margin:15px 0;font-size:1.2em;font-weight:bold;min-height:30px;text-shadow:0 0 5px currentColor}"
". status-uploading{color:#0ff;animation:pulse 1. 5s ease-in-out infinite}"
". status-success{color:#00ff88}"
".status-error{color:#ff4444}"
"@keyframes pulse{0%,100%{opacity:1}50%{opacity: 0.6}}"
"#upload-form. uploading{opacity:0.5;pointer-events:none}"
"</style>"
"</head><body>"
"<div class='container'>"
"<h1>FIRMWARE UPDATE</h1>"
"<div class='info'>"
"<p><strong>Device:</strong> Weather Guide</p>"
"<p><strong>Version:</strong> " FIRMWARE_VERSION "</p>"
"<p><strong>IP:</strong> <span id='device-ip'></span></p>"
"</div>"
"<form id='upload-form' enctype='multipart/form-data'>"
"<div class='radio-group'>"
"<label class='radio-option'>"
"<input type='radio' name='cmd' value='0' checked>"
"<span><strong>FIRMWARE</strong> (. bin)</span>"
"</label>"
"<label class='radio-option'>"
"<input type='radio' name='cmd' value='100'>"
"<span><strong>FILESYSTEM</strong> (LittleFS/SPIFFS)</span>"
"</label>"
"</div>"
"<div class='file-input-wrapper'>"
"<label id='file-label' for='file-input'>CLICK TO SELECT FILE</label>"
"<input type='file' id='file-input' name='update' accept='.bin'>"
"</div>"
"<button type='submit' id='submit-btn' disabled>UPLOAD & FLASH</button>"
"</form>"
"<div id='progress-container'>"
"<div id='progress-bar-bg'>"
"<div id='progress-bar'></div>"
"</div>"
"<div id='status'></div>"
"</div>"
"<button onclick='location.href=\"/\"' id='back-btn' style='background:linear-gradient(135deg,#666 0%,#444 100%)'>BACK TO HOME</button>"
"</div>"
"<script>"
"document.getElementById('device-ip').textContent=window.location.hostname;"
"var fileInput=document.getElementById('file-input');"
"var fileLabel=document.getElementById('file-label');"
"var submitBtn=document. getElementById('submit-btn');"
"var uploadForm=document.getElementById('upload-form');"
"var statusDiv=document.getElementById('status');"
"var progressContainer=document.getElementById('progress-container');"
"var progressBar=document. getElementById('progress-bar');"
"var backBtn=document.getElementById('back-btn');"
"fileInput.addEventListener('change',function(){"
"if(this.files. length>0){"
"fileLabel.innerHTML='Selected: <br><strong style=\"color:#fff\">'+this.files[0].name+'</strong>';"
"submitBtn.disabled=false;"
"}else{"
"fileLabel.innerHTML='CLICK TO SELECT FILE';"
"submitBtn.disabled=true;"
"}"
"});"
"uploadForm.addEventListener('submit',function(e){"
"e.preventDefault();"
"var formData=new FormData(uploadForm);"
"var cmd=document.querySelector('input[name=\"cmd\"]:checked').value;"
"var fileName=fileInput.files[0].name. toLowerCase();"
"if(cmd=='0'&&! fileName.includes('firmware')){"
"if(! confirm('You selected FIRMWARE mode, but filename does not contain \"firmware\". \\n\\nContinue?'))return;"
"}"
"if(cmd=='100'&&! fileName.includes('littlefs')&&!fileName.includes('spiffs')){"
"if(!confirm('You selected FILESYSTEM mode, but filename does not contain \"littlefs\". \\n\\nContinue? '))return;"
"}"
"uploadForm.classList.add('uploading');"
"submitBtn.disabled=true;"
"backBtn.disabled=true;"
"progressContainer.style.display='block';"
"progressBar.style.width='0%';"
"statusDiv.textContent='Starting upload.. .';"
"statusDiv.className='status-uploading';"
"var xhr=new XMLHttpRequest();"
"xhr.upload.addEventListener('progress',function(e){"
"if(e.lengthComputable){"
"var percent=Math.round((e.loaded/e.total)*100);"
"progressBar.style.width=percent+'%';"
"statusDiv. textContent='Uploading: '+percent+'%';"
"statusDiv.className='status-uploading';"
"}"
"});"
"xhr.addEventListener('load',function(){"
"if(xhr.status===200&&xhr.responseText==='OK'){"
"progressBar.style.width='100%';"
"statusDiv.textContent='SUCCESS!  Device is rebooting...';"
"statusDiv.className='status-success';"
"setTimeout(function(){"
"statusDiv.textContent='Reconnecting...';"
"setTimeout(function(){location.href='/';},3000);"
"},5000);"
"}else{"
"progressBar.style.width='0%';"
"statusDiv.textContent='UPDATE FAILED!  '+xhr.statusText;"
"statusDiv.className='status-error';"
"uploadForm.classList.remove('uploading');"
"submitBtn.disabled=false;"
"backBtn. disabled=false;"
"}"
"});"
"xhr.addEventListener('error',function(){"
"progressBar.style.width='0%';"
"statusDiv. textContent='UPLOAD ERROR! Check connection.';"
"statusDiv.className='status-error';"
"uploadForm.classList.remove('uploading');"
"submitBtn.disabled=false;"
"backBtn. disabled=false;"
"});"
"xhr.addEventListener('abort',function(){"
"progressBar.style.width='0%';"
"statusDiv.textContent='Upload cancelled.';"
"statusDiv.className='status-error';"
"uploadForm.classList.remove('uploading');"
"submitBtn.disabled=false;"
"backBtn. disabled=false;"
"});"
"xhr.open('POST','/update? cmd='+cmd,true);"
"xhr.send(formData);"
"});"
"</script></body></html>";

// Timezone configuration HTML
const char timezone_html[] PROGMEM = 
"<!DOCTYPE html><html><head>"
"<title>Weather Guide Config</title>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<meta charset='UTF-8'>"
"<style>"
"*{margin:0;padding:0;box-sizing:border-box}"
"body{font-family: Arial;background:#1a1a1a;color:#fff;padding:20px;text-align:center}"
"h1{color:#00ffff;margin: 20px 0}"
". container{max-width:600px;margin:auto;background:#2a2a2a;padding:20px;border-radius:10px;box-shadow:0 0 20px rgba(0,255,255,0.3)}"
". info{background:#3a3a3a;padding:15px;margin:20px 0;border-radius: 5px}"
"select,button{font-size:18px;padding:12px;margin:10px;width:90%;max-width:400px;border-radius:5px}"
"select{background:#2a2a2a;color:#fff;border:2px solid #00ffff}"
"button{background:#00ffff;border:none;cursor:pointer;color:#000;font-weight:bold}"
"button:hover{background:#00cccc}"
". nav-btn{background:#666;margin-top:20px}"
".nav-btn:hover{background:#888}"
"label{display:block;margin:15px 0 5px 0;font-weight:bold;color:#00ffff}"
". radio-group{margin:15px 0;padding:10px;background:#3a3a3a;border-radius:5px}"
".radio-option{display:inline-block;margin:10px 15px}"
".radio-option input{margin-right:5px}"
"</style>"
"</head><body>"
"<div class='container'>"
"<h1>Weather Guide</h1>"
"<div class='info'>"
"<p><strong>Firmware:</strong> " FIRMWARE_VERSION "</p>"
"<p><strong>IP:</strong> %IP%</p>"
"<p><strong>mDNS:</strong> http://weatherguide.local</p>"
"</div>"
"<form action='/settings' method='POST'>"
"<label for='timezone'>Select Timezone:</label>"
"<select name='timezone' id='timezone'>"
"<option value='-43200,0'>UTC-12 (Baker Island)</option>"
"<option value='-39600,0'>UTC-11 (American Samoa)</option>"
"<option value='-36000,0'>UTC-10 (Hawaii)</option>"
"<option value='-32400,0'>UTC-9 (Alaska)</option>"
"<option value='-28800,0'>UTC-8 (PST)</option>"
"<option value='-28800,3600'>UTC-8 (PDT)</option>"
"<option value='-25200,0'>UTC-7 (MST)</option>"
"<option value='-25200,3600'>UTC-7 (MDT)</option>"
"<option value='-21600,0'>UTC-6 (CST)</option>"
"<option value='-21600,3600'>UTC-6 (CDT)</option>"
"<option value='-18000,0'>UTC-5 (EST)</option>"
"<option value='-18000,3600'>UTC-5 (EDT)</option>"
"<option value='-14400,0'>UTC-4 (Atlantic)</option>"
"<option value='-10800,0'>UTC-3 (Brazil)</option>"
"<option value='0,0'>UTC+0 (GMT)</option>"
"<option value='3600,0'>UTC+1 (CET)</option>"
"<option value='3600,3600'>UTC+1 (CEST)</option>"
"<option value='7200,0'>UTC+2 (EET)</option>"
"<option value='10800,0'>UTC+3 (Moscow)</option>"
"<option value='14400,0'>UTC+4 (Dubai)</option>"
"<option value='19800,0'>UTC+5:30 (India)</option>"
"<option value='21600,0'>UTC+6 (Bangladesh)</option>"
"<option value='25200,0'>UTC+7 (Bangkok)</option>"
"<option value='28800,0'>UTC+8 (China)</option>"
"<option value='32400,0'>UTC+9 (Japan)</option>"
"<option value='36000,0'>UTC+10 (Sydney)</option>"
"<option value='43200,0'>UTC+12 (New Zealand)</option>"
"</select>"
"<label>Clock Format:</label>"
"<div class='radio-group'>"
"<label class='radio-option'>"
"<input type='radio' name='format' value='12' %FORMAT12%> 12 Hour (AM/PM)"
"</label>"
"<label class='radio-option'>"
"<input type='radio' name='format' value='24' %FORMAT24%> 24 Hour"
"</label>"
"</div>"
"<button type='submit'>Save Settings</button>"
"</form>"
"<div class='info' style='margin-top:20px'>"
"<p><strong>Current Settings:</strong></p>"
"<p>GMT: %GMT% sec | DST: %DST% sec</p>"
"<p>Format: %FORMATDISP%</p>"
"</div>"
"<button class='nav-btn' onclick='location.href=\"/update\"'>OTA Update</button>"
"<button class='nav-btn' onclick='location.href=\"/resetwifi\"'>Reset WiFi</button>"
"</div>"
"<script>"
"var currentTz='%GMT%,%DST%';"
"var select=document.getElementById('timezone');"
"for(var i=0;i<select.options.length;i++){"
"if(select.options[i].value===currentTz){select.selectedIndex=i;break;}"
"}"
"</script></body></html>";

void setupWebServer() {
    // Main page
    server.on("/", HTTP_GET, [](){
        String html = String(timezone_html);
        html. replace("%IP%", WiFi.localIP().toString());
        html.replace("%GMT%", String(gmtOffset_sec));
        html.replace("%DST%", String(daylightOffset_sec));
        html.replace("%FORMAT12%", use24HourFormat ? "" : "checked");
        html.replace("%FORMAT24%", use24HourFormat ? "checked" : "");
        html.replace("%FORMATDISP%", use24HourFormat ? "24 Hour" : "12 Hour (AM/PM)");
        server.send(200, "text/html", html);
    });
    
    // OTA update page
    server.on("/update", HTTP_GET, [](){
        server.send_P(200, "text/html", update_html);
    });
    
    // Handle settings
    server.on("/settings", HTTP_POST, [](){
        if(server.hasArg("timezone")) {
            String tz = server.arg("timezone");
            int commaIndex = tz.indexOf(',');
            if(commaIndex > 0) {
                gmtOffset_sec = tz.substring(0, commaIndex).toInt();
                daylightOffset_sec = tz.substring(commaIndex + 1).toInt();
            }
        }
        
        if(server.hasArg("format")) {
            use24HourFormat = (server.arg("format") == "24");
        }
        
        saveTimezoneSettings();
        configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
        DisplayLog. printf("Settings updated:  GMT=%ld, DST=%d, Format=%s\n", 
                         gmtOffset_sec, daylightOffset_sec,
                         use24HourFormat ? "24h" : "12h");
        
        server.sendHeader("Location", "/");
        server.send(303);
    });
    
    // Reset WiFi credentials
    server.on("/resetwifi", HTTP_GET, [](){
        server.send(200, "text/html", 
            "<html><body style='background:#000;color:#0ff;text-align:center;padding:50px;font-family:Arial'>"
            "<h1>WiFi Reset</h1><p>Credentials cleared.  Device will reboot into setup mode... </p></body></html>");
        delay(2000);
        WiFiManager wm;
        wm.resetSettings();
        ESP.restart();
    });
    
    // OTA update handler - FIXED
    server.on("/update", HTTP_POST, 
        // Response handler (after upload completes)
        []() {
            server.sendHeader("Connection", "close");
            server.send(200, "text/plain", (Update.hasError()) ? "FAIL" : "OK");
            g_rebootRequired = true;
        },
        // Upload handler (processes chunks as they arrive)
        []() {
            HTTPUpload& upload = server.upload();
            
            if (upload.status == UPLOAD_FILE_START) {
                DisplayLog.printf("Update Start: %s\n", upload. filename. c_str());
                
                // Get update type from query parameter
                int command = U_FLASH;
                String cmdStr = server.arg("cmd");
                
                if (cmdStr == "100") {
                    command = U_SPIFFS;
                    DisplayLog.println("Target:  Filesystem");
                } else {
                    DisplayLog.println("Target: Firmware");
                }
                
                // Start update
                if (! Update.begin(UPDATE_SIZE_UNKNOWN, command)) {
                    Update.printError(Serial);
                    DisplayLog.println("Update begin FAILED!");
                } else {
                    DisplayLog. println("Update started.. .");
                }
            } 
            else if (upload.status == UPLOAD_FILE_WRITE) {
                // Write chunk
                size_t written = Update.write(upload.buf, upload.currentSize);
                if (written != upload.currentSize) {
                    Update.printError(Serial);
                    DisplayLog.printf("Write error: %d/%d bytes\n", written, upload.currentSize);
                }
                
                // Show progress every 10%
                static uint32_t lastProgress = 0;
                if (Update.size() > 0) {
                    uint32_t progress = (Update.progress() * 100) / Update.size();
                    if (progress >= lastProgress + 10) {
                        DisplayLog.printf("Progress: %u%%\n", progress);
                        lastProgress = progress;
                    }
                }
            } 
            else if (upload. status == UPLOAD_FILE_END) {
                // Finish update
                if (Update. end(true)) {
                    DisplayLog.printf("Update SUCCESS:  %u bytes\n", upload.totalSize);
                    DisplayLog.println("Rebooting in 3 seconds...");
                } else {
                    Update.printError(Serial);
                    DisplayLog.println("Update END FAILED!");
                }
            }
            else if (upload.status == UPLOAD_FILE_ABORTED) {
                Update.end();
                DisplayLog.println("Update ABORTED!");
            }
        }
    );
    
    server.begin();
    DisplayLog.println("Web server started");
}

// Initialize WiFi with WiFiManager
void initWiFi() {
    WiFiManager wm;
    
    // Uncomment to force reset (then comment back out after one boot)
    // wm.resetSettings();
    
    wm.setConfigPortalTimeout(180); // 3 minute timeout
    wm.setConnectTimeout(20);       // 20 second connect timeout
    
    DisplayLog.println("Starting WiFi.. .");
    
    // Check if we have saved credentials
    if (WiFi. SSID() == "") {
        DisplayLog.println("=================================");
        DisplayLog.println("WIFI SETUP MODE");
        DisplayLog.println("=================================");
        DisplayLog.println("1. Connect phone/computer to:");
        DisplayLog.println("   Network: WeatherGuide_Setup");
        DisplayLog.println("   Password: weather123");
        DisplayLog.println("");
        DisplayLog.println("2. Browser should open automatically");
        DisplayLog.println("   If not, go to:  http://192.168.4.1");
        DisplayLog.println("");
        DisplayLog.println("3. Select your WiFi network");
        DisplayLog. println("4. Enter password and save");
        DisplayLog.println("=================================");
    }
    
    // Try to auto-connect with saved credentials
    if(! wm.autoConnect("WeatherGuide_Setup", "weather123")) {
        DisplayLog.println("Failed to connect!");
        DisplayLog.println("Starting config portal...");
        DisplayLog.println("");
        DisplayLog.println("Connect to:  WeatherGuide_Setup");
        DisplayLog.println("Password: weather123");
        DisplayLog.println("Then go to: http://192.168.4.1");
        
        // If autoConnect fails, explicitly start config portal
        if (! wm.startConfigPortal("WeatherGuide_Setup", "weather123")) {
            DisplayLog.println("Failed to start config portal, restarting...");
            delay(3000);
            ESP.restart();
        }
    }
    
    DisplayLog.println("WiFi connected!");
    DisplayLog.print("IP address: ");
    DisplayLog.println(WiFi.localIP());
    
    if (MDNS.begin(mdnsName)) {
        DisplayLog. printf("mDNS started: http://%s.local\n", mdnsName);
        MDNS.addService("http", "tcp", 80);
    }
}

// Initialize NTP
void initTime() {
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
    DisplayLog.println("Waiting for NTP sync...");
    
    struct tm timeinfo;
    int attempts = 0;
    while (!getLocalTime(&timeinfo) && attempts < 10) {
        DisplayLog.print(".");
        delay(1000);
        attempts++;
    }
    
    if (getLocalTime(&timeinfo)) {
        DisplayLog.println("Time synchronized!");
    } else {
        DisplayLog. println("Failed to obtain time");
    }
}

// Turn off backlight only (touch still works)
void backlightOff() {
    if (expander != NULL) {
        expander->digitalWrite(LCD_BL, LOW);
        isBacklightOff = true;
        DisplayLog.println("Backlight OFF");
    }
}

// Turn on backlight
void backlightOn() {
    if (expander != NULL) {
        expander->digitalWrite(LCD_BL, HIGH);
        isBacklightOff = false;
        DisplayLog.println("Backlight ON");
    }
}

// Toggle backlight
void toggleBacklight() {
    if (isBacklightOff) {
        backlightOn();
    } else {
        backlightOff();
    }
}

// Show continue button when setup is complete
void showContinueButton() {
    if (continue_btn != NULL && currentUIState == UI_SETUP) {
        lvgl_port_lock(-1);
        lv_obj_clear_flag(continue_btn, LV_OBJ_FLAG_HIDDEN);
        lvgl_port_unlock();
    }
}

// Switch to main screen
void showMainScreen() {
    currentUIState = UI_MAIN;
    
    lvgl_port_lock(-1);
    
    // Hide serial output and continue button
    if (serial_output != NULL) {
        lv_obj_add_flag(serial_output, LV_OBJ_FLAG_HIDDEN);
    }
    if (continue_btn != NULL) {
        lv_obj_add_flag(continue_btn, LV_OBJ_FLAG_HIDDEN);
    }
    
    // Show coming soon message
    if (coming_soon_label != NULL) {
        lv_obj_clear_flag(coming_soon_label, LV_OBJ_FLAG_HIDDEN);
    }
    
    lvgl_port_unlock();
}

// Global touch handler to wake backlight
void screen_touch_handler(lv_event_t * e) {
    if (isBacklightOff) {
        backlightOn();
    }
}

// Create UI
// Create UI
void createUI() {
    lv_obj_t *scr = lv_scr_act();
    
    lv_obj_set_style_bg_color(scr, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    
    // Add touch event to screen for wake-on-touch
    lv_obj_add_event_cb(scr, screen_touch_handler, LV_EVENT_PRESSED, NULL);
    
    // Title in upper left - FONT 24
    title_label = lv_label_create(scr);
    lv_obj_set_style_text_font(title_label, &lv_font_montserrat_24, LV_PART_MAIN);
    lv_obj_set_style_text_color(title_label, lv_color_hex(0x00FFFF), LV_PART_MAIN);
    lv_label_set_text(title_label, "Weather Guide " FIRMWARE_VERSION);
    lv_obj_align(title_label, LV_ALIGN_TOP_LEFT, 10, 5);
    
    // Backlight toggle button in center top
    backlight_btn = lv_btn_create(scr);
    lv_obj_set_size(backlight_btn, 100, 30);
    lv_obj_align(backlight_btn, LV_ALIGN_TOP_MID, 0, 5);
    lv_obj_set_style_bg_color(backlight_btn, lv_color_hex(0xFF6600), LV_PART_MAIN);
    lv_obj_set_style_radius(backlight_btn, 5, LV_PART_MAIN);
    
    lv_obj_t *backlight_label = lv_label_create(backlight_btn);
    lv_label_set_text(backlight_label, "Light");
    lv_obj_set_style_text_font(backlight_label, &lv_font_montserrat_12, LV_PART_MAIN);
    lv_obj_set_style_text_color(backlight_label, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_center(backlight_label);
    
    lv_obj_add_event_cb(backlight_btn, [](lv_event_t * e) {
        if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
            toggleBacklight();
        }
    }, LV_EVENT_CLICKED, NULL);
    
    // Clock in upper right - TIME ONLY (HH:MM, no seconds)
    clock_label = lv_label_create(scr);
    lv_obj_set_style_text_font(clock_label, &lv_font_montserrat_24, LV_PART_MAIN);
    lv_obj_set_style_text_color(clock_label, lv_color_hex(0xFFFF00), LV_PART_MAIN);
    lv_label_set_text(clock_label, "00:00");  // Just HH:MM
    lv_obj_set_width(clock_label, 80);  // Fixed width for HH:MM
    lv_obj_set_style_text_align(clock_label, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    lv_obj_align(clock_label, LV_ALIGN_TOP_RIGHT, -60, 5);  // Leave room for AM/PM

    // AM/PM label - right-aligned next to clock
    ampm_label = lv_label_create(scr);
    lv_obj_set_style_text_font(ampm_label, &lv_font_montserrat_24, LV_PART_MAIN);
    lv_obj_set_style_text_color(ampm_label, lv_color_hex(0xFFFF00), LV_PART_MAIN);
    lv_label_set_text(ampm_label, "AM");
    lv_obj_set_width(ampm_label, 50);
    lv_obj_set_style_text_align(ampm_label, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
    lv_obj_align(ampm_label, LV_ALIGN_TOP_RIGHT, -10, 5);  // 10px from right edge

    // Serial output - FONT 20 (smaller than 24, bigger than 14)
    serial_output = lv_label_create(scr);
    lv_obj_set_style_text_font(serial_output, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(serial_output, lv_color_hex(0x00FF00), LV_PART_MAIN);
    lv_label_set_long_mode(serial_output, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(serial_output, 780);
    lv_obj_set_height(serial_output, 360);  // Reduced height so it doesn't cut off
    lv_obj_align(serial_output, LV_ALIGN_TOP_LEFT, 10, 45);
    lv_label_set_text(serial_output, "Serial Output:\n");
    
    // Continue button (bottom right - initially hidden)
    continue_btn = lv_btn_create(scr);
    lv_obj_set_size(continue_btn, 120, 50);
    lv_obj_align(continue_btn, LV_ALIGN_BOTTOM_RIGHT, -10, -10);
    lv_obj_set_style_bg_color(continue_btn, lv_color_hex(0x00FFFF), LV_PART_MAIN);
    lv_obj_add_flag(continue_btn, LV_OBJ_FLAG_HIDDEN);  // Hidden initially
    
    lv_obj_t *btn_label = lv_label_create(continue_btn);
    lv_label_set_text(btn_label, "Continue");
    lv_obj_set_style_text_color(btn_label, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_center(btn_label);
    
    lv_obj_add_event_cb(continue_btn, [](lv_event_t * e) {
        if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
            showMainScreen();
        }
    }, LV_EVENT_CLICKED, NULL);
    
    // Coming soon label (initially hidden)
    coming_soon_label = lv_label_create(scr);
    lv_obj_set_style_text_font(coming_soon_label, &lv_font_montserrat_48, LV_PART_MAIN);
    lv_obj_set_style_text_color(coming_soon_label, lv_color_hex(0x00FFFF), LV_PART_MAIN);
    lv_label_set_text(coming_soon_label, "Weather Guide\nComing Soon");
    lv_obj_set_style_text_align(coming_soon_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(coming_soon_label, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(coming_soon_label, LV_OBJ_FLAG_HIDDEN);  // Hidden initially
}


// Update clock display
void updateClock() {
    struct tm timeinfo;
    if (getLocalTime(&timeinfo)) {
        char timeStr[16];
        char ampmStr[4];
        
        if (use24HourFormat) {
            strftime(timeStr, sizeof(timeStr), "%H:%M", &timeinfo);  // No seconds
            ampmStr[0] = '\0';  // Empty for 24-hour
        } else {
            strftime(timeStr, sizeof(timeStr), "%I:%M", &timeinfo);  // No seconds
            strftime(ampmStr, sizeof(ampmStr), "%p", &timeinfo);
        }
        
        lvgl_port_lock(-1);
        if (clock_label != NULL) {
            lv_label_set_text(clock_label, timeStr);
        }
        if (ampm_label != NULL) {
            lv_label_set_text(ampm_label, ampmStr);
        }
        lvgl_port_unlock();
    }
}

void setup()
{
    Serial.begin(115200);
    
    // Initialize NVS first
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_err = nvs_flash_init();
    }

    Serial.println("Weather Guide for Waveshare ESP32-S3 LCD 4.3\"");
    Serial.println("Firmware:  " FIRMWARE_VERSION);

    loadTimezoneSettings();

    panel = new ESP_Panel();
    lv_init();

    static lv_disp_draw_buf_t draw_buf;
    uint8_t *buf = (uint8_t *)heap_caps_calloc(1, LVGL_BUF_SIZE * sizeof(lv_color_t), MALLOC_CAP_INTERNAL);
    assert(buf);
    lv_disp_draw_buf_init(&draw_buf, buf, NULL, LVGL_BUF_SIZE);

    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv. hor_res = ESP_PANEL_LCD_H_RES;
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

    DisplayLog.println("Initialize IO expander");
    expander = new ESP_IOExpander_CH422G((i2c_port_t)I2C_MASTER_NUM, ESP_IO_EXPANDER_I2C_CH422G_ADDRESS_000);
    expander->init();
    expander->begin();
    expander->multiPinMode(TP_RST | LCD_BL | LCD_RST | SD_CS | USB_SEL, OUTPUT);
    expander->multiDigitalWrite(TP_RST | LCD_BL | LCD_RST | SD_CS, HIGH);
    expander->digitalWrite(USB_SEL, LOW);
    panel->addIOExpander(expander);
  
    // Ensure backlight is ON at startup (in case it was off before reset)
    expander->digitalWrite(LCD_BL, HIGH);
    isBacklightOff = false;
    DisplayLog.println("Backlight initialized:  ON");

    panel->begin();

    // Force backlight ON again after panel starts
    delay(100);  // Small delay to let panel initialize
    expander->digitalWrite(LCD_BL, HIGH);
    DisplayLog.println("Backlight forced ON after panel start");


    lvgl_mux = xSemaphoreCreateRecursiveMutex();
    xTaskCreate(lvgl_port_task, "lvgl", LVGL_TASK_STACK_SIZE, NULL, LVGL_TASK_PRIORITY, NULL);

    lvgl_port_lock(-1);
    createUI();
    lvgl_port_unlock();

    initWiFi();
    initTime();
    setupWebServer();

    DisplayLog.println("Setup complete!");
    DisplayLog.printf("Configuration: http://%s.local or http://%s/\n", mdnsName, WiFi.localIP().toString().c_str());
    showContinueButton();
}

void loop()
{
    server.handleClient();
    
    if (g_rebootRequired) {
        DisplayLog.println("Rebooting after OTA update...");
        delay(2000);
        ESP.restart();
    }
    
    unsigned long currentMillis = millis();
    if (currentMillis - lastUpdate >= UPDATE_INTERVAL_MS) {
        lastUpdate = currentMillis;
        updateClock();
    }
}