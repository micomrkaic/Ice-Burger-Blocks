/*
  Mico Mrkaic, September 2025

 * IceBurger Tetris — C + SDL2 (single-file)
 * Blocks are made from 🍦 and 🍔 “tiles”; clearing lines triggers a chunky particle explosion.
 *
 * Build (any platform with pkg-config):
 *   make
 *
 * Build by hand (macOS/Homebrew):
 *   brew install sdl2 sdl2_ttf
 *   clang -O2 -Wall -Wextra -std=c11 tetris.c $(pkg-config --cflags --libs sdl2 SDL2_ttf) -o tetris
 *
 * Build by hand (Linux/Debian/Ubuntu):
 *   sudo apt-get install libsdl2-dev libsdl2-ttf-dev
 *   gcc -O2 -Wall -Wextra -std=c11 tetris.c $(pkg-config --cflags --libs sdl2 SDL2_ttf) -lm -o tetris
 *
 * Run:
 *   ./tetris
 *
 * Controls:
 *   ←/→ move, ↓ soft drop, ↑ rotate CW, Z rotate CCW, Space hard drop, C hold, P pause, R restart, Esc quit
 *
 * Notes:
 * - Uses SDL2 for rendering and SDL_ttf to draw emoji/text. Tiles fall back to plain colored squares
 *   unless a font that actually contains 🍦 and 🍔 is found. Color-bitmap emoji fonts
 *   (Noto Color Emoji, Apple Color Emoji) need SDL_ttf >= 2.0.18 and FreeType built with PNG support.
 * - If no UI font is found, score and game state are shown in the window title instead.
 * - Rotation follows the Super Rotation System (SRS), including its wall-kick tables.
 * - Particle system is simple + efficient; feel free to tweak constants.
 */

#include <SDL.h>
#include <SDL_ttf.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>

#define COLS 10
#define ROWS 20
#define TILE 32
#define BORDER 2
#define PREVIEW_W 6
#define PREVIEW_H 6

#define START_SPEED_MS 900
#define SPEED_STEP_MS 70
#define MIN_SPEED_MS 90

#define MAX_PARTICLES 4096

#define MAX_DT 0.1f          // clamp frame time (s) so a stalled frame can't cause a burst of drops
#define FRAME_CAP_MS 7       // ~144 fps cap when vsync is unavailable or the window is hidden

// Utility min/max
static int imax(int a, int b){return a>b?a:b;}
static int imin(int a, int b){return a<b?a:b;}

// RNG
static float frandf(){return (float)rand()/(float)RAND_MAX;}

// Colors
static SDL_Color col_bg = {20, 24, 28, 255};
static SDL_Color col_grid = {36, 42, 48, 255};
static SDL_Color col_text = {235, 235, 235, 255};

// Per-piece tint
static SDL_Color col_piece[7] = {
  {45, 212, 191, 255}, // I
  {250, 204, 21, 255}, // O
  {192, 132, 252, 255}, // T
  {74, 222, 128, 255}, // S
  {251, 113, 133, 255}, // Z
  {96, 165, 250, 255}, // J
  {245, 158, 11, 255}  // L
};

// Emoji strings
static const char *EMOJI_ICE = "🍦"; // UTF-8
static const char *EMOJI_BURGER = "🍔"; // UTF-8
#define CP_ICE    0x1F366u
#define CP_BURGER 0x1F354u

// Board cell
typedef struct { bool filled; int type; int tint; } Cell; // type: 0=ice,1=burger

// Piece
typedef struct {
  int k;                // 0..6 which tetromino
  int w, h;             // dims of shape
  int x, y;             // top-left on board
  int rot;              // SRS rotation state: 0=spawn, 1=R, 2=180, 3=L
  unsigned char m[4][4];// shape mask (up to 4x4)
  int type;             // 0 ice / 1 burger (visual)
  int tint;             // color index
} Piece;

// Particle
typedef struct {
  bool alive;
  float x,y,vx,vy; // in pixels, board-relative
  float life, maxlife;
  SDL_Color c;
} Particle;

