/* see LICENSE for copyright and license */

#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdbool.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <sys/wait.h>
#include <xcb/xcb.h>
#include <xcb/xcb_atom.h>
#include <xcb/xcb_icccm.h>

#define XCB_MOVE_RESIZE XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y | XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT
#define NUMLOCK_KEYCODE 77

#define LENGTH(x) (sizeof(x)/sizeof(*x))

static const unsigned char XCB_ATOM_NULL = 0;
static char *WM_ATOM_NAME[]   = { "WM_PROTOCOLS", "WM_DELETE_WINDOW" };
static char *NET_ATOM_NAME[]  = { "_NET_SUPPORTED", "_NET_WM_STATE_FULLSCREEN", "_NET_WM_STATE", "_NET_ACTIVE_WINDOW" };

enum { WM_PROTOCOLS, WM_DELETE_WINDOW, WM_COUNT };
enum { NET_SUPPORTED, NET_FULLSCREEN, NET_WM_STATE, NET_ACTIVE, NET_COUNT };
enum { TILE, MONOCLE, BSTACK, GRID, };

/* structs */
typedef union {
    const char** com;
    const int i;
} Arg;

typedef struct {
    unsigned int mod;
    unsigned int keysym;
    void (*function)(const Arg *);
    const Arg arg;
} key;

typedef struct client {
    struct client *next;
    bool isurgent, istransient, isfullscreen;
    xcb_window_t win;
} client;

typedef struct {
    int master_size;
    int mode;
    int growth;
    client *head;
    client *current;
    client *prevfocus;
    bool showpanel;
} desktop;

typedef struct {
    const char *class;
    const int desktop;
    const bool follow;
} AppRule;

/* Functions */
static void addwindow(xcb_window_t w);
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
static void grabkeys(void);
static void keypress(xcb_generic_event_t *e);
static void killclient();
static void last_desktop();
static void maprequest(xcb_generic_event_t *e);
static void move_down();
static void move_up();
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
static void update_current(void);
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

static unsigned int win_focus;
static unsigned int win_unfocus;
static unsigned int numlockmask = 0; /* dynamic key lock mask */
static xcb_connection_t *dis;
static xcb_screen_t *screen;
static client *head, *prevfocus, *current;

static xcb_atom_t wmatoms[WM_COUNT], netatoms[NET_COUNT];
static desktop desktops[DESKTOPS];

/* events array */
static void (*events[XCB_NO_OPERATION])(xcb_generic_event_t *e);

static xcb_screen_t *screen_of_display(xcb_connection_t *con, int screen) {
    xcb_screen_iterator_t iter;

    iter = xcb_setup_roots_iterator(xcb_get_setup(con));
    for (; iter.rem; --screen, xcb_screen_next(&iter))
        if (screen == 0)
            return iter.data;

    return NULL;
}

static inline void xcb_move_resize(xcb_connection_t *con, xcb_window_t win, int x, int y, int w, int h) {
    unsigned int pos[4] = { x, y, w, h };
    xcb_configure_window(con, win, XCB_MOVE_RESIZE, pos);
}

static inline void xcb_raise_window(xcb_connection_t *con, xcb_window_t win) {
    unsigned int arg[1] = { XCB_STACK_MODE_ABOVE };
    xcb_configure_window(con, win, XCB_CONFIG_WINDOW_STACK_MODE, arg);
}

static inline void xcb_border_width(xcb_connection_t *con, xcb_window_t win, int w) {
   unsigned int arg[1] = { w };
   xcb_configure_window(con, win, XCB_CONFIG_WINDOW_BORDER_WIDTH, arg);
}

static unsigned int xcb_get_colorpixel(char *hex) {
    char strgroups[3][3]  = {{hex[1], hex[2], '\0'},
                             {hex[3], hex[4], '\0'},
                             {hex[5], hex[6], '\0'}};
    unsigned int rgb16[3] = {(strtol(strgroups[0], NULL, 16)),
                             (strtol(strgroups[1], NULL, 16)),
                             (strtol(strgroups[2], NULL, 16))};

    return (rgb16[0] << 16) + (rgb16[1] << 8) + rgb16[2];
}

