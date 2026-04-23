#include "board_compat.h"

#include <esp_sleep.h>

namespace {
constexpr uint8_t BOARD_BUTTON_A_PIN = 0;   // CONFIG
constexpr uint8_t BOARD_NO_BUTTON_PIN = 0xFF;
constexpr uint8_t BOARD_LCD_BACKLIGHT_PINS[] = {45, 47};
constexpr uint8_t BOARD_LCD_BACKLIGHT_CHANNELS[] = {0, 1};
constexpr uint32_t BOARD_LCD_BACKLIGHT_PWM_HZ = 5000;
constexpr uint8_t BOARD_LCD_BACKLIGHT_PWM_BITS = 8;
constexpr uint32_t BUTTON_DEBOUNCE_MS = 20;
constexpr uint8_t BOARD_GT911_ADDR_1 = 0x14;
constexpr uint8_t BOARD_GT911_ADDR_2 = 0x5D;
constexpr uint16_t BOARD_GT911_TOUCH_STATUS_REG = 0x814E;
constexpr uint8_t BOARD_GT911_TOUCH_READY = 0x80;
constexpr uint8_t BOARD_GT911_HAVE_KEY = 0x10;
constexpr uint32_t BOARD_MAIN_BUTTON_LATCH_MS = 120;
constexpr uint8_t BOARD_IMU_ADDR = 0x68;
constexpr uint8_t BOARD_IMU_REG_ACCEL_X1 = 0x0B;
constexpr uint8_t BOARD_IMU_REG_GYRO_X1 = 0x11;
constexpr uint8_t BOARD_IMU_REG_PWR_MGMT0 = 0x1F;
constexpr uint8_t BOARD_IMU_REG_GYRO_CONFIG0 = 0x20;
constexpr uint8_t BOARD_IMU_REG_ACCEL_CONFIG0 = 0x21;
constexpr uint8_t BOARD_IMU_REG_WHO_AM_I = 0x75;
constexpr uint8_t BOARD_IMU_WHO_AM_I = 0x60;
constexpr float BOARD_IMU_ACCEL_SCALE_2G = 16384.0f;
constexpr float BOARD_IMU_GYRO_SCALE_250DPS = 131.072f;

void setupBacklightPins() {
  for (size_t i = 0; i < sizeof(BOARD_LCD_BACKLIGHT_PINS); ++i) {
    ledcSetup(BOARD_LCD_BACKLIGHT_CHANNELS[i],
              BOARD_LCD_BACKLIGHT_PWM_HZ,
              BOARD_LCD_BACKLIGHT_PWM_BITS);
    ledcAttachPin(BOARD_LCD_BACKLIGHT_PINS[i], BOARD_LCD_BACKLIGHT_CHANNELS[i]);
  }
}

bool i2cReadReg16(uint8_t addr, uint16_t reg, uint8_t* data, size_t len) {
  Wire.beginTransmission(addr);
  Wire.write((uint8_t)(reg >> 8));
  Wire.write((uint8_t)(reg & 0xFF));
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)addr, (int)len) != (int)len) return false;
  for (size_t i = 0; i < len; ++i) data[i] = Wire.read();
  return true;
}

bool readGt911Status(uint8_t addr, uint8_t* status) {
  uint8_t value = 0;
  if (!i2cReadReg16(addr, BOARD_GT911_TOUCH_STATUS_REG, &value, 1)) return false;
  if (status) *status = value;
  return true;
}

bool readMainTouchButton(uint8_t* debugStatus) {
  uint8_t status = 0;
  if (!readGt911Status(BOARD_GT911_ADDR_1, &status)) {
    readGt911Status(BOARD_GT911_ADDR_2, &status);
  }
  if (debugStatus) *debugStatus = status;
  return (status & BOARD_GT911_TOUCH_READY) && (status & BOARD_GT911_HAVE_KEY);
}
}

BoardCompat M5;

BoardButton::BoardButton() = default;

void BoardButton::begin(uint8_t pin, bool active_low) {
  _pin = pin;
  _activeLow = active_low;
  if (_pin != BOARD_NO_BUTTON_PIN) {
    pinMode(_pin, active_low ? INPUT_PULLUP : INPUT);
  }
  _pressed = isPressed();
  _lastRaw = _pressed;
  _pressedSinceMs = _pressed ? millis() : 0;
}

