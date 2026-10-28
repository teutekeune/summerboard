/* summerboard_gl.h  --  EGL / SGX back end
 *
 * Same drawing API as the GLX version (fill_rr, draw_text, img_draw, ... gl_init, gl_begin_frame,
 * gl_end_frame, gl_screen), but built for a box where X has no DRI path to the GPU:
 *
 *   - A headless EGL context (surfaceless Mesa platform, desktop GL 2.1 / GLSL 1.20) is created on the
 *     SGX render node. Nothing in the rendering path goes through X's GLX, which on this machine can
 *     only reach softpipe.
 *   - Everything is drawn into an offscreen FBO (gl_screen). At the end of the frame it is read back
 *     with glReadPixels straight into a MIT-SHM XImage and shown in the shell window with
 *     XShmPutImage (falls back to XPutImage if MIT-SHM is missing).
 *   - SUMMERBOARD_PRESENT=egl (needs X with DRI3, i.e. glamor on the SGX): the frame is instead drawn straight
 *     into an EGL window surface on the X11 platform, with no readback at all and (default) no intermediate
 *     FBO or copy pass either: the scene is rendered directly into the back buffer. SUMMERBOARD_DIRECT=0 brings
 *     the old offscreen FBO + full-screen copy back.
 *   - Fill-rate savings: large rounded rects / images are a flat interior (no SDF; opaque ones with blending
 *     off) plus a thin SDF border ring; shaders are specialised per mode and per clip on/off, built lazily.
 *   - Shaders are GLSL 1.20 (no flat, no in/out, no texture()); atlas is GL_LUMINANCE, not GL_R8.
 *
 *   cc summerboard.c -o summerboard -I/usr/local/lib/sgx-mesa/include -L/usr/local/lib/sgx-mesa/lib \
 *      -lX11 -lXext -lEGL -lGL -lm
 *   run with LD_LIBRARY_PATH=/usr/local/lib/sgx-mesa/lib LIBGL_DRIVERS_PATH=/usr/local/lib/sgx-mesa/lib/dri
 *
 * Design (unchanged):
 *   - Every primitive is a quad appended to one vertex batch. The fragment shader evaluates
 *     the rounded-rect / segment / shadow SDFs analytically.
 *   - The batch is flushed only when the render target or the bound image changes.
 *   - All blending is premultiplied: glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA).
 *   - Render targets are FBO textures with top-left logical coordinates (top row first in memory),
 *     so the FBO can be read back and handed to X as-is.
 */
#ifndef SUMMERBOARD_GL_H
#define SUMMERBOARD_GL_H

#ifndef GL_GLEXT_PROTOTYPES
#define GL_GLEXT_PROTOTYPES
#endif
#include <GL/gl.h>
#include <GL/glext.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <X11/extensions/XShm.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ------------------------------------------------------------------------ */
/* Shapes, glyphs, images                                                   */
/* ------------------------------------------------------------------------ */
typedef struct { float cx, cy, hx, hy, r, x0, y0, x1, y1; } RR;

static RR rr_make(double x, double y, double w, double h, double r) {
  RR c;
  if (w < 0) { x += w; w = -w; }
  if (h < 0) { y += h; h = -h; }
  double m = fmin(w, h) / 2;
  if (r > m) r = m;
  if (r < 0) r = 0;
  c.cx = (float)(x + w / 2); c.cy = (float)(y + h / 2);
  c.hx = (float)(w / 2);     c.hy = (float)(h / 2);
  c.r = (float)r;
  c.x0 = (float)x; c.y0 = (float)y; c.x1 = (float)(x + w); c.y1 = (float)(y + h);
  return c;
}

static uint32_t mix32(uint32_t d, uint32_t s, unsigned a) {   /* CPU colour mix, unchanged API */
  uint32_t rb = (((d & 0xff00ff) * (256 - a) + (s & 0xff00ff) * a) >> 8) & 0xff00ff;
  uint32_t g  = (((d & 0x00ff00) * (256 - a) + (s & 0x00ff00) * a) >> 8) & 0x00ff00;
  return rb | g;
}

/* An image or render target. fbo == 0 means a plain texture. The window itself is gl_screen (fbo 0). */
typedef struct { GLuint tex, fbo; int w, h; } GImg;
static GImg gl_screen;     /* the window; w/h track the current shell size */
static GImg gl_white;      /* 1x1 white, used as the flight placeholder */

typedef struct { int w, h, lb, asc; float adv; uint8_t *bits; uint8_t *cov; int cw, ch; float u0, v0, u1, v1; } Glyph;
typedef struct { XFontStruct *xf; double ss; Glyph g[NGLYPH]; } AppFont;

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
static double text_width(const AppFont *f, const char *s, double scale, double ls) {
  double w = 0;
  while (*s) { uint32_t cp = utf8_next(&s); w += f->g[glyph_index(cp)].adv / f->ss * scale + ls; }
  return w;
}

/* ------------------------------------------------------------------------ */
/* Shaders                                                                  */
/* ------------------------------------------------------------------------ */
#define SB_VF 17   /* 32-bit words per batch vertex: pos2 uv2 col(4 x u8) shape4 par4 clip4 */
#define SB_MAXQ 16384   /* quads per batch (4 vertices each, 16-bit indices) */

/* The batch shader is compiled once per primitive mode (-DMODE) and, for the modes that can use one, once
   with and once without a rounded clip (-DCLIP), so each program is tiny and has no dynamic branch on the
   mode or on the clip flag. Programs are built lazily on first use. Each program only declares and
   interpolates the varyings its mode actually reads.
   mode: 0 solid rounded rect (SDF), 1 glyph (atlas alpha), 2 image with rounded coverage (premultiplied),
         3 vertical gradient (opaque), 4 card shadow (hole = card), 5 round-capped segment,
         6 flat translucent colour (no SDF), 7 flat opaque colour (no SDF, blending off),
         8 flat image (no SDF, premultiplied) */
#define SB_NMODE 9
#define SB_CLIPOFF 16                 /* program index = mode, plus SB_CLIPOFF for the clipped variant */
#define SB_NPROG (2 * SB_CLIPOFF)

/* Prepended to both stages: which varyings exist for this MODE/CLIP. */
static const char *SB_COMMON =
  "#if MODE == 0 || MODE == 2 || MODE == 4 || MODE == 5 || (MODE == 1 && CLIP)\n"
  "#define U_PIX 1\n"
  "#endif\n"
  "#if MODE == 1 || MODE == 2 || MODE == 3 || MODE == 8\n"
  "#define U_UV 1\n"
  "#endif\n"
  "#if MODE == 0 || MODE == 2 || MODE == 4 || MODE == 5\n"
  "#define U_SHAPE 1\n"
  "#endif\n"
  "#if MODE == 0 || MODE == 2 || MODE == 4 || MODE == 5 || (MODE == 1 && CLIP)\n"
  "#define U_PAR 1\n"
  "#endif\n"
  "#if ((MODE == 0 || MODE == 1 || MODE == 2 || MODE == 5) && CLIP) || MODE == 3 || MODE == 4\n"
  "#define U_CLIP 1\n"
  "#endif\n"
  "#ifdef U_PIX\n varying vec2 vPix;\n#endif\n"
  "#ifdef U_UV\n varying vec2 vUv;\n#endif\n"
  "varying vec4 vCol;\n"
  "#ifdef U_SHAPE\n varying vec4 vShape;\n#endif\n"
  "#ifdef U_PAR\n varying vec4 vPar;\n#endif\n"
  "#ifdef U_CLIP\n varying vec4 vClip;\n#endif\n";

static const char *VS_BATCH_BODY =
  "attribute vec2 aPos; attribute vec2 aUv; attribute vec4 aCol; attribute vec4 aShape; attribute vec4 aPar; attribute vec4 aClip;\n"
  "uniform vec2 uSize; uniform float uFlip;\n"
  "void main(){\n"
  "#ifdef U_PIX\n  vPix = aPos;\n#endif\n"
  "#ifdef U_UV\n  vUv = aUv;\n#endif\n"
  "  vCol = aCol;\n"
  "#ifdef U_SHAPE\n  vShape = aShape;\n#endif\n"
  "#ifdef U_PAR\n  vPar = aPar;\n#endif\n"
  "#ifdef U_CLIP\n  vClip = aClip;\n#endif\n"
  "  float ny = (uFlip > 0.5) ? (2.0 * aPos.y / uSize.y - 1.0) : (1.0 - 2.0 * aPos.y / uSize.y);\n"
  "  gl_Position = vec4(2.0 * aPos.x / uSize.x - 1.0, ny, 0.0, 1.0);\n"
  "}\n";

/* GLSL 1.20 has no 'flat'. All four vertices of a quad carry identical vCol/vShape/vPar/vClip, so the
   interpolated value equals the constant. */