static void xcb_get_atoms(char **names, xcb_atom_t *atoms, unsigned int count) {
    xcb_intern_atom_cookie_t cookies[count];
    xcb_intern_atom_reply_t  *reply;

    for (unsigned int i = 0; i < count; ++i)
        cookies[i] = xcb_intern_atom(dis, 0, strlen(names[i]), names[i]);

    /* get responses */
    for (unsigned int i = 0; i < count; ++i) {
        reply = xcb_intern_atom_reply(dis, cookies[i], NULL); /* TODO: Handle error */
        if (reply) {
            printf("%s : %d\n", names[i], reply->atom);
            atoms[i] = reply->atom;
            free(reply);
        }
    }
}

static void xcb_get_attributes(xcb_window_t *windows, xcb_get_window_attributes_reply_t **reply, unsigned int count) {
    xcb_get_window_attributes_cookie_t cookies[count];

    for (unsigned int i = 0; i < count; ++i)
       cookies[i] = xcb_get_window_attributes(dis, windows[i]);

    for (unsigned int i = 0; i < count; ++i)
       reply[i] = xcb_get_window_attributes_reply(dis, cookies[i], NULL); /* TODO: Handle error */
}

static int checkotherwm(void) {
    xcb_void_cookie_t cookie;
    xcb_generic_error_t *error;
    unsigned int mask = XCB_CW_EVENT_MASK;
    unsigned int values[1] = {XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_STRUCTURE_NOTIFY | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY};

    cookie = xcb_change_window_attributes_checked(dis, screen->root, mask, values);
    error = xcb_request_check(dis, cookie);
    xcb_flush(dis);

    if (!error)
       return 0;

    fprintf(stderr, "error: another WM running [%d]",
            error->error_code);

    return 1;
}

void addwindow(xcb_window_t w) {
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
#if 0
    XSelectInput(dis, ((current = c)->win = w), PropertyChangeMask);
    if (FOLLOW_MOUSE) XSelectInput(dis, c->win, EnterWindowMask);
#else
    xcb_set_input_focus(dis, XCB_INPUT_FOCUS_POINTER_ROOT, ((current= c)->win = w), XCB_CURRENT_TIME);
    if (FOLLOW_MOUSE) xcb_set_input_focus(dis, XCB_INPUT_FOCUS_POINTER_ROOT, c->win, XCB_CURRENT_TIME);
#endif
}

void buttonpress(xcb_generic_event_t *e) {
    xcb_button_press_event_t *ev = (xcb_button_press_event_t*)e;
    if (CLICK_TO_FOCUS && ev->event != current->win && ev->detail == XCB_BUTTON_INDEX_1)
        for (current=head; current; current=current->next) if (ev->event == current->win) break;
    if (!current) current = head;
    update_current();
}

void change_desktop(const Arg *arg) {
    if (arg->i == current_desktop) return;
    previous_desktop = current_desktop;
    select_desktop(arg->i);
    tile();
    if (mode == MONOCLE && current) xcb_map_window(dis, current->win);
    else for (client *c=head; c; c=c->next) xcb_map_window(dis, c->win);
    update_current();
    select_desktop(previous_desktop);
    for (client *c=head; c; c=c->next) xcb_unmap_window(dis, c->win);
    select_desktop(arg->i);
    desktopinfo();
}

void cleanup(void) {
    xcb_query_tree_cookie_t cookie;
    xcb_query_tree_reply_t  *reply;
    unsigned int nchildren;

    xcb_ungrab_key(dis, XCB_GRAB_ANY, screen->root, XCB_MOD_MASK_ANY);
    xcb_set_input_focus(dis, XCB_NONE, XCB_INPUT_FOCUS_POINTER_ROOT, XCB_CURRENT_TIME);

    cookie = xcb_query_tree(dis, screen->root);
    reply  = xcb_query_tree_reply(dis, cookie, NULL); /* TODO: error handling */
    if (reply) {
        nchildren = reply[0].children_len;
        for (unsigned int i = 0; i<nchildren; ++i) sendevent(reply[i].parent, WM_DELETE_WINDOW);
        free(reply);
    }
    xcb_flush(dis);
}

