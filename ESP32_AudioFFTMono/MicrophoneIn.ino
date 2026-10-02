/*
INMP441 -> ESP32-C3:
    VDD >> 3.3V
    GND >> GND
    SCK >> GPIO3
    WS  >> GPIO2
    SD  >> GPIO4
    L/R >> GND   (L/R low = mic outputs on the LEFT slot, high = RIGHT slot)
*/

#define I2S_SCK  GPIO_NUM_3
#define I2S_WS   GPIO_NUM_2
#define I2S_SD   GPIO_NUM_4
#define I2S_PORT I2S_NUM_0   // the C3 has a single I2S peripheral

// L/R tied to GND -> left slot. If you get a flat/noisy signal, try I2S_CHANNEL_FMT_ONLY_RIGHT.
#define I2S_MIC_CHANNEL I2S_CHANNEL_FMT_ONLY_LEFT

void initMicrophone()
{
  Serial.println("Setup I2S Microphone...");

  delay(1000);
  i2s_install();
  i2s_setpin();
  i2s_start(I2S_PORT);
  delay(500);
}

esp_err_t getWave()
{
  size_t bytesIn = 0;
  esp_err_t result = i2s_read(I2S_PORT, wave, sizeof(wave), &bytesIn, portMAX_DELAY);

  if (result == ESP_OK)
  {
    int samples_read = bytesIn / sizeof(wave[0]);
    int32_t peak = 0;
    for (int i = 0; i < samples_read; ++i)
    {
      wave[i] >>= 16;               // keep the top 16 bits of the 24-bit sample
      int32_t a = abs(wave[i]);
      if (a > peak) peak = a;
    }
    for (int i = samples_read; i < SAMPLE_BUFFER_SIZE; ++i) wave[i] = 0;  // short read safety
    maxWave = peak;
  }
  return result;
}

void i2s_install()
{
  i2s_config_t i2s_config = {};
  i2s_config.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX);
  i2s_config.sample_rate = SAMPLE_RATE;
  i2s_config.bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT;
  i2s_config.channel_format = I2S_MIC_CHANNEL;
  i2s_config.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  i2s_config.intr_alloc_flags = 0;
  i2s_config.dma_buf_count = 4;
  i2s_config.dma_buf_len = 256;     // frames per buffer; keeps each DMA buffer under the 4092-byte limit
  i2s_config.use_apll = false;
  i2s_config.tx_desc_auto_clear = false;
  i2s_config.fixed_mclk = 0;

  esp_err_t err = i2s_driver_install(I2S_PORT, &i2s_config, 0, NULL);
  if (err != ESP_OK) Serial.printf("i2s_driver_install failed: %d\n", err);
}

void i2s_setpin()
{
  i2s_pin_config_t pin_config = {};
  pin_config.mck_io_num = I2S_PIN_NO_CHANGE;   // no MCLK needed (and don't grab GPIO0)
  pin_config.bck_io_num = I2S_SCK;
  pin_config.ws_io_num = I2S_WS;
  pin_config.data_out_num = I2S_PIN_NO_CHANGE;
  pin_config.data_in_num = I2S_SD;

  esp_err_t err = i2s_set_pin(I2S_PORT, &pin_config);
  if (err != ESP_OK) Serial.printf("i2s_set_pin failed: %d\n", err);
}
