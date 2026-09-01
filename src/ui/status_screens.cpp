#include "ui/status_screens.h"

#include <lgfx/v1/lgfx_fonts.hpp>

#include <cmath>
#include <cstdio>
#include <cstddef>
#include <cstring>

#include "config.h"
#include "hardware/display.h"
#include "hardware/display_font.h"

namespace {

constexpr int kLineGap = 6;
const int kCenterX = config::kDisplayWidth / 2;
const int kCenterY = config::kDisplayHeight / 2;

constexpr int kSpinnerDotCount = 10;
constexpr int kSpinnerRadius = 113;
constexpr int kSpinnerDotRadius = 2;
constexpr int kSpinnerEraseRadius = 4;
constexpr float kSpinnerStepDeg = 6.0f;

struct SpinnerDot {
  int x = 0;
  int y = 0;
  bool drawn = false;
};

char s_connecting_ssid[33];
char s_ssid_line[33];
constexpr int kConnectingTextMaxWidthPx = 220;
float s_spinner_angle_deg = -90.0f;
SpinnerDot s_spinner_dots[kSpinnerDotCount];
bool s_connecting_text_drawn = false;

constexpr auto& kGfxTitle = fonts::FreeSans18pt7b;
constexpr auto& kGfxBody = fonts::FreeSans12pt7b;
constexpr auto& kGfxDetail = fonts::Font2;
constexpr auto& kPortalGfxTitle = fonts::FreeSansBold18pt7b;
constexpr auto& kPortalGfxBody = fonts::FreeSansBold12pt7b;
constexpr auto& kPortalGfxEmphasis = fonts::FreeSansBold18pt7b;
constexpr auto& kConnectingGfxDetail = fonts::FreeSans9pt7b;

struct TextLine {
  const char* text;
  float vlw_size;
  const lgfx::GFXfont* gfx_font;
};

int lineHeightGfx(const lgfx::GFXfont* font) {
  displayFontSetBitmap(tft, font);
  return tft.fontHeight();
}

int lineHeightVlw(float size) {
  displayFontSetSmoothSize(tft, size);
  return tft.fontHeight();
}

void applyLineStyle(const TextLine& line) {
  if (displayFontIsSmooth()) {
    displayFontSetSmoothSize(tft, line.vlw_size);
  } else {
    displayFontSetBitmap(tft, line.gfx_font);
  }
}

void drawTextBlock(uint16_t bg, uint16_t fg, const TextLine* lines, size_t count) {
  tft.fillScreen(bg);
  tft.setTextColor(fg, bg);
  tft.setTextDatum(textdatum_t::middle_center);

  int total_h = 0;
  for (size_t i = 0; i < count; ++i) {
    if (displayFontIsSmooth()) {
      total_h += lineHeightVlw(lines[i].vlw_size);
    } else {
      total_h += lineHeightGfx(lines[i].gfx_font);
    }
    if (i + 1 < count) {
      total_h += kLineGap;
    }
  }

  int y = (config::kDisplayHeight - total_h) / 2;
  for (size_t i = 0; i < count; ++i) {
    applyLineStyle(lines[i]);
    const int h =
        displayFontIsSmooth() ? lineHeightVlw(lines[i].vlw_size)
                              : lineHeightGfx(lines[i].gfx_font);
    tft.drawString(lines[i].text, kCenterX, y + h / 2);
    y += h + kLineGap;
  }
}

constexpr float kConnectingDetailVlw = 0.92f;

void applyConnectingDetailStyle() {
  if (displayFontIsSmooth()) {
    displayFontSetSmoothSize(tft, kConnectingDetailVlw);
  } else {
    displayFontSetBitmap(tft, &kConnectingGfxDetail);
  }
}

/** SSID on one line; truncate with … if wider than kConnectingTextMaxWidthPx. */
void fitSsidLine() {
  strncpy(s_ssid_line, s_connecting_ssid, sizeof(s_ssid_line) - 1);
  s_ssid_line[sizeof(s_ssid_line) - 1] = '\0';
  applyConnectingDetailStyle();
  if (tft.textWidth(s_ssid_line) <= kConnectingTextMaxWidthPx) {
    return;
  }
  const size_t len = strlen(s_connecting_ssid);
  for (size_t n = len; n > 0; --n) {
    snprintf(s_ssid_line, sizeof(s_ssid_line), "%.*s…", static_cast<int>(n),
             s_connecting_ssid);
    if (tft.textWidth(s_ssid_line) <= kConnectingTextMaxWidthPx) {
      return;
    }
  }
  strncpy(s_ssid_line, "…", sizeof(s_ssid_line) - 1);
  s_ssid_line[sizeof(s_ssid_line) - 1] = '\0';
}

void drawConnectingText() {
  tft.fillScreen(config::kColorBlack);

  tft.setTextDatum(textdatum_t::middle_center);
  tft.setTextColor(config::kTextOnBlack, config::kColorBlack);

  applyConnectingDetailStyle();
  const int detail_h = tft.fontHeight();
  const int total_h = detail_h * 2 + kLineGap;
  const int block_top = (config::kDisplayHeight - total_h) / 2;
  constexpr int kPanelPadY = 8;
  tft.fillRect(kCenterX - kConnectingTextMaxWidthPx / 2, block_top - kPanelPadY,
               kConnectingTextMaxWidthPx, total_h + kPanelPadY * 2, config::kColorBlack);

  int y = block_top;
  tft.drawString("Connecting to", kCenterX, y + detail_h / 2);
  y += detail_h + kLineGap;
  tft.drawString(s_ssid_line, kCenterX, y + detail_h / 2);

  s_connecting_text_drawn = true;
}

void eraseSpinnerDots() {
  for (int i = 0; i < kSpinnerDotCount; ++i) {
    if (!s_spinner_dots[i].drawn) {
      continue;
    }
    tft.fillCircle(s_spinner_dots[i].x, s_spinner_dots[i].y, kSpinnerEraseRadius,
                   config::kColorBlack);
    s_spinner_dots[i].drawn = false;
  }
}

void drawSpinnerDots() {
  constexpr float kDegToRad = 0.01745329252f;
  const float head_rad = s_spinner_angle_deg * kDegToRad;

  for (int i = 0; i < kSpinnerDotCount; ++i) {
    const float a = head_rad - static_cast<float>(i) * (6.283185307f / kSpinnerDotCount);
    const int x = kCenterX + static_cast<int>(std::lround(std::cos(a) * kSpinnerRadius));
    const int y = kCenterY + static_cast<int>(std::lround(std::sin(a) * kSpinnerRadius));

    const int fade = 255 - i * 22;
    const uint16_t color = tft.color565(0, fade, 0);
    tft.fillSmoothCircle(x, y, kSpinnerDotRadius, color);

    s_spinner_dots[i].x = x;
    s_spinner_dots[i].y = y;
    s_spinner_dots[i].drawn = true;
  }
}

}  // namespace

