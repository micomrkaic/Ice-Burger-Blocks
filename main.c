/*
  Mico Mrkaic, September 2025

 * IceBurger — C + SDL2
 * Blocks are made of 🍦 and 🍔 tiles drawn procedurally; clearing lines throws food everywhere.
 * Score is counted in calories.
 *
 * Build:   make            (needs SDL2 only; pkg-config is used when available)
 * Tests:   make test       (rules only, no SDL)
 * Web:     make wasm       (needs Emscripten; output in web/)
 *
 * Files:
 *   game.c/h    rules: SRS rotation, 7-bag, lock delay, hold, scoring, bonuses (no SDL)
 *   render.c/h  batched SDL_RenderGeometry drawing, food tiles, 8x8 text, particles
 *   audio.c/h   synthesized sound effects via SDL_QueueAudio
 *   font8x8.h   public-domain 8x8 bitmap font
 *   main.c      window, input (DAS/ARR, gamepad), effects, high scores, main loop
 *
 * Controls:
 *   Left/Right move, Down soft drop, Space hard drop, Up/X rotate CW, Z rotate CCW,
 *   C/Shift hold, P pause, R restart, M mute, Esc quit
 *   Gamepad: D-pad/left stick move, D-pad up hard drop, A rotate CW, B rotate CCW,
 *   X/Y/shoulders hold, Start pause, Back restart
 *
 * Bonuses:
 *   All-burger row   "Combo Meal"    row worth double
 *   All-ice row      "Brain Freeze"  gravity halved for 5 s
 *   Four lines       "Food Fight"    x2 calories, a burp and a screen shake
 */

#include <SDL.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>

#include "game.h"
#include "render.h"
#include "audio.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

// ---------- Layout (logical pixels; scaled to the window by SDL) ----------
#define WIN_W 720
#define WIN_H 760
#define TILE 32
#define BOARD_X 40
#define BOARD_Y 40
#define PANEL_X 400
#define NEXT_W 150
#define HOLD_X 570
#define HOLD_W 110

// ---------- Timing ----------
#define MAX_DT 0.1f           // clamp frame time so a stalled frame can't cause a burst of drops
#define FRAME_CAP_MS 7        // ~144 fps cap when vsync is unavailable or the window is hidden
#define DAS_S 0.170           // delayed auto-shift
#define ARR_S 0.050           // auto-repeat rate
#define SOFT_ARR_S 0.035      // soft-drop repeat while Down is held

#define HS_COUNT 5

static const SDL_Color col_bg     = {20, 24, 28, 255};
static const SDL_Color col_frame  = {36, 42, 48, 255};
static const SDL_Color col_cell   = {30, 35, 40, 255};
static const SDL_Color col_text   = {235, 235, 235, 255};
static const SDL_Color col_dim    = {140, 150, 160, 255};
static const SDL_Color col_accent = {255, 196, 70, 255};
static const SDL_Color col_ice    = {140, 205, 255, 255};
static const SDL_Color col_red    = {255, 120, 120, 255};

static const SDL_Color col_piece[7] = {
  {45, 212, 191, 255},  // I
  {250, 204, 21, 255},  // O
  {192, 132, 252, 255}, // T
  {74, 222, 128, 255},  // S
  {251, 113, 133, 255}, // Z
  {96, 165, 250, 255},  // J
  {245, 158, 11, 255},  // L
};

// ---------- Input ----------
enum { ACT_LEFT, ACT_RIGHT, ACT_DOWN, ACT_HARD, ACT_CW, ACT_CCW, ACT_HOLD,
       ACT_PAUSE, ACT_RESTART, ACT_MUTE, ACT_QUIT, ACT_COUNT };
enum { SRC_KEY, SRC_PAD, SRC_STICK, SRC_COUNT };

typedef struct {
  char text[24], sub[40];
  float t, dur; int prio;
  SDL_Color color;
} Banner;