static const char *FS_BATCH_BODY =
  "uniform sampler2D uTex; uniform sampler2D uAtlas; uniform float uUI;\n"
  "float sdRR(vec2 p, vec4 b, float r){\n"
  "  vec2 d = abs(p - b.xy) - (b.zw - vec2(r));\n"
  "  return length(max(d, 0.0)) + min(max(d.x, d.y), 0.0) - r;\n"
  "}\n"
  "float covRR(vec2 p, vec4 b, float r){ return clamp(0.5 - sdRR(p, b, r), 0.0, 1.0); }\n"
  "void main(){\n"
  "#if MODE == 0\n"
  "#if CLIP\n"
  "  float clipCov = covRR(vPix, vClip, vPar.w);\n"
  "#else\n"
  "  float clipCov = 1.0;\n"
  "#endif\n"
  "  float a = vCol.a * covRR(vPix, vShape, vPar.x) * clipCov;\n"
  "  gl_FragColor = vec4(vCol.rgb * a, a);\n"
  "#elif MODE == 1\n"
  "#if CLIP\n"
  "  float clipCov = covRR(vPix, vClip, vPar.w);\n"
  "#else\n"
  "  float clipCov = 1.0;\n"
  "#endif\n"
  "  float a = vCol.a * texture2D(uAtlas, vUv).r * clipCov;\n"
  "  gl_FragColor = vec4(vCol.rgb * a, a);\n"
  "#elif MODE == 2\n"
  "#if CLIP\n"
  "  float clipCov = covRR(vPix, vClip, vPar.w);\n"
  "#else\n"
  "  float clipCov = 1.0;\n"
  "#endif\n"
  "  float c = vCol.a * covRR(vPix, vShape, vPar.x) * clipCov;\n"
  "  gl_FragColor = texture2D(uTex, vUv) * c;\n"
  "#elif MODE == 3\n"
  "  gl_FragColor = vec4(mix(vCol.rgb, vClip.rgb, vUv.y), 1.0);\n"
  "#elif MODE == 4\n"
  "  float fall = sdRR(vPix, vShape, vPar.x) / uUI;\n"
  "  float k = smoothstep(-2.0, 12.0, fall);\n"
  "  float hole = covRR(vPix, vClip, vPar.w);\n"
  "  float a = 0.22 * vCol.a * (1.0 - k) * (1.0 - hole);\n"
  "  gl_FragColor = vec4(0.0, 0.0, 0.0, a);\n"
  "#elif MODE == 5\n"
  "#if CLIP\n"
  "  float clipCov = covRR(vPix, vClip, vPar.w);\n"
  "#else\n"
  "  float clipCov = 1.0;\n"
  "#endif\n"
  "  vec2 a = vShape.xy, b = vShape.zw;\n"
  "  vec2 pa = vPix - a, ba = b - a;\n"
  "  float h = clamp(dot(pa, ba) / max(dot(ba, ba), 0.000001), 0.0, 1.0);\n"
  "  float d = length(pa - ba * h) - vPar.x;\n"
  "  float c = clamp(0.5 - d, 0.0, 1.0) * clipCov * vCol.a;\n"
  "  gl_FragColor = vec4(vCol.rgb * c, c);\n"
  "#elif MODE == 6\n"
  "  gl_FragColor = vec4(vCol.rgb * vCol.a, vCol.a);\n"
  "#elif MODE == 7\n"
  "  gl_FragColor = vec4(vCol.rgb, 1.0);\n"
  "#else\n"
  "  gl_FragColor = texture2D(uTex, vUv) * vCol.a;\n"
  "#endif\n"
  "}\n";

/* \"#version 120\" + MODE + CLIP + the shared varying declarations + body */
static char *sb_src(int mode, int clip, const char *body) {
  size_t n = strlen(SB_COMMON) + strlen(body) + 96;
  char *o = malloc(n);
  if (o) snprintf(o, n, "#version 120\n#define MODE %d\n#define CLIP %d\n%s%s", mode, clip, SB_COMMON, body);
  return o;
}

static const char *VS_FLIGHT =
  "#version 120\n"
  "attribute vec2 aPos; attribute vec2 aLoc;\n"
  "uniform vec2 uSize; uniform float uFlip; uniform vec2 uDim;\n"
  "varying vec2 vLoc; varying vec2 vUv;\n"
  "void main(){\n"
  "  vLoc = aLoc; vUv = aLoc / uDim;\n"
  "  float ny = (uFlip > 0.5) ? (2.0 * aPos.y / uSize.y - 1.0) : (1.0 - 2.0 * aPos.y / uSize.y);\n"
  "  gl_Position = vec4(2.0 * aPos.x / uSize.x - 1.0, ny, 0.0, 1.0);\n"
  "}\n";

static const char *FS_FLIGHT =
  "#version 120\n"
  "varying vec2 vLoc; varying vec2 vUv;\n"
  "uniform sampler2D uTex; uniform vec2 uHalf; uniform float uRad; uniform float uAlpha;\n"
  "float sdRR(vec2 p, vec4 b, float r){\n"
  "  vec2 d = abs(p - b.xy) - (b.zw - vec2(r));\n"
  "  return length(max(d, 0.0)) + min(max(d.x, d.y), 0.0) - r;\n"
  "}\n"
  "void main(){\n"
  "  float cov = clamp(0.5 - sdRR(vLoc, vec4(uHalf, uHalf), uRad), 0.0, 1.0);\n"
  "  gl_FragColor = texture2D(uTex, vUv) * (cov * uAlpha);\n"
  "}\n";

static GLuint gl_compile(GLenum type, const char *src) {
  GLuint s = glCreateShader(type);
  glShaderSource(s, 1, &src, NULL);
  glCompileShader(s);
  GLint ok = 0;
  glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[1024];
    glGetShaderInfoLog(s, sizeof log, NULL, log);
    fprintf(stderr, "summerboard: shader compile failed: %s\n", log);
    glDeleteShader(s);
    return 0;
  }
  return s;
}

static GLuint gl_link(const char *vs, const char *fs, const char **attribs, int nattr) {
  GLuint v = gl_compile(GL_VERTEX_SHADER, vs);
  GLuint f = gl_compile(GL_FRAGMENT_SHADER, fs);
  if (!v || !f) return 0;
  GLuint p = glCreateProgram();
  glAttachShader(p, v);
  glAttachShader(p, f);
  for (int i = 0; i < nattr; i++) glBindAttribLocation(p, (GLuint)i, attribs[i]);
  glLinkProgram(p);
  GLint ok = 0;
  glGetProgramiv(p, GL_LINK_STATUS, &ok);
  glDeleteShader(v);
  glDeleteShader(f);
  if (!ok) {
    char log[1024];
    glGetProgramInfoLog(p, sizeof log, NULL, log);
    fprintf(stderr, "summerboard: program link failed: %s\n", log);
    glDeleteProgram(p);
    return 0;
  }
  return p;
}

/* ------------------------------------------------------------------------ */
/* GL state, batch, render-target switching                                 */
/* ------------------------------------------------------------------------ */
static EGLDisplay egl_dpy = EGL_NO_DISPLAY;
static EGLContext egl_ctx = EGL_NO_CONTEXT;
static EGLSurface egl_pb = EGL_NO_SURFACE;     /* only if surfaceless make-current is refused */
static XImage *x_img;                          /* readback target: SHM image, or malloc'd for XPutImage */
static XShmSegmentInfo x_shm;
static int x_use_shm;
static GC x_gc;
static int rb_mode;                            /* readback: 0 = GPU swizzle pass + RGBA read (default), 1 = GL_BGRA read, 2 = RGBA read + CPU swizzle */
static GImg gl_swz;                            /* B/R-swapped copy of gl_screen that is read back */
static GLuint sw_prog, sw_vao, sw_vbo;
static GLint sw_scale, sw_flip, sw_swap;
static int present_egl;                        /* SUMMERBOARD_PRESENT=egl: EGL window surface instead of readback */
static EGLSurface egl_win_surf = EGL_NO_SURFACE;
static EGLConfig egl_cfg;
static int gl_npot;                            /* GL_ARB_texture_non_power_of_two */
static int opt_nomip = 1;                      /* mipmaps are off by default (glGenerateMipmap costs 250+ ms per snapshot here); SUMMERBOARD_MIP=1 enables them */
static double prof_sync, prof_read, prof_put;
static int scr_aw, scr_ah;                     /* allocated size of the gl_screen FBO */
static int scr_ready;                          /* gl_screen target built for the current screen size */
static GLuint gl_progm[SB_NPROG], gl_vao, gl_vbo, gl_ibo, gl_atlas;
static GLint pu_size[SB_NPROG], pu_flip[SB_NPROG], pu_ui[SB_NPROG];   /* per-program uniform locations */
static const GImg *pu_t[SB_NPROG];                                    /* target each program's uniforms describe */
static int pu_w[SB_NPROG], pu_h[SB_NPROG], pu_f[SB_NPROG];
static int batch_mode;          /* program index of the queued quads (mode, + SB_CLIPOFF if clipped) */
static int blend_on = 1;        /* current GL_BLEND state; modes 3 and 7 are opaque and draw with blending off */
static int egl_direct;          /* EGL present: draw straight into the window surface, no gl_screen FBO and no copy pass */
static int gl_vp_y;             /* viewport y offset of gl_screen when it is the window (keyboard shrinks the frame) */
static GLuint fl_prog, fl_vao, fl_vbo;
static GLint f_size, f_flip, f_dim, f_half, f_rad, f_alpha;

