/*
 * myfreeze.c - Wayland screen freeze overlay
 *
 * Captures the screen directly via wlr-screencopy (no grim needed),
 * displays it as a pixel-perfect fullscreen overlay using wlr-layer-shell,
 * and handles HiDPI/fractional scaling via wp-fractional-scale +
 * wp-viewporter so the freeze is always sharp on any display.
 *
 * How it works:
 *   1. Connect to Wayland, bind all required globals via the registry
 *   2. Query the output's fractional scale factor (wp-fractional-scale)
 *   3. Capture the screen into a wl_shm buffer via zwlr_screencopy_frame_v1
 *      - Buffer size = logical_size * scale (actual physical pixels)
 *      - No temp PNG file, no grim, pixels go straight into shared memory
 *   4. Create a zwlr_layer_surface_v1 on the OVERLAY layer stretched
 *      fullscreen with exclusive_zone -1 (covers everything)
 *   5. Attach a wp_viewport to the surface and set the destination size
 *      to logical pixels so the compositor maps the buffer 1:1 to screen
 *      pixels (pixel-perfect on HiDPI)
 *   6. Set an empty input region so pointer/keyboard events pass through
 *      to whatever runs on top (slurp)
 *   7. Register a wl_surface.frame callback - fires when the compositor
 *      has actually rendered the frame to screen (not just queued it)
 *   8. Print PID to stdout so the caller knows the freeze is visible,
 *      then block reading stdin
 *   9. When stdin closes: destroy layer surface, commit null surface,
 *      two roundtrips so compositor fully redraws the real desktop, exit
 *
 * Usage:
 *   myfreeze
 *
 * Driven via bash coproc in myshot.sh:
 *   coproc FREEZE { myfreeze; }
 *   read FREEZE_PID <&"${FREEZE[0]}"  # blocks until frame is on screen
 *   slurp ...                          # runs on top of frozen screen
 *   exec {FREEZE[1]}>&-               # close pipe -> myfreeze exits
 *   wait "$FREEZE_PID"                # wait for overlay to be cleared
 *
 * Dependencies:
 *   - wayland-client
 *   - zwlr-screencopy-unstable-v1  (wlr-protocols)
 *   - zwlr-layer-shell-unstable-v1 (wlr-protocols)
 *   - wp-fractional-scale-v1       (wayland-protocols/staging)
 *   - wp-viewporter                (wayland-protocols/stable)
 *   - xdg-shell                    (wayland-protocols/stable)
 */

#define _GNU_SOURCE /* memfd_create, MFD_CLOEXEC */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <wayland-client.h>

/* Generated from protocol XMLs by wayland-scanner (see Makefile) */
#include "fractional-scale-v1-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "wlr-screencopy-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

/* -------------------------------------------------------------------------
 * Wayland global objects
 * Populated by registry_handle_global() when the compositor advertises them.
 * ------------------------------------------------------------------------- */
static struct wl_display                     *g_display        = NULL; // compositor connection
static struct wl_registry                    *g_registry       = NULL; // global object list
static struct wl_compositor                  *g_compositor     = NULL; // surface/region factory
static struct wl_shm                         *g_shm            = NULL; // shared memory factory
static struct wl_output                      *g_output         = NULL; // first monitor found
static struct zwlr_screencopy_manager_v1     *g_screencopy_mgr = NULL; // screen capture protocol
static struct zwlr_layer_shell_v1            *g_layer_shell    = NULL; // layer surface protocol
static struct wp_fractional_scale_manager_v1 *g_frac_mgr       = NULL; // fractional scale protocol
static struct wp_viewporter                  *g_viewporter     = NULL; // viewport/scaling protocol

/* -------------------------------------------------------------------------
 * Output geometry
 * We need the logical size of the output to set the viewport destination
 * and layer surface size correctly.
 * ------------------------------------------------------------------------- */
