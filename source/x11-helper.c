/**
 * rofi
 *
 * MIT/X11 License
 * Copyright (c) 2012 Sean Pringle <sean.pringle@gmail.com>
 * Modified 2013-2016 Qball Cow <qball@gmpclient.org>
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 */
#include <config.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <glib.h>
#include <cairo.h>
#include <cairo-xcb.h>

#include <xcb/xcb.h>
#include <xcb/randr.h>
#include <xcb/xinerama.h>
#include <xcb/xcb_ewmh.h>
#include <xcb/xproto.h>
#include "xcb-internal.h"
#include "xcb.h"
#include "settings.h"
#include "helper.h"

#include <rofi.h>
/** Checks if the if x and y is inside rectangle. */
#define INTERSECT( x, y, x1, y1, w1, h1 )    ( ( ( ( x ) >= ( x1 ) ) && ( ( x ) < ( x1 + w1 ) ) ) && ( ( ( y ) >= ( y1 ) ) && ( ( y ) < ( y1 + h1 ) ) ) )
#include "x11-helper.h"
#include "xkb-internal.h"

/** Log domain for this module */
#define LOG_DOMAIN    "X11Helper"

/**
 * Structure holding xcb objects needed to function.
 */
struct _xcb_stuff xcb_int = {
    .connection = NULL,
    .screen     = NULL,
    .screen_nbr =   -1,
    .sndisplay  = NULL,
    .sncontext  = NULL,
    .monitors   = NULL
};
xcb_stuff         *xcb = &xcb_int;

/**
 * Depth of root window.
 */
xcb_depth_t      *depth  = NULL;
xcb_visualtype_t *visual = NULL;
xcb_colormap_t   map     = XCB_COLORMAP_NONE;
/**
 * Visual of the root window.
 */
static xcb_visualtype_t *root_visual = NULL;
xcb_atom_t              netatoms[NUM_NETATOMS];
const char              *netatom_names[] = { EWMH_ATOMS ( ATOM_CHAR ) };

/**
 * Holds for each supported modifier the possible modifier mask.
 * Check x11_mod_masks[MODIFIER]&mask != 0 to see if MODIFIER is activated.
 */
static unsigned int x11_mod_masks[NUM_X11MOD];

cairo_surface_t *x11_helper_get_screenshot_surface ( void )
{
    return cairo_xcb_surface_create ( xcb->connection,
                                      xcb_stuff_get_root_window ( xcb ), root_visual,
                                      xcb->screen->width_in_pixels, xcb->screen->height_in_pixels );
}

static xcb_pixmap_t get_root_pixmap ( xcb_connection_t *c,
                                      xcb_screen_t *screen,
                                      xcb_atom_t atom )
{
    xcb_get_property_cookie_t cookie;
    xcb_get_property_reply_t  *reply;
    xcb_pixmap_t              rootpixmap = XCB_NONE;

    cookie = xcb_get_property ( c,
                                0,
                                screen->root,
                                atom,
                                XCB_ATOM_PIXMAP,
                                0,
                                1 );

    reply = xcb_get_property_reply ( c, cookie, NULL );

    if ( reply ) {
        if ( xcb_get_property_value_length ( reply ) == sizeof ( xcb_pixmap_t ) ) {
            memcpy ( &rootpixmap, xcb_get_property_value ( reply ), sizeof ( xcb_pixmap_t ) );
        }
        free ( reply );
    }

    return rootpixmap;
}

cairo_surface_t * x11_helper_get_bg_surface ( void )
{
    xcb_pixmap_t pm = get_root_pixmap ( xcb->connection, xcb->screen, netatoms[ESETROOT_PMAP_ID] );
    if ( pm == XCB_NONE ) {
        return NULL;
    }
    return cairo_xcb_surface_create ( xcb->connection, pm, root_visual,
                                      xcb->screen->width_in_pixels, xcb->screen->height_in_pixels );
}

// retrieve a text property from a window
// technically we could use window_get_prop(), but this is better for character set support
char* window_get_text_prop ( xcb_window_t w, xcb_atom_t atom )
{
    xcb_get_property_cookie_t c  = xcb_get_property ( xcb->connection, 0, w, atom, XCB_GET_PROPERTY_TYPE_ANY, 0, UINT_MAX );
    xcb_get_property_reply_t  *r = xcb_get_property_reply ( xcb->connection, c, NULL );
    if ( r ) {
        if ( xcb_get_property_value_length ( r ) > 0 ) {
            char *str = NULL;
            if ( r->type == netatoms[UTF8_STRING] ) {
                str = g_strndup ( xcb_get_property_value ( r ), xcb_get_property_value_length ( r ) );
            }
            else if ( r->type == netatoms[STRING] ) {
                str = rofi_latin_to_utf8_strdup ( xcb_get_property_value ( r ), xcb_get_property_value_length ( r ) );
            }
            else {
                str = g_strdup ( "Invalid encoding." );
            }

            free ( r );
            return str;
        }
        free ( r );
    }
    return NULL;
}

