/*
 * rv — Reading Viewer
 * Vertical image stitching viewer for Manhwa/Webtoon reading.
 * Driven by CLI args + IPC (unix socket).
 *
 * Usage: rv [options] [image1 image2 ...]
 *   --scroll-speed <px>       normal scroll (default: 80)
 *   --fast-scroll-speed <px>  fast scroll (default: 600)
 *   --fullscreen              start fullscreen
 *   --title <str>             window title
 *   --sock <path>             custom socket path (default: /tmp/rv-<pid>.sock)
 *   --bind <key>=<action>     keybind (repeatable). e.g. --bind e=scroll_down
 *
 * Actions: scroll_down, scroll_up, fast_scroll_down, fast_scroll_up,
 *          page_down, page_up, top, bottom, zoom_in, zoom_out, zoom_reset,
 *          fullscreen, quit, pan_left, pan_right
 *
 * Key names: a-z, 0-9, space, escape, up, down, left, right, end,
 *            plus, minus, equals
 */

#include <SDL.h>
#include <SDL_image.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>

/* ---------- constants ---------- */

#define MAX_IMAGES      4096
#define IPC_BUF_SIZE    4096
#define SCROLL_SMOOTH   0.15
#define ZOOM_STEP       0.1
#define MIN_ZOOM        0.1
#define MAX_ZOOM        5.0
#define SOCK_PATH_MAX   (sizeof(((struct sockaddr_un *)0)->sun_path))
#define MAX_BINDS       64
#define TEX_BUFFER      3    /* screens above/below viewport to keep textures */
#define LOAD_QUEUE_SIZE 256

/* ---------- actions ---------- */

enum action {
    ACT_NONE = 0,
    ACT_SCROLL_DOWN,
    ACT_SCROLL_UP,
    ACT_FAST_SCROLL_DOWN,
    ACT_FAST_SCROLL_UP,
    ACT_PAGE_DOWN,
    ACT_PAGE_UP,
    ACT_TOP,
    ACT_BOTTOM,
    ACT_ZOOM_IN,
    ACT_ZOOM_OUT,
    ACT_ZOOM_RESET,
    ACT_FULLSCREEN,
    ACT_QUIT,
    ACT_PAN_LEFT,
    ACT_PAN_RIGHT,
};

static const struct { const char *name; enum action act; } action_map[] = {
    { "scroll_down",      ACT_SCROLL_DOWN },
    { "scroll_up",        ACT_SCROLL_UP },
    { "fast_scroll_down", ACT_FAST_SCROLL_DOWN },
    { "fast_scroll_up",   ACT_FAST_SCROLL_UP },
    { "page_down",        ACT_PAGE_DOWN },
    { "page_up",          ACT_PAGE_UP },
    { "top",              ACT_TOP },
    { "bottom",           ACT_BOTTOM },
    { "zoom_in",          ACT_ZOOM_IN },
    { "zoom_out",         ACT_ZOOM_OUT },
    { "zoom_reset",       ACT_ZOOM_RESET },
    { "fullscreen",       ACT_FULLSCREEN },
    { "quit",             ACT_QUIT },
    { "pan_left",         ACT_PAN_LEFT },
    { "pan_right",        ACT_PAN_RIGHT },
    { NULL, ACT_NONE },
};

static enum action parse_action(const char *name)
{
    for (int i = 0; action_map[i].name; i++)
        if (strcmp(action_map[i].name, name) == 0)
            return action_map[i].act;
    return ACT_NONE;
}

/* ---------- key name lookup ---------- */

static const struct { const char *name; SDL_Keycode key; } key_map[] = {
    { "space",  SDLK_SPACE },  { "escape", SDLK_ESCAPE },
    { "up",     SDLK_UP },     { "down",   SDLK_DOWN },
    { "left",   SDLK_LEFT },   { "right",  SDLK_RIGHT },
    { "end",    SDLK_END },    { "home",   SDLK_HOME },
    { "plus",   SDLK_PLUS },   { "minus",  SDLK_MINUS },
    { "equals", SDLK_EQUALS },
    { NULL, 0 },
};