void statusScreenConnectingBegin(const char* ssid) {
  const char* name = (ssid != nullptr && ssid[0] != '\0') ? ssid : "network";
  strncpy(s_connecting_ssid, name, sizeof(s_connecting_ssid) - 1);
  s_connecting_ssid[sizeof(s_connecting_ssid) - 1] = '\0';
  fitSsidLine();
  s_spinner_angle_deg = -90.0f;
  for (auto& dot : s_spinner_dots) {
    dot.drawn = false;
  }
  s_connecting_text_drawn = false;
  drawConnectingText();
  drawSpinnerDots();
}

void statusScreenConnectingTick() {
  if (!s_connecting_text_drawn) {
    drawConnectingText();
  }
  eraseSpinnerDots();
  s_spinner_angle_deg += kSpinnerStepDeg;
  if (s_spinner_angle_deg >= 270.0f) {
    s_spinner_angle_deg -= 360.0f;
  }
  drawSpinnerDots();
}

void statusScreenPortal() {
  const TextLine lines[] = {
      {"Wi-Fi setup", 1.15f, &kPortalGfxTitle},
      {"1. Join network:", 1.05f, &kPortalGfxBody},
      {config::kPortalApName, 1.12f, &kPortalGfxEmphasis},
      {"2. Open in browser:", 1.05f, &kPortalGfxBody},
      {config::kPortalHostUrl, 1.12f, &kPortalGfxEmphasis},
      {"or 192.168.4.1", 1.0f, &kPortalGfxBody},
  };
  drawTextBlock(config::kColorYellow, config::kTextOnYellow, lines,
                sizeof(lines) / sizeof(lines[0]));
}

void statusScreenConnectFailed() {
  const TextLine lines[] = {
      {"Could not connect", 1.15f, &kGfxTitle},
      {"Check Wi-Fi password", 1.0f, &kGfxBody},
      {"and signal strength.", 1.0f, &kGfxBody},
      {"Hold BOOT 3 sec", 1.0f, &kGfxBody},
      {"to reset Wi-Fi", 1.0f, &kGfxBody},
  };
  drawTextBlock(config::kColorYellow, config::kTextOnYellow, lines,
                sizeof(lines) / sizeof(lines[0]));
}

void statusScreenWifiReset() {
  const TextLine lines[] = {
      {"Wi-Fi reset", 1.15f, &kPortalGfxTitle},
      {"Restarting...", 1.05f, &kPortalGfxBody},
  };
  drawTextBlock(config::kColorYellow, config::kTextOnYellow, lines,
                sizeof(lines) / sizeof(lines[0]));
}

