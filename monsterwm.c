/* see license for copyright and license */

#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdbool.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <sys/wait.h>
#include <X11/keysym.h>
#include <xcb/xcb.h>
#include <xcb/xcb_atom.h>
#include <xcb/xcb_icccm.h>
#include <xcb/xcb_keysyms.h>

/* TODO: Reduce SLOC */

/* set this to 1 to enable debug prints */
#if 0
#  define DEBUG(x)      puts(x);
#  define DEBUGP(x,...) printf(x, ##__VA_ARGS__);
#else
#  define DEBUG(x)      ;
#  define DEBUGP(x,...) ;
#endif

/* upstream compatility */
#define True  true
#define False false
#define Mod1Mask     XCB_MOD_MASK_1
#define Mod4Mask     XCB_MOD_MASK_4
#define ShiftMask    XCB_MOD_MASK_SHIFT
#define ControlMask  XCB_MOD_MASK_CONTROL
#define Button1      XCB_BUTTON_INDEX_1
#define Button2      XCB_BUTTON_INDEX_2
#define Button3      XCB_BUTTON_INDEX_3
#define XCB_MOVE_RESIZE XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y | XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT
#define XCB_MOVE        XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y
#define XCB_RESIZE      XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT

static const xcb_atom_t XCB_ATOM_NULL = 0;
static char *WM_ATOM_NAME[]   = { "WM_PROTOCOLS", "WM_DELETE_WINDOW" };
static char *NET_ATOM_NAME[]  = { "_NET_SUPPORTED", "_NET_WM_STATE_FULLSCREEN", "_NET_WM_STATE", "_NET_ACTIVE_WINDOW" };

#define LENGTH(x) (sizeof(x)/sizeof(*x))
#define CLEANMASK(mask) (mask & ~(numlockmask | XCB_MOD_MASK_LOCK))
#define BUTTONMASK      XCB_EVENT_MASK_BUTTON_PRESS|XCB_EVENT_MASK_BUTTON_RELEASE

/* mouse motion actions */
enum { RESIZE, MOVE, };
/* tiling layout modes */
enum { TILE, MONOCLE, BSTACK, GRID, };
/* wm and net atoms selected through wmatoms and netatoms arrays */
enum { WM_PROTOCOLS, WM_DELETE_WINDOW, WM_COUNT };
enum { NET_SUPPORTED, NET_FULLSCREEN, NET_WM_STATE, NET_ACTIVE, NET_COUNT };

/* argument structure to be passed to function by config.h
 * com  - a command to run
 * i    - an integer to indicate different states
 */
typedef union {
    const char** com;
    const int i;
} Arg;

/* a key struct represents a combination of
 * mod      - a modifier mask
 * keysym   - and the key pressed
 * func     - the function to be triggered because of the above combo
 * arg      - the argument to the function
 */
typedef struct {
    unsigned int mod;
    xcb_keysym_t keysym;
    void (*func)(const Arg *);
    const Arg arg;
} key;

/* a button struct represents a combination of
 * mask     - a modifier mask
 * button   - and the mouse button pressed
 * func     - the function to be triggered because of the above combo
 * arg      - the argument to the function
 */
typedef struct {
    unsigned int mask;
    xcb_button_t button;
    void (*func)(const Arg *);
    const Arg arg;
} Button;

/* a client is a wrapper to a window that additionally
 * holds some properties for that window
 *
 * next         - the client after this one, or NULL if the current is the only or last client
 * isurgent     - the window received an urgent hint
 * istransient  - the window is transient
 * isfullscreen - the window is fullscreen
 * isfloating   - the window is floating
 * win          - the window
 *
 * istransient is separate from isfloating as floating window can be reset
 * to their tiling positions, while the transients will always be floating
 */
typedef struct client {
    struct client *next;
    bool isurgent, istransient, isfullscreen, isfloating;
    xcb_window_t win;
} client;

/* properties of each desktop
 * master_size  - the size of the master window
 * mode         - the desktop's tiling layout mode
 * growth       - growth factor of the first stack window
 * head         - the start of the client list
 * current      - the currently highlighted window
 * prevfocus    - the client that previously had focus
 * showpanel    - the visibility status of the panel
 */
typedef struct {
    int master_size, mode, growth;
    client *head, *current, *prevfocus;
    bool showpanel;
} desktop;

/* define behavior of certain applications
 * configured in config.h
 * class    - the class or name of the instance
 * desktop  - what desktop it should be spawned at
 * follow   - whether to change desktop focus to the specified desktop
 */
typedef struct {
    const char *class;
    const int desktop;
    const bool follow;
    const bool floating;
} AppRule;

/* Functions */
static client* addwindow(xcb_window_t w);
static void buttonpress(xcb_generic_event_t *e);
static void change_desktop(const Arg *arg);
static void cleanup(void);
static void client_to_desktop(const Arg *arg);
static void clientmessage(xcb_generic_event_t *e);
static void configurerequest(xcb_generic_event_t *e);
static void desktopinfo(void);
static void destroynotify(xcb_generic_event_t *e);
static void die(const char* errstr, ...);
static void enternotify(xcb_generic_event_t *e);
static void focusurgent();
static unsigned int getcolor(char* color);
static void grabbuttons(client *c);
static void grabkeys(void);
static void keypress(xcb_generic_event_t *e);
static void killclient();
static void last_desktop();
static void maprequest(xcb_generic_event_t *e);
static void move_down();
static void move_up();
static void mousemotion(const Arg *arg);
static void next_win();
static void prev_win();
static void propertynotify(xcb_generic_event_t *e);
static void quit(const Arg *arg);
static void removeclient(client *c);
static void resize_master(const Arg *arg);
static void resize_stack(const Arg *arg);
static void rotate_desktop(const Arg *arg);
static void run(void);
static void save_desktop(int i);
static void select_desktop(int i);
static void sendevent(xcb_window_t, int atom);
static void setfullscreen(client *c, bool fullscreen);
static int setup(int default_screen);
static void sigchld();
static void spawn(const Arg *arg);
static void swap_master();
static void switch_mode(const Arg *arg);
static void tile(void);
static void togglepanel();
static void update_current(client *c);
static void unmapnotify(xcb_generic_event_t *e);
static client* wintoclient(xcb_window_t w);

#include "config.h"

