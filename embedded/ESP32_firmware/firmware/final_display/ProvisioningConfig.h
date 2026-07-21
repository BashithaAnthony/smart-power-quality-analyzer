#pragma once

#include <stddef.h>

namespace ProvisioningConfig {

// These are the credentials passed to WiFiManager during the blocking startup
// auto-connect/configuration flow and shown on that same startup screen.
static constexpr char kPortalSsid[] = "Analyzer_Setup";
static constexpr char kStartupPortalPassword[] = "password123";

// The existing Settings-initiated portal intentionally keeps its established
// password. It shares the SSID but remains a distinct entry path.
static constexpr char kSettingsPortalPassword[] = "setup123";

// TFT_eSPI font 2 is approximately six pixels per ASCII character. The
// startup credential column occupies x=10..180, beside (not under) the logo.
static constexpr size_t kStartupTextMaximumCharacters = 28U;
static_assert(sizeof("Device ID: ") - 1U + 15U <=
                  kStartupTextMaximumCharacters,
              "The device identifier row must fit beside the startup logo");
static_assert(sizeof("Setup Wi-Fi: ") - 1U + sizeof(kPortalSsid) - 1U <=
                  kStartupTextMaximumCharacters,
              "The setup SSID row must fit beside the startup logo");
static_assert(sizeof("Password: ") - 1U +
                      sizeof(kStartupPortalPassword) - 1U <=
                  kStartupTextMaximumCharacters,
              "The setup password row must fit beside the startup logo");

}  // namespace ProvisioningConfig
