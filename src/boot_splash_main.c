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
#include <sys/stat.h>
#include <linux/vt.h>
#include <linux/kd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <cairo/cairo.h>

#include "loginui.h"

#define TAKING_OVER "/run/singularity/compositor-taking-over"
#define MASTER_RELEASED "/run/singularity/boot-splash-master-released"
#define FIRST_FRAME "/run/singularity/compositor-first-frame"
#define LOADER_SURFACE "/boot/efi/EFI/atom/surface.bin"

struct fb {
    uint32_t fb_id;
    uint32_t handle;
    uint32_t pitch;
    uint64_t size;
    uint8_t *map;
    cairo_surface_t *surface;
};

static int drm_fd = -1;
static uint32_t crtc_id, connector_id;
static drmModeModeInfo mode;
static struct fb fbs[2];
static int front = 0;
static volatile sig_atomic_t want_quit = 0;
static cairo_surface_t *logo = NULL;

static int tty_fd = -1;
static volatile sig_atomic_t vt_active = 1;
static volatile sig_atomic_t vt_event = 0;

static void on_vt_release(int s) { (void)s; vt_event = 1; }
static void on_vt_acquire(int s) { (void)s; vt_event = 2; }
static void on_signal(int s) { (void)s; want_quit = 1; }

static double boot_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_BOOTTIME, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static bool exists(const char *p) { return access(p, F_OK) == 0; }

static bool try_logo_file(const char *path) {
    if (access(path, R_OK) != 0) return false;
    logo = loginui_load_image(path, LOGINUI_BRAND_LOGO_SIZE * 2, LOGINUI_BRAND_LOGO_SIZE * 2);
    return logo != NULL;
}

static void load_logo(void) {
    const char *env = getenv("SINGULARITY_SPLASH_LOGO");
    if (env && try_logo_file(env)) return;
    try_logo_file(LOGINUI_BRAND_LOGO_PATH);
}

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

static bool drm_is_fallback = false;
static char drm_path[64];

static bool fallback_driver(int fd) {
    drmVersionPtr v = drmGetVersion(fd);
    if (!v) return true;
    bool fb = v->name && (strcmp(v->name, "simpledrm") == 0 || strcmp(v->name, "efidrm") == 0 ||
                          strcmp(v->name, "vesadrm") == 0 || strcmp(v->name, "ofdrm") == 0);
    drmFreeVersion(v);
    return fb;
}

static bool open_card(const char *path) {
    drm_fd = open(path, O_RDWR | O_CLOEXEC);
    if (drm_fd < 0) return false;
    drmSetMaster(drm_fd);
    if (pick_output()) {
        snprintf(drm_path, sizeof drm_path, "%s", path);
        drm_is_fallback = fallback_driver(drm_fd);
        return true;
    }
    close(drm_fd);
    drm_fd = -1;
    return false;
}

static bool real_card_ready(void) {
    char path[32];
    for (int i = 0; i < 8; i++) {
        snprintf(path, sizeof path, "/dev/dri/card%d", i);
        if (strcmp(path, drm_path) == 0) continue;
        int fd = open(path, O_RDWR | O_CLOEXEC);
        if (fd < 0) continue;
        bool real = !fallback_driver(fd);
        bool connected = false;
        drmModeRes *res = real ? drmModeGetResources(fd) : NULL;
        if (res) {
            for (int c = 0; c < res->count_connectors && !connected; c++) {
                drmModeConnector *conn = drmModeGetConnector(fd, res->connectors[c]);
                if (!conn) continue;
                connected = conn->connection == DRM_MODE_CONNECTED && conn->count_modes > 0;
                drmModeFreeConnector(conn);
            }
            drmModeFreeResources(res);
        }
        close(fd);
        if (connected) return true;
    }
    return false;
}

static bool wait_for_card(const char *dev, double timeout, double fallback_after) {
    double t0 = boot_seconds();
    for (;;) {
        if (dev) {
            if (open_card(dev)) return true;
        } else {
            char path[32];
            for (int i = 0; i < 8 && drm_fd < 0; i++) {
                snprintf(path, sizeof path, "/dev/dri/card%d", i);
                int fd = open(path, O_RDWR | O_CLOEXEC);
                if (fd < 0) continue;
                bool fb = fallback_driver(fd);
                close(fd);
                if (fb && boot_seconds() - t0 < fallback_after) continue;
                open_card(path);
            }
            if (drm_fd >= 0) return true;
        }
        if (boot_seconds() - t0 > timeout) return false;
        usleep(50000);
    }
}

