/* floe - a super-minimal floating window manager for X11
 *
 * A window manager is just a regular X11 client with one extra privilege:
 * it asks the X server for "SubstructureRedirect" on the root window, which
 * means every other client's request to map/move/resize a window is handed
 * to floe as an event instead of being carried out directly. floe's whole
 * job is to react to those events. There is no periodic polling and no
 * hidden state machine: the event loop in main() is the entire program.
 *
 * This WM is purely floating: windows are placed and sized wherever the
 * client (or the user, via Alt+drag) puts them. There is no tiling layout
 * to maintain, so the code stays tiny.
 *
 * build: cc -Os -Wall -Wextra -o floe floe.c -lX11
 *
 * keys : Alt+Shift+Return  spawn terminal
 *        Alt+Shift+c       close focused window
 *        Alt+Shift+q       quit floe
 * mouse: Alt+Button1 drag  move window
 *        Alt+Button3 drag  resize window
 */
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>


/*
 * config
 *
 * Everything you are likely to want to change lives here. There is no
 * config file and no parser: this is the config, in dwm's spirit. Edit a
 * value, run `make`, restart floe.
 */
#define MOD           Mod1Mask  /* modifier key for all bindings below (Alt) */
#define BORDER_WIDTH  2         /* window border thickness, in pixels */
#define COL_FOCUS     0x5f87af  /* border color of the focused window (0xRRGGBB) */
#define COL_NORMAL    0x222222  /* border color of every other window */
static const char *termcmd[] = { "xterm", NULL };       /* argv for Alt+Shift+Return */


#define MAX(a, b) ((a) > (b) ? (a) : (b))

/* the X11 connection and the root window we manage */
static Display *dpy;
static Window root;

/* window that currently holds input focus, if any */
static Window focused = None;

/* geometry of the window being dragged, captured at the start of the drag
 * so move/resize can compute deltas against a fixed reference point */
static XWindowAttributes attr;

/* button/window/pointer-position that started the current drag, or
 * .subwindow == None when no drag is in progress */
static XButtonEvent start = {.subwindow = None };

/* WM_PROTOCOLS and WM_DELETE_WINDOW atoms, used to ask a client to close
 * itself gracefully instead of killing its connection outright */
static Atom wm_protocols, wm_delete;

/* set by on_wm_detected() if another WM already owns the root window */
static int wm_detected = 0;

/* sentinel for the main event loop; set to 0 by the quit keybinding */
static int running = 1;

/* bit of the modifier state that corresponds to NumLock on this keyboard,
 * computed once in update_numlockmask(). Needed because XGrabKey/XGrabButton
 * match an exact modifier state: without accounting for it, every binding
 * would silently stop working the moment NumLock is toggled on. */
static unsigned int numlockmask = 0;


/* Launch cmd as a detached child process (used for the terminal keybinding).
 * We fork, close our copy of the X connection in the child (so the child
 * doesn't keep it alive), detach from floe's session with setsid() so the
 * spawned program survives independently, then exec. SIGCHLD is ignored in
 * main() so the forked child is reaped automatically without a wait(). */
static void spawn(const char **cmd)
{
    if (fork() == 0) {
        if (dpy)
            close(ConnectionNumber(dpy));
        setsid();
        execvp(cmd[0], (char *const *) cmd);
        exit(EXIT_FAILURE);
    }
}


/*
 * Temporary X error handler installed only while we try to select
 * SubstructureRedirect on the root window. Only one client may hold that
 * privilege at a time, so if another window manager is already running,
 * the X server answers with a BadAccess error instead of granting it. That
 * error would normally abort the program (Xlib's default handler calls
 * exit()), so we catch it here and just record that it happened.
 */
static int on_wm_detected(Display *d, XErrorEvent *e)
{
    (void) d;
    if (e->error_code == BadAccess)
        wm_detected = 1;
    return 0;
}


/*
 * Permanent X error handler used once floe is running. X is asynchronous:
 * by the time a request like XConfigureWindow() reaches the server, the
 * target window may already be gone (its client raced us and closed it
 * first). Those errors are harmless and expected, so we swallow all of
 * them rather than letting Xlib kill the WM over a lost race.
 */
static int on_x_error(Display *d, XErrorEvent *e)
{
    (void) d;
    (void) e;
    return 0;
}


/* Give input focus to window w and update border colors to reflect it:
 * the previously focused window (if any) goes back to COL_NORMAL, and w
 * is highlighted with COL_FOCUS. Called both when a new window is mapped
 * and on EnterNotify, which gives us focus-follows-mouse for free. */
