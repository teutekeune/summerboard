#define _GNU_SOURCE
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <X11/cursorfont.h>
#include <X11/extensions/XShm.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <dirent.h>
#include <ctype.h>
#include <fcntl.h>
#include <stdarg.h>

#define HUD_TITLE_VER "v10"
#define MAX_APPS 64
#define MAX_WINS 6
#define NGRID 16
#define NGLYPH 96

#define GRID_COLS 4
#define GRID_ROWS 5
#define APPS_PER_PAGE (GRID_COLS * GRID_ROWS)

enum { ST_CLOSED, ST_FG, ST_BG };

static int dbg_on;
#define DBG(...) do { if (dbg_on) { fprintf(stderr, "[summerboard] " __VA_ARGS__); fputc('\n', stderr); } } while (0)

static double now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}
static inline double clampd(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }

/* ------------------------------------------------------------------------ */
/* SPRING PHYSICS                                                           */
/* ------------------------------------------------------------------------ */
typedef struct { double x, v, target, k, c, eps; } Spring;

static void sp_init(Spring *s, double val, double k, double c, double eps) {
  s->x = val; s->v = 0; s->target = val; s->k = k; s->c = c; s->eps = eps;
}
static void sp_snap(Spring *s, double val) { s->x = val; s->v = 0; s->target = val; }
static int sp_busy(const Spring *s) { return !(s->x == s->target && s->v == 0); }
static void sp_update(Spring *s, double dt) {
  if (!sp_busy(s)) return;
  const int n = 4;
  double sub = fmin(dt, 0.032) / n;
  for (int i = 0; i < n; i++) {
    double a = -s->k * (s->x - s->target) - s->c * s->v;
    s->v += a * sub;
    s->x += s->v * sub;
  }
  if (fabs(s->x - s->target) < s->eps && fabs(s->v) < s->eps * 12) { s->x = s->target; s->v = 0; }
}

typedef struct { double x, y, w, h, r; } Rect;
typedef struct { Spring x, y, w, h, r; } RectSpring;

static void rs_init(RectSpring *s, Rect r, double k, double c) {
  sp_init(&s->x, r.x, k, c, 0.05); sp_init(&s->y, r.y, k, c, 0.05);
  sp_init(&s->w, r.w, k, c, 0.05); sp_init(&s->h, r.h, k, c, 0.05);
  sp_init(&s->r, r.r, k, c, 0.05);
}
static void rs_target(RectSpring *s, Rect r) {
  s->x.target = r.x; s->y.target = r.y; s->w.target = r.w; s->h.target = r.h; s->r.target = r.r;
}
static void rs_snap(RectSpring *s, Rect r) {
  sp_snap(&s->x, r.x); sp_snap(&s->y, r.y); sp_snap(&s->w, r.w); sp_snap(&s->h, r.h); sp_snap(&s->r, r.r);
}
static void rs_update(RectSpring *s, double dt) {
  sp_update(&s->x, dt); sp_update(&s->y, dt); sp_update(&s->w, dt); sp_update(&s->h, dt); sp_update(&s->r, dt);
}
static void rs_inject(RectSpring *s, double vx, double vy) {
  s->x.v = vx; s->y.v = vy; s->w.v = 0; s->h.v = 0; s->r.v = 0;
}
static Rect rs_val(const RectSpring *s) { Rect r = { s->x.x, s->y.x, s->w.x, s->h.x, s->r.x }; return r; }

typedef struct { double x1, y1, x2, y2; } Bez;
static double bez_ease(const Bez *b, double x) {
  if (x <= 0) return 0;
  if (x >= 1) return 1;
#define BA(a1, a2) (1 - 3 * (a2) + 3 * (a1))
#define BB(a1, a2) (3 * (a2) - 6 * (a1))
#define BC(a1) (3 * (a1))
#define BEZ(t, a1, a2) (((BA(a1, a2) * (t) + BB(a1, a2)) * (t) + BC(a1)) * (t))
#define BDV(t, a1, a2) (3 * BA(a1, a2) * (t) * (t) + 2 * BB(a1, a2) * (t) + BC(a1))
  double t = x;
  for (int i = 0; i < 6; i++) {
    double d = BDV(t, b->x1, b->x2);
    if (fabs(d) < 1e-6) break;
    t -= (BEZ(t, b->x1, b->x2) - x) / d;
    t = clampd(t, 0, 1);
  }
  return BEZ(t, b->y1, b->y2);
}
static const Bez APPEAR = { 0.35, 0.14, 0.41, 1 };
static double appear_ease(double x) { return bez_ease(&APPEAR, x); }
#define APPEAR_DUR 380.0

/* ------------------------------------------------------------------------ */
/* SOFTWARE RASTERISER                                                       */
/* ------------------------------------------------------------------------ */
typedef struct { int w, h; uint32_t *px; } Surf;

static Surf surf_new(int w, int h) { Surf s = { w, h, calloc((size_t)w * h, 4) }; return s; }
static Surf surf_new_raw(int w, int h) { Surf s = { w, h, malloc((size_t)w * h * 4) }; return s; }
static void surf_free(Surf *s) { if (s->px) free(s->px); s->px = NULL; s->w = s->h = 0; }

static inline uint32_t mix32(uint32_t d, uint32_t s, unsigned a) {
  uint32_t rb = (((d & 0xff00ff) * (256 - a) + (s & 0xff00ff) * a) >> 8) & 0xff00ff;
  uint32_t g = (((d & 0x00ff00) * (256 - a) + (s & 0x00ff00) * a) >> 8) & 0x00ff00;
  return rb | g;
}

/* Blend a constant colour over n pixels. The per-pixel source terms are hoisted
   out of the loop (2 multiplies/pixel instead of 4) and the opaque case is a plain
   store. Results are bit-identical to calling mix32() per pixel. */
static inline void blend_span(uint32_t *p, int n, uint32_t col, unsigned a) {
  if (n <= 0 || a == 0) return;
  if (a >= 256) {
    uint32_t c = col & 0xffffff;
    for (int i = 0; i < n; i++) p[i] = c;
    return;
  }
  const uint32_t srb = (col & 0xff00ff) * a, sg = (col & 0x00ff00) * a;
  const unsigned ia = 256 - a;
  for (int i = 0; i < n; i++) {
    uint32_t d = p[i];
    p[i] = ((((d & 0xff00ff) * ia + srb) >> 8) & 0xff00ff) |
           ((((d & 0x00ff00) * ia + sg) >> 8) & 0x00ff00);
  }
}

typedef struct { float cx, cy, hx, hy, r, x0, y0, x1, y1; } RR;

static RR rr_make(double x, double y, double w, double h, double r) {
  RR c;
  if (w < 0) { x += w; w = -w; }
  if (h < 0) { y += h; h = -h; }
  double m = fmin(w, h) / 2;
  if (r > m) r = m;
  if (r < 0) r = 0;
  c.cx = x + w / 2; c.cy = y + h / 2; c.hx = w / 2; c.hy = h / 2; c.r = r;
  c.x0 = x; c.y0 = y; c.x1 = x + w; c.y1 = y + h;
  return c;
}
static inline float rr_cov(const RR *c, float px, float py) {
  float dx = fabsf(px - c->cx), dy = fabsf(py - c->cy);
  if ((dx <= c->hx - 1.f && dy <= c->hy - c->r) || (dx <= c->hx - c->r && dy <= c->hy - 1.f)) return 1.f;
  float qx = dx - (c->hx - c->r), qy = dy - (c->hy - c->r);
  float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
  float m = qx > qy ? qx : qy; if (m > 0) m = 0;
  float d = sqrtf(ox * ox + oy * oy) + m - c->r;
  float a = 0.5f - d;
  return a < 0 ? 0 : (a > 1 ? 1 : a);
}
static int rr_row_span(const RR *c, float py, int *l, int *r) {
  float dy = fabsf(py - c->cy), hw;
  if (dy <= c->hy - c->r) hw = c->hx - 1.f;
  else if (dy <= c->hy - 1.f) hw = c->hx - c->r;
  else return 0;
  if (hw < 0) return 0;
  *l = (int)ceilf(c->cx - hw - 0.5f);
  *r = (int)floorf(c->cx + hw - 0.5f);
  return *r >= *l;
}
static int solid_span(const RR *a, const RR *b, int y, int *l, int *r) {
  int l1, r1;
  if (!rr_row_span(a, y + 0.5f, &l1, &r1)) return 0;
  if (b) {
    int l2, r2;
    if (!rr_row_span(b, y + 0.5f, &l2, &r2)) return 0;
    if (l2 > l1) l1 = l2;
    if (r2 < r1) r1 = r2;
  }
  *l = l1; *r = r1;
  return r1 >= l1;
}
static void bbox(const RR *a, const RR *clip, const Surf *t, int *x0, int *y0, int *x1, int *y1) {
  *x0 = (int)floorf(a->x0); *y0 = (int)floorf(a->y0); *x1 = (int)ceilf(a->x1); *y1 = (int)ceilf(a->y1);
  if (clip) {
    int cx0 = (int)floorf(clip->x0), cy0 = (int)floorf(clip->y0), cx1 = (int)ceilf(clip->x1), cy1 = (int)ceilf(clip->y1);
    if (cx0 > *x0) *x0 = cx0;
    if (cy0 > *y0) *y0 = cy0;
    if (cx1 < *x1) *x1 = cx1;
    if (cy1 < *y1) *y1 = cy1;
  }
  if (*x0 < 0) *x0 = 0;
  if (*y0 < 0) *y0 = 0;
  if (*x1 > t->w) *x1 = t->w;
  if (*y1 > t->h) *y1 = t->h;
}

static void fill_rr(Surf *t, const RR *rr, uint32_t col, double alpha, const RR *clip) {
  int x0, y0, x1, y1;
  bbox(rr, clip, t, &x0, &y0, &x1, &y1);
  unsigned ac = (unsigned)(alpha * 256 + 0.5);
  if (ac > 256) ac = 256;
  if (!ac) return;
  for (int y = y0; y < y1; y++) {
    uint32_t *row = t->px + (size_t)y * t->w;
    int sl = 1, sr = 0;
    int have = solid_span(rr, clip, y, &sl, &sr);
    int x = x0;
    while (x < x1) {
      if (have && x >= sl && x <= sr) {
        int end = sr < x1 - 1 ? sr : x1 - 1;
        blend_span(row + x, end - x + 1, col, ac);
        x = end + 1;
        continue;
      }
      float cov = rr_cov(rr, x + 0.5f, y + 0.5f);
      if (cov > 0 && clip) cov *= rr_cov(clip, x + 0.5f, y + 0.5f);
      unsigned a = (unsigned)(cov * ac + 0.5f);
      if (a) row[x] = mix32(row[x], col, a);
      x++;
    }
  }
}
static void fill_rect(Surf *t, int x, int y, int w, int h, uint32_t col, double alpha) {
  unsigned a = (unsigned)(alpha * 256 + 0.5);
  if (a > 256) a = 256;
  int x1 = x + w, y1 = y + h;
  if (x < 0) x = 0; if (y < 0) y = 0;
  if (x1 > t->w) x1 = t->w; if (y1 > t->h) y1 = t->h;
  if (x1 <= x || !a) return;
  for (int j = y; j < y1; j++) blend_span(t->px + (size_t)j * t->w + x, x1 - x, col, a);
}

/* Per-column sampling tables for draw_image (independent of y, so built once per call). */
static int *di_xa, *di_xb;
static unsigned *di_tx;
static int di_cap;

static inline uint32_t bilerp(const uint32_t *r0, const uint32_t *r1, int xa, int xb, unsigned tx, unsigned ty) {
  uint32_t a = tx ? mix32(r0[xa], r0[xb], tx) : (r0[xa] & 0xffffff);
  if (!ty) return a;
  uint32_t b = tx ? mix32(r1[xa], r1[xb], tx) : (r1[xa] & 0xffffff);
  return mix32(a, b, ty);
}

static void draw_image(Surf *t, const Surf *src, const Rect *r, const RR *clip) {
  if (!src->px || r->w < 1 || r->h < 1) return;
  double sc = fmax(r->w / src->w, r->h / src->h);
  double ox = (src->w - r->w / sc) / 2, oy = (src->h - r->h / sc) / 2;
  RR rr = rr_make(r->x, r->y, r->w, r->h, r->r);
  int x0, y0, x1, y1;
  bbox(&rr, clip, t, &x0, &y0, &x1, &y1);
  if (x1 <= x0 || y1 <= y0) return;
  int n = x1 - x0;
  if (n > di_cap) {
    int cap = n + 64;
    int *a = realloc(di_xa, sizeof(int) * cap); if (!a) return; di_xa = a;
    int *b = realloc(di_xb, sizeof(int) * cap); if (!b) return; di_xb = b;
    unsigned *c = realloc(di_tx, sizeof(unsigned) * cap); if (!c) return; di_tx = c;
    di_cap = cap;
  }
  float inv = (float)(1.0 / sc);
  for (int i = 0; i < n; i++) {
    float fx = (float)(ox + (x0 + i + 0.5 - r->x) * inv - 0.5);
    int xa = (int)floorf(fx);
    di_tx[i] = (unsigned)((fx - xa) * 256);
    int xb = xa + 1;
    if (xa < 0) xa = 0; if (xa >= src->w) xa = src->w - 1;
    if (xb < 0) xb = 0; if (xb >= src->w) xb = src->w - 1;
    di_xa[i] = xa; di_xb[i] = xb;
  }

  /* 1:1 blit fast path: unscaled and integer-aligned (e.g. a full-screen preview at rest). */
  int copy1 = 0, dxo = 0, dyo = 0;
  if (fabs(sc - 1.0) < 1e-9) {
    double ddx = ox - r->x, ddy = oy - r->y;
    if (fabs(ddx - round(ddx)) < 1e-6 && fabs(ddy - round(ddy)) < 1e-6) {
      copy1 = 1; dxo = (int)lround(ddx); dyo = (int)lround(ddy);
    }
  }

  for (int y = y0; y < y1; y++) {
    uint32_t *row = t->px + (size_t)y * t->w;
    float fy = (float)(oy + (y + 0.5 - r->y) * inv - 0.5);
    int ya = (int)floorf(fy);
    unsigned ty = (unsigned)((fy - ya) * 256);
    int yb = ya + 1;
    if (ya < 0) ya = 0; if (ya >= src->h) ya = src->h - 1;
    if (yb < 0) yb = 0; if (yb >= src->h) yb = src->h - 1;
    const uint32_t *r0 = src->px + (size_t)ya * src->w, *r1 = src->px + (size_t)yb * src->w;

    int sl = 1, sr = 0;
    int have = solid_span(&rr, clip, y, &sl, &sr);
    int ls = 1, rs = 0;
    if (have) { ls = sl > x0 ? sl : x0; rs = sr < x1 - 1 ? sr : x1 - 1; }
    int can_copy = 0;
    if (copy1 && rs >= ls) {
      int sy = y + dyo;
      can_copy = sy >= 0 && sy < src->h && ls + dxo >= 0 && rs + dxo < src->w;
    }
    const uint32_t *crow = can_copy ? src->px + (size_t)(y + dyo) * src->w + dxo : NULL;

    int x = x0;
    while (x < x1) {
      if (x >= ls && x <= rs) {
        if (can_copy) {
          for (int i = x; i <= rs; i++) row[i] = crow[i] & 0xffffff;
          x = rs + 1;
          continue;
        }
        int i = x - x0;
        row[x] = bilerp(r0, r1, di_xa[i], di_xb[i], di_tx[i], ty);
        x++;
        continue;
      }
      float cov = rr_cov(&rr, x + 0.5f, y + 0.5f);
      if (cov > 0 && clip) cov *= rr_cov(clip, x + 0.5f, y + 0.5f);
      unsigned a = (unsigned)(cov * 256 + 0.5f);
      if (a) {
        int i = x - x0;
        row[x] = mix32(row[x], bilerp(r0, r1, di_xa[i], di_xb[i], di_tx[i], ty), a);
      }
      x++;
    }
  }
}

/* ------------------------------------------------------------------------ */
/* X GLOBALS & FONTS                                                        */
/* ------------------------------------------------------------------------ */
static Display *dpy;
static int scr, W, H, depth;
static Window root, shell;
static GC gc;
static double UI = 1.0;
#define S(x) ((x) * UI)

typedef struct { int w, h, lb, asc; float adv; uint8_t *bits; uint8_t *cov; int cw, ch; } Glyph;
typedef struct { XFontStruct *xf; double ss; Glyph g[NGLYPH]; } AppFont;
static AppFont F_lbl, F_card, F_hdr, F_hud, F_body, F_big;

static pid_t kbd_pid = 0;
static int kbd_active = 0;
static Window kbd_win = None;

static int glyph_index(uint32_t cp) {
  if (cp >= 32 && cp <= 126) return (int)cp - 32;
  if (cp == 0x2192) return 95;
  return '?' - 32;
}
static uint32_t utf8_next(const char **p) {
  const unsigned char *s = (const unsigned char *)*p;
  if (s[0] < 0x80) { (*p)++; return s[0]; }
  if ((s[0] & 0xe0) == 0xc0 && (s[1] & 0xc0) == 0x80) { *p += 2; return ((s[0] & 0x1f) << 6) | (s[1] & 0x3f); }
  if ((s[0] & 0xf0) == 0xe0 && (s[1] & 0xc0) == 0x80 && (s[2] & 0xc0) == 0x80) {
    *p += 3; return ((s[0] & 0x0f) << 12) | ((s[1] & 0x3f) << 6) | (s[2] & 0x3f);
  }
  (*p)++; return '?';
}

static XFontStruct *load_any_font(int px, int bold) {
  static const char *fam[] = { "dejavu sans", "liberation sans", "helvetica", "lucida", "arial", NULL };
  static const char *reg[] = { "iso10646-1", "iso8859-1", NULL };
  char pat[256];
  const char *wt = bold ? "bold" : "medium";
  for (int f = 0; fam[f]; f++)
    for (int r = 0; reg[r]; r++) {
      snprintf(pat, sizeof pat, "-*-%s-%s-r-normal-*-%d-*-*-*-*-*-%s", fam[f], wt, px, reg[r]);
      XFontStruct *xf = XLoadQueryFont(dpy, pat);
      if (xf) return xf;
    }
  snprintf(pat, sizeof pat, "-*-*-%s-r-normal-*-%d-*-*-*-p-*-iso8859-1", wt, px);
  XFontStruct *xf = XLoadQueryFont(dpy, pat);
  if (xf) return xf;
  return XLoadQueryFont(dpy, "fixed");
}

/* Pre-render a glyph's 1-bit supersampled bitmap into 8-bit coverage at destination
   scale k (pixel-aligned origin). Same box filter draw_text used to evaluate per pixel. */
static void glyph_bake(Glyph *g, double k) {
  g->cw = (int)ceil(g->w * k); g->ch = (int)ceil(g->h * k);
  if (g->cw <= 0 || g->ch <= 0) { g->cw = g->ch = 0; return; }
  g->cov = calloc((size_t)g->cw * g->ch, 1);
  if (!g->cov) { g->cw = g->ch = 0; return; }
  for (int iy = 0; iy < g->ch; iy++) {
    int b0 = (int)lround(iy / k), b1 = (int)lround((iy + 1) / k);
    if (b1 <= b0) b1 = b0 + 1;
    for (int ix = 0; ix < g->cw; ix++) {
      int a0 = (int)lround(ix / k), a1 = (int)lround((ix + 1) / k);
      if (a1 <= a0) a1 = a0 + 1;
      int cnt = 0, tot = (a1 - a0) * (b1 - b0);
      for (int sy = b0; sy < b1; sy++) {
        if (sy < 0 || sy >= g->h) continue;
        const uint8_t *row = g->bits + (size_t)sy * g->w;
        for (int sx = a0; sx < a1; sx++) if (sx >= 0 && sx < g->w) cnt += row[sx];
      }
      g->cov[(size_t)iy * g->cw + ix] = (uint8_t)((cnt * 255 + tot / 2) / tot);
    }
  }
}

static void font_build(AppFont *f, double dest_px, int bold) {
  int SS = dest_px >= 24 ? 3 : 4;
  int src = (int)lround(dest_px * SS);
  if (src < 8) src = 8;
  f->xf = load_any_font(src, bold);
  if (!f->xf) { fprintf(stderr, "summerboard: no usable X font\n"); exit(1); }
  f->ss = (f->xf->ascent + f->xf->descent) / (dest_px * 1.164);
  if (f->ss < 0.5) f->ss = 0.5;
  Pixmap dummy = XCreatePixmap(dpy, root, 1, 1, 1);
  GC g1 = XCreateGC(dpy, dummy, 0, NULL);
  XSetFont(dpy, g1, f->xf->fid);
  for (int i = 0; i < NGLYPH; i++) {
    uint32_t cp = i < 95 ? (uint32_t)(i + 32) : 0x2192;
    XChar2b c2 = { (unsigned char)(cp >> 8), (unsigned char)(cp & 255) };
    int dir, fa, fd;
    XCharStruct ov;
    XTextExtents16(f->xf, &c2, 1, &dir, &fa, &fd, &ov);
    if (i == 95 && ov.width == 0 && ov.ascent + ov.descent == 0) {
      f->g[95] = f->g['>' - 32];
      continue;
    }
    Glyph *g = &f->g[i];
    g->adv = ov.width; g->lb = ov.lbearing; g->asc = ov.ascent;
    g->w = ov.rbearing - ov.lbearing; g->h = ov.ascent + ov.descent; g->bits = NULL;
    if (g->w <= 0 || g->h <= 0 || cp == ' ') { g->w = g->h = 0; continue; }
    Pixmap pm = XCreatePixmap(dpy, root, g->w, g->h, 1);
    XSetForeground(dpy, g1, 0); XFillRectangle(dpy, pm, g1, 0, 0, g->w, g->h);
    XSetForeground(dpy, g1, 1); XDrawString16(dpy, pm, g1, -ov.lbearing, ov.ascent, &c2, 1);
    XImage *im = XGetImage(dpy, pm, 0, 0, g->w, g->h, 1UL, XYPixmap);
    if (im) {
      g->bits = malloc((size_t)g->w * g->h);
      for (int y = 0; y < g->h; y++)
        for (int x = 0; x < g->w; x++) g->bits[(size_t)y * g->w + x] = XGetPixel(im, x, y) ? 1 : 0;
      XDestroyImage(im);
      glyph_bake(g, 1.0 / f->ss);
    }
    XFreePixmap(dpy, pm);
  }
  XFreeGC(dpy, g1); XFreePixmap(dpy, dummy);
}