/* variables */
static bool running = true;
static bool showpanel = SHOW_PANEL;
static int retval = 0;
static int current_desktop = 0;
static int previous_desktop = 0;
static int growth = 0;
static int mode = DEFAULT_MODE;
static int master_size;
static int wh; /* window area height - screen height minus the border size and panel height */
static int ww; /* window area width - screen width minus the border size */
static unsigned int win_unfocus, win_focus;
static unsigned int numlockmask = 0; /* dynamic key lock mask */
static xcb_connection_t *dis;
static xcb_screen_t *screen;
static client *head, *prevfocus, *current;

static xcb_atom_t wmatoms[WM_COUNT], netatoms[NET_COUNT];
static desktop desktops[DESKTOPS];

/* events array
 * on receival of a new event, call the appropriate function to handle it
 */
static void (*events[XCB_NO_OPERATION])(xcb_generic_event_t *e);

/* get screen of display */
static xcb_screen_t *screen_of_display(xcb_connection_t *con, int screen) {
    xcb_screen_iterator_t iter;

    iter = xcb_setup_roots_iterator(xcb_get_setup(con));
    for (; iter.rem; --screen, xcb_screen_next(&iter))
        if (screen == 0) return iter.data;

    return NULL;
}

/* wrapper to move and resize window */
static inline void xcb_move_resize(xcb_connection_t *con, xcb_window_t win, int x, int y, int w, int h) {
    unsigned int pos[4] = { x, y, w, h };
    xcb_configure_window(con, win, XCB_MOVE_RESIZE, pos);
}

/* wrapper to move window */
static inline void xcb_move(xcb_connection_t *con, xcb_window_t win, int x, int y) {
    unsigned int pos[2] = { x, y };
    xcb_configure_window(con, win, XCB_MOVE, pos);
}

/* wrapper to resize window */
static inline void xcb_resize(xcb_connection_t *con, xcb_window_t win, int w, int h) {
    unsigned int pos[2] = { w, h };
    xcb_configure_window(con, win, XCB_RESIZE, pos);
}

/* wrapper to raise window */
static inline void xcb_raise_window(xcb_connection_t *con, xcb_window_t win) {
    unsigned int arg[1] = { XCB_STACK_MODE_ABOVE };
    xcb_configure_window(con, win, XCB_CONFIG_WINDOW_STACK_MODE, arg);
}

/* wrapper to set xcb border width */
static inline void xcb_border_width(xcb_connection_t *con, xcb_window_t win, int w) {
    unsigned int arg[1] = { w };
    xcb_configure_window(con, win, XCB_CONFIG_WINDOW_BORDER_WIDTH, arg);
}

/* wrapper to get xcb keysymbol from keycode */
static xcb_keysym_t xcb_get_keysym(xcb_keycode_t keycode) {
    xcb_key_symbols_t *keysyms;
    xcb_keysym_t       keysym;

    if (!(keysyms = xcb_key_symbols_alloc(dis))) return 0;
    keysym = xcb_key_symbols_get_keysym(keysyms, keycode, 0);
    xcb_key_symbols_free(keysyms);

    return keysym;
}

/* wrapper to get xcb keycodes from keysymbol */
static xcb_keycode_t* xcb_get_keycodes(xcb_keysym_t keysym) {
    xcb_key_symbols_t *keysyms;
    xcb_keycode_t     *keycode;

    if (!(keysyms = xcb_key_symbols_alloc(dis))) return NULL;
    keycode = xcb_key_symbols_get_keycode(keysyms, keysym);
    xcb_key_symbols_free(keysyms);

    return keycode;
}

/* retieve RGB color from hex (think of html) */
static unsigned int xcb_get_colorpixel(char *hex) {
    char strgroups[3][3]  = {{hex[1], hex[2], '\0'},
                             {hex[3], hex[4], '\0'},
                             {hex[5], hex[6], '\0'}};
    unsigned int rgb16[3] = {(strtol(strgroups[0], NULL, 16)),
                             (strtol(strgroups[1], NULL, 16)),
                             (strtol(strgroups[2], NULL, 16))};

    return (rgb16[0] << 16) + (rgb16[1] << 8) + rgb16[2];
}

/* wrapper to get atoms using xcb */
static void xcb_get_atoms(char **names, xcb_atom_t *atoms, unsigned int count) {
    xcb_intern_atom_cookie_t cookies[count];
    xcb_intern_atom_reply_t  *reply;

    for (unsigned int i = 0; i < count; i++)
        cookies[i] = xcb_intern_atom(dis, 0, strlen(names[i]), names[i]);

    /* get responses */
    for (unsigned int i = 0; i < count; i++) {
        reply = xcb_intern_atom_reply(dis, cookies[i], NULL); /* TODO: Handle error */
        if (reply) {
            DEBUGP("%s : %d\n", names[i], reply->atom);
            atoms[i] = reply->atom;
            free(reply);
        } else puts("WARN: monsterwm failed to register %s atom.\nThings might not work right.");
    }
}

/* wrapper to window get attributes using xcb */
static void xcb_get_attributes(xcb_window_t *windows, xcb_get_window_attributes_reply_t **reply, unsigned int count) {
    xcb_get_window_attributes_cookie_t cookies[count];

    for (unsigned int i = 0; i < count; i++)
       cookies[i] = xcb_get_window_attributes(dis, windows[i]);

    for (unsigned int i = 0; i < count; i++)
       reply[i] = xcb_get_window_attributes_reply(dis, cookies[i], NULL); /* TODO: Handle error */
}

/* check if other wm exists */
static int checkotherwm(void) {
    xcb_generic_error_t *error;
    unsigned int mask = XCB_CW_EVENT_MASK;
    unsigned int values[1] = {XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT|XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY|
                              XCB_EVENT_MASK_PROPERTY_CHANGE|XCB_EVENT_MASK_BUTTON_PRESS};

    error = xcb_request_check(dis, xcb_change_window_attributes_checked(dis, screen->root, mask, values));
    xcb_flush(dis);

    if (error) return 1;
    return 0;
}

/* create a new client and add the new window
 * window should notify of property change events
 */
client* addwindow(xcb_window_t w) {
    client *c, *t;
    if (!(c = (client *)calloc(1, sizeof(client))))
        die("error: could not calloc() %u bytes\n", sizeof(client));

    if (!head) head = c;
    else if (ATTACH_ASIDE) {
        for(t=head; t->next; t=t->next); /* get the last client */
        t->next = c;
    } else {
        c->next = (t = head);
        head = c;
    }

    prevfocus = current;
    unsigned int mask = XCB_CW_EVENT_MASK;
    unsigned int values[1] = { XCB_EVENT_MASK_PROPERTY_CHANGE|(FOLLOW_MOUSE?XCB_EVENT_MASK_ENTER_WINDOW:0) };
    xcb_change_window_attributes_checked(dis, (current=c)->win = w, mask, values);
    return c;
}

