/* see license for copyright and license */

#include <stdlib.h>
#include <stdio.h>
#include <err.h>
#include <stdarg.h>
#include <stdbool.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <regex.h>
#include <sys/wait.h>
#include <X11/keysym.h>
#include <xcb/xcb.h>
#include <xcb/xcb_atom.h>
#include <xcb/xcb_icccm.h>
#include <xcb/xcb_keysyms.h>
#include <xcb/xcb_ewmh.h>

/* set this to 1 to enable debug prints */
#if 0
#  define DEBUG(x)      puts(x);
#  define DEBUGP(x, ...) printf(x, ##__VA_ARGS__);
#else
#  define DEBUG(x);
#  define DEBUGP(x, ...);
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

static char *WM_ATOM_NAME[]   = { "WM_PROTOCOLS", "WM_DELETE_WINDOW" };
enum { WM_PROTOCOLS, WM_DELETE_WINDOW, WM_COUNT };

static char *NET_ATOM_NAME[]  = { "_NET_SUPPORTED",
                                  "_NET_WM_STATE_FULLSCREEN",
                                  "_NET_WM_STATE",
                                  "_NET_SUPPORTING_WM_CHECK",
                                  "_NET_ACTIVE_WINDOW",
                                  "_NET_NUMBER_OF_DESKTOPS",
                                  "_NET_CURRENT_DESKTOP",
                                  "_NET_DESKTOP_GEOMETRY",
                                  "_NET_DESKTOP_VIEWPORT",
                                  "_NET_WORKAREA",
                                  "_NET_SHOWING_DESKTOP",
                                  "_NET_CLOSE_WINDOW",
                                  "_NET_WM_DESKTOP",
                                  "_NET_WM_WINDOW_TYPE" };
enum { NET_SUPPORTED,
       NET_FULLSCREEN,
       NET_WM_STATE,
       NET_SUPPORTING_WM_CHECK,
       NET_ACTIVE,
       NET_NUMBER_OF_DESKTOPS,
       NET_CURRENT_DESKTOP,
       NET_DESKTOP_GEOMETRY,
       NET_DESKTOP_VIEWPORT,
       NET_WORKAREA,
       NET_SHOWING_DESKTOP,
       NET_CLOSE_WINDOW,
       NET_WM_WINDOW_TYPE,
       NET_WM_DESKTOP,
       NET_COUNT };

#define LENGTH(x) (sizeof(x)/sizeof(*x))
#define CLEANMASK(mask) (mask & ~(numlockmask | XCB_MOD_MASK_LOCK))
#define BUTTONMASK      XCB_EVENT_MASK_BUTTON_PRESS|XCB_EVENT_MASK_BUTTON_RELEASE
#define ISFFTM(c)        (c->isfullscrn || c->isfloating || c->istransient || c->isminimized)
#define USAGE           "usage: frankenwm [-h] [-v]"

enum { RESIZE, MOVE };
enum { TILE, MONOCLE, BSTACK, GRID, FIBONACCI, DUALSTACK, EQUAL, MODES };

/* argument structure to be passed to function by config.h
 * com  - a command to run
 * i    - an integer to indicate different states
 */
typedef union {
    const char **com;
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
    unsigned int mask, button;
    void (*func)(const Arg *);
    const Arg arg;
} Button;

/* a client is a wrapper to a window that additionally
 * holds some properties for that window
 *
 * next        - the client after this one, or NULL if the current is the last
 *               client
 * isurgent    - set when the window received an urgent hint
 * istransient - set when the window is transient
 * isfullscrn  - set when the window is fullscreen
 * isfloating  - set when the window is floating
 * win         - the window this client is representing
 *
 * istransient is separate from isfloating as floating window can be reset
 * to their tiling positions, while the transients will always be floating
 */
typedef struct client {
    struct client *next;
    bool isurgent, istransient, isfullscrn, isfloating, isminimized;
    xcb_window_t win;
    unsigned int dim[2];
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
    int mode, growth, gaps;
    float master_size;
    client *head, *current, *prevfocus;
    bool showpanel, invert;
} desktop;

/* filo for minimized clients */
typedef struct filo {
    client *c;
    struct filo *next;
} filo;

/* define behavior of certain applications
 * configured in config.h
 * class    - the class or name of the instance
 * desktop  - what desktop it should be spawned at
 * follow   - whether to change desktop focus to the specified desktop
 */
typedef struct {
    const char *class;
    const int desktop;
    const bool follow, floating;
} AppRule;

 /* function prototypes sorted alphabetically */
static client *addwindow(xcb_window_t w);
static void adjust_borders(const Arg *arg);
static void adjust_gaps(const Arg *arg);
static void buttonpress(xcb_generic_event_t *e);
static void change_desktop(const Arg *arg);
static void centerwindow();
static void cleanup(void);
static void client_to_desktop(const Arg *arg);
static void clientmessage(xcb_generic_event_t *e);
static void configurerequest(xcb_generic_event_t *e);
static void deletewindow(xcb_window_t w);
static void desktopinfo(void);
static void destroynotify(xcb_generic_event_t *e);
static void dualstack(int hh, int cy);
static void enternotify(xcb_generic_event_t *e);
static void equal(int h, int y);
static void fibonacci(int h, int y);
static void float_client(client *c);
static void float_x(const Arg *arg);
static void float_y(const Arg *arg);
static void focusmaster();
static void focusurgent();
static unsigned int getcolor(char *color);
static void grabbuttons(client *c);
static void grabkeys(void);
static void grid(int h, int y);
static void invertstack();
static void keypress(xcb_generic_event_t *e);
static void killclient();
static void last_desktop();
static void maprequest(xcb_generic_event_t *e);
static void maximize();
static void minimize();
static void monocle(int h, int y);
static void move_down();
static void move_up();
static void mousemotion(const Arg *arg);
static void next_win();
static client *prev_client();
static void prev_win();
static void propertynotify(xcb_generic_event_t *e);
static void quit(const Arg *arg);
static void removeclient(client *c);
static void resize_master(const Arg *arg);
static void resize_stack(const Arg *arg);
static void resize_x(const Arg *arg);
static void resize_y(const Arg *arg);
static void restore();
static void rotate(const Arg *arg);
static void rotate_client(const Arg *arg);
static void rotate_filled(const Arg *arg);
static void rotate_mode(const Arg *arg);
static void run(void);
static void save_desktop(int i);
static void select_desktop(int i);
static void setfullscreen(client *c, bool fullscrn);
static int setup(int default_screen);
static void showhide();
static void sigchld();
static void spawn(const Arg *arg);
static void stack(int h, int y);
static void swap_master();
static void switch_mode(const Arg *arg);
static void tile(void);
static void tilemize();
static void togglepanel();
static void unfloat_client(client *c);
static void togglescratchpad();
static void update_current(client *c);
static void unmapnotify(xcb_generic_event_t *e);
static client *wintoclient(xcb_window_t w);

#include "config.h"

/* variables */
static bool running = true, showpanel = SHOW_PANEL, show = true,
            invert = INVERT, showscratchpad = false;
static int default_screen, previous_desktop, current_desktop, retval;
static int wh, ww, mode = DEFAULT_MODE, master_size, growth, borders, gaps;
static unsigned int numlockmask, win_unfocus, win_focus;
static xcb_connection_t *dis;
static xcb_screen_t *screen;
static client *head = NULL, *prevfocus = NULL, *current = NULL, *scrpd = NULL;

static xcb_ewmh_connection_t *ewmh;
static xcb_atom_t wmatoms[WM_COUNT], netatoms[NET_COUNT];
static desktop desktops[DESKTOPS];
static filo *miniq[DESKTOPS];
static regex_t appruleregex[LENGTH(rules)];

/* events array
 * on receival of a new event, call the appropriate function to handle it
 */
static void (*events[XCB_NO_OPERATION])(xcb_generic_event_t *e);

/* layout array - given the current layout mode, tile the windows
 * h (or hh) - avaible height that windows have to expand
 * y (or cy) - offset from top to place the windows (reserved by the panel) */
static void (*layout[MODES])(int h, int y) = {
    [TILE] = stack,
    [BSTACK] = stack,
    [GRID] = grid,
    [MONOCLE] = monocle,
    [FIBONACCI] = fibonacci,
    [DUALSTACK] = dualstack,
    [EQUAL] = equal,
};

/* get screen of display */
static xcb_screen_t *xcb_screen_of_display(xcb_connection_t *con, int screen)
{
    xcb_screen_iterator_t iter;

    iter = xcb_setup_roots_iterator(xcb_get_setup(con));
    for (; iter.rem; --screen, xcb_screen_next(&iter))
        if (screen == 0)
            return iter.data;

    return NULL;
}

/* wrapper to move and resize window */
static inline void xcb_move_resize(xcb_connection_t *con, xcb_window_t win,
                                   int x, int y, int w, int h)
{
    unsigned int pos[4] = { x, y, w, h };

    xcb_configure_window(con, win, XCB_MOVE_RESIZE, pos);
}

/* wrapper to move window */
static inline void xcb_move(xcb_connection_t *con, xcb_window_t win, int x,
                            int y)
{
    unsigned int pos[2] = { x, y };

    xcb_configure_window(con, win, XCB_MOVE, pos);
}

/* wrapper to resize window */
static inline void xcb_resize(xcb_connection_t *con, xcb_window_t win, int w,
                              int h)
{
    unsigned int pos[2] = { w, h };

    xcb_configure_window(con, win, XCB_RESIZE, pos);
}

/* wrapper to raise window */
static inline void xcb_raise_window(xcb_connection_t *con, xcb_window_t win)
{
    unsigned int arg[1] = { XCB_STACK_MODE_ABOVE };

    xcb_configure_window(con, win, XCB_CONFIG_WINDOW_STACK_MODE, arg);
}

/* wrapper to set xcb border width */
static inline void xcb_border_width(xcb_connection_t *con, xcb_window_t win,
                                    int w)
{
    unsigned int arg[1] = { w };

    xcb_configure_window(con, win, XCB_CONFIG_WINDOW_BORDER_WIDTH, arg);
}

/* wrapper to get xcb keysymbol from keycode */
static xcb_keysym_t xcb_get_keysym(xcb_keycode_t keycode)
{
    xcb_key_symbols_t *keysyms;
    xcb_keysym_t       keysym;

    if (!(keysyms = xcb_key_symbols_alloc(dis)))
        return 0;
    keysym = xcb_key_symbols_get_keysym(keysyms, keycode, 0);
    xcb_key_symbols_free(keysyms);

    return keysym;
}

