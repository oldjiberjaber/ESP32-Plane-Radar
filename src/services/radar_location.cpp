#include "services/radar_location.h"

#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFi.h>
#include <ArduinoJson.h>
#include <cstdlib>
#include <cstring>

#include "config.h"

namespace services::location {

namespace {

constexpr char kPrefsNamespace[] = "radar";
constexpr char kKeyLat[] = "lat";
constexpr char kKeyLon[] = "lon";
constexpr char kKeyCustom[] = "custom";

double s_lat = config::kDefaultRadarLat;
double s_lon = config::kDefaultRadarLon;
bool s_configured = false;

bool parseCoord(const char* text, double* out) {
  if (text == nullptr || text[0] == '\0') {
    return false;
  }
  char* end = nullptr;
  const double v = strtod(text, &end);
  if (end == text || (end != nullptr && *end != '\0')) {
    return false;
  }
  *out = v;
  return true;
}

bool validLatLon(double lat, double lon) {
  return lat >= -90.0 && lat <= 90.0 && lon >= -180.0 && lon <= 180.0;
}

void persist(double lat, double lon, bool is_custom) {
  Preferences prefs;
  prefs.begin(kPrefsNamespace, false);
  prefs.putDouble(kKeyLat, lat);
  prefs.putDouble(kKeyLon, lon);
  prefs.putBool(kKeyCustom, is_custom);
  prefs.end();
  s_lat = lat;
  s_lon = lon;
  s_configured = is_custom;
}

}  // namespace

void init() {
  Preferences prefs;
  prefs.begin(kPrefsNamespace, true);
  if (prefs.isKey(kKeyLat) && prefs.isKey(kKeyLon)) {
    const double lat = prefs.getDouble(kKeyLat, config::kDefaultRadarLat);
    const double lon = prefs.getDouble(kKeyLon, config::kDefaultRadarLon);
    if (validLatLon(lat, lon)) {
      s_lat = lat;
      s_lon = lon;
      s_configured = prefs.getBool(kKeyCustom, true);
    }
  }
  prefs.end();
}

bool isConfigured() { return s_configured; }

double lat() { return s_lat; }

double lon() { return s_lon; }

bool save(double lat, double lon, bool is_custom) {
  if (!validLatLon(lat, lon)) {
    return false;
  }
  persist(lat, lon, is_custom);
  Serial.printf("Radar location saved: %.6f, %.6f (custom=%d)\n", lat, lon, is_custom);
  return true;
}

bool saveFromStrings(const char* lat_str, const char* lon_str) {
  double lat = 0.0;
  double lon = 0.0;
  if (!parseCoord(lat_str, &lat) || !parseCoord(lon_str, &lon)) {
    return false;
  }
  return save(lat, lon, true);
}

bool fetchFromIpGeolocation() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("IP Geolocation skipped: WiFi not connected");
    return false;
  }

  Serial.printf("Querying IP geolocation (%s)...\n", config::kIpGeoUrl);
  HTTPClient http;
  http.begin(config::kIpGeoUrl);
  http.setTimeout(config::kIpGeoTimeoutMs);

  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("IP geolocation request failed (HTTP %d)\n", code);
    http.end();
    return false;
  }

  const String payload = http.getString();
  http.end();

  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    Serial.printf("IP geolocation JSON parse error: %s\n", err.c_str());
    return false;
  }

  const char* status = doc["status"];
  if (status == nullptr || strcmp(status, "success") != 0) {
    Serial.println("IP geolocation returned non-success response");
    return false;
  }

  const double lat = doc["lat"] | 0.0;
  const double lon = doc["lon"] | 0.0;
  const char* city = doc["city"] | "Unknown";
  const char* country = doc["country"] | "Unknown";

  if (!validLatLon(lat, lon)) {
    Serial.println("IP geolocation returned invalid coordinates");
    return false;
  }

  persist(lat, lon, false);
  Serial.printf("Auto-detected location from IP: %.6f, %.6f (%s, %s)\n",
                lat, lon, city, country);
  return true;
}

void clear() {
  Preferences prefs;
  prefs.begin(kPrefsNamespace, false);
  prefs.remove(kKeyLat);
  prefs.remove(kKeyLon);
  prefs.remove(kKeyCustom);
  prefs.end();
  s_lat = config::kDefaultRadarLat;
  s_lon = config::kDefaultRadarLon;
  s_configured = false;
}

}  // namespace services::location
