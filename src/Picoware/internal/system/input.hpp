#pragma once
#include "Arduino.h"
#include "../../internal/gui/vector.hpp"
#include "../../internal/boards.hpp"
#include "../../internal/system/buttons.hpp"
class TFT_eSPI; // fwd-declared: only the resistive backend touches it

namespace Picoware
{
    // TouchInput — panel touch, with two backends:
    //   * FT6336 capacitive over I2C (Pancake) — the default.
    //   * XPT2046 resistive via TFT_eSPI::getTouch() (Marauder V8) — selected by
    //     calling attachTFT(); getTouch() already returns calibrated, rotated
    //     screen coords, so no mapping is done for it here.
    // Branching on the attached pointer keeps this file board-agnostic.
    //
    // Produces two things each run():
    //   * lastButton: a BUTTON_* code derived from screen tap-zones so Picoware's
    //     existing button-navigated views (menus, on-screen keyboard, games) work
    //     unchanged. Zones:  top edge = UP, bottom edge = DOWN, left = LEFT,
    //     right = RIGHT, center = CENTER/OK.
    //   * a raw touch point (x, y in rotated screen coords) + pressed flag for
    //     views that want direct hit-testing (the hybrid touch UI).
    // Assumes Wire.begin() + FT6336 reset has already run (done in the sketch
    // setup via ft6336_init()).
    class TouchInput
    {
    public:
        TouchInput(uint16_t width, uint16_t height, uint8_t rotation);
        void run();                 // poll the panel, update lastButton / point
        void reset();               // clear state + debounce window
        // Switch to the resistive backend (XPT2046 read through TFT_eSPI).
        // Leave unset for FT6336 capacitive.
        void attachTFT(TFT_eSPI *t) noexcept { tft = t; }
        // Rotation is fixed at construction from the board config, but a view may
        // rotate the panel at runtime (the Scrabble board runs landscape). Call
        // this alongside TFT_eSPI::setRotation() so the coordinate mapping AND
        // the clamp bounds follow — leaving either stale makes taps land in the
        // wrong place and puts the far edge of the screen out of reach.
        void setRotation(uint8_t rotation) noexcept;
        bool isPressed() const noexcept { return pressed; }
        uint16_t x() const noexcept { return px; }
        uint16_t y() const noexcept { return py; }
        Vector point() const noexcept { return Vector(px, py); }

        int lastButton; // BUTTON_* from tap zone this frame, or -1

    private:
        bool readPanel(uint16_t &sx, uint16_t &sy); // raw read + rotation map
        TFT_eSPI *tft = nullptr;                    // set => resistive backend
        uint16_t w, h;        // screen bounds for the ACTIVE rotation (clamp)
        uint16_t nw, nh;      // panel-native (portrait) bounds, fixed at construction
        uint8_t rot;
        uint16_t px, py;
        bool pressed;
        bool wasDown;
        uint32_t lastMs;
        static const uint32_t DEBOUNCE_MS = 120;
    };

    // Input — a single logical input source. Pancake uses the TouchInput backend;
    // the (pin, button) GPIO backend is retained for boards with physical buttons.
    class Input
    {
    public:
        Input();
        Input(uint8_t pin, uint8_t button, float debounce = 0.05f);
        Input(TouchInput *touch);
        //
        TouchInput *getTouch() const noexcept { return this->touch; }
        uint8_t getButtonAssignment() const noexcept { return this->buttonAssignment; }
        int getLastButton() const noexcept { return this->lastButton; }
        uint8_t getPin() const noexcept { return this->pin; }
        //
        bool isPressed();
        bool isHeld(uint8_t duration = 3);
        void reset();
        void run();
        //
        operator bool() const;

    private:
        uint8_t pin;
        uint8_t buttonAssignment;
        int lastButton;
        float debounce;
        unsigned long startTime;
        float elapsedTime;
        bool wasPressed;
        TouchInput *touch;
    };
}