void window_set_atom_prop ( xcb_window_t w, xcb_atom_t prop, xcb_atom_t *atoms, int count )
{
    xcb_change_property ( xcb->connection, XCB_PROP_MODE_REPLACE, w, prop, XCB_ATOM_ATOM, 32, count, atoms );
}

/****
 * Code used to get monitor layout.
 */

/**
 * Free monitor structure.
 */
static void x11_monitor_free ( workarea *m )
{
    g_free ( m->name );
    g_free ( m );
}

static void x11_monitors_free ( void )
{
    while ( xcb->monitors != NULL ) {
        workarea *m = xcb->monitors;
        xcb->monitors = m->next;
        x11_monitor_free ( m );
    }
}

/**
 * Create monitor based on output id
 */
static workarea * x11_get_monitor_from_output ( xcb_randr_output_t out )
{
    xcb_randr_get_output_info_reply_t  *op_reply;
    xcb_randr_get_crtc_info_reply_t    *crtc_reply;
    xcb_randr_get_output_info_cookie_t it = xcb_randr_get_output_info ( xcb->connection, out, XCB_CURRENT_TIME );
    op_reply = xcb_randr_get_output_info_reply ( xcb->connection, it, NULL );
    if ( op_reply->crtc == XCB_NONE ) {
        free ( op_reply );
        return NULL;
    }
    xcb_randr_get_crtc_info_cookie_t ct = xcb_randr_get_crtc_info ( xcb->connection, op_reply->crtc, XCB_CURRENT_TIME );
    crtc_reply = xcb_randr_get_crtc_info_reply ( xcb->connection, ct, NULL );
    if ( !crtc_reply ) {
        free ( op_reply );
        return NULL;
    }
    workarea *retv = g_malloc0 ( sizeof ( workarea ) );
    retv->x = crtc_reply->x;
    retv->y = crtc_reply->y;
    retv->w = crtc_reply->width;
    retv->h = crtc_reply->height;

    char *tname    = (char *) xcb_randr_get_output_info_name ( op_reply );
    int  tname_len = xcb_randr_get_output_info_name_length ( op_reply );

    retv->name = g_malloc0 ( ( tname_len + 1 ) * sizeof ( char ) );
    memcpy ( retv->name, tname, tname_len );

    free ( crtc_reply );
    free ( op_reply );
    return retv;
}

static int x11_is_extension_present ( const char *extension )
{
    xcb_query_extension_cookie_t randr_cookie = xcb_query_extension ( xcb->connection, strlen ( extension ), extension );

    xcb_query_extension_reply_t  *randr_reply = xcb_query_extension_reply ( xcb->connection, randr_cookie, NULL );

    int                          present = randr_reply->present;

    free ( randr_reply );

    return present;
}

static void x11_build_monitor_layout_xinerama ()
{
    xcb_xinerama_query_screens_cookie_t screens_cookie = xcb_xinerama_query_screens_unchecked (
        xcb->connection
        );

    xcb_xinerama_query_screens_reply_t *screens_reply = xcb_xinerama_query_screens_reply (
        xcb->connection,
        screens_cookie,
        NULL
        );

    xcb_xinerama_screen_info_iterator_t screens_iterator = xcb_xinerama_query_screens_screen_info_iterator (
        screens_reply
        );

    for (; screens_iterator.rem > 0; xcb_xinerama_screen_info_next ( &screens_iterator ) ) {
        workarea *w = g_malloc0 ( sizeof ( workarea ) );

        w->x = screens_iterator.data->x_org;
        w->y = screens_iterator.data->y_org;
        w->w = screens_iterator.data->width;
        w->h = screens_iterator.data->height;

        w->next       = xcb->monitors;
        xcb->monitors = w;
    }

    int index = 0;
    for ( workarea *iter = xcb->monitors; iter; iter = iter->next ) {
        iter->monitor_id = index++;
    }

    free ( screens_reply );
}

