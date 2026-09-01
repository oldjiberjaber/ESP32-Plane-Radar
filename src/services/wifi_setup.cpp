#include "services/wifi_setup.h"

#include <ArduinoOTA.h>
#include <Update.h>
#include <WiFi.h>
#include <WiFiManager.h>

#include <cstdio>
#include <cstring>

#include <Preferences.h>
#include <esp_system.h>
#include <esp_wifi.h>

#ifdef WM_MDNS
#include <ESPmDNS.h>
#endif

#include "config.h"
#include "services/radar_location.h"
#include "ui/radar_range.h"
#include "ui/status_screens.h"

portMUX_TYPE s_boot_mux = portMUX_INITIALIZER_UNLOCKED;
volatile bool s_boot_tap_pending = false;
volatile bool s_boot_is_down = false;
volatile unsigned long s_boot_down_ms = 0;
bool s_long_press_handled = false;
bool s_boot_interrupt_attached = false;

void IRAM_ATTR onBootButtonIsr() {
  const bool down = digitalRead(config::kBootPin) == LOW;
  const unsigned long now = millis();
  portENTER_CRITICAL_ISR(&s_boot_mux);
  if (down) {
    s_boot_is_down = true;
    s_boot_down_ms = now;
  } else if (s_boot_is_down) {
    const unsigned long held = now - s_boot_down_ms;
    if (held >= config::kBootTapMinMs && held < config::kBootResetHoldMs) {
      s_boot_tap_pending = true;
    }
    s_boot_is_down = false;
  }
  portEXIT_CRITICAL_ISR(&s_boot_mux);
}

void initBootButton() {
  pinMode(config::kBootPin, INPUT_PULLUP);
  if (s_boot_interrupt_attached) {
    return;
  }
  attachInterrupt(digitalPinToInterrupt(static_cast<uint8_t>(config::kBootPin)),
                  onBootButtonIsr, CHANGE);
  s_boot_interrupt_attached = true;
}

