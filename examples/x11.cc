#include "neatvnc.h"

#include <stdio.h>

#include <X11/Xlib.h>
#include <X11/extensions/Xcomposite.h>

int main() {
    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) {
        exit(1);
    }

    int screen = DefaultScreen(dpy);
    Window root = RootWindow(dpy, screen);
    int w = DisplayWidth(dpy, screen);
    int h = DisplayHeight(dpy, screen);
    int off_x = 0;
    int off_y = 0;

    int major, minor;
    XCompositeQueryVersion(dpy, &major, &minor);
    if (major > 0 || minor >= 2) {
        printf("XComposite available: %d.%d\n", major, minor);
    }

    return 0;
}