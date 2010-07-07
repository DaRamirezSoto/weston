/*
 * Copyright © 2008-2010 Kristian Høgsberg
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/time.h>
#include <linux/input.h>

#include <xcb/xcb.h>
#include <xcb/dri2.h>
#include <xcb/xfixes.h>

#define GL_GLEXT_PROTOTYPES
#define EGL_EGLEXT_PROTOTYPES
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>

#include "wayland.h"
#include "wayland-protocol.h"
#include "compositor.h"

#define ARRAY_SIZE(array) (sizeof(array) / sizeof(array[0]))

struct x11_compositor {
	struct wlsc_compositor	 base;

	xcb_connection_t	*conn;
	xcb_screen_t		*screen;
	xcb_cursor_t		 null_cursor;
	int			 dri2_major;
	int			 dri2_minor;
	int			 drm_fd;
	struct wl_event_source	*xcb_source;
	struct {
		xcb_atom_t		 wm_protocols;
		xcb_atom_t		 wm_normal_hints;
		xcb_atom_t		 wm_size_hints;
		xcb_atom_t		 wm_delete_window;
		xcb_atom_t		 net_wm_name;
		xcb_atom_t		 utf8_string;
	} atom;
};

struct x11_output {
	struct wlsc_output	base;

	xcb_xfixes_region_t	region;
	xcb_window_t		window;
	GLuint			rbo;
	EGLImageKHR		image;
	xcb_rectangle_t		damage[16];
	int			damage_count;
};

struct x11_input {
	struct wlsc_input_device base;
};


static void
x11_input_create(struct x11_compositor *c)
{
	struct x11_input *input;

	input = malloc(sizeof *input);
	if (input == NULL)
		return;

	memset(input, 0, sizeof *input);
	wlsc_input_device_init(&input->base, &c->base);

	c->base.input_device = &input->base;
}


static int
dri2_connect(struct x11_compositor *c)
{
	xcb_xfixes_query_version_reply_t *xfixes_query;
	xcb_xfixes_query_version_cookie_t xfixes_query_cookie;
	xcb_dri2_query_version_reply_t *dri2_query;
	xcb_dri2_query_version_cookie_t dri2_query_cookie;
	xcb_dri2_connect_reply_t *connect;
	xcb_dri2_connect_cookie_t connect_cookie;
	xcb_generic_error_t *error;

	xcb_prefetch_extension_data (c->conn, &xcb_xfixes_id);
	xcb_prefetch_extension_data (c->conn, &xcb_dri2_id);

	xfixes_query_cookie =
		xcb_xfixes_query_version(c->conn,
					 XCB_XFIXES_MAJOR_VERSION,
					 XCB_XFIXES_MINOR_VERSION);
   
	dri2_query_cookie =
		xcb_dri2_query_version (c->conn,
					XCB_DRI2_MAJOR_VERSION,
					XCB_DRI2_MINOR_VERSION);

	connect_cookie = xcb_dri2_connect_unchecked (c->conn,
						     c->screen->root,
						     XCB_DRI2_DRIVER_TYPE_DRI);
   
	xfixes_query =
		xcb_xfixes_query_version_reply (c->conn,
						xfixes_query_cookie, &error);
	if (xfixes_query == NULL ||
	    error != NULL || xfixes_query->major_version < 2) {
		free(error);
		return -1;
	}
	free(xfixes_query);

	dri2_query =
		xcb_dri2_query_version_reply (c->conn,
					      dri2_query_cookie, &error);
	if (dri2_query == NULL || error != NULL) {
		fprintf(stderr, "DRI2: failed to query version");
		free(error);
		return EGL_FALSE;
	}
	c->dri2_major = dri2_query->major_version;
	c->dri2_minor = dri2_query->minor_version;
	free(dri2_query);

	connect = xcb_dri2_connect_reply (c->conn,
					  connect_cookie, NULL);
	if (connect == NULL ||
	    connect->driver_name_length + connect->device_name_length == 0) {
		fprintf(stderr, "DRI2: failed to authenticate");
		return -1;
	}

	c->base.base.device =
		strndup(xcb_dri2_connect_device_name (connect),
			xcb_dri2_connect_device_name_length (connect));
		   
	if (c->base.base.device == NULL) {
		free(connect);
		return -1;
	}
	free(connect);

	return 0;
}

static int
dri2_authenticate(struct x11_compositor *c)
{
	xcb_dri2_authenticate_reply_t *authenticate;
	xcb_dri2_authenticate_cookie_t authenticate_cookie;
	drm_magic_t magic;

	if (drmGetMagic(c->drm_fd, &magic)) {
		fprintf(stderr, "DRI2: failed to get drm magic");
		return -1;
	}

	authenticate_cookie =
		xcb_dri2_authenticate_unchecked(c->conn,
						c->screen->root, magic);
	authenticate =
		xcb_dri2_authenticate_reply(c->conn,
					    authenticate_cookie, NULL);
	if (authenticate == NULL || !authenticate->authenticated) {
		fprintf(stderr, "DRI2: failed to authenticate");
		free(authenticate);
		return -1;
	}

	free(authenticate);

	return 0;
}

static int
x11_compositor_init_egl(struct x11_compositor *c)
{
	PFNEGLGETTYPEDDISPLAYMESA get_typed_display_mesa;
	EGLint major, minor, count;
	EGLConfig config;

	static const EGLint config_attribs[] = {
		EGL_SURFACE_TYPE,		0,
		EGL_NO_SURFACE_CAPABLE_MESA,	EGL_OPENGL_BIT,
		EGL_RENDERABLE_TYPE,		EGL_OPENGL_BIT,
		EGL_NONE
	};

	if (dri2_connect(c) < 0)
		return -1;
	
	c->drm_fd = open(c->base.base.device, O_RDWR);
	if (c->drm_fd == -1) {
		fprintf(stderr,
			"DRI2: could not open %s (%s)", c->base.base.device,
			strerror(errno));
		return -1;
	}

	if (dri2_authenticate(c) < 0)
		return -1;

	get_typed_display_mesa =
		(PFNEGLGETTYPEDDISPLAYMESA)
		eglGetProcAddress("eglGetTypedDisplayMESA");
	if (get_typed_display_mesa == NULL) {
		fprintf(stderr, "eglGetTypedDisplayMESA() not found\n");
		return -1;
	}

	c->base.display = get_typed_display_mesa(EGL_DRM_DISPLAY_TYPE_MESA,
						 (void *) c->drm_fd);
	if (c->base.display == NULL) {
		fprintf(stderr, "failed to create display\n");
		return -1;
	}

	if (!eglInitialize(c->base.display, &major, &minor)) {
		fprintf(stderr, "failed to initialize display\n");
		return -1;
	}

	if (!eglChooseConfig(c->base.display,
			     config_attribs, &config, 1, &count) ||
	    count == 0) {
		fprintf(stderr, "eglChooseConfig() failed\n");
		return -1;
	}

	eglBindAPI(EGL_OPENGL_API);
	c->base.context = eglCreateContext(c->base.display,
					   config, EGL_NO_CONTEXT, NULL);
	if (c->base.context == NULL) {
		fprintf(stderr, "failed to create context\n");
		return -1;
	}

	if (!eglMakeCurrent(c->base.display, EGL_NO_SURFACE,
			    EGL_NO_SURFACE, c->base.context)) {
		fprintf(stderr, "failed to make context current\n");
		return -1;
	}

	return 0;
}

static void
x11_compositor_present(struct wlsc_compositor *base)
{
	struct x11_compositor *c = (struct x11_compositor *) base;
	struct x11_output *output;
	xcb_dri2_copy_region_cookie_t cookie;
	struct timeval tv;
	uint32_t msec;

	glFlush();

	wl_list_for_each(output, &c->base.output_list, base.link) {
		cookie = xcb_dri2_copy_region_unchecked(c->conn,
							output->window,
							output->region,
							XCB_DRI2_ATTACHMENT_BUFFER_FRONT_LEFT,
							XCB_DRI2_ATTACHMENT_BUFFER_BACK_LEFT);
		free(xcb_dri2_copy_region_reply(c->conn, cookie, NULL));
	}	

	gettimeofday(&tv, NULL);
	msec = tv.tv_sec * 1000 + tv.tv_usec / 1000;
	wlsc_compositor_finish_frame(&c->base, msec);
}

static void
x11_output_set_wm_protocols(struct x11_output *output)
{
	xcb_atom_t list[1];
	struct x11_compositor *c =
		(struct x11_compositor *) output->base.compositor;

	list[0] = c->atom.wm_delete_window;
	xcb_change_property (c->conn, 
			     XCB_PROP_MODE_REPLACE,
			     output->window,
			     c->atom.wm_protocols,
			     XCB_ATOM_ATOM,
			     32,
			     ARRAY_SIZE(list),
			     list);
}

struct wm_normal_hints {
    	uint32_t flags;
	uint32_t pad[4];
	int32_t min_width, min_height;
	int32_t max_width, max_height;
    	int32_t width_inc, height_inc;
    	int32_t min_aspect_x, min_aspect_y;
    	int32_t max_aspect_x, max_aspect_y;
	int32_t base_width, base_height;
	int32_t win_gravity;
};

#define WM_NORMAL_HINTS_MIN_SIZE	16
#define WM_NORMAL_HINTS_MAX_SIZE	32

static int
x11_compositor_create_output(struct x11_compositor *c, int width, int height)
{
	static const char name[] = "Wayland Compositor";
	struct x11_output *output;
	xcb_dri2_dri2_buffer_t *buffers;
	xcb_dri2_get_buffers_reply_t *reply;
	xcb_dri2_get_buffers_cookie_t cookie;
	xcb_screen_iterator_t iter;
	xcb_rectangle_t rectangle;
	struct wm_normal_hints normal_hints;
	unsigned int attachments[] =
		{ XCB_DRI2_ATTACHMENT_BUFFER_BACK_LEFT};
	uint32_t mask = XCB_CW_EVENT_MASK | XCB_CW_CURSOR;
	uint32_t values[2] = { 
		XCB_EVENT_MASK_KEY_PRESS |
		XCB_EVENT_MASK_KEY_RELEASE |
		XCB_EVENT_MASK_BUTTON_PRESS |
		XCB_EVENT_MASK_BUTTON_RELEASE |
		XCB_EVENT_MASK_POINTER_MOTION |
		XCB_EVENT_MASK_EXPOSURE |
		XCB_EVENT_MASK_STRUCTURE_NOTIFY |
		XCB_EVENT_MASK_ENTER_WINDOW |
		XCB_EVENT_MASK_LEAVE_WINDOW,
		0
	};

	EGLint attribs[] = {
		EGL_WIDTH,		0,
		EGL_HEIGHT,		0,
		EGL_IMAGE_STRIDE_MESA,	0,
		EGL_IMAGE_FORMAT_MESA,	EGL_IMAGE_FORMAT_ARGB8888_MESA,
		EGL_NONE
	};

	output = malloc(sizeof *output);
	if (output == NULL)
		return -1;

	memset(output, 0, sizeof *output);
	wlsc_output_init(&output->base, &c->base, 0, 0, width, height);

	values[1] = c->null_cursor;
	output->window = xcb_generate_id(c->conn);
	iter = xcb_setup_roots_iterator(xcb_get_setup(c->conn));
	xcb_create_window(c->conn,
			  XCB_COPY_FROM_PARENT,
			  output->window,
			  iter.data->root,
			  0, 0,
			  width, height,
			  0,
			  XCB_WINDOW_CLASS_INPUT_OUTPUT,
			  iter.data->root_visual,
			  mask, values);

	/* Don't resize me. */
	memset(&normal_hints, 0, sizeof normal_hints);
	normal_hints.flags =
		WM_NORMAL_HINTS_MAX_SIZE | WM_NORMAL_HINTS_MIN_SIZE;
	normal_hints.min_width = width;
	normal_hints.min_height = height;
	normal_hints.max_width = width;
	normal_hints.max_height = height;
	xcb_change_property (c->conn, XCB_PROP_MODE_REPLACE, output->window,
			     c->atom.wm_normal_hints,
			     c->atom.wm_size_hints, 32,
			     sizeof normal_hints / 4,
			     (uint8_t *) &normal_hints);

        xcb_map_window(c->conn, output->window);

	/* Set window name.  Don't bother with non-EWMH WMs. */
	xcb_change_property(c->conn, XCB_PROP_MODE_REPLACE, output->window,
			    c->atom.net_wm_name, c->atom.utf8_string, 8,
			    strlen(name), name);

	rectangle.x = 0;
	rectangle.y = 0;
	rectangle.width = width;
	rectangle.height = height;
	output->region = xcb_generate_id(c->conn);
	xcb_xfixes_create_region(c->conn, output->region, 1, &rectangle);

	xcb_dri2_create_drawable (c->conn, output->window);

	x11_output_set_wm_protocols(output);

	cookie = xcb_dri2_get_buffers_unchecked (c->conn,
						 output->window,
						 1, 1, attachments);
	reply = xcb_dri2_get_buffers_reply (c->conn, cookie, NULL);
	if (reply == NULL)
		return -1;
	buffers = xcb_dri2_get_buffers_buffers (reply);
	if (buffers == NULL)
		return -1;

	if (reply->count != 1) {
		fprintf(stderr,
			"got wrong number of buffers (%d)\n", reply->count);
		return -1;
	}

	attribs[1] = reply->width;
	attribs[3] = reply->height;
	attribs[5] = buffers[0].pitch / 4;
	free(reply);

	output->image =
		eglCreateImageKHR(c->base.display, c->base.context,
				  EGL_DRM_IMAGE_MESA,
				  (EGLClientBuffer) buffers[0].name,
				  attribs);

	glGenRenderbuffers(1, &output->rbo);
	glBindRenderbuffer(GL_RENDERBUFFER, output->rbo);

	glEGLImageTargetRenderbufferStorageOES(GL_RENDERBUFFER,
					       output->image);

	glFramebufferRenderbuffer(GL_FRAMEBUFFER,
				  GL_COLOR_ATTACHMENT0,
				  GL_RENDERBUFFER,
				  output->rbo);

	wl_list_insert(c->base.output_list.prev, &output->base.link);

	return 0;
}