typedef struct {
  SDL_Window *win; SDL_Renderer *ren; Batch batch;
  bool running, paused, vsync;
  Game g;
  Uint64 now; double freq;
  // input
  bool held[SRC_COUNT][ACT_COUNT];
  int das_dir; double das_t, arr_t, soft_t;
  SDL_GameController *pad; SDL_JoystickID pad_id;
  // effects
  float shake_t, shake_dur, shake_amp;
  Banner banner;
  // high scores
  long best[HS_COUNT]; int new_rank; bool recorded;
} App;

// ---------- High scores ----------
#ifdef __EMSCRIPTEN__
EM_JS(double, js_hs_get, (int i), {
  try { var v = parseFloat(localStorage.getItem("iceburger.hs" + i)); return isFinite(v) ? v : 0; } catch (e) { return 0; }
});
EM_JS(void, js_hs_set, (int i, double v), {
  try { localStorage.setItem("iceburger.hs" + i, String(v)); } catch (e) {}
});
static void hs_load(App *a){ for(int i=0;i<HS_COUNT;i++) a->best[i] = (long)js_hs_get(i); }
static void hs_save(const App *a){ for(int i=0;i<HS_COUNT;i++) js_hs_set(i, (double)a->best[i]); }
#else
// $XDG_DATA_HOME/iceburger/ (~/.local/share/iceburger/) on Linux,
// ~/Library/Application Support/iceburger/ on macOS
static FILE *hs_open(const char *mode){
  char *dir = SDL_GetPrefPath("", "iceburger");
  if(!dir) return NULL;
  char path[1024];
  snprintf(path, sizeof path, "%shighscores.txt", dir);
  SDL_free(dir);
  return fopen(path, mode);
}
static void hs_load(App *a){
  memset(a->best, 0, sizeof a->best);
  FILE *f = hs_open("r");
  if(!f) return;
  for(int i=0;i<HS_COUNT;i++) if(fscanf(f, "%ld", &a->best[i]) != 1) break;
  fclose(f);
}
static void hs_save(const App *a){
  FILE *f = hs_open("w");
  if(!f) return;
  for(int i=0;i<HS_COUNT;i++) fprintf(f, "%ld\n", a->best[i]);
  fclose(f);
}
#endif

static void hs_record(App *a, long kcal){
  a->new_rank = -1;
  if(kcal <= 0) return;
  for(int i=0;i<HS_COUNT;i++) if(kcal > a->best[i]){
    memmove(&a->best[i+1], &a->best[i], sizeof(long) * (size_t)(HS_COUNT-1-i));
    a->best[i] = kcal; a->new_rank = i;
    hs_save(a);
    return;
  }
}

// ---------- Effects ----------
static void shake(App *a, float amp, float dur){
  if(amp >= a->shake_amp * (a->shake_dur > 0 ? a->shake_t / a->shake_dur : 0)){ a->shake_amp = amp; a->shake_dur = dur; a->shake_t = dur; }
}

static void banner(App *a, const char *text, const char *sub, SDL_Color c, int prio, float dur){
  if(a->banner.t > 0 && prio < a->banner.prio) return;
  snprintf(a->banner.text, sizeof a->banner.text, "%s", text);
  snprintf(a->banner.sub, sizeof a->banner.sub, "%s", sub);
  a->banner.color = c; a->banner.prio = prio; a->banner.dur = a->banner.t = dur;
}

static void new_game(App *a){
  game_reset(&a->g);
  fx_reset();
  a->paused = false; a->recorded = false; a->new_rank = -1;
  a->banner.t = 0; a->shake_t = 0;
}