static void focus(Window w)
{
    if (w == None || w == root)
        return;
    if (focused != None && focused != w)
        XSetWindowBorder(dpy, focused, COL_NORMAL);
    XSetWindowBorder(dpy, w, COL_FOCUS);
    XSetInputFocus(dpy, w, RevertToPointerRoot, CurrentTime);
    focused = w;
}


/* Close window w, preferring a graceful shutdown over a hard kill.
 * If the client advertises support for the WM_DELETE_WINDOW protocol (via
 * WM_PROTOCOLS, see ICCCM), we send it a ClientMessage asking it to close
 * itself, so it gets a chance to prompt for unsaved changes, run cleanup,
 * etc. Otherwise we fall back to XKillClient, which forcibly terminates
 * its connection to the X server. */
static void close_window(Window w)
{
    Atom *protos;
    int n, i, deletable = 0;

    if (w == None || w == root)
        return;
    if (XGetWMProtocols(dpy, w, &protos, &n)) {
        for (i = 0; i < n; i++)
            if (protos[i] == wm_delete)
                deletable = 1;
        XFree(protos);
    }
    if (deletable) {
        XEvent ev = { 0 };
        ev.type = ClientMessage;
        ev.xclient.window = w;
        ev.xclient.message_type = wm_protocols;
        ev.xclient.format = 32;
        ev.xclient.data.l[0] = (long) wm_delete;
        ev.xclient.data.l[1] = CurrentTime;
        XSendEvent(dpy, w, False, NoEventMask, &ev);
    }
    else {
        XKillClient(dpy, w);
    }
}


/* Find which modifier bit (Mod1Mask..Mod5Mask, or none) the X server has
 * NumLock bound to on this keyboard, and cache it in numlockmask. Xlib
 * exposes the keyboard's modifier table as 8 columns (Shift, Lock,
 * Control, Mod1..Mod5), each listing the keycodes bound to it; we just
 * scan every column for the keycode NumLock is on. */
static void update_numlockmask(void)
{
    unsigned int i, j;
    XModifierKeymap *modmap;
    KeyCode numlock_kc = XKeysymToKeycode(dpy, XK_Num_Lock);

    numlockmask = 0;
    modmap = XGetModifierMapping(dpy);
    for (i = 0; i < 8; i++)
        for (j = 0; j < (unsigned int) modmap->max_keypermod; j++)
            if (modmap->modifiermap[i * modmap->max_keypermod + j] == numlock_kc)
                numlockmask = 1 << i;
    XFreeModifiermap(modmap);
}


/* Register every keyboard/mouse binding floe reacts to. These are
 * "passive grabs": we tell the X server up front which key/button
 * combinations we want reported to us (as KeyPress/ButtonPress events on
 * the root window) instead of being delivered straight to the client
 * under the pointer. This is the mechanism behind every global shortcut
 * in this WM; there is no separate keybinding table to maintain, this
 * function IS the table.
 *
 * XGrabKey/XGrabButton match an *exact* modifier state, and NumLock/
 * CapsLock/ScrollLock show up in that state like any other modifier. So a
 * grab for just MOD|ShiftMask only fires while every lock key happens to
 * be off. We work around this the way dwm does: grab each binding once
 * per combination of "real" modifiers and lock modifiers, so the lock
 * keys' state is simply irrelevant to whether a shortcut fires. */
static void grab(void)
{
    unsigned int i;
    unsigned int m = MOD | ShiftMask;
    unsigned int locks[] = { 0, LockMask, numlockmask, numlockmask | LockMask };

    for (i = 0; i < sizeof(locks) / sizeof(locks[0]); i++) {
        XGrabKey(dpy, XKeysymToKeycode(dpy, XK_Return), m | locks[i], root, True, GrabModeAsync, GrabModeAsync);
        XGrabKey(dpy, XKeysymToKeycode(dpy, XK_c), m | locks[i], root, True, GrabModeAsync, GrabModeAsync);
        XGrabKey(dpy, XKeysymToKeycode(dpy, XK_q), m | locks[i], root, True, GrabModeAsync, GrabModeAsync);
        XGrabButton(dpy, 1, MOD | locks[i], root, True, ButtonPressMask, GrabModeAsync, GrabModeAsync, None, None);
        XGrabButton(dpy, 3, MOD | locks[i], root, True, ButtonPressMask, GrabModeAsync, GrabModeAsync, None, None);
    }
}