namespace {

/** Separate from planeradar prefs (rangeInit) to avoid NVS handle conflicts. */
constexpr char kWifiPrefsNamespace[] = "wifi";
constexpr char kPrefsForcePortalKey[] = "portal";

bool s_force_config_portal = false;
WiFiManager s_wm;
bool s_wm_configured = false;

void ensureWifiManager();
void startLanWebPortal();
void stopLanWebPortal();
bool wifiLinkUp();

constexpr int kCoordParamLen = 20;
constexpr char kCoordInputAttrs[] =
    " inputmode=\"decimal\" placeholder=\"e.g. 52.367600\"";

constexpr char kCustomHead[] =
    "<style>"
    ".radar-card{background:rgba(128,128,128,0.12);border:1px solid rgba(128,128,128,0.3);border-radius:8px;padding:12px;margin:12px 0 16px 0;text-align:center;}"
    ".radar-btn-map{background-color:#107c41!important;color:#fff!important;border:none!important;padding:10px 14px!important;border-radius:6px!important;font-weight:bold!important;cursor:pointer!important;width:100%!important;font-size:14px!important;margin:4px 0 8px 0!important;display:block!important;}"
    ".radar-btn-ip{background-color:#0078d4!important;color:#fff!important;border:none!important;padding:9px 12px!important;border-radius:6px!important;font-size:13px!important;cursor:pointer!important;width:100%!important;margin:4px 0 8px 0!important;display:block!important;}"
    ".radar-btn-gps{background-color:#4a5568!important;color:#fff!important;border:none!important;padding:8px 12px!important;border-radius:6px!important;font-size:13px!important;cursor:pointer!important;width:100%!important;margin:4px 0 8px 0!important;display:block!important;}"
    "#map-view{height:250px;width:100%;border-radius:6px;margin:10px 0 6px 0;border:1px solid rgba(128,128,128,0.4);display:none;z-index:10;}"
    "#gps-status{font-size:12px;line-height:1.4;margin-top:6px;min-height:16px;}"
    "</style>";

constexpr char kGpsToolsHtml[] =
    "<div class='radar-card'>"
    "<div style='font-size:15px;font-weight:bold;margin-bottom:8px;'>&#x1F4CD; Radar Center Location</div>"
    "<button type='button' class='radar-btn-map' onclick='openMapPicker()'>&#x1F5FA;&#xFE0F; Pick Location on Interactive Map</button>"
    "<button type='button' class='radar-btn-ip' onclick='fetchIpLocation()'>&#x1F310; Auto-Detect via IP</button>"
    "<button type='button' class='radar-btn-gps' onclick='fetchPhoneGps()'>&#x1F4CD; Use Phone GPS Sensor</button>"
    "<div id='map-view'></div>"
    "<div id='gps-status'></div>"
    "</div>"
    "<script>"
    "function openMapPicker(){"
    "var mapEl=document.getElementById('map-view');"
    "var s=document.getElementById('gps-status');"
    "mapEl.style.display='block';"
    "var elLat=document.getElementById('radar_lat')||document.querySelector('input[name=\"radar_lat\"]');"
    "var elLon=document.getElementById('radar_lon')||document.querySelector('input[name=\"radar_lon\"]');"
    "var curLat=parseFloat(elLat?elLat.value:0)||52.3676;"
    "var curLon=parseFloat(elLon?elLon.value:0)||4.9041;"
    "function initMap(){"
    "if(window._radarMap){"
    "window._radarMap.invalidateSize();"
    "window._radarMap.setView([curLat,curLon],14);"
    "if(window._radarMarker) window._radarMarker.setLatLng([curLat,curLon]);"
    "return;"
    "}"
    "var map=L.map('map-view').setView([curLat,curLon],14);"
    "window._radarMap=map;"
    "L.tileLayer('https://tile.openstreetmap.org/{z}/{x}/{y}.png',{maxZoom:19,attribution:'&copy; OSM'}).addTo(map);"
    "var marker=L.marker([curLat,curLon],{draggable:true}).addTo(map);"
    "window._radarMarker=marker;"
    "function updatePos(lat,lon){"
    "if(elLat) elLat.value=lat.toFixed(6);"
    "if(elLon) elLon.value=lon.toFixed(6);"
    "s.innerHTML='<span style=\"color:#28a745;font-weight:bold;\">&#x2714; Selected: '+lat.toFixed(6)+', '+lon.toFixed(6)+'</span>';"
    "}"
    "marker.on('dragend',function(e){var p=e.target.getLatLng();updatePos(p.lat,p.lng);});"
    "map.on('click',function(e){marker.setLatLng(e.latlng);updatePos(e.latlng.lat,e.latlng.lng);});"
    "s.innerHTML='<span style=\"color:#107c41;font-weight:bold;\">&#x2705; Map active: Tap anywhere or drag pin</span>';"
    "setTimeout(function(){map.invalidateSize();},250);"
    "}"
    "if(window.L){"
    "initMap();"
    "}else{"
    "s.innerHTML='<span style=\"color:#0078d4;\">&#x23F3; Loading map...</span>';"
    "var css=document.createElement('link');"
    "css.rel='stylesheet';"
    "css.href='https://unpkg.com/leaflet@1.9.4/dist/leaflet.css';"
    "document.head.appendChild(css);"
    "var js=document.createElement('script');"
    "js.src='https://unpkg.com/leaflet@1.9.4/dist/leaflet.js';"
    "js.onload=function(){initMap();};"
    "js.onerror=function(){s.innerHTML='<span style=\"color:#d9534f;\">Map could not be loaded. Use Auto-Detect via IP or enter coordinates manually.</span>';};"
    "document.head.appendChild(js);"
    "}"
    "}"
    "function fetchPhoneGps(){"
    "var s=document.getElementById('gps-status');"
    "if(!navigator.geolocation){"
    "s.innerHTML='<span style=\"color:#d9534f\">&#x26A0; Geolocation is not supported by this browser.</span>';"
    "return;"
    "}"
    "s.innerHTML='<span style=\"color:#0078d4\">&#x23F3; Requesting GPS position from device...</span>';"
    "navigator.geolocation.getCurrentPosition("
    "function(pos){"
    "var lat=pos.coords.latitude.toFixed(6);"
    "var lon=pos.coords.longitude.toFixed(6);"
    "var acc=Math.round(pos.coords.accuracy||0);"
    "var elLat=document.getElementById('radar_lat')||document.querySelector('input[name=\"radar_lat\"]');"
    "var elLon=document.getElementById('radar_lon')||document.querySelector('input[name=\"radar_lon\"]');"
    "if(elLat)elLat.value=lat;"
    "if(elLon)elLon.value=lon;"
    "s.innerHTML='<span style=\"color:#28a745;font-weight:bold;\">&#x2714; Acquired: '+lat+', '+lon+(acc?' (&plusmn;'+acc+'m)':'')+'</span>';"
    "if(window._radarMap){"
    "window._radarMap.setView([parseFloat(lat),parseFloat(lon)],15);"
    "if(window._radarMarker) window._radarMarker.setLatLng([parseFloat(lat),parseFloat(lon)]);"
    "}"
    "},"
    "function(err){"
    "var m='<div style=\"color:#d9534f;margin-bottom:6px;\">&#x26A0; Browser blocked direct GPS (Chrome/Safari require HTTPS for phone GPS).</div>';"
    "m+='<div style=\"font-size:12px;color:#555;margin-bottom:8px;\">Please tap <strong>Pick Location on Interactive Map</strong> above to select your location visually.</div>';"
    "s.innerHTML=m;"
    "},"
    "{enableHighAccuracy:true,timeout:10000,maximumAge:0}"
    ");"
    "}"
    "function fetchIpLocation(){"
    "var s=document.getElementById('gps-status');"
    "s.innerHTML='<span style=\"color:#0078d4\">&#x23F3; Detecting location from IP...</span>';"
    "fetch('/api/geolocate')"
    ".then(function(r){return r.json();})"
    ".then(function(d){"
    "if(d&&d.lat&&d.lon){"
    "var lat=Number(d.lat).toFixed(6);"
    "var lon=Number(d.lon).toFixed(6);"
    "var elLat=document.getElementById('radar_lat')||document.querySelector('input[name=\"radar_lat\"]');"
    "var elLon=document.getElementById('radar_lon')||document.querySelector('input[name=\"radar_lon\"]');"
    "if(elLat)elLat.value=lat;"
    "if(elLon)elLon.value=lon;"
    "s.innerHTML='<span style=\"color:#28a745;font-weight:bold;\">&#x2714; IP Location: '+lat+', '+lon+'</span>';"
    "if(window._radarMap){"
    "window._radarMap.setView([parseFloat(lat),parseFloat(lon)],14);"
    "if(window._radarMarker) window._radarMarker.setLatLng([parseFloat(lat),parseFloat(lon)]);"
    "}"
    "}else{throw new Error();}"
    "})"
    ".catch(function(){"
    "fetch('https://ipapi.co/json/')"
    ".then(function(r){return r.json();})"
    ".then(function(d){"
    "if(d&&d.latitude&&d.longitude){"
    "var lat=Number(d.latitude).toFixed(6);"
    "var lon=Number(d.longitude).toFixed(6);"
    "var elLat=document.getElementById('radar_lat')||document.querySelector('input[name=\"radar_lat\"]');"
    "var elLon=document.getElementById('radar_lon')||document.querySelector('input[name=\"radar_lon\"]');"
    "if(elLat)elLat.value=lat;"
    "if(elLon)elLon.value=lon;"
    "s.innerHTML='<span style=\"color:#28a745;font-weight:bold;\">&#x2714; IP Location ('+(d.city||'Detected')+'): '+lat+', '+lon+'</span>';"
    "if(window._radarMap){"
    "window._radarMap.setView([parseFloat(lat),parseFloat(lon)],14);"
    "if(window._radarMarker) window._radarMarker.setLatLng([parseFloat(lat),parseFloat(lon)]);"
    "}"
    "}else{throw new Error();}"
    "})"
    ".catch(function(){"
    "s.innerHTML='<span style=\"color:#d9534f\">IP lookup unavailable offline. Radar will auto-detect from IP once connected to Wi-Fi.</span>';"
    "});"
    "});"
    "}"
    "try{"
    "var p=new URLSearchParams(window.location.search);"
    "var qLat=p.get('radar_lat')||p.get('lat');"
    "var qLon=p.get('radar_lon')||p.get('lon');"
    "if(qLat&&qLon){"
    "var eLa=document.getElementById('radar_lat')||document.querySelector('input[name=\"radar_lat\"]');"
    "var eLo=document.getElementById('radar_lon')||document.querySelector('input[name=\"radar_lon\"]');"
    "if(eLa)eLa.value=qLat;"
    "if(eLo)eLo.value=qLon;"
    "}"
    "}catch(e){}"
    "</script>";

WiFiManagerParameter s_param_gps_tools(kGpsToolsHtml);
WiFiManagerParameter s_param_lat("radar_lat", "Latitude (deg)", "0",
                                kCoordParamLen, kCoordInputAttrs);
WiFiManagerParameter s_param_lon("radar_lon", "Longitude (deg)", "0",
                                kCoordParamLen, kCoordInputAttrs);

char s_miles_checkbox_attrs[32] = "type=\"checkbox\"";
WiFiManagerParameter s_param_miles("use_miles", "Display distances in miles", "T", 2,
                                   s_miles_checkbox_attrs, WFM_LABEL_AFTER);

char s_runways_checkbox_attrs[32] = "type=\"checkbox\"";
WiFiManagerParameter s_param_runways("show_runways", "Show airport runways", "T", 2,
                                     s_runways_checkbox_attrs, WFM_LABEL_AFTER);

void refreshPortalParamDefaults() {
  char lat_buf[kCoordParamLen + 1];
  char lon_buf[kCoordParamLen + 1];
  snprintf(lat_buf, sizeof(lat_buf), "%.6f", services::location::lat());
  snprintf(lon_buf, sizeof(lon_buf), "%.6f", services::location::lon());
  s_param_lat.setValue(lat_buf, kCoordParamLen);
  s_param_lon.setValue(lon_buf, kCoordParamLen);
  snprintf(s_miles_checkbox_attrs, sizeof(s_miles_checkbox_attrs), "type=\"checkbox\"%s",
           ui::radar::useMiles() ? " checked" : "");
  s_param_miles.setValue("T", 2);
  snprintf(s_runways_checkbox_attrs, sizeof(s_runways_checkbox_attrs),
           "type=\"checkbox\"%s", ui::radar::showRunways() ? " checked" : "");
  s_param_runways.setValue("T", 2);
}

void onPortalParamsSaved() {
  if (!services::location::saveFromStrings(s_param_lat.getValue(),
                                           s_param_lon.getValue())) {
    Serial.println("Invalid lat/lon in portal — keeping previous location");
  }
  ui::radar::saveMilesFromPortal(s_param_miles.getValue());
  ui::radar::saveRunwaysFromPortal(s_param_runways.getValue());
}

void attachPortalParams(WiFiManager& wm) {
  refreshPortalParamDefaults();
  wm.addParameter(&s_param_gps_tools);
  wm.addParameter(&s_param_lat);
  wm.addParameter(&s_param_lon);
  wm.addParameter(&s_param_miles);
  wm.addParameter(&s_param_runways);
  wm.setSaveParamsCallback(onPortalParamsSaved);
}

void markForceConfigPortal() {
  s_force_config_portal = true;
  Preferences prefs;
  if (!prefs.begin(kWifiPrefsNamespace, false)) {
    return;
  }
  prefs.putBool(kPrefsForcePortalKey, true);
  prefs.end();
}

bool consumeForceConfigPortal() {
  if (s_force_config_portal) {
    s_force_config_portal = false;
    Preferences prefs;
    if (prefs.begin(kWifiPrefsNamespace, false)) {
      prefs.remove(kPrefsForcePortalKey);
      prefs.end();
    }
    return true;
  }

  Preferences prefs;
  if (!prefs.begin(kWifiPrefsNamespace, true)) {
    return false;
  }
  const bool pending = prefs.getBool(kPrefsForcePortalKey, false);
  prefs.end();
  if (!pending) {
    return false;
  }

  if (prefs.begin(kWifiPrefsNamespace, false)) {
    prefs.remove(kPrefsForcePortalKey);
    prefs.end();
  }
  return true;
}

bool storedWifiCredentials() {
  wifi_mode_t mode = WIFI_MODE_NULL;
  if (esp_wifi_get_mode(&mode) != ESP_OK || mode == WIFI_MODE_NULL) {
    WiFi.mode(WIFI_STA);
    delay(50);
  }

  wifi_config_t conf = {};
  if (esp_wifi_get_config(WIFI_IF_STA, &conf) != ESP_OK) {
    return false;
  }
  return conf.sta.ssid[0] != '\0';
}

void eraseWifiCredentials() {
  stopLanWebPortal();
  WiFi.setAutoReconnect(false);
  WiFi.mode(WIFI_OFF);
  delay(100);

  ensureWifiManager();
  WiFi.persistent(true);
  s_wm.resetSettings();
  s_wm.erase();
  WiFi.disconnect(true, true);
  WiFi.persistent(false);

  WiFi.mode(WIFI_OFF);
  delay(100);
}

void resetWifiCredentials() {
  markForceConfigPortal();
  eraseWifiCredentials();
  services::location::clear();
  ui::radar::unitsReset();
  Serial.println("WiFi credentials, location, and units cleared");
}

void onConfigPortalApStarted(WiFiManager*) {
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
  statusScreenPortal();
#ifdef WM_MDNS
  if (MDNS.begin(config::kPortalHostname)) {
    MDNS.addService("http", "tcp", 80);
    Serial.printf("Setup portal: http://%s.local (or http://%s)\n",
                  config::kPortalHostname, config::kPortalIp);
  } else {
    Serial.printf("Setup portal: http://%s (mDNS unavailable)\n", config::kPortalIp);
  }
#else
  Serial.printf("Setup portal: http://%s\n", config::kPortalIp);
#endif
}

bool wifiLinkUp() {
  return WiFi.status() == WL_CONNECTED &&
         WiFi.localIP() != IPAddress(0, 0, 0, 0);
}

void setupCustomWebRoutes() {
  if (s_wm.server) {
    s_wm.server->on("/api/geolocate", HTTP_GET, []() {
      if (services::location::fetchFromIpGeolocation()) {
        char json[128];
        snprintf(json, sizeof(json), "{\"success\":true,\"lat\":%.6f,\"lon\":%.6f}",
                 services::location::lat(), services::location::lon());
        s_wm.server->send(200, "application/json", json);
      } else {
        s_wm.server->send(500, "application/json", "{\"success\":false}");
      }
    });
  }
}

void ensureWifiManager() {
  if (s_wm_configured) {
    return;
  }
  s_wm.setConfigPortalTimeout(config::kWifiPortalTimeoutSec);
  s_wm.setAPStaticIPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1),
                           IPAddress(255, 255, 255, 0));
  s_wm.setHostname(config::kPortalHostname);
  s_wm.setTitle("Plane Radar");

  std::vector<const char*> menu = {"wifi", "param", "info", "update", "exit"};
  s_wm.setMenu(menu);
  s_wm.setCustomHeadElement(kCustomHead);

  s_wm.setAPCallback(onConfigPortalApStarted);
  s_wm.setWebServerCallback(setupCustomWebRoutes);
  attachPortalParams(s_wm);
  s_wm_configured = true;
}

