#pragma once

void statusScreenPortal();
void statusScreenConnectFailed();
void statusScreenWifiReset();
void statusScreenConnected(const char* ip, const char* hostname);

/** Saved-network connect animation (call Tick until connect finishes). */
void statusScreenConnectingBegin(const char* ssid);
void statusScreenConnectingTick();

/** Firmware upload / OTA update progress screen. */
void statusScreenUpdateBegin(const char* title = "Firmware Update");
void statusScreenUpdateProgress(int percent);
void statusScreenUpdateEnd();
void statusScreenUpdateError(const char* message = "Update Failed");
