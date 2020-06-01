#include <stdint.h>
#include <stdlib.h>

#include <xcb/xcb.h>
#include <xcb/xproto.h>

#include "../3rdparty/pt/pt.h"

#include "../c.h"

#include "../algebra.h"
#include "../colour.h"
#include "../image.h"

#include "../x11/draw.h"
#include "../x11/wevent.h"


int displaybpp(Display *disp);


Connection *
newconn(char *connstr)
{
	/* XXX we get the screen but throw it away */
	int n;
	Connection *c = xcb_connect(connstr, &n);

	if (xcb_connection_has_error(c)) {
		freeconn(c);
		return nil;
	}
	
	return c;
}

void
freeconn(Connection *c)
{
	xcb_disconnect(c);
}


Display *
newdisplay(Connection *c, int n, SCProfile *cp)
{
	Display *d;

	xcb_screen_iterator_t siter;
	xcb_depth_iterator_t diter;
	xcb_visualtype_iterator_t viter;
	xcb_void_cookie_t cookie;

	int depth; /* depth per channel */

	if ((d=calloc(1, sizeof(Display))) == nil) {
		return nil;
	}

	d->c = c;

	siter = xcb_setup_roots_iterator(xcb_get_setup(c));
	for (; siter.rem; n--, xcb_screen_next(&siter))
		if (n == 0) {
			d->s = siter.data;
			break;
		}

	if (d->s == nil) {
		freedisplay(d);
		return nil;
	}

	if ((d->root=calloc(1, sizeof(Win))) == nil) {
		freedisplay(d);
		return nil;
	}
	
	*d->root = rootwin(d);

	d->vid = d->s->root_visual;

	diter = xcb_screen_allowed_depths_iterator(d->s);
	for (; diter.rem; xcb_depth_next(&diter)) {
		viter = xcb_depth_visuals_iterator(diter.data);
		for (; viter.rem; xcb_visualtype_next (&viter)) {
			if (d->vid == viter.data->visual_id) {
				d->v = viter.data;
				break;
			}
		}
	}

	if (d->v == nil) {
		freedisplay(d);
		return nil;
	}

	if (d->v->_class != XCB_VISUAL_CLASS_TRUE_COLOR && d->v->_class != XCB_VISUAL_CLASS_DIRECT_COLOR) {
		freedisplay(d);
		return nil;
	}

	d->gc0id = xcb_generate_id(d->c);
	cookie = xcb_create_gc_checked(d->c, d->gc0id, d->s->root, XCB_GC_FOREGROUND | XCB_GC_BACKGROUND, (uint32_t[]) { d->s->black_pixel, d->s->white_pixel });

	if (xcb_request_check(d->c, cookie)) {
		freedisplay(d);
		return nil;
	}

	/* bits_per_rgb_value doesn't always match the colour mask */
	//depth = d->v->bits_per_rgb_value;

	/* we require all channels to have the same depth */
	depth = nsetbits(d->v->red_mask);
	if (nsetbits(d->v->green_mask) != depth) {
		freedisplay(d);
		return nil;
	}
	if (nsetbits(d->v->blue_mask) != depth) {
		freedisplay(d);
		return nil;
	}
	if (depth == 0) {
		freedisplay(d);
		return nil;
	}

	if (cp != nil) {
		if ((d->cp = newscprofile(depth, cp->red, cp->grn, cp->blu, cp->wht, cp->gR, cp->gG, cp->gB)) == nil) {
			freedisplay(d);
			return nil;
		}
	} else {
		/* default */
		if ((d->cp = newscprofile0(depth)) == nil) {
			freedisplay(d);
			return nil;
		}
	}

	return d;
}

void
freedisplay(Display *d)
{
	if (d->cp) {
		freescprofile(d->cp);
	}

	if (d->root) {
		free(d->root);
	}

	free(d);
}

int
displaybpp(Display *d)
{
	xcb_format_iterator_t iter = xcb_setup_pixmap_formats_iterator(xcb_get_setup(d->c));

	for (; iter.rem; xcb_format_next(&iter))
		if (iter.data->depth == d->s->root_depth)
			return iter.data->bits_per_pixel;

    return 0;
}

Win *
newwin(Win *parent, Rect r)
{
	Win *w;
	xcb_void_cookie_t cookie;

	if (!parent) return nil;

	if ((w = calloc(1, sizeof(Win))) == nil) {
		return nil;
	}

	w->disp = parent->disp;
	w->id = xcb_generate_id(w->disp->c);
	w->parent = parent;
	w->r = r;
	w->visible = 0;
	
	cookie = xcb_create_window_checked(w->disp->c, XCB_COPY_FROM_PARENT, w->id, parent->id, r.min.x, r.min.y, dRx(r), dRy(r), BORDER_WIDTH, XCB_WINDOW_CLASS_INPUT_OUTPUT, w->disp->vid, 0, nil);

	if (xcb_request_check(w->disp->c, cookie)) {
		freewin(w);
		return nil;
	}

	flushwin(w);

	return w;
}

void
freewin(Win *w)
{
	if (w->id) xcb_destroy_window(w->disp->c, w->id);

	free(w);
}

int
flushwin(Win *w)
{
	return xcb_flush(w->disp->c) > 0;
}

