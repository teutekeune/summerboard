/* Hold a touchscreen finger still until it moves past a small slop,
   so a tap lands inside a ~20px tool button. A real drag is followed. */
#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define SRC "/dev/input/event1"
#define SLOP_PX 28

static int abs_max_x = 2591, abs_max_y = 1470;

static void ev(int fd, int type, int code, int val) {
  struct input_event e;
  memset(&e, 0, sizeof e);
  e.type = type;
  e.code = code;
  e.value = val;
  write(fd, &e, sizeof e);
}

static int open_uinput(void) {
  int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
  if (fd < 0) return -1;
  ioctl(fd, UI_SET_EVBIT, EV_KEY);
  ioctl(fd, UI_SET_EVBIT, EV_ABS);
  ioctl(fd, UI_SET_EVBIT, EV_SYN);
  ioctl(fd, UI_SET_KEYBIT, BTN_TOUCH);
  ioctl(fd, UI_SET_ABSBIT, ABS_X);
  ioctl(fd, UI_SET_ABSBIT, ABS_Y);
  ioctl(fd, UI_SET_ABSBIT, ABS_MT_SLOT);
  ioctl(fd, UI_SET_ABSBIT, ABS_MT_TRACKING_ID);
  ioctl(fd, UI_SET_ABSBIT, ABS_MT_POSITION_X);
  ioctl(fd, UI_SET_ABSBIT, ABS_MT_POSITION_Y);
  ioctl(fd, UI_SET_PROPBIT, INPUT_PROP_DIRECT);
  struct uinput_user_dev u;
  memset(&u, 0, sizeof u);
  snprintf(u.name, sizeof u.name, "Summerboard Touch");
  u.absmin[ABS_X] = u.absmin[ABS_MT_POSITION_X] = 0;
  u.absmax[ABS_X] = u.absmax[ABS_MT_POSITION_X] = abs_max_x;
  u.absmin[ABS_Y] = u.absmin[ABS_MT_POSITION_Y] = 0;
  u.absmax[ABS_Y] = u.absmax[ABS_MT_POSITION_Y] = abs_max_y;
  u.absmax[ABS_MT_SLOT] = 5;
  u.absmin[ABS_MT_TRACKING_ID] = 0;
  u.absmax[ABS_MT_TRACKING_ID] = 65535;
  if (write(fd, &u, sizeof u) != (ssize_t)sizeof u) return -1;
  if (ioctl(fd, UI_DEV_CREATE) < 0) return -1;
  return fd;
}

int main(void) {
  int src = open(SRC, O_RDONLY);
  if (src < 0) return 0;
  struct input_absinfo ax, ay;
  if (ioctl(src, EVIOCGABS(ABS_MT_POSITION_X), &ax) == 0 && ax.maximum > 0)
    abs_max_x = ax.maximum;
  if (ioctl(src, EVIOCGABS(ABS_MT_POSITION_Y), &ay) == 0 && ay.maximum > 0)
    abs_max_y = ay.maximum;
  int out = open_uinput();
  if (out < 0) return 0;
  usleep(200000);
  if (ioctl(src, EVIOCGRAB, 1) < 0) {
    ioctl(out, UI_DEV_DESTROY);
    return 0;
  }
  fprintf(stderr, "touch-tap: holding taps within %dpx\n", SLOP_PX);
  int slop_x = SLOP_PX * abs_max_x / 1366;
  int slop_y = SLOP_PX * abs_max_y / 768;
  if (slop_x < 1) slop_x = 1;
  if (slop_y < 1) slop_y = 1;

  int down = 0, dragging = 0;
  int ox = 0, oy = 0, px = 0, py = 0;
  int have_x = 0, have_y = 0;
  for (;;) {
    struct input_event e;
    if (read(src, &e, sizeof e) != (ssize_t)sizeof e) continue;
    if (e.type == EV_ABS && (e.code == ABS_MT_POSITION_X || e.code == ABS_X)) {
      px = e.value; have_x = 1; continue;
    }
    if (e.type == EV_ABS && (e.code == ABS_MT_POSITION_Y || e.code == ABS_Y)) {
      py = e.value; have_y = 1; continue;
    }
    if (e.type == EV_KEY && e.code == BTN_TOUCH) {
      if (e.value) {
        ox = px; oy = py; down = 1; dragging = 0;
        ev(out, EV_ABS, ABS_MT_SLOT, 0);
        ev(out, EV_ABS, ABS_MT_TRACKING_ID, 1);
        ev(out, EV_ABS, ABS_MT_POSITION_X, ox);
        ev(out, EV_ABS, ABS_MT_POSITION_Y, oy);
        ev(out, EV_ABS, ABS_X, ox);
        ev(out, EV_ABS, ABS_Y, oy);
        ev(out, EV_KEY, BTN_TOUCH, 1);
        ev(out, EV_SYN, SYN_REPORT, 0);
      } else if (down) {
        int x = dragging ? px : ox;
        int y = dragging ? py : oy;
        ev(out, EV_ABS, ABS_MT_POSITION_X, x);
        ev(out, EV_ABS, ABS_MT_POSITION_Y, y);
        ev(out, EV_ABS, ABS_X, x);
        ev(out, EV_ABS, ABS_Y, y);
        ev(out, EV_KEY, BTN_TOUCH, 0);
        ev(out, EV_ABS, ABS_MT_TRACKING_ID, -1);
        ev(out, EV_SYN, SYN_REPORT, 0);
        down = 0;
      }
      continue;
    }
    if (e.type == EV_SYN && e.code == SYN_REPORT && down && have_x && have_y) {
      int dx = px - ox; if (dx < 0) dx = -dx;
      int dy = py - oy; if (dy < 0) dy = -dy;
      if (!dragging && (dx > slop_x || dy > slop_y)) dragging = 1;
      if (dragging) {
        ev(out, EV_ABS, ABS_MT_POSITION_X, px);
        ev(out, EV_ABS, ABS_MT_POSITION_Y, py);
        ev(out, EV_ABS, ABS_X, px);
        ev(out, EV_ABS, ABS_Y, py);
        ev(out, EV_SYN, SYN_REPORT, 0);
      }
      continue;
    }
  }
}