static int g_output_width  = 0; // logical width  in compositor coordinates
static int g_output_height = 0; // logical height in compositor coordinates

static void output_handle_geometry(void             *data,
                                   struct wl_output *output,
                                   int32_t           x,
                                   int32_t           y,
                                   int32_t           phys_w,
                                   int32_t           phys_h,
                                   int32_t           subpixel,
                                   const char       *make,
                                   const char       *model,
                                   int32_t           transform) {
    (void)data;
    (void)output;
    (void)x;
    (void)y;
    (void)phys_w;
    (void)phys_h;
    (void)subpixel;
    (void)make;
    (void)model;
    (void)transform;
}

static void output_handle_mode(void             *data,
                               struct wl_output *output,
                               uint32_t          flags,
                               int32_t           width,
                               int32_t           height,
                               int32_t           refresh) {
    (void)data;
    (void)output;
    (void)flags;
    (void)refresh;
    /* mode gives physical pixel size; we'll use it as fallback if
     * no fractional scale is advertised (i.e. scale = 1) */
    if (g_output_width == 0) {
        g_output_width  = width;
        g_output_height = height;
    }
}

static void output_handle_done(void *data, struct wl_output *output) {
    (void)data;
    (void)output;
}

static void output_handle_scale(void *data, struct wl_output *output, int32_t factor) {
    (void)data;
    (void)output;
    (void)factor;
}

static void output_handle_name(void *data, struct wl_output *output, const char *name) {
    (void)data;
    (void)output;
    (void)name;
}

static void output_handle_description(void *data, struct wl_output *output, const char *desc) {
    (void)data;
    (void)output;
    (void)desc;
}

static const struct wl_output_listener output_listener = {
    .geometry    = output_handle_geometry,
    .mode        = output_handle_mode,
    .done        = output_handle_done,
    .scale       = output_handle_scale,
    .name        = output_handle_name,
    .description = output_handle_description,
};

/* -------------------------------------------------------------------------
 * Registry listener - binds globals we care about
 * ------------------------------------------------------------------------- */

/* Called for every global the compositor advertises. We bind only what
 * we need and ignore everything else. */
static void registry_handle_global(
    void *data, struct wl_registry *reg, uint32_t name, const char *iface, uint32_t version) {
    (void)data;
    if (strcmp(iface, wl_compositor_interface.name) == 0) {
        g_compositor = wl_registry_bind(reg, name, &wl_compositor_interface, 4);
    } else if (strcmp(iface, wl_shm_interface.name) == 0) {
        g_shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
    } else if (strcmp(iface, wl_output_interface.name) == 0 && g_output == NULL) {
        /* Bind the first output and listen for its mode/geometry */
        g_output = wl_registry_bind(reg, name, &wl_output_interface, 4);
        wl_output_add_listener(g_output, &output_listener, NULL);
    } else if (strcmp(iface, zwlr_screencopy_manager_v1_interface.name) == 0) {
        g_screencopy_mgr = wl_registry_bind(reg, name, &zwlr_screencopy_manager_v1_interface, 3);
    } else if (strcmp(iface, zwlr_layer_shell_v1_interface.name) == 0) {
        /* Cap at version 4 - that's all we use */
        g_layer_shell =
            wl_registry_bind(reg, name, &zwlr_layer_shell_v1_interface, version < 4 ? version : 4);
    } else if (strcmp(iface, wp_fractional_scale_manager_v1_interface.name) == 0) {
        g_frac_mgr = wl_registry_bind(reg, name, &wp_fractional_scale_manager_v1_interface, 1);
    } else if (strcmp(iface, wp_viewporter_interface.name) == 0) {
        g_viewporter = wl_registry_bind(reg, name, &wp_viewporter_interface, 1);
    }
}

/* Called when a compositor global is removed (e.g. monitor unplugged).
 * We don't handle hot-unplug since myfreeze is a one-shot tool. */