/* wrapper to get xcb keycodes from keysymbol (caller must free) */
static xcb_keycode_t *xcb_get_keycodes(xcb_keysym_t keysym)
{
    xcb_key_symbols_t *keysyms;
    xcb_keycode_t     *keycode;

    if (!(keysyms = xcb_key_symbols_alloc(dis)))
        return NULL;
    keycode = xcb_key_symbols_get_keycode(keysyms, keysym);
    xcb_key_symbols_free(keysyms);

    return keycode;
}

/* retieve RGB color from hex (think of html) */
static unsigned int xcb_get_colorpixel(char *hex)
{
    char strgroups[3][3]  = {{hex[1], hex[2], '\0'},
                             {hex[3], hex[4], '\0'},
                             {hex[5], hex[6], '\0'}};
    unsigned int rgb16[3] = {(strtol(strgroups[0], NULL, 16)),
                             (strtol(strgroups[1], NULL, 16)),
                             (strtol(strgroups[2], NULL, 16))};

    return (rgb16[0] << 16) + (rgb16[1] << 8) + rgb16[2];
}

/* wrapper to get atoms using xcb */
static void xcb_get_atoms(char **names, xcb_atom_t *atoms, unsigned int count)
{
    xcb_intern_atom_cookie_t cookies[count];
    xcb_intern_atom_reply_t  *reply;

    for (unsigned int i = 0; i < count; i++)
        cookies[i] = xcb_intern_atom(dis, 0, strlen(names[i]), names[i]);

    for (unsigned int i = 0; i < count; i++) {
        reply = xcb_intern_atom_reply(dis, cookies[i], NULL);
        /* TODO: Handle error */
        if (reply) {
            DEBUGP("%s : %d\n", names[i], reply->atom);
            atoms[i] = reply->atom; free(reply);
        } else {
            puts("WARN: frankenwm failed to register %s atom.\nThings might not work right.");
        }
    }
}

/* wrapper to window get attributes using xcb */
static void xcb_get_attributes(xcb_window_t *windows,
                               xcb_get_window_attributes_reply_t **reply,
                               unsigned int count)
{
    xcb_get_window_attributes_cookie_t cookies[count];

    for (unsigned int i = 0; i < count; i++)
        cookies[i] = xcb_get_window_attributes(dis, windows[i]);
    for (unsigned int i = 0; i < count; i++)
        reply[i] = xcb_get_window_attributes_reply(dis, cookies[i], NULL);
        /* TODO: Handle error */
}

/* check if other wm exists */
static int xcb_checkotherwm(void)
{
    xcb_generic_error_t *error;
    unsigned int values[1] = {XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT|
                              XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY|
                              XCB_EVENT_MASK_PROPERTY_CHANGE|
                              XCB_EVENT_MASK_BUTTON_PRESS};

    error = xcb_request_check(dis, xcb_change_window_attributes_checked(dis,
                                    screen->root, XCB_CW_EVENT_MASK, values));
    xcb_flush(dis);
    if (error)
        return 1;
    return 0;
}

/* create a new client and add the new window
 * window should notify of property change events
 */
client *addwindow(xcb_window_t w)
{
    client *c, *t = prev_client(head);

    if (!(c = (client *)calloc(1, sizeof(client))))
         err(EXIT_FAILURE, "cannot allocate client");

    if (!head) {
        head = c;
    } else if (!ATTACH_ASIDE) {
        c->next = head; head = c;
    } else if (t) {
        t->next = c;
    } else {
        head->next = c;
    }

    unsigned int values[1] = {XCB_EVENT_MASK_PROPERTY_CHANGE|
                              (FOLLOW_MOUSE ? XCB_EVENT_MASK_ENTER_WINDOW : 0)};
    xcb_change_window_attributes_checked(dis, (c->win = w), XCB_CW_EVENT_MASK,
                                         values);
    xcb_ewmh_set_wm_desktop(ewmh, w, current_desktop);

    return c;
}

/* change the size of the window borders */
void adjust_borders(const Arg *arg)
{
    if (arg->i > 0 || borders >= -arg->i)
        borders += arg->i;
    tile();
    update_current(current);
}

/* change the size of the useless gaps on the fly and re-tile */
void adjust_gaps(const Arg *arg)
{
    if (arg->i > 0 || gaps >= -arg->i)
        gaps += arg->i;

    if (GLOBALGAPS)
        for (int i = 0; i < DESKTOPS; i++)
            desktops[i].gaps = gaps;
    else
        desktops[current_desktop].gaps = gaps;
    tile();
}

/* on the press of a button check to see if there's a binded function to call */
void buttonpress(xcb_generic_event_t *e)
{
    xcb_button_press_event_t *ev = (xcb_button_press_event_t *)e;
    DEBUGP("xcb: button press: %d state: %d\n", ev->detail, ev->state);

    client *c = wintoclient(ev->event);
    if (!c)
        return;
    if (CLICK_TO_FOCUS && current != c && ev->detail == XCB_BUTTON_INDEX_1)
        update_current(c);

    for (unsigned int i = 0; i < LENGTH(buttons); i++)
        if (buttons[i].func && buttons[i].button == ev->detail &&
            CLEANMASK(buttons[i].mask) == CLEANMASK(ev->state)) {
            if (current != c)
                update_current(c);
            buttons[i].func(&(buttons[i].arg));
        }

    if (CLICK_TO_FOCUS) {
        xcb_allow_events(dis, XCB_ALLOW_REPLAY_POINTER, ev->time);
        xcb_flush(dis);
    }
}

/* focus another desktop
 *
 * to avoid flickering
 * first map the new windows
 * first the current window and then all other
 * then unmap the old windows
 * first all others then the current */
void change_desktop(const Arg *arg)
{
    if (arg->i == current_desktop)
        return;
    previous_desktop = current_desktop;
    select_desktop(arg->i);
    if (current && show)
        xcb_map_window(dis, current->win);
    for (client *c = head; c && show; c = c->next)
        xcb_map_window(dis, c->win);
    select_desktop(previous_desktop);
    for (client *c = head; c; c = c->next)
        if (c != current)
            xcb_unmap_window(dis, c->win);
    if (current)
        xcb_unmap_window(dis, current->win);
    select_desktop(arg->i);
    tile();
    update_current(current);
    desktopinfo();
    xcb_ewmh_set_current_desktop(ewmh, default_screen, arg->i);

    if (USE_SCRATCHPAD && scrpd && showscratchpad) {
        xcb_map_window(dis, scrpd->win);
        update_current(scrpd);
        xcb_raise_window(dis, scrpd->win);
    }
}

/*
 * place the current window in the center of the screen floating
 */
void centerwindow(void)
{
    xcb_get_geometry_reply_t *wa;
    desktop *d = &desktops[current_desktop];

    if (!d->current)
        return;

    if (!d->current->isfloating && !d->current->istransient) {
        float_client(d->current);
        tile();
    }

    wa = xcb_get_geometry_reply(dis, xcb_get_geometry(dis, current->win), NULL);
    if (!wa)
        /* TODO this is not particularly nice if we fail */
        return;

    xcb_raise_window(dis, d->current->win);
    xcb_move(dis, d->current->win, (ww - wa->width) / 2, (wh - wa->height) / 2);
    free(wa);
}

/* remove all windows in all desktops by sending a delete message */
void cleanup(void)
{
    xcb_query_tree_reply_t *query;
    xcb_window_t *c;

    xcb_ungrab_key(dis, XCB_GRAB_ANY, screen->root, XCB_MOD_MASK_ANY);
    if ((query = xcb_query_tree_reply(dis,
                                      xcb_query_tree(dis, screen->root), 0))) {
        c = xcb_query_tree_children(query);
        for (unsigned int i = 0; i != query->children_len; ++i)
            deletewindow(c[i]);
        free(query);
    }
    xcb_set_input_focus(dis, XCB_INPUT_FOCUS_POINTER_ROOT, screen->root,
                        XCB_CURRENT_TIME);
    xcb_ewmh_connection_wipe(ewmh);
    if (ewmh)
        free(ewmh);

    for (unsigned int i = 0; i < LENGTH(rules); i++)
        regfree(&appruleregex[i]);

    for (unsigned int i = 0; i < DESKTOPS; i++) {
        for (struct filo *tmp = miniq[i], *tmp_next; tmp; tmp = tmp_next) {
            tmp_next = tmp->next;
            free(tmp);
        }

        for (client *c = desktops[i].head, *c_next; c; c = c_next) {
            c_next = c->next;
            free(c);
        }
    }
}

/* move a client to another desktop
 *
 * remove the current client from the current desktop's client list
 * and add it as last client of the new desktop's client list */
void client_to_desktop(const Arg *arg)
{
    if (!current || arg->i == current_desktop)
        return;
    int cd = current_desktop;
    client *p = prev_client(current), *c = current;

    select_desktop(arg->i);
    client *l = prev_client(head);
    update_current(l ? (l->next = c) : head ? (head->next = c) : (head = c));

    select_desktop(cd);
    if (c == head || !p)
        head = c->next;
    else
        p->next = c->next;
    c->next = NULL;
    xcb_unmap_window(dis, c->win);
    update_current(prevfocus);
    xcb_ewmh_set_wm_desktop(ewmh, c->win, arg->i);

    if (FOLLOW_WINDOW)
        change_desktop(arg);
    else
        tile();
    desktopinfo();
}

/*
 * Here we take and process client messages. Currently supported messages are:
 * _NET_WM_STATE
 * _NET_CURRENT_DESKTOP
 * _NET_ACTIVE_WINDOW
 * _NET_CLOSE_WINDOW
 *
 * data.data32[0] is the action to be taken
 * data.data32[1] is the property to alter three actions:
 *   remove/unset _NET_WM_STATE_REMOVE=0
 *   add/set _NET_WM_STATE_ADD=1
 *   toggle _NET_WM_STATE_TOGGLE=2
 */
void clientmessage(xcb_generic_event_t *e)
{
    xcb_client_message_event_t *ev = (xcb_client_message_event_t *)e;
    client *t = NULL, *c = wintoclient(ev->window);

    if (c && ev->type == netatoms[NET_WM_STATE]
          && ((unsigned)ev->data.data32[1] == netatoms[NET_FULLSCREEN]
           || (unsigned)ev->data.data32[2] == netatoms[NET_FULLSCREEN]))
        setfullscreen(c, (ev->data.data32[0] == 1 ||
                         (ev->data.data32[0] == 2 &&
                         !c->isfullscrn)));
    else if (c && ev->type == netatoms[NET_CURRENT_DESKTOP]
             && ev->data.data32[0] < DESKTOPS)
        change_desktop(&(Arg){.i = ev->data.data32[0]});
    else if (c && ev->type == netatoms[NET_CLOSE_WINDOW])
        removeclient(c);
    else if (c && ev->type == netatoms[NET_ACTIVE])
        for (t = head; t && t != c; t = t->next);
    else if (c && ev->type == netatoms[NET_WM_DESKTOP]
             && ev->data.data32[0] < DESKTOPS)
        client_to_desktop(&(Arg){.i = ev->data.data32[0]});
    if (t)
        update_current(c);
    tile();
}