void startLanWebPortal() {
  if (!wifiLinkUp() || s_wm.getWebPortalActive() ||
      s_wm.getConfigPortalActive()) {
    return;
  }
  refreshPortalParamDefaults();
  WiFi.mode(WIFI_STA);
  s_wm.setConfigPortalBlocking(false);
#ifdef WM_MDNS
  MDNS.end();
  if (MDNS.begin(config::kPortalHostname)) {
    MDNS.addService("http", "tcp", 80);
  }
#endif
  s_wm.startWebPortal();
  Serial.printf("LAN config: http://%s.local or http://%s\n",
                config::kPortalHostname, WiFi.localIP().toString().c_str());
}

void stopLanWebPortal() {
  if (!s_wm.getWebPortalActive()) {
    return;
  }
  s_wm.stopWebPortal();
#ifdef WM_MDNS
  MDNS.end();
#endif
}

void prepareSta() {
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(WIFI_PS_NONE);
  WiFi.setAutoReconnect(true);
}

void startStaConnect(const String& ssid, const String& pass) {
  prepareSta();
  if (ssid.length() > 0) {
    WiFi.begin(ssid.c_str(), pass.c_str());
  } else {
    WiFi.begin();
  }
}

bool waitForLinkWithUi(const char* ssid_for_ui, unsigned long attempt_ms) {
  const unsigned long deadline = millis() + attempt_ms;
  while (millis() < deadline) {
    if (wifiLinkUp()) {
      return true;
    }
    bootButtonPollLongPress();
    statusScreenConnectingTick();
    delay(config::kWifiConnectingFrameMs);
  }
  return wifiLinkUp();
}