void x11_build_monitor_layout ()
{
    if ( xcb->monitors ) {
        return;
    }
    // If RANDR is not available, try Xinerama
    if ( !x11_is_extension_present ( "RANDR" ) ) {
        // Check if xinerama is available.
        if ( x11_is_extension_present ( "XINERAMA" ) ) {
            g_log ( LOG_DOMAIN, G_LOG_LEVEL_DEBUG, "Query XINERAMA for monitor layout." );
            x11_build_monitor_layout_xinerama ();
            return;
        }
        g_log ( LOG_DOMAIN, G_LOG_LEVEL_DEBUG, "No RANDR or Xinerama available for getting monitor layout." );
        return;
    }
    g_log ( LOG_DOMAIN, G_LOG_LEVEL_DEBUG, "Query RANDR for monitor layout." );

    xcb_randr_get_screen_resources_current_reply_t  *res_reply;
    xcb_randr_get_screen_resources_current_cookie_t src;
    src       = xcb_randr_get_screen_resources_current ( xcb->connection, xcb->screen->root );
    res_reply = xcb_randr_get_screen_resources_current_reply ( xcb->connection, src, NULL );
    if ( !res_reply ) {
        return;  //just report error
    }
    int                mon_num = xcb_randr_get_screen_resources_current_outputs_length ( res_reply );
    xcb_randr_output_t *ops    = xcb_randr_get_screen_resources_current_outputs ( res_reply );

    // Get primary.
    xcb_randr_get_output_primary_cookie_t pc      = xcb_randr_get_output_primary ( xcb->connection, xcb->screen->root );
    xcb_randr_get_output_primary_reply_t  *pc_rep = xcb_randr_get_output_primary_reply ( xcb->connection, pc, NULL );

    for ( int i = mon_num - 1; i >= 0; i-- ) {
        workarea *w = x11_get_monitor_from_output ( ops[i] );
        if ( w ) {
            w->next       = xcb->monitors;
            xcb->monitors = w;
            if ( pc_rep && pc_rep->output == ops[i] ) {
                w->primary = TRUE;
            }
        }
    }
    // Number monitor
    int index = 0;
    for ( workarea *iter = xcb->monitors; iter; iter = iter->next ) {
        iter->monitor_id = index++;
    }
    // If exists, free primary output reply.
    if ( pc_rep ) {
        free ( pc_rep );
    }
    free ( res_reply );
}

void x11_dump_monitor_layout ( void )
{
    int is_term = isatty ( fileno ( stdout ) );
    printf ( "Monitor layout:\n" );
    for ( workarea *iter = xcb->monitors; iter; iter = iter->next ) {
        printf ( "%s              ID%s: %d", ( is_term ) ? color_bold : "", is_term ? color_reset : "", iter->monitor_id );
        if ( iter->primary ) {
            printf ( " (primary)" );
        }
        printf ( "\n" );
        printf ( "%s            name%s: %s\n", ( is_term ) ? color_bold : "", is_term ? color_reset : "", iter->name );
        printf ( "%s        position%s: %d,%d\n", ( is_term ) ? color_bold : "", is_term ? color_reset : "", iter->x, iter->y );
        printf ( "%s            size%s: %d,%d\n", ( is_term ) ? color_bold : "", is_term ? color_reset : "", iter->w, iter->h );
        printf ( "\n" );
    }
}

static int monitor_get_dimension ( int monitor_id, workarea *mon )
{
    memset ( mon, 0, sizeof ( workarea ) );
    mon->w = xcb->screen->width_in_pixels;
    mon->h = xcb->screen->height_in_pixels;

    workarea *iter = NULL;
    for ( iter = xcb->monitors; iter; iter = iter->next ) {
        if ( iter->monitor_id == monitor_id ) {
            *mon = *iter;
            return TRUE;
        }
    }
    return FALSE;
}
// find the dimensions of the monitor displaying point x,y
static void monitor_dimensions ( int x, int y, workarea *mon )
{
    memset ( mon, 0, sizeof ( workarea ) );
    mon->w = xcb->screen->width_in_pixels;
    mon->h = xcb->screen->height_in_pixels;

    for ( workarea *iter = xcb->monitors; iter; iter = iter->next ) {
        if ( INTERSECT ( x, y, iter->x, iter->y, iter->w, iter->h ) ) {
            *mon = *iter;
            break;
        }
    }
}

/**
 * @param root The X11 window used to find the pointer position. Usually the root window.
 * @param x    The x position of the mouse [out]
 * @param y    The y position of the mouse [out]
 *
 * find mouse pointer location
 *
 * @returns TRUE when found, FALSE otherwise
 */