/* on the press of a button check to see if there's a binded function to call */
void buttonpress(xcb_generic_event_t *e) {
    xcb_button_press_event_t *ev = (xcb_button_press_event_t*)e;
    DEBUGP("xcb: button press: %d state: %d\n", ev->detail, ev->state);

    client *c = wintoclient(ev->event);
    if (!c) return;
    if (CLICK_TO_FOCUS && current != c && ev->detail == XCB_BUTTON_INDEX_1) update_current(c);

    for (unsigned int i=0; i<LENGTH(buttons); i++)
        if (buttons[i].func && buttons[i].button == ev->detail &&
            CLEANMASK(buttons[i].mask) == CLEANMASK(ev->state)) {
            update_current(c);
            buttons[i].func(&(buttons[i].arg));
        }
}

/* focus another desktop
 * to avoid flickering
 * first map the new windows
 * if the layout mode is fullscreen map only one window
 * then unmap previous windows
 */
void change_desktop(const Arg *arg) {
    if (arg->i == current_desktop) return;
    previous_desktop = current_desktop;
    select_desktop(arg->i);
    tile();
    if (mode == MONOCLE && current) xcb_map_window(dis, current->win);
    else for (client *c=head; c; c=c->next) xcb_map_window(dis, c->win);
    update_current(current);
    select_desktop(previous_desktop);
    for (client *c=head; c; c=c->next) xcb_unmap_window(dis, c->win);
    select_desktop(arg->i);
    desktopinfo();
}

/* remove all windows in all desktops by sending a delete message */
void cleanup(void) {
    xcb_query_tree_reply_t  *reply;
    unsigned int nchildren;

    xcb_ungrab_key(dis, XCB_GRAB_ANY, screen->root, XCB_MOD_MASK_ANY);
    reply  = xcb_query_tree_reply(dis, xcb_query_tree(dis, screen->root), NULL); /* TODO: error handling */
    if (reply) {
        nchildren = reply[0].children_len;
        for (unsigned int i = 0; i<nchildren; i++) sendevent(reply[i].parent, WM_DELETE_WINDOW);
        free(reply);
    }
    xcb_set_input_focus(dis, XCB_NONE, XCB_INPUT_FOCUS_POINTER_ROOT, XCB_CURRENT_TIME);
    xcb_flush(dis);
}

/* move a client to another desktop
 * store the client's window
 * remove the client
 * add the window to the new desktop
 * if defined change focus to the new desktop
 *
 * keep in mind that current pointer might
 * change with each select_desktop() invocation
 */
void client_to_desktop(const Arg *arg) {
    if (arg->i == current_desktop || !current) return;
    xcb_window_t w = current->win;
    int cd = current_desktop;

    xcb_unmap_window(dis, current->win);
    if (current->isfullscreen) setfullscreen(current, false);
    removeclient(current);

    select_desktop(arg->i);
    current = addwindow(w);

    select_desktop(cd);
    tile();
    update_current(current);
    if (FOLLOW_WINDOW) change_desktop(arg);
    desktopinfo();
}

/* check if window requested fullscreen or activation
 * To change the state of a mapped window, a client MUST
 * send a _NET_WM_STATE client message to the root window
 * message_type must be _NET_WM_STATE
 *   data.l[0] is the action to be taken
 *   data.l[1] is the property to alter three actions:
 *     remove/unset _NET_WM_STATE_REMOVE=0
 *     add/set _NET_WM_STATE_ADD=1,
 *     toggle _NET_WM_STATE_TOGGLE=2
 */
void clientmessage(xcb_generic_event_t *e) {
    xcb_client_message_event_t *ev = (xcb_client_message_event_t*)e;
    client *c = wintoclient(ev->window);

    if (ev->format != 32) return;
    DEBUGP("xcb: client message: %d, %d, %d\n", ev->data.data32[0], ev->data.data32[1], ev->data.data32[2]);
    if (c && ev->type == netatoms[NET_WM_STATE] && ((xcb_atom_t)ev->data.data32[1]
        == netatoms[NET_FULLSCREEN] || (xcb_atom_t)ev->data.data32[2] == netatoms[NET_FULLSCREEN]))
        setfullscreen(c, (ev->data.data32[0] == 1 || (ev->data.data32[0] == 2 && !c->isfullscreen)));
    else if (c && ev->type == netatoms[NET_ACTIVE]) current = c;
    tile();
    update_current(current);
}

/* a configure request means that the window requested changes in its geometry
 * state. if the window is fullscreen discard and fill the screen, else set the
 * appropriate values as requested, and tile the window again so that it fills
 * the gaps that otherwise could have been created
 */
void configurerequest(xcb_generic_event_t *e) {
    xcb_configure_request_event_t *ev = (xcb_configure_request_event_t*)e;
    client *c = wintoclient(ev->window);

    if (c && c->isfullscreen)
        xcb_move_resize(dis, c->win, 0, 0, ww + BORDER_WIDTH, wh + BORDER_WIDTH + PANEL_HEIGHT);
    else {
        unsigned int v[7];
        unsigned int i = 0;
        if (ev->value_mask & XCB_CONFIG_WINDOW_X)              v[i++] = ev->x;
        if (ev->value_mask & XCB_CONFIG_WINDOW_Y)              v[i++] = ev->y + (showpanel && TOP_PANEL) ? PANEL_HEIGHT : 0;
        if (ev->value_mask & XCB_CONFIG_WINDOW_WIDTH)          v[i++] = (ev->width  < ww - BORDER_WIDTH) ? ev->width  : ww + BORDER_WIDTH;
        if (ev->value_mask & XCB_CONFIG_WINDOW_HEIGHT)         v[i++] = (ev->height < wh - BORDER_WIDTH) ? ev->height : wh + BORDER_WIDTH;
        if (ev->value_mask & XCB_CONFIG_WINDOW_BORDER_WIDTH)   v[i++] = ev->border_width;
        if (ev->value_mask & XCB_CONFIG_WINDOW_SIBLING)        v[i++] = ev->sibling;
        if (ev->value_mask & XCB_CONFIG_WINDOW_STACK_MODE)     v[i++] = ev->stack_mode;
        xcb_configure_window(dis, ev->window, ev->value_mask, v);
    }
    tile();
}