void client_to_desktop(const Arg *arg) {
    if (arg->i == current_desktop || !current) return;
    xcb_window_t w = current->win;
    int cd = current_desktop;

    xcb_unmap_window(dis, current->win);
    if (current->isfullscreen) setfullscreen(current, false);
    removeclient(current);

    select_desktop(arg->i);
    addwindow(w);

    select_desktop(cd);
    tile();
    update_current();
    if (FOLLOW_WINDOW) change_desktop(arg);
    desktopinfo();
}

/* check if window requested fullscreen wm_state
 * To change the state of a mapped window, a client MUST send a _NET_WM_STATE client message to the root window
 * message_type must be _NET_WM_STATE, data.l[0] is the action to be taken, data.l[1] is the property to alter
 * three actions: remove/unset _NET_WM_STATE_REMOVE=0, add/set _NET_WM_STATE_ADD=1, toggle _NET_WM_STATE_TOGGLE=2
 */
void clientmessage(xcb_generic_event_t *e) {
    xcb_client_message_event_t *ev = (xcb_client_message_event_t*)e;
    client *c = wintoclient(ev->window);

    printf("client message: %d\n", ev->data.data32[1]);

    if (ev->format != 32) return;
    if (c && ev->type == netatoms[NET_WM_STATE] && ((unsigned)ev->data.data32[1]
        == netatoms[NET_FULLSCREEN] || (unsigned)ev->data.data32[2] == netatoms[NET_FULLSCREEN]))
        setfullscreen(c, (ev->data.data32[0] == 1 || (ev->data.data32[0] == 2 && !c->isfullscreen)));
    else if (c && ev->type == netatoms[NET_ACTIVE]) current = c;
    tile();
    update_current();
}

void configurerequest(xcb_generic_event_t *e) {
    xcb_configure_request_event_t *ev = (xcb_configure_request_event_t*)e;

    puts("configure request");

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
    xcb_flush(dis);
}

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

void destroynotify(xcb_generic_event_t *e) {
    puts("destoroy notify");
    xcb_destroy_notify_event_t *ev = (xcb_destroy_notify_event_t*)e;
    client *c = wintoclient(ev->window);
    if (c) removeclient(c);
    desktopinfo();
}

void die(const char *errstr, ...) {
    va_list ap;
    va_start(ap, errstr);
    vfprintf(stderr, errstr, ap);
    va_end(ap);
    exit(EXIT_FAILURE);
}

void enternotify(xcb_generic_event_t *e) {
    xcb_enter_notify_event_t *ev = (xcb_enter_notify_event_t*)e;
    puts("enter notify");
    if (FOLLOW_MOUSE)
        if ((ev->mode == XCB_NOTIFY_MODE_NORMAL && ev->detail != XCB_NOTIFY_DETAIL_INFERIOR) || ev->event == screen->root)
            for (current=head; current; current=current->next) if (ev->event == current->win) break;
    if (!current) current = head;
    update_current();
}

void focusurgent() {
    for (client *c=head; c; c=c->next) if (c->isurgent) current = c;
    update_current();
}

unsigned int getcolor(char* color) {
    xcb_colormap_t map = screen->default_colormap;
    xcb_alloc_color_reply_t *c;
    unsigned int r, g, b, rgb, pixel;

    rgb = xcb_get_colorpixel(color);
    r = rgb >> 16; g = rgb >> 8 & 0xFF; b = rgb & 0xFF;
    c = xcb_alloc_color_reply(dis, xcb_alloc_color(dis, map, r, g, b), NULL);
    if (!c)
        die("error: cannot allocate color '%s'\n", c);

    pixel = c->pixel; free(c);
    return pixel;
}

