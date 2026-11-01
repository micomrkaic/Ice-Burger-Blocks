/*
 * IceBurger Tetris — procedural renderer: one batched SDL_RenderGeometry call per flush,
 * food tiles drawn from triangles, 8x8 bitmap text, themed particles.
 * Mico Mrkaic, 2025
 */
#ifndef RENDER_H
#define RENDER_H

#include <SDL.h>
#include <stdbool.h>

typedef struct {
  SDL_Renderer *ren;
  SDL_Vertex *v; int n, cap;
  float dx, dy;           // global offset (screen shake)
} Batch;

bool  batch_init(Batch *b, SDL_Renderer *ren, int cap);
void  batch_free(Batch *b);
void  batch_flush(Batch *b);

void  draw_tri(Batch *b, float x0, float y0, float x1, float y1, float x2, float y2, SDL_Color c);
void  draw_rect(Batch *b, float x, float y, float w, float h, SDL_Color c);
void  draw_rect_outline(Batch *b, float x, float y, float w, float h, float t, SDL_Color c);
void  draw_line(Batch *b, float x0, float y0, float x1, float y1, float w, SDL_Color c);
void  draw_arc(Batch *b, float cx, float cy, float r, float a0, float a1, SDL_Color c); // filled sector
void  draw_circle(Batch *b, float cx, float cy, float r, SDL_Color c);

// Food tile at (x,y) of size s; tint is the tetromino colour, alpha scales the whole tile
void  draw_food_tile(Batch *b, float x, float y, float s, int food, SDL_Color tint);

// Text: 8x8 font scaled by `scale` (integer scales look crisp)
float text_width(const char *s, float scale);
void  draw_text(Batch *b, const char *s, float x, float y, float scale, SDL_Color c);
void  draw_text_centered(Batch *b, const char *s, float cx, float y, float scale, SDL_Color c);

// Particles (board-relative pixel coordinates)
void  fx_reset(void);
void  fx_food_burst(float x, float y, int food);
void  fx_update(float dt);
void  fx_draw(Batch *b, float ox, float oy);

#endif