static int pointer_get ( xcb_window_t root, int *x, int *y )
{
    *x = 0;
    *y = 0;
    xcb_query_pointer_cookie_t c  = xcb_query_pointer ( xcb->connection, root );
    xcb_query_pointer_reply_t  *r = xcb_query_pointer_reply ( xcb->connection, c, NULL );
    if ( r ) {
        *x = r->root_x;
        *y = r->root_y;
        free ( r );
        return TRUE;
    }

    return FALSE;
}

static int monitor_active_from_id ( int mon_id, workarea *mon )
{
    xcb_window_t root = xcb->screen->root;
    int          x, y;
    // At mouse position.
    if ( mon_id == -3 ) {
        if ( pointer_get ( root, &x, &y ) ) {
            monitor_dimensions ( x, y, mon );
            mon->x = x;
            mon->y = y;
            return TRUE;
        }
    }
    // Focused monitor
    else if ( mon_id == -1 ) {
        // Get the current desktop.
        unsigned int              current_desktop = 0;
        xcb_get_property_cookie_t gcdc;
        gcdc = xcb_ewmh_get_current_desktop ( &xcb->ewmh, xcb->screen_nbr );
        if  ( xcb_ewmh_get_current_desktop_reply ( &xcb->ewmh, gcdc, &current_desktop, NULL ) ) {
            xcb_get_property_cookie_t             c = xcb_ewmh_get_desktop_viewport ( &xcb->ewmh, xcb->screen_nbr );
            xcb_ewmh_get_desktop_viewport_reply_t vp;
            if ( xcb_ewmh_get_desktop_viewport_reply ( &xcb->ewmh, c, &vp, NULL ) ) {
                if ( current_desktop < vp.desktop_viewport_len ) {
                    monitor_dimensions ( vp.desktop_viewport[current_desktop].x,
                                         vp.desktop_viewport[current_desktop].y, mon );
                    xcb_ewmh_get_desktop_viewport_reply_wipe ( &vp );
                    return TRUE;
                }
                xcb_ewmh_get_desktop_viewport_reply_wipe ( &vp );
            }
        }
    }
    else if ( mon_id == -2 || mon_id == -4 ) {
        xcb_window_t              active_window;
        xcb_get_property_cookie_t awc;
        awc = xcb_ewmh_get_active_window ( &xcb->ewmh, xcb->screen_nbr );
        if ( xcb_ewmh_get_active_window_reply ( &xcb->ewmh, awc, &active_window, NULL ) ) {
            // get geometry.
            xcb_get_geometry_cookie_t c  = xcb_get_geometry ( xcb->connection, active_window );
            xcb_get_geometry_reply_t  *r = xcb_get_geometry_reply ( xcb->connection, c, NULL );
            if ( r ) {
                xcb_translate_coordinates_cookie_t ct = xcb_translate_coordinates ( xcb->connection, active_window, root, r->x, r->y );
                xcb_translate_coordinates_reply_t  *t = xcb_translate_coordinates_reply ( xcb->connection, ct, NULL );
                if ( t ) {
                    if ( mon_id == -2 ) {
                        // place the menu above the window
                        // if some window is focused, place menu above window, else fall
                        // back to selected monitor.
                        mon->x = t->dst_x - r->x;
                        mon->y = t->dst_y - r->y;
                        mon->w = r->width;
                        mon->h = r->height;
                        free ( r );
                        free ( t );
                        return TRUE;
                    }
                    else if ( mon_id == -4 ) {
                        monitor_dimensions ( t->dst_x, t->dst_y, mon );
                        free ( r );
                        free ( t );
                        return TRUE;
                    }
                }
                free ( r );
            }
        }
    }
    // Monitor that has mouse pointer.
    else if ( mon_id == -5 ) {
        if ( pointer_get ( root, &x, &y ) ) {
            monitor_dimensions ( x, y, mon );
            return TRUE;
        }
        // This is our give up point.
        return FALSE;
    }
    g_log ( LOG_DOMAIN, G_LOG_LEVEL_DEBUG, "Failed to find monitor, fall back to monitor showing mouse." );
    return monitor_active_from_id ( -5, mon );
}

