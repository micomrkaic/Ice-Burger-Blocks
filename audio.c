/*
 * IceBurger Tetris — synthesized sound effects
 * Mico Mrkaic, 2025
 */
#include "audio.h"
#include <SDL.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define MAX_VOICES 16
#define LEAD_S 0.045f       // how far ahead of playback we keep the queue filled

typedef struct { float *data; int len; } Buffer;
typedef struct { const Buffer *buf; int pos; } Voice;

static SDL_AudioDeviceID dev = 0;
static int rate = 44100;
static Buffer sfx[SFX_COUNT];
static Voice voices[MAX_VOICES];
static bool muted = false;
static float *mixbuf = NULL; static int mixcap = 0;

// ---------- Synthesis ----------
typedef enum { W_SQUARE, W_TRI, W_SINE, W_SAW, W_NOISE } Wave;

static uint32_t noise_state = 0x12345678u;
static float noise(void){
  noise_state ^= noise_state << 13; noise_state ^= noise_state >> 17; noise_state ^= noise_state << 5;
  return (float)(noise_state & 0xFFFF) / 32767.5f - 1.0f;
}

static Buffer buf_new(float seconds){
  Buffer b; b.len = (int)(seconds * (float)rate) + 1;
  b.data = (float *)calloc((size_t)b.len, sizeof(float));
  if(!b.data) b.len = 0;
  return b;
}

// Add a note: exponential sweep f0->f1, short attack, decaying envelope.
// trem_hz > 0 adds amplitude wobble (used for "brrr" and the burp).
static void tone(Buffer *b, float t0, float dur, float f0, float f1, Wave w, float vol, float trem_hz){
  int s0 = (int)(t0 * (float)rate), n = (int)(dur * (float)rate);
  float phase = 0.0f, lp = 0.0f;
  for(int i=0;i<n && s0+i<b->len;i++){
    float t = (float)i / (float)rate, u = (float)i / (float)n;
    float f = f0 * powf(f1/f0, u);
    phase += f / (float)rate; phase -= floorf(phase);
    float s;
    switch(w){
      case W_SQUARE: s = phase < 0.5f ? 1.0f : -1.0f; break;
      case W_TRI:    s = 4.0f * fabsf(phase - 0.5f) - 1.0f; break;
      case W_SINE:   s = sinf(2.0f * (float)M_PI * phase); break;
      case W_SAW:    s = 2.0f * phase - 1.0f; break;
      default:       lp += 0.15f * (noise() - lp); s = lp * 2.5f; break; // low-passed noise
    }
    float env = fminf(1.0f, t / 0.004f) * powf(1.0f - u, 1.5f);
    if(trem_hz > 0.0f) env *= 0.55f + 0.45f * sinf(2.0f * (float)M_PI * trem_hz * t);
    b->data[s0+i] += s * env * vol;
  }
}

static void notes(Buffer *b, float t0, const float *hz, int count, float each, Wave w, float vol){
  for(int i=0;i<count;i++) tone(b, t0 + each*(float)i, each*1.6f, hz[i], hz[i], w, vol, 0.0f);
}