static float *bf;               /* batch vertex data */
static size_t bnf, bcapf;       /* used / capacity, in floats */
static GImg *cur_t;             /* render target currently configured, NULL = must reconfigure */
static GLuint batch_tex;        /* image bound to unit 0 for the batch */

#define ATLAS_N 2048
static int atlas_x, atlas_y, atlas_rowh;

/* SUMMERBOARD_PROF=1 prints, every 120 frames: CPU time to build+submit a frame, GPU time (glFinish),
   swap time, draw calls, quads and render-target switches per frame. */
static int prof_on;   /* 1 = frame summary, 2 = also log every slow draw call */
static double prof_t0, prof_cpu, prof_gpu, prof_swap;
static unsigned prof_draws, prof_quads, prof_tgt, prof_frames;
static double prof_buf, prof_drw, prof_up, prof_clr, prof_worst, prof_wall0;
static const char *prof_worst_what = "";
static void prof_note(const char *what, double ms) {
  if (ms > prof_worst) { prof_worst = ms; prof_worst_what = what; }
}

/* Build (once) the batch program for index m = mode + SB_CLIPOFF * clip. */
static char sb_alias[SB_NPROG];
static GLuint sb_prog(int m) {
  if (gl_progm[m]) return gl_progm[m];
  static const char *ba[] = { "aPos", "aUv", "aCol", "aShape", "aPar", "aClip" };
  int mode = m & (SB_CLIPOFF - 1), clip = m >= SB_CLIPOFF;
  char *vs = sb_src(mode, clip, VS_BATCH_BODY), *fs = sb_src(mode, clip, FS_BATCH_BODY);
  GLuint pr = (vs && fs) ? gl_link(vs, fs, ba, 6) : 0;
  free(vs); free(fs);
  if (!pr) {
    fprintf(stderr, "summerboard: batch program %d failed, falling back to mode 0\n", m);
    if (m == 0) return 0;
    gl_progm[m] = sb_prog(0);                 /* a broken variant draws as a plain rounded rect */
    pu_size[m] = pu_size[0]; pu_flip[m] = pu_flip[0]; pu_ui[m] = pu_ui[0];
    sb_alias[m] = 1;                          /* shares program 0's uniforms: never trust the per-index cache */
    return gl_progm[m];
  }
  gl_progm[m] = pr;
  glUseProgram(pr);
  pu_size[m] = glGetUniformLocation(pr, "uSize");
  pu_flip[m] = glGetUniformLocation(pr, "uFlip");
  pu_ui[m]   = glGetUniformLocation(pr, "uUI");
  glUniform1i(glGetUniformLocation(pr, "uTex"), 0);
  glUniform1i(glGetUniformLocation(pr, "uAtlas"), 1);
  if (pu_ui[m] >= 0) glUniform1f(pu_ui[m], (float)UI);
  return pr;
}

static void flush(void) {
  if (!bnf) return;
  if (prof_on) { prof_draws++; prof_quads += (unsigned)(bnf / (SB_VF * 4)); }
  int m = batch_mode;
  if (!sb_prog(m)) { bnf = 0; return; }
  glUseProgram(gl_progm[m]);
  { int want_blend = !((m & (SB_CLIPOFF - 1)) == 3 || (m & (SB_CLIPOFF - 1)) == 7);   /* opaque modes overwrite */
    if (want_blend != blend_on) { if (want_blend) glEnable(GL_BLEND); else glDisable(GL_BLEND); blend_on = want_blend; } }
  if (cur_t && (sb_alias[m] || pu_t[m] != cur_t || pu_w[m] != cur_t->w || pu_h[m] != cur_t->h || pu_f[m] != (cur_t->fbo != 0))) {
    glUniform2f(pu_size[m], (float)cur_t->w, (float)cur_t->h);
    glUniform1f(pu_flip[m], cur_t->fbo ? 1.f : 0.f);
    pu_t[m] = cur_t; pu_w[m] = cur_t->w; pu_h[m] = cur_t->h; pu_f[m] = cur_t->fbo != 0;
  }
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, batch_tex);
  glBindVertexArray(gl_vao);
  glBindBuffer(GL_ARRAY_BUFFER, gl_vbo);
  if (!prof_on) {
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(bnf * sizeof(float)), bf, GL_STREAM_DRAW);
    glDrawElements(GL_TRIANGLES, (GLsizei)(bnf / (SB_VF * 4) * 6), GL_UNSIGNED_SHORT, (void *)0);
  } else {
    double a = now_ms();
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(bnf * sizeof(float)), bf, GL_STREAM_DRAW);
    double b = now_ms();
    glDrawElements(GL_TRIANGLES, (GLsizei)(bnf / (SB_VF * 4) * 6), GL_UNSIGNED_SHORT, (void *)0);
    double c = now_ms();
    prof_buf += b - a; prof_drw += c - b;
    prof_note("glBufferData", b - a); prof_note("glDrawElements", c - b);
    if (prof_on >= 2 && c - b > 20.0)
      fprintf(stderr, "[slow] %.0f ms: mode %d, %d quads, first quad (%.0f,%.0f)-(%.0f,%.0f), tex %u, target %dx%d%s\n",
              c - b, m, (int)(bnf / (SB_VF * 4)), bf[0], bf[1], bf[2 * SB_VF], bf[2 * SB_VF + 1], batch_tex,
              cur_t ? cur_t->w : 0, cur_t ? cur_t->h : 0, cur_t && cur_t->fbo ? " (fbo)" : "");
  }
  bnf = 0;
}

/* Make t the render target. Flushes first, so everything queued so far lands on the old target. */
static void gl_target(GImg *t) {
  if (t == cur_t) return;
  flush();
  cur_t = t;
  if (prof_on && t->fbo) prof_tgt++;
  glBindFramebuffer(GL_FRAMEBUFFER, t->fbo);
  glViewport(0, (t == &gl_screen && egl_direct) ? gl_vp_y : 0, t->w, t->h);
}

static void bind_tex(GLuint t) {
  if (t != batch_tex) { flush(); batch_tex = t; }
}

static void vtx(float x, float y, float u, float v, uint32_t c, const float *s, const float *p, const float *k) {
  if (bnf + SB_VF > bcapf) {
    size_t nc = bcapf ? bcapf * 2 : (size_t)SB_VF * 4 * 512;
    if (nc > (size_t)SB_VF * 4 * SB_MAXQ) nc = (size_t)SB_VF * 4 * SB_MAXQ;
    if (nc <= bcapf) return;
    float *nb = realloc(bf, nc * sizeof *bf);
    if (!nb) return;
    bf = nb; bcapf = nc;
  }
  float *d = bf + bnf;
  d[0] = x; d[1] = y; d[2] = u; d[3] = v;
  memcpy(d + 4, &c, 4);
  memcpy(d + 5, s, 4 * sizeof(float));
  memcpy(d + 9, p, 4 * sizeof(float));
  memcpy(d + 13, k, 4 * sizeof(float));
  bnf += SB_VF;
}

static void quad(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1,
                 const float *c, const float *s, const float *p, const float *k) {
  if (bnf >= (size_t)SB_VF * 4 * SB_MAXQ) flush();
  int qm = (int)(p[1] + 0.5f);
  if (qm < 0 || qm >= SB_NMODE) qm = 0;
  if (p[2] > 0.5f && (qm == 0 || qm == 1 || qm == 2 || qm == 5)) qm += SB_CLIPOFF;   /* clipped variant */
  if (bnf && qm != batch_mode) flush();
  batch_mode = qm;
  uint32_t pc = 0;
  for (int i = 0; i < 4; i++) {
    float f = c[i] < 0.f ? 0.f : (c[i] > 1.f ? 1.f : c[i]);
    pc |= (uint32_t)(f * 255.f + 0.5f) << (8 * i);      /* byte order r,g,b,a on little-endian */
  }
  vtx(x0, y0, u0, v0, pc, s, p, k);
  vtx(x1, y0, u1, v0, pc, s, p, k);
  vtx(x1, y1, u1, v1, pc, s, p, k);
  vtx(x0, y1, u0, v1, pc, s, p, k);
}

static void col3(uint32_t c, double a, float *o) {
  o[0] = ((c >> 16) & 255) / 255.f;
  o[1] = ((c >> 8) & 255) / 255.f;
  o[2] = (c & 255) / 255.f;
  o[3] = (float)a;
}
static void shp(float *s, const RR *r) { s[0] = r->cx; s[1] = r->cy; s[2] = r->hx; s[3] = r->hy; }
static void clp(float *c, const RR *r) { c[0] = r->cx; c[1] = r->cy; c[2] = r->hx; c[3] = r->hy; }
static void par(float *p, float mode, float rad, int clip, float clipr) {
  p[0] = rad; p[1] = mode; p[2] = clip ? 1.f : 0.f; p[3] = clipr;
}
static const float NO_SHAPE[4] = { 0.f, 0.f, 1e6f, 1e6f };

