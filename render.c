/*
 * IceBurger — procedural renderer
 * Mico Mrkaic, 2025
 */
#include "render.h"
#include "font8x8.h"
#include "game.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#if !SDL_VERSION_ATLEAST(2,0,18)
#error "IceBurger needs SDL 2.0.18 or newer (SDL_RenderGeometry)"
#endif

// ---------- Batch ----------
bool batch_init(Batch *b, SDL_Renderer *ren, int cap){
  memset(b, 0, sizeof *b);
  b->ren = ren; b->cap = cap - cap % 3;
  b->v = (SDL_Vertex *)malloc(sizeof(SDL_Vertex) * (size_t)b->cap);
  return b->v != NULL;
}
void batch_free(Batch *b){ free(b->v); b->v = NULL; b->n = b->cap = 0; }
void batch_flush(Batch *b){
  if(b->n) SDL_RenderGeometry(b->ren, NULL, b->v, b->n, NULL, 0);
  b->n = 0;
}
static inline void vtx(Batch *b, float x, float y, SDL_Color c){
  SDL_Vertex *v = &b->v[b->n++];
  v->position.x = x + b->dx; v->position.y = y + b->dy;
  v->color = c; v->tex_coord.x = 0; v->tex_coord.y = 0;
}
void draw_tri(Batch *b, float x0, float y0, float x1, float y1, float x2, float y2, SDL_Color c){
  if(b->n + 3 > b->cap) batch_flush(b);
  vtx(b, x0, y0, c); vtx(b, x1, y1, c); vtx(b, x2, y2, c);
}
void draw_rect(Batch *b, float x, float y, float w, float h, SDL_Color c){
  draw_tri(b, x, y, x+w, y, x+w, y+h, c);
  draw_tri(b, x, y, x+w, y+h, x, y+h, c);
}
void draw_rect_outline(Batch *b, float x, float y, float w, float h, float t, SDL_Color c){
  draw_rect(b, x, y, w, t, c);           draw_rect(b, x, y+h-t, w, t, c);
  draw_rect(b, x, y+t, t, h-2*t, c);     draw_rect(b, x+w-t, y+t, t, h-2*t, c);
}
void draw_line(Batch *b, float x0, float y0, float x1, float y1, float w, SDL_Color c){
  float dx = x1-x0, dy = y1-y0, len = sqrtf(dx*dx + dy*dy);
  if(len <= 0.0f) return;
  float nx = -dy/len * w*0.5f, ny = dx/len * w*0.5f;
  draw_tri(b, x0+nx, y0+ny, x1+nx, y1+ny, x1-nx, y1-ny, c);
  draw_tri(b, x0+nx, y0+ny, x1-nx, y1-ny, x0-nx, y0-ny, c);
}
void draw_arc(Batch *b, float cx, float cy, float r, float a0, float a1, SDL_Color c){
  int segs = (int)(r * 1.2f * (a1-a0) / (2.0f*(float)M_PI)) + 6;
  if(segs > 32) segs = 32;
  float step = (a1-a0) / (float)segs;
  for(int i=0;i<segs;i++){
    float t0 = a0 + step*(float)i, t1 = t0 + step;
    draw_tri(b, cx, cy, cx + r*cosf(t0), cy + r*sinf(t0), cx + r*cosf(t1), cy + r*sinf(t1), c);
  }
}
void draw_circle(Batch *b, float cx, float cy, float r, SDL_Color c){ draw_arc(b, cx, cy, r, 0.0f, 2.0f*(float)M_PI, c); }

static void draw_pill(Batch *b, float x0, float y0, float x1, float y1, SDL_Color c){
  float r = (y1-y0)*0.5f;
  draw_rect(b, x0+r, y0, (x1-x0)-2*r, y1-y0, c);
  draw_circle(b, x0+r, y0+r, r, c);
  draw_circle(b, x1-r, y0+r, r, c);
}

// ---------- Food tiles ----------
static SDL_Color shade(SDL_Color c, float f){
  float r = c.r*f, g = c.g*f, bl = c.b*f;
  SDL_Color o = { (Uint8)(r>255?255:r), (Uint8)(g>255?255:g), (Uint8)(bl>255?255:bl), c.a };
  return o;
}
static SDL_Color rgb(int r, int g, int b){ SDL_Color c = {(Uint8)r,(Uint8)g,(Uint8)b,255}; return c; }