static double text_width(const AppFont *f, const char *s, double scale, double ls) {
  double w = 0;
  while (*s) { uint32_t cp = utf8_next(&s); w += f->g[glyph_index(cp)].adv / f->ss * scale + ls; }
  return w;
}
static void draw_text(Surf *t, const AppFont *f, const char *s, double x, double y, double scale, int align,
                      uint32_t col, double alpha, double ls, const RR *clip) {
  double k = scale / f->ss;
  double tw = text_width(f, s, scale, ls);
  double pen = align == 1 ? x - tw / 2 : (align == 2 ? x - tw : x);
  if (alpha < 0) alpha = 0;
  unsigned ac = (unsigned)(alpha * 256 + 0.5);
  if (ac > 256) ac = 256;
  if (!ac) return;
  const int baked = fabs(scale - 1.0) < 1e-6;   /* the common case: use pre-baked coverage */
  const double inv_k = 1.0 / k;
  int ca0[256], ca1[256];

  while (*s) {
    uint32_t cp = utf8_next(&s);
    const Glyph *g = &f->g[glyph_index(cp)];
    if (g->bits && g->w > 0) {
      double gl = pen + g->lb * k, gt = y - g->asc * k;
      if (baked && g->cov) {
        /* Blit: glyph origin snapped to whole pixels. */
        int gx = (int)lround(gl), gy = (int)lround(gt);
        int cx0 = gx < 0 ? -gx : 0, cy0 = gy < 0 ? -gy : 0;
        int cx1 = g->cw < t->w - gx ? g->cw : t->w - gx;
        int cy1 = g->ch < t->h - gy ? g->ch : t->h - gy;
        for (int cy = cy0; cy < cy1; cy++) {
          const uint8_t *c = g->cov + (size_t)cy * g->cw;
          uint32_t *p = t->px + (size_t)(gy + cy) * t->w + gx;
          for (int cx = cx0; cx < cx1; cx++) {
            unsigned cv = c[cx];
            if (!cv) continue;
            unsigned a = (cv * ac + 128) / 255;
            if (clip && a) a = (unsigned)(a * rr_cov(clip, gx + cx + 0.5f, gy + cy + 0.5f) + 0.5f);
            if (a) p[cx] = mix32(p[cx], col, a > 256 ? 256 : a);
          }
        }
      } else {
        /* Arbitrary scale: box-sample the bitmap, with the per-column bounds hoisted out of the row loop. */
        int x0 = (int)floor(gl), x1 = (int)ceil(gl + g->w * k), y0 = (int)floor(gt), y1 = (int)ceil(gt + g->h * k);
        if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0;
        if (x1 > t->w) x1 = t->w; if (y1 > t->h) y1 = t->h;
        if (x1 - x0 > 256) x1 = x0 + 256;
        for (int ix = x0; ix < x1; ix++) {
          int a0 = (int)lround((ix - gl) * inv_k), a1 = (int)lround((ix + 1 - gl) * inv_k);
          if (a1 <= a0) a1 = a0 + 1;
          ca0[ix - x0] = a0; ca1[ix - x0] = a1;
        }
        for (int iy = y0; iy < y1; iy++) {
          int b0 = (int)lround((iy - gt) * inv_k), b1 = (int)lround((iy + 1 - gt) * inv_k);
          if (b1 <= b0) b1 = b0 + 1;
          uint32_t *prow = t->px + (size_t)iy * t->w;
          for (int ix = x0; ix < x1; ix++) {
            int a0 = ca0[ix - x0], a1 = ca1[ix - x0];
            int cnt = 0, tot = (a1 - a0) * (b1 - b0);
            for (int sy = b0; sy < b1; sy++) {
              if (sy < 0 || sy >= g->h) continue;
              const uint8_t *row = g->bits + (size_t)sy * g->w;
              for (int sx = a0; sx < a1; sx++) if (sx >= 0 && sx < g->w) cnt += row[sx];
            }
            if (!cnt) continue;
            float cov = (float)cnt / tot;
            if (clip) cov *= rr_cov(clip, ix + 0.5f, iy + 0.5f);
            unsigned a = (unsigned)(cov * alpha * 256 + 0.5);
            if (a) prow[ix] = mix32(prow[ix], col, a > 256 ? 256 : a);
          }
        }
      }
    }
    pen += g->adv / f->ss * scale + ls;
  }
}

/* ------------------------------------------------------------------------ */
/* APPLICATION MODELS & LAYOUT                                             */
/* ------------------------------------------------------------------------ */
typedef struct App App;

struct App {
  int id;
  char name[64];
  uint32_t color;
  int state;
  Rect icon;
  int is_dock;
  int page;
  Spring press_scale;
  char cmd[256];
  Window wins[MAX_WINS];
  int nwin;
  Surf snap;
  Surf thumb;   /* card-sized pre-scaled copy of snap; rounded-corner alpha lives in the top byte */
  int is_kbd;
  int builtin;   /* drawn by the compositor itself, no X client window (e.g. Settings) */
  pid_t pid;
  /* Last geometry actually sent to each client window (for change detection / throttling). */
  Window sent_win[MAX_WINS];
  int sent_x[MAX_WINS], sent_y[MAX_WINS], sent_w[MAX_WINS], sent_h[MAX_WINS];
  double last_resize_ms;
};

static const uint32_t PALETTE[] = {
  0x34C759, 0x1C1C1E, 0x8E8E93, 0xFF2D55, 0x30B0C7, 0x30D158, 0x007AFF,
  0xFFCC00, 0x50A7EA, 0xF2F2F7, 0xFF9500, 0xFF3B30, 0x9B59B6, 0x1ABC9C
};

static App apps[MAX_APPS];
static int app_count = 0;
static Rect dock_box;

static int current_page = 0;
static int total_pages = 1;
static Spring page_offset_spring;

static void layout_grid(void);

static int bd_valid;   /* switcher backdrop cache (see render_frame); cleared when the layout changes */
static void snap_drop(App *a) { surf_free(&a->snap); surf_free(&a->thumb); }

/* ---- X error trap (used for shm attach and snapshots) ---- */
static int xerr_trap = 0;
static int xerr_handler(Display *d, XErrorEvent *e) { (void)d; (void)e; xerr_trap = 1; return 0; }

/* Permanent handler: windows owned by other clients can vanish at any time, so
   BadWindow/BadMatch etc. must not kill the window manager (Xlib's default aborts). */
static int wm_error_handler(Display *d, XErrorEvent *e) {
  char msg[128];
  XGetErrorText(d, e->error_code, msg, sizeof msg);
  DBG("ignored X error: %s (request %d, resource 0x%lx)", msg, e->request_code, e->resourceid);
  return 0;
}

/* ---- MIT-SHM double-buffered canvas ----
   Two framebuffers ping-pong: while the X server reads buffer A, we render into B, and we only
   ever wait for the buffer we are about to overwrite (normally long since finished).
   Each buffer remembers which rows the cards last covered, so in the switcher's cached-backdrop
   state we restore and present only that band. */
typedef struct {
  Surf s;
  XImage *img;
  XShmSegmentInfo si;
  int is_shm;
  int pending;            /* an XShmPutImage reading this buffer is still in flight */
  unsigned bd_gen;        /* backdrop generation this buffer's non-band rows hold    */
  int bd_ok;              /* rows outside [b0,b1) are a clean copy of bd_dim         */
  int b0, b1;             /* rows drawn over the backdrop last time we rendered here */
} FBuf;
static FBuf fb[2];
static int nbuf = 1, fb_cur = 0;
static int use_shm = 0;      /* server supports MIT-SHM and attach worked */
static int shm_ev_base = 0;
/* What the window currently shows, for dirty-rect presents. */
static int pres_band_ok = 0; static unsigned pres_gen = 0; static int pres_b0 = 0, pres_b1 = 0;
static int force_full_present = 1;   /* Expose / resize: next present must cover the whole window */

static FBuf *fb_by_seg(ShmSeg seg) {
  for (int i = 0; i < nbuf; i++) if (fb[i].is_shm && fb[i].si.shmseg == seg) return &fb[i];
  return NULL;
}
static void shm_completion(XEvent *e) {
  FBuf *f = fb_by_seg(((XShmCompletionEvent *)e)->shmseg);
  if (f) f->pending = 0;
}
static Bool is_shm_done(Display *d, XEvent *e, XPointer arg) {
  (void)d;
  return e->type == shm_ev_base + ShmCompletion && ((XShmCompletionEvent *)e)->shmseg == *(ShmSeg *)arg;
}
/* Block until the server has finished reading THIS buffer. */
static void shm_wait(FBuf *f) {
  if (!f->pending) return;
  XEvent e;
  ShmSeg seg = f->si.shmseg;
  XIfEvent(dpy, &e, is_shm_done, (XPointer)&seg);
  f->pending = 0;
}

static void fb_release(FBuf *f) {
  if (f->is_shm) {
    shm_wait(f);
    XShmDetach(dpy, &f->si);
    XSync(dpy, False);
    if (f->img) { f->img->data = NULL; XDestroyImage(f->img); }
    shmdt(f->si.shmaddr);
    f->is_shm = 0;   /* memory was shm, not malloc */
  } else {
    if (f->img) { f->img->data = NULL; XDestroyImage(f->img); }
    surf_free(&f->s);
  }
  memset(f, 0, sizeof *f);
}
static void canvas_release(void) {
  for (int i = 0; i < nbuf; i++) fb_release(&fb[i]);
  nbuf = 1; fb_cur = 0;
  pres_band_ok = 0; force_full_present = 1;
}

static int fb_alloc_shm(FBuf *f, int w, int h) {
  memset(f, 0, sizeof *f);
  f->img = XShmCreateImage(dpy, DefaultVisual(dpy, scr), depth, ZPixmap, NULL, &f->si, w, h);
  if (!f->img) return 0;
  if (f->img->bits_per_pixel != 32 || f->img->bytes_per_line != w * 4) { XDestroyImage(f->img); f->img = NULL; return 0; }

  f->si.shmid = shmget(IPC_PRIVATE, (size_t)f->img->bytes_per_line * h, IPC_CREAT | 0600);
  if (f->si.shmid < 0) { XDestroyImage(f->img); f->img = NULL; return 0; }
  f->si.shmaddr = f->img->data = shmat(f->si.shmid, NULL, 0);
  if (f->si.shmaddr == (char *)-1) {
    shmctl(f->si.shmid, IPC_RMID, NULL);
    f->img->data = NULL; XDestroyImage(f->img); f->img = NULL; return 0;
  }
  f->si.readOnly = False;

  XErrorHandler old_h = XSetErrorHandler(xerr_handler);
  xerr_trap = 0;
  XShmAttach(dpy, &f->si);
  XSync(dpy, False);
  XSetErrorHandler(old_h);
  shmctl(f->si.shmid, IPC_RMID, NULL);   /* auto-free when both sides detach */

  if (xerr_trap) {                        /* e.g. remote display */
    shmdt(f->si.shmaddr);
    f->img->data = NULL; XDestroyImage(f->img); f->img = NULL; return 0;
  }
  f->s.w = w; f->s.h = h; f->s.px = (uint32_t *)f->img->data;
  memset(f->s.px, 0, (size_t)w * h * 4);
  f->is_shm = 1;
  return 1;
}

static void realloc_canvas(int w, int h) {
  canvas_release();
  if (use_shm) {
    if (fb_alloc_shm(&fb[0], w, h)) {
      if (fb_alloc_shm(&fb[1], w, h)) { nbuf = 2; return; }
      fb_release(&fb[0]);
    }
    use_shm = 0;   /* fall back permanently */
    DBG("MIT-SHM unavailable, using XPutImage");
  }
  memset(&fb[0], 0, sizeof fb[0]);
  fb[0].s = surf_new(w, h);
  fb[0].img = XCreateImage(dpy, DefaultVisual(dpy, scr), depth, ZPixmap, 0, (char *)fb[0].s.px, w, h, 32, 0);
  nbuf = 1;
}

/* Present rows [y0,y1) of a buffer (full width). */
static void canvas_present(FBuf *f, int y0, int y1) {
  if (y1 <= y0) return;
  if (f->is_shm) {
    XShmPutImage(dpy, shell, gc, f->img, 0, y0, 0, y0, f->s.w, y1 - y0, True);
    f->pending = 1;
  } else {
    XPutImage(dpy, shell, gc, f->img, 0, y0, 0, y0, f->s.w, y1 - y0);
  }
}

static void toggle_matchbox_keyboard(void) {
  const char *target_disp = XDisplayString(dpy);
  if (!kbd_active) {
    kbd_active = 1;
    int cur_h = (int)lround(H * 0.75);
    
    XMoveResizeWindow(dpy, shell, 0, 0, W, cur_h);
    realloc_canvas(W, cur_h);

    kbd_pid = fork();
    if (kbd_pid == 0) {
      setsid();
      setenv("DISPLAY", target_disp, 1);
      execlp("matchbox-keyboard", "matchbox-keyboard", NULL);
      _exit(127);
    }
  } else {
    kbd_active = 0;
    kbd_win = None;
    if (kbd_pid > 0) {
      kill(kbd_pid, SIGTERM);
      waitpid(kbd_pid, NULL, WNOHANG);
      kbd_pid = 0;
    }

    XMoveResizeWindow(dpy, shell, 0, 0, W, H);
    realloc_canvas(W, H);
  }
  layout_grid();
}

static int is_executable(const char *cmd) {
  char binary[128] = "";
  sscanf(cmd, "%127s", binary);
  if (binary[0] == '/') return access(binary, X_OK) == 0;

  char *path_env = getenv("PATH");
  if (!path_env) return 0;
  char *path_copy = strdup(path_env);
  char *dir = strtok(path_copy, ":");
  int found = 0;
  while (dir) {
    char full_path[512];
    snprintf(full_path, sizeof(full_path), "%s/%s", dir, binary);
    if (access(full_path, X_OK) == 0) {
      found = 1;
      break;
    }
    dir = strtok(NULL, ":");
  }
  free(path_copy);
  return found;
}

static void add_app_manual(const char *name, const char *cmd, int is_kbd) {
  if (app_count >= MAX_APPS) return;
  if (!is_kbd && !is_executable(cmd)) return;

  for (int i = 0; i < app_count; i++) {
    if (strcmp(apps[i].name, name) == 0) return;
  }

  App *a = &apps[app_count];
  a->id = app_count;
  strncpy(a->name, name, sizeof(a->name) - 1);
  strncpy(a->cmd, cmd, sizeof(a->cmd) - 1);
  a->color = is_kbd ? 0xE67E22 : PALETTE[app_count % (sizeof(PALETTE) / sizeof(PALETTE[0]))];
  a->state = ST_CLOSED;
  a->is_dock = (app_count < 6);
  a->is_kbd = is_kbd;
  sp_init(&a->press_scale, 1.0, 350, 28, 0.001);

  app_count++;
}

static void add_builtin_app(const char *name, uint32_t color) {
  if (app_count >= MAX_APPS) return;
  for (int i = 0; i < app_count; i++) if (strcmp(apps[i].name, name) == 0) return;
  App *a = &apps[app_count];
  a->id = app_count;
  strncpy(a->name, name, sizeof(a->name) - 1);
  a->cmd[0] = 0;
  a->color = color;
  a->state = ST_CLOSED;
  a->is_dock = (app_count < 6);
  a->builtin = 1;
  sp_init(&a->press_scale, 1.0, 350, 28, 0.001);
  app_count++;
}

static void parse_desktop_file(const char *filepath) {
  if (app_count >= MAX_APPS) return;

  FILE *f = fopen(filepath, "r");
  if (!f) return;

  char line[512];
  char name[64] = "";
  char exec[256] = "";
  int no_display = 0;
  int in_desktop_entry = 0;

  while (fgets(line, sizeof(line), f)) {
    if (line[0] == '[') {
      if (strncmp(line, "[Desktop Entry]", 15) == 0) in_desktop_entry = 1;
      else in_desktop_entry = 0;
      continue;
    }
    if (!in_desktop_entry) continue;

    if (strncmp(line, "Name=", 5) == 0 && name[0] == '\0') {
      sscanf(line + 5, "%63[^\n]", name);
    } else if (strncmp(line, "Exec=", 5) == 0 && exec[0] == '\0') {
      sscanf(line + 5, "%255[^\n]", exec);
      char *p = strchr(exec, '%');
      if (p) *p = '\0';
    } else if (strncmp(line, "NoDisplay=true", 14) == 0) {
      no_display = 1;
    }
  }
  fclose(f);

  if (no_display || name[0] == '\0' || exec[0] == '\0') return;

  add_app_manual(name, exec, 0);
}

static void scan_dir(const char *dirpath) {
  DIR *d = opendir(dirpath);
  if (!d) return;

  struct dirent *entry;
  while ((entry = readdir(d)) != NULL) {
    if (strstr(entry->d_name, ".desktop")) {
      char full_path[512];
      snprintf(full_path, sizeof(full_path), "%s/%s", dirpath, entry->d_name);
      parse_desktop_file(full_path);
    }
  }
  closedir(d);
}

static void scan_system_apps(void) {
  add_app_manual("Keyboard", "matchbox-keyboard", 1);
  add_builtin_app("Settings", 0x8E8E93);

  scan_dir("/usr/share/applications");
  scan_dir("/usr/local/share/applications");
  scan_dir("/var/lib/flatpak/exports/share/applications");
  
  char user_apps[512];
  const char *home = getenv("HOME");
  if (home) {
    snprintf(user_apps, sizeof(user_apps), "%s/.local/share/applications", home);
    scan_dir(user_apps);
  }

  static const char *known_binaries[][2] = {
    {"Terminal", "xterm"},
    {"X Eyes", "xeyes"},
    {"Calculator", "xcalc"},
    {"Clock", "xclock"},
    {"Leafpad", "leafpad"},
    {"Geany", "geany"},
    {"Firefox", "firefox"},
    {"Chromium", "chromium-browser"},
    {"Htop", "xterm -e htop"},
    {"Top", "xterm -e top"},
    {"Cmatrix", "xterm -e cmatrix"},
    {"Glx Gears", "glxgears"}
  };

  for (size_t i = 0; i < sizeof(known_binaries)/sizeof(known_binaries[0]); i++) {
    add_app_manual(known_binaries[i][0], known_binaries[i][1], 0);
  }

  if (app_count == 0) {
    add_app_manual("Terminal", "xterm", 0);
  }
}

static void layout_grid(void) {
  bd_valid = 0;
  int eff_h = kbd_active ? (int)lround(H * 0.75) : H;
  
  // Sıkışıklığı önlemek için kenar boşluğunu daraltıp sütun boşluğunu artırdık
  double outer_pad = S(20);
  int cols = GRID_COLS;
  
  // İkon genişliğini artırarak ekran yayılımını optimize ettik
  double gap = (W - (outer_pad * 2)) * 0.10;
  if (gap < S(24)) gap = S(24);
  double row_gap = S(36);   /* vertical space between rows (includes label) */

  double icon_w = (W - (outer_pad * 2) - (gap * (cols - 1))) / cols;
  if (icon_w > S(84)) icon_w = S(84);
  {
    /* Shrink icons if needed so GRID_ROWS rows + labels + dock fit vertically. */
    double fit = (eff_h - S(45) - S(14) - S(16) - S(12) - GRID_ROWS * row_gap) / (GRID_ROWS + 1);
    if (fit > S(24) && icon_w > fit) icon_w = fit;
  }
  
  double icon_r = icon_w * 0.225;

  // Dock alanını ve yerleşimini hesapla
  int ndock = 0;
  for (int i = 0; i < app_count; i++) if (apps[i].is_dock) ndock++;
  
  double dock_h = icon_w + S(16);
  double dock_item_spacing = S(14);
  double dock_w = ndock * icon_w + (ndock + 1) * dock_item_spacing;
  if (dock_w > W - S(40)) dock_w = W - S(40);
  
  double dock_x = (W - dock_w) / 2;
  double dock_y = eff_h - dock_h - S(14);
  dock_box = (Rect){ dock_x, dock_y, dock_w, dock_h, dock_h * 0.38 };

  double dock_gap = (dock_w - ndock * icon_w) / (ndock + 1);

  int dock_idx = 0;
  for (int i = 0; i < app_count; i++) {
    App *a = &apps[i];
    if (a->is_dock) {
      a->icon = (Rect){ dock_x + dock_gap + dock_idx * (icon_w + dock_gap), dock_y + S(8), icon_w, icon_w, icon_r };
      dock_idx++;
    }
  }

  // Grid elemanları dikey olarak ideal mesafesini korur
  int main_indices[MAX_APPS], nmain = 0;
  for (int i = 0; i < app_count; i++) {
    if (!apps[i].is_dock) main_indices[nmain++] = i;
  }

  double top_off = S(45);

  for (int i = 0; i < nmain; i++) {
    App *a = &apps[main_indices[i]];
    a->page = i / APPS_PER_PAGE;
    int page_idx = i % APPS_PER_PAGE;
    int c = page_idx % cols;
    int r = page_idx / cols;

    double page_base_x = a->page * W;
    
    /* Fixed grid: every row uses the full column width, so partly filled rows stay aligned. */
    double row_w = cols * icon_w + (cols - 1) * gap;
    double grid_start_x = page_base_x + (W - row_w) / 2;

    a->icon = (Rect){ grid_start_x + c * (icon_w + gap), top_off + r * (icon_w + row_gap), icon_w, icon_w, icon_r };
  }

  total_pages = (nmain + APPS_PER_PAGE - 1) / APPS_PER_PAGE;
  if (total_pages < 1) total_pages = 1;
}

/* ------------------------------------------------------------------------ */
/* SWITCHER METRICS                                                         */
/* ------------------------------------------------------------------------ */
typedef struct { App *app; Spring vy_sp; } Card;

static App *active_app = NULL;
static RectSpring win_spring;
static Spring home_scale_spring, switcher_scroll_spring;
static struct { int open; double scroll_x; Card cards[MAX_APPS]; int ncards; struct { int active_idx; Rect stage; double t0, dur; } entry; } switcher;

typedef struct {
  char mode[16];
  double dy, start_x, start_y, px, py, entry_x, entry_y, last_x, last_y, pan_start_scroll, last_move_t, last_move_x, last_move_y, appear_t0;
  int hold_fired, grab_idx;
  char axis;
  Rect rect;
  App *app;
  double page_start_offset;
} Gesture;

static Gesture *gesture = NULL;

static const double PHI = 1.618033988749895;
#define GAP_PX S(22)
#define CARD_R_PX S(32)

static Rect full_rect(void) { 
  int eff_h = kbd_active ? (int)lround(H * 0.75) : H;
  return (Rect){ 0, 0, W, eff_h, S(0) }; 
}

static Rect icon_rect(const App *a) {
  Rect r = a->icon;
  if (!a->is_dock) {
    r.x -= page_offset_spring.x;
  }
  return r;
}

static void card_size(double *cw, double *ch) { 
  int eff_h = kbd_active ? (int)lround(H * 0.75) : H;
  *cw = W / PHI; 
  *ch = eff_h / PHI; 
}

static void switcher_metrics(double *cw, double *ch, double *left_margin, double *max_scroll) {
  card_size(cw, ch);
  int N = switcher.ncards;
  double content_w = N * (*cw) + fmax(0, N - 1) * GAP_PX;
  *left_margin = content_w < W ? (W - content_w) / 2 : GAP_PX;
  *max_scroll = fmax(0, content_w + *left_margin + GAP_PX - W);
}

static Rect card_render_box(int i) {
  double cw, ch, left_margin, max_scroll;
  switcher_metrics(&cw, &ch, &left_margin, &max_scroll);
  int eff_h = kbd_active ? (int)lround(H * 0.75) : H;
  double final_y = (eff_h - ch) / 2;
  if (switcher.entry.dur > 0) {
    double t = clampd((now_ms() - switcher.entry.t0) / switcher.entry.dur, 0, 1);
    double k = appear_ease(t);
    Rect s = switcher.entry.stage;
    double w = s.w + (cw - s.w) * k, h = s.h + (ch - s.h) * k, r = s.r + (CARD_R_PX - s.r) * k;
    double y = s.y + (final_y - s.y) * k;
    double from_x = s.x + (i - switcher.entry.active_idx) * (s.w + GAP_PX);
    double to_x = left_margin + i * (cw + GAP_PX) - switcher_scroll_spring.x;
    double x = from_x + (to_x - from_x) * k;
    if (t >= 1) switcher.entry.dur = 0;
    return (Rect){ x, y, w, h, r };
  }
  double vy = switcher.cards[i].vy_sp.x;
  return (Rect){ left_margin + i * (cw + GAP_PX) - switcher_scroll_spring.x, final_y + vy, cw, ch, CARD_R_PX };
}

static Rect live_anchor_rect(void) {
  if (gesture && (gesture->rect.w > 0 || gesture->rect.h > 0)) return gesture->rect;
  if (gesture) return (Rect){ gesture->px, gesture->py, 0, 0, 0 };
  return full_rect();
}

/* Client windows are covered by the snapshot preview while animating, so the real window only
   needs to be resized a few times per second, not every frame. Every ConfigureNotify makes the
   client re-layout and repaint, so flooding it makes the app look glitchy.
     - unchanged geometry           -> nothing sent
     - position-only change         -> XMoveWindow (client doesn't re-layout)
     - size change                  -> at most one per RESIZE_MIN_MS, latest value wins
   A throttled rect is remembered and delivered by geom_flush_pending() (trailing edge), so the
   final size is always exact. */
#define RESIZE_MIN_MS 25.0
static App *geom_pending_app = NULL;
static Rect geom_pending_rect;

