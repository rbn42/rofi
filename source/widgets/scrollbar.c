/**
 *   MIT/X11 License
 *   Modified  (c) 2015-2016 Qball Cow <qball@gmpclient.org>
 *
 *   Permission is hereby granted, free of charge, to any person obtaining
 *   a copy of this software and associated documentation files (the
 *   "Software"), to deal in the Software without restriction, including
 *   without limitation the rights to use, copy, modify, merge, publish,
 *   distribute, sublicense, and/or sell copies of the Software, and to
 *   permit persons to whom the Software is furnished to do so, subject to
 *   the following conditions:
 *
 *   The above copyright notice and this permission notice shall be
 *   included in all copies or substantial portions of the Software.
 *
 *   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 *   OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 *   MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 *   IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 *   CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 *   TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 *   SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include <config.h>
#include <xkbcommon/xkbcommon.h>
#include <glib.h>
#include "widgets/scrollbar.h"
#include "x11-helper.h"
#include "settings.h"

static void scrollbar_draw ( widget *, cairo_t * );
static void scrollbar_free ( widget * );
static gboolean scrollbar_motion_notify ( widget *wid, xcb_motion_notify_event_t *xme );

scrollbar *scrollbar_create ( short x, short y, short w, short h )
{
    scrollbar *sb = g_malloc0 ( sizeof ( scrollbar ) );

    sb->widget.x = x;
    sb->widget.y = y;
    sb->widget.w = MAX ( 1, w );
    sb->widget.h = MAX ( 1, h );

    sb->widget.draw          = scrollbar_draw;
    sb->widget.free          = scrollbar_free;
    sb->widget.motion_notify = scrollbar_motion_notify;

    sb->length     = 10;
    sb->pos        = 0;
    sb->pos_length = 4;

    // Enabled by default
    sb->widget.enabled = TRUE;
    return sb;
}

static void scrollbar_free ( widget *wid )
{
    scrollbar *sb = (scrollbar *) wid;
    g_free ( sb );
}

void scrollbar_set_max_value ( scrollbar *sb, unsigned int max )
{
    if ( sb != NULL ) {
        sb->length = MAX ( 1u, max );
    }
}

void scrollbar_set_handle ( scrollbar *sb, unsigned int pos )
{
    if ( sb != NULL ) {
        sb->pos = MIN ( sb->length, pos );
    }
}

void scrollbar_set_handle_length ( scrollbar *sb, unsigned int pos_length )
{
    if ( sb != NULL ) {
        sb->pos_length = MIN ( sb->length, MAX ( 1u, pos_length ) );
    }
}

/**
 * The range is the height - handle length.
 * r = h - handle;
 * handle is the element length of the handle* length of one element.
 * handle =  r / ( num ) * hl
 *
 * r = h - r / ( num) *hl
 * r*num = num*h - r*hl
 * r*num+r*hl = num*h;
 * r ( num+hl ) = num*h
 * r = (num*h)/(num+hl)
 */
static void scrollbar_draw ( widget *wid, cairo_t *draw )
{
    scrollbar    *sb = (scrollbar *) wid;
    // Calculate position and size.
    unsigned int r      = ( sb->length * wid->h ) / ( (double) ( sb->length + sb->pos_length ) );
    unsigned int handle = wid->h - r;
    double       sec    = ( ( r ) / (double) ( sb->length - 1 ) );
    unsigned int height = handle;
    unsigned int y      = sb->pos * sec;
    // Set max pos.
    y = MIN ( y, wid->h - handle );
    // Never go out of bar.
    height = MAX ( 2, height );
    // Cap length;
    color_separator ( draw );

    cairo_rectangle ( draw, sb->widget.x, sb->widget.y + y, sb->widget.w, height );
    cairo_fill ( draw );
}
static gboolean scrollbar_motion_notify ( widget *wid, xcb_motion_notify_event_t *xme )
{
    xcb_button_press_event_t xle;
    xle.event_x = xme->event_x;
    xle.event_y = xme->event_y;
    return widget_clicked ( WIDGET ( wid ), &xle );
}

// TODO
// This should behave more like a real scrollbar.
unsigned int scrollbar_clicked ( const scrollbar *sb, int y )
{
    if ( sb != NULL ) {
        if ( y >= sb->widget.y && y <= ( sb->widget.y + sb->widget.h ) ) {
            unsigned int r           = ( sb->length * sb->widget.h ) / ( (double) ( sb->length + sb->pos_length ) );
            unsigned int handle      = sb->widget.h - r;
            double       sec         = ( ( r ) / (double) ( sb->length - 1 ) );
            unsigned int half_handle = handle / 2;
            y -= sb->widget.y + half_handle;
            y  = MIN ( MAX ( 0, y ), sb->widget.h - 2 * half_handle );

            unsigned int sel = ( ( y ) / sec );
            return MIN ( sel, sb->length - 1 );
        }
    }
    return 0;
}