/* ------------------------------------------------------------------------ */
/* Images and render targets                                                */
/* ------------------------------------------------------------------------ */
static void img_free(GImg *im) {
  if (!im->tex && !im->fbo) { im->w = im->h = 0; return; }
  flush();
  if (cur_t == im) cur_t = NULL;
  if (im->fbo) glDeleteFramebuffers(1, &im->fbo);
  if (im->tex) glDeleteTextures(1, &im->tex);
  memset(im, 0, sizeof *im);
}

static void img_set_filters(int mip) {
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, mip ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

/* Upload a top-row-first 0xAARRGGBB buffer (premultiplied or opaque). With mip the texture is mipmapped
   (snapshots that get scaled down); icons drawn ~1:1 pass mip = 0. Same-size uploads reuse the texture,
   which avoids reallocating GPU memory for every snapshot. */
static int is_pow2(int v) { return v > 0 && (v & (v - 1)) == 0; }

static void img_upload_ex(GImg *im, const uint32_t *px, int w, int h, int mip) {
  double pu0 = prof_on ? now_ms() : 0;
  if (opt_nomip) mip = 0;
  if (mip && !gl_npot && !(is_pow2(w) && is_pow2(h))) mip = 0;   /* no NPOT mipmaps in plain GL 2.1 */
  flush();
  glActiveTexture(GL_TEXTURE0);
  if (im->tex && !im->fbo && im->w == w && im->h == h) {
    glBindTexture(GL_TEXTURE_2D, im->tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, px);
    if (mip) glGenerateMipmap(GL_TEXTURE_2D);
    batch_tex = 0;   /* the binding changed behind bind_tex's back */
    if (prof_on) { prof_up += now_ms() - pu0; prof_note("img_upload", now_ms() - pu0); }
    return;
  }
  img_free(im);
  glGenTextures(1, &im->tex);
  glBindTexture(GL_TEXTURE_2D, im->tex);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, px);
  if (mip) glGenerateMipmap(GL_TEXTURE_2D);
  img_set_filters(mip);
  im->w = w; im->h = h;
  batch_tex = 0;
  if (prof_on) { prof_up += now_ms() - pu0; prof_note("img_upload", now_ms() - pu0); }
}
static void img_upload(GImg *im, const uint32_t *px, int w, int h) { img_upload_ex(im, px, w, h, 1); }

/* A render target (texture + FBO), cleared to transparent black. */
static void img_make_target(GImg *im, int w, int h) {
  img_free(im);
  flush();
  glGenTextures(1, &im->tex);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, im->tex);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
  img_set_filters(0);
  glGenFramebuffers(1, &im->fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, im->fbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, im->tex, 0);
  { GLenum fst = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (fst != GL_FRAMEBUFFER_COMPLETE) fprintf(stderr, "summerboard: FBO %dx%d incomplete (0x%x)\n", w, h, fst); }
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  im->w = w; im->h = h;
  cur_t = NULL;
}

static void img_clear(GImg *im) {
  flush();
  gl_target(im);
  glClearColor(0, 0, 0, 0);
  glClear(GL_COLOR_BUFFER_BIT);
}

/* ------------------------------------------------------------------------ */
/* Glyph atlas (coverage bitmaps from the X font, packed into one R8 texture) */
/* ------------------------------------------------------------------------ */
static int atlas_put(Glyph *g) {
  if (!g->cov || g->cw <= 0 || g->ch <= 0) return 0;
  int cw = g->cw, ch = g->ch;
  if (cw > ATLAS_N || ch > ATLAS_N) return 0;
  if (atlas_x + cw + 1 > ATLAS_N) { atlas_x = 0; atlas_y += atlas_rowh + 1; atlas_rowh = 0; }
  if (atlas_y + ch + 1 > ATLAS_N) return 0;
  flush();
  glActiveTexture(GL_TEXTURE1);
  glBindTexture(GL_TEXTURE_2D, gl_atlas);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glTexSubImage2D(GL_TEXTURE_2D, 0, atlas_x, atlas_y, cw, ch, GL_LUMINANCE, GL_UNSIGNED_BYTE, g->cov);
  glActiveTexture(GL_TEXTURE0);
  g->u0 = (float)atlas_x / ATLAS_N;
  g->v0 = (float)atlas_y / ATLAS_N;
  g->u1 = (float)(atlas_x + cw) / ATLAS_N;
  g->v1 = (float)(atlas_y + ch) / ATLAS_N;
  atlas_x += cw + 1;
  if (ch > atlas_rowh) atlas_rowh = ch;
  free(g->cov);
  g->cov = NULL;
  return 1;
}

/* ------------------------------------------------------------------------ */
/* Primitives (same names and arguments as the software version)            */
/* ------------------------------------------------------------------------ */
/* Flat (no SDF) quad. Opaque colours use mode 7 (blending off), translucent ones mode 6. */
static void flat_quad(float x0, float y0, float x1, float y1, const float *c, const float *s) {
  float p[4], k[4] = { 0, 0, 0, 0 };
  par(p, c[3] >= 0.998f ? 7.f : 6.f, 0, 0, 0);
  quad(x0, y0, x1, y1, 0, 0, 0, 0, c, s, p, k);
}

/* A rounded rect is drawn as a flat interior plus four thin SDF strips around it. The interior is the largest
   rect that lies entirely inside the shape with full coverage (inset 0.3 r + 2 px, which keeps its four corners
   at least half a pixel inside the corner arcs), so it needs no distance field and, when opaque, no blending.
   Only the border ring pays for the SDF. Small shapes keep the single SDF quad: the extra quads would cost
   more than they save. */
#define RR_SPLIT_MIN_AREA 1500.0f
static int rr_inner(const RR *rr, float *ix0, float *iy0, float *ix1, float *iy1) {
  float in = rr->r * 0.3f + 2.f;
  *ix0 = rr->x0 + in; *iy0 = rr->y0 + in; *ix1 = rr->x1 - in; *iy1 = rr->y1 - in;
  return (*ix1 - *ix0) >= 4.f && (*iy1 - *iy0) >= 4.f && (rr->x1 - rr->x0) * (rr->y1 - rr->y0) >= RR_SPLIT_MIN_AREA;
}

static void fill_rr(GImg *t, const RR *rr, uint32_t col, double alpha, const RR *clip) {
  if (alpha <= 0) return;
  gl_target(t);
  float c[4], s[4], p[4], k[4] = { 0, 0, 0, 0 };
  col3(col, alpha, c);
  shp(s, rr);
  par(p, 0, rr->r, clip != NULL, clip ? clip->r : 0);
  if (clip) clp(k, clip);
  float ix0, iy0, ix1, iy1;
  if (!clip && rr_inner(rr, &ix0, &iy0, &ix1, &iy1)) {
    float x0 = rr->x0 - 1, y0 = rr->y0 - 1, x1 = rr->x1 + 1, y1 = rr->y1 + 1;
    flat_quad(ix0, iy0, ix1, iy1, c, s);
    quad(x0, y0, x1, iy0, 0, 0, 0, 0, c, s, p, k);     /* top */
    quad(x0, iy1, x1, y1, 0, 0, 0, 0, c, s, p, k);     /* bottom */
    quad(x0, iy0, ix0, iy1, 0, 0, 0, 0, c, s, p, k);   /* left */
    quad(ix1, iy0, x1, iy1, 0, 0, 0, 0, c, s, p, k);   /* right */
    return;
  }
  quad(rr->x0 - 1, rr->y0 - 1, rr->x1 + 1, rr->y1 + 1, 0, 0, 0, 0, c, s, p, k);
}

/* Integer-aligned, square-cornered: every covered pixel has coverage exactly 1, so one flat quad is exact. */
static void fill_rect(GImg *t, int x, int y, int w, int h, uint32_t col, double alpha) {
  if (alpha <= 0 || w <= 0 || h <= 0) return;
  gl_target(t);
  float c[4], s[4] = { 0, 0, 0, 0 };
  col3(col, alpha, c);
  flat_quad((float)x, (float)y, (float)(x + w), (float)(y + h), c, s);
}

static void gl_gradient(GImg *t, int h, uint32_t top, uint32_t bot) {
  gl_target(t);
  float c[4], s[4] = { 0, 0, 0, 0 }, p[4], k[4];
  col3(top, 1, c);
  col3(bot, 1, k);
  par(p, 3, 0, 0, 0);
  quad(0, 0, (float)t->w, (float)h, 0, 0, 1, 1, c, s, p, k);
}