// Game state
typedef struct {
  Cell board[ROWS][COLS];
  Piece cur, next, hold;
  bool has_hold;
  bool can_hold;
  bool game_over;
  int score;
  int lines;
  int level;
  int fall_ms;
  float fall_accum; // seconds accumulator
} Game;

// Shapes (SRS spawn orientation, top-left of each piece's rotation box)
static const unsigned char SHAPES[7][4][4] = {
  // I
  {{0,0,0,0},{1,1,1,1},{0,0,0,0},{0,0,0,0}},
  // O
  {{1,1,0,0},{1,1,0,0},{0,0,0,0},{0,0,0,0}},
  // T
  {{0,1,0,0},{1,1,1,0},{0,0,0,0},{0,0,0,0}},
  // S
  {{0,1,1,0},{1,1,0,0},{0,0,0,0},{0,0,0,0}},
  // Z
  {{1,1,0,0},{0,1,1,0},{0,0,0,0},{0,0,0,0}},
  // J
  {{1,0,0,0},{1,1,1,0},{0,0,0,0},{0,0,0,0}},
  // L
  {{0,0,1,0},{1,1,1,0},{0,0,0,0},{0,0,0,0}},
};

// Rotation box size: I=4, O=2, everything else 3
static int box_n(int k){ return k==0 ? 4 : k==1 ? 2 : 3; }
// Spawn column: 3-wide pieces and I occupy columns 3..5/3..6, O occupies 4..5
static int spawn_x(int k){ return k==1 ? COLS/2 - 1 : COLS/2 - 2; }

// SRS wall kicks, (dx, dy) with y pointing UP as in the SRS spec; negate dy when applying.
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

// Restore spawn orientation and position, keeping the piece's kind and visual type
static void piece_respawn(Piece *p){
  for(int r=0;r<4;r++)for(int c=0;c<4;c++) p->m[r][c]=SHAPES[p->k][r][c];
  p->rot = 0;
  p->x = spawn_x(p->k); p->y = 0;
}

static void piece_from_k(Piece *p, int k){
  memset(p,0,sizeof(*p));
  p->k = k;
  p->w = 4; p->h = 4;
  p->type = (rand()%2);
  p->tint = k;
  piece_respawn(p);
}

static void rotate_cw(Piece *p){
  int n = box_n(p->k); unsigned char t[4][4] = {0};
  for(int r=0;r<n;r++) for(int c=0;c<n;c++) t[c][n-1-r]=p->m[r][c];
  memcpy(p->m,t,sizeof t);
  p->rot = (p->rot+1)&3;
}
static void rotate_ccw(Piece *p){
  int n = box_n(p->k); unsigned char t[4][4] = {0};
  for(int r=0;r<n;r++) for(int c=0;c<n;c++) t[n-1-c][r]=p->m[r][c];
  memcpy(p->m,t,sizeof t);
  p->rot = (p->rot+3)&3;
}

static bool collide(const Game *g, const Piece *p, int nx, int ny){
  for(int r=0;r<4;r++){
    for(int c=0;c<4;c++){
      if(!p->m[r][c]) continue;
      int x = nx + c;
      int y = ny + r;
      if(x<0||x>=COLS||y<0||y>=ROWS) return true;
      if(g->board[y][x].filled) return true;
    }
  }
  return false;
}

static void lock_piece(Game *g){
  for(int r=0;r<4;r++){
    for(int c=0;c<4;c++){
      if(!g->cur.m[r][c]) continue;
      int x=g->cur.x+c, y=g->cur.y+r;
      if(y>=0 && y<ROWS && x>=0 && x<COLS){
        g->board[y][x].filled=true;
        g->board[y][x].type=g->cur.type;
        g->board[y][x].tint=g->cur.tint;
      }
    }
  }
}

