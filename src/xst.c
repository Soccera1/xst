// xst.c - A simple X11+OpenGL terminal.
//
// To compile:
// gcc xst.c -o xst $(pkg-config --cflags --libs x11 gl freetype2) -lutil -lm
//
// To run:
// ./xst

#define _XOPEN_SOURCE 600
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pty.h>
#include <fcntl.h>
#include <sys/select.h>
#include <errno.h>
#include <limits.h>
#include <sys/ioctl.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <GL/gl.h>
#include <GL/glx.h>

#include <ft2build.h>
#include FT_FREETYPE_H

// --- Structs & Enums ---

typedef struct {
    float ax; float ay; float bw; float bh;
    float bl; float bt; float tx;
} Glyph;

typedef struct {
    char c;
} Cell;

typedef enum {
    STATE_NORMAL,
    STATE_ESC,
    STATE_CSI,
    STATE_OSC,
} AnsiState;

// --- Globals ---
Display *dpy;
Window win;
GLXContext ctx;
int pty_master_fd;

int COLS = 80, ROWS = 24;
int win_width = 800, win_height = 600;
Cell *grid = NULL;

int cursor_x = 0, cursor_y = 0;

FT_Library ft_lib;
FT_Face ft_face;
Glyph glyphs[128];
GLuint font_texture;
int font_atlas_w, font_atlas_h;
float char_w, char_h;

AnsiState ansi_state = STATE_NORMAL;
char csi_buf[256];
int csi_len = 0;

// --- Function Prototypes ---
void die(const char *s);
void x11_init();
void gl_init();
void font_init(const char* font_path, int font_size);
void pty_init();
void main_loop();
void term_draw();
void term_handle_char(char c);
void term_scroll();
void term_resize(int w, int h);
void csi_dispatch();
void clear_line(int mode);
void clear_screen(int mode);

// --- Implementation ---

void die(const char *s) {
    perror(s);
    exit(1);
}

void csi_dispatch() {
    char *p = csi_buf;
    char cmd = csi_buf[csi_len - 1];
    csi_buf[csi_len - 1] = '\0';

    int params[16] = {0};
    int num_params = 0;
    int is_private = (*p == '?');
    if (is_private) p++;

    char *tok = strtok(p, ";");
    while (tok && num_params < 16) {
        params[num_params++] = atoi(tok);
        tok = strtok(NULL, ";");
    }

    if (is_private) {
        return; // Ignore private modes like '?2004h'
    }
    
    switch (cmd) {
        case 'H': // Set cursor position
        case 'f': {
            int r = (num_params > 0 && params[0] > 0) ? params[0] - 1 : 0;
            int c = (num_params > 1 && params[1] > 0) ? params[1] - 1 : 0;
            cursor_y = r;
            cursor_x = c;
            break;
        }
        case 'A': cursor_y -= (num_params > 0 && params[0] > 0) ? params[0] : 1; break; // Up
        case 'B': cursor_y += (num_params > 0 && params[0] > 0) ? params[0] : 1; break; // Down
        case 'C': cursor_x += (num_params > 0 && params[0] > 0) ? params[0] : 1; break; // Forward
        case 'D': cursor_x -= (num_params > 0 && params[0] > 0) ? params[0] : 1; break; // Backward
        case 'J': clear_screen(params[0]); break; // Default param is 0
        case 'K': clear_line(params[0]); break;   // Default param is 0
        case 'm': /* Ignore color codes */ break;
    }

    if (cursor_x < 0) cursor_x = 0;
    if (cursor_x >= COLS) cursor_x = COLS - 1;
    if (cursor_y < 0) cursor_y = 0;
    if (cursor_y >= ROWS) cursor_y = ROWS - 1;
}

