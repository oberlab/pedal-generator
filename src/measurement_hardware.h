#pragma once

#include <Arduino.h>

namespace measurement {

constexpr uint8_t CURRENT_CHANNEL_COUNT = 4;
constexpr uint8_t CURRENT_CALIBRATION_POINT_COUNT = 1;
constexpr uint8_t VOLTAGE_CALIBRATION_POINT_COUNT = 4;
constexpr uint8_t BATTERY_CURRENT_CALIBRATION_POINT_COUNT = 5;
constexpr uint16_t ADC_AVERAGING_WINDOW_MS = 500;

// Reference measurement of the bike current channels: 43.5 ADC digits/A were
// measured with the former 22k/22k divider (ratio 0.5). Other dividers scale
// this value with their ratio as long as no explicit rawPerAmp is configured.
constexpr float REFERENCE_CURRENT_DIVIDER_RATIO = 0.5F;
constexpr float REFERENCE_CURRENT_RAW_PER_AMP = 43.5F;

// Defaults of the bike current channels (ACS712-30A, schematic rev 0.2):
// R1/R3/R5/R7 = 30k in series, R2/R4/R6/R14 = 100k to GND.
constexpr float DEFAULT_CURRENT_DIVIDER_UPPER_OHM = 30000.0F;
constexpr float DEFAULT_CURRENT_DIVIDER_LOWER_OHM = 100000.0F;
// Estimated raw value at 0 A (about 1.92 V at the ADC pin). Only used until the
// zero calibration has been stored; interpolated from the battery-voltage
// calibration curve, accuracy roughly +-60 digits.
constexpr float DEFAULT_CURRENT_ZERO_RAW = 2170.0F;

// Runtime parameters of the raw -> physical value conversion. The defaults
// below are used for every value that is not set in /config.json.
struct Parameters {
  float currentDividerUpperOhm = DEFAULT_CURRENT_DIVIDER_UPPER_OHM;
  float currentDividerLowerOhm = DEFAULT_CURRENT_DIVIDER_LOWER_OHM;
  // 0 = derive from the divider (see REFERENCE_CURRENT_*).
  float currentRawPerAmp = 0.0F;
  float currentZeroRaw = DEFAULT_CURRENT_ZERO_RAW;
  float voltageRawPerVolt = 2620.0F / 12.0F;
  // Provisional for a bidirectional 5 V / 50 A Hall sensor with 40 mV/A
  // sensitivity and the same 22k/22k divider as the former bike channels.
  // It is not used for system power until two real calibration points exist.
  float batteryCurrentRawPerAmp = (2792.0F - 1551.0F) / 50.0F;
  float batteryCurrentZeroRaw = 1551.0F;

  // Voltage at the ADC pin per volt at the sensor output.
  float currentDividerRatio() const {
    return currentDividerLowerOhm / (currentDividerUpperOhm + currentDividerLowerOhm);
  }
  float effectiveCurrentRawPerAmp() const {
    return currentRawPerAmp > 0.0F
               ? currentRawPerAmp
               : REFERENCE_CURRENT_RAW_PER_AMP * currentDividerRatio() /
                     REFERENCE_CURRENT_DIVIDER_RATIO;
  }
};

struct CalibrationPoint {
  uint16_t raw = 0;
  float value = 0.0F;
  bool calibrated = false;
};

struct Sample {
  uint16_t raw = 0;
  uint16_t rawMin = 0;
  uint16_t rawMax = 0;
  float value = 0.0F;
  bool valid = false;
};

struct Snapshot {
  Sample batteryVoltage;
  Sample batteryCurrent;
  Sample current[CURRENT_CHANNEL_COUNT];
  uint32_t sequence = 0;
  uint32_t sampledAtMs = 0;
};

// Hardware abstraction for the five existing ADC1 inputs plus the new
// bidirectional battery-current input on GPIO32.
// Each published sample contains all ADC readings collected over 500 ms.
class Hardware {
 public:
  void begin();
  bool update(uint32_t nowMs);
  const Snapshot &snapshot() const { return snapshot_; }
  void setParameters(const Parameters &parameters) { parameters_ = parameters; }
  const Parameters &parameters() const { return parameters_; }
  void setCurrentCalibration(uint8_t channel, uint8_t point, uint16_t raw,
                             bool calibrated);
  void setVoltageCalibration(uint8_t point, uint16_t raw, bool calibrated);
  void setBatteryCurrentCalibration(uint8_t point, uint16_t raw,
                                    bool calibrated);
  const CalibrationPoint &currentCalibration(uint8_t channel,
                                              uint8_t point) const;
  const CalibrationPoint &voltageCalibration(uint8_t point) const;
  const CalibrationPoint &batteryCurrentCalibration(uint8_t point) const;
  bool batteryCurrentIsCalibrated() const;

 private:
  void resetAveragingWindow(uint32_t nowMs);

  Snapshot snapshot_;
  Parameters parameters_;
  CalibrationPoint currentCalibration_[CURRENT_CHANNEL_COUNT]
                                      [CURRENT_CALIBRATION_POINT_COUNT];
  CalibrationPoint voltageCalibration_[VOLTAGE_CALIBRATION_POINT_COUNT];
  CalibrationPoint batteryCurrentCalibration_
      [BATTERY_CURRENT_CALIBRATION_POINT_COUNT];
  uint32_t windowStartedAtMs_ = 0;
  uint32_t voltageSum_ = 0;
  uint32_t batteryCurrentSum_ = 0;
  uint32_t currentSum_[CURRENT_CHANNEL_COUNT] = {};
  uint16_t voltageMinimum_ = 4095;
  uint16_t voltageMaximum_ = 0;
  uint16_t batteryCurrentMinimum_ = 4095;
  uint16_t batteryCurrentMaximum_ = 0;
  uint16_t currentMinimum_[CURRENT_CHANNEL_COUNT] = {};
  uint16_t currentMaximum_[CURRENT_CHANNEL_COUNT] = {};
  uint16_t sampleCount_ = 0;
};

}  // namespace measurement