bool tryConnectWithUi(const String& ssid, const String& pass, bool show_ui) {
  if (wifiLinkUp()) {
    return true;
  }

  const char* ui_ssid = ssid.length() > 0 ? ssid.c_str() : "network";
  if (show_ui) {
    statusScreenConnectingBegin(ui_ssid);
  }

  for (uint8_t attempt = 1; attempt <= config::kWifiConnectAttempts; ++attempt) {
    if (attempt > 1) {
      Serial.printf("WiFi connect retry %u/%u\n", attempt,
                    config::kWifiConnectAttempts);
      WiFi.disconnect(true);
      WiFi.mode(WIFI_OFF);
      delay(400);
    }

    startStaConnect(ssid, pass);

    if (waitForLinkWithUi(ui_ssid, config::kWifiConnectAttemptMs)) {
      return true;
    }
  }

  return false;
}

bool connectSavedNetwork(bool show_ui) {
  wifi_mode_t mode = WIFI_MODE_NULL;
  if (esp_wifi_get_mode(&mode) != ESP_OK || mode == WIFI_MODE_NULL) {
    WiFi.mode(WIFI_STA);
    delay(50);
  }

  wifi_config_t conf = {};
  if (esp_wifi_get_config(WIFI_IF_STA, &conf) != ESP_OK) {
    return false;
  }

  if (conf.sta.ssid[0] == '\0') {
    return false;
  }

  // ESP-IDF stores the SSID in a fixed 32-byte field. A maximum-length
  // SSID has no room for a trailing NUL, so copy it to a larger buffer
  // and explicitly terminate it before constructing an Arduino String.
  char ssid_buf[sizeof(conf.sta.ssid) + 1] = {};
  memcpy(ssid_buf, conf.sta.ssid, sizeof(conf.sta.ssid));
  ssid_buf[sizeof(conf.sta.ssid)] = '\0';

  char pass_buf[sizeof(conf.sta.password) + 1] = {};
  memcpy(pass_buf, conf.sta.password, sizeof(conf.sta.password));
  pass_buf[sizeof(conf.sta.password)] = '\0';

  const String ssid(ssid_buf);
  const String pass(pass_buf);

  return tryConnectWithUi(ssid, pass, show_ui);
}

