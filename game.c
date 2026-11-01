/*
 * IceBurger — game rules (no SDL; testable headless)
 * Mico Mrkaic, 2025
 */
#include "game.h"
#include <string.h>

// ---------- RNG (xorshift64*) ----------
static uint64_t rng_next(uint64_t *s){
  uint64_t x = *s; x ^= x >> 12; x ^= x << 25; x ^= x >> 27; *s = x;
  return x * 0x2545F4914F6CDD1DULL;
}
static int rng_int(Game *g, int n){ return (int)((rng_next(&g->rng) >> 33) % (uint64_t)n); }

// ---------- Shapes and SRS ----------
// SRS spawn orientation, top-left of each piece's rotation box
static const uint8_t SHAPES[7][4][4] = {
  {{0,0,0,0},{1,1,1,1},{0,0,0,0},{0,0,0,0}}, // I
  {{1,1,0,0},{1,1,0,0},{0,0,0,0},{0,0,0,0}}, // O
  {{0,1,0,0},{1,1,1,0},{0,0,0,0},{0,0,0,0}}, // T
  {{0,1,1,0},{1,1,0,0},{0,0,0,0},{0,0,0,0}}, // S
  {{1,1,0,0},{0,1,1,0},{0,0,0,0},{0,0,0,0}}, // Z
  {{1,0,0,0},{1,1,1,0},{0,0,0,0},{0,0,0,0}}, // J
  {{0,0,1,0},{1,1,1,0},{0,0,0,0},{0,0,0,0}}, // L
};

static int box_n(int k){ return k==PK_I ? 4 : k==PK_O ? 2 : 3; }
static int spawn_x(int k){ return k==PK_O ? COLS/2 - 1 : COLS/2 - 2; }

// SRS wall kicks, (dx, dy) with y UP as in the spec; dy is negated when applied.
// Transition order: 0->R, R->0, R->2, 2->R, 2->L, L->2, L->0, 0->L
static const int KICKS_JLSTZ[8][5][2] = {
  {{0,0},{-1,0},{-1, 1},{0,-2},{-1,-2}},
  {{0,0},{ 1,0},{ 1,-1},{0, 2},{ 1, 2}},
  {{0,0},{ 1,0},{ 1,-1},{0, 2},{ 1, 2}},
  {{0,0},{-1,0},{-1, 1},{0,-2},{-1,-2}},
  {{0,0},{ 1,0},{ 1, 1},{0,-2},{ 1,-2}},
  {{0,0},{-1,0},{-1,-1},{0, 2},{-1, 2}},
  {{0,0},{-1,0},{-1,-1},{0, 2},{-1, 2}},
  {{0,0},{ 1,0},{ 1, 1},{0,-2},{ 1,-2}},
};
static const int KICKS_I[8][5][2] = {
  {{0,0},{-2,0},{ 1,0},{-2,-1},{ 1, 2}},
  {{0,0},{ 2,0},{-1,0},{ 2, 1},{-1,-2}},
  {{0,0},{-1,0},{ 2,0},{-1, 2},{ 2,-1}},
  {{0,0},{ 1,0},{-2,0},{ 1,-2},{-2, 1}},
  {{0,0},{ 2,0},{-1,0},{ 2, 1},{-1,-2}},
  {{0,0},{-2,0},{ 1,0},{-2,-1},{ 1, 2}},
  {{0,0},{ 1,0},{-2,0},{ 1,-2},{-2, 1}},
  {{0,0},{-1,0},{ 2,0},{-1, 2},{ 2,-1}},
};

static void piece_respawn(Piece *p){
  memcpy(p->m, SHAPES[p->k], sizeof p->m);
  p->rot = 0;
  p->x = spawn_x(p->k); p->y = 0;
}

static void rotate_mask(Piece *p, bool cw){
  int n = box_n(p->k); uint8_t t[4][4] = {{0}};
  for(int r=0;r<n;r++) for(int c=0;c<n;c++){
    if(cw) t[c][n-1-r] = p->m[r][c];
    else   t[n-1-c][r] = p->m[r][c];
  }
  memcpy(p->m, t, sizeof t);
  p->rot = (p->rot + (cw ? 1 : 3)) & 3;
}

// ---------- Events ----------
static void emit(Game *g, EventType t, int a){
  if(g->n_events < MAX_EVENTS){ g->events[g->n_events].type = t; g->events[g->n_events].a = a; g->n_events++; }
}
void game_clear_events(Game *g){ g->n_events = 0; g->n_cleared = 0; }