static void dispatch_events(App *a){
  Game *g = &a->g;
  bool hard = false;
  for(int i=0;i<g->n_events;i++){
    const GameEvent *e = &g->events[i];
    switch(e->type){
      case EV_MOVE:      audio_play(SFX_MOVE); break;
      case EV_ROTATE:    audio_play(SFX_ROTATE); break;
      case EV_HARD_DROP: audio_play(SFX_HARD_DROP); shake(a, 3.0f, 0.12f); hard = true; break;
      case EV_LOCK:      if(!hard) audio_play(SFX_LOCK); break;
      case EV_HOLD:      audio_play(SFX_HOLD); break;
      case EV_CLEAR:
        if(e->a == 4){
          audio_play(SFX_BURP);
          banner(a, "FOOD FIGHT!", "4 LINES  x2 KCAL", col_accent, 3, 1.8f);
          shake(a, 10.0f, 0.55f);
        } else audio_play(e->a == 1 ? SFX_CLEAR1 : e->a == 2 ? SFX_CLEAR2 : SFX_CLEAR3);
        break;
      case EV_COMBO_MEAL:
        audio_play(SFX_COMBO);
        banner(a, "COMBO MEAL", "ALL-BURGER ROW x2", col_red, 2, 1.5f);
        break;
      case EV_BRAIN_FREEZE:
        audio_play(SFX_FREEZE);
        banner(a, "BRAIN FREEZE", "GRAVITY HALVED 5 S", col_ice, 2, 1.5f);
        break;
      case EV_FREEZE_END: break;
      case EV_LEVEL_UP: {
        audio_play(SFX_LEVEL_UP);
        char sub[40]; snprintf(sub, sizeof sub, "LEVEL %d", e->a);
        banner(a, game_level_name(e->a), sub, col_text, 1, 1.4f);
        break;
      }
      case EV_GAME_OVER:
        audio_play(SFX_GAME_OVER);
        if(!a->recorded){ hs_record(a, g->kcal); a->recorded = true; }
        break;
    }
  }
  // Food debris from every cleared cell, in board-relative pixels
  for(int i=0;i<g->n_cleared;i++)
    for(int c=0;c<COLS;c++)
      fx_food_burst((float)(c*TILE + TILE/2), (float)(g->cleared[i].row*TILE + TILE/2), g->cleared[i].cells[c].food);
  game_clear_events(g);
}

// ---------- Input ----------
static bool act_held(const App *a, int act){
  for(int s=0;s<SRC_COUNT;s++) if(a->held[s][act]) return true;
  return false;
}

static bool playing(const App *a){ return !a->paused && !a->g.game_over; }

static void act_press(App *a, int act){
  Game *g = &a->g;
  switch(act){
    case ACT_QUIT:
#ifdef __EMSCRIPTEN__
      if(!g->game_over) a->paused = true;   // a web page can't quit; pause instead
#else
      a->running = false;
#endif
      return;
    case ACT_PAUSE:   if(!g->game_over) a->paused = !a->paused; return;
    case ACT_RESTART: new_game(a); return;
    case ACT_MUTE:    audio_set_muted(!audio_muted()); return;
    default: break;
  }
  if(!playing(a)) return;
  switch(act){
    case ACT_LEFT:  a->das_dir = -1; a->das_t = a->arr_t = 0; game_move(g, -1); break;
    case ACT_RIGHT: a->das_dir = +1; a->das_t = a->arr_t = 0; game_move(g, +1); break;
    case ACT_DOWN:  a->soft_t = 0; game_soft_drop(g); break;
    case ACT_HARD:  game_hard_drop(g); break;
    case ACT_CW:    game_rotate(g, true); break;
    case ACT_CCW:   game_rotate(g, false); break;
    case ACT_HOLD:  game_hold(g); break;
  }
}

static void act_release(App *a, int act){
  // Releasing one direction while the other is still held hands auto-shift back to it
  if(act == ACT_LEFT && a->das_dir == -1){ a->das_dir = act_held(a, ACT_RIGHT) ? +1 : 0; a->das_t = a->arr_t = 0; }
  if(act == ACT_RIGHT && a->das_dir == +1){ a->das_dir = act_held(a, ACT_LEFT) ? -1 : 0; a->das_t = a->arr_t = 0; }
}

static void set_held(App *a, int src, int act, bool down){
  if(act < 0) return;
  bool was = act_held(a, act);
  a->held[src][act] = down;
  bool now = act_held(a, act);
  if(!was && now) act_press(a, act);
  else if(was && !now) act_release(a, act);
}