static void
idle_repaint(void *data)
{
	struct x11_output *output = data;
	struct x11_compositor *c =
		(struct x11_compositor *) output->base.compositor;
	xcb_xfixes_region_t region;
	xcb_dri2_copy_region_cookie_t cookie;
	
	if (output->damage_count <= ARRAY_SIZE(output->damage)) {
		region = xcb_generate_id(c->conn);
		xcb_xfixes_create_region(c->conn, region,
					 output->damage_count, output->damage);
	} else {
		region = output->region;
	}

	cookie = xcb_dri2_copy_region_unchecked(c->conn,
						output->window,
						region,
						XCB_DRI2_ATTACHMENT_BUFFER_FRONT_LEFT,
						XCB_DRI2_ATTACHMENT_BUFFER_BACK_LEFT);

	if (region != output->region)
		xcb_xfixes_destroy_region(c->conn, region);

	free(xcb_dri2_copy_region_reply(c->conn, cookie, NULL));
	output->damage_count = 0;
}

static struct x11_output *
x11_compositor_find_output(struct x11_compositor *c, xcb_window_t window)
{
	struct x11_output *output;

	wl_list_for_each(output, &c->base.output_list, base.link) {
		if (output->window == window)
			return output;
	}

	return NULL;
}

static void
x11_compositor_handle_event(int fd, uint32_t mask, void *data)
{
	struct x11_compositor *c = data;
	struct x11_output *output;
        xcb_generic_event_t *event;
	struct wl_event_loop *loop;
	xcb_client_message_event_t *client_message;
	xcb_motion_notify_event_t *motion_notify;
	xcb_key_press_event_t *key_press;
	xcb_button_press_event_t *button_press;
	xcb_expose_event_t *expose;
	xcb_rectangle_t *r;
	xcb_atom_t atom;

	loop = wl_display_get_event_loop(c->base.wl_display);
        while (event = xcb_poll_for_event (c->conn), event != NULL) {
		switch (event->response_type & ~0x80) {

		case XCB_KEY_PRESS:
			key_press = (xcb_key_press_event_t *) event;
			notify_key(c->base.input_device,
				   key_press->detail - 8, 1);
			break;
		case XCB_KEY_RELEASE:
			key_press = (xcb_key_press_event_t *) event;
			notify_key(c->base.input_device,
				   key_press->detail - 8, 0);
			break;
		case XCB_BUTTON_PRESS:
			button_press = (xcb_button_press_event_t *) event;
			notify_button(c->base.input_device,
				      button_press->detail + BTN_LEFT - 1, 1);
			break;
		case XCB_BUTTON_RELEASE:
			button_press = (xcb_button_press_event_t *) event;
			notify_button(c->base.input_device,
				      button_press->detail + BTN_LEFT - 1, 0);
			break;

		case XCB_MOTION_NOTIFY:
			motion_notify = (xcb_motion_notify_event_t *) event;
			notify_motion(c->base.input_device,
				      motion_notify->event_x,
				      motion_notify->event_y);
			break;

		case XCB_EXPOSE:
			expose = (xcb_expose_event_t *) event;
			output = x11_compositor_find_output(c, expose->window);
			if (output->damage_count == 0)
				wl_event_loop_add_idle(loop,
						       idle_repaint, output);

			r = &output->damage[output->damage_count++];
			if (output->damage_count > 16)
				break;
			r->x = expose->x;
			r->y = expose->y;
			r->width = expose->width;
			r->height = expose->height;
			break;

		case XCB_ENTER_NOTIFY:
			c->base.focus = 1;
			wlsc_compositor_schedule_repaint(&c->base);
			break;

		case XCB_LEAVE_NOTIFY:
			c->base.focus = 0;
			wlsc_compositor_schedule_repaint(&c->base);
			break;

		case XCB_CLIENT_MESSAGE:
			client_message = (xcb_client_message_event_t *) event;
			atom = client_message->data.data32[0];
			if (atom == c->atom.wm_delete_window)
				exit(1);
			break;
		default: 

			break;
		}

		free (event);
        }
}

