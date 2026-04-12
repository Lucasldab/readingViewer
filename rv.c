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
 *          fullscreen, quit
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

/* ---------- constants ---------- */

#define MAX_IMAGES      4096
#define IPC_BUF_SIZE    4096
#define SCROLL_SMOOTH   0.15
#define ZOOM_STEP       0.1
#define MIN_ZOOM        0.1
#define MAX_ZOOM        5.0
#define SOCK_PATH_MAX   (sizeof(((struct sockaddr_un *)0)->sun_path))
#define MAX_BINDS       64

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
    /* single character */
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

typedef struct {
    char           *path;
    SDL_Texture    *tex;
    int             w, h;        /* original pixel dimensions */
    int             y_offset;    /* vertical offset in the strip */
} Image;

/* ---------- viewer state ---------- */

static struct {
    SDL_Window      *win;
    SDL_Renderer    *ren;
    int              win_w, win_h;

    Image            images[MAX_IMAGES];
    int              count;
    int              total_h;    /* total strip height in pixels */

    double           scroll_y;       /* current scroll position (smooth) */
    double           scroll_target;  /* target scroll position */
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
} V;

/* ---------- keybind helpers ---------- */

static void bind_key(SDL_Keycode key, enum action act)
{
    /* overwrite existing bind for this key */
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
    const char *keyname = buf;
    const char *actname = eq + 1;

    SDL_Keycode key = parse_key(keyname);
    if (key == SDLK_UNKNOWN) {
        fprintf(stderr, "rv: unknown key: %s\n", keyname);
        return -1;
    }
    enum action act = parse_action(actname);
    if (act == ACT_NONE) {
        fprintf(stderr, "rv: unknown action: %s\n", actname);
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
static int  load_image(const char *path);
static void ipc_init(void);
static void ipc_poll(void);
static void ipc_cleanup(void);
static void ipc_handle(const char *cmd);
static void clamp_scroll(void);

/* ---------- image loading ---------- */

static int load_image(const char *path)
{
    if (V.count >= MAX_IMAGES) {
        fprintf(stderr, "rv: max images (%d) reached\n", MAX_IMAGES);
        return -1;
    }

    SDL_Surface *surf = IMG_Load(path);
    if (!surf) {
        fprintf(stderr, "rv: failed to load %s: %s\n", path, IMG_GetError());
        return -1;
    }

    SDL_Texture *tex = SDL_CreateTextureFromSurface(V.ren, surf);
    int w = surf->w, h = surf->h;
    SDL_FreeSurface(surf);

    if (!tex) {
        fprintf(stderr, "rv: texture creation failed: %s\n", SDL_GetError());
        return -1;
    }

    Image *img = &V.images[V.count];
    img->path = strdup(path);
    img->tex  = tex;
    img->w    = w;
    img->h    = h;
    img->y_offset = 0; /* set by layout() */
    V.count++;
    V.needs_layout = 1;

    return V.count - 1;
}

/* ---------- layout: compute y offsets ---------- */

static void layout(void)
{
    int y = 0;
    for (int i = 0; i < V.count; i++) {
        V.images[i].y_offset = y;
        /* scale width to window, adjust height proportionally */
        double scale = (double)V.win_w / V.images[i].w;
        int scaled_h = (int)(V.images[i].h * scale);
        y += scaled_h;
    }
    V.total_h = y;
    V.needs_layout = 0;
}

/* ---------- rendering ---------- */

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
        double scale = (double)V.win_w / img->w;
        int scaled_w = (int)(img->w * scale * V.zoom);
        int scaled_h = (int)(img->h * scale * V.zoom);
        int y = (int)(img->y_offset * V.zoom - zy);

        /* cull off-screen */
        if (y + scaled_h < 0 || y > V.win_h)
            continue;

        /* center horizontally if zoomed wider than window */
        int x = (V.win_w - scaled_w) / 2;

        SDL_Rect dst = { x, y, scaled_w, scaled_h };
        SDL_RenderCopy(V.ren, img->tex, NULL, &dst);
    }

    SDL_RenderPresent(V.ren);
}

/* ---------- scroll helpers ---------- */

static void clamp_scroll(void)
{
    if (V.scroll_target < 0)
        V.scroll_target = 0;

    double max_scroll = V.total_h - (double)V.win_h / V.zoom;
    if (max_scroll < 0) max_scroll = 0;
    if (V.scroll_target > max_scroll)
        V.scroll_target = max_scroll;
}

static void smooth_scroll(void)
{
    double diff = V.scroll_target - V.scroll_y;
    if (diff > -0.5 && diff < 0.5) {
        V.scroll_y = V.scroll_target;
    } else {
        V.scroll_y += diff * SCROLL_SMOOTH;
    }
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
        /* strip trailing newline */
        if (n > 0 && buf[n-1] == '\n') buf[n-1] = '\0';
        ipc_handle(buf);
    }
    close(client);
}

static void ipc_handle(const char *cmd)
{
    if (strncmp(cmd, "open ", 5) == 0) {
        const char *path = cmd + 5;
        /* skip leading whitespace */
        while (*path == ' ') path++;
        if (*path) {
            load_image(path);
            fprintf(stderr, "rv: opened %s (total: %d)\n", path, V.count);
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

        /* shift upgrades scroll to fast_scroll */
        if (shift && act == ACT_SCROLL_DOWN) act = ACT_FAST_SCROLL_DOWN;
        if (shift && act == ACT_SCROLL_UP)   act = ACT_FAST_SCROLL_UP;

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
            break;
        case ACT_ZOOM_RESET:
            V.zoom = 1.0;
            V.needs_layout = 1;
            break;
        case ACT_FULLSCREEN:
            V.fullscreen = !V.fullscreen;
            SDL_SetWindowFullscreen(V.win,
                V.fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
            break;
        case ACT_NONE:
            break;
        }
        break;
    }

    case SDL_MOUSEWHEEL: {
        int shift = (SDL_GetModState() & KMOD_SHIFT) != 0;
        if (SDL_GetModState() & KMOD_CTRL) {
            /* zoom with ctrl+scroll */
            double dz = e->wheel.y > 0 ? ZOOM_STEP : -ZOOM_STEP;
            V.zoom += dz;
            if (V.zoom < MIN_ZOOM) V.zoom = MIN_ZOOM;
            if (V.zoom > MAX_ZOOM) V.zoom = MAX_ZOOM;
            V.needs_layout = 1;
        } else {
            int speed = shift ? V.fast_scroll_speed : V.scroll_speed;
            V.scroll_target -= e->wheel.y * speed;
            clamp_scroll();
        }
        break;
    }

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
            /* first non-option = start of image paths */
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

    /* load initial images */
    if (img_start > 0) {
        for (int i = img_start; i < argc; i++)
            load_image(argv[i]);
    }

    /* setup IPC */
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
    ipc_init();

    /* print socket path for parent process */
    printf("%s\n", V.sock_path);
    fflush(stdout);

    /* main loop */
    while (!V.quit) {
        SDL_Event e;
        while (SDL_PollEvent(&e))
            handle_event(&e);

        ipc_poll();
        smooth_scroll();
        render();

        SDL_Delay(4); /* ~240fps cap, vsync does actual limiting */
    }

    /* cleanup */
    ipc_cleanup();
    for (int i = 0; i < V.count; i++) {
        SDL_DestroyTexture(V.images[i].tex);
        free(V.images[i].path);
    }
    SDL_DestroyRenderer(V.ren);
    SDL_DestroyWindow(V.win);
    IMG_Quit();
    SDL_Quit();

    return 0;
}