static void release_all(App *a){
  for(int s=0;s<SRC_COUNT;s++) for(int k=0;k<ACT_COUNT;k++) set_held(a, s, k, false);
}

// Auto-shift and soft-drop repeat, driven by held state rather than OS key repeat
static void update_input(App *a, double dt){
  if(!playing(a)) return;
  if(a->das_dir){
    a->das_t += dt;
    if(a->das_t >= DAS_S){
      a->arr_t += dt;
      while(a->arr_t >= ARR_S){
        a->arr_t -= ARR_S;
        if(!game_move(&a->g, a->das_dir)){ a->arr_t = 0; break; }
      }
    }
  }
  if(act_held(a, ACT_DOWN)){
    a->soft_t += dt;
    while(a->soft_t >= SOFT_ARR_S){ a->soft_t -= SOFT_ARR_S; if(!game_soft_drop(&a->g)){ a->soft_t = 0; break; } }
  }
}

static int key_action(SDL_Keycode k){
  switch(k){
    case SDLK_LEFT:  return ACT_LEFT;
    case SDLK_RIGHT: return ACT_RIGHT;
    case SDLK_DOWN:  return ACT_DOWN;
    case SDLK_SPACE: return ACT_HARD;
    case SDLK_UP: case SDLK_x: return ACT_CW;
    case SDLK_z:     return ACT_CCW;
    case SDLK_c: case SDLK_LSHIFT: case SDLK_RSHIFT: return ACT_HOLD;
    case SDLK_p: case SDLK_F1: return ACT_PAUSE;
    case SDLK_r:     return ACT_RESTART;
    case SDLK_m:     return ACT_MUTE;
    case SDLK_ESCAPE: return ACT_QUIT;
    default: return -1;
  }
}

static int pad_action(Uint8 b){
  switch(b){
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT:  return ACT_LEFT;
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return ACT_RIGHT;
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN:  return ACT_DOWN;
    case SDL_CONTROLLER_BUTTON_DPAD_UP:    return ACT_HARD;
    case SDL_CONTROLLER_BUTTON_A:          return ACT_CW;
    case SDL_CONTROLLER_BUTTON_B:          return ACT_CCW;
    case SDL_CONTROLLER_BUTTON_X: case SDL_CONTROLLER_BUTTON_Y:
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return ACT_HOLD;
    case SDL_CONTROLLER_BUTTON_START:      return ACT_PAUSE;
    case SDL_CONTROLLER_BUTTON_BACK:       return ACT_RESTART;
    default: return -1;
  }
}

static void pad_close(App *a){
  if(!a->pad) return;
  SDL_GameControllerClose(a->pad); a->pad = NULL;
  for(int k=0;k<ACT_COUNT;k++){ set_held(a, SRC_PAD, k, false); set_held(a, SRC_STICK, k, false); }
}

static void handle_event(App *a, const SDL_Event *e){
  switch(e->type){
    case SDL_QUIT: a->running = false; break;
    case SDL_KEYDOWN:
      if(!e->key.repeat) set_held(a, SRC_KEY, key_action(e->key.keysym.sym), true);
      break;
    case SDL_KEYUP:
      set_held(a, SRC_KEY, key_action(e->key.keysym.sym), false);
      break;
    case SDL_WINDOWEVENT:
      if(e->window.event == SDL_WINDOWEVENT_FOCUS_LOST){
        if(!a->g.game_over) a->paused = true;
        release_all(a);
      }
      break;
    case SDL_CONTROLLERDEVICEADDED:
      if(!a->pad){
        a->pad = SDL_GameControllerOpen(e->cdevice.which);
        if(a->pad) a->pad_id = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(a->pad));
      }
      break;
    case SDL_CONTROLLERDEVICEREMOVED:
      if(a->pad && e->cdevice.which == a->pad_id) pad_close(a);
      break;
    case SDL_CONTROLLERBUTTONDOWN:
    case SDL_CONTROLLERBUTTONUP:
      if(a->pad && e->cbutton.which == a->pad_id)
        set_held(a, SRC_PAD, pad_action(e->cbutton.button), e->type == SDL_CONTROLLERBUTTONDOWN);
      break;
    case SDL_CONTROLLERAXISMOTION:
      if(a->pad && e->caxis.which == a->pad_id){
        int v = e->caxis.value;  // press beyond 50%, release inside 25% (hysteresis)
        if(e->caxis.axis == SDL_CONTROLLER_AXIS_LEFTX){
          if(v < -16000) set_held(a, SRC_STICK, ACT_LEFT, true);  else if(v > -8000) set_held(a, SRC_STICK, ACT_LEFT, false);
          if(v >  16000) set_held(a, SRC_STICK, ACT_RIGHT, true); else if(v <  8000) set_held(a, SRC_STICK, ACT_RIGHT, false);
        } else if(e->caxis.axis == SDL_CONTROLLER_AXIS_LEFTY){
          if(v >  16000) set_held(a, SRC_STICK, ACT_DOWN, true);  else if(v <  8000) set_held(a, SRC_STICK, ACT_DOWN, false);
        }
      }
      break;
  }
}