// ---------- 7-bag ----------
static void bag_refill(Game *g){
  for(int i=0;i<7;i++) g->bag[i] = (uint8_t)i;
  for(int i=6;i>0;i--){ int j = rng_int(g, i+1); uint8_t t = g->bag[i]; g->bag[i] = g->bag[j]; g->bag[j] = t; }
  g->bag_pos = 0;
}
static Piece bag_draw(Game *g){
  if(g->bag_pos >= 7) bag_refill(g);
  Piece p; memset(&p, 0, sizeof p);
  p.k = g->bag[g->bag_pos++];
  p.food = rng_int(g, 2);
  piece_respawn(&p);
  return p;
}

// ---------- Queries ----------
bool game_collides(const Game *g, const Piece *p, int nx, int ny){
  for(int r=0;r<4;r++) for(int c=0;c<4;c++){
    if(!p->m[r][c]) continue;
    int x = nx + c, y = ny + r;
    if(x<0 || x>=COLS || y<0 || y>=ROWS) return true;
    if(g->board[y][x].filled) return true;
  }
  return false;
}

bool game_on_ground(const Game *g){ return game_collides(g, &g->cur, g->cur.x, g->cur.y + 1); }

int game_ghost_y(const Game *g){
  int y = g->cur.y;
  while(!game_collides(g, &g->cur, g->cur.x, y + 1)) y++;
  return y;
}

double game_fall_interval(const Game *g){
  int ms = START_SPEED_MS - g->level * SPEED_STEP_MS;
  if(ms < MIN_SPEED_MS) ms = MIN_SPEED_MS;
  double s = ms / 1000.0;
  return g->freeze_timer > 0.0 ? s * 2.0 : s;   // Brain Freeze: half gravity
}

const char *game_level_name(int level){
  if(level < 3) return "SALAD";
  if(level < 6) return "SNACK";
  if(level < 9) return "DRIVE-THRU";
  return "FOOD COMA";
}

static bool row_pure(const Cell row[COLS], int food){
  for(int c=0;c<COLS;c++) if(row[c].food != food) return false;
  return true;
}

long game_row_kcal(const Cell row[COLS]){
  long k = 0;
  for(int c=0;c<COLS;c++) k += row[c].food == FOOD_BURGER ? KCAL_BURGER : KCAL_ICE;
  return row_pure(row, FOOD_BURGER) ? 2 * k : k;  // Combo Meal: double
}

// ---------- Spawning, locking, clearing ----------
static void lock_state_reset(Game *g){
  g->lock_timer = 0.0; g->lock_resets = 0; g->lowest_y = g->cur.y; g->fall_accum = 0.0;
}

static void spawn_next(Game *g){
  g->cur = g->queue[0];
  memmove(&g->queue[0], &g->queue[1], sizeof(Piece) * (QUEUE_LEN - 1));
  g->queue[QUEUE_LEN-1] = bag_draw(g);
  piece_respawn(&g->cur);
  g->can_hold = true;
  lock_state_reset(g);
  if(game_collides(g, &g->cur, g->cur.x, g->cur.y)){ g->game_over = true; emit(g, EV_GAME_OVER, 0); }
}

static void clear_lines(Game *g){
  int cleared = 0, combo = 0, freeze = 0;
  long kcal = 0;
  for(int r=ROWS-1;r>=0;r--){
    bool full = true;
    for(int c=0;c<COLS;c++) if(!g->board[r][c].filled){ full = false; break; }
    if(!full) continue;
    // Every clear below has pulled this row down one, so before the clear it was at r - cleared
    if(g->n_cleared < MAX_CLEARED){
      g->cleared[g->n_cleared].row = r - cleared;
      memcpy(g->cleared[g->n_cleared].cells, g->board[r], sizeof g->board[r]);
      g->n_cleared++;
    }
    if(row_pure(g->board[r], FOOD_BURGER)) combo++;
    if(row_pure(g->board[r], FOOD_ICE)) freeze++;
    kcal += game_row_kcal(g->board[r]);
    cleared++;
    for(int rr=r; rr>0; rr--) memcpy(g->board[rr], g->board[rr-1], sizeof g->board[rr]);
    memset(g->board[0], 0, sizeof g->board[0]);
    r++; // recheck this index: it now holds the row from above
  }
  if(!cleared) return;

  static const int mult4[5] = {0, 4, 5, 6, 8}; // x1, x1.25, x1.5, x2 (Food Fight)
  g->kcal += kcal * mult4[cleared] / 4;
  emit(g, EV_CLEAR, cleared);
  if(combo)  emit(g, EV_COMBO_MEAL, combo);
  if(freeze){ g->freeze_timer = FREEZE_S; emit(g, EV_BRAIN_FREEZE, freeze); }

  int old = g->level;
  g->lines += cleared;
  g->level = g->lines / 10;
  if(g->level > old) emit(g, EV_LEVEL_UP, g->level);
}