// determine which monitor holds the active window, or failing that the mouse pointer
int monitor_active ( workarea *mon )
{
    if ( config.monitor != NULL ) {
        for ( workarea *iter = xcb->monitors; iter; iter = iter->next ) {
            if ( g_strcmp0 ( config.monitor, iter->name ) == 0 ) {
                *mon = *iter;
                return TRUE;
            }
        }
    }
    // Grab primary.
    if ( g_strcmp0 ( config.monitor, "primary" ) == 0 ) {
        for ( workarea *iter = xcb->monitors; iter; iter = iter->next ) {
            if ( iter->primary ) {
                *mon = *iter;
                return TRUE;
            }
        }
    }
    // IF fail, fall back to classic mode.
    char   *end   = NULL;
    gint64 mon_id = g_ascii_strtoll ( config.monitor, &end, 0 );
    if ( end != config.monitor ) {
        if ( mon_id >= 0 ) {
            if ( monitor_get_dimension ( mon_id, mon ) ) {
                return TRUE;
            }
            fprintf ( stderr, "Failed to find selected monitor.\n" );
        }
        else {
            return monitor_active_from_id ( mon_id, mon );
        }
    }
    // Fallback.
    monitor_dimensions ( 0, 0, mon );
    return FALSE;
}
int take_pointer ( xcb_window_t w )
{
    for ( int i = 0; i < 500; i++ ) {
        if ( xcb_connection_has_error ( xcb->connection ) ) {
            fprintf ( stderr, "Connection has error\n" );
            exit ( EXIT_FAILURE );
        }
        xcb_grab_pointer_cookie_t cc = xcb_grab_pointer ( xcb->connection, 1, w, XCB_EVENT_MASK_BUTTON_RELEASE,
                                                          XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC, w, XCB_NONE, XCB_CURRENT_TIME );
        xcb_grab_pointer_reply_t  *r = xcb_grab_pointer_reply ( xcb->connection, cc, NULL );
        if ( r ) {
            if ( r->status == XCB_GRAB_STATUS_SUCCESS ) {
                free ( r );
                return 1;
            }
            free ( r );
        }
        usleep ( 1000 );
    }
    fprintf ( stderr, "Failed to grab pointer.\n" );
    return 0;
}
int take_keyboard ( xcb_window_t w )
{
    for ( int i = 0; i < 500; i++ ) {
        if ( xcb_connection_has_error ( xcb->connection ) ) {
            fprintf ( stderr, "Connection has error\n" );
            exit ( EXIT_FAILURE );
        }
        xcb_grab_keyboard_cookie_t cc = xcb_grab_keyboard ( xcb->connection,
                                                            1, w, XCB_CURRENT_TIME, XCB_GRAB_MODE_ASYNC,
                                                            XCB_GRAB_MODE_ASYNC );
        xcb_grab_keyboard_reply_t *r = xcb_grab_keyboard_reply ( xcb->connection, cc, NULL );
        if ( r ) {
            if ( r->status == XCB_GRAB_STATUS_SUCCESS ) {
                free ( r );
                return 1;
            }
            free ( r );
        }
        usleep ( 1000 );
    }

    return 0;
}

void release_keyboard ( void )
{
    xcb_ungrab_keyboard ( xcb->connection, XCB_CURRENT_TIME );
}
void release_pointer ( void )
{
    xcb_ungrab_pointer ( xcb->connection, XCB_CURRENT_TIME );
}

static unsigned int x11_find_mod_mask ( xkb_stuff *xkb, ... )
{
    va_list         names;
    const char      *name;
    xkb_mod_index_t i;
    unsigned int    mask = 0;
    va_start ( names, xkb );
    while ( ( name = va_arg ( names, const char * ) ) != NULL ) {
        i = xkb_keymap_mod_get_index ( xkb->keymap, name );
        if ( i != XKB_MOD_INVALID ) {
            mask |= 1 << i;
        }
    }
    va_end ( names );
    return mask;
}

static void x11_figure_out_masks ( xkb_stuff *xkb )
{
    x11_mod_masks[X11MOD_SHIFT]   = x11_find_mod_mask ( xkb, XKB_MOD_NAME_SHIFT, NULL );
    x11_mod_masks[X11MOD_CONTROL] = x11_find_mod_mask ( xkb, XKB_MOD_NAME_CTRL, NULL );
    x11_mod_masks[X11MOD_ALT]     = x11_find_mod_mask ( xkb, XKB_MOD_NAME_ALT, "Alt", "LAlt", "RAlt", "AltGr", "Mod5", "LevelThree", NULL );
    x11_mod_masks[X11MOD_META]    = x11_find_mod_mask ( xkb, "Meta", NULL );
    x11_mod_masks[X11MOD_SUPER]   = x11_find_mod_mask ( xkb, XKB_MOD_NAME_LOGO, "Super", NULL );
    x11_mod_masks[X11MOD_HYPER]   = x11_find_mod_mask ( xkb, "Hyper", NULL );

    gsize i;
    for ( i = 0; i < X11MOD_ANY; ++i ) {
        x11_mod_masks[X11MOD_ANY] |= x11_mod_masks[i];
    }
}