static SDL_Keycode parse_key(const char *name)
{
    if (name[0] && !name[1])
        return (SDL_Keycode)name[0];

    for (int i = 0; key_map[i].name; i++)
        if (strcmp(key_map[i].name, name) == 0)
            return key_map[i].key;

    return SDLK_UNKNOWN;
}

/* ---------- keybind entry ---------- */

typedef struct {
    SDL_Keycode    key;
    enum action    act;
} Keybind;

/* ---------- image entry ---------- */

enum img_state {
    IMG_EMPTY,       /* path set, not yet queued */
    IMG_LOADING,     /* worker thread decoding */
    IMG_DECODED,     /* surface ready, no texture yet */
    IMG_TEXTURED,    /* texture created, on GPU */
    IMG_ERROR,       /* failed to load */
};

typedef struct {
    char           *path;
    int             w, h;        /* pixel dimensions (set after decode) */
    int             y_offset;    /* vertical offset in strip */
    enum img_state  state;
    SDL_Surface    *surf;        /* CPU pixel data (set by worker, consumed by main) */
    SDL_Texture    *tex;         /* GPU texture (created/destroyed by main thread) */
} Image;

/* ---------- background loader ---------- */

typedef struct {
    int idx;                     /* index into V.images */
    char path[4096];
} LoadRequest;

/* ---------- viewer state ---------- */

static struct {
    SDL_Window      *win;
    SDL_Renderer    *ren;
    int              win_w, win_h;

    Image            images[MAX_IMAGES];
    int              count;
    int              total_h;    /* total strip height in pixels */

    double           scroll_y;
    double           scroll_target;
    double           pan_x;          /* horizontal pan offset */
    double           pan_x_target;
    double           zoom;
    int              scroll_speed;
    int              fast_scroll_speed;

    int              fullscreen;
    char             title[256];
    char             sock_path[SOCK_PATH_MAX];
    int              sock_fd;

    int              quit;
    int              needs_layout;

    Keybind          binds[MAX_BINDS];
    int              nbinds;

    /* mouse drag */
    int              dragging;
    int              drag_x, drag_y;

    /* background loader */
    pthread_t        loader_thread;
    pthread_mutex_t  loader_mtx;
    pthread_cond_t   loader_cond;
    LoadRequest      load_queue[LOAD_QUEUE_SIZE];
    int              lq_head, lq_tail, lq_count;
    int              loader_quit;
} V;

/* ---------- keybind helpers ---------- */

static void bind_key(SDL_Keycode key, enum action act)
{
    for (int i = 0; i < V.nbinds; i++) {
        if (V.binds[i].key == key) {
            V.binds[i].act = act;
            return;
        }
    }
    if (V.nbinds < MAX_BINDS) {
        V.binds[V.nbinds].key = key;
        V.binds[V.nbinds].act = act;
        V.nbinds++;
    }
}

static int parse_bind_arg(const char *arg)
{
    char buf[256];
    snprintf(buf, sizeof(buf), "%s", arg);
    char *eq = strchr(buf, '=');
    if (!eq) return -1;
    *eq = '\0';

    SDL_Keycode key = parse_key(buf);
    if (key == SDLK_UNKNOWN) {
        fprintf(stderr, "rv: unknown key: %s\n", buf);
        return -1;
    }
    enum action act = parse_action(eq + 1);
    if (act == ACT_NONE) {
        fprintf(stderr, "rv: unknown action: %s\n", eq + 1);
        return -1;
    }
    bind_key(key, act);
    return 0;
}

