/* Headless tests for game.c — no SDL needed.  Run: make test */
#include "../game.h"
#include <stdio.h>
#include <string.h>

static int fails = 0, checks = 0;
#define CHECK(cond) do{ checks++; if(!(cond)){ fails++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } }while(0)

static void clear_board(Game *g){ memset(g->board, 0, sizeof g->board); }
static int min_x(const Piece *p){ int m=99; for(int r=0;r<4;r++)for(int c=0;c<4;c++) if(p->m[r][c] && p->x+c<m) m=p->x+c; return m; }
static bool has_event(const Game *g, EventType t, int *a){
  for(int i=0;i<g->n_events;i++) if(g->events[i].type==t){ if(a) *a=g->events[i].a; return true; }
  return false;
}
static void set_cur(Game *g, int k, int food){
  Game tmp; game_init(&tmp, 1);
  // build a spawn-orientation piece of kind k via the public API: cycle the queue until it shows up
  while(tmp.cur.k != k){ game_hard_drop(&tmp); if(tmp.game_over) game_reset(&tmp); }
  g->cur = tmp.cur; g->cur.food = food;
  g->lowest_y = g->cur.y; g->lock_timer = 0; g->lock_resets = 0; g->can_hold = true;
}
static void fill_row(Game *g, int r, int food, int gap){
  for(int c=0;c<COLS;c++) if(c!=gap){ g->board[r][c].filled=true; g->board[r][c].food=(uint8_t)food; }
}

static void test_bag(void){
  Game g; game_init(&g, 42);
  // The first 7*20 pieces dealt (current + queue + draws) come in complete bags of seven
  int seq[140]; int n=0;
  seq[n++] = g.cur.k;
  for(int i=0;i<QUEUE_LEN;i++) seq[n++] = g.queue[i].k;
  while(n<140){ clear_board(&g); game_hard_drop(&g); seq[n++] = g.queue[QUEUE_LEN-1].k; }
  for(int b=0;b<20;b++){
    int seen=0; for(int i=0;i<7;i++) seen |= 1<<seq[b*7+i];
    CHECK(seen == 0x7F);
  }
}

static void test_rotation(void){
  Game g; game_init(&g, 7); clear_board(&g);
  for(int k=0;k<7;k++){
    set_cur(&g, k, FOOD_ICE); g.cur.y = 8;
    Piece s = g.cur;
    for(int i=0;i<4;i++) game_rotate(&g, true);
    CHECK(memcmp(s.m, g.cur.m, sizeof s.m)==0 && s.x==g.cur.x && s.y==g.cur.y && g.cur.rot==0);
    for(int i=0;i<4;i++) game_rotate(&g, false);
    CHECK(memcmp(s.m, g.cur.m, sizeof s.m)==0 && s.x==g.cur.x && s.y==g.cur.y);
  }
  // O stays put
  set_cur(&g, PK_O, FOOD_ICE); int x0 = min_x(&g.cur);
  CHECK(!game_rotate(&g, true)); CHECK(min_x(&g.cur)==x0);
  // I kicks off the left wall
  set_cur(&g, PK_I, FOOD_ICE); g.cur.y = 8; game_rotate(&g, true);
  while(game_move(&g, -1)) {}
  CHECK(game_rotate(&g, true)); CHECK(min_x(&g.cur)==0);
  // T pivots on its centre in open space
  set_cur(&g, PK_T, FOOD_ICE); g.cur.y = 8;
  int cx = g.cur.x+1, cy = g.cur.y+1; game_rotate(&g, true);
  CHECK(g.cur.m[cy-g.cur.y][cx-g.cur.x]);
}