static void lock_piece(Game *g){
  for(int r=0;r<4;r++) for(int c=0;c<4;c++){
    if(!g->cur.m[r][c]) continue;
    int x = g->cur.x + c, y = g->cur.y + r;
    if(y>=0 && y<ROWS && x>=0 && x<COLS){
      g->board[y][x].filled = true;
      g->board[y][x].food = (uint8_t)g->cur.food;
      g->board[y][x].kind = (uint8_t)g->cur.k;
    }
  }
  emit(g, EV_LOCK, 0);
  clear_lines(g);
  spawn_next(g);
}

// A successful move/rotate while grounded restarts the lock timer, up to LOCK_MAX_RESETS times
static void after_manipulation(Game *g){
  if(game_on_ground(g) && g->lock_resets < LOCK_MAX_RESETS){ g->lock_timer = 0.0; g->lock_resets++; }
}

static void note_descent(Game *g){
  if(g->cur.y > g->lowest_y){ g->lowest_y = g->cur.y; g->lock_resets = 0; }
}

// ---------- Public actions ----------
void game_init(Game *g, uint64_t seed){
  memset(g, 0, sizeof *g);
  g->rng = seed ? seed : 0x9E3779B97F4A7C15ULL;
  game_reset(g);
}

void game_reset(Game *g){
  uint64_t rng = g->rng;
  memset(g, 0, sizeof *g);
  g->rng = rng;
  g->bag_pos = 7; // force a refill on first draw
  g->cur = bag_draw(g);
  for(int i=0;i<QUEUE_LEN;i++) g->queue[i] = bag_draw(g);
  g->can_hold = true;
  lock_state_reset(g);
}

bool game_move(Game *g, int dx){
  if(g->game_over || game_collides(g, &g->cur, g->cur.x + dx, g->cur.y)) return false;
  g->cur.x += dx;
  emit(g, EV_MOVE, dx);
  after_manipulation(g);
  return true;
}

bool game_rotate(Game *g, bool cw){
  if(g->game_over || g->cur.k == PK_O) return false; // O never changes shape
  Piece t = g->cur;
  int from = t.rot;
  rotate_mask(&t, cw);
  int idx = cw ? from*2 : (from*2 + 7) % 8;
  const int (*kicks)[2] = t.k == PK_I ? KICKS_I[idx] : KICKS_JLSTZ[idx];
  for(int i=0;i<5;i++){
    int nx = t.x + kicks[i][0];
    int ny = t.y - kicks[i][1]; // SRS y is up, board y is down
    if(!game_collides(g, &t, nx, ny)){
      t.x = nx; t.y = ny; g->cur = t;
      emit(g, EV_ROTATE, cw ? 1 : -1);
      note_descent(g);
      after_manipulation(g);
      return true;
    }
  }
  return false;
}

bool game_soft_drop(Game *g){
  if(g->game_over || game_on_ground(g)) return false;
  g->cur.y++;
  g->kcal += KCAL_SOFT_DROP;
  g->fall_accum = 0.0;
  note_descent(g);
  return true;
}

void game_hard_drop(Game *g){
  if(g->game_over) return;
  int y = game_ghost_y(g);
  g->kcal += (long)(y - g->cur.y) * KCAL_HARD_DROP;
  g->cur.y = y;
  emit(g, EV_HARD_DROP, 0);
  lock_piece(g);
}

void game_hold(Game *g){
  if(g->game_over || !g->can_hold) return;
  Piece prev = g->cur;
  piece_respawn(&prev);              // held pieces go back to spawn orientation
  if(!g->has_hold){
    g->hold = prev; g->has_hold = true;
    spawn_next(g);
  } else {
    g->cur = g->hold; g->hold = prev;
    piece_respawn(&g->cur);
    lock_state_reset(g);
    if(game_collides(g, &g->cur, g->cur.x, g->cur.y)){ g->game_over = true; emit(g, EV_GAME_OVER, 0); }
  }
  g->can_hold = false;
  emit(g, EV_HOLD, 0);
}

void game_update(Game *g, double dt){
  if(g->game_over) return;

  if(g->freeze_timer > 0.0){
    g->freeze_timer -= dt;
    if(g->freeze_timer <= 0.0){ g->freeze_timer = 0.0; emit(g, EV_FREEZE_END, 0); }
  }

  // Gravity
  g->fall_accum += dt;
  double step = game_fall_interval(g);
  while(g->fall_accum >= step){
    g->fall_accum -= step;
    if(game_on_ground(g)){ g->fall_accum = 0.0; break; }
    g->cur.y++;
    note_descent(g);
  }

  // Lock delay: runs only while grounded; with resets used up, touching down locks at once
  if(game_on_ground(g)){
    g->lock_timer += dt;
    if(g->lock_timer >= LOCK_DELAY_S || g->lock_resets >= LOCK_MAX_RESETS) lock_piece(g);
  } else {
    g->lock_timer = 0.0;
  }
}
