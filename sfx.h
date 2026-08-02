// sfx.h
// Non-blocking sound effects for the passive buzzer (Pancake, GPIO6), modelled
// on M5PORKCHOP's sfx module: each effect is a short sequence of
// {frequency, duration, pause} notes, advanced a step at a time by update()
// from the main loop — never with delay(), so the game keeps running.
//
// The buzzer is driven with an LEDC square-wave tone. On a board without a
// buzzer (HAS_BUZZER undefined, e.g. the V8) every call is a no-op.
#pragma once
#include <stdint.h>

namespace Sfx {

enum Event : uint8_t {
  NONE = 0,
  REVEAL,       // a safe block (or flood) opened - soft tick
  FLAG,         // flag placed / removed - lower click
  CHORD,        // chord opened neighbours - quick double tick
  BOOM,         // hit a mine - descending explosion (priority)
  WIN,          // board solved - ascending fanfare (priority)
  BURST,        // Burst Clear powerup - rising pop
  LIGHTNING,    // Lightning powerup - zap crackle
  LIFE_ARM,     // Lifesaver armed - ready two-step
  LIFE_SAVE,    // Lifesaver spent a mine - relief resolve
  SONAR,        // Sonar ping - rising ping with an echo
  MODE_ENTER,   // opened a menu item / screen - ascending pair
  MODE_EXIT,    // backed out of a screen - descending pair
  CLICK         // generic UI tap
};

// Attach the buzzer pin. Call once from setup().
void init();

// Turn sound on/off (the Settings toggle). When off, update() stays silent and
// drops anything queued.
void setEnabled(bool on);
bool enabled();

// Queue an effect. Cheap and safe to call from anywhere in the loop; BOOM and
// WIN pre-empt whatever is playing so they are always heard.
void play(Event e);

// Pump the sequencer. MUST be called often (every loop iteration) from every
// screen that can make sound.
void update();

}  // namespace Sfx