// Particle system
static Particle particles[MAX_PARTICLES];
static int particle_cursor = 0; // next slot to try
static void particles_reset(){ memset(particles,0,sizeof particles); particle_cursor = 0; }
static int particle_alloc(void){
  for(int n=0;n<MAX_PARTICLES;n++){
    int j = (particle_cursor + n) % MAX_PARTICLES;
    if(!particles[j].alive){ particle_cursor = (j+1) % MAX_PARTICLES; return j; }
  }
  return -1; // pool full
}
static void spawn_explosion(int cx, int cy, SDL_Color base){
  // cx,cy: board-relative pixel centre of the cleared line
  int count = 120 + rand()%80;
  for(int i=0;i<count;i++){
    int j = particle_alloc();
    if(j<0) break;
    particles[j].alive=true;
    particles[j].x=cx+(frandf()-0.5f)*TILE*COLS*0.1f;
    particles[j].y=cy+(frandf()-0.5f)*TILE*2;
    float ang = frandf()*6.28318f;
    float spd = 100.0f + frandf()*300.0f;
    particles[j].vx=cosf(ang)*spd;
    particles[j].vy=sinf(ang)*spd - (50.0f+frandf()*100.0f);
    particles[j].life=0.0f;
    particles[j].maxlife=0.6f+frandf()*0.6f;
    SDL_Color c = base;
    int d = (int)(frandf()*40.0f);
    c.r = (Uint8)imax(0, imin(255, c.r + d - 20));
    c.g = (Uint8)imax(0, imin(255, c.g + d - 20));
    c.b = (Uint8)imax(0, imin(255, c.b + d - 20));
    particles[j].c=c;
  }
}
static void particles_update(float dt){
  for(int i=0;i<MAX_PARTICLES;i++) if(particles[i].alive){
    particles[i].life += dt;
    if(particles[i].life >= particles[i].maxlife){ particles[i].alive=false; continue; }
    // gravity + drag
    particles[i].vy += 900.0f * dt;
    particles[i].vx *= (1.0f - 0.8f*dt);
    particles[i].x += particles[i].vx * dt;
    particles[i].y += particles[i].vy * dt;
  }
}

static void clear_lines(Game *g){
  int cleared = 0;
  for(int r=ROWS-1;r>=0;r--){
    bool full=true; for(int c=0;c<COLS;c++) if(!g->board[r][c].filled){ full=false; break; }
    if(full){
      // Each clear below pulled this row down by one, so its original index is r - cleared
      int orig_r = r - cleared;
      int cx = TILE*COLS/2; int cy = (int)(TILE*(orig_r+0.5f));
      SDL_Color base = {255, 200, 120, 255};
      spawn_explosion(cx, cy, base);
      cleared++;
      // pull down
      for(int rr=r; rr>0; rr--) memcpy(g->board[rr], g->board[rr-1], sizeof g->board[rr]);
      memset(g->board[0], 0, sizeof g->board[0]);
      r++; // recheck same row after pull
    }
  }
  if(cleared){
    static const int score_tbl[5]={0,40,100,300,1200};
    g->score += score_tbl[cleared]*(g->level+1);
    g->lines += cleared;
    g->level = g->lines/10;
    g->fall_ms = imax(MIN_SPEED_MS, START_SPEED_MS - g->level * SPEED_STEP_MS);
  }
}

static void new_bag_piece(Piece *p){ piece_from_k(p, rand()%7); }

static void spawn_piece(Game *g){
  g->cur = g->next;
  new_bag_piece(&g->next);
  piece_respawn(&g->cur);
  if(collide(g,&g->cur,g->cur.x,g->cur.y)) g->game_over=true;
  g->can_hold=true;
}

static void hold_piece(Game *g){
  if(!g->can_hold) return;
  Piece prev = g->cur;
  piece_respawn(&prev); // held pieces go back to spawn orientation
  if(!g->has_hold){ g->hold = prev; g->has_hold=true; spawn_piece(g); }
  else {
    g->cur = g->hold; g->hold = prev;
    piece_respawn(&g->cur);
    if(collide(g,&g->cur,g->cur.x,g->cur.y)) g->game_over=true;
  }
  g->can_hold=false;
}