/* output info about the desktops on standard output stream
 *
 * the info is a list of ':' separated values for each desktop
 * desktop to desktop info is separated by ' ' single spaces
 * the info values are
 *   the desktop number/id
 *   the desktop's client count
 *   the desktop's tiling layout mode/id
 *   whether the desktop is the current focused (1) or not (0)
 *   whether any client in that desktop has received an urgent hint
 *
 * once the info is collected, immediately flush the stream
 */
void desktopinfo(void) {
    bool urgent = false;
    int cd = current_desktop, n=0, d=0;
    for (client *c; d<DESKTOPS; d++) {
        for (select_desktop(d), c=head, n=0, urgent=false; c; c=c->next, ++n) if (c->isurgent) urgent = true;
        fprintf(stdout, "%d:%d:%d:%d:%d%c", d, n, mode, current_desktop == cd, urgent, d+1==DESKTOPS?'\n':' ');
    }
    fflush(stdout);
    select_desktop(cd);
}

/* a destroy notification is received when a window is being closed
 * on receival, remove the appropriate client that held that window
 */
void destroynotify(xcb_generic_event_t *e) {
    DEBUG("xcb: destoroy notify");
    xcb_destroy_notify_event_t *ev = (xcb_destroy_notify_event_t*)e;
    client *c = wintoclient(ev->window);
    if (c) removeclient(c);
    desktopinfo();
}

/* print a message on standard error stream
 * and exit with failure exit code
 */
void die(const char *errstr, ...) {
    va_list ap;
    va_start(ap, errstr);
    vfprintf(stderr, errstr, ap);
    va_end(ap);
    exit(EXIT_FAILURE);
}

/* when the mouse enters a window's borders
 * the window, if notifying of such events (EnterWindowMask)
 * will notify the wm and will get focus
 */
void enternotify(xcb_generic_event_t *e) {
    xcb_enter_notify_event_t *ev = (xcb_enter_notify_event_t*)e;
    if (!FOLLOW_MOUSE) return;
    DEBUG("xcb: enter notify");
    client *c = wintoclient(ev->event);
    if (c && ev->mode == XCB_NOTIFY_MODE_NORMAL && ev->detail != XCB_NOTIFY_DETAIL_INFERIOR) update_current(c);
}

/* find and focus the client which received
 * the urgent hint in the current desktop
 */
void focusurgent() {
    for (client *c=head; c; c=c->next) if (c->isurgent) update_current(c);
}

/* get a pixel with the requested color
 * to fill some window area - borders
 */
unsigned int getcolor(char* color) {
    xcb_colormap_t map = screen->default_colormap;
    xcb_alloc_color_reply_t *c;
    unsigned int r, g, b, rgb, pixel;

    rgb = xcb_get_colorpixel(color);
    r = rgb >> 16; g = rgb >> 8 & 0xFF; b = rgb & 0xFF;
    c = xcb_alloc_color_reply(dis, xcb_alloc_color(dis, map, r * 257, g * 257, b * 257), NULL);
    if (!c)
        die("error: cannot allocate color '%s'\n", c);

    pixel = c->pixel; free(c);
    return pixel;
}

/* set the given client to listen to button events (presses / releases) */
void grabbuttons(client *c) {
    unsigned int modifiers[] = { 0, XCB_MOD_MASK_LOCK, numlockmask, numlockmask|XCB_MOD_MASK_LOCK };
    for (unsigned int b=0; b<LENGTH(buttons); b++)
        for (unsigned int m=0; m<LENGTH(modifiers); m++)
            xcb_grab_button(dis, 1, c->win, XCB_EVENT_MASK_BUTTON_PRESS, XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC,
                    screen->root, XCB_NONE, buttons[b].button, buttons[b].mask|modifiers[m]);
}

/* the wm should listen to key presses */
void grabkeys(void) {
    xcb_keycode_t *keycode;
    unsigned int modifiers[] = { 0, XCB_MOD_MASK_LOCK, numlockmask, numlockmask|XCB_MOD_MASK_LOCK };
    xcb_ungrab_key(dis, XCB_GRAB_ANY, screen->root, XCB_MOD_MASK_ANY);
    for (unsigned int i=0; i<LENGTH(keys); i++) {
        keycode = xcb_get_keycodes(keys[i].keysym);
        for (unsigned int k=0; keycode[k] != XCB_NO_SYMBOL; k++)
            for (unsigned int m=0; m<LENGTH(modifiers); m++)
                xcb_grab_key(dis, 1, screen->root, keys[i].mod | modifiers[m], keycode[k], XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC);
    }
}

/* on the press of a key check to see if there's a binded function to call */
void keypress(xcb_generic_event_t *e) {
    xcb_key_press_event_t *ev       = (xcb_key_press_event_t *)e;
    xcb_keysym_t           keysym   = xcb_get_keysym(ev->detail);

    DEBUGP("xcb: keypress: code: %d mod: %d\n", ev->detail, ev->state);
    for (unsigned int i=0; i<LENGTH(keys); i++)
        if (keysym == keys[i].keysym && CLEANMASK(keys[i].mod) == CLEANMASK(ev->state) && keys[i].func)
                keys[i].func(&keys[i].arg);
}

/* explicitly kill a client - close the highlighted window
 * send a delete message and remove the client
 */
void killclient() {
    if (!current) return;
    sendevent(current->win, WM_DELETE_WINDOW);
    removeclient(current);
}

/* focus the previously focused desktop */
void last_desktop() {
    change_desktop(&(Arg){.i = previous_desktop});
}

/* a map request is received when a window wants to display itself
 * if the window has override_redirect flag set then it should not be handled
 * by the wm. if the window already has a client then there is nothing to do.
 *
 * get the window class and name instance and try to match against an app rule.
 * create a client for the window, that client will always be current.
 * check for transient state, and fullscreen state and the appropriate values.
 * if the desktop in which the window was spawned is the current desktop then
 * display the window, else, if set, focus the new desktop.
 */
