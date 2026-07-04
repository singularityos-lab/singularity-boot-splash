#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <poll.h>
#include <signal.h>
#include <ctype.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/vt.h>
#include <linux/kd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <cairo/cairo.h>

#include "loginui.h"

struct fb {
    uint32_t fb_id;
    uint32_t handle;
    uint32_t pitch;
    uint64_t size;
    uint8_t *map;
    cairo_surface_t *surface;
};

static int drm_fd = -1;
static drmModeCrtc *saved_crtc = NULL;
static uint32_t crtc_id, connector_id;
static drmModeModeInfo mode;
static struct fb fbs[2];
static int front = 0;
static volatile sig_atomic_t want_quit = 0;
static cairo_surface_t *logo = NULL;

/* VT handover so Ctrl+Alt+Fn can switch to a console and back. */
static int tty_fd = -1;
static volatile sig_atomic_t vt_active = 1;   /* are we the foreground VT */
static volatile sig_atomic_t vt_event = 0;    /* 1 = release request, 2 = acquired */

static void on_vt_release(int s) { (void)s; vt_event = 1; }
static void on_vt_acquire(int s) { (void)s; vt_event = 2; }

static double mono_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void on_signal(int s) { (void)s; want_quit = 1; }

/* ── logo (same source as the Wayland splash) ───────────────────────────── */

static bool try_logo_file(const char *name) {
    if (!name || !name[0]) return false;
    const char *tpl[] = {
        "/opt/local/share/icons/hicolor/scalable/apps/%s.svg",
        "/usr/local/share/icons/hicolor/scalable/apps/%s.svg",
        "/usr/share/icons/hicolor/scalable/apps/%s.svg",
        "/usr/share/pixmaps/%s.svg",
        "/usr/share/pixmaps/%s.png",
        "/usr/share/icons/hicolor/256x256/apps/%s.png",
        NULL
    };
    for (int i = 0; tpl[i]; i++) {
        char p[1024];
        snprintf(p, sizeof p, tpl[i], name);
        if (access(p, R_OK) == 0) { logo = loginui_load_image(p, -1, 256); if (logo) return true; }
    }
    return false;
}

static void os_release_value(const char *key, char *out, size_t n) {
    out[0] = '\0';
    FILE *f = fopen("/etc/os-release", "r");
    if (!f) return;
    char line[512];
    size_t klen = strlen(key);
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, key, klen) != 0 || line[klen] != '=') continue;
        char *v = line + klen + 1;
        while (*v == '"' || *v == '\'') v++;
        char *end = v + strlen(v);
        while (end > v && (end[-1] == '\n' || end[-1] == '"' || end[-1] == '\'' || isspace((unsigned char)end[-1]))) end--;
        *end = '\0';
        snprintf(out, n, "%s", v);
        break;
    }
    fclose(f);
}

static void load_logo(void) {
    char logo_name[128], id[128];
    os_release_value("LOGO", logo_name, sizeof logo_name);
    os_release_value("ID", id, sizeof id);
    if (try_logo_file(logo_name)) return;
    if (try_logo_file(id)) return;
    try_logo_file("emblem-singularity");
}

/* ── KMS ────────────────────────────────────────────────────────────────── */

static int create_fb(struct fb *f, uint32_t w, uint32_t h) {
    struct drm_mode_create_dumb creq = { .width = w, .height = h, .bpp = 32 };
    if (drmIoctl(drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq) < 0) return -1;
    f->handle = creq.handle; f->pitch = creq.pitch; f->size = creq.size;

    if (drmModeAddFB(drm_fd, w, h, 24, 32, f->pitch, f->handle, &f->fb_id) < 0) return -1;

    struct drm_mode_map_dumb mreq = { .handle = f->handle };
    if (drmIoctl(drm_fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq) < 0) return -1;
    f->map = mmap(0, f->size, PROT_READ | PROT_WRITE, MAP_SHARED, drm_fd, mreq.offset);
    if (f->map == MAP_FAILED) return -1;

    f->surface = cairo_image_surface_create_for_data(f->map, CAIRO_FORMAT_RGB24,
                                                     (int)w, (int)h, (int)f->pitch);
    return 0;
}