void grabkeys(void) {
    unsigned int code;
    xcb_ungrab_key(dis, XCB_GRAB_ANY, screen->root, XCB_MOD_MASK_ANY);
    for (unsigned int i=0; i<LENGTH(keys); i++) {
        code = keys[i].keysym;
        xcb_grab_key(dis, 1, screen->root, keys[i].mod,               code, XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC);
        xcb_grab_key(dis, 1, screen->root, keys[i].mod | numlockmask, code, XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC);
    }
#if 0
    xcb_grab_key(dis, 1, screen->root, XCB_MOD_MASK_ANY, XCB_GRAB_ANY, XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC);
#endif
    xcb_flush(dis);
}

void keypress(xcb_generic_event_t *e) {
    //KeySym keysym;
    //keysym = XKeycodeToKeysym(dis, (KeyCode)e->xkey.keycode, 0);
    xcb_key_press_event_t *ev = (xcb_key_press_event_t *)e;

    printf("keypress: code: %d mod: %d\n", ev->detail, ev->state);
    for (unsigned int i=0; i<LENGTH(keys); i++)
        if (ev->detail == keys[i].keysym && keys[i].mod == ev->state && keys[i].function)
                keys[i].function(&keys[i].arg);
    xcb_flush(dis);
}

void killclient() {
    if (!current) return;
    sendevent(current->win, WM_DELETE_WINDOW);
    removeclient(current);
}

void last_desktop() {
    change_desktop(&(Arg){.i = previous_desktop});
}

void maprequest(xcb_generic_event_t *e) {
    xcb_map_request_event_t            *ev = (xcb_map_request_event_t*)e;
    xcb_window_t                       windows[] = { ev->window }, transient;
    xcb_get_window_attributes_reply_t  *attr[1];
    xcb_get_property_cookie_t          prop_cookie;
    xcb_get_geometry_cookie_t          geom_cookie;
    xcb_icccm_get_wm_class_reply_t     ch;
    xcb_get_geometry_reply_t           *geometry;
    xcb_get_property_reply_t           *prop_reply;

    puts("map request");

    bool follow = false;
    int cd = current_desktop, newdsk = current_desktop;
    prop_cookie = xcb_icccm_get_wm_class(dis, ev->window);
    geom_cookie = xcb_get_geometry(dis, ev->window);

    xcb_icccm_get_wm_class_reply(dis, prop_cookie, &ch, NULL); /* TODO: error handling */
    printf("class: %s instance: %s\n", ch.class_name, ch.instance_name);
    for (unsigned int i=0; i<LENGTH(rules); i++)
        if (!strcmp(ch.class_name, rules[i].class) || !strcmp(ch.instance_name, rules[i].class)) {
            follow = rules[i].follow;
            newdsk = rules[i].desktop;
            break;
        }
    xcb_icccm_get_wm_class_reply_wipe(&ch);

    geometry = xcb_get_geometry_reply(dis, geom_cookie, NULL); /* TODO: error handling */
    if (geometry) {
        printf("geom: %ux%u+%d+%d\n", geometry->width, geometry->height,
                                      geometry->x,     geometry->y);
        free(geometry);
    }

    xcb_get_attributes(windows, attr, 1);
    if (attr[0]->override_redirect) return;
    if (wintoclient(ev->window))    return;

    select_desktop(newdsk);
    addwindow(ev->window);

    prop_cookie = xcb_icccm_get_wm_transient_for(dis, ev->window);
    xcb_icccm_get_wm_transient_for_reply(dis, prop_cookie, &transient, NULL); /* TODO: error handling */
    if (transient) current->istransient = 1;

    prop_cookie = xcb_get_property(dis, 0, screen->root, netatoms[NET_WM_STATE], XCB_ATOM, 0L, sizeof(xcb_atom_t));
    prop_reply  = xcb_get_property_reply(dis, prop_cookie, NULL);
    if (prop_reply) {
        setfullscreen(current, (prop_reply->type == netatoms[NET_FULLSCREEN]));
        free(prop_reply);
    }

    select_desktop(cd);
    if (cd == newdsk) {
        tile();
        xcb_map_window(dis, ev->window);
        update_current();
    } else if (follow) change_desktop(&(Arg){.i = newdsk});
    desktopinfo();
}

/* move the current client, to current->next
 * and current->next to current client's position
 */