/* Cover-crop image into r (rounded by r.r), optional extra rounded clip, global alpha. */
static void img_draw(GImg *t, const GImg *src, Rect r, const RR *clip, double alpha) {
  if (!src->tex || alpha <= 0 || r.w < 1 || r.h < 1) return;
  double sc = fmax(r.w / src->w, r.h / src->h);
  double ox = (src->w - r.w / sc) / 2, oy = (src->h - r.h / sc) / 2;
  RR rr = rr_make(r.x, r.y, r.w, r.h, r.r);
  float c[4], s[4], p[4], k[4] = { 0, 0, 0, 0 };
  col3(0xffffff, alpha, c);
  shp(s, &rr);
  par(p, 2, rr.r, clip != NULL, clip ? clip->r : 0);
  if (clip) clp(k, clip);
  float x0 = rr.x0 - 1, x1 = rr.x1 + 1, y0 = rr.y0 - 1, y1 = rr.y1 + 1;
  gl_target(t);
  bind_tex(src->tex);
#define IMG_U(px_) ((float)((ox + ((px_) - r.x) / sc) / src->w))
#define IMG_V(py_) ((float)((oy + ((py_) - r.y) / sc) / src->h))
  float ix0, iy0, ix1, iy1;
  if (!clip && rr_inner(&rr, &ix0, &iy0, &ix1, &iy1)) {
    /* flat textured interior (no SDF), SDF only on the border ring */
    float pf[4];
    par(pf, 8, 0, 0, 0);
    quad(ix0, iy0, ix1, iy1, IMG_U(ix0), IMG_V(iy0), IMG_U(ix1), IMG_V(iy1), c, s, pf, k);
    quad(x0, y0, x1, iy0, IMG_U(x0), IMG_V(y0), IMG_U(x1), IMG_V(iy0), c, s, p, k);     /* top */
    quad(x0, iy1, x1, y1, IMG_U(x0), IMG_V(iy1), IMG_U(x1), IMG_V(y1), c, s, p, k);     /* bottom */
    quad(x0, iy0, ix0, iy1, IMG_U(x0), IMG_V(iy0), IMG_U(ix0), IMG_V(iy1), c, s, p, k); /* left */
    quad(ix1, iy0, x1, iy1, IMG_U(ix1), IMG_V(iy0), IMG_U(x1), IMG_V(iy1), c, s, p, k); /* right */
    return;
  }
  quad(x0, y0, x1, y1, IMG_U(x0), IMG_V(y0), IMG_U(x1), IMG_V(y1), c, s, p, k);
#undef IMG_U
#undef IMG_V
}

/* Premultiplied image at exact size (WhiteSur icons). */
static void draw_rgba(GImg *t, const GImg *src, double x, double y, double w, double h, double alpha) {
  if (!src->tex || alpha <= 0 || w < 1 || h < 1) return;
  float c[4], s[4], p[4], k[4] = { 0, 0, 0, 0 };
  col3(0xffffff, alpha, c);
  memcpy(s, NO_SHAPE, sizeof s);
  par(p, 8, 0, 0, 0);                        /* no rounding, no clip: plain premultiplied texture */
  gl_target(t);
  bind_tex(src->tex);
  quad((float)x, (float)y, (float)(x + w), (float)(y + h), 0, 0, 1, 1, c, s, p, k);
}

/* Round-capped anti-aliased segment (same name as the old ui_seg). */
static void ui_seg(GImg *t, double x0, double y0, double x1, double y1, double w, uint32_t col, double al) {
  if (al <= 0) return;
  double r = w / 2;
  gl_target(t);
  float c[4], s[4] = { (float)x0, (float)y0, (float)x1, (float)y1 }, p[4], k[4] = { 0, 0, 0, 0 };
  col3(col, al, c);
  par(p, 5, (float)r, 0, 0);
  quad((float)(fmin(x0, x1) - r - 1), (float)(fmin(y0, y1) - r - 1),
       (float)(fmax(x0, x1) + r + 1), (float)(fmax(y0, y1) + r + 1), 0, 0, 0, 0, c, s, p, k);
}

static void draw_text(GImg *t, const AppFont *f, const char *s, double x, double y, double scale, int align,
                      uint32_t col, double alpha, double ls, const RR *clip) {
  if (alpha <= 0 || !*s) return;
  double k = scale / f->ss;
  double tw = text_width(f, s, scale, ls);
  double pen = align == 1 ? x - tw / 2 : (align == 2 ? x - tw : x);
  float c[4], sh[4] = { 0, 0, 0, 0 }, p[4], cl[4] = { 0, 0, 0, 0 };
  col3(col, alpha, c);
  par(p, 1, 0, clip != NULL, clip ? clip->r : 0);
  if (clip) clp(cl, clip);
  gl_target(t);
  int integer_scale = fabs(scale - 1.0) < 1e-6;
  while (*s) {
    uint32_t cp = utf8_next(&s);
    const Glyph *g = &f->g[glyph_index(cp)];
    if (g->cw > 0) {
      double gl = pen + g->lb * k, gt = y - g->asc * k;
      if (integer_scale) { gl = round(gl); gt = round(gt); }
      quad((float)gl, (float)gt, (float)(gl + g->w * k), (float)(gt + g->h * k),
           g->u0, g->v0, g->u1, g->v1, c, sh, p, cl);
    }
    pen += g->adv / f->ss * scale + ls;
  }
}

/* Card shadow: same falloff as the old draw_card_shadow; the card's own shape is a hole. */
#define SH_REACH (S(12) + 1)   /* the falloff (smoothstep to 12 UI) is exactly zero beyond this */
#define SH_DROP  S(3)
static void draw_card_shadow(GImg *t, Rect r, double strength) {
  if (strength < 0.01 || r.w < 2 || r.h < 2) return;
  RR card = rr_make(r.x, r.y, r.w, r.h, r.r);
  RR sh = rr_make(r.x, r.y + SH_DROP, r.w, r.h, r.r);
  float c[4], s[4], p[4], k[4];
  col3(0, strength, c);
  shp(s, &sh);
  par(p, 4, sh.r, 0, card.r);
  clp(k, &card);
  double re = SH_REACH;
  gl_target(t);
  float ox0 = sh.x0 - (float)re, oy0 = sh.y0 - (float)re, ox1 = sh.x1 + (float)re, oy1 = sh.y1 + (float)re;
  /* Inside the card the hole makes the output zero. For big cards (the full-screen window) skip that
     interior instead of shading it: draw the outer rect minus the rect inscribed in the card. */
  float in = card.r * 0.3f + 2.f;
  float ix0 = card.x0 + in, iy0 = card.y0 + in, ix1 = card.x1 - in, iy1 = card.y1 - in;
  if (ix1 - ix0 < 2.f || iy1 - iy0 < 2.f) {   /* too small to have an interior worth skipping */
    quad(ox0, oy0, ox1, oy1, 0, 0, 0, 0, c, s, p, k);
    return;
  }
  quad(ox0, oy0, ox1, iy0, 0, 0, 0, 0, c, s, p, k);   /* top */
  quad(ox0, iy1, ox1, oy1, 0, 0, 0, 0, c, s, p, k);   /* bottom */
  quad(ox0, iy0, ix0, iy1, 0, 0, 0, 0, c, s, p, k);   /* left */
  quad(ix1, iy0, ox1, iy1, 0, 0, 0, 0, c, s, p, k);   /* right */
}

/* ------------------------------------------------------------------------ */
/* Icon flight: pinched mesh, corners evaluated per fragment in local space */
/* ------------------------------------------------------------------------ */
#define FL_PINCH 0.14
#define FL_PEAK  S(26)
#define FL_ROWS  48
static float fl_verts[FL_ROWS * 6 * 4];

/* src is the snapshot, or &gl_white for an app with no picture yet. */
static void draw_flight(GImg *t, const GImg *src, Rect f, Rect icon, Rect rest) {
  if (!src->tex || f.w < 1 || f.h < 1) return;

  double dw = rest.w - icon.w, dh = rest.h - icon.h;
  double pw = dw > 1 ? (rest.w - f.w) / dw : 0, ph = dh > 1 ? (rest.h - f.h) / dh : 0;
  double p = clampd((pw + ph) * 0.5, 0, 1);

  double aspect = f.w / fmax(f.h, 1e-9), toward = icon.w / fmax(icon.h, 1e-9);
  double k = 1 - pow(1 - p, 3);
  double want = aspect + (toward - aspect) * k;
  double w = f.w, h = f.h;
  if (want < aspect) w = h * want; else h = w / fmax(want, 1e-9);
  double cx = f.x + f.w / 2, cy = f.y + f.h / 2;
  double x0 = cx - w / 2, y0 = cy - h / 2;

  double pinch = FL_PINCH * sin(M_PI * p);
  double ix = icon.x + icon.w / 2;
  int above = icon.y + icon.h / 2 < cy;
  double radius = p * icon.r + FL_PEAK * sin(M_PI * p);
  radius = fmin(radius, 0.5 * fmin(w, h));

  double xl[FL_ROWS + 1], xr[FL_ROWS + 1], ys[FL_ROWS + 1];
  for (int j = 0; j <= FL_ROWS; j++) {
    double vv = (double)j / FL_ROWS;
    double s = pinch * (above ? 1 - vv : vv);
    double left = x0 + (ix - x0) * s;
    xl[j] = left;
    xr[j] = left + w * (1 - s);
    ys[j] = y0 + h * vv;
  }

  float *o = fl_verts;
#define FLV(xx, yy, lx, ly) do { o[0] = (float)(xx); o[1] = (float)(yy); o[2] = (float)(lx); o[3] = (float)(ly); o += 4; } while (0)
  for (int j = 0; j < FL_ROWS; j++) {
    double ly0 = h * j / FL_ROWS, ly1 = h * (j + 1) / FL_ROWS;
    FLV(xl[j],   ys[j],   0, ly0);
    FLV(xr[j],   ys[j],   w, ly0);
    FLV(xl[j+1], ys[j+1], 0, ly1);
    FLV(xr[j],   ys[j],   w, ly0);
    FLV(xr[j+1], ys[j+1], w, ly1);
    FLV(xl[j+1], ys[j+1], 0, ly1);
  }
#undef FLV

  gl_target(t);
  flush();
  if (!blend_on) { glEnable(GL_BLEND); blend_on = 1; }   /* the flight shader is translucent */
  glUseProgram(fl_prog);
  glUniform2f(f_size, (float)t->w, (float)t->h);
  glUniform1f(f_flip, t->fbo ? 1.f : 0.f);
  glUniform2f(f_dim, (float)w, (float)h);
  glUniform2f(f_half, (float)(w / 2), (float)(h / 2));
  glUniform1f(f_rad, (float)radius);
  glUniform1f(f_alpha, 1.f);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, src->tex);
  glBindVertexArray(fl_vao);
  glBindBuffer(GL_ARRAY_BUFFER, fl_vbo);
  glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)sizeof fl_verts, fl_verts, GL_STREAM_DRAW);
  glDrawArrays(GL_TRIANGLES, 0, FL_ROWS * 6);
  cur_t = NULL;   /* the next batch draw re-selects the batch program and target state */
}

