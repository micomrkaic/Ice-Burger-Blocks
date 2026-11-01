/*
 * IceBurger Tetris — game rules (no SDL; testable headless)
 * Mico Mrkaic, 2025
 */
#ifndef GAME_H
#define GAME_H

#include <stdbool.h>
#include <stdint.h>

#define COLS 10
#define ROWS 20
#define QUEUE_LEN 5             // pieces shown in the NEXT column

#define START_SPEED_MS 900
#define SPEED_STEP_MS 70
#define MIN_SPEED_MS 90

#define LOCK_DELAY_S 0.5        // time a grounded piece may slide before it locks
#define LOCK_MAX_RESETS 15      // moves/rotations that may restart the lock timer
#define FREEZE_S 5.0            // Brain Freeze duration (gravity halved)

#define KCAL_BURGER 550         // per cleared cell
#define KCAL_ICE 250
#define KCAL_SOFT_DROP 1        // per row
#define KCAL_HARD_DROP 2        // per row

enum { FOOD_ICE = 0, FOOD_BURGER = 1 };
enum { PK_I, PK_O, PK_T, PK_S, PK_Z, PK_J, PK_L };

typedef struct { bool filled; uint8_t food; uint8_t kind; } Cell;   // kind = tetromino, picks the tint

typedef struct {
  int k;                  // PK_*
  int x, y;               // top-left of the rotation box on the board
  int rot;                // SRS state: 0=spawn, 1=R, 2=180, 3=L
  int food;               // FOOD_*
  uint8_t m[4][4];        // occupancy mask
} Piece;

typedef enum {
  EV_MOVE, EV_ROTATE, EV_HARD_DROP, EV_LOCK, EV_HOLD,
  EV_CLEAR,               // a = lines cleared (1..4)
  EV_COMBO_MEAL,          // a = all-burger rows in this clear
  EV_BRAIN_FREEZE,        // a = all-ice-cream rows in this clear
  EV_FREEZE_END,
  EV_LEVEL_UP,            // a = new level
  EV_GAME_OVER
} EventType;

typedef struct { EventType type; int a; } GameEvent;

// A cleared row as it was just before it vanished; row = index on the board at that time
typedef struct { int row; Cell cells[COLS]; } ClearedRow;

#define MAX_EVENTS 32
#define MAX_CLEARED 8

typedef struct {
  Cell board[ROWS][COLS];
  Piece cur, hold;
  Piece queue[QUEUE_LEN];
  uint8_t bag[7]; int bag_pos;
  bool has_hold, can_hold, game_over;
  long kcal; int lines, level;
  double fall_accum;      // seconds
  double lock_timer;      // seconds the current piece has been grounded
  int lock_resets, lowest_y;
  double freeze_timer;    // Brain Freeze time left
  uint64_t rng;
  // Output for the frontend; cleared by game_clear_events()
  GameEvent events[MAX_EVENTS]; int n_events;
  ClearedRow cleared[MAX_CLEARED]; int n_cleared;
} Game;

void   game_init(Game *g, uint64_t seed);   // seeds the RNG and starts a game
void   game_reset(Game *g);                 // new game; RNG stream continues
bool   game_move(Game *g, int dx);
bool   game_rotate(Game *g, bool cw);
bool   game_soft_drop(Game *g);             // one row; false if grounded
void   game_hard_drop(Game *g);
void   game_hold(Game *g);
void   game_update(Game *g, double dt);     // gravity, lock delay, timers
void   game_clear_events(Game *g);

bool   game_collides(const Game *g, const Piece *p, int x, int y);
bool   game_on_ground(const Game *g);
int    game_ghost_y(const Game *g);
double game_fall_interval(const Game *g);   // seconds per gravity row (Brain Freeze aware)
const char *game_level_name(int level);
long   game_row_kcal(const Cell row[COLS]); // value of one row, Combo Meal included

#endif