bool openConfigPortal() {
  stopLanWebPortal();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  delay(50);
  statusScreenPortal();
  refreshPortalParamDefaults();
  s_wm.setConfigPortalBlocking(false);
  s_wm.startConfigPortal(config::kPortalApName);
  while (s_wm.getConfigPortalActive()) {
    bootButtonPollLongPress();
    if (s_wm.process()) {
      return true;
    }
    delay(10);
  }
  return wifiLinkUp();
}

}  // namespace

bool wifiShowsSetupScreenOnBoot() {
  if (s_force_config_portal) {
    return true;
  }
  Preferences prefs;
  if (!prefs.begin(kWifiPrefsNamespace, true)) {
    return false;
  }
  const bool pending = prefs.getBool(kPrefsForcePortalKey, false);
  prefs.end();
  return pending;
}

bool wifiBootButtonPressed() {
  return digitalRead(config::kBootPin) == LOW;
}

void bootButtonInit() { initBootButton(); }

bool bootButtonConsumeTap() {
  portENTER_CRITICAL(&s_boot_mux);
  const bool tap = s_boot_tap_pending;
  if (tap) {
    s_boot_tap_pending = false;
  }
  portEXIT_CRITICAL(&s_boot_mux);
  return tap;
}

void bootButtonPollLongPress() {
  if (wifiBootButtonPressed()) {
    portENTER_CRITICAL(&s_boot_mux);
    if (!s_boot_is_down) {
      s_boot_is_down = true;
      s_boot_down_ms = millis();
    }
    const unsigned long down_ms = s_boot_down_ms;
    portEXIT_CRITICAL(&s_boot_mux);

    if (!s_long_press_handled &&
        millis() - down_ms >= config::kBootResetHoldMs) {
      s_long_press_handled = true;
      Serial.println("BOOT held — resetting WiFi");
      wifiResetCredentialsAndReboot();
    }
  } else {
    portENTER_CRITICAL(&s_boot_mux);
    s_boot_is_down = false;
    portEXIT_CRITICAL(&s_boot_mux);
    s_long_press_handled = false;
  }
}