static void registry_handle_global_remove(void *data, struct wl_registry *reg, uint32_t name) {
    (void)data;
    (void)reg;
    (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global        = registry_handle_global,
    .global_remove = registry_handle_global_remove,
};

/* -------------------------------------------------------------------------
 * Fractional scale listener
 * wp-fractional-scale tells us the exact scale factor for our surface
 * as a fixed-point value: scale = preferred_scale / 120
 * e.g. 1x = 120, 1.5x = 180, 2x = 240
 * ------------------------------------------------------------------------- */
static uint32_t g_scale_120 = 120; /* default 1x (120/120) until compositor tells us */

static void
frac_scale_preferred(void *data, struct wp_fractional_scale_v1 *frac, uint32_t scale_times_120) {
    (void)data;
    (void)frac;
    g_scale_120 = scale_times_120; /* store for buffer size calculation */
}

static const struct wp_fractional_scale_v1_listener frac_scale_listener = {
    .preferred_scale = frac_scale_preferred,
};

/* -------------------------------------------------------------------------
 * Layer surface listener
 * ------------------------------------------------------------------------- */

/* Set to 1 once the compositor sends configure - we must ack before
 * attaching a buffer. */
static int g_configured = 0;

/* Compositor sends configure to assign our layer surface a size.
 * Since we use anchor-all + exclusive_zone=-1 we go fullscreen;
 * we don't care about the w/h, just ack it. */
static void layer_surface_configure(
    void *data, struct zwlr_layer_surface_v1 *surf, uint32_t serial, uint32_t w, uint32_t h) {
    (void)data;
    (void)w;
    (void)h;
    zwlr_layer_surface_v1_ack_configure(surf, serial);
    g_configured = 1;
}

static void layer_surface_closed(void *data, struct zwlr_layer_surface_v1 *surf) {
    (void)data;
    (void)surf;
}

static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
    .configure = layer_surface_configure,
    .closed    = layer_surface_closed,
};

/* -------------------------------------------------------------------------
 * Frame callback listener
 * ------------------------------------------------------------------------- */

/* Set to 1 when the compositor confirms it rendered our surface to screen.
 * Guarantees the freeze is visible before we signal the shell to run slurp.
 * Using a frame callback is more reliable than any sleep-based approach. */
static int g_frame_done = 0;

static void frame_callback_done(void *data, struct wl_callback *cb, uint32_t time) {
    (void)data;
    (void)time;
    g_frame_done = 1;
    wl_callback_destroy(cb);
}

static const struct wl_callback_listener frame_listener = {
    .done = frame_callback_done,
};

/* -------------------------------------------------------------------------
 * Screencopy listener
 * zwlr_screencopy_frame_v1 tells us what buffer format/size it needs,
 * then signals ready when the capture is complete.
 * ------------------------------------------------------------------------- */

typedef struct {
    /* Buffer info filled in by the 'buffer' event */
    uint32_t format; /* wl_shm format (e.g. WL_SHM_FORMAT_ARGB8888) */
    uint32_t width;  /* buffer width  in physical pixels */
    uint32_t height; /* buffer height in physical pixels */
    uint32_t stride; /* bytes per row */

    /* Shared memory backing */
    int    fd;       /* memfd file descriptor */
    void  *shm_data; /* mmap'd pointer to pixel data */
    size_t shm_size; /* total size in bytes */

    /* wl_shm objects */
    struct wl_shm_pool *pool;
    struct wl_buffer   *buffer;

    /* State flags */
    int buffer_ready; /* set to 1 by the 'buffer' event */
    int copy_done;    /* set to 1 by the 'ready' event (capture complete) */
    int copy_failed;  /* set to 1 by the 'failed' event */
} ScreencopyFrame;

/* Called by the compositor to tell us what buffer it wants us to provide.
 * We allocate a wl_shm buffer of the requested size/format here. */