static void hard_drop(Game *g){
  while(!collide(g,&g->cur,g->cur.x,g->cur.y+1)) g->cur.y++;
  lock_piece(g);
  clear_lines(g);
  spawn_piece(g);
}

static void soft_step(Game *g){
  if(!collide(g,&g->cur,g->cur.x,g->cur.y+1)) g->cur.y++;
  else { lock_piece(g); clear_lines(g); spawn_piece(g); }
}

static void attempt_rotate(Game *g, bool cw){
  if(g->cur.k==1) return; // O never changes shape
  Piece t = g->cur;
  int from = t.rot;
  if(cw) rotate_cw(&t); else rotate_ccw(&t);
  int idx = cw ? from*2 : (from*2 + 7) % 8;
  const int (*kicks)[2] = (t.k==0) ? KICKS_I[idx] : KICKS_JLSTZ[idx];
  for(int i=0;i<5;i++){
    int nx = t.x + kicks[i][0];
    int ny = t.y - kicks[i][1]; // SRS y is up, board y is down
    if(!collide(g,&t,nx,ny)){ t.x=nx; t.y=ny; g->cur=t; return; }
  }
}

static void game_reset(Game *g){
  memset(g,0,sizeof *g);
  g->fall_ms = START_SPEED_MS; g->level=0; g->lines=0; g->score=0; g->fall_accum=0.0f;
  for(int r=0;r<ROWS;r++) for(int c=0;c<COLS;c++) g->board[r][c].filled=false;
  new_bag_piece(&g->cur); new_bag_piece(&g->next);
  g->can_hold=true; g->has_hold=false; g->game_over=false;
  particles_reset();
}

// ---------- Rendering ----------

// Pre-rendered emoji tiles (built once at startup; NULL tex => plain colored squares)
typedef struct { SDL_Texture *tex; int w, h; } Sprite;
static Sprite emoji_sprite[2];

// Cached text texture; re-rendered only when the string changes
typedef struct { SDL_Texture *tex; int w, h; char str[128]; } TextCache;

static bool font_has_glyph(TTF_Font *f, Uint32 cp){
#ifdef SDL_TTF_VERSION_ATLEAST
#if SDL_TTF_VERSION_ATLEAST(2,0,18)
  return TTF_GlyphIsProvided32(f, cp) != 0;
#else
  (void)f; (void)cp; return false; // older SDL_ttf cannot address glyphs outside the BMP
#endif
#else
  (void)f; (void)cp; return false;
#endif
}

static bool emoji_sprites_load(SDL_Renderer *ren, TTF_Font *font){
  const char *txt[2] = { EMOJI_ICE, EMOJI_BURGER };
  for(int i=0;i<2;i++){
    SDL_Surface *surf = TTF_RenderUTF8_Blended(font, txt[i], (SDL_Color){255,255,255,255});
    if(!surf) return false;
    float scale = (float)(TILE-6) / (float)imax(1, imax(surf->w, surf->h));
    emoji_sprite[i].w = (int)(surf->w * scale);
    emoji_sprite[i].h = (int)(surf->h * scale);
    emoji_sprite[i].tex = SDL_CreateTextureFromSurface(ren, surf);
    SDL_FreeSurface(surf);
    if(!emoji_sprite[i].tex) return false;
  }
  return true;
}

static void emoji_sprites_free(void){
  for(int i=0;i<2;i++){ if(emoji_sprite[i].tex) SDL_DestroyTexture(emoji_sprite[i].tex); emoji_sprite[i].tex=NULL; }
}

static void fill_rect(SDL_Renderer *ren, int x,int y,int w,int h, SDL_Color c){
  SDL_SetRenderDrawColor(ren,c.r,c.g,c.b,c.a); SDL_Rect R={x,y,w,h}; SDL_RenderFillRect(ren,&R);
}

