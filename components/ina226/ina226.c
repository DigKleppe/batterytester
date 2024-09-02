/*
 * Copyright (c) 2020 Ruslan V. Uss <unclerus@gmail.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holder nor the names of itscontributors
 *    may be used to endorse or promote products derived from this software without
 *    specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/**
 * @file ina226.c
 *
 *	adapted from ina260  by Dig Kleppe <digKleppe@gmail.com>
 *
 * ESP-IDF driver for INA226 precision digital current and power monitor
 *
 * Copyright (c) 2020 Ruslan V. Uss <unclerus@gmail.com>
 *
 * BSD Licensed as described in the file LICENSE
 *
 */

#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG

#include <esp_log.h>
#include <math.h>
#include <esp_idf_lib_helpers.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "ina226.h"

#define I2C_FREQ_HZ 400000 // no more than 400 kHz, otherwise enabling HS mode on the chip is required

static const char *TAG = "ina226";

#define REG_CONFIG      	0
#define REG_SHUNT_VOLATGE   1
#define REG_BUS_VOLTAGE 	2
#define REG_POWER       	3
#define REG_CALIBRATION		5
#define REG_MASK_EN     	6
#define REG_ALERT_LIMIT 	7
#define REG_MFR_ID      	0xfe
#define REG_DIE_ID      	0xff

#define BIT_MODE   0
#define BIT_ISHCT  3
#define BIT_VBUSCT 6
#define BIT_AVG    9
#define BIT_RST    15

#define BIT_LEN  0
#define BIT_APOL 1
#define BIT_OVF  2
#define BIT_CVRF 3
#define BIT_AFF  4
#define BIT_CNVR 10
#define BIT_POL  11
#define BIT_BUL  12
#define BIT_BOL  13
#define BIT_UCL  14
#define BIT_OCL  15

#define MASK_MODE   (7 << BIT_MODE)
#define MASK_ISHCT  (7 << BIT_ISHCT)
#define MASK_VBUSCT (7 << BIT_VBUSCT)
#define MASK_AVG    (7 << BIT_AVG)

#define DEF_CONF 0x6000

#define BV(x) (1 << (x))

#define CHECK(x) do { esp_err_t __; if ((__ = x) != ESP_OK) return __; } while (0)
#define CHECK_ARG(VAL) do { if (!(VAL)) return ESP_ERR_INVALID_ARG; } while (0)

static esp_err_t read_reg_16(ina226_t *dev, uint8_t reg, uint16_t *val)
{
    CHECK_ARG(val);

    I2C_DEV_TAKE_MUTEX(&dev->i2c_dev);
    I2C_DEV_CHECK(&dev->i2c_dev, i2c_dev_read_reg(&dev->i2c_dev, reg, val, 2));
    I2C_DEV_GIVE_MUTEX(&dev->i2c_dev);

    *val = (*val >> 8) | (*val << 8);

    return ESP_OK;
}

static esp_err_t write_reg_16(ina226_t *dev, uint8_t reg, uint16_t val)
{
    uint16_t v = (val >> 8) | (val << 8);

    I2C_DEV_TAKE_MUTEX(&dev->i2c_dev);
    I2C_DEV_CHECK(&dev->i2c_dev, i2c_dev_write_reg(&dev->i2c_dev, reg, &v, 2));
    I2C_DEV_GIVE_MUTEX(&dev->i2c_dev);

    return ESP_OK;
}

///////////////////////////////////////////////////////////////////////////////

esp_err_t ina226_init_desc(ina226_t *dev, uint8_t addr, i2c_port_t port, int clkSpeedHz)
{
    CHECK_ARG(dev);

    if (addr < INA226_ADDR(INA226_ADDR_PIN_GND, INA226_ADDR_PIN_GND) || addr > INA226_ADDR(INA226_ADDR_PIN_SCL, INA226_ADDR_PIN_SCL))
    {
        ESP_LOGE(TAG, "Invalid I2C address");
        return ESP_ERR_INVALID_ARG;
    }
    dev->i2c_dev.port = port;
    dev->i2c_dev.cfg.device_address = addr;
    dev->i2c_dev.cfg.scl_speed_hz = clkSpeedHz;
    return i2c_dev_create_mutex(&dev->i2c_dev);
}

esp_err_t ina226_free_desc(ina226_t *dev)
{
    CHECK_ARG(dev);

    return i2c_dev_delete_mutex(&dev->i2c_dev);
}

esp_err_t ina226_init(ina226_t *dev)
{
    CHECK_ARG(dev);
    ESP_LOGI(TAG,"ina226_init");
    CHECK(read_reg_16(dev, REG_CONFIG, &dev->config));
    CHECK(read_reg_16(dev, REG_MFR_ID, &dev->mfr_id));
    CHECK(read_reg_16(dev, REG_DIE_ID, &dev->die_id));

    ESP_LOGI(TAG, "[0x%02x@%u] Found device MFR_ID: %04x, DIE_ID: %04x",
             dev->i2c_dev.cfg.device_address, dev->i2c_dev.port, dev->mfr_id, dev->die_id);

    return ESP_OK;
}

esp_err_t ina226_reset(ina226_t *dev)
{
    CHECK_ARG(dev);

    ESP_LOGD(TAG, "[0x%02x@%u] Resetting...", dev->i2c_dev.cfg.device_address, dev->i2c_dev.port);
    CHECK(write_reg_16(dev, REG_CONFIG, 1 << BIT_RST));

    vTaskDelay(pdMS_TO_TICKS(100));

    return ina226_init(dev);
}