static void destroy_fb(struct fb *f) {
    if (f->surface) cairo_surface_destroy(f->surface);
    if (f->map && f->map != MAP_FAILED) munmap(f->map, f->size);
    if (f->fb_id) drmModeRmFB(drm_fd, f->fb_id);
    if (f->handle) {
        struct drm_mode_destroy_dumb dreq = { .handle = f->handle };
        drmIoctl(drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
    }
    memset(f, 0, sizeof *f);
}

static void render(struct fb *f, double progress) {
    cairo_t *cr = cairo_create(f->surface);
    loginui_render_brand_progress(cr, mode.hdisplay, mode.vdisplay, 1.0, logo, progress, 1.0);
    cairo_destroy(cr);
    cairo_surface_flush(f->surface);
}

static void flush_fb(struct fb *f) {
    drmModeClip clip = { 0, 0, mode.hdisplay, mode.vdisplay };
    drmModeDirtyFB(drm_fd, f->fb_id, &clip, 1);
}

static void on_flip(int fd, unsigned seq, unsigned tv_sec, unsigned tv_usec, void *data) {
    (void)fd; (void)seq; (void)tv_sec; (void)tv_usec;
    *((int *)data) = 0;
}

static void write_loader_surface(const char *path, struct fb *f) {
    LoginUiBrandLayout l;
    loginui_brand_layout(mode.hdisplay, mode.vdisplay, 1.0, &l);
    int w = mode.hdisplay, h = mode.vdisplay;
    double down = l.bar_y + l.bar_h - h / 2.0;
    double up = h / 2.0 - l.logo_y;
    int half = (int)(down > up ? down : up) + 2;
    int lw = ((int)l.logo_size + 3) & ~1;
    if (lw < (int)l.bar_w + 2) lw = ((int)l.bar_w + 3) & ~1;
    int lh = 2 * half;
    if (lw > w || lh > h) return;
    int ox = (w - lw) / 2, oy = (h - lh) / 2;
    size_t n = 8 + (size_t)lw * lh * 4;
    uint8_t *buf = malloc(n);
    if (!buf) return;
    uint32_t hdr[2] = { (uint32_t)lw, (uint32_t)lh };
    memcpy(buf, hdr, 8);
    for (int y = 0; y < lh; y++)
        memcpy(buf + 8 + (size_t)y * lw * 4, f->map + (size_t)(oy + y) * f->pitch + (size_t)ox * 4, (size_t)lw * 4);
    for (size_t i = 8 + 3; i < n; i += 4) buf[i] = 0;

    bool same = false;
    FILE *old = fopen(path, "rb");
    if (old) {
        uint8_t *cur = malloc(n + 1);
        if (cur) {
            size_t r = fread(cur, 1, n + 1, old);
            same = r == n && memcmp(cur, buf, n) == 0;
            free(cur);
        }
        fclose(old);
    }
    if (!same) {
        char tmp[512];
        snprintf(tmp, sizeof tmp, "%s.new", path);
        FILE *out = fopen(tmp, "wb");
        if (out) {
            bool ok = fwrite(buf, 1, n, out) == n;
            ok = fflush(out) == 0 && ok;
            fsync(fileno(out));
            fclose(out);
            if (ok && rename(tmp, path) == 0)
                fprintf(stderr, "boot-splash: loader surface updated (%dx%d)\n", lw, lh);
            else
                unlink(tmp);
        }
    }
    free(buf);
}

int main(int argc, char **argv) {
    const char *dev = NULL;
    const char *surface_path = LOADER_SURFACE;
    double wait_seconds = 10.0;
    double safety_seconds = 60.0;
    double hold_seconds = 20.0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--device") == 0 && i + 1 < argc) dev = argv[++i];
        else if (strcmp(argv[i], "--wait-seconds") == 0 && i + 1 < argc) wait_seconds = atof(argv[++i]);
        else if (strcmp(argv[i], "--max-life") == 0 && i + 1 < argc) safety_seconds = atof(argv[++i]);
        else if (strcmp(argv[i], "--loader-surface") == 0 && i + 1 < argc) surface_path = argv[++i];
        else if (strcmp(argv[i], "--log-file") == 0 && i + 1 < argc) {
            if (freopen(argv[++i], "a", stderr)) setvbuf(stderr, NULL, _IOLBF, 0);
        }
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    load_logo();

    if (!wait_for_card(dev, wait_seconds, 3.0)) {
        fprintf(stderr, "boot-splash: no ready DRM output on %s within %.1fs\n", dev ? dev : "any card", wait_seconds);
        return 1;
    }
    if (create_fb(&fbs[0], mode.hdisplay, mode.vdisplay) < 0 ||
        create_fb(&fbs[1], mode.hdisplay, mode.vdisplay) < 0) {
        fprintf(stderr, "boot-splash: cannot create framebuffers\n");
        return 1;
    }

    double start_t = boot_seconds();
    double progress = 0.0;
    render(&fbs[0], 0.0);
    if (drmModeSetCrtc(drm_fd, crtc_id, fbs[0].fb_id, 0, 0, &connector_id, 1, &mode) < 0) {
        fprintf(stderr, "boot-splash: setcrtc failed: %s\n", strerror(errno));
        return 1;
    }
    flush_fb(&fbs[0]);
    front = 0;
    fprintf(stderr, "boot-splash: showing on %s connector %u at %ux%u, %.2fs after boot%s\n",
            drm_path, connector_id, mode.hdisplay, mode.vdisplay, start_t, drm_is_fallback ? " (firmware framebuffer)" : "");
    if (surface_path[0] && !drm_is_fallback) write_loader_surface(surface_path, &fbs[0]);

    tty_fd = open("/dev/tty1", O_RDWR | O_CLOEXEC);
    if (tty_fd >= 0) {
        ioctl(tty_fd, KDSETMODE, KD_GRAPHICS);
        signal(SIGUSR1, on_vt_release);
        signal(SIGUSR2, on_vt_acquire);
        struct vt_mode vtm = { .mode = VT_PROCESS, .waitv = 0,
                               .relsig = SIGUSR1, .acqsig = SIGUSR2 };
        ioctl(tty_fd, VT_SETMODE, &vtm);
    }

    int flip_pending = 0;
    drmEventContext ev = { .version = 2, .page_flip_handler = on_flip };
    double last_write = 0.0;
    double last_probe = 0.0;
    bool handed_over = false;
    bool switch_card = false;

    while (!want_quit) {
        if (vt_event == 1) {
            vt_event = 0;
            drmDropMaster(drm_fd);
            vt_active = 0;
            if (tty_fd >= 0) ioctl(tty_fd, VT_RELDISP, 1);
        } else if (vt_event == 2) {
            vt_event = 0;
            if (tty_fd >= 0) ioctl(tty_fd, VT_RELDISP, VT_ACKACQ);
            drmSetMaster(drm_fd);
            drmModeSetCrtc(drm_fd, crtc_id, fbs[front].fb_id, 0, 0, &connector_id, 1, &mode);
            flush_fb(&fbs[front]);
            vt_active = 1;
        }

        double t = boot_seconds();
        progress = loginui_progress_curve(0.0, 0.45, t - start_t, 4.0);
        if (t - last_write > 0.1) {
            loginui_progress_write(LOGINUI_PROGRESS_PATH, progress);
            last_write = t;
        }
        if (exists(TAKING_OVER)) { handed_over = true; break; }
        if (t - start_t > safety_seconds) break;

        if (!dev && drm_is_fallback && t - last_probe > 0.25) {
            last_probe = t;
            if (real_card_ready()) switch_card = true;
        }
        if (switch_card) {
            switch_card = false;
            destroy_fb(&fbs[0]);
            destroy_fb(&fbs[1]);
            drmDropMaster(drm_fd);
            close(drm_fd);
            drm_fd = -1;
            drm_path[0] = 0;
            if (!wait_for_card(dev, 5.0, 0.0) ||
                create_fb(&fbs[0], mode.hdisplay, mode.vdisplay) < 0 ||
                create_fb(&fbs[1], mode.hdisplay, mode.vdisplay) < 0) {
                fprintf(stderr, "boot-splash: lost the display while switching cards\n");
                break;
            }
            render(&fbs[0], progress);
            drmModeSetCrtc(drm_fd, crtc_id, fbs[0].fb_id, 0, 0, &connector_id, 1, &mode);
            flush_fb(&fbs[0]);
            front = 0;
            fprintf(stderr, "boot-splash: moved to %s connector %u at %ux%u, %.2fs after boot%s\n",
                    drm_path, connector_id, mode.hdisplay, mode.vdisplay, t, drm_is_fallback ? " (firmware framebuffer)" : "");
            if (surface_path[0] && !drm_is_fallback) write_loader_surface(surface_path, &fbs[0]);
            continue;
        }

        if (!vt_active) {
            struct pollfd pfd = { tty_fd, 0, 0 };
            poll(&pfd, 1, 100);
            continue;
        }

        int back = front ^ 1;
        render(&fbs[back], progress);
        flip_pending = 1;
        if (drmModePageFlip(drm_fd, crtc_id, fbs[back].fb_id, DRM_MODE_PAGE_FLIP_EVENT, &flip_pending) < 0) {
            flip_pending = 0;
            if (errno == EACCES || errno == EPERM) break;
            if (errno == ENODEV || errno == ENOENT || errno == EIO) { switch_card = true; continue; }
            usleep(16000);
            continue;
        }
        flush_fb(&fbs[back]);
        while (flip_pending && !want_quit) {
            struct pollfd pfd = { drm_fd, POLLIN, 0 };
            if (poll(&pfd, 1, 100) > 0 && (pfd.revents & POLLIN)) drmHandleEvent(drm_fd, &ev);
            else break;
        }
        front = back;
    }

    loginui_progress_write(LOGINUI_PROGRESS_PATH, progress);
    drmDropMaster(drm_fd);
    int fd = open(MASTER_RELEASED, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd >= 0) close(fd);
    fprintf(stderr, "boot-splash: released the display at %.2f (%s)\n", progress,
            handed_over ? "compositor taking over" : "no compositor");

    double h0 = boot_seconds();
    while (!exists(FIRST_FRAME) && boot_seconds() - h0 < hold_seconds && !want_quit) {
        double t = boot_seconds();
        progress = loginui_progress_curve(0.0, 0.45, t - start_t, 4.0);
        render(&fbs[front], progress);
        flush_fb(&fbs[front]);
        loginui_progress_write(LOGINUI_PROGRESS_PATH, progress);
        usleep(50000);
    }

    if (tty_fd >= 0) {
        struct vt_mode vtm = { .mode = VT_AUTO };
        ioctl(tty_fd, VT_SETMODE, &vtm);
        close(tty_fd);
    }
    return 0;
}
