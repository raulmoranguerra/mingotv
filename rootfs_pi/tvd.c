// tvd.c - Simpsons TV daemon (no libgpiod)
// - Switch ON/OFF via raspi-gpio get 26 (level=1 => ON)
// - Touch via /dev/input/event0 (evdev)
// - Single tap: pause/resume (SIGSTOP/SIGCONT on ffmpeg)
// - Double tap: next episode (only if playing and not paused)
// - OFF: stop + backlight off + clear fb
//
// Build: gcc -O2 -Wall -Wextra -o /usr/bin/tvd tvd.c

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static const char *VIDEO_DIR  = "/mnt/videos";
static const char *FBDEV      = "/dev/fb0";
static const char *AUDIO_DEV  = "hw:0,0";
static const char *TOUCH_DEV  = "/dev/input/event0";

// FB params (los tuyos)
static const int FB_STRIDE = 2560;
static const int FB_H      = 480;

// Static noise
static const int STATIC_MS = 250;         // duración total transición
static const int STATIC_FRAMES = 3;       // nº frames que pintamos

// Touch detection
static const int HOLD_MS = 1200;          // no usado como acción, se trata como tap
static const int DOUBLE_TAP_WINDOW_MS = 320;

// Poll loop
static const int SWITCH_POLL_MS = 40;

// ---------- util time ----------
static long long now_ms_monotonic(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

static void ms_sleep(int ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

// ---------- logging ----------
static void log_console(const char *fmt, ...) {
    int fd = open("/dev/console", O_WRONLY | O_CLOEXEC);
    if (fd < 0) return;

    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if (n > 0) {
        (void)write(fd, "[tv] ", 5);
        (void)write(fd, buf, (size_t)n);
        (void)write(fd, "\n", 1);
    }
    close(fd);
}

// ---------- run command ----------
static int run_cmd(const char *cmd) {
    int ret = system(cmd);
    if (ret == -1) return -1;
    if (WIFEXITED(ret)) return WEXITSTATUS(ret);
    return -1;
}

// ---------- backlight ----------
static void backlight_on(void)  { run_cmd("raspi-gpio set 18 op dh >/dev/null 2>&1"); }
static void backlight_off(void) { run_cmd("raspi-gpio set 18 op dl >/dev/null 2>&1"); }

// ---------- fb clear / static ----------
static void fb_clear(void) {
    int fd = open(FBDEV, O_WRONLY | O_CLOEXEC);
    if (fd < 0) return;

    const size_t frame = (size_t)FB_STRIDE * (size_t)FB_H;
    static unsigned char zeros[4096];
    memset(zeros, 0, sizeof(zeros));

    // rewind
    (void)lseek(fd, 0, SEEK_SET);

    size_t remaining = frame;
    while (remaining) {
        size_t chunk = remaining > sizeof(zeros) ? sizeof(zeros) : remaining;
        if (write(fd, zeros, chunk) < 0) break;
        remaining -= chunk;
    }
    close(fd);
}

// --- static audio: ruido blanco a ALSA durante STATIC_MS ---
static pid_t start_static_audio(void) {
    // ffmpeg -t 0.xxx -f s16le -ar 48000 -ac 2 -i /dev/urandom -af volume=0.18 -f alsa hw:0,0
    char tbuf[32];
    snprintf(tbuf, sizeof(tbuf), "0.%03d", STATIC_MS);

    pid_t pid = fork();
    if (pid == 0) {
        // child
        // stdout/stderr a /dev/null
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            if (devnull > 2) close(devnull);
        }

        execlp("ffmpeg", "ffmpeg",
               "-nostdin",
               "-hide_banner", "-loglevel", "quiet",
               "-t", tbuf,
               "-f", "s16le", "-ar", "48000", "-ac", "2", "-i", "/dev/urandom",
               "-af", "volume=0.18",
               "-f", "alsa", AUDIO_DEV,
               (char*)NULL);
        _exit(127);
    }
    return (pid > 0) ? pid : -1;
}

static void static_noise_av(void) {
    // Audio en paralelo + vídeo en fb
    pid_t apid = start_static_audio();

    int fbur = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    int fb   = open(FBDEV, O_WRONLY | O_CLOEXEC);
    if (fbur >= 0 && fb >= 0) {
        const size_t frame = (size_t)FB_STRIDE * (size_t)FB_H;
        unsigned char *buf = malloc(frame);
        if (buf) {
            for (int i = 0; i < STATIC_FRAMES; i++) {
                ssize_t r = read(fbur, buf, frame);
                if (r <= 0) break;
                (void)lseek(fb, 0, SEEK_SET);
                (void)write(fb, buf, (size_t)r);
                ms_sleep(20);
            }
            free(buf);
        }
    }
    if (fbur >= 0) close(fbur);
    if (fb >= 0) close(fb);

    // mantener transición
    ms_sleep(STATIC_MS);

    // esperar al audio (por si arrancó y aún vive)
    if (apid > 0) {
        int st;
        (void)waitpid(apid, &st, WNOHANG);
    }
}

// ---------- switch via raspi-gpio get 26 ----------
static void switch_init(void) {
    run_cmd("raspi-gpio set 26 ip pu >/dev/null 2>&1");
}

static bool switch_is_on(void) {
    // ON si level=1 (según tu salida actual)
    FILE *fp = popen("raspi-gpio get 26 2>/dev/null", "r");
    if (!fp) return false;

    char line[256];
    bool on = false;
    if (fgets(line, sizeof(line), fp)) {
        if (strstr(line, "level=1")) on = true;
    }
    pclose(fp);
    return on;
}

// ---------- ffmpeg control (process group) ----------
static pid_t ffmpeg_pid = -1;
static bool ffmpeg_paused = false;

static bool ffmpeg_alive(void) {
    return (ffmpeg_pid > 0 && kill(ffmpeg_pid, 0) == 0);
}

static void stop_playback(void) {
    if (ffmpeg_pid <= 0) return;

    // mata el grupo entero
    (void)kill(-ffmpeg_pid, SIGTERM);

    // espera breve
    for (int i = 0; i < 25; i++) {
        if (!ffmpeg_alive()) break;
        ms_sleep(30);
    }

    // fuerza
    (void)kill(-ffmpeg_pid, SIGKILL);

    // recoge (si sigue existiendo)
    int status;
    (void)waitpid(ffmpeg_pid, &status, WNOHANG);

    ffmpeg_pid = -1;
    ffmpeg_paused = false;
}

static void pause_ffmpeg(void) {
    if (!ffmpeg_alive()) return;
    // Pausa el grupo para congelar audio+vídeo
    (void)kill(-ffmpeg_pid, SIGSTOP);
    ffmpeg_paused = true;
}

static void resume_ffmpeg(void) {
    if (!ffmpeg_alive()) return;
    (void)kill(-ffmpeg_pid, SIGCONT);
    ffmpeg_paused = false;
}

static char *pick_random_video(void) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "find '%s' -maxdepth 1 -type f \\( -iname '*.mkv' -o -iname '*.mp4' \\) 2>/dev/null | "
             "awk 'BEGIN{srand()} {n++; if (rand()<1/n) pick=$0} END{if(pick!=\"\") print pick}'",
             VIDEO_DIR);

    FILE *fp = popen(cmd, "r");
    if (!fp) return NULL;

    char line[1024];
    if (!fgets(line, sizeof(line), fp)) { pclose(fp); return NULL; }
    pclose(fp);

    size_t len = strlen(line);
    while (len && (line[len-1] == '\n' || line[len-1] == '\r')) line[--len] = 0;
    if (len == 0) return NULL;

    return strdup(line);
}