static void draw_text_cached(SDL_Renderer *ren, TTF_Font *font, TextCache *tc, const char *txt, int x, int y, SDL_Color color){
  if(!font||!txt||!*txt) return;
  if(!tc->tex || strcmp(tc->str, txt)!=0){
    if(tc->tex){ SDL_DestroyTexture(tc->tex); tc->tex=NULL; }
    SDL_Surface *surf = TTF_RenderUTF8_Blended(font, txt, color);
    if(!surf) return;
    tc->tex = SDL_CreateTextureFromSurface(ren, surf);
    tc->w = surf->w; tc->h = surf->h;
    SDL_FreeSurface(surf);
    if(!tc->tex) return;
    snprintf(tc->str, sizeof tc->str, "%s", txt);
  }
  SDL_Rect dst = {x, y, tc->w, tc->h};
  SDL_RenderCopy(ren, tc->tex, NULL, &dst);
}

static void text_cache_free(TextCache *tc){ if(tc->tex) SDL_DestroyTexture(tc->tex); tc->tex=NULL; tc->str[0]=0; }

static void draw_tile(SDL_Renderer *ren, int px, int py, int type, SDL_Color tint){
  // Background rounded-ish square
  SDL_Color shadow = { (Uint8)(tint.r*0.6f), (Uint8)(tint.g*0.6f), (Uint8)(tint.b*0.6f), 255 };
  fill_rect(ren, px+2, py+2, TILE-4, TILE-4, shadow);
  fill_rect(ren, px, py, TILE-4, TILE-4, tint);
  // Emoji overlay if available
  const Sprite *s = &emoji_sprite[type ? 1 : 0];
  if(s->tex){
    SDL_Rect dst={px + (TILE-4-s->w)/2, py + (TILE-4-s->h)/2, s->w, s->h};
    SDL_RenderCopy(ren, s->tex, NULL, &dst);
  }
}

static void render_board(SDL_Renderer *ren, const Game *g, int ox, int oy){
  // grid bg
  fill_rect(ren, ox-8, oy-8, COLS*TILE+16, ROWS*TILE+16, col_grid);
  for(int r=0;r<ROWS;r++){
    for(int c=0;c<COLS;c++){
      int px = ox + c*TILE; int py = oy + r*TILE;
      fill_rect(ren, px, py, TILE-1, TILE-1, (SDL_Color){30,35,40,255});
      if(g->board[r][c].filled){
        draw_tile(ren, px, py, g->board[r][c].type, col_piece[g->board[r][c].tint]);
      }
    }
  }
  // current piece
  for(int r=0;r<4;r++) for(int c=0;c<4;c++) if(g->cur.m[r][c]){
    int x = g->cur.x+c, y=g->cur.y+r; if(y<0) continue; if(x<0||x>=COLS||y>=ROWS) continue;
    int px = ox + x*TILE; int py = oy + y*TILE;
    draw_tile(ren, px, py, g->cur.type, col_piece[g->cur.tint]);
  }
}

static void render_preview(SDL_Renderer *ren, const Piece *p, int ox, int oy){
  fill_rect(ren, ox-8, oy-8, PREVIEW_W*TILE+16, PREVIEW_H*TILE+16, col_grid);
  Piece t=*p; // draw centered
  for(int r=0;r<4;r++) for(int c=0;c<4;c++) if(t.m[r][c]){
    int px = ox + c*TILE; int py = oy + r*TILE;
    draw_tile(ren, px, py, t.type, col_piece[t.tint]);
  }
}

static void render_particles(SDL_Renderer *ren, int ox, int oy){
  SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
  for(int i=0;i<MAX_PARTICLES;i++) if(particles[i].alive){
    float a = 1.0f - (particles[i].life / particles[i].maxlife);
    SDL_SetRenderDrawColor(ren, particles[i].c.r, particles[i].c.g, particles[i].c.b, (Uint8)(a*255));
    SDL_Rect R = { ox + (int)particles[i].x, oy + (int)particles[i].y, 4, 4 };
    SDL_RenderFillRect(ren, &R);
  }
  SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_NONE);
}