#define F(field) offsetof(struct x11_compositor, field)

static void
x11_compositor_get_resources(struct x11_compositor *c)
{
	static const struct { const char *name; int offset; } atoms[] = {
		{ "WM_PROTOCOLS",	F(atom.wm_protocols) },
		{ "WM_NORMAL_HINTS",	F(atom.wm_normal_hints) },
		{ "WM_SIZE_HINTS",	F(atom.wm_size_hints) },
		{ "WM_DELETE_WINDOW",	F(atom.wm_delete_window) },
		{ "_NET_WM_NAME",	F(atom.net_wm_name) },
		{ "UTF8_STRING",	F(atom.utf8_string) },
	};

	xcb_intern_atom_cookie_t cookies[ARRAY_SIZE(atoms)];
	xcb_intern_atom_reply_t *reply;
	xcb_pixmap_t pixmap;
	xcb_gc_t gc;
	int i;
	uint8_t data[] = { 0, 0, 0, 0 };

	for (i = 0; i < ARRAY_SIZE(atoms); i++)
		cookies[i] = xcb_intern_atom (c->conn, 0,
					      strlen(atoms[i].name),
					      atoms[i].name);

	for (i = 0; i < ARRAY_SIZE(atoms); i++) {
		reply = xcb_intern_atom_reply (c->conn, cookies[i], NULL);
		*(xcb_atom_t *) ((char *) c + atoms[i].offset) = reply->atom;
		free(reply);
	}

	pixmap = xcb_generate_id(c->conn);
	gc = xcb_generate_id(c->conn);
	xcb_create_pixmap(c->conn, 1, pixmap, c->screen->root, 1, 1);
	xcb_create_gc(c->conn, gc, pixmap, 0, NULL);
	xcb_put_image(c->conn, XCB_IMAGE_FORMAT_XY_PIXMAP,
		      pixmap, gc, 1, 1, 0, 0, 0, 32, sizeof data, data);
	c->null_cursor = xcb_generate_id(c->conn);
	xcb_create_cursor (c->conn, c->null_cursor,
			   pixmap, pixmap, 0, 0, 0,  0, 0, 0,  1, 1);
	xcb_free_gc(c->conn, gc);
	xcb_free_pixmap(c->conn, pixmap);
}

struct wlsc_compositor *
x11_compositor_create(struct wl_display *display)
{
	struct x11_compositor *c;
	struct wl_event_loop *loop;
	xcb_screen_iterator_t s;

	c = malloc(sizeof *c);
	if (c == NULL)
		return NULL;

	memset(c, 0, sizeof *c);
	c->conn = xcb_connect(0, 0);

	if (xcb_connection_has_error(c->conn))
		return NULL;

	s = xcb_setup_roots_iterator(xcb_get_setup(c->conn));
	c->screen = s.data;

	x11_compositor_get_resources(c);

	x11_compositor_init_egl(c);

	/* Can't init base class until we have a current egl context */
	if (wlsc_compositor_init(&c->base, display) < 0)
		return NULL;

	x11_compositor_create_output(c, 1024, 640);

	x11_input_create(c);

	loop = wl_display_get_event_loop(c->base.wl_display);

	c->xcb_source =
		wl_event_loop_add_fd(loop, xcb_get_file_descriptor(c->conn),
				     WL_EVENT_READABLE,
				     x11_compositor_handle_event, c);

	c->base.present = x11_compositor_present;

	return &c->base;
}
