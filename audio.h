/*
 * IceBurger — synthesized sound effects (no asset files).
 * Effects are rendered into buffers at start-up, mixed in software and fed to
 * SDL_QueueAudio a little ahead of playback by audio_pump().
 * Mico Mrkaic, 2025
 */
#ifndef AUDIO_H
#define AUDIO_H

#include <stdbool.h>

typedef enum {
  SFX_MOVE, SFX_ROTATE, SFX_LOCK, SFX_HARD_DROP, SFX_HOLD,
  SFX_CLEAR1, SFX_CLEAR2, SFX_CLEAR3,
  SFX_BURP,        // four-line clear (Food Fight)
  SFX_COMBO,       // all-burger row
  SFX_FREEZE,      // all-ice-cream row
  SFX_LEVEL_UP, SFX_GAME_OVER,
  SFX_COUNT
} Sfx;

bool audio_init(void);          // false if no audio device; the game still runs
void audio_quit(void);
void audio_play(Sfx s);
void audio_pump(void);          // call once per frame
void audio_set_muted(bool m);
bool audio_muted(void);

#endif