void BoardButton::update() {
  _wasPressed = false;
  _wasReleased = false;

  bool raw = isPressed();
  uint32_t now = millis();
  if (raw != _lastRaw) {
    _lastRaw = raw;
    _lastChangeMs = now;
  }

  if ((now - _lastChangeMs) < BUTTON_DEBOUNCE_MS || raw == _pressed) return;

  _pressed = raw;
  if (_pressed) {
    _pressedSinceMs = now;
    _wasPressed = true;
  } else {
    _wasReleased = true;
  }
}

bool BoardButton::isPressed() const {
  bool gpioPressed = false;
  if (_pin != 0xFF) {
    bool level = digitalRead(_pin);
    gpioPressed = _activeLow ? !level : level;
  }
  return gpioPressed || _virtualPressed;
}

void BoardButton::setVirtualPressed(bool pressed) {
  _virtualPressed = pressed;
}

bool BoardButton::wasPressed() {
  bool v = _wasPressed;
  _wasPressed = false;
  return v;
}

bool BoardButton::wasReleased() {
  bool v = _wasReleased;
  _wasReleased = false;
  return v;
}

bool BoardButton::pressedFor(uint32_t ms) const {
  return _pressed && (millis() - _pressedSinceMs >= ms);
}

void BoardImu::Init() {
  Wire.begin(I2C_SDA, I2C_SCL, 400000U);
  delay(10);

  uint8_t who = 0;
  if (!readBytes(BOARD_IMU_REG_WHO_AM_I, &who, 1) || who != BOARD_IMU_WHO_AM_I) {
    _ready = false;
    Serial.printf("imu: WHO_AM_I failed (0x%02X)\n", who);
    return;
  }

  if (!writeByte(BOARD_IMU_REG_PWR_MGMT0, 0x0F)) {
    _ready = false;
    Serial.println("imu: failed to enable accel+gyro");
    return;
  }
  delay(50);

  // GYRO_CONFIG0: FS=+-250dps (bits 6:5 = 11), ODR=100Hz (bits 3:0 = 1001)
  if (!writeByte(BOARD_IMU_REG_GYRO_CONFIG0, 0x69)) {
    _ready = false;
    Serial.println("imu: failed to configure gyro");
    return;
  }
  delay(2);

  // ACCEL_CONFIG0: FS=+-2g (bits 6:5 = 11), ODR=100Hz (bits 3:0 = 1001)
  if (!writeByte(BOARD_IMU_REG_ACCEL_CONFIG0, 0x69)) {
    _ready = false;
    Serial.println("imu: failed to configure accel");
    return;
  }
  delay(2);

  _ready = true;
  Serial.println("imu: ready");
}

void BoardImu::getAccelData(float* ax, float* ay, float* az) const {
  if (!_ready) {
    if (ax) *ax = 0.0f;
    if (ay) *ay = 0.0f;
    if (az) *az = 1.0f;
    return;
  }

  uint8_t raw[6] = {0};
  if (!readBytes(BOARD_IMU_REG_ACCEL_X1, raw, sizeof(raw))) {
    if (ax) *ax = 0.0f;
    if (ay) *ay = 0.0f;
    if (az) *az = 1.0f;
    return;
  }

  int16_t rx = (int16_t)((raw[0] << 8) | raw[1]);
  int16_t ry = (int16_t)((raw[2] << 8) | raw[3]);
  int16_t rz = (int16_t)((raw[4] << 8) | raw[5]);

  // Board-space remap for the portrait UI orientation.
  if (ax) *ax = -(float)ry / BOARD_IMU_ACCEL_SCALE_2G;
  if (ay) *ay =  (float)rx / BOARD_IMU_ACCEL_SCALE_2G;
  if (az) *az =  (float)rz / BOARD_IMU_ACCEL_SCALE_2G;
}