Win
rootwin(Display *disp)
{
	Win w;

	w.parent = nil;
	w.disp = disp;
	w.id = disp->s->root;
	w.r = (Rect){ .min = {.x = 0, .y = 0}, .max = { .x = disp->s->width_in_pixels, .y = disp->s->height_in_pixels } };
	w.visible = 1;

	return w;
}

int
showwin(Win *w)
{
	xcb_void_cookie_t cookie = xcb_map_window_checked(w->disp->c, w->id);

	if (xcb_request_check(w->disp->c, cookie))
		return -1;

	return 0;
}

int
hidewin(Win *w)
{
	xcb_void_cookie_t cookie = xcb_unmap_window_checked(w->disp->c, w->id);

	if (xcb_request_check(w->disp->c, cookie))
		return -1;

	return 0;
}

int
movewin(Win *w, Rect r)
{
	uint16_t mask = XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y | XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT;
	uint32_t values[] = { r.min.x, r.min.y, dRx(r), dRy(r) };

	xcb_void_cookie_t cookie = xcb_configure_window_checked(w->disp->c, w->id, mask, values);

	if (xcb_request_check(w->disp->c, cookie))
		return -1;

	return 0;
}

int
drawraster(Win *w, Raster *rst, Rect r_from, Rect r_to)
{
	int wd, ht;
	xcb_void_cookie_t cookie;

	if (rst->disp != w->disp) return -1;

	if (!RinR(r_from, rst->r)) return -1;
	if (!RinR(r_to, w->r)) return -1;

	if ((wd = dRx(r_from)) != dRx(r_to)) return -1;
	if ((ht = dRy(r_from)) != dRy(r_to)) return -1;

	cookie = xcb_copy_area_checked(w->disp->c, rst->id, w->id, w->disp->gc0id, r_from.min.x, r_from.min.y, r_to.min.x, r_to.min.y, wd, ht);

	if (xcb_request_check(rst->disp->c, cookie)) {
		return -2;
	}

	return 0;
}


Raster *
newraster(Display *disp, Rect r)
{
	Raster *rst;
	xcb_void_cookie_t cookie;

	if (!disp) return nil;

	if ((rst = calloc(1, sizeof(Raster))) == nil) {
		return nil;
	}

	rst->disp = disp;
	rst->id = xcb_generate_id(disp->c);
	rst->r = r;
	
	cookie = xcb_create_pixmap_checked(rst->disp->c, XCB_COPY_FROM_PARENT, rst->id, rootwin(rst->disp).id, dRx(r), dRy(r));

	if (xcb_request_check(rst->disp->c, cookie)) {
		freeraster(rst);
		return nil;
	}

	return rst;
}

void
freeraster(Raster *rst)
{
	if (rst->id) xcb_free_pixmap(rst->disp->c, rst->id);
	free(rst);
}


int
ldraster(Raster *rst, Image *im, Rect r_from, Rect r_to)
{
	void *row;
	uint8_t *r, *g, *b;

	int wd, ht, i;
	int chanr, chang, chanb;
	long idx_r, idx_g, idx_b;
	Point pt;

	int srcBs = imchanbytesize(im->typ);
	int destBs = rst->disp->cp->depth / 8;
	int destBpp = displaybpp(rst->disp) / 8;
	int padding =  destBpp - 3*destBs;

	if (padding < 0) return -3;

	for (i = 0; i < im->nchans; i++)
		if (!RinR(r_from, im->chans[i]->r)) return -1;

	if (!RinR(r_to, rst->r)) return -1;

	if ((wd = dRx(r_from)) != dRx(r_to)) return -1;
	if ((ht = dRy(r_from)) != dRy(r_to)) return -1;

	switch(im->typ) {
		case 	Imono_uint8:
		case 	Imono_uint16:
			chanr = chang = chanb = 0;
			break;
		case 	Irgb_uint8:
		case 	Irgb_uint16:
		case 	Irgb_bayer_rggb_uint8:
		case 	Irgb_bayer_rggb_uint16:
			chanr = 0; chang = 1; chanb = 2;
			break;
		default:
			return -1;
	}

	if ((row = calloc(1, destBpp*wd)) == nil)
		return -2;

	for (i = 0; i < ht; i++) {
		pt = (Point){ r_from.min.x, r_from.min.y+i };
		idx_r = P2rowM(im->chans[chanr]->r, pt) * srcBs;
		idx_g = P2rowM(im->chans[chang]->r, pt) * srcBs;
		idx_b = P2rowM(im->chans[chanb]->r, pt) * srcBs;

		r = (uint8_t *)(im->chans[chanr]->data) + idx_r;
		g = (uint8_t *)(im->chans[chang]->data) + idx_g;
		b = (uint8_t *)(im->chans[chanb]->data) + idx_b;

		if (packrgb(row, r, g, b, wd, destBs, srcBs, padding)) {
			free(row);
			return -3;
		}

		xcb_put_image(rst->disp->c, XCB_IMAGE_FORMAT_Z_PIXMAP, rst->id, rst->disp->gc0id, wd, 1, r_to.min.x, r_to.min.y+i, 0, rst->disp->s->root_depth, wd * destBpp, row);
	}
	
	free(row);
	return 0;
}