// sfx.cpp
// Passive-buzzer sound effects. See sfx.h. Adapted from M5PORKCHOP's sfx engine:
// a note-sequence state machine pumped by update(), except the tone is produced
// with LEDC (ledcWriteTone) rather than M5's speaker, so each note has to be
// switched off explicitly — LEDC holds a frequency until told otherwise.
#include "sfx.h"
#include "configs.h"
#include <Arduino.h>

namespace Sfx {

// One step of an effect. freq 0 = silent step; duration 0 = END of sequence.
struct Note { uint16_t freq, duration, pause; };

// REVEAL: soft short tick.
static const Note SND_REVEAL[]    = { {1200, 8, 0}, {0, 0, 0} };
// FLAG: lower, cushioned click.
static const Note SND_FLAG[]      = { {700, 10, 0}, {0, 0, 0} };
// CHORD: quick double tick.
static const Note SND_CHORD[]     = { {900, 8, 6}, {1200, 8, 0}, {0, 0, 0} };
// BOOM: falling explosion, ends on a low thud.
static const Note SND_BOOM[]      = { {500, 25, 0}, {300, 45, 0}, {180, 70, 0}, {90, 170, 0}, {0, 0, 0} };
// WIN: ascending fanfare with a held resolve.
static const Note SND_WIN[]       = { {600, 90, 25}, {800, 90, 25}, {1000, 90, 25}, {1350, 200, 0}, {0, 0, 0} };
// BURST: rising pop.
static const Note SND_BURST[]     = { {500, 18, 0}, {800, 18, 0}, {1200, 22, 0}, {1600, 40, 0}, {0, 0, 0} };
// LIGHTNING: bright zap crackle.
static const Note SND_LIGHTNING[] = { {1900, 14, 0}, {1200, 14, 0}, {1700, 12, 0}, {800, 34, 0}, {0, 0, 0} };
// LIFE_ARM: ready two-step, rising.
static const Note SND_LIFE_ARM[]  = { {900, 40, 12}, {1200, 55, 0}, {0, 0, 0} };
// LIFE_SAVE: relief, resolves back down.
static const Note SND_LIFE_SAVE[] = { {1000, 45, 8}, {1300, 55, 8}, {1050, 95, 0}, {0, 0, 0} };
// SONAR: single blip.
static const Note SND_SONAR[]     = { {1150, 22, 0}, {0, 0, 0} };
// CLICK: minimal UI tap.
static const Note SND_CLICK[]     = { {1050, 6, 0}, {0, 0, 0} };

static const Note *seqFor(Event e) {
  switch (e) {
    case REVEAL:    return SND_REVEAL;
    case FLAG:      return SND_FLAG;
    case CHORD:     return SND_CHORD;
    case BOOM:      return SND_BOOM;
    case WIN:       return SND_WIN;
    case BURST:     return SND_BURST;
    case LIGHTNING: return SND_LIGHTNING;
    case LIFE_ARM:  return SND_LIFE_ARM;
    case LIFE_SAVE: return SND_LIFE_SAVE;
    case SONAR:     return SND_SONAR;
    case CLICK:     return SND_CLICK;
    default:        return nullptr;
  }
}

// BOOM and WIN pre-empt whatever is playing, so they are never masked by a
// still-ringing tick.
static bool isPriority(Event e) { return e == BOOM || e == WIN; }

// Buzzer driver. LEDC's tone API changed at core 3.0: it takes the pin directly
// there, a channel before. On a buzzer-less board these compile to nothing.
static void buzzTone(uint16_t freq) {
#ifdef HAS_BUZZER
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWriteTone(BUZZER_PIN, freq);
#else
  ledcWriteTone(1, freq);            // channel 1 (backlight owns 0)
#endif
#else
  (void)freq;
#endif
}
static void buzzOff() {
#ifdef HAS_BUZZER
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWrite(BUZZER_PIN, 0);
#else
  ledcWrite(1, 0);
#endif
#endif
}

// State machine.
static bool          s_enabled = true;
static const Note   *s_seq = nullptr;
static uint8_t       s_step = 0;
static bool          s_inNote = false;      // true = tone phase, false = pause phase
static uint32_t      s_stepStart = 0;

// Small event ring buffer, so a burst of events (reveal that also wins, say) is
// not lost. Single-threaded — everything runs from the loop — so no locking.
static const uint8_t Q = 6;
static Event   s_queue[Q];
static uint8_t s_head = 0, s_tail = 0;

static void beginStep() {
  const Note &n = s_seq[s_step];
  if (n.freq > 0 && n.duration > 0) buzzTone(n.freq);
  else                              buzzOff();
}

static void startSequence(const Note *seq) {
  s_seq = seq;
  s_step = 0;
  s_inNote = true;
  s_stepStart = millis();
  beginStep();
}

static void clearQueue() { s_head = s_tail = 0; }

void init() {
#ifdef HAS_BUZZER
  pinMode(BUZZER_PIN, OUTPUT);
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcAttach(BUZZER_PIN, 2000, 10);    // seed freq/res; ledcWriteTone retunes it
#else
  ledcSetup(1, 2000, 10);
  ledcAttachPin(BUZZER_PIN, 1);
#endif
#endif
  buzzOff();
  s_seq = nullptr;
  clearQueue();
}

void setEnabled(bool on) {
  s_enabled = on;
  if (!on) { buzzOff(); s_seq = nullptr; clearQueue(); }
}

bool enabled() { return s_enabled; }

void play(Event e) {
  if (!s_enabled || e == NONE || !seqFor(e)) return;

  if (isPriority(e)) {              // interrupt so the important sound is heard now
    buzzOff();
    s_seq = nullptr;
    clearQueue();
  }

  uint8_t next = (uint8_t)((s_head + 1) % Q);
  if (next == s_tail) s_tail = (uint8_t)((s_tail + 1) % Q);   // full: drop oldest
  s_queue[s_head] = e;
  s_head = next;
}

void update() {
  if (!s_enabled) { if (s_seq) { buzzOff(); s_seq = nullptr; } clearQueue(); return; }

  // Nothing playing? start the next queued effect.
  if (s_seq == nullptr && s_tail != s_head) {
    Event e = s_queue[s_tail];
    s_tail = (uint8_t)((s_tail + 1) % Q);
    const Note *seq = seqFor(e);
    if (seq) startSequence(seq);
    if (s_seq == nullptr) return;
  }
  if (s_seq == nullptr) return;

  const uint32_t now = millis();
  const Note &n = s_seq[s_step];

  if (n.duration == 0) {           // end marker
    buzzOff();
    s_seq = nullptr;
    return;
  }

  if (s_inNote) {
    if (now - s_stepStart >= n.duration) {
      buzzOff();                   // LEDC holds the tone, so stop it ourselves
      s_inNote = false;
      s_stepStart = now;
      if (n.pause == 0) {          // no gap: straight into the next note
        s_step++;
        s_inNote = true;
        beginStep();
      }
    }
  } else {
    if (now - s_stepStart >= n.pause) {
      s_step++;
      s_inNote = true;
      s_stepStart = now;
      beginStep();
    }
  }
}

}  // namespace Sfx