void maprequest(xcb_generic_event_t *e) {
    xcb_map_request_event_t            *ev = (xcb_map_request_event_t*)e;
    xcb_window_t                       windows[] = { ev->window }, transient = 0;
    xcb_get_window_attributes_reply_t  *attr[1];
    xcb_icccm_get_wm_class_reply_t     ch;
    xcb_get_geometry_reply_t           *geometry;
    xcb_get_property_reply_t           *prop_reply;
    xcb_atom_t                         *fullscreen_atom;

    xcb_get_attributes(windows, attr, 1);
    if (attr[0]->override_redirect) return;
    if (wintoclient(ev->window))    return;
    DEBUG("xcb: map request");

    bool follow = false, floating = false;
    int cd = current_desktop, newdsk = current_desktop;
    if (xcb_icccm_get_wm_class_reply(dis, xcb_icccm_get_wm_class(dis, ev->window), &ch, NULL)) { /* TODO: error handling */
        DEBUGP("class: %s instance: %s\n", ch.class_name, ch.instance_name);
        for (unsigned int i=0; i<LENGTH(rules); i++)
            if (!strcmp(ch.class_name, rules[i].class) || !strcmp(ch.instance_name, rules[i].class)) {
                follow = rules[i].follow;
                newdsk = rules[i].desktop;
                floating = rules[i].floating;
                break;
            }
        xcb_icccm_get_wm_class_reply_wipe(&ch);
    }

    geometry = xcb_get_geometry_reply(dis, xcb_get_geometry(dis, ev->window), NULL); /* TODO: error handling */
    if (geometry) {
        DEBUGP("geom: %ux%u+%d+%d\n", geometry->width, geometry->height,
                                      geometry->x,     geometry->y);
        free(geometry);
    }

    select_desktop(newdsk);
    addwindow(ev->window);

    xcb_icccm_get_wm_transient_for_reply(dis, xcb_icccm_get_wm_transient_for_unchecked(dis, ev->window), &transient, NULL); /* TODO: error handling */
    current->istransient = transient?true:false;
    current->isfloating  = floating;
    DEBUGP("transient: %d\n", current->istransient);

    prop_reply  = xcb_get_property_reply(dis, xcb_get_property(dis, 0, screen->root, netatoms[NET_WM_STATE], XCB_ATOM, 0L, 1), NULL); /* TODO: error handling */
    if (prop_reply) {
        fullscreen_atom = xcb_get_property_value(prop_reply);
        setfullscreen(current, (fullscreen_atom[0] == netatoms[NET_FULLSCREEN]));
        free(prop_reply);
    }

    select_desktop(cd);
    if (cd == newdsk) {
        tile();
        xcb_map_window(dis, ev->window);
        update_current(current);
        grabbuttons(current);
    } else if (follow) change_desktop(&(Arg){.i = newdsk});
    desktopinfo();
}

/* grab the pointer and get it's current position
 * all pointer movement events will be reported until it's ungrabbed
 * until the mouse button has not been released,
 * grab the interesting events - button press/release and pointer motion
 * and on on pointer movement resize or move the window under the curson.
 * if the received event is a map request or a configure request call the
 * appropriate handler, and stop listening for other events.
 * Ungrab the poitner and event handling is passed back to run() function.
 * Once a window has been moved or resized, it's marked as floating.
 */
void mousemotion(const Arg *arg) {
    if (!current) return;
    xcb_get_geometry_reply_t           *geometry;
    xcb_query_pointer_reply_t          *pointer;
    int mx, my, winx, winy, winw, winh, xw, yh;

    geometry = xcb_get_geometry_reply(dis, xcb_get_geometry(dis, current->win), NULL); /* TODO: error handling */
    if (geometry) {
        winx = geometry->x;     winy = geometry->y;
        winw = geometry->width; winh = geometry->height;
        free(geometry);
    } else return;

    xcb_grab_pointer(dis, 0, current->win, BUTTONMASK|XCB_EVENT_MASK_BUTTON_MOTION|XCB_EVENT_MASK_POINTER_MOTION,
            XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC, screen->root, XCB_NONE, XCB_CURRENT_TIME);

    pointer = xcb_query_pointer_reply(dis, xcb_query_pointer(dis, screen->root), 0);
    if (!pointer) return;
    mx = pointer->root_x; my = pointer->root_y;
    xcb_flush(dis);

    xcb_generic_event_t *e;
    xcb_motion_notify_event_t *ev = NULL;
    do {
        while(!(e = xcb_wait_for_event(dis)));
        switch (e->response_type & ~0x80) {
            case XCB_CONFIGURE_REQUEST:
            case XCB_MAP_REQUEST:
                events[e->response_type & ~0x80](e);
                break;
            case XCB_MOTION_NOTIFY:
                ev = (xcb_motion_notify_event_t*)e;
                xw = (arg->i == MOVE ? winx : winw) + ev->root_x - mx;
                yh = (arg->i == MOVE ? winy : winh) + ev->root_y - my;
                if (arg->i == RESIZE) xcb_resize(dis, current->win, xw>MINWSZ?xw:MINWSZ, yh>MINWSZ?yh:MINWSZ);
                else if (arg->i == MOVE) xcb_move(dis, current->win, xw, yh);
                xcb_flush(dis);
                break;
        }
        current->isfloating = true;
    } while((e->response_type & ~0x80) != XCB_BUTTON_RELEASE);
    DEBUG("ungrab");
    xcb_ungrab_pointer(dis, XCB_CURRENT_TIME);
    tile();
}

/* move the current client, to current->next
 * and current->next to current client's position
 */
void move_down() {
    if (!current || !head->next) return;

    /* p is previous, n is next, if current is head n is last, c is current */
    client *p = NULL, *n = (current->next) ? current->next : head;
    for (p=head; p && p->next != current; p=p->next);
    /* if there's a previous client then p->next should be what's after c
     * ..->[p]->[c]->[n]->..  ==>  ..->[p]->[n]->[c]->..
     */
    if (p) p->next = current->next;
    /* else if no p client, then c is head, swapping with n should update head
     * [c]->[n]->..  ==>  [n]->[c]->..
     *  ^head              ^head
     */
    else head = n;
    /* if c is the last client, c will be the current head
     * [n]->..->[p]->[c]->NULL  ==>  [c]->[n]->..->[p]->NULL
     *  ^head                         ^head
     * else c will take the place of n, so c-next will be n->next
     * ..->[p]->[c]->[n]->..  ==>  ..->[p]->[n]->[c]->..
     */
    current->next = (current->next) ? n->next : n;
    /* if c was swapped with n then they now point to the same ->next. n->next should be c
     * ..->[p]->[c]->[n]->..  ==>  ..->[p]->[n]->..  ==>  ..->[p]->[n]->[c]->..
     *                                        [c]-^
     */
    if (current->next == n->next) n->next = current;
    /* else c is the last client and n is head,
     * so c will be move to be head, no need to update n->next
     * [n]->..->[p]->[c]->NULL  ==>  [c]->[n]->..->[p]->NULL
     *  ^head                         ^head
     */
    else head = current;

    tile();
    update_current(current);
}