static void draw_ice_cream(Batch *b, float x, float y, float u){
  SDL_Color ink = {60, 38, 28, 255}, cone = rgb(214,160,90), waffle = rgb(160,105,50);
  SDL_Color cream = rgb(255,250,238), cream_sh = rgb(222,208,190);
  float o = u*0.035f; // outline width
  // Cone with outline and waffle cross-hatch
  float lx0 = x+0.24f*u, ly0 = y+0.50f*u, rx0 = x+0.76f*u, bx = x+0.50f*u, by = y+0.98f*u;
  draw_tri(b, lx0-o, ly0-o, rx0+o, ly0-o, bx, by+o, ink);
  draw_tri(b, lx0, ly0, rx0, ly0, bx, by, cone);
  draw_line(b, lx0+(bx-lx0)*0.5f, ly0+(by-ly0)*0.5f, rx0, ly0, u*0.045f, waffle);
  draw_line(b, lx0, ly0, rx0+(bx-rx0)*0.5f, ly0+(by-ly0)*0.5f, u*0.045f, waffle);
  // Soft-serve swirl: three stacked bands, each outlined so the layers read separately
  const float L[3][4] = { // x0, y0, x1, y1
    {0.16f, 0.38f, 0.84f, 0.56f},
    {0.26f, 0.24f, 0.74f, 0.40f},
    {0.36f, 0.12f, 0.64f, 0.26f},
  };
  for(int i=0;i<3;i++){
    draw_pill(b, x+L[i][0]*u-o, y+L[i][1]*u-o, x+L[i][2]*u+o, y+L[i][3]*u+o, ink);
    draw_pill(b, x+L[i][0]*u, y+L[i][1]*u, x+L[i][2]*u, y+L[i][3]*u, cream_sh);
    draw_pill(b, x+L[i][0]*u, y+L[i][1]*u, x+(L[i][2]-0.04f)*u, y+(L[i][3]-0.04f)*u, cream);
  }
  // Curl on top
  draw_tri(b, x+0.42f*u-o, y+0.14f*u, x+0.60f*u+o, y+0.14f*u, x+0.56f*u, y+0.00f*u-o, ink);
  draw_tri(b, x+0.44f*u, y+0.14f*u, x+0.58f*u, y+0.14f*u, x+0.56f*u, y+0.02f*u, cream);
}

static void draw_burger(Batch *b, float x, float y, float u){
  SDL_Color ink = {60, 38, 28, 255}, bun = rgb(232,158,66), bun_hi = rgb(248,196,110);
  SDL_Color patty = rgb(108,56,28), cheese = rgb(255,204,40), lettuce = rgb(104,196,64), seed = rgb(252,242,210);
  float o = u*0.035f;
  // Bottom bun
  draw_pill(b, x+0.12f*u-o, y+0.73f*u-o, x+0.88f*u+o, y+0.89f*u+o, ink);
  draw_pill(b, x+0.12f*u, y+0.73f*u, x+0.88f*u, y+0.89f*u, bun);
  // Patty
  draw_pill(b, x+0.08f*u-o, y+0.58f*u-o, x+0.92f*u+o, y+0.75f*u+o, ink);
  draw_pill(b, x+0.08f*u, y+0.58f*u, x+0.92f*u, y+0.75f*u, patty);
  // Cheese with two drips
  draw_rect(b, x+0.12f*u, y+0.53f*u, 0.76f*u, 0.07f*u, cheese);
  draw_tri(b, x+0.28f*u, y+0.59f*u, x+0.42f*u, y+0.59f*u, x+0.35f*u, y+0.70f*u, cheese);
  draw_tri(b, x+0.60f*u, y+0.59f*u, x+0.72f*u, y+0.59f*u, x+0.66f*u, y+0.67f*u, cheese);
  // Lettuce: band plus a ruffled lower edge
  draw_rect(b, x+0.08f*u, y+0.48f*u, 0.84f*u, 0.06f*u, lettuce);
  for(int i=0;i<6;i++){
    float lx = x + (0.08f + 0.14f*(float)i)*u;
    draw_tri(b, lx, y+0.53f*u, lx+0.14f*u, y+0.53f*u, lx+0.07f*u, y+0.60f*u, lettuce);
  }
  // Top bun: dome with outline and highlight
  draw_arc(b, x+0.50f*u, y+0.50f*u, 0.42f*u + o, (float)M_PI, 2.0f*(float)M_PI, ink);
  draw_rect(b, x+0.08f*u-o, y+0.50f*u-o*0.2f, 0.84f*u+2*o, o*1.4f, ink);
  draw_arc(b, x+0.50f*u, y+0.50f*u, 0.42f*u, (float)M_PI, 2.0f*(float)M_PI, bun);
  draw_arc(b, x+0.42f*u, y+0.40f*u, 0.20f*u, (float)M_PI*1.1f, (float)M_PI*1.6f, bun_hi);
  // Sesame seeds
  const float S[6][2] = {{0.34f,0.26f},{0.50f,0.18f},{0.64f,0.27f},{0.42f,0.38f},{0.58f,0.39f},{0.26f,0.40f}};
  for(int i=0;i<6;i++) draw_rect(b, x+S[i][0]*u, y+S[i][1]*u, 0.07f*u, 0.04f*u, seed);
}