static void default_binds(void)
{
    bind_key(SDLK_j,       ACT_SCROLL_DOWN);
    bind_key(SDLK_DOWN,    ACT_SCROLL_DOWN);
    bind_key(SDLK_k,       ACT_SCROLL_UP);
    bind_key(SDLK_UP,      ACT_SCROLL_UP);
    bind_key(SDLK_SPACE,   ACT_PAGE_DOWN);
    bind_key(SDLK_b,       ACT_PAGE_UP);
    bind_key(SDLK_g,       ACT_TOP);
    bind_key(SDLK_END,     ACT_BOTTOM);
    bind_key(SDLK_PLUS,    ACT_ZOOM_IN);
    bind_key(SDLK_EQUALS,  ACT_ZOOM_IN);
    bind_key(SDLK_MINUS,   ACT_ZOOM_OUT);
    bind_key(SDLK_0,       ACT_ZOOM_RESET);
    bind_key(SDLK_f,       ACT_FULLSCREEN);
    bind_key(SDLK_q,       ACT_QUIT);
    bind_key(SDLK_ESCAPE,  ACT_QUIT);
    bind_key(SDLK_h,       ACT_PAN_LEFT);
    bind_key(SDLK_LEFT,    ACT_PAN_LEFT);
    bind_key(SDLK_l,       ACT_PAN_RIGHT);
    bind_key(SDLK_RIGHT,   ACT_PAN_RIGHT);
}

static enum action lookup_bind(SDL_Keycode key)
{
    for (int i = 0; i < V.nbinds; i++)
        if (V.binds[i].key == key)
            return V.binds[i].act;
    return ACT_NONE;
}

/* ---------- forward declarations ---------- */

static void layout(void);
static void render(void);
static void ipc_init(void);
static void ipc_poll(void);
static void ipc_cleanup(void);
static void ipc_handle(const char *cmd);
static void clamp_scroll(void);
static void clamp_pan(void);
static void manage_textures(void);

/* ---------- background loader thread ---------- */

static void loader_enqueue(int idx, const char *path)
{
    pthread_mutex_lock(&V.loader_mtx);
    if (V.lq_count < LOAD_QUEUE_SIZE) {
        LoadRequest *req = &V.load_queue[V.lq_tail];
        req->idx = idx;
        snprintf(req->path, sizeof(req->path), "%s", path);
        V.lq_tail = (V.lq_tail + 1) % LOAD_QUEUE_SIZE;
        V.lq_count++;
        pthread_cond_signal(&V.loader_cond);
    }
    pthread_mutex_unlock(&V.loader_mtx);
}

static void *loader_thread_fn(void *arg)
{
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&V.loader_mtx);
        while (V.lq_count == 0 && !V.loader_quit)
            pthread_cond_wait(&V.loader_cond, &V.loader_mtx);

        if (V.loader_quit) {
            pthread_mutex_unlock(&V.loader_mtx);
            break;
        }

        LoadRequest req = V.load_queue[V.lq_head];
        V.lq_head = (V.lq_head + 1) % LOAD_QUEUE_SIZE;
        V.lq_count--;
        pthread_mutex_unlock(&V.loader_mtx);

        SDL_Surface *surf = IMG_Load(req.path);
        if (surf) {
            /* convert to RGBA for fast texture upload */
            SDL_Surface *conv = SDL_ConvertSurfaceFormat(surf, SDL_PIXELFORMAT_RGBA32, 0);
            SDL_FreeSurface(surf);
            surf = conv;
        }

        Image *img = &V.images[req.idx];
        if (surf) {
            img->w = surf->w;
            img->h = surf->h;
            img->surf = surf;
            __sync_synchronize();
            img->state = IMG_DECODED;
        } else {
            fprintf(stderr, "rv: failed to load %s: %s\n", req.path, IMG_GetError());
            img->state = IMG_ERROR;
        }
    }
    return NULL;
}

static void loader_init(void)
{
    pthread_mutex_init(&V.loader_mtx, NULL);
    pthread_cond_init(&V.loader_cond, NULL);
    pthread_create(&V.loader_thread, NULL, loader_thread_fn, NULL);
}

