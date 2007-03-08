/*      $Id$

        This program is free software; you can redistribute it and/or modify
        it under the terms of the GNU General Public License as published by
        the Free Software Foundation; either version 2, or (at your option)
        any later version.

        This program is distributed in the hope that it will be useful,
        but WITHOUT ANY WARRANTY; without even the implied warranty of
        MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
        GNU General Public License for more details.

        You should have received a copy of the GNU General Public License
        along with this program; if not, write to the Free Software
        Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.

        oroborus - (c) 2001 Ken Lynch
        xfwm4    - (c) 2002-2006 Olivier Fourdan

 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <string.h>

#include <X11/X.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/extensions/shape.h>

#include <glib.h>
#include <gdk/gdk.h>
#include <gdk/gdkx.h>
#include <gtk/gtk.h>
#ifdef HAVE_RANDR
#include <X11/extensions/Xrandr.h>
#endif
#include <libxfce4util/libxfce4util.h>

#include "misc.h"
#include "workspaces.h"
#include "settings.h"
#include "mywindow.h"
#include "frame.h"
#include "client.h"
#include "stacking.h"
#include "transients.h"
#include "focus.h"
#include "netwm.h"
#include "menu.h"
#include "hints.h"
#include "startup_notification.h"
#include "compositor.h"
#include "events.h"
#include "event_filter.h"

#ifndef CHECK_BUTTON_TIME
#define CHECK_BUTTON_TIME 0
#endif

#define WIN_IS_BUTTON(win)      ((win == MYWINDOW_XWINDOW(c->buttons[HIDE_BUTTON])) || \
                                 (win == MYWINDOW_XWINDOW(c->buttons[CLOSE_BUTTON])) || \
                                 (win == MYWINDOW_XWINDOW(c->buttons[MAXIMIZE_BUTTON])) || \
                                 (win == MYWINDOW_XWINDOW(c->buttons[SHADE_BUTTON])) || \
                                 (win == MYWINDOW_XWINDOW(c->buttons[STICK_BUTTON])))

#define DBL_CLICK_GRAB          (ButtonMotionMask | \
                                 PointerMotionMask | \
                                 ButtonPressMask | \
                                 ButtonReleaseMask)
                  
#define MODIFIER_MASK           (ShiftMask | \
                                 ControlMask | \
                                 AltMask | \
                                 MetaMask | \
                                 SuperMask | \
                                 HyperMask)

static GdkAtom atom_rcfiles = GDK_NONE;
static xfwmWindow menu_event_window;
static int edge_scroll_x = 0;
static int edge_scroll_y = 0;

/* Forward decl. */

static void handleEvent         (DisplayInfo *display_info,
                                 XEvent * ev);
static void menu_callback       (Menu * menu,
                                 MenuOp op,
                                 Window xid,
                                 gpointer menu_data,
                                 gpointer item_data);
static void show_window_menu    (Client *c,
                                 gint px,
                                 gint py,
                                 guint button,
                                 guint32 time);
static gboolean show_popup_cb   (GtkWidget * widget,
                                 GdkEventButton * ev,
                                 gpointer data);
static gboolean client_event_cb (GtkWidget * widget,
                                 GdkEventClient * ev,
                                 gpointer data);

typedef enum
{
    XFWM_BUTTON_UNDEFINED = 0,
    XFWM_BUTTON_DRAG = 1,
    XFWM_BUTTON_CLICK = 2,
    XFWM_BUTTON_CLICK_AND_DRAG = 3,
    XFWM_BUTTON_DOUBLE_CLICK = 4
}
XfwmButtonClickType;

typedef struct _XfwmButtonClickData XfwmButtonClickData;
struct _XfwmButtonClickData
{
    DisplayInfo *display_info;
    Window w;
    guint button;
    guint clicks;
    guint timeout;
    gint x;
    gint y;
    gint xcurrent;
    gint ycurrent;
    gboolean allow_double_click;
};

static gboolean
typeOfClick_break (gpointer data)
{
    XfwmButtonClickData *passdata;

    passdata = (XfwmButtonClickData *) data;
    if (passdata->timeout)
    {
        g_source_remove (passdata->timeout);
        passdata->timeout = 0;
    }

    gtk_main_quit ();

    return (TRUE);
}

static eventFilterStatus
typeOfClick_event_filter (XEvent * xevent, gpointer data)
{
    XfwmButtonClickData *passdata;
    eventFilterStatus status;
    gboolean keep_going;

    keep_going = TRUE;
    passdata = (XfwmButtonClickData *) data;
    status = EVENT_FILTER_STOP;

    /* Update the display time */
    myDisplayUpdateCurrentTime (passdata->display_info, xevent);

    if ((xevent->type == ButtonRelease) || (xevent->type == ButtonPress))
    {
        if (xevent->xbutton.button == passdata->button)
        {
            passdata->clicks++;
        }
        if (((XfwmButtonClickType) passdata->clicks == XFWM_BUTTON_DOUBLE_CLICK)
            || (!(passdata->allow_double_click) &&
                 (XfwmButtonClickType) passdata->clicks == XFWM_BUTTON_CLICK))
        {
            keep_going = FALSE;
        }
    }
    else if (xevent->type == MotionNotify)
    {
        passdata->xcurrent = xevent->xmotion.x_root;
        passdata->ycurrent = xevent->xmotion.y_root;
    }
    else if ((xevent->type == DestroyNotify) || (xevent->type == UnmapNotify))
    {
        if (xevent->xany.window == passdata->w)
        {
            /* Discard, mark the click as undefined */
            passdata->clicks = (guint) XFWM_BUTTON_UNDEFINED;
            keep_going = FALSE;
        }
        status = EVENT_FILTER_CONTINUE;
    }
    else
    {
        status = EVENT_FILTER_CONTINUE;
    }

    if ((ABS (passdata->x - passdata->xcurrent) > 1) ||
        (ABS (passdata->y - passdata->ycurrent) > 1) ||
        (!keep_going))
    {
        TRACE ("event loop now finished");
        typeOfClick_break (data);
    }

    return status;
}

static XfwmButtonClickType
typeOfClick (ScreenInfo *screen_info, Window w, XEvent * ev, gboolean allow_double_click)
{
    DisplayInfo *display_info;
    XfwmButtonClickData passdata;
    gboolean g;

    g_return_val_if_fail (screen_info != NULL, XFWM_BUTTON_UNDEFINED);
    g_return_val_if_fail (ev != NULL, XFWM_BUTTON_UNDEFINED);
    g_return_val_if_fail (w != None, XFWM_BUTTON_UNDEFINED);

    display_info = screen_info->display_info;
    XFlush (display_info->dpy);
    g = myScreenGrabPointer (screen_info, DBL_CLICK_GRAB, None, ev->xbutton.time);

    if (!g)
    {
        TRACE ("grab failed in typeOfClick");
        gdk_beep ();
        myScreenUngrabPointer (screen_info);
        return XFWM_BUTTON_UNDEFINED;
    }

    passdata.display_info = display_info;
    passdata.button = ev->xbutton.button;
    passdata.w = w;
    passdata.x = ev->xbutton.x_root;
    passdata.y = ev->xbutton.y_root;
    passdata.xcurrent = passdata.x;
    passdata.ycurrent = passdata.y;
    passdata.clicks = 1;
    passdata.allow_double_click = allow_double_click;
    passdata.timeout = g_timeout_add_full (G_PRIORITY_DEFAULT, 
                                           display_info->dbl_click_time,
                                           (GSourceFunc) typeOfClick_break,
                                           (gpointer) &passdata, NULL);

    TRACE ("entering typeOfClick loop");
    eventFilterPush (display_info->xfilter, typeOfClick_event_filter, &passdata);
    gtk_main ();
    eventFilterPop (display_info->xfilter);
    TRACE ("leaving typeOfClick loop");

    myScreenUngrabPointer (screen_info);
    XFlush (display_info->dpy);
    return (XfwmButtonClickType) passdata.clicks;
}

#if CHECK_BUTTON_TIME
static gboolean
check_button_time (XButtonEvent *ev)
{
    static Time last_button_time = (Time) CurrentTime;

    if (last_button_time > ev->time)
    {
        return FALSE;
    }

    last_button_time = ev->time;
    return TRUE;
}
#endif

static void
moveRequest (Client * c, XEvent * ev)
{
    if (FLAG_TEST (c->xfwm_flags, XFWM_FLAG_HAS_MOVE)
        && !FLAG_TEST (c->flags, CLIENT_FLAG_FULLSCREEN))
    {
        clientMove (c, ev);
    }
}

static void
resizeRequest (Client * c, int corner, XEvent * ev)
{
    if (!FLAG_TEST (c->flags, CLIENT_FLAG_FULLSCREEN))
    {
        if (FLAG_TEST_ALL (c->xfwm_flags,
                XFWM_FLAG_HAS_RESIZE | XFWM_FLAG_IS_RESIZABLE))
        {
            clientResize (c, corner, ev);
        }
        else if (FLAG_TEST (c->xfwm_flags, XFWM_FLAG_HAS_MOVE))
        {
            clientMove (c, ev);
        }
    }
}

static void
toggle_show_desktop (ScreenInfo *screen_info)
{
    screen_info->show_desktop = !screen_info->show_desktop;
    setHint (screen_info->display_info, screen_info->xroot, NET_SHOWING_DESKTOP,
             screen_info->show_desktop);
    sendRootMessage (screen_info, NET_SHOWING_DESKTOP, screen_info->show_desktop,
                     myDisplayGetCurrentTime (screen_info->display_info));
}

static void
handleMotionNotify (DisplayInfo *display_info, XMotionEvent * ev)
{
    TRACE ("entering handleMotionNotify");
}

static int
getKeyPressed (ScreenInfo *screen_info, XKeyEvent * ev)
{
    int key, state;

    state = ev->state & MODIFIER_MASK;
    for (key = 0; key < KEY_LAST; key++)
    {
        if ((screen_info->params->keys[key].keycode == ev->keycode)
            && (screen_info->params->keys[key].modifier == state))
        {
            break;
        }
    }

    return key;
}