static void start_playback(void) {
    stop_playback(); // garantía: nunca dos a la vez

    char *v = pick_random_video();
    if (!v) {
        log_console("no videos found in %s", VIDEO_DIR);
        return;
    }

    log_console("play: %s", v);

    pid_t pid = fork();
    if (pid == 0) {
        // child: grupo propio
        setpgid(0, 0);

        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            if (devnull > 2) close(devnull);
        }

        execlp("ffmpeg", "ffmpeg",
               "-nostdin",
               "-hide_banner", "-loglevel", "quiet",
               "-re", "-fflags", "+genpts",
               "-i", v,
               "-vf", "scale=640:480,format=bgra",
               "-pix_fmt", "bgra",
               "-vsync", "0",
               "-map", "0:v:0",
               "-f", "fbdev", FBDEV,
               "-map", "0:a:0?",
               "-f", "alsa", AUDIO_DEV,
               (char*)NULL);

        _exit(127);
    }

    if (pid > 0) {
        setpgid(pid, pid);
        ffmpeg_pid = pid;
        ffmpeg_paused = false;
    }

    free(v);
}

// ---------- touch parsing (evdev) ----------
typedef struct {
    bool touching;
    long long t_down_ms;
} touch_state_t;

static void touch_state_reset(touch_state_t *st) {
    st->touching = false;
    st->t_down_ms = 0;
}

// taps scheduling
static bool pending_single = false;
static long long pending_deadline_ms = 0;

