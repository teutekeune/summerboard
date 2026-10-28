/* summerboard_gl.c
 *
 * Summerboard window manager / home screen. Rendering is done with OpenGL through Mesa
 * (GLX). The renderer lives in summerboard_gl.h; everything else is the original program.
 *
 * Build (Alpine ARM, headers are in /usr/include/freetype2; apk add freetype-dev):
 *   cc summerboardv28_gl.c -o summerboard \
 *     -I/usr/local/lib/sgx-mesa/include -I/usr/include/freetype2 \
 *     -L/usr/local/lib/sgx-mesa/lib \
 *     -lX11 -lXext -lXfixes -lEGL -lGL -lfreetype -lm
 *
 * Keep nanosvg.h and nanosvgrast.h next to this file, and the WhiteSur icon theme under
 * ./summerboard/WhiteSur (or set SUMMERBOARD_DIR).
 *
 * Wallpaper (PNG or JPEG), cover-cropped in place of the gradient:
 *   Settings → Display lists the PNG files in this program's folder (or SUMMERBOARD_DIR).
 *   -b PATH, SUMMERBOARD_BG, or $XDG_CONFIG_HOME/summerboard/background.png still work.
 *
 * Home screen editing (iPadOS style):
 *   - touch and hold an icon (or empty space) to enter edit mode; icons jiggle
 *   - drag icons to rearrange them, between pages (hold at a screen edge) and to/from the dock
 *   - tap the minus badge to remove an app from the home screen, "+" (top left) to add it back
 *   - "Done" (top right) or Escape leaves edit mode
 *   The arrangement is saved to $XDG_CONFIG_HOME/summerboard/layout (default ~/.config/summerboard/layout).
 */
#define _GNU_SOURCE
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <X11/extensions/Xfixes.h>
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
#include <glob.h>
#include <ctype.h>
#include <fcntl.h>
#include <linux/input.h>
#include <sys/prctl.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <stdarg.h>

/* SVG rasteriser for the WhiteSur icon theme (single-header, https://github.com/memononen/nanosvg).
   Put nanosvg.h and nanosvgrast.h next to this file. */
#define NANOSVG_IMPLEMENTATION
#include "nanosvg.h"
#define NANOSVGRAST_IMPLEMENTATION
#include "nanosvgrast.h"

/* PNG and JPEG wallpaper. One implementation, JPEG and PNG only. */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#include "stb_image.h"

/* 1: a running app's icon is its snapshot (falls back to the WhiteSur icon while it is closed).
   0: always show the WhiteSur icon. */
#define SNAPSHOT_ICONS 1

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
static int rs_busy(const RectSpring *s) {
  return sp_busy(&s->x) || sp_busy(&s->y) || sp_busy(&s->w) || sp_busy(&s->h) || sp_busy(&s->r);
}

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
/* X GLOBALS                                                                */
/* ------------------------------------------------------------------------ */
static Display *dpy;
static int scr, W, H, depth;
static Window root, shell;
static double UI = 1.0;
#define S(x) ((x) * UI)

static pid_t kbd_pid = 0;
static int kbd_active = 0;
static Window kbd_win = None;

/* Renderer: RR, rr_make, mix32, GImg, Glyph/AppFont, glyph_index, utf8_next, text_width,
   batch + primitives (fill_rr, fill_rect, ui_seg, draw_text, img_draw, draw_rgba,
   draw_card_shadow, draw_flight), images (img_upload, img_make_target, img_clear, img_free),
   the glyph atlas (atlas_put) and GL setup (gl_init, gl_begin_frame, gl_end_frame). */
#include "summerboard_gl.h"

/* ------------------------------------------------------------------------ */
/* FONTS                                                                    */
/* ------------------------------------------------------------------------ */
static AppFont F_lbl, F_card, F_hdr, F_hud, F_body, F_big;

/* SF Pro. The file is CFF inside an .otf, which the X core fonts cannot read, so FreeType rasterises
   it straight into the glyph atlas. Look in this program's folder (and SUMMERBOARD_DIR) first, then
   the copy under İndirilenler. Bold uses SFPRODISPLAYBOLD.OTF from the same place when it is there. */
static FT_Library ft_lib;
static FT_Face ft_reg, ft_bold;
static int ft_ready;

static void font_add_dir(char dirs[][512], int *nd, const char *d) {
  if (!d || !d[0] || *nd >= 6) return;
  for (int i = 0; i < *nd; i++) if (!strcmp(dirs[i], d)) return;
  snprintf(dirs[(*nd)++], 512, "%s", d);
}

static void font_dirs(char dirs[][512], int *nd) {
  *nd = 0;
  const char *env = getenv("SUMMERBOARD_DIR");
  if (env && *env) font_add_dir(dirs, nd, env);
  char exe[512];
  ssize_t k = readlink("/proc/self/exe", exe, sizeof exe - 1);
  if (k > 0) {
    exe[k] = 0;
    char *sl = strrchr(exe, '/');
    if (sl) { *sl = 0; font_add_dir(dirs, nd, exe); }
  }
  font_add_dir(dirs, nd, ".");
  font_add_dir(dirs, nd, "/home/egemen/İndirilenler/sf-pro-display");
}

static int font_named(const char *dir, const char *name, char *out, size_t n) {
  snprintf(out, n, "%s/%s", dir, name);
  return access(out, R_OK) == 0;
}

/* 1 = regular, 2 = bold, 0 = something else (italic, medium, ...). */
static int font_role(const char *s) {
  char b[256];
  size_t n = strlen(s);
  if (n < 5 || n >= sizeof b) return 0;
  if (strcasecmp(s + n - 4, ".otf") && strcasecmp(s + n - 4, ".ttf")) return 0;
  for (size_t i = 0; i < n; i++) b[i] = (char)tolower((unsigned char)s[i]);
  b[n] = 0;
  if (!strstr(b, "sfpro") && !strstr(b, "sf-pro")) return 0;
  if (strstr(b, "italic")) return 0;
  if (strstr(b, "semibold") || strstr(b, "medium") || strstr(b, "light") || strstr(b, "thin")
      || strstr(b, "black") || strstr(b, "heavy")) return 0;
  if (strstr(b, "bold")) return 2;
  if (strstr(b, "regular")) return 1;
  return 0;
}

static int font_find(char *out, size_t n, int bold) {
  char dirs[6][512];
  int nd = 0;
  font_dirs(dirs, &nd);
  const char *exact = bold ? "SFPRODISPLAYBOLD.OTF" : "SFPRODISPLAYREGULAR.OTF";
  for (int i = 0; i < nd; i++) if (font_named(dirs[i], exact, out, n)) return 1;
  int want = bold ? 2 : 1;
  for (int i = 0; i < nd; i++) {
    DIR *d = opendir(dirs[i]);
    if (!d) continue;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
      if (font_role(e->d_name) != want) continue;
      if (font_named(dirs[i], e->d_name, out, n)) { closedir(d); return 1; }
    }
    closedir(d);
  }
  return 0;
}

static int font_sf_init(void) {
  if (ft_ready) return ft_ready > 0;
  ft_ready = -1;
  char reg[512], bold[512];
  if (!font_find(reg, sizeof reg, 0)) return 0;
  if (FT_Init_FreeType(&ft_lib) || FT_New_Face(ft_lib, reg, 0, &ft_reg)) {
    fprintf(stderr, "summerboard: FreeType could not open %s\n", reg);
    return 0;
  }
  if (font_find(bold, sizeof bold, 1) && FT_New_Face(ft_lib, bold, 0, &ft_bold)) ft_bold = NULL;
  ft_ready = 1;
  fprintf(stderr, "summerboard: SF Pro %s%s\n", reg, ft_bold ? " + bold" : "");
  return 1;
}

static void font_build_sf(AppFont *f, double dest_px, FT_Face face) {
  int px = (int)lround(dest_px);
  if (px < 8) px = 8;
  f->xf = NULL;
  f->ss = (double)px / dest_px;
  if (f->ss < 0.5) f->ss = 0.5;
  FT_Set_Pixel_Sizes(face, 0, (FT_UInt)px);
  for (int i = 0; i < NGLYPH; i++) {
    uint32_t cp = i < 95 ? (uint32_t)(i + 32) : 0x2192u;
    Glyph *g = &f->g[i];
    memset(g, 0, sizeof *g);
    if (!FT_Get_Char_Index(face, cp) || FT_Load_Char(face, cp, FT_LOAD_RENDER)) {
      if (i == 95) f->g[95] = f->g['>' - 32];
      continue;
    }
    FT_GlyphSlot slot = face->glyph;
    g->adv = (float)(slot->advance.x / 64.0);
    g->lb = slot->bitmap_left;
    g->asc = slot->bitmap_top;
    FT_Bitmap *bm = &slot->bitmap;
    if (bm->pixel_mode != FT_PIXEL_MODE_GRAY || bm->width <= 0 || bm->rows <= 0 || cp == ' ') continue;
    g->w = g->cw = (int)bm->width;
    g->h = g->ch = (int)bm->rows;
    g->cov = malloc((size_t)g->cw * g->ch);
    if (!g->cov) { g->cw = g->ch = 0; continue; }
    for (int y = 0; y < g->ch; y++) {
      int sy = bm->pitch < 0 ? g->ch - 1 - y : y;
      const uint8_t *row = bm->buffer + (ptrdiff_t)sy * bm->pitch;
      memcpy(g->cov + (size_t)y * g->cw, row, (size_t)g->cw);
    }
    if (!atlas_put(g)) g->cw = g->ch = 0;
  }
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

/* Pre-render a glyph's 1-bit supersampled bitmap into 8-bit coverage at destination scale k,
   then hand it to the GL atlas (atlas_put frees the CPU copy). */
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
  if (!atlas_put(g)) { g->cw = g->ch = 0; }
}