/* ------------------------------------------------------------------------ */
/* Context, programs, frame bracketing                                      */
/* ------------------------------------------------------------------------ */
#ifndef EGL_PLATFORM_SURFACELESS_MESA
#define EGL_PLATFORM_SURFACELESS_MESA 0x31DD
#endif

static int gl_has_ext(const char *name) {
  const char *e = (const char *)glGetString(GL_EXTENSIONS);
  size_t n = strlen(name);
  while (e && (e = strstr(e, name)) != NULL) {
    if ((e == (const char *)glGetString(GL_EXTENSIONS) || e[-1] == ' ') && (e[n] == ' ' || e[n] == 0)) return 1;
    e += n;
  }
  return 0;
}

#ifndef EGL_PLATFORM_X11_KHR
#define EGL_PLATFORM_X11_KHR 0x31D5
#endif

static int egl_setup(void) {
  typedef EGLDisplay (*GetPlat)(EGLenum, void *, const EGLint *);
  GetPlat gp = (GetPlat)eglGetProcAddress("eglGetPlatformDisplayEXT");
  if (!gp) gp = (GetPlat)eglGetProcAddress("eglGetPlatformDisplay");
  if (present_egl) {
    if (gp) egl_dpy = gp(EGL_PLATFORM_X11_KHR, dpy, NULL);
  } else {
    if (gp) egl_dpy = gp(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
    if (egl_dpy == EGL_NO_DISPLAY) {
      setenv("EGL_PLATFORM", "surfaceless", 1);
      egl_dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    }
  }
  EGLint maj, min;
  if (egl_dpy == EGL_NO_DISPLAY || !eglInitialize(egl_dpy, &maj, &min)) {
    fprintf(stderr, "summerboard: eglInitialize failed (0x%x)\n", eglGetError());
    return 0;
  }
  if (!eglBindAPI(EGL_OPENGL_API)) { fprintf(stderr, "summerboard: eglBindAPI(OpenGL) failed\n"); return 0; }
  static const EGLint ca_pb[]  = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
                                   EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE };
  static const EGLint ca_win[] = { EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
                                   EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE };
  static const EGLint ca_any[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_NONE };
  EGLint n = 0;
  if (!eglChooseConfig(egl_dpy, present_egl ? ca_win : ca_pb, &egl_cfg, 1, &n) || n < 1) {
    if (present_egl || !eglChooseConfig(egl_dpy, ca_any, &egl_cfg, 1, &n) || n < 1) {
      fprintf(stderr, "summerboard: no EGL config for desktop GL%s\n", present_egl ? " + window" : "");
      return 0;
    }
  }
  egl_ctx = eglCreateContext(egl_dpy, egl_cfg, EGL_NO_CONTEXT, NULL);
  if (egl_ctx == EGL_NO_CONTEXT) { fprintf(stderr, "summerboard: eglCreateContext failed (0x%x)\n", eglGetError()); return 0; }
  if (present_egl) return 1;                 /* made current once the window and its surface exist */
  if (!eglMakeCurrent(egl_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, egl_ctx)) {
    static const EGLint pa[] = { EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE };
    egl_pb = eglCreatePbufferSurface(egl_dpy, egl_cfg, pa);
    if (egl_pb == EGL_NO_SURFACE || !eglMakeCurrent(egl_dpy, egl_pb, egl_pb, egl_ctx)) {
      fprintf(stderr, "summerboard: eglMakeCurrent failed (0x%x)\n", eglGetError());
      return 0;
    }
  }
  return 1;
}

/* The readback image (non-EGL present): a MIT-SHM XImage of the screen size, or a malloc'd one for XPutImage. */
static int x_image_create(Visual *vis, int depth) {
  const char *ns = getenv("SUMMERBOARD_NOSHM");
  x_img = NULL; x_use_shm = 0;
  if (present_egl) return 1;
  if (!(ns && ns[0] == '1') && XShmQueryExtension(dpy)) {
    x_img = XShmCreateImage(dpy, vis, (unsigned)depth, ZPixmap, NULL, &x_shm, (unsigned)W, (unsigned)H);
    if (x_img && x_img->bits_per_pixel == 32) {
      x_shm.shmid = shmget(IPC_PRIVATE, (size_t)x_img->bytes_per_line * (size_t)x_img->height, IPC_CREAT | 0600);
      if (x_shm.shmid >= 0) {
        x_shm.shmaddr = x_img->data = shmat(x_shm.shmid, NULL, 0);
        if (x_shm.shmaddr != (char *)-1) {
          x_shm.readOnly = False;
          if (XShmAttach(dpy, &x_shm)) { XSync(dpy, False); x_use_shm = 1; }
          shmctl(x_shm.shmid, IPC_RMID, NULL);   /* freed when the server detaches / we exit */
        }
      }
    }
    if (!x_use_shm) {
      if (x_img) { x_img->data = NULL; XDestroyImage(x_img); x_img = NULL; }
      fprintf(stderr, "summerboard: MIT-SHM unavailable, using XPutImage\n");
    }
  }
  if (!x_img) {
    x_img = XCreateImage(dpy, vis, (unsigned)depth, ZPixmap, 0, NULL, (unsigned)W, (unsigned)H, 32, 0);
    if (!x_img || x_img->bits_per_pixel != 32) { fprintf(stderr, "summerboard: need a 32 bpp X visual\n"); return 0; }
    x_img->data = malloc((size_t)x_img->bytes_per_line * (size_t)x_img->height);
    if (!x_img->data) return 0;
  }
  return 1;
}

/* The screen changed size (rotation). W and H are already the new values and the shell window has been
   resized; rebuild everything that was allocated at the old size. */
static void gl_resize(void) {
  scr_aw = W; scr_ah = H;
  if (present_egl && egl_direct) return;       /* the window surface follows the window; nothing else is sized to the screen */
  scr_ready = 0;                               /* gl_begin_frame rebuilds the offscreen targets at the new size */
  if (gl_screen.fbo) img_free(&gl_screen);
  if (gl_swz.fbo) img_free(&gl_swz);
  if (!present_egl && x_img) {
    if (x_use_shm) { XShmDetach(dpy, &x_shm); XSync(dpy, False); shmdt(x_shm.shmaddr); x_img->data = NULL; }
    XDestroyImage(x_img);                      /* frees a malloc'd buffer itself; the SHM one was detached above */
    x_img = NULL;
    x_image_create(DefaultVisual(dpy, scr), DefaultDepth(dpy, scr));
  }
}

/* The shell window. Default: a plain X window fed by readback + MIT-SHM. With SUMMERBOARD_PRESENT=egl: a window
   whose visual matches the EGL config, with an EGL window surface on it. */