// ---------- Drawing ----------
static void fmt_thousands(long v, char *out, size_t cap){
  char tmp[32]; snprintf(tmp, sizeof tmp, "%ld", v < 0 ? -v : v);
  int len = (int)strlen(tmp), o = 0;
  if(v < 0 && o < (int)cap-1) out[o++] = '-';
  for(int i=0;i<len && o < (int)cap-1;i++){
    out[o++] = tmp[i];
    int left = len - 1 - i;
    if(left > 0 && left % 3 == 0 && o < (int)cap-1) out[o++] = ',';
  }
  out[o] = 0;
}

static SDL_Color with_alpha(SDL_Color c, Uint8 alpha){ c.a = alpha; return c; }

static void draw_piece_in(Batch *b, const Piece *p, float x, float y, float w, float h, float tile){
  int r0=4, r1=-1, c0=4, c1=-1;
  for(int r=0;r<4;r++) for(int c=0;c<4;c++) if(p->m[r][c]){
    if(r < r0) r0 = r;
    if(r > r1) r1 = r;
    if(c < c0) c0 = c;
    if(c > c1) c1 = c;
  }
  if(r1 < 0) return;
  float pw = (float)(c1-c0+1)*tile, ph = (float)(r1-r0+1)*tile;
  float px = floorf(x + (w-pw)*0.5f) - (float)c0*tile, py = floorf(y + (h-ph)*0.5f) - (float)r0*tile;
  for(int r=0;r<4;r++) for(int c=0;c<4;c++) if(p->m[r][c])
    draw_food_tile(b, px + (float)c*tile, py + (float)r*tile, tile, p->food, col_piece[p->k]);
}

static void draw_board(App *a){
  Batch *b = &a->batch; const Game *g = &a->g;
  bool frozen = g->freeze_timer > 0;
  SDL_Color frame = frozen ? (SDL_Color){70, 110, 150, 255} : col_frame;
  SDL_Color cell  = frozen ? (SDL_Color){30, 42, 56, 255}  : col_cell;
  draw_rect(b, BOARD_X-8, BOARD_Y-8, COLS*TILE+16, ROWS*TILE+16, frame);
  for(int r=0;r<ROWS;r++) for(int c=0;c<COLS;c++){
    float px = (float)(BOARD_X + c*TILE), py = (float)(BOARD_Y + r*TILE);
    draw_rect(b, px, py, TILE-1, TILE-1, cell);
    const Cell *ce = &g->board[r][c];
    if(ce->filled) draw_food_tile(b, px, py, TILE, ce->food, col_piece[ce->kind]);
  }
  if(g->game_over) return;
  // Ghost: translucent outline where the piece will land
  int gy = game_ghost_y(g);
  if(gy != g->cur.y){
    SDL_Color t = col_piece[g->cur.k];
    for(int r=0;r<4;r++) for(int c=0;c<4;c++) if(g->cur.m[r][c]){
      float px = (float)(BOARD_X + (g->cur.x+c)*TILE), py = (float)(BOARD_Y + (gy+r)*TILE);
      draw_rect(b, px, py, TILE-4, TILE-4, with_alpha(t, 40));
      draw_rect_outline(b, px, py, TILE-4, TILE-4, 2, with_alpha(t, 170));
    }
  }
  for(int r=0;r<4;r++) for(int c=0;c<4;c++) if(g->cur.m[r][c]){
    int x = g->cur.x+c, y = g->cur.y+r;
    if(x<0 || x>=COLS || y<0 || y>=ROWS) continue;
    draw_food_tile(b, (float)(BOARD_X + x*TILE), (float)(BOARD_Y + y*TILE), TILE, g->cur.food, col_piece[g->cur.k]);
  }
}

