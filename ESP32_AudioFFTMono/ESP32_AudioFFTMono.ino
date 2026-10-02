/*
 * Audio Spectrum Visualizer for 128x64 I2C OLED (ESP32-C3 / ESP32)
 *
 * Libraries: U8g2, arduinoFFT 2.x
 * Board:     "ESP32C3 Dev Module", USB CDC On Boot: Enabled
 *
 * Wiring:
 *   INMP441:  VDD -> 3.3V, GND -> GND, SCK -> GPIO3, WS -> GPIO2, SD -> GPIO4, L/R -> GND
 *   OLED:     VCC -> 3.3V, GND -> GND, SDA -> GPIO8, SCL -> GPIO9
 */
#include <Arduino.h>
#include <U8g2lib.h>
#include <Wire.h>
#include <Preferences.h>
#include <driver/i2s.h>
#include "arduinoFFT.h"

#define I2C_SDA 8
#define I2C_SCL 9

#define SAMPLE_RATE 16000
#define SAMPLE_BUFFER_SIZE 512  // number of samples

// The C3 has no hardware FPU, so float is much cheaper than double.
float vReal[SAMPLE_BUFFER_SIZE];
float vImag[SAMPLE_BUFFER_SIZE];
ArduinoFFT<float> FFT = ArduinoFFT<float>(vReal, vImag, SAMPLE_BUFFER_SIZE, (float)SAMPLE_RATE);

// raw waveform data (INMP441 delivers 24-bit data in a 32-bit slot):
int32_t wave[SAMPLE_BUFFER_SIZE];
int32_t maxWave = 0;  // to compute scaling factor for wave display

void setup()
{
  Serial.begin(115200);
  Serial.setTimeout(100);
  Wire.begin(I2C_SDA, I2C_SCL);

  loadSettings();
  initMicrophone();
  initDisplay();
  calibrateNoise();   // ~2 s, keep it quiet

  delay(1000);
}

void loop()
{
  handleSerial();   // Serial Monitor commands: m, a, +, -, c, t<text>

  if (getWave() != ESP_OK)
  {
    Serial.println("Error: i2s_read() failed");
    displayError("i2s_read() failed");
    delay(100);
    return;
  }

  performFFT();

  displayAll();
}
