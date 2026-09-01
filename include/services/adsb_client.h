#pragma once

#include <cstddef>
#include <cstdint>

namespace services::adsb {

enum class AircraftCategory : uint8_t {
  Commercial,
  GeneralAviation,
  Helicopter,
  Military,
};

struct Aircraft {
  float lat;
  float lon;
  float nose_deg;
  float track_deg;
  float gs_knots;
  char callsign[9];
  char type[5];
  char alt[12];
  bool is_military;
  bool is_emergency;
  AircraftCategory category;
};

constexpr size_t kMaxAircraft = 64;

size_t aircraftCount();
const Aircraft* aircraftList();
bool hasEmergencyAircraft();

/** Hook invoked during long HTTP I/O (e.g. wifiLoop). Optional. */
using PollFn = void (*)();
void setPollFn(PollFn fn);

/** Fetch aircraft within fetch_radius_km of center_lat/lon from adsb.fi. */
bool fetchUpdate(double center_lat, double center_lon, float fetch_radius_km);

}  // namespace services::adsb