static void draw_panel(App *a){
  Batch *b = &a->batch; const Game *g = &a->g;
  char buf[64], num[32];

  // NEXT: one large slot and the rest of the queue below it
  draw_text(b, "NEXT", PANEL_X, 40, 2, col_dim);
  draw_rect(b, PANEL_X-8, 64-8, NEXT_W+16, 80+4*42+16, col_frame);
  draw_piece_in(b, &g->queue[0], PANEL_X, 64, NEXT_W, 80, 28);
  for(int i=1;i<QUEUE_LEN;i++) draw_piece_in(b, &g->queue[i], PANEL_X, 144 + (float)(i-1)*42, NEXT_W, 42, 16);

  // HOLD
  draw_text(b, "HOLD", HOLD_X, 40, 2, col_dim);
  draw_rect(b, HOLD_X-8, 64-8, HOLD_W+16, 80+16, col_frame);
  if(g->has_hold){
    draw_piece_in(b, &g->hold, HOLD_X, 64, HOLD_W, 80, 22);
    if(!g->can_hold) draw_rect(b, HOLD_X-8, 64-8, HOLD_W+16, 80+16, (SDL_Color){20, 24, 28, 150});
  }

  float y = 348;
  draw_text(b, "KCAL EATEN", PANEL_X, y, 2, col_dim);
  fmt_thousands(g->kcal, num, sizeof num);
  draw_text(b, num, PANEL_X, y + 22, 3, col_accent);
  snprintf(buf, sizeof buf, "LINES %d", g->lines);
  draw_text(b, buf, PANEL_X, y + 62, 2, col_text);
  snprintf(buf, sizeof buf, "LEVEL %d", g->level);
  draw_text(b, buf, PANEL_X, y + 86, 2, col_text);
  draw_text(b, game_level_name(g->level), PANEL_X, y + 110, 2, col_accent);
  fmt_thousands(a->best[0], num, sizeof num);
  snprintf(buf, sizeof buf, "BEST %s", num);
  draw_text(b, buf, PANEL_X, y + 134, 2, col_dim);
  if(g->freeze_timer > 0){
    snprintf(buf, sizeof buf, "BRAIN FREEZE %.1f", g->freeze_timer);
    draw_text(b, buf, PANEL_X, y + 166, 2, col_ice);
  }

  static const char *help[] = {
    "LEFT/RIGHT  MOVE",  "DOWN        SOFT DROP", "SPACE       HARD DROP",
    "UP / X      ROTATE", "Z           ROTATE CCW", "C / SHIFT   HOLD",
    "P PAUSE   R RESTART",
  };
  for(int i=0;i<7;i++) draw_text(b, help[i], PANEL_X, 588 + (float)i*13, 1, col_dim);
  draw_text(b, audio_muted() ? "M SOUND: OFF" : "M SOUND: ON", PANEL_X, 588 + 7*13, 1, col_dim);
}