void wifiResetCredentialsAndReboot() {
  resetWifiCredentials();
  statusScreenWifiReset();
  delay(800);
  esp_restart();
}

bool wifiReconnect() {
  initBootButton();
  Serial.println("WiFi reconnecting...");
  return connectSavedNetwork(true);
}

void setupOtaProgressHooks() {
  static bool s_ota_hooks_initialized = false;
  if (s_ota_hooks_initialized) {
    return;
  }
  s_ota_hooks_initialized = true;

  Update.onProgress([](size_t progress, size_t size) {
    static bool s_update_started = false;
    if (!s_update_started) {
      statusScreenUpdateBegin("Firmware Update");
      s_update_started = true;
    }
    if (size > 0) {
      const int percent = static_cast<int>((progress * 100) / size);
      statusScreenUpdateProgress(percent);
    }
  });

  s_wm.setPreOtaUpdateCallback([]() {
    statusScreenUpdateBegin("Web OTA Update");
  });

  ArduinoOTA.onStart([]() {
    statusScreenUpdateBegin("OTA Update");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    if (total > 0) {
      const int percent = static_cast<int>((progress * 100) / total);
      statusScreenUpdateProgress(percent);
    }
  });
  ArduinoOTA.onEnd([]() {
    statusScreenUpdateEnd();
  });
  ArduinoOTA.onError([](ota_error_t error) {
    const char* err_str = "Update Error";
    if (error == OTA_AUTH_ERROR) err_str = "Auth Failed";
    else if (error == OTA_BEGIN_ERROR) err_str = "Begin Failed";
    else if (error == OTA_CONNECT_ERROR) err_str = "Connect Failed";
    else if (error == OTA_RECEIVE_ERROR) err_str = "Receive Failed";
    else if (error == OTA_END_ERROR) err_str = "End Failed";
    statusScreenUpdateError(err_str);
  });
  ArduinoOTA.setHostname(config::kPortalHostname);
  ArduinoOTA.begin();
}