static void
handleKeyPress (DisplayInfo *display_info, XKeyEvent * ev)
{
    ScreenInfo *screen_info;
    ScreenInfo *ev_screen_info;
    Client *c;
    int key;

    TRACE ("entering handleKeyEvent");

    ev_screen_info = myDisplayGetScreenFromRoot (display_info, ev->root);
    if (!ev_screen_info)
    {
        return;
    }

    c = clientGetFocus ();
    if (c)
    {
        screen_info = c->screen_info;
        key = getKeyPressed (screen_info, ev);

        switch (key)
        {
            case KEY_MOVE_UP:
            case KEY_MOVE_DOWN:
            case KEY_MOVE_LEFT:
            case KEY_MOVE_RIGHT:
                moveRequest (c, (XEvent *) ev);
                break;
            case KEY_RESIZE_UP:
            case KEY_RESIZE_DOWN:
            case KEY_RESIZE_LEFT:
            case KEY_RESIZE_RIGHT:
                if (FLAG_TEST_ALL (c->xfwm_flags, XFWM_FLAG_HAS_RESIZE | XFWM_FLAG_IS_RESIZABLE))
                {
                    clientResize (c, CORNER_BOTTOM_RIGHT, (XEvent *) ev);
                }
                break;
            case KEY_CYCLE_WINDOWS:
                clientCycle (c, (XEvent *) ev);
                break;
            case KEY_CLOSE_WINDOW:
                clientClose (c);
                break;
            case KEY_HIDE_WINDOW:
                if (CLIENT_CAN_HIDE_WINDOW (c))
                {
                    clientHide (c, c->win_workspace, TRUE);
                }
                break;
            case KEY_MAXIMIZE_WINDOW:
                clientToggleMaximized (c, WIN_STATE_MAXIMIZED, TRUE);
                break;
            case KEY_MAXIMIZE_VERT:
                clientToggleMaximized (c, WIN_STATE_MAXIMIZED_VERT, TRUE);
                break;
            case KEY_MAXIMIZE_HORIZ:
                clientToggleMaximized (c, WIN_STATE_MAXIMIZED_HORIZ, TRUE);
                break;
            case KEY_SHADE_WINDOW:
                clientToggleShaded (c);
                break;
            case KEY_STICK_WINDOW:
                if (CLIENT_CAN_STICK_WINDOW(c))
                {
                    clientToggleSticky (c, TRUE);
                    frameDraw (c, FALSE);
                }
                break;
            case KEY_RAISE_WINDOW:
                clientRaise (c, None);
                break;
            case KEY_LOWER_WINDOW:
                clientLower (c, None);
                break;
            case KEY_TOGGLE_FULLSCREEN:
                clientToggleFullscreen (c);
                break;
            case KEY_MOVE_NEXT_WORKSPACE:
                workspaceSwitch (screen_info, screen_info->current_ws + 1, c, TRUE, ev->time);
                break;
            case KEY_MOVE_PREV_WORKSPACE:
                workspaceSwitch (screen_info, screen_info->current_ws - 1, c, TRUE, ev->time);
                break;
            case KEY_MOVE_UP_WORKSPACE:
                workspaceMove (screen_info, -1, 0, c, ev->time);
                break;
            case KEY_MOVE_DOWN_WORKSPACE:
                workspaceMove (screen_info, 1, 0, c, ev->time);
                break;
            case KEY_MOVE_LEFT_WORKSPACE:
                workspaceMove (screen_info, 0, -1, c, ev->time);
                break;
            case KEY_MOVE_RIGHT_WORKSPACE:
                workspaceMove (screen_info, 0, 1, c, ev->time);
                break;
            case KEY_MOVE_WORKSPACE_1:
            case KEY_MOVE_WORKSPACE_2:
            case KEY_MOVE_WORKSPACE_3:
            case KEY_MOVE_WORKSPACE_4:
            case KEY_MOVE_WORKSPACE_5:
            case KEY_MOVE_WORKSPACE_6:
            case KEY_MOVE_WORKSPACE_7:
            case KEY_MOVE_WORKSPACE_8:
            case KEY_MOVE_WORKSPACE_9:
            case KEY_MOVE_WORKSPACE_10:
            case KEY_MOVE_WORKSPACE_11:
            case KEY_MOVE_WORKSPACE_12:
                if (key - KEY_MOVE_WORKSPACE_1 < screen_info->workspace_count)
                {
                    clientRaise (c, None);
                    workspaceSwitch (screen_info, key - KEY_MOVE_WORKSPACE_1, c, TRUE, ev->time);
                }
                break;
            case KEY_POPUP_MENU:
                /* 
                   We need to release the events here prior to grabbing 
                   the keyboard in gtk menu otherwise we end with a dead lock...
                  */
                XAllowEvents (display_info->dpy, AsyncKeyboard, CurrentTime);
                show_window_menu (c, frameX (c) + frameLeft (c), 
                                     frameY (c) + frameTop (c), 
                                     Button1, GDK_CURRENT_TIME);
                /* 'nuff for now */
                return;

                break;
            default:
                break;
        }
    }
    else
    {
        key = getKeyPressed (ev_screen_info, ev);
        switch (key)
        {
            case KEY_CYCLE_WINDOWS:
                if (ev_screen_info->clients)
                {
                    clientCycle (ev_screen_info->clients->prev, (XEvent *) ev);
                }
                break;
            case KEY_CLOSE_WINDOW:
                if (display_info->session)
                {
                    logout_session (display_info->session);
                }
                break;
            default:
                break;
        }
    }

    switch (key)
    {
        case KEY_NEXT_WORKSPACE:
            workspaceSwitch (ev_screen_info, ev_screen_info->current_ws + 1, NULL, TRUE, ev->time);
            break;
        case KEY_PREV_WORKSPACE:
            workspaceSwitch (ev_screen_info, ev_screen_info->current_ws - 1, NULL, TRUE, ev->time);
            break;
        case KEY_UP_WORKSPACE:
            workspaceMove(ev_screen_info, -1, 0, NULL, ev->time);
            break;
        case KEY_DOWN_WORKSPACE:
            workspaceMove(ev_screen_info, 1, 0, NULL, ev->time);
            break;
        case KEY_LEFT_WORKSPACE:
            workspaceMove(ev_screen_info, 0, -1, NULL, ev->time);
            break;
        case KEY_RIGHT_WORKSPACE:
            workspaceMove(ev_screen_info, 0, 1, NULL, ev->time);
            break;
        case KEY_ADD_WORKSPACE:
            workspaceSetCount (ev_screen_info, ev_screen_info->workspace_count + 1);
            break;
        case KEY_DEL_WORKSPACE:
            workspaceSetCount (ev_screen_info, ev_screen_info->workspace_count - 1);
            break;
        case KEY_WORKSPACE_1:
        case KEY_WORKSPACE_2:
        case KEY_WORKSPACE_3:
        case KEY_WORKSPACE_4:
        case KEY_WORKSPACE_5:
        case KEY_WORKSPACE_6:
        case KEY_WORKSPACE_7:
        case KEY_WORKSPACE_8:
        case KEY_WORKSPACE_9:
        case KEY_WORKSPACE_10:
        case KEY_WORKSPACE_11:
        case KEY_WORKSPACE_12:
            if (key - KEY_WORKSPACE_1 < ev_screen_info->workspace_count)
            {
                workspaceSwitch (ev_screen_info, key - KEY_WORKSPACE_1, NULL, TRUE, ev->time);
            }
            break;
        case KEY_SHOW_DESKTOP:
            toggle_show_desktop (ev_screen_info);
            break;
        default:
            break;
    }
    XAllowEvents (display_info->dpy, AsyncKeyboard, CurrentTime);
}

/* User has clicked on an edge or corner.
 * Button 1 : Raise and resize
 * Button 2 : Move
 * Button 3 : Resize
 */
static void
edgeButton (Client * c, int part, XButtonEvent * ev)
{
    ScreenInfo *screen_info;
    int state;

    screen_info = c->screen_info;
    state = ev->state & MODIFIER_MASK;

    if (ev->button == Button2)
    {
        XfwmButtonClickType tclick;

        tclick = typeOfClick (screen_info, c->window, (XEvent *) ev, FALSE);
        if (tclick == XFWM_BUTTON_CLICK)
        {
            clientLower (c, None);
        }
        else if (tclick != XFWM_BUTTON_UNDEFINED)
        {
            moveRequest (c, (XEvent *) ev);
        }
    }
    else if ((ev->button == Button1) || (ev->button == Button3))
    {
        if ((ev->button == Button1) ||
            ((screen_info->params->easy_click) && (state == screen_info->params->easy_click)))
        {
            if (!(c->type & WINDOW_TYPE_DONT_FOCUS))
            {
                clientSetFocus (screen_info, c, ev->time, NO_FOCUS_FLAG);
            }
            clientRaise (c, None);
        }
        resizeRequest (c, part, (XEvent *) ev);
    }
}

static int 
edgeGetPart (Client *c, XButtonEvent * ev)
{
    int part, x_corner_pixels, y_corner_pixels, x_distance, y_distance;

    /* Corner is 1/3 of the side */
    x_corner_pixels = MAX(c->width / 3, 50);
    y_corner_pixels = MAX(c->height / 3, 50);

    /* Distance from event to edge of client window */
    x_distance = c->width / 2 - abs(c->width / 2 - ev->x);
    y_distance = c->height / 2 - abs(c->height / 2 - ev->y);

    /* Set a sensible default value */
    part = CORNER_BOTTOM_RIGHT;

    if (x_distance < x_corner_pixels && y_distance < y_corner_pixels)
    {
        /* In a corner */
        if (ev->x < c->width / 2)
        {
            if (ev->y < c->height / 2)
            {
                part = CORNER_TOP_LEFT;
            }
            else
            {
                part = CORNER_BOTTOM_LEFT;
            }
        }
        else
        {
            if (ev->y < c->height / 2)
            {
                part = CORNER_TOP_RIGHT;
            }
            else
            {
                part = CORNER_BOTTOM_RIGHT;
            }
        }
    }
    else
    {
        /* Not a corner - some side */
        if (x_distance / x_corner_pixels < y_distance / y_corner_pixels)
        {
            /* Left or right side */
            if (ev->x < c->width / 2)
            {
                part = CORNER_COUNT + SIDE_LEFT;
            }
            else
            {
                part = CORNER_COUNT + SIDE_RIGHT;
            }
        }
        else
        {
            /* Top or bottom side */
            if (ev->y < c->height / 2)
            {
                part = CORNER_COUNT + SIDE_TOP;
            }
            else
            {
                part = CORNER_COUNT + SIDE_BOTTOM;
            }
        }
    }

    return part;
}
static void
button1Action (Client * c, XButtonEvent * ev)
{
    ScreenInfo *screen_info;
    DisplayInfo *display_info;
    XEvent copy_event;
    XfwmButtonClickType tclick;

    g_return_if_fail (c != NULL);
    g_return_if_fail (ev != NULL);

    screen_info = c->screen_info;
    display_info = screen_info->display_info;

    if (!(c->type & WINDOW_TYPE_DONT_FOCUS))
    {
        clientSetFocus (screen_info, c, ev->time, NO_FOCUS_FLAG);
    }
    clientRaise (c, None);

    memcpy(&copy_event, ev, sizeof(XEvent));
    tclick = typeOfClick (screen_info, c->window, &copy_event, TRUE);

    if ((tclick == XFWM_BUTTON_DRAG)
        || (tclick == XFWM_BUTTON_CLICK_AND_DRAG))
    {
        moveRequest (c, (XEvent *) ev);
    }
    else if (tclick == XFWM_BUTTON_DOUBLE_CLICK)
    {
        switch (screen_info->params->double_click_action)
        {
            case ACTION_MAXIMIZE:
                clientToggleMaximized (c, WIN_STATE_MAXIMIZED, TRUE);
                break;
            case ACTION_SHADE:
                clientToggleShaded (c);
                break;
            case ACTION_HIDE:
                if (CLIENT_CAN_HIDE_WINDOW (c))
                {
                    clientHide (c, c->win_workspace, TRUE);
                }
                break;
        }
    }
}