/* a configure request means that the window requested changes in its geometry
 * state. if the window is fullscreen discard and fill the screen else set the
 * appropriate values as requested, and tile the window again so that it fills
 * the gaps that otherwise could have been created
 */
void configurerequest(xcb_generic_event_t *e)
{
    xcb_configure_request_event_t *ev = (xcb_configure_request_event_t *)e;
    client *c = wintoclient(ev->window);

    if (c && c->isfullscrn) {
        setfullscreen(c, true);
    } else {
        unsigned int v[7];
        unsigned int i = 0;
        if (ev->value_mask & XCB_CONFIG_WINDOW_X)
            v[i++] = ev->x;
        if (ev->value_mask & XCB_CONFIG_WINDOW_Y)
            v[i++] = (ev->y + (showpanel && TOP_PANEL)) ? PANEL_HEIGHT : 0;
        if (ev->value_mask & XCB_CONFIG_WINDOW_WIDTH)
            v[i++] = (ev->width < ww - borders) ? ev->width : ww + borders;
        if (ev->value_mask & XCB_CONFIG_WINDOW_HEIGHT)
            v[i++] = (ev->height < wh - borders) ? ev->height : wh + borders;
        if (ev->value_mask & XCB_CONFIG_WINDOW_BORDER_WIDTH)
            v[i++] = ev->border_width;
        if (ev->value_mask & XCB_CONFIG_WINDOW_SIBLING)
            v[i++] = ev->sibling;
        if (ev->value_mask & XCB_CONFIG_WINDOW_STACK_MODE)
            v[i++] = ev->stack_mode;
        xcb_configure_window(dis, ev->window, ev->value_mask, v);
    }
    tile();
}

/* close the window */
void deletewindow(xcb_window_t w)
{
    xcb_client_message_event_t ev = {0};

    ev.response_type = XCB_CLIENT_MESSAGE;
    ev.window = w;
    ev.format = 32;
    ev.sequence = 0;
    ev.type = wmatoms[WM_PROTOCOLS];
    ev.data.data32[0] = wmatoms[WM_DELETE_WINDOW];
    ev.data.data32[1] = XCB_CURRENT_TIME;
    xcb_send_event(dis, 0, w, XCB_EVENT_MASK_NO_EVENT, (char *)&ev);
}

/*
 * output info about the desktops on standard output stream
 *
 * the info is a list of ':' separated values for each desktop
 * desktop to desktop info is separated by ' ' single spaces
 * the info values are
 *   the desktop number/id
 *   the desktop's client count
 *   the desktop's tiling layout mode/id
 *   whether the desktop is the current focused (1) or not (0)
 *   whether any client in that desktop has received an urgent hint
 *   and the current window's title
 *
 * once the info is collected, immediately flush the stream
 */
void desktopinfo(void)
{
    bool urgent = false;
    int cd = current_desktop, n = 0, d = 0;
    xcb_get_property_cookie_t cookie;
    xcb_ewmh_get_utf8_strings_reply_t wtitle;
    wtitle.strings = NULL;

    if (current) {
        cookie = xcb_ewmh_get_wm_name_unchecked(ewmh, current->win);
        xcb_ewmh_get_wm_name_reply(ewmh, cookie, &wtitle, (void *)0);
    }

    for (client *c; d < DESKTOPS; d++) {
        for (select_desktop(d), c = head, n = 0, urgent = false;
             c; c = c->next, ++n)
            if (c->isurgent)
                urgent = true;
        fprintf(stdout, "%d:%d:%d:%d:%d ", d, n, mode, current_desktop == cd,
                urgent);
        if (d + 1 == DESKTOPS)
            fprintf(stdout, "%s\n", current && OUTPUT_TITLE && wtitle.strings ?
                    wtitle.strings : "");
    }

    if (wtitle.strings) {
        xcb_ewmh_get_utf8_strings_reply_wipe(&wtitle);
    }

    fflush(stdout);
    if (cd != d - 1)
        select_desktop(cd);
}

/*
 * a destroy notification is received when a window is being closed
 * on receival, remove the appropriate client that held that window
 */
void destroynotify(xcb_generic_event_t *e)
{
    DEBUG("xcb: destroy notify");
    xcb_destroy_notify_event_t *ev = (xcb_destroy_notify_event_t *)e;
    client *c = wintoclient(ev->window);

    if (c) {
        removeclient(c);
    } else if (USE_SCRATCHPAD && scrpd && ev->window == scrpd->win) {
        free(scrpd);
        scrpd = NULL;
        update_current(head);
    }
    desktopinfo();
}

/* dualstack layout (three-column-layout, tcl in dwm) */
void dualstack(int hh, int cy)
{
    client *c = NULL, *t = NULL;
    int n = 0, z = hh, d = 0, l = 0, r = 0, cb = cy,
        ma = (invert ? wh : ww) * MASTER_SIZE + master_size;

    /* count stack windows and grab first non-floating, non-fullscreen window */
    for (t = head; t; t = t->next) {
        if (!ISFFTM(t)) {
            if (c)
                ++n;
            else
                c = t;
        }
    }

    l = (n - 1) / 2 + 1; /* left stack size */
    r = n - l;          /* right stack size */

    if (!c) {
        return;
    } else if (!n) {
        xcb_move_resize(dis, c->win, gaps, cy + gaps,
                        ww - 2 * (borders + gaps),
                        hh - 2 * (borders + gaps));
        return;
    }

    /* tile the first non-floating, non-fullscreen window to cover the master area */
    if (invert)
        xcb_move_resize(dis, c->win, gaps,
                        cy + (hh - ma) / 2 + gaps,
                        ww - 2 * (borders + gaps),
                        n > 1 ? ma - 2 * gaps - 2 * borders
                              : ma + (hh - ma) / 2 - 2 * borders - 2 * gaps);
    else
        xcb_move_resize(dis, c->win, (ww - ma) / 2 + borders + gaps,
                        cy + gaps,
                        n > 1 ? ma - 4 * borders - 2 * gaps
                              : ma + (ww - ma) / 2 - 3 * borders - 2 * gaps,
                        hh - 2 * (borders + gaps));

    int cx = gaps,
        cw = (ww - ma) / 2 - borders - gaps,
        ch = z;
        cy += gaps;

    /* tile the non-floating, non-fullscreen stack windows */
    for (c = c->next; c; c = c->next) {
        for (d = 0, t = head; t != c; t = t->next, d++);
        if (ISFFTM(c))
            continue;
        if (invert) {
            if (d == l + 1) /* we are on the -right- bottom stack, reset cy */
                cx = gaps;
            if (d > 1 && d != l + 1)
                cx += (ww - gaps) / (d <= l ? l : r);
            xcb_move_resize(dis, c->win,
                        cx, (d <= l) ? cy : cy + (hh - ma) / 2 + ma - gaps,
                        (ww - gaps) / (d <= l ? l : r) - 2 * borders - gaps,
                        (hh - ma) / 2 - 2 * borders - gaps);
        } else {
            if (d == l + 1) /* we are on the right stack, reset cy */
                cy = cb + gaps;
            if (d > 1 && d != l + 1)
                cy += (ch - gaps) / (d <= l ? l : r);
            xcb_move_resize(dis, c->win,
                        d <= l ? cx : ww - cw - 2 * borders - gaps, cy, cw,
                        (ch - gaps) / (d <= l ? l : r) - 2 * borders - gaps);
        }
    }
}

/*
 * when the mouse enters a window's borders
 * the window, if notifying of such events (EnterWindowMask)
 * will notify the wm and will get focus
 */
void enternotify(xcb_generic_event_t *e)
{
    xcb_enter_notify_event_t *ev = (xcb_enter_notify_event_t *)e;

    if (!FOLLOW_MOUSE)
        return;
    DEBUG("xcb: enter notify");
    client *c = wintoclient(ev->event);

    if (c && ev->mode == XCB_NOTIFY_MODE_NORMAL
        && current != c
        && ev->detail != XCB_NOTIFY_DETAIL_INFERIOR) {
        update_current(c);
    }
}

/*
 * equal mode
 * tile the windows in rows or columns, givin each window an equal amount of
 * screen space
 * will use rows when inverted and columns otherwise
 */
void equal(int h, int y)
{
    int n = 0, j = -1;

    for (client *c = head; c; c = c->next) {
        if (ISFFTM(c))
            continue;
        n++;
    }

    for (client *c = head; c; c = c->next) {
        if (ISFFTM(c))
            continue;
        else
            j++;
        if (invert)
            xcb_move_resize(dis, c->win, gaps,
                            y + h / n * j + (c == head ? gaps : 0),
                            ww - 2 * borders - 2 * gaps,
                            h / n - 2 * borders - (c == head ? 2 : 1) * gaps);
        else
            xcb_move_resize(dis, c->win, ww / n * j + (c == head ? gaps : 0),
                            y + gaps,
                            ww / n - 2 * borders - (c == head ? 2 : 1) * gaps,
                            h - 2 * borders - 2 * gaps);
    }
}

/*
 * fibonacci mode / fibonacci layout
 * tile the windows based on the fibonacci series pattern.
 * arrange windows in such a way that every new window shares
 * half the space of the space taken by the last window
 * inverting changes between right/down and right/up
 */
void fibonacci(int h, int y)
{
    int j = -1, x = gaps, tt = 0,
        cw = ww - 2 * gaps - 2 * borders,
        ch = h - 2 * gaps - 2 * borders;

    for (client *n, *c = head; c; c = c->next) {
        if (ISFFTM(c))
            continue;
        else
            j++;
        for (n = c->next; n; n = n->next)
            if (!ISFFTM(n))
                break;

        /*
         * not the last window in stack ? -> half the client size, and also
         * check if we have too many windows to keep them larger than MINWSZ
         */
        if (n
            && ch > MINWSZ * 2 + borders + gaps
            && cw > MINWSZ * 2 + borders + gaps) {
            (j & 1) ? (ch = ch / 2 - borders - gaps / 2)
                    : (cw = cw / 2 - borders - gaps / 2);
            tt = j;
        }

        /* not the master client ? -> shift client right or down (or up) */
        if (j) {
            (j & 1) ? (x = x + cw + 2 * borders + gaps)
                    : (y = invert ? (y - ch - 2 * borders - gaps)
                                  : (y + ch + 2 * borders + gaps));

            if (j & 1 && n && invert)
                y += ch + 2 * borders + gaps;
        }

        /* if the window does not fit in the stack, do not jam it in there */
        if (j <= tt + 1)
            xcb_move_resize(dis, c->win, x, y + gaps, cw, ch);
    }
}

