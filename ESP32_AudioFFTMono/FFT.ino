// Audio FFT processing implementation using arduinoFFT 2.x API

#define CAL_SKIP   15    // frames thrown away while the mic settles (~0.5 s)
#define CAL_FRAMES 60    // frames used to measure the idle noise
#define NOISE_K    2.5f  // floor = mean + K * standard deviation (higher = cleaner idle, less sensitive)

float noiseFloor[SAMPLE_BUFFER_SIZE / 2];   // per-bin idle noise level

void performFFT()
{
  // Remove the mic's DC offset, then scale
  float mean = 0;
  for (int i = 0; i < SAMPLE_BUFFER_SIZE; i++) mean += wave[i];
  mean /= SAMPLE_BUFFER_SIZE;

  for (int i = 0; i < SAMPLE_BUFFER_SIZE; i++) {
    vReal[i] = (wave[i] - mean) * 3.3f / 4096.0f;
    vImag[i] = 0;
  }

  FFT.windowing(FFTWindow::Hamming, FFTDirection::Forward);  // Hamming window
  FFT.compute(FFTDirection::Forward);                        // FFT
  FFT.complexToMagnitude();                                  // absolute value
}

// Measure idle noise (mean + spread per bin). The display is refreshed during
// calibration exactly like in normal running, so OLED-induced supply noise on the
// mic is included in the measurement.
void calibrateNoise()
{
  static float sum[SAMPLE_BUFFER_SIZE / 2];
  static float sumSq[SAMPLE_BUFFER_SIZE / 2];
  for (int i = 0; i < SAMPLE_BUFFER_SIZE / 2; i++) { sum[i] = 0; sumSq[i] = 0; }

  int n = 0;
  for (int f = 0; f < CAL_SKIP + CAL_FRAMES; f++) {
    showMessage("Calibrating...", "keep it quiet");   // keeps the OLED active like normal use
    if (getWave() != ESP_OK) continue;
    performFFT();
    if (f < CAL_SKIP) continue;
    for (int i = 0; i < SAMPLE_BUFFER_SIZE / 2; i++) {
      sum[i]   += vReal[i];
      sumSq[i] += vReal[i] * vReal[i];
    }
    n++;
  }
  if (n == 0) n = 1;

  float avg = 0;
  for (int i = 0; i < SAMPLE_BUFFER_SIZE / 2; i++) {
    float m = sum[i] / n;
    float var = sumSq[i] / n - m * m;
    if (var < 0) var = 0;
    noiseFloor[i] = m + NOISE_K * sqrtf(var);
    avg += noiseFloor[i];
  }
  Serial.printf("Calibrated, average noise floor: %.3f  (send 'c' to recalibrate)\n", avg / (SAMPLE_BUFFER_SIZE / 2));
}