static void
titleButton (Client * c, int state, XButtonEvent * ev)
{
    ScreenInfo *screen_info;
    DisplayInfo *display_info;

    g_return_if_fail (c != NULL);
    g_return_if_fail (ev != NULL);

    /* Get Screen data from the client itself */
    screen_info = c->screen_info;
    display_info = screen_info->display_info;

    if (ev->button == Button1)
    {
        button1Action (c, ev);
    }
    else if (ev->button == Button2)
    {
        clientLower (c, None);
    }
    else if (ev->button == Button3)
    {
        /*
           We need to copy the event to keep the original event untouched
           for gtk to handle it (in case we open up the menu)
         */

        XEvent copy_event;
        XfwmButtonClickType tclick;

        memcpy(&copy_event, ev, sizeof(XEvent));
        tclick = typeOfClick (screen_info, c->window, &copy_event, FALSE);

        if (tclick == XFWM_BUTTON_DRAG)
        {
            moveRequest (c, (XEvent *) ev);
        }
        else if (tclick != XFWM_BUTTON_UNDEFINED)
        {
            if (!(c->type & WINDOW_TYPE_DONT_FOCUS))
            {
                clientSetFocus (screen_info, c, ev->time, NO_FOCUS_FLAG);
            }
            if (screen_info->params->raise_on_click)
            {
                clientRaise (c, None);
            }
            ev->window = ev->root;
            if (screen_info->button_handler_id)
            {
                g_signal_handler_disconnect (GTK_OBJECT (myScreenGetGtkWidget (screen_info)), screen_info->button_handler_id);
            }
            screen_info->button_handler_id = g_signal_connect (GTK_OBJECT (myScreenGetGtkWidget (screen_info)),
                                                      "button_press_event", GTK_SIGNAL_FUNC (show_popup_cb), (gpointer) c);
            /* Let GTK handle this for us. */
        }
    }
    else if (ev->button == Button4)
    {

        /* Mouse wheel scroll up */

        if (state == AltMask)
        {
            clientIncOpacity(c);
        }
        else if (!FLAG_TEST (c->flags, CLIENT_FLAG_SHADED))
        {
            clientShade (c);
        }
    }
    else if (ev->button == Button5)
    {
        /* Mouse wheel scroll down */

        if (state == AltMask)
        {
            clientDecOpacity(c);
        }
        else if (FLAG_TEST (c->flags, CLIENT_FLAG_SHADED))
        {
            clientUnshade (c);
        }
    }
    else if (ev->button == Button6)
    {
        /* Mouse wheel scroll left, or left side button */
        clientDecOpacity(c);
    }
    else if (ev->button == Button7)
    {
        /* Mouse wheel scroll right, or right side button */
        clientIncOpacity(c);
    }
}

static void
rootScrollButton (DisplayInfo *display_info, XButtonEvent * ev)
{
    static Time lastscroll = (Time) CurrentTime;
    ScreenInfo *screen_info;

    if ((ev->time - lastscroll) < 25)  /* ms */
    {
        /* Too many events in too little time, drop this event... */
        return;
    }
    lastscroll = ev->time;

    /* Get the screen structure from the root of the event */
    screen_info = myDisplayGetScreenFromRoot (display_info, ev->root);
    if (!screen_info)
    {
        return;
    }

    if (ev->button == Button4)
    {
        workspaceSwitch (screen_info, screen_info->current_ws - 1, NULL, TRUE, ev->time);
    }
    else if (ev->button == Button5)
    {
        workspaceSwitch (screen_info, screen_info->current_ws + 1, NULL, TRUE, ev->time);
    }
}


static void
handleButtonPress (DisplayInfo *display_info, XButtonEvent * ev)
{
    ScreenInfo *screen_info;
    Client *c;
    Window win;
    int state, part;
    gboolean replay;

    TRACE ("entering handleButtonPress");

#if CHECK_BUTTON_TIME
    /* Avoid treating the same event twice */
    if (!check_button_time (ev))
    {
        TRACE ("ignoring ButtonPress event because it has been already handled");
        return;
    }
#endif

    replay = FALSE;
    c = myDisplayGetClientFromWindow (display_info, ev->window, ANY);
    if (c)
    {
        state = ev->state & MODIFIER_MASK;
        win = ev->subwindow;
        screen_info = c->screen_info;

        if ((ev->button == Button1) && (screen_info->params->easy_click) && (state == screen_info->params->easy_click))
        {
            button1Action (c, ev);
        }
        else if ((ev->button == Button2) && (screen_info->params->easy_click) && (state == screen_info->params->easy_click))
        {
            clientLower (c, None);
        }
        else if ((ev->button == Button3) && (screen_info->params->easy_click) && (state == screen_info->params->easy_click))
        {
            part = edgeGetPart (c, ev);
            edgeButton (c, part, ev);
        }
        else if (WIN_IS_BUTTON (win))
        {
            if (ev->button <= Button3)
            {
                if (!(c->type & WINDOW_TYPE_DONT_FOCUS))
                {
                    clientSetFocus (screen_info, c, ev->time, NO_FOCUS_FLAG);
                }
                if (screen_info->params->raise_on_click)
                {
                    clientClearDelayedRaise ();
                    clientRaise (c, None);
                }
                clientButtonPress (c, win, ev);
            }
        }
        else if (win == MYWINDOW_XWINDOW (c->title))
        {
            titleButton (c, state, ev);
        }
        else if (win == MYWINDOW_XWINDOW (c->buttons[MENU_BUTTON]))
        {
            if (ev->button == Button1)
            {
                /*
                   We need to copy the event to keep the original event untouched
                   for gtk to handle it (in case we open up the menu)
                 */

                XEvent copy_event;
                XfwmButtonClickType tclick;

                memcpy(&copy_event, ev, sizeof(XEvent));
                tclick = typeOfClick (screen_info, c->window, &copy_event, TRUE);

                if (tclick == XFWM_BUTTON_DOUBLE_CLICK)
                {
                    clientClose (c);
                }
                else if (tclick != XFWM_BUTTON_UNDEFINED)
                {
                    if (!(c->type & WINDOW_TYPE_DONT_FOCUS))
                    {
                        clientSetFocus (screen_info, c, ev->time, NO_FOCUS_FLAG);
                    }
                    if (screen_info->params->raise_on_click)
                    {
                        clientClearDelayedRaise ();
                        clientRaise (c, None);
                    }
                    ev->window = ev->root;
                    if (screen_info->button_handler_id)
                    {
                        g_signal_handler_disconnect (GTK_OBJECT (myScreenGetGtkWidget (screen_info)), screen_info->button_handler_id);
                    }
                    screen_info->button_handler_id = g_signal_connect (GTK_OBJECT (myScreenGetGtkWidget (screen_info)),
                                                              "button_press_event", GTK_SIGNAL_FUNC (show_popup_cb), (gpointer) c);
                    /* Let GTK handle this for us. */
                }
            }
        }
        else if ((win == MYWINDOW_XWINDOW (c->corners[CORNER_TOP_LEFT]))
            && (state == 0))
        {
            edgeButton (c, CORNER_TOP_LEFT, ev);
        }
        else if ((win == MYWINDOW_XWINDOW (c->corners[CORNER_TOP_RIGHT]))
            && (state == 0))
        {
            edgeButton (c, CORNER_TOP_RIGHT, ev);
        }
        else if ((win == MYWINDOW_XWINDOW (c->corners[CORNER_BOTTOM_LEFT]))
            && (state == 0))
        {
            edgeButton (c, CORNER_BOTTOM_LEFT, ev);
        }
        else if ((win == MYWINDOW_XWINDOW (c->corners[CORNER_BOTTOM_RIGHT]))
            && (state == 0))
        {
            edgeButton (c, CORNER_BOTTOM_RIGHT, ev);
        }
        else if ((win == MYWINDOW_XWINDOW (c->sides[SIDE_BOTTOM]))
            && (state == 0))
        {
            edgeButton (c, CORNER_COUNT + SIDE_BOTTOM, ev);
        }
        else if ((win == MYWINDOW_XWINDOW (c->sides[SIDE_LEFT]))
            && (state == 0))
        {
            edgeButton (c, CORNER_COUNT + SIDE_LEFT, ev);
        }
        else if ((win == MYWINDOW_XWINDOW (c->sides[SIDE_RIGHT]))
            && (state == 0))
        {
            edgeButton (c, CORNER_COUNT + SIDE_RIGHT, ev);
        }
        else if (ev->window == c->window)
        {
            clientPassGrabMouseButton (c);
            if (((screen_info->params->raise_with_any_button) && (c->type & WINDOW_REGULAR_FOCUSABLE)) || (ev->button == Button1))
            {
                if (!(c->type & WINDOW_TYPE_DONT_FOCUS))
                {
                    clientSetFocus (screen_info, c, ev->time, NO_FOCUS_FLAG);
                }
                if ((screen_info->params->raise_on_click) ||
                    !FLAG_TEST (c->xfwm_flags, XFWM_FLAG_HAS_BORDER))
                {
                    clientClearDelayedRaise ();
                    clientRaise (c, None);
                }
            }
            replay = TRUE;
        }

        if (replay)
        {
            XAllowEvents (display_info->dpy, ReplayPointer, CurrentTime);
        }
        else
        {
            XAllowEvents (display_info->dpy, SyncPointer, CurrentTime);
        }

        return;
    }

    /*
       The event did not occur in one of our known good client...
       Get the screen structure from the root of the event.
     */
    screen_info = myDisplayGetScreenFromRoot (display_info, ev->root);
    if (!screen_info)
    {
        return;
    }

    if ((ev->window == screen_info->xroot) && (screen_info->params->scroll_workspaces)
            && ((ev->button == Button4) || (ev->button == Button5)))
    {
        rootScrollButton (display_info, ev);
    }
    else
    {
        XUngrabPointer (display_info->dpy, ev->time);
        XSendEvent (display_info->dpy, screen_info->xfwm4_win, FALSE, SubstructureNotifyMask, (XEvent *) ev);
    }
}

static void
handleButtonRelease (DisplayInfo *display_info, XButtonEvent * ev)
{
    ScreenInfo *screen_info;

    TRACE ("entering handleButtonRelease");

#if CHECK_BUTTON_TIME
    /* Avoid treating the same event twice */
    if (!check_button_time (ev))
    {
        TRACE ("ignoring ButtonRelease event because it has been already handled");
        return;
    }
#endif

    /* Get the screen structure from the root of the event */
    screen_info = myDisplayGetScreenFromRoot (display_info, ev->root);
    if (!screen_info)
    {
        return;
    }

    XSendEvent (display_info->dpy, screen_info->xfwm4_win, FALSE, SubstructureNotifyMask, (XEvent *) ev);
}

static void
handleDestroyNotify (DisplayInfo *display_info, XDestroyWindowEvent * ev)
{
    Client *c;
#ifdef ENABLE_KDE_SYSTRAY_PROXY
    ScreenInfo *screen_info;
#endif

    TRACE ("entering handleDestroyNotify");
    TRACE ("DestroyNotify on window (0x%lx)", ev->window);

#ifdef ENABLE_KDE_SYSTRAY_PROXY
    screen_info = myDisplayGetScreenFromSystray (display_info, ev->window);
    if (screen_info)
    {
        /* systray window is gone */
        screen_info->systray = None;
        return;
    }
#endif

    c = myDisplayGetClientFromWindow (display_info, ev->window, WINDOW);
    if (c)
    {
        TRACE ("DestroyNotify for \"%s\" (0x%lx)", c->name, c->window);
        clientPassFocus (c->screen_info, c, c);
        clientUnframe (c, FALSE);
    }
}

