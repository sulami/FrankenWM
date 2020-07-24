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
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <xcb/xcb.h>
#include <xcb/xcb_atom.h>
#include <xcb/xcb_aux.h>
#include <xcb/xcb_icccm.h>
#include <xcb/xcb_keysyms.h>
#include <xcb/xcb_ewmh.h>

/* compile with -DDEBUGGING for debugging output */
#ifdef DEBUGGING
#  define DEBUG(x)      fprintf(stderr, "%s\n", x);
#  define DEBUGP(x, ...) fprintf(stderr, x, ##__VA_ARGS__);
#else
#  define DEBUG(x);
#  define DEBUGP(x, ...);
#endif

/* upstream compatility */
#define XCB_MOVE_RESIZE XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y | XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT
#define XCB_MOVE        XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y
#define XCB_RESIZE      XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT

static char *WM_NAME   = "FrankenWM";
static char *WM_ATOM_NAME[]   = { "WM_PROTOCOLS", "WM_DELETE_WINDOW", "WM_STATE", "WM_TAKE_FOCUS" };
enum { WM_PROTOCOLS, WM_DELETE_WINDOW, WM_STATE, WM_TAKE_FOCUS, WM_COUNT };
enum { _NET_WM_STATE_REMOVE, _NET_WM_STATE_ADD, _NET_WM_STATE_TOGGLE };

#define LENGTH(x)       (sizeof(x)/sizeof(*x))
#define CLEANMASK(mask) (mask & ~(numlockmask | XCB_MOD_MASK_LOCK))
#define BUTTONMASK      XCB_EVENT_MASK_BUTTON_PRESS|XCB_EVENT_MASK_BUTTON_RELEASE
#define ISFMFTM(c)      (c->isfullscreen || c->ismaximized || c->isfloating || c->istransient || c->isminimized || c->type != ewmh->_NET_WM_WINDOW_TYPE_NORMAL)
#define USAGE           "usage: frankenwm [-h] [-v]"
/* future enhancements */
#define MONITORS 1

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

struct list {
    struct node *head;
    struct node *tail;
    void        *master;    /* backpointer to the list's owner */
};
typedef struct list list;

struct node {
    struct node *prev;
    struct node *next;
    struct list *parent;
};
typedef struct node node;

typedef struct {
    int previous_x, previous_y;
    int current_x, current_y;
} posxy_t;

/*
 * aliens are unmanaged & selfmapped windows, ie dunst notifications.
 * They are always on top of all other windows.
 */
struct alien {
    node link;
    xcb_window_t win;
    xcb_atom_t type;
    posxy_t position_info;
};
typedef struct alien alien;

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
 * link          - doubly linked list node
 * isurgent      - set when the window received an urgent hint
 * istransient   - set when the window is transient
 * isfullscreen  - set when the window is fullscreen (not maximized)
 * ismaximized   - set when the window is maximized (not fullscreen)
 * isfloating    - set when the window is floating
 * win           - the window this client is representing
 * type          - the _NET_WM_WINDOW_TYPE
 * dim           - the window dimensions when floating
 * borderwidth   - the border width if not using the global one
 * setfocus      - True: focus directly, else send wm_take_focus
 *
 * istransient is separate from isfloating as floating window can be reset
 * to their tiling positions, while the transients will always be floating
 */
/*
 * Developer reminder: do not forget to update create_client();
 */
typedef struct {
    node link;  /* must be first */
    bool isurgent, istransient, isfloating, isfullscreen, ismaximized, isminimized;
    xcb_window_t win;
    xcb_atom_t type;
    unsigned int dim[2];
    posxy_t position_info;
    int borderwidth;
    bool setfocus;
} client;

/* properties of each desktop
 * current      - the currently highlighted window
 * prevfocus    - the client that previously had focus
 * mode         - the desktop's tiling layout mode
 * growth       - growth factor of the first stack window
 * master_size  - the size of the master window
 * showpanel    - the visibility status of the panel
 */
typedef struct {
    float master_size;
    int mode, growth, gaps;
    bool showpanel, invert;
} displayinfo;
#define M_MASTER_SIZE (current_display->di.master_size)
#define M_MODE        (current_display->di.mode)
#define M_GROWTH      (current_display->di.growth)
#define M_GAPS        (current_display->di.gaps)
#define M_SHOWPANEL   (current_display->di.showpanel)
#define M_INVERT      (current_display->di.invert)

/* properties of each display
 * current      - the currently highlighted window
 */
typedef struct {
    node link;      /* must be first */
    list clients;   /* must be second */
    client *current, *prevfocus;
    list miniq;
    displayinfo di;
} display;
#define M_CURRENT     (current_display->current)
#define M_PREVFOCUS   (current_display->prevfocus)

typedef struct {
    node link;      /* must be first */
    list displays;  /* must be second */
    unsigned int num;
    int ww, wh;
    int wy;
} monitor;

/* desktop */
typedef struct {
    node link;      /* must be first */
    list monitors;  /* must be second */
    unsigned int num;
} desktop;

/* lifo for minimized clients */
typedef struct lifo {
    node link;      /* must be first */
    client *c;
} lifo;

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
    const int border_width;
} AppRule;

 /* function prototypes sorted alphabetically */
static client *addwindow(xcb_window_t w, xcb_atom_t wtype);
static void adjust_borders(const Arg *arg);
static void adjust_gaps(const Arg *arg);
static void buttonpress(xcb_generic_event_t *e);
static void change_desktop(const Arg *arg);
static bool check_if_window_is_alien(xcb_window_t win, bool *isFloating, xcb_atom_t *wtype);
static bool check_wmproto(xcb_window_t win, xcb_atom_t proto);
static void centerfloating(client *c);
static void centerwindow();
static void cleanup(void);
static void cleanup_display(void);
static int client_borders(const client *c);
static void client_to_desktop(const Arg *arg);
static void clientmessage(xcb_generic_event_t *e);
static void configurerequest(xcb_generic_event_t *e);
static inline alien *create_alien(xcb_window_t win, xcb_atom_t atom);
static client *create_client(xcb_window_t win, xcb_atom_t wtype);
static bool deletewindow(xcb_window_t w);
static void desktopinfo(void);
static void destroynotify(xcb_generic_event_t *e);
static void dualstack(int hh, int cy);
static void enternotify(xcb_generic_event_t *e);
static void equal(int h, int y);
static void fibonacci(int h, int y);
static client *find_client(xcb_window_t w);
static desktop *find_desktop(unsigned int n);
/* static monitor *find_monitor(unsigned int n);  future enhancement */
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
static void mapnotify(xcb_generic_event_t *e);
static void maprequest(xcb_generic_event_t *e);
static void maximize();
static void minimize_client(client *c);
static void minimize();
static void monocle(int h, int y);
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
static void resize_x(const Arg *arg);
static void resize_y(const Arg *arg);
static void restore_client(client *c);
static void restore();
static bool desktop_populated(desktop *d);
static void rotate(const Arg *arg);
static void rotate_client(const Arg *arg);
static void rotate_filled(const Arg *arg);
static void rotate_mode(const Arg *arg);
static void run(void);
static void select_desktop(int i);
static bool sendevent(xcb_window_t win, xcb_atom_t proto);
static void setmaximize(client *c, bool fullscrn);
void setfullscreen(client *c, bool fullscrn);
static int setup(int default_screen);
static void setup_display(void);
static void setwindefattr(xcb_window_t w);
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
static void xerror(xcb_generic_event_t *e);
static alien *wintoalien(list *l, xcb_window_t win);
static client *wintoclient(xcb_window_t w);

#include "config.h"

#ifdef EWMH_TASKBAR
typedef struct
{
    uint16_t left, right, top, bottom;
//    uint16_t left_start_y, left_end_y;
//    uint16_t right_start_y, right_end_y;
//    uint16_t top_start_x, top_end_x;
//    uint16_t bottom_start_x, bottom_end_x;
} strut_t;

static void Setup_EWMH_Taskbar_Support(void);
static void Cleanup_EWMH_Taskbar_Support(void);
static inline void Update_EWMH_Taskbar_Properties(void);
static void Setup_Global_Strut(void);
static void Cleanup_Global_Strut(void);
static inline void Reset_Global_Strut(void);
static void Update_Global_Strut(void);

static strut_t gstrut;
#endif /* EWMH_TASKBAR */

/* variables */
static bool running = true, show = true, showscratchpad = false;
static int default_screen, previous_desktop, current_desktop_number, retval;
static int borders;
static unsigned int numlockmask, win_unfocus, win_focus, win_scratch;
static xcb_connection_t *dis;
static xcb_screen_t *screen;
static uint32_t checkwin;
static xcb_atom_t scrpd_atom;
static client *scrpd = NULL;
static list desktops;
static list aliens;

static desktop *current_desktop = NULL;

static monitor *current_monitor = NULL;
#define M_WW          (current_monitor->ww)
#define M_WH          (current_monitor->wh)
#define M_WY          (current_monitor->wy)

static display *current_display = NULL;
#define M_HEAD        ((client *)(current_display->clients.head))
#define M_TAIL        ((client *)(current_display->clients.tail))

static xcb_ewmh_connection_t *ewmh;
static xcb_atom_t wmatoms[WM_COUNT];
static regex_t appruleregex[LENGTH(rules)];
static xcb_key_symbols_t *keysyms;

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

/*
 * lowlevel doubly linked list functions
 */

static node *rem_node(node *n)
{
    list *l;

    if (!n)
        return NULL;
    l = n->parent;
    if (l) {
        if (n == l->head) {
            l->head = l->head->next;
            if(l->head)
                l->head->prev = NULL;
            else
                l->tail = NULL;
        }
        else if (n == l->tail) {
            l->tail = l->tail->prev;
            l->tail->next = NULL;
        }
        else {
            n->prev->next = n->next;
            n->next->prev = n->prev;
        }
    }
    n->prev = n->next = NULL;
    n->parent = NULL;
    return n;
}

static void add_head(list *l, node *i)
{
    node *o = l->head;
    if (o == NULL) {
        l->head = i;
        l->tail = i;
    }
    else {
        l->head = i;
        i->prev = NULL;
        i->next = o;
        o->prev = i;
    }
    i->parent = l;
}

static void add_tail(list *l, node *i)
{
    if(l->head == NULL)
        add_head(l, i);
    else {
        node *o = l->tail;
        l->tail = i;
        o->next = i;
        i->prev = o;
        i->next = NULL;
    }
    i->parent = l;
}

static void insert_node_after(list *l, node *c, node *i)
{
    node *n;
    if (!c || !(n = c->next))
        add_tail(l, i);
    else {
        c->next = i;
        i->prev = c;
        i->next = n;
        n->prev = i;
    }
    i->parent = l;
}

static void insert_node_before(list *l, node *c, node *i)
{
    node *p;
    if (!c || !(p = c->prev))
        add_head(l, i);
    else {
        p->next = i;
        i->prev = p;
        i->next = c;
        c->prev = i;
    }
    i->parent = l;
}

/*
* glue functions for doubly linked stuff
*/
static inline bool check_head(list *l) { return (l && l->head) ? True : False; }

// static inline bool check_tail(list *l) { return (l && l->tail) ? True : False; }

static inline node *get_head(list *l) { return (l) ? l->head : NULL; }

static inline node *rem_head(list *l) { return (l) ? rem_node(l->head) : NULL; }

static inline node *get_tail(list *l) { return (l) ? l->tail : NULL; }

static inline node *rem_tail(list *l) { return (l) ? rem_node(l->tail) : NULL; }

static inline node *get_next(node *n) { return (n) ? n->next : NULL; }

static inline node *get_prev(node *n) { return (n) ? n->prev : NULL; }

static inline node *get_node_head(node *n) { return (n && n->parent) ? n->parent->head : NULL; }

static inline node *get_node_tail(node *n) { return (n && n->parent) ? n->parent->tail : NULL; }

#define M_GETNEXT(c)  ((client *)get_next(&c->link))
#define M_GETPREV(c)  ((client *)get_prev(&c->link))

/*
 * Add an atom to a list of atoms the given property defines.
 * This is useful, for example, for manipulating _NET_WM_STATE.
 *
 */
void xcb_add_property(xcb_connection_t *con, xcb_window_t win, xcb_atom_t prop, xcb_atom_t atom)
{
    xcb_change_property(con, XCB_PROP_MODE_APPEND, win, prop, XCB_ATOM_ATOM, 32, 1, (uint32_t[]){atom});
}

/*
 * Remove an atom from a list of atoms the given property defines without
 * removing any other potentially set atoms.  This is useful, for example, for
 * manipulating _NET_WM_STATE.
 *
 */
void xcb_remove_property(xcb_connection_t *con, xcb_window_t win, xcb_atom_t prop, xcb_atom_t atom)
{
    xcb_grab_server(con);

    xcb_get_property_reply_t *reply =
        xcb_get_property_reply(con,
                               xcb_get_property(con, False, win, prop, XCB_GET_PROPERTY_TYPE_ANY, 0, 4096), NULL);
    if (reply == NULL || xcb_get_property_value_length(reply) == 0)
        goto release_grab;
    xcb_atom_t *atoms = xcb_get_property_value(reply);
    if (atoms == NULL) {
        goto release_grab;
    }

    {
        int num = 0;
        const int current_size = xcb_get_property_value_length(reply) / (reply->format / 8);
        xcb_atom_t values[current_size];
        for (int i = 0; i < current_size; i++) {
            if (atoms[i] != atom)
                values[num++] = atoms[i];
        }

        xcb_change_property(con, XCB_PROP_MODE_REPLACE, win, prop, XCB_ATOM_ATOM, 32, num, values);
    }

release_grab:
    if (reply)
        free(reply);
    xcb_ungrab_server(con);
}