static void screencopy_buffer(void                            *data,
                              struct zwlr_screencopy_frame_v1 *frame,
                              uint32_t                         format,
                              uint32_t                         width,
                              uint32_t                         height,
                              uint32_t                         stride) {
    ScreencopyFrame *sf = data;
    sf->format          = format;
    sf->width           = width;
    sf->height          = height;
    sf->stride          = stride;

    sf->shm_size = (size_t)stride * height;

    /* Allocate anonymous shared memory for the capture buffer */
    sf->fd = memfd_create("myfreeze-screencopy", MFD_CLOEXEC);
    if (sf->fd < 0 || ftruncate(sf->fd, (off_t)sf->shm_size) < 0) {
        sf->copy_failed = 1;
        return;
    }

    sf->shm_data = mmap(NULL, sf->shm_size, PROT_READ | PROT_WRITE, MAP_SHARED, sf->fd, 0);
    if (sf->shm_data == MAP_FAILED) {
        close(sf->fd);
        sf->copy_failed = 1;
        return;
    }

    /* Create wl_shm_pool and wl_buffer backed by our memfd */
    sf->pool   = wl_shm_create_pool(g_shm, sf->fd, (int32_t)sf->shm_size);
    sf->buffer = wl_shm_pool_create_buffer(
        sf->pool, 0, (int32_t)width, (int32_t)height, (int32_t)stride, format);
    wl_shm_pool_destroy(sf->pool);

    /* Tell the screencopy frame to copy into our buffer */
    zwlr_screencopy_frame_v1_copy(frame, sf->buffer);
    sf->buffer_ready = 1;
}

/* Called when the compositor has finished copying the screen into our buffer.
 * The pixel data is now valid and ready to display. */
static void screencopy_ready(void                            *data,
                             struct zwlr_screencopy_frame_v1 *frame,
                             uint32_t                         tv_sec_hi,
                             uint32_t                         tv_sec_lo,
                             uint32_t                         tv_nsec) {
    (void)frame;
    (void)tv_sec_hi;
    (void)tv_sec_lo;
    (void)tv_nsec;
    ((ScreencopyFrame *)data)->copy_done = 1;
}

/* Called when the compositor can't perform the capture (e.g. output gone) */
static void screencopy_failed(void *data, struct zwlr_screencopy_frame_v1 *frame) {
    (void)frame;
    ((ScreencopyFrame *)data)->copy_failed = 1;
}

/* Unused but required by the listener struct */
static void screencopy_flags(void *data, struct zwlr_screencopy_frame_v1 *frame, uint32_t flags) {
    (void)data;
    (void)frame;
    (void)flags;
}

static void screencopy_damage(void                            *data,
                              struct zwlr_screencopy_frame_v1 *frame,
                              uint32_t                         x,
                              uint32_t                         y,
                              uint32_t                         width,
                              uint32_t                         height) {
    (void)data;
    (void)frame;
    (void)x;
    (void)y;
    (void)width;
    (void)height;
}

static void screencopy_linux_dmabuf(void                            *data,
                                    struct zwlr_screencopy_frame_v1 *frame,
                                    uint32_t                         format,
                                    uint32_t                         width,
                                    uint32_t                         height) {
    (void)data;
    (void)frame;
    (void)format;
    (void)width;
    (void)height;
}

static void screencopy_buffer_done(void *data, struct zwlr_screencopy_frame_v1 *frame) {
    (void)data;
    (void)frame;
}

static const struct zwlr_screencopy_frame_v1_listener screencopy_listener = {
    .buffer       = screencopy_buffer,
    .flags        = screencopy_flags,
    .ready        = screencopy_ready,
    .failed       = screencopy_failed,
    .damage       = screencopy_damage,
    .linux_dmabuf = screencopy_linux_dmabuf,
    .buffer_done  = screencopy_buffer_done,
};

