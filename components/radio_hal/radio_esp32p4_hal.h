#ifndef ESP32P4_HAL_H
#define ESP32P4_HAL_H

// include RadioLib
#include <RadioLib.h>

// only supports ESP32-P4
#if CONFIG_IDF_TARGET_ESP32P4 == 0
  #error This HAL only supports ESP32-P4 target.
#endif

// include all the dependencies
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "hal/gpio_hal.h"
#include "esp_timer.h"
#include "esp_err.h"
#include "esp_log.h"

// define Arduino-style macros
#define LOW                         (0x0)
#define HIGH                        (0x1)
#define INPUT                       (0x01)
#define OUTPUT                      (0x03)
#define RISING                      (0x01)
#define FALLING                     (0x02)
#define NOP()                       asm volatile ("nop")

static const char* TAG_HAL = "Esp32p4Hal";
static bool s_gpio_isr_service_ready = false;

// ESP32-P4 HAL for RadioLib using ESP-IDF SPI Master driver
class Esp32p4Hal : public RadioLibHal {
  public:
    Esp32p4Hal(int8_t sck, int8_t miso, int8_t mosi, uint32_t spiFreq = 2000000, spi_host_device_t spiHost = SPI2_HOST)
      : RadioLibHal(INPUT, OUTPUT, LOW, HIGH, RISING, FALLING),
      spiSCK(sck), spiMISO(miso), spiMOSI(mosi), spiFrequency(spiFreq), spiHostDevice(spiHost) {
    }

    void init() override {
      spiBegin();
    }

    void term() override {
      spiEnd();
    }

    void pinMode(uint32_t pin, uint32_t mode) override {
      if(pin == RADIOLIB_NC) {
        return;
      }

      gpio_config_t conf = {};
      conf.pin_bit_mask = (1ULL << pin);
      conf.mode = (gpio_mode_t)mode;
      conf.pull_up_en = GPIO_PULLUP_DISABLE;
      conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
      conf.intr_type = GPIO_INTR_DISABLE;
      gpio_config(&conf);
    }

    void digitalWrite(uint32_t pin, uint32_t value) override {
      if(pin == RADIOLIB_NC) {
        return;
      }

      gpio_set_level((gpio_num_t)pin, value);
    }

    uint32_t digitalRead(uint32_t pin) override {
      if(pin == RADIOLIB_NC) {
        return(0);
      }

      return(gpio_get_level((gpio_num_t)pin));
    }

    void attachInterrupt(uint32_t interruptNum, void (*interruptCb)(void), uint32_t mode) override {
      if(interruptNum == RADIOLIB_NC) {
        return;
      }

      if(!s_gpio_isr_service_ready) {
        esp_err_t err = gpio_install_isr_service((int)ESP_INTR_FLAG_IRAM);
        if((err == ESP_OK) || (err == ESP_ERR_INVALID_STATE)) {
          s_gpio_isr_service_ready = true;
        } else {
          ESP_LOGE(TAG_HAL, "gpio_install_isr_service failed: %s", esp_err_to_name(err));
          return;
        }
      }
      gpio_set_intr_type((gpio_num_t)interruptNum, (gpio_int_type_t)(mode & 0x7));
      gpio_isr_handler_add((gpio_num_t)interruptNum, (void (*)(void*))interruptCb, NULL);
    }

    void detachInterrupt(uint32_t interruptNum) override {
      if(interruptNum == RADIOLIB_NC) {
        return;
      }

      if(s_gpio_isr_service_ready) {
        gpio_isr_handler_remove((gpio_num_t)interruptNum);
      }
      gpio_wakeup_disable((gpio_num_t)interruptNum);
      gpio_set_intr_type((gpio_num_t)interruptNum, GPIO_INTR_DISABLE);
    }

    void delay(RadioLibTime_t ms) override {
      vTaskDelay(ms / portTICK_PERIOD_MS);
    }

    void delayMicroseconds(RadioLibTime_t us) override {
      uint64_t m = (uint64_t)esp_timer_get_time();
      if(us) {
        uint64_t e = (m + us);
        if(m > e) {
          while((uint64_t)esp_timer_get_time() > e) {
            NOP();
          }
        }
        while((uint64_t)esp_timer_get_time() < e) {
          NOP();
        }
      }
    }

    RadioLibTime_t millis() override {
      return((RadioLibTime_t)(esp_timer_get_time() / 1000ULL));
    }

    RadioLibTime_t micros() override {
      return((RadioLibTime_t)(esp_timer_get_time()));
    }

    long pulseIn(uint32_t pin, uint32_t state, RadioLibTime_t timeout) override {
      if(pin == RADIOLIB_NC) {
        return(0);
      }

      this->pinMode(pin, INPUT);
      uint32_t start = this->micros();
      uint32_t curtick = this->micros();

      while(this->digitalRead(pin) == state) {
        if((this->micros() - curtick) > timeout) {
          return(0);
        }
      }

      return(this->micros() - start);
    }

    void spiBegin() {
      spi_bus_config_t buscfg = {};
      buscfg.mosi_io_num = this->spiMOSI;
      buscfg.miso_io_num = this->spiMISO;
      buscfg.sclk_io_num = this->spiSCK;
      buscfg.quadwp_io_num = -1;
      buscfg.quadhd_io_num = -1;
      buscfg.max_transfer_sz = 0;

      esp_err_t ret = spi_bus_initialize(this->spiHostDevice, &buscfg, SPI_DMA_CH_AUTO);
      if(ret != ESP_OK) {
        ESP_LOGE(TAG_HAL, "SPI bus init failed: %s", esp_err_to_name(ret));
        return;
      }

      spi_device_interface_config_t devcfg = {};
      devcfg.clock_speed_hz = this->spiFrequency;
      devcfg.mode = 0;
      devcfg.spics_io_num = -1;
      devcfg.queue_size = 1;
      devcfg.flags = 0;

      ret = spi_bus_add_device(this->spiHostDevice, &devcfg, &this->spiDevice);
      if(ret != ESP_OK) {
        ESP_LOGE(TAG_HAL, "SPI add device failed: %s", esp_err_to_name(ret));
      }
    }

    void spiBeginTransaction() {
      spi_device_acquire_bus(this->spiDevice, portMAX_DELAY);
    }

    void spiTransfer(uint8_t* out, size_t len, uint8_t* in) {
      spi_transaction_t trans = {};
      trans.length = len * 8;
      trans.tx_buffer = out;
      trans.rx_buffer = in;
      spi_device_transmit(this->spiDevice, &trans);
    }

    void spiEndTransaction() {
      spi_device_release_bus(this->spiDevice);
    }

    void spiEnd() {
      if(this->spiDevice != NULL) {
        spi_bus_remove_device(this->spiDevice);
        this->spiDevice = NULL;
      }
      spi_bus_free(this->spiHostDevice);
    }

  private:
    int8_t spiSCK;
    int8_t spiMISO;
    int8_t spiMOSI;
    uint32_t spiFrequency;
    spi_host_device_t spiHostDevice;
    spi_device_handle_t spiDevice = NULL;
};

#endif