static void draw_overlays(App *a){
  Batch *b = &a->batch; const Game *g = &a->g;
  const float cx = BOARD_X + COLS*TILE/2.0f;

  if(a->banner.t > 0){
    float k = a->banner.t / a->banner.dur;                  // 1 -> 0
    float fade = fminf(1.0f, a->banner.t / 0.35f);
    float rise = (1.0f - k) * 24.0f;
    Uint8 al = (Uint8)(255 * fade);
    float scale = text_width(a->banner.text, 4) <= COLS*TILE - 16 ? 4 : 3;
    float y = BOARD_Y + 230 - rise;
    draw_rect(b, BOARD_X, y - 14, COLS*TILE, scale*8 + 46, (SDL_Color){10, 12, 14, (Uint8)(170 * fade)});
    draw_text_centered(b, a->banner.text, cx + 2, y + 2, scale, (SDL_Color){0, 0, 0, al});
    draw_text_centered(b, a->banner.text, cx, y, scale, with_alpha(a->banner.color, al));
    draw_text_centered(b, a->banner.sub, cx, y + scale*8 + 10, 2, with_alpha(col_text, al));
  }

  if(a->paused && !g->game_over){
    draw_rect(b, BOARD_X-8, BOARD_Y-8, COLS*TILE+16, ROWS*TILE+16, (SDL_Color){10, 12, 14, 235});
    draw_text_centered(b, "PAUSED", cx, BOARD_Y + 260, 4, col_accent);
    draw_text_centered(b, "P TO RESUME", cx, BOARD_Y + 310, 2, col_text);
  }

  if(g->game_over){
    char buf[64], num[32];
    draw_rect(b, BOARD_X-8, BOARD_Y-8, COLS*TILE+16, ROWS*TILE+16, (SDL_Color){10, 12, 14, 228});
    draw_text_centered(b, "FOOD COMA", cx, BOARD_Y + 120, 4, col_red);
    fmt_thousands(g->kcal, num, sizeof num);
    snprintf(buf, sizeof buf, "YOU ATE %s KCAL", num);
    draw_text_centered(b, buf, cx, BOARD_Y + 170, 2, col_text);
    draw_text_centered(b, "TOP MEALS", cx, BOARD_Y + 230, 2, col_dim);
    for(int i=0;i<HS_COUNT;i++){
      fmt_thousands(a->best[i], num, sizeof num);
      snprintf(buf, sizeof buf, "%d. %12s", i+1, num);
      draw_text_centered(b, buf, cx, BOARD_Y + 260 + (float)i*24, 2, i == a->new_rank ? col_accent : col_text);
    }
    if(a->new_rank == 0) draw_text_centered(b, "NEW RECORD!", cx, BOARD_Y + 400, 2, col_accent);
    draw_text_centered(b, "R TO RESTART", cx, BOARD_Y + 450, 2, col_text);
  }
}

static void render(App *a){
  SDL_SetRenderDrawColor(a->ren, col_bg.r, col_bg.g, col_bg.b, 255);
  SDL_RenderClear(a->ren);
  Batch *b = &a->batch;

  if(a->shake_t > 0){
    float amp = a->shake_amp * (a->shake_t / a->shake_dur);
    b->dx = ((float)rand() / (float)RAND_MAX * 2.0f - 1.0f) * amp;
    b->dy = ((float)rand() / (float)RAND_MAX * 2.0f - 1.0f) * amp;
  } else b->dx = b->dy = 0;

  draw_board(a);
  fx_draw(b, BOARD_X, BOARD_Y);
  b->dx = b->dy = 0;           // the side panel stays still
  draw_panel(a);
  draw_overlays(a);
  batch_flush(b);
  SDL_RenderPresent(a->ren);
}

// ---------- Main loop ----------
static void frame(void *arg){
  App *a = (App *)arg;
  Uint64 last = a->now; a->now = SDL_GetPerformanceCounter();
  float dt = (float)((double)(a->now - last) / a->freq);
  if(dt > MAX_DT) dt = MAX_DT;

  SDL_Event e;
  while(SDL_PollEvent(&e)) handle_event(a, &e);

  update_input(a, dt);
  if(playing(a)) game_update(&a->g, dt);
  dispatch_events(a);

  fx_update(dt);
  if(a->shake_t > 0) a->shake_t -= dt;
  if(a->banner.t > 0) a->banner.t -= dt;
  audio_pump();

  render(a);

#ifndef __EMSCRIPTEN__
  // Without vsync (or while minimized/hidden, where some drivers return immediately) cap the frame rate
  if(!a->vsync || (SDL_GetWindowFlags(a->win) & (SDL_WINDOW_MINIMIZED|SDL_WINDOW_HIDDEN))){
    double spent_ms = (double)(SDL_GetPerformanceCounter() - a->now) * 1000.0 / a->freq;
    double wait_ms = FRAME_CAP_MS - spent_ms;
    SDL_Delay(wait_ms > 1.0 ? (Uint32)wait_ms : 1);
  }
#endif
}