static bool xcb_check_attribute(xcb_connection_t *con, xcb_window_t win, xcb_atom_t atom)
{
    xcb_get_property_reply_t *prop_reply;
    if ((prop_reply = xcb_get_property_reply(con, xcb_get_property(dis, 0, win, atom,
                                        XCB_GET_PROPERTY_TYPE_ANY, 0, 0), NULL))) {
        xcb_atom_t reply_type = prop_reply->type;
        free(prop_reply);
        if (reply_type != XCB_NONE)
            return True;
    }
    return False;
}

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

/* wrapper to intern atom */
static inline xcb_atom_t xcb_internatom(xcb_connection_t *con, char *name, uint8_t only_if_exists)
{
    xcb_atom_t atom;
    xcb_intern_atom_cookie_t cookie;
    xcb_intern_atom_reply_t *reply;

    atom = 0;
    cookie = xcb_intern_atom(con, only_if_exists, strlen(name), name);
    reply = xcb_intern_atom_reply(con, cookie, NULL);
    if (reply) {
        atom = reply->atom;
        free(reply);
    }
/* TODO: Handle error */

    return atom; // may be zero
}

/* wrapper to move and resize window */
static inline void xcb_move_resize(xcb_connection_t *con, xcb_window_t win, int x, int y, int w, int h, posxy_t *pi)
{
    unsigned int pos[4] = { x, y, w, h };

    if (pi) {
        pi->previous_x = pi->current_x;
        pi->previous_y = pi->current_y;
        pi->current_x = x;
        pi->current_y = y;
    }
    xcb_configure_window(con, win, XCB_MOVE_RESIZE, pos);
}