/* move the current client, to the previous from current
 * and the previous from  current to current client's position
 */
void move_up() {
    if (!current || !head->next) return;

    client *pp = NULL, *p;
    /* p is previous from current or last if current is head */
    for (p=head; p->next && p->next != current; p=p->next);
    /* pp is previous from p, or null if current is head and thus p is last */
    if (p->next) for (pp=head; pp; pp=pp->next) if (pp->next == p) break;
    /* if p has a previous client then the next client should be current (current is c)
     * ..->[pp]->[p]->[c]->..  ==>  ..->[pp]->[c]->[p]->..
     */
    if (pp) pp->next = current;
    /* if p doesn't have a previous client, then p might be head, so head must change to c
     * [p]->[c]->..  ==>  [c]->[p]->..
     *  ^head              ^head
     * if p is not head, then c is head (and p is last), so the new head is next of c
     * [c]->[n]->..->[p]->NULL  ==>  [n]->..->[p]->[c]->NULL
     *  ^head         ^last           ^head         ^last
     */
    else head = (current == head) ? current->next : current;
    /* next of p should be next of c
     * ..->[pp]->[p]->[c]->[n]->..  ==>  ..->[pp]->[c]->[p]->[n]->..
     * except if c was head (now c->next is head), so next of p should be c
     * [c]->[n]->..->[p]->NULL  ==>  [n]->..->[p]->[c]->NULL
     *  ^head         ^last           ^head         ^last
     */
    p->next = (current->next == head) ? current : current->next;
    /* next of c should be p
     * ..->[pp]->[p]->[c]->[n]->..  ==>  ..->[pp]->[c]->[p]->[n]->..
     * except if c was head (now c->next is head), so c is must be last
     * [c]->[n]->..->[p]->NULL  ==>  [n]->..->[p]->[c]->NULL
     *  ^head         ^last           ^head         ^last
     */
    current->next = (current->next == head) ? NULL : p;

    tile();
    update_current(current);
}

/* cyclic focus the next window
 * if the window is the last on stack, focus head
 */
void next_win() {
    if (!current || !head->next) return;
    update_current((prevfocus = current)->next ? current->next : head);
    if (mode == MONOCLE) xcb_map_window(dis, current->win);
}

/* cyclic focus the previous window
 * if the window is the head, focus the last stack window
 */
void prev_win() {
    if (!current || !head->next) return;
    if (head == (prevfocus = current)) while (current->next) current=current->next;
    else for (client *t=head; t; t=t->next) if (t->next == current) { current = t; break; }
    if (mode == MONOCLE) xcb_map_window(dis, current->win);
    update_current(current);
}

/* property notify is called when one of the window's properties
 * is changed, such as an urgent hint is received
 */
void propertynotify(xcb_generic_event_t *e) {
    xcb_property_notify_event_t *ev = (xcb_property_notify_event_t*)e;
    xcb_icccm_wm_hints_t wmh;
    client *c;

    DEBUG("xcb: property notify");
    c = wintoclient(ev->window);
    if (!c || ev->atom != XCB_ICCCM_WM_ALL_HINTS) return;
    DEBUG("xcb: got hint!");
    if (xcb_icccm_get_wm_hints_reply(dis, xcb_icccm_get_wm_hints(dis, ev->window), &wmh, NULL)) /* TODO: error handling */
        c->isurgent = (wmh.flags & XCB_ICCCM_WM_HINT_X_URGENCY);
    desktopinfo();
}

/* to quit just stop receiving events
 * run() is stopped and control is back to main()
 */
void quit(const Arg *arg) {
    retval = arg->i;
    running = false;
}

/* remove the specified client
 * the previous client must point to the next client of the given
 * the removing client can be on any desktop, so we must return
 * back the current focused desktop
 *
 * keep in mind that the current set and the current update may
 * differ. current pointer changes in every select_desktop()
 * invocation.
 */
void removeclient(client *c) {
    client **p = NULL;
    int nd = 0, cd = current_desktop;
    for (bool found = false; nd<DESKTOPS && !found; nd++)
        for (select_desktop(nd), p = &head; *p && !(found = *p == c); p = &(*p)->next);
    *p = c->next;
    current = (prevfocus && prevfocus != c) ? prevfocus : (*p) ? (prevfocus = *p) : (prevfocus = head);
    select_desktop(cd);
    tile();
    if (mode == MONOCLE && cd == --nd && current) xcb_map_window(dis, current->win);
    update_current(current);
    free(c);
}

/* resize the master window - check for boundary size limits
 * the size of a window can't be less than MINWSZ
 */
void resize_master(const Arg *arg) {
    int msz = master_size + arg->i;
    if ((mode == BSTACK ? wh : ww) - msz <= MINWSZ || msz <= MINWSZ) return;
    master_size = msz;
    tile();
}

/* resize the first stack window - no boundary checks */
void resize_stack(const Arg *arg) {
    growth += arg->i;
    tile();
}

/* jump and focus the 'current + n' desktop */
void rotate_desktop(const Arg *arg) {
    change_desktop(&(Arg){.i = (current_desktop + DESKTOPS + arg->i) % DESKTOPS});
}

/* main event loop - on receival of an event call the appropriate event handler */
void run(void) {
    xcb_generic_event_t *ev;
    while(running) {
        xcb_flush(dis);
        if (xcb_connection_has_error(dis)) die("error: X11 connection got interrupted\n");
        if ((ev = xcb_wait_for_event(dis))) {
            if (events[ev->response_type & ~0x80]) events[ev->response_type & ~0x80](ev);
            else { DEBUGP("xcb: unimplented event: %d\n", ev->response_type & ~0x80); }
            free(ev);
        }
    }
}

/* save specified desktop's properties */
void save_desktop(int i) {
    if (i >= DESKTOPS) return;
    desktops[i].master_size = master_size;
    desktops[i].mode = mode;
    desktops[i].growth = growth;
    desktops[i].head = head;
    desktops[i].current = current;
    desktops[i].showpanel = showpanel;
    desktops[i].prevfocus = prevfocus;
}

/* set the specified desktop's properties */
void select_desktop(int i) {
    if (i >= DESKTOPS) return;
    save_desktop(current_desktop);
    master_size = desktops[i].master_size;
    mode = desktops[i].mode;
    growth = desktops[i].growth;
    head = desktops[i].head;
    current = desktops[i].current;
    showpanel = desktops[i].showpanel;
    prevfocus = desktops[i].prevfocus;
    current_desktop = i;
}