static bool pick_output(void) {
    drmModeRes *res = drmModeGetResources(drm_fd);
    if (!res) return false;
    bool ok = false;

    for (int i = 0; i < res->count_connectors && !ok; i++) {
        drmModeConnector *conn = drmModeGetConnector(drm_fd, res->connectors[i]);
        if (!conn) continue;
        if (conn->connection == DRM_MODE_CONNECTED && conn->count_modes > 0) {
            connector_id = conn->connector_id;
            mode = conn->modes[0];
            for (int m = 0; m < conn->count_modes; m++)
                if (conn->modes[m].type & DRM_MODE_TYPE_PREFERRED) { mode = conn->modes[m]; break; }

            drmModeEncoder *enc = conn->encoder_id ? drmModeGetEncoder(drm_fd, conn->encoder_id) : NULL;
            if (enc && enc->crtc_id) {
                crtc_id = enc->crtc_id; ok = true;
            } else {
                for (int e = 0; e < conn->count_encoders && !ok; e++) {
                    drmModeEncoder *en = drmModeGetEncoder(drm_fd, conn->encoders[e]);
                    if (!en) continue;
                    for (int c = 0; c < res->count_crtcs; c++) {
                        if (en->possible_crtcs & (1 << c)) { crtc_id = res->crtcs[c]; ok = true; break; }
                    }
                    drmModeFreeEncoder(en);
                }
            }
            if (enc) drmModeFreeEncoder(enc);
        }
        drmModeFreeConnector(conn);
    }
    drmModeFreeResources(res);
    return ok;
}

/* Wait until the DRM device exists AND exposes a connected output with a mode,
 * then become master. Early in boot the card node can be absent and the
 * connector unprobed (i915 finishes its modeset a couple of seconds in); drawing
 * before that is what paints the black/glitch spots before the greeter. Retrying
 * until a real output is ready means the first frame we set is a stable one. */
static bool wait_for_card(const char *dev, double timeout) {
    double t0 = mono_seconds();
    for (;;) {
        if (drm_fd < 0) {
            drm_fd = open(dev, O_RDWR | O_CLOEXEC);
            if (drm_fd >= 0) drmSetMaster(drm_fd);
        }
        if (drm_fd >= 0 && pick_output()) return true;
        if (mono_seconds() - t0 > timeout) return false;
        usleep(100000); /* 100 ms */
    }
}