static int gl_setup(Window *out_win) {
  { const char *pe = getenv("SUMMERBOARD_PRESENT"); present_egl = pe && !strcmp(pe, "egl"); }
  if (!egl_setup()) return 0;

  Visual *vis = DefaultVisual(dpy, scr);
  int depth = DefaultDepth(dpy, scr);
  Colormap cm = 0;
  if (present_egl) {
    EGLint vid = 0;
    XVisualInfo tmpl, *vi;
    int nv = 0;
    if (!eglGetConfigAttrib(egl_dpy, egl_cfg, EGL_NATIVE_VISUAL_ID, &vid) || !vid) {
      fprintf(stderr, "summerboard: EGL config has no native X visual\n");
      return 0;
    }
    memset(&tmpl, 0, sizeof tmpl);
    tmpl.visualid = (VisualID)vid;
    vi = XGetVisualInfo(dpy, VisualIDMask, &tmpl, &nv);
    if (!vi || nv < 1) { fprintf(stderr, "summerboard: X visual 0x%x not found\n", (unsigned)vid); return 0; }
    vis = vi->visual; depth = vi->depth;
    XFree(vi);
    cm = XCreateColormap(dpy, root, vis, AllocNone);
  }
  XSetWindowAttributes swa;
  memset(&swa, 0, sizeof swa);
  swa.background_pixmap = None;
  swa.border_pixel = 0;
  swa.override_redirect = True;
  swa.event_mask = ExposureMask | KeyPressMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask;
  unsigned long vm = CWBackPixmap | CWBorderPixel | CWOverrideRedirect | CWEventMask;
  if (cm) { swa.colormap = cm; vm |= CWColormap; }
  *out_win = XCreateWindow(dpy, root, 0, 0, W, H, 0, depth, InputOutput, vis, vm, &swa);
  x_gc = XCreateGC(dpy, *out_win, 0, NULL);

  if (present_egl) {
    egl_win_surf = eglCreateWindowSurface(egl_dpy, egl_cfg, *out_win, NULL);
    if (egl_win_surf == EGL_NO_SURFACE) { fprintf(stderr, "summerboard: eglCreateWindowSurface failed (0x%x)\n", eglGetError()); return 0; }
    if (!eglMakeCurrent(egl_dpy, egl_win_surf, egl_win_surf, egl_ctx)) {
      fprintf(stderr, "summerboard: eglMakeCurrent(window) failed (0x%x)\n", eglGetError());
      return 0;
    }
    { const char *ev = getenv("SUMMERBOARD_VSYNC"); eglSwapInterval(egl_dpy, (ev && ev[0] == '1') ? 1 : 0); }
    XMapWindow(dpy, *out_win);
    return 1;
  }

  if (!x_image_create(vis, depth)) return 0;
  XMapWindow(dpy, *out_win);
  return 1;
}

#define SB_ATTR(i, n, off, stride) do { \
  glVertexAttribPointer((GLuint)(i), (n), GL_FLOAT, GL_FALSE, (stride), (void *)(size_t)(off)); \
  glEnableVertexAttribArray((GLuint)(i)); } while (0)

/* Create the window and GL context, the batch and flight programs, the atlas and the white texture.
   Call before font_build(). */
static int sw_init(void);

static int gl_init(Window *out_win) {
  if (!gl_setup(out_win)) return 0;
  {
    const char *rd = (const char *)glGetString(GL_RENDERER);
    fprintf(stderr, "summerboard: GL_RENDERER=%s | GL_VERSION=%s | GLSL=%s | %s\n", rd,
            glGetString(GL_VERSION), glGetString(GL_SHADING_LANGUAGE_VERSION),
            present_egl ? "EGL window (no readback, drawn straight into the back buffer unless SUMMERBOARD_DIRECT=0)" : (x_use_shm ? "MIT-SHM readback" : "XPutImage readback"));
    if (rd && (strstr(rd, "softpipe") || strstr(rd, "llvmpipe") || strstr(rd, "oftware")))
      fprintf(stderr, "summerboard: WARNING: software renderer, the GPU driver was not loaded\n");
    gl_npot = gl_has_ext("GL_ARB_texture_non_power_of_two");
    if (!gl_has_ext("GL_ARB_vertex_array_object")) { fprintf(stderr, "summerboard: GL_ARB_vertex_array_object missing\n"); return 0; }
    if (!gl_has_ext("GL_ARB_framebuffer_object") && !gl_has_ext("GL_EXT_framebuffer_object")) {
      fprintf(stderr, "summerboard: framebuffer objects missing\n"); return 0;
    }
  }
  { const char *pe = getenv("SUMMERBOARD_PROF"); prof_on = pe ? atoi(pe) : 0; }
  {   /* switches: SUMMERBOARD_MIP=1 (mipmaps on), SUMMERBOARD_RB=bgra|cpu (readback path, default = GPU swizzle) */
    const char *e = getenv("SUMMERBOARD_MIP"); opt_nomip = !(e && e[0] == '1');
    e = getenv("SUMMERBOARD_RB");
    if (e && !strcmp(e, "bgra")) rb_mode = 1;
    else if (e && !strcmp(e, "cpu")) rb_mode = 2;
  }
  glEnable(GL_BLEND);
  glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

  /* The two most common programs are built now; the rest are built lazily on first use (see sb_prog). */
  if (!sb_prog(0) || !sb_prog(1) || !sb_prog(3) || !sb_prog(7)) return 0;

  glGenVertexArrays(1, &gl_vao);
  glBindVertexArray(gl_vao);
  glGenBuffers(1, &gl_vbo);
  glBindBuffer(GL_ARRAY_BUFFER, gl_vbo);
  GLsizei st = (GLsizei)(SB_VF * sizeof(float));
  SB_ATTR(0, 2, 0, st);
  SB_ATTR(1, 2, 8, st);
  glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, st, (void *)(size_t)16);   /* packed colour */
  glEnableVertexAttribArray(2);
  SB_ATTR(3, 4, 20, st);
  SB_ATTR(4, 4, 36, st);
  SB_ATTR(5, 4, 52, st);
  {   /* static index buffer for SB_MAXQ quads: 0 1 2, 0 2 3 */
    static uint16_t idx[SB_MAXQ * 6];
    for (int q = 0; q < SB_MAXQ; q++) {
      uint16_t b = (uint16_t)(q * 4);
      idx[q * 6 + 0] = b;     idx[q * 6 + 1] = b + 1; idx[q * 6 + 2] = b + 2;
      idx[q * 6 + 3] = b;     idx[q * 6 + 4] = b + 2; idx[q * 6 + 5] = b + 3;
    }
    glGenBuffers(1, &gl_ibo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gl_ibo);   /* stays attached to gl_vao */
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof idx, idx, GL_STATIC_DRAW);
  }

  static const char *fa[] = { "aPos", "aLoc" };
  fl_prog = gl_link(VS_FLIGHT, FS_FLIGHT, fa, 2);
  if (!fl_prog) return 0;
  f_size  = glGetUniformLocation(fl_prog, "uSize");
  f_flip  = glGetUniformLocation(fl_prog, "uFlip");
  f_dim   = glGetUniformLocation(fl_prog, "uDim");
  f_half  = glGetUniformLocation(fl_prog, "uHalf");
  f_rad   = glGetUniformLocation(fl_prog, "uRad");
  f_alpha = glGetUniformLocation(fl_prog, "uAlpha");
  glUseProgram(fl_prog);
  glUniform1i(glGetUniformLocation(fl_prog, "uTex"), 0);
  glGenVertexArrays(1, &fl_vao);
  glBindVertexArray(fl_vao);
  glGenBuffers(1, &fl_vbo);
  glBindBuffer(GL_ARRAY_BUFFER, fl_vbo);
  GLsizei fst = (GLsizei)(4 * sizeof(float));
  SB_ATTR(0, 2, 0, fst);
  SB_ATTR(1, 2, 8, fst);
  glBindVertexArray(0);

  /* Glyph atlas on unit 1; it stays bound there for the rest of the run. */
  glActiveTexture(GL_TEXTURE1);
  glGenTextures(1, &gl_atlas);
  glBindTexture(GL_TEXTURE_2D, gl_atlas);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, ATLAS_N, ATLAS_N, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, NULL);
  img_set_filters(0);
  glActiveTexture(GL_TEXTURE0);

  static const uint32_t white = 0xffffffffu;
  img_upload(&gl_white, &white, 1, 1);
  /* EGL present draws straight into the window surface (SUMMERBOARD_DIRECT=0 keeps the old offscreen FBO +
     full-screen copy pass). */
  { const char *de = getenv("SUMMERBOARD_DIRECT"); egl_direct = present_egl && !(de && de[0] == '0'); }
  if (present_egl && !egl_direct && !sw_init()) { fprintf(stderr, "summerboard: present shader failed\n"); return 0; }

  cur_t = NULL;
  return 1;
}

/* Final pass: copy gl_screen into gl_swz with red and blue swapped, so that a plain GL_RGBA read returns the
   bytes in X's B,G,R,A order. The driver's GL_BGRA readback is ~2x slower and a CPU swap over 3 M pixels is
   slower still, while this pass costs the GPU almost nothing. */
static const char *VS_SWZ =
  "#version 120\n"
  "attribute vec2 aPos; uniform float uScale; uniform float uFlip; varying vec2 vUv;\n"
  "void main(){ float t = aPos.y * 0.5 + 0.5; vUv = vec2(aPos.x * 0.5 + 0.5, (uFlip > 0.5 ? 1.0 - t : t) * uScale); gl_Position = vec4(aPos, 0.0, 1.0); }\n";
static const char *FS_SWZ =
  "#version 120\n"
  "varying vec2 vUv; uniform sampler2D uTex; uniform float uSwap;\n"
  "void main(){ vec4 c = texture2D(uTex, vUv); gl_FragColor = (uSwap > 0.5) ? c.bgra : c; }\n";