static void loader_shutdown(void)
{
    pthread_mutex_lock(&V.loader_mtx);
    V.loader_quit = 1;
    pthread_cond_signal(&V.loader_cond);
    pthread_mutex_unlock(&V.loader_mtx);
    pthread_join(V.loader_thread, NULL);
    pthread_mutex_destroy(&V.loader_mtx);
    pthread_cond_destroy(&V.loader_cond);
}

/* ---------- image management ---------- */

static int add_image(const char *path)
{
    if (V.count >= MAX_IMAGES) {
        fprintf(stderr, "rv: max images (%d) reached\n", MAX_IMAGES);
        return -1;
    }

    int idx = V.count;
    Image *img = &V.images[idx];
    img->path = strdup(path);
    img->w = 0;
    img->h = 0;
    img->y_offset = 0;
    img->state = IMG_LOADING;
    img->surf = NULL;
    img->tex = NULL;
    V.count++;

    loader_enqueue(idx, path);
    return idx;
}

/* ---------- texture streaming ---------- */

static void manage_textures(void)
{
    if (V.count == 0) return;

    /* check for newly decoded images */
    int any_new = 0;
    for (int i = 0; i < V.count; i++) {
        if (V.images[i].state == IMG_DECODED && V.images[i].surf) {
            any_new = 1;
            /* don't create texture yet — that happens in viewport check below */
        }
    }
    if (any_new)
        V.needs_layout = 1;

    if (V.needs_layout)
        layout();

    /* determine visible range in layout coordinates */
    double view_top = V.scroll_y;
    double view_bot = V.scroll_y + V.win_h / V.zoom;
    double buffer = V.win_h * TEX_BUFFER;

    for (int i = 0; i < V.count; i++) {
        Image *img = &V.images[i];
        if (img->state == IMG_LOADING || img->state == IMG_EMPTY || img->state == IMG_ERROR)
            continue;

        double scale = (img->w > 0) ? (double)V.win_w / img->w : 1.0;
        int scaled_h = (int)(img->h * scale);
        double img_top = img->y_offset;
        double img_bot = img->y_offset + scaled_h;

        int near_viewport = (img_bot >= view_top - buffer && img_top <= view_bot + buffer);

        if (near_viewport) {
            /* need texture */
            if (img->state == IMG_DECODED && img->surf) {
                img->tex = SDL_CreateTextureFromSurface(V.ren, img->surf);
                SDL_FreeSurface(img->surf);
                img->surf = NULL;
                if (img->tex)
                    img->state = IMG_TEXTURED;
                else
                    img->state = IMG_ERROR;
            }
        } else {
            /* evict texture to save GPU memory */
            if (img->state == IMG_TEXTURED && img->tex) {
                SDL_DestroyTexture(img->tex);
                img->tex = NULL;
                /* keep dimensions, mark as needing reload */
                img->state = IMG_EMPTY;
            }
        }
    }

    /* re-enqueue evicted images that are now near viewport again */
    for (int i = 0; i < V.count; i++) {
        Image *img = &V.images[i];
        if (img->state != IMG_EMPTY || !img->path)
            continue;

        double scale = (img->w > 0) ? (double)V.win_w / img->w : 1.0;
        int scaled_h = (int)(img->h * scale);
        double img_top = img->y_offset;
        double img_bot = img->y_offset + scaled_h;

        double view_top2 = V.scroll_y;
        double view_bot2 = V.scroll_y + V.win_h / V.zoom;
        double buf2 = V.win_h * (TEX_BUFFER - 1); /* tighter re-enqueue to avoid thrash */

        if (img_bot >= view_top2 - buf2 && img_top <= view_bot2 + buf2) {
            img->state = IMG_LOADING;
            loader_enqueue(i, img->path);
        }
    }
}

/* ---------- layout: compute y offsets ---------- */

