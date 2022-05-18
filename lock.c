#define _XOPEN_SOURCE 500
#if HAVE_SHADOW_H
#include <shadow.h>
#endif

#include <ctype.h>
#include <errno.h>
#include <pwd.h>
#include <stdarg.h>
#include <stdlib.h>
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <X11/keysym.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>

#if HAVE_BSD_AUTH
#include <login_cap.h>
#include <bsd_auth.h>
#endif

#define CMD_LENGTH 500

#define POWEROFF 1
#define USBOFF 1
#define STRICT_USBOFF 0
#define TRANSPARENT 1

char *g_pw = NULL;
int lock_tries = 0;

typedef struct {
  int screen;
  Window root, win;
  Pixmap pmap;
  unsigned long colors[2];
} Lock;

static Lock **locks;
static int nscreens;
static Bool running = True;

static void die(const char *errstr, ...) {
  va_list ap;

  va_start(ap, errstr);
  vfprintf(stderr, errstr, ap);
  va_end(ap);
  exit(EXIT_FAILURE);
}

#ifdef __linux__
#include <fcntl.h>

static void dontkillme(void) {
  errno = 0;
  int fd = open("/proc/self/oom_score_adj", O_WRONLY);

  if (fd < 0 && errno == ENOENT)
    return;

  if (fd < 0)
    goto error;

  if (write(fd, "-1000\n", 6) != 6) {
    close(fd);
    goto error;
  }

  if (close(fd) != 0)
    goto error;

  return;

error:
  pid_t pid = getpid();

  char cmd[CMD_LENGTH];

  int r = snprintf(
    cmd,
    CMD_LENGTH,
    "echo -1000 | sudo -n tee /proc/%u/oom_score_adj > /dev/null 2>& 1",
    (unsigned int)pid
  );

  if (r >= 0 && r < CMD_LENGTH)
    system(cmd);
}
#endif

static void disable_kill(void) {
#if POWEROFF
  // Needs sudo privileges - alter your /etc/sudoers file:
  // [username] [hostname] =NOPASSWD: /usr/bin/tee /proc/sys/kernel/sysrq
  // Needs sudo privileges - alter your /etc/sudoers file:
  // [username] [hostname] =NOPASSWD:
  // /usr/bin/tee /proc/sys/kernel/sysrq,/usr/bin/tee /proc/sysrq-trigger
  // system("echo 1 | sudo -n tee /proc/sys/kernel/sysrq > /dev/null");
  // system("echo o | sudo -n tee /proc/sysrq-trigger > /dev/null");
  system("echo 0 | sudo -n tee /proc/sys/kernel/sysrq > /dev/null 2>& 1 &");
  // Disable ctrl+alt+backspace
  system("setxkbmap -option &");
#else
  return;
#endif
}

// Turn USB off on lock.
static void usboff(void) {
#if USBOFF
  // Needs sudo privileges - alter your /etc/sudoers file:
  // [username] [hostname] =NOPASSWD:
  // /sbin/sysctl kernel.grsecurity.deny_new_usb=1
  system("sudo -n sysctl kernel.grsecurity.deny_new_usb=1 2> /dev/null");
#if STRICT_USBOFF
  system("sudo -n sysctl kernel.grsecurity.grsec_lock=1 2> /dev/null");
#endif
#else
  return;
#endif
}

// Turn on USB when the correct password is entered.
static void usbon(void) {
#if USBOFF
  // Needs sudo privileges - alter your /etc/sudoers file:
  // [username] [hostname] =NOPASSWD:
  // /sbin/sysctl kernel.grsecurity.deny_new_usb=0
  system("sudo -n sysctl kernel.grsecurity.deny_new_usb=0 2> /dev/null");
#else
  return;
#endif
}

static void unlockscreen(Display *dpy, Lock *lock) {
  usbon();

  if (dpy == NULL || lock == NULL)
    return;

  XUngrabPointer(dpy, CurrentTime);

#if !TRANSPARENT
  XFreeColors(dpy, DefaultColormap(dpy, lock->screen), lock->colors, 2, 0);
  XFreePixmap(dpy, lock->pmap);
#endif

  XDestroyWindow(dpy, lock->win);

  free(lock);
}

