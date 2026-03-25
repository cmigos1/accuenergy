/**
 * @file    calibration.cpp
 * @brief   Calibration system with NVS persistence
 */

#include "calibration.h"
#include <Preferences.h>
#include <Arduino.h>

static Preferences prefs;
static calibration_t s_cal;

/* NVS namespace and keys */
#define NVS_NAMESPACE   "nilm_cal"
#define KEY_V_GAIN      "v_gain"
#define KEY_V_OFFSET    "v_offset"
#define KEY_I_GAIN      "i_gain"
#define KEY_I_OFFSET    "i_offset"
#define KEY_P_GAIN      "p_gain"
#define KEY_P_OFFSET    "p_offset"

void calibration_init()
{
    prefs.begin(NVS_NAMESPACE, false);  /* read-write mode */

    /* Load from NVS or use defaults */
    s_cal.voltage_gain   = prefs.getFloat(KEY_V_GAIN,   1.0f);
    s_cal.voltage_offset = prefs.getFloat(KEY_V_OFFSET, 0.0f);
    s_cal.current_gain   = prefs.getFloat(KEY_I_GAIN,   1.0f);
    s_cal.current_offset = prefs.getFloat(KEY_I_OFFSET, 0.0f);
    s_cal.power_gain     = prefs.getFloat(KEY_P_GAIN,   1.0f);
    s_cal.power_offset   = prefs.getFloat(KEY_P_OFFSET, 0.0f);

    prefs.end();

    Serial.printf("[CAL] Loaded: Vg=%.4f Vo=%.2f Ig=%.4f Io=%.2f Pg=%.4f Po=%.2f\n",
                  s_cal.voltage_gain, s_cal.voltage_offset,
                  s_cal.current_gain, s_cal.current_offset,
                  s_cal.power_gain,   s_cal.power_offset);
}

const calibration_t* calibration_get()
{
    return &s_cal;
}

void calibration_set(const calibration_t *cal)
{
    s_cal = *cal;

    /* Persist to NVS */
    prefs.begin(NVS_NAMESPACE, false);
    prefs.putFloat(KEY_V_GAIN,   s_cal.voltage_gain);
    prefs.putFloat(KEY_V_OFFSET, s_cal.voltage_offset);
    prefs.putFloat(KEY_I_GAIN,   s_cal.current_gain);
    prefs.putFloat(KEY_I_OFFSET, s_cal.current_offset);
    prefs.putFloat(KEY_P_GAIN,   s_cal.power_gain);
    prefs.putFloat(KEY_P_OFFSET, s_cal.power_offset);
    prefs.end();

    Serial.println("[CAL] Calibration updated and saved to NVS");
}

void calibration_reset()
{
    calibration_t defaults = {1.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f};
    calibration_set(&defaults);
    Serial.println("[CAL] Reset to factory defaults");
}

void calibration_apply(nilm_data_t *data)
{
    data->vrms           = (data->vrms * s_cal.voltage_gain) + s_cal.voltage_offset;
    data->irms           = (data->irms * s_cal.current_gain) + s_cal.current_offset;
    data->power_active   = (data->power_active * s_cal.power_gain) + s_cal.power_offset;
    data->power_apparent = (data->power_apparent * s_cal.power_gain) + s_cal.power_offset;

    /* Recalculate PF after calibration */
    if (data->power_apparent > 0.001f) {
        data->power_factor = data->power_active / data->power_apparent;
    }
}