void clear_screen(int mode) {
    switch(mode) {
        case 0: // From cursor to end of screen
            clear_line(0);
            for(int y = cursor_y + 1; y < ROWS; y++) {
                for (int x = 0; x < COLS; x++) grid[y * COLS + x].c = ' ';
            }
            break;
        case 1: // From cursor to beginning of screen
            for(int y = 0; y < cursor_y; y++) {
                for (int x = 0; x < COLS; x++) grid[y * COLS + x].c = ' ';
            }
            clear_line(1);
            break;
        case 2: // Entire screen
        case 3:
            for(int i = 0; i < ROWS * COLS; i++) grid[i].c = ' ';
            cursor_x = cursor_y = 0;
            break;
    }
}

void clear_line(int mode) {
    int start, end;
    switch(mode) {
        case 0: start = cursor_x; end = COLS; break; // To end of line
        case 1: start = 0; end = cursor_x + 1; break; // To beginning of line
        case 2: start = 0; end = COLS; break; // Entire line
        default: return;
    }
    for(int x = start; x < end; x++) {
        if (cursor_y < ROWS && x < COLS) grid[cursor_y * COLS + x].c = ' ';
    }
}

void term_resize(int w, int h) {
    win_width = w; win_height = h;
    int new_cols = win_width / char_w;
    int new_rows = win_height / char_h;
    if (new_cols < 1) new_cols = 1;
    if (new_rows < 1) new_rows = 1;

    int old_cols = COLS;
    int old_rows = ROWS;
    Cell* old_grid = grid;

    COLS = new_cols; ROWS = new_rows;
    grid = malloc(ROWS * COLS * sizeof(Cell));
    if (!grid) die("malloc failed for new grid");
    for(int i=0; i<ROWS*COLS; ++i) grid[i].c = ' ';

    if (old_grid) {
        int min_rows = (old_rows < ROWS) ? old_rows : ROWS;
        int min_cols = (old_cols < COLS) ? old_cols : COLS;
        for (int y = 0; y < min_rows; y++) {
            memcpy(&grid[y * COLS], &old_grid[y * old_cols], min_cols * sizeof(Cell));
        }
        free(old_grid);
    }
    
    if (cursor_x >= COLS) cursor_x = COLS - 1;
    if (cursor_y >= ROWS) cursor_y = ROWS - 1;

    glViewport(0, 0, win_width, win_height);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0.0, win_width, win_height, 0.0, -1.0, 1.0);
    glMatrixMode(GL_MODELVIEW);
    
    struct winsize ws = { .ws_row = ROWS, .ws_col = COLS, .ws_xpixel = w, .ws_ypixel = h };
    ioctl(pty_master_fd, TIOCSWINSZ, &ws);
}

void x11_init() {
    dpy = XOpenDisplay(NULL);
    if (!dpy) die("Cannot connect to X server");
    Window root = DefaultRootWindow(dpy);
    GLint att[] = { GLX_RGBA, GLX_DEPTH_SIZE, 24, GLX_DOUBLEBUFFER, None };
    XVisualInfo *vi = glXChooseVisual(dpy, 0, att);
    if (!vi) die("No appropriate visual found");
    Colormap cmap = XCreateColormap(dpy, root, vi->visual, AllocNone);
    XSetWindowAttributes swa;
    swa.colormap = cmap;
    swa.event_mask = ExposureMask | KeyPressMask | StructureNotifyMask;
    win = XCreateWindow(dpy, root, 0, 0, win_width, win_height, 0, vi->depth, InputOutput, vi->visual, CWColormap | CWEventMask, &swa);
    XMapWindow(dpy, win);
    XStoreName(dpy, win, "xst");
    Atom wm_delete_window = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, win, &wm_delete_window, 1);
    ctx = glXCreateContext(dpy, vi, NULL, GL_TRUE);
    glXMakeCurrent(dpy, win, ctx);
}

void gl_init() {
    glEnable(GL_TEXTURE_2D);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}