static void
handleMapRequest (DisplayInfo *display_info, XMapRequestEvent * ev)
{
    Client *c;

    TRACE ("entering handleMapRequest");
    TRACE ("MapRequest on window (0x%lx)", ev->window);

    if (ev->window == None)
    {
        TRACE ("Mapping None ???");
        return;
    }

    c = myDisplayGetClientFromWindow (display_info, ev->window, WINDOW);
    if (c)
    {
        ScreenInfo *screen_info = c->screen_info;

        TRACE ("handleMapRequest: clientShow");

        if (FLAG_TEST (c->xfwm_flags, XFWM_FLAG_MAP_PENDING))
        {
            TRACE ("Ignoring MapRequest on window (0x%lx)", ev->window);
            return;
        }

        clientShow (c, TRUE);
        clientClearAllShowDesktop (screen_info);

        if (FLAG_TEST (c->flags, CLIENT_FLAG_STICKY) ||
            (c->win_workspace == screen_info->current_ws))
        {
            clientFocusNew(c);
        }
    }
    else
    {
        TRACE ("handleMapRequest: clientFrame");
        clientFrame (display_info, ev->window, FALSE);
    }
}

static void
handleMapNotify (DisplayInfo *display_info, XMapEvent * ev)
{
    Client *c;

    TRACE ("entering handleMapNotify");
    TRACE ("MapNotify on window (0x%lx)", ev->window);

    c = myDisplayGetClientFromWindow (display_info, ev->window, WINDOW);
    if (c)
    {
        TRACE ("MapNotify for \"%s\" (0x%lx)", c->name, c->window);
        if (FLAG_TEST (c->xfwm_flags, XFWM_FLAG_MAP_PENDING))
        {
            FLAG_UNSET (c->xfwm_flags, XFWM_FLAG_MAP_PENDING);
        }
    }
}

static void
handleUnmapNotify (DisplayInfo *display_info, XUnmapEvent * ev)
{
    ScreenInfo *screen_info;
    Client *c;

    TRACE ("entering handleUnmapNotify");
    TRACE ("UnmapNotify on window (0x%lx)", ev->window);

    if (ev->from_configure)
    {
        TRACE ("Ignoring UnmapNotify caused by parent's resize");
        return;
    }

    screen_info = myDisplayGetScreenFromWindow (display_info, ev->window);
    if (screen_info && (ev->event != ev->window) && (ev->event != screen_info->xroot || !ev->send_event))
    {
        TRACE ("handleUnmapNotify (): Event ignored");
        return;
    }

    c = myDisplayGetClientFromWindow (display_info, ev->window, WINDOW);
    if (c)
    {
        TRACE ("UnmapNotify for \"%s\" (0x%lx)", c->name, c->window);
        TRACE ("ignore_unmap for \"%s\" is %i", c->name, c->ignore_unmap);

        if (FLAG_TEST (c->xfwm_flags, XFWM_FLAG_MAP_PENDING))
        {
            /*
             * This UnmapNotify event is caused by reparenting
             * so we just ignore it, so the window won't return
             * to withdrawn state by mistake.
             */
            TRACE ("Client \"%s\" is not mapped, event ignored", c->name);
            return;
        }

        screen_info = c->screen_info;

        /*
         * ICCCM spec states that a client wishing to switch
         * to WithdrawnState should send a synthetic UnmapNotify
         * with the event field set to root if the client window
         * is already unmapped.
         * Therefore, bypass the ignore_unmap counter and
         * unframe the client.
         */
        if ((ev->event == screen_info->xroot) && (ev->send_event))
        {
            TRACE ("ICCCM UnmapNotify for \"%s\"", c->name);
            clientPassFocus (screen_info, c, c);
            clientUnframe (c, FALSE);
            return;
        }

        if (c->ignore_unmap)
        {
            c->ignore_unmap--;
            TRACE ("ignore_unmap for \"%s\" is now %i",
                 c->name, c->ignore_unmap);
        }
        else
        {
            TRACE ("unmapping \"%s\" as ignore_unmap is %i",
                 c->name, c->ignore_unmap);
            clientPassFocus (screen_info, c, c);
            clientUnframe (c, FALSE);
        }
    }
}

static gboolean
update_screen_idle_cb (gpointer data)
{
    ScreenInfo *screen_info;
    DisplayInfo *display_info;

    TRACE ("entering update_screen_idle_cb");

    screen_info = (ScreenInfo *) data;
    g_return_val_if_fail (screen_info, FALSE);

    display_info = screen_info->display_info;
    setNetWorkarea (display_info, screen_info->xroot, screen_info->workspace_count,
                    screen_info->width, screen_info->height, screen_info->margins);
    placeSidewalks (screen_info, screen_info->params->wrap_workspaces);
    clientScreenResize (screen_info);
    compositorUpdateScreenSize (screen_info);

    return FALSE;
}

static void
handleConfigureNotify (DisplayInfo *display_info, XConfigureEvent * ev)
{
    ScreenInfo *screen_info;

    TRACE ("entering handleConfigureNotify");

    screen_info = myDisplayGetScreenFromRoot (display_info, ev->window);
    if (!screen_info)
    {
        return;
    }

    if (display_info->have_xrandr)
    {
#ifdef HAVE_RANDR
        XRRUpdateConfiguration ((XEvent *) ev);
#endif
    }
    else
    {
        TRACE ("ConfigureNotify on the screen_info->xroot win (0x%lx)", ev->window);
        screen_info->xscreen->width  = ev->width;
        screen_info->xscreen->height = ev->height;
    }

    screen_info->width = WidthOfScreen (screen_info->xscreen);
    screen_info->height = HeightOfScreen (screen_info->xscreen);

    /*
       We need to use an idle function to update our screen layout to give gdk the
       time to update its internal structures for Xinerama and monitor size,
       otherwise the functions gdk_screen_get_monitor_geometry () don't return
       accurate values...
     */
    g_idle_add_full (G_PRIORITY_DEFAULT_IDLE, update_screen_idle_cb, screen_info, NULL);
}

static void
handleConfigureRequest (DisplayInfo *display_info, XConfigureRequestEvent * ev)
{
    Client *c = NULL;
    XWindowChanges wc;

    TRACE ("entering handleConfigureRequest");
    TRACE ("ConfigureRequest on window (0x%lx)", ev->window);

    wc.x = ev->x;
    wc.y = ev->y;
    wc.width = ev->width;
    wc.height = ev->height;
    wc.sibling = ev->above;
    wc.stack_mode = ev->detail;
    wc.border_width = ev->border_width;

    c = myDisplayGetClientFromWindow (display_info, ev->window, WINDOW);
    if (!c)
    {
        /* Some app tend or try to manipulate the wm frame to achieve fullscreen mode */
        c = myDisplayGetClientFromWindow (display_info, ev->window, FRAME);
        if (c)
        {
            TRACE ("client %s (0x%lx) is attempting to manipulate its frame!", c->name, c->window);
            if (ev->value_mask & CWX)
            {
                wc.x += frameLeft (c);
            }
            if (ev->value_mask & CWY)
            {
                wc.y += frameTop (c);
            }
            if (ev->value_mask & CWWidth)
            {
                wc.width -= frameLeft (c) + frameRight (c);
            }
            if (ev->value_mask & CWHeight)
            {
                wc.height -= frameTop (c) + frameBottom (c);
            }
            /* We don't allow changing stacking order by accessing the frame
               window because that would break the layer management in xfwm4
             */
            ev->value_mask &= ~(CWSibling | CWStackMode);
        }
    }
    if (c)
    {
        gboolean constrained = FALSE;
        ScreenInfo *screen_info = c->screen_info;

        TRACE ("handleConfigureRequest managed window \"%s\" (0x%lx)", c->name, c->window);
        if (FLAG_TEST (c->xfwm_flags, XFWM_FLAG_MOVING_RESIZING))
        {
            /* Sorry, but it's not the right time for configure request */
            return;
        }
        if (c->type == WINDOW_DESKTOP)
        {
            /* Ignore stacking request for DESKTOP windows */
            ev->value_mask &= ~(CWSibling | CWStackMode);
        }
        clientCoordGravitate (c, APPLY, &wc.x, &wc.y);
        if (FLAG_TEST (c->flags, CLIENT_FLAG_FULLSCREEN))
        {
            GdkRectangle rect;
            gint monitor_nbr;
            int cx, cy;

            /* size request from fullscreen windows get fullscreen */

            cx = frameX (c) + (frameWidth (c) / 2);
            cy = frameY (c) + (frameHeight (c) / 2);

            monitor_nbr = find_monitor_at_point (screen_info->gscr, cx, cy);
            gdk_screen_get_monitor_geometry (screen_info->gscr, monitor_nbr, &rect);

            wc.x = rect.x;
            wc.y = rect.y;
            wc.width = rect.width;
            wc.height = rect.height;

            ev->value_mask |= (CWX | CWY | CWWidth | CWHeight);
        }
        else if (FLAG_TEST_ALL (c->flags, CLIENT_FLAG_MAXIMIZED)
                 && (screen_info->params->borderless_maximize))
        {
            wc.x = c->x;
            wc.y = c->y;
            wc.width = c->width;
            wc.height = c->height;
        }
        /* Clean up buggy requests that set all flags */
        if ((ev->value_mask & CWX) && (wc.x == c->x))
        {
            ev->value_mask &= ~CWX;
        }
        if ((ev->value_mask & CWY) && (wc.y == c->y))
        {
            ev->value_mask &= ~CWY;
        }
        if ((ev->value_mask & CWWidth) && (wc.width == c->width))
        {
            ev->value_mask &= ~CWWidth;
        }
        if ((ev->value_mask & CWHeight) && (wc.height == c->height))
        {
            ev->value_mask &= ~CWHeight;
        }
        /* Still a move/resize after cleanup? */
        if (ev->value_mask & (CWX | CWY | CWWidth | CWHeight))
        {
            if (FLAG_TEST (c->flags, CLIENT_FLAG_MAXIMIZED))
            {
                clientRemoveMaximizeFlag (c);
            }
            constrained = TRUE;
        }

        /* 
           Let's say that if the client performs a XRaiseWindow, we show the window if focus 
           stealing prevention is not activated, otherwise we just set the "demands attention"
           flag...
         */
        if ((ev->value_mask & CWStackMode) && (wc.stack_mode == Above) && (wc.sibling == None))
        {
            Client *last_raised;
            
            last_raised = clientGetLastRaise (screen_info);
            if (last_raised && (c != last_raised))
            {
                if ((screen_info->params->prevent_focus_stealing) && !(screen_info->params->bring_on_activate))
                {
                    ev->value_mask &= ~(CWSibling | CWStackMode);
                    TRACE ("Setting WM_STATE_DEMANDS_ATTENTION flag on \"%s\" (0x%lx)", c->name, c->window); 
                    FLAG_SET (c->flags, CLIENT_FLAG_DEMANDS_ATTENTION);
                    clientSetNetState (c);
                }
                else
                {
                    clientActivate (c, getXServerTime (display_info));
                }
            }
        }

        clientConfigure (c, &wc, ev->value_mask, (constrained ? CFG_CONSTRAINED : 0) | CFG_REQUEST);
    }
    else
    {
        TRACE ("unmanaged configure request for win 0x%lx", ev->window);
        XConfigureWindow (display_info->dpy, ev->window, ev->value_mask, &wc);
    }
}