static void layout(void)
{
    int y = 0;
    for (int i = 0; i < V.count; i++) {
        V.images[i].y_offset = y;
        int w = V.images[i].w;
        int h = V.images[i].h;
        if (w > 0 && h > 0) {
            double scale = (double)V.win_w / w;
            y += (int)(h * scale);
        } else {
            /* placeholder height for loading images */
            y += V.win_h / 2;
        }
    }
    V.total_h = y;
    V.needs_layout = 0;
}

/* ---------- rendering ---------- */

static int current_image_index(void)
{
    if (V.count == 0) return -1;
    double center = V.scroll_y + (V.win_h / V.zoom) / 2.0;
    for (int i = V.count - 1; i >= 0; i--) {
        if (V.images[i].y_offset <= center)
            return i;
    }
    return 0;
}

static void render(void)
{
    SDL_SetRenderDrawColor(V.ren, 18, 18, 18, 255);
    SDL_RenderClear(V.ren);

    if (V.count == 0) {
        SDL_RenderPresent(V.ren);
        return;
    }

    if (V.needs_layout)
        layout();

    double zy = V.scroll_y * V.zoom;

    for (int i = 0; i < V.count; i++) {
        Image *img = &V.images[i];
        int w = img->w > 0 ? img->w : V.win_w;
        int h = img->h > 0 ? img->h : V.win_h / 2;
        double scale = (double)V.win_w / w;
        int scaled_w = (int)(w * scale * V.zoom);
        int scaled_h = (int)(h * scale * V.zoom);
        int y = (int)(img->y_offset * V.zoom - zy);

        /* cull off-screen */
        if (y + scaled_h < 0 || y > V.win_h)
            continue;

        int x = (V.win_w - scaled_w) / 2 + (int)V.pan_x;

        SDL_Rect dst = { x, y, scaled_w, scaled_h };

        if (img->state == IMG_TEXTURED && img->tex) {
            SDL_RenderCopy(V.ren, img->tex, NULL, &dst);
        } else {
            /* loading placeholder */
            SDL_SetRenderDrawColor(V.ren, 30, 30, 30, 255);
            SDL_RenderFillRect(V.ren, &dst);
            SDL_SetRenderDrawColor(V.ren, 60, 60, 60, 255);
            SDL_RenderDrawRect(V.ren, &dst);

            if (img->state == IMG_ERROR) {
                /* red border for errors */
                SDL_SetRenderDrawColor(V.ren, 180, 40, 40, 255);
                SDL_RenderDrawRect(V.ren, &dst);
            }
        }
    }

    /* progress indicator */
    int cur = current_image_index();
    if (cur >= 0) {
        double pct = 0;
        if (V.total_h > V.win_h / V.zoom)
            pct = V.scroll_y / (V.total_h - V.win_h / V.zoom) * 100.0;
        if (pct > 100) pct = 100;
        if (pct < 0) pct = 0;

        char info[64];
        snprintf(info, sizeof(info), "%d/%d  %.0f%%", cur + 1, V.count, pct);

        /* render text as simple rectangles (no SDL_ttf dependency) */
        /* use window title instead for now */
        char title_buf[512];
        snprintf(title_buf, sizeof(title_buf), "%s — %s", V.title, info);
        SDL_SetWindowTitle(V.win, title_buf);
    }

    SDL_RenderPresent(V.ren);
}

/* ---------- scroll/pan helpers ---------- */

static void clamp_scroll(void)
{
    if (V.scroll_target < 0)
        V.scroll_target = 0;

    double max_scroll = V.total_h - (double)V.win_h / V.zoom;
    if (max_scroll < 0) max_scroll = 0;
    if (V.scroll_target > max_scroll)
        V.scroll_target = max_scroll;
}

