#include "measurement_hardware.h"

#include <driver/adc.h>

namespace measurement {
namespace {

constexpr uint8_t CURRENT_PINS[CURRENT_CHANNEL_COUNT] = {36, 39, 34, 35};
constexpr uint8_t BATTERY_VOLTAGE_PIN = 33;
constexpr uint8_t BATTERY_CURRENT_PIN = 32;
constexpr float DEFAULT_VOLTAGE_RAW_PER_VOLT = 2620.0F / 12.0F;
// Provisional for a bidirectional 5 V / 50 A Hall sensor with 40 mV/A
// sensitivity and the same 22k/22k divider as the bike current channels.
// It is not used for system power until two real calibration points exist.
constexpr float DEFAULT_BATTERY_CURRENT_RAW_PER_AMP =
    (2792.0F - 1551.0F) / 50.0F;

float interpolatePair(const uint16_t raw, const CalibrationPoint &left,
                      const CalibrationPoint &right) {
  const float span = static_cast<float>(right.raw) - left.raw;
  if (span <= 0.0F) {
    return left.value;
  }
  const float fraction =
      (static_cast<float>(raw) - static_cast<float>(left.raw)) / span;
  return left.value + fraction * (right.value - left.value);
}

template <size_t PointCount>
float calibratedValue(const uint16_t raw,
                      const CalibrationPoint (&calibration)[PointCount],
                      const float defaultRawPerUnit,
                      const float defaultRawAtZero) {
  CalibrationPoint points[PointCount];
  uint8_t count = 0;
  for (const CalibrationPoint &point : calibration) {
    if (!point.calibrated) continue;
    points[count++] = point;
  }

  if (count == 0) {
    return (static_cast<float>(raw) - defaultRawAtZero) / defaultRawPerUnit;
  }
  if (count == 1) {
    return points[0].value +
           (static_cast<float>(raw) - points[0].raw) / defaultRawPerUnit;
  }

  // Raw values normally rise with the reference value. Sorting also keeps the
  // conversion deterministic if points were captured in an unusual order.
  for (uint8_t index = 1; index < count; ++index) {
    CalibrationPoint point = points[index];
    int8_t insertAt = index - 1;
    while (insertAt >= 0 && points[insertAt].raw > point.raw) {
      points[insertAt + 1] = points[insertAt];
      --insertAt;
    }
    points[insertAt + 1] = point;
  }

  uint8_t upper = 1;
  while (upper < count - 1 && raw > points[upper].raw) ++upper;
  return interpolatePair(raw, points[upper - 1], points[upper]);
}

}  // namespace

void Hardware::begin() {
  analogReadResolution(12);
  for (const uint8_t pin : CURRENT_PINS) {
    pinMode(pin, INPUT);
    analogSetPinAttenuation(pin, ADC_11db);
  }
  pinMode(BATTERY_VOLTAGE_PIN, INPUT);
  analogSetPinAttenuation(BATTERY_VOLTAGE_PIN, ADC_11db);
  pinMode(BATTERY_CURRENT_PIN, INPUT);
  analogSetPinAttenuation(BATTERY_CURRENT_PIN, ADC_11db);

  for (uint8_t channel = 0; channel < CURRENT_CHANNEL_COUNT; ++channel) {
    for (uint8_t point = 0; point < CURRENT_CALIBRATION_POINT_COUNT; ++point) {
      currentCalibration_[channel][point].value = static_cast<float>(point);
    }
  }
  constexpr float voltageValues[VOLTAGE_CALIBRATION_POINT_COUNT] = {
      8.0F, 10.0F, 12.0F, 14.0F};
  for (uint8_t point = 0; point < VOLTAGE_CALIBRATION_POINT_COUNT; ++point) {
    voltageCalibration_[point].value = voltageValues[point];
  }
  constexpr float batteryCurrentValues[BATTERY_CURRENT_CALIBRATION_POINT_COUNT] = {
      -4.0F, -2.0F, 0.0F, 2.0F, 4.0F};
  for (uint8_t point = 0;
       point < BATTERY_CURRENT_CALIBRATION_POINT_COUNT; ++point) {
    batteryCurrentCalibration_[point].value = batteryCurrentValues[point];
  }
  resetAveragingWindow(millis());
}

bool Hardware::update(const uint32_t nowMs) {
  const uint16_t voltageRaw = analogRead(BATTERY_VOLTAGE_PIN);
  voltageSum_ += voltageRaw;
  voltageMinimum_ = min(voltageMinimum_, voltageRaw);
  voltageMaximum_ = max(voltageMaximum_, voltageRaw);
  const uint16_t batteryCurrentRaw = analogRead(BATTERY_CURRENT_PIN);
  batteryCurrentSum_ += batteryCurrentRaw;
  batteryCurrentMinimum_ = min(batteryCurrentMinimum_, batteryCurrentRaw);
  batteryCurrentMaximum_ = max(batteryCurrentMaximum_, batteryCurrentRaw);
  for (uint8_t index = 0; index < CURRENT_CHANNEL_COUNT; ++index) {
    const uint16_t raw = analogRead(CURRENT_PINS[index]);
    currentSum_[index] += raw;
    currentMinimum_[index] = min(currentMinimum_[index], raw);
    currentMaximum_[index] = max(currentMaximum_[index], raw);
  }
  ++sampleCount_;

  if (nowMs - windowStartedAtMs_ < ADC_AVERAGING_WINDOW_MS) {
    return false;
  }

  Sample &voltage = snapshot_.batteryVoltage;
  voltage.raw = static_cast<uint16_t>((voltageSum_ + sampleCount_ / 2) / sampleCount_);
  voltage.rawMin = voltageMinimum_;
  voltage.rawMax = voltageMaximum_;
  voltage.value = calibratedValue(voltage.raw, voltageCalibration_,
                                  DEFAULT_VOLTAGE_RAW_PER_VOLT, 0.0F);
  voltage.valid = voltageMaximum_ < 4095;
  Sample &batteryCurrent = snapshot_.batteryCurrent;
  batteryCurrent.raw = static_cast<uint16_t>(
      (batteryCurrentSum_ + sampleCount_ / 2) / sampleCount_);
  batteryCurrent.rawMin = batteryCurrentMinimum_;
  batteryCurrent.rawMax = batteryCurrentMaximum_;
  batteryCurrent.value = calibratedValue(
      batteryCurrent.raw, batteryCurrentCalibration_,
      DEFAULT_BATTERY_CURRENT_RAW_PER_AMP, 1551.0F);
  batteryCurrent.valid = batteryCurrentMinimum_ > 0 &&
                         batteryCurrentMaximum_ < 4095;
  for (uint8_t index = 0; index < CURRENT_CHANNEL_COUNT; ++index) {
    Sample &current = snapshot_.current[index];
    current.raw = static_cast<uint16_t>(
        (currentSum_[index] + sampleCount_ / 2) / sampleCount_);
    current.rawMin = currentMinimum_[index];
    current.rawMax = currentMaximum_[index];
    current.value = calibratedValue(current.raw, currentCalibration_[index],
                                    CURRENT_RAW_PER_AMP, 1551.0F);
    current.valid = currentMinimum_[index] > 0 && currentMaximum_[index] < 4095;
  }
  snapshot_.sampledAtMs = millis();
  ++snapshot_.sequence;
  resetAveragingWindow(nowMs);
  return true;
}

void Hardware::resetAveragingWindow(const uint32_t nowMs) {
  windowStartedAtMs_ = nowMs;
  voltageSum_ = 0;
  batteryCurrentSum_ = 0;
  voltageMinimum_ = 4095;
  voltageMaximum_ = 0;
  batteryCurrentMinimum_ = 4095;
  batteryCurrentMaximum_ = 0;
  sampleCount_ = 0;
  for (uint8_t index = 0; index < CURRENT_CHANNEL_COUNT; ++index) {
    currentSum_[index] = 0;
    currentMinimum_[index] = 4095;
    currentMaximum_[index] = 0;
  }
}

void Hardware::setCurrentCalibration(const uint8_t channel, const uint8_t point,
                                     const uint16_t raw,
                                     const bool calibrated) {
  if (channel >= CURRENT_CHANNEL_COUNT ||
      point >= CURRENT_CALIBRATION_POINT_COUNT) return;
  currentCalibration_[channel][point].raw = raw;
  currentCalibration_[channel][point].calibrated = calibrated;
}

void Hardware::setVoltageCalibration(const uint8_t point, const uint16_t raw,
                                     const bool calibrated) {
  if (point >= VOLTAGE_CALIBRATION_POINT_COUNT) return;
  voltageCalibration_[point].raw = raw;
  voltageCalibration_[point].calibrated = calibrated;
}

void Hardware::setBatteryCurrentCalibration(const uint8_t point,
                                            const uint16_t raw,
                                            const bool calibrated) {
  if (point >= BATTERY_CURRENT_CALIBRATION_POINT_COUNT) return;
  batteryCurrentCalibration_[point].raw = raw;
  batteryCurrentCalibration_[point].calibrated = calibrated;
}

const CalibrationPoint &Hardware::currentCalibration(const uint8_t channel,
                                                      const uint8_t point) const {
  return currentCalibration_[channel][point];
}

const CalibrationPoint &Hardware::voltageCalibration(const uint8_t point) const {
  return voltageCalibration_[point];
}

const CalibrationPoint &Hardware::batteryCurrentCalibration(
    const uint8_t point) const {
  return batteryCurrentCalibration_[point];
}

bool Hardware::batteryCurrentIsCalibrated() const {
  uint8_t count = 0;
  for (const CalibrationPoint &point : batteryCurrentCalibration_) {
    if (point.calibrated) ++count;
  }
  return count >= 2;
}

}  // namespace measurement