static TTF_Font *open_first_font(const char *const *paths, int size){
  for(int i=0;paths[i];i++){ TTF_Font *f = TTF_OpenFont(paths[i], size); if(f) return f; }
  return NULL;
}

int main(int argc, char **argv){
  (void)argc; (void)argv;
  srand((unsigned)time(NULL));
  if(SDL_Init(SDL_INIT_VIDEO|SDL_INIT_TIMER)!=0){ fprintf(stderr,"SDL_Init error: %s\n", SDL_GetError()); return 1; }
  if(TTF_Init()!=0){ fprintf(stderr,"TTF_Init error: %s\n", TTF_GetError()); SDL_Quit(); return 1; }

  int winW = 720, winH = 760;
  SDL_Window *win = SDL_CreateWindow("IceBurger Tetris", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, winW, winH, SDL_WINDOW_SHOWN);
  if(!win){ fprintf(stderr,"SDL_CreateWindow error: %s\n", SDL_GetError()); TTF_Quit(); SDL_Quit(); return 1; }

  // Accelerated+vsync first, then any renderer with vsync, then software
  SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED|SDL_RENDERER_PRESENTVSYNC);
  if(!ren) ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_PRESENTVSYNC);
  if(!ren) ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
  if(!ren){ fprintf(stderr,"SDL_CreateRenderer error: %s\n", SDL_GetError()); SDL_DestroyWindow(win); TTF_Quit(); SDL_Quit(); return 1; }
  SDL_RendererInfo rinfo;
  bool vsync = SDL_GetRendererInfo(ren, &rinfo)==0 && (rinfo.flags & SDL_RENDERER_PRESENTVSYNC);

  // Emoji are downscaled to tile size: use linear filtering for the textures created below
  SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");

  // Emoji font: accept only a font that really has both glyphs; otherwise use plain squares.
  const char *const emoji_candidates[] = {
    "/System/Library/Fonts/Apple Color Emoji.ttc",                          // macOS
    "/usr/share/fonts/truetype/noto/NotoColorEmoji.ttf",                    // Debian/Ubuntu
    "/usr/share/fonts/noto/NotoColorEmoji.ttf",                             // Arch
    "/usr/share/fonts/google-noto-color-emoji-fonts/NotoColorEmoji.ttf",    // Fedora
    NULL
  };
  for(int i=0;emoji_candidates[i];i++){
    TTF_Font *f = TTF_OpenFont(emoji_candidates[i], 64);
    if(!f) continue;
    bool ok = font_has_glyph(f, CP_ICE) && font_has_glyph(f, CP_BURGER) && emoji_sprites_load(ren, f);
    TTF_CloseFont(f); // glyphs are baked into textures; the font is no longer needed
    if(ok) break;
    emoji_sprites_free();
  }
  if(!emoji_sprite[0].tex) fprintf(stderr, "Note: no usable emoji font found; using plain tiles.\n");

  const char *const ui_candidates[] = {
    "/System/Library/Fonts/SFNS.ttf",                                       // macOS
    "/System/Library/Fonts/Supplemental/Arial.ttf",                         // macOS
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",                      // Debian/Ubuntu
    "/usr/share/fonts/TTF/DejaVuSans.ttf",                                  // Arch
    "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf",                    // Fedora
    "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
    NULL
  };
  TTF_Font *ui_font = open_first_font(ui_candidates, 22);
  if(!ui_font) fprintf(stderr, "Note: no UI font found; showing score in the window title.\n");

  TextCache hud_txt = {0}, pause_txt = {0}, over_txt = {0};
  char title_last[256] = "";

  Game g; game_reset(&g);

  bool running=true, paused=false;
  Uint64 now=SDL_GetPerformanceCounter();
  Uint64 last=now; double freq=(double)SDL_GetPerformanceFrequency();

  while(running){
    last = now; now = SDL_GetPerformanceCounter();
    float dt = (float)((now-last)/freq);
    if(dt > MAX_DT) dt = MAX_DT;

    // input
    SDL_Event e; while(SDL_PollEvent(&e)){
      if(e.type==SDL_QUIT) running=false;
      if(e.type==SDL_KEYDOWN){
        SDL_Keycode k = e.key.keysym.sym;
        // OS key repeat only for movement; hard drop, rotate, hold, pause etc. need a fresh press
        if(e.key.repeat && k!=SDLK_LEFT && k!=SDLK_RIGHT && k!=SDLK_DOWN) continue;
        if(k==SDLK_ESCAPE) running=false;
        else if(k==SDLK_p) paused=!paused;
        else if(k==SDLK_r) { game_reset(&g); paused=false; }
        if(g.game_over||paused) continue;
        if(k==SDLK_LEFT && !collide(&g,&g.cur,g.cur.x-1,g.cur.y)) g.cur.x--;
        else if(k==SDLK_RIGHT && !collide(&g,&g.cur,g.cur.x+1,g.cur.y)) g.cur.x++;
        else if(k==SDLK_DOWN) soft_step(&g);
        else if(k==SDLK_SPACE) hard_drop(&g);
        else if(k==SDLK_c) hold_piece(&g);
        else if(k==SDLK_z) attempt_rotate(&g,false);
        else if(k==SDLK_UP) attempt_rotate(&g,true);
      }
    }

    if(!paused && !g.game_over){
      g.fall_accum += dt;
      float step = g.fall_ms / 1000.0f;
      while(g.fall_accum >= step && !g.game_over){ g.fall_accum -= step; soft_step(&g); step = g.fall_ms / 1000.0f; }
    }

    particles_update(dt);

    // draw
    SDL_SetRenderDrawColor(ren, col_bg.r,col_bg.g,col_bg.b,255);
    SDL_RenderClear(ren);

    int ox = 40, oy = 40;
    render_board(ren, &g, ox, oy);
    render_preview(ren, &g.next, ox + COLS*TILE + 40, oy);
    if(g.has_hold) render_preview(ren, &g.hold, ox + COLS*TILE + 40, oy + PREVIEW_H*TILE + 24);

    render_particles(ren, ox, oy);

    char buf[128];
    snprintf(buf,sizeof buf, "Score %d  Lines %d  Level %d", g.score, g.lines, g.level);
    if(ui_font){
      draw_text_cached(ren, ui_font, &hud_txt, buf, ox, oy + ROWS*TILE + 24, col_text);
      if(paused) draw_text_cached(ren, ui_font, &pause_txt, "PAUSED (P)", ox+220, oy+200, (SDL_Color){255,210,60,255});
      if(g.game_over) draw_text_cached(ren, ui_font, &over_txt, "GAME OVER (R to restart)", ox+120, oy+220, (SDL_Color){255,120,120,255});
    } else {
      char title[256];
      snprintf(title, sizeof title, "IceBurger Tetris — %s%s", buf,
               g.game_over ? "  [GAME OVER — R to restart]" : paused ? "  [PAUSED]" : "");
      if(strcmp(title, title_last)!=0){ SDL_SetWindowTitle(win, title); snprintf(title_last, sizeof title_last, "%s", title); }
    }

    SDL_RenderPresent(ren);

    // Without vsync (or while minimized/hidden, where some drivers return immediately) cap the frame rate
    if(!vsync || (SDL_GetWindowFlags(win) & (SDL_WINDOW_MINIMIZED|SDL_WINDOW_HIDDEN))){
      double spent_ms = (double)(SDL_GetPerformanceCounter() - now) * 1000.0 / freq;
      double wait_ms = FRAME_CAP_MS - spent_ms;
      SDL_Delay(wait_ms > 1.0 ? (Uint32)wait_ms : 1);
    }
  }

  text_cache_free(&hud_txt); text_cache_free(&pause_txt); text_cache_free(&over_txt);
  emoji_sprites_free();
  if(ui_font) TTF_CloseFont(ui_font);
  SDL_DestroyRenderer(ren); SDL_DestroyWindow(win);
  TTF_Quit(); SDL_Quit();
  return 0;
}