static void clamp_pan(void)
{
    double scaled_content_w = V.win_w * V.zoom;
    if (scaled_content_w <= V.win_w) {
        V.pan_x_target = 0;
        return;
    }
    double max_pan = (scaled_content_w - V.win_w) / 2.0;
    if (V.pan_x_target > max_pan) V.pan_x_target = max_pan;
    if (V.pan_x_target < -max_pan) V.pan_x_target = -max_pan;
}

static void smooth_scroll(void)
{
    double diff = V.scroll_target - V.scroll_y;
    if (diff > -0.5 && diff < 0.5)
        V.scroll_y = V.scroll_target;
    else
        V.scroll_y += diff * SCROLL_SMOOTH;

    double pdiff = V.pan_x_target - V.pan_x;
    if (pdiff > -0.5 && pdiff < 0.5)
        V.pan_x = V.pan_x_target;
    else
        V.pan_x += pdiff * SCROLL_SMOOTH;
}

/* ---------- IPC ---------- */

static void ipc_init(void)
{
    V.sock_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (V.sock_fd < 0) {
        perror("rv: socket");
        return;
    }

    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", V.sock_path);

    unlink(V.sock_path);
    if (bind(V.sock_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("rv: bind");
        close(V.sock_fd);
        V.sock_fd = -1;
        return;
    }

    if (listen(V.sock_fd, 4) < 0) {
        perror("rv: listen");
        close(V.sock_fd);
        V.sock_fd = -1;
        return;
    }

    fprintf(stderr, "rv: IPC listening on %s\n", V.sock_path);
}

static void ipc_poll(void)
{
    if (V.sock_fd < 0) return;

    int client = accept(V.sock_fd, NULL, NULL);
    if (client < 0) return;

    char buf[IPC_BUF_SIZE] = {0};
    ssize_t n = read(client, buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';
        if (n > 0 && buf[n-1] == '\n') buf[n-1] = '\0';
        ipc_handle(buf);
    }
    close(client);
}

static void ipc_handle(const char *cmd)
{
    if (strncmp(cmd, "open ", 5) == 0) {
        const char *path = cmd + 5;
        while (*path == ' ') path++;
        if (*path) {
            add_image(path);
            fprintf(stderr, "rv: queued %s (total: %d)\n", path, V.count);
        }
    } else if (strcmp(cmd, "quit") == 0) {
        V.quit = 1;
    } else if (strncmp(cmd, "goto ", 5) == 0) {
        int idx = atoi(cmd + 5);
        if (idx >= 0 && idx < V.count) {
            if (V.needs_layout) layout();
            V.scroll_target = V.images[idx].y_offset;
            clamp_scroll();
        }
    } else if (strncmp(cmd, "scroll ", 7) == 0) {
        int amount = atoi(cmd + 7);
        V.scroll_target += amount;
        clamp_scroll();
    } else {
        fprintf(stderr, "rv: unknown command: %s\n", cmd);
    }
}

static void ipc_cleanup(void)
{
    if (V.sock_fd >= 0) {
        close(V.sock_fd);
        unlink(V.sock_path);
        V.sock_fd = -1;
    }
}

/* ---------- action dispatch ---------- */

static void do_action(enum action act)
{
    switch (act) {
    case ACT_QUIT:
        V.quit = 1;
        break;
    case ACT_SCROLL_DOWN:
        V.scroll_target += V.scroll_speed;
        clamp_scroll();
        break;
    case ACT_SCROLL_UP:
        V.scroll_target -= V.scroll_speed;
        clamp_scroll();
        break;
    case ACT_FAST_SCROLL_DOWN:
        V.scroll_target += V.fast_scroll_speed;
        clamp_scroll();
        break;
    case ACT_FAST_SCROLL_UP:
        V.scroll_target -= V.fast_scroll_speed;
        clamp_scroll();
        break;
    case ACT_PAGE_DOWN:
        V.scroll_target += V.win_h / V.zoom * 0.9;
        clamp_scroll();
        break;
    case ACT_PAGE_UP:
        V.scroll_target -= V.win_h / V.zoom * 0.9;
        clamp_scroll();
        break;
    case ACT_TOP:
        V.scroll_target = 0;
        break;
    case ACT_BOTTOM:
        V.scroll_target = V.total_h;
        clamp_scroll();
        break;
    case ACT_ZOOM_IN:
        V.zoom += ZOOM_STEP;
        if (V.zoom > MAX_ZOOM) V.zoom = MAX_ZOOM;
        V.needs_layout = 1;
        break;
    case ACT_ZOOM_OUT:
        V.zoom -= ZOOM_STEP;
        if (V.zoom < MIN_ZOOM) V.zoom = MIN_ZOOM;
        V.needs_layout = 1;
        clamp_pan();
        break;
    case ACT_ZOOM_RESET:
        V.zoom = 1.0;
        V.pan_x = V.pan_x_target = 0;
        V.needs_layout = 1;
        break;
    case ACT_FULLSCREEN:
        V.fullscreen = !V.fullscreen;
        SDL_SetWindowFullscreen(V.win,
            V.fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
        break;
    case ACT_PAN_LEFT:
        V.pan_x_target += V.scroll_speed;
        clamp_pan();
        break;
    case ACT_PAN_RIGHT:
        V.pan_x_target -= V.scroll_speed;
        clamp_pan();
        break;
    case ACT_NONE:
        break;
    }
}

/* ---------- input handling ---------- */

static void handle_event(SDL_Event *e)
{
    switch (e->type) {
    case SDL_QUIT:
        V.quit = 1;
        break;

    case SDL_KEYDOWN: {
        enum action act = lookup_bind(e->key.keysym.sym);
        int shift = (e->key.keysym.mod & KMOD_SHIFT) != 0;

        if (shift && act == ACT_SCROLL_DOWN) act = ACT_FAST_SCROLL_DOWN;
        if (shift && act == ACT_SCROLL_UP)   act = ACT_FAST_SCROLL_UP;

        /* Shift+G = bottom (vim style) */
        if (shift && e->key.keysym.sym == SDLK_g) act = ACT_BOTTOM;

        do_action(act);
        break;
    }

    case SDL_MOUSEWHEEL: {
        int shift = (SDL_GetModState() & KMOD_SHIFT) != 0;
        if (SDL_GetModState() & KMOD_CTRL) {
            double dz = e->wheel.y > 0 ? ZOOM_STEP : -ZOOM_STEP;
            V.zoom += dz;
            if (V.zoom < MIN_ZOOM) V.zoom = MIN_ZOOM;
            if (V.zoom > MAX_ZOOM) V.zoom = MAX_ZOOM;
            V.needs_layout = 1;
            clamp_pan();
        } else {
            int speed = shift ? V.fast_scroll_speed : V.scroll_speed;
            V.scroll_target -= e->wheel.y * speed;
            clamp_scroll();
        }
        break;
    }

    case SDL_MOUSEBUTTONDOWN:
        if (e->button.button == SDL_BUTTON_LEFT) {
            V.dragging = 1;
            V.drag_x = e->button.x;
            V.drag_y = e->button.y;
        }
        break;

    case SDL_MOUSEBUTTONUP:
        if (e->button.button == SDL_BUTTON_LEFT)
            V.dragging = 0;
        break;

    case SDL_MOUSEMOTION:
        if (V.dragging) {
            int dx = e->motion.x - V.drag_x;
            int dy = e->motion.y - V.drag_y;
            V.drag_x = e->motion.x;
            V.drag_y = e->motion.y;

            V.scroll_target -= dy / V.zoom;
            V.scroll_y -= dy / V.zoom;
            clamp_scroll();
            V.scroll_target = V.scroll_y; /* snap to avoid drift */

            V.pan_x_target += dx;
            V.pan_x += dx;
            clamp_pan();
            V.pan_x_target = V.pan_x;
        }
        break;

    case SDL_WINDOWEVENT:
        if (e->window.event == SDL_WINDOWEVENT_RESIZED ||
            e->window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
            V.win_w = e->window.data1;
            V.win_h = e->window.data2;
            V.needs_layout = 1;
        }
        break;
    }
}

/* ---------- signal handler ---------- */

static void sig_handler(int sig)
{
    (void)sig;
    V.quit = 1;
}

/* ---------- main ---------- */

int main(int argc, char **argv)
{
    /* defaults */
    V.scroll_speed      = 80;
    V.fast_scroll_speed = 600;
    V.zoom              = 1.0;
    V.fullscreen        = 0;
    V.sock_fd           = -1;
    V.needs_layout      = 1;
    default_binds();
    snprintf(V.title, sizeof(V.title), "rv");
    snprintf(V.sock_path, sizeof(V.sock_path), "/tmp/rv-%d.sock", getpid());

    /* parse args */
    int img_start = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--scroll-speed") == 0 && i + 1 < argc) {
            V.scroll_speed = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--fast-scroll-speed") == 0 && i + 1 < argc) {
            V.fast_scroll_speed = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--fullscreen") == 0) {
            V.fullscreen = 1;
        } else if (strcmp(argv[i], "--title") == 0 && i + 1 < argc) {
            snprintf(V.title, sizeof(V.title), "%s", argv[++i]);
        } else if (strcmp(argv[i], "--sock") == 0 && i + 1 < argc) {
            snprintf(V.sock_path, sizeof(V.sock_path), "%s", argv[++i]);
        } else if (strcmp(argv[i], "--bind") == 0 && i + 1 < argc) {
            if (parse_bind_arg(argv[++i]) < 0)
                return 1;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "rv: unknown option: %s\n", argv[i]);
            return 1;
        } else {
            img_start = i;
            break;
        }
    }

    /* init SDL */
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        fprintf(stderr, "rv: SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    int img_flags = IMG_INIT_PNG | IMG_INIT_JPG | IMG_INIT_WEBP;
    if ((IMG_Init(img_flags) & img_flags) != img_flags) {
        fprintf(stderr, "rv: IMG_Init failed: %s\n", IMG_GetError());
        SDL_Quit();
        return 1;
    }

    Uint32 win_flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_SHOWN;
    if (V.fullscreen)
        win_flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;

    V.win = SDL_CreateWindow(V.title,
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        800, 900, win_flags);
    if (!V.win) {
        fprintf(stderr, "rv: window creation failed: %s\n", SDL_GetError());
        IMG_Quit();
        SDL_Quit();
        return 1;
    }

    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");

    V.ren = SDL_CreateRenderer(V.win, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!V.ren) {
        fprintf(stderr, "rv: renderer creation failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(V.win);
        IMG_Quit();
        SDL_Quit();
        return 1;
    }

    SDL_GetWindowSize(V.win, &V.win_w, &V.win_h);

    /* start background loader */
    loader_init();

    /* load initial images */
    if (img_start > 0) {
        for (int i = img_start; i < argc; i++)
            add_image(argv[i]);
    }

    /* setup IPC */
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
    ipc_init();

    printf("%s\n", V.sock_path);
    fflush(stdout);

    /* main loop */
    while (!V.quit) {
        SDL_Event e;
        while (SDL_PollEvent(&e))
            handle_event(&e);

        ipc_poll();
        manage_textures();
        smooth_scroll();
        render();

        SDL_Delay(4);
    }

    /* cleanup */
    loader_shutdown();
    ipc_cleanup();
    for (int i = 0; i < V.count; i++) {
        if (V.images[i].tex) SDL_DestroyTexture(V.images[i].tex);
        if (V.images[i].surf) SDL_FreeSurface(V.images[i].surf);
        free(V.images[i].path);
    }
    SDL_DestroyRenderer(V.ren);
    SDL_DestroyWindow(V.win);
    IMG_Quit();
    SDL_Quit();

    return 0;
}