static void
handleEnterNotify (DisplayInfo *display_info, XCrossingEvent * ev)
{
    static Time lastresist = (Time) CurrentTime;
    ScreenInfo *screen_info;
    Client *c;
    gboolean warp_pointer;

    /* See http://rfc-ref.org/RFC-TEXTS/1013/chapter12.html for details */

    TRACE ("entering handleEnterNotify");

    if ((ev->mode == NotifyGrab) || (ev->mode == NotifyUngrab)
        || (ev->detail > NotifyNonlinearVirtual))
    {
        /* We're not interested in such notifications */
        return;
    }

    TRACE ("EnterNotify on window (0x%lx)", ev->window);

    warp_pointer = FALSE;
    c = myDisplayGetClientFromWindow (display_info, ev->window, FRAME);
    if (c)
    {
        screen_info = c->screen_info;

        if (!(screen_info->params->click_to_focus) && clientAcceptFocus (c))
        {
            TRACE ("EnterNotify window is \"%s\"", c->name);
            if (!(c->type & (WINDOW_DOCK | WINDOW_DESKTOP)))
            {
                if(screen_info->params->focus_delay)
                {
                    clientClearDelayedFocus ();
                    clientAddDelayedFocus (c, ev->time);
                }
                else
                {
                    clientSetFocus (c->screen_info, c, ev->time, NO_FOCUS_FLAG);
                }
            } 
            else
            {
                clientClearDelayedFocus ();
            }
        }

        /* No need to process the event any further */
        return;
    }

    /* The event was not for a client window */

    if (display_info->nb_screens > 1)
    {
        /* Wrap workspace/wrap windows is disabled with multiscreen */
        return;
    }

    /* Get the screen structure from the root of the event */
    screen_info = myDisplayGetScreenFromRoot (display_info, ev->root);

    if (!screen_info)
    {
        return;
    }

    if (screen_info->workspace_count && screen_info->params->wrap_workspaces
        && screen_info->params->wrap_resistance)
    {
        int msx, msy, maxx, maxy;
        int rx, ry;

        msx = ev->x_root;
        msy = ev->y_root;
        maxx = screen_info->width - 1;
        maxy = screen_info->height - 1;
        rx = 0;
        ry = 0;
        warp_pointer = FALSE;

        if ((msx == 0) || (msx == maxx))
        {
            if ((ev->time - lastresist) > 250)  /* ms */
            {
                edge_scroll_x = 0;
            }
            else
            {
                edge_scroll_x++;
            }
            if (msx == 0)
            {
                rx = 1;
            }
            else
            {
                rx = -1;
            }
            warp_pointer = TRUE;
            lastresist = ev->time;
        }
        if ((msy == 0) || (msy == maxy))
        {
            if ((ev->time - lastresist) > 250)  /* ms */
            {
                edge_scroll_y = 0;
            }
            else
            {
                edge_scroll_y++;
            }
            if (msy == 0)
            {
                ry = 1;
            }
            else
            {
                ry = -1;
            }
            warp_pointer = TRUE;
            lastresist = ev->time;
        }

        if (edge_scroll_x > screen_info->params->wrap_resistance)
        {
            edge_scroll_x = 0;
            if (msx == 0)
            {
                if (workspaceMove (screen_info, 0, -1, NULL, ev->time))
                {
                    rx = 4 * maxx / 5;
                }
            }
            else
            {
                if (workspaceMove (screen_info, 0, 1, NULL, ev->time))
                {
                    rx = -4 * maxx / 5;
                }
            }
            warp_pointer = TRUE;
        }
        if (edge_scroll_y > screen_info->params->wrap_resistance)
        {
            edge_scroll_y = 0;
            if (msy == 0)
            {
                if (workspaceMove (screen_info, -1, 0, NULL, ev->time))
                {
                    ry = 4 * maxy / 5;
                }
            }
            else
            {
                if (workspaceMove (screen_info, 1, 0, NULL, ev->time))
                {
                    ry = -4 * maxy / 5;
                }
            }
            warp_pointer = TRUE;
        }
        if (warp_pointer)
        {
            XWarpPointer (display_info->dpy, None, None, 0, 0, 0, 0, rx, ry);
            XFlush (display_info->dpy);
        }
    }
}

static void
handleLeaveNotify (DisplayInfo *display_info, XCrossingEvent * ev)
{
    TRACE ("entering handleLeaveNotify");
}

static void
handleFocusIn (DisplayInfo *display_info, XFocusChangeEvent * ev)
{
    ScreenInfo *screen_info;
    Client *c, *user_focus, *current_focus;

    /* See http://rfc-ref.org/RFC-TEXTS/1013/chapter12.html for details */

    TRACE ("entering handleFocusIn");
    TRACE ("handleFocusIn (0x%lx) mode = %s",
                ev->window,
                (ev->mode == NotifyNormal) ?
                "NotifyNormal" :
                (ev->mode == NotifyWhileGrabbed) ?
                "NotifyWhileGrabbed" :
                (ev->mode == NotifyGrab) ?
                "NotifyGrab" :
                (ev->mode == NotifyUngrab) ?
                "NotifyUngrab" :
                "(unknown)");
    TRACE ("handleFocusIn (0x%lx) detail = %s",
                ev->window,
                (ev->detail == NotifyAncestor) ?
                "NotifyAncestor" :
                (ev->detail == NotifyVirtual) ?
                "NotifyVirtual" :
                (ev->detail == NotifyInferior) ?
                "NotifyInferior" :
                (ev->detail == NotifyNonlinear) ?
                "NotifyNonlinear" :
                (ev->detail == NotifyNonlinearVirtual) ?
                "NotifyNonlinearVirtual" :
                (ev->detail == NotifyPointer) ?
                "NotifyPointer" :
                (ev->detail == NotifyPointerRoot) ?
                "NotifyPointerRoot" :
                (ev->detail == NotifyDetailNone) ?
                "NotifyDetailNone" :
                "(unknown)");

    screen_info = myDisplayGetScreenFromWindow (display_info, ev->window);
    if (!screen_info)
    {
        /* Not for us */
        return;
    }

    if ((ev->window == screen_info->xroot)
        && ((ev->detail == NotifyDetailNone) 
            || ((ev->mode == NotifyNormal) && (ev->detail == NotifyInferior))))
    {
        /* 
           Handle unexpected focus transition to root (means that an unknown
           window has vanished and the focus is returned to the root).
         */
        c = clientGetFocusOrPending ();
        clientSetFocus (screen_info, c, getXServerTime (display_info), FOCUS_FORCE);
        return;
    }
    if ((ev->mode == NotifyGrab) || (ev->mode == NotifyUngrab))
    {
        /* We're not interested in such notifications */
        return;
    }
    c = myDisplayGetClientFromWindow (display_info, ev->window, ANY);
    user_focus = clientGetUserFocus ();
    current_focus = clientGetFocus ();

    TRACE ("FocusIn on window (0x%lx)", ev->window);
    if ((c) && (c != current_focus))
    {
        TRACE ("Focus transfered to \"%s\" (0x%lx)", c->name, c->window);

        screen_info = c->screen_info;

        clientUpdateFocus (screen_info, c, FOCUS_SORT);
        if ((user_focus != c) && (user_focus != NULL))
        {
            /* 
               Focus stealing prevention:
               Some apps tend to focus the window directly. If focus stealing prevention is enabled, 
               we revert the user set focus to the window that we think has focus and then set the 
               demand attention flag.

               Note that focus stealing prevention is ignored between windows of the same group or
               between windows that have a transient relationship, as some apps tend to play with
               focus with their "own" windows.
             */

            if (screen_info->params->prevent_focus_stealing &&
                !clientSameGroup (c, user_focus) &&
                !clientIsTransientOrModalFor (c, user_focus))
            {
                TRACE ("Setting focus back to \"%s\" (0x%lx)", user_focus->name, user_focus->window); 
                clientSetFocus (user_focus->screen_info, user_focus, getXServerTime (display_info), NO_FOCUS_FLAG);

                if (current_focus)
                {
                    TRACE ("Setting WM_STATE_DEMANDS_ATTENTION flag on \"%s\" (0x%lx)", c->name, c->window); 
                    FLAG_SET (c->flags, CLIENT_FLAG_DEMANDS_ATTENTION);
                    clientSetNetState (c);
                }
            }
        }

        if (screen_info->params->raise_on_focus)
        {
            clientResetDelayedRaise (screen_info);
        }
    }
}

static void
handleFocusOut (DisplayInfo *display_info, XFocusChangeEvent * ev)
{
    Client *c;

    /* See http://rfc-ref.org/RFC-TEXTS/1013/chapter12.html for details */

    TRACE ("entering handleFocusOut");
    TRACE ("handleFocusOut (0x%lx) mode = %s",
                ev->window,
                (ev->mode == NotifyNormal) ?
                "NotifyNormal" :
                (ev->mode == NotifyWhileGrabbed) ?
                "NotifyWhileGrabbed" :
                "(unknown)");
    TRACE ("handleFocusOut (0x%lx) detail = %s",
                ev->window,
                (ev->detail == NotifyAncestor) ?
                "NotifyAncestor" :
                (ev->detail == NotifyVirtual) ?
                "NotifyVirtual" :
                (ev->detail == NotifyInferior) ?
                "NotifyInferior" :
                (ev->detail == NotifyNonlinear) ?
                "NotifyNonlinear" :
                (ev->detail == NotifyNonlinearVirtual) ?
                "NotifyNonlinearVirtual" :
                (ev->detail == NotifyPointer) ?
                "NotifyPointer" :
                (ev->detail == NotifyPointerRoot) ?
                "NotifyPointerRoot" :
                (ev->detail == NotifyDetailNone) ?
                "NotifyDetailNone" :
                "(unknown)");

    if ((ev->mode == NotifyGrab) || (ev->mode == NotifyUngrab) || 
        (ev->detail == NotifyInferior) || (ev->detail > NotifyNonlinearVirtual))
    {
        /* We're not interested in such notifications */
        return;
    }

    if ((ev->mode == NotifyNormal)
        && ((ev->detail == NotifyNonlinear)
            || (ev->detail == NotifyNonlinearVirtual)))
    {
        c = myDisplayGetClientFromWindow (display_info, ev->window, ANY);
        TRACE ("FocusOut on window (0x%lx)", ev->window);
        if ((c) && (c == clientGetFocus ()))
        {
            TRACE ("focus lost from \"%s\" (0x%lx)", c->name, c->window);
            clientPassGrabMouseButton (NULL);
            clientUpdateFocus (c->screen_info, NULL, NO_FOCUS_FLAG);
            clientClearDelayedRaise ();
        }
    }
}