/* -------------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------------- */
int main(void) {
    /* --- Connect to Wayland and collect globals ------------------------ */
    g_display = wl_display_connect(NULL);
    if (!g_display) {
        fprintf(stderr, "myfreeze: cannot connect to Wayland\n");
        return 1;
    }

    g_registry = wl_display_get_registry(g_display);
    wl_registry_add_listener(g_registry, &registry_listener, NULL);
    wl_display_roundtrip(g_display); /* populate globals + output listener */
    wl_display_roundtrip(g_display); /* flush output mode/geometry events */

    /* Verify we got everything we need */
    if (!g_compositor || !g_shm || !g_layer_shell || !g_screencopy_mgr) {
        fprintf(stderr,
                "myfreeze: missing required Wayland protocols\n"
                "  need: wl_compositor, wl_shm, "
                "zwlr_layer_shell_v1, zwlr_screencopy_manager_v1\n");
        return 1;
    }

    /* wp-fractional-scale and wp-viewporter are optional (not all
     * compositors support them). Without them we still work correctly
     * at integer scales; the overlay may be slightly blurry on fractional
     * scaled displays but still fully functional. */
    int has_hires = (g_frac_mgr != NULL && g_viewporter != NULL);

    /* --- Get fractional scale for our surface -------------------------- */
    /* We create a temporary surface just to get the preferred scale.
     * The compositor fires the preferred_scale event in response to
     * wp_fractional_scale_v1 being attached to a surface. */
    uint32_t scale_120 = 120; /* default: 1x scale */

    if (has_hires) {
        struct wl_surface             *probe_surf = wl_compositor_create_surface(g_compositor);
        struct wp_fractional_scale_v1 *frac =
            wp_fractional_scale_manager_v1_get_fractional_scale(g_frac_mgr, probe_surf);
        wp_fractional_scale_v1_add_listener(frac, &frac_scale_listener, NULL);
        wl_surface_commit(probe_surf);
        wl_display_roundtrip(g_display); /* get preferred_scale event */
        scale_120 = g_scale_120;
        wp_fractional_scale_v1_destroy(frac);
        wl_surface_destroy(probe_surf);
    }

    /* Physical pixel dimensions = logical size * (scale / 120) */
    int buf_width  = (int)((g_output_width * scale_120 + 60) / 120);
    int buf_height = (int)((g_output_height * scale_120 + 60) / 120);

    /* Fallback: if output size wasn't reported, use safe defaults */
    if (buf_width <= 0 || buf_height <= 0) {
        buf_width  = g_output_width > 0 ? g_output_width : 1920;
        buf_height = g_output_height > 0 ? g_output_height : 1080;
    }

    /* --- Capture screen via wlr-screencopy ----------------------------- */
    ScreencopyFrame sf = {0};

    /* Request a capture of the entire output */
    struct zwlr_screencopy_frame_v1 *copy_frame =
        zwlr_screencopy_manager_v1_capture_output(g_screencopy_mgr,
                                                  0, /* overlay_cursor: 0 = don't include cursor */
                                                  g_output);
    zwlr_screencopy_frame_v1_add_listener(copy_frame, &screencopy_listener, &sf);

    /* Pump events until the capture completes (or fails) */
    while (!sf.copy_done && !sf.copy_failed)
        wl_display_dispatch(g_display);

    zwlr_screencopy_frame_v1_destroy(copy_frame);

    if (sf.copy_failed || !sf.buffer) {
        fprintf(stderr, "myfreeze: screencopy failed\n");
        return 1;
    }

    /* --- Create layer surface ------------------------------------------ */
    struct wl_surface *surface = wl_compositor_create_surface(g_compositor);

    /* Attach fractional scale listener so the compositor can notify us
     * of scale changes while the overlay is active */
    if (has_hires) {
        struct wp_fractional_scale_v1 *frac =
            wp_fractional_scale_manager_v1_get_fractional_scale(g_frac_mgr, surface);
        wp_fractional_scale_v1_add_listener(frac, &frac_scale_listener, NULL);
    }

    /* Request OVERLAY layer: above all windows, below the cursor */
    struct zwlr_layer_surface_v1 *layer_surf = zwlr_layer_shell_v1_get_layer_surface(
        g_layer_shell, surface, g_output, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "myfreeze");

    /* size=0,0 + all anchors + exclusive_zone=-1 -> stretch to full output */
    zwlr_layer_surface_v1_set_size(layer_surf, 0, 0);
    zwlr_layer_surface_v1_set_anchor(
        layer_surf,
        ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
            ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
    zwlr_layer_surface_v1_set_exclusive_zone(layer_surf, -1);
    zwlr_layer_surface_v1_add_listener(layer_surf, &layer_surface_listener, NULL);
    wl_surface_commit(surface);

    /* Pump events until compositor sends configure (assigns the size) */
    while (!g_configured)
        wl_display_dispatch(g_display);

    /* --- Set up viewport for pixel-perfect HiDPI rendering ------------ */
    /* wp_viewport lets us tell the compositor: "this buffer is buf_width x
     * buf_height physical pixels, display it as g_output_width x
     * g_output_height logical pixels" - resulting in a 1:1 pixel mapping
     * with no blurring on fractional scaled displays. */
    if (has_hires) {
        struct wp_viewport *viewport = wp_viewporter_get_viewport(g_viewporter, surface);
        /* destination = logical size of the output */
        wp_viewport_set_destination(viewport, g_output_width, g_output_height);
        /* We don't destroy viewport here - it must live as long as surface */
    }

    /* --- Attach the captured buffer and make the overlay visible ------- */
    wl_surface_attach(surface, sf.buffer, 0, 0);
    wl_surface_damage_buffer(surface, 0, 0, (int32_t)sf.width, (int32_t)sf.height);

    /* Empty input region: our overlay won't steal any pointer/keyboard
     * events, so slurp running on top gets full input control */
    struct wl_region *empty = wl_compositor_create_region(g_compositor);
    wl_surface_set_input_region(surface, empty);
    wl_region_destroy(empty);

    /* Register frame callback BEFORE commit - fired when the compositor
     * actually presents this frame to the display (not just queues it) */
    struct wl_callback *frame_cb = wl_surface_frame(surface);
    wl_callback_add_listener(frame_cb, &frame_listener, NULL);
    wl_surface_commit(surface);
    wl_display_flush(g_display);

    /* Block until compositor confirms the frozen frame is on screen */
    while (!g_frame_done)
        wl_display_dispatch(g_display);

    /* --- Signal caller ------------------------------------------------- */
    /* Print PID so the shell script knows the freeze is visible and can
     * proceed to launch slurp. Then block on stdin - the shell holds
     * the write end of a coproc pipe; closing it is our unfreeze signal. */
    printf("%d\n", getpid());
    fflush(stdout);

    char buf[1];
    while (read(STDIN_FILENO, buf, 1) > 0)
        wl_display_dispatch_pending(g_display); /* keep compositor alive */

    /* --- Teardown ------------------------------------------------------ */
    /* Destroy layer surface, then commit a null buffer so the compositor
     * gets a clean "surface gone" signal and redraws the real desktop.
     * Without this double-roundtrip the screen can stay stuck frozen. */
    zwlr_layer_surface_v1_destroy(layer_surf);
    wl_surface_attach(surface, NULL, 0, 0);
    wl_surface_commit(surface);
    wl_display_roundtrip(g_display); /* flush destroy + null commit */
    wl_display_roundtrip(g_display); /* ensure compositor processed removal */

    wl_surface_destroy(surface);
    wl_buffer_destroy(sf.buffer);
    munmap(sf.shm_data, sf.shm_size);
    close(sf.fd);
    wl_registry_destroy(g_registry);
    wl_display_roundtrip(g_display);
    wl_display_disconnect(g_display);
    return 0;
}