#ifdef __EMSCRIPTEN__
// Some packaged emcc/LLVM pairings (e.g. Ubuntu's) only export a plain main(), not main(argc, argv)
int main(void){
#else
int main(int argc, char **argv){
  (void)argc; (void)argv;
#endif
  static App app;   // static: large, and must outlive main() under Emscripten
  App *a = &app;
  srand((unsigned)time(NULL));

  if(SDL_Init(SDL_INIT_VIDEO|SDL_INIT_TIMER|SDL_INIT_GAMECONTROLLER) != 0){
    // Retry without controllers (some platforms lack the subsystem)
    if(SDL_Init(SDL_INIT_VIDEO|SDL_INIT_TIMER) != 0){ fprintf(stderr, "SDL_Init error: %s\n", SDL_GetError()); return 1; }
  }

#ifdef __EMSCRIPTEN__
  Uint32 wflags = SDL_WINDOW_SHOWN | SDL_WINDOW_ALLOW_HIGHDPI;  // the page owns the canvas size
#else
  Uint32 wflags = SDL_WINDOW_SHOWN | SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_RESIZABLE;
#endif
  a->win = SDL_CreateWindow("IceBurger", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, WIN_W, WIN_H, wflags);
  if(!a->win){ fprintf(stderr, "SDL_CreateWindow error: %s\n", SDL_GetError()); SDL_Quit(); return 1; }

  a->ren = SDL_CreateRenderer(a->win, -1, SDL_RENDERER_ACCELERATED|SDL_RENDERER_PRESENTVSYNC);
  if(!a->ren) a->ren = SDL_CreateRenderer(a->win, -1, SDL_RENDERER_PRESENTVSYNC);
  if(!a->ren) a->ren = SDL_CreateRenderer(a->win, -1, SDL_RENDERER_SOFTWARE);
  if(!a->ren){ fprintf(stderr, "SDL_CreateRenderer error: %s\n", SDL_GetError()); SDL_DestroyWindow(a->win); SDL_Quit(); return 1; }
  SDL_RendererInfo rinfo;
  a->vsync = SDL_GetRendererInfo(a->ren, &rinfo) == 0 && (rinfo.flags & SDL_RENDERER_PRESENTVSYNC);

  // Draw in 720x760 logical units; SDL scales to the real (possibly HiDPI) pixel size,
  // and since everything is geometry it stays sharp at any scale.
  SDL_RenderSetLogicalSize(a->ren, WIN_W, WIN_H);
  SDL_SetRenderDrawBlendMode(a->ren, SDL_BLENDMODE_BLEND);

  if(!batch_init(&a->batch, a->ren, 65535)){ fprintf(stderr, "out of memory\n"); return 1; }
  if(!audio_init()) fprintf(stderr, "Note: no audio device; running silent.\n");

  hs_load(a);
  game_init(&a->g, ((uint64_t)time(NULL) << 32) ^ (uint64_t)SDL_GetPerformanceCounter());
  new_game(a);
  a->running = true;
  a->freq = (double)SDL_GetPerformanceFrequency();
  a->now = SDL_GetPerformanceCounter();

#ifdef __EMSCRIPTEN__
  emscripten_set_main_loop_arg(frame, a, 0, 1);
#else
  while(a->running) frame(a);
#endif

  pad_close(a);
  audio_quit();
  batch_free(&a->batch);
  SDL_DestroyRenderer(a->ren); SDL_DestroyWindow(a->win);
  SDL_Quit();
  return 0;
}
