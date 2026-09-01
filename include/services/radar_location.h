#pragma once

namespace services::location {

/** Load saved lat/lon from NVS, or use config defaults. Call once before WiFi setup. */
void init();

/** True if custom/valid coordinates have been explicitly saved in NVS. */
bool isConfigured();

/** Factory defaults when nothing is stored (also used for portal field prefill). */
double lat();
double lon();

/** Save numeric coordinates directly to NVS and update runtime values. */
bool save(double lat, double lon, bool is_custom = true);

/** Parse portal strings, validate, persist to NVS, update runtime values. */
bool saveFromStrings(const char* lat_str, const char* lon_str);

/** Automatically query IP geolocation service to determine and save location. */
bool fetchFromIpGeolocation();

/** Clear stored coordinates (e.g. with WiFi credential reset). */
void clear();

}  // namespace services::location