void font_init(const char* font_path, int font_size) {
    if (FT_Init_FreeType(&ft_lib)) die("Could not init freetype library");
    if (FT_New_Face(ft_lib, font_path, 0, &ft_face)) die("Could not open font");
    FT_Set_Pixel_Sizes(ft_face, 0, font_size);
    FT_GlyphSlot g = ft_face->glyph;
    char_h = font_size;
    if (FT_Load_Char(ft_face, 'M', FT_LOAD_RENDER)) die("Could not load 'M' character");
    char_w = (g->advance.x >> 6);
    if (char_w == 0) char_w = font_size / 2;
    font_atlas_w = 0; font_atlas_h = 0;
    for (int i = 32; i < 128; i++) {
        if (FT_Load_Char(ft_face, i, FT_LOAD_RENDER)) continue;
        font_atlas_w += g->bitmap.width;
        font_atlas_h = (g->bitmap.rows > font_atlas_h) ? g->bitmap.rows : font_atlas_h;
    }
    glGenTextures(1, &font_texture);
    glBindTexture(GL_TEXTURE_2D, font_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, font_atlas_w, font_atlas_h, 0, GL_ALPHA, GL_UNSIGNED_BYTE, NULL);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    int x = 0;
    for (int i = 32; i < 128; i++) {
        if (FT_Load_Char(ft_face, i, FT_LOAD_RENDER)) continue;
        glTexSubImage2D(GL_TEXTURE_2D, 0, x, 0, g->bitmap.width, g->bitmap.rows,
                        GL_ALPHA, GL_UNSIGNED_BYTE, g->bitmap.buffer);
        glyphs[i] = (Glyph){ .ax = g->advance.x >> 6, .ay = g->advance.y >> 6,
                             .bw = g->bitmap.width, .bh = g->bitmap.rows,
                             .bl = g->bitmap_left, .bt = g->bitmap_top,
                             .tx = (float)x / font_atlas_w };
        x += g->bitmap.width;
    }
}

void pty_init() {
    pid_t pid = forkpty(&pty_master_fd, NULL, NULL, NULL);
    if (pid < 0) die("forkpty failed");
    if (pid == 0) {
        setenv("TERM", "xterm-256color", 1);
        char *shell = getenv("SHELL");
        if (!shell) shell = "/bin/sh";
        execl(shell, shell, (char *)NULL);
        exit(0);
    }
    int flags = fcntl(pty_master_fd, F_GETFL, 0);
    fcntl(pty_master_fd, F_SETFL, flags | O_NONBLOCK);
}

void term_scroll() {
    memmove(&grid[0], &grid[COLS], (ROWS - 1) * COLS * sizeof(Cell));
    for (int i = 0; i < COLS; i++) {
        grid[(ROWS - 1) * COLS + i].c = ' ';
    }
    cursor_y--;
}

void term_handle_char(char c) {
    switch (ansi_state) {
        case STATE_NORMAL:
            if (c == '\x1b') {
                ansi_state = STATE_ESC;
            } else if (c == '\n') {
                cursor_y++;
            } else if (c == '\r') {
                cursor_x = 0;
            } else if (c == '\b') {
                if (cursor_x > 0) cursor_x--;
            } else if (c == '\t') {
                cursor_x = (cursor_x + 8) & ~7;
            } else if (c >= 32) {
                if (cursor_x >= COLS) {
                    cursor_x = 0;
                    cursor_y++;
                }
                if (cursor_y >= ROWS) {
                    term_scroll();
                }
                if (cursor_y < ROWS && cursor_x < COLS) {
                    grid[cursor_y * COLS + cursor_x].c = c;
                    cursor_x++;
                }
            }
            break;
        case STATE_ESC:
            if (c == '[') {
                ansi_state = STATE_CSI;
                csi_len = 0;
            } else if (c == ']') {
                ansi_state = STATE_OSC;
            } else {
                ansi_state = STATE_NORMAL;
            }
            break;
        case STATE_CSI:
            if (csi_len < sizeof(csi_buf) - 1) {
                csi_buf[csi_len++] = c;
                if ((c >= '@' && c <= '~')) {
                    csi_dispatch();
                    ansi_state = STATE_NORMAL;
                }
            } else {
                ansi_state = STATE_NORMAL;
            }
            break;
        case STATE_OSC:
            if (c == '\x07' || (c == '\x1b' && (ansi_state = STATE_ESC))) {
                ansi_state = STATE_NORMAL;
            }
            break;
    }

    if (cursor_y >= ROWS) {
        term_scroll();
    }
}