static void sync_window_geometry_ex(App *a, Rect r, int force) {
  if (!a || a->nwin == 0) return;

  int eff_h = kbd_active ? (int)lround(H * 0.75) : H;
  if (r.h >= eff_h - 1) {
    r.h = eff_h - (int)lround(S(12));
  }

  int x = (int)lround(r.x), y = (int)lround(r.y);
  int w = (int)fmax(1, lround(r.w)), h = (int)fmax(1, lround(r.h));
  double now = now_ms();
  int throttled = 0;

  for (int i = 0; i < a->nwin; i++) {
    int known = (a->sent_win[i] == a->wins[i]);
    if (known && a->sent_x[i] == x && a->sent_y[i] == y && a->sent_w[i] == w && a->sent_h[i] == h)
      continue;                                   /* nothing changed */

    int size_changed = !known || a->sent_w[i] != w || a->sent_h[i] != h;
    if (size_changed && !force && known && now - a->last_resize_ms < RESIZE_MIN_MS) {
      throttled = 1;                              /* too soon: keep the last value for the trailing flush */
      if (a->sent_x[i] != x || a->sent_y[i] != y) {
        XMoveWindow(dpy, a->wins[i], x, y);       /* position is cheap, keep it tracking */
        a->sent_x[i] = x; a->sent_y[i] = y;
      }
      continue;
    }

    if (size_changed) XMoveResizeWindow(dpy, a->wins[i], x, y, (unsigned)w, (unsigned)h);
    else              XMoveWindow(dpy, a->wins[i], x, y);
    a->sent_win[i] = a->wins[i];
    a->sent_x[i] = x; a->sent_y[i] = y; a->sent_w[i] = w; a->sent_h[i] = h;
    if (size_changed) a->last_resize_ms = now;
  }

  if (throttled) { geom_pending_app = a; geom_pending_rect = r; }
  else if (geom_pending_app == a) geom_pending_app = NULL;
}

static void sync_window_geometry(App *a, Rect r) { sync_window_geometry_ex(a, r, 0); }

/* Called from the main loop: deliver the last throttled size once the interval has passed. */
static int geom_flush_pending(void) {
  if (!geom_pending_app) return 0;
  if (now_ms() - geom_pending_app->last_resize_ms < RESIZE_MIN_MS) return 1;
  App *a = geom_pending_app;
  geom_pending_app = NULL;
  sync_window_geometry_ex(a, geom_pending_rect, 1);
  return 0;
}

static void grab_snapshot(App *a);

/* Built-in (compositor-drawn) app hooks; defined next to the Settings code below. */
static void builtin_enter(App *a, int fresh);
static void builtin_leave(App *a);
static void builtin_close(App *a);

static void app_spawn(App *a) {
  if (!a->cmd[0]) return;
  
  const char *target_disp = XDisplayString(dpy);

  a->pid = fork();
  if (a->pid == 0) {
    setsid();
    setenv("DISPLAY", target_disp, 1);

    char full_shell_cmd[512];
    snprintf(full_shell_cmd, sizeof(full_shell_cmd), "DISPLAY=%s exec %s", target_disp, a->cmd);

    execlp("sh", "sh", "-c", full_shell_cmd, NULL);
    _exit(127);
  }
}

static void app_raise(App *a) {
  for (int i = 0; i < a->nwin; i++) {
    XMapWindow(dpy, a->wins[i]);
    XRaiseWindow(dpy, a->wins[i]);
  }
  XSetInputFocus(dpy, a->nwin > 0 ? a->wins[a->nwin - 1] : shell, RevertToPointerRoot, CurrentTime);
}

static void app_lower(App *a) {
  for (int i = 0; i < a->nwin; i++) XUnmapWindow(dpy, a->wins[i]);
}

static void launch(App *a) {
  if (a->is_kbd) {
    toggle_matchbox_keyboard();
    return;
  }

  int fresh = (a->state == ST_CLOSED);
  snap_drop(a);
  a->state = ST_FG;
  active_app = a;
  rs_snap(&win_spring, icon_rect(a));
  
  Rect target = full_rect();
  {
    int eff_h = kbd_active ? (int)lround(H * 0.75) : H;
    target.h = eff_h - (int)lround(S(12));
  }
  
  rs_target(&win_spring, target);
  home_scale_spring.target = 0.92;
  if (a->builtin) builtin_enter(a, fresh);
  else if (a->nwin == 0) app_spawn(a);
  else app_raise(a);
}

static void minimize_to_icon(App *a, double vx, double vy) {
  a->state = ST_BG;
  if (a->builtin) builtin_leave(a);
  grab_snapshot(a);
  app_lower(a);
  rs_target(&win_spring, icon_rect(a));
  rs_inject(&win_spring, vx * 0.15, vy * 0.15);
  home_scale_spring.target = 1.0;
  active_app = NULL;
  if (gesture) { free(gesture); gesture = NULL; }
}

/* ---------------------------------------------------------------------- */
/* PID-BASED PROCESS TREE KILLING                                          */
/* ---------------------------------------------------------------------- */
#define KT_MAX 512

typedef struct { pid_t pid; unsigned long long start; } KtProc;

/* Read ppid, state and starttime from /proc/<pid>/stat.
   comm can contain spaces and ')' so parse from the LAST ')'. */
static int kt_stat(pid_t pid, pid_t *ppid, char *state, unsigned long long *start) {
  char path[64], buf[1024];
  snprintf(path, sizeof path, "/proc/%d/stat", (int)pid);
  FILE *f = fopen(path, "r");
  if (!f) return -1;
  size_t n = fread(buf, 1, sizeof buf - 1, f);
  fclose(f);
  buf[n] = 0;
  char *p = strrchr(buf, ')');
  if (!p) return -1;
  char st; int pp;
  unsigned long long st_time = 0;
  /* after ')': state ppid pgrp session tty tpgid flags minflt cminflt majflt
     cmajflt utime stime cutime cstime priority nice threads itrealvalue starttime */
  if (sscanf(p + 2, "%c %d %*d %*d %*d %*d %*u %*u %*u %*u %*u %*u %*u %*d %*d %*d %*d %*d %*d %llu",
             &st, &pp, &st_time) < 3) return -1;
  if (state) *state = st;
  if (ppid) *ppid = pp;
  if (start) *start = st_time;
  return 0;
}

/* Alive = exists and is not a zombie. Start time guards against PID reuse. */
static int kt_alive(const KtProc *p) {
  char st; unsigned long long start;
  if (kt_stat(p->pid, NULL, &st, &start) < 0) return 0;
  if (start != p->start) return 0;
  return st != 'Z' && st != 'X';
}

/* Collect root + all descendants (BFS over /proc). Returns count. */
static int kt_collect(pid_t root, KtProc *out, int max) {
  pid_t  pids[4096], ppids[4096];
  unsigned long long starts[4096];
  int n = 0;

  DIR *d = opendir("/proc");
  if (!d) return 0;
  struct dirent *e;
  while ((e = readdir(d)) && n < 4096) {
    if (!isdigit((unsigned char)e->d_name[0])) continue;
    pid_t pid = atoi(e->d_name), pp; unsigned long long st;
    if (kt_stat(pid, &pp, NULL, &st) == 0) { pids[n] = pid; ppids[n] = pp; starts[n] = st; n++; }
  }
  closedir(d);

  int cnt = 0;
  for (int i = 0; i < n && cnt == 0; i++)
    if (pids[i] == root) { out[cnt].pid = root; out[cnt].start = starts[i]; cnt++; }
  if (cnt == 0) return 0;

  for (int head = 0; head < cnt; head++)          /* BFS: cnt grows as we go */
    for (int i = 0; i < n && cnt < max; i++)
      if (ppids[i] == out[head].pid) {
        int seen = 0;
        for (int k = 0; k < cnt; k++) if (out[k].pid == pids[i]) { seen = 1; break; }
        if (!seen) { out[cnt].pid = pids[i]; out[cnt].start = starts[i]; cnt++; }
      }
  return cnt;
}

static void kt_signal_all(const KtProc *l, int n, int sig) {
  for (int i = n - 1; i >= 0; i--)                /* children first, root last */
    if (kt_alive(&l[i])) kill(l[i].pid, sig);
}

static int kt_any_alive(const KtProc *l, int n) {
  for (int i = 0; i < n; i++) if (kt_alive(&l[i])) return 1;
  return 0;
}

/* Kill `pid` and every descendant.
   SIGTERM first, wait up to grace_ms, then SIGKILL whatever is left.
   grace_ms <= 0 means straight SIGKILL. BLOCKS up to grace_ms.
   Returns number of processes that were targeted (0 = pid was not found). */
static int kill_process_tree(pid_t pid, int grace_ms) {
  if (pid <= 1 || pid == getpid()) return 0;      /* never init / ourselves */

  KtProc list[KT_MAX];
  int n = kt_collect(pid, list, KT_MAX);
  if (n == 0) return 0;

  /* Freeze the tree so it can't fork new children while we work,
     then re-scan to catch anything spawned during the first scan. */
  kt_signal_all(list, n, SIGSTOP);
  n = kt_collect(pid, list, KT_MAX);
  kt_signal_all(list, n, SIGSTOP);

  if (grace_ms > 0) {
    kt_signal_all(list, n, SIGTERM);
    kt_signal_all(list, n, SIGCONT);              /* stopped procs only act on TERM after CONT */
    for (int waited = 0; waited < grace_ms && kt_any_alive(list, n); waited += 10)
      usleep(10000);
  }

  if (kt_any_alive(list, n)) {
    kt_signal_all(list, n, SIGKILL);              /* SIGKILL works on stopped procs */
  }
  return n;
}

/* Non-blocking wrapper for the UI thread: a short-lived helper process does the
   waiting, so animations don't stutter. The helper never touches X. */
static void kill_process_tree_async(pid_t pid, int grace_ms) {
  if (pid <= 1) return;
  pid_t h = fork();
  if (h == 0) {
    kill_process_tree(pid, grace_ms);
    _exit(0);
  }
  /* h > 0: reaped by the SIGCHLD handler below */
}

/* Reap every exited child (app roots, helpers, keyboard). Install once in main(). */
static void sigchld_reaper(int sig) {
  (void)sig;
  int saved = errno;
  while (waitpid(-1, NULL, WNOHANG) > 0) {}
  errno = saved;
}
static void install_sigchld_reaper(void) {
  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_handler = sigchld_reaper;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
  sigaction(SIGCHLD, &sa, NULL);
}

/* ------------------------------------------------------------------------ */
/* BUILT-IN APP: SETTINGS  (Wi-Fi prototype)                                */
/*                                                                          */
/* Drawn by the compositor itself (no X client window), so it gets the same */
/* open/close/switcher animations as everything else. Wi-Fi is driven by    */
/* `iw` (scanning) + `wpa_supplicant` (joining; saved networks live in its  */
/* config file, no wpa_cli needed) + `ip` and a DHCP client, driven by      */
/* small sh scripts run as short-lived  */
/* children whose output is collected without ever blocking the render      */
/* loop. No NetworkManager needed. Without those tools (or with -m) a demo  */
/* backend with fake networks is used. `-i wlan0` forces the interface.     */
/* ------------------------------------------------------------------------ */
#define MAX_NETS   32
#define MAX_SAVED  48
#define MAX_HITS   96
#define WJ_QLEN    8
#define WJ_TIMEOUT_MS 45000.0

enum { WJ_NONE, WJ_RADIO, WJ_TOGGLE, WJ_SCAN, WJ_SAVED, WJ_CONNECT, WJ_DISCONNECT, WJ_FORGET };
enum { PG_ROOT, PG_WIFI };
enum { SH_NONE, SH_PASSWORD, SH_DETAILS };
enum { H_NONE, H_BACK, H_SCAN, H_TOGGLE, H_ROOT_WIFI, H_NET, H_INFO, H_PW_FIELD, H_PW_SHOW,
       H_PW_CANCEL, H_PW_JOIN, H_D_DISCONNECT, H_D_FORGET, H_D_CLOSE, H_DIM };

#define UI_BG   0x110e1e
#define UI_CARD 0x1e1a2e
#define UI_HI   0x2c2745
#define UI_TXT  0xf2eefc
#define UI_SUB  0x9d94b8
#define UI_ACC  0x0a84ff
#define UI_GRN  0x34c759
#define UI_RED  0xff453a
#define BL(cy, px) ((cy) + (px) * 0.34)   /* text baseline that vertically centres a font of `px` on cy */

typedef struct { char ssid[64]; char sec[32]; int signal, active, saved; } WNet;
typedef struct { int kind, arg, quiet, nid; char ssid[64]; char sec[32]; char pw[80]; } WReq;
typedef struct { Rect r; int id, arg; } Hit;

static int opt_mock_wifi = 0;
static char opt_wifi_if[32] = "";
static char wifi_missing[24] = "";   /* first missing tool, when falling back to demo data */   /* -i: force the wireless interface name */

static struct {
  int page;
  Spring nav, scroll, tog, sheet;
  int sheet_kind, sheet_open;
  int radio_known, radio_on, scanned, mock;
  WNet nets[MAX_NETS]; int nnets;
  char saved[MAX_SAVED][64]; int saved_id[MAX_SAVED]; int nsaved;   /* wpa_supplicant network ids */
  char connecting[64], active_ssid[64];
  char toast[96]; uint32_t toast_col; double toast_t0, toast_until;
  struct { int kind; pid_t pid; int fd; size_t len; double t0, mock_end; WReq req; char buf[8192]; } job;
  WReq q[WJ_QLEN]; int qn;
  char pw[80]; int pw_len, pw_show, opened_kbd;
  WNet sel;                      /* network the open sheet is about */
  Hit hits[MAX_HITS]; int nhits; int rec;
  int press_id, press_arg, dragging;
  double press_x, press_y, press_scroll, max_scroll;
} st;

/* ---- demo backend ------------------------------------------------------- */
static const struct { const char *ssid, *sec, *pw; int sig; } MOCK_NETS[] = {
  { "SummerHome",   "WPA2",      "summer2026", 88 },
  { "Kafe_Misafir", "",          "",           71 },
  { "Ev_WiFi_5G",   "WPA2",      "12345678",   64 },
  { "TurkNet-2F9A", "WPA2 WPA3", "istanbul34", 47 },
  { "Office-Guest", "WPA2",      "guest1234",  33 },
  { "linksys",      "WEP",       "abcde",      18 },
};
#define N_MOCK ((int)(sizeof MOCK_NETS / sizeof MOCK_NETS[0]))
static int mock_saved[N_MOCK], mock_active = -1, mock_radio = 1;

static int mock_find(const char *ssid) {
  for (int i = 0; i < N_MOCK; i++) if (!strcmp(MOCK_NETS[i].ssid, ssid)) return i;
  return -1;
}

static void ssid_hex(const char *s, char *out, size_t n);

static void mock_output(const WReq *q, char *out, size_t n) {
  out[0] = 0;
  size_t l = 0;
  int idx = mock_find(q->ssid);
  switch (q->kind) {
    case WJ_RADIO: snprintf(out, n, "%s\n", mock_radio ? "enabled" : "disabled"); break;
    case WJ_TOGGLE: mock_radio = q->arg; if (!mock_radio) mock_active = -1; break;
    case WJ_SCAN:
      if (!mock_radio) break;
      for (int i = 0; i < N_MOCK; i++) {
        int sg = MOCK_NETS[i].sig + (int)lround(3 * sin(now_ms() / 900.0 + i));
        l += snprintf(out + l, n - l, "%s\t%d\t%s\t%s\n", i == mock_active ? "*" : " ",
                      (int)clampd(sg, 1, 100), MOCK_NETS[i].sec[0] ? MOCK_NETS[i].sec : "--", MOCK_NETS[i].ssid);
      }
      break;
    case WJ_SAVED:
      for (int i = 0; i < N_MOCK; i++)
        if (mock_saved[i]) { char hx[160]; ssid_hex(MOCK_NETS[i].ssid, hx, sizeof hx); l += snprintf(out + l, n - l, "%d\t%s\t\n", i, hx); }
      break;
    case WJ_CONNECT:
      if (idx < 0) snprintf(out, n, "NOTFOUND\n");
      else if (!MOCK_NETS[idx].sec[0] || mock_saved[idx] || !strcmp(q->pw, MOCK_NETS[idx].pw)) {
        mock_active = idx; mock_saved[idx] = 1;
        snprintf(out, n, "NID=%d\nOK\n", idx);
      } else snprintf(out, n, "NID=%d\nAUTH\n", idx);
      break;
    case WJ_DISCONNECT: if (idx >= 0 && idx == mock_active) mock_active = -1; break;
    case WJ_FORGET: if (idx >= 0) { mock_saved[idx] = 0; if (idx == mock_active) mock_active = -1; } break;
  }
}

/* ---- background jobs ------------------------------------------------------ */
static void toast(uint32_t col, const char *fmt, ...) {
  va_list ap; va_start(ap, fmt);
  vsnprintf(st.toast, sizeof st.toast, fmt, ap);
  va_end(ap);
  st.toast_col = col; st.toast_t0 = now_ms(); st.toast_until = st.toast_t0 + 2800;
}

static void wj_queue(int kind, const char *ssid, const char *pw, int arg, int quiet) {
  if (kind == WJ_SCAN || kind == WJ_RADIO || kind == WJ_SAVED)
    for (int i = 0; i < st.qn; i++) if (st.q[i].kind == kind) return;
  if (st.qn >= WJ_QLEN) return;
  WReq *r = &st.q[st.qn++];
  memset(r, 0, sizeof *r);
  r->kind = kind; r->arg = arg; r->quiet = quiet; r->nid = -1;
  if (ssid) snprintf(r->ssid, sizeof r->ssid, "%s", ssid);
  if (pw) snprintf(r->pw, sizeof r->pw, "%s", pw);
  if (ssid && ssid[0]) {
    for (int j = 0; j < st.nsaved; j++) if (!strcmp(st.saved[j], ssid)) { r->nid = st.saved_id[j]; break; }
    for (int j = 0; j < st.nnets; j++)
      if (!strcmp(st.nets[j].ssid, ssid)) {
        size_t l = strnlen(st.nets[j].sec, sizeof r->sec - 1);
        memcpy(r->sec, st.nets[j].sec, l); r->sec[l] = 0;
        break;
      }
  }
  if (kind == WJ_CONNECT && pw && pw[0]) r->nid = -1;   /* a typed password always makes a fresh profile */
  if (kind == WJ_FORGET && ssid && st.active_ssid[0] && !strcmp(ssid, st.active_ssid)) r->arg = 1;   /* also disconnect */
}

/* Forget a profile by its wpa_supplicant id (used for profiles that never reached our saved list). */
static void saved_drop(const char *ssid) {
  for (int j = 0; j < st.nsaved; j++)
    if (!strcmp(st.saved[j], ssid)) {
      memmove(&st.saved[j], &st.saved[j + 1], (st.nsaved - j - 1) * sizeof st.saved[0]);
      memmove(&st.saved_id[j], &st.saved_id[j + 1], (st.nsaved - j - 1) * sizeof st.saved_id[0]);
      st.nsaved--; j--;
    }
}
static void wj_forget_nid(const char *ssid, int nid) {
  if (nid < 0) return;
  wj_queue(WJ_FORGET, ssid, NULL, 0, 1);
  for (int i = st.qn - 1; i >= 0; i--)
    if (st.q[i].kind == WJ_FORGET && !strcmp(st.q[i].ssid, ssid)) { st.q[i].nid = nid; break; }
  saved_drop(ssid);
  for (int i = 0; i < st.nnets; i++) if (!strcmp(st.nets[i].ssid, ssid)) st.nets[i].saved = 0;
}

static void wj_cancel_scan(void) {
  for (int i = 0; i < st.qn; i++)
    if (st.q[i].kind == WJ_SCAN) { memmove(&st.q[i], &st.q[i + 1], (st.qn - i - 1) * sizeof(WReq)); st.qn--; i--; }
  if (st.job.kind != WJ_SCAN) return;
  if (st.job.pid > 0) { kill(-st.job.pid, SIGTERM); kill(st.job.pid, SIGTERM); }
  if (st.job.fd >= 0) close(st.job.fd);
  st.job.fd = -1; st.job.pid = 0; st.job.kind = WJ_NONE;
}


