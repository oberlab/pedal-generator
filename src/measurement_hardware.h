#pragma once

#include <Arduino.h>

namespace measurement {

constexpr uint8_t CURRENT_CHANNEL_COUNT = 4;
constexpr uint8_t CURRENT_CALIBRATION_POINT_COUNT = 1;
constexpr float CURRENT_RAW_PER_AMP = 43.5F;
constexpr uint8_t VOLTAGE_CALIBRATION_POINT_COUNT = 4;
constexpr uint8_t BATTERY_CURRENT_CALIBRATION_POINT_COUNT = 5;
constexpr uint16_t ADC_AVERAGING_WINDOW_MS = 500;

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