void BoardImu::getGyroData(float* gx, float* gy, float* gz) const {
  if (!_ready) {
    if (gx) *gx = 0.0f;
    if (gy) *gy = 0.0f;
    if (gz) *gz = 0.0f;
    return;
  }

  uint8_t raw[6] = {0};
  if (!readBytes(BOARD_IMU_REG_GYRO_X1, raw, sizeof(raw))) {
    if (gx) *gx = 0.0f;
    if (gy) *gy = 0.0f;
    if (gz) *gz = 0.0f;
    return;
  }

  int16_t rx = (int16_t)((raw[0] << 8) | raw[1]);
  int16_t ry = (int16_t)((raw[2] << 8) | raw[3]);
  int16_t rz = (int16_t)((raw[4] << 8) | raw[5]);

  // Match the portrait-space axis remap used by accelerometer readings.
  if (gx) *gx = -(float)ry / BOARD_IMU_GYRO_SCALE_250DPS;
  if (gy) *gy =  (float)rx / BOARD_IMU_GYRO_SCALE_250DPS;
  if (gz) *gz =  (float)rz / BOARD_IMU_GYRO_SCALE_250DPS;
}

bool BoardImu::readBytes(uint8_t reg, uint8_t* data, size_t len) const {
  Wire.beginTransmission(BOARD_IMU_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)BOARD_IMU_ADDR, (int)len) != (int)len) return false;
  for (size_t i = 0; i < len; ++i) data[i] = Wire.read();
  return true;
}