static void test_lock_delay(void){
  Game g; game_init(&g, 3); clear_board(&g); set_cur(&g, PK_T, FOOD_ICE);
  game_hard_drop(&g); // reference: hard drop locks at once
  CHECK(has_event(&g, EV_LOCK, NULL)); game_clear_events(&g);

  clear_board(&g); set_cur(&g, PK_T, FOOD_ICE);
  while(game_soft_drop(&g)) {}
  CHECK(game_on_ground(&g));
  game_update(&g, 0.3); CHECK(!has_event(&g, EV_LOCK, NULL));
  CHECK(game_move(&g, 1));            // resets the timer
  game_update(&g, 0.3); CHECK(!has_event(&g, EV_LOCK, NULL));
  game_update(&g, 0.25); CHECK(has_event(&g, EV_LOCK, NULL));
  game_clear_events(&g);

  // Resets are capped: after LOCK_MAX_RESETS moves on the ground, the piece locks
  clear_board(&g); set_cur(&g, PK_T, FOOD_ICE);
  while(game_soft_drop(&g)) {}
  for(int i=0;i<LOCK_MAX_RESETS;i++){ game_move(&g, (i&1)?-1:1); game_update(&g, 0.01); }
  CHECK(has_event(&g, EV_LOCK, NULL));
}

static void test_gravity(void){
  Game g; game_init(&g, 5); clear_board(&g); set_cur(&g, PK_T, FOOD_ICE);
  int y0 = g.cur.y;
  for(int i=0;i<60*4;i++) game_update(&g, 1.0/60); // 4.0 s at 900 ms per row
  CHECK(g.cur.y - y0 == 4);
  // Brain Freeze doubles the interval
  g.freeze_timer = 3.0;
  CHECK(game_fall_interval(&g) > 1.79 && game_fall_interval(&g) < 1.81);
  game_clear_events(&g);
  game_update(&g, 3.1); CHECK(has_event(&g, EV_FREEZE_END, NULL)); CHECK(g.freeze_timer == 0.0);
}

static void test_ghost_and_drop_scoring(void){
  Game g; game_init(&g, 9); clear_board(&g); set_cur(&g, PK_O, FOOD_ICE);
  int gy = game_ghost_y(&g);
  CHECK(gy == ROWS-2);
  long k0 = g.kcal; int y0 = g.cur.y;
  game_hard_drop(&g);
  CHECK(g.kcal - k0 == (long)(gy - y0) * KCAL_HARD_DROP);
  k0 = g.kcal; CHECK(game_soft_drop(&g)); CHECK(g.kcal - k0 == KCAL_SOFT_DROP);
}