void move_down() {
    if (!current || !head->next) return;
    for (client *t=head; t; t=t->next) if (t->isfullscreen) return;

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
    update_current();
}

/* move the current client, to the previous from current
 * and the previous from  current to current client's position
 */
void move_up() {
    if (!current || !head->next) return;
    for (client *t=head; t; t=t->next) if (t->isfullscreen) return;

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
    update_current();
}

void next_win() {
    if (!current || !head->next) return;
    current = ((prevfocus = current)->next) ? current->next : head;
    if (mode == MONOCLE) xcb_map_window(dis, current->win);
    update_current();
}

void prev_win() {
    if (!current || !head->next) return;
    if (head == (prevfocus = current)) while (current->next) current=current->next;
    else for (client *t=head; t; t=t->next) if (t->next == current) { current = t; break; }
    if (mode == MONOCLE) xcb_map_window(dis, current->win);
    update_current();
}

void propertynotify(xcb_generic_event_t *e) {
    xcb_property_notify_event_t *ev = (xcb_property_notify_event_t*)e;
    xcb_get_property_cookie_t cookie;
    xcb_icccm_wm_hints_t wmh;
    client *c;
    puts("xcb: property notify");
    if ((c = wintoclient(ev->window)))
        if (ev->atom == XCB_ICCCM_WM_ALL_HINTS) {
            puts("xcb: got hint!");
            cookie = xcb_icccm_get_wm_hints(dis, ev->window);
            if (xcb_icccm_get_wm_hints_reply(dis, cookie, &wmh, NULL)) /* TODO: error handling */
               c->isurgent = (wmh.flags & XCB_ICCCM_WM_HINT_X_URGENCY);
            desktopinfo();
        }
}

void quit(const Arg *arg) {
    retval = arg->i;
    running = false;
}

void removeclient(client *c) {
    client **p = NULL;
    int nd = 0, cd = current_desktop;
    for (bool found = false; nd<DESKTOPS && !found; nd++)
        for (select_desktop(nd), p = &head; *p && !(found = *p == c); p = &(*p)->next);
    *p = c->next;
    free(c);
    current = (prevfocus && prevfocus != current) ? prevfocus : (*p) ? (prevfocus = *p) : (prevfocus = head);
    select_desktop(cd);
    tile();
    if (mode == MONOCLE && cd == --nd && current) xcb_map_window(dis, current->win);
    update_current();
}

void resize_master(const Arg *arg) {
    int msz = master_size + arg->i;
    if ((mode == BSTACK ? wh : ww) - msz <= MINWSZ || msz <= MINWSZ) return;
    master_size = msz;
    tile();
}

void resize_stack(const Arg *arg) {
    growth += arg->i;
    tile();
}

void rotate_desktop(const Arg *arg) {
    change_desktop(&(Arg){.i = (current_desktop + DESKTOPS + arg->i) % DESKTOPS});
}

void run(void) {
    xcb_generic_event_t *ev;
    while(running)
        if ((ev = xcb_poll_for_event(dis)))
        {
            if (events[ev->response_type & ~0x80])
               events[ev->response_type & ~0x80](ev);
            else
               printf("unimplented event: %d\n", ev->response_type & ~0x80);
            free(ev);
        }
}

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