static void build_sfx(void){
  static const float C5=523.25f, E5=659.25f, G5=783.99f, C6=1046.5f, G4=392.0f, E4=329.63f, C4=261.63f;
  Buffer *b;

  b = &sfx[SFX_MOVE];      *b = buf_new(0.04f); tone(b, 0, 0.03f, 220, 220, W_SQUARE, 0.06f, 0);
  b = &sfx[SFX_ROTATE];    *b = buf_new(0.06f); tone(b, 0, 0.05f, 520, 780, W_SQUARE, 0.07f, 0);
  b = &sfx[SFX_LOCK];      *b = buf_new(0.10f); tone(b, 0, 0.09f, 160, 70, W_SINE, 0.45f, 0);
                                                tone(b, 0, 0.03f, 1, 1, W_NOISE, 0.12f, 0);
  b = &sfx[SFX_HARD_DROP]; *b = buf_new(0.18f); tone(b, 0, 0.16f, 220, 45, W_SINE, 0.6f, 0);
                                                tone(b, 0, 0.08f, 1, 1, W_NOISE, 0.30f, 0);
  b = &sfx[SFX_HOLD];      *b = buf_new(0.08f); tone(b, 0, 0.07f, 600, 420, W_TRI, 0.22f, 0);

  { const float n[] = {C5, E5};          b = &sfx[SFX_CLEAR1]; *b = buf_new(0.25f); notes(b, 0, n, 2, 0.07f, W_SQUARE, 0.10f); }
  { const float n[] = {C5, E5, G5};      b = &sfx[SFX_CLEAR2]; *b = buf_new(0.32f); notes(b, 0, n, 3, 0.07f, W_SQUARE, 0.10f); }
  { const float n[] = {C5, E5, G5, C6};  b = &sfx[SFX_CLEAR3]; *b = buf_new(0.40f); notes(b, 0, n, 4, 0.07f, W_SQUARE, 0.10f); }

  // Happy Meal: quick fanfare, then a pitch-bent, rattling burp
  { const float n[] = {C5, E5, G5, C6};
    b = &sfx[SFX_BURP]; *b = buf_new(1.05f);
    notes(b, 0, n, 4, 0.05f, W_SQUARE, 0.09f);
    tone(b, 0.24f, 0.75f, 110, 62, W_SAW,   0.30f, 27.0f);
    tone(b, 0.24f, 0.75f, 1, 1,    W_NOISE, 0.22f, 31.0f); }

  // Combo Meal: cash-register "ka-ching"
  b = &sfx[SFX_COMBO]; *b = buf_new(0.45f);
  tone(b, 0, 0.015f, 1, 1, W_NOISE, 0.4f, 0);
  tone(b, 0.0f, 0.30f, 1318.5f, 1318.5f, W_SINE, 0.22f, 0);
  tone(b, 0.08f, 0.35f, 1760.0f, 1760.0f, W_SINE, 0.22f, 0);

  // Brain Freeze: falling, shivering whistle
  b = &sfx[SFX_FREEZE]; *b = buf_new(0.6f); tone(b, 0, 0.55f, 1900, 500, W_TRI, 0.22f, 18.0f);

  { const float n[] = {G4, C5, E5, G5, C6}; b = &sfx[SFX_LEVEL_UP]; *b = buf_new(0.55f); notes(b, 0, n, 5, 0.07f, W_TRI, 0.20f); }

  { const float n[] = {G4, E4, C4};
    b = &sfx[SFX_GAME_OVER]; *b = buf_new(1.35f);
    notes(b, 0, n, 3, 0.18f, W_SQUARE, 0.10f);
    tone(b, 0.55f, 0.75f, 260, 90, W_SAW, 0.16f, 6.0f); }
}

// ---------- Device and mixer ----------
bool audio_init(void){
  if(SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) return false;
  SDL_AudioSpec want, have;
  SDL_zero(want);
  want.freq = 44100; want.format = AUDIO_F32SYS; want.channels = 1; want.samples = 512;
  dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
  if(!dev){ SDL_QuitSubSystem(SDL_INIT_AUDIO); return false; }
  rate = have.freq;
  build_sfx();
  mixcap = rate / 4;
  mixbuf = (float *)malloc(sizeof(float) * (size_t)mixcap);
  if(!mixbuf){ audio_quit(); return false; }
  SDL_PauseAudioDevice(dev, 0);
  return true;
}

void audio_quit(void){
  if(dev){ SDL_CloseAudioDevice(dev); dev = 0; }
  for(int i=0;i<SFX_COUNT;i++){ free(sfx[i].data); sfx[i].data = NULL; sfx[i].len = 0; }
  free(mixbuf); mixbuf = NULL;
  memset(voices, 0, sizeof voices);
  SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

void audio_play(Sfx s){
  if(!dev || muted || s < 0 || s >= SFX_COUNT || !sfx[s].len) return;
  int slot = 0, best = -1;
  for(int i=0;i<MAX_VOICES;i++){
    if(!voices[i].buf){ slot = i; best = -2; break; }
    if(voices[i].pos > best){ best = voices[i].pos; slot = i; } // steal the voice furthest along
  }
  voices[slot].buf = &sfx[s]; voices[slot].pos = 0;
}

void audio_pump(void){
  if(!dev) return;
  int queued = (int)(SDL_GetQueuedAudioSize(dev) / sizeof(float));
  int need = (int)(LEAD_S * (float)rate) - queued;
  if(need <= 0) return;
  if(need > mixcap) need = mixcap;
  memset(mixbuf, 0, sizeof(float) * (size_t)need);
  for(int v=0;v<MAX_VOICES;v++){
    Voice *vo = &voices[v];
    if(!vo->buf) continue;
    int n = vo->buf->len - vo->pos; if(n > need) n = need;
    for(int i=0;i<n;i++) mixbuf[i] += vo->buf->data[vo->pos + i];
    vo->pos += n;
    if(vo->pos >= vo->buf->len) vo->buf = NULL;
  }
  for(int i=0;i<need;i++){ float x = mixbuf[i] * 0.8f; mixbuf[i] = x > 1.0f ? 1.0f : x < -1.0f ? -1.0f : x; }
  SDL_QueueAudio(dev, mixbuf, (Uint32)(sizeof(float) * (size_t)need));
}

void audio_set_muted(bool m){ muted = m; if(m) memset(voices, 0, sizeof voices); }
bool audio_muted(void){ return muted; }