static void
handlePropertyNotify (DisplayInfo *display_info, XPropertyEvent * ev)
{
    ScreenInfo *screen_info;
    Client *c;

    TRACE ("entering handlePropertyNotify");

    c = myDisplayGetClientFromWindow (display_info, ev->window, WINDOW);
    if (c)
    {
        screen_info = c->screen_info;
        if (ev->atom == XA_WM_NORMAL_HINTS)
        {
            TRACE ("client \"%s\" (0x%lx) has received a XA_WM_NORMAL_HINTS notify", c->name, c->window);
            clientGetWMNormalHints (c, TRUE);
        }
        else if ((ev->atom == XA_WM_NAME) ||
                 (ev->atom == display_info->atoms[NET_WM_NAME]) ||
                 (ev->atom == display_info->atoms[WM_CLIENT_MACHINE]))
        {
            TRACE ("client \"%s\" (0x%lx) has received a XA_WM_NAME/NET_WM_NAME/WM_CLIENT_MACHINE notify", c->name, c->window);
            clientUpdateName (c);
        }
        else if (ev->atom == display_info->atoms[MOTIF_WM_HINTS])
        {
            TRACE ("client \"%s\" (0x%lx) has received a MOTIF_WM_HINTS notify", c->name, c->window);
            clientGetMWMHints (c, TRUE);
        }
        else if (ev->atom == XA_WM_HINTS)
        {
            TRACE ("client \"%s\" (0x%lx) has received a XA_WM_HINTS notify", c->name, c->window);
            c->wmhints = XGetWMHints (display_info->dpy, c->window);
            if (c->wmhints)
            {
                if (c->wmhints->flags & WindowGroupHint)
                {
                    c->group_leader = c->wmhints->window_group;
                }
                if ((c->wmhints->flags & IconPixmapHint) && (screen_info->params->show_app_icon))
                {
                    clientUpdateIcon (c);
                }
                if (HINTS_ACCEPT_INPUT (c->wmhints))
                {
                    FLAG_SET (c->wm_flags, WM_FLAG_INPUT);
                }
                else
                {
                    FLAG_UNSET (c->wm_flags, WM_FLAG_INPUT);
                }
            }
            clientUpdateUrgency (c);
        }
        else if (ev->atom == display_info->atoms[WM_PROTOCOLS])
        {
            TRACE ("client \"%s\" (0x%lx) has received a WM_PROTOCOLS notify", c->name, c->window);
            clientGetWMProtocols (c);
        }
        else if (ev->atom == display_info->atoms[WM_TRANSIENT_FOR])
        {
            Window w;

            TRACE ("client \"%s\" (0x%lx) has received a WM_TRANSIENT_FOR notify", c->name, c->window);
            getTransientFor (display_info, c->screen_info->xroot, c->window, &w);
            if (clientCheckTransientWindow (c, w))
            {
                c->transient_for = w;
#if 0                
                /*
                  Java 1.6 updates the WM_TRANSIENT_FOR properties "on-the-fly"
                  of its windows to maintain the z-order. 
                  
                  If we raise the transient then, we clearly have a race 
                  condition between the WM and Java... And that breaks 
                  the z-order. Bug #2483.
                  
                  I still think that raising here makes sense, to ensure
                  that the newly promoted transient window is placed above
                  its parent.
                  
                  Chances are that Java 1.6 won't change any time soon (heh,
                  it's not even released yet), so let's adjust the WM to
                  work with Java 1.6...
                 */
                clientRaise (c, w);
#endif
            }
        }
        else if (ev->atom == display_info->atoms[WIN_HINTS])
        {
            TRACE ("client \"%s\" (0x%lx) has received a WIN_HINTS notify", c->name, c->window);
            getHint (display_info, c->window, WIN_HINTS, (long *) &c->win_hints);
        }
        else if (ev->atom == display_info->atoms[NET_WM_WINDOW_TYPE])
        {
            TRACE ("client \"%s\" (0x%lx) has received a NET_WM_WINDOW_TYPE notify", c->name, c->window);
            clientGetNetWmType (c);
            frameQueueDraw (c);
        }
        else if ((ev->atom == display_info->atoms[NET_WM_STRUT]) ||
                 (ev->atom == display_info->atoms[NET_WM_STRUT_PARTIAL]))
        {
            TRACE ("client \"%s\" (0x%lx) has received a NET_WM_STRUT notify", c->name, c->window);
            if (clientGetNetStruts (c) && FLAG_TEST (c->xfwm_flags, XFWM_FLAG_VISIBLE))
            {
                workspaceUpdateArea (c->screen_info);
            }
        }
        else if (ev->atom == display_info->atoms[WM_COLORMAP_WINDOWS])
        {
            TRACE ("client \"%s\" (0x%lx) has received a WM_COLORMAP_WINDOWS notify", c->name, c->window);
            clientUpdateColormaps (c);
            if (c == clientGetFocus ())
            {
                clientInstallColormaps (c);
            }
        }
        else if (ev->atom == display_info->atoms[NET_WM_USER_TIME])
        {
            TRACE ("client \"%s\" (0x%lx) has received a NET_WM_USER_TIME notify", c->name, c->window);
            if (getNetWMUserTime (display_info, c->window, &c->user_time))
            {
                myDisplaySetLastUserTime (display_info, c->user_time);
                FLAG_SET (c->flags, CLIENT_FLAG_HAS_USER_TIME);
            }
        }
        else if (ev->atom == display_info->atoms[NET_WM_WINDOW_OPACITY])
        {
            TRACE ("client \"%s\" (0x%lx) has received a NET_WM_OPACITY notify", c->name, c->window);
            if (!getOpacity (display_info, c->window, &c->opacity))
            {
                c->opacity =  NET_WM_OPAQUE;
            }
            compositorWindowSetOpacity (display_info, c->frame, c->opacity);
        }
        else if (ev->atom == display_info->atoms[NET_WM_WINDOW_OPACITY_LOCKED])
        {
            TRACE ("client \"%s\" (0x%lx) has received a NET_WM_OPACITY_LOCKED notify", c->name, c->window);
            c->opacity_locked = getOpacityLock (display_info, c->window);
        }
        else if ((screen_info->params->show_app_icon) &&
                 ((ev->atom == display_info->atoms[NET_WM_ICON]) ||
                  (ev->atom == display_info->atoms[KWM_WIN_ICON])))
        {
            clientUpdateIcon (c);
        }
#ifdef HAVE_STARTUP_NOTIFICATION
        else if (ev->atom == display_info->atoms[NET_STARTUP_ID])
        {
            if (c->startup_id)
            {
                g_free (c->startup_id);
                c->startup_id = NULL;
            }
            getWindowStartupId (display_info, c->window, &c->startup_id);
        }
#endif /* HAVE_STARTUP_NOTIFICATION */
#ifdef HAVE_XSYNC
        else if (ev->atom == display_info->atoms[NET_WM_SYNC_REQUEST_COUNTER])
        {
            getXSyncCounter (display_info, c->window, &c->xsync_counter);
            TRACE ("Window 0x%lx has NET_WM_SYNC_REQUEST_COUNTER set to 0x%lx", c->window, c->xsync_counter);
        }
#endif /* HAVE_XSYNC */

        return;
    }

    screen_info = myDisplayGetScreenFromWindow (display_info, ev->window);
    if (!screen_info)
    {
        return;
    }

    if (ev->atom == display_info->atoms[NET_DESKTOP_NAMES])
    {
        gchar **names;
        int items;

        TRACE ("root has received a NET_DESKTOP_NAMES notify");
        if (getUTF8StringList (display_info, screen_info->xroot, NET_DESKTOP_NAMES, &names, &items))
        {
            workspaceSetNames (screen_info, names, items);
        }
    }
    else if (ev->atom == display_info->atoms[GNOME_PANEL_DESKTOP_AREA])
    {
        TRACE ("root has received a GNOME_PANEL_DESKTOP_AREA notify");
        getGnomeDesktopMargins (display_info, screen_info->xroot, screen_info->gnome_margins);
        workspaceUpdateArea (screen_info);
    }
    else if (ev->atom == display_info->atoms[NET_DESKTOP_LAYOUT])
    {
        TRACE ("root has received a NET_DESKTOP_LAYOUT notify");
        getDesktopLayout(display_info, screen_info->xroot, screen_info->workspace_count, &screen_info->desktop_layout);
        placeSidewalks(screen_info, screen_info->params->wrap_workspaces);
    }
}