/* send the given event - WM_DELETE_WINDOW for now */
void sendevent(xcb_window_t w, int atom) {
    if (atom >= WM_COUNT) return;
    xcb_client_message_event_t ev;
    ev.response_type = XCB_CLIENT_MESSAGE;
    ev.window = w;
    ev.format = 32;
    ev.sequence = 0;
    ev.type = wmatoms[WM_PROTOCOLS];
    ev.data.data32[0] = wmatoms[atom];
    ev.data.data32[1] = XCB_CURRENT_TIME;
    xcb_send_event(dis, 0, w, XCB_EVENT_MASK_NO_EVENT, (char*)&ev);
}

void setfullscreen(client *c, bool fullscreen) {
    DEBUGP("xcb: set fullscreen: %d\n", fullscreen);
    xcb_change_property(dis, XCB_PROP_MODE_REPLACE, c->win, netatoms[NET_WM_STATE], XCB_ATOM, 32, sizeof(xcb_atom_t),
                       ((c->isfullscreen = fullscreen) ? &netatoms[NET_FULLSCREEN] : &XCB_ATOM_NULL));
    if (c->isfullscreen) xcb_move_resize(dis, c->win, 0, 0, ww + BORDER_WIDTH, wh + BORDER_WIDTH + PANEL_HEIGHT);
}

/* get numlock modifier using xcb */
int setup_keyboard(void)
{
    xcb_get_modifier_mapping_reply_t *reply;
    xcb_keycode_t                    *modmap;
    xcb_keycode_t                    *numlock;

    reply   = xcb_get_modifier_mapping_reply(dis, xcb_get_modifier_mapping_unchecked(dis), NULL); /* TODO: error checking */
    if (!reply) return -1;

    modmap = xcb_get_modifier_mapping_keycodes(reply);
    if (!modmap) return -1;

    numlock = xcb_get_keycodes(XK_Num_Lock);
    for (unsigned int i=0; i<8; i++)
       for (unsigned int j=0; j<reply->keycodes_per_modifier; j++) {
           xcb_keycode_t keycode = modmap[i * reply->keycodes_per_modifier + j];
           if (keycode == XCB_NO_SYMBOL) continue;
           for (unsigned int n=0; numlock[n] != XCB_NO_SYMBOL; n++)
               if (numlock[n] == keycode) {
                   DEBUGP("xcb: found num-lock %d\n", 1 << i);
                   numlockmask = 1 << i;
                   break;
               }
       }

    return 0;
}

/* set initial values
 * root window - screen height/width - atoms - xerror handler
 * set masks for reporting events handled by the wm
 * and propagate the suported net atoms
 */
int setup(int default_screen) {
    sigchld();
    screen = screen_of_display(dis, default_screen);
    if (!screen) die("error: cannot aquire screen\n");

    ww = screen->width_in_pixels  - BORDER_WIDTH;
    wh = screen->height_in_pixels - (SHOW_PANEL ? PANEL_HEIGHT : 0) - BORDER_WIDTH;
    master_size = ((mode == BSTACK) ? wh : ww) * MASTER_SIZE;
    for (unsigned int i=0; i<DESKTOPS; i++) save_desktop(i);
    change_desktop(&(Arg){.i = DEFAULT_DESKTOP});

    win_focus   = getcolor(FOCUS);
    win_unfocus = getcolor(UNFOCUS);

    /* setup keyboard */
    if (setup_keyboard() == -1)
        die("error: failed to setup keyboard\n");

    /* set up atoms for dialog/notification windows */
    xcb_get_atoms(WM_ATOM_NAME, wmatoms, WM_COUNT);
    xcb_get_atoms(NET_ATOM_NAME, netatoms, NET_COUNT);

    /* check if another wm is running */
    if (checkotherwm())
        die("error: other wm is running\n");

    xcb_change_property(dis, XCB_PROP_MODE_REPLACE, screen->root, netatoms[NET_SUPPORTED], XCB_ATOM, 32, sizeof(xcb_atom_t) * NET_COUNT, netatoms);
    grabkeys();

    /* set events */
    for (unsigned int i=0; i<XCB_NO_OPERATION; i++) events[i] = NULL;
    events[XCB_BUTTON_PRESS]        = buttonpress;
    events[XCB_CLIENT_MESSAGE]      = clientmessage;
    events[XCB_CONFIGURE_REQUEST]   = configurerequest;
    events[XCB_DESTROY_NOTIFY]      = destroynotify;
    events[XCB_ENTER_NOTIFY]        = enternotify;
    events[XCB_KEY_PRESS]           = keypress;
    events[XCB_MAP_REQUEST]         = maprequest;
    events[XCB_PROPERTY_NOTIFY]     = propertynotify;
    events[XCB_UNMAP_NOTIFY]        = unmapnotify;

    return 0;
}

void sigchld() {
    if (signal(SIGCHLD, sigchld) == SIG_ERR)
        die("error: can't install SIGCHLD handler\n");
    while(0 < waitpid(-1, NULL, WNOHANG));
}

/* execute a command */
void spawn(const Arg *arg) {
    if (fork() == 0) {
        if (dis) close(screen->root);
        setsid();
        execvp((char*)arg->com[0], (char**)arg->com);
        fprintf(stderr, "error: execvp %s", (char *)arg->com[0]);
        perror(" failed"); /* also prints the err msg */
        exit(EXIT_SUCCESS);
    }
}

/* swap master window with current or
 * if current is head swap with next
 * if current is not head, then head
 * is behind us, so move_up until we
 * are the head
 */
void swap_master() {
    if (!current || !head->next || mode == MONOCLE) return;
    for (client *t=head; t; t=t->next) if (t->isfullscreen) return;
    if (current == head) move_down();
    else while (current != head) move_up();
    update_current(head);
    tile();
}

/* switch the tiling mode and reset all floating windows */
void switch_mode(const Arg *arg) {
    if (mode == MONOCLE) for (client *c=head; c; c=c->next) xcb_map_window(dis, c->win);
    for (client *c=head; c; c=c->next) c->isfloating = False;
    mode = arg->i;
    master_size = (mode == BSTACK ? wh : ww) * MASTER_SIZE;
    tile();
    update_current(current);
    desktopinfo();
}