int x11_modifier_active ( unsigned int mask, int key )
{
    return ( x11_mod_masks[key] & mask ) != 0;
}

unsigned int x11_canonalize_mask ( unsigned int mask )
{
    // Bits 13 and 14 of the modifiers together are the group number, and
    // should be ignored when looking up key bindings
    mask &= x11_mod_masks[X11MOD_ANY];

    gsize i;
    for ( i = 0; i < X11MOD_ANY; ++i ) {
        if ( mask & x11_mod_masks[i] ) {
            mask |= x11_mod_masks[i];
        }
    }
    return mask;
}

unsigned int x11_get_current_mask ( xkb_stuff *xkb )
{
    unsigned int mask = 0;
    for ( gsize i = 0; i < xkb_keymap_num_mods ( xkb->keymap ); ++i ) {
        if ( xkb_state_mod_index_is_active ( xkb->state, i, XKB_STATE_MODS_EFFECTIVE ) ) {
            mask |= ( 1 << i );
        }
    }
    return x11_canonalize_mask ( mask );
}

// convert a Mod+key arg to mod mask and keysym
gboolean x11_parse_key ( const char *combo, unsigned int *mod, xkb_keysym_t *key, gboolean *release, GString *str )
{
    char         *input_key = g_strdup ( combo );
    char         *mod_key   = input_key;
    char         *error_msg = NULL;
    unsigned int modmask    = 0;
    // Test if this works on release.
    if ( g_str_has_prefix ( mod_key, "!" ) ) {
        ++mod_key;
        *release = TRUE;
    }

    char         *saveptr = NULL;
    xkb_keysym_t sym      = XKB_KEY_NoSymbol;

    char **entries = g_strsplit_set ( mod_key, "+-", -1);
    for ( int i = 0; entries && entries[i]; i++ ) {
        char *entry = entries[i];
        // Remove trailing and leading spaces.
        entry = g_strstrip ( entry );
        // Compare against lowered version.
        char *entry_lowered = g_utf8_strdown ( entry, -1 );
        if ( g_utf8_collate ( entry_lowered, "shift" ) == 0  ) {
            modmask |= x11_mod_masks[X11MOD_SHIFT];
            if ( x11_mod_masks[X11MOD_SHIFT] == 0 ) {
                error_msg = g_strdup ( "X11 configured keyboard has no <b>Shift</b> key.\n" );
            }
        }
        else if  ( g_utf8_collate ( entry_lowered, "control" ) == 0 ) {
            modmask |= x11_mod_masks[X11MOD_CONTROL];
            if  ( x11_mod_masks[X11MOD_CONTROL] == 0 ) {
                error_msg = g_strdup ( "X11 configured keyboard has no <b>Control</b> key.\n" );
            }
        }
        else if  ( g_utf8_collate ( entry_lowered, "alt" ) == 0 ) {
            modmask |= x11_mod_masks[X11MOD_ALT];
            if  ( x11_mod_masks[X11MOD_ALT] == 0 ) {
                error_msg = g_strdup ( "X11 configured keyboard has no <b>Alt</b> key.\n" );
            }
        }
        else if  ( g_utf8_collate ( entry_lowered, "super" ) == 0 ) {
            modmask |= x11_mod_masks[X11MOD_SUPER];
            if  ( x11_mod_masks[X11MOD_SUPER] == 0 ) {
                error_msg = g_strdup ( "X11 configured keyboard has no <b>Super</b> key.\n" );
            }
        }
        else if  ( g_utf8_collate ( entry_lowered, "meta" ) == 0 ) {
            modmask |= x11_mod_masks[X11MOD_META];
            if  ( x11_mod_masks[X11MOD_META] == 0 ) {
                error_msg = g_strdup ( "X11 configured keyboard has no <b>Meta</b> key.\n" );
            }
        }
        else if  ( g_utf8_collate ( entry_lowered, "hyper" ) == 0 ) {
            modmask |= x11_mod_masks[X11MOD_HYPER];
            if  ( x11_mod_masks[X11MOD_HYPER] == 0 ) {
                error_msg = g_strdup ( "X11 configured keyboard has no <b>Hyper</b> key.\n" );
            }
        }
        else {
            sym = xkb_keysym_from_name ( entry, XKB_KEYSYM_NO_FLAGS );
            if ( sym == XKB_KEY_NoSymbol ) {
                error_msg = g_markup_printf_escaped ( "∙ Key <i>%s</i> is not understood\n", entry );
            }
        }
        g_free ( entry_lowered );
    }
    g_strfreev(entries);

    g_free ( input_key );

    if ( error_msg ) {
        char *name = g_markup_escape_text ( combo, -1 );
        g_string_append_printf ( str, "Cannot understand the key combination: <i>%s</i>\n", name );
        g_string_append ( str, error_msg );
        g_free ( name );
        g_free ( error_msg );
        return FALSE;
    }
    *key = sym;
    *mod = modmask;
    return TRUE;
}