static void test_clears_and_bonuses(void){
  Game g; game_init(&g, 11);

  // Mixed single: 9 ice + ... the O piece supplies the rest
  clear_board(&g); game_clear_events(&g);
  fill_row(&g, ROWS-1, FOOD_ICE, 0);  fill_row(&g, ROWS-2, FOOD_ICE, 0);
  g.board[ROWS-1][1].food = FOOD_BURGER;   // break purity on the bottom row
  g.board[ROWS-2][1].food = FOOD_BURGER;
  set_cur(&g, PK_I, FOOD_BURGER); game_rotate(&g, true);  // vertical I
  while(game_move(&g, -1)) {}
  long k0 = g.kcal; int y0 = g.cur.y, gy = game_ghost_y(&g);
  game_hard_drop(&g);
  int n=0; CHECK(has_event(&g, EV_CLEAR, &n)); CHECK(n==2);
  CHECK(!has_event(&g, EV_COMBO_MEAL, NULL) && !has_event(&g, EV_BRAIN_FREEZE, NULL));
  long row = 8*KCAL_ICE + 2*KCAL_BURGER;
  CHECK(g.kcal - k0 == (long)(gy-y0)*KCAL_HARD_DROP + 2*row*5/4);
  CHECK(g.n_cleared == 2 && g.cleared[0].row == ROWS-1 && g.cleared[1].row == ROWS-2);

  // Combo Meal: all-burger row pays double
  clear_board(&g); game_clear_events(&g);
  fill_row(&g, ROWS-1, FOOD_BURGER, 0);
  set_cur(&g, PK_I, FOOD_BURGER); game_rotate(&g, true); while(game_move(&g, -1)) {}
  k0 = g.kcal; y0 = g.cur.y; gy = game_ghost_y(&g);
  game_hard_drop(&g);
  CHECK(has_event(&g, EV_COMBO_MEAL, NULL));
  CHECK(g.kcal - k0 == (long)(gy-y0)*KCAL_HARD_DROP + 2*10*KCAL_BURGER);

  // Brain Freeze: all-ice row starts the timer
  clear_board(&g); game_clear_events(&g); g.freeze_timer = 0;
  fill_row(&g, ROWS-1, FOOD_ICE, 0);
  set_cur(&g, PK_I, FOOD_ICE); game_rotate(&g, true); while(game_move(&g, -1)) {}
  game_hard_drop(&g);
  CHECK(has_event(&g, EV_BRAIN_FREEZE, NULL)); CHECK(g.freeze_timer == FREEZE_S);

  // Food Fight (4 lines) uses the x2 multiplier; rows reported at their pre-clear positions
  clear_board(&g); game_clear_events(&g);
  for(int r=ROWS-4;r<ROWS;r++){ fill_row(&g, r, FOOD_ICE, 0); g.board[r][5].food = FOOD_BURGER; }
  set_cur(&g, PK_I, FOOD_ICE); game_rotate(&g, true); while(game_move(&g, -1)) {}
  k0 = g.kcal; y0 = g.cur.y; gy = game_ghost_y(&g);
  game_hard_drop(&g);
  CHECK(has_event(&g, EV_CLEAR, &n)); CHECK(n==4);
  CHECK(g.kcal - k0 == (long)(gy-y0)*KCAL_HARD_DROP + 4*(9L*KCAL_ICE + KCAL_BURGER)*2);
  for(int i=0;i<4;i++) CHECK(g.cleared[i].row == ROWS-1-i);

  // Level up after 10 lines
  game_init(&g, 12); game_clear_events(&g); g.lines = 9;
  clear_board(&g); fill_row(&g, ROWS-1, FOOD_ICE, 0); g.board[ROWS-1][3].food = FOOD_BURGER;
  set_cur(&g, PK_I, FOOD_ICE); game_rotate(&g, true); while(game_move(&g, -1)) {}
  game_hard_drop(&g);
  CHECK(has_event(&g, EV_LEVEL_UP, &n) && n==1);
}

static void test_hold(void){
  Game g; game_init(&g, 13); clear_board(&g); set_cur(&g, PK_T, FOOD_BURGER);
  game_rotate(&g, true); CHECK(g.cur.rot==1);
  int next_k = g.queue[0].k;
  game_hold(&g);
  CHECK(g.has_hold && g.hold.k==PK_T && g.hold.rot==0 && g.hold.food==FOOD_BURGER);
  CHECK(g.cur.k == next_k);
  CHECK(!g.can_hold);
  int k = g.cur.k; game_hold(&g); CHECK(g.cur.k == k);  // second hold ignored
  game_hard_drop(&g); CHECK(g.can_hold);
  game_hold(&g); CHECK(g.cur.k == PK_T && g.cur.rot == 0);
}

static void test_game_over(void){
  Game g; game_init(&g, 14);
  for(int r=0;r<ROWS;r++) for(int c=0;c<COLS;c++) if(c!=0){ g.board[r][c].filled=true; }
  game_clear_events(&g);
  game_hard_drop(&g);
  CHECK(g.game_over); CHECK(has_event(&g, EV_GAME_OVER, NULL));
  CHECK(!game_move(&g, -1));
}

int main(void){
  test_bag(); test_rotation(); test_lock_delay(); test_gravity();
  test_ghost_and_drop_scoring(); test_clears_and_bonuses(); test_hold(); test_game_over();
  printf("%d/%d checks passed\n", checks-fails, checks);
  return fails ? 1 : 0;
}