static int sw_init(void) {
  static const char *a[] = { "aPos" };
  sw_prog = gl_link(VS_SWZ, FS_SWZ, a, 1);
  if (!sw_prog) return 0;
  sw_scale = glGetUniformLocation(sw_prog, "uScale");
  sw_flip = glGetUniformLocation(sw_prog, "uFlip");
  sw_swap = glGetUniformLocation(sw_prog, "uSwap");
  glUseProgram(sw_prog);
  glUniform1i(glGetUniformLocation(sw_prog, "uTex"), 0);
  static const float q[8] = { -1, -1, 1, -1, -1, 1, 1, 1 };
  glGenVertexArrays(1, &sw_vao);
  glBindVertexArray(sw_vao);
  glGenBuffers(1, &sw_vbo);
  glBindBuffer(GL_ARRAY_BUFFER, sw_vbo);
  glBufferData(GL_ARRAY_BUFFER, sizeof q, q, GL_STATIC_DRAW);
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void *)0);
  glEnableVertexAttribArray(0);
  glBindVertexArray(0);
  return 1;
}

static void gl_begin_frame(int eff_h) {
  if (eff_h < 1) eff_h = 1;
  if (eff_h > H) eff_h = H;
  if (!scr_ready) {                          /* first frame, or after a rotation */
    scr_ready = 1;
    scr_aw = W; scr_ah = H;
    if (egl_direct) {                        /* the window surface itself is the target: fbo 0, no texture */
      gl_screen.fbo = 0; gl_screen.tex = 0;
    } else {                                 /* the window-sized FBO everything is drawn into */
      img_make_target(&gl_screen, scr_aw, scr_ah);
      if (!present_egl && rb_mode == 0) {
        if (sw_init()) img_make_target(&gl_swz, scr_aw, scr_ah);
        else { fprintf(stderr, "summerboard: swizzle pass unavailable, using GL_BGRA readback\n"); rb_mode = 1; }
      }
    }
  }
  gl_screen.w = W;                           /* logical size; the texture itself stays W x H */
  gl_screen.h = eff_h;
  gl_vp_y = 0;
  if (egl_direct) {                          /* top-align in the window's real height: the shell is resized to eff_h while the keyboard is up */
    EGLint sh = 0;
    eglQuerySurface(egl_dpy, egl_win_surf, EGL_HEIGHT, &sh);
    gl_vp_y = sh > eff_h ? (int)sh - eff_h : 0;
  }
  cur_t = NULL;
  if (prof_on) prof_t0 = now_ms();
  gl_target(&gl_screen);
  double pc0 = prof_on ? now_ms() : 0;
  glClearColor(0, 0, 0, 0);
  glClear(GL_COLOR_BUFFER_BIT);
  if (prof_on) { prof_clr += now_ms() - pc0; prof_note("glClear", now_ms() - pc0); }
}

static void swz_rows(unsigned char *dst, const unsigned char *src, int w, int h, int stride) {
  for (int y = 0; y < h; y++) {
    const uint32_t *sp = (const uint32_t *)(src + (size_t)y * stride);
    uint32_t *dp = (uint32_t *)(dst + (size_t)y * stride);
    for (int x = 0; x < w; x++) { uint32_t v = sp[x]; dp[x] = (v & 0xff00ff00u) | ((v & 0xffu) << 16) | ((v >> 16) & 0xffu); }
  }
}

static void gl_swizzle_pass(int w, int h) {
  glBindFramebuffer(GL_FRAMEBUFFER, gl_swz.fbo);
  cur_t = NULL;                              /* viewport and target are re-established by the next gl_target */
  glViewport(0, 0, w, h);
  glDisable(GL_BLEND);
  glUseProgram(sw_prog);
  glUniform1f(sw_scale, (float)h / (float)scr_ah);
  glUniform1f(sw_flip, 0.f);
  glUniform1f(sw_swap, 1.f);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, gl_screen.tex);
  batch_tex = 0;
  glBindVertexArray(sw_vao);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  glEnable(GL_BLEND);
  blend_on = 1;
}

/* EGL window present: draw gl_screen (top row first) into the window's back buffer, top-aligned and
   flipped to GL's bottom-up window coordinates, then swap. No readback. */
static void gl_present_egl(int w, int h) {
  if (egl_direct) { eglSwapBuffers(egl_dpy, egl_win_surf); return; }   /* already drawn into the back buffer */
  EGLint sh = 0;
  eglQuerySurface(egl_dpy, egl_win_surf, EGL_HEIGHT, &sh);
  int vy = sh > h ? (int)sh - h : 0;
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  cur_t = NULL;
  glViewport(0, vy, w, h);
  glDisable(GL_BLEND);
  glUseProgram(sw_prog);
  glUniform1f(sw_scale, (float)h / (float)scr_ah);
  glUniform1f(sw_flip, 1.f);
  glUniform1f(sw_swap, 0.f);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, gl_screen.tex);
  batch_tex = 0;
  glBindVertexArray(sw_vao);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  glEnable(GL_BLEND);
  blend_on = 1;
  eglSwapBuffers(egl_dpy, egl_win_surf);
}

/* Read the frame into the X image. Rows come out top first, matching how the FBO is drawn. */
static void gl_readback(int w, int h) {
  unsigned char *d = (unsigned char *)x_img->data;
  glPixelStorei(GL_PACK_ALIGNMENT, 4);
  glPixelStorei(GL_PACK_ROW_LENGTH, x_img->bytes_per_line / 4);
  if (rb_mode == 0) {
    gl_swizzle_pass(w, h);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, d);       /* gl_swz is still the bound framebuffer */
  } else {
    glBindFramebuffer(GL_FRAMEBUFFER, gl_screen.fbo);
    cur_t = NULL;
    if (rb_mode == 1) {
      while (glGetError() != GL_NO_ERROR) {}
      glReadPixels(0, 0, w, h, GL_BGRA, GL_UNSIGNED_BYTE, d);
      if (glGetError() != GL_NO_ERROR) { fprintf(stderr, "summerboard: GL_BGRA readback refused, swizzling on the CPU\n"); rb_mode = 2; }
    }
    if (rb_mode == 2) {
      glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, d);
      swz_rows(d, d, w, h, x_img->bytes_per_line);
    }
  }
  glPixelStorei(GL_PACK_ROW_LENGTH, 0);
}

static void gl_present(int w, int h) {
  if (x_use_shm) XShmPutImage(dpy, shell, x_gc, x_img, 0, 0, 0, 0, (unsigned)w, (unsigned)h, False);
  else XPutImage(dpy, shell, x_gc, x_img, 0, 0, 0, 0, (unsigned)w, (unsigned)h);
  XFlush(dpy);
}

static void gl_end_frame(void) {
  flush();
  int w = W, h = gl_screen.h;
  double t1 = 0, t2 = 0, ta = 0, tb = 0, tc = 0, td = 0;
  if (prof_on) { t1 = now_ms(); glFinish(); t2 = now_ms(); }
  if (present_egl) {
    ta = prof_on ? now_ms() : 0;
    gl_present_egl(w, h);
    if (!prof_on) return;
    tb = tc = ta;                            /* no sync/readback in this mode: the swap is the whole present */
    td = now_ms();
    prof_put += td - tc;
    goto prof_out;
  }
  ta = prof_on ? now_ms() : 0;
  if (x_use_shm) XSync(dpy, False);          /* the server is done reading the previous frame from the shared buffer */
  tb = prof_on ? now_ms() : 0;
  gl_readback(w, h);
  tc = prof_on ? now_ms() : 0;
  gl_present(w, h);
  if (!prof_on) return;
  td = now_ms();
  prof_sync += tb - ta; prof_read += tc - tb; prof_put += td - tc;
prof_out:
  prof_note("glFinish", t2 - t1); prof_note("XSync", tb - ta); prof_note("readback", tc - tb); prof_note("present", td - tc);
  if (!prof_wall0) prof_wall0 = prof_t0;
  prof_cpu += t1 - prof_t0; prof_gpu += t2 - t1;
  if (++prof_frames == 30) {
    double n = prof_frames, wall = td - prof_wall0;
    static const char *rbn[] = { "gpu-swz", "bgra", "cpu-swz" };
    fprintf(stderr, "[prof] wall %.1f ms/frame (%.1f fps) | cpu %.2f glFinish %.2f | xsync %.2f readback %.2f present %.2f ms | in cpu: glBufferData %.2f glDrawElements %.2f upload %.2f clear %.2f | %.1f draws %.0f quads %.1f fbo/frame | worst call: %s %.1f ms [rb=%s%s]\n",
            wall / n, 1000.0 * n / wall, prof_cpu / n, prof_gpu / n, prof_sync / n, prof_read / n, prof_put / n,
            prof_buf / n, prof_drw / n, prof_up / n, prof_clr / n,
            prof_draws / n, prof_quads / n, prof_tgt / n, prof_worst_what, prof_worst, present_egl ? "egl" : rbn[rb_mode], opt_nomip ? "" : " mip");
    prof_cpu = prof_gpu = prof_sync = prof_read = prof_put = prof_buf = prof_drw = prof_up = prof_clr = prof_worst = 0;
    prof_draws = prof_quads = prof_tgt = prof_frames = 0; prof_wall0 = 0; prof_worst_what = "";
  }
}

#endif /* SUMMERBOARD_GL_H */