/**
 * Fill in the list of frequently used X11 Atoms.
 */
static void x11_create_frequently_used_atoms ( void )
{
    // X atom values
    for ( int i = 0; i < NUM_NETATOMS; i++ ) {
        xcb_intern_atom_cookie_t cc = xcb_intern_atom ( xcb->connection, 0, strlen ( netatom_names[i] ), netatom_names[i] );
        xcb_intern_atom_reply_t  *r = xcb_intern_atom_reply ( xcb->connection, cc, NULL );
        if ( r ) {
            netatoms[i] = r->atom;
            free ( r );
        }
    }
}

void x11_setup ( xkb_stuff *xkb )
{
    // determine numlock mask so we can bind on keys with and without it
    x11_figure_out_masks ( xkb );
    x11_create_frequently_used_atoms (  );
}

void x11_create_visual_and_colormap ( void )
{
    xcb_depth_t          *root_depth = NULL;
    xcb_depth_iterator_t depth_iter;
    for ( depth_iter = xcb_screen_allowed_depths_iterator ( xcb->screen ); depth_iter.rem; xcb_depth_next ( &depth_iter ) ) {
        xcb_depth_t               *d = depth_iter.data;

        xcb_visualtype_iterator_t visual_iter;
        for ( visual_iter = xcb_depth_visuals_iterator ( d ); visual_iter.rem; xcb_visualtype_next ( &visual_iter ) ) {
            xcb_visualtype_t *v = visual_iter.data;
            if ( ( v->bits_per_rgb_value == 8 ) && ( d->depth == 32 ) && ( v->_class == XCB_VISUAL_CLASS_TRUE_COLOR ) ) {
                depth  = d;
                visual = v;
            }
            if ( xcb->screen->root_visual == v->visual_id ) {
                root_depth  = d;
                root_visual = v;
            }
        }
    }
    if ( visual != NULL ) {
        xcb_void_cookie_t   c;
        xcb_generic_error_t *e;
        map = xcb_generate_id ( xcb->connection );
        c   = xcb_create_colormap_checked ( xcb->connection, XCB_COLORMAP_ALLOC_NONE, map, xcb->screen->root, visual->visual_id );
        e   = xcb_request_check ( xcb->connection, c );
        if ( e ) {
            depth  = NULL;
            visual = NULL;
            free ( e );
        }
    }

    if ( visual == NULL ) {
        depth  = root_depth;
        visual = root_visual;
        map    = xcb->screen->default_colormap;
    }
}

Color color_get ( const char *const name )
{
    char *copy  = g_strdup ( name );
    char *cname = g_strstrip ( copy );

    union
    {
        struct
        {
            uint8_t b;
            uint8_t g;
            uint8_t r;
            uint8_t a;
        }        sep;
        uint32_t pixel;
    } color = {
        .pixel = 0xffffffff,
    };
    // Special format.
    if ( strncmp ( cname, "argb:", 5 ) == 0 ) {
        color.pixel = strtoul ( &cname[5], NULL, 16 );
    }
    else if ( strncmp ( cname, "#", 1 ) == 0 ) {
        unsigned long val    = strtoul ( &cname[1], NULL, 16 );
        ssize_t       length = strlen ( &cname[1] );
        switch ( length )
        {
        case 3:
            color.sep.a = 0xff;
            color.sep.r = 16 * ( ( val & 0xF00 ) >> 8 );
            color.sep.g = 16 * ( ( val & 0x0F0 ) >> 4 );
            color.sep.b = 16 * ( val & 0x00F );
            break;
        case 6:
            color.pixel = val;
            color.sep.a = 0xff;
            break;
        case 8:
            color.pixel = val;
            break;
        default:
            break;
        }
    }
    else {
        xcb_alloc_named_color_cookie_t cc = xcb_alloc_named_color ( xcb->connection,
                                                                    map, strlen ( cname ), cname );
        xcb_alloc_named_color_reply_t  *r = xcb_alloc_named_color_reply ( xcb->connection, cc, NULL );
        if ( r ) {
            color.sep.a = 0xFF;
            color.sep.r = r->visual_red;
            color.sep.g = r->visual_green;
            color.sep.b = r->visual_blue;
            free ( r );
        }
    }
    g_free ( copy );

    Color ret = {
        .red   = color.sep.r / 255.0,
        .green = color.sep.g / 255.0,
        .blue  = color.sep.b / 255.0,
        .alpha = color.sep.a / 255.0,
    };
    return ret;
}

