#define SDL_MAIN_HANDLED

#include "host_ipc.h"
#include "log.h"

#include <SDL.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static int event_fd = -1;

static int map_key(SDL_Scancode scancode)
{
    switch (scancode) {
    case SDL_SCANCODE_ESCAPE:    return 0x01;
    case SDL_SCANCODE_LEFT:      return 0x02;
    case SDL_SCANCODE_UP:        return 0x03;
    case SDL_SCANCODE_RIGHT:     return 0x04;
    case SDL_SCANCODE_DOWN:      return 0x05;
    case SDL_SCANCODE_BACKSPACE: return 0x0c;
    case SDL_SCANCODE_RETURN:
    case SDL_SCANCODE_KP_ENTER:  return 0x0d;
    case SDL_SCANCODE_SPACE:     return 0x20;
    case SDL_SCANCODE_LSHIFT:
    case SDL_SCANCODE_RSHIFT:    return 0x8b;
    case SDL_SCANCODE_0:         return 0x30;
    case SDL_SCANCODE_Y:
    case SDL_SCANCODE_1:         return 0x59;
    case SDL_SCANCODE_Z:
    case SDL_SCANCODE_2:         return 0x5a;
    case SDL_SCANCODE_3:         return 0x33;
    case SDL_SCANCODE_U:
    case SDL_SCANCODE_4:         return 0x55;
    case SDL_SCANCODE_V:
    case SDL_SCANCODE_5:         return 0x56;
    case SDL_SCANCODE_W:
    case SDL_SCANCODE_6:         return 0x57;
    case SDL_SCANCODE_Q:
    case SDL_SCANCODE_7:         return 0x51;
    case SDL_SCANCODE_R:
    case SDL_SCANCODE_8:         return 0x52;
    case SDL_SCANCODE_S:
    case SDL_SCANCODE_9:         return 0x53;
    case SDL_SCANCODE_D:         return 0x44;
    case SDL_SCANCODE_O:
    case SDL_SCANCODE_COMMA:     return 0x4f;
    case SDL_SCANCODE_T:
    case SDL_SCANCODE_SLASH:
    case SDL_SCANCODE_KP_DIVIDE: return 0x54;
    case SDL_SCANCODE_X:
    case SDL_SCANCODE_KP_MULTIPLY:return 0x58;
    case SDL_SCANCODE_PERIOD:    return 0xb8;
    case SDL_SCANCODE_MINUS:     return 0xb7;
    case SDL_SCANCODE_EQUALS:    return 0xb9;
    case SDL_SCANCODE_A:         return 0x41;
    case SDL_SCANCODE_B:         return 0x42;
    case SDL_SCANCODE_C:         return 0x43;
    case SDL_SCANCODE_E:         return 0x45;
    case SDL_SCANCODE_F:         return 0x46;
    case SDL_SCANCODE_G:         return 0x47;
    case SDL_SCANCODE_H:         return 0x48;
    case SDL_SCANCODE_I:         return 0x49;
    case SDL_SCANCODE_J:         return 0x4a;
    case SDL_SCANCODE_K:         return 0x4b;
    case SDL_SCANCODE_L:         return 0x4c;
    case SDL_SCANCODE_M:         return 0x4d;
    case SDL_SCANCODE_N:         return 0x4e;
    case SDL_SCANCODE_P:         return 0x50;
    case SDL_SCANCODE_LCTRL:
    case SDL_SCANCODE_RCTRL:     return 0x83;
    case SDL_SCANCODE_F1:        return 0x91;
    case SDL_SCANCODE_F2:        return 0x93;
    case SDL_SCANCODE_F3:        return 0xb2;
    case SDL_SCANCODE_F4:        return 0xb3;
    case SDL_SCANCODE_F5:        return 0xb4;
    case SDL_SCANCODE_F6:        return 0xb5;
    case SDL_SCANCODE_F7:        return 0xb6;
    case SDL_SCANCODE_F8:        return 0x95;
    case SDL_SCANCODE_F9:        return 0xb1;
    case SDL_SCANCODE_F10:       return 0xe047;
    default:                     return -1;
    }
}