int main(void)
{
    XEvent ev;

    if (!(dpy = XOpenDisplay(NULL))) {
        fputs("floe: cannot open display\n", stderr);
        return EXIT_FAILURE;
    }
    root = DefaultRootWindow(dpy);
    wm_protocols = XInternAtom(dpy, "WM_PROTOCOLS", False);
    wm_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    signal(SIGCHLD, SIG_IGN);   /* reap children, no zombies */

    /* Try to become the window manager. Selecting SubstructureRedirect on
     * the root window is the one privileged operation that defines "being
     * a WM"; only one client can hold it, so we install a scratch error
     * handler, ask for it, and force the request to the server with
     * XSync() so any BadAccess reply arrives before we check wm_detected. */
    XSetErrorHandler(on_wm_detected);
    XSelectInput(dpy, root, SubstructureRedirectMask | SubstructureNotifyMask);
    XSync(dpy, False);
    if (wm_detected) {
        fputs("floe: another window manager is already running\n", stderr);
        return EXIT_FAILURE;
    }
    XSetErrorHandler(on_x_error);
    update_numlockmask();
    grab();

    /* The event loop is the whole program: floe does nothing until the X
     * server hands it something to react to. Each case below corresponds
     * to one thing a client or the user can do. */
    while (running && !XNextEvent(dpy, &ev)) {
        switch (ev.type) {
        case MapRequest:{
                /* A client wants to become visible. Because we hold
                 * SubstructureRedirect, mapping doesn't happen until we
                 * do it ourselves. Being floating, we don't impose any
                 * position or size of our own: we just decorate with a
                 * border, map it wherever the client asked, and focus
                 * it, tracking future EnterNotify events for it. */
                Window w = ev.xmaprequest.window;
                XSetWindowBorderWidth(dpy, w, BORDER_WIDTH);
                XSelectInput(dpy, w, EnterWindowMask);
                XMapWindow(dpy, w);
                focus(w);
                break;
            }
        case ConfigureRequest:{
                /* A client wants to move/resize/restack itself. This is
                 * the crux of "floating": we don't second-guess the
                 * request, we just grant it verbatim by replaying the
                 * requested fields into XConfigureWindow(). A tiling WM
                 * would instead compute its own geometry here. */
                XConfigureRequestEvent *e = &ev.xconfigurerequest;
                XWindowChanges wc = {
                    .x = e->x,.y = e->y,
                    .width = e->width,.height = e->height,
                    .border_width = e->border_width,
                    .sibling = e->above,.stack_mode = e->detail
                };
                XConfigureWindow(dpy, e->window, e->value_mask, &wc);
                break;
            }
        case EnterNotify:
            /* Pointer entered a window: focus-follows-mouse. */
            focus(ev.xcrossing.window);
            break;
        case KeyPress:{
                /* One of the three global shortcuts grabbed in grab(). */
                KeySym ks = XLookupKeysym(&ev.xkey, 0);
                if (ks == XK_Return)
                    spawn(termcmd);
                else if (ks == XK_c)
                    close_window(focused);
                else if (ks == XK_q)
                    running = 0;
                break;
            }
        case ButtonPress:
            /* Start of an Alt+drag: remember which window, which button,
             * and the window's geometry/pointer position at this instant
             * (attr, start), so MotionNotify can compute a delta from a
             * fixed reference instead of from the previous event. We also
             * grab the pointer so motion/release events keep arriving
             * even if the cursor leaves the window, and raise+focus the
             * window being interacted with. */
            if (ev.xbutton.subwindow == None)
                break;
            XGetWindowAttributes(dpy, ev.xbutton.subwindow, &attr);
            XGrabPointer(dpy, ev.xbutton.subwindow, True,
                         PointerMotionMask | ButtonReleaseMask, GrabModeAsync, GrabModeAsync, None, None, CurrentTime);
            XRaiseWindow(dpy, ev.xbutton.subwindow);
            focus(ev.xbutton.subwindow);
            start = ev.xbutton;
            break;
        case MotionNotify:{
                /* Dragging in progress. The X server can queue up motion
                 * events faster than we redraw, so we drain the queue and
                 * keep only the most recent one before acting, to avoid
                 * visibly lagging behind the pointer. Button 1 moves the
                 * window, button 3 resizes it (clamped to at least 1px). */
                int dx, dy;
                if (start.subwindow == None)
                    break;
                while (XCheckTypedEvent(dpy, MotionNotify, &ev));       /* use only the most recent motion */
                dx = ev.xmotion.x_root - start.x_root;
                dy = ev.xmotion.y_root - start.y_root;
                if (start.button == 1)
                    XMoveWindow(dpy, start.subwindow, attr.x + dx, attr.y + dy);
                else if (start.button == 3)
                    XResizeWindow(dpy, start.subwindow, MAX(1, attr.width + dx), MAX(1, attr.height + dy));
                break;
            }
        case ButtonRelease:
            /* End of the drag: release the pointer grab and clear the
             * drag state so MotionNotify becomes a no-op again. */
            XUngrabPointer(dpy, CurrentTime);
            start.subwindow = None;
            break;
        }
    }
    XCloseDisplay(dpy);
    return EXIT_SUCCESS;
}