void term_draw() {
    glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glBindTexture(GL_TEXTURE_2D, font_texture);
    glColor3f(0.9f, 0.9f, 0.9f);
    glBegin(GL_QUADS);
    for (int y = 0; y < ROWS; y++) {
        for (int x = 0; x < COLS; x++) {
            char c = grid[y * COLS + x].c;
            if (c < 32 || c > 126) continue;
            Glyph *g = &glyphs[(int)c];
            float xpos = x * char_w + g->bl;
            float ypos = y * char_h + (char_h - g->bt);
            float w = g->bw; float h = g->bh;
            float u0 = g->tx, v0 = 0.0f;
            float u1 = g->tx + g->bw / font_atlas_w, v1 = g->bh / font_atlas_h;
            glTexCoord2f(u0, v0); glVertex2f(xpos, ypos);
            glTexCoord2f(u1, v0); glVertex2f(xpos + w, ypos);
            glTexCoord2f(u1, v1); glVertex2f(xpos + w, ypos + h);
            glTexCoord2f(u0, v1); glVertex2f(xpos, ypos + h);
        }
    }
    glEnd();
    glDisable(GL_TEXTURE_2D);
    glColor3f(1.0f, 1.0f, 0.0f);
    glRectf(cursor_x * char_w, cursor_y * char_h,
            (cursor_x + 1) * char_w, (cursor_y + 1) * char_h);
    glEnable(GL_TEXTURE_2D);
    glXSwapBuffers(dpy, win);
}

void main_loop() {
    XEvent e;
    char buf[4096];
    int running = 1;
    while (running) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(ConnectionNumber(dpy), &fds);
        FD_SET(pty_master_fd, &fds);
        struct timeval timeout = { .tv_sec = 0, .tv_usec = 16666 };
        select(pty_master_fd + 1, &fds, NULL, NULL, &timeout);
        while (XPending(dpy)) {
            XNextEvent(dpy, &e);
            if (e.type == KeyPress) {
                int count = XLookupString(&e.xkey, buf, sizeof(buf), NULL, NULL);
                if (count > 0) write(pty_master_fd, buf, count);
            } else if (e.type == ConfigureNotify) {
                XConfigureEvent xce = e.xconfigure;
                if (xce.width != win_width || xce.height != win_height) {
                    term_resize(xce.width, xce.height);
                }
            } else if (e.type == ClientMessage) {
                running = 0;
            }
        }
        if (FD_ISSET(pty_master_fd, &fds)) {
            int count = read(pty_master_fd, buf, sizeof(buf));
            if (count > 0) {
                for (int i = 0; i < count; i++) {
                    term_handle_char(buf[i]);
                }
            } else if (count <= 0 && errno != EAGAIN) {
                running = 0;
            }
        }
        term_draw();
    }
}

int main(int argc, char *argv[]) {
    int font_size = 16;
    if (argc > 1) {
        font_size = atoi(argv[1]);
    } else {
        char config_path[PATH_MAX];
        char *home = getenv("HOME");
        // MODIFIED: Use xst config file
        if (home) {
            snprintf(config_path, sizeof(config_path), "%s/.xst", home);
            FILE *f = fopen(config_path, "r");
            if (f) {
                char line[16];
                if (fgets(line, sizeof(line), f)) font_size = atoi(line);
                fclose(f);
            }
        }
    }
    if (font_size <= 5) font_size = 16;
    
    const char* font_path = "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf";
    if(access(font_path, F_OK) == -1) {
        fprintf(stderr, "Font not found: %s\n", font_path); return 1;
    }

    x11_init();
    gl_init();
    font_init(font_path, font_size);
    pty_init();
    term_resize(win_width, win_height);
    main_loop();

    free(grid);
    glXMakeCurrent(dpy, None, NULL);
    glXDestroyContext(dpy, ctx);
    XDestroyWindow(dpy, win);
    XCloseDisplay(dpy);
    FT_Done_Face(ft_face);
    FT_Done_FreeType(ft_lib);
    close(pty_master_fd);
    return 0;
}