bool BoardImu::writeByte(uint8_t reg, uint8_t value) const {
  Wire.beginTransmission(BOARD_IMU_ADDR);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

void BoardAxp::begin() {
  setupBacklightPins();
  _backlightOn = true;
  _backlightLevel = 255;
  applyBacklight();
}

void BoardAxp::ScreenBreath(uint8_t level) {
  _backlightLevel = map(level, 0, 100, 0, 255);
  applyBacklight();
}

void BoardAxp::SetLDO2(bool on) {
  _backlightOn = on;
  applyBacklight();
}

void BoardAxp::PowerOff() {
  SetLDO2(false);
  esp_sleep_enable_ext0_wakeup((gpio_num_t)BOARD_BUTTON_A_PIN, 0);
  esp_deep_sleep_start();
}

float BoardAxp::GetVBusVoltage() const {
  return 5.0f;
}

float BoardAxp::GetBatVoltage() const {
  return 0.0f;
}

int BoardAxp::GetBatCurrent() const {
  return 0;
}

float BoardAxp::GetTempInAXP192() const {
  return 0.0f;
}

uint8_t BoardAxp::GetBtnPress() const {
  return 0;
}

void BoardAxp::applyBacklight() const {
  uint8_t duty = _backlightOn ? _backlightLevel : 0;
  for (size_t i = 0; i < sizeof(BOARD_LCD_BACKLIGHT_CHANNELS); ++i) {
    ledcWrite(BOARD_LCD_BACKLIGHT_CHANNELS[i], duty);
  }
}

void BoardRtc::syncEpoch() {
  struct tm tmv = {};
  tmv.tm_sec = _time.Seconds;
  tmv.tm_min = _time.Minutes;
  tmv.tm_hour = _time.Hours;
  tmv.tm_mday = _date.Date ? _date.Date : 1;
  tmv.tm_mon = (_date.Month ? _date.Month : 1) - 1;
  tmv.tm_year = (_date.Year ? _date.Year : 2024) - 1900;
  _epochLocal = mktime(&tmv);
  _epochSetMs = millis();
}

void BoardRtc::SetTime(const RTC_TimeTypeDef* tm) {
  if (!tm) return;
  _time = *tm;
  syncEpoch();
}

void BoardRtc::SetDate(const RTC_DateTypeDef* dt) {
  if (!dt) return;
  _date = *dt;
  syncEpoch();
}

void BoardRtc::GetTime(RTC_TimeTypeDef* tm) const {
  if (!tm) return;
  time_t now = _epochLocal + (millis() - _epochSetMs) / 1000;
  struct tm local_tm = {};
  localtime_r(&now, &local_tm);
  tm->Hours = local_tm.tm_hour;
  tm->Minutes = local_tm.tm_min;
  tm->Seconds = local_tm.tm_sec;
}

void BoardRtc::GetDate(RTC_DateTypeDef* dt) const {
  if (!dt) return;
  time_t now = _epochLocal + (millis() - _epochSetMs) / 1000;
  struct tm local_tm = {};
  localtime_r(&now, &local_tm);
  dt->WeekDay = local_tm.tm_wday;
  dt->Month = local_tm.tm_mon + 1;
  dt->Date = local_tm.tm_mday;
  dt->Year = local_tm.tm_year + 1900;
}

void BoardCompat::begin() {
  BtnA.begin(BOARD_BUTTON_A_PIN, true);
  // BOX-3B MUTE is GPIO1 and must not act as B. The lower red-ring button is
  // exposed by the touch controller as a "main" key, so BtnB is virtual.
  BtnB.begin(BOARD_NO_BUTTON_PIN, true);
  Axp.begin();
  delay(20);
  Lcd.init();
  Axp.SetLDO2(true);
  Lcd.setRotation(1);
  Lcd.fillScreen(TFT_BLACK);
  _touchReady = initTouch();
}

void BoardCompat::update() {
  uint32_t now = millis();
  bool mainButtonPressed = false;
  _touchDebugStatus = 0;
  if (_touchReady) {
    mainButtonPressed = readMainTouchButton(&_touchDebugStatus);
    if (mainButtonPressed) _mainButtonLatchUntil = now + BOARD_MAIN_BUTTON_LATCH_MS;
    mainButtonPressed = mainButtonPressed || (int32_t)(now - _mainButtonLatchUntil) < 0;
  }
  if (_touchReady) {
    uint16_t x = 0, y = 0;
    _touchDown = readTouchPoint(&x, &y);
    if (_touchDown) {
      _touchX = x;
      _touchY = y;
      // LovyanGFX config exposes the BOX-3 red-ring area below active LCD.
      // Treat that virtual key zone as B if it appears as a touch coordinate.
      if (_touchY >= Lcd.height()) mainButtonPressed = true;
    }
  } else {
    _touchDown = false;
  }
  _mainButtonDown = mainButtonPressed;
  BtnA.setVirtualPressed(false);
  BtnB.setVirtualPressed(mainButtonPressed);
  BtnA.update();
  BtnB.update();

  static bool lastTouch = false;
  static bool lastMain = false;
  static uint16_t lastX = 0;
  static uint16_t lastY = 0;
  static uint8_t lastStatus = 0;
  bool changed = _touchDown != lastTouch || _mainButtonDown != lastMain ||
                 _touchX != lastX || _touchY != lastY ||
                 _touchDebugStatus != lastStatus;
  if (changed && (_touchDown || _mainButtonDown || lastTouch || lastMain || _touchDebugStatus || lastStatus)) {
    Serial.printf("input: touch=%u x=%u y=%u main=%u gt911=0x%02X a=%u b=%u\n",
                  _touchDown ? 1 : 0, _touchX, _touchY,
                  _mainButtonDown ? 1 : 0, _touchDebugStatus,
                  BtnA.isPressed() ? 1 : 0, BtnB.isPressed() ? 1 : 0);
  }
  lastTouch = _touchDown;
  lastMain = _mainButtonDown;
  lastX = _touchX;
  lastY = _touchY;
  lastStatus = _touchDebugStatus;
}

bool BoardCompat::getTouch(uint16_t* x, uint16_t* y) const {
  if (!_touchDown) return false;
  if (x) *x = _touchX;
  if (y) *y = _touchY;
  return true;
}

bool BoardCompat::getMainButton() const {
  return _mainButtonDown;
}

uint8_t BoardCompat::getTouchDebugStatus() const {
  return _touchDebugStatus;
}

bool BoardCompat::initTouch() {
  return Lcd.panel()->getTouch() != nullptr;
}

bool BoardCompat::readTouchPoint(uint16_t* x, uint16_t* y) {
  if (!_touchReady) return false;
  uint16_t tx = 0;
  uint16_t ty = 0;
  if (!Lcd.getTouch(&tx, &ty)) return false;
  if (x) *x = tx;
  if (y) *y = ty;
  return true;
}