static Lock* lockscreen(Display *dpy, int screen) {
  unsigned int len;
  Lock *lock;
  XSetWindowAttributes wa;

  if (dpy == NULL || screen < 0)
    return NULL;

  lock = malloc(sizeof(Lock));

  if (lock == NULL)
    return NULL;

  lock->screen = screen;

  lock->root = RootWindow(dpy, lock->screen);

  #if TRANSPARENT
    XVisualInfo vi;
    XMatchVisualInfo(dpy, DefaultScreen(dpy), 32, TrueColor, &vi);
    wa.colormap = XCreateColormap(
      dpy,
      DefaultRootWindow(dpy),
      vi.visual,
      AllocNone
    );
  #endif

  // init
  wa.override_redirect = 1;
  #if !TRANSPARENT
      wa.background_pixel = BlackPixel(dpy, lock->screen);
  #else
    wa.border_pixel = 0;
    wa.background_pixel = 0xaa000000;
  #endif

  #if !TRANSPARENT
    int field = CWOverrideRedirect | CWBackPixel;
    lock->win = XCreateWindow(
      dpy,
      lock->root,
      0,
      0,
      DisplayWidth(dpy, lock->screen),
      DisplayHeight(dpy, lock->screen),
      0,
      DefaultDepth(dpy, lock->screen),
      CopyFromParent,
      DefaultVisual(dpy, lock->screen),
      field,
      &wa
    );
  #else
    int field = CWOverrideRedirect | CWBackPixel | CWColormap | CWBorderPixel;
    lock->win = XCreateWindow(
      dpy,
      lock->root,
      0,
      0,
      DisplayWidth(dpy, lock->screen),
      DisplayHeight(dpy, lock->screen),
      0,
      vi.depth,
      CopyFromParent,
      vi.visual,
      field,
      &wa
    );
  #endif

  Atom name_atom = XA_WM_NAME;
  XTextProperty name_prop = { "slock", name_atom, 8, 5 };
  XSetWMName(dpy, lock->win, &name_prop);

  XClassHint *hint = XAllocClassHint();
  if (hint) {
    hint->res_name = "slock";
    hint->res_class = "slock";
    XSetClassHint(dpy, lock->win, hint);
    XFree(hint);
  }

  #if !TRANSPARENT
    Cursor invisible;
    XColor color, dummy;
    char curs[] = {0, 0, 0, 0, 0, 0, 0, 0};
    int cmap = DefaultColormap(dpy, lock->screen);

    XAllocNamedColor(dpy, cmap, COLOR2, &color, &dummy);
    lock->colors[1] = color.pixel;

    XAllocNamedColor(dpy, cmap, COLOR1, &color, &dummy);
    lock->colors[0] = color.pixel;

    lock->pmap = XCreateBitmapFromData(dpy, lock->win, curs, 8, 8);

    invisible = XCreatePixmapCursor(
      dpy, lock->pmap, lock->pmap, &color, &color, 0, 0);

    XDefineCursor(dpy, lock->win, invisible);
  #endif

  XMapRaised(dpy, lock->win);

  for (len = 1000; len > 0; len--) {
    int field = ButtonPressMask | ButtonReleaseMask | PointerMotionMask;

  #if !TRANSPARENT
      int grab = XGrabPointer(
        dpy,
        lock->root,
        False,
        field,
        GrabModeAsync,
        GrabModeAsync,
        None,
        invisible,
        CurrentTime
      );
  #else
      int grab = XGrabPointer(
        dpy,
        lock->root,
        False,
        field,
        GrabModeAsync,
        GrabModeAsync,
        None,
        None,
        CurrentTime
      );
  #endif

    if (grab == GrabSuccess)
      break;

    usleep(1000);
  }

  if (running && (len > 0)) {
    for (len = 1000; len; len--) {
      int grab = XGrabKeyboard(
        dpy,
        lock->root,
        True,
        GrabModeAsync,
        GrabModeAsync,
        CurrentTime
      );

      if (grab == GrabSuccess)
        break;

      usleep(1000);
    }
  }

  running &= (len > 0);

  if (!running) {
    unlockscreen(dpy, lock);
    lock = NULL;
  } else {
    XSelectInput(dpy, lock->root, SubstructureNotifyMask);
    usboff();
  }

  return lock;
}

int main(int argc, char **argv) {
  if (getuid()) exit(0);
  Display *dpy;
  int screen;

  #ifdef SLOCK_QUIET
    freopen("/dev/null", "a", stdout);
    freopen("/dev/null", "a", stderr);
  #endif

  #ifdef __linux__
    dontkillme();
  #endif

  dpy = XOpenDisplay(0);
  if (!dpy)
    die("slock: cannot open display\n");

    nscreens = ScreenCount(dpy);

    errno = 0;
    locks = malloc(sizeof(Lock *) * nscreens);

    if (locks == NULL)
      die("slock: malloc: %s\n", strerror(errno));

    int nlocks = 0;

    for (screen = 0; screen < nscreens; screen++) {
      locks[screen] = lockscreen(dpy, screen);
      if (locks[screen] != NULL)
        nlocks++;
  }

  XSync(dpy, False);

  if (nlocks == 0) {
    free(locks);
    XCloseDisplay(dpy);
    return 1;
  }

  // Insert authentication logic here
  //sleep(3);
  int code = WEXITSTATUS(system("python /usr/lib/security/howdy/compare.py prayuj 2>/dev/null 1>&2"));
  while (code != 0) {
    code = WEXITSTATUS(system("python /usr/lib/security/howdy/compare.py prayuj 2>/dev/null 1>&2"));
  }

  for (screen = 0; screen < nscreens; screen++)
    unlockscreen(dpy, locks[screen]);

  free(locks);
  XCloseDisplay(dpy);
}