static void render(struct fb *f, double alpha) {
    cairo_t *cr = cairo_create(f->surface);
    cairo_push_group(cr);
    loginui_render_splash(cr, mode.hdisplay, mode.vdisplay, NULL, logo, mono_seconds());
    cairo_pop_group_to_source(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_paint_with_alpha(cr, alpha);
    cairo_destroy(cr);
    cairo_surface_flush(f->surface);
}

static void on_flip(int fd, unsigned seq, unsigned tv_sec, unsigned tv_usec, void *data) {
    (void)fd; (void)seq; (void)tv_sec; (void)tv_usec;
    *((int *)data) = 0;
}

int main(int argc, char **argv) {
    const char *dev = "/dev/dri/card0";
    double max_seconds = 0.0;
    double wait_seconds = 8.0;    /* wait this long for the DRM output to be ready */
    double handoff_seconds = 1.5; /* hold the last frame this long after dropping master */
    double safety_seconds = 30.0; /* hard cap: never hold the DRM master longer than this */
    char ready_path[512] = "";
    const char *rt = getenv("XDG_RUNTIME_DIR");
    if (rt && rt[0]) snprintf(ready_path, sizeof ready_path, "%s/singularity-shell-ready", rt);
    /* The boot-splash runs as root in sysinit, so XDG_RUNTIME_DIR is usually
     * unset and the shell-ready file (written by the user session after login)
     * never applies to the GREETER handoff. Watch a fixed, root-readable path the
     * greeter touches when it comes up, so boot -> greeter hands off adaptively. */
    char greeter_path[512] = "/run/singularity/greeter-ready";

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--device") == 0 && i + 1 < argc) dev = argv[++i];
        else if (strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) max_seconds = atof(argv[++i]);
        else if (strcmp(argv[i], "--wait-seconds") == 0 && i + 1 < argc) wait_seconds = atof(argv[++i]);
        else if (strcmp(argv[i], "--handoff") == 0 && i + 1 < argc) handoff_seconds = atof(argv[++i]);
        else if (strcmp(argv[i], "--max-life") == 0 && i + 1 < argc) safety_seconds = atof(argv[++i]);
        else if (strcmp(argv[i], "--greeter-ready") == 0 && i + 1 < argc) snprintf(greeter_path, sizeof greeter_path, "%s", argv[++i]);
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    load_logo();

    if (!wait_for_card(dev, wait_seconds)) {
        fprintf(stderr, "boot-splash: no ready DRM output on %s within %.1fs\n", dev, wait_seconds);
        return 1;
    }
    if (create_fb(&fbs[0], mode.hdisplay, mode.vdisplay) < 0 ||
        create_fb(&fbs[1], mode.hdisplay, mode.vdisplay) < 0) {
        fprintf(stderr, "boot-splash: cannot create framebuffers\n"); return 1;
    }

    saved_crtc = drmModeGetCrtc(drm_fd, crtc_id);

    render(&fbs[0], 1.0);
    if (drmModeSetCrtc(drm_fd, crtc_id, fbs[0].fb_id, 0, 0, &connector_id, 1, &mode) < 0) {
        fprintf(stderr, "boot-splash: setcrtc failed: %s\n", strerror(errno)); return 1;
    }
    front = 0;

    /* Own VT1 in process mode so Ctrl+Alt+Fn releases us (drop DRM master,
     * reveal the console) and Ctrl+Alt+F1 brings us back (regain it, redraw). */
    tty_fd = open("/dev/tty1", O_RDWR | O_CLOEXEC);
    if (tty_fd >= 0) {
        ioctl(tty_fd, VT_ACTIVATE, 1);
        ioctl(tty_fd, VT_WAITACTIVE, 1);
        ioctl(tty_fd, KDSETMODE, KD_GRAPHICS);
        signal(SIGUSR1, on_vt_release);
        signal(SIGUSR2, on_vt_acquire);
        struct vt_mode vtm = { .mode = VT_PROCESS, .waitv = 0,
                               .relsig = SIGUSR1, .acqsig = SIGUSR2 };
        ioctl(tty_fd, VT_SETMODE, &vtm);
    }

    double start_t = mono_seconds();
    double alpha = 1.0;
    int flip_pending = 0;
    drmEventContext ev = { .version = 2, .page_flip_handler = on_flip };

    while (!want_quit) {
        if (vt_event == 1) {            /* switching away: let the console show */
            vt_event = 0;
            drmDropMaster(drm_fd);
            vt_active = 0;
            if (tty_fd >= 0) ioctl(tty_fd, VT_RELDISP, 1);
        } else if (vt_event == 2) {     /* switched back: reclaim and redraw */
            vt_event = 0;
            if (tty_fd >= 0) ioctl(tty_fd, VT_RELDISP, VT_ACKACQ);
            drmSetMaster(drm_fd);
            drmModeSetCrtc(drm_fd, crtc_id, fbs[front].fb_id, 0, 0, &connector_id, 1, &mode);
            vt_active = 1;
        }
        if (!vt_active) {               /* console is foreground; idle, wait for a signal */
            struct pollfd pfd = { tty_fd, 0, 0 };
            poll(&pfd, 1, 200);
            continue;
        }

        int back = front ^ 1;
        render(&fbs[back], alpha);

        flip_pending = 1;
        int flip_rc = drmModePageFlip(drm_fd, crtc_id, fbs[back].fb_id, DRM_MODE_PAGE_FLIP_EVENT, &flip_pending);
        if (flip_rc < 0) {
            flip_pending = 0;
            /* Losing the master (EACCES/EPERM) means the compositor, the greeter's
             * or the session's, has taken the display: that IS the handoff signal.
             * Exit so it can paint. This is what makes removing greetd's
             * Conflicts= safe even before the shell-ready sentinel exists. */
            if (flip_rc == -EACCES || flip_rc == -EPERM || errno == EACCES || errno == EPERM)
                want_quit = 1;
            else
                usleep(16000);
        }
        while (flip_pending && !want_quit) {
            struct pollfd pfd = { drm_fd, POLLIN, 0 };
            if (poll(&pfd, 1, 100) > 0 && (pfd.revents & POLLIN)) drmHandleEvent(drm_fd, &ev);
            else break;
        }
        front = back;

        double t = mono_seconds();
        bool ready = (ready_path[0] && access(ready_path, F_OK) == 0) ||
                     (greeter_path[0] && access(greeter_path, F_OK) == 0);
        bool timed_out = max_seconds > 0.0 && (t - start_t) > max_seconds;
        bool safety = (t - start_t) > safety_seconds;   /* never hold the master forever */
        if (ready || timed_out || safety) break;   /* hold the last full frame; the compositor cross-fades in over it */
    }

    /* Graceful handoff (plymouth-style): drop the master so the compositor can
     * take it via seatd, then hold our last frame on the CRTC for a short grace
     * window before freeing the framebuffers, so the compositor paints over a
     * live image instead of a blanked CRTC and there is no black gap. Kept brief
     * because under this init a conflicting greeter waits for us to exit. */
    drmDropMaster(drm_fd);
    for (double h0 = mono_seconds(); mono_seconds() - h0 < handoff_seconds;)
        usleep(50000);
    if (saved_crtc) drmModeFreeCrtc(saved_crtc);
    if (tty_fd >= 0) {
        struct vt_mode vtm = { .mode = VT_AUTO };
        ioctl(tty_fd, VT_SETMODE, &vtm);
        close(tty_fd);
    }
    return 0;
}