/* switch a client from tiling to float and manage everything involved */
void float_client(client *c)
{
    if (!c)
        return;

    c->isfloating = true;

    if (c->dim[0] && c->dim[1]) {
        if (c->dim[0] < MINWSZ)
            c->dim[0] = MINWSZ;
        if (c->dim[1] < MINWSZ)
            c->dim[1] = MINWSZ;

        xcb_resize(dis, c->win, c->dim[0], c->dim[1]);
    }
}

/*
 * handles x-movement of floating windows
 */
void float_x(const Arg *arg)
{
    xcb_get_geometry_reply_t *r;

    if (!arg->i || !current)
        return;

    if (!current->isfloating) {
        float_client(current);
        tile();
    }

    r = xcb_get_geometry_reply(dis, xcb_get_geometry(dis, current->win), NULL);
    r->x += arg->i;
    xcb_move(dis, current->win, r->x, r->y);
}

/*
 * handles y-movement of floating windows
 */
void float_y(const Arg *arg)
{
    xcb_get_geometry_reply_t *r;

    if (!arg->i || !current)
        return;

    if (!current->isfloating) {
        float_client(current);
        tile();
    }

    r = xcb_get_geometry_reply(dis, xcb_get_geometry(dis, current->win), NULL);
    r->y += arg->i;
    xcb_move(dis, current->win, r->x, r->y);
}

/*
 * focus the (first) master window, or switch back to the slave previously
 * focussed, toggling between them
 */
void focusmaster()
{
    if (!head || !current || (current == head && !head->next)
        || prevfocus->isminimized)
        return;

    /* fix for glitchy toggle behaviour between head and head->next */
    if (current == head->next)
        prevfocus = current;

    if (current == head)
        update_current(prevfocus);
    else
        update_current(head);
}

/* find and focus the client which received
 * the urgent hint in the current desktop */
void focusurgent()
{
    client *c;
    int cd = current_desktop, d = 0;

    for (c = head; c && !c->isurgent; c = c->next);
    if (c) {
        update_current(c);
        return;
    } else {
        for (bool f = false; d < DESKTOPS && !f; d++) {
            for (select_desktop(d), c = head; c && !(f = c->isurgent);
            c = c->next);
        }
    }
    select_desktop(cd);
    if (c) {
        change_desktop(&(Arg){.i = --d});
        update_current(c);
    }
}

/* get a pixel with the requested color
 * to fill some window area - borders */
unsigned int getcolor(char *color)
{
    xcb_colormap_t map = screen->default_colormap;
    xcb_alloc_color_reply_t *c;
    unsigned int r, g, b, rgb, pixel;

    rgb = xcb_get_colorpixel(color);
    r = rgb >> 16; g = rgb >> 8 & 0xFF; b = rgb & 0xFF;
    c = xcb_alloc_color_reply(dis, xcb_alloc_color(dis, map, r * 257, g * 257,
                                                   b * 257), NULL);
    if (!c)
        errx(EXIT_FAILURE, "error: cannot allocate color '%s'\n", color);

    pixel = c->pixel;
    free(c);

    return pixel;
}

/* set the given client to listen to button events (presses / releases) */
void grabbuttons(client *c)
{
    unsigned int modifiers[] = { 0, XCB_MOD_MASK_LOCK, numlockmask,
                                 numlockmask|XCB_MOD_MASK_LOCK };

    xcb_ungrab_button(dis, XCB_BUTTON_INDEX_ANY, c->win, XCB_GRAB_ANY);
    for (unsigned int b = 0; b < LENGTH(buttons); b++)
        for (unsigned int m = 0; m < LENGTH(modifiers); m++)
            if (CLICK_TO_FOCUS)
                xcb_grab_button(dis, 1, c->win, XCB_EVENT_MASK_BUTTON_PRESS,
                                XCB_GRAB_MODE_SYNC, XCB_GRAB_MODE_ASYNC,
                                XCB_WINDOW_NONE, XCB_CURSOR_NONE,
                                XCB_BUTTON_INDEX_ANY, XCB_BUTTON_MASK_ANY);
            else
                xcb_grab_button(dis, 1, c->win, XCB_EVENT_MASK_BUTTON_PRESS,
                                XCB_GRAB_MODE_SYNC, XCB_GRAB_MODE_ASYNC,
                                XCB_WINDOW_NONE, XCB_CURSOR_NONE,
                                buttons[b].button,
                                buttons[b].mask|modifiers[m]);
}

/* the wm should listen to key presses */
void grabkeys(void)
{
    xcb_keycode_t *keycode;
    unsigned int modifiers[] = { 0, XCB_MOD_MASK_LOCK, numlockmask,
                                 numlockmask|XCB_MOD_MASK_LOCK };

    xcb_ungrab_key(dis, XCB_GRAB_ANY, screen->root, XCB_MOD_MASK_ANY);
    for (unsigned int i = 0; i < LENGTH(keys); i++) {
        keycode = xcb_get_keycodes(keys[i].keysym);
        for (unsigned int k = 0; keycode[k] != XCB_NO_SYMBOL; k++)
            for (unsigned int m = 0; m < LENGTH(modifiers); m++)
                xcb_grab_key(dis, 1, screen->root, keys[i].mod | modifiers[m],
                             keycode[k], XCB_GRAB_MODE_ASYNC,
                             XCB_GRAB_MODE_ASYNC);
        free(keycode);
    }
}

/* arrange windows in a grid */
void grid(int hh, int cy)
{
    int n = 0, cols = 0, cn = 0, rn = 0, i = -1;
    for (client *c = head; c; c = c->next)
        if (!ISFFTM(c))
            ++n;
    if (!n)
        return;
    for (cols = 0; cols <= n / 2; cols++)
        if (cols * cols >= n)
            break; /* emulate square root */
    if (n == 5)
        cols = 2;

    int rows = n / cols,
        ch = hh - gaps,
        cw = (ww - gaps) / (cols ? cols : 1);
    for (client *c = head; c; c = c->next) {
        if (ISFFTM(c))
            continue;
        else
            ++i;
        if (i / rows + 1 > cols - n % cols)
            rows = n / cols + 1;
        xcb_move_resize(dis, c->win, cn * cw + gaps,
                        cy + rn * ch / rows + gaps,
                        cw - 2 * borders - gaps,
                        ch / rows - 2 * borders - gaps);
        if (++rn >= rows) {
            rn = 0;
            cn++;
        }
    }
}

/* invert v-stack left-right */
void invertstack()
{
    if ((invert = !invert))
        desktops[current_desktop].invert = invert;
    tile();
}

/* on the press of a key check to see if there's a binded function to call */
void keypress(xcb_generic_event_t *e)
{
    xcb_key_press_event_t *ev       = (xcb_key_press_event_t *)e;
    xcb_keysym_t           keysym   = xcb_get_keysym(ev->detail);

    DEBUGP("xcb: keypress: code: %d mod: %d\n", ev->detail, ev->state);
    for (unsigned int i = 0; i < LENGTH(keys); i++)
        if (keysym == keys[i].keysym &&
            CLEANMASK(keys[i].mod) == CLEANMASK(ev->state) &&
            keys[i].func)
                keys[i].func(&keys[i].arg);
}

/* explicitly kill a client - close the highlighted window
 * send a delete message and remove the client */
void killclient()
{
    if (!current)
        return;
    xcb_icccm_get_wm_protocols_reply_t reply;
    unsigned int n = 0;
    bool got = false;

    if (xcb_icccm_get_wm_protocols_reply(dis,
        xcb_icccm_get_wm_protocols(dis, current->win, wmatoms[WM_PROTOCOLS]),
        &reply, NULL)) { /* TODO: Handle error? */
        for (; n != reply.atoms_len; ++n)
            if ((got = reply.atoms[n] == wmatoms[WM_DELETE_WINDOW]))
                break;
        xcb_icccm_get_wm_protocols_reply_wipe(&reply);
    }
    if (got)
        deletewindow(current->win);
    else {
        xcb_kill_client(dis, current->win);
        removeclient(current);
    }
}

