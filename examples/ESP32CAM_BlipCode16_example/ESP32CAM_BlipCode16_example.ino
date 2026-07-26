/**
 * ESP32CAM_BlipCode16.ino
 *
 * Minimal example: initializes the ESP32 camera in grayscale mode (which
 * BlipCode16 requires -- see BlipCode16.h for why), grabs a frame, and
 * decodes it. Pin numbers below are for the common AI-Thinker ESP32-CAM
 * board; adjust for your specific board if different.
 */

#include "esp_camera.h"
#include <BlipCode16.h>

// ---- AI-Thinker ESP32-CAM pin map -- change if using a different board ----
#define PWDN_GPIO_NUM   32
#define RESET_GPIO_NUM  -1
#define XCLK_GPIO_NUM    0
#define SIOD_GPIO_NUM   26
#define SIOC_GPIO_NUM   27
#define Y9_GPIO_NUM     35
#define Y8_GPIO_NUM     34
#define Y7_GPIO_NUM     39
#define Y6_GPIO_NUM     36
#define Y5_GPIO_NUM     21
#define Y4_GPIO_NUM     19
#define Y3_GPIO_NUM     18
#define Y2_GPIO_NUM      5
#define VSYNC_GPIO_NUM  25
#define HREF_GPIO_NUM   23
#define PCLK_GPIO_NUM   22

BlipCode16 reader;

void setup() {
  Serial.begin(115200);
  delay(300);

  camera_config_t config = {};
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sscb_sda = SIOD_GPIO_NUM;
  config.pin_sscb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;

  // BlipCode16 reads a single-byte-per-pixel luminance buffer directly --
  // grayscale mode means camera_fb_t::buf is already exactly that, with
  // zero conversion needed before calling decode().
  config.pixel_format = PIXFORMAT_GRAYSCALE;
  config.frame_size = FRAMESIZE_VGA; // 640x480 -- raise for more range, lower for more speed
  config.fb_count = 1;

  if (esp_camera_init(&config) != ESP_OK) {
    Serial.println("Camera init failed");
    while (true) delay(1000);
  }

  // Targets only ever appear in a horizontal band of the frame -- restrict
  // scanning to that band rather than the full height. Tune this to your
  // own camera mounting/field of view; see the README for how this trades
  // off against detection range.
  reader.setBandHeightFraction(0.5f);
}

void loop() {
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("Frame capture failed");
    delay(50);
    return;
  }

  BlipCode16Result results[4];
  uint8_t count = reader.decode(fb->buf, fb->width, fb->height, results, 4);

  for (uint8_t i = 0; i < count; i++) {
    const BlipCode16Result &r = results[i];
    Serial.printf(
      "code=%u  corners=(%d,%d)(%d,%d)(%d,%d)(%d,%d)\n",
      r.value,
      r.corners[0].x, r.corners[0].y,
      r.corners[1].x, r.corners[1].y,
      r.corners[2].x, r.corners[2].y,
      r.corners[3].x, r.corners[3].y
    );
  }

  esp_camera_fb_return(fb);
}