static void font_build(AppFont *f, double dest_px, int bold) {
  if (font_sf_init()) { font_build_sf(f, dest_px, (bold && ft_bold) ? ft_bold : ft_reg); return; }
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
    memset(g, 0, sizeof *g);
    g->adv = ov.width; g->lb = ov.lbearing; g->asc = ov.ascent;
    g->w = ov.rbearing - ov.lbearing; g->h = ov.ascent + ov.descent;
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
  GImg snap;              /* snapshot texture (X window grab, or the Settings render target) */
  int is_kbd;
  int builtin;            /* drawn by the compositor itself, no X client window (e.g. Settings) */
  pid_t pid;
  /* Last geometry actually sent to each client window (for change detection / throttling). */
  Window sent_win[MAX_WINS];
  int sent_x[MAX_WINS], sent_y[MAX_WINS], sent_w[MAX_WINS], sent_h[MAX_WINS];
  double last_resize_ms;
  int covered;            /* real window is unmapped/withheld while the shell animates its snapshot */
  unsigned snap_gen;
  char iconname[160];     /* icon name hints (comma separated): .desktop Icon= or built-in defaults */
  char theme_path[512];   /* resolved WhiteSur svg ("" = not looked up yet) */
  GImg themeimg;          /* rasterised WhiteSur icon, premultiplied, icon-sized */
  int theme_state;        /* 0 = untried, 1 = ok, -1 = no theme icon for this app */
  int hidden;             /* removed from the home screen (restorable from edit mode's "+") */
  int removing;           /* shrinking away after its minus badge was tapped */
  Spring ox, oy;          /* layout animation: displayed position minus slot position, eases to 0 */
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

/* ---- home screen editing state ---- */
#define MAX_DOCK 6
#define LONGPRESS_MS 500.0
static int ord[MAX_APPS];                 /* display order: indices into apps[] (apps[] itself never moves) */
static int edit_mode = 0;
static int add_panel_open = 0;
static Spring edit_spring, panel_spring;  /* 0..1 visual state of edit mode / the "add" panel */
static RectSpring dock_spring;            /* the dock plate eases when its width changes */
static int dock_spring_ready = 0;
typedef struct { double left, icon_w, gap, row_gap, top; int cols; } GridMetrics;
static GridMetrics gm;                    /* filled in by layout_grid, used to map a finger to a slot */
static void exit_edit_mode(void);

static void layout_grid(void);
static int ensure_theme_icon(App *a);

static unsigned snap_serial = 1;
static void snap_drop(App *a) { img_free(&a->snap); a->snap_gen = ++snap_serial; }

/* ---- X error trap (used for snapshots) ---- */
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

static void toggle_matchbox_keyboard(void) {
  const char *target_disp = XDisplayString(dpy);
  if (!kbd_active) {
    kbd_active = 1;
    int cur_h = (int)lround(H * 0.75);
    XMoveResizeWindow(dpy, shell, 0, 0, W, cur_h);

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
  }
  layout_grid();
}

/* Hardware home button: gpio-keys shows up as an evdev node and reports KEY_HOMEPAGE (172) with value 1 on
   press, 2 on autorepeat and 0 on release. A press toggles the on-screen keyboard. Device: /dev/input/event1,
   or SUMMERBOARD_HOME_DEV. */
static int home_fd = -1;

static void home_button_init(void) {
  const char *dev = getenv("SUMMERBOARD_HOME_DEV");
  if (!dev || !dev[0]) dev = "/dev/input/event1";
  home_fd = open(dev, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
  if (home_fd < 0) fprintf(stderr, "summerboard: cannot open home button device %s: %s\n", dev, strerror(errno));
}

/* Drain pending events. Returns 1 if the keyboard was toggled. */
static int home_button_poll(void) {
  if (home_fd < 0) return 0;
  int toggled = 0;
  struct input_event ev;
  for (;;) {
    ssize_t n = read(home_fd, &ev, sizeof ev);
    if (n == (ssize_t)sizeof ev) {
      if (ev.type == EV_KEY && ev.code == KEY_HOMEPAGE && ev.value == 1) {   /* press only: not repeat (2) or release (0) */
        toggle_matchbox_keyboard();
        toggled = 1;
      }
      continue;
    }
    if (n < 0 && errno == EINTR) continue;
    if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {   /* device went away */
      close(home_fd); home_fd = -1;
    }
    break;                                  /* EAGAIN, short read or EOF: nothing more to do now */
  }
  return toggled;
}

/* ------------------------------------------------------------------------ */
/* Auto-rotation (autorotate.sh)                                            */
/* ------------------------------------------------------------------------ */
/* The accelerometer is an IIO device read through sysfs. A raw read wakes the chip and waits two of its
   samples, so the polling lives in a helper process (forked first thing in main) and never blocks drawing.
   It decides the orientation exactly as the script did and runs `rotate <normal|inverted|left|right>`, which
   turns the screen and the touch screen. The main process only sees the result: the root window gets a
   ConfigureNotify with the new size (screen_resized). The rotation lock is the file /run/rotation-lock: while
   it exists nothing turns. The helper ends with the X server and will not start if that server's autorotate
   is already running (/run/autorotate-<xpid>.pid). SUMMERBOARD_AUTOROTATE=0 switches this off.
   The script's XFCE panel button is not here: summerboard has no panel. p105-keys still toggles the lock. */
#define ROT_LOCK "/run/rotation-lock"
static char rot_pidf[80];

static void rot_pid_cleanup(void) {
  if (rot_pidf[0]) { unlink(rot_pidf); rot_pidf[0] = 0; }
}
static void rot_die(int sig) { (void)sig; rot_pid_cleanup(); _exit(0); }

static int rot_read_long(const char *path, long *out) {
  FILE *f = fopen(path, "r");
  if (!f) return 0;
  int ok = fscanf(f, "%ld", out) == 1;
  fclose(f);
  return ok;
}

/* The device's mount matrix ("x0, y0, z0; x1, ..." of -1, 0, 1) or the identity. */
static void rot_mount_matrix(const char *dev, int m[9]) {
  static const int ident[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
  memcpy(m, ident, sizeof ident);
  const char *names[] = { "in_accel_mount_matrix", "mount_matrix" };
  for (int n = 0; n < 2; n++) {
    char path[300], buf[256];
    snprintf(path, sizeof path, "%s/%s", dev, names[n]);
    FILE *f = fopen(path, "r");
    if (!f) continue;
    size_t len = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[len] = 0;
    int v[9], k = 0;
    const char *q = buf;
    while (k < 9) {
      while (*q && strchr(", ;\n\t", *q)) q++;
      if (!*q) break;
      char *end;
      double d = strtod(q, &end);
      if (end == q) break;
      q = end;
      if (fabs(d) < 0.01) v[k++] = 0;
      else if (fabs(d - 1) < 0.01) v[k++] = 1;
      else if (fabs(d + 1) < 0.01) v[k++] = -1;
      else { fprintf(stderr, "autorotate: mount matrix not of -1, 0, 1; the identity then\n"); return; }
    }
    if (k == 9) memcpy(m, v, sizeof v);
    return;
  }
}

/* x right, y up (away from Home), z out of the glass. Lying flat (under half of the pull in the glass's plane)
   says nothing; otherwise the larger of x and y, when the other is under 2/3 of it (34 degrees off the axis, so
   a diagonal does not flicker). */
static const char *rot_classify(long X, long Y, long Z) {
  long ax = labs(X), ay = labs(Y);
  if (4 * (X * X + Y * Y) < X * X + Y * Y + Z * Z) return NULL;
  if (3 * ax < 2 * ay) return Y > 0 ? "normal" : "inverted";
  if (3 * ay < 2 * ax) return X < 0 ? "left" : "right";
  return NULL;
}

/* Current turn, as `rotate` with no argument prints it. */
static int rot_current(char *out, size_t n) {
  out[0] = 0;
  FILE *p = popen("rotate", "r");
  if (!p) return 0;
  if (fgets(out, (int)n, p)) out[strcspn(out, "\r\n")] = 0;
  pclose(p);
  return out[0] != 0;
}

static void rot_apply(const char *o) {
  pid_t pid = fork();
  if (pid == 0) { execlp("rotate", "rotate", o, (char *)NULL); _exit(127); }
  if (pid > 0) waitpid(pid, NULL, 0);
}

/* Xorg's lock is /tmp/.X<n>-lock and holds the server's pid. The script lives only as long as that pid. */
static int rot_xpid(pid_t *out) {
  const char *disp = getenv("DISPLAY");
  if (!disp || !disp[0]) disp = ":0";
  const char *col = strrchr(disp, ':');
  char path[64];
  snprintf(path, sizeof path, "/tmp/.X%d-lock", atoi(col ? col + 1 : disp));
  FILE *f = fopen(path, "r");
  if (!f) return 0;
  long p = 0;
  int ok = fscanf(f, " %ld", &p) == 1 && p > 0;
  fclose(f);
  if (!ok) return 0;
  *out = (pid_t)p;
  return 1;
}

/* One autorotate per X server, the same pid file the script uses. A stale file (dead pid) is replaced. */
static int rot_claim(pid_t xpid) {
  snprintf(rot_pidf, sizeof rot_pidf, "/run/autorotate-%ld.pid", (long)xpid);
  FILE *f = fopen(rot_pidf, "r");
  if (f) {
    long p = 0;
    int alive = fscanf(f, " %ld", &p) == 1 && p > 1 && p != getpid() && kill((pid_t)p, 0) == 0;
    fclose(f);
    if (alive) {
      fprintf(stderr, "autorotate: running already (%ld)\n", p);
      rot_pidf[0] = 0;
      return 0;
    }
  }
  f = fopen(rot_pidf, "w");
  if (f) { fprintf(f, "%ld\n", (long)getpid()); fclose(f); }
  return 1;
}

static void autorotate_child(pid_t parent) {
  signal(SIGCHLD, SIG_DFL);                 /* the parent's reaper would steal our waitpid/pclose */
  prctl(PR_SET_PDEATHSIG, SIGTERM);
  if (getppid() != parent) _exit(0);

  /* rotate is /usr/local/bin/rotate. A session started over ssh does not have that on PATH.
     DISPLAY defaults to :0, which is also what the script and rotate.sh do. */
  {
    const char *old = getenv("PATH");
    char buf[1200];
    snprintf(buf, sizeof buf, "/usr/local/bin%s%s", (old && *old) ? ":" : "", old ? old : "");
    setenv("PATH", buf, 1);
  }
  { const char *d = getenv("DISPLAY"); if (!d || !d[0]) setenv("DISPLAY", ":0", 1); }

  char dev[300] = "", path[400];
  glob_t g;
  if (glob("/sys/bus/iio/devices/iio:device*", 0, NULL, &g) == 0) {
    for (size_t i = 0; i < g.gl_pathc; i++) {
      snprintf(path, sizeof path, "%s/in_accel_x_raw", g.gl_pathv[i]);
      if (access(path, R_OK) == 0) { snprintf(dev, sizeof dev, "%s", g.gl_pathv[i]); break; }
    }
    globfree(&g);
  }
  if (!dev[0]) { fprintf(stderr, "autorotate: no accelerometer (no IIO device with in_accel_x_raw)\n"); _exit(1); }

  pid_t xpid = 0;
  if (!rot_xpid(&xpid)) {
    fprintf(stderr, "autorotate: no X server on %s\n", getenv("DISPLAY"));
    _exit(1);
  }
  signal(SIGINT, rot_die);
  signal(SIGTERM, rot_die);
  signal(SIGHUP, rot_die);
  if (!rot_claim(xpid)) _exit(0);

  snprintf(path, sizeof path, "%s/sampling_frequency", dev);
  { FILE *f = fopen(path, "w"); if (f) { fputs("100\n", f); fclose(f); }
    else fprintf(stderr, "autorotate: cannot set %s; reads will be slow\n", path); }

  int m[9];
  rot_mount_matrix(dev, m);

  const char *last = "", *seen = "";
  int locked = -1;
  char pa[400], pb[400], pc[400];
  snprintf(pa, sizeof pa, "%s/in_accel_x_raw", dev);
  snprintf(pb, sizeof pb, "%s/in_accel_y_raw", dev);
  snprintf(pc, sizeof pc, "%s/in_accel_z_raw", dev);

  while (getppid() == parent && kill(xpid, 0) == 0) {
    int l = access(ROT_LOCK, F_OK) == 0;
    if (l != locked) {
      locked = l;
      if (!l) seen = "";                    /* unlocked: the way it is held now, at once */
    }
    long x, y, z;
    if (!rot_read_long(pa, &x) || !rot_read_long(pb, &y) || !rot_read_long(pc, &z)) { sleep(1); continue; }
    /* the screen's axes, in 1/16 of the raw (a g is ~1000 of them at 2 g) */
    long X = (m[0] * x + m[1] * y + m[2] * z) / 16;
    long Y = (m[3] * x + m[4] * y + m[5] * z) / 16;
    long Z = (m[6] * x + m[7] * y + m[8] * z) / 16;
    const char *o = rot_classify(X, Y, Z);
    if (o && strcmp(o, last) == 0 && strcmp(o, seen) != 0) {   /* the same twice running */
      seen = o;
      if (!locked) {
        char cur[64];
        if (!rot_current(cur, sizeof cur) || strcmp(cur, o) != 0) rot_apply(o);
      }
    }
    last = o ? o : "";
    usleep(250000);
  }
  rot_pid_cleanup();
  _exit(0);
}

static void autorotate_start(void) {
  const char *e = getenv("SUMMERBOARD_AUTOROTATE");
  if (e && e[0] == '0') return;
  pid_t parent = getpid();
  pid_t pid = fork();
  if (pid == 0) autorotate_child(parent);
  if (pid < 0) fprintf(stderr, "summerboard: cannot start autorotate: %s\n", strerror(errno));
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

/* ------------------------------------------------------------------------ */
/* WHITESUR ICON THEME                                                      */
/*   <base>/WhiteSur/apps/<name>.svg   icon files                           */
/*   <base>/WhiteSur/names.js          app name -> icon name (optional)     */
/* <base> is $SUMMERBOARD_DIR, ./summerboard, ~/summerboard, <exe dir>/summerboard, */
/* <exe dir>, /usr/local/share/summerboard or /usr/share/summerboard.        */
/* ------------------------------------------------------------------------ */
typedef struct { char key[96]; char val[96]; } Alias;
static Alias *aliases; static int n_alias, cap_alias;
static char theme_root[512];
static int theme_inited;

static void norm_key(const char *in, char *out, size_t n) {
  size_t o = 0;
  for (; *in && o + 1 < n; in++) if (isalnum((unsigned char)*in)) out[o++] = (char)tolower((unsigned char)*in);
  out[o] = 0;
}
static void alias_add(const char *k, const char *v) {
  if (!k[0] || !v[0]) return;
  if (n_alias == cap_alias) {
    int nc = cap_alias ? cap_alias * 2 : 128;
    Alias *na = realloc(aliases, sizeof(Alias) * nc);
    if (!na) return;
    aliases = na; cap_alias = nc;
  }
  norm_key(k, aliases[n_alias].key, sizeof aliases[n_alias].key);
  snprintf(aliases[n_alias].val, sizeof aliases[n_alias].val, "%s", v);
  if (aliases[n_alias].key[0]) n_alias++;
}
static const char *alias_lookup(const char *name) {
  char nk[96]; norm_key(name, nk, sizeof nk);
  if (!nk[0]) return NULL;
  for (int i = 0; i < n_alias; i++) if (!strcmp(aliases[i].key, nk)) return aliases[i].val;
  return NULL;
}
/* names.js may also map icon -> display name; find the icon whose value is this app's name */
static const char *alias_reverse(const char *name) {
  char nk[96], vk[96]; norm_key(name, nk, sizeof nk);
  if (!nk[0]) return NULL;
  for (int i = 0; i < n_alias; i++) { norm_key(aliases[i].val, vk, sizeof vk); if (!strcmp(vk, nk)) return aliases[i].key; }
  return NULL;
}

static int is_name_key(const char *k) {
  return !strcasecmp(k, "name") || !strcasecmp(k, "title") || !strcasecmp(k, "app") || !strcasecmp(k, "appname") || !strcasecmp(k, "label");
}
static int is_icon_key(const char *k) {
  return !strcasecmp(k, "icon") || !strcasecmp(k, "file") || !strcasecmp(k, "svg") || !strcasecmp(k, "id") || !strcasecmp(k, "path") || !strcasecmp(k, "image");
}

/* Tolerant reader for names.js. It does not run JavaScript; it only looks at  key: "value"  pairs, so
   these all work:   { "Firefox": "firefox", ... }   { firefox: "Firefox", ... }
                     [ { name: "Firefox", icon: "firefox" }, ... ]   { "Firefox": { icon: "firefox" } } */
static void names_parse(const char *s) {
  enum { MAXD = 16, MAXKV = 512 };
  static struct { char k[96], v[96]; } kv[MAXKV];
  int nkv = 0, depth = 0, fstart[MAXD];
  char fparent[MAXD][96];
  char prev[96] = "", key[96] = "";
  int have_prev = 0, have_key = 0;

  for (const char *p = s; *p; ) {
    if (isspace((unsigned char)*p)) { p++; continue; }
    if (p[0] == '/' && p[1] == '/') { while (*p && *p != '\n') p++; continue; }
    if (p[0] == '/' && p[1] == '*') { p += 2; while (*p && !(p[0] == '*' && p[1] == '/')) p++; if (*p) p += 2; continue; }

    char tok[96]; int is_str = 0, is_tok = 0;
    if (*p == '"' || *p == '\'' || *p == '`') {
      char q = *p++; size_t o = 0;
      while (*p && *p != q) {
        if (*p == '\\' && p[1]) p++;
        if (o + 1 < sizeof tok) tok[o++] = *p;
        p++;
      }
      if (*p) p++;
      tok[o] = 0; is_str = 1; is_tok = 1;
    } else if (isalpha((unsigned char)*p) || *p == '_' || *p == '$') {
      size_t o = 0;
      while (isalnum((unsigned char)*p) || *p == '_' || *p == '$') { if (o + 1 < sizeof tok) tok[o++] = *p; p++; }
      tok[o] = 0; is_tok = 1;
    } else if (*p == ':') {
      if (have_prev) { snprintf(key, sizeof key, "%s", prev); have_key = 1; }
      have_prev = 0; p++; continue;
    } else if (*p == '{') {
      if (depth < MAXD) { fstart[depth] = nkv; snprintf(fparent[depth], 96, "%s", have_key ? key : ""); depth++; }
      have_key = 0; have_prev = 0; p++; continue;
    } else if (*p == '}') {
      if (depth > 0) {
        depth--;
        const char *nm = NULL, *ic = NULL;
        for (int i = fstart[depth]; i < nkv; i++) {
          if (!nm && is_name_key(kv[i].k)) nm = kv[i].v;
          if (!ic && is_icon_key(kv[i].k)) ic = kv[i].v;
        }
        if (ic && nm) alias_add(nm, ic);
        else if (ic && fparent[depth][0]) alias_add(fparent[depth], ic);
        else if (!ic) for (int i = fstart[depth]; i < nkv; i++) alias_add(kv[i].k, kv[i].v);
        nkv = fstart[depth];
      }
      have_key = 0; have_prev = 0; p++; continue;
    } else { have_prev = 0; if (*p != ',' && *p != '[' && *p != ']') have_key = 0; p++; continue; }

    if (is_tok) {
      if (have_key && is_str && depth > 0 && nkv < MAXKV) {
        snprintf(kv[nkv].k, 96, "%s", key); snprintf(kv[nkv].v, 96, "%s", tok); nkv++;
      }
      have_key = 0;
      snprintf(prev, sizeof prev, "%s", tok); have_prev = 1;
    }
  }
}

static int dir_exists(const char *p) { struct stat st; return stat(p, &st) == 0 && S_ISDIR(st.st_mode); }

static void theme_init(void) {
  if (theme_inited) return;
  theme_inited = 1;
  char exe[512] = "", exedir[512] = "";
  ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
  if (n > 0) { exe[n] = 0; snprintf(exedir, sizeof exedir, "%s", exe); char *sl = strrchr(exedir, '/'); if (sl) *sl = 0; }
  const char *env = getenv("SUMMERBOARD_DIR"), *home = getenv("HOME");
  char cand[10][1024]; int nc = 0;
  if (env && *env) snprintf(cand[nc++], 1024, "%s", env);
  snprintf(cand[nc++], 1024, "./summerboard");
  if (home) snprintf(cand[nc++], 1024, "%.900s/summerboard", home);
  if (exedir[0]) { snprintf(cand[nc++], 1024, "%.900s/summerboard", exedir); snprintf(cand[nc++], 1024, "%.1000s", exedir); }
  snprintf(cand[nc++], 1024, "/usr/local/share/summerboard");
  snprintf(cand[nc++], 1024, "/usr/share/summerboard");
  for (int i = 0; i < nc; i++) {
    char t[1100];
    snprintf(t, sizeof t, "%s/WhiteSur/apps", cand[i]);
    if (dir_exists(t)) { snprintf(theme_root, sizeof theme_root, "%.480s/WhiteSur", cand[i]); break; }
  }
  if (!theme_root[0]) { DBG("WhiteSur theme not found (set SUMMERBOARD_DIR to the folder that contains WhiteSur/)"); return; }
  DBG("WhiteSur theme: %s", theme_root);

  char np[640];
  snprintf(np, sizeof np, "%.600s/names.js", theme_root);
  FILE *f = fopen(np, "r");
  if (!f) return;
  fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
  if (len > 0 && len < (4L << 20)) {
    char *buf = malloc((size_t)len + 1);
    if (buf) { size_t got = fread(buf, 1, (size_t)len, f); buf[got] = 0; names_parse(buf); free(buf); }
  }
  fclose(f);
  DBG("names.js: %d entries", n_alias);
}

static int try_icon(const char *base, char *out, size_t on) {
  if (!base || !base[0] || !theme_root[0]) return 0;
  char b[160]; snprintf(b, sizeof b, "%s", base);
  size_t bl = strlen(b);
  if (bl > 4 && !strcasecmp(b + bl - 4, ".svg")) b[bl - 4] = 0;
  if (b[0] == '/') {            /* absolute path from a .desktop Icon= */
    snprintf(out, on, "%s.svg", b);
    return access(out, R_OK) == 0;
  }
  char lo[160]; size_t i = 0;
  for (; b[i] && i + 1 < sizeof lo; i++) lo[i] = (char)tolower((unsigned char)b[i]);
  lo[i] = 0;
  static const char *dirs[] = { "apps", "apps/scalable" };
  for (int pass = 0; pass < 2; pass++)
    for (size_t d = 0; d < sizeof dirs / sizeof dirs[0]; d++) {
      snprintf(out, on, "%s/%s/%s.svg", theme_root, dirs[d], pass ? lo : b);
      if (access(out, R_OK) == 0) return 1;
    }
  return 0;
}
/* a name is looked up through names.js first, then used as a file name as-is */
static int try_name(const char *n, char *out, size_t on) {
  if (!n || !n[0]) return 0;
  const char *v = alias_lookup(n);
  if (v && try_icon(v, out, on)) return 1;
  return try_icon(n, out, on);
}

static int theme_find(const App *a, char *out, size_t on) {
  theme_init();
  if (!theme_root[0]) return 0;
  if (try_name(a->name, out, on)) return 1;
  char hints[160]; snprintf(hints, sizeof hints, "%s", a->iconname);
  for (char *h = strtok(hints, ","); h; h = strtok(NULL, ",")) if (try_name(h, out, on)) return 1;
  if (a->cmd[0]) {   /* binary name */
    char bin[96] = ""; sscanf(a->cmd, "%95s", bin);
    const char *sl = strrchr(bin, '/');
    if (try_name(sl ? sl + 1 : bin, out, on)) return 1;
  }
  char dashed[96], plain[96]; size_t di = 0, pi = 0;
  for (const char *c = a->name; *c && di + 1 < sizeof dashed; c++) {
    char ch = (char)tolower((unsigned char)*c);
    if (ch == ' ') { dashed[di++] = '-'; continue; }
    dashed[di++] = ch; if (pi + 1 < sizeof plain) plain[pi++] = ch;
  }
  dashed[di] = 0; plain[pi] = 0;
  if (try_icon(dashed, out, on) || try_icon(plain, out, on)) return 1;
  const char *rv = alias_reverse(a->name);
  return rv && try_icon(rv, out, on);
}

static void app_init_springs(App *a) {
  sp_init(&a->press_scale, 1.0, 350, 28, 0.001);
  sp_init(&a->ox, 0, 260, 26, 0.05);
  sp_init(&a->oy, 0, 260, 26, 0.05);
}

static void add_app_icon(const char *name, const char *cmd, int is_kbd, const char *icon) {
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
  if (icon) snprintf(a->iconname, sizeof a->iconname, "%s", icon);
  app_init_springs(a);

  app_count++;
}

static void add_app_manual(const char *name, const char *cmd, int is_kbd) { add_app_icon(name, cmd, is_kbd, NULL); }

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
  snprintf(a->iconname, sizeof a->iconname, "preferences-system,settings,org.gnome.Settings,gnome-settings");
  app_init_springs(a);
  app_count++;
}

static void parse_desktop_file(const char *filepath) {
  if (app_count >= MAX_APPS) return;

  FILE *f = fopen(filepath, "r");
  if (!f) return;

  char line[512];
  char name[64] = "";
  char exec[256] = "";
  char icon[160] = "";
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
    } else if (strncmp(line, "Icon=", 5) == 0 && icon[0] == '\0') {
      sscanf(line + 5, "%159[^\n]", icon);
    } else if (strncmp(line, "NoDisplay=true", 14) == 0) {
      no_display = 1;
    }
  }
  fclose(f);

  if (no_display || name[0] == '\0' || exec[0] == '\0') return;
  if (strstr(exec, "matchbox-keyboard")) return;   /* opened with the home button instead of an icon */

  add_app_icon(name, exec, 0, icon);
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

  static const char *known_binaries[][3] = {
    {"Terminal", "xterm", "utilities-terminal,terminal,xterm"},
    {"X Eyes", "xeyes", "xeyes"},
    {"Calculator", "xcalc", "accessories-calculator,calculator,xcalc"},
    {"Clock", "xclock", "clock,xclock,preferences-system-time"},
    {"Leafpad", "leafpad", "accessories-text-editor,text-editor,leafpad"},
    {"Geany", "geany", "geany"},
    {"Firefox", "firefox", "firefox"},
    {"Chromium", "chromium-browser", "chromium,chromium-browser"},
    {"Htop", "xterm -e htop", "utilities-system-monitor,htop,utilities-terminal"},
    {"Top", "xterm -e top", "utilities-system-monitor,utilities-terminal"},
    {"Cmatrix", "xterm -e cmatrix", "utilities-terminal,terminal"},
    {"Glx Gears", "glxgears", "glxgears,applications-games"}
  };

  for (size_t i = 0; i < sizeof(known_binaries)/sizeof(known_binaries[0]); i++) {
    add_app_icon(known_binaries[i][0], known_binaries[i][1], 0, known_binaries[i][2]);
  }

  if (app_count == 0) {
    add_app_icon("Terminal", "xterm", 0, "utilities-terminal,terminal,xterm");
  }
}

static void layout_grid(void) {
  int eff_h = kbd_active ? (int)lround(H * 0.75) : H;

  /* Narrow outer margin, wider column gaps. */
  double outer_pad = S(20);
  int cols = GRID_COLS;

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

  int ndock = 0;
  for (int i = 0; i < app_count; i++) if (apps[i].is_dock && !apps[i].hidden) ndock++;

  double dock_h = icon_w + S(20);
  double dock_item_spacing = S(14);
  double dock_w = ndock * icon_w + (ndock + 1) * dock_item_spacing;
  if (ndock == 0) dock_w = icon_w * 1.6;   /* an empty dock stays a visible drop target */
  if (dock_w > W - S(40)) dock_w = W - S(40);

  double dock_x = (W - dock_w) / 2;
  double dock_y = eff_h - dock_h - S(14);
  dock_box = (Rect){ dock_x, dock_y, dock_w, dock_h, dock_h * 0.23 };

  double dock_gap = (dock_w - ndock * icon_w) / (ndock + 1);

  int dock_idx = 0;
  for (int k = 0; k < app_count; k++) {
    App *a = &apps[ord[k]];
    if (a->hidden) {                 /* parked mid-screen so a running hidden app still has somewhere to fly */
      a->icon = (Rect){ (W - icon_w) / 2, (eff_h - icon_w) / 2, icon_w, icon_w, icon_r };
      a->page = 0;
      continue;
    }
    if (a->is_dock) {
      a->icon = (Rect){ dock_x + dock_gap + dock_idx * (icon_w + dock_gap), dock_y + S(8), icon_w, icon_w, icon_r };
      dock_idx++;
    }
  }

  int main_indices[MAX_APPS], nmain = 0;
  for (int k = 0; k < app_count; k++) {
    App *a = &apps[ord[k]];
    if (!a->is_dock && !a->hidden) main_indices[nmain++] = ord[k];
  }

  double top_off = S(45);
  gm = (GridMetrics){ (W - (cols * icon_w + (cols - 1) * gap)) / 2, icon_w, gap, row_gap, top_off, cols };

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
  if (current_page > total_pages - 1) {      /* the last page emptied out while editing */
    current_page = total_pages - 1;
    page_offset_spring.target = current_page * (double)W;
  }

  if (!dock_spring_ready) { rs_init(&dock_spring, dock_box, 260, 28); dock_spring_ready = 1; }
  else rs_target(&dock_spring, dock_box);

  for (int i = 0; i < app_count; i++) ensure_theme_icon(&apps[i]);   /* rasterise the WhiteSur icons up front */
}

/* ------------------------------------------------------------------------ */
/* SWITCHER METRICS                                                         */
/* ------------------------------------------------------------------------ */
typedef struct { App *app; Spring vy_sp; } Card;

static App *active_app = NULL;
static App *flight_app = NULL;   /* app whose window is flying to/from its icon */
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
  /* home screen editing */
  double down_t, gx, gy, cand_t, edge_t;   /* press time, grab offset inside the icon, reorder dwell, edge-flip dwell */
  int moved, cand_dock, cand_slot;
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
  if (!a->is_dock && !a->hidden) {
    r.x -= page_offset_spring.x;
  }
  r.x += a->ox.x; r.y += a->oy.x;
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
   needs to be resized a few times per second, not every frame.
     - unchanged geometry           -> nothing sent
     - position-only change         -> XMoveWindow
     - size change                  -> at most one per RESIZE_MIN_MS, latest value wins
   A throttled rect is remembered and delivered by geom_flush_pending() (trailing edge). */
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
      continue;

    int size_changed = !known || a->sent_w[i] != w || a->sent_h[i] != h;
    if (size_changed && !force && known && now - a->last_resize_ms < RESIZE_MIN_MS) {
      throttled = 1;
      if (a->sent_x[i] != x || a->sent_y[i] != y) {
        XMoveWindow(dpy, a->wins[i], x, y);
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

/* The real window is never resized while the shell animates: it stays at its rest size, hidden
   (unmapped), and the shell draws and scales the snapshot instead. */
static void app_cover(App *a) {
  if (a->builtin) return;
  app_lower(a);
  a->covered = 1;
}

static void launch(App *a) {
  if (a->is_kbd) {
    toggle_matchbox_keyboard();
    return;
  }

  exit_edit_mode();
  int fresh = (a->state == ST_CLOSED);
  if (fresh || !a->snap.tex) snap_drop(a);   /* a running app keeps its snapshot for the flight */
  a->state = ST_FG;
  active_app = a;
  flight_app = a->builtin ? NULL : a;
  rs_snap(&win_spring, icon_rect(a));

  Rect target = full_rect();
  {
    int eff_h = kbd_active ? (int)lround(H * 0.75) : H;
    target.h = eff_h - (int)lround(S(12));
  }

  rs_target(&win_spring, target);
  home_scale_spring.target = 0.92;
  a->covered = 0;
  if (a->builtin) builtin_enter(a, fresh);
  else if (a->nwin == 0) app_spawn(a);   /* its map request is held back until the flight lands */
  else app_cover(a);                      /* revealed when the flight lands */
}

static void minimize_to_icon(App *a, double vx, double vy) {
  a->state = ST_BG;
  flight_app = a->builtin ? NULL : a;   /* keep drawing the window while it flies home */
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
static int kt_collect(pid_t root_pid, KtProc *out, int max) {
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
    if (pids[i] == root_pid) { out[cnt].pid = root_pid; out[cnt].start = starts[i]; cnt++; }
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

/* Kill `pid` and every descendant. SIGTERM first, wait up to grace_ms, then SIGKILL.
   BLOCKS up to grace_ms; always called through kill_process_tree_async from the UI. */
static int kill_process_tree(pid_t pid, int grace_ms) {
  if (pid <= 1 || pid == getpid()) return 0;      /* never init / ourselves */

  KtProc list[KT_MAX];
  int n = kt_collect(pid, list, KT_MAX);
  if (n == 0) return 0;

  kt_signal_all(list, n, SIGSTOP);
  n = kt_collect(pid, list, KT_MAX);
  kt_signal_all(list, n, SIGSTOP);

  if (grace_ms > 0) {
    kt_signal_all(list, n, SIGTERM);
    kt_signal_all(list, n, SIGCONT);
    for (int waited = 0; waited < grace_ms && kt_any_alive(list, n); waited += 10)
      usleep(10000);
  }

  if (kt_any_alive(list, n)) {
    kt_signal_all(list, n, SIGKILL);
  }
  return n;
}

/* Non-blocking wrapper for the UI thread: a short-lived helper process does the waiting. */
static void kill_process_tree_async(pid_t pid, int grace_ms) {
  if (pid <= 1) return;
  pid_t h = fork();
  if (h == 0) {
    kill_process_tree(pid, grace_ms);
    _exit(0);
  }
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
/* config file) + `ip` and a DHCP client, run as short-lived sh scripts    */
/* whose output is collected without ever blocking the render loop.         */
/* No NetworkManager needed. Without those tools (or with -m) a demo        */
/* backend with fake networks is used. `-i wlan0` forces the interface.     */
/* ------------------------------------------------------------------------ */
#define MAX_NETS   32
#define MAX_SAVED  48
#define MAX_HITS   96
#define WJ_QLEN    8
#define WJ_TIMEOUT_MS 45000.0

enum { WJ_NONE, WJ_RADIO, WJ_TOGGLE, WJ_SCAN, WJ_SAVED, WJ_CONNECT, WJ_DISCONNECT, WJ_FORGET };
enum { PG_ROOT, PG_WIFI, PG_DISPLAY };
enum { SH_NONE, SH_PASSWORD, SH_DETAILS };
enum { H_NONE, H_BACK, H_SCAN, H_TOGGLE, H_ROOT_WIFI, H_ROOT_DISPLAY, H_WALL, H_NET, H_INFO, H_PW_FIELD, H_PW_SHOW,
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
static char wifi_missing[24] = "";   /* first missing tool, when falling back to demo data */

static struct {
  int page, shown;           /* shown keeps the subpage on screen while Back animates home */
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

/* ---- drawing helpers (primitives come from summerboard_gl.h) -------------- */
static void ui_disc(GImg *t, double cx, double cy, double r, uint32_t col, double al) {
  RR rr = rr_make(cx - r, cy - r, r * 2, r * 2, r);
  fill_rr(t, &rr, col, al, NULL);
}

static void ui_arc(GImg *t, double cx, double cy, double r, double a0, double a1, double w, uint32_t col, double al) {
  int n = (int)ceil(fabs(a1 - a0) * r / 3.0);
  if (n < 4) n = 4;
  double px = cx + cos(a0) * r, py = cy + sin(a0) * r;
  for (int i = 1; i <= n; i++) {
    double a = a0 + (a1 - a0) * i / n, x = cx + cos(a) * r, y = cy + sin(a) * r;
    ui_seg(t, px, py, x, y, w, col, al);
    px = x; py = y;
  }
}

static void ui_wifi(GImg *t, double cx, double cy, double s, uint32_t col, double al) {
  double oy = cy + s * 0.30;
  ui_disc(t, cx, oy, s * 0.075, col, al);
  for (int k = 1; k <= 3; k++) ui_arc(t, cx, oy, s * (0.04 + 0.18 * k), -2.356, -0.785, s * 0.085, col, al);
}

static void ui_bars(GImg *t, double x, double yb, double h, int lvl, uint32_t col) {
  double bw = h * 0.2, gap = h * 0.12;
  for (int i = 0; i < 4; i++) {
    double bh = h * (0.32 + 0.227 * i);
    RR r = rr_make(x + i * (bw + gap), yb - bh, bw, bh, bw * 0.3);
    fill_rr(t, &r, col, i < lvl ? 0.95 : 0.25, NULL);
  }
}

static void ui_lock(GImg *t, double cx, double cy, double s, uint32_t col, uint32_t bg) {
  double sw = s * 0.16;
  RR o = rr_make(cx - s * 0.30, cy - s * 0.55, s * 0.60, s * 0.80, s * 0.30);
  fill_rr(t, &o, col, 1.0, NULL);
  RR i = rr_make(cx - s * 0.30 + sw, cy - s * 0.55 + sw, s * 0.60 - 2 * sw, s * 0.80, s * 0.30 - sw);
  fill_rr(t, &i, bg, 1.0, NULL);
  RR b = rr_make(cx - s * 0.44, cy - s * 0.10, s * 0.88, s * 0.62, s * 0.12);
  fill_rr(t, &b, col, 1.0, NULL);
}

static void ui_check(GImg *t, double cx, double cy, double s, uint32_t col, double al) {
  ui_seg(t, cx - s * 0.5, cy + s * 0.02, cx - s * 0.16, cy + s * 0.38, s * 0.2, col, al);
  ui_seg(t, cx - s * 0.16, cy + s * 0.38, cx + s * 0.5, cy - s * 0.38, s * 0.2, col, al);
}

static void ui_chevron(GImg *t, double x, double cy, double s, int dir, uint32_t col, double al) {
  /* dir +1 points right (tip at x), dir -1 points left (tip at x) */
  ui_seg(t, x - dir * s * 0.55, cy - s, x, cy, S(2.2), col, al);
  ui_seg(t, x, cy, x - dir * s * 0.55, cy + s, S(2.2), col, al);
}

static void ui_spinner(GImg *t, double cx, double cy, double r, uint32_t col, double al) {
  int head = (int)(now_ms() / 75.0) % 12;
  for (int i = 0; i < 12; i++) {
    double a = i * M_PI / 6, k = ((head - i + 12) % 12) / 12.0;
    ui_seg(t, cx + sin(a) * r * 0.5, cy - cos(a) * r * 0.5, cx + sin(a) * r, cy - cos(a) * r, r * 0.2,
           col, al * (0.15 + 0.85 * (1.0 - k)));
  }
}

static void ui_toggle(GImg *t, double x, double y, double k) {
  double w = S(51), h = S(31);
  RR tr = rr_make(x, y, w, h, h / 2);
  fill_rr(t, &tr, mix32(0x3a3654, UI_GRN, (unsigned)(clampd(k, 0, 1) * 256)), 1.0, NULL);
  double kx = x + h / 2 + clampd(k, 0, 1) * (w - h);
  ui_disc(t, kx, y + h / 2 + S(1.2), h / 2 - S(1), 0x000000, 0.25);
  ui_disc(t, kx, y + h / 2, h / 2 - S(2), 0xffffff, 1.0);
}

static void ui_text_fit(GImg *t, const AppFont *f, const char *s, double x, double y, double maxw,
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

/* ---- wallpaper pictures (PNG files next to this program) ------------------ */
#define MAX_WALLS 48
typedef struct { char name[96]; char path[512]; } WallPic;
static WallPic walls[MAX_WALLS];
static int nwalls, wall_cur = -1;
static char wall_dir[512];
static char wallpaper_arg[512];
static char wallpaper_path[512];
static GImg wallpaper;
static int wallpaper_apply(const char *path, int remember);

static void summerboard_dir(char *out, size_t n) {
  const char *env = getenv("SUMMERBOARD_DIR");
  if (env && *env && dir_exists(env)) { snprintf(out, n, "%s", env); return; }
  char exe[512];
  ssize_t k = readlink("/proc/self/exe", exe, sizeof exe - 1);
  if (k > 0) {
    exe[k] = 0;
    char *sl = strrchr(exe, '/');
    if (sl) { *sl = 0; snprintf(out, n, "%s", exe); return; }
  }
  snprintf(out, n, ".");
}

static void wall_label(const char *name, char *out, size_t n) {
  snprintf(out, n, "%s", name);
  size_t L = strlen(out);
  if (L > 4 && !strcasecmp(out + L - 4, ".png")) out[L - 4] = 0;
}

static void wall_mark(void) {
  wall_cur = -1;
  if (!wallpaper_path[0]) return;
  for (int i = 0; i < nwalls; i++)
    if (!strcmp(walls[i].path, wallpaper_path)) { wall_cur = i; return; }
}

/* Non-recursive: pictures live next to the program, not inside WhiteSur. */
static void wall_scan(void) {
  nwalls = 0;
  summerboard_dir(wall_dir, sizeof wall_dir);
  DIR *d = opendir(wall_dir);
  if (!d) { wall_mark(); return; }
  struct dirent *e;
  while ((e = readdir(d)) != NULL && nwalls < MAX_WALLS) {
    size_t L = strlen(e->d_name), dl = strlen(wall_dir);
    if (L < 5 || L >= sizeof walls[0].name || strcasecmp(e->d_name + L - 4, ".png")) continue;
    if (dl + 1 + L + 1 > sizeof walls[0].path) continue;
    char path[512];
    memcpy(path, wall_dir, dl);
    path[dl] = '/';
    memcpy(path + dl + 1, e->d_name, L + 1);
    struct stat sb;
    if (stat(path, &sb) != 0 || !S_ISREG(sb.st_mode)) continue;
    memcpy(walls[nwalls].name, e->d_name, L + 1);
    memcpy(walls[nwalls].path, path, dl + 1 + L + 1);
    nwalls++;
  }
  closedir(d);
  for (int i = 1; i < nwalls; i++) {
    WallPic tmp = walls[i];
    int j = i;
    while (j > 0 && strcasecmp(walls[j - 1].name, tmp.name) > 0) { walls[j] = walls[j - 1]; j--; }
    walls[j] = tmp;
  }
  wall_mark();
}

static void ui_screen(GImg *t, double cx, double cy, double s, uint32_t col, double al) {
  RR r = rr_make(cx - s * 0.46, cy - s * 0.32, s * 0.92, s * 0.52, s * 0.08);
  fill_rr(t, &r, col, al, NULL);
  ui_seg(t, cx, cy + s * 0.20, cx, cy + s * 0.36, fmax(1.6, s * 0.08), col, al);
  ui_seg(t, cx - s * 0.22, cy + s * 0.36, cx + s * 0.22, cy + s * 0.36, fmax(1.6, s * 0.08), col, al);
}

/* ---- pages ------------------------------------------------------------------ */
static void draw_page_root(GImg *t, double dx) {
  fill_rect(t, (int)lround(dx), 0, t->w, t->h, UI_BG, 1.0);
  double m = S(16), rh = S(54), cw = t->w - 2 * m;
  draw_text(t, &F_big, "Settings", dx + m + S(4), S(66), 1.0, 0, UI_TXT, 1.0, 0, NULL);

  char wv[72] = "";
  if (st.radio_known) snprintf(wv, sizeof wv, "%s", !st.radio_on ? "Off" : st.active_ssid[0] ? st.active_ssid : "Not Connected");

  static const struct { const char *name; uint32_t col; } soon[] = {
    { "Bluetooth", 0x0a84ff }, { "Sounds", 0xff375f }, { "About", 0x8e8e93 } };

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

  y += rh + S(12);
  RR cd = rr_make(dx + m, y, cw, rh, S(12));
  fill_rr(t, &cd, UI_CARD, 1.0, NULL);
  if (pressed(H_ROOT_DISPLAY, 0)) { RR hr = rr_make(dx + m, y, cw, rh, 0); fill_rr(t, &hr, UI_HI, 1.0, &cd); }
  cy = y + rh / 2;
  RR di = rr_make(ix, cy - is / 2, is, is, S(7));
  fill_rr(t, &di, 0xff9f0a, 1.0, NULL);
  ui_screen(t, ix + is / 2, cy, is * 0.62, 0xffffff, 1.0);
  ui_chevron(t, rx - S(2), cy, S(5.5), 1, UI_SUB, 0.7);
  char dv[72] = "Gradient";
  if (wall_cur >= 0) wall_label(walls[wall_cur].name, dv, sizeof dv);
  else if (wallpaper.tex) snprintf(dv, sizeof dv, "Custom");
  ui_text_fit(t, &F_body, dv, rx - S(20), BL(cy, 16), cw * 0.45, 2, UI_SUB, 1.0);
  draw_text(t, &F_body, "Display", lx, BL(cy, 16), 1.0, 0, UI_TXT, 1.0, 0, NULL);
  add_hit(dx + m, y, cw, rh, H_ROOT_DISPLAY, 0, 0, t->h);

  y += rh + S(26);
  draw_text(t, &F_lbl, "COMING SOON", dx + m + S(16), y - S(8), 1.0, 0, UI_SUB, 1.0, S(0.6), NULL);
  RR c2 = rr_make(dx + m, y, cw, rh * 3, S(12));
  fill_rr(t, &c2, UI_CARD, 1.0, NULL);
  for (int i = 0; i < 3; i++) {
    double ry = y + i * rh, rcy = ry + rh / 2;
    RR si = rr_make(ix, rcy - is / 2, is, is, S(7));
    fill_rr(t, &si, soon[i].col, 0.45, NULL);
    draw_text(t, &F_body, soon[i].name, lx, BL(rcy, 16), 1.0, 0, UI_TXT, 0.4, 0, NULL);
    if (i < 2) fill_rect(t, (int)(dx + m + S(56)), (int)(ry + rh - 1), (int)(cw - S(56)), 1, 0xffffff, 0.07);
  }
}

static void draw_page_wifi(GImg *t, double dx) {
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
  if (scanning && st.radio_on) ui_spinner(t, dx + m + S(102), y - S(12), S(7), UI_SUB, 1.0);

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

static void draw_page_display(GImg *t, double dx) {
  fill_rect(t, (int)lround(dx), 0, t->w, t->h, UI_BG, 1.0);
  double m = S(16), rh = S(54), cw = t->w - 2 * m, nav_h = S(60), H_ = t->h;
  double y = nav_h + S(12) - st.scroll.x;
  int rows = nwalls + 1;   /* gradient, then each PNG */

  draw_text(t, &F_lbl, "WALLPAPER", dx + m + S(16), y - S(8), 1.0, 0, UI_SUB, 1.0, S(0.6), NULL);
  RR card = rr_make(dx + m, y, cw, rh * rows, S(12));
  fill_rr(t, &card, UI_CARD, 1.0, NULL);
  for (int i = 0; i < rows; i++) {
    double ry = y + i * rh, cy = ry + rh / 2;
    if (ry + rh >= nav_h && ry <= H_) {
      int on = (i == 0) ? !wallpaper.tex : wall_cur == i - 1;
      if (pressed(H_WALL, i - 1)) { RR hr = rr_make(dx + m, ry, cw, rh, 0); fill_rr(t, &hr, UI_HI, 1.0, &card); }
      if (on) ui_check(t, dx + m + S(24), cy, S(9), UI_ACC, 1.0);
      char lab[96];
      if (i == 0) snprintf(lab, sizeof lab, "Gradient");
      else wall_label(walls[i - 1].name, lab, sizeof lab);
      ui_text_fit(t, &F_body, lab, dx + m + S(48), BL(cy, 16), cw - S(64), 0, UI_TXT, 1.0);
      if (i < rows - 1) fill_rect(t, (int)(dx + m + S(48)), (int)(ry + rh - 1), (int)(cw - S(48)), 1, 0xffffff, 0.07);
      add_hit(dx + m, ry, cw, rh, H_WALL, i - 1, nav_h, H_);
    }
  }
  y += rh * rows;
  if (nwalls == 0) {
    y += S(18);
    draw_text(t, &F_body, "No PNG pictures in this folder", dx + t->w / 2, y, 1.0, 1, UI_SUB, 0.8, 0, NULL);
    y += S(28);
  }
  st.max_scroll = fmax(0, (y + st.scroll.x + S(40)) - H_);

  fill_rect(t, (int)lround(dx), 0, t->w, (int)nav_h, UI_BG, 1.0);
  fill_rect(t, (int)lround(dx), (int)nav_h, t->w, 1, 0xffffff, 0.08);
  double ncy = S(34), ba = pressed(H_BACK, 0) ? 0.45 : 1.0;
  ui_chevron(t, dx + m + S(4), ncy, S(7), -1, UI_ACC, ba);
  draw_text(t, &F_body, "Settings", dx + m + S(14), BL(ncy, 16), 1.0, 0, UI_ACC, ba, 0, NULL);
  add_hit(dx, 0, S(130), nav_h, H_BACK, 0, 0, H_);
  draw_text(t, &F_hdr, "Display", dx + t->w / 2, BL(ncy, 20), 1.0, 1, UI_TXT, 1.0, 0, NULL);
}

static void draw_sheet(GImg *t) {
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

static void draw_toast(GImg *t) {
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

static void settings_draw(GImg *t, int rec) {
  st.rec = rec;
  if (rec) st.nhits = 0;
  double nav = clampd(st.nav.x, 0, 1);
  int sub = st.page != PG_ROOT ? st.page : st.shown;
  if (nav < 0.999) draw_page_root(t, -nav * t->w * 0.28);
  if (nav > 0.001 && nav < 0.999) fill_rect(t, 0, 0, t->w, t->h, 0x000000, 0.35 * nav);
  if (nav > 0.001 && sub == PG_DISPLAY) draw_page_display(t, (1 - nav) * t->w);
  else if (nav > 0.001) draw_page_wifi(t, (1 - nav) * t->w);
  draw_sheet(t);
  draw_toast(t);
}

/* Live paint used by the compositor. At rest this draws straight into the window; while the
   window is opening/closing it is rendered into an off-screen target and scaled into `r`. */
static void settings_paint(GImg *t, Rect r) {
  if (fabs(r.x) < 1 && r.w >= t->w - 1 && r.h >= t->h - S(12) - 2) { settings_draw(t, 1); return; }
  static GImg off;
  if (!off.fbo || off.w != t->w || off.h != t->h) img_make_target(&off, t->w, t->h);
  img_clear(&off);
  settings_draw(&off, 0);
  RR clip = rr_make(r.x, r.y, r.w, r.h, r.r);
  img_draw(t, &off, r, &clip, 1.0);
}

static void settings_snapshot(App *a) {
  int eff_h = kbd_active ? (int)lround(H * 0.75) : H;
  if (!a->snap.fbo || a->snap.w != W || a->snap.h != eff_h) img_make_target(&a->snap, W, eff_h);
  img_clear(&a->snap);
  settings_draw(&a->snap, 0);
}

/* Gear glyph on the home-screen icon. */
static void settings_icon(GImg *t, double cx, double cy, double s, double al) {
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
      st.page = st.shown = PG_WIFI; st.nav.target = 1; sp_snap(&st.scroll, 0);
      wj_queue(WJ_RADIO, NULL, NULL, 0, 0);
      break;
    case H_ROOT_DISPLAY:
      wall_scan();
      st.page = st.shown = PG_DISPLAY; st.nav.target = 1; sp_snap(&st.scroll, 0);
      break;
    case H_WALL:
      if (arg < 0) wallpaper_apply(NULL, 1);
      else if (arg < nwalls) wallpaper_apply(walls[arg].path, 1);
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
    if ((st.page == PG_WIFI || st.page == PG_DISPLAY) && !st.sheet_open) st.dragging = 1;
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
  if (st.page == PG_ROOT && st.nav.x < 0.001 && !sp_busy(&st.nav)) st.shown = PG_ROOT;
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

/* Leaving the foreground: dismiss an open password sheet (and the on-screen keyboard it raised). */
static void builtin_leave(App *a) {
  (void)a;
  if (st.sheet_open) close_sheet();
  sp_snap(&st.sheet, 0);
  st.press_id = 0; st.dragging = 0;
}

static void builtin_close(App *a) { (void)a; settings_close(); }

static void builtin_paint(GImg *t, App *a, Rect r) { (void)a; settings_paint(t, r); }

static void terminate(App *a) {
  if (flight_app == a) flight_app = NULL;
  a->state = ST_CLOSED;
  if (a->builtin) builtin_close(a);

  for (int i = 0; i < a->nwin; i++) XDestroyWindow(dpy, a->wins[i]);
  a->nwin = 0;
  snap_drop(a);
  if (active_app == a) active_app = NULL;

  /* Kill the app's whole process tree by PID (children included). */
  if (a->pid > 0) {
    pid_t pp;
    if (kt_stat(a->pid, &pp, NULL, NULL) == 0 && pp == getpid())
      kill_process_tree_async(a->pid, 500);
    a->pid = 0;
  }
}

static void resume(App *a, Rect from_rect) {
  flight_app = NULL;   /* opening out of a card, not out of an icon */
  exit_edit_mode();
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
  app_cover(a);   /* the card keeps showing its snapshot until the window has opened */
}

static void close_switcher_to_home(void) {
  switcher.open = 0; switcher.entry.dur = 0; active_app = NULL;
  if (gesture) { free(gesture); gesture = NULL; }
  home_scale_spring.target = 1.0;
}

/* ------------------------------------------------------------------------ */
/* SNAPSHOT CAPTURE                                                         */
/* ------------------------------------------------------------------------ */
/* An X window is read back once per gesture into a plain buffer and uploaded as a texture.
   The Settings app renders straight into its own target instead (settings_snapshot). */
static void grab_snapshot_impl(App *a) {
  if (a->builtin) { settings_snapshot(a); return; }
  if (a->nwin == 0) return;
  Window w = a->wins[a->nwin - 1];
  XWindowAttributes wa;
  if (!XGetWindowAttributes(dpy, w, &wa) || wa.map_state != IsViewable) return;
  if (wa.width < 1 || wa.height < 1) return;

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

  int wd = wa.width, ht = wa.height;
  uint32_t *buf = malloc((size_t)wd * ht * 4);
  if (buf) {
    const uint16_t one = 1;
    int native = im->bits_per_pixel == 32 && im->byte_order == (*(const uint8_t *)&one ? LSBFirst : MSBFirst);
    for (int y = 0; y < ht; y++) {
      uint32_t *drow = buf + (size_t)y * wd;
      if (native) {
        const uint32_t *srow = (const uint32_t *)(im->data + (size_t)y * im->bytes_per_line);
        for (int x = 0; x < wd; x++) drow[x] = srow[x] | 0xff000000u;
      } else {
        for (int x = 0; x < wd; x++) drow[x] = 0xff000000u | (uint32_t)XGetPixel(im, x, y);
      }
    }
    img_upload(&a->snap, buf, wd, ht);
    free(buf);
  }
  XDestroyImage(im);
}

static void grab_snapshot(App *a) {
  grab_snapshot_impl(a);
  a->snap_gen = ++snap_serial;
}

static void activate_switcher(void) {
  flight_app = NULL;
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
/* HOME SCREEN EDITING                                                       */
/* ------------------------------------------------------------------------ */
static void sp_from(Spring *s, double from) { s->x = from; s->v = 0; s->target = 0; }

static int is_removable(const App *a) { return !a->builtin && !a->is_kbd; }

static int hidden_count(void) {
  int n = 0;
  for (int i = 0; i < app_count; i++) if (apps[i].hidden && !apps[i].removing) n++;
  return n;
}

/* ---- persistence ---- */
static void layout_path(char *out, size_t n, int make_dirs) {
  const char *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
  char dir[400];
  if (xdg && *xdg) snprintf(dir, sizeof dir, "%s/summerboard", xdg);
  else if (home && *home) {
    snprintf(dir, sizeof dir, "%s/.config", home);
    if (make_dirs) mkdir(dir, 0755);
    snprintf(dir, sizeof dir, "%s/.config/summerboard", home);
  } else { out[0] = 0; return; }
  if (make_dirs) mkdir(dir, 0755);
  snprintf(out, n, "%s/layout", dir);
}

static void save_layout(void) {
  char path[512], tmp[540];
  layout_path(path, sizeof path, 1);
  if (!path[0]) return;
  snprintf(tmp, sizeof tmp, "%s.tmp", path);
  FILE *f = fopen(tmp, "w");
  if (!f) return;
  for (int k = 0; k < app_count; k++) {
    const App *a = &apps[ord[k]];
    fprintf(f, "%c %s\n", a->hidden ? 'H' : (a->is_dock ? 'D' : 'M'), a->name);
  }
  fclose(f);
  rename(tmp, path);
}

/* Starts from the scan order, then applies the saved file (apps it doesn't mention go to the end). */
static void load_layout(void) {
  for (int i = 0; i < app_count; i++) ord[i] = i;
  char path[512];
  layout_path(path, sizeof path, 0);
  if (!path[0]) return;
  FILE *f = fopen(path, "r");
  if (!f) return;

  int placed[MAX_APPS] = { 0 }, order[MAX_APPS], n = 0;
  char line[160];
  while (fgets(line, sizeof line, f)) {
    size_t len = strcspn(line, "\r\n");
    line[len] = 0;
    if (len < 3 || line[1] != ' ') continue;
    int idx = -1;
    for (int i = 0; i < app_count; i++) if (!strcmp(apps[i].name, line + 2)) { idx = i; break; }
    if (idx < 0 || placed[idx]) continue;
    placed[idx] = 1;
    order[n++] = idx;
    apps[idx].is_dock = (line[0] == 'D');
    apps[idx].hidden = (line[0] == 'H') && is_removable(&apps[idx]);
  }
  fclose(f);
  if (n == 0) return;

  for (int i = 0; i < app_count; i++) {
    if (placed[i]) continue;
    apps[i].is_dock = 0; apps[i].hidden = 0;
    order[n++] = i;
  }
  memcpy(ord, order, sizeof(int) * app_count);

  int nd = 0;                                   /* a hand-edited file must not overfill the dock */
  for (int k = 0; k < app_count; k++) {
    App *a = &apps[ord[k]];
    if (a->is_dock && !a->hidden && ++nd > MAX_DOCK) a->is_dock = 0;
  }
}

/* ---- ordering ---- */
/* Lay the grid out again, and let every other icon glide from where it was to its new slot. */
static void relayout_animated(const App *skip) {
  double ox[MAX_APPS], oy[MAX_APPS];
  for (int i = 0; i < app_count; i++) { ox[i] = apps[i].icon.x + apps[i].ox.x; oy[i] = apps[i].icon.y + apps[i].oy.x; }
  layout_grid();
  for (int i = 0; i < app_count; i++) {
    App *a = &apps[i];
    if (a == skip || a->hidden) continue;
    double dx = ox[i] - a->icon.x, dy = oy[i] - a->icon.y;
    if (fabs(dx) + fabs(dy) < 0.5) continue;
    sp_from(&a->ox, dx); sp_from(&a->oy, dy);
  }
}

/* Put `a` at index `slot` of the main grid (to_dock = 0) or of the dock (to_dock = 1). */
static void move_app(App *a, int to_dock, int slot) {
  int dk[MAX_APPS], mn[MAX_APPS], hd[MAX_APPS], nd = 0, nm = 0, nh = 0, self = (int)(a - apps);
  for (int k = 0; k < app_count; k++) {
    int i = ord[k];
    if (i == self) continue;
    if (apps[i].hidden) hd[nh++] = i;
    else if (apps[i].is_dock) dk[nd++] = i;
    else mn[nm++] = i;
  }
  int *seq = to_dock ? dk : mn, *cnt = to_dock ? &nd : &nm;
  slot = (int)clampd(slot, 0, *cnt);
  memmove(&seq[slot + 1], &seq[slot], (size_t)(*cnt - slot) * sizeof(int));
  seq[slot] = self;
  (*cnt)++;
  int n = 0;
  for (int i = 0; i < nd; i++) ord[n++] = dk[i];
  for (int i = 0; i < nm; i++) ord[n++] = mn[i];
  for (int i = 0; i < nh; i++) ord[n++] = hd[i];
  a->is_dock = to_dock;
}

static int slot_of(const App *a) {
  int n = 0;
  for (int k = 0; k < app_count; k++) {
    const App *b = &apps[ord[k]];
    if (b == a) return n;
    if (!b->hidden && b->is_dock == a->is_dock) n++;
  }
  return n;
}

/* Where would a dragged icon land if it were dropped with the finger at (px, py)? */
static void drag_target(const App *self, double px, double py, int *to_dock, int *slot) {
  int nd = 0, nm = 0;
  for (int i = 0; i < app_count; i++) {
    if (&apps[i] == self || apps[i].hidden) continue;
    if (apps[i].is_dock) nd++; else nm++;
  }
  if (py > dock_box.y - S(24) && nd < MAX_DOCK) {
    int s = 0;
    for (int k = 0; k < app_count; k++) {
      const App *b = &apps[ord[k]];
      if (b == self || b->hidden || !b->is_dock) continue;
      if (px > b->icon.x + b->icon.w / 2) s++;
    }
    *to_dock = 1; *slot = s;
    return;
  }
  double wx = px + page_offset_spring.x;
  int page = (int)clampd(floor(wx / W), 0, total_pages - 1);
  double lx = wx - page * (double)W;
  int col = (int)clampd(lround((lx - gm.left - gm.icon_w / 2) / (gm.icon_w + gm.gap)), 0, gm.cols - 1);
  int row = (int)clampd(lround((py - gm.top - gm.icon_w / 2) / (gm.icon_w + gm.row_gap)), 0, GRID_ROWS - 1);
  int s = page * APPS_PER_PAGE + row * GRID_COLS + col;
  *to_dock = 0; *slot = s > nm ? nm : s;
}

/* ---- enter / leave ---- */
static void enter_edit_mode(void) {
  edit_mode = 1;
  edit_spring.target = 1.0;
}

static void end_drag(void);
static void close_panel(void) { add_panel_open = 0; panel_spring.target = 0; }

static void exit_edit_mode(void) {
  if (!edit_mode) return;
  if (gesture && !strcmp(gesture->mode, "iconDrag")) end_drag();
  edit_mode = 0;
  edit_spring.target = 0.0;
  close_panel();
  save_layout();
}

/* ---- dragging an icon ---- */
static void begin_drag(App *a, double px, double py) {
  Rect r = icon_rect(a);
  if (!gesture) gesture = calloc(1, sizeof *gesture);
  strcpy(gesture->mode, "iconDrag");
  gesture->app = a;
  gesture->px = px; gesture->py = py;
  gesture->gx = px - r.x; gesture->gy = py - r.y;
  gesture->cand_slot = -1; gesture->edge_t = 0;
  a->press_scale.target = 1.12;
  /* the icon is drawn from the finger from now on; its own offsets must not fight that */
  sp_snap(&a->ox, 0); sp_snap(&a->oy, 0);
}

/* Per frame while an icon is held: flip pages at the screen edges and rearrange after a short dwell. */
static void drag_tick(double now) {
  if (!gesture || strcmp(gesture->mode, "iconDrag")) return;
  App *a = gesture->app;
  double px = gesture->px, py = gesture->py;

  int dir = px < S(44) ? -1 : (px > W - S(44) ? 1 : 0);
  if (!dir) gesture->edge_t = 0;
  else if (gesture->edge_t == 0) gesture->edge_t = now;
  else if (now - gesture->edge_t > 550.0) {
    int np = current_page + dir;
    if (np >= 0 && np < total_pages) { current_page = np; page_offset_spring.target = np * (double)W; }
    gesture->edge_t = now;
  }

  if (fabs(page_offset_spring.x - page_offset_spring.target) > S(2)) { gesture->cand_slot = -1; return; }

  int td, ts;
  drag_target(a, px, py, &td, &ts);
  if (td == a->is_dock && ts == slot_of(a)) { gesture->cand_slot = -1; return; }
  if (gesture->cand_slot != ts || gesture->cand_dock != td) {
    gesture->cand_slot = ts; gesture->cand_dock = td; gesture->cand_t = now;
    return;
  }
  if (now - gesture->cand_t < 140.0) return;
  move_app(a, td, ts);
  relayout_animated(a);
  gesture->cand_slot = -1;
}

static void end_drag(void) {
  if (!gesture || strcmp(gesture->mode, "iconDrag")) return;
  App *a = gesture->app;
  if (fabs(page_offset_spring.x - page_offset_spring.target) <= S(2)) {   /* commit a pending rearrangement */
    int td, ts;
    drag_target(a, gesture->px, gesture->py, &td, &ts);
    if (td != a->is_dock || ts != slot_of(a)) { move_app(a, td, ts); layout_grid(); }
  }
  double p = a->is_dock ? 0 : page_offset_spring.x;
  sp_from(&a->ox, (gesture->px - gesture->gx) - (a->icon.x - p));
  sp_from(&a->oy, (gesture->py - gesture->gy) - a->icon.y);
  a->press_scale.target = 1.0;
  free(gesture); gesture = NULL;
  save_layout();
}

/* ---- removing / restoring ---- */
static void remove_app(App *a) {
  if (!is_removable(a) || a->removing) return;
  a->removing = 1;
  a->press_scale.target = 0.0;
}

/* Called every frame: once a removed icon has shrunk away, close the gap. */
static void finalize_removals(void) {
  for (int i = 0; i < app_count; i++) {
    App *a = &apps[i];
    if (!a->removing || a->press_scale.x > 0.06) continue;
    a->removing = 0; a->hidden = 1; a->is_dock = 0;
    sp_snap(&a->press_scale, 1.0);
    relayout_animated(NULL);
    save_layout();
  }
}

static void restore_app(App *a) {
  a->hidden = 0; a->removing = 0;
  move_app(a, 0, 1 << 20);                 /* end of the grid */
  relayout_animated(a);
  a->press_scale.x = 0; a->press_scale.v = 0; a->press_scale.target = 1.0;   /* grows in */
  current_page = a->page;
  page_offset_spring.target = current_page * (double)W;
  if (hidden_count() == 0) close_panel();
  save_layout();
}

/* ---- geometry of the edit-mode chrome ---- */
static Rect done_rect(void) { return (Rect){ W - S(20) - S(68), S(8), S(68), S(30), S(15) }; }
static Rect plus_rect(void) { return (Rect){ S(20), S(8), S(30), S(30), S(15) }; }
static int in_rect(Rect r, double x, double y, double pad) {
  return x >= r.x - pad && x <= r.x + r.w + pad && y >= r.y - pad && y <= r.y + r.h + pad;
}

/* The minus badge sits on the icon's top-left corner. */
static void badge_geom(Rect ir, double *cx, double *cy, double *d) {
  *d = fmin(S(26), ir.w * 0.32);
  *cx = ir.x + *d * 0.1;
  *cy = ir.y + *d * 0.1;
}

typedef struct { Rect box; double x0, y0, cell_w, cell_h, icon; int cols; } PanelGeom;
#define PANEL_MAX 20
static void panel_geom(PanelGeom *g, int n) {
  int eff_h = kbd_active ? (int)lround(H * 0.75) : H;
  g->cols = 4;
  int rows = (n + g->cols - 1) / g->cols; if (rows < 1) rows = 1;
  double w = fmin(W - S(40), S(440));
  g->icon = fmin(gm.icon_w * 0.85, S(64));
  g->cell_w = (w - S(32)) / g->cols;
  g->cell_h = g->icon + S(34);
  double h = S(64) + rows * g->cell_h + S(14);
  g->box = (Rect){ (W - w) / 2, (eff_h - h) / 2, w, h, S(24) };
  g->x0 = g->box.x + S(16);
  g->y0 = g->box.y + S(56);
}

/* hidden apps shown in the panel, in display order */
static int panel_list(App **out) {
  int n = 0;
  for (int k = 0; k < app_count && n < PANEL_MAX; k++) {
    App *a = &apps[ord[k]];
    if (a->hidden && !a->removing) out[n++] = a;
  }
  return n;
}

/* ---- drawing ---- */
static void draw_icon_at(GImg *t, App *a, double cx, double cy, double w, double h, double r, double alpha) {
  Rect ir = { cx - w / 2, cy - h / 2, w, h, r };
  if (SNAPSHOT_ICONS && a->state != ST_CLOSED && a->snap.tex) {
    img_draw(t, &a->snap, ir, NULL, alpha);
  } else if (ensure_theme_icon(a)) {
    draw_rgba(t, &a->themeimg, cx - w / 2, cy - h / 2, w, h, alpha);
  } else {
    RR icon = rr_make(ir.x, ir.y, w, h, r);
    fill_rr(t, &icon, a->color, alpha, NULL);
    if (a->builtin) settings_icon(t, cx, cy, w, alpha);
  }
}

static void draw_minus_badge(GImg *t, Rect ir, double alpha) {
  double cx, cy, d;
  badge_geom(ir, &cx, &cy, &d);
  RR disc = rr_make(cx - d / 2, cy - d / 2, d, d, d / 2);
  fill_rr(t, &disc, 0x48484a, 0.96 * alpha, NULL);
  double bw = d * 0.5, bh = fmax(S(2), d * 0.1);
  RR bar = rr_make(cx - bw / 2, cy - bh / 2, bw, bh, bh / 2);
  fill_rr(t, &bar, 0xffffff, alpha, NULL);
}

static void draw_pill(GImg *t, Rect r, const char *label, double alpha) {
  RR pill = rr_make(r.x, r.y, r.w, r.h, r.r);
  fill_rr(t, &pill, 0xffffff, 0.18 * alpha, NULL);
  draw_text(t, &F_card, label, r.x + r.w / 2, BL(r.y + r.h / 2, S(13)), 1.0, 1, 0xffffff, alpha, 0, NULL);
}

/* "Done", "+", the add-panel and the icon being dragged. */
static void draw_edit_chrome(GImg *t, double home_alpha) {
  int eff_h = t->h;
  double es = clampd(edit_spring.x, 0, 1) * home_alpha;

  if (es > 0.01) {
    draw_pill(t, done_rect(), "Done", es);
    if (hidden_count() > 0) {
      Rect pr = plus_rect();
      RR disc = rr_make(pr.x, pr.y, pr.w, pr.h, pr.r);
      fill_rr(t, &disc, 0xffffff, 0.18 * es, NULL);
      double pcx = pr.x + pr.w / 2, pcy = pr.y + pr.h / 2, L = S(14), th = S(2.4);
      RR h1 = rr_make(pcx - L / 2, pcy - th / 2, L, th, th / 2);
      RR v1 = rr_make(pcx - th / 2, pcy - L / 2, th, L, th / 2);
      fill_rr(t, &h1, 0xffffff, es, NULL);
      fill_rr(t, &v1, 0xffffff, es, NULL);
    }
  }

  /* the held icon floats above everything else on the home screen */
  if (gesture && !strcmp(gesture->mode, "iconDrag")) {
    App *a = gesture->app;
    double sc = fmax(0, a->press_scale.x);
    double w = a->icon.w * sc, h = a->icon.h * sc;
    double cx = gesture->px - gesture->gx + a->icon.w / 2, cy = gesture->py - gesture->gy + a->icon.h / 2;
    draw_card_shadow(t, (Rect){ cx - w / 2, cy - h / 2, w, h, a->icon.r * sc }, 0.5);
    draw_icon_at(t, a, cx, cy, w, h, a->icon.r * sc, home_alpha);
    draw_text(t, &F_lbl, a->name, cx, cy + h / 2 + S(12), 1.0, 1, 0xf2eefc, home_alpha, 0, NULL);
  }

  double pk = clampd(panel_spring.x, 0, 1) * home_alpha;
  if (pk > 0.01) {
    App *list[PANEL_MAX];
    int n = panel_list(list);
    PanelGeom g; panel_geom(&g, n);
    fill_rect(t, 0, 0, W, eff_h, 0x000000, 0.5 * pk);
    RR box = rr_make(g.box.x, g.box.y, g.box.w, g.box.h, g.box.r);
    fill_rr(t, &box, 0x2a2640, 0.97 * pk, NULL);
    draw_text(t, &F_hdr, "Add to Home Screen", g.box.x + g.box.w / 2, g.box.y + S(36), 1.0, 1, 0xf2eefc, pk, 0, NULL);
    for (int i = 0; i < n; i++) {
      double cx = g.x0 + (i % g.cols) * g.cell_w + g.cell_w / 2;
      double iy = g.y0 + (i / g.cols) * g.cell_h;
      draw_icon_at(t, list[i], cx, iy + g.icon / 2, g.icon, g.icon, g.icon * 0.225, pk);
      draw_text(t, &F_lbl, list[i]->name, cx, iy + g.icon + S(14), 1.0, 1, 0xf2eefc, pk, 0, NULL);
    }
  }
}

/* ---- touch handling while editing ---- */
static int edit_pointer_down(double x, double y) {
  gesture = calloc(1, sizeof *gesture);
  gesture->start_x = x; gesture->start_y = y; gesture->px = x; gesture->py = y;
  gesture->down_t = now_ms();

  if (add_panel_open) { strcpy(gesture->mode, "panelTap"); return 1; }
  if (in_rect(done_rect(), x, y, S(6))) { strcpy(gesture->mode, "doneTap"); return 1; }
  if (hidden_count() > 0 && in_rect(plus_rect(), x, y, S(6))) { strcpy(gesture->mode, "plusTap"); return 1; }

  for (int i = 0; i < app_count; i++) {          /* badges first: they overhang the icon's corner */
    App *a = &apps[i];
    if (a->hidden || a->removing || !is_removable(a)) continue;
    double bx, by, bd;
    badge_geom(icon_rect(a), &bx, &by, &bd);
    if (hypot(x - bx, y - by) < fmax(bd * 0.8, S(16))) {
      strcpy(gesture->mode, "badgeTap"); gesture->app = a;
      return 1;
    }
  }
  for (int i = 0; i < app_count; i++) {
    App *a = &apps[i];
    if (a->hidden || a->removing) continue;
    Rect r = icon_rect(a);
    if (in_rect(r, x, y, 0)) {
      strcpy(gesture->mode, "editPress"); gesture->app = a;
      return 1;
    }
  }
  strcpy(gesture->mode, "pagePan");
  gesture->page_start_offset = page_offset_spring.x;
  return 1;
}

static void edit_pointer_up(double x, double y) {
  const char *m = gesture->mode;
  int tap = !gesture->moved;
  if (!strcmp(m, "doneTap")) { if (tap && in_rect(done_rect(), x, y, S(10))) exit_edit_mode(); }
  else if (!strcmp(m, "plusTap")) {
    if (tap && in_rect(plus_rect(), x, y, S(10))) { add_panel_open = 1; panel_spring.target = 1.0; }
  } else if (!strcmp(m, "badgeTap")) {
    double bx, by, bd;
    badge_geom(icon_rect(gesture->app), &bx, &by, &bd);
    if (hypot(x - bx, y - by) < fmax(bd * 0.8, S(16)) + S(8)) remove_app(gesture->app);
  } else if (!strcmp(m, "panelTap")) {
    App *list[PANEL_MAX];
    int n = panel_list(list);
    PanelGeom g; panel_geom(&g, n);
    if (tap) {
      int hit = -1;
      for (int i = 0; i < n; i++) {
        double cx = g.x0 + (i % g.cols) * g.cell_w + g.cell_w / 2;
        double iy = g.y0 + (i / g.cols) * g.cell_h;
        if (fabs(x - cx) < g.cell_w / 2 && y >= iy - S(4) && y <= iy + g.cell_h - S(4)) { hit = i; break; }
      }
      if (hit >= 0) restore_app(list[hit]);
      else if (!in_rect(g.box, x, y, 0)) close_panel();
    }
  }
  /* "editPress" with no movement does nothing: icons don't launch while editing */
  free(gesture); gesture = NULL;
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
  if (y > eff_h - edge && (active_app || has_running) && !edit_mode) {   /* editing owns the dock's lower edge */
    gesture = calloc(1, sizeof *gesture);
    strcpy(gesture->mode, "drag");
    gesture->rect = active_app ? full_rect() : (Rect){0};
    if (active_app && !active_app->builtin) {
      grab_snapshot(active_app);   /* the screenshot is taken the moment the bar is touched */
      app_cover(active_app);       /* from here on the shell scales the snapshot, not the window */
    }
    gesture->start_x = x; gesture->start_y = y; gesture->px = x; gesture->py = y;
    gesture->last_move_t = now_ms(); gesture->last_move_x = x; gesture->last_move_y = y;
    return 1;
  }

  if (!active_app && edit_mode) return edit_pointer_down(x, y);

  if (!active_app) {
    for (int i = 0; i < app_count; i++) {
      App *a = &apps[i];
      if (a->hidden) continue;
      Rect r = icon_rect(a);
      if (x >= r.x && x <= r.x + r.w && y >= r.y && y <= r.y + r.h) {
        a->press_scale.target = 0.88;
        gesture = calloc(1, sizeof *gesture);
        strcpy(gesture->mode, "iconPress");
        gesture->app = a;
        gesture->start_x = x; gesture->start_y = y; gesture->px = x; gesture->py = y;
        gesture->down_t = now_ms();
        return 1;
      }
    }

    gesture = calloc(1, sizeof *gesture);
    strcpy(gesture->mode, "pagePan");
    gesture->start_x = x; gesture->start_y = y; gesture->px = x; gesture->py = y;
    gesture->down_t = now_ms();
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
  if (!gesture->moved && hypot(x - gesture->start_x, y - gesture->start_y) > S(8)) gesture->moved = 1;

  if (strcmp(gesture->mode, "iconDrag") == 0) return;           /* drawn from the finger; drag_tick() reorders */
  if (strcmp(gesture->mode, "editPress") == 0) {
    if (gesture->moved) begin_drag(gesture->app, x, y);        /* pick the icon up */
    return;
  }

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
    if (!active_app->builtin) flight_app = active_app;   /* following the finger toward the icon */
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

  if (strcmp(gesture->mode, "iconDrag") == 0) { end_drag(); return; }
  if (strcmp(gesture->mode, "editPress") == 0 || strcmp(gesture->mode, "doneTap") == 0 ||
      strcmp(gesture->mode, "plusTap") == 0 || strcmp(gesture->mode, "badgeTap") == 0 ||
      strcmp(gesture->mode, "panelTap") == 0) { edit_pointer_up(x, y); return; }

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
        free(gesture); gesture = NULL;   /* the window is revealed once the snapshot is back */
      }
    } else { free(gesture); gesture = NULL; }
  }
}

/* ------------------------------------------------------------------------ */
/* COMPOSITOR                                                               */
/* ------------------------------------------------------------------------ */
static int ensure_theme_icon(App *a);

static void draw_app_preview(GImg *t, App *app, Rect r) {
  if (app->snap.tex) { img_draw(t, &app->snap, r, NULL, 1.0); return; }
  /* placeholder card (no snapshot yet): plain white with a dark home bar, no name */
  RR clip = rr_make(r.x, r.y, r.w, r.h, r.r);
  fill_rr(t, &clip, 0xffffff, 1.0, NULL);
  double bar_w = fmin(S(134), r.w * 0.35);
  RR homebar = rr_make(r.x + (r.w - bar_w) / 2, r.y + r.h - S(12), bar_w, S(5), S(2.5));
  fill_rr(t, &homebar, 0x1c1c1e, 0.9, &clip);
}

/* A window drawn as a card: shadow + rounded preview. `shadow` is the overall strength. */
static void draw_card(GImg *t, App *app, Rect r, double shadow) {
  draw_card_shadow(t, r, shadow * clampd(r.r / CARD_R_PX, 0, 1));
  draw_app_preview(t, app, r);
}

/* Home layer: dock, icons, labels, page dots. With use_target every spring is taken at its
   resting value. */
static void draw_home(GImg *t, double hs, double p_off, double home_alpha, int use_target) {
  int eff_h = t->h;
  double hcx = W / 2.0, hcy = eff_h / 2.0;

  Rect dp = rs_val(&dock_spring);
  RR dbox = rr_make(hcx + (dp.x - hcx) * hs, hcy + (dp.y - hcy) * hs,
                    dp.w * hs, dp.h * hs, dp.r * hs);
  fill_rr(t, &dbox, 0xffffff, 0.15 * home_alpha, NULL);

  double es = use_target ? 0 : clampd(edit_spring.x, 0, 1);
  double secs = now_ms() / 1000.0;
  int dragging = gesture && strcmp(gesture->mode, "iconDrag") == 0;

  for (int i = 0; i < app_count; i++) {
    App *a = &apps[i];
    if (active_app == a && gesture) continue;
    if (a->hidden || (dragging && gesture->app == a)) continue;
    double ps = fmax(0, (use_target ? a->press_scale.target : a->press_scale.x)) * hs;
    Rect r = a->icon;
    if (!use_target) { r.x += a->ox.x; r.y += a->oy.x; }
    if (!a->is_dock) r.x -= p_off;
    if (r.x + r.w < -100 || r.x > W + 100) continue;

    double jx = 0, jy = 0;                 /* edit-mode jiggle */
    if (es > 0.001 && !a->removing) {
      double ph = a->id * 2.399;
      jx = sin(secs * 34 + ph) * S(0.8) * es;
      jy = cos(secs * 29 + ph * 1.7) * S(0.8) * es;
      ps *= 1.0 + 0.012 * sin(secs * 31 + ph) * es;
    }

    double cx = hcx + (r.x + r.w / 2 - hcx) * hs + jx;
    double cy = hcy + (r.y + r.h / 2 - hcy) * hs + jy;
    double w = r.w * ps, h = r.h * ps;
    draw_icon_at(t, a, cx, cy, w, h, r.r * ps, home_alpha);
    double la = a->removing ? home_alpha * clampd(ps / hs, 0, 1) : home_alpha;
    draw_text(t, &F_lbl, a->name, cx, cy + h / 2 + S(12) * hs, hs, 1, 0xf2eefc, la, 0, NULL);
    if (es > 0.01 && is_removable(a) && !a->removing)
      draw_minus_badge(t, (Rect){ cx - w / 2, cy - h / 2, w, h, 0 }, es * home_alpha);
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

  if (!use_target) draw_edit_chrome(t, home_alpha);
}

/* Wallpaper. Decoded once, uploaded as a premultiplied texture, drawn cover-cropped by img_draw.
   No file (or a file that will not decode) keeps the gradient. The choice from Display is one path
   per line in $XDG_CONFIG_HOME/summerboard/wallpaper; an empty file means the gradient. */
static int config_wallpaper_file(char *out, size_t n, int make) {
  const char *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
  char dir[400];
  if (xdg && *xdg) snprintf(dir, sizeof dir, "%s/summerboard", xdg);
  else if (home && *home) {
    if (make) { char dot[400]; snprintf(dot, sizeof dot, "%s/.config", home); mkdir(dot, 0755); }
    snprintf(dir, sizeof dir, "%s/.config/summerboard", home);
  } else return 0;
  if (make) mkdir(dir, 0755);
  snprintf(out, n, "%s/wallpaper", dir);
  return 1;
}

static void wallpaper_remember(const char *path) {
  char fpath[512];
  if (!config_wallpaper_file(fpath, sizeof fpath, 1)) return;
  FILE *f = fopen(fpath, "w");
  if (!f) return;
  if (path && path[0]) fprintf(f, "%s\n", path);
  fclose(f);
}

static int wallpaper_pick(char *out, size_t n) {
  out[0] = 0;
  if (wallpaper_arg[0]) { snprintf(out, n, "%s", wallpaper_arg); return 1; }
  const char *env = getenv("SUMMERBOARD_BG");
  if (env && env[0]) { snprintf(out, n, "%s", env); return 1; }
  char savedf[512];
  if (config_wallpaper_file(savedf, sizeof savedf, 0) && access(savedf, R_OK) == 0) {
    FILE *f = fopen(savedf, "r");
    if (f) {
      char line[512];
      if (!fgets(line, sizeof line, f)) line[0] = 0;
      fclose(f);
      line[strcspn(line, "\r\n")] = 0;
      if (!line[0]) return 0;                 /* Display chose the gradient */
      if (access(line, R_OK) == 0) { snprintf(out, n, "%s", line); return 1; }
    }
  }
  const char *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
  char dir[400];
  if (xdg && *xdg) snprintf(dir, sizeof dir, "%s/summerboard", xdg);
  else if (home && *home) snprintf(dir, sizeof dir, "%s/.config/summerboard", home);
  else return 0;
  static const char *names[] = { "background.png", "background.jpg", "background.jpeg", "background", NULL };
  for (int i = 0; names[i]; i++) {
    snprintf(out, n, "%s/%s", dir, names[i]);
    if (access(out, R_OK) == 0) return 1;
  }
  out[0] = 0;
  return 0;
}

/* Area-average shrink so a camera photo fits the GPU (and does not sit in RAM at full size). */
static unsigned char *bg_shrink(const unsigned char *src, int sw, int sh, int dw, int dh) {
  unsigned char *dst = malloc((size_t)dw * dh * 4);
  if (!dst) return NULL;
  for (int y = 0; y < dh; y++) {
    int y0 = (int)((long)y * sh / dh), y1 = (int)((long)(y + 1) * sh / dh);
    if (y1 <= y0) y1 = y0 + 1;
    if (y1 > sh) y1 = sh;
    for (int x = 0; x < dw; x++) {
      int x0 = (int)((long)x * sw / dw), x1 = (int)((long)(x + 1) * sw / dw);
      if (x1 <= x0) x1 = x0 + 1;
      if (x1 > sw) x1 = sw;
      unsigned r = 0, g = 0, b = 0, a = 0, n = 0;
      for (int yy = y0; yy < y1; yy++) {
        const unsigned char *row = src + ((size_t)yy * sw) * 4;
        for (int xx = x0; xx < x1; xx++) {
          const unsigned char *p = row + xx * 4;
          r += p[0]; g += p[1]; b += p[2]; a += p[3];
          n++;
        }
      }
      unsigned char *o = dst + ((size_t)y * dw + x) * 4;
      if (!n) n = 1;
      o[0] = (unsigned char)(r / n); o[1] = (unsigned char)(g / n);
      o[2] = (unsigned char)(b / n); o[3] = (unsigned char)(a / n);
    }
  }
  return dst;
}

static int wallpaper_apply(const char *path, int remember) {
  if (!path || !path[0]) {
    img_free(&wallpaper);
    wallpaper_path[0] = 0;
    wall_cur = -1;
    if (remember) wallpaper_remember(NULL);
    return 1;
  }
  int w = 0, h = 0, comp = 0;
  unsigned char *rgba = stbi_load(path, &w, &h, &comp, 4);
  if (!rgba || w < 1 || h < 1) {
    const char *why = stbi_failure_reason();
    fprintf(stderr, "summerboard: cannot read wallpaper %s (%s)\n", path, why ? why : "unknown");
    if (rgba) stbi_image_free(rgba);
    return 0;
  }
  GLint max_tex = 0;
  glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_tex);
  int lim = (int)max_tex;
  if (lim < 64 || lim > 2048) lim = 2048;   /* the panel is 1024x768; more only costs RAM */
  unsigned char *use = rgba, *shrunk = NULL;
  int uw = w, uh = h;
  if (w > lim || h > lim) {
    double s = (double)lim / (double)(w > h ? w : h);
    uw = (int)lround(w * s); uh = (int)lround(h * s);
    if (uw < 1) uw = 1;
    if (uh < 1) uh = 1;
    shrunk = bg_shrink(rgba, w, h, uw, uh);
    if (shrunk) use = shrunk;
    else { uw = w; uh = h; }
  }
  uint32_t *pm = malloc((size_t)uw * uh * 4);
  if (pm) {
    for (int i = 0; i < uw * uh; i++) {     /* straight RGBA -> premultiplied 0xAARRGGBB, as img_upload wants */
      unsigned r = use[i * 4], g = use[i * 4 + 1], b = use[i * 4 + 2], a = use[i * 4 + 3];
      r = (r * a + 127) / 255; g = (g * a + 127) / 255; b = (b * a + 127) / 255;
      pm[i] = (a << 24) | (r << 16) | (g << 8) | b;
    }
    img_upload(&wallpaper, pm, uw, uh);
    free(pm);
  }
  free(shrunk);
  stbi_image_free(rgba);
  if (!wallpaper.tex) { fprintf(stderr, "summerboard: wallpaper %s did not upload\n", path); return 0; }
  snprintf(wallpaper_path, sizeof wallpaper_path, "%s", path);
  wall_mark();
  if (remember) wallpaper_remember(path);
  fprintf(stderr, "summerboard: wallpaper %s (%dx%d)\n", path, wallpaper.w, wallpaper.h);
  return 1;
}

static void wallpaper_load(void) {
  wall_scan();
  char path[512];
  if (!wallpaper_pick(path, sizeof path) || !path[0]) return;
  wallpaper_apply(path, 0);
}

static void draw_background(GImg *t, int eff_h) {
  if (wallpaper.tex)
    img_draw(t, &wallpaper, (Rect){ 0, 0, (double)t->w, (double)eff_h, 0 }, NULL, 1.0);
  else
    gl_gradient(t, eff_h, 0x1c1730, 0x0a0814);
}

static void render_frame(void) {
  int eff_h = kbd_active ? (int)lround(H * 0.75) : H;
  GImg *t = &gl_screen;
  draw_background(t, eff_h);

  Rect rect = rs_val(&win_spring);
  /* A window flying home after the app was minimised: active_app is already NULL, but it is
     still on screen until the spring lands. */
  int fl_busy = !active_app && !switcher.open && flight_app && flight_app->state == ST_BG && rs_busy(&win_spring);
  if (!active_app && !fl_busy) flight_app = NULL;
  App *shown = active_app ? active_app : (fl_busy ? flight_app : NULL);

  double home_cover = 0;
  if (shown) {
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

  if (home_alpha > 0.01) {
    double hs = home_scale_spring.x * (1.0 - 0.12 * home_recede);
    draw_home(t, hs, page_offset_spring.x, home_alpha, 0);
  }

  /* Hold-to-switch: the other cards fly in around the live window. */
  if (gesture && strcmp(gesture->mode, "drag") == 0 && gesture->hold_fired && !switcher.open) {
    Rect anchor = live_anchor_rect();
    int list[MAX_APPS], nlist = 0;
    for (int i = 0; i < app_count; i++) if (apps[i].state != ST_CLOSED) list[nlist++] = i;
    int active_idx = 0;
    if (active_app) {
      for (int i = 0; i < nlist; i++) if (&apps[list[i]] == active_app) { active_idx = i; break; }
    } else active_idx = (nlist - 1) / 2;

    double ak = appear_ease(clampd((now_ms() - gesture->appear_t0) / APPEAR_DUR, 0, 1));
    fill_rect(t, 0, 0, W, eff_h, 0x000000, 0.55 * ak);

    for (int i = 0; i < nlist; i++) {
      App *a = &apps[list[i]];
      if (a == active_app) continue;
      double live_x = anchor.x + (i - active_idx) * (anchor.w + GAP_PX);
      double off_x = i < active_idx ? -anchor.w : W;
      Rect b = { off_x + (live_x - off_x) * ak, anchor.y, anchor.w, anchor.h, anchor.r };
      draw_card(t, a, b, 1.0);
    }
  }

  if (shown && (fl_busy || shown->builtin || shown->covered || gesture || switcher.open || win_spring.w.x < W - 2)) {
    if (rect.w > 1 && rect.h > 1) {
      if (shown->builtin) {
        builtin_paint(t, shown, rect);      /* no X window: the compositor draws it live, even at rest */
      } else if (shown == flight_app) {     /* icon <-> window: pinched, squared, corners rise and land */
        Rect rest = full_rect();
        rest.h = eff_h - (int)lround(S(12));
        draw_flight(t, shown->snap.tex ? &shown->snap : &gl_white, rect, icon_rect(shown), rest);
      } else {
        draw_card(t, shown, rect, 1.0);     /* card <-> window: rounded corners + shadow */
      }
    }
  }

  if (switcher.open) {
    fill_rect(t, 0, 0, W, eff_h, 0x000000, 0.55 * (1.0 - 0.5 * exit_k));
    double sc = 1.0 - 0.12 * exit_k;
    for (int i = 0; i < switcher.ncards; i++) {
      Card *c = &switcher.cards[i];
      Rect b = card_render_box(i);
      double ccx = b.x + b.w / 2, ccy = b.y + b.h / 2;
      double w = b.w * sc, h = b.h * sc;
      Rect card_r = { ccx - w / 2, ccy - h / 2 + exit_k * S(26), w, h, b.r };
      if (card_r.x + card_r.w < 0 || card_r.x > W) continue;
      draw_card_shadow(t, card_r, clampd(card_r.r / CARD_R_PX, 0, 1));
      draw_app_preview(t, c->app, card_r);
      draw_text(t, &F_card, c->app->name, ccx, card_r.y - S(14), 1.0, 1, 0xffffff, 1.0 - exit_k, 0, NULL);
    }
  }

  if (active_app && !switcher.open) {
    double bar_w = S(134);
    RR homebar = rr_make((W - bar_w) / 2, eff_h - S(10), bar_w, S(5), S(2.5));
    fill_rr(t, &homebar, 0xffffff, 0.9, NULL);
  }

  /* Spring settled at rest with nobody touching: hand the screen to the real window (this frame
     still carried the snapshot, so there is no gap). */
  if (active_app && active_app->covered && !active_app->builtin && !gesture && !switcher.open
      && !rs_busy(&win_spring)) {
    App *a = active_app;
    Rect rest = full_rect();
    rest.h = eff_h - (int)lround(S(12));
    a->covered = 0;
    sync_window_geometry_ex(a, rest, 1);
    app_raise(a);
    snap_drop(a);
  }
}

/* ---- WhiteSur icons: rasterise once per size, upload as a premultiplied texture ---- */
static NSVGrasterizer *svg_rast;

static int theme_raster(const char *path, int n, GImg *out) {
  NSVGimage *img = nsvgParseFromFile(path, "px", 96.0f);
  if (!img) return 0;
  if (img->width <= 0 || img->height <= 0) { nsvgDelete(img); return 0; }
  if (!svg_rast) svg_rast = nsvgCreateRasterizer();
  if (!svg_rast) { nsvgDelete(img); return 0; }
  float sc = (float)n / fmaxf(img->width, img->height);
  float tx = ((float)n - img->width * sc) / 2.f, ty = ((float)n - img->height * sc) / 2.f;
  unsigned char *buf = calloc((size_t)n * n, 4);
  if (!buf) { nsvgDelete(img); return 0; }
  nsvgRasterize(svg_rast, img, tx, ty, sc, buf, n, n, n * 4);
  {
    /* WhiteSur keeps a transparent margin inside its canvas; trim it so the icon fills the same
       grid cell as a plain tile (re-rasterise once, slightly enlarged). */
    int bx0 = n, by0 = n, bx1 = -1, by1 = -1;
    for (int y = 0; y < n; y++)
      for (int x = 0; x < n; x++)
        if (buf[((size_t)y * n + x) * 4 + 3] > 16) {
          if (x < bx0) bx0 = x;
          if (x > bx1) bx1 = x;
          if (y < by0) by0 = y;
          if (y > by1) by1 = y;
        }
    if (bx1 >= 0) {
      float f = (float)n / fmaxf((float)(bx1 - bx0 + 1), (float)(by1 - by0 + 1));
      if (f > 1.02f && f <= 1.2f) {
        float ux = ((bx0 + bx1 + 1) / 2.f - tx) / sc, uy = ((by0 + by1 + 1) / 2.f - ty) / sc;
        float sc2 = sc * f;
        memset(buf, 0, (size_t)n * n * 4);
        nsvgRasterize(svg_rast, img, n / 2.f - ux * sc2, n / 2.f - uy * sc2, sc2, buf, n, n, n * 4);
      }
    }
  }
  nsvgDelete(img);
  uint32_t *pm = malloc((size_t)n * n * 4);
  if (!pm) { free(buf); return 0; }
  for (int i = 0; i < n * n; i++) {          /* RGBA straight -> premultiplied 0xAARRGGBB */
    unsigned r = buf[i * 4], g = buf[i * 4 + 1], b = buf[i * 4 + 2], a = buf[i * 4 + 3];
    r = (r * a + 127) / 255; g = (g * a + 127) / 255; b = (b * a + 127) / 255;
    pm[i] = (a << 24) | (r << 16) | (g << 8) | b;
  }
  free(buf);
  img_upload(out, pm, n, n);
  free(pm);
  return 1;
}

static int ensure_theme_icon(App *a) {
  if (a->theme_state < 0) return 0;
  int n = (int)ceil(a->icon.w);
  if (n < 2) return 0;
  if (a->themeimg.tex && a->themeimg.w == n) return 1;
  if (!a->theme_path[0] && !theme_find(a, a->theme_path, sizeof a->theme_path)) { a->theme_state = -1; return 0; }
  if (!theme_raster(a->theme_path, n, &a->themeimg)) { a->theme_state = -1; return 0; }
  a->theme_state = 1;
  return 1;
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

  XSetWindowBorderWidth(dpy, e->window, 0);   /* a border pushes the content off-screen */
  XMoveResizeWindow(dpy, e->window, 0, 0, W, target_h);
  for (int i = 0; i < a->nwin; i++)
    if (a->wins[i] == e->window) { a->sent_win[i] = None; }   /* force next sync to resend */
  a->state = ST_FG;
  active_app = a;

  if (rs_busy(&win_spring) || gesture) {
    /* Launching: keep the window unmapped at its rest size until the flight lands. */
    a->covered = 1;
    return;
  }
  a->covered = 0;
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
  wc.border_width = 0;
  e->value_mask |= CWBorderWidth;
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
/* True while something on screen is time-driven and needs continuous frames. */
static int anim_active(void) {
  if (settings_animating()) return 1;
  if (rs_busy(&win_spring)) return 1;
  if (sp_busy(&home_scale_spring) || sp_busy(&page_offset_spring) || sp_busy(&switcher_scroll_spring)) return 1;
  for (int i = 0; i < app_count; i++)
    if (sp_busy(&apps[i].press_scale) || sp_busy(&apps[i].ox) || sp_busy(&apps[i].oy)) return 1;
  if (edit_mode || sp_busy(&edit_spring) || sp_busy(&panel_spring) || rs_busy(&dock_spring)) return 1;   /* jiggle */
  if (gesture && !edit_mode && !gesture->moved &&
      (strcmp(gesture->mode, "iconPress") == 0 || strcmp(gesture->mode, "pagePan") == 0)) return 1;   /* long-press timer */
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

/* ------------------------------------------------------------------------ */
/* MAIN                                                                     */
/* The root window changed size (RandR rotation). Everything sized to the old screen is rebuilt. */
static void screen_resized(int nw, int nh) {
  if (nw <= 0 || nh <= 0 || (nw == W && nh == H)) return;
  W = nw; H = nh;
  int eff_h = kbd_active ? (int)lround(H * 0.75) : H;
  XResizeWindow(dpy, shell, (unsigned)W, (unsigned)eff_h);
  if (kbd_win != None) XMoveResizeWindow(dpy, kbd_win, 0, eff_h, (unsigned)W, (unsigned)(H - eff_h));
  gl_resize();
  layout_grid();
  sp_snap(&page_offset_spring, current_page * (double)W);
  rs_snap(&dock_spring, dock_box);
  if (switcher.open) sp_snap(&switcher_scroll_spring, 0);
  for (int i = 0; i < app_count; i++)
    for (int k = 0; k < apps[i].nwin; k++) apps[i].sent_win[k] = None;   /* resend each window's geometry */
  if (active_app && !gesture) {
    Rect full = full_rect();
    full.h -= (int)lround(S(12));
    rs_snap(&win_spring, full);
    /* The live window is mapped at the old size. Clearing sent_win does not send anything until
       the next sync, which does not happen while the app is sitting still. */
    if (!active_app->builtin && active_app->nwin > 0 && !active_app->covered)
      sync_window_geometry_ex(active_app, full, 1);
  }
  /* Snapshots were taken in the old orientation. The next home gesture grabs the live window again;
     a background window is unmapped, so its picture is dropped instead of shown sideways. */
  for (int i = 0; i < app_count; i++) {
    if (apps[i].builtin) { if (apps[i].state != ST_CLOSED) settings_snapshot(&apps[i]); }
    else if (apps[i].snap.tex) snap_drop(&apps[i]);
  }
  XSync(dpy, False);
}

/* ------------------------------------------------------------------------ */
int main(int argc, char **argv) {
  autorotate_start();                       /* before the reaper and the X connection: the helper inherits neither */
  install_sigchld_reaper();
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "-v")) dbg_on = 1;
    else if (!strcmp(argv[i], "-m")) opt_mock_wifi = 1;   /* Settings: use fake Wi-Fi networks */
    else if (!strcmp(argv[i], "-i") && i + 1 < argc) snprintf(opt_wifi_if, sizeof opt_wifi_if, "%s", argv[++i]);
    else if ((!strcmp(argv[i], "-b") || !strcmp(argv[i], "--background")) && i + 1 < argc)
      snprintf(wallpaper_arg, sizeof wallpaper_arg, "%s", argv[++i]);
  }

  dpy = XOpenDisplay(NULL);
  if (!dpy) { fprintf(stderr, "summerboard: cannot open X display\n"); return 1; }
  scr = DefaultScreen(dpy);
  root = RootWindow(dpy, scr);
  W = DisplayWidth(dpy, scr);
  H = DisplayHeight(dpy, scr);
  depth = DefaultDepth(dpy, scr);

  /* The same in both orientations, so rotating never changes the font and icon scale. */
  UI = fmax(1.0, fmin((double)fmax(W, H) / 1024.0, (double)fmin(W, H) / 768.0));

  XSetWindowAttributes wa = { .event_mask = StructureNotifyMask | SubstructureRedirectMask | SubstructureNotifyMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask };
  XChangeWindowAttributes(dpy, root, CWEventMask, &wa);

  /* Alpine has no cursor theme, so the server draws the built-in X cursor whenever a window
     leaves its cursor unset. Hide it for the screen; touch still delivers pointer events. */
  {
    int evb, errb;
    if (XFixesQueryExtension(dpy, &evb, &errb)) XFixesHideCursor(dpy, root);
    Pixmap pm = XCreatePixmap(dpy, root, 1, 1, 1);
    GC gc = XCreateGC(dpy, pm, 0, NULL);
    XSetForeground(dpy, gc, 0);
    XFillRectangle(dpy, pm, gc, 0, 0, 1, 1);
    XFreeGC(dpy, gc);
    XColor empty = {0};
    Cursor blank = XCreatePixmapCursor(dpy, pm, pm, &empty, &empty, 0, 0);
    XFreePixmap(dpy, pm);
    if (blank) XDefineCursor(dpy, root, blank);
  }

  XSync(dpy, False);                     /* surfaces BadAccess if another WM is running */
  XSetErrorHandler(wm_error_handler);

  if (!gl_init(&shell)) {
    fprintf(stderr, "summerboard: OpenGL initialisation failed (is Mesa / GLX available?)\n");
    return 1;
  }
  wallpaper_load();

  font_build(&F_lbl, S(11), 0);
  font_build(&F_card, S(13), 1);
  font_build(&F_hdr, S(20), 1);
  font_build(&F_hud, S(11), 1);
  font_build(&F_body, S(16), 0);
  font_build(&F_big, S(32), 1);

  settings_init();
  scan_system_apps();
  sp_init(&edit_spring, 0.0, 300, 26, 0.001);
  sp_init(&panel_spring, 0.0, 320, 30, 0.001);
  load_layout();
  layout_grid();

  rs_init(&win_spring, (Rect){0,0,0,0,0}, 280, 30);
  sp_init(&home_scale_spring, 1.0, 240, 26, 0.001);
  sp_init(&switcher_scroll_spring, 0.0, 260, 27, 0.05);
  sp_init(&page_offset_spring, 0.0, 260, 28, 0.05);

  double last_time = now_ms();
  int xfd = ConnectionNumber(dpy);
  home_button_init();
  {   /* a rotation that happened while we were starting up */
    Window rr; int gx, gy; unsigned gw, gh, gb, gd;
    if (XGetGeometry(dpy, root, &rr, &gx, &gy, &gw, &gh, &gb, &gd)) screen_resized((int)gw, (int)gh);
  }
  int dirty = 1;   /* force first frame */
  double next_frame = 0;
  const double frame_ms = 1000.0 / 60.0;

  while (1) {
    if (home_button_poll()) dirty = 1;
    while (XPending(dpy)) {
      XEvent ev;
      XNextEvent(dpy, &ev);
      switch (ev.type) {
        case ConfigureNotify:
          if (ev.xconfigure.window == root) { screen_resized(ev.xconfigure.width, ev.xconfigure.height); dirty = 1; }
          break;
        case MapRequest: on_map_request(&ev.xmaprequest); dirty = 1; break;
        case ConfigureRequest: on_configure_request(&ev.xconfigurerequest); break;
        case UnmapNotify: on_unmap_notify(&ev.xunmap); dirty = 1; break;
        case DestroyNotify: on_destroy_notify(&ev.xdestroywindow); dirty = 1; break;
        case Expose: dirty = 1; break;
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
        case KeyPress:
          if (edit_mode && XLookupKeysym(&ev.xkey, 0) == XK_Escape) {
            if (add_panel_open) close_panel(); else exit_edit_mode();
          }
          settings_key(&ev.xkey); dirty = 1; break;
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
      int nfds = xfd;
      if (home_fd >= 0) { FD_SET(home_fd, &idle_fds); if (home_fd > nfds) nfds = home_fd; }
      select(nfds + 1, &idle_fds, NULL, NULL, NULL);
      last_time = now_ms() - 16.0;   /* don't feed a huge dt into the springs after idling */
      next_frame = 0;
      continue;
    }

    double now = now_ms();
    if (now < next_frame) {
      fd_set wf; FD_ZERO(&wf); FD_SET(xfd, &wf);
      int wn = xfd;
      if (home_fd >= 0) { FD_SET(home_fd, &wf); if (home_fd > wn) wn = home_fd; }
      long us = (long)((next_frame - now) * 1000.0);
      struct timeval wtv = { .tv_sec = 0, .tv_usec = us < 200 ? 200 : us };
      select(wn + 1, &wf, NULL, NULL, &wtv);
      continue;
    }
    double dt = clampd((now - last_time) / 1000.0, 0.001, 0.05);
    last_time = now;

    rs_update(&win_spring, dt);
    rs_update(&dock_spring, dt);
    sp_update(&edit_spring, dt);
    sp_update(&panel_spring, dt);
    sp_update(&home_scale_spring, dt);
    sp_update(&switcher_scroll_spring, dt);
    sp_update(&page_offset_spring, dt);
    settings_tick(dt);
    for (int i = 0; i < app_count; i++) {
      sp_update(&apps[i].press_scale, dt);
      sp_update(&apps[i].ox, dt);
      sp_update(&apps[i].oy, dt);
    }
    if (switcher.open) {
      for (int i = 0; i < switcher.ncards; i++) sp_update(&switcher.cards[i].vy_sp, dt);
    }

    /* Touch and hold on an icon (or on empty space) starts editing; the held icon is picked up. */
    if (gesture && !edit_mode && !active_app && !switcher.open && !gesture->moved
        && now - gesture->down_t > LONGPRESS_MS
        && (!strcmp(gesture->mode, "iconPress") || !strcmp(gesture->mode, "pagePan"))) {
      if (!strcmp(gesture->mode, "iconPress")) {
        App *a = gesture->app;
        a->press_scale.target = 1.0;
        enter_edit_mode();
        begin_drag(a, gesture->px, gesture->py);
      } else {
        page_offset_spring.target = current_page * (double)W;
        enter_edit_mode();
        free(gesture); gesture = NULL;
      }
    }
    drag_tick(now);
    finalize_removals();

    int eff_h = kbd_active ? (int)lround(H * 0.75) : H;
    if (gesture && strcmp(gesture->mode, "drag") == 0 && !gesture->hold_fired
        && gesture->py < eff_h * 0.85
        && (now - gesture->last_move_t) > (active_app ? 450.0 : 60.0)) {
      gesture->hold_fired = 1;
      gesture->appear_t0 = now;
    }

    gl_begin_frame(eff_h);
    render_frame();
    gl_end_frame();
    dirty = 0;

    /* Deadline pacing: the period is 16.7 ms regardless of render time. */
    if (next_frame == 0) next_frame = now + frame_ms;
    else { next_frame += frame_ms; if (next_frame < now) next_frame = now; }
  }

  return 0;
}