void draw_food_tile(Batch *b, float x, float y, float s, int food, SDL_Color tint){
  float u = s - 4.0f;
  SDL_Color sh = shade(tint, 0.6f); sh.a = tint.a;
  SDL_Color hi = shade(tint, 1.18f); hi.a = tint.a;
  draw_rect(b, x+2, y+2, u, u, sh);
  draw_rect(b, x, y, u, u, tint);
  draw_rect(b, x, y, u, u*0.10f, hi);
  if(food == FOOD_BURGER) draw_burger(b, x + u*0.06f, y + u*0.04f, u*0.88f);
  else                    draw_ice_cream(b, x + u*0.06f, y + u*0.02f, u*0.88f);
}

// ---------- Text ----------
float text_width(const char *s, float scale){ return (float)strlen(s) * 8.0f * scale; }

void draw_text(Batch *b, const char *s, float x, float y, float scale, SDL_Color c){
  for(; *s; s++, x += 8.0f*scale){
    unsigned char ch = (unsigned char)*s;
    if(ch < 32 || ch > 127) ch = '?';
    const unsigned char *gl = FONT8X8[ch - 32];
    for(int r=0;r<8;r++){
      unsigned bits = gl[r];
      int col = 0;
      while(bits){
        while(!(bits & 1u)){ bits >>= 1; col++; }
        int run = 0; while(bits & 1u){ bits >>= 1; run++; }
        draw_rect(b, x + (float)col*scale, y + (float)r*scale, (float)run*scale, scale, c);
        col += run;
      }
    }
  }
}

void draw_text_centered(Batch *b, const char *s, float cx, float y, float scale, SDL_Color c){
  draw_text(b, s, floorf(cx - text_width(s, scale)*0.5f), y, scale, c);
}

// ---------- Particles ----------
#define MAX_PARTICLES 4096
typedef struct { bool alive; float x, y, vx, vy, life, maxlife, grav, w, h; SDL_Color c; } Particle;
static Particle particles[MAX_PARTICLES];
static int particle_cursor = 0;

static float frandf(void){ return (float)rand() / (float)RAND_MAX; }

void fx_reset(void){ memset(particles, 0, sizeof particles); particle_cursor = 0; }

static Particle *particle_alloc(void){
  for(int n=0;n<MAX_PARTICLES;n++){
    int j = (particle_cursor + n) % MAX_PARTICLES;
    if(!particles[j].alive){ particle_cursor = (j+1) % MAX_PARTICLES; return &particles[j]; }
  }
  return NULL;
}

void fx_food_burst(float x, float y, int food){
  static const SDL_Color BURGER[5] = {{238,214,160,255},{214,40,32,255},{96,190,60,255},{112,60,30,255},{255,200,40,255}};
  static const SDL_Color ICE[4]    = {{255,250,238,255},{255,236,214,255},{255,186,206,255},{200,150,95,255}};
  int n = food == FOOD_BURGER ? 10 : 8;
  for(int i=0;i<n;i++){
    Particle *p = particle_alloc();
    if(!p) return;
    p->alive = true; p->life = 0.0f;
    p->x = x + (frandf()-0.5f)*20.0f; p->y = y + (frandf()-0.5f)*20.0f;
    float ang = frandf()*6.28318f;
    if(food == FOOD_BURGER){   // sesame, ketchup, lettuce, patty and cheese crumbs flung hard
      float spd = 150.0f + frandf()*300.0f;
      p->vx = cosf(ang)*spd; p->vy = sinf(ang)*spd - 120.0f;
      p->grav = 950.0f; p->maxlife = 0.6f + frandf()*0.6f;
      p->w = p->h = 3.0f + frandf()*3.0f;
      p->c = BURGER[rand()%5];
    } else {                   // slow, sagging drips of cream
      float spd = 30.0f + frandf()*90.0f;
      p->vx = cosf(ang)*spd; p->vy = sinf(ang)*spd*0.5f - 40.0f;
      p->grav = 160.0f; p->maxlife = 1.0f + frandf()*0.9f;
      p->w = 4.0f + frandf()*3.0f; p->h = p->w * 1.6f;
      p->c = ICE[rand()%4];
    }
  }
}

void fx_update(float dt){
  for(int i=0;i<MAX_PARTICLES;i++){
    Particle *p = &particles[i];
    if(!p->alive) continue;
    p->life += dt;
    if(p->life >= p->maxlife){ p->alive = false; continue; }
    p->vy += p->grav * dt;
    p->vx *= (1.0f - 0.8f*dt);
    p->x += p->vx * dt; p->y += p->vy * dt;
  }
}

void fx_draw(Batch *b, float ox, float oy){
  for(int i=0;i<MAX_PARTICLES;i++){
    const Particle *p = &particles[i];
    if(!p->alive) continue;
    SDL_Color c = p->c;
    c.a = (Uint8)(255.0f * (1.0f - p->life / p->maxlife));
    draw_rect(b, ox + p->x - p->w*0.5f, oy + p->y - p->h*0.5f, p->w, p->h, c);
  }
}