static void
handleClientMessage (DisplayInfo *display_info, XClientMessageEvent * ev)
{
    ScreenInfo *screen_info;
    Client *c;
    gboolean is_transient;

    TRACE ("entering handleClientMessage");

    c = myDisplayGetClientFromWindow (display_info, ev->window, WINDOW);
    if (c)
    {
        screen_info = c->screen_info;
        is_transient = clientIsValidTransientOrModal (c);

        if ((ev->message_type == display_info->atoms[WM_CHANGE_STATE]) && (ev->format == 32) && (ev->data.l[0] == IconicState))
        {
            TRACE ("client \"%s\" (0x%lx) has received a WM_CHANGE_STATE event", c->name, c->window);
            if (!FLAG_TEST (c->flags, CLIENT_FLAG_ICONIFIED) && CLIENT_CAN_HIDE_WINDOW (c))
            {
                clientHide (c, c->win_workspace, TRUE);
            }
        }
        else if ((ev->message_type == display_info->atoms[WIN_STATE]) && (ev->format == 32))
        {
            TRACE ("client \"%s\" (0x%lx) has received a WIN_STATE event", c->name, c->window);
            clientUpdateWinState (c, ev);
        }
        else if ((ev->message_type == display_info->atoms[WIN_LAYER]) && (ev->format == 32))
        {
            TRACE ("client \"%s\" (0x%lx) has received a WIN_LAYER event", c->name, c->window);
            if ((ev->data.l[0] != c->win_layer) && !is_transient)
            {
                clientSetLayer (c, ev->data.l[0]);
            }
        }
        else if ((ev->message_type == display_info->atoms[WIN_WORKSPACE]) && (ev->format == 32))
        {
            TRACE ("client \"%s\" (0x%lx) has received a WIN_WORKSPACE event", c->name, c->window);
            if ((ev->data.l[0] != c->win_workspace) && !is_transient)
            {
                clientSetWorkspace (c, ev->data.l[0], TRUE);
            }
        }
        else if ((ev->message_type == display_info->atoms[NET_WM_DESKTOP]) && (ev->format == 32))
        {
            TRACE ("client \"%s\" (0x%lx) has received a NET_WM_DESKTOP event", c->name, c->window);
            if (!is_transient)
            {
                if (ev->data.l[0] == ALL_WORKSPACES)
                {
                    if (FLAG_TEST (c->xfwm_flags, XFWM_FLAG_HAS_STICK) && !FLAG_TEST (c->flags, CLIENT_FLAG_STICKY))
                    {
                        clientStick (c, TRUE);
                        frameDraw (c, FALSE);
                    }
                }
                else
                {
                    if (FLAG_TEST (c->xfwm_flags, XFWM_FLAG_HAS_STICK) && FLAG_TEST (c->flags, CLIENT_FLAG_STICKY))
                    {
                        clientUnstick (c, TRUE);
                        frameDraw (c, FALSE);
                    }
                    if (ev->data.l[0] != c->win_workspace)
                    {
                        clientSetWorkspace (c, ev->data.l[0], TRUE);
                    }
                }
            }
        }
        else if ((ev->message_type == display_info->atoms[NET_CLOSE_WINDOW]) && (ev->format == 32))
        {
            TRACE ("client \"%s\" (0x%lx) has received a NET_CLOSE_WINDOW event", c->name, c->window);
            clientClose (c);
        }
        else if ((ev->message_type == display_info->atoms[NET_WM_STATE]) && (ev->format == 32))
        {
            TRACE ("client \"%s\" (0x%lx) has received a NET_WM_STATE event", c->name, c->window);
            clientUpdateNetState (c, ev);
        }
        else if ((ev->message_type == display_info->atoms[NET_WM_MOVERESIZE]) && (ev->format == 32))
        {
            TRACE ("client \"%s\" (0x%lx) has received a NET_WM_MOVERESIZE event", c->name, c->window);
            clientNetMoveResize (c, ev);
        }
        else if ((ev->message_type == display_info->atoms[NET_ACTIVE_WINDOW]) && (ev->format == 32))
        {
            gboolean source_is_application;
            Time ev_time;

            ev_time = myDisplayGetTime (display_info, (Time) ev->data.l[1]);
            source_is_application = (ev->data.l[0] == 1);

            TRACE ("client \"%s\" (0x%lx) has received a NET_ACTIVE_WINDOW event", c->name, c->window);
            if (source_is_application)
            {
                Time current = myDisplayGetLastUserTime (display_info);

                TRACE ("Time of event received is %u, current XServer time is %u", (unsigned int) ev_time, (unsigned int) current);
                if ((screen_info->params->prevent_focus_stealing) && TIMESTAMP_IS_BEFORE(ev_time, current))
                {
                    TRACE ("Setting WM_STATE_DEMANDS_ATTENTION flag on \"%s\" (0x%lx)", c->name, c->window); 
                    FLAG_SET (c->flags, CLIENT_FLAG_DEMANDS_ATTENTION);
                    clientSetNetState (c);
                }
                else
                {
                    clientActivate (c, ev_time);
                }
            }
            else
            {
                /* The request is either from a pager or an older client, use the most accurate timestamp */
                clientActivate (c, getXServerTime (display_info));
            }
        }
        else if (ev->message_type == display_info->atoms[NET_REQUEST_FRAME_EXTENTS])
        {
            TRACE ("client \"%s\" (0x%lx) has received a NET_REQUEST_FRAME_EXTENTS event", c->name, c->window);
            setNetFrameExtents (display_info, c->window, frameTop (c), frameLeft (c),
                                                         frameRight (c), frameBottom (c));
        }
    }
    else
    {
        screen_info = myDisplayGetScreenFromWindow (display_info, ev->window);
        if (!screen_info)
        {
            return;
        }

        if (((ev->message_type == display_info->atoms[WIN_WORKSPACE]) ||
             (ev->message_type == display_info->atoms[NET_CURRENT_DESKTOP])) && (ev->format == 32))
        {
            TRACE ("root has received a win_workspace or a NET_CURRENT_DESKTOP event %li", ev->data.l[0]);
            if ((ev->data.l[0] >= 0) && (ev->data.l[0] < screen_info->workspace_count) && 
                (ev->data.l[0] != screen_info->current_ws))
            {
                workspaceSwitch (screen_info, ev->data.l[0], NULL, TRUE, 
                                 myDisplayGetTime (display_info, (Time) ev->data.l[1]));
            }
        }
        else if (((ev->message_type == display_info->atoms[WIN_WORKSPACE_COUNT]) ||
                  (ev->message_type == display_info->atoms[NET_NUMBER_OF_DESKTOPS])) && (ev->format == 32))
        {
            TRACE ("root has received a win_workspace_count event");
            if (ev->data.l[0] != screen_info->workspace_count)
            {
                workspaceSetCount (screen_info, ev->data.l[0]);
                getDesktopLayout(display_info, screen_info->xroot, screen_info->workspace_count, &screen_info->desktop_layout);
            }
        }
        else if ((ev->message_type == display_info->atoms[NET_SHOWING_DESKTOP]) && (ev->format == 32))
        {
            TRACE ("root has received a NET_SHOWING_DESKTOP event");
            screen_info->show_desktop = (ev->data.l[0] != 0);
            clientToggleShowDesktop (screen_info);
            setHint (display_info, screen_info->xroot, NET_SHOWING_DESKTOP, ev->data.l[0]);
        }
        else if (ev->message_type == display_info->atoms[NET_REQUEST_FRAME_EXTENTS])
        {
            TRACE ("window (0x%lx) has received a NET_REQUEST_FRAME_EXTENTS event", ev->window);
            /* Size estimate from the decoration extents */
            setNetFrameExtents (display_info, ev->window,
                                frameDecorationTop (screen_info),
                                frameDecorationLeft (screen_info),
                                frameDecorationRight (screen_info),
                                frameDecorationBottom (screen_info));
        }
        else if ((ev->message_type == display_info->atoms[MANAGER]) && (ev->format == 32))
        {
            Atom selection;
            
            TRACE ("window (0x%lx) has received a MANAGER event", ev->window);
            selection = (Atom) ev->data.l[1];
            
#ifdef ENABLE_KDE_SYSTRAY_PROXY
            if (selection == screen_info->net_system_tray_selection)
            {
                TRACE ("root has received a NET_SYSTEM_TRAY_MANAGER selection event");
                screen_info->systray = getSystrayWindow (display_info, screen_info->net_system_tray_selection);
            }
            else
#endif
            if (myScreenCheckWMAtom (screen_info, selection))
            {
                TRACE ("root has received a WM_Sn selection event");
                display_info->quit = TRUE;
            }
        }
        else
        {
            TRACE ("unidentified client message for window 0x%lx", ev->window);
        }
    }
}

static void
handleShape (DisplayInfo *display_info, XShapeEvent * ev)
{
    Client *c;
    gboolean update;

    TRACE ("entering handleShape");

    c = myDisplayGetClientFromWindow (display_info, ev->window, WINDOW);
    if (c)
    {
        update = FALSE;
        if (ev->kind == ShapeBounding)
        {
            if ((ev->shaped) && !FLAG_TEST (c->flags, CLIENT_FLAG_HAS_SHAPE))
            {
                FLAG_SET (c->flags, CLIENT_FLAG_HAS_SHAPE);
                clientGetMWMHints (c, TRUE);
                update = TRUE;
            }
            else if (!(ev->shaped) && FLAG_TEST (c->flags, CLIENT_FLAG_HAS_SHAPE))
            {
                FLAG_UNSET (c->flags, CLIENT_FLAG_HAS_SHAPE);
                clientGetMWMHints (c, TRUE);
                update = TRUE;
            }
        }
        if (!update)
        {
            frameDraw (c, FALSE);
        }
    }
}

static void
handleColormapNotify (DisplayInfo *display_info, XColormapEvent * ev)
{
    Client *c;

    TRACE ("entering handleColormapNotify");

    c = myDisplayGetClientFromWindow (display_info, ev->window, WINDOW);
    if ((c) && (ev->window == c->window) && (ev->new))
    {
        if (c == clientGetFocus ())
        {
            clientInstallColormaps (c);
        }
    }
}

static void
handleMappingNotify (DisplayInfo *display_info, XMappingEvent * ev)
{
    TRACE ("entering handleMappingNotify");

    /* Refreshes the stored modifier and keymap information */
    XRefreshKeyboardMapping (ev);

    /* Update internal modifiers masks if necessary */
    if (ev->request == MappingModifier)
    {
        TRACE ("handleMappingNotify: modifiers mapping has changed");
        initModifiers (display_info->dpy);
    }

    /* Regrab all keys if the notify is for keyboard (ie not pointer) */
    if (ev->request != MappingPointer)
    {
        TRACE ("handleMappingNotify: Reload settings");
        reloadSettings (display_info, UPDATE_BUTTON_GRABS);
    }
}

#ifdef HAVE_XSYNC
static void
handleXSyncAlarmNotify (DisplayInfo *display_info, XSyncAlarmNotifyEvent * ev)
{
    XWindowChanges wc;
    Client *c;

    TRACE ("entering handleXSyncAlarmNotify");

    if (!display_info->have_xsync)
    {
        return;
    }

    c = myDisplayGetClientFromXSyncAlarm (display_info, ev->alarm);
    if (c)
    {
        c->xsync_waiting = FALSE;
        c->xsync_value = ev->counter_value;
        if (c->xsync_enabled)
        {
            wc.x = c->x;
            wc.y = c->y;
            wc.width = c->width;
            wc.height = c->height;
            clientConfigure (c, &wc, CWX | CWY | CWWidth | CWHeight, NO_CFG_FLAG);
        }
    }
}
#endif /* HAVE_XSYNC */

static void
handleEvent (DisplayInfo *display_info, XEvent * ev)
{
    TRACE ("entering handleEvent");

    /* Update the display time */
    myDisplayUpdateCurrentTime (display_info, ev);

    sn_process_event (ev);
    compositorHandleEvent (display_info, ev);
    switch (ev->type)
    {
        case MotionNotify:
            handleMotionNotify (display_info, (XMotionEvent *) ev);
            break;
        case KeyPress:
            handleKeyPress (display_info, (XKeyEvent *) ev);
            break;
        case ButtonPress:
            handleButtonPress (display_info, (XButtonEvent *) ev);
            break;
        case ButtonRelease:
            handleButtonRelease (display_info, (XButtonEvent *) ev);
            break;
        case DestroyNotify:
            handleDestroyNotify (display_info, (XDestroyWindowEvent *) ev);
            break;
        case UnmapNotify:
            handleUnmapNotify (display_info, (XUnmapEvent *) ev);
            break;
        case MapRequest:
            handleMapRequest (display_info, (XMapRequestEvent *) ev);
            break;
        case MapNotify:
            handleMapNotify (display_info, (XMapEvent *) ev);
            break;
        case ConfigureNotify:
            handleConfigureNotify (display_info, (XConfigureEvent *) ev);
            break;
        case ConfigureRequest:
            handleConfigureRequest (display_info, (XConfigureRequestEvent *) ev);
            break;
        case EnterNotify:
            handleEnterNotify (display_info, (XCrossingEvent *) ev);
            break;
        case LeaveNotify:
            handleLeaveNotify (display_info, (XCrossingEvent *) ev);
            break;
        case FocusIn:
            handleFocusIn (display_info, (XFocusChangeEvent *) ev);
            break;
        case FocusOut:
            handleFocusOut (display_info, (XFocusChangeEvent *) ev);
            break;
        case PropertyNotify:
            handlePropertyNotify (display_info, (XPropertyEvent *) ev);
            break;
        case ClientMessage:
            handleClientMessage (display_info, (XClientMessageEvent *) ev);
            break;
        case ColormapNotify:
            handleColormapNotify (display_info, (XColormapEvent *) ev);
            break;
        case MappingNotify:
            handleMappingNotify (display_info, (XMappingEvent *) ev);
            break;
        default:
            if ((display_info->have_shape) && (ev->type == display_info->shape_event_base))
            {
                handleShape (display_info, (XShapeEvent *) ev);
            }
#ifdef HAVE_XSYNC
            if ((display_info->have_xsync) && (ev->type == (display_info->xsync_event_base + XSyncAlarmNotify)))
            {
                handleXSyncAlarmNotify (display_info, (XSyncAlarmNotifyEvent *) ev);
            }
#endif /* HAVE_XSYNC */
    }
    if (!gdk_events_pending () && !XPending (display_info->dpy))
    {
        if (display_info->reload)
        {
            reloadSettings (display_info, UPDATE_ALL);
            display_info->reload = FALSE;
        }
        else if (display_info->quit)
        {
            gtk_main_quit ();
        }
    }
}

eventFilterStatus
xfwm4_event_filter (XEvent * xevent, gpointer data)
{
    DisplayInfo *display_info;

    display_info = (DisplayInfo *) data;

    TRACE ("entering xfwm4_event_filter");
    handleEvent (display_info, xevent);
    TRACE ("leaving xfwm4_event_filter");
    return EVENT_FILTER_STOP;
}