/* focus the previously focused desktop */
void last_desktop()
{
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
void maprequest(xcb_generic_event_t *e)
{
    xcb_map_request_event_t            *ev = (xcb_map_request_event_t *)e;
    xcb_window_t                       windows[] = {ev->window}, transient = 0;
    xcb_get_window_attributes_reply_t  *attr[1];
    xcb_get_geometry_reply_t           *geometry;
    xcb_get_property_reply_t           *prop_reply;
    xcb_ewmh_get_atoms_reply_t         type;
    xcb_get_property_cookie_t          cookie;
    xcb_ewmh_get_utf8_strings_reply_t  wtitle;
    bool atom_success = false;

    xcb_get_attributes(windows, attr, 1);
    if (!attr[0] || attr[0]->override_redirect) {
        free(attr[0]);
        return;
    } else {
        free(attr[0]);
    }

    if (wintoclient(ev->window))
        return;
    if (xcb_ewmh_get_wm_window_type_reply(ewmh,
                                      xcb_ewmh_get_wm_window_type(ewmh,
                                      ev->window), &type, NULL) == 1) {
        for (unsigned int i = 0; i < type.atoms_len; i++) {
            xcb_atom_t a = type.atoms[i];
            if (a == ewmh->_NET_WM_WINDOW_TYPE_TOOLBAR
                || a == ewmh->_NET_WM_WINDOW_TYPE_DOCK) {
                xcb_ewmh_get_atoms_reply_wipe(&type);
                return;
            }
        }
        atom_success = true;
    }

    DEBUG("xcb: map request");

    bool follow = false, floating = false;
    int cd = current_desktop, newdsk = current_desktop;

    cookie = xcb_ewmh_get_wm_name_unchecked(ewmh, ev->window);

    if (xcb_ewmh_get_wm_name_reply(ewmh, cookie, &wtitle, (void *)0)) {
        DEBUGP("EWMH window title: %s\n", wtitle.strings);
        if (!strcmp(wtitle.strings, SCRPDNAME)) {
            client *c;

            if (!(c = (client *)calloc(1, sizeof(client))))
                err(EXIT_FAILURE, "cannot allocate client");

            unsigned int values[1] = {XCB_EVENT_MASK_PROPERTY_CHANGE|
                                      (FOLLOW_MOUSE
                                      ? XCB_EVENT_MASK_ENTER_WINDOW : 0)};
            xcb_change_window_attributes_checked(dis, (c->win = ev->window),
                                                 XCB_CW_EVENT_MASK, values);
            scrpd = c;
            xcb_map_window(dis, scrpd->win);
            xcb_move(dis, scrpd->win, -2 * ww, 0);
            xcb_ewmh_get_utf8_strings_reply_wipe(&wtitle);

            if (atom_success) {
                xcb_ewmh_get_atoms_reply_wipe(&type);
            }
            return;
        }
        for (unsigned int i = 0; i < LENGTH(appruleregex); i++)
            if (!regexec(&appruleregex[i], &wtitle.strings[0], 0, NULL, 0)) {
                follow = rules[i].follow;
                newdsk = (rules[i].desktop < 0 ||
                          rules[i].desktop >= DESKTOPS) ? current_desktop
                                                        : rules[i].desktop;
                floating = rules[i].floating;
                break;
            }

        xcb_ewmh_get_utf8_strings_reply_wipe(&wtitle);
    }
    if (atom_success) {
        for (unsigned int i = 0; i < type.atoms_len; i++) {
            xcb_atom_t a = type.atoms[i];
            if (a == ewmh->_NET_WM_WINDOW_TYPE_SPLASH
                || a == ewmh->_NET_WM_WINDOW_TYPE_DIALOG
                || a == ewmh->_NET_WM_WINDOW_TYPE_DROPDOWN_MENU
                || a == ewmh->_NET_WM_WINDOW_TYPE_POPUP_MENU
                || a == ewmh->_NET_WM_WINDOW_TYPE_TOOLTIP
                || a == ewmh->_NET_WM_WINDOW_TYPE_NOTIFICATION) {
                floating = true;
            }
        }
        xcb_ewmh_get_atoms_reply_wipe(&type);
    }

    /* might be useful in future */
    if ((geometry = xcb_get_geometry_reply(dis,
                                           xcb_get_geometry(dis, ev->window),
                                           NULL))) { /* TODO: error handling */
        DEBUGP("geom: %ux%u+%d+%d\n", geometry->width, geometry->height,
                                      geometry->x,     geometry->y);
        free(geometry);
    }

    if (cd != newdsk)
        select_desktop(newdsk);
    client *c = addwindow(ev->window);

    xcb_icccm_get_wm_transient_for_reply(dis,
                    xcb_icccm_get_wm_transient_for_unchecked(dis, ev->window),
                    &transient, NULL); /* TODO: error handling */
    c->istransient = transient ? true : false;
    c->isfloating  = floating || c->istransient;

    prop_reply = xcb_get_property_reply(dis, xcb_get_property_unchecked(
                                    dis, 0, ev->window, netatoms[NET_WM_STATE],
                                    XCB_ATOM_ATOM, 0, 1), NULL);
                                    /* TODO: error handling */
    if (prop_reply) {
        if (prop_reply->format == 32) {
            xcb_atom_t *v = xcb_get_property_value(prop_reply);
            for (unsigned int i = 0; i < prop_reply->value_len; i++)
                DEBUGP("%d : %d\n", i, v[0]);
            setfullscreen(c, (v[0] == netatoms[NET_FULLSCREEN]));
        }
        free(prop_reply);
    }

    /** information for stdout **/
    DEBUGP("transient: %d\n", c->istransient);
    DEBUGP("floating:  %d\n", c->isfloating);

    if (cd != newdsk)
        select_desktop(cd);
    if (cd == newdsk) {
        tile();
        if (show)
            xcb_map_window(dis, c->win);
        update_current(c);
    } else if (follow) {
        change_desktop(&(Arg){.i = newdsk});
        update_current(c);
    }
    grabbuttons(c);

    desktopinfo();

    if (c->isfloating && AUTOCENTER)
        centerwindow();
}

/* maximize the current window, or if we are maximized, tile() */
void maximize()
{
    xcb_get_geometry_reply_t *r;
    int hh, cy;

    if (!current)
        return;

    hh = wh + (showpanel ? 0 : PANEL_HEIGHT);
    cy = (TOP_PANEL && showpanel ? PANEL_HEIGHT : 0);

    /* TODO: save floating geo before maximizing to reinstate floating ? */
    /* if we float, reinstate tiling layout to exit maximize */
    if (current->isfloating)
        switch_mode(&(Arg){.i = mode});

    /* check if we are already maximized, using actual window size to check */
    r = xcb_get_geometry_reply(dis, xcb_get_geometry(dis, current->win), NULL);
    if (r->width == ww - 2 * gaps - 2 * borders
        && r->height == hh - 2 * gaps - 2 * borders) {
        tile();
        free(r);
        return;
    } else {
        free(r);
    }

    xcb_move_resize(dis, current->win, gaps, cy + gaps,
                    ww - 2 * gaps - 2 * borders, hh - 2 * gaps - 2 * borders);
}

/* push the current client down the miniq and minimize the window */
void minimize()
{
    filo *tmp, *new;

    if (!current)
        return;

    tmp = miniq[current_desktop];
    while (tmp->next)
        tmp = tmp->next;

    /* we always have an empty filo at the end of the miniq */
    new = calloc(1, sizeof(filo));
    if (!new)
        return;

    tmp->c = current;
    tmp->next = new;

    tmp->c->isminimized = true;
    xcb_move(dis, tmp->c->win, -2 * ww, 0);

    client *t = head;
    while (t) {
        if (t && !t->isminimized)
            break;
        t = t->next;
    }
    if (t)
        update_current(t);

    tile();
}

/* grab the pointer and get it's current position
 * all pointer movement events will be reported until it's ungrabbed
 * until the mouse button has not been released,
 * grab the interesting events - button press/release and pointer motion
 * and on on pointer movement resize or move the window under the curson.
 * if the received event is a map request or a configure request call the
 * appropriate handler, and stop listening for other events.
 * Ungrab the poitner and event handling is passed back to run() function.
 * Once a window has been moved or resized, it's marked as floating. */
void mousemotion(const Arg *arg)
{
    xcb_get_geometry_reply_t  *geometry;
    xcb_query_pointer_reply_t *pointer;
    xcb_grab_pointer_reply_t  *grab_reply;
    int mx, my, winx, winy, winw, winh, xw, yh;

    if (!current)
        return;
    geometry = xcb_get_geometry_reply(dis, xcb_get_geometry(dis, current->win),
                                      NULL); /* TODO: error handling */
    if (geometry) {
        winx = geometry->x;     winy = geometry->y;
        winw = geometry->width; winh = geometry->height;
        free(geometry);
    } else {
        return;
    }

    pointer = xcb_query_pointer_reply(dis,
                                      xcb_query_pointer(dis, screen->root), 0);
    if (!pointer)
        return;
    mx = pointer->root_x;
    my = pointer->root_y;

    grab_reply = xcb_grab_pointer_reply(dis, xcb_grab_pointer(dis, 0,
        screen->root,
        BUTTONMASK|XCB_EVENT_MASK_BUTTON_MOTION|XCB_EVENT_MASK_POINTER_MOTION,
        XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC, XCB_NONE, XCB_NONE,
        XCB_CURRENT_TIME), NULL);

    if (!grab_reply || grab_reply->status != XCB_GRAB_STATUS_SUCCESS) {
        free(grab_reply);
        return;
    } else {
        free(grab_reply);
    }

    if (current->isfullscrn)
        setfullscreen(current, False);
    if (!current->isfloating)
        float_client(current);
    tile();
    update_current(current);

    xcb_generic_event_t *e = NULL;
    xcb_motion_notify_event_t *ev = NULL;
    bool ungrab = false;
    do {
        xcb_flush(dis);
        while (!(e = xcb_wait_for_event(dis)))
            xcb_flush(dis);
        switch (e->response_type & ~0x80) {
            case XCB_CONFIGURE_REQUEST: case XCB_MAP_REQUEST:
                events[e->response_type & ~0x80](e);
                break;
            case XCB_MOTION_NOTIFY:
                ev = (xcb_motion_notify_event_t *)e;
                xw = (arg->i == MOVE ? winx : winw) + ev->root_x - mx;
                yh = (arg->i == MOVE ? winy : winh) + ev->root_y - my;
                if (arg->i == RESIZE) xcb_resize(dis, current->win,
                                      xw > MINWSZ ? xw : winw,
                                      yh > MINWSZ ? yh : winh);
                else if (arg->i == MOVE) xcb_move(dis, current->win, xw, yh);
                xcb_flush(dis);
                break;
            case XCB_KEY_PRESS:
            case XCB_KEY_RELEASE:
            case XCB_BUTTON_PRESS:
            case XCB_BUTTON_RELEASE:
                ungrab = true;
        }
        if (e)
            free(e);
    } while (!ungrab && current);
    DEBUG("xcb: ungrab");
    xcb_ungrab_pointer(dis, XCB_CURRENT_TIME);

    free(pointer);
}

/* each window should cover all the available screen space */
void monocle(int hh, int cy)
{
    unsigned int b = MONOCLE_BORDERS ? 2 * borders : 0;

    for (client *c = head; c; c = c->next)
        if (!ISFFTM(c))
            xcb_move_resize(dis, c->win, gaps, cy + gaps,
                            ww - 2 * gaps - b, hh - 2 * gaps - b);
}

/* move the current client, to current->next
 * and current->next to current client's position */
void move_down()
{
    if (!current)
        return;
    /* p is previous, c is current, n is next, if current is head n is last */
    client *p = NULL, *n = (current->next) ? current->next : head;
    if (!(p = prev_client(current)))
        return;
    /*
     * if c is head, swapping with n should update head to n
     * [c]->[n]->..  ==>  [n]->[c]->..
     *  ^head              ^head
     *
     * else there is a previous client and p->next should be what's after c
     * ..->[p]->[c]->[n]->..  ==>  ..->[p]->[n]->[c]->..
     */
    if (current == head)
        head = n;
    else
        p->next = current->next;
    /*
     * if c is the last client, c will be the current head
     * [n]->..->[p]->[c]->NULL  ==>  [c]->[n]->..->[p]->NULL
     *  ^head                         ^head
     * else c will take the place of n, so c-next will be n->next
     * ..->[p]->[c]->[n]->..  ==>  ..->[p]->[n]->[c]->..
     */
    current->next = (current->next) ? n->next : n;
    /*
     * if c was swapped with n then they now point to the same ->next.
     * n->next should be c
     * ..->[p]->[c]->[n]->..  ==>  ..->[p]->[n]->..  ==>  ..->[p]->[n]->[c]->..
     *                                        [c]-^
     *
     * else c is the last client and n is head,
     * so c will be move to be head, no need to update n->next
     * [n]->..->[p]->[c]->NULL  ==>  [c]->[n]->..->[p]->NULL
     *  ^head                         ^head
     */
    if (current->next == n->next)
        n->next = current;
    else
        head = current;
    tile();
}

/* move the current client, to the previous from current and
 * the previous from  current to current client's position */
void move_up()
{
    client *pp = NULL, *p;
    /* p is previous from current or last if current is head */
    if (!(p = prev_client(current)))
        return;
    /* pp is previous from p, or null if current is head and thus p is last */
    if (p->next)
        for (pp = head; pp && pp->next != p; pp = pp->next);
    /*
     * if p has a previous client then the next client should be current
     * (current is c)
     * ..->[pp]->[p]->[c]->..  ==>  ..->[pp]->[c]->[p]->..
     *
     * if p doesn't have a previous client, then p might be head, so head must
     * change to c
     * [p]->[c]->..  ==>  [c]->[p]->..
     *  ^head              ^head
     * if p is not head, then c is head (and p is last), so the new head is
     * next of c
     * [c]->[n]->..->[p]->NULL  ==>  [n]->..->[p]->[c]->NULL
     *  ^head         ^last           ^head         ^last
     */
    if (pp)
        pp->next = current;
    else
        head = (current == head) ? current->next : current;
    /*
     * next of p should be next of c
     * ..->[pp]->[p]->[c]->[n]->..  ==>  ..->[pp]->[c]->[p]->[n]->..
     * except if c was head (now c->next is head), so next of p should be c
     * [c]->[n]->..->[p]->NULL  ==>  [n]->..->[p]->[c]->NULL
     *  ^head         ^last           ^head         ^last
     */
    p->next = (current->next == head) ? current : current->next;
    /*
     * next of c should be p
     * ..->[pp]->[p]->[c]->[n]->..  ==>  ..->[pp]->[c]->[p]->[n]->..
     * except if c was head (now c->next is head), so c is must be last
     * [c]->[n]->..->[p]->NULL  ==>  [n]->..->[p]->[c]->NULL
     *  ^head         ^last           ^head         ^last
     */
    current->next = (current->next == head) ? NULL : p;
    tile();
}

/* cyclic focus the next window
 * if the window is the last on stack, focus head */
void next_win()
{
    client *t = current;

    if (!current || !head->next)
        return;

    while (1) {
        if (!t->next)
            t = head;
        else
            t = t->next;
        if (!t->isminimized)
            break;
        if (t == current)
            break;
    }

    prevfocus = current;
    update_current(t);
}

/* get the previous client from the given
 * if no such client, return NULL */
client *prev_client(client *c)
{
    if (!c || !head->next)
        return NULL;
    client *p;
    for (p = head; p->next && p->next != c; p = p->next);
    return p;
}

/* cyclic focus the previous window
 * if the window is the head, focus the last stack window */
void prev_win()
{
    client *t = current;

    if (!current || !head->next)
        return;

    while (1) {
        t = prev_client(t);
        if (!t->isminimized)
            break;
        if (t == current)
            break;
    }

    prevfocus = current;
    update_current(t);
}

/* property notify is called when one of the window's properties
 * is changed, such as an urgent hint is received
 */
void propertynotify(xcb_generic_event_t *e)
{
    xcb_property_notify_event_t *ev = (xcb_property_notify_event_t *)e;
    xcb_icccm_wm_hints_t wmh;
    client *c;

    DEBUG("xcb: property notify");
    c = wintoclient(ev->window);
    if (!c || ev->atom != XCB_ICCCM_WM_ALL_HINTS)
        return;
    DEBUG("xcb: got hint!");
    if (xcb_icccm_get_wm_hints_reply(dis,
                                     xcb_icccm_get_wm_hints(dis, ev->window),
                                                            &wmh, NULL))
                                     /* TODO: error handling */
        c->isurgent = c != current &&
                           (wmh.flags & XCB_ICCCM_WM_HINT_X_URGENCY);
    desktopinfo();
}

/* to quit just stop receiving events
 * run() is stopped and control is back to main()
 */
void quit(const Arg *arg)
{
    retval = arg->i;
    running = false;
}

/* remove the specified client
 *
 * note, the removing client can be on any desktop,
 * we must return back to the current focused desktop.
 * if c was the previously focused, prevfocus must be updated
 * else if c was the current one, current must be updated. */
void removeclient(client *c)
{
    client **p = NULL;
    int nd = 0, cd = current_desktop;

    for (bool found = false; nd < DESKTOPS && !found; nd++)
        for (select_desktop(nd), p = &head; *p && !(found = *p == c);
             p = &(*p)->next);
    *p = c->next;
    if (c == prevfocus)
        prevfocus = prev_client(current);
    if (c == current || !head->next)
        update_current(prevfocus);
    free(c);
    c = NULL;
    if (cd == nd - 1)
        tile();
    else
        select_desktop(cd);
}

/* resize the master window - check for boundary size limits
 * the size of a window can't be less than MINWSZ
 */
void resize_master(const Arg *arg)
{
    int msz = (mode == BSTACK ? wh : ww) * MASTER_SIZE + master_size + arg->i;

    if (msz < MINWSZ || (mode == BSTACK ? wh : ww) - msz < MINWSZ)
        return;
    master_size += arg->i;
    tile();
}

/* resize the first stack window - no boundary checks */
void resize_stack(const Arg *arg)
{
    growth += arg->i;
    tile();
}

/*
 * resize floating windows in x-dimension (and float them if not already)
 */
void resize_x(const Arg *arg)
{
    xcb_get_geometry_reply_t *r;

    if (!arg->i || !current)
        return;

    if (!current->isfloating) {
        float_client(current);
        tile();
    }

    r = xcb_get_geometry_reply(dis, xcb_get_geometry(dis, current->win), NULL);

    if (r->width + arg->i < MINWSZ || r->width + arg->i <= 0)
        return;

    r->width += arg->i;
    xcb_move_resize(dis, current->win, r->x, r->y, r->width, r->height);
}

/*
 * resize floating windows in y-dimension (and float them if not already)
 */
void resize_y(const Arg *arg)
{
    xcb_get_geometry_reply_t *r;

    if (!arg->i || !current)
        return;

    if (!current->isfloating) {
        float_client(current);
        tile();
    }

    r = xcb_get_geometry_reply(dis, xcb_get_geometry(dis, current->win), NULL);

    if (r->height + arg->i < MINWSZ || r->height + arg->i <= 0)
        return;

    r->height += arg->i;
    xcb_move_resize(dis, current->win, r->x, r->y, r->width, r->height);
}

/* get the last client from the current miniq and restore it */
void restore()
{
    filo *tmp;

    if (!miniq[current_desktop]->c)
        return;

    /* find the last occupied filo, before the free one */
    tmp = miniq[current_desktop];
    while (tmp->next) {
        if (!tmp->next->next)
            break;
        tmp = tmp->next;
    }

    free(tmp->next);
    tmp->next = NULL;
    tmp->c->isminimized = false;

    /*
     * if our window is floating, center it to move it back onto the visible
     * screen, TODO: save geometry in some way to restore it where it was
     * before minimizing, TODO: fix it to use centerwindow() instead of copying
     * half of it
     */
    if (tmp->c->isfloating) {
        xcb_get_geometry_reply_t *wa;
        wa = xcb_get_geometry_reply(dis, xcb_get_geometry(dis, tmp->c->win),
                                    NULL);
        xcb_raise_window(dis, tmp->c->win);
        xcb_move(dis, tmp->c->win, (ww - wa->width) / 2, (wh - wa->height) / 2);
        free(wa);
    }
    tile();
    update_current(tmp->c);
    tmp->c = NULL;
}

/* jump and focus the next or previous desktop */
void rotate(const Arg *arg)
{
    change_desktop(&(Arg)
                   {.i = (DESKTOPS + current_desktop + arg->i) % DESKTOPS});
}

/* jump and focus the next or previous desktop
 * and take the current client with us. */
void rotate_client(const Arg *arg)
{
    int i = (DESKTOPS + current_desktop + arg->i) % DESKTOPS;

    client_to_desktop(&(Arg){.i = i});
    change_desktop(&(Arg){.i = i});
}

/* jump and focus the next or previous desktop that has clients */
void rotate_filled(const Arg *arg)
{
    int n = arg->i;

    while (n < DESKTOPS && !desktops[(DESKTOPS + current_desktop + n) %
                                     DESKTOPS].head)
        (n += arg->i);
    change_desktop(&(Arg){.i = (DESKTOPS + current_desktop + n) % DESKTOPS});
}

/*
 * main event loop - on receival of an event call the appropriate event
 * handler
 */
void run(void)
{
    xcb_generic_event_t *ev;

    while(running) {
        xcb_flush(dis);
        if (xcb_connection_has_error(dis))
            err(EXIT_FAILURE, "error: X11 connection got interrupted\n");
        if ((ev = xcb_wait_for_event(dis))) {
            if (events[ev->response_type & ~0x80]) {
                events[ev->response_type & ~0x80](ev);
            } else {
                DEBUGP("xcb: unimplented event: %d\n",
                       ev->response_type & ~0x80);
            }
            free(ev);
        }
    }
}

/* save specified desktop's properties */
void save_desktop(int i)
{
    if (i < 0 || i >= DESKTOPS)
        return;
    desktops[i].master_size = master_size;
    desktops[i].mode        = mode;
    desktops[i].growth      = growth;
    desktops[i].head        = head;
    desktops[i].current     = current;
    desktops[i].showpanel   = showpanel;
    desktops[i].prevfocus   = prevfocus;
    desktops[i].gaps        = gaps;
    desktops[i].invert = invert;
}

/* set the specified desktop's properties */
void select_desktop(int i)
{
    if (i < 0 || i >= DESKTOPS)
        return;
    save_desktop(current_desktop);
    master_size     = desktops[i].master_size;
    mode            = desktops[i].mode;
    growth          = desktops[i].growth;
    head            = desktops[i].head;
    current         = desktops[i].current;
    showpanel       = desktops[i].showpanel;
    prevfocus       = desktops[i].prevfocus;
    gaps            = desktops[i].gaps;
    invert     = desktops[i].invert;
    current_desktop = i;
}

/* set or unset fullscreen state of client */
void setfullscreen(client *c, bool fullscrn)
{
    DEBUGP("xcb: set fullscreen: %d\n", fullscrn);
    long data[] = { fullscrn ? netatoms[NET_FULLSCREEN] : XCB_NONE };

    if (fullscrn != c->isfullscrn)
        xcb_change_property(dis, XCB_PROP_MODE_REPLACE,
                            c->win, netatoms[NET_WM_STATE], XCB_ATOM_ATOM, 32,
                            fullscrn, data);
    if ((c->isfullscrn = fullscrn))
        xcb_move_resize(dis, c->win, 0, 0, ww, wh + PANEL_HEIGHT);
    xcb_border_width(dis, c->win,
                     (!head->next ||
                      c->isfullscrn ||
                      (mode == MONOCLE && !ISFFTM(c) && !MONOCLE_BORDERS)
                     ) ? 0:borders);
    update_current(c);
}

/* get numlock modifier using xcb */
int setup_keyboard(void)
{
    xcb_get_modifier_mapping_reply_t *reply;
    xcb_keycode_t                    *modmap;
    xcb_keycode_t                    *numlock;

    reply = xcb_get_modifier_mapping_reply(dis,
                            xcb_get_modifier_mapping_unchecked(dis), NULL);
                            /* TODO: error checking */
    if (!reply)
        return -1;

    modmap = xcb_get_modifier_mapping_keycodes(reply);
    if (!modmap) {
        free(reply);
        return -1;
    }

    numlock = xcb_get_keycodes(XK_Num_Lock);
    for (unsigned int i = 0; i < 8; i++)
       for (unsigned int j = 0; j < reply->keycodes_per_modifier; j++) {
            xcb_keycode_t keycode = modmap[i * reply->keycodes_per_modifier +
                                           j];
            if (keycode == XCB_NO_SYMBOL)
                continue;
            for (unsigned int n = 0; numlock[n] != XCB_NO_SYMBOL; n++)
                if (numlock[n] == keycode) {
                    DEBUGP("xcb: found num-lock %d\n", 1 << i);
                    numlockmask = 1 << i;
                    break;
                }
        }
    free(reply);
    free(numlock);

    return 0;
}

/* set initial values
 * root window - screen height/width - atoms - xerror handler
 * set masks for reporting events handled by the wm
 * and propagate the suported net atoms
 */
int setup(int default_screen)
{
    xcb_intern_atom_cookie_t *cookie;

    sigchld();
    screen = xcb_screen_of_display(dis, default_screen);
    if (!screen)
        err(EXIT_FAILURE, "error: cannot aquire screen\n");

    ww = screen->width_in_pixels;
    wh = screen->height_in_pixels - PANEL_HEIGHT;
    borders = BORDER_WIDTH;
    gaps = USELESSGAP;
    for (unsigned int i = 0; i < DESKTOPS; i++) {
        desktops[i].gaps = USELESSGAP;
        save_desktop(i);
        miniq[i] = calloc(1, sizeof(struct filo));
        if (!miniq[i])
            err(EXIT_FAILURE, "error: cannot allocate miniq\n");
    }

    win_focus   = getcolor(FOCUS);
    win_unfocus = getcolor(UNFOCUS);

    /* setup keyboard */
    if (setup_keyboard() == -1)
        err(EXIT_FAILURE, "error: failed to setup keyboard\n");

    /* set up atoms for dialog/notification windows */
    xcb_get_atoms(WM_ATOM_NAME, wmatoms, WM_COUNT);
    xcb_get_atoms(NET_ATOM_NAME, netatoms, NET_COUNT);

    /* check if another wm is running */
    if (xcb_checkotherwm())
        err(EXIT_FAILURE, "error: other wm is running\n");

    /* initialize apprule regexes */
    for (unsigned int i = 0; i < LENGTH(rules); i++)
        if (regcomp(&appruleregex[i], rules[i].class, 0))
            err(EXIT_FAILURE, "error: failed to compile apprule regexes\n");

    /* initialize EWMH */
    ewmh = calloc(1, sizeof(xcb_ewmh_connection_t));
    if (!ewmh)
        err(EXIT_FAILURE, "error: failed to set ewmh atoms\n");
    cookie = xcb_ewmh_init_atoms(dis, ewmh);
    xcb_ewmh_init_atoms_replies(ewmh, cookie, (void *)0);

    /* set EWMH atoms */
    xcb_atom_t net_atoms[] = { ewmh->_NET_SUPPORTED,
                               ewmh->_NET_WM_STATE_FULLSCREEN,
                               ewmh->_NET_WM_STATE,
                               ewmh->_NET_SUPPORTING_WM_CHECK,
                               ewmh->_NET_ACTIVE_WINDOW,
                               ewmh->_NET_NUMBER_OF_DESKTOPS,
                               ewmh->_NET_CURRENT_DESKTOP,
                               ewmh->_NET_DESKTOP_GEOMETRY,
                               ewmh->_NET_DESKTOP_VIEWPORT,
                               ewmh->_NET_WORKAREA,
                               ewmh->_NET_SHOWING_DESKTOP,
                               ewmh->_NET_CLOSE_WINDOW,
                               ewmh->_NET_WM_DESKTOP,
                               ewmh->_NET_WM_WINDOW_TYPE };

    xcb_ewmh_coordinates_t viewports[2] = {{ 0, 0 }};
    /* TODO: calculate workarea properly by substracting optional panel space */
    xcb_ewmh_geometry_t workarea[2] = {{ 0, 0, ww, wh }};

    xcb_ewmh_set_supported(ewmh, default_screen, NET_COUNT, net_atoms);
    xcb_ewmh_set_supporting_wm_check(ewmh, default_screen, screen->root);
    xcb_ewmh_set_number_of_desktops(ewmh, default_screen, DESKTOPS);
    xcb_ewmh_set_current_desktop(ewmh, default_screen, DEFAULT_DESKTOP);
    xcb_ewmh_set_desktop_geometry(ewmh, default_screen, ww, wh);
    xcb_ewmh_set_desktop_viewport(ewmh, default_screen, 1, viewports);
    xcb_ewmh_set_workarea(ewmh, default_screen, 1, workarea);
    xcb_ewmh_set_showing_desktop(ewmh, default_screen, 0);

    xcb_change_property(dis, XCB_PROP_MODE_REPLACE, screen->root,
                        netatoms[NET_SUPPORTED], XCB_ATOM_ATOM, 32, NET_COUNT,
                        netatoms);
    grabkeys();

    /* set events */
    for (unsigned int i = 0; i < XCB_NO_OPERATION; i++)
        events[i] = NULL;
    events[XCB_BUTTON_PRESS]        = buttonpress;
    events[XCB_CLIENT_MESSAGE]      = clientmessage;
    events[XCB_CONFIGURE_REQUEST]   = configurerequest;
    events[XCB_DESTROY_NOTIFY]      = destroynotify;
    events[XCB_ENTER_NOTIFY]        = enternotify;
    events[XCB_KEY_PRESS]           = keypress;
    events[XCB_MAP_REQUEST]         = maprequest;
    events[XCB_PROPERTY_NOTIFY]     = propertynotify;
    events[XCB_UNMAP_NOTIFY]        = unmapnotify;

    /* grab existing windows */
    xcb_get_window_attributes_reply_t *attr;
    xcb_query_tree_reply_t *reply = xcb_query_tree_reply(dis,
                                        xcb_query_tree(dis, screen->root), 0);
    if (reply) {
        int len = xcb_query_tree_children_length(reply);
        xcb_window_t *children = xcb_query_tree_children(reply);
        int cd = current_desktop;
        for (int i = 0; i < len; i++) {
            attr = xcb_get_window_attributes_reply(dis,
                            xcb_get_window_attributes(dis, children[i]), NULL);
            if (!attr)
                continue;
            /* ignore windows in override redirect mode or with input only
             * class as we won't see them */
            if (!attr->override_redirect
                && attr->_class != XCB_WINDOW_CLASS_INPUT_ONLY) {
                uint32_t dsk = cd;
                int haddsk = xcb_ewmh_get_wm_desktop_reply(ewmh,
                    xcb_ewmh_get_wm_desktop(ewmh, children[i]), &dsk, NULL);
                if ((!haddsk || dsk == cd) && attr->map_state == XCB_MAP_STATE_UNMAPPED) {
                    /* if a window is unmapped and not from different desktop,
                     * it hasn't requested mapping */
                    continue;
                }
                if (cd != dsk)
                    select_desktop(dsk);
                addwindow(children[i]);
                grabbuttons(wintoclient(children[i]));
                if (cd != dsk) {
                    xcb_unmap_window(dis, children[i]);
                    select_desktop(cd);
                }
            }
            free(attr);
        }
        free(reply);
    }

    change_desktop(&(Arg){.i = DEFAULT_DESKTOP});
    switch_mode(&(Arg){.i = DEFAULT_MODE});

    /* open the scratchpad terminal if enabled */
    if (USE_SCRATCHPAD)
        spawn(&(Arg){.com = scrpcmd});

    return 0;
}

/*
 * toggle visibility of all windows in all desktops
 */
void showhide(void)
{
    if ((show = !show)) {
        tile();
        if (show)
            for (client *c = desktops[current_desktop].head; c; c = c->next)
                xcb_map_window(dis, c->win);
        xcb_ewmh_set_showing_desktop(ewmh, default_screen, 1);
    } else {
        for (client *c = desktops[current_desktop].head; c; c = c->next)
            xcb_move(dis, c->win, -2 * ww, 0);
        xcb_ewmh_set_showing_desktop(ewmh, default_screen, 0);
    }
}

void sigchld()
{
    if (signal(SIGCHLD, sigchld) == SIG_ERR)
        err(EXIT_FAILURE, "cannot install SIGCHLD handler");
    while (0 < waitpid(-1, NULL, WNOHANG));
}

/* execute a command, save the child pid if we start the scratchpad */
void spawn(const Arg *arg)
{
    if (fork())
        return;
    if (dis)
        close(screen->root);
    setsid();
    execvp((char *)arg->com[0], (char **)arg->com);
    fprintf(stderr, "error: execvp %s", (char *)arg->com[0]);
    perror(" failed"); /* also prints the err msg */
    exit(EXIT_SUCCESS);
}

/* arrange windows in normal or bottom stack tile */
void stack(int hh, int cy)
{
    client *c = NULL, *t = NULL; bool b = mode == BSTACK;
    int n = 0, d = 0, z = b ? ww : hh,
        ma = (mode == BSTACK ? wh : ww) * MASTER_SIZE + master_size;

    /* count stack windows and grab first non-floating, non-fullscreen window */
    for (t = head; t; t = t->next) {
        if (!ISFFTM(t)) {
            if (c)
                ++n;
            else
                c = t;
        }
    }

    /*
     * if there is only one window, it should cover the available screen space
     * if there is only one stack window (n == 1) then we don't care about
     * growth if more than one stack windows (n > 1) on screen then adjustments
     * may be needed
     *   - d is the num of pixels than remain when spliting
     *   the available width/height to the number of windows
     *   - z is the clients' height/width
     *
     *      ----------  -.    --------------------.
     *      |   |----| --|--> growth               `}--> first client will get
     *      |   |    |   |                          |    (z+d) height/width
     *      |   |----|   }--> screen height - hh  --'
     *      |   |    | }-|--> client height - z
     *      ----------  -'
     *
     *     ->  piece of art by c00kiemon5ter o.O om nom nom nom nom
     *
     *     what we do is, remove the growth from the screen height   : (z -
     *     growth) and then divide that space with the windows on the stack  :
     *     (z - growth)/n so all windows have equal height/width (z)
     *     : growth is left out and will later be added to the first's client
     *     height/width before that, there will be cases when the num of
     *     windows is not perfectly divided with then available screen
     *     height/width (ie 100px scr. height, and 3 windows) so we get that
     *     remaining space and merge growth to it (d) : (z - growth) % n +
     *     growth finally we know each client's height, and how many pixels
     *     should be added to the first stack window so that it satisfies
     *     growth, and doesn't create gaps
     *     on the bottom of the screen.
     */
    if (!c) {
        return;
    } else if (!n) {
        xcb_move_resize(dis, c->win, gaps, cy + gaps,
                        ww - 2 * (borders + gaps),
                        hh - 2 * (borders + gaps));
        return;
    } else if (n > 1) {
        d = (z - growth) % n + growth; z = (z - growth) / n;
    }

    /* tile the first non-floating, non-fullscreen window to cover the master area */
    if (b)
        xcb_move_resize(dis, c->win, gaps,
                        invert ? (cy + hh - ma + gaps) : (cy + gaps),
                        ww - 2 * (borders + gaps),
                        ma - 2 * (borders + gaps));
    else
        xcb_move_resize(dis, c->win, invert ? (ww - ma + gaps) : gaps,
                        cy + gaps,
                        ma - 2 * (borders + gaps),
                        hh - 2 * (borders + gaps));

    /* tile the next non-floating, non-fullscreen (first) stack window with growth|d */
    for (c = c->next; c && ISFFTM(c); c = c->next);
    int cx = b ? 0 : (invert ? gaps : ma),
        cw = (b ? hh : ww) - 2 * borders - ma - gaps,
        ch = z - 2 * borders - gaps;
    if (b)
        xcb_move_resize(dis, c->win, cx += gaps, cy += invert ? gaps : ma,
                        ch - gaps + d, cw);
    else
        xcb_move_resize(dis, c->win, cx, cy += gaps, cw, ch - gaps + d);

    /* tile the rest of the non-floating, non-fullscreen stack windows */
    for (b ? (cx += z + d - gaps) : (cy += z + d - gaps),
         c = c->next; c; c = c->next) {
        if (ISFFTM(c))
            continue;
        if (b) {
            xcb_move_resize(dis, c->win, cx, cy, ch, cw); cx += z;
        } else {
            xcb_move_resize(dis, c->win, cx, cy, cw, ch); cy += z;
        }
    }
}

/* swap master window with current or
 * if current is head swap with next
 * if current is not head, then head
 * is behind us, so move_up until we
 * are the head */
void swap_master()
{
    if (!current || !head->next)
        return;
    if (current == head)
        move_down();
    else
        while (current != head)
            move_up();
    update_current(head);
}

/* switch the tiling mode and reset all floating windows */
void switch_mode(const Arg *arg)
{
    if (!show)
        showhide();
    if (mode == arg->i)
        for (client *c = head; c; c = c->next)
            unfloat_client(c);
    mode = arg->i;
    tile();
    update_current(current);
    desktopinfo();
}


/* cycle the tiling mode and reset all floating windows */
void rotate_mode(const Arg *arg)
{
    if (!show)
        showhide();
    mode = (mode + arg->i + MODES) % MODES;
    tile();
    update_current(current);
    desktopinfo();
}

/* tile all windows of current desktop - call the handler tiling function */
void tile(void)
{
    desktopinfo();
    if (!head)
        return; /* nothing to arange */
    layout[head->next ? mode : MONOCLE](wh + (showpanel ? 0 : PANEL_HEIGHT),
                                (TOP_PANEL && showpanel ? PANEL_HEIGHT : 0));
}

/* reset the active window from floating to tiling, if not already */
void tilemize()
{
    if (!current || !current->isfloating)
        return;
    unfloat_client(current);
    update_current(current);
}

/* toggle visibility state of the panel */
void togglepanel()
{
    showpanel = !showpanel;
    tile();
}

/*
 * Toggle the scratchpad terminal, also attempt to reopen it if it is
 * not present.
 */
void togglescratchpad()
{
    if (!USE_SCRATCHPAD) {
        return;
    } else if (!scrpd) {
        spawn(&(Arg){.com = scrpcmd});
        showscratchpad = false;
        if (!scrpd)
            return;
    }

    showscratchpad = !showscratchpad;

    if (showscratchpad) {
        xcb_get_geometry_reply_t *wa;

        wa = xcb_get_geometry_reply(dis, xcb_get_geometry(dis, scrpd->win),
                                    NULL);
        xcb_move(dis, scrpd->win, (ww - wa->width) / 2, (wh - wa->height) / 2);
        update_current(scrpd);
        xcb_raise_window(dis, scrpd->win);
    } else {
        xcb_move(dis, scrpd->win, -2 * ww, 0);
        if (current == scrpd)
            update_current(prevfocus->isminimized ? head : prevfocus);
    }
}

/* tile a floating client and save its size for re-floating */
void unfloat_client(client *c)
{
    xcb_get_geometry_reply_t *r;

    if (!c)
        return;

    c->isfloating = false;

    r = xcb_get_geometry_reply(dis, xcb_get_geometry(dis, c->win), NULL);
    c->dim[0] = r->width;
    c->dim[1] = r->height;
    free(r);
}

/* windows that request to unmap should lose their
 * client, so no invisible windows exist on screen
 */
void unmapnotify(xcb_generic_event_t *e)
{
    xcb_unmap_notify_event_t *ev = (xcb_unmap_notify_event_t *)e;
    client *c = wintoclient(ev->window);
    if (c)
        removeclient(c);
    desktopinfo();
}

/*
 * highlight borders and set active window and input focus
 * if given current is NULL then delete the active window property
 *
 * stack order by client properties, top to bottom:
 *  - current when floating or transient
 *  - floating or trancient windows
 *  - current when tiled
 *  - current when fullscreen
 *  - fullscreen windows
 *  - tiled windows
 *
 * a window should have borders in any case, except if
 *  - the window is the only window on screen
 *  - the window is fullscreen
 *  - the mode is MONOCLE and the window is not floating or transient
 *    and MONOCLE_BORDERS is set to false
 */
void update_current(client *c)
{
    if (!head) {
        xcb_delete_property(dis, screen->root, netatoms[NET_ACTIVE]);
        current = prevfocus = NULL;
        return;
    } else if (c == prevfocus) {
        prevfocus = prev_client(current = prevfocus ? prevfocus : head);
    } else if (c != current) {
        prevfocus = current; current = c;
    }

    /* num of n:all fl:fullscreen ft:floating/transient windows */
    int n = 0, fl = 0, ft = 0;
    for (c = head; c; c = c->next, ++n)
        if (ISFFTM(c)) {
            fl++;
            if (!c->isfullscrn)
                ft++;
        }
    xcb_window_t w[n];
    w[(current->isfloating || current->istransient) ? 0 : ft] = current->win;
    for (fl += !ISFFTM(current) ? 1 : 0, c = head; c; c = c->next) {
        xcb_change_window_attributes(dis, c->win, XCB_CW_BORDER_PIXEL,
                                (c == current ? &win_focus : &win_unfocus));
        xcb_border_width(dis, c->win,
                         (c->isfullscrn ||
                          (!MONOCLE_BORDERS && !head->next) ||
                          (mode == MONOCLE && !ISFFTM(c) && !MONOCLE_BORDERS)
                          ) ? 0 : borders);
        /*
         * if (CLICK_TO_FOCUS) xcb_grab_button(dis, 1, c->win,
         *     XCB_EVENT_MASK_BUTTON_PRESS, XCB_GRAB_MODE_ASYNC,
         *     XCB_GRAB_MODE_ASYNC, screen->root, XCB_NONE, XCB_BUTTON_INDEX_1,
         *     XCB_BUTTON_MASK_ANY);
         */
        if (c != current)
            w[c->isfullscrn ? --fl : ISFFTM(c) ? --ft : --n] = c->win;
    }

    /* restack */
    if (!current->isfloating)
        for (ft = 0; ft <= n; ++ft)
            xcb_raise_window(dis, w[n-ft]);
    else
        xcb_raise_window(dis, current->win);

    if (USE_SCRATCHPAD && showscratchpad && scrpd)
        xcb_raise_window(dis, scrpd->win);

    xcb_change_property(dis, XCB_PROP_MODE_REPLACE, screen->root,
                        netatoms[NET_ACTIVE], XCB_ATOM_WINDOW, 32, 1,
                        &current->win);
    xcb_set_input_focus(dis, XCB_INPUT_FOCUS_POINTER_ROOT, current->win,
                        XCB_CURRENT_TIME);
    /* if (CLICK_TO_FOCUS) xcb_ungrab_button(dis, XCB_BUTTON_INDEX_1, XCB_NONE,
    *                                        current->win); */
    tile();
}

/* find to which client the given window belongs to */
client *wintoclient(xcb_window_t w)
{
    client *c = NULL;
    int d = 0, cd = current_desktop;
    for (bool found = false; d < DESKTOPS && !found; ++d)
        for (select_desktop(d), c = head; c && !(found = (w == c->win));
             c = c->next);
    if (cd != d-1)
        select_desktop(cd);
    return c;
}

int main(int argc, char *argv[])
{
    if (argc == 2 && argv[1][0] == '-') switch (argv[1][1]) {
        case 'v':
            errx(EXIT_SUCCESS,
            "%s - by sulami (thanks to c00kiemon5ter and Cloudef)",
            VERSION);
        case 'h':
            errx(EXIT_SUCCESS, "%s", USAGE);
        default:
            errx(EXIT_FAILURE, "%s", USAGE);
    } else if (argc != 1) {
        errx(EXIT_FAILURE, "%s", USAGE);
    }
    if (xcb_connection_has_error((dis = xcb_connect(NULL, &default_screen))))
        errx(EXIT_FAILURE, "error: cannot open display\n");
    if (setup(default_screen) != -1) {
        desktopinfo(); /* zero out every desktop on (re)start */
        run();
    }
    cleanup();
    xcb_disconnect(dis);
    return retval;
}

/* vim: set ts=4 sw=4 expandtab :*/