static const char WSH_PRE[] =
  "[ -e /tmp/summerboard-trace ] && set -x\n"
  "export PATH=\"/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin:$PATH\"\n"
  "IF=\"$SB_IF\"\n"
  "if [ -z \"$IF\" ]; then\n"
  "  for d in /sys/class/net/*/wireless /sys/class/net/*/phy80211; do\n"
  "    if [ -e \"$d\" ]; then IF=\"${d#/sys/class/net/}\"; IF=\"${IF%%/*}\"; break; fi\n"
  "  done\n"
  "fi\n"
  "[ -n \"$IF\" ] || { echo NODEV; exit 0; }\n"
  "CONF=\"${SB_WPA_CONF:-/etc/wpa_supplicant/wpa_supplicant.conf}\"\n"
  "RUNCONF=/tmp/summerboard-wpa-run.conf\n"
  "WLOG=/tmp/summerboard-wpa.log\n"
  "LOG=/tmp/summerboard-wifi.log\n"
  "OFF=/tmp/summerboard-wifi-off\n"
  "PIDF=\"/var/run/summerboard-dhcp.$IF.pid\"\n"
  "DSCRIPT=/tmp/summerboard-dhcp.script\n"
  "radio_on() {\n"
  "  ip link show \"$IF\" 2>/dev/null | head -n 1 | grep -q '[<,]UP[,>]' || return 1\n"
  "  rfkill list wifi 2>/dev/null | grep -qi 'soft blocked: yes' && return 1\n"
  "  return 0\n"
  "}\n"
  "have_ip() { ip -4 addr show dev \"$IF\" 2>/dev/null | grep -q 'inet '; }\n"
  "linked() { iw dev \"$IF\" link 2>/dev/null | grep -q '^Connected'; }\n"
  "wpa_pids() {\n"
  "  for d in /proc/[0-9]*; do\n"
  "    c=\"\"; { read -r c < \"$d/comm\"; } 2>/dev/null\n"
  "    [ \"$c\" = wpa_supplicant ] && echo \"${d#/proc/}\"\n"
  "  done\n"
  "  return 0\n"
  "}\n"
  "wpa_running() { [ -n \"$(wpa_pids)\" ]; }\n"
  "kill_wpa() {\n"
  "  for p in $(wpa_pids); do kill \"$p\" 2>/dev/null; done\n"
  "  n=0; while wpa_running && [ \"$n\" -lt 5 ]; do n=$((n+1)); sleep 1; done\n"
  "  for p in $(wpa_pids); do kill -9 \"$p\" 2>/dev/null; done\n"
  "  rm -f \"/var/run/wpa_supplicant/$IF\" 2>/dev/null\n"
  "  return 0\n"
  "}\n"
  "start_wpa() {\n"
  "  : > \"$WLOG\"\n"
  "  for D in nl80211 wext; do\n"
  "    if command -v setsid >/dev/null 2>&1; then\n"
  "      setsid wpa_supplicant -i \"$IF\" -c \"$1\" -D \"$D\" </dev/null >>\"$WLOG\" 2>&1 &\n"
  "    else\n"
  "      wpa_supplicant -i \"$IF\" -c \"$1\" -D \"$D\" </dev/null >>\"$WLOG\" 2>&1 &\n"
  "    fi\n"
  "    sleep 1\n"
  "    wpa_running && return 0\n"
  "    echo \"wpa_supplicant -D $D did not stay running:\" >> \"$LOG\"; tail -n 5 \"$WLOG\" >> \"$LOG\" 2>/dev/null\n"
  "  done\n"
  "  return 1\n"
  "}\n"
  "# conf_awk <list|get|drop|header> [ssid-hex]  <  wpa_supplicant.conf\n"
  "conf_awk() {\n"
  "  awk -v mode=\"$1\" -v h=\"$2\" '\n"
  "  function tohex(s,   i, r) { r = \"\"; for (i = 1; i <= length(s); i++) r = r sprintf(\"%02x\", ord[substr(s, i, 1)]); return r }\n"
  "  BEGIN { for (i = 1; i < 256; i++) ord[sprintf(\"%c\", i)] = i; inb = 0; n = 0 }\n"
  "  /^[ \\t]*network[ \\t]*=[ \\t]*[{]/ { inb = 1; blk = $0 \"\\n\"; bh = \"\"; next }\n"
  "  inb {\n"
  "    blk = blk $0 \"\\n\"\n"
  "    if ($0 ~ /^[ \\t]*ssid[ \\t]*=/) {\n"
  "      v = $0; sub(/^[ \\t]*ssid[ \\t]*=[ \\t]*/, \"\", v); sub(/[ \\t\\r]+$/, \"\", v)\n"
  "      if (v ~ /^\"/) { sub(/^\"/, \"\", v); sub(/\"$/, \"\", v); bh = tohex(v) } else bh = tolower(v)\n"
  "    }\n"
  "    if ($0 ~ /^[ \\t]*[}]/) {\n"
  "      inb = 0\n"
  "      if (mode == \"list\") printf \"%d\\t%s\\t\\n\", n, bh\n"
  "      else if (mode == \"get\" && bh == h) printf \"%s\", blk\n"
  "      else if (mode == \"drop\" && bh != h) printf \"%s\", blk\n"
  "      n++\n"
  "    }\n"
  "    next\n"
  "  }\n"
  "  { if (mode == \"header\" || mode == \"drop\") print }'\n"
  "}\n"
  "conf_has_nets() { [ -f \"$CONF\" ] && conf_awk list < \"$CONF\" | grep -q .; }\n"
  "ensure_conf() {\n"
  "  [ -f \"$CONF\" ] && return 0\n"
  "  mkdir -p \"$(dirname \"$CONF\")\" 2>/dev/null\n"
  "  printf 'ctrl_interface=/var/run/wpa_supplicant\\nupdate_config=1\\n' > \"$CONF\"\n"
  "  chmod 600 \"$CONF\" 2>/dev/null\n"
  "}\n"
  "dhcp_running() { [ -f \"$PIDF\" ] && kill -0 \"$(cat \"$PIDF\")\" 2>/dev/null; }\n"
  "kill_dhcp() {\n"
  "  [ -f \"$PIDF\" ] && kill \"$(cat \"$PIDF\")\" 2>/dev/null\n"
  "  rm -f \"$PIDF\"\n"
  "}\n"
  "write_dscript() {\n"
  "  cat > \"$DSCRIPT\" <<'DSEOF'\n"
  "#!/bin/sh\n"
  "case \"$1\" in\n"
  "  bound|renew)\n"
  "    ip addr flush dev \"$interface\" 2>/dev/null\n"
  "    ip addr add \"$ip/${mask:-24}\" ${broadcast:+broadcast \"$broadcast\"} dev \"$interface\"\n"
  "    if [ -n \"$router\" ]; then\n"
  "      ip route del default dev \"$interface\" 2>/dev/null\n"
  "      for r in $router; do ip route add default via \"$r\" dev \"$interface\"; break; done\n"
  "    fi\n"
  "    if [ -n \"$dns\" ]; then\n"
  "      : > /tmp/summerboard-resolv.conf\n"
  "      for d in $dns; do echo \"nameserver $d\" >> /tmp/summerboard-resolv.conf; done\n"
  "      cat /tmp/summerboard-resolv.conf > /etc/resolv.conf 2>/dev/null\n"
  "    fi ;;\n"
  "  deconfig) ip addr flush dev \"$interface\" 2>/dev/null ;;\n"
  "esac\n"
  "exit 0\n"
  "DSEOF\n"
  "  chmod +x \"$DSCRIPT\"\n"
  "}\n"
  "# returns 2 when no DHCP client exists at all\n"
  "run_dhcp() {\n"
  "  echo \"--- dhcp on $IF $(date 2>/dev/null)\" >> \"$LOG\"\n"
  "  if command -v dhclient >/dev/null 2>&1; then\n"
  "    dhclient -1 -4 -pf \"$PIDF\" \"$IF\" >>\"$LOG\" 2>&1\n"
  "  elif command -v udhcpc >/dev/null 2>&1; then\n"
  "    S=\"\"\n"
  "    if [ -x /usr/share/udhcpc/default.script ] || [ -x /etc/udhcpc/default.script ] || [ -x /etc/udhcpc.script ]; then :; else write_dscript; S=\"-s $DSCRIPT\"; fi\n"
  "    udhcpc -i \"$IF\" -n -t 5 -T 2 -p \"$PIDF\" $S >>\"$LOG\" 2>&1\n"
  "  elif command -v dhcpcd >/dev/null 2>&1; then\n"
  "    dhcpcd -4 -w \"$IF\" >>\"$LOG\" 2>&1\n"
  "  else\n"
  "    echo \"no DHCP client found (dhclient/udhcpc/dhcpcd)\" >> \"$LOG\"\n"
  "    return 2\n"
  "  fi\n"
  "  return 0\n"
  "}\n"
  "pskq() {\n"
  "  if [ \"${#1}\" -eq 64 ] && ! printf '%s' \"$1\" | grep -q '[^0-9a-fA-F]'; then printf '%s' \"$1\"; else printf '\"%s\"' \"$1\"; fi\n"
  "}\n"
  "wepq() {\n"
  "  case \"${#1}\" in\n"
  "    10|26) if ! printf '%s' \"$1\" | grep -q '[^0-9a-fA-F]'; then printf '%s' \"$1\"; return; fi ;;\n"
  "  esac\n"
  "  printf '\"%s\"' \"$1\"\n"
  "}\n";
static const char WSH_RADIO[] =
  "if radio_on; then echo enabled; else echo disabled; fi\n";
static const char WSH_TOGGLE[] =
  "if [ \"$SB_ARG\" = 1 ]; then\n"
  "  rfkill unblock wifi 2>/dev/null\n"
  "  ip link set \"$IF\" up 2>/dev/null\n"
  "  rm -f \"$OFF\"\n"
  "  ensure_conf\n"
  "  wpa_running || start_wpa \"$CONF\"\n"
  "else\n"
  "  kill_dhcp\n"
  "  kill_wpa\n"
  "  ip addr flush dev \"$IF\" 2>/dev/null\n"
  "  ip link set \"$IF\" down 2>/dev/null\n"
  "fi\n"
  "echo done\n";
static const char WSH_SCAN[] =
  "radio_on || { echo DOWN; exit 0; }\n"
  "if [ ! -f \"$OFF\" ] && ! wpa_running && conf_has_nets; then start_wpa \"$CONF\"; fi\n"
  "n=0; out=\"\"\n"
  "while [ \"$n\" -lt 4 ]; do\n"
  "  out=$(iw dev \"$IF\" scan 2>/dev/null) && break\n"
  "  out=$(iw dev \"$IF\" scan dump 2>/dev/null) && [ -n \"$out\" ] && break\n"
  "  n=$((n+1)); sleep 1\n"
  "done\n"
  "printf '%s\\n' \"$out\" | awk '\n"
  "function flush(   s, w2, w3, e) {\n"
  "  if (!have) return\n"
  "  if (rsn) {\n"
  "    w2 = (akm ~ /PSK/); w3 = (akm ~ /SAE/); e = (akm ~ /802\\.1X/)\n"
  "    if (e && !w2 && !w3) s = \"802.1X\"\n"
  "    else if (w2 && w3) s = \"WPA2 WPA3\"\n"
  "    else if (w3) s = \"WPA3\"\n"
  "    else s = \"WPA2\"\n"
  "  } else if (wpa) s = \"WPA\"\n"
  "  else if (priv) s = \"WEP\"\n"
  "  else s = \"--\"\n"
  "  printf \"%s\\t%d\\t%s\\t%s\\n\", act, pct, s, ssid\n"
  "  have = 0\n"
  "}\n"
  "/^BSS / { flush(); have = 1; act = ($0 ~ /-- associated/) ? \"*\" : \" \"; pct = 0; ssid = \"\"; rsn = 0; wpa = 0; priv = 0; akm = \"\"; ctx = \"\"; next }\n"
  "/^\\t[A-Za-z]/ { ctx = \"\" }\n"
  "/^\\tRSN:/ { rsn = 1; ctx = \"rsn\"; next }\n"
  "/^\\tWPA:/ { wpa = 1; ctx = \"wpa\"; next }\n"
  "/Authentication suites:/ { if (ctx == \"rsn\") akm = akm \" \" $0; next }\n"
  "/^\\tsignal:/ { v = $2; if ($0 ~ /dBm/) pct = int(2 * (v + 100) + 0.5); else { split(v, a, \"/\"); pct = a[1] + 0 } if (pct < 0) pct = 0; if (pct > 100) pct = 100; next }\n"
  "/^\\tSSID:/ { t = $0; sub(/^\\tSSID: ?/, \"\", t); ssid = t; next }\n"
  "/^\\tcapability:/ { if ($0 ~ /Privacy/) priv = 1; next }\n"
  "END { flush() }'\n"
  "# associated but never got an address (e.g. the board was joined by hand): fetch one, detached\n"
  "if linked && ! have_ip && ! dhcp_running; then\n"
  "  ( run_dhcp ) >/dev/null 2>&1 &\n"
  "fi\n";
static const char WSH_SAVED[] =
  "[ -f \"$CONF\" ] && conf_awk list < \"$CONF\"\n"
  "exit 0\n";
static const char WSH_CONNECT[] =
  "HEX=\"$SB_SSID_HEX\"\n"
  "radio_on || { rfkill unblock wifi 2>/dev/null; ip link set \"$IF\" up 2>/dev/null; }\n"
  "rm -f \"$OFF\"\n"
  "ensure_conf\n"
  "BLOCK=\"\"\n"
  "[ -z \"$SB_PW\" ] && BLOCK=$(conf_awk get \"$HEX\" < \"$CONF\")\n"
  "NEW=\"\"\n"
  "if [ -z \"$BLOCK\" ]; then\n"
  "  [ \"$SB_SEC\" = \"802.1X\" ] && { echo UNSUPPORTED; exit 0; }\n"
  "  [ \"$SB_SEC\" = \"--\" ] || [ -n \"$SB_PW\" ] || { echo NEEDPW; exit 0; }\n"
  "  NEW=1\n"
  "  case \"$SB_SEC\" in\n"
  "    \"--\"|\"\")     EXTRA=\"	key_mgmt=NONE\" ;;\n"
  "    WEP)         EXTRA=\"	key_mgmt=NONE\n"
  "	wep_key0=$(wepq \"$SB_PW\")\n"
  "	wep_tx_keyidx=0\" ;;\n"
  "    WPA3)        EXTRA=\"	key_mgmt=SAE\n"
  "	sae_password=\\\"$SB_PW\\\"\n"
  "	ieee80211w=2\" ;;\n"
  "    \"WPA2 WPA3\") EXTRA=\"	key_mgmt=WPA-PSK SAE\n"
  "	psk=$(pskq \"$SB_PW\")\n"
  "	sae_password=\\\"$SB_PW\\\"\n"
  "	ieee80211w=1\" ;;\n"
  "    *)           EXTRA=\"	key_mgmt=WPA-PSK\n"
  "	psk=$(pskq \"$SB_PW\")\" ;;\n"
  "  esac\n"
  "  BLOCK=\"network={\n"
  "	ssid=$HEX\n"
  "$EXTRA\n"
  "}\"\n"
  "  echo \"NID=0\"\n"
  "fi\n"
  "kill_dhcp\n"
  "kill_wpa\n"
  "ip addr flush dev \"$IF\" 2>/dev/null\n"
  "{ conf_awk header < \"$CONF\"; printf '%s\\n' \"$BLOCK\"; } > \"$RUNCONF\"\n"
  "chmod 600 \"$RUNCONF\" 2>/dev/null\n"
  "start_wpa \"$RUNCONF\" || { echo NOWPA; exit 0; }\n"
  "n=0; up=0; res=TIMEOUT\n"
  "while [ \"$n\" -lt 25 ]; do\n"
  "  if grep -q 'CTRL-EVENT-CONNECTED' \"$WLOG\" 2>/dev/null; then res=ASSOC; break; fi\n"
  "  if grep -Eq 'WRONG_KEY|pre-shared key may be incorrect|SSID-TEMP-DISABLED|4-Way Handshake failed' \"$WLOG\" 2>/dev/null; then res=AUTH; break; fi\n"
  "  if ! grep -Eq 'wpa_supplicant|CTRL-EVENT|nl80211|EAPOL|WPA:' \"$WLOG\" 2>/dev/null; then\n"
  "    if linked; then up=$((up+1)); [ \"$up\" -ge 4 ] && { res=ASSOC; break; }; else up=0; fi\n"
  "  fi\n"
  "  n=$((n+1)); sleep 1\n"
  "done\n"
  "rm -f \"$RUNCONF\"\n"
  "if [ \"$res\" = ASSOC ]; then\n"
  "  n=0\n"
  "  while [ \"$n\" -lt 3 ]; do have_ip && break; n=$((n+1)); sleep 1; done\n"
  "  have_ip || { run_dhcp; rc=$?; }\n"
  "  if [ -n \"$NEW\" ]; then\n"
  "    tmp=\"$CONF.sb.$$\"\n"
  "    { conf_awk drop \"$HEX\" < \"$CONF\"; printf '\\n%s\\n' \"$BLOCK\"; } > \"$tmp\" && cat \"$tmp\" > \"$CONF\"\n"
  "    rm -f \"$tmp\"\n"
  "  fi\n"
  "  if have_ip; then echo OK\n"
  "  elif [ \"$rc\" = 2 ]; then echo NODHCP\n"
  "  else echo NOIP; fi\n"
  "else\n"
  "  kill_wpa\n"
  "  start_wpa \"$CONF\"\n"
  "  echo \"$res\"\n"
  "fi\n";
static const char WSH_DISCONNECT[] =
  "touch \"$OFF\"\n"
  "kill_dhcp\n"
  "kill_wpa\n"
  "iw dev \"$IF\" disconnect 2>/dev/null\n"
  "ip addr flush dev \"$IF\" 2>/dev/null\n"
  "echo OK\n";
static const char WSH_FORGET[] =
  "[ -f \"$CONF\" ] || { echo OK; exit 0; }\n"
  "tmp=\"$CONF.sb.$$\"\n"
  "conf_awk drop \"$SB_SSID_HEX\" < \"$CONF\" > \"$tmp\" && cat \"$tmp\" > \"$CONF\"\n"
  "rm -f \"$tmp\"\n"
  "if [ \"$SB_ARG\" = 1 ]; then\n"
  "  kill_dhcp\n"
  "  kill_wpa\n"
  "  ip addr flush dev \"$IF\" 2>/dev/null\n"
  "  rm -f \"$OFF\"\n"
  "  conf_has_nets && start_wpa \"$CONF\"\n"
  "fi\n"
  "echo OK\n";
static const char *const WSH_BODY[] = {
  [WJ_RADIO] = WSH_RADIO, [WJ_TOGGLE] = WSH_TOGGLE, [WJ_SCAN] = WSH_SCAN, [WJ_SAVED] = WSH_SAVED,
  [WJ_CONNECT] = WSH_CONNECT, [WJ_DISCONNECT] = WSH_DISCONNECT, [WJ_FORGET] = WSH_FORGET,
};

/* iw / wpa_cli print SSIDs with C-style escapes (\xHH, \\, \"). Decode them to raw bytes. */
static void ssid_unescape(const char *in, char *out, size_t n) {
  size_t o = 0;
  for (const char *p = in; *p && o + 1 < n; ) {
    if (*p == '\\' && p[1]) {
      p++;
      if (*p == 'x' && isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2])) {
        char h[3] = { p[1], p[2], 0 };
        int v = (int)strtol(h, NULL, 16);
        p += 3;
        if (v == 0) break;                         /* hidden network: SSID of NULs -> empty */
        out[o++] = (char)v;
      } else {
        char c = *p++;
        out[o++] = c == 'n' ? '\n' : c == 'r' ? '\r' : c == 't' ? '\t' : c == 'e' ? 27 : c;
      }
    } else out[o++] = *p++;
  }
  out[o] = 0;
}

static void ssid_hex(const char *s, char *out, size_t n) {
  size_t o = 0;
  for (; *s && o + 3 <= n; s++) o += (size_t)snprintf(out + o, n - o, "%02x", (unsigned char)*s);
  if (n) out[o < n ? o : n - 1] = 0;
}

static int split_tab(char *line, char **out, int max) {
  int n = 0; char *p = line;
  out[n++] = p;
  while ((p = strchr(p, '\t')) && n < max) { *p++ = 0; out[n++] = p; }
  return n;
}

/* Hex string -> raw bytes (wpa_supplicant.conf stores SSIDs as hex). */
static void ssid_unhex(const char *hex, char *out, size_t n) {
  size_t o = 0;
  while (hex[0] && hex[1] && o + 1 < n) {
    if (!isxdigit((unsigned char)hex[0]) || !isxdigit((unsigned char)hex[1])) break;
    char h[3] = { hex[0], hex[1], 0 };
    int v = (int)strtol(h, NULL, 16);
    if (v == 0) break;
    out[o++] = (char)v;
    hex += 2;
  }
  out[o] = 0;
}

/* True when `tok` is exactly one whole line of the script output. */
static int has_line(const char *s, const char *tok) {
  size_t n = strlen(tok);
  for (const char *p = s; *p; ) {
    const char *e = strchr(p, '\n');
    size_t l = e ? (size_t)(e - p) : strlen(p);
    if (l && p[l - 1] == '\r') l--;
    if (l == n && !strncmp(p, tok, n)) return 1;
    if (!e) break;
    p = e + 1;
  }
  return 0;
}

static int wifi_tools_present(void) {
  static const char *const need[] = { "iw", "wpa_supplicant", "ip" };
  static const char *const dirs[] = { "/usr/local/sbin", "/usr/local/bin", "/usr/sbin", "/usr/bin", "/sbin", "/bin" };
  for (size_t i = 0; i < sizeof need / sizeof need[0]; i++) {
    int ok = is_executable(need[i]);
    for (size_t d = 0; d < sizeof dirs / sizeof dirs[0] && !ok; d++) {
      char p[160]; snprintf(p, sizeof p, "%s/%s", dirs[d], need[i]);
      ok = access(p, X_OK) == 0;
    }
    if (!ok) { snprintf(wifi_missing, sizeof wifi_missing, "%s", need[i]); return 0; }
  }
  return 1;
}