esp_err_t ina226_set_config(ina226_t *dev, ina226_mode_t mode, ina226_averaging_mode_t avg_mode,
                            ina226_conversion_time_t vbus_ct, ina226_conversion_time_t ish_ct)
{
    CHECK_ARG(dev
              && mode <= INA226_MODE_CONT_SHUNT_BUS
              && avg_mode <= INA226_AVG_1024
              && vbus_ct <= INA226_CT_8244
              && ish_ct <= INA226_CT_8244);

    dev->config = DEF_CONF
            | (avg_mode << BIT_AVG)
            | (vbus_ct << BIT_VBUSCT)
            | (ish_ct << BIT_ISHCT)
            | (mode << BIT_MODE);

    return write_reg_16(dev, REG_CONFIG, dev->config);
}

esp_err_t ina226_get_config(ina226_t *dev, ina226_mode_t *mode, ina226_averaging_mode_t *avg_mode,
                            ina226_conversion_time_t *vbus_ct, ina226_conversion_time_t *ish_ct)
{
    CHECK_ARG(dev);

    CHECK(read_reg_16(dev, REG_CONFIG, &dev->config));
    if (mode)
        *mode = (dev->config & MASK_MODE) >> BIT_MODE;
    if (avg_mode)
        *avg_mode = (dev->config & MASK_AVG) >> BIT_AVG;
    if (vbus_ct)
        *vbus_ct = (dev->config & MASK_VBUSCT) >> BIT_VBUSCT;
    if (ish_ct)
        *ish_ct = (dev->config & MASK_ISHCT) >> BIT_ISHCT;
    return ESP_OK;
}

esp_err_t ina226_set_alert(ina226_t *dev, ina226_alert_mode_t mode, float limit,
                           bool cvrf, bool active_high, bool latch)
{
    CHECK_ARG(dev);

    uint16_t reg = 0;
    bool wl = true;
    int16_t l = 0;
    switch (mode)
    {
        case INA226_ALERT_OCL:
            reg |= BV(BIT_OCL);
            l = (int16_t)(limit / 0.00125);
            break;
        case INA226_ALERT_UCL:
            reg |= BV(BIT_UCL);
            l = (int16_t)(limit / 0.00125);
            break;
        case INA226_ALERT_BOL:
            reg |= BV(BIT_BOL);
            l = (int16_t)(limit / 0.00125);
            break;
        case INA226_ALERT_BUL:
            reg |= BV(BIT_BUL);
            l = (int16_t)(limit / 0.00125);
            break;
        case INA226_ALERT_POL:
            reg |= BV(BIT_POL);
            l = (int16_t)(limit / 0.01);
            break;
        default:
            wl = false;
    }
    if (cvrf)        reg |= BV(BIT_CVRF);
    if (active_high) reg |= BV(BIT_APOL);
    if (latch)       reg |= BV(BIT_LEN);
    if (wl)
        CHECK(write_reg_16(dev, REG_ALERT_LIMIT, l));

    CHECK(write_reg_16(dev, REG_MASK_EN, reg));

    return ESP_OK;
}

esp_err_t ina226_trigger(ina226_t *dev)
{
    CHECK_ARG(dev);

    uint16_t mode = (dev->config & MASK_MODE) >> BIT_MODE;
    if (mode < INA226_MODE_TRIG_SHUNT || mode > INA226_MODE_TRIG_SHUNT_BUS)
    {
        ESP_LOGE(TAG, "Could not trigger conversion in this mode: %d", mode);
        return ESP_ERR_INVALID_STATE;
    }

    return write_reg_16(dev, REG_CONFIG, dev->config);
}

esp_err_t ina226_get_status(ina226_t *dev, bool *ready, bool *alert, bool *overflow)
{
    CHECK_ARG(dev && (ready || alert || overflow));

    uint16_t val;
    CHECK(read_reg_16(dev, REG_MASK_EN, &val));
    ESP_LOGI(TAG, "Status: %x", val);

    if (ready)
        *ready = val & BIT_CVRF ? 1 : 0;
    if (alert)
        *alert = val & BIT_AFF ? 1 : 0;
    if (overflow)
        *overflow = val & BIT_OVF ? 1 : 0;

    return ESP_OK;
}

//esp_err_t ina226_get_current(ina226_t *dev, float *current)
esp_err_t ina226_get_shunt_voltage(ina226_t *dev, float *voltage)
{
    CHECK_ARG(dev && voltage);

    int16_t raw;
    CHECK(read_reg_16(dev, REG_SHUNT_VOLATGE, (uint16_t *)&raw));
    *voltage = raw * 2.5e-6; //  fixed 2.50 uV

    return ESP_OK;
}

esp_err_t ina226_get_bus_voltage(ina226_t *dev, float *voltage)
{
    CHECK_ARG(dev && voltage);

    uint16_t raw;
    CHECK(read_reg_16(dev, REG_BUS_VOLTAGE, &raw));

    *voltage = raw * 0.00125;

    return ESP_OK;
}

esp_err_t ina226_get_power(ina226_t *dev, float *power)
{
    CHECK_ARG(dev && power);

    uint16_t raw;
    CHECK(read_reg_16(dev, REG_POWER, &raw));

    *power = raw * 0.01;

    return ESP_OK;
}

esp_err_t ina226_set_calibrationregister(ina226_t *dev, uint16_t value)
{
    CHECK_ARG(dev);


    CHECK(write_reg_16(dev, REG_CALIBRATION, value));


    return ESP_OK;
}