/* GTK specific stuff */

static void
menu_callback (Menu * menu, MenuOp op, Window xid, gpointer menu_data, gpointer item_data)
{
    Client *c;

    TRACE ("entering menu_callback");

    if (!xfwmWindowDeleted(&menu_event_window))
    {
        xfwmWindowDelete (&menu_event_window);
    }

    c = NULL;
    if ((menu_data != NULL) && (xid != None))
    {
        ScreenInfo *screen_info = (ScreenInfo *) menu_data;
        c = clientGetFromWindow (screen_info, xid, WINDOW);
    }

    if (c)
    {
        c->button_pressed[MENU_BUTTON] = FALSE;

        switch (op)
        {
            case MENU_OP_QUIT:
                gtk_main_quit ();
                break;
            case MENU_OP_MAXIMIZE:
            case MENU_OP_UNMAXIMIZE:
                if (CLIENT_CAN_MAXIMIZE_WINDOW (c))
                {
                    clientToggleMaximized (c, WIN_STATE_MAXIMIZED, TRUE);
                }
                break;
            case MENU_OP_MINIMIZE:
                if (CLIENT_CAN_HIDE_WINDOW (c))
                {
                    clientHide (c, c->win_workspace, TRUE);
                }
                frameDraw (c, FALSE);
                break;
            case MENU_OP_MINIMIZE_ALL:
                clientHideAll (c, c->win_workspace);
                frameDraw (c, FALSE);
                break;
            case MENU_OP_UNMINIMIZE:
                clientShow (c, TRUE);
                clientClearAllShowDesktop (c->screen_info);
                break;
            case MENU_OP_SHADE:
            case MENU_OP_UNSHADE:
                clientToggleShaded (c);
                break;
            case MENU_OP_STICK:
            case MENU_OP_UNSTICK:
                clientToggleSticky (c, TRUE);
                frameDraw (c, FALSE);
                break;
            case MENU_OP_WORKSPACES:
                clientSetWorkspace (c, GPOINTER_TO_INT (item_data), TRUE);
                frameDraw (c, FALSE);
                break;
            case MENU_OP_DELETE:
                frameDraw (c, FALSE);
                clientClose (c);
                break;
            case MENU_OP_CONTEXT_HELP:
                clientEnterContextMenuState (c);
                frameDraw (c, FALSE);
                break;
            case MENU_OP_ABOVE:
            case MENU_OP_NORMAL:
                clientToggleAbove (c);
                /* Fall thru */
            default:
                frameDraw (c, FALSE);
                break;
        }
    }
    else
    {
        gdk_beep ();
    }
    menu_free (menu);
}

void
initMenuEventWin (void)
{
    xfwmWindowInit (&menu_event_window);
}

static void
show_window_menu (Client *c, gint px, gint py, guint button, guint32 time)
{
    ScreenInfo *screen_info;
    DisplayInfo *display_info;
    Menu *menu;
    MenuOp ops;
    MenuOp insensitive;
    gint x, y;

    TRACE ("entering show_window_menu");

    if (!c || ((button != Button1) && (button != Button3)))
    {
        return;
    }

    x = px;
    y = py;

    c->button_pressed[MENU_BUTTON] = TRUE;
    frameDraw (c, FALSE);
    y = (gdouble) c->y;
    ops = MENU_OP_DELETE | MENU_OP_MINIMIZE_ALL | MENU_OP_WORKSPACES;
    insensitive = 0;

    if (!FLAG_TEST (c->xfwm_flags, XFWM_FLAG_HAS_CLOSE))
    {
        insensitive |= MENU_OP_DELETE;
    }

    if (FLAG_TEST (c->flags, CLIENT_FLAG_MAXIMIZED))
    {
        ops |= MENU_OP_UNMAXIMIZE;
        if (!CLIENT_CAN_MAXIMIZE_WINDOW (c))
        {
            insensitive |= MENU_OP_UNMAXIMIZE;
        }
    }
    else
    {
        ops |= MENU_OP_MAXIMIZE;
        if (!CLIENT_CAN_MAXIMIZE_WINDOW (c))
        {
            insensitive |= MENU_OP_MAXIMIZE;
        }
    }

    if (FLAG_TEST (c->flags, CLIENT_FLAG_ICONIFIED))
    {
        ops |= MENU_OP_UNMINIMIZE;
        if (!CLIENT_CAN_HIDE_WINDOW (c))
        {
            insensitive |= MENU_OP_UNMINIMIZE;
        }
    }
    else
    {
        ops |= MENU_OP_MINIMIZE;
        if (!CLIENT_CAN_HIDE_WINDOW (c))
        {
            insensitive |= MENU_OP_MINIMIZE;
        }
    }

    if (FLAG_TEST (c->flags, CLIENT_FLAG_SHADED))
    {
        ops |= MENU_OP_UNSHADE;
    }
    else
    {
        ops |= MENU_OP_SHADE;
    }

    if (FLAG_TEST (c->flags, CLIENT_FLAG_STICKY))
    {
        ops |= MENU_OP_UNSTICK;
        if (!CLIENT_CAN_STICK_WINDOW(c))
        {
            insensitive |= MENU_OP_UNSTICK;
        }
    }
    else
    {
        ops |= MENU_OP_STICK;
        if (!CLIENT_CAN_STICK_WINDOW(c))
        {
            insensitive |= MENU_OP_STICK;
        }
    }

    /* KDE extension */
    clientGetWMProtocols(c);
    if (FLAG_TEST (c->wm_flags, WM_FLAG_CONTEXT_HELP))
    {
        ops |= MENU_OP_CONTEXT_HELP;
    }

    if (FLAG_TEST(c->flags, CLIENT_FLAG_ABOVE))
    {
        ops |= MENU_OP_NORMAL;
        if (clientIsValidTransientOrModal (c) ||
            FLAG_TEST (c->flags, CLIENT_FLAG_BELOW | CLIENT_FLAG_FULLSCREEN))
        {
            insensitive |= MENU_OP_NORMAL;
        }
    }
    else
    {
        ops |= MENU_OP_ABOVE;
        if (clientIsValidTransientOrModal (c) ||
            FLAG_TEST (c->flags, CLIENT_FLAG_BELOW | CLIENT_FLAG_FULLSCREEN))
        {
            insensitive |= MENU_OP_ABOVE;
        }
    }

    if (clientIsValidTransientOrModal (c)
        || !FLAG_TEST (c->xfwm_flags, XFWM_FLAG_HAS_STICK)
        || FLAG_TEST (c->flags, CLIENT_FLAG_STICKY))
    {
        insensitive |= MENU_OP_WORKSPACES;
    }

    /* c is not null here */
    screen_info = c->screen_info;
    display_info = screen_info->display_info;

    if (screen_info->button_handler_id)
    {
        g_signal_handler_disconnect (GTK_OBJECT (myScreenGetGtkWidget (screen_info)), screen_info->button_handler_id);
    }
    screen_info->button_handler_id = g_signal_connect (GTK_OBJECT (myScreenGetGtkWidget (screen_info)),
                                              "button_press_event", GTK_SIGNAL_FUNC (show_popup_cb), (gpointer) NULL);

    if (!xfwmWindowDeleted(&menu_event_window))
    {
        xfwmWindowDelete (&menu_event_window);
    }
    /*
       Since all button press/release events are catched by the windows frames, there is some
       side effect with GTK menu. When a menu is opened, any click on the window frame is not
       detected as a click outside the menu, and the menu doesn't close.
       To avoid this (painless but annoying) behaviour, we just setup a no event window that
       "hides" the events to regular windows.
       That might look tricky, but it's very efficient and save plenty of lines of complicated
       code.
       Don't forget to delete that window once the menu is closed, though, or we'll get in
       trouble.
     */
    xfwmWindowTemp (screen_info,
                    NULL, 0,
                    screen_info->xroot,
                    &menu_event_window, 0, 0,
                    screen_info->width,
                    screen_info->height,
                    NoEventMask,
                    FALSE);
    menu = menu_default (screen_info->gscr, c->window, ops, insensitive, menu_callback,
                         c->win_workspace, screen_info->workspace_count,
                         screen_info->workspace_names, screen_info->workspace_names_items,
                         display_info->xfilter, screen_info);

    if (!menu_popup (menu, x, y, button, time))
    {
        TRACE ("Cannot open menu");
        gdk_beep ();
        c->button_pressed[MENU_BUTTON] = FALSE;
        frameDraw (c, FALSE);
        xfwmWindowDelete (&menu_event_window);
        menu_free (menu);
    }
}

static gboolean
show_popup_cb (GtkWidget * widget, GdkEventButton * ev, gpointer data)
{
    TRACE ("entering show_popup_cb");

    show_window_menu ((Client *) data, (gint) ev->x_root, (gint) ev->y_root, ev->button, ev->time);

    return (TRUE);
}

static gboolean
set_reload (GObject * obj, GdkEvent * ev, gpointer data)
{
    DisplayInfo *display_info;

    TRACE ("setting reload flag so all prefs will be reread at next event loop");

    display_info = (DisplayInfo *) data;
    display_info->reload = TRUE;
    return (TRUE);
}

static gboolean
dbl_click_time_cb (GObject * obj, GdkEvent * ev, gpointer data)
{
    DisplayInfo *display_info;
    GValue tmp_val = { 0, };

    display_info = (DisplayInfo *) data;
    g_return_val_if_fail (display_info, TRUE);

    g_value_init (&tmp_val, G_TYPE_INT);
    if (gdk_setting_get ("gtk-double-click-time", &tmp_val))
    {
        display_info->dbl_click_time = abs (g_value_get_int (&tmp_val));
    }

    return (TRUE);
}

static gboolean
client_event_cb (GtkWidget * widget, GdkEventClient * ev, gpointer data)
{
    TRACE ("entering client_event_cb");

    if (!atom_rcfiles)
    {
        atom_rcfiles = gdk_atom_intern ("_GTK_READ_RCFILES", FALSE);
    }

    if (ev->message_type == atom_rcfiles)
    {
        set_reload (G_OBJECT (widget), (GdkEvent *) ev, data);
    }

    return (FALSE);
}

void
initGtkCallbacks (ScreenInfo *screen_info)
{
    GtkSettings *settings;

    screen_info->button_handler_id =
        g_signal_connect (GTK_OBJECT (myScreenGetGtkWidget (screen_info)),
                          "button_press_event", GTK_SIGNAL_FUNC (show_popup_cb), (gpointer) NULL);
    g_signal_connect (GTK_OBJECT (myScreenGetGtkWidget (screen_info)), "client_event",
                      GTK_SIGNAL_FUNC (client_event_cb), (gpointer) (screen_info->display_info));
    settings = gtk_settings_get_default ();
    if (settings)
    {
        g_signal_connect (settings, "notify::gtk-theme-name",
            G_CALLBACK (set_reload), (gpointer) (screen_info->display_info));
        g_signal_connect (settings, "notify::gtk-font-name",
            G_CALLBACK (set_reload), (gpointer) (screen_info->display_info));
        g_signal_connect (settings, "notify::gtk-double-click-time",
            G_CALLBACK (dbl_click_time_cb), (gpointer) (screen_info->display_info));
    }
}