static int send_event(uint32_t type, int value0, int value1, int value2)
{
    primeshim_host_event_t event = {
        .type = type,
        .value0 = value0,
        .value1 = value1,
        .value2 = value2
    };
    const unsigned char *cursor = (const unsigned char *)&event;
    size_t remaining = sizeof(event);

    PS_LOG_TRACE("input", "sending type=%u values=%d,%d,%d", type, value0,
                 value1, value2);

    while (remaining) {
        ssize_t count = write(event_fd, cursor, remaining);
        if (count > 0) {
            cursor += count;
            remaining -= (size_t)count;
        } else if (count < 0 && errno == EINTR) {
            continue;
        } else {
            return -1;
        }
    }
    return 0;
}

static void window_to_lcd(SDL_Window *window, int window_x, int window_y,
                          int *lcd_x, int *lcd_y)
{
    int window_width = 1;
    int window_height = 1;
    int window_screen_x;
    int window_screen_y;
    int global_x;
    int global_y;
    double scale_x;
    double scale_y;
    double scale;
    double viewport_x;
    double viewport_y;

    /* SDL mouse positions and SDL_GetWindowSize use the same coordinate
     * space on most backends. X11 desktop scaling can report event positions
     * in logical points while window geometry remains in screen pixels, so
     * prefer a valid global-pointer offset there. Wayland falls back to the
     * event coordinates because global pointer positions are unavailable. */
    SDL_GetWindowSize(window, &window_width, &window_height);
    SDL_GetWindowPosition(window, &window_screen_x, &window_screen_y);
    SDL_GetGlobalMouseState(&global_x, &global_y);
    global_x -= window_screen_x;
    global_y -= window_screen_y;
    if (global_x >= 0 && global_x < window_width &&
        global_y >= 0 && global_y < window_height) {
        window_x = global_x;
        window_y = global_y;
    }
    scale_x = (double)window_width / PRIMESHIM_LCD_WIDTH;
    scale_y = (double)window_height / PRIMESHIM_LCD_HEIGHT;
    scale = scale_x < scale_y ? scale_x : scale_y;
    if (scale <= 0.0) scale = 1.0;
    viewport_x = (window_width - PRIMESHIM_LCD_WIDTH * scale) / 2.0;
    viewport_y = (window_height - PRIMESHIM_LCD_HEIGHT * scale) / 2.0;
    *lcd_x = (int)((window_x - viewport_x) / scale);
    *lcd_y = (int)((window_y - viewport_y) / scale);
    if (*lcd_x < 0) *lcd_x = 0;
    if (*lcd_y < 0) *lcd_y = 0;
    if (*lcd_x >= PRIMESHIM_LCD_WIDTH) *lcd_x = PRIMESHIM_LCD_WIDTH - 1;
    if (*lcd_y >= PRIMESHIM_LCD_HEIGHT) *lcd_y = PRIMESHIM_LCD_HEIGHT - 1;
    PS_LOG_TRACE("input", "window=%d,%d size=%dx%d -> LCD=%d,%d", window_x,
                 window_y, window_width, window_height, *lcd_x, *lcd_y);
}

static pid_t launch_child(char **command, int framebuffer_fd,
                          int input_read_fd, int input_write_fd)
{
    pid_t child = fork();
    if (child == 0) {
        char framebuffer_text[32];
        char input_text[32];

        close(input_write_fd);
        snprintf(framebuffer_text, sizeof(framebuffer_text), "%d", framebuffer_fd);
        snprintf(input_text, sizeof(input_text), "%d", input_read_fd);
        setenv(PRIMESHIM_FRAMEBUFFER_FD_ENV, framebuffer_text, 1);
        setenv(PRIMESHIM_INPUT_FD_ENV, input_text, 1);
        execvp(command[0], command);
        PS_LOG_ERRNO_ERROR("host", errno, "cannot execute %s", command[0]);
        _exit(127);
    }
    return child;
}

