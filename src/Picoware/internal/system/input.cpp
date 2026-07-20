#include "../../internal/system/input.hpp"
#include <Wire.h>
#include <TFT_eSPI.h>
#include <string.h>

// FT6336 capacitive touch controller (shares the I2C bus brought up in setup()).
#define PW_FT6336_ADDR      0x38
#define PW_FT6336_TD_STATUS 0x02
// Panel-native (portrait) resolution of the Pancake ST7796 + FT6336.
#define PW_PANEL_W 320
#define PW_PANEL_H 480
// XPT2046 pressure threshold (resistive backend); matches the touch keyboard.
#define PW_XPT_THRESHOLD 600

namespace Picoware
{
    // FT6336 raw read (panel-native portrait coords)
    static bool ft6336_read_raw(uint16_t &rx, uint16_t &ry)
    {
        uint8_t d[7];
        Wire.beginTransmission(PW_FT6336_ADDR);
        Wire.write(PW_FT6336_TD_STATUS);
        if (Wire.endTransmission(false) != 0)
            return false;
        Wire.requestFrom((int)PW_FT6336_ADDR, 7);
        for (uint8_t i = 0; i < 7; i++)
            d[i] = Wire.available() ? Wire.read() : 0;
        if ((d[0] & 0x0F) == 0)
            return false;
        rx = ((uint16_t)(d[1] & 0x0F) << 8) | d[2];
        ry = ((uint16_t)(d[3] & 0x0F) << 8) | d[4];
        return true;
    }

    // TouchInput
    TouchInput::TouchInput(uint16_t width, uint16_t height, uint8_t rotation)
        : lastButton(-1), w(width), h(height), nw(width), nh(height), rot(rotation & 3),
          px(0), py(0), pressed(false), wasDown(false), lastMs(0)
    {
    }

    // Keep the rotation mapping and the clamp bounds in step. w/h are the
    // SCREEN dimensions for the active rotation, so they swap on the odd
    // rotations; without that the clamp below would cut a landscape screen off
    // at the panel's portrait width.
    void TouchInput::setRotation(uint8_t rotation) noexcept
    {
        rot = rotation & 3;
        if (rot & 1) { w = nh; h = nw; }    // odd rotations swap the screen axes
        else         { w = nw; h = nh; }
    }

    bool TouchInput::readPanel(uint16_t &sx, uint16_t &sy)
    {
        // Resistive backend: TFT_eSPI applies the stored calibration and the
        // active rotation, so the coords come back already in screen space.
        if (tft != nullptr)
        {
            uint16_t tx, ty;
            if (!tft->getTouch(&tx, &ty, PW_XPT_THRESHOLD))
                return false;
            sx = tx;
            sy = ty;
            if (sx >= w) sx = w - 1;
            if (sy >= h) sy = h - 1;
            return true;
        }

        uint16_t rx, ry;
        if (!ft6336_read_raw(rx, ry))
            return false;
        // Map panel-native portrait coords into the active rotation's screen space.
        switch (rot)
        {
        case 0: sx = rx;                     sy = ry;                     break;
        case 1: sx = ry;                     sy = (uint16_t)(PW_PANEL_W - 1 - rx); break;
        case 2: sx = (uint16_t)(PW_PANEL_W - 1 - rx); sy = (uint16_t)(PW_PANEL_H - 1 - ry); break;
        case 3: sx = (uint16_t)(PW_PANEL_H - 1 - ry); sy = rx;           break;
        }
        if (sx >= w) sx = w - 1;
        if (sy >= h) sy = h - 1;
        return true;
    }

    void TouchInput::run()
    {
        lastButton = -1;
        uint16_t sx, sy;
        bool down = readPanel(sx, sy);

        if (down)
        {
            px = sx;
            py = sy;
            pressed = true;
            // Fire a button code only on a fresh press edge (debounced).
            uint32_t now = millis();
            if (!wasDown && (now - lastMs) >= DEBOUNCE_MS)
            {
                lastMs = now;
                if (sy < (uint16_t)(h / 5))
                    lastButton = BUTTON_UP;
                else if (sy > (uint16_t)(h * 4 / 5))
                    lastButton = BUTTON_DOWN;
                else if (sx < (uint16_t)(w / 4))
                    lastButton = BUTTON_LEFT;
                else if (sx > (uint16_t)(w * 3 / 4))
                    lastButton = BUTTON_RIGHT;
                else
                    lastButton = BUTTON_CENTER;
            }
            wasDown = true;
        }
        else
        {
            pressed = false;
            wasDown = false;
        }
    }

    void TouchInput::reset()
    {
        lastButton = -1;
        pressed = false;
        wasDown = false;
        lastMs = millis();
    }

    // Input
    Input::Input()
        : pin(-1), buttonAssignment(-1), lastButton(-1), debounce(0.05f),
          startTime(0), elapsedTime(0), wasPressed(false), touch(nullptr)
    {
    }

    Input::Input(uint8_t pin, uint8_t button, float debounce)
        : pin(pin), buttonAssignment(button), lastButton(-1), debounce(debounce),
          startTime(0), elapsedTime(0), wasPressed(false), touch(nullptr)
    {
        pinMode(this->pin, INPUT_PULLUP);
    }

    Input::Input(TouchInput *touch)
        : pin(-1), buttonAssignment(BUTTON_NONE), lastButton(-1), debounce(0.01f),
          startTime(0), elapsedTime(0), wasPressed(false), touch(touch)
    {
    }

    bool Input::isPressed()
    {
        if (this->touch)
            return this->touch->isPressed();
        if (this->pin != -1)
            return digitalRead(this->pin) == LOW;
        return false;
    }

    bool Input::isHeld(uint8_t duration)
    {
        return this->isPressed() && this->elapsedTime >= duration;
    }

    void Input::reset()
    {
        this->elapsedTime = 0;
        this->wasPressed = false;
        this->lastButton = -1;
        if (this->touch)
            this->touch->reset();
        else
            this->startTime = millis();
    }

    void Input::run()
    {
        if (this->touch)
        {
            this->touch->run();
            this->lastButton = this->touch->lastButton;
            if (this->lastButton != -1)
            {
                this->buttonAssignment = (uint8_t)this->lastButton;
                this->elapsedTime++;
                this->wasPressed = true;
            }
            else
            {
                this->wasPressed = false;
                this->elapsedTime = 0;
            }
        }
        else if (this->pin != -1 && millis() - this->startTime > this->debounce)
        {
            this->startTime = millis();
            if (digitalRead(this->pin) == LOW)
            {
                this->lastButton = this->buttonAssignment;
                this->elapsedTime++;
                this->wasPressed = true;
            }
            else
            {
                this->lastButton = -1;
                this->wasPressed = false;
                this->elapsedTime = 0;
            }
        }
    }

    Input::operator bool() const
    {
        if (this->touch)
            return this->touch != nullptr;
        return this->pin != -1;
    }
}