/* wrapper to move window */
static inline void xcb_move(xcb_connection_t *con, xcb_window_t win, int x, int y, posxy_t *pi)
{
    unsigned int pos[2] = { x, y };

    if (pi) {
        pi->previous_x = pi->current_x;
        pi->previous_y = pi->current_y;
        pi->current_x = x;
        pi->current_y = y;
    }
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

/* wrapper to lower window */
static inline void xcb_lower_window(xcb_connection_t *con, xcb_window_t win)
{
    unsigned int arg[1] = { XCB_STACK_MODE_BELOW };

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
    xcb_keysym_t       keysym;
    keysym = xcb_key_symbols_get_keysym(keysyms, keycode, 0);
    return keysym;
}

/* wrapper to get xcb keycodes from keysymbol (caller must free) */
static xcb_keycode_t *xcb_get_keycodes(xcb_keysym_t keysym)
{
    xcb_keycode_t     *keycode;
    keycode = xcb_key_symbols_get_keycode(keysyms, keysym);
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
        if (!reply)
            errx(EXIT_FAILURE, "failed to register %s atom", names[i]);
        DEBUGP("%s : %d\n", names[i], reply->atom);
        atoms[i] = reply->atom; free(reply);
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

/* wrapper to get window geometry */
static inline xcb_get_geometry_reply_t *get_geometry(xcb_window_t win)
{
    xcb_get_geometry_reply_t *r;
    r = xcb_get_geometry_reply(dis, xcb_get_geometry(dis, win), NULL);
    if (!r)
        errx(EXIT_FAILURE, "failed to get geometry for window %i", win);
    return r;
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
    if (error)
        return 1;
    return 0;
}

/*
static bool window_is_override_redirect(xcb_window_t win)
{
    xcb_get_window_attributes_reply_t *attr[1];
    xcb_window_t windows[] = {win};
    bool override = True;

    xcb_get_attributes(windows, attr, 1);
    if (attr[0]) {
        if (!attr[0]->override_redirect)
            override = False;
        free(attr[0]);
    }
    return override;
}
*/

/* find client in current_display by window id */
static client *find_client(xcb_window_t w)
{
    client *c;
    for (c = (client *)get_head(&current_display->clients);
            c && c->win != w; c = (client *)get_next(&c->link)) ;
    return c;
}

/* find desktop by number */
static desktop *find_desktop(unsigned int n)
{
    desktop *d;
    for (d = (desktop *)get_head(&desktops);
            d && d->num != n; d = (desktop *)get_next(&d->link)) ;
    return d;
}

/* find monitor in current_desktop by number */
/*
static monitor *find_monitor(unsigned int n)
{
    monitor *m;
    for (m = (monitor *)get_head(&current_desktop->monitors);
            m && m->num != n; m = (monitor *)get_next(&m->link)) ;
    return m;
}
*/

static void getparents(client *c, display **di, monitor **mo, desktop **de)
{
    display *disp;
    monitor *moni;
    desktop *desk;
    list *l;

/* backpointer mambo-jambo */
    if (!c) return;
    l = c->link.parent;
    disp = l->master;
    l = disp->link.parent;
    moni = l->master;
    l = moni->link.parent;
    desk = l->master;

    if (di)
        *di = disp;
    if (mo)
        *mo = moni;
    if (de)
        *de = desk;
}

/* create a new client and add the new window
 * window should notify of property change events
 */
client *addwindow(xcb_window_t win, xcb_atom_t wtype)
{
    client *c = create_client(win, wtype);

/* c is valid, else we would not get here */
    if (!check_head(&current_display->clients)) {
        add_head(&current_display->clients, &c->link);
    }
    else {
        if (!ATTACH_ASIDE)
            add_head(&current_display->clients, &c->link);
        else
            add_tail(&current_display->clients, &c->link);
    }
    DEBUG("client added");
    setwindefattr(win);
    return c;
}

/* change the size of the window borders */
void adjust_borders(const Arg *arg)
{
    if (arg->i > 0 || borders >= -arg->i)
        borders += arg->i;
    tile();
    update_current(M_CURRENT);
}

/* change the size of the useless gaps on the fly and re-tile */
void adjust_gaps(const Arg *arg)
{
    int gaps = M_GAPS;

    if (arg->i > 0 || gaps >= -arg->i)
        gaps += arg->i;
    else
        return;

    if (GLOBALGAPS) {
        desktop *desk;
        for (desk = (desktop *)get_head(&desktops); desk; desk = (desktop *)get_next(&desk->link)) {
            monitor *moni;
            for (moni = (monitor *)get_head(&desk->monitors); moni; moni = (monitor *)get_next(&moni->link)) {
                display *disp;
                for (disp = (display *)get_head(&moni->displays); disp; disp = (display *)get_next(&disp->link)) {
                    disp->di.gaps = gaps;
                }
            }
        }
    }
    else
        M_GAPS = gaps;

    tile();
}

/* on the press of a button check to see if there's a binded function to call */
void buttonpress(xcb_generic_event_t *e)
{
    xcb_button_press_event_t *ev = (xcb_button_press_event_t *)e;
    DEBUGP("xcb: button press: %d state: %d\n", ev->detail, ev->state);

    client *c = wintoclient(ev->event);
    if (!c) {
        if(USE_SCRATCHPAD && showscratchpad && scrpd && ev->event == scrpd->win)
            c = scrpd;
        else
            return;
    }

    if (CLICK_TO_FOCUS && M_CURRENT != c && ev->detail == XCB_BUTTON_INDEX_1)
        update_current(c);

    if (c != scrpd) {
        for (unsigned int i = 0; i < LENGTH(buttons); i++)
            if (buttons[i].func && buttons[i].button == ev->detail &&
                CLEANMASK(buttons[i].mask) == CLEANMASK(ev->state)) {
                if (M_CURRENT != c)
                    update_current(c);
                buttons[i].func(&(buttons[i].arg));
            }
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
    if (arg->i == current_desktop_number || arg->i > DESKTOPS-1)
        return;
    previous_desktop = current_desktop_number;
    select_desktop(arg->i);
    if (show) {
        if (M_CURRENT && M_CURRENT != scrpd)
            xcb_move(dis, M_CURRENT->win, M_CURRENT->position_info.previous_x, M_CURRENT->position_info.previous_y, &M_CURRENT->position_info);
        for (client *c = M_HEAD; c; c = M_GETNEXT(c)) {
            if (c != M_CURRENT)
                xcb_move(dis, c->win, c->position_info.previous_x, c->position_info.previous_y, &c->position_info);
        }
    }
    select_desktop(previous_desktop);
    for (client *c = M_HEAD; c; c = M_GETNEXT(c)) {
        if (c != M_CURRENT)
            xcb_move(dis, c->win, -2 * M_WW, 0, &c->position_info);
    }
    if (M_CURRENT && M_CURRENT != scrpd)
        xcb_move(dis, M_CURRENT->win, -2 * M_WW, 0, &M_CURRENT->position_info);
    select_desktop(arg->i);
    update_current(M_CURRENT);
    desktopinfo();
    xcb_ewmh_set_current_desktop(ewmh, default_screen, arg->i);
}

static void print_window_type(xcb_window_t w, xcb_atom_t a)
{
    char *s;

    if      (a == ewmh->_NET_WM_WINDOW_TYPE_DESKTOP)        s = "_NET_WM_WINDOW_TYPE_DESKTOP";
    else if (a == ewmh->_NET_WM_WINDOW_TYPE_DOCK)           s = "_NET_WM_WINDOW_TYPE_DOCK";
    else if (a == ewmh->_NET_WM_WINDOW_TYPE_TOOLBAR)        s = "_NET_WM_WINDOW_TYPE_TOOLBAR";
    else if (a == ewmh->_NET_WM_WINDOW_TYPE_MENU)           s = "_NET_WM_WINDOW_TYPE_MENU";
    else if (a == ewmh->_NET_WM_WINDOW_TYPE_UTILITY)        s = "_NET_WM_WINDOW_TYPE_UTILITY";
    else if (a == ewmh->_NET_WM_WINDOW_TYPE_SPLASH)         s = "_NET_WM_WINDOW_TYPE_SPLASH";
    else if (a == ewmh->_NET_WM_WINDOW_TYPE_DIALOG)         s = "_NET_WM_WINDOW_TYPE_DIALOG";
    else if (a == ewmh->_NET_WM_WINDOW_TYPE_DROPDOWN_MENU)  s = "_NET_WM_WINDOW_TYPE_DROPDOWN_MENU";
    else if (a == ewmh->_NET_WM_WINDOW_TYPE_POPUP_MENU)     s = "_NET_WM_WINDOW_TYPE_POPUP_MENU";
    else if (a == ewmh->_NET_WM_WINDOW_TYPE_TOOLTIP)        s = "_NET_WM_WINDOW_TYPE_TOOLTIP";
    else if (a == ewmh->_NET_WM_WINDOW_TYPE_NOTIFICATION)   s = "_NET_WM_WINDOW_TYPE_NOTIFICATION";
    else if (a == ewmh->_NET_WM_WINDOW_TYPE_COMBO)          s = "_NET_WM_WINDOW_TYPE_COMBO";
    else if (a == ewmh->_NET_WM_WINDOW_TYPE_DND)            s = "_NET_WM_WINDOW_TYPE_DND";
    else if (a == ewmh->_NET_WM_WINDOW_TYPE_NORMAL)         s = "_NET_WM_WINDOW_TYPE_NORMAL";
    else s = "undefined window type";

    if (w && s) {
        DEBUGP("window %x has type %s\n", w, s);
    }
}

/*
 * returns:
 * True if window is alien
 * False if window will be client
 */
static bool check_if_window_is_alien(xcb_window_t win, bool *isFloating, xcb_atom_t *wtype)
{
    xcb_get_window_attributes_reply_t *attr[1];
    xcb_window_t windows[] = {win};
    bool isAlien = False;

    if (isFloating) *isFloating = False;
    if (wtype) *wtype = ewmh->_NET_WM_WINDOW_TYPE_NORMAL;

    xcb_get_attributes(windows, attr, 1);
    if (!attr[0])   /* dead on arrival */
        return True;

    if (attr[0]->override_redirect) {
        free(attr[0]);
        return True;
    }
    else
        free(attr[0]);

    /*
     * check if window type is not _NET_WM_WINDOW_TYPE_NORMAL.
     * if yes, then we add it to alien list and map it.
     */
    xcb_ewmh_get_atoms_reply_t type;
    xcb_atom_t atype = 0;
    if (xcb_ewmh_get_wm_window_type_reply(ewmh,
                                xcb_ewmh_get_wm_window_type(ewmh,
                                win), &type, NULL) == 1) {
        if (wtype) *wtype = type.atoms[0];
        for (unsigned int i = 0; i < type.atoms_len; i++) {
print_window_type(win, type.atoms[i]);
            if (type.atoms[i] == ewmh->_NET_WM_WINDOW_TYPE_NORMAL) {
                isAlien = False;
                break;
            }
            if (type.atoms[i] == ewmh->_NET_WM_WINDOW_TYPE_DIALOG) {
                if (wtype)      *wtype = type.atoms[i];
                if (isFloating) *isFloating = True;
                isAlien = False;
                break;
            }
            else {
                if (atype == 0)
                    atype = type.atoms[i];
                isAlien = True;
            }
        }
        xcb_ewmh_get_atoms_reply_wipe(&type);
    }
    if (isAlien)
        create_alien(win, atype);

    return isAlien;
}

/*
 * place a floating client in the center of the screen
 */
static void centerfloating(client *c)
{
    if (!c || !c->isfloating)
        return;

    xcb_get_geometry_reply_t *wa = get_geometry(c->win);
    xcb_raise_window(dis, c->win);
    xcb_move(dis, c->win, ((M_WW - wa->width) / 2) - c->borderwidth,
                     ((M_WH - wa->height) / 2) - c->borderwidth, &c->position_info);
    free(wa);
}

/*
 * centerfloating(); wrapper
 */
void centerwindow(void)
{
    if (!M_CURRENT)
        return;

    if (!M_CURRENT->isfloating
     && !M_CURRENT->istransient) {
        float_client(M_CURRENT);
        tile();
    }

    centerfloating(M_CURRENT);
}

/* remove all windows in all desktops by sending a delete message */
void cleanup(void)
{
#ifdef EWMH_TASKBAR
    Cleanup_Global_Strut();
    Cleanup_EWMH_Taskbar_Support();
#endif /* EWMH_TASKBAR */

    if(USE_SCRATCHPAD && scrpd) {
        if(CLOSE_SCRATCHPAD) {
            deletewindow(scrpd->win);
        }
        else {
            xcb_border_width(dis, scrpd->win, 0);
            xcb_get_geometry_reply_t *wa = get_geometry(scrpd->win);
            xcb_move(dis, scrpd->win, (M_WW - wa->width) / 2, (M_WH - wa->height) / 2, &scrpd->position_info);
            free(wa);
        }
        free(scrpd);
        scrpd = NULL;
    }

    xcb_ewmh_connection_wipe(ewmh);
    free(ewmh);

    xcb_delete_property(dis, screen->root, ewmh->_NET_SUPPORTED);
    xcb_destroy_window(dis, checkwin);

    for (unsigned int i = 0; i < LENGTH(rules); i++)
        regfree(&appruleregex[i]);

    cleanup_display();

    alien *a;
    while ((a = (alien *)rem_head(&aliens)))
        free(a);
}

static void cleanup_display(void)
{
    desktop *desk;
    for (desk = (desktop *)rem_head(&desktops); desk; desk = (desktop *)rem_head(&desktops)) {
        monitor *moni;
        for (moni = (monitor *)rem_head(&desk->monitors); moni; moni = (monitor *)rem_head(&desk->monitors)) {
            display *disp;
            for (disp = (display *)rem_head(&moni->displays); disp; disp = (display *)rem_head(&moni->displays)) {
                client *c;
                for (c = (client *)rem_head(&disp->clients); c; c = (client *)rem_head(&disp->clients)) {
                    xcb_border_width(dis, c->win, 0);
                    free(c);
                }
                for (struct lifo *l = (lifo *)rem_head(&disp->miniq); l; l = (lifo *)rem_head(&disp->miniq))
                    free(l);

                free(disp);
            }
            free(moni);
        }
        free(desk);
    }
}

/*
 * return the border width of a client for calculations
 *
 * this will just return the current global border width if this client does
 * not use a custom one
 */
int client_borders(const client *c)
{
    return c->borderwidth >= 0 ? c->borderwidth : borders;
}

/* move a client to another desktop
 *
 * remove the current client from the current desktop's client list
 * and add it as last client of the new desktop's client list */
void client_to_desktop(const Arg *arg)
{
    if (!M_CURRENT || arg->i == current_desktop_number || arg->i > DESKTOPS-1)
        return;
    int cd = current_desktop_number;
    client *c = M_CURRENT;

    rem_node(&c->link);
    select_desktop(arg->i);
    add_tail(&current_display->clients, &c->link);
    select_desktop(cd);
    xcb_move(dis, c->win, -2 * M_WW, 0, &c->position_info);
    xcb_ewmh_set_wm_desktop(ewmh, c->win, arg->i);

    if (FOLLOW_WINDOW)
        change_desktop(arg);
    else
        update_current(M_PREVFOCUS);

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
    client *c = wintoclient(ev->window);

    DEBUG("xcb: client message");

    if (c && ev->type == ewmh->_NET_WM_STATE) {
        if (((unsigned)ev->data.data32[1] == ewmh->_NET_WM_STATE_FULLSCREEN
          || (unsigned)ev->data.data32[2] == ewmh->_NET_WM_STATE_FULLSCREEN)) {
            uint32_t mode = ev->data.data32[0];

            if (mode == _NET_WM_STATE_TOGGLE)
                mode = (c->isfullscreen) ? _NET_WM_STATE_REMOVE : _NET_WM_STATE_ADD;
            setfullscreen(c, mode == _NET_WM_STATE_ADD);
        }
        if (((unsigned)ev->data.data32[1] == ewmh->_NET_WM_STATE_HIDDEN
          || (unsigned)ev->data.data32[2] == ewmh->_NET_WM_STATE_HIDDEN)) {
            switch (ev->data.data32[0]) {
                case _NET_WM_STATE_REMOVE:
                    restore_client(c);
                break;

                case _NET_WM_STATE_ADD:
                    minimize_client(c);
                break;

                case _NET_WM_STATE_TOGGLE:
                    if (c->isminimized)
                        restore_client(c);
                    else
                        minimize_client(c);
                break;
            }
        }
    }
    else {
        if (ev->type == ewmh->_NET_CURRENT_DESKTOP
            && ev->data.data32[0] < DESKTOPS)
            change_desktop(&(Arg){.i = ev->data.data32[0]});
        else {
            if (c && ev->type == ewmh->_NET_CLOSE_WINDOW)
                killclient(c);
            else {
                if (ev->type == ewmh->_NET_ACTIVE_WINDOW) {
                    if (c) {
                        client *t = NULL;
                        for (t = M_HEAD; t && t != c; t = M_GETNEXT(t))
                            ;
                        if (t)
                            update_current(c);
                    }
                    else {
                        if (showscratchpad && scrpd && scrpd->win == ev->window)
                            update_current(scrpd);
                    }
                }
                else {
                    if (c && ev->type == ewmh->_NET_WM_DESKTOP
                          && ev->data.data32[0] < DESKTOPS) {
                        client_to_desktop(&(Arg){.i = ev->data.data32[0]});
                    }
                }
            }
        }
    }
}

/* a configure request means that the window requested changes in its geometry
 * state. if the window is maximize, discard and fill the screen else set the
 * appropriate values as requested, and tile the window again so that it fills
 * the gaps that otherwise could have been created
 */
void configurerequest(xcb_generic_event_t *e)
{
    xcb_configure_request_event_t *ev = (xcb_configure_request_event_t *)e;
    client *c = wintoclient(ev->window);

    DEBUG("xcb: configure request");

    if (c && c->ismaximized) {
        setmaximize(c, true);
    } else {
        unsigned int v[7];
        unsigned int i = 0;
        int borders = c ? client_borders(c) : 0;
        if (ev->value_mask & XCB_CONFIG_WINDOW_X)
            v[i++] = ev->x;
        if (ev->value_mask & XCB_CONFIG_WINDOW_Y) {
            int y = ev->y;
            if (c && c->type == ewmh->_NET_WM_WINDOW_TYPE_NORMAL) {
#ifndef EWMH_TASKBAR
                if (M_SHOWPANEL && TOP_PANEL && y < PANEL_HEIGHT)
#else
                if (y < M_WY)
#endif /* EWMH_TASKBAR */
                     y = PANEL_HEIGHT;
            }
            v[i++] = y;
        }
        if (ev->value_mask & XCB_CONFIG_WINDOW_WIDTH)
            v[i++] = (ev->width < M_WW - borders) ? ev->width : M_WW + borders;
        if (ev->value_mask & XCB_CONFIG_WINDOW_HEIGHT)
            v[i++] = (ev->height < M_WH - borders) ? ev->height : M_WH + borders;
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

static inline alien *create_alien(xcb_window_t win, xcb_atom_t atom)
{
    alien *a;
    if((a = (alien *)calloc(1, sizeof(alien)))) {
        unsigned int values[1] = { XCB_EVENT_MASK_PROPERTY_CHANGE };
        xcb_change_window_attributes(dis, win, XCB_CW_EVENT_MASK, values);
        a->win = win;
        a->type = atom;
        add_tail(&aliens, &a->link);
        xcb_raise_window(dis, win);
        xcb_map_window(dis, win);

        xcb_get_geometry_reply_t *g = get_geometry(a->win);
        a->position_info.previous_x = a->position_info.current_x = g->x;
        a->position_info.previous_y = a->position_info.current_y = g->y;
        free(g);
    }
    return(a);
}

/*
 * allocate client structure and fill in sane defaults
 * exit FrankenWM if memory allocation fails
 */
static client *create_client(xcb_window_t win, xcb_atom_t wtype)
{
    xcb_icccm_wm_hints_t hints;
    client *c = calloc(1, sizeof(client));
    if (!c)
        err(EXIT_FAILURE, "cannot allocate client");
    c->isurgent = False;
    c->istransient = False;
    c->isfullscreen = False;
    c->ismaximized = False;
    c->isfloating = False;
    c->isminimized = False;
    c->win = win;
    c->type = wtype;
    c->dim[0] = c->dim[1] = 0;
    c->borderwidth = -1;    /* default: use global border width */
    c->setfocus = True;     /* default: prefer xcb_set_input_focus(); */
    if (xcb_icccm_get_wm_hints_reply(dis,
            xcb_icccm_get_wm_hints(dis, win), &hints, NULL))
        c->setfocus = (hints.input) ? True : False;

    xcb_get_geometry_reply_t *g = get_geometry(c->win);
    c->position_info.previous_x = c->position_info.current_x = g->x;
    c->position_info.previous_y = c->position_info.current_y = g->y;
    free(g);

    return c;
}

static void create_display(client *c)
{
    desktop *desk=NULL;
    monitor *moni=NULL;
    display *disp=NULL, *new;

    if (!c)
        return;
    getparents(c, &disp, &moni, &desk);     /* get the client's display, monitor and desktop. */
    if (!(new = calloc(1, sizeof(display))))
        err(EXIT_FAILURE, "cannot allocate new display");
    new->clients.master = new;  /* backpointer */
    rem_node(&c->link);          /* unlink client from its display client list. */
    for (client *t = M_HEAD; t; t = M_GETNEXT(t))   /* hide current windows */
        xcb_move(dis, t->win, -2 * M_WW, 0, &t->position_info);
    for (alien *t = (alien *)get_head(&aliens); t; t = (alien *)get_next(&t->link))   /* hide aliens */
        xcb_move(dis, t->win, -2 * M_WW, 0, &t->position_info);
    add_head(&new->clients, &c->link);      /* set client as head in new display */
    add_head(&moni->displays, &new->link);  /* set new display as head. */
    select_desktop(current_desktop_number); /* update global pointers */
    memcpy(&new->di, &disp->di, sizeof(displayinfo));   /* copy settings */
    current_display = new;                  /* update current display pointer */
    update_current(NULL);                   /* update focus and tiling */
}

/* close the window */
bool deletewindow(xcb_window_t win)
{
    return sendevent(win, wmatoms[WM_DELETE_WINDOW]);
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
#ifndef EWMH_TASKBAR
    bool urgent = false;
    int cd = current_desktop_number, n = 0, d = 0;
    xcb_get_property_cookie_t cookie;
    xcb_ewmh_get_utf8_strings_reply_t wtitle;
    wtitle.strings = NULL;

    if (M_CURRENT) {
        cookie = xcb_ewmh_get_wm_name_unchecked(ewmh, M_CURRENT->win);
        xcb_ewmh_get_wm_name_reply(ewmh, cookie, &wtitle, (void *)0);
    }

    for (client *c; d < DESKTOPS; d++) {
        for (select_desktop(d), c = M_HEAD, n = 0, urgent = false;
             c; c = M_GETNEXT(c), ++n)
            if (c->isurgent)
                urgent = true;
        fprintf(stdout, "%d:%d:%d:%d:%d ", d, n, M_MODE, current_desktop_number == cd,
                urgent);
        if (d + 1 == DESKTOPS)
            fprintf(stdout, "%s\n", M_CURRENT && OUTPUT_TITLE && wtitle.strings ?
                    wtitle.strings : "");
    }

    if (wtitle.strings) {
        xcb_ewmh_get_utf8_strings_reply_wipe(&wtitle);
    }

    fflush(stdout);
    if (cd != d - 1)
        select_desktop(cd);
#else
    Update_EWMH_Taskbar_Properties();
#endif /* EWMH_TASKBAR */
}

static void destroy_display(client *c)
{
    desktop *desk=NULL;
    monitor *moni=NULL;
    display *disp=NULL, *next;

    getparents(c, &disp, &moni, &desk);     /* get the client's display, monitor and desktop. */
    if (!(next = (display *)get_next(&disp->link)))     /* cannot destroy the last display. */
        return;
    for (client *t = (client *)rem_head(&disp->clients); t; t = (client *)rem_head(&disp->clients)) {
    /* relink entire clientlist to the tail of next display clientlist. */
        add_tail(&next->clients, &t->link);
    }
    for (lifo *t = (lifo *)rem_head(&disp->miniq); t; t = (lifo *)rem_head(&disp->miniq)) {
    /* relink minimized clients to the tail of next display clientlist. */
        xcb_move(dis, t->c->win, t->c->position_info.previous_x,
                                 t->c->position_info.previous_y, NULL);
        t->c->position_info.previous_x = t->c->position_info.current_x;
        t->c->position_info.previous_y = t->c->position_info.current_y;
        add_tail(&next->clients, &t->c->link);
        t->c->isminimized = False;
        xcb_remove_property(dis, t->c->win, ewmh->_NET_WM_STATE, ewmh->_NET_WM_STATE_HIDDEN);
        free(t);

    }
    rem_node(&disp->link);                   /* unlink now empty display */
    select_desktop(current_desktop_number);     /* update global pointers */
    for (alien *t = (alien *)get_head(&aliens); t; t = (alien *)get_next(&t->link))   /* show aliens */
        xcb_move(dis, t->win, t->position_info.previous_x, t->position_info.previous_y, &t->position_info);
    if (current_display == next) {
        update_current(c);
    }
    free(disp);
}

/*
 * a destroy notification is received when a window is being closed
 * on receival, remove the appropriate client that held that window
 */
void destroynotify(xcb_generic_event_t *e)
{
    xcb_destroy_notify_event_t *ev = (xcb_destroy_notify_event_t *)e;
    client *c = wintoclient(ev->window);

    DEBUG("xcb: destroy notify");

    if (c) {
        if (c->isfullscreen)
            destroy_display(c);
        removeclient(c);
    }
    else if (USE_SCRATCHPAD && scrpd && ev->window == scrpd->win) {
        free(scrpd);
        scrpd = NULL;
        update_current(M_CURRENT);
    }
   else {
        alien *a;

        if((a = wintoalien(&aliens, ev->window))) {
            DEBUG("unlink selfmapped window");
            rem_node(&a->link);
            free(a);
        }
    }
    desktopinfo();
}

/* dualstack layout (three-column-layout, tcl in dwm) */
void dualstack(int hh, int cy)
{
    client *c = NULL, *t = NULL;
    int n = 0, z = hh, d = 0, l = 0, r = 0, cb = cy,
        ma = (M_INVERT ? M_WH : M_WW) * MASTER_SIZE + M_MASTER_SIZE;

    /* count stack windows and grab first non-floating, non-maximize window */
    for (t = M_HEAD; t; t = M_GETNEXT(t)) {
        if (!ISFMFTM(t)) {
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
        int borders = client_borders(c);
        xcb_move_resize(dis, c->win, M_GAPS, cy + M_GAPS,
                        M_WW - 2 * (borders + M_GAPS),
                        hh - 2 * (borders + M_GAPS), &c->position_info);
        return;
    }

    /* tile the first non-floating, non-maximize window to cover the master area */
    int borders = client_borders(c);
    if (current_display->di.invert)
        xcb_move_resize(dis, c->win, M_GAPS,
                        cy + (hh - ma) / 2 + M_GAPS,
                        M_WW - 2 * (borders + M_GAPS),
                        n > 1 ? ma - 2 * M_GAPS - 2 * borders
                              : ma + (hh - ma) / 2 - 2 * borders - 2 * M_GAPS, &c->position_info);
    else
        xcb_move_resize(dis, c->win, (M_WW - ma) / 2 + borders + M_GAPS,
                        cy + M_GAPS,
                        n > 1 ? (ma - 4 * borders - 2 * M_GAPS)
                              : (ma + (M_WW - ma) / 2 - 3 * borders - 2 * M_GAPS),
                        hh - 2 * (borders + M_GAPS), &c->position_info);

    int cx = M_GAPS,
        cw = (M_WW - ma) / 2 - borders - M_GAPS,
        ch = z;
        cy += M_GAPS;

    /* tile the non-floating, non-maximize stack windows */
    for (c = M_GETNEXT(c); c; c = M_GETNEXT(c)) {
        for (d = 0, t = M_HEAD; t != c; t = M_GETNEXT(t), d++);
        if (ISFMFTM(c))
            continue;
        int borders = client_borders(c);
        if (M_INVERT) {
            if (d == l + 1) /* we are on the -right- bottom stack, reset cy */
                cx = M_GAPS;
            if (d > 1 && d != l + 1)
                cx += (M_WW - M_GAPS) / (d <= l ? l : r);
            xcb_move_resize(dis, c->win,
                        cx, (d <= l) ? cy : cy + (hh - ma) / 2 + ma - M_GAPS,
                        (M_WW - M_GAPS) / (d <= l ? l : r) - 2 * borders - M_GAPS,
                        (hh - ma) / 2 - 2 * borders - M_GAPS, &c->position_info);
        } else {
            if (d == l + 1) /* we are on the right stack, reset cy */
                cy = cb + M_GAPS;
            if (d > 1 && d != l + 1)
                cy += (ch - M_GAPS) / (d <= l ? l : r);
            xcb_move_resize(dis, c->win,
                        d <= l ? cx : M_WW - cw - 2 * borders - M_GAPS, cy, cw,
                        (ch - M_GAPS) / (d <= l ? l : r) - 2 * borders - M_GAPS, &c->position_info);
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

    DEBUG("xcb: enter notify");

    if (!FOLLOW_MOUSE)
        return;

    DEBUG("event is valid");

    if(USE_SCRATCHPAD && showscratchpad && scrpd && ev->event == scrpd->win) {
        update_current(scrpd);
    }
    else {
        client *c = wintoclient(ev->event);

        if (c
         && ev->mode == XCB_NOTIFY_MODE_NORMAL
         && c != M_CURRENT
         && ev->detail != XCB_NOTIFY_DETAIL_INFERIOR) {
            update_current(c);
        }
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

    for (client *c = M_HEAD; c; c = M_GETNEXT(c)) {
        if (ISFMFTM(c))
            continue;
        n++;
    }

    for (client *c = M_HEAD; c; c = M_GETNEXT(c)) {
        int borders = client_borders(c);
        if (ISFMFTM(c))
            continue;
        else
            j++;
        if (M_INVERT)
            xcb_move_resize(dis, c->win, M_GAPS,
                            y + h / n * j + (c == M_HEAD ? M_GAPS : 0),
                            M_WW - 2 * borders - 2 * M_GAPS,
                            h / n - 2 * borders - (c == M_HEAD ? 2 : 1) * M_GAPS, &c->position_info);
        else
            xcb_move_resize(dis, c->win, M_WW / n * j + (c == M_HEAD ? M_GAPS : 0),
                            y + M_GAPS,
                            M_WW / n - 2 * borders - (c == M_HEAD ? 2 : 1) * M_GAPS,
                            h - 2 * borders - 2 * M_GAPS, &c->position_info);
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
    int borders = borders;
    if (M_HEAD)
        borders = client_borders(M_HEAD);

    int j = -1, x = M_GAPS, tt = 0,
        cw = M_WW - 2 * M_GAPS - 2 * borders,
        ch = h - 2 * M_GAPS - 2 * borders;

    for (client *n, *c = M_HEAD; c; c = M_GETNEXT(c)) {
        int borders = client_borders(c);
        if (ISFMFTM(c))
            continue;
        else
            j++;
        for (n = M_GETNEXT(c); n; n = M_GETNEXT(n))
            if (!ISFMFTM(n))
                break;

        /*
         * not the last window in stack ? -> half the client size, and also
         * check if we have too many windows to keep them larger than MINWSZ
         */
        if (n
            && ch > MINWSZ * 2 + borders + M_GAPS
            && cw > MINWSZ * 2 + borders + M_GAPS) {
            (j & 1) ? (ch = ch / 2 - borders - M_GAPS / 2)
                    : (cw = cw / 2 - borders - M_GAPS / 2);
            tt = j;
        }

        /* not the master client ? -> shift client right or down (or up) */
        if (j) {
            (j & 1) ? (x = x + cw + 2 * borders + M_GAPS)
                    : (y = M_INVERT ? (y - ch - 2 * borders - M_GAPS)
                                    : (y + ch + 2 * borders + M_GAPS));

            if (j & 1 && n && M_INVERT)
                y += ch + 2 * borders + M_GAPS;
        }

        /* if the window does not fit in the stack, do not jam it in there */
        if (j <= tt + 1)
            xcb_move_resize(dis, c->win, x, y + M_GAPS, cw, ch, &c->position_info);
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

    if (!arg->i || !M_CURRENT)
        return;

    if (!M_CURRENT->isfloating) {
        float_client(M_CURRENT);
        tile();
    }

    r = get_geometry(M_CURRENT->win);
    r->x += arg->i;
    xcb_move(dis, M_CURRENT->win, r->x, r->y, &M_CURRENT->position_info);
    free(r);
}

/*
 * handles y-movement of floating windows
 */
void float_y(const Arg *arg)
{
    xcb_get_geometry_reply_t *r;

    if (!arg->i || !M_CURRENT)
        return;

    if (!M_CURRENT->isfloating) {
        float_client(M_CURRENT);
        tile();
    }

    r = get_geometry(M_CURRENT->win);
    r->y += arg->i;
    xcb_move(dis, M_CURRENT->win, r->x, r->y, &M_CURRENT->position_info);
    free(r);
}

/*
 * focus the (first) master window, or switch back to the slave previously
 * focussed, toggling between them
 */
void focusmaster()
{
    if (!M_HEAD
     || !M_CURRENT
     || (M_CURRENT == M_HEAD && !M_GETNEXT(M_HEAD))
     || !M_PREVFOCUS
     ||  M_PREVFOCUS->isminimized)
        return;

    /* fix for glitchy toggle behaviour between head and head->next */
    if (M_CURRENT == M_GETNEXT(M_HEAD))
        M_PREVFOCUS = M_CURRENT;

    if (M_CURRENT == M_HEAD)
        update_current(M_PREVFOCUS);
    else
        update_current(M_HEAD);
}

/* find and focus the client which received
 * the urgent hint in the current desktop */
void focusurgent()
{
    client *c;
    int cd = current_desktop_number, d = 0;

    for (c = M_HEAD; c && !c->isurgent; c = M_GETNEXT(c));
    if (c) {
        update_current(c);
        return;
    } else {
        for (bool f = false; d < DESKTOPS && !f; d++) {
            for (select_desktop(d), c = M_HEAD; c && !(f = c->isurgent); c = M_GETNEXT(c))
                ;
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
    if (!c)
        return;

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
/*
void grabbuttons(client *c)
{
    if (!c)
        return;

    xcb_ungrab_button(dis, XCB_BUTTON_INDEX_ANY, c->win, XCB_GRAB_ANY);
    if (CLICK_TO_FOCUS)
        xcb_grab_button(dis, 1, c->win, XCB_EVENT_MASK_BUTTON_PRESS,
                        XCB_GRAB_MODE_SYNC, XCB_GRAB_MODE_ASYNC,
                        XCB_WINDOW_NONE, XCB_CURSOR_NONE,
                        (c == scrpd) ? XCB_BUTTON_INDEX_1 : XCB_BUTTON_INDEX_ANY,
                        XCB_BUTTON_MASK_ANY);
    else {
        unsigned int modifiers[] = { 0, XCB_MOD_MASK_LOCK, numlockmask,
                                     numlockmask|XCB_MOD_MASK_LOCK };

        for (unsigned int b = 0; b < LENGTH(buttons); b++) {
            for (unsigned int m = 0; m < LENGTH(modifiers); m++) {
                    xcb_grab_button(dis, 1, c->win, XCB_EVENT_MASK_BUTTON_PRESS,
                                    XCB_GRAB_MODE_SYNC, XCB_GRAB_MODE_ASYNC,
                                    XCB_WINDOW_NONE, XCB_CURSOR_NONE,
                                    buttons[b].button,
                                    buttons[b].mask|modifiers[m]);
            }
        }
    }
}
*/

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

    for (client *c = M_HEAD; c; c = M_GETNEXT(c))
        if (!ISFMFTM(c))
            ++n;
    if (!n)
        return;
    for (cols = 0; cols <= n / 2; cols++)
        if (cols * cols >= n)
            break; /* emulate square root */
    if (n == 5)
        cols = 2;

    int rows = n / cols,
        ch = hh - M_GAPS,
        cw = (M_WW - M_GAPS) / (cols ? cols : 1);
    for (client *c = M_HEAD; c; c = M_GETNEXT(c)) {
        int borders = client_borders(c);
        if (ISFMFTM(c))
            continue;
        else
            ++i;
        if (i / rows + 1 > cols - n % cols)
            rows = n / cols + 1;
        xcb_move_resize(dis, c->win, cn * cw + M_GAPS,
                        cy + rn * ch / rows + M_GAPS,
                        cw - 2 * borders - M_GAPS,
                        ch / rows - 2 * borders - M_GAPS, &c->position_info);
        if (++rn >= rows) {
            rn = 0;
            cn++;
        }
    }
}

/* invert v-stack left-right */
void invertstack()
{
    M_INVERT = !M_INVERT;
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
    if (!M_CURRENT)
        return;
    if (!deletewindow(M_CURRENT->win)) {
        xcb_kill_client(dis, M_CURRENT->win);
        DEBUG("client killed");
    }
    else {
        DEBUG("client deleted");
    }
    removeclient(M_CURRENT);
}

static bool check_wmproto(xcb_window_t win, xcb_atom_t proto)
{
    xcb_icccm_get_wm_protocols_reply_t reply;
    bool got = false;

    if (xcb_icccm_get_wm_protocols_reply(dis,
        xcb_icccm_get_wm_protocols(dis, win, wmatoms[WM_PROTOCOLS]), &reply, NULL)) {
        /* TODO: Handle error? */
        unsigned int n;
        for (n = 0; n != reply.atoms_len; ++n)
            if ((got = reply.atoms[n] == proto))
                break;
        xcb_icccm_get_wm_protocols_reply_wipe(&reply);
    }
    return got;
}

/* focus the previously focused desktop */
void last_desktop()
{
    change_desktop(&(Arg){.i = previous_desktop});
}

void mapnotify(xcb_generic_event_t *e)
{
    xcb_map_notify_event_t *ev = (xcb_map_notify_event_t *)e;

    DEBUG("xcb: map notify");

    if (wintoclient(ev->window) || (scrpd && scrpd->win == ev->window))
        return;

    xcb_window_t wins[] = {ev->window};
    xcb_get_window_attributes_reply_t *attr[1];

    xcb_get_attributes(wins, attr, 1);
    if (!attr[0])
        return;     /* dead on arrival */
    if (attr[0]->override_redirect) {
        free(attr[0]);
        return;
    }
    else
        free(attr[0]);

    if (wintoalien(&aliens, ev->window)) {
        DEBUG("alien window already in list");
        return;
    }

    xcb_ewmh_get_atoms_reply_t type;
    if (xcb_ewmh_get_wm_window_type_reply(ewmh,
                                xcb_ewmh_get_wm_window_type(ewmh,
                                ev->window), &type, NULL) == 1) {
        create_alien(ev->window, type.atoms[0]);
        xcb_ewmh_get_atoms_reply_wipe(&type);
        DEBUG("caught a new selfmapped window");
    }
    else {
        DEBUG("alien has no _NET_WM_WINDOW_TYPE property");
    }
}

/* a map request is received when a window wants to display itself
 * if the window has override_redirect flag set then it should not be handled
 * by the wm. if the window already has a client then there is nothing to do.
 *
 * get the window class and name instance and try to match against an app rule.
 * create a client for the window, that client will always be current.
 * check for transient state, and maximize state and the appropriate values.
 * if the desktop in which the window was spawned is the current desktop then
 * display the window, else, if set, focus the new desktop.
 */
void maprequest(xcb_generic_event_t *e)
{
    xcb_map_request_event_t            *ev = (xcb_map_request_event_t *)e;
    xcb_window_t                       transient = 0;
    xcb_get_property_reply_t           *prop_reply;
    xcb_get_property_cookie_t          cookie;
    xcb_ewmh_get_utf8_strings_reply_t  wtitle;
    xcb_atom_t                         wtype = ewmh->_NET_WM_WINDOW_TYPE_NORMAL;
    client *c;
    bool isFloating = False;

    DEBUG("xcb: map request");

    if ((c = wintoclient(ev->window))) {
        if (!find_client(c->win)) {     /* client is on different display */
            rem_node(&c->link);
            add_tail(&current_display->clients, &c->link);
        }
        xcb_map_window(dis, c->win);
        update_current(c);
        return;
    }

    if (check_if_window_is_alien(ev->window, &isFloating, &wtype))
        return;

    xcb_remove_property(dis, ev->window, ewmh->_NET_WM_STATE, ewmh->_NET_WM_STATE_HIDDEN);

    DEBUG("event is valid");

    bool follow = false;
    int cd = current_desktop_number, newdsk = current_desktop_number, border_width = -1;

    cookie = xcb_ewmh_get_wm_name_unchecked(ewmh, ev->window);

    if (xcb_ewmh_get_wm_name_reply(ewmh, cookie, &wtitle, (void *)0)) {
        DEBUGP("EWMH window title: %s\n", wtitle.strings);

        if (!strcmp(wtitle.strings, SCRPDNAME)) {
            scrpd = create_client(ev->window, wtype);
            setwindefattr(scrpd->win);
            grabbuttons(scrpd);

            xcb_move(dis, scrpd->win, -2 * M_WW, 0, &scrpd->position_info);
            xcb_map_window(dis, scrpd->win);
            xcb_ewmh_get_utf8_strings_reply_wipe(&wtitle);

            if (scrpd_atom)
                xcb_change_property(dis, XCB_PROP_MODE_REPLACE, scrpd->win, scrpd_atom,
                                    XCB_ATOM_WINDOW, 32, 1, &scrpd->win);
            return;
        }

        for (unsigned int i = 0; i < LENGTH(appruleregex); i++)
            if (!regexec(&appruleregex[i], &wtitle.strings[0], 0, NULL, 0)) {
                follow = rules[i].follow;
                newdsk = (rules[i].desktop < 0 ||
                          rules[i].desktop >= DESKTOPS) ? current_desktop_number
                                                        : rules[i].desktop;
                isFloating = rules[i].floating;
                border_width = rules[i].border_width;
                break;
            }

        xcb_ewmh_get_utf8_strings_reply_wipe(&wtitle);
    }

    if (cd != newdsk)
        select_desktop(newdsk);
    c = addwindow(ev->window, wtype);

    xcb_icccm_get_wm_transient_for_reply(dis,
                    xcb_icccm_get_wm_transient_for_unchecked(dis, ev->window),
                    &transient, NULL); /* TODO: error handling */
    c->istransient = transient ? true : false;
    c->isfloating  = isFloating || c->istransient;
    c->borderwidth = border_width;

    prop_reply = xcb_get_property_reply(dis, xcb_get_property_unchecked(
                                    dis, 0, ev->window, ewmh->_NET_WM_STATE,
                                    XCB_ATOM_ATOM, 0, 1), NULL);
                                    /* TODO: error handling */
    if (prop_reply) {
        if (prop_reply->format == 32) {
            xcb_atom_t *v = xcb_get_property_value(prop_reply);
            for (unsigned int i = 0; i < prop_reply->value_len; i++) {
                DEBUGP("%d : %d\n", i, v[i]);
                if (v[i] == ewmh->_NET_WM_STATE_FULLSCREEN)
                    setfullscreen(c, True);
            }
        }
        free(prop_reply);
    }

    DEBUGP("transient: %d\n", c->istransient);
    DEBUGP("floating:  %d\n", c->isfloating);

    int wmdsk = cd;
    bool visible = True;
    xcb_move(dis, c->win, -2 * M_WW, 0, &c->position_info);
    xcb_map_window(dis, c->win);
    if (cd != newdsk) {
        visible = False;
        rem_node(&c->link);
        select_desktop(newdsk);
        add_tail(&current_display->clients, &c->link);
        select_desktop(cd);
        wmdsk = newdsk;
        if (follow) {
            visible = True;
            change_desktop(&(Arg){.i = newdsk});
        }
    }
    if (visible && show) {
        xcb_move(dis, c->win, c->position_info.previous_x,
                              c->position_info.previous_y, NULL);
        c->position_info.previous_x = c->position_info.current_x;
        c->position_info.previous_y = c->position_info.current_y;
        update_current(c);
        if (c->isfloating && AUTOCENTER)
            centerfloating(c);
    }
    xcb_ewmh_set_wm_desktop(ewmh, c->win, wmdsk);
    grabbuttons(c);
    desktopinfo();
}

/* maximize the current window, or if we are maximized, tile() */
void maximize()
{
    if (!M_CURRENT)
        return;

    setmaximize(M_CURRENT, !M_CURRENT->ismaximized);
}

/* push the current client down the miniq and minimize the window */
void minimize_client(client *c)
{
    lifo *new;

    if (!c || c->isfullscreen)
        return;

    new = calloc(1, sizeof(lifo));
    if (!new)
        return;

    new->c = c;
    add_head(&current_display->miniq, &new->link);

    new->c->isminimized = true;
    xcb_move(dis, new->c->win, -2 * M_WW, 0, &new->c->position_info);
    xcb_add_property(dis, new->c->win, ewmh->_NET_WM_STATE, ewmh->_NET_WM_STATE_HIDDEN);

    client *t = M_HEAD;
    while (t) {
        if (t && !t->isminimized)
            break;
        t = M_GETNEXT(t);
    }
    if (t)
        update_current(t);

    tile();
}

/* minimize_client(); wrapper */
void minimize()
{
    minimize_client(M_CURRENT);
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

    if (!M_CURRENT || M_CURRENT->isfullscreen)
        return;
    geometry = get_geometry(M_CURRENT->win);
    winx = geometry->x;     winy = geometry->y;
    winw = geometry->width; winh = geometry->height;
    free(geometry);

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

    if (M_CURRENT->ismaximized)
        setmaximize(M_CURRENT, False);
    if (!M_CURRENT->isfloating)
        float_client(M_CURRENT);
    update_current(M_CURRENT);

    xcb_generic_event_t *e = NULL;
    xcb_motion_notify_event_t *ev = NULL;
    bool ungrab = false;
    do {
        xcb_flush(dis);
        while (!(e = xcb_wait_for_event(dis)))
            xcb_flush(dis);
        switch (e->response_type & ~0x80) {
            case XCB_CONFIGURE_REQUEST:
            case XCB_MAP_REQUEST:
                events[e->response_type & ~0x80](e);
                break;
            case XCB_MOTION_NOTIFY:
                ev = (xcb_motion_notify_event_t *)e;
                xw = (arg->i == MOVE ? winx : winw) + ev->root_x - mx;
                yh = (arg->i == MOVE ? winy : winh) + ev->root_y - my;
                if (arg->i == RESIZE) xcb_resize(dis, M_CURRENT->win,
                                      xw > MINWSZ ? xw : winw,
                                      yh > MINWSZ ? yh : winh);
                else if (arg->i == MOVE) xcb_move(dis, M_CURRENT->win, xw, yh, &M_CURRENT->position_info);
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
    } while (!ungrab && M_CURRENT);
    DEBUG("xcb: ungrab");
    xcb_ungrab_pointer(dis, XCB_CURRENT_TIME);

    free(pointer);
}

/* each window should cover all the available screen space */
void monocle(int hh, int cy)
{
    unsigned int b = MONOCLE_BORDERS ? 2 * client_borders(M_CURRENT) : 0;

    for (client *c = M_HEAD; c; c = M_GETNEXT(c))
        if (!ISFMFTM(c))
            xcb_move_resize(dis, c->win, M_GAPS, cy + M_GAPS,
                            M_WW - 2 * M_GAPS - b, hh - 2 * M_GAPS - b, &c->position_info);
}

/* move the current client, to current->next
* and current->next to current client's position */
void move_down()
{
    if (!M_CURRENT || !M_GETNEXT(M_CURRENT))
        return;
    client *c = M_CURRENT;
    client *n = M_GETNEXT(c);
    list   *l = c->link.parent;
    rem_node(&c->link);
    insert_node_after(l, &n->link, &c->link);
    tile();
}

/* move the current client, to the previous from current and
* the previous from current to current client's position */
void move_up()
{
    if (!M_CURRENT || !M_GETPREV(M_CURRENT))
        return;
    client *c = M_CURRENT;
    client *p = M_GETPREV(c);
    list   *l = c->link.parent;
    rem_node(&c->link);
    insert_node_before(l, &p->link, &c->link);
    tile();
}

/* cyclic focus the next window
 * if the window is the last on stack, focus head */
void next_win()
{
    client *t = M_CURRENT;

    if (!M_CURRENT || !M_GETNEXT(M_HEAD))
        return;

    while (1) {
        if (!M_GETNEXT(t))
            t = M_HEAD;
        else
            t = M_GETNEXT(t);
        if (!t->isminimized)
            break;
        if (t == M_CURRENT)
            break;
    }

    M_PREVFOCUS = M_CURRENT;
    update_current(t);
}

/* cyclic focus the previous window
 * if the window is the head, focus the last stack window */
void prev_win()
{
    client *t = M_CURRENT;

    if (!M_CURRENT || !M_GETNEXT(M_HEAD))
        return;

    for (;;) {
        if(!(t = M_GETPREV(t)))
            t = M_TAIL;
        if (!t->isminimized)
            break;
        if (t == M_CURRENT)
            break;
    }

    M_PREVFOCUS = M_CURRENT;
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

#ifdef EWMH_TASKBAR
    if (ev->atom == ewmh->_NET_WM_STRUT
     || ev->atom == ewmh->_NET_WM_STRUT_PARTIAL) {
        tile();
        return;
    }
#endif /* EWMH_TASKBAR */

    c = wintoclient(ev->window);
    if (!c || ev->atom != XCB_ICCCM_WM_ALL_HINTS)
        return;
    DEBUG("xcb: got hint!");
    if (xcb_icccm_get_wm_hints_reply(dis,
                                     xcb_icccm_get_wm_hints(dis, ev->window),
                                                            &wmh, NULL))
                                     /* TODO: error handling */
        c->isurgent = c != M_CURRENT &&
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
    int nd = 0, cd = current_desktop_number;

    if (!c)
        return;
    rem_node(&c->link);
    if (c == M_PREVFOCUS)
        M_PREVFOCUS = M_GETPREV(M_CURRENT);
    if (c == M_CURRENT || !M_GETNEXT(M_HEAD))
        update_current(M_PREVFOCUS);
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
    int msz = (M_MODE == BSTACK ? M_WH : M_WW) * MASTER_SIZE + M_MASTER_SIZE + arg->i;

    if (msz < MINWSZ || (M_MODE == BSTACK ? M_WH : M_WW) - msz < MINWSZ)
        return;
    M_MASTER_SIZE += arg->i;
    tile();
}

/* resize the first stack window - no boundary checks */
void resize_stack(const Arg *arg)
{
    M_GROWTH += arg->i;
    tile();
}

/*
 * resize floating windows in x-dimension (and float them if not already)
 */
void resize_x(const Arg *arg)
{
    xcb_get_geometry_reply_t *r;

    if (!arg->i || !M_CURRENT)
        return;

    if (!M_CURRENT->isfloating) {
        float_client(M_CURRENT);
        tile();
    }

    r = get_geometry(M_CURRENT->win);
    if (r->width + arg->i < MINWSZ || r->width + arg->i <= 0)
        return;

    r->width += arg->i;
    xcb_move_resize(dis, M_CURRENT->win, r->x, r->y, r->width, r->height, &M_CURRENT->position_info);
    free(r);
}

/*
 * resize floating windows in y-dimension (and float them if not already)
 */
void resize_y(const Arg *arg)
{
    xcb_get_geometry_reply_t *r;

    if (!arg->i || !M_CURRENT)
        return;

    if (!M_CURRENT->isfloating) {
        float_client(M_CURRENT);
        tile();
    }

    r = get_geometry(M_CURRENT->win);
    if (r->height + arg->i < MINWSZ || r->height + arg->i <= 0)
        return;

    r->height += arg->i;
    xcb_move_resize(dis, M_CURRENT->win, r->x, r->y, r->width, r->height, &M_CURRENT->position_info);
    free(r);
}

/* get (the last) client from the current miniq and restore it */
void restore_client(client *c)
{
    lifo *t;

    if (!check_head(&current_display->miniq))
        return;

    if (c == NULL)
        t = (lifo *)get_head(&current_display->miniq);
    else
        for (t = (lifo *)get_head(&current_display->miniq); t && t->c != c; t = (lifo *)get_next(&t->link)) ;
    if (!t)
        return;
    else
        rem_node(&t->link);

    t->c->isminimized = false;
    xcb_remove_property(dis, t->c->win, ewmh->_NET_WM_STATE, ewmh->_NET_WM_STATE_HIDDEN);

    /*
     * if our window is floating, center it to move it back onto the visible
     * screen, TODO: save geometry in some way to restore it where it was
     * before minimizing, TODO: fix it to use centerwindow() instead of copying
     * half of it
     */
    if (t->c->isfloating)
        centerfloating(t->c);
    tile();
    update_current(t->c);
    free(t);
}

/* return true if desktop has clients */
bool desktop_populated(desktop *d)
{
    monitor *moni;
    for (moni = (monitor *)get_head(&d->monitors); moni; moni = (monitor *)get_next(&moni->link)) {
        display *disp;
        for (disp = (display *)get_head(&moni->displays); disp; disp = (display *)get_next(&disp->link)) {
            if(get_head(&disp->clients)) {
                return true;
            }
        }
    }
    return false;
}

/* restore_client(); wrapper */
void restore()
{
   restore_client(NULL);
}

/* jump and focus the next or previous desktop */
void rotate(const Arg *arg)
{
    change_desktop(&(Arg)
                   {.i = (DESKTOPS + current_desktop_number + arg->i) % DESKTOPS});
}

/* jump and focus the next or previous desktop
 * and take the current client with us. */
void rotate_client(const Arg *arg)
{
    int i = (DESKTOPS + current_desktop_number + arg->i) % DESKTOPS;

    client_to_desktop(&(Arg){.i = i});
    change_desktop(&(Arg){.i = i});
}

/* jump and focus the next or previous desktop that has clients */
void rotate_filled(const Arg *arg)
{
    desktop *newdesk;

    if (arg->i > 0) {   /* forward */
        for (newdesk = (desktop *)get_next(&current_desktop->link);;) {
            if (!newdesk) {
                newdesk = (desktop *)get_head(&desktops);
            } else {
                if (newdesk == current_desktop || desktop_populated(newdesk)) break;
                newdesk = (desktop *)get_next(&newdesk->link);
            }
        }
    }
    else {
        for (newdesk = (desktop *)get_prev(&current_desktop->link);;) {
            if (!newdesk) {
                newdesk = (desktop *)get_tail(&desktops);
            } else {
                if (newdesk == current_desktop || desktop_populated(newdesk)) break;
                newdesk = (desktop *)get_prev(&newdesk->link);
            }
        }
    }

    if (newdesk)
        change_desktop(&(Arg){.i = newdesk->num});
}

/*
 * main event loop - on receival of an event call the appropriate event
 * handler
 */

#ifdef DEBUGGING
static char *xcb_event_str(xcb_generic_event_t *ev)
{
    switch(ev->response_type & ~0x80) {
        case XCB_KEY_PRESS:         return "XCB_KEY_PRESS";
        case XCB_KEY_RELEASE:       return "XCB_KEY_RELEASE";
        case XCB_BUTTON_PRESS:      return "XCB_BUTTON_PRESS";
        case XCB_BUTTON_RELEASE:    return "XCB_BUTTON_RELEASE";
        case XCB_MOTION_NOTIFY:     return "XCB_MOTION_NOTIFY";
        case XCB_ENTER_NOTIFY:      return "XCB_ENTER_NOTIFY";
        case XCB_LEAVE_NOTIFY:      return "XCB_LEAVE_NOTIFY";
        case XCB_FOCUS_IN:          return "XCB_FOCUS_IN";
        case XCB_FOCUS_OUT:         return "XCB_FOCUS_OUT";
        case XCB_KEYMAP_NOTIFY:     return "XCB_KEYMAP_NOTIFY";
        case XCB_EXPOSE:            return "XCB_EXPOSE";
        case XCB_GRAPHICS_EXPOSURE: return "XCB_GRAPHICS_EXPOSURE";
        case XCB_NO_EXPOSURE:       return "XCB_NO_EXPOSURE";
        case XCB_VISIBILITY_NOTIFY: return "XCB_VISIBILITY_NOTIFY";
        case XCB_CREATE_NOTIFY:     return "XCB_CREATE_NOTIFY";
        case XCB_DESTROY_NOTIFY:    return "XCB_DESTROY_NOTIFY";
        case XCB_UNMAP_NOTIFY:      return "XCB_UNMAP_NOTIFY ";
        case XCB_MAP_NOTIFY:        return "XCB_MAP_NOTIFY";
        case XCB_MAP_REQUEST:       return "XCB_MAP_REQUEST ";
        case XCB_REPARENT_NOTIFY:   return "XCB_REPARENT_NOTIFY";
        case XCB_CONFIGURE_NOTIFY:  return "XCB_CONFIGURE_NOTIFY";
        case XCB_CONFIGURE_REQUEST: return "XCB_CONFIGURE_REQUEST";
        case XCB_GRAVITY_NOTIFY:    return "XCB_GRAVITY_NOTIFY";
        case XCB_RESIZE_REQUEST:    return "XCB_RESIZE_REQUEST";
        default: return "undefined event";
    }
}
#endif /* DEBUGGING */

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
                DEBUGP("xcb: unimplemented event: %s\n", xcb_event_str(ev));
            }
            free(ev);
        }
    }
}

/* set the specified desktop's properties */
void select_desktop(int i)
{
    if (i < 0 || i >= DESKTOPS)
        return;
    current_desktop = find_desktop(i);
    if (!current_desktop)
        current_desktop = (desktop *)get_head(&desktops);
    current_desktop_number = current_desktop->num;
    current_monitor = (monitor *)get_head(&current_desktop->monitors);
    current_display = (display *)get_head(&current_monitor->displays);
}

static bool sendevent(xcb_window_t win, xcb_atom_t proto)
{
    bool got = check_wmproto(win, proto);
    if (got) {
        xcb_client_message_event_t ev = {0};

        ev.response_type = XCB_CLIENT_MESSAGE;
        ev.window = win;
        ev.format = 32;
        ev.sequence = 0;
        ev.type = wmatoms[WM_PROTOCOLS];
        ev.data.data32[0] = proto;
        ev.data.data32[1] = XCB_CURRENT_TIME;
        xcb_send_event(dis, 0, win, XCB_EVENT_MASK_NO_EVENT, (char *)&ev);
    }

    return got;
}

/* set or unset maximize state of client */
void setmaximize(client *c, bool maximize)
{
    DEBUGP("xcb: set maximize: %d\n", maximize);

    if (!c || c->isfullscreen)
        return;

    int borders = client_borders(c);
    borders = (!M_GETNEXT(M_HEAD) ||
               (M_MODE == MONOCLE && !ISFMFTM(c) && !MONOCLE_BORDERS)
              ) ? 0 : borders;
    xcb_border_width(dis, c->win, borders);

    if (maximize) {
        xcb_move_resize(dis, c->win, M_GAPS, M_WY + M_GAPS,
                        M_WW - 2 * (borders + M_GAPS),
                        M_WH - 2 * (borders + M_GAPS), &c->position_info);
        c->ismaximized = True;
    }
    else
        c->ismaximized = False;

    update_current(c);
}

/* get numlock modifier using xcb */
int setup_keyboard(void)
{
    xcb_get_modifier_mapping_reply_t *reply;
    xcb_keycode_t                    *modmap;
    xcb_keycode_t                    *numlock;

    if (!(keysyms = xcb_key_symbols_alloc(dis)))
        return -1;

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
    if (numlock) {
        for (unsigned int i = 0; i < 8; i++) {
            for (unsigned int j = 0; j < reply->keycodes_per_modifier; j++) {
                xcb_keycode_t keycode =
                        modmap[i * reply->keycodes_per_modifier +
                                               j];
                if (keycode == XCB_NO_SYMBOL)
                    continue;
                for (unsigned int n = 0; numlock[n] != XCB_NO_SYMBOL; n++) {
                    if (numlock[n] == keycode) {
                        DEBUGP("xcb: found num-lock %d\n", 1 << i);
                        numlockmask = 1 << i;
                        break;
                    }
                }
            }
        }
        free(numlock);
    }
    free(reply);


    return 0;
}

/* set or unset fullscreen state of client */
void setfullscreen(client *c, bool fullscrn)
{
    DEBUGP("xcb: set fullscreen: %d\n", fullscrn);

    if (fullscrn) {
        long data[] = { ewmh->_NET_WM_STATE_FULLSCREEN };
        c->isfullscreen = True;
        xcb_border_width(dis, c->win, 0);
        xcb_move_resize(dis, c->win, 0, 0, screen->width_in_pixels, screen->height_in_pixels, &c->position_info);
        xcb_change_property(dis, XCB_PROP_MODE_REPLACE,
                            c->win, ewmh->_NET_WM_STATE,
                            XCB_ATOM_ATOM, 32, True, data);
        create_display(c);
    }
    else {
        c->isfullscreen = False;
        xcb_border_width(dis, c->win,
                     (!M_GETNEXT(M_HEAD) ||
                      (M_MODE == MONOCLE && !ISFMFTM(c) && !MONOCLE_BORDERS)
                     ) ? 0 : client_borders(c));
        xcb_remove_property(dis, c->win, ewmh->_NET_WM_STATE, ewmh->_NET_WM_STATE_FULLSCREEN);
        destroy_display(c);
    }
    update_current(c);
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
    setup_display();
    select_desktop(0);      /* initialize global pointers */

#ifdef EWMH_TASKBAR
    Reset_Global_Strut();   /* struts are not yet ready. */
#endif /* EWMH_TASKBAR */
    borders = BORDER_WIDTH;

    aliens.head = NULL;
    aliens.tail = NULL;

    win_focus   = getcolor(FOCUS);
    win_unfocus = getcolor(UNFOCUS);
    win_scratch = getcolor(SCRATCH);

    /* setup keyboard */
    if (setup_keyboard() == -1)
        err(EXIT_FAILURE, "error: failed to setup keyboard\n");

    /* set up atoms for dialog/notification windows */
    xcb_get_atoms(WM_ATOM_NAME, wmatoms, WM_COUNT);

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
#ifdef EWMH_TASKBAR
                               ewmh->_NET_CLIENT_LIST,
                               ewmh->_NET_WM_STRUT,
                               ewmh->_NET_WM_STRUT_PARTIAL,
#endif /* EWMH_TASKBAR */
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
    xcb_ewmh_geometry_t workarea[2] = {{ 0, 0, M_WW, M_WH }};

    /* functionless window required by the EWMH standard */
    uint32_t noevents = 0;
    checkwin = xcb_generate_id(dis);
    xcb_create_window(dis, 0, checkwin, screen->root, 0, 0, 1, 1, 0,
                      XCB_WINDOW_CLASS_INPUT_ONLY, 0, XCB_CW_EVENT_MASK, &noevents);
    xcb_ewmh_set_wm_name(ewmh, checkwin, sizeof(WM_NAME)-1, WM_NAME);

    xcb_ewmh_set_supported(ewmh, default_screen, LENGTH(net_atoms), net_atoms);
    xcb_ewmh_set_supporting_wm_check(ewmh, screen->root, checkwin);
    xcb_ewmh_set_number_of_desktops(ewmh, default_screen, DESKTOPS);
    xcb_ewmh_set_current_desktop(ewmh, default_screen, DEFAULT_DESKTOP);
    xcb_ewmh_set_desktop_geometry(ewmh, default_screen, M_WW, M_WH);
    xcb_ewmh_set_desktop_viewport(ewmh, default_screen, 1, viewports);
    xcb_ewmh_set_workarea(ewmh, default_screen, 1, workarea);
    xcb_ewmh_set_showing_desktop(ewmh, default_screen, 0);

    xcb_change_property(dis, XCB_PROP_MODE_REPLACE, screen->root,
                        ewmh->_NET_SUPPORTED, XCB_ATOM_ATOM, 32,
                        LENGTH(net_atoms), net_atoms);

    if (USE_SCRATCHPAD && !CLOSE_SCRATCHPAD)
        scrpd_atom = xcb_internatom(dis, SCRPDNAME, 0);
    else
        scrpd_atom = 0;

    grabkeys();

    /* set events */
    for (unsigned int i = 0; i < XCB_NO_OPERATION; i++)
        events[i] = NULL;
    events[0]                       = xerror;
    events[XCB_BUTTON_PRESS]        = buttonpress;
    events[XCB_CLIENT_MESSAGE]      = clientmessage;
    events[XCB_CONFIGURE_REQUEST]   = configurerequest;
    events[XCB_DESTROY_NOTIFY]      = destroynotify;
    events[XCB_ENTER_NOTIFY]        = enternotify;
    events[XCB_KEY_PRESS]           = keypress;
    events[XCB_MAP_NOTIFY]          = mapnotify;
    events[XCB_MAP_REQUEST]         = maprequest;
    events[XCB_PROPERTY_NOTIFY]     = propertynotify;
    events[XCB_UNMAP_NOTIFY]        = unmapnotify;

    /* grab existing windows */
    xcb_query_tree_reply_t *reply;

    reply = xcb_query_tree_reply(dis, xcb_query_tree(dis, screen->root), 0);
    if (reply) {
        int len = xcb_query_tree_children_length(reply);
        xcb_window_t *children = xcb_query_tree_children(reply);
        uint32_t cd = current_desktop_number;
        for (int i = 0; i < len; i++) {
            xcb_atom_t wtype = ewmh->_NET_WM_WINDOW_TYPE_NORMAL;
            xcb_get_window_attributes_reply_t *attr;

//            if (window_is_override_redirect(children[i]))
            if (check_if_window_is_alien(children[i], NULL, &wtype))
                continue;

            attr = xcb_get_window_attributes_reply(dis,
                            xcb_get_window_attributes(dis, children[i]), NULL);
            if (!attr)
                continue;
            /* ignore windows in override redirect mode or with input only
             * class as we won't see them */
            if (!attr->override_redirect
                && attr->_class != XCB_WINDOW_CLASS_INPUT_ONLY) {
                uint32_t dsk = cd;

                if (scrpd_atom && !scrpd) {
                    if (xcb_check_attribute(dis, children[i], scrpd_atom)) {
                        scrpd = create_client(children[i], wtype);
                        setwindefattr(scrpd->win);
                        grabbuttons(scrpd);
                        xcb_move(dis, scrpd->win, -2 * M_WW, 0, &scrpd->position_info);
                        showscratchpad = False;
                        continue;
                    }
                }

                xcb_get_property_reply_t *prop_reply;
                bool isHidden = False, doMinimize = False;
                prop_reply = xcb_get_property_reply(dis, xcb_get_property_unchecked(
                                               dis, 0, children[i], ewmh->_NET_WM_STATE,
                                                XCB_ATOM_ATOM, 0, 1), NULL);
                                                /* TODO: error handling */
                if (prop_reply) {
                    if (prop_reply->format == 32) {
                        xcb_atom_t *v = xcb_get_property_value(prop_reply);
                        for (unsigned int i = 0; i < prop_reply->value_len; i++) {
                            DEBUGP("%d : %d\n", i, v[i]);
                            if (v[i] == ewmh->_NET_WM_STATE_HIDDEN)
                                isHidden = True;
                        }
                    }
                    free(prop_reply);
                }

/*
 * case 1: window has no desktop property and is unmapped --> ignore
 * case 2: window has no desktop property and is mapped --> add desktop property and append to client list.
 * case 3: window has current desktop property and is unmapped --> map and append to client list.
 * case 4: window has desktop and hidden property -> move to miniq and append to client list.
 * case 5: window has current desktop property and is mapped  --> append to client list.
 * case 6: window has different desktop property and is unmapped -> append to client list.
 * case 7: window has different desktop property and is mapped -> unmap and append to client list.
 * case 8: window has desktop property > DESKTOPS -> move window to last desktop
 * case 9: window has desktop property = -1 -> TODO: sticky window support.
 */
                bool case7 = False;
                if (!(xcb_ewmh_get_wm_desktop_reply(ewmh,
                      xcb_ewmh_get_wm_desktop(ewmh, children[i]), &dsk, NULL))) {
                    if (attr->map_state == XCB_MAP_STATE_UNMAPPED)
                        continue;                                               /* case 1 */
                    else
                        xcb_ewmh_set_wm_desktop(ewmh, children[i], dsk = cd);   /* case 2 */
                }
                else {
                    if (isHidden)
                        doMinimize = True;                                      /* case 4 */
                    if ((int)dsk > DESKTOPS-1)
                        xcb_ewmh_set_wm_desktop(ewmh, children[i], dsk = DESKTOPS-1);  /* case 8 */
                    if (dsk == cd) {
                        if (attr->map_state == XCB_MAP_STATE_UNMAPPED) {
                            if (wtype == ewmh->_NET_WM_WINDOW_TYPE_NORMAL)
                                xcb_map_window(dis, children[i]);               /* case 3 */
                            else
                                continue;   /* ignore _NET_WM_WINDOW_TYPE_DIALOG windows */
                        }
                        else
                            { ; }                                               /* case 5 */
                    }
                    else {  /* different desktop */
                        if (attr->map_state == XCB_MAP_STATE_UNMAPPED)
                            { ; }                                               /* case 6 */
                        else
                            case7 = True;                                       /* case 7 */
                    }
                }

            /* sane defaults */
                xcb_remove_property(dis, children[i], ewmh->_NET_WM_STATE, ewmh->_NET_WM_STATE_FULLSCREEN);
                xcb_remove_property(dis, children[i], ewmh->_NET_WM_STATE, ewmh->_NET_WM_STATE_HIDDEN);

                if (cd != dsk)
                    select_desktop(dsk);
                client *c = addwindow(children[i], wtype);

                if (doMinimize)
                    minimize_client(c);
                if (case7)
                    xcb_move(dis, c->win, -2 * M_WW, 0, &c->position_info);
                grabbuttons(c);
                if (cd != dsk) {
                    xcb_move(dis, c->win, -2 * M_WW, 0, &c->position_info);
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
    if (USE_SCRATCHPAD && !scrpd)
        spawn(&(Arg){.com = scrpcmd});

#ifdef EWMH_TASKBAR
    Setup_EWMH_Taskbar_Support();
    Setup_Global_Strut();
#endif

    return 0;
}

static void setup_display(void)
{
    desktops.head = desktops.tail = NULL;
    for (int d = 0; d < DESKTOPS; d++) {
        desktop *desk;
        if (!(desk = calloc(1, sizeof(desktop))))
            err(EXIT_FAILURE, "cannot allocate desktop");
        add_tail(&desktops, &desk->link);
        desk->monitors.master = desk;
        desk->num = d;

        for (int m = 0; m < MONITORS; m++) {
            monitor *moni;
            display *disp;
            if (!(moni = calloc(1, sizeof(monitor))))
                err(EXIT_FAILURE, "cannot allocate monitor");
            add_tail(&desk->monitors, &moni->link);
            moni->displays.master = moni;

/* TODO: multi monitor support */
            moni->num = m;
            moni->ww = screen->width_in_pixels;
            moni->wh = screen->height_in_pixels;
#ifndef EWMH_TASKBAR
            moni->wh -= PANEL_HEIGHT;
#endif /* EWMH_TASKBAR */

/* each monitor gets 1 default display. */
            if (!(disp = calloc(1, sizeof(display))))
                err(EXIT_FAILURE, "cannot allocate display");
            add_tail(&moni->displays, &disp->link);
            disp->clients.master = disp;

            /* disp->di.master_size = MASTER_SIZE; */
            disp->di.gaps = USELESSGAP;
            disp->di.mode = DEFAULT_MODE;
            disp->di.showpanel = SHOW_PANEL;
            disp->di.invert = INVERT;

/* Pivot monitor support */
            if (moni->wh > moni->ww) {
                if (disp->di.mode == TILE)
                    disp->di.mode = BSTACK;
                else {
                    if (disp->di.mode == BSTACK)
                        disp->di.mode = TILE;
                }
            }
        }
    }
    current_desktop = (desktop *)get_head(&desktops);
    current_monitor = (monitor *)get_head(&current_desktop->monitors);
    current_display = (display *)get_head(&current_monitor->displays);
}

/*
 * set default window attributes
 */
static void setwindefattr(xcb_window_t w)
{
    unsigned int values[1] = {XCB_EVENT_MASK_PROPERTY_CHANGE|
                            (FOLLOW_MOUSE ? XCB_EVENT_MASK_ENTER_WINDOW : 0)};
    if (w) xcb_change_window_attributes(dis, w, XCB_CW_EVENT_MASK, values);
}

/*
 * toggle visibility of all windows in all desktops
 */
void showhide(void)
{
    if ((show = !show)) {
        for (client *c = (client *)get_node_head(&M_HEAD->link); c; c = M_GETNEXT(c))
            xcb_move(dis, c->win, c->position_info.previous_x, c->position_info.previous_y, &c->position_info);
        tile();
        xcb_ewmh_set_showing_desktop(ewmh, default_screen, 1);
    } else {
        for (client *c = (client *)get_node_head(&M_HEAD->link); c; c = M_GETNEXT(c))
            xcb_move(dis, c->win, -2 * M_WW, 0, &c->position_info);
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
    err(EXIT_SUCCESS, "error: execvp %s", (char *)arg->com[0]);
}

/* arrange windows in normal or bottom stack tile */
void stack(int hh, int cy)
{
    client *c = NULL, *t = NULL; bool b = M_MODE == BSTACK;
    int n = 0, d = 0, z = b ? M_WW : hh,
        ma = (M_MODE == BSTACK ? M_WH : M_WW) * MASTER_SIZE + M_MASTER_SIZE;

    /* count stack windows and grab first non-floating, non-maximize window */
    for (t = M_HEAD; t; t = M_GETNEXT(t)) {
        if (!ISFMFTM(t)) {
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
        int borders = client_borders(c);
        xcb_move_resize(dis, c->win, M_GAPS, cy + M_GAPS,
                        M_WW - 2 * (borders + M_GAPS),
                        hh - 2 * (borders + M_GAPS), &c->position_info);
        return;
    } else if (n > 1) {
        d = (z - M_GROWTH) % n + M_GROWTH; z = (z - M_GROWTH) / n;
    }

    /* tile the first non-floating, non-maximize window to cover the master area */
    int borders = client_borders(c);
    if (b)
        xcb_move_resize(dis, c->win, M_GAPS,
                        M_INVERT ? (cy + hh - ma + M_GAPS) : (cy + M_GAPS),
                        M_WW - 2 * (borders + M_GAPS),
                        ma - 2 * (borders + M_GAPS), &c->position_info);
    else
        xcb_move_resize(dis, c->win, M_INVERT ? (M_WW - ma + M_GAPS) : M_GAPS,
                        cy + M_GAPS,
                        ma - 2 * (borders + M_GAPS),
                        hh - 2 * (borders + M_GAPS), &c->position_info);

    /* tile the next non-floating, non-maximize (first) stack window with growth|d */
    for (c = M_GETNEXT(c); c && ISFMFTM(c); c = M_GETNEXT(c));
    borders = client_borders(c);
    int cx = b ? 0 : (M_INVERT ? M_GAPS : ma),
        cw = (b ? hh : M_WW) - 2 * borders - ma - M_GAPS,
        ch = z - 2 * borders - M_GAPS;
    if (b)
        xcb_move_resize(dis, c->win, cx += M_GAPS, cy += M_INVERT ? M_GAPS : ma,
                        ch - M_GAPS + d, cw, &c->position_info);
    else
        xcb_move_resize(dis, c->win, cx, cy += M_GAPS, cw, ch - M_GAPS + d, &c->position_info);

    /* tile the rest of the non-floating, non-maximize stack windows */
    for (b ? (cx += z + d - M_GAPS) : (cy += z + d - M_GAPS),
         c = M_GETNEXT(c); c; c = M_GETNEXT(c)) {
        if (ISFMFTM(c))
            continue;
        if (b) {
            xcb_move_resize(dis, c->win, cx, cy, ch, cw, &c->position_info); cx += z;
        } else {
            xcb_move_resize(dis, c->win, cx, cy, cw, ch, &c->position_info); cy += z;
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
    if (!M_CURRENT || !M_GETNEXT(M_HEAD))
        return;
    if (M_CURRENT == M_HEAD) {
        client *c = M_GETNEXT(M_HEAD);
        rem_node(&c->link);
        add_head(&current_display->clients, &c->link);
    }
    else {
        client *c = M_CURRENT;
        rem_node(&c->link);
        add_head(&current_display->clients, &c->link);
    }
    update_current(M_HEAD);
}

/* switch the tiling mode and reset all floating windows */
void switch_mode(const Arg *arg)
{
    if (!show)
        showhide();
    if (M_MODE == arg->i)
        for (client *c = M_HEAD; c; c = M_GETNEXT(c))
            unfloat_client(c);
    M_MODE = arg->i;
    tile();
    update_current(M_CURRENT);
    desktopinfo();
}


/* cycle the tiling mode and reset all floating windows */
void rotate_mode(const Arg *arg)
{
    if (!show)
        showhide();
    M_MODE = (M_MODE + arg->i + MODES) % MODES;
    tile();
    update_current(M_CURRENT);
    desktopinfo();
}

/* tile all windows of current desktop - call the handler tiling function */
void tile(void)
{
    desktopinfo();
    if (!M_HEAD)
        return; /* nothing to arange */
#ifndef EWMH_TASKBAR
    layout[M_GETNEXT(M_HEAD) ? M_MODE : MONOCLE](M_WH + (M_SHOWPANEL ? 0 : PANEL_HEIGHT),
                                (TOP_PANEL && M_SHOWPANEL ? PANEL_HEIGHT : 0));
#else
    Update_Global_Strut();
    layout[M_GETNEXT(M_HEAD) ? M_MODE : MONOCLE](M_WH, M_WY);
#endif /* EWMH_TASKBAR */
}

/* reset the active window from floating to tiling, if not already */
void tilemize()
{
    if (!M_CURRENT || !M_CURRENT->isfloating)
        return;
    unfloat_client(M_CURRENT);
    update_current(M_CURRENT);
}

/* toggle visibility state of the panel */
void togglepanel()
{
    M_SHOWPANEL = !M_SHOWPANEL;
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
        xcb_get_geometry_reply_t *wa = get_geometry(scrpd->win);
        xcb_move(dis, scrpd->win, (M_WW - wa->width) / 2, (M_WH - wa->height) / 2, &scrpd->position_info);
        free(wa);
        update_current(scrpd);
        xcb_raise_window(dis, scrpd->win);
    } else {
        xcb_move(dis, scrpd->win, -2 * M_WW, 0, &scrpd->position_info);
        if(M_CURRENT == scrpd) {
            if(!M_PREVFOCUS)
                update_current(M_HEAD);
            else
                update_current(M_PREVFOCUS->isminimized ? M_HEAD : M_PREVFOCUS);
        }
    }
}

/* tile a floating client and save its size for re-floating */
void unfloat_client(client *c)
{
    if (!c)
        return;

    c->isfloating = false;

    xcb_get_geometry_reply_t *r = get_geometry(c->win);
    c->dim[0] = r->width;
    c->dim[1] = r->height;
    free(r);
}

static inline bool on_current_desktop(client *c) {
    client *p;
    for (p = M_HEAD; p && p != c; p = M_GETNEXT(p))
        ;
    return (p != NULL);
}

/* windows that request to unmap should lose their
 * client, so no invisible windows exist on screen
 */
void unmapnotify(xcb_generic_event_t *e)
{
    xcb_unmap_notify_event_t *ev = (xcb_unmap_notify_event_t *)e;
    client *c = wintoclient(ev->window);

    DEBUG("xcb: unmap notify");

    if (c && on_current_desktop(c)) {
        if (c->isfullscreen)
            destroy_display(c);
        removeclient(c);
        desktopinfo();
    }
}

/*
 * highlight borders and set active window and input focus
 * if given current is NULL then delete the active window property
 *
 * stack order by client properties, top to bottom:
 *  - current when floating or transient
 *  - floating or trancient windows
 *  - current when tiled
 *  - current when maximized
 *  - maximized windows
 *  - tiled windows
 *
 * a window should have borders in any case, except if
 *  - the window is the only window on screen
 *  - the window is maximized
 *  - the mode is MONOCLE and the window is not floating or transient
 *    and MONOCLE_BORDERS is set to false
 */
static inline void nada(void)
{
        xcb_delete_property(dis, screen->root, ewmh->_NET_ACTIVE_WINDOW);
        xcb_set_input_focus(dis, XCB_INPUT_FOCUS_POINTER_ROOT, screen->root, XCB_CURRENT_TIME);
        M_PREVFOCUS = M_CURRENT = NULL;
}
void update_current(client *newfocus)   // newfocus may be NULL
{
    if(!M_HEAD && USE_SCRATCHPAD && !showscratchpad) {                // empty desktop. no clients, no scratchpad.
        nada();
        return;
    }

    if(!newfocus) {
        if(M_PREVFOCUS)
            M_CURRENT = M_PREVFOCUS;
        else
            M_CURRENT = M_HEAD;
        M_PREVFOCUS = M_GETPREV(M_CURRENT);             // get previous client in list, may be NULL
    }
    else {
        if(newfocus == M_PREVFOCUS) {
            M_CURRENT = M_PREVFOCUS;
            M_PREVFOCUS = M_GETPREV(M_CURRENT);         // get previous client in list, may be NULL
        }
        else {
            M_PREVFOCUS = M_CURRENT;
            M_CURRENT = newfocus;
        }
    }

    if(!M_CURRENT && (USE_SCRATCHPAD && showscratchpad && scrpd))     // focus scratchpad, if visible
        M_CURRENT = scrpd;

    if(!M_CURRENT) {  // there is really really really nothing to focus.
        nada();
        return;
    }

    for (client *c = M_HEAD; c; c = M_GETNEXT(c)) {
        if (!c->isfullscreen) {
            xcb_change_window_attributes(dis, c->win, XCB_CW_BORDER_PIXEL,
                                    (c == M_CURRENT ? &win_focus : &win_unfocus));
            xcb_border_width(dis, c->win, ((!MONOCLE_BORDERS && !M_GETNEXT(M_HEAD))
                                        || (M_MODE == MONOCLE && !ISFMFTM(c) && !MONOCLE_BORDERS)
                                           ) ? 0 : client_borders(c));
        }
    }

    if (USE_SCRATCHPAD && SCRATCH_WIDTH && showscratchpad && scrpd) {
        xcb_change_window_attributes(dis, scrpd->win, XCB_CW_BORDER_PIXEL,
                            (M_CURRENT == scrpd ? &win_scratch : &win_unfocus));
        xcb_border_width(dis, scrpd->win, SCRATCH_WIDTH);

    }

    tile();

    for (client *c = M_HEAD; c; c = M_GETNEXT(c)) {
        if (c->isfullscreen) {
//            xcb_border_width(dis, c->win, 0);
            xcb_lower_window(dis, c->win);
//            xcb_move_resize(dis, c->win, 0, 0,
//                            screen->width_in_pixels, screen->height_in_pixels, &c->position_info);
            break;
        }
    }

    client *rl = NULL;
    for (client *c = M_HEAD; c; c = M_GETNEXT(c)) {
        if (c->ismaximized || c->isfloating || c->istransient || c->type != ewmh->_NET_WM_WINDOW_TYPE_NORMAL) {
            if (c == M_CURRENT) {
                rl = c;
                continue;
            }
        }
        xcb_raise_window(dis, c->win);
    }
    if(rl)
        xcb_raise_window(dis, rl->win);

    if (USE_SCRATCHPAD && showscratchpad && scrpd)
        xcb_raise_window(dis, scrpd->win);

    if (check_head(&aliens)) {
        alien *a;
        for (a=(alien *)get_head(&aliens); a; a=(alien *)get_next(&a->link)) {
            if (M_CURRENT->isfullscreen
             && a->type != ewmh->_NET_WM_WINDOW_TYPE_NOTIFICATION)
                continue;
            xcb_raise_window(dis, a->win);
        }
    }

    if (M_CURRENT) {
        if (M_CURRENT->setfocus) {
            xcb_change_property(dis, XCB_PROP_MODE_REPLACE, screen->root,
                                ewmh->_NET_ACTIVE_WINDOW, XCB_ATOM_WINDOW, 32, 1,
                                &M_CURRENT->win);
            xcb_set_input_focus(dis, XCB_INPUT_FOCUS_POINTER_ROOT, M_CURRENT->win,
                                XCB_CURRENT_TIME);
            DEBUG("xcb_set_input_focus();");
        }
        else {
            sendevent(M_CURRENT->win, wmatoms[WM_TAKE_FOCUS]);
            DEBUG("send WM_TAKE_FOCUS");
        }
    }
}

static alien *wintoalien(list *l, xcb_window_t win)
{
    alien *t;
    if (!l || !win)
        return NULL;

    for (t=(alien *)get_head(l); t; t=(alien *)get_next((node *)t)) {
        if (t->win == win)
            break;
    }
    return t;
}

/* find to which client the given window belongs to */
client *wintoclient(xcb_window_t win)
{
    desktop *desk;
    for (desk = (desktop *)get_head(&desktops); desk; desk = (desktop *)get_next(&desk->link)) {
        monitor *moni;
        for (moni = (monitor *)get_head(&desk->monitors); moni; moni = (monitor *)get_next(&moni->link)) {
            display *disp;
            for (disp = (display *)get_head(&moni->displays); disp; disp = (display *)get_next(&disp->link)) {
                client *c;
                for (c = (client *)get_head(&disp->clients); c; c = (client *)get_next(&c->link)) {
                    if (c->win == win) {
                        return c;
                    }
                }
            }
        }
    }
    return NULL;
}

void xerror(xcb_generic_event_t *e)
{
#ifdef DEBUGGING
    xcb_generic_error_t *error = (xcb_generic_error_t *)e;
    DEBUGP("X error: %i, %i:%i [%i]\n", error->error_code,
           (int)error->major_code, (int)error->minor_code,
           (int)error->resource_id);
#else
    if(e){;} /* silencing gcc warning */
#endif /* DEBUGGING */
}

static void ungrab_focus(void)
{
    Display * dpy;


// if use xcb_set_input_focus(dis, XCB_INPUT_FOCUS_POINTER_ROOT, screen->root, XCB_CURRENT_TIME);
// then the focus gets frozen to one window, and there's no way to set focus to different window.
// if set focus to PointerRoot, then focus follows mouse after quitting the window manager.
// TODO: convert to xcb

    if ((dpy = XOpenDisplay(0x0))) {
        XSetInputFocus(dpy, PointerRoot, RevertToNone, CurrentTime);
        XCloseDisplay(dpy);
    }

}
int main(int argc, char *argv[])
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IOLBF, 0);
    if (argc == 2 && argv[1][0] == '-') switch (argv[1][1]) {
        case 'v':
            errx(EXIT_SUCCESS,
            "FrankenWM - by sulami (thanks to c00kiemon5ter and Cloudef)");
        case 'h':
            errx(EXIT_SUCCESS, "%s", USAGE);
        default:
            errx(EXIT_FAILURE, "%s", USAGE);
    } else if (argc != 1) {
        errx(EXIT_FAILURE, "%s", USAGE);
    }
    if (xcb_connection_has_error((dis = xcb_connect(NULL, &default_screen))))
        errx(EXIT_FAILURE, "error: cannot open display\n");
    DEBUG("connected to display");
    if (setup(default_screen) != -1) {
        desktopinfo(); /* zero out every desktop on (re)start */
        run();
    }
    cleanup();
    xcb_aux_sync(dis);
    xcb_disconnect(dis);
    ungrab_focus();
    return retval;
}

#ifdef EWMH_TASKBAR
/*
 * Optional EWMH Taskbar (Panel) Support functions
 */

static void Setup_EWMH_Taskbar_Support(void)
{
    /*
     * initial _NET_CLIENT_LIST property
     */
    Update_EWMH_Taskbar_Properties();

    xcb_change_property(dis, XCB_PROP_MODE_REPLACE, screen->root,
                        ewmh->_NET_DESKTOP_NAMES, ewmh->UTF8_STRING, 8, 0, NULL);
}

static void Cleanup_EWMH_Taskbar_Support(void)
{
    /*
     * set _NET_CLIENT_LIST property to zero
     */
    xcb_window_t empty = 0;
    xcb_change_property(dis, XCB_PROP_MODE_REPLACE,
                        screen->root, ewmh->_NET_CLIENT_LIST,
                        XCB_ATOM_WINDOW, 32, 0, &empty);
}

static inline void Update_EWMH_Taskbar_Properties(void)
{
    /*
     * update _NET_CLIENT_LIST property, may be empty
     */

    xcb_window_t *wins;
    client *c;
    int num=0;

    if(showscratchpad && scrpd)
        num++;
    for (c = M_HEAD; c; c = M_GETNEXT(c))
        num++;

    if((wins = (xcb_window_t *)calloc(num, sizeof(xcb_window_t))))
    {
        num = 0;
        if(showscratchpad && scrpd)
            wins[num++] = scrpd->win;
        for (c = M_HEAD; c; c = M_GETNEXT(c))
            wins[num++] = c->win;
        xcb_change_property(dis, XCB_PROP_MODE_REPLACE,
                            screen->root, ewmh->_NET_CLIENT_LIST,
                            XCB_ATOM_WINDOW, 32, num, wins);
        xcb_flush(dis);
        free(wins);
        DEBUGP("update _NET_CLIENT_LIST property (%d entries)\n", num);
    }
}
#endif /* EWMH_TASKBAR */


#ifdef EWMH_TASKBAR
static void Setup_Global_Strut(void)
{
    Update_Global_Strut();
}

static void Cleanup_Global_Strut(void)
{
}

static inline void Reset_Global_Strut(void)
{
    gstrut.left = gstrut.right = gstrut.top = gstrut.bottom = 0;
    M_WW = screen->width_in_pixels;
    M_WH = screen->height_in_pixels;
    M_WY = 0;
}

static void Update_Global_Strut(void)
{
    /* TODO(?): Struts for each desktop. */

    Reset_Global_Strut();

    /* grab existing windows */
    xcb_query_tree_reply_t *reply = xcb_query_tree_reply(dis,
                                            xcb_query_tree(dis, screen->root), 0);
    if (reply) {
        int len = xcb_query_tree_children_length(reply);
        xcb_window_t *children = xcb_query_tree_children(reply);
        bool gstrut_modified = False;
        for (int i = 0; i < len; i++) {
            xcb_get_window_attributes_reply_t *attr = xcb_get_window_attributes_reply(dis,
                            xcb_get_window_attributes(dis, children[i]), NULL);
            if (!attr)
                continue;

            if (!(attr->map_state == XCB_MAP_STATE_UNMAPPED)) {
                void *data;
                xcb_get_property_reply_t *strut_r;
/*
 * Read newer _NET_WM_STRUT_PARTIAL property first. Only the first 4 values.
 * Fall back to older _NET_WM_STRUT property.
 */
                strut_r = xcb_get_property_reply(dis,
                          xcb_get_property_unchecked(dis, false, children[i],
                          ewmh->_NET_WM_STRUT_PARTIAL, XCB_ATOM_CARDINAL, 0, 4), NULL);
                if (strut_r->type == XCB_NONE) {
                    strut_r = xcb_get_property_reply(dis,
                              xcb_get_property_unchecked(dis, false, children[i],
                              ewmh->_NET_WM_STRUT, XCB_ATOM_CARDINAL, 0, 4), NULL);
                }
                if(strut_r && strut_r->value_len && (data = xcb_get_property_value(strut_r)))
                {
                    uint32_t *strut = data;
                    if (gstrut.top < strut[2]) {
                        gstrut.top = strut[2];
                        gstrut_modified = True;
                    }
                    if (gstrut.bottom < strut[3]) {
                        gstrut.bottom = strut[3];
                        gstrut_modified = True;
                    }
                }
            }
            free(attr);
        }
        free(reply);
        if (gstrut_modified) {
            M_WW = screen->width_in_pixels;
            M_WH = screen->height_in_pixels;
            M_WH -= gstrut.top;
            M_WH -= gstrut.bottom;
            M_WY = gstrut.top;
        }
    }
}
#endif /* EWMH_TASKBAR */

/* vim: set ts=4 sw=4 expandtab :*/
