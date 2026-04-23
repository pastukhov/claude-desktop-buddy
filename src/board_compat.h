#pragma once

#include <Arduino.h>
#include <FS.h>
#include <LGFX_TFT_eSPI.hpp>
#include <time.h>
#include <Wire.h>

using File = fs::File;

typedef struct {
  uint8_t Hours;
  uint8_t Minutes;
  uint8_t Seconds;
} RTC_TimeTypeDef;

typedef struct {
  uint8_t WeekDay;
  uint8_t Month;
  uint8_t Date;
  uint16_t Year;
} RTC_DateTypeDef;

class BoardButton {
public:
  BoardButton();
  void begin(uint8_t pin, bool active_low = true);
  void update();
  void setVirtualPressed(bool pressed);
  bool isPressed() const;
  bool wasPressed();
  bool wasReleased();
  bool pressedFor(uint32_t ms) const;

private:
  uint8_t _pin = 0xFF;
  bool _activeLow = true;
  bool _pressed = false;
  bool _lastRaw = false;
  bool _virtualPressed = false;
  bool _wasPressed = false;
  bool _wasReleased = false;
  uint32_t _lastChangeMs = 0;
  uint32_t _pressedSinceMs = 0;
};

class BoardBeep {
public:
  void begin() {}
  void update() {}
  void tone(uint16_t, uint16_t) {}
};

class BoardImu {
public:
  void Init();
  void getAccelData(float* ax, float* ay, float* az) const;
  void getGyroData(float* gx, float* gy, float* gz) const;

private:
  bool readBytes(uint8_t reg, uint8_t* data, size_t len) const;
  bool writeByte(uint8_t reg, uint8_t value) const;

  bool _ready = false;
};

class BoardAxp {
public:
  void begin();
  void ScreenBreath(uint8_t level);
  void SetLDO2(bool on);
  void PowerOff();
  float GetVBusVoltage() const;
  float GetBatVoltage() const;
  int GetBatCurrent() const;
  float GetTempInAXP192() const;
  uint8_t GetBtnPress() const;

private:
  void applyBacklight() const;

  bool _backlightOn = true;
  uint8_t _backlightLevel = 255;
};

class BoardRtc {
public:
  void SetTime(const RTC_TimeTypeDef* tm);
  void SetDate(const RTC_DateTypeDef* dt);
  void GetTime(RTC_TimeTypeDef* tm) const;
  void GetDate(RTC_DateTypeDef* dt) const;

private:
  void syncEpoch();

  RTC_TimeTypeDef _time = {0, 0, 0};
  RTC_DateTypeDef _date = {0, 1, 1, 2024};
  time_t _epochLocal = 0;
  uint32_t _epochSetMs = 0;
};

class BoardCompat {
public:
  TFT_eSPI Lcd;
  BoardButton BtnA;
  BoardButton BtnB;
  BoardBeep Beep;
  BoardImu Imu;
  BoardAxp Axp;
  BoardRtc Rtc;

  void begin();
  void update();
  bool getTouch(uint16_t* x, uint16_t* y) const;
  bool getMainButton() const;
  uint8_t getTouchDebugStatus() const;

private:
  bool initTouch();
  bool readTouchPoint(uint16_t* x, uint16_t* y);

  bool _touchReady = false;
  bool _touchDown = false;
  bool _mainButtonDown = false;
  uint32_t _mainButtonLatchUntil = 0;
  uint8_t _touchDebugStatus = 0;
  uint16_t _touchX = 0;
  uint16_t _touchY = 0;
};

extern BoardCompat M5;