void statusScreenConnected(const char* ip, const char* hostname) {
  const TextLine lines[] = {
      {"Connected", 1.15f, &kPortalGfxTitle},
      {"IP address:", 1.0f, &kPortalGfxBody},
      {ip, 1.12f, &kPortalGfxEmphasis},
      {"Web config:", 1.0f, &kPortalGfxBody},
      {hostname, 1.10f, &kPortalGfxEmphasis},
  };
  drawTextBlock(config::kColorYellow, config::kTextOnYellow, lines,
                sizeof(lines) / sizeof(lines[0]));
}

namespace {
int s_last_update_percent = -1;
constexpr int kBarX = 35;
constexpr int kBarY = 110;
constexpr int kBarWidth = 170;
constexpr int kBarHeight = 16;
constexpr int kBarRadius = 4;
}  // namespace

void statusScreenUpdateBegin(const char* title) {
  s_last_update_percent = -1;
  tft.fillScreen(config::kColorBlack);
  tft.setTextColor(config::kTextOnBlack, config::kColorBlack);
  tft.setTextDatum(textdatum_t::middle_center);

  if (displayFontIsSmooth()) {
    displayFontSetSmoothSize(tft, 1.15f);
  } else {
    displayFontSetBitmap(tft, &kPortalGfxTitle);
  }
  tft.drawString(title != nullptr ? title : "Firmware Update", kCenterX, 48);

  if (displayFontIsSmooth()) {
    displayFontSetSmoothSize(tft, 0.95f);
  } else {
    displayFontSetBitmap(tft, &kConnectingGfxDetail);
  }
  tft.setTextColor(tft.color565(120, 200, 255), config::kColorBlack);
  tft.drawString("Receiving image...", kCenterX, 76);

  // Outer progress bar border
  tft.drawRoundRect(kBarX, kBarY, kBarWidth, kBarHeight, kBarRadius, config::kTextOnBlack);
  tft.fillRect(kBarX + 2, kBarY + 2, kBarWidth - 4, kBarHeight - 4, config::kColorBlack);

  // Warning text
  tft.setTextColor(config::kColorYellow, config::kColorBlack);
  if (displayFontIsSmooth()) {
    displayFontSetSmoothSize(tft, 0.88f);
  } else {
    displayFontSetBitmap(tft, &kConnectingGfxDetail);
  }
  tft.drawString("Do not unplug power", kCenterX, 185);

  statusScreenUpdateProgress(0);
}

void statusScreenUpdateProgress(int percent) {
  percent = constrain(percent, 0, 100);
  if (percent == s_last_update_percent) {
    return;
  }
  s_last_update_percent = percent;

  // Fill inner bar
  const int inner_max_w = kBarWidth - 4;
  const int fill_w = (inner_max_w * percent) / 100;
  if (fill_w > 0) {
    tft.fillRoundRect(kBarX + 2, kBarY + 2, fill_w, kBarHeight - 4, 2, tft.color565(0, 220, 80));
  }
  if (inner_max_w - fill_w > 0) {
    tft.fillRect(kBarX + 2 + fill_w, kBarY + 2, inner_max_w - fill_w, kBarHeight - 4, config::kColorBlack);
  }

  // Draw percentage text
  char pct_buf[16];
  snprintf(pct_buf, sizeof(pct_buf), "%d%%", percent);
  tft.setTextDatum(textdatum_t::middle_center);
  tft.setTextColor(config::kTextOnBlack, config::kColorBlack);
  if (displayFontIsSmooth()) {
    displayFontSetSmoothSize(tft, 1.10f);
  } else {
    displayFontSetBitmap(tft, &kPortalGfxBody);
  }
  tft.fillRect(kCenterX - 40, 138, 80, 24, config::kColorBlack);
  tft.drawString(pct_buf, kCenterX, 150);
}

void statusScreenUpdateEnd() {
  const TextLine lines[] = {
      {"Update complete", 1.15f, &kPortalGfxTitle},
      {"Rebooting...", 1.05f, &kPortalGfxBody},
  };
  drawTextBlock(config::kColorBlack, tft.color565(0, 255, 100), lines,
                sizeof(lines) / sizeof(lines[0]));
}

void statusScreenUpdateError(const char* message) {
  const TextLine lines[] = {
      {"Update failed", 1.15f, &kGfxTitle},
      {message != nullptr ? message : "Error writing flash", 1.0f, &kGfxBody},
      {"Please reboot device", 0.95f, &kConnectingGfxDetail},
  };
  drawTextBlock(config::kColorYellow, config::kTextOnYellow, lines,
                sizeof(lines) / sizeof(lines[0]));
}