void x11_helper_set_cairo_rgba ( cairo_t *d, Color col )
{
    cairo_set_source_rgba ( d, col.red, col.green, col.blue, col.alpha );
}

/**
 * Type of colors stored
 */
enum
{
    BACKGROUND,
    BORDER,
    SEPARATOR
};
/**
 * Color cache.
 *
 * This stores the current color until
 */
static struct
{
    /** The color */
    Color        color;
    /** Flag indicating it is set. */
    unsigned int set;
} color_cache[3];

void color_background ( cairo_t *d )
{
    if ( !color_cache[BACKGROUND].set ) {
        gchar **vals = g_strsplit ( config.color_window, ",", 3 );
        if ( vals != NULL && vals[0] != NULL ) {
            color_cache[BACKGROUND].color = color_get ( vals[0] );
        }
        g_strfreev ( vals );
        color_cache[BACKGROUND].set = TRUE;
    }

    x11_helper_set_cairo_rgba ( d, color_cache[BACKGROUND].color );
}

void color_border ( cairo_t *d  )
{
    if ( !color_cache[BORDER].set ) {
        gchar **vals = g_strsplit ( config.color_window, ",", 3 );
        if ( vals != NULL && vals[0] != NULL && vals[1] != NULL ) {
            color_cache[BORDER].color = color_get ( vals[1] );
        }
        g_strfreev ( vals );
        color_cache[BORDER].set = TRUE;
    }
    x11_helper_set_cairo_rgba ( d, color_cache[BORDER].color );
}

void color_separator ( cairo_t *d )
{
    if ( !color_cache[SEPARATOR].set ) {
        gchar **vals = g_strsplit ( config.color_window, ",", 3 );
        if ( vals != NULL && vals[0] != NULL && vals[1] != NULL && vals[2] != NULL  ) {
            color_cache[SEPARATOR].color = color_get ( vals[2] );
        }
        else if ( vals != NULL && vals[0] != NULL && vals[1] != NULL ) {
            color_cache[SEPARATOR].color = color_get ( vals[1] );
        }
        g_strfreev ( vals );
        color_cache[SEPARATOR].set = TRUE;
    }
    x11_helper_set_cairo_rgba ( d, color_cache[SEPARATOR].color );
}

xcb_window_t xcb_stuff_get_root_window ( xcb_stuff *xcb )
{
    return xcb->screen->root;
}

void xcb_stuff_wipe ( xcb_stuff *xcb )
{
    if ( xcb->connection != NULL ) {
        if ( xcb->sncontext != NULL ) {
            sn_launchee_context_unref ( xcb->sncontext );
            xcb->sncontext = NULL;
        }
        if ( xcb->sndisplay != NULL ) {
            sn_display_unref ( xcb->sndisplay );
            xcb->sndisplay = NULL;
        }
        x11_monitors_free ();
        xcb_disconnect ( xcb->connection );
        xcb_ewmh_connection_wipe ( &( xcb->ewmh ) );
        xcb->connection = NULL;
        xcb->screen     = NULL;
        xcb->screen_nbr = 0;
    }
}

void x11_disable_decoration ( xcb_window_t window )
{
    // Flag used to indicate we are setting the decoration type.
    const uint32_t MWM_HINTS_DECORATIONS = ( 1 << 1 );
    // Motif property data structure
    struct MotifWMHints
    {
        uint32_t flags;
        uint32_t functions;
        uint32_t decorations;
        int32_t  inputMode;
        uint32_t state;
    };

    struct MotifWMHints hints;
    hints.flags       = MWM_HINTS_DECORATIONS;
    hints.decorations = 0;
    hints.functions   = 0;
    hints.inputMode   = 0;
    hints.state       = 0;

    xcb_atom_t ha = netatoms[_MOTIF_WM_HINTS];
    xcb_change_property ( xcb->connection, XCB_PROP_MODE_REPLACE, window, ha, ha, 32, 5, &hints );
}