static void stop_child(pid_t child)
{
    int status;
    int attempts;

    if (child <= 0 || waitpid(child, &status, WNOHANG) == child) return;
    kill(child, SIGTERM);
    for (attempts = 0; attempts < 50; ++attempts) {
        if (waitpid(child, &status, WNOHANG) == child) return;
        SDL_Delay(20);
    }
    kill(child, SIGKILL);
    (void)waitpid(child, &status, 0);
}

int main(int argc, char **argv)
{
    SDL_Window *window = NULL;
    SDL_Renderer *renderer = NULL;
    SDL_Texture *texture = NULL;
    uint32_t *framebuffer = MAP_FAILED;
    int framebuffer_fd = -1;
    int event_pipe[2] = {-1, -1};
    pid_t child = -1;
    int child_status = 1;
    int running = 1;
    int mouse_down = 0;
    int requested_shutdown = 0;

    if (argc < 2) {
        fprintf(stderr, "usage: %s command [args ...]\n", argv[0]);
        return 2;
    }
    primeshim_log_init();
    signal(SIGPIPE, SIG_IGN);
    SDL_SetMainReady();
    SDL_SetHint(SDL_HINT_MOUSE_TOUCH_EVENTS, "0");
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
        PS_LOG_ERROR("host", "SDL initialization failed: %s", SDL_GetError());
        goto cleanup;
    }

    window = SDL_CreateWindow("PrimeShim", SDL_WINDOWPOS_CENTERED,
                              SDL_WINDOWPOS_CENTERED,
                              PRIMESHIM_LCD_WIDTH * 2,
                              PRIMESHIM_LCD_HEIGHT * 2,
                              SDL_WINDOW_RESIZABLE);
    if (!window) {
        PS_LOG_ERROR("host", "cannot create SDL window: %s", SDL_GetError());
        goto cleanup;
    }
    renderer = SDL_CreateRenderer(window, -1,
                                  SDL_RENDERER_ACCELERATED |
                                  SDL_RENDERER_PRESENTVSYNC);
    if (!renderer) renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    if (!renderer) {
        PS_LOG_ERROR("host", "cannot create SDL renderer: %s", SDL_GetError());
        goto cleanup;
    }
    SDL_RenderSetLogicalSize(renderer, PRIMESHIM_LCD_WIDTH,
                            PRIMESHIM_LCD_HEIGHT);
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");
    texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                                SDL_TEXTUREACCESS_STREAMING,
                                PRIMESHIM_LCD_WIDTH,
                                PRIMESHIM_LCD_HEIGHT);
    if (!texture) {
        PS_LOG_ERROR("host", "cannot create SDL texture: %s", SDL_GetError());
        goto cleanup;
    }

    framebuffer_fd = memfd_create("primeshim-framebuffer", 0);
    if (framebuffer_fd < 0 ||
        ftruncate(framebuffer_fd, PRIMESHIM_FRAMEBUFFER_BYTES) != 0) {
        PS_LOG_ERRNO_ERROR("host", errno, "cannot create framebuffer memfd");
        goto cleanup;
    }
    framebuffer = mmap(NULL, PRIMESHIM_FRAMEBUFFER_BYTES,
                       PROT_READ | PROT_WRITE, MAP_SHARED, framebuffer_fd, 0);
    if (framebuffer == MAP_FAILED) {
        PS_LOG_ERRNO_ERROR("host", errno, "cannot map framebuffer");
        goto cleanup;
    }
    memset(framebuffer, 0, PRIMESHIM_FRAMEBUFFER_BYTES);
    if (pipe(event_pipe) != 0) {
        PS_LOG_ERRNO_ERROR("host", errno, "cannot create input pipe");
        goto cleanup;
    }
    event_fd = event_pipe[1];
    child = launch_child(&argv[1], framebuffer_fd, event_pipe[0], event_pipe[1]);
    if (child < 0) {
        PS_LOG_ERRNO_ERROR("host", errno, "cannot launch emulator child");
        goto cleanup;
    }
    close(event_pipe[0]);
    event_pipe[0] = -1;

    while (running) {
        SDL_Event event;
        pid_t wait_result = waitpid(child, &child_status, WNOHANG);
        if (wait_result == child || wait_result < 0) {
            child = -1;
            break;
        }

        while (SDL_PollEvent(&event)) {
            switch (event.type) {
            case SDL_QUIT:
                requested_shutdown = 1;
                running = 0;
                break;
            case SDL_KEYDOWN:
            case SDL_KEYUP: {
                int keycode;
                if (event.key.repeat) break;
                keycode = map_key(event.key.keysym.scancode);
                if (keycode >= 0 && send_event(PRIMESHIM_HOST_EVENT_KEY,
                                               keycode,
                                               event.type == SDL_KEYDOWN,
                                               0) != 0) {
                    running = 0;
                }
                break;
            }
            case SDL_MOUSEBUTTONDOWN:
            case SDL_MOUSEBUTTONUP:
                if (event.button.which == SDL_TOUCH_MOUSEID) break;
                if (event.button.button == SDL_BUTTON_LEFT) {
                    int x, y;
                    mouse_down = event.type == SDL_MOUSEBUTTONDOWN;
                    window_to_lcd(window, event.button.x, event.button.y, &x, &y);
                    if (send_event(mouse_down ? PRIMESHIM_HOST_EVENT_TOUCH_BEGIN
                                              : PRIMESHIM_HOST_EVENT_TOUCH_END,
                                   x, y, 0) != 0) running = 0;
                }
                break;
            case SDL_MOUSEMOTION:
                if (event.motion.which == SDL_TOUCH_MOUSEID) break;
                if (mouse_down) {
                    int x, y;
                    window_to_lcd(window, event.motion.x, event.motion.y, &x, &y);
                    if (send_event(PRIMESHIM_HOST_EVENT_TOUCH_MOVE,
                                   x, y, 0) != 0) running = 0;
                }
                break;
            case SDL_FINGERDOWN:
            case SDL_FINGERMOTION:
            case SDL_FINGERUP: {
                uint32_t type = event.type == SDL_FINGERDOWN
                              ? PRIMESHIM_HOST_EVENT_TOUCH_BEGIN
                              : event.type == SDL_FINGERUP
                              ? PRIMESHIM_HOST_EVENT_TOUCH_END
                              : PRIMESHIM_HOST_EVENT_TOUCH_MOVE;
                int x = (int)(event.tfinger.x * PRIMESHIM_LCD_WIDTH);
                int y = (int)(event.tfinger.y * PRIMESHIM_LCD_HEIGHT);
                if (send_event(type, x, y, 0) != 0) running = 0;
                break;
            }
            default:
                break;
            }
        }

        SDL_UpdateTexture(texture, NULL, framebuffer,
                          PRIMESHIM_LCD_WIDTH * sizeof(uint32_t));
        SDL_RenderClear(renderer);
        SDL_RenderCopy(renderer, texture, NULL, NULL);
        SDL_RenderPresent(renderer);
    }

cleanup:
    stop_child(child);
    if (event_pipe[0] >= 0) close(event_pipe[0]);
    if (event_pipe[1] >= 0) close(event_pipe[1]);
    if (framebuffer != MAP_FAILED)
        munmap(framebuffer, PRIMESHIM_FRAMEBUFFER_BYTES);
    if (framebuffer_fd >= 0) close(framebuffer_fd);
    if (texture) SDL_DestroyTexture(texture);
    if (renderer) SDL_DestroyRenderer(renderer);
    if (window) SDL_DestroyWindow(window);
    SDL_Quit();

    if (requested_shutdown) return 0;
    if (child_status == 1) return 1;
    if (WIFEXITED(child_status)) return WEXITSTATUS(child_status);
    if (WIFSIGNALED(child_status)) return 128 + WTERMSIG(child_status);
    return 0;
}