void wifiLoop() {
  ensureWifiManager();
  if (wifiLinkUp()) {
    setupOtaProgressHooks();
    ArduinoOTA.handle();
    if (!s_wm.getWebPortalActive() && !s_wm.getConfigPortalActive()) {
      startLanWebPortal();
    }
    if (s_wm.getWebPortalActive() || s_wm.getConfigPortalActive()) {
      bootButtonPollLongPress();
      s_wm.process();
    }
  } else {
    stopLanWebPortal();
  }
}

bool wifiSetupConnect() {
  initBootButton();
  ensureWifiManager();

  const bool force_portal = consumeForceConfigPortal();
  WiFi.setAutoReconnect(false);

  if (force_portal) {
    eraseWifiCredentials();
    WiFi.mode(WIFI_OFF);
    delay(100);
  }

  if (force_portal) {
    Serial.println("Opening WiFi setup portal (after reset)");
    if (openConfigPortal() && wifiLinkUp()) {
      WiFi.setAutoReconnect(true);
      if (!services::location::isConfigured()) {
        services::location::fetchFromIpGeolocation();
      }
      Serial.printf("Connected: %s  IP %s\n", WiFi.SSID().c_str(),
                    WiFi.localIP().toString().c_str());
      return true;
    }
    Serial.println("WiFi connection failed");
    statusScreenConnectFailed();
    return false;
  }

  Serial.println("Connecting to WiFi (portal opens if needed)...");

  if (wifiLinkUp()) {
    WiFi.setAutoReconnect(true);
    if (!services::location::isConfigured()) {
      services::location::fetchFromIpGeolocation();
    }
    Serial.printf("Connected: %s  IP %s\n", WiFi.SSID().c_str(),
                  WiFi.localIP().toString().c_str());
    return true;
  }

  if (storedWifiCredentials() && connectSavedNetwork(true)) {
    WiFi.setAutoReconnect(true);
    if (!services::location::isConfigured()) {
      services::location::fetchFromIpGeolocation();
    }
    Serial.printf("Connected: %s  IP %s\n", WiFi.SSID().c_str(),
                  WiFi.localIP().toString().c_str());
    return true;
  }

  if (storedWifiCredentials()) {
    Serial.println("Saved WiFi could not connect — opening setup portal");
  } else {
    Serial.println("No saved WiFi — opening setup portal");
  }

  if (openConfigPortal() && wifiLinkUp()) {
    WiFi.setAutoReconnect(true);
    if (!services::location::isConfigured()) {
      services::location::fetchFromIpGeolocation();
    }
    Serial.printf("Connected: %s  IP %s\n", WiFi.SSID().c_str(),
                  WiFi.localIP().toString().c_str());
    return true;
  }

  Serial.println("WiFi connection failed");
  statusScreenConnectFailed();
  return false;
}