/* tile all windows of current desktop to the set tiling mode */
void tile(void) {
    if (!head) return; /* nothing to arange */

    client *c;
    /* n:number of windows, d:difference, h:available height, z:client height */
    int n = 0, d = 0, h = wh + (showpanel ? 0 : PANEL_HEIGHT), z = mode == BSTACK ? ww : h;
    /* client's x,y coordinates, width and height */
    int cx = 0, cy = (TOP_PANEL && showpanel ? PANEL_HEIGHT : 0), cw = 0, ch = 0;

    /* count stack windows -- do not consider fullscreen or transient clients */
    for (n=0, c=head->next; c; c=c->next) if (!c->istransient && !c->isfullscreen && !c->isfloating) ++n;

    if (!head->next || (head->next->istransient && !head->next->next) || mode == MONOCLE) {
        for (c=head; c; c=c->next) if (!c->isfullscreen && !c->istransient && !c->isfloating)
            xcb_move_resize(dis, c->win, cx, cy, ww + BORDER_WIDTH, h + BORDER_WIDTH);
    } else if (mode == TILE || mode == BSTACK) {
        d = (z - growth)%n + growth;       /* n should be greater than one */
        z = (z - growth)/n;         /* adjust to match screen height/width */
        if (!head->isfullscreen && !head->istransient && !head->isfloating)
            (mode == BSTACK) ? xcb_move_resize(dis, head->win, cx, cy, ww - BORDER_WIDTH, master_size - BORDER_WIDTH)
                             : xcb_move_resize(dis, head->win, cx, cy, master_size - BORDER_WIDTH,  h - BORDER_WIDTH);
        for (c=head->next; c && (c->isfullscreen || c->istransient || c->isfloating); c=c->next);
        if (c) (mode == BSTACK) ? xcb_move_resize(dis, c->win, cx, (cy += master_size),
                                (cw = z - BORDER_WIDTH) + d, (ch = h - master_size - BORDER_WIDTH))
                                : xcb_move_resize(dis, c->win, (cx += master_size), cy,
                                (cw = ww - master_size - BORDER_WIDTH), (ch = z - BORDER_WIDTH) + d);
        if (c) for (mode==BSTACK?(cx+=z+d):(cy+=z+d), c=c->next; c; c=c->next)
            if (!c->isfullscreen && !c->istransient && !c->isfloating) {
                xcb_move_resize(dis, c->win, cx, cy, cw, ch);
                (mode == BSTACK) ? (cx+=z) : (cy+=z);
            }
    } else if (mode == GRID) {
        ++n;                              /* include head on window count */
        int cols, rows, cn=0, rn=0, i=0;  /* columns, rows, and current column and row number */
        for (cols=0; cols <= n/2; cols++) if (cols*cols >= n) break;   /* emulate square root */
        if (n == 5) cols = 2;
        rows = n/cols;
        cw = cols ? ww/cols : ww;
        for (i=0, c=head; c; c=c->next, i++) {
            if (i/rows + 1 > cols - n%cols) rows = n/cols + 1;
            ch = h/rows;
            cx = cn*cw;
            cy = (TOP_PANEL && showpanel ? PANEL_HEIGHT : 0) + rn*ch;
            if (!c->isfullscreen && !c->istransient && !c->isfloating)
                xcb_move_resize(dis, c->win, cx, cy, cw - BORDER_WIDTH, ch - BORDER_WIDTH);
            if (++rn >= rows) { rn = 0; cn++; }
        }
    } else fprintf(stderr, "error: no such layout mode: %d\n", mode);
    free(c);
}

/* toggle visibility state of the panel */
void togglepanel() {
    showpanel = !showpanel;
    tile();
}

/* windows that request to unmap should lose their
 * client, so no invisible windows exist on screen
 */
void unmapnotify(xcb_generic_event_t *e) {
    xcb_unmap_notify_event_t *ev = (xcb_unmap_notify_event_t *)e;
    client *c = wintoclient(ev->window);
    if (c && ev->event != screen->root) removeclient(c);
    desktopinfo();
}

/* update client - set highlighted borders and active window
 * if no client is given update current
 * if current is NULL then delete the active window property
 */
void update_current(client *c) {
    if (!c) {
        xcb_delete_property(dis, screen->root, netatoms[NET_ACTIVE]);
        return;
    } else current = c;

    int border_width = (!head->next || (head->next->istransient &&
                        !head->next->next) || mode == MONOCLE) ? 0 : BORDER_WIDTH;

    for (c=head; c; c=c->next) {
        xcb_border_width(dis, c->win, (c->isfullscreen ? 0 : border_width));
        xcb_change_window_attributes(dis, c->win, XCB_CW_BORDER_PIXEL, (current == c ? &win_focus : &win_unfocus));
        if (CLICK_TO_FOCUS) xcb_grab_button(dis, 1, c->win, XCB_EVENT_MASK_BUTTON_PRESS, XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC,
           screen->root, XCB_NONE, XCB_BUTTON_INDEX_1, XCB_BUTTON_MASK_ANY);
    }

    xcb_change_property(dis, XCB_PROP_MODE_REPLACE, screen->root, netatoms[NET_ACTIVE], XCB_ATOM_WINDOW, 32, sizeof(xcb_window_t), &current->win);
    xcb_set_input_focus(dis, XCB_INPUT_FOCUS_POINTER_ROOT, current->win, XCB_CURRENT_TIME);
    xcb_raise_window(dis, current->win);

    if (CLICK_TO_FOCUS) {
        xcb_ungrab_button(dis, XCB_BUTTON_INDEX_1, current->win, XCB_BUTTON_MASK_ANY);
        grabbuttons(current);
    }
}

/* find to which client the given window belongs to */
client* wintoclient(xcb_window_t w) {
    client *c = NULL;
    int d = 0, cd = current_desktop;
    for (bool found = false; d<DESKTOPS && !found; ++d)
        for (select_desktop(d), c=head; c && !(found = (w == c->win)); c=c->next);
    select_desktop(cd);
    return c;
}

int xerrorstart() {
    die("error: another window manager is already running\n");
    return -1;
}

int main(int argc, char *argv[]) {
    int default_screen;
    if (argc == 2 && strcmp("-v", argv[1]) == 0) {
        fprintf(stdout, "%s-%s\n", WMNAME, VERSION);
        return EXIT_SUCCESS;
    } else if (argc != 1) die("usage: %s [-v]\n", WMNAME);
    if (xcb_connection_has_error((dis = xcb_connect(NULL, &default_screen))))
        die("error: cannot open display\n");
    if (setup(default_screen) != -1) {
      desktopinfo(); /* zero out every desktop on (re)start */
      run();
    }
    cleanup();
    xcb_disconnect(dis);
    return retval;
}

/* vim: set ts=4 sw=4 :*/