static void schedule_single_tap(void) {
    pending_single = true;
    pending_deadline_ms = now_ms_monotonic() + DOUBLE_TAP_WINDOW_MS;
}

static void cancel_single_tap(void) {
    pending_single = false;
    pending_deadline_ms = 0;
}

// ---------- main state ----------
typedef enum { ST_OFF=0, ST_PLAYING=1, ST_PAUSED=2 } tv_state_t;
static tv_state_t state = ST_OFF;

static void do_power_off(void) {
    stop_playback();
    fb_clear();
    backlight_off();
    state = ST_OFF;
    cancel_single_tap();
    log_console("power OFF");
}

static void do_power_on(void) {
    backlight_on();
    fb_clear();
    start_playback();
    state = ST_PLAYING;
    cancel_single_tap();
    log_console("power ON");
}

static void do_pause_toggle(void) {
    if (state == ST_PLAYING) {
        pause_ffmpeg();
        state = ST_PAUSED;
        log_console("pause");
    } else if (state == ST_PAUSED) {
        // opcional: meter estática al reanudar
        // static_noise_av();
        resume_ffmpeg();
        state = ST_PLAYING;
        log_console("resume");
    }
}

static void do_next_episode(void) {
    if (state != ST_PLAYING) return; // durante pausa no se permite
    stop_playback();
    static_noise_av();
    start_playback();
    state = ST_PLAYING;
    log_console("next");
}

// graceful stop
static volatile sig_atomic_t g_stop = 0;
static void on_sig(int s) { (void)s; g_stop = 1; }

// reap zombies (incluye el ffmpeg si muere solo)
static void reap_children(void) {
    int st;
    pid_t p;
    while ((p = waitpid(-1, &st, WNOHANG)) > 0) {
        if (p == ffmpeg_pid) {
            ffmpeg_pid = -1;
            ffmpeg_paused = false;
            if (state != ST_OFF) {
                // si se muere solo mientras estamos ON, lo relanzamos (modo TV)
                // ojo: si estabas PAUSED, no relanzamos.
                if (state == ST_PLAYING) {
                    log_console("ffmpeg died, restarting");
                    start_playback();
                }
            }
        }
    }
}

int main(void) {
    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);

    // init gpio
    switch_init();

    // open touch
    int tfd = open(TOUCH_DEV, O_RDONLY | O_CLOEXEC);
    if (tfd < 0) {
        log_console("ERROR opening %s: %s", TOUCH_DEV, strerror(errno));
        // seguimos, pero sin touch
    } else {
        int flags = fcntl(tfd, F_GETFL, 0);
        fcntl(tfd, F_SETFL, flags | O_NONBLOCK);
    }

    // initial power state
    if (switch_is_on()) do_power_on();
    else                do_power_off();

    touch_state_t ts;
    touch_state_reset(&ts);

    long long last_switch_poll = 0;

    while (!g_stop) {
        long long now = now_ms_monotonic();

        reap_children();

        // 1) switch poll
        if (now - last_switch_poll >= SWITCH_POLL_MS) {
            last_switch_poll = now;
            bool cur_on = switch_is_on();

            if (!cur_on && state != ST_OFF) {
                do_power_off();
            } else if (cur_on && state == ST_OFF) {
                do_power_on();
            }
        }

        // 2) touch read (solo si ON)
        if (tfd >= 0 && state != ST_OFF) {
            struct input_event ev;
            while (read(tfd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
                if (ev.type == EV_KEY && ev.code == BTN_TOUCH) {
                    if (ev.value == 1) {
                        ts.touching = true;
                        ts.t_down_ms = now_ms_monotonic();
                    } else if (ev.value == 0) {
                        long long t_up = now_ms_monotonic();
                        long long dt = t_up - ts.t_down_ms;

                        // HOLD lo tratamos como TAP (solo por si el touch a veces tarda)
                        (void)dt;

                        if (!pending_single) {
                            schedule_single_tap();
                        } else {
                            // segundo tap dentro de ventana => doble tap
                            cancel_single_tap();
                            do_next_episode();
                        }

                        ts.touching = false;
                        ts.t_down_ms = 0;
                    }
                }
            }
        }

        // 3) ejecutar single tap si expiró ventana y no hubo doble
        if (pending_single && now_ms_monotonic() >= pending_deadline_ms) {
            cancel_single_tap();
            do_pause_toggle();
        }

        ms_sleep(10);
    }

    // cleanup
    stop_playback();
    fb_clear();
    if (tfd >= 0) close(tfd);
    return 0;
}