static void wj_begin(const WReq *q) {
  st.job.req = *q; st.job.kind = q->kind; st.job.len = 0; st.job.fd = -1; st.job.pid = 0;
  st.job.t0 = now_ms();
  if (st.mock) {
    static const double delay[] = { 0, 150, 450, 1400, 150, 2200, 500, 500 };
    st.job.mock_end = st.job.t0 + delay[q->kind];
    return;
  }
  const char *body = (q->kind > 0 && q->kind < (int)(sizeof WSH_BODY / sizeof WSH_BODY[0])) ? WSH_BODY[q->kind] : NULL;
  if (!body) return;
  static char script[8192];
  snprintf(script, sizeof script, "%s%s", WSH_PRE, body);

  int pfd[2];
  if (pipe(pfd) != 0) return;
  pid_t pid = fork();
  if (pid == 0) {
    setpgid(0, 0);                               /* own group so a timeout can kill the whole script */
    dup2(pfd[1], 1);
    close(pfd[0]); close(pfd[1]);
    int dn = open("/dev/null", O_RDONLY);
    if (dn >= 0) { dup2(dn, 0); close(dn); }
    int de = open("/tmp/summerboard-wifi.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (de >= 0) { dup2(de, 2); close(de); }
    /* arguments travel in the environment: no quoting problems, nothing secret in `ps` */
    char hex[160], nb[16];
    ssid_hex(q->ssid, hex, sizeof hex);
    setenv("SB_SSID_HEX", hex, 1);
    snprintf(nb, sizeof nb, "%d", q->nid); setenv("SB_NID", nb, 1);
    snprintf(nb, sizeof nb, "%d", q->arg); setenv("SB_ARG", nb, 1);
    setenv("SB_SEC", q->sec[0] ? q->sec : "--", 1);
    setenv("SB_PW", q->pw, 1);
    if (opt_wifi_if[0]) setenv("SB_IF", opt_wifi_if, 1); else unsetenv("SB_IF");
    setenv("LC_ALL", "C", 1);
    execl("/bin/sh", "sh", "-c", script, (char *)NULL);
    _exit(127);
  }
  close(pfd[1]);
  if (pid < 0) { close(pfd[0]); return; }
  setpgid(pid, pid);
  fcntl(pfd[0], F_SETFL, O_NONBLOCK);
  st.job.fd = pfd[0]; st.job.pid = pid;
}

static int net_cmp(const void *pa, const void *pb) {
  const WNet *a = pa, *b = pb;
  if (a->active != b->active) return b->active - a->active;
  return b->signal - a->signal;
}
static int net_secure(const WNet *n) { return n->sec[0] && strcmp(n->sec, "--") != 0; }
static void apply_saved(void) {
  for (int i = 0; i < st.nnets; i++) {
    st.nets[i].saved = 0;
    for (int j = 0; j < st.nsaved; j++) if (!strcmp(st.saved[j], st.nets[i].ssid)) st.nets[i].saved = 1;
  }
}

static void open_password_sheet(const WNet *n);

static void wj_finish(int timed_out) {
  WReq rq = st.job.req;
  char *out = st.job.buf;
  if (timed_out) snprintf(out, sizeof st.job.buf, "TIMEOUT\n");
  else if (st.mock) mock_output(&rq, out, sizeof st.job.buf);
  else out[st.job.len < sizeof st.job.buf ? st.job.len : sizeof st.job.buf - 1] = 0;
  if (st.job.fd >= 0) { close(st.job.fd); st.job.fd = -1; }
  if (st.job.pid > 0) { if (timed_out) { kill(-st.job.pid, SIGTERM); kill(st.job.pid, SIGTERM); } st.job.pid = 0; }
  st.job.kind = WJ_NONE;

  if (!st.mock && rq.kind != WJ_SCAN && rq.kind != WJ_RADIO && rq.kind != WJ_SAVED) {
    static const char *const kn[] = { "none", "radio", "toggle", "scan", "saved", "connect", "disconnect", "forget" };
    FILE *lf = fopen("/tmp/summerboard-wifi.log", "a");
    if (lf) {
      char shown[200]; size_t k = 0;
      for (const char *p = out; *p && k + 1 < sizeof shown; p++) shown[k++] = *p == '\n' ? ' ' : *p;
      shown[k] = 0;
      fprintf(lf, "[summerboard] %s ssid='%s' sec='%s' saved=%d%s -> '%s'\n", kn[rq.kind], rq.ssid, rq.sec, rq.nid >= 0,
              timed_out ? " (timed out)" : "", shown);
      fclose(lf);
    }
  }
  if (has_line(out, "NODEV") && rq.kind != WJ_RADIO) toast(UI_RED, "No Wi-Fi adapter found");
  char *save = NULL, *f[8];
  switch (rq.kind) {
    case WJ_RADIO:
      st.radio_known = 1; st.radio_on = has_line(out, "enabled");
      st.tog.target = st.radio_on;
      if (!st.radio_on) { st.nnets = 0; st.active_ssid[0] = 0; }
      else if (st.page == PG_WIFI || !st.scanned) wj_queue(WJ_SCAN, NULL, NULL, 0, 0);
      break;
    case WJ_TOGGLE:
      wj_queue(WJ_RADIO, NULL, NULL, 0, 0);       /* re-sync with what the system really did */
      break;
    case WJ_SCAN: {
      if (has_line(out, "DOWN") || has_line(out, "NODEV")) { wj_queue(WJ_RADIO, NULL, NULL, 0, 0); break; }
      WNet tmp[MAX_NETS]; int nt = 0;
      st.active_ssid[0] = 0;
      for (char *ln = strtok_r(out, "\n", &save); ln; ln = strtok_r(NULL, "\n", &save)) {
        /* <active>\t<signal>\t<security>\t<ssid> */
        if (split_tab(ln, f, 4) < 4) continue;
        char ssid[64];
        ssid_unescape(f[3], ssid, sizeof ssid);
        if (!ssid[0]) continue;                   /* hidden network */
        int k = -1;
        for (int i = 0; i < nt; i++) if (!strcmp(tmp[i].ssid, ssid)) k = i;
        int sg = atoi(f[1]), act = f[0][0] == '*';
        if (k < 0) {
          if (nt >= MAX_NETS) continue;
          k = nt++;
          memset(&tmp[k], 0, sizeof tmp[k]);
          snprintf(tmp[k].ssid, sizeof tmp[k].ssid, "%s", ssid);
          snprintf(tmp[k].sec, sizeof tmp[k].sec, "%s", f[2]);
        }
        if (sg > tmp[k].signal) tmp[k].signal = sg;
        if (act) { tmp[k].active = 1; snprintf(st.active_ssid, sizeof st.active_ssid, "%s", ssid); }
      }
      qsort(tmp, nt, sizeof tmp[0], net_cmp);
      memcpy(st.nets, tmp, sizeof tmp[0] * nt);
      st.nnets = nt; st.scanned = 1;
      apply_saved();
      wj_queue(WJ_SAVED, NULL, NULL, 0, 0);
      break;
    }
    case WJ_SAVED:
      st.nsaved = 0;
      for (char *ln = strtok_r(out, "\n", &save); ln; ln = strtok_r(NULL, "\n", &save)) {
        /* <id>\t<ssid>\t<flags> */
        if (split_tab(ln, f, 4) < 2 || !isdigit((unsigned char)f[0][0])) continue;
        if (st.nsaved >= MAX_SAVED) break;
        ssid_unhex(f[1], st.saved[st.nsaved], 64);
        if (!st.saved[st.nsaved][0]) continue;
        st.saved_id[st.nsaved++] = atoi(f[0]);
      }
      apply_saved();
      break;
    case WJ_CONNECT: {
      int new_id = -1;
      { const char *p = strstr(out, "NID="); if (p) new_id = atoi(p + 4); }
      int fresh_profile = rq.nid < 0 && new_id >= 0;     /* created by this very job */
      int stale_saved   = rq.nid >= 0;
      st.connecting[0] = 0;
      if (has_line(out, "OK")) {
        toast(UI_GRN, "Connected to %s", rq.ssid);
      } else if (has_line(out, "NODHCP")) {
        toast(UI_RED, "Joined %s, but no DHCP client is installed", rq.ssid);
      } else if (has_line(out, "NOIP")) {
        toast(UI_RED, "Joined %s, but got no IP address", rq.ssid);
      } else if (has_line(out, "AUTH")) {
        if (fresh_profile) {                              /* a typed password failed: drop the half-made profile */
          wj_forget_nid(rq.ssid, new_id);
          toast(UI_RED, "Couldn't join \"%s\" - check the password", rq.ssid);
        } else if (stale_saved) {                         /* saved password no longer works: ask again */
          wj_forget_nid(rq.ssid, rq.nid);
          for (int i = 0; i < st.nnets; i++) if (!strcmp(st.nets[i].ssid, rq.ssid)) { open_password_sheet(&st.nets[i]); break; }
        } else toast(UI_RED, "Couldn't join \"%s\"", rq.ssid);
      } else if (has_line(out, "NEEDPW")) {
        for (int i = 0; i < st.nnets; i++) if (!strcmp(st.nets[i].ssid, rq.ssid)) { open_password_sheet(&st.nets[i]); break; }
      } else {
        if (fresh_profile) wj_forget_nid(rq.ssid, new_id);
        if (has_line(out, "TIMEOUT")) toast(UI_RED, "Connection timed out");
        else if (has_line(out, "NOTFOUND")) toast(UI_RED, "\"%s\" is out of range", rq.ssid);
        else if (has_line(out, "NOWPA")) toast(UI_RED, "Couldn't start wpa_supplicant");
        else if (has_line(out, "UNSUPPORTED")) toast(UI_RED, "Enterprise networks aren't supported");
        else if (!has_line(out, "NODEV")) toast(UI_RED, "Couldn't connect - see /tmp/summerboard-wifi.log");
      }
      wj_queue(WJ_SCAN, NULL, NULL, 0, 0);
      break;
    }
    case WJ_DISCONNECT:
      toast(UI_SUB, "Disconnected from %s", rq.ssid);
      wj_queue(WJ_SCAN, NULL, NULL, 0, 0);
      break;
    case WJ_FORGET:
      if (!rq.quiet) toast(UI_SUB, "Forgot %s", rq.ssid);
      wj_queue(WJ_SCAN, NULL, NULL, 0, 0);
      break;
  }
}

static void wj_poll(void) {
  if (st.job.kind == WJ_NONE) {
    if (st.qn > 0) {
      WReq r = st.q[0];
      memmove(&st.q[0], &st.q[1], (st.qn - 1) * sizeof(WReq));
      st.qn--;
      wj_begin(&r);
      memset(r.pw, 0, sizeof r.pw);
      if (st.job.kind != WJ_NONE) memset(st.job.req.pw, 0, sizeof st.job.req.pw);
      if (st.job.kind != WJ_NONE && !st.mock && st.job.fd < 0) wj_finish(0);   /* could not even start */
    }
    return;
  }
  double now = now_ms();
  if (now - st.job.t0 > WJ_TIMEOUT_MS) { wj_finish(1); return; }
  if (st.mock) { if (now >= st.job.mock_end) wj_finish(0); return; }
  for (;;) {
    size_t room = sizeof st.job.buf - 1 - st.job.len;
    char tmp[1024];
    ssize_t n = read(st.job.fd, tmp, sizeof tmp);
    if (n > 0) {
      size_t take = (size_t)n < room ? (size_t)n : room;
      memcpy(st.job.buf + st.job.len, tmp, take);
      st.job.len += take;
    } else if (n == 0) { wj_finish(0); return; }
    else if (errno == EINTR) continue;
    else if (errno == EAGAIN) return;
    else { wj_finish(0); return; }
  }
}

/* ---- drawing helpers ------------------------------------------------------ */
static void ui_disc(Surf *t, double cx, double cy, double r, uint32_t col, double al) {
  RR rr = rr_make(cx - r, cy - r, r * 2, r * 2, r);
  fill_rr(t, &rr, col, al, NULL);
}

/* Anti-aliased thick line segment with round caps. */
static void ui_seg(Surf *t, double x0, double y0, double x1, double y1, double w, uint32_t col, double al) {
  double r = w / 2;
  int bx0 = (int)floor(fmin(x0, x1) - r - 1), bx1 = (int)ceil(fmax(x0, x1) + r + 1);
  int by0 = (int)floor(fmin(y0, y1) - r - 1), by1 = (int)ceil(fmax(y0, y1) + r + 1);
  if (bx0 < 0) bx0 = 0; if (by0 < 0) by0 = 0;
  if (bx1 > t->w) bx1 = t->w; if (by1 > t->h) by1 = t->h;
  unsigned ac = (unsigned)(al * 256 + 0.5);
  double dx = x1 - x0, dy = y1 - y0, l2 = dx * dx + dy * dy;
  for (int y = by0; y < by1; y++)
    for (int x = bx0; x < bx1; x++) {
      double px = x + 0.5, py = y + 0.5;
      double u = l2 > 0 ? clampd(((px - x0) * dx + (py - y0) * dy) / l2, 0, 1) : 0;
      double ex = px - (x0 + u * dx), ey = py - (y0 + u * dy);
      double cov = clampd(r - sqrt(ex * ex + ey * ey) + 0.5, 0, 1);
      unsigned a = (unsigned)(cov * ac + 0.5);
      if (a) { uint32_t *p = t->px + (size_t)y * t->w + x; *p = mix32(*p, col, a > 256 ? 256 : a); }
    }
}

static void ui_arc(Surf *t, double cx, double cy, double r, double a0, double a1, double w, uint32_t col, double al) {
  int n = (int)ceil(fabs(a1 - a0) * r / 3.0);
  if (n < 4) n = 4;
  double px = cx + cos(a0) * r, py = cy + sin(a0) * r;
  for (int i = 1; i <= n; i++) {
    double a = a0 + (a1 - a0) * i / n, x = cx + cos(a) * r, y = cy + sin(a) * r;
    ui_seg(t, px, py, x, y, w, col, al);
    px = x; py = y;
  }
}

static void ui_wifi(Surf *t, double cx, double cy, double s, uint32_t col, double al) {
  double oy = cy + s * 0.30;
  ui_disc(t, cx, oy, s * 0.075, col, al);
  for (int k = 1; k <= 3; k++) ui_arc(t, cx, oy, s * (0.04 + 0.18 * k), -2.356, -0.785, s * 0.085, col, al);
}

static void ui_bars(Surf *t, double x, double yb, double h, int lvl, uint32_t col) {
  double bw = h * 0.2, gap = h * 0.12;
  for (int i = 0; i < 4; i++) {
    double bh = h * (0.32 + 0.227 * i);
    RR r = rr_make(x + i * (bw + gap), yb - bh, bw, bh, bw * 0.3);
    fill_rr(t, &r, col, i < lvl ? 0.95 : 0.25, NULL);
  }
}

static void ui_lock(Surf *t, double cx, double cy, double s, uint32_t col, uint32_t bg) {
  double sw = s * 0.16;
  RR o = rr_make(cx - s * 0.30, cy - s * 0.55, s * 0.60, s * 0.80, s * 0.30);
  fill_rr(t, &o, col, 1.0, NULL);
  RR i = rr_make(cx - s * 0.30 + sw, cy - s * 0.55 + sw, s * 0.60 - 2 * sw, s * 0.80, s * 0.30 - sw);
  fill_rr(t, &i, bg, 1.0, NULL);
  RR b = rr_make(cx - s * 0.44, cy - s * 0.10, s * 0.88, s * 0.62, s * 0.12);
  fill_rr(t, &b, col, 1.0, NULL);
}

static void ui_check(Surf *t, double cx, double cy, double s, uint32_t col, double al) {
  ui_seg(t, cx - s * 0.5, cy + s * 0.02, cx - s * 0.16, cy + s * 0.38, s * 0.2, col, al);
  ui_seg(t, cx - s * 0.16, cy + s * 0.38, cx + s * 0.5, cy - s * 0.38, s * 0.2, col, al);
}

static void ui_chevron(Surf *t, double x, double cy, double s, int dir, uint32_t col, double al) {
  /* dir +1 points right (tip at x), dir -1 points left (tip at x) */
  ui_seg(t, x - dir * s * 0.55, cy - s, x, cy, S(2.2), col, al);
  ui_seg(t, x, cy, x - dir * s * 0.55, cy + s, S(2.2), col, al);
}

static void ui_spinner(Surf *t, double cx, double cy, double r, uint32_t col, double al) {
  int head = (int)(now_ms() / 75.0) % 12;
  for (int i = 0; i < 12; i++) {
    double a = i * M_PI / 6, k = ((head - i + 12) % 12) / 12.0;
    ui_seg(t, cx + sin(a) * r * 0.5, cy - cos(a) * r * 0.5, cx + sin(a) * r, cy - cos(a) * r, r * 0.2,
           col, al * (0.15 + 0.85 * (1.0 - k)));
  }
}

static void ui_toggle(Surf *t, double x, double y, double k) {
  double w = S(51), h = S(31);
  RR tr = rr_make(x, y, w, h, h / 2);
  fill_rr(t, &tr, mix32(0x3a3654, UI_GRN, (unsigned)(clampd(k, 0, 1) * 256)), 1.0, NULL);
  double kx = x + h / 2 + clampd(k, 0, 1) * (w - h);
  ui_disc(t, kx, y + h / 2 + S(1.2), h / 2 - S(1), 0x000000, 0.25);
  ui_disc(t, kx, y + h / 2, h / 2 - S(2), 0xffffff, 1.0);
}

static void ui_text_fit(Surf *t, const AppFont *f, const char *s, double x, double y, double maxw,
                        int align, uint32_t col, double al) {
  if (maxw < 8) return;
  if (text_width(f, s, 1.0, 0) <= maxw) { draw_text(t, f, s, x, y, 1.0, align, col, al, 0, NULL); return; }
  char buf[128];
  double ell = text_width(f, "...", 1.0, 0), acc = 0;
  const char *p = s; size_t cut = 0;
  while (*p) {
    const char *q = p; uint32_t cp = utf8_next(&q);
    double adv = f->g[glyph_index(cp)].adv / f->ss;
    if (acc + adv + ell > maxw) break;
    acc += adv; p = q; cut = (size_t)(p - s);
  }
  if (cut > sizeof buf - 4) cut = sizeof buf - 4;
  memcpy(buf, s, cut); strcpy(buf + cut, "...");
  draw_text(t, f, buf, x, y, 1.0, align, col, al, 0, NULL);
}

static void add_hit(double x, double y, double w, double h, int id, int arg, double top, double bot) {
  if (!st.rec || st.nhits >= MAX_HITS) return;
  if (y < top) { h -= top - y; y = top; }
  if (y + h > bot) h = bot - y;
  if (h <= 0 || w <= 0) return;
  st.hits[st.nhits++] = (Hit){ { x, y, w, h, 0 }, id, arg };
}
static const Hit *hit_at(double x, double y) {
  for (int i = st.nhits - 1; i >= 0; i--) {
    const Rect *r = &st.hits[i].r;
    if (x >= r->x && x <= r->x + r->w && y >= r->y && y <= r->y + r->h) return &st.hits[i];
  }
  return NULL;
}
static int pressed(int id, int arg) { return st.press_id == id && st.press_arg == arg && !st.dragging; }
static int sig_level(int s) { return s >= 75 ? 4 : s >= 55 ? 3 : s >= 35 ? 2 : s >= 15 ? 1 : 0; }

static int pw_valid(void) {
  int wep = strstr(st.sel.sec, "WEP") != NULL;
  return st.pw_len >= (wep ? 5 : 8) && st.pw_len <= 63;
}

/* ---- pages ------------------------------------------------------------------ */
static void draw_page_root(Surf *t, double dx) {
  fill_rect(t, (int)lround(dx), 0, t->w, t->h, UI_BG, 1.0);
  double m = S(16), rh = S(54), cw = t->w - 2 * m;
  draw_text(t, &F_big, "Settings", dx + m + S(4), S(66), 1.0, 0, UI_TXT, 1.0, 0, NULL);

  char wv[72] = "";
  if (st.radio_known) snprintf(wv, sizeof wv, "%s", !st.radio_on ? "Off" : st.active_ssid[0] ? st.active_ssid : "Not Connected");

  static const struct { const char *name; uint32_t col; } soon[] = {
    { "Bluetooth", 0x0a84ff }, { "Display & Brightness", 0xff9f0a }, { "Sounds", 0xff375f }, { "About", 0x8e8e93 } };

  double y = S(96);
  RR c1 = rr_make(dx + m, y, cw, rh, S(12));
  fill_rr(t, &c1, UI_CARD, 1.0, NULL);
  if (pressed(H_ROOT_WIFI, 0)) { RR hr = rr_make(dx + m, y, cw, rh, 0); fill_rr(t, &hr, UI_HI, 1.0, &c1); }
  double is = S(30), ix = dx + m + S(14), cy = y + rh / 2;
  RR ic = rr_make(ix, cy - is / 2, is, is, S(7));
  fill_rr(t, &ic, UI_ACC, 1.0, NULL);
  ui_wifi(t, ix + is / 2, cy, is * 0.72, 0xffffff, 1.0);
  double lx = ix + is + S(12), rx = dx + m + cw - S(14);
  ui_chevron(t, rx - S(2), cy, S(5.5), 1, UI_SUB, 0.7);
  ui_text_fit(t, &F_body, wv, rx - S(20), BL(cy, 16), cw * 0.5, 2, UI_SUB, 1.0);
  draw_text(t, &F_body, "Wi-Fi", lx, BL(cy, 16), 1.0, 0, UI_TXT, 1.0, 0, NULL);
  add_hit(dx + m, y, cw, rh, H_ROOT_WIFI, 0, 0, t->h);

  y += rh + S(26);
  draw_text(t, &F_lbl, "COMING SOON", dx + m + S(16), y - S(8), 1.0, 0, UI_SUB, 1.0, S(0.6), NULL);
  RR c2 = rr_make(dx + m, y, cw, rh * 4, S(12));
  fill_rr(t, &c2, UI_CARD, 1.0, NULL);
  for (int i = 0; i < 4; i++) {
    double ry = y + i * rh, rcy = ry + rh / 2;
    RR si = rr_make(ix, rcy - is / 2, is, is, S(7));
    fill_rr(t, &si, soon[i].col, 0.45, NULL);
    draw_text(t, &F_body, soon[i].name, lx, BL(rcy, 16), 1.0, 0, UI_TXT, 0.4, 0, NULL);
    if (i < 3) fill_rect(t, (int)(dx + m + S(56)), (int)(ry + rh - 1), (int)(cw - S(56)), 1, 0xffffff, 0.07);
  }
}

static void draw_page_wifi(Surf *t, double dx) {
  fill_rect(t, (int)lround(dx), 0, t->w, t->h, UI_BG, 1.0);
  double m = S(16), rh = S(54), cw = t->w - 2 * m, nav_h = S(60), H_ = t->h;
  double top = nav_h + S(12);
  double y = top - st.scroll.x;

  /* Wi-Fi on/off */
  RR c1 = rr_make(dx + m, y, cw, rh, S(12));
  fill_rr(t, &c1, pressed(H_TOGGLE, 0) ? UI_HI : UI_CARD, 1.0, NULL);
  draw_text(t, &F_body, "Wi-Fi", dx + m + S(16), BL(y + rh / 2, 16), 1.0, 0, UI_TXT, 1.0, 0, NULL);
  ui_toggle(t, dx + m + cw - S(16) - S(51), y + (rh - S(31)) / 2, st.tog.x);
  add_hit(dx + m, y, cw, rh, H_TOGGLE, 0, nav_h, H_);
  y += rh + S(28);

  /* section header */
  int scanning = st.job.kind == WJ_SCAN;
  draw_text(t, &F_lbl, "NETWORKS", dx + m + S(16), y - S(8), 1.0, 0, UI_SUB, 1.0, S(0.6), NULL);
  if (st.mock) {
    char demo[48];
    if (wifi_missing[0] && !opt_mock_wifi) snprintf(demo, sizeof demo, "DEMO - %s MISSING", wifi_missing);
    else snprintf(demo, sizeof demo, "DEMO DATA");
    draw_text(t, &F_lbl, demo, dx + m + cw - S(16), y - S(8), 1.0, 2, wifi_missing[0] && !opt_mock_wifi ? UI_RED : UI_SUB, 0.8, S(0.6), NULL);
  }
  else if (scanning && st.radio_on) ui_spinner(t, dx + m + S(102), y - S(12), S(7), UI_SUB, 1.0);
  else if (scanning) {}
  if (st.mock && scanning && st.radio_on) ui_spinner(t, dx + m + S(102), y - S(12), S(7), UI_SUB, 1.0);

  if (!st.radio_on) {
    draw_text(t, &F_body, "Turn on Wi-Fi to see nearby networks", dx + t->w / 2, y + S(50), 1.0, 1, UI_SUB, 0.8, 0, NULL);
    y += S(80);
  } else if (st.nnets == 0) {
    RR c = rr_make(dx + m, y, cw, rh, S(12));
    fill_rr(t, &c, UI_CARD, 1.0, NULL);
    if (scanning || !st.scanned) ui_spinner(t, dx + m + S(28), y + rh / 2, S(8), UI_SUB, 1.0);
    draw_text(t, &F_body, scanning || !st.scanned ? "Searching..." : "No networks found", dx + m + S(50), BL(y + rh / 2, 16),
              1.0, 0, UI_SUB, 1.0, 0, NULL);
    y += rh;
  } else {
    RR card = rr_make(dx + m, y, cw, rh * st.nnets, S(12));
    fill_rr(t, &card, UI_CARD, 1.0, NULL);
    for (int i = 0; i < st.nnets; i++) {
      const WNet *n = &st.nets[i];
      double ry = y + i * rh, cy = ry + rh / 2;
      if (ry + rh < nav_h || ry > H_) continue;
      int hl = pressed(H_NET, i), connecting = !strcmp(st.connecting, n->ssid);
      uint32_t bg = hl ? UI_HI : UI_CARD;
      if (hl) { RR hr = rr_make(dx + m, ry, cw, rh, 0); fill_rr(t, &hr, UI_HI, 1.0, &card); }

      double x0 = dx + m + S(16), lx = dx + m + S(44), rx = dx + m + cw - S(14);
      if (connecting) ui_spinner(t, x0 + S(8), cy, S(8), UI_TXT, 1.0);
      else if (n->active) ui_check(t, x0 + S(8), cy, S(9), UI_ACC, 1.0);

      int info = n->active || n->saved;
      if (info) {
        double icx = rx - S(15);
        ui_disc(t, icx, cy, S(11), UI_ACC, 1.0);
        ui_disc(t, icx, cy, S(9.6), bg, 1.0);
        draw_text(t, &F_card, "i", icx, BL(cy, 13), 1.0, 1, UI_ACC, 1.0, 0, NULL);
        rx -= S(38);
      }
      ui_bars(t, rx - S(19), cy + S(8), S(16), sig_level(n->signal), UI_TXT);
      rx -= S(30);
      if (net_secure(n)) { ui_lock(t, rx - S(6), cy + S(1), S(15), UI_TXT, bg); rx -= S(24); }

      double lw = rx - lx;
      if (connecting) {
        ui_text_fit(t, &F_body, n->ssid, lx, BL(cy - S(8), 16), lw, 0, UI_TXT, 1.0);
        draw_text(t, &F_lbl, "Connecting...", lx, BL(cy + S(11), 11), 1.0, 0, UI_SUB, 1.0, 0, NULL);
      } else ui_text_fit(t, &F_body, n->ssid, lx, BL(cy, 16), lw, 0, UI_TXT, 1.0);

      if (i < st.nnets - 1) fill_rect(t, (int)lx, (int)(ry + rh - 1), (int)(dx + m + cw - lx), 1, 0xffffff, 0.07);
      add_hit(dx + m, ry, cw, rh, H_NET, i, nav_h, H_);
      if (info) add_hit(dx + m + cw - S(54), ry, S(54), rh, H_INFO, i, nav_h, H_);
    }
    y += rh * st.nnets;
  }
  st.max_scroll = fmax(0, (y + st.scroll.x + S(64)) - H_);

  /* navigation bar (drawn last so content scrolls underneath it) */
  fill_rect(t, (int)lround(dx), 0, t->w, (int)nav_h, UI_BG, 1.0);
  fill_rect(t, (int)lround(dx), (int)nav_h, t->w, 1, 0xffffff, 0.08);
  double ncy = S(34);
  double ba = pressed(H_BACK, 0) ? 0.45 : 1.0;
  ui_chevron(t, dx + m + S(4), ncy, S(7), -1, UI_ACC, ba);
  draw_text(t, &F_body, "Settings", dx + m + S(14), BL(ncy, 16), 1.0, 0, UI_ACC, ba, 0, NULL);
  add_hit(dx, 0, S(130), nav_h, H_BACK, 0, 0, H_);
  draw_text(t, &F_hdr, "Wi-Fi", dx + t->w / 2, BL(ncy, 20), 1.0, 1, UI_TXT, 1.0, 0, NULL);
  if (st.radio_on) {
    if (scanning) ui_spinner(t, dx + t->w - m - S(10), ncy, S(9), UI_ACC, 1.0);
    else {
      draw_text(t, &F_body, "Scan", dx + t->w - m, BL(ncy, 16), 1.0, 2, UI_ACC, pressed(H_SCAN, 0) ? 0.45 : 1.0, 0, NULL);
      add_hit(dx + t->w - S(100), 0, S(100), nav_h, H_SCAN, 0, 0, H_);
    }
  }
}

static void draw_sheet(Surf *t) {
  double k = clampd(st.sheet.x, 0, 1);
  if (k < 0.004) return;
  if (st.sheet_open && st.rec) st.nhits = 0;
  fill_rect(t, 0, 0, t->w, t->h, 0x000000, 0.55 * k);
  add_hit(0, 0, t->w, t->h, H_DIM, st.sheet_kind, 0, t->h);

  double pad = S(18), w = fmin(t->w - 2 * S(20), S(400)), x = (t->w - w) / 2;
  if (st.sheet_kind == SH_PASSWORD) {
    double h = S(222), y = fmax(S(14), (t->h - h) * 0.28) + (1 - k) * S(28);
    RR card = rr_make(x, y, w, h, S(18));
    fill_rr(t, &card, 0x262040, k, NULL);
    draw_text(t, &F_hdr, "Enter Password", x + w / 2, y + S(36), 1.0, 1, UI_TXT, k, 0, NULL);
    char sub[110]; snprintf(sub, sizeof sub, "for \"%s\"", st.sel.ssid);
    ui_text_fit(t, &F_lbl, sub, x + w / 2, y + S(58), w - 2 * pad, 1, UI_SUB, k);

    double fy = y + S(72), fh = S(46);
    RR field = rr_make(x + pad, fy, w - 2 * pad, fh, S(10));
    fill_rr(t, &field, 0x14111f, k, NULL);
    add_hit(x + pad, fy, w - 2 * pad - S(64), fh, H_PW_FIELD, 0, 0, t->h);
    double tx = x + pad + S(14), tr = x + w - pad - S(64), cy = fy + fh / 2, caret = tx;
    if (st.pw_show) {
      ui_text_fit(t, &F_body, st.pw, tx, BL(cy, 16), tr - tx, 0, UI_TXT, k);
      caret = tx + fmin(text_width(&F_body, st.pw, 1.0, 0), tr - tx);
    } else {
      int fit = (int)((tr - tx) / S(14)), start = st.pw_len > fit ? st.pw_len - fit : 0, c = 0;
      for (int i = start; i < st.pw_len; i++, c++) ui_disc(t, tx + c * S(14) + S(5), cy, S(4.2), UI_TXT, k);
      caret = tx + c * S(14);
    }
    if (((int)(now_ms() / 500)) % 2 == 0) fill_rect(t, (int)caret, (int)(cy - S(10)), (int)fmax(2, S(2)), (int)S(20), UI_ACC, k);
    draw_text(t, &F_lbl, st.pw_show ? "Hide" : "Show", x + w - pad - S(14), BL(cy, 11), 1.0, 2, UI_ACC,
              pressed(H_PW_SHOW, 0) ? 0.45 * k : k, 0, NULL);
    add_hit(x + w - pad - S(64), fy, S(64), fh, H_PW_SHOW, 0, 0, t->h);

    if (st.pw_len > 0 && !pw_valid())
      draw_text(t, &F_lbl, strstr(st.sel.sec, "WEP") ? "WEP keys need at least 5 characters" : "Passwords need 8 to 63 characters",
                x + pad + S(2), fy + fh + S(18), 1.0, 0, UI_SUB, k, 0, NULL);

    double bh = S(46), by = y + h - bh - pad, bw = (w - 2 * pad - S(10)) / 2;
    RR cb = rr_make(x + pad, by, bw, bh, S(12));
    fill_rr(t, &cb, pressed(H_PW_CANCEL, 0) ? UI_HI : 0x322c50, k, NULL);
    draw_text(t, &F_body, "Cancel", x + pad + bw / 2, BL(by + bh / 2, 16), 1.0, 1, UI_TXT, k, 0, NULL);
    add_hit(x + pad, by, bw, bh, H_PW_CANCEL, 0, 0, t->h);
    RR jb = rr_make(x + pad + bw + S(10), by, bw, bh, S(12));
    int ok = pw_valid();
    fill_rr(t, &jb, UI_ACC, (ok ? (pressed(H_PW_JOIN, 0) ? 0.75 : 1.0) : 0.3) * k, NULL);
    draw_text(t, &F_body, "Join", x + pad + bw + S(10) + bw / 2, BL(by + bh / 2, 16), 1.0, 1, 0xffffff, (ok ? 1.0 : 0.5) * k, 0, NULL);
    if (ok) add_hit(x + pad + bw + S(10), by, bw, bh, H_PW_JOIN, 0, 0, t->h);
  } else {
    int nb = st.sel.active ? 3 : 2;
    double bh = S(48), gap = S(8), h = S(84) + nb * bh + (nb - 1) * gap + pad;
    double y = (t->h - h) / 2 + (1 - k) * S(28);
    RR card = rr_make(x, y, w, h, S(18));
    fill_rr(t, &card, 0x262040, k, NULL);
    ui_text_fit(t, &F_hdr, st.sel.ssid, x + w / 2, y + S(36), w - 2 * pad, 1, UI_TXT, k);
    draw_text(t, &F_lbl, st.sel.active ? "Connected" : "Saved network", x + w / 2, y + S(58), 1.0, 1, UI_SUB, k, 0, NULL);
    double by = y + S(76);
    static const char *lab[3] = { "Disconnect", "Forget This Network", "Cancel" };
    static const int ids[3] = { H_D_DISCONNECT, H_D_FORGET, H_D_CLOSE };
    for (int i = st.sel.active ? 0 : 1; i < 3; i++) {
      RR b = rr_make(x + pad, by, w - 2 * pad, bh, S(12));
      fill_rr(t, &b, pressed(ids[i], 0) ? UI_HI : 0x322c50, k, NULL);
      draw_text(t, &F_body, lab[i], x + w / 2, BL(by + bh / 2, 16), 1.0, 1, i == 1 ? UI_RED : (i == 0 ? UI_ACC : UI_TXT), k, 0, NULL);
      add_hit(x + pad, by, w - 2 * pad, bh, ids[i], 0, 0, t->h);
      by += bh + gap;
    }
  }
}

static void draw_toast(Surf *t) {
  double now = now_ms();
  if (!st.toast[0] || now >= st.toast_until) return;
  double a = clampd(fmin((now - st.toast_t0) / 150.0, (st.toast_until - now) / 350.0), 0, 1);
  double w = fmin(text_width(&F_card, st.toast, 1.0, 0) + S(40), t->w - S(24)), h = S(40);
  double x = (t->w - w) / 2, y = t->h - S(84) + (1 - a) * S(12);
  RR r = rr_make(x, y, w, h, h / 2);
  fill_rr(t, &r, 0x000000, 0.35 * a, NULL);
  fill_rr(t, &r, mix32(0x2a2542, st.toast_col, 70), 0.97 * a, NULL);
  ui_text_fit(t, &F_card, st.toast, x + w / 2, BL(y + h / 2, 13), w - S(28), 1, 0xffffff, a);
}

static void settings_draw(Surf *t, int rec) {
  st.rec = rec;
  if (rec) st.nhits = 0;
  double nav = clampd(st.nav.x, 0, 1);
  if (nav < 0.999) draw_page_root(t, -nav * t->w * 0.28);
  if (nav > 0.001 && nav < 0.999) fill_rect(t, 0, 0, t->w, t->h, 0x000000, 0.35 * nav);
  if (nav > 0.001) draw_page_wifi(t, (1 - nav) * t->w);
  draw_sheet(t);
  draw_toast(t);
}

/* Live paint used by the compositor. At rest this is drawn straight onto the screen buffer;
   while the window is opening/closing it is rendered off-screen and scaled into `r`. */
static void settings_paint(Surf *t, Rect r) {
  if (fabs(r.x) < 1 && r.w >= t->w - 1 && r.h >= t->h - S(12) - 2) { settings_draw(t, 1); return; }
  static Surf off;
  if (!off.px || off.w != t->w || off.h != t->h) { surf_free(&off); off = surf_new_raw(t->w, t->h); }
  if (!off.px) return;
  settings_draw(&off, 0);
  RR clip = rr_make(r.x, r.y, r.w, r.h, r.r);
  draw_image(t, &off, &r, &clip);
}

static void settings_snapshot(App *a) {
  int eff_h = kbd_active ? (int)lround(H * 0.75) : H;
  if (a->snap.px && (a->snap.w != W || a->snap.h != eff_h)) snap_drop(a);
  if (!a->snap.px) a->snap = surf_new_raw(W, eff_h);
  if (!a->snap.px) return;
  surf_free(&a->thumb);
  settings_draw(&a->snap, 0);
}

/* Gear glyph on the home-screen icon. */
static void settings_icon(Surf *t, double cx, double cy, double s, double al) {
  uint32_t fg = 0xe9e9ee;
  for (int i = 0; i < 8; i++) {
    double a = i * M_PI / 4;
    ui_seg(t, cx + cos(a) * s * 0.30, cy + sin(a) * s * 0.30, cx + cos(a) * s * 0.40, cy + sin(a) * s * 0.40, s * 0.17, fg, al);
  }
  ui_disc(t, cx, cy, s * 0.33, fg, al);
  ui_disc(t, cx, cy, s * 0.13, 0x8e8e93, al);
}

/* ---- interaction ------------------------------------------------------------ */
static void kbd_focus(void) {
  if (!kbd_active) { toggle_matchbox_keyboard(); st.opened_kbd = 1; }
  XSetInputFocus(dpy, shell, RevertToPointerRoot, CurrentTime);
}

static void close_sheet(void) {
  st.sheet_open = 0; st.sheet.target = 0;
  memset(st.pw, 0, sizeof st.pw); st.pw_len = 0; st.pw_show = 0;
  if (st.opened_kbd && kbd_active) toggle_matchbox_keyboard();
  st.opened_kbd = 0;
}

static void open_password_sheet(const WNet *n) {
  st.sel = *n;
  memset(st.pw, 0, sizeof st.pw); st.pw_len = 0; st.pw_show = 0;
  st.sheet_kind = SH_PASSWORD; st.sheet_open = 1; st.sheet.target = 1;
  kbd_focus();
}

static void open_details(const WNet *n) {
  st.sel = *n;
  st.sheet_kind = SH_DETAILS; st.sheet_open = 1; st.sheet.target = 1;
}

static void connect_to(const char *ssid, const char *pw) {
  wj_cancel_scan();
  snprintf(st.connecting, sizeof st.connecting, "%s", ssid);
  wj_queue(WJ_CONNECT, ssid, pw, pw != NULL, 0);
}

static void wifi_toggle(void) {
  int on = !st.radio_on;
  wj_cancel_scan();
  st.radio_on = on; st.radio_known = 1; st.tog.target = on;
  if (!on) { st.nnets = 0; st.active_ssid[0] = 0; st.connecting[0] = 0; }
  wj_queue(WJ_TOGGLE, "", NULL, on, 0);
}

static void net_tap(int i) {
  if (i < 0 || i >= st.nnets) return;
  const WNet *n = &st.nets[i];
  if (n->active) { open_details(n); return; }
  if (net_secure(n) && !n->saved) open_password_sheet(n);
  else connect_to(n->ssid, NULL);
}

static void pw_join(void) {
  if (!pw_valid()) return;
  char ssid[64], pw[80];
  snprintf(ssid, sizeof ssid, "%s", st.sel.ssid);
  snprintf(pw, sizeof pw, "%s", st.pw);
  close_sheet();
  connect_to(ssid, pw);
  memset(pw, 0, sizeof pw);
}

static void settings_dispatch(int id, int arg) {
  switch (id) {
    case H_ROOT_WIFI:
      st.page = PG_WIFI; st.nav.target = 1; sp_snap(&st.scroll, 0);
      wj_queue(WJ_RADIO, NULL, NULL, 0, 0);
      break;
    case H_BACK: st.page = PG_ROOT; st.nav.target = 0; break;
    case H_SCAN: if (st.radio_on && st.job.kind != WJ_SCAN) wj_queue(WJ_SCAN, NULL, NULL, 0, 0); break;
    case H_TOGGLE: wifi_toggle(); break;
    case H_NET: net_tap(arg); break;
    case H_INFO: if (arg >= 0 && arg < st.nnets) open_details(&st.nets[arg]); break;
    case H_PW_FIELD: kbd_focus(); break;
    case H_PW_SHOW: st.pw_show = !st.pw_show; break;
    case H_PW_CANCEL: close_sheet(); break;
    case H_PW_JOIN: pw_join(); break;
    case H_D_DISCONNECT: wj_queue(WJ_DISCONNECT, st.sel.ssid, NULL, 0, 0); close_sheet(); break;
    case H_D_FORGET: wj_queue(WJ_FORGET, st.sel.ssid, NULL, 0, 0); saved_drop(st.sel.ssid); apply_saved(); close_sheet(); break;
    case H_D_CLOSE: close_sheet(); break;
    case H_DIM: if (arg == SH_DETAILS) close_sheet(); break;   /* a stray tap must not throw away a typed password */
  }
}

static void settings_pointer_down(double x, double y) {
  st.press_id = 0; st.dragging = 0; st.press_x = x; st.press_y = y;
  st.press_scroll = st.scroll.x; st.scroll.target = st.scroll.x; st.scroll.v = 0;
  const Hit *h = hit_at(x, y);
  if (h) { st.press_id = h->id; st.press_arg = h->arg; }
}

static void settings_pointer_move(double x, double y) {
  if (hypot(x - st.press_x, y - st.press_y) > S(10)) {
    st.press_id = 0;
    if (st.page == PG_WIFI && !st.sheet_open) st.dragging = 1;
  }
  if (!st.dragging) return;
  double v = st.press_scroll - (y - st.press_y);
  if (v < 0) v *= 0.3;
  if (v > st.max_scroll) v = st.max_scroll + (v - st.max_scroll) * 0.3;
  st.scroll.target = st.scroll.x = v; st.scroll.v = 0;
}

static void settings_pointer_up(double x, double y, double vy) {
  if (st.dragging) {
    st.dragging = 0;
    st.scroll.target = clampd(st.scroll.x - vy * 0.22, 0, st.max_scroll);
    return;
  }
  int id = st.press_id, arg = st.press_arg;
  st.press_id = 0;
  if (!id) return;
  const Hit *h = hit_at(x, y);
  if (h && h->id == id && h->arg == arg) settings_dispatch(id, arg);
}

/* Keyboard input (physical keys, or matchbox-keyboard via XTest) for the password sheet. */
static void settings_key(XKeyEvent *e) {
  if (!st.sheet_open || st.sheet_kind != SH_PASSWORD) return;
  char buf[16]; KeySym ks;
  int n = XLookupString(e, buf, sizeof buf, &ks, NULL);
  if (ks == XK_Return || ks == XK_KP_Enter) pw_join();
  else if (ks == XK_Escape) close_sheet();
  else if (ks == XK_BackSpace) { if (st.pw_len > 0) st.pw[--st.pw_len] = 0; }
  else if (n == 1 && (unsigned char)buf[0] >= 32 && (unsigned char)buf[0] < 127 && st.pw_len < 63)
    { st.pw[st.pw_len++] = buf[0]; st.pw[st.pw_len] = 0; }
}

/* ---- lifecycle --------------------------------------------------------------- */
static void settings_init(void) {
  memset(&st, 0, sizeof st);
  st.job.fd = -1;
  sp_init(&st.nav, 0, 300, 30, 0.002);
  sp_init(&st.scroll, 0, 260, 27, 0.05);
  sp_init(&st.tog, 0, 380, 30, 0.002);
  sp_init(&st.sheet, 0, 340, 30, 0.002);
  st.mock = opt_mock_wifi || !wifi_tools_present();
  if (st.mock) { mock_active = 0; mock_saved[0] = 1; }
}

static void settings_open(int fresh) {
  if (fresh) {
    st.nhits = 0;
    st.page = PG_ROOT; sp_snap(&st.nav, 0); sp_snap(&st.scroll, 0);
    st.sheet_open = 0; sp_snap(&st.sheet, 0);
  }
  wj_queue(WJ_RADIO, NULL, NULL, 0, 0);
  XSetInputFocus(dpy, shell, RevertToPointerRoot, CurrentTime);
}

static void settings_close(void) {
  if (st.sheet_open) close_sheet();
  st.page = PG_ROOT; sp_snap(&st.nav, 0); sp_snap(&st.sheet, 0);
  st.toast[0] = 0;
}

static void settings_tick(double dt) {
  sp_update(&st.nav, dt); sp_update(&st.scroll, dt); sp_update(&st.tog, dt); sp_update(&st.sheet, dt);
  if (!st.dragging && st.scroll.target > st.max_scroll) st.scroll.target = st.max_scroll;
  wj_poll();
}

static int settings_animating(void) {
  if (st.job.kind != WJ_NONE || st.qn > 0) return 1;
  if (!(active_app && active_app->builtin)) return 0;
  return sp_busy(&st.nav) || sp_busy(&st.scroll) || sp_busy(&st.tog) || sp_busy(&st.sheet)
      || st.toast_until > now_ms() || (st.sheet_open && st.sheet_kind == SH_PASSWORD);
}


/* ---- glue between the Settings app and the compositor ------------------------- */
static int builtin_touch = 0;   /* a touch that started inside a built-in app is in progress */

static void builtin_enter(App *a, int fresh) { (void)a; settings_open(fresh); }

/* Leaving the foreground (home swipe / switcher): dismiss an open password sheet (and the
   on-screen keyboard it raised) so neither lingers over the home screen. */
static void builtin_leave(App *a) {
  (void)a;
  if (st.sheet_open) close_sheet();
  sp_snap(&st.sheet, 0);
  st.press_id = 0; st.dragging = 0;
}

static void builtin_close(App *a) { (void)a; settings_close(); }

static void builtin_paint(Surf *t, App *a, Rect r) { (void)a; settings_paint(t, r); }


static void terminate(App *a) {
  a->state = ST_CLOSED;
  if (a->builtin) builtin_close(a);
  
  // X11 pencerelerini kapat
  for (int i = 0; i < a->nwin; i++) XDestroyWindow(dpy, a->wins[i]);
  a->nwin = 0;
  snap_drop(a);
  if (active_app == a) active_app = NULL;

  /* Kill the app's whole process tree by PID (children included). */
  if (a->pid > 0) {
    pid_t pp;
    /* only if it's still our direct child (guards against PID reuse) */
    if (kt_stat(a->pid, &pp, NULL, NULL) == 0 && pp == getpid())
      kill_process_tree_async(a->pid, 500);
    a->pid = 0;
  }
}

static void resume(App *a, Rect from_rect) {
  snap_drop(a);
  a->state = ST_FG;
  active_app = a;
  switcher.open = 0; switcher.entry.dur = 0;
  rs_snap(&win_spring, from_rect);
  
  Rect target = full_rect();
  {
    int eff_h = kbd_active ? (int)lround(H * 0.75) : H;
    target.h = eff_h - (int)lround(S(12));
  }
  
  rs_target(&win_spring, target);
  home_scale_spring.target = 0.92;
  if (a->builtin) builtin_enter(a, 0);
  app_raise(a);
}

static void close_switcher_to_home(void) {
  switcher.open = 0; switcher.entry.dur = 0; active_app = NULL;
  if (gesture) { free(gesture); gesture = NULL; }
  home_scale_spring.target = 1.0;
}

/* ------------------------------------------------------------------------ */
/* SAFE SNAPSHOT CAPTURE                                                    */
/* ------------------------------------------------------------------------ */

/* Reusable MIT-SHM segment for window grabs: attached once, sized for a full-screen window.
   XShmGetImage has the server write pixels straight into it (no trip through the X socket). */
static XShmSegmentInfo snap_si;
static int snap_shm_state = 0;        /* 0 = untried, 1 = ready, -1 = unavailable */
static size_t snap_shm_cap = 0;

static int snap_shm_init(void) {
  if (snap_shm_state) return snap_shm_state > 0;
  snap_shm_state = -1;
  if (!use_shm) return 0;
  snap_shm_cap = (size_t)W * H * 4;
  snap_si.shmid = shmget(IPC_PRIVATE, snap_shm_cap, IPC_CREAT | 0600);
  if (snap_si.shmid < 0) return 0;
  snap_si.shmaddr = shmat(snap_si.shmid, NULL, 0);
  if (snap_si.shmaddr == (char *)-1) { shmctl(snap_si.shmid, IPC_RMID, NULL); return 0; }
  snap_si.readOnly = False;
  XErrorHandler old_h = XSetErrorHandler(xerr_handler);
  xerr_trap = 0;
  XShmAttach(dpy, &snap_si);
  XSync(dpy, False);
  XSetErrorHandler(old_h);
  shmctl(snap_si.shmid, IPC_RMID, NULL);
  if (xerr_trap) { shmdt(snap_si.shmaddr); return 0; }
  snap_shm_state = 1;
  return 1;
}

static void thumb_bake_alpha(Surf *th, double r);   /* defined with the thumbnail code */

/* One pass over the grabbed pixels: each source row is forced opaque into a->snap and, while it
   is still hot in cache, every thumbnail row whose lower bilinear tap is that row is emitted.
   The thumbnail maths is exactly draw_image()'s (cover-crop, bilinear) so the result is identical
   to what ensure_thumb() would have built later. */
static int snap_fused(App *a, const XImage *im, int w, int h) {
  double cwd, chd; card_size(&cwd, &chd);
  int tcw = (int)lround(cwd), tch = (int)lround(chd);
  int want_thumb = tcw >= 2 && tch >= 2;

  if (a->snap.px && (a->snap.w != w || a->snap.h != h)) snap_drop(a);
  if (!a->snap.px) a->snap = surf_new_raw(w, h);
  if (!a->snap.px) return 0;
  surf_free(&a->thumb);

  Surf th = { 0, 0, NULL };
  int *xa = NULL, *xb = NULL, *ya = NULL, *yb = NULL; unsigned *tx = NULL, *ty = NULL;
  if (want_thumb) {
    th = surf_new(tcw, tch);
    xa = malloc(sizeof(int) * tcw); xb = malloc(sizeof(int) * tcw); tx = malloc(sizeof(unsigned) * tcw);
    ya = malloc(sizeof(int) * tch); yb = malloc(sizeof(int) * tch); ty = malloc(sizeof(unsigned) * tch);
    if (!th.px || !xa || !xb || !tx || !ya || !yb || !ty) {
      surf_free(&th); free(xa); free(xb); free(tx); free(ya); free(yb); free(ty);
      xa = xb = ya = yb = NULL; tx = ty = NULL; want_thumb = 0;
    }
  }
  if (want_thumb) {
    double sc = fmax((double)tcw / w, (double)tch / h);
    double ox = (w - tcw / sc) / 2, oy = (h - tch / sc) / 2;
    float inv = (float)(1.0 / sc);
    for (int i = 0; i < tcw; i++) {
      float fx = (float)(ox + (i + 0.5) * inv - 0.5);
      int a0 = (int)floorf(fx);
      tx[i] = (unsigned)((fx - a0) * 256);
      int b0 = a0 + 1;
      if (a0 < 0) a0 = 0; if (a0 >= w) a0 = w - 1;
      if (b0 < 0) b0 = 0; if (b0 >= w) b0 = w - 1;
      xa[i] = a0; xb[i] = b0;
    }
    for (int j = 0; j < tch; j++) {
      float fy = (float)(oy + (j + 0.5) * inv - 0.5);
      int a0 = (int)floorf(fy);
      ty[j] = (unsigned)((fy - a0) * 256);
      int b0 = a0 + 1;
      if (a0 < 0) a0 = 0; if (a0 >= h) a0 = h - 1;
      if (b0 < 0) b0 = 0; if (b0 >= h) b0 = h - 1;
      ya[j] = a0; yb[j] = b0;
    }
  }

  int nt = 0;   /* next thumbnail row to emit (yb[] is non-decreasing) */
  for (int y = 0; y < h; y++) {
    const uint32_t *srow = (const uint32_t *)(im->data + (size_t)y * im->bytes_per_line);
    uint32_t *drow = a->snap.px + (size_t)y * w;
    for (int x = 0; x < w; x++) drow[x] = srow[x] | 0xff000000u;
    if (want_thumb) {
      while (nt < tch && yb[nt] <= y) {
        const uint32_t *r0 = a->snap.px + (size_t)ya[nt] * w, *r1 = a->snap.px + (size_t)yb[nt] * w;
        uint32_t *trow = th.px + (size_t)nt * tcw;
        unsigned t_y = ty[nt];
        for (int i = 0; i < tcw; i++) trow[i] = bilerp(r0, r1, xa[i], xb[i], tx[i], t_y);
        nt++;
      }
    }
  }
  if (want_thumb) {
    thumb_bake_alpha(&th, CARD_R_PX);
    a->thumb = th;
    free(xa); free(xb); free(tx); free(ya); free(yb); free(ty);
  }
  return 1;
}

static void grab_snapshot(App *a) {
  if (a->builtin) { settings_snapshot(a); return; }
  if (a->nwin == 0) return;
  Window w = a->wins[a->nwin - 1];
  XWindowAttributes wa;
  if (!XGetWindowAttributes(dpy, w, &wa) || wa.map_state != IsViewable) return;

  const uint16_t one = 1;
  const int native_order = *(const uint8_t *)&one ? LSBFirst : MSBFirst;

  /* ---- fast path: XShmGetImage into the persistent segment ---- */
  if (wa.depth == depth && snap_shm_init() && (size_t)wa.width * wa.height * 4 <= snap_shm_cap) {
    XImage *sim = XShmCreateImage(dpy, DefaultVisual(dpy, scr), depth, ZPixmap, NULL, &snap_si, wa.width, wa.height);
    if (sim) {
      if (sim->bits_per_pixel == 32 && sim->byte_order == native_order) {
        sim->data = snap_si.shmaddr;
        XSync(dpy, False);
        XErrorHandler old_h = XSetErrorHandler(xerr_handler);
        xerr_trap = 0;
        Status ok = XShmGetImage(dpy, w, sim, 0, 0, AllPlanes);
        XSync(dpy, False);
        XSetErrorHandler(old_h);
        int good = ok && !xerr_trap;
        if (good) good = snap_fused(a, sim, wa.width, wa.height);
        sim->data = NULL; XDestroyImage(sim);   /* the segment itself stays attached */
        if (good) return;
        if (xerr_trap) return;                  /* window went away: nothing to grab */
      } else {
        XDestroyImage(sim);
      }
    }
  }

  /* ---- fallback: plain XGetImage (no MIT-SHM, odd pixel format, oversized window) ---- */
  XSync(dpy, False);
  XErrorHandler old_h = XSetErrorHandler(xerr_handler);
  xerr_trap = 0;

  XImage *im = XGetImage(dpy, w, 0, 0, wa.width, wa.height, AllPlanes, ZPixmap);

  XSync(dpy, False);
  XSetErrorHandler(old_h);

  if (xerr_trap || !im) {
    if (im) XDestroyImage(im);
    return;
  }

  /* Reuse the existing buffer when the window size is unchanged. */
  if (a->snap.px && (a->snap.w != wa.width || a->snap.h != wa.height)) snap_drop(a);
  if (!a->snap.px) a->snap = surf_new_raw(wa.width, wa.height);
  if (!a->snap.px) { XDestroyImage(im); return; }

  if (im->bits_per_pixel == 32 && im->byte_order == native_order) {
    for (int y = 0; y < wa.height; y++) {
      const uint32_t *srow = (const uint32_t *)(im->data + (size_t)y * im->bytes_per_line);
      uint32_t *drow = a->snap.px + (size_t)y * wa.width;
      for (int x = 0; x < wa.width; x++) drow[x] = srow[x] | 0xff000000u;
    }
  } else {
    for (int y = 0; y < wa.height; y++)
      for (int x = 0; x < wa.width; x++) {
        unsigned long p = XGetPixel(im, x, y);
        a->snap.px[(size_t)y * wa.width + x] = 0xff000000 | (uint32_t)p;
      }
  }
  XDestroyImage(im);
  surf_free(&a->thumb);   /* content changed: drop the stale card thumbnail */
}

static void activate_switcher(void) {
  Rect stage = live_anchor_rect();
  App *held = active_app;
  if (held) { held->state = ST_BG; if (held->builtin) builtin_leave(held); grab_snapshot(held); app_lower(held); }

  int running_indices[MAX_APPS], nrun = 0;
  for (int i = 0; i < app_count; i++) if (apps[i].state != ST_CLOSED) running_indices[nrun++] = i;
  if (nrun == 0) { active_app = NULL; if (gesture) { free(gesture); gesture = NULL; } return; }

  switcher.ncards = nrun;
  for (int i = 0; i < nrun; i++) {
    switcher.cards[i].app = &apps[running_indices[i]];
    sp_init(&switcher.cards[i].vy_sp, 0, 300, 26, 0.05);
  }
  switcher.open = 1;
  double cw, ch, left_margin, max_scroll;
  switcher_metrics(&cw, &ch, &left_margin, &max_scroll);
  int active_idx = 0;
  if (held) {
    for (int i = 0; i < nrun; i++) if (switcher.cards[i].app == held) { active_idx = i; break; }
  } else active_idx = (nrun - 1) / 2;

  double stage_cx = stage.x + stage.w / 2;
  double initial_scroll = (left_margin + active_idx * (cw + GAP_PX) + cw / 2) - stage_cx;
  sp_snap(&switcher_scroll_spring, initial_scroll);
  switcher_scroll_spring.target = initial_scroll;

  switcher.entry.active_idx = active_idx;
  switcher.entry.stage = stage;
  switcher.entry.t0 = now_ms();
  switcher.entry.dur = 260.0;

  active_app = NULL;
  if (gesture) { free(gesture); gesture = NULL; }
  home_scale_spring.target = 0.92;
}

/* ------------------------------------------------------------------------ */
/* GESTURE / TOUCH ENGINE                                                    */
/* ------------------------------------------------------------------------ */
typedef struct { double x, y, t; } TouchSample;
static TouchSample touch_history[16];
static int touch_cnt = 0;

static void record_touch(double x, double y) {
  if (touch_cnt < 16) touch_history[touch_cnt++] = (TouchSample){ x, y, now_ms() };
  else {
    memmove(&touch_history[0], &touch_history[1], 15 * sizeof(TouchSample));
    touch_history[15] = (TouchSample){ x, y, now_ms() };
  }
}
static void compute_velocity(double *vx, double *vy) {
  if (touch_cnt < 2) { *vx = *vy = 0; return; }
  TouchSample f = touch_history[0], l = touch_history[touch_cnt - 1];
  double dt = (l.t - f.t) / 1000.0;
  if (dt <= 0.001) { *vx = *vy = 0; return; }
  *vx = (l.x - f.x) / dt; *vy = (l.y - f.y) / dt;
}

static int on_pointer_down(double x, double y) {
  int eff_h = kbd_active ? (int)lround(H * 0.75) : H;
  
  if (kbd_active && y >= eff_h) {
    if (active_app && active_app->nwin > 0) {
      XSetInputFocus(dpy, active_app->wins[active_app->nwin - 1], RevertToPointerRoot, CurrentTime);
    } else if (active_app && active_app->builtin) {
      XSetInputFocus(dpy, shell, RevertToPointerRoot, CurrentTime);
    }
    return 0;
  }

  touch_cnt = 0; record_touch(x, y);
  double edge = S(36);

  if (switcher.open) {
    if (y > eff_h - edge) {
      gesture = calloc(1, sizeof *gesture);
      strcpy(gesture->mode, "switcherExit");
      gesture->start_y = y; gesture->px = x; gesture->py = y;
      return 1;
    }
    switcher_scroll_spring.target = switcher_scroll_spring.x;
    switcher_scroll_spring.v = 0;
    int hit_idx = -1;
    for (int i = 0; i < switcher.ncards; i++) {
      Rect b = card_render_box(i);
      if (x >= b.x && x <= b.x + b.w && y >= b.y && y <= b.y + b.h) { hit_idx = i; break; }
    }
    gesture = calloc(1, sizeof *gesture);
    strcpy(gesture->mode, "switcherPan");
    gesture->px = x; gesture->py = y; gesture->entry_x = x; gesture->entry_y = y;
    gesture->last_x = x; gesture->last_y = y; gesture->grab_idx = hit_idx;
    gesture->pan_start_scroll = switcher_scroll_spring.x;
    return 1;
  }

  int has_running = 0;
  for (int i = 0; i < app_count; i++) if (apps[i].state != ST_CLOSED) has_running = 1;
  if (y > eff_h - edge && (active_app || has_running)) {
    gesture = calloc(1, sizeof *gesture);
    strcpy(gesture->mode, "drag");
    gesture->rect = active_app ? full_rect() : (Rect){0};
    gesture->start_x = x; gesture->start_y = y; gesture->px = x; gesture->py = y;
    gesture->last_move_t = now_ms(); gesture->last_move_x = x; gesture->last_move_y = y;
    return 1;
  }

  if (!active_app) {
    for (int i = 0; i < app_count; i++) {
      App *a = &apps[i];
      Rect r = icon_rect(a);
      if (x >= r.x && x <= r.x + r.w && y >= r.y && y <= r.y + r.h) {
        a->press_scale.target = 0.88;
        gesture = calloc(1, sizeof *gesture);
        strcpy(gesture->mode, "iconPress");
        gesture->app = a;
        return 1;
      }
    }

    gesture = calloc(1, sizeof *gesture);
    strcpy(gesture->mode, "pagePan");
    gesture->start_x = x; gesture->start_y = y; gesture->px = x; gesture->py = y;
    gesture->page_start_offset = page_offset_spring.x;
    return 1;
  }

  if (active_app && active_app->builtin) {      /* touch inside a compositor-drawn app */
    settings_pointer_down(x, y);
    builtin_touch = 1;
    return 1;
  }

  return 0;
}

static void on_pointer_move(double x, double y) {
  if (builtin_touch) { record_touch(x, y); settings_pointer_move(x, y); return; }
  if (!gesture) return;
  record_touch(x, y);
  gesture->px = x; gesture->py = y;
  int eff_h = kbd_active ? (int)lround(H * 0.75) : H;

  if (strcmp(gesture->mode, "iconPress") == 0) {
    App *a = gesture->app;
    Rect r = icon_rect(a);
    int hit = (x >= r.x && x <= r.x + r.w && y >= r.y && y <= r.y + r.h);
    if (!hit) { a->press_scale.target = 1.0; free(gesture); gesture = NULL; }
    return;
  }
  if (strcmp(gesture->mode, "pagePan") == 0) {
    double dx = gesture->start_x - x;
    double target_offset = gesture->page_start_offset + dx;
    double min_off = 0;
    double max_off = (total_pages - 1) * W;
    if (target_offset < min_off) target_offset = min_off + (target_offset - min_off) * 0.3;
    if (target_offset > max_off) target_offset = max_off + (target_offset - max_off) * 0.3;

    page_offset_spring.target = target_offset;
    page_offset_spring.x = target_offset;
    return;
  }
  if (strcmp(gesture->mode, "switcherPan") == 0) {
    gesture->last_x = x; gesture->last_y = y;
    double tdx = x - gesture->entry_x, tdy = y - gesture->entry_y;
    if (!gesture->axis && hypot(tdx, tdy) > S(10)) gesture->axis = fabs(tdx) > fabs(tdy) ? 'x' : 'y';
    if (gesture->axis == 'x') {
      switcher_scroll_spring.target = gesture->pan_start_scroll - tdx;
      switcher_scroll_spring.x = switcher_scroll_spring.target;
      switcher_scroll_spring.v = 0;
    } else if (gesture->axis == 'y' && gesture->grab_idx >= 0 && gesture->grab_idx < switcher.ncards) {
      switcher.cards[gesture->grab_idx].vy_sp.target = fmin(0, tdy);
    }
    return;
  }
  if (strcmp(gesture->mode, "switcherExit") == 0) {
    gesture->dy = fmax(0, gesture->start_y - y);
    return;
  }
  if (strcmp(gesture->mode, "drag") != 0) return;

  double drag_up = eff_h * 0.55;
  double raw_dy = gesture->start_y - y;
  gesture->dy = clampd(raw_dy, 0, drag_up);
  double t = gesture->dy / drag_up;
  double dx = x - gesture->start_x;

  if (active_app) {
    Rect full = full_rect(), icon = icon_rect(active_app);
    double w = full.w + (icon.w - full.w) * t;
    double h = full.h + (icon.h - full.h) * t;
    double cx = (full.x + full.w / 2) + dx;
    double cy = clampd((full.y + full.h / 2) - gesture->dy * 0.4, h / 2, eff_h - h / 2);
    gesture->rect = (Rect){ cx - w / 2, cy - h / 2, w, h, icon.r * t };
    rs_target(&win_spring, gesture->rect);
  } else {
    double cw, ch; card_size(&cw, &ch);
    double w = cw * t, h = ch * t;
    double cy = clampd(gesture->start_y - gesture->dy * 0.4, h / 2, eff_h - h / 2);
    gesture->rect = (Rect){ x - w / 2, cy - h / 2, w, h, CARD_R_PX * t };
    rs_target(&win_spring, gesture->rect);
  }

  if (hypot(x - gesture->last_move_x, y - gesture->last_move_y) > S(12)) {
    gesture->last_move_t = now_ms();
    gesture->last_move_x = x; gesture->last_move_y = y;
  }
}

static void on_pointer_up(double x, double y) {
  if (builtin_touch) {
    builtin_touch = 0;
    double bvx, bvy; compute_velocity(&bvx, &bvy);
    settings_pointer_up(x, y, bvy);
    return;
  }
  if (!gesture) return;
  double vx, vy; compute_velocity(&vx, &vy);
  int eff_h = kbd_active ? (int)lround(H * 0.75) : H;

  if (strcmp(gesture->mode, "iconPress") == 0) {
    App *a = gesture->app;
    a->press_scale.target = 1.0;
    launch(a);
    free(gesture); gesture = NULL;
    return;
  }
  if (strcmp(gesture->mode, "pagePan") == 0) {
    double dx = gesture->start_x - x;
    if (dx > S(60) || vx < -350) {
      if (current_page < total_pages - 1) current_page++;
    } else if (dx < -S(60) || vx > 350) {
      if (current_page > 0) current_page--;
    }
    page_offset_spring.target = current_page * W;
    free(gesture); gesture = NULL;
    return;
  }
  if (strcmp(gesture->mode, "switcherExit") == 0) {
    if (gesture->dy > S(60)) close_switcher_to_home();
    free(gesture); gesture = NULL;
    return;
  }
  if (strcmp(gesture->mode, "switcherPan") == 0) {
    if (gesture->axis == 'y' && gesture->grab_idx >= 0 && gesture->grab_idx < switcher.ncards) {
      int idx = gesture->grab_idx; Card *c = &switcher.cards[idx];
      double raw_dy = gesture->py - gesture->entry_y;
      if (raw_dy < -S(70) || vy < -500) {
        terminate(c->app);
        memmove(&switcher.cards[idx], &switcher.cards[idx + 1], (switcher.ncards - idx - 1) * sizeof(Card));
        switcher.ncards--;
        if (switcher.ncards == 0) close_switcher_to_home();
      } else c->vy_sp.target = 0;
    } else if (gesture->axis == 'x') {
      double cw, ch, left_margin, max_scroll;
      switcher_metrics(&cw, &ch, &left_margin, &max_scroll);
      double step = cw + GAP_PX;
      double target_scroll = round(switcher_scroll_spring.target / step) * step;
      if (vx < -350) target_scroll += step;
      if (vx > 350) target_scroll -= step;
      switcher_scroll_spring.target = clampd(target_scroll, 0, max_scroll);
    } else if (!gesture->axis && gesture->grab_idx >= 0 && gesture->grab_idx < switcher.ncards) {
      resume(switcher.cards[gesture->grab_idx].app, card_render_box(gesture->grab_idx));
    }
    free(gesture); gesture = NULL;
    return;
  }
  if (strcmp(gesture->mode, "drag") == 0) {
    if (gesture->hold_fired) activate_switcher();
    else if (active_app) {
      double t = gesture->dy / (eff_h * 0.55);
      if (t > 0.35 || vy < -300) minimize_to_icon(active_app, vx, vy);
      else { 
        Rect target = full_rect();
        target.h = eff_h - (int)lround(S(12));
        rs_target(&win_spring, target); 
        app_raise(active_app); 
        free(gesture); gesture = NULL; 
      }
    } else { free(gesture); gesture = NULL; }
  }
}

/* ------------------------------------------------------------------------ */
/* COMPOSITOR & RENDERER                                                    */
/* ------------------------------------------------------------------------ */
/* ---- Card thumbnails -------------------------------------------------------
   The switcher shows every card at one fixed size (cw x ch) while scrolling, so the
   full-resolution snapshot is scaled down ONCE into `thumb`. Per frame a card is then
   a masked row copy instead of a bilinear resample of a multi-MB source.
   thumb pixels: RGB = cover-cropped content (also under the corner cut-outs, so later
   resampling never fades to black), top byte = rounded-corner coverage 0..255. */
static int thumb_budget;   /* at most this many thumbnails are built per frame (set in render_frame) */

static void thumb_bake_alpha(Surf *th, double r) {
  RR rr = rr_make(0, 0, th->w, th->h, r);
  for (int y = 0; y < th->h; y++) {
    uint32_t *row = th->px + (size_t)y * th->w;
    int sl = 1, sr = 0;
    int have = solid_span(&rr, NULL, y, &sl, &sr);
    int x = 0;
    while (x < th->w) {
      if (have && x >= sl && x <= sr) {
        int e = sr < th->w - 1 ? sr : th->w - 1;
        for (int i = x; i <= e; i++) row[i] |= 0xff000000u;
        x = e + 1;
        continue;
      }
      float cov = rr_cov(&rr, x + 0.5f, y + 0.5f);
      row[x] = (row[x] & 0xffffff) | ((uint32_t)(cov * 255.f + 0.5f) << 24);
      x++;
    }
  }
}

static int ensure_thumb(App *a, int cw, int ch) {
  if (!a->snap.px || cw < 2 || ch < 2) return 0;
  if (a->thumb.px && a->thumb.w == cw && a->thumb.h == ch) return 1;
  if (thumb_budget <= 0) return 0;
  thumb_budget--;
  surf_free(&a->thumb);
  Surf th = surf_new(cw, ch);
  if (!th.px) return 0;
  Rect r = { 0, 0, cw, ch, 0 };
  draw_image(&th, &a->snap, &r, NULL);
  thumb_bake_alpha(&th, CARD_R_PX);
  a->thumb = th;
  return 1;
}

/* Integer-positioned blit of a baked thumbnail. Rows inside the straight part of the
   rounded rect are a masked copy (vectorises); only the corner rows look at alpha. */
static void blit_thumb(Surf *t, const Surf *th, int x, int y) {
  const int rc = (int)ceil(CARD_R_PX) + 1;
  for (int j = 0; j < th->h; j++) {
    int dy = y + j;
    if (dy < 0 || dy >= t->h) continue;
    int x0 = x < 0 ? 0 : x;
    int x1 = x + th->w > t->w ? t->w : x + th->w;
    if (x1 <= x0) continue;
    const uint32_t *s = th->px + (size_t)j * th->w + (x0 - x);
    uint32_t *d = t->px + (size_t)dy * t->w + x0;
    int n = x1 - x0;
    if (j >= rc && j < th->h - rc) {
      for (int i = 0; i < n; i++) d[i] = s[i] & 0xffffff;
    } else {
      for (int i = 0; i < n; i++) {
        uint32_t p = s[i];
        unsigned a = p >> 24;
        if (a == 255) d[i] = p & 0xffffff;
        else if (a) d[i] = mix32(d[i], p, a + (a >> 7));
      }
    }
  }
}

/* use_thumb: only the switcher cards pass 1. The thumbnail is used when the destination is
   no larger than it (so nothing is ever upscaled); larger (entry animation start) falls back
   to the full-resolution snapshot. */
static void draw_app_preview(Surf *t, App *app, Rect r, int use_thumb) {
  RR clip = rr_make(r.x, r.y, r.w, r.h, r.r);
  if (app->snap.px) {
    const Surf *src = &app->snap;
    if (use_thumb && app->thumb.px && r.w <= app->thumb.w + 1 && r.h <= app->thumb.h + 1) src = &app->thumb;
    draw_image(t, src, &r, &clip);
    return;
  }
  fill_rr(t, &clip, app->color, 1.0, NULL);
  RR bar = rr_make(r.x, r.y, r.w, fmin(r.h, S(60)), 0);
  fill_rr(t, &bar, 0x000000, 0.18, &clip);
  draw_text(t, &F_hdr, app->name, r.x + S(20), r.y + S(38), 1.0, 0, 0xffffff, 1.0, 0, &clip);
  double bar_w = fmin(S(134), r.w * 0.35);
  RR homebar = rr_make(r.x + (r.w - bar_w) / 2, r.y + r.h - S(12), bar_w, S(5), S(2.5));
  fill_rr(t, &homebar, 0xffffff, 0.9, &clip);
}

/* Home layer: dock, icons, labels, page dots. With use_target every spring is taken at its
   resting value, which makes the output stable so it can be cached behind the switcher. */
static void draw_home(Surf *t, double hs, double p_off, double home_alpha, int use_target) {
  int eff_h = t->h;
  double hcx = W / 2, hcy = eff_h / 2;

  RR dbox = rr_make(hcx + (dock_box.x - hcx) * hs, hcy + (dock_box.y - hcy) * hs,
                    dock_box.w * hs, dock_box.h * hs, dock_box.r * hs);
  fill_rr(t, &dbox, 0xffffff, 0.15 * home_alpha, NULL);

  for (int i = 0; i < app_count; i++) {
    App *a = &apps[i];
    if (active_app == a && gesture) continue;
    double ps = (use_target ? a->press_scale.target : a->press_scale.x) * hs;
    Rect r = a->icon;

    if (!a->is_dock) {
      r.x -= p_off;
    }

    if (r.x + r.w < -100 || r.x > W + 100) continue;

    double cx = hcx + (r.x + r.w / 2 - hcx) * hs;
    double cy = hcy + (r.y + r.h / 2 - hcy) * hs;
    double w = r.w * ps, h = r.h * ps;
    RR icon = rr_make(cx - w / 2, cy - h / 2, w, h, r.r * ps);
    fill_rr(t, &icon, a->color, home_alpha, NULL);
    if (a->builtin) settings_icon(t, cx, cy, w, home_alpha);

    draw_text(t, &F_lbl, a->name, cx, cy + h / 2 + S(12) * hs, hs, 1, 0xf2eefc, home_alpha, 0, NULL);

    if (a->state == ST_BG) {
      RR dot = rr_make(cx + w / 2 - S(5) * hs, cy - h / 2 + S(5) * hs, S(7) * hs, S(7) * hs, S(3.5) * hs);
      fill_rr(t, &dot, 0xffffff, home_alpha, NULL);
    }
  }

  if (total_pages > 1) {
    double dot_r = S(3.5);
    double dot_gap = S(12);
    double dots_w = total_pages * (dot_r * 2) + (total_pages - 1) * dot_gap;
    double dots_x = (W - dots_w) / 2;
    double dots_y = dock_box.y - S(18);

    double active_page_exact = p_off / (double)W;

    for (int p = 0; p < total_pages; p++) {
      double dx = dots_x + p * (dot_r * 2 + dot_gap) + dot_r;
      double dist = fabs(p - active_page_exact);
      double a = clampd(1.0 - dist * 0.6, 0.25, 0.9);
      RR dot = rr_make(dx - dot_r, dots_y - dot_r, dot_r * 2, dot_r * 2, dot_r);
      fill_rr(t, &dot, 0xffffff, a * home_alpha, NULL);
    }
  }
}

/* ---- Switcher backdrop cache ----------------------------------------------
   While the switcher is open and the home springs have settled, the layer behind the cards
   (gradient + dock + icons + labels + dots [+ the 0.55 dim]) is identical every frame.
   bd_home = home layer only (used while the exit-swipe changes the dim),
   bd_dim  = home layer + full dim (the common case: one memcpy per frame). */
typedef struct { double hs, poff; double ps[MAX_APPS]; int st[MAX_APPS]; int w, h, n, pages; } BdKey;
static Surf bd_home, bd_dim;
static BdKey bd_key;
static unsigned bd_gen = 1;   /* bumped whenever bd_dim is rebuilt; framebuffers compare against it */

static void draw_hud(Surf *t) {
  char running_str[256] = "", hud_str[512];
  for (int i = 0; i < app_count; i++) {
    if (apps[i].state != ST_CLOSED) {
      char tmp[32]; snprintf(tmp, sizeof tmp, "%s:%c ", apps[i].name, apps[i].state == ST_FG ? 'F' : 'B');
      strcat(running_str, tmp);
    }
  }
  snprintf(hud_str, sizeof hud_str, "%s%s%s",
           active_app ? active_app->name : "",
           active_app ? " -> " : "",
           switcher.open ? "SWITCHER " : (running_str[0] ? running_str : "idle"));

  draw_text(t, &F_hud, "Summerboard", S(12), S(22), 1.0, 0, 0x9d94b8, 1.0, 0, NULL);
  draw_text(t, &F_hud, HUD_TITLE_VER, S(82), S(22), 1.0, 0, 0xf2eefc, 1.0, 0, NULL);
  draw_text(t, &F_hud, hud_str, W - S(12), S(22), 1.0, 2, 0x9d94b8, 1.0, 0, NULL);
}

/* Rows the switcher cards (plus their name labels) can touch this frame, [*y0,*y1). Only valid
   for exit_k == 0 and no entry animation, where card_r == card_render_box(i). */
static void switcher_band(int hgt, int *y0, int *y1) {
  int lo = hgt, hi = -1;
  for (int i = 0; i < switcher.ncards; i++) {
    Rect b = card_render_box(i);
    if (b.x + b.w < 0 || b.x > W) continue;
    int t = (int)floor(b.y - S(14) - S(16)) - 2;       /* label baseline - ascent */
    int bt = (int)ceil(b.y + b.h) + 2;
    if (t < lo) lo = t;
    if (bt > hi) hi = bt;
  }
  if (hi < 0) { *y0 = *y1 = 0; return; }
  if (lo < 0) lo = 0;
  if (hi > hgt) hi = hgt;
  *y0 = lo; *y1 = hi > lo ? hi : lo;
}
static inline void row_hull(int a0, int a1, int c0, int c1, int *o0, int *o1) {
  if (a1 <= a0) { *o0 = c0; *o1 = c1; return; }
  if (c1 <= c0) { *o0 = a0; *o1 = a1; return; }
  *o0 = a0 < c0 ? a0 : c0; *o1 = a1 > c1 ? a1 : c1;
}

static int home_settled(void) {
  if (fabs(home_scale_spring.x - home_scale_spring.target) > 0.004 || fabs(home_scale_spring.v) > 0.1) return 0;
  if (fabs(page_offset_spring.x - page_offset_spring.target) > 0.5 || fabs(page_offset_spring.v) > 5.0) return 0;
  for (int i = 0; i < app_count; i++) {
    const Spring *p = &apps[i].press_scale;
    if (fabs(p->x - p->target) > 0.004 || fabs(p->v) > 0.1) return 0;
  }
  return 1;
}

static int backdrop_ready(int w, int h, const Surf *bg) {
  BdKey k;
  memset(&k, 0, sizeof k);
  k.hs = home_scale_spring.target * (1.0 - 0.12);   /* home_recede == 1 while the switcher is open */
  k.poff = page_offset_spring.target;
  for (int i = 0; i < app_count; i++) { k.ps[i] = apps[i].press_scale.target; k.st[i] = apps[i].state; }
  k.w = w; k.h = h; k.n = app_count; k.pages = total_pages;
  if (bd_valid && bd_home.w == w && bd_home.h == h && !memcmp(&k, &bd_key, sizeof k)) return 1;

  if (bd_home.w != w || bd_home.h != h) {
    surf_free(&bd_home); surf_free(&bd_dim);
    bd_home = surf_new_raw(w, h); bd_dim = surf_new_raw(w, h);
    if (!bd_home.px || !bd_dim.px) { surf_free(&bd_home); surf_free(&bd_dim); bd_valid = 0; return 0; }
  }
  size_t bytes = (size_t)w * h * 4;
  memcpy(bd_home.px, bg->px, bytes);
  draw_home(&bd_home, k.hs, k.poff, 1.0 - 0.6, 1);
  memcpy(bd_dim.px, bd_home.px, bytes);
  fill_rect(&bd_dim, 0, 0, w, h, 0x000000, 0.55);
  draw_hud(&bd_dim);   /* HUD is constant while the switcher is open, so it lives in the cache too */
  bd_key = k; bd_valid = 1; bd_gen++;
  return 1;
}

static void render_frame(Surf *t, FBuf *f, int *pres_y0, int *pres_y1) {
  int eff_h = t->h;
  /* The gradient background never changes: build it once per size, then just copy it. */
  static Surf bg;
  if (!bg.px || bg.w != t->w || bg.h != t->h) {
    surf_free(&bg);
    bg = surf_new_raw(t->w, t->h);
    if (bg.px)
      for (int y = 0; y < eff_h; y++) {
        double k = (double)y / eff_h;
        uint32_t c = mix32(0x1c1730, 0x0a0814, (unsigned)(k * 256));
        uint32_t *row = bg.px + (size_t)y * bg.w;
        for (int x = 0; x < bg.w; x++) row[x] = c;
      }
  }
  thumb_budget = 1;

  Rect rect = rs_val(&win_spring);
  double home_cover = 0;
  if (active_app) {
    Rect full = full_rect();
    home_cover = clampd((rect.w * rect.h) / (full.w * full.h), 0, 1);
  }
  double home_recede = 0;
  if (switcher.open) home_recede = 1.0;
  else if (gesture && strcmp(gesture->mode, "drag") == 0 && gesture->hold_fired) {
    home_recede = appear_ease(clampd((now_ms() - gesture->appear_t0) / APPEAR_DUR, 0, 1));
  }

  double home_alpha = (1.0 - home_cover) * (1.0 - 0.6 * home_recede);

  double exit_k = 0;
  if (switcher.open && gesture && strcmp(gesture->mode, "switcherExit") == 0) exit_k = clampd(gesture->dy / S(170), 0, 1);

  size_t fb_bytes = (size_t)t->w * t->h * 4;
  int dim_done = 0, hud_done = 0, band_mode = 0;
  *pres_y0 = 0; *pres_y1 = t->h;
  if (bg.px && switcher.open && !active_app && home_settled() && backdrop_ready(t->w, t->h, &bg)) {
    if (exit_k == 0) {
      dim_done = 1; hud_done = 1;
      band_mode = switcher.entry.dur <= 0;
      if (!band_mode) {
        memcpy(t->px, bd_dim.px, fb_bytes);
      } else {
        int b0, b1;
        switcher_band(t->h, &b0, &b1);
        if (f->bd_ok && f->bd_gen == bd_gen) {
          /* Everything outside the rows the cards covered last time here is already backdrop. */
          int c0, c1;
          row_hull(b0, b1, f->b0, f->b1, &c0, &c1);
          if (c1 > c0) memcpy(t->px + (size_t)c0 * t->w, bd_dim.px + (size_t)c0 * t->w, (size_t)(c1 - c0) * t->w * 4);
        } else {
          memcpy(t->px, bd_dim.px, fb_bytes);
        }
        f->bd_ok = 1; f->bd_gen = bd_gen; f->b0 = b0; f->b1 = b1;
        /* The window shows the previous frame: only rows either frame's cards covered differ. */
        if (!force_full_present && pres_band_ok && pres_gen == bd_gen)
          row_hull(b0, b1, pres_b0, pres_b1, pres_y0, pres_y1);
        pres_band_ok = 1; pres_gen = bd_gen; pres_b0 = b0; pres_b1 = b1;
      }
    } else memcpy(t->px, bd_home.px, fb_bytes);
  } else {
    if (bg.px) memcpy(t->px, bg.px, fb_bytes);
    if (home_alpha > 0.01) {
      double hs = home_scale_spring.x * (1.0 - 0.12 * home_recede);
      draw_home(t, hs, page_offset_spring.x, home_alpha, 0);
    }
  }

  if (gesture && strcmp(gesture->mode, "drag") == 0 && gesture->hold_fired && !switcher.open) {
    Rect anchor = live_anchor_rect();
    int list[MAX_APPS], nlist = 0;
    for (int i = 0; i < app_count; i++) if (apps[i].state != ST_CLOSED) list[nlist++] = i;
    int active_idx = 0;
    if (active_app) {
      for (int i = 0; i < nlist; i++) if (&apps[list[i]] == active_app) { active_idx = i; break; }
    } else active_idx = (nlist - 1) / 2;

    double ak = appear_ease(clampd((now_ms() - gesture->appear_t0) / APPEAR_DUR, 0, 1));
    fill_rect(t, 0, 0, W, eff_h, 0x000000, 0.55);

    for (int i = 0; i < nlist; i++) {
      App *a = &apps[list[i]];
      if (a == active_app) continue;
      double live_x = anchor.x + (i - active_idx) * (anchor.w + GAP_PX);
      double off_x = i < active_idx ? -anchor.w : W;
      Rect b = { off_x + (live_x - off_x) * ak, anchor.y, anchor.w, anchor.h, anchor.r };
      draw_app_preview(t, a, b, 0);
    }
  }

  if (active_app && (active_app->builtin || gesture || switcher.open || win_spring.w.x < W - 2)) {
    if (rect.w > 1 && rect.h > 1) {
      if (active_app->builtin) {
        builtin_paint(t, active_app, rect);   /* no X window: the compositor draws it live, even at rest */
      } else {
        draw_app_preview(t, active_app, rect, 0);
        sync_window_geometry(active_app, rect);
      }
    }
  }

  if (switcher.open) {
    if (!dim_done) fill_rect(t, 0, 0, W, eff_h, 0x000000, 0.55 * (1.0 - 0.5 * exit_k));

    double cw, ch; card_size(&cw, &ch);
    int tcw = (int)lround(cw), tch = (int)lround(ch);
    double sc = 1.0 - 0.12 * exit_k;
    int near_idx[MAX_APPS], nnear = 0;

    for (int i = 0; i < switcher.ncards; i++) {
      Card *c = &switcher.cards[i];
      Rect b = card_render_box(i);
      double ccx = b.x + b.w / 2, ccy = b.y + b.h / 2;
      double w = b.w * sc, h = b.h * sc;
      Rect card_r = { ccx - w / 2, ccy - h / 2 + exit_k * S(26), w, h, b.r };

      /* Off-screen cards cost nothing; the ones about to scroll in get their thumbnail prepared. */
      if (card_r.x + card_r.w < 0 || card_r.x > W) {
        if (card_r.x + card_r.w > -card_r.w && card_r.x < W + card_r.w) near_idx[nnear++] = i;
        continue;
      }

      int have_thumb = ensure_thumb(c->app, tcw, tch);
      if (have_thumb && exit_k == 0 && fabs(card_r.w - cw) < 0.01 && fabs(card_r.h - ch) < 0.01)
        blit_thumb(t, &c->app->thumb, (int)lround(card_r.x), (int)lround(card_r.y));
      else
        draw_app_preview(t, c->app, card_r, 1);
      draw_text(t, &F_card, c->app->name, ccx, card_r.y - S(14), 1.0, 1, 0xffffff, 1.0 - exit_k, 0, NULL);
    }
    for (int k = 0; k < nnear && thumb_budget > 0; k++)
      ensure_thumb(switcher.cards[near_idx[k]].app, tcw, tch);
  }

  if (!band_mode) { f->bd_ok = 0; pres_band_ok = 0; }
  force_full_present = 0;

  if (active_app && !switcher.open) {
    double bar_w = S(134);
    RR homebar = rr_make((W - bar_w) / 2, eff_h - S(10), bar_w, S(5), S(2.5));
    fill_rr(t, &homebar, 0xffffff, 0.9, NULL);
  }

  if (!hud_done) draw_hud(t);
}

/* ------------------------------------------------------------------------ */
/* WM EVENT LOOP                                                            */
/* ------------------------------------------------------------------------ */
static App *find_app_by_win(Window w) {
  for (int i = 0; i < app_count; i++)
    for (int j = 0; j < apps[i].nwin; j++)
      if (apps[i].wins[j] == w) return &apps[i];
  return NULL;
}

static void on_map_request(XMapRequestEvent *e) {
  XSelectInput(dpy, e->window, StructureNotifyMask | PropertyChangeMask);
  
  XClassHint ch;
  int is_keyboard_window = 0;
  if (XGetClassHint(dpy, e->window, &ch)) {
    if ((ch.res_name && strstr(ch.res_name, "matchbox-keyboard")) ||
        (ch.res_class && strstr(ch.res_class, "Matchbox-keyboard"))) {
      is_keyboard_window = 1;
    }
    if (ch.res_name) XFree(ch.res_name);
    if (ch.res_class) XFree(ch.res_class);
  }

  if (is_keyboard_window || (kbd_active && kbd_win == None)) {
    kbd_win = e->window;
    int kbd_y = (int)lround(H * 0.75);
    int kbd_h = H - kbd_y;

    Atom wm_type = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE", False);
    Atom wm_type_input = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_INPUT_WINDOW", False);
    XChangeProperty(dpy, e->window, wm_type, XA_ATOM, 32, PropModeReplace, (unsigned char *)&wm_type_input, 1);

    XMoveResizeWindow(dpy, e->window, 0, kbd_y, W, kbd_h);
    XMapWindow(dpy, e->window);
    XRaiseWindow(dpy, e->window);
    
    if (active_app && active_app->nwin > 0) {
      XSetInputFocus(dpy, active_app->wins[active_app->nwin - 1], RevertToPointerRoot, CurrentTime);
    }
    return;
  }

  App *a = find_app_by_win(e->window);
  if (!a) {
    if (active_app && active_app->builtin) {   /* built-ins own no windows: send them to the background */
      App *b = active_app;
      b->state = ST_BG; builtin_leave(b); grab_snapshot(b);
      active_app = NULL;
    }
    a = active_app ? active_app : &apps[0];
    if (a->nwin < MAX_WINS) a->wins[a->nwin++] = e->window;
  }
  int eff_h = kbd_active ? (int)lround(H * 0.75) : H;
  
  int target_h = eff_h - (int)lround(S(12));

  XMoveResizeWindow(dpy, e->window, 0, 0, W, target_h);
  for (int i = 0; i < a->nwin; i++)
    if (a->wins[i] == e->window) { a->sent_win[i] = None; }   /* force next sync to resend */
  a->state = ST_FG;
  active_app = a;
  XMapWindow(dpy, e->window);
  XRaiseWindow(dpy, e->window);
  XSetInputFocus(dpy, e->window, RevertToPointerRoot, CurrentTime);
  
  Rect full = full_rect();
  full.h = target_h;
  rs_snap(&win_spring, full);
}

static void on_configure_request(XConfigureRequestEvent *e) {
  XWindowChanges wc;
  wc.x = e->x;
  wc.y = e->y;
  wc.width = e->width;
  wc.height = e->height;
  wc.border_width = e->border_width;
  wc.sibling = e->above;
  wc.stack_mode = e->detail;

  if (e->window == kbd_win) {
    wc.x = 0;
    wc.y = (int)lround(H * 0.75);
    wc.width = W;
    wc.height = H - wc.y;
  }

  XConfigureWindow(dpy, e->window, e->value_mask, &wc);
}

static void on_unmap_notify(XUnmapEvent *e) {
  if (e->window == kbd_win) {
    kbd_win = None;
    return;
  }
  App *a = find_app_by_win(e->window);
  if (a && a->state == ST_CLOSED) {
    for (int i = 0; i < a->nwin; i++) {
      if (a->wins[i] == e->window) {
        memmove(&a->wins[i], &a->wins[i+1], (a->nwin - i - 1) * sizeof(Window));
        a->nwin--;
        break;
      }
    }
    if (a->nwin == 0 && active_app == a) active_app = NULL;
  }
}

/* ------------------------------------------------------------------------ */
/* IDLE DETECTION                                                           */
/* ------------------------------------------------------------------------ */
static int rs_busy(const RectSpring *s) {
  return sp_busy(&s->x) || sp_busy(&s->y) || sp_busy(&s->w) || sp_busy(&s->h) || sp_busy(&s->r);
}
/* True while something on screen is time-driven and needs continuous frames.
   Purely event-driven changes (touch drags that write springs directly, HUD
   text, map/unmap) are covered by the `dirty` flag instead. */
static int anim_active(void) {
  if (settings_animating()) return 1;
  if (rs_busy(&win_spring)) return 1;
  if (sp_busy(&home_scale_spring) || sp_busy(&page_offset_spring) || sp_busy(&switcher_scroll_spring)) return 1;
  for (int i = 0; i < app_count; i++) if (sp_busy(&apps[i].press_scale)) return 1;
  if (switcher.open) {
    if (switcher.entry.dur > 0) return 1;
    for (int i = 0; i < switcher.ncards; i++) if (sp_busy(&switcher.cards[i].vy_sp)) return 1;
  }
  if (gesture && strcmp(gesture->mode, "drag") == 0) {
    if (!gesture->hold_fired) return 1;                       /* hold timer polling */
    if (now_ms() - gesture->appear_t0 < APPEAR_DUR) return 1; /* appear animation   */
  }
  return 0;
}

static void on_destroy_notify(XDestroyWindowEvent *e) {
  if (e->window == kbd_win) { kbd_win = None; return; }
  App *a = find_app_by_win(e->window);
  if (!a) return;
  for (int i = 0; i < a->nwin; i++) {
    if (a->wins[i] == e->window) {
      memmove(&a->wins[i], &a->wins[i + 1], (a->nwin - i - 1) * sizeof(Window));
      a->nwin--;
      break;
    }
  }
  if (a->nwin > 0) return;

  /* last window of this app is gone: the app quit or crashed on its own */
  a->state = ST_CLOSED;
  snap_drop(a);
  if (active_app == a) {
    active_app = NULL;
    home_scale_spring.target = 1.0;
    if (gesture) { free(gesture); gesture = NULL; }
  }
  if (switcher.open) {
    for (int i = 0; i < switcher.ncards; i++) {
      if (switcher.cards[i].app == a) {
        memmove(&switcher.cards[i], &switcher.cards[i + 1], (switcher.ncards - i - 1) * sizeof(Card));
        switcher.ncards--;
        break;
      }
    }
    if (switcher.ncards == 0) close_switcher_to_home();
  }
}

int main(int argc, char **argv) {
  install_sigchld_reaper();
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "-v")) dbg_on = 1;
    else if (!strcmp(argv[i], "-m")) opt_mock_wifi = 1;   /* Settings: use fake Wi-Fi networks */
    else if (!strcmp(argv[i], "-i") && i + 1 < argc) snprintf(opt_wifi_if, sizeof opt_wifi_if, "%s", argv[++i]);   /* Wi-Fi interface */
  }

  dpy = XOpenDisplay(NULL);
  if (!dpy) { fprintf(stderr, "summerboard: cannot open X display\n"); return 1; }
  scr = DefaultScreen(dpy);
  root = RootWindow(dpy, scr);
  W = DisplayWidth(dpy, scr);
  H = DisplayHeight(dpy, scr);
  depth = DefaultDepth(dpy, scr);
  gc = DefaultGC(dpy, scr);

  UI = fmax(1.0, fmin((double)W / 1024.0, (double)H / 768.0));

  XSetWindowAttributes wa = { .event_mask = SubstructureRedirectMask | SubstructureNotifyMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask };
  XChangeWindowAttributes(dpy, root, CWEventMask, &wa);

  XSync(dpy, False);                     /* surfaces BadAccess if another WM is running */
  XSetErrorHandler(wm_error_handler);

  XSetWindowAttributes swa = { .override_redirect = True, .event_mask = ExposureMask | KeyPressMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask };
  shell = XCreateWindow(dpy, root, 0, 0, W, H, 0, depth, InputOutput, CopyFromParent, CWOverrideRedirect | CWEventMask, &swa);
  XMapWindow(dpy, shell);

  font_build(&F_lbl, S(11), 0);
  font_build(&F_card, S(13), 1);
  font_build(&F_hdr, S(20), 1);
  font_build(&F_hud, S(11), 1);
  font_build(&F_body, S(16), 0);
  font_build(&F_big, S(32), 1);

  settings_init();
  scan_system_apps();
  layout_grid();

  rs_init(&win_spring, (Rect){0,0,0,0,0}, 280, 30);
  sp_init(&home_scale_spring, 1.0, 240, 26, 0.001);
  sp_init(&switcher_scroll_spring, 0.0, 260, 27, 0.05);
  sp_init(&page_offset_spring, 0.0, 260, 28, 0.05);

  if (XShmQueryExtension(dpy)) {
    use_shm = 1;
    shm_ev_base = XShmGetEventBase(dpy);
  }
  realloc_canvas(W, H);

  double last_time = now_ms();
  int xfd = ConnectionNumber(dpy);
  int dirty = 1;   /* force first frame */
  double next_frame = 0;   /* 0 = render immediately (first frame / first frame after idle) */
  const double frame_ms = 1000.0 / 60.0;

  while (1) {
    while (XPending(dpy)) {
      XEvent ev;
      XNextEvent(dpy, &ev);
      if (use_shm && ev.type == shm_ev_base + ShmCompletion) { shm_completion(&ev); continue; }
      switch (ev.type) {
        case MapRequest: on_map_request(&ev.xmaprequest); dirty = 1; break;
        case ConfigureRequest: on_configure_request(&ev.xconfigurerequest); break;
        case UnmapNotify: on_unmap_notify(&ev.xunmap); dirty = 1; break;
        case DestroyNotify: on_destroy_notify(&ev.xdestroywindow); dirty = 1; break;
        case Expose: force_full_present = 1; dirty = 1; break;
        case ButtonPress:
          if (!on_pointer_down(ev.xbutton.x_root, ev.xbutton.y_root)) {
            XAllowEvents(dpy, ReplayPointer, CurrentTime);
          } else {
            XAllowEvents(dpy, AsyncPointer, CurrentTime);
          }
          dirty = 1;
          break;
        case MotionNotify:
          on_pointer_move(ev.xmotion.x_root, ev.xmotion.y_root);
          if (gesture || builtin_touch) dirty = 1;
          break;
        case KeyPress: settings_key(&ev.xkey); dirty = 1; break;
        case ButtonRelease:
          on_pointer_up(ev.xbutton.x_root, ev.xbutton.y_root);
          XAllowEvents(dpy, AsyncPointer, CurrentTime);
          dirty = 1;
          break;
      }
    }

    int geom_wait = geom_flush_pending();   /* deliver the final throttled size */
    /* Nothing changed and nothing is animating: sleep until the next X event. */
    if (!dirty && !anim_active() && !geom_wait) {
      XFlush(dpy);
      fd_set idle_fds; FD_ZERO(&idle_fds); FD_SET(xfd, &idle_fds);
      select(xfd + 1, &idle_fds, NULL, NULL, NULL);
      last_time = now_ms() - 16.0;   /* don't feed a huge dt into the springs after idling */
      next_frame = 0;
      continue;
    }

    double now = now_ms();
    if (now < next_frame) {
      /* Too early for the next frame: sleep until the deadline or the next X event. Touch-motion
         bursts are coalesced into one frame per tick instead of one render per event. */
      fd_set wf; FD_ZERO(&wf); FD_SET(xfd, &wf);
      long us = (long)((next_frame - now) * 1000.0);
      struct timeval wtv = { .tv_sec = 0, .tv_usec = us < 200 ? 200 : us };
      select(xfd + 1, &wf, NULL, NULL, &wtv);
      continue;
    }
    double dt = clampd((now - last_time) / 1000.0, 0.001, 0.05);
    last_time = now;

    rs_update(&win_spring, dt);
    sp_update(&home_scale_spring, dt);
    sp_update(&switcher_scroll_spring, dt);
    sp_update(&page_offset_spring, dt);
    settings_tick(dt);
    for (int i = 0; i < app_count; i++) sp_update(&apps[i].press_scale, dt);
    if (switcher.open) {
      for (int i = 0; i < switcher.ncards; i++) sp_update(&switcher.cards[i].vy_sp, dt);
    }

    int eff_h = kbd_active ? (int)lround(H * 0.75) : H;
    if (gesture && strcmp(gesture->mode, "drag") == 0 && !gesture->hold_fired
        && gesture->py < eff_h * 0.85
        && (now - gesture->last_move_t) > (active_app ? 450.0 : 60.0)) {
      gesture->hold_fired = 1;
      gesture->appear_t0 = now;
    }

    FBuf *cf = &fb[fb_cur];
    if (cf->is_shm) shm_wait(cf);  /* only the buffer we are about to overwrite */
    int py0, py1;
    render_frame(&cf->s, cf, &py0, &py1);
    canvas_present(cf, py0, py1);
    fb_cur = (fb_cur + 1) % nbuf;
    dirty = 0;
    XFlush(dpy);

    /* Deadline pacing: the period is 16.7 ms regardless of render time (the old fixed
       16 ms sleep made it render time + 16 ms). If we're running late, don't sleep and don't
       try to catch up. */
    if (next_frame == 0) next_frame = now + frame_ms;
    else { next_frame += frame_ms; if (next_frame < now) next_frame = now; }
  }

  return 0;
}