void select_desktop(int i) {
    if (i >= DESKTOPS || i == current_desktop) return;
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

void sendevent(xcb_window_t w, int atom) {
    if (atom >= WM_COUNT) return;
    xcb_client_message_event_t ev;
    ev.type = XCB_CLIENT_MESSAGE;
    ev.window = w;
    ev.type = wmatoms[WM_PROTOCOLS];
    ev.data.data32[0] = wmatoms[atom];
    ev.data.data32[1] = XCB_CURRENT_TIME;
    xcb_send_event(dis, 1, XCB_SEND_EVENT_DEST_POINTER_WINDOW, XCB_EVENT_MASK_NO_EVENT, (char*)&ev);

    puts("send event");
}

void setfullscreen(client *c, bool fullscreen) {
    xcb_change_property(dis, XCB_PROP_MODE_REPLACE, c->win, netatoms[NET_WM_STATE], XCB_ATOM, 32, sizeof(xcb_atom_t),
                       ((c->isfullscreen = fullscreen) ? &netatoms[NET_FULLSCREEN] : &XCB_ATOM_NULL));
    if (c->isfullscreen) xcb_move_resize(dis, c->win, 0, 0, ww + BORDER_WIDTH, wh + BORDER_WIDTH + PANEL_HEIGHT);
}

int setup_keyboard(void)
{
    xcb_get_modifier_mapping_cookie_t cookie;
    xcb_get_modifier_mapping_reply_t *reply;
    xcb_keycode_t                    *modmap;

    cookie  = xcb_get_modifier_mapping_unchecked(dis);
    reply   = xcb_get_modifier_mapping_reply(dis, cookie, NULL);
    if (!reply)
    {
       puts("error: failed to get modifier mapping");
       return -1;
    }

    modmap = xcb_get_modifier_mapping_keycodes(reply);
    if (!modmap)
    {
        puts("error: failed to get modifier mapping");
        return -1;
    }

    for (int i=0; i<8; i++)
       for (int j=0; j<reply->keycodes_per_modifier; j++)
       {
           xcb_keycode_t keycode = modmap[i * reply->keycodes_per_modifier + j];
           if (keycode == XCB_NO_SYMBOL) continue;
           if (NUMLOCK_KEYCODE == keycode) {
               printf("found num-lock %d\n", 1 << i);
               numlockmask = 1 << i;
               break;
           }
       }

    return 0;
}

int setup(int default_screen) {
    sigchld();
    screen = screen_of_display(dis, default_screen);
    if (!screen) die("error: cannot aquire screen");

    ww = screen->width_in_pixels  - BORDER_WIDTH;
    wh = screen->height_in_pixels - (SHOW_PANEL ? PANEL_HEIGHT : 0) - BORDER_WIDTH;
    master_size = ((mode == BSTACK) ? wh : ww) * MASTER_SIZE;
    for (unsigned int i=0; i<DESKTOPS; i++) save_desktop(i);
    change_desktop(&(Arg){.i = DEFAULT_DESKTOP});

    win_focus   = getcolor(FOCUS);
    win_unfocus = getcolor(UNFOCUS);

    /* setup keyboard */
    if (setup_keyboard() == -1)
        return -1;

    /* set up atoms for dialog/notification windows */
    xcb_get_atoms(WM_ATOM_NAME, wmatoms, WM_COUNT);
    xcb_get_atoms(NET_ATOM_NAME, netatoms, NET_COUNT);

    /* check if another wm is running */
    if (checkotherwm())
        return -1;

    grabkeys();

    /* set events */
    for (unsigned int i=0; i<XCB_NO_OPERATION; ++i) events[i] = NULL;
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

void swap_master() {
    if (!current || !head->next || mode == MONOCLE) return;
    for (client *t=head; t; t=t->next) if (t->isfullscreen) return;
    /* if current is head swap with next window */
    if (current == head) move_down();
    /* if not head, then head is always behind us, so move_up until is head */
    else while (current != head) move_up();
    current = head;
    tile();
    update_current();
}

void switch_mode(const Arg *arg) {
    if (mode == arg->i) return;
    if (mode == MONOCLE) for (client *c=head; c; c=c->next) xcb_map_window(dis, c->win);
    mode = arg->i;
    master_size = (mode == BSTACK ? wh : ww) * MASTER_SIZE;
    tile();
    update_current();
    desktopinfo();
}

void tile(void) {
    if (!head) return; /* nothing to arange */

    client *c;
    /* n:number of windows, d:difference, h:available height, z:client height */
    int n = 0, d = 0, h = wh + (showpanel ? 0 : PANEL_HEIGHT), z = mode == BSTACK ? ww : h;
    /* client's x,y coordinates, width and height */
    int cx = 0, cy = (TOP_PANEL && showpanel ? PANEL_HEIGHT : 0), cw = 0, ch = 0;

    for (n=0, c=head->next; c; c=c->next) if (!c->istransient && !c->isfullscreen) ++n;
    if (!head->next || head->next->istransient || mode == MONOCLE) {
        for (c=head; c; c=c->next) if (!c->isfullscreen && !c->istransient)
            xcb_move_resize(dis, c->win, cx, cy, ww + BORDER_WIDTH, h + BORDER_WIDTH);
    } else if ((mode == TILE || mode == BSTACK) && n) { /* adjust to match screen height/width */
        if (n>1) { d = (z - growth)%n + growth; z = (z - growth)/n; }
        if (!head->isfullscreen && !head->istransient)
            (mode == BSTACK) ? xcb_move_resize(dis, head->win, cx, cy, ww - BORDER_WIDTH, master_size - BORDER_WIDTH)
                             : xcb_move_resize(dis, head->win, cx, cy, master_size - BORDER_WIDTH,  h - BORDER_WIDTH);
        if (!head->next->isfullscreen && !head->next->istransient)
            (mode == BSTACK) ? xcb_move_resize(dis, head->next->win, cx, (cy += master_size),
                            (cw = z - BORDER_WIDTH) + d, (ch = h - master_size - BORDER_WIDTH))
                             : xcb_move_resize(dis, head->next->win, (cx += master_size), cy,
                            (cw = ww - master_size - BORDER_WIDTH), (ch = z - BORDER_WIDTH) + d);
        for ((mode==BSTACK)?(cx+=z+d):(cy+=z+d), c=head->next->next; c; c=c->next, (mode==BSTACK)?(cx+=z):(cy+=z))
            if (!c->isfullscreen && !c->istransient) xcb_move_resize(dis, c->win, cx, cy, cw, ch );
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
            if (!c->isfullscreen && !c->istransient) xcb_move_resize(dis, c->win, cx, cy, cw - BORDER_WIDTH, ch - BORDER_WIDTH);
            if (++rn >= rows) { rn = 0; cn++; }
        }
    } else fprintf(stderr, "error: no such layout mode: %d\n", mode);
    free(c);
}

void togglepanel() {
    showpanel = !showpanel;
    tile();
}

void unmapnotify(xcb_generic_event_t *e) {
    xcb_unmap_notify_event_t *ev = (xcb_unmap_notify_event_t *)e;
    client *c = wintoclient(ev->window);
    if (c && ev->event != screen->root) removeclient(c);
    desktopinfo();
}

void update_current(void) {
    if (!current) { xcb_delete_property(dis, screen->root, netatoms[NET_ACTIVE]); return; }
    int border_width = (!head->next || head->next->istransient || mode == MONOCLE) ? 0 : BORDER_WIDTH;

    for (client *c=head; c; c=c->next) {
        xcb_border_width(dis, c->win, (c->isfullscreen ? 0 : border_width));
        xcb_change_window_attributes(dis, c->win, XCB_CW_BORDER_PIXEL, (current == c ? &win_focus : &win_unfocus));
        if (CLICK_TO_FOCUS) xcb_grab_button(dis, 1, c->win, XCB_BUTTON_PRESS|XCB_BUTTON_RELEASE, XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC,
           XCB_NONE, XCB_NONE, XCB_BUTTON_INDEX_ANY, XCB_BUTTON_MASK_ANY);
    }
    xcb_change_property(dis, XCB_PROP_MODE_REPLACE, screen->root, netatoms[NET_ACTIVE], XCB_ATOM_WINDOW, 32, sizeof(xcb_window_t), (unsigned char *)&current->win);
    xcb_set_input_focus(dis, screen->root, current->win, XCB_CURRENT_TIME);
    xcb_raise_window(dis, current->win);
    if (CLICK_TO_FOCUS) xcb_ungrab_button(dis, XCB_BUTTON_INDEX_ANY, current->win, XCB_BUTTON_MASK_ANY);
    xcb_flush(dis);
}

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
    if (!(dis = xcb_connect(NULL, &default_screen))) die("error: cannot open display\n");
    if (setup(default_screen) != -1)
    {
      desktopinfo(); /* zero out every desktop on (re)start */
      run();
    }
    cleanup();
    xcb_disconnect(dis);
    return retval;
}

/* vim: set ts=4 sw=4 :*/
