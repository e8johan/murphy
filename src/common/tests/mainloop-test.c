/*
 * Copyright (c) 2012, Intel Corporation
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *  * Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *  * Neither the name of Intel Corporation nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <signal.h>
#include <getopt.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <glib.h>
#include <dbus/dbus.h>

#include <murphy/config.h>
#include <murphy/common/macros.h>
#include <murphy/common/mm.h>
#include <murphy/common/mainloop.h>

#ifdef PULSE_ENABLED
#  include <pulse/mainloop.h>
#  include <murphy/common/pulse-glue.h>
#endif

#define info(fmt, args...) do {                                           \
        fprintf(stdout, "I: "fmt"\n" ,  ## args);                         \
        fflush(stdout);                                                   \
    } while (0)

#define warning(fmt, args...) do {                                        \
        fprintf(stderr, "W: "fmt"\n" ,  ## args);                         \
        fflush(stderr);                                                   \
    } while (0)

#define error(fmt, args...) do {                                          \
        fprintf(stderr, "E: "fmt"\n" ,  ## args);                         \
        fflush(stderr);                                                   \
    } while (0)

#define fatal(fmt, args...) do {                                          \
        fprintf(stderr, "C: "fmt"\n" ,  ## args);                         \
        fflush(stderr);                                                   \
        exit(1);                                                          \
    } while (0)

#define USECS_PER_SEC (1000 * 1000)

#define DEFAULT_RUNTIME 30                            /* run for 30 seconds */



typedef struct {
    int nio;
    int ntimer;
    int deferred;
    int nsignal;

    int ngio;
    int ngtimer;
    int ngidle;

    int ndbus_method;
    int ndbus_signal;

    int         log_mask;
    const char *log_target;

#ifdef PULSE_ENABLED
    pa_mainloop     *pa_main;
    pa_mainloop_api *pa;
#endif

    int nrunning;
    int runtime;
} test_config_t;


static test_config_t cfg;



/*
 * native timers
 */

#define TIMER_INTERVALS 1, 2, 3, 4, 6, 8, 1, 3, 12, 15, 18, 21, 24


typedef struct {
    int             id;
    mrp_timer_t    *timer;
    int             interval;
    int             count;
    int             target;
    struct timeval  prev;
} test_timer_t;


static test_timer_t *timers;


static int timeval_diff(struct timeval *tv1, struct timeval *tv2)
{
    int64_t u1, u2;

    u1 = tv1->tv_sec * USECS_PER_SEC + tv1->tv_usec;
    u2 = tv2->tv_sec * USECS_PER_SEC + tv2->tv_usec;

    return (int)(u1 - u2);
}


static void timeval_now(struct timeval *tv)
{
    gettimeofday(tv, NULL);
}


void timer_cb(mrp_mainloop_t *ml, mrp_timer_t *timer, void *user_data)
{
    test_timer_t   *t = (test_timer_t *)user_data;
    struct timeval  now;
    double          diff, error;

    MRP_UNUSED(ml);
    MRP_UNUSED(timer);

    timeval_now(&now);
    diff  = timeval_diff(&now, &t->prev) / 1000.0;
    error = diff - t->interval;
    if (error < 0.0)
        error = -error;

    info("MRPH timer #%d: %d/%d, diff %.2f (lag %.2f, %.3f %%)",
         t->id, t->count, t->target, diff, error, 100 * error / diff);

    t->count++;
    t->prev = now;

    if (t->count >= t->target) {
        info("MRPH timer #%d has finished.", t->id);

        mrp_del_timer(t->timer);
        t->timer = NULL;
        cfg.nrunning--;
    }
}


static void setup_timers(mrp_mainloop_t *ml)
{
    test_timer_t *t;
    int           intervals[] = { TIMER_INTERVALS, 0 }, *iv = intervals;
    int           msecs, i;

    if ((timers = mrp_allocz_array(test_timer_t, cfg.ntimer)) != NULL) {
        for (i = 0, t = timers; i < cfg.ntimer; i++, t++) {
            t->id = i;

            msecs = *iv;
            while (cfg.runtime / msecs < 1 && msecs > 0)
                msecs /= 2;
            msecs *= 1000;
            if (!msecs)
                msecs = 500;

            t->interval = msecs;
            t->target   = 1000 * cfg.runtime / msecs;
            if (!t->target)
                continue;

            timeval_now(&t->prev);
            t->timer = mrp_add_timer(ml, t->interval, timer_cb, t);

            if (t->timer != NULL)
                info("MRPH timer #%d: interval=%d, target=%d", t->id, *iv,
                     t->target);
            else
                fatal("MRPH timer #%d: failed to create", t->id);

            cfg.nrunning++;
            iv++;
            if (!*iv)
                iv = intervals;
        }
    }
    else
        fatal("could not allocate %d timers", cfg.ntimer);
}


static void check_timers(void)
{
    test_timer_t *t;
    int           i;

    for (i = 0, t = timers; i < cfg.ntimer; i++, t++) {
        if (t->target != 0 && t->count != t->target)
            warning("MRPH timer #%d: FAIL (only %d/%d)", t->id, t->count,
                    t->target);
        else
            info("MRPH timer #%d: OK (%d/%d)", t->id, t->count, t->target);
    }
}


/*
 * native I/O
 */

#define IO_INTERVALS    1, 3, 5, 9, 12, 15, 18, 21

typedef struct {
    int             id;
    int             pipe[2];
    mrp_io_watch_t *watch;
    mrp_timer_t    *timer;
    int             target;
    int             sent;
    int             received;
} test_io_t;


static test_io_t *ios;


static void send_io(mrp_mainloop_t *ml, mrp_timer_t *timer, void *user_data)
{
    test_io_t *w = (test_io_t *)user_data;
    char       buf[1024];
    int        plural, size;

    MRP_UNUSED(ml);
    MRP_UNUSED(timer);

    plural = (w->target - w->sent) != 1;
    size   = snprintf(buf, sizeof(buf),
                      "I/O #%d: %d message%s remain%s.", w->id,
                      w->target - w->sent,
                      plural ? "s" : "", plural ? "" : "s");

    if (write(w->pipe[1], buf, size) < 0) {
        /* just ignore it... */
    }
    w->sent++;

    info("MRPH I/O #%d: sent message %d/%d.", w->id, w->sent, w->target);

    if (w->sent >= w->target) {
        info("MRPH I/O #%d: sending done.", w->id);

        close(w->pipe[1]);
        mrp_del_timer(timer);
        w->timer = NULL;

        cfg.nrunning--;
    }
}


static void recv_io(mrp_mainloop_t *ml, mrp_io_watch_t *watch, int fd,
                    mrp_io_event_t events, void *user_data)
{
    test_io_t *w = (test_io_t *)user_data;
    char       buf[1024];
    int        size;

    MRP_UNUSED(ml);
    MRP_UNUSED(watch);

    if (watch != w->watch)
        fatal("MRPH I/O #%d called with incorrect data.", w->id);

    if (events & MRP_IO_EVENT_IN) {
        size = read(fd, buf, sizeof(buf) - 1);

        if (size > 0) {
            w->received++;
            buf[size] = '\0';
            info("MRPH I/O #%d: received message [%s]", w->id, buf);
        }
        else
            warning("MRPH I/O #%d: got empty message", w->id);
    }

    if (events & MRP_IO_EVENT_HUP) {
        info("MRPH I/O #%d: receiver done (got %d/%d)", w->id, w->received,
             w->sent);
        close(w->pipe[0]);
        mrp_del_io_watch(watch);
    }
}


void setup_io(mrp_mainloop_t *ml)
{
    test_io_t      *w;
    mrp_io_event_t  mask;
    int             intervals[] = { IO_INTERVALS, 0 }, *iv = intervals;
    int             msecs, i;


    if ((ios = mrp_allocz_array(test_io_t, cfg.nio)) != NULL) {
        mask = MRP_IO_EVENT_IN | MRP_IO_EVENT_HUP;

        for (i = 0, w = ios; i < cfg.nio; i++, w++) {
            w->id = i;

            msecs = *iv;
            while (cfg.runtime / msecs < 1 && msecs > 0)
                msecs /= 2;
            msecs *= 1000;
            if (!msecs)
                msecs = 500;

            w->target = 1000 * cfg.runtime / msecs;
            if (!w->target)
                continue;

            if (pipe(w->pipe) != 0)
                fatal("MRPH I/O #%d: could not create pipe", w->id);

            w->watch = mrp_add_io_watch(ml, w->pipe[0], mask, recv_io, w);
            w->timer = mrp_add_timer(ml, msecs, send_io, w);

            if (w->timer == NULL)
                fatal("MRPH I/O #%d: could not create I/O timer", w->id);

            if (w->watch == NULL)
                fatal("MRPH I/O #%d: could not create I/O watch", w->id);
            else
                info("MRPH I/O #%d: interval=%d, target=%d", w->id, *iv,
                     w->target);

            cfg.nrunning++;
            iv++;
            if (!*iv)
                iv = intervals;
        }
    }
    else
        fatal("could not allocate %d I/O watches", cfg.nio);
}


static void check_io(void)
{
    test_io_t *w;
    int        i;

    for (i = 0, w = ios; i < cfg.nio; i++, w++) {
        if (w->target != 0 && w->sent != w->received)
            warning("MRPH I/O #%d: FAIL (only %d/%d)", w->id, w->received,
                    w->sent);
        else
            info("MRPH I/O #%d: OK (%d/%d)", w->id, w->received, w->sent);
    }
}


/*
 * native deferred/idle callbacks
 */


static void setup_deferred(void)
{
    return;
}



/*
 * native signals
 */

#define SIG_INTERVALS   1, 5, 9, 3, 6, 12
#define SIGNUMS         { SIGUSR1, SIGUSR2, SIGTERM, SIGCONT, SIGQUIT, 0 }

static const char *signames[] = {
    [SIGINT]  = "SIGINT",  [SIGTERM] = "SIGTERM", [SIGQUIT] = "SIGQUIT",
    [SIGCONT] = "SIGCONT", [SIGUSR1] = "SIGUSR1", [SIGUSR2] = "SIGUSR2",
    [SIGCHLD] = "SIGCHLD"
};


typedef struct {
    int               id;
    int               signum;
    mrp_sighandler_t *watch;
    mrp_timer_t      *timer;
    int               target;
    int               sent;
    int               received;
} test_signal_t;

test_signal_t *signals;


static void send_signal(mrp_mainloop_t *ml, mrp_timer_t *timer, void *user_data)
{
    test_signal_t *t = (test_signal_t *)user_data;

    MRP_UNUSED(ml);
    MRP_UNUSED(timer);

    if (t->sent >= t->target)
        return;

    kill(getpid(), t->signum);
    t->sent++;
    info("MRPH signal #%d: sent signal %d/%d of %s", t->id,
         t->sent, t->target, strsignal(t->signum));

    if (t->sent >= t->target) {
        info("MRPH signal #%d: sending done", t->id);
        mrp_del_timer(t->timer);
        t->timer = NULL;
    }
}


static void recv_signal(mrp_mainloop_t *ml, mrp_sighandler_t *h, int signum,
                        void *user_data)
{
    test_signal_t *t = (test_signal_t *)user_data;

    MRP_UNUSED(ml);
    MRP_UNUSED(h);

    if (h != t->watch)
        fatal("MRPH signal #%d called with incorrect data", t->id);

    t->received++;
    info("MRPH signal #%d: received signal %d/%d of %s", t->id,
         t->received, t->target, signames[signum]);

    if (t->sent >= t->target) {
        info("MRPH signal #%d: receiving done", t->id);
        cfg.nrunning--;
    }
}


static void setup_signals(mrp_mainloop_t *ml)
{
    test_signal_t *t;
    int            intervals[] = { SIG_INTERVALS, 0 }, *iv = intervals;
    int            signums[] = SIGNUMS, *s = signums;
    int            msecs, i;

    if ((signals = mrp_allocz_array(test_signal_t, cfg.nsignal)) != NULL) {
        for (i = 0, t = signals; i < cfg.nsignal; i++, t++) {
            t->id = i;
            t->signum = *s;

            msecs = *iv;
            while (cfg.runtime / msecs < 1 && msecs > 0)
                msecs /= 2;
            msecs *= 1000;
            if (!msecs)
                msecs = 500;

            t->target = 1000 * cfg.runtime / msecs;
            if (!t->target)
                continue;

            t->watch = mrp_add_sighandler(ml, *s, recv_signal, t);
            t->timer = mrp_add_timer(ml, msecs, send_signal, t);

            if (t->timer == NULL)
                fatal("MRPH signal #%d: could not create timer", t->id);

            if (t->watch == NULL)
                fatal("MRPH signal #%d: could not create watch", t->id);
            else
                info("MRPH signal #%d: interval=%d, target=%d", t->id, *iv,
                     t->target);

            cfg.nrunning++;
            iv++;
            if (!*iv)
                iv = intervals;

            s++;
            if (!*s)
                s = signums;
        }
    }
    else
        fatal("could not allocate %d signal watches", cfg.nsignal);
}


static void check_signals(void)
{
    test_signal_t *t;
    int            i;

    for (i = 0, t = signals; i < cfg.nsignal; i++, t++) {
        if (t->sent < t->received)
            warning("MRPH signal #%d: FAIL (only %d/%d", t->id,
                    t->received, t->sent);
        else
            info("MRPH signal #%d: OK (%d/%d)", t->id, t->received, t->sent);
    }
}


static void check_quit(mrp_mainloop_t *ml, mrp_timer_t *timer, void *user_data)
{
    MRP_UNUSED(user_data);

    if (cfg.nrunning <= 0) {
        mrp_del_timer(timer);
#ifdef PULSE_ENABLED
        MRP_UNUSED(ml);

        if (cfg.pa_main != NULL)
            pa_mainloop_quit(cfg.pa_main, 0);
        else
#endif
        mrp_mainloop_quit(ml, 0);
    }
}




/*
 * glib timers
 */

#define GTIMER_INTERVALS 1, 2, 3, 4, 6, 8, 1, 3, 12, 15, 18, 21, 24

typedef struct {
    int            id;
    guint          gsrc;
    int            interval;
    int            count;
    int            target;
    struct timeval prev;
} glib_timer_t;


static glib_timer_t *gtimers;


static gboolean glib_timer_cb(gpointer user_data)
{
    glib_timer_t   *t = (glib_timer_t *)user_data;
    struct timeval  now;
    double          diff, error;

    timeval_now(&now);
    diff  = timeval_diff(&now, &t->prev) / 1000.0;
    error = diff - t->interval;
    if (error < 0.0)
        error = -error;

    info("GLIB timer #%d: %d/%d, diff %.2f (lag %.2f, %.3f %%)",
         t->id, t->count, t->target, diff, error, 100 * error / diff);

    t->count++;
    t->prev = now;

    if (t->count >= t->target) {
        info("GLIB timer #%d has finished.", t->id);

        t->gsrc = 0;
        cfg.nrunning--;
        return FALSE;
    }
    else
        return TRUE;
}


static void setup_glib_timers(void)
{
    glib_timer_t *t;
    int           intervals[] = { GTIMER_INTERVALS, 0 }, *iv = intervals;
    int           msecs, i;

    if ((gtimers = mrp_allocz_array(glib_timer_t, cfg.ntimer)) != NULL) {
        for (i = 0, t = gtimers; i < cfg.ngtimer; i++, t++) {
            t->id = i;

            msecs = *iv;
            while (cfg.runtime / msecs < 1 && msecs > 0)
                msecs /= 2;
            msecs *= 1000;
            if (!msecs)
                msecs = 500;

            t->interval = msecs;
            t->target   = 1000 * cfg.runtime / msecs;
            if (!t->target)
                continue;

            timeval_now(&t->prev);
            t->gsrc = g_timeout_add(msecs, glib_timer_cb, t);

            if (t->gsrc != 0)
                info("GLIB timer #%d: interval=%d, target=%d", t->id, *iv,
                     t->target);
            else
                fatal("GLIB timer #%d: failed to create", t->id);

            cfg.nrunning++;
            iv++;
            if (!*iv)
                iv = intervals;
        }
    }
    else
        fatal("could not allocate %d GLIB timers", cfg.ngtimer);
}


static void check_glib_timers(void)
{
    glib_timer_t *t;
    int           i;

    for (i = 0, t = gtimers; i < cfg.ntimer; i++, t++) {
        if (t->target != 0 && t->count != t->target)
            warning("GLIB timer #%d: FAIL (only %d/%d)", t->id, t->count,
                    t->target);
        else
            info("GLIB timer #%d: OK (%d/%d)", t->id, t->count, t->target);
    }
}


/*
 * glib I/O
 */

#define GIO_INTERVALS    1, 3, 4, 5, 6, 7, 9, 12, 15, 18, 21

typedef struct {
    int             id;
    int             pipe[2];
    GIOChannel     *gioc;
    guint           gsrc;
    guint           timer;
    int             target;
    int             sent;
    int             received;
} glib_io_t;


static glib_io_t *gios;


static gboolean glib_send_io(gpointer user_data)
{
    glib_io_t *t = (glib_io_t *)user_data;
    char       buf[1024];
    int        plural, size;

    plural = (t->target - t->sent) != 1;
    size   = snprintf(buf, sizeof(buf),
                      "I/O #%d: %d message%s remain%s.", t->id,
                      t->target - t->sent,
                      plural ? "s" : "", plural ? "" : "s");

    if (write(t->pipe[1], buf, size) < 0) {
        /* just ignore it... */
    }
    t->sent++;

    info("GLIB I/O #%d: sent message %d/%d.", t->id, t->sent, t->target);

    if (t->sent >= t->target) {
        info("GLIB I/O #%d: sending done.", t->id);

        close(t->pipe[1]);
        t->timer = 0;

        cfg.nrunning--;
        return FALSE;
    }
    else
        return TRUE;
}


static gboolean glib_recv_io(GIOChannel *ioc, GIOCondition cond,
                             gpointer user_data)
{
    glib_io_t *t  = (glib_io_t *)user_data;
    int        fd = g_io_channel_unix_get_fd(ioc);
    char       buf[1024];
    int        size;

    if (cond & G_IO_IN) {
        size = read(fd, buf, sizeof(buf) - 1);

        if (size > 0) {
            t->received++;
            buf[size] = '\0';
            info("GLIB I/O #%d: received message [%s]", t->id, buf);
        }
        else
            warning("GLIB I/O #%d: got empty message", t->id);
    }

    if (cond & G_IO_HUP) {
        info("GLIB I/O #%d: receiver done (got %d/%d)", t->id, t->received,
             t->sent);
        close(fd);
        return FALSE;
    }
    else
        return TRUE;
}


void setup_glib_io(void)
{
    glib_io_t    *t;
    GIOCondition  cond;
    int           intervals[] = { GIO_INTERVALS, 0 }, *iv = intervals;
    int           msecs, i;

    if ((gios = mrp_allocz_array(glib_io_t, cfg.ngio)) != NULL) {
        cond = G_IO_IN | G_IO_HUP;

        for (i = 0, t = gios; i < cfg.ngio; i++, t++) {
            t->id = i;

            msecs = *iv;
            while (cfg.runtime / msecs < 1 && msecs > 0)
                msecs /= 2;
            msecs *= 1000;
            if (!msecs)
                msecs = 500;

            t->target = 1000 * cfg.runtime / msecs;
            if (!t->target)
                continue;

            if (pipe(t->pipe) != 0)
                fatal("GLIB I/O #%d: could not create pipe", t->id);

            t->gioc = g_io_channel_unix_new(t->pipe[0]);
            if (t->gioc == NULL)
                fatal("GLIB I/O #%d: failed to create I/O channel", t->id);

            t->gsrc = g_io_add_watch(t->gioc, cond, glib_recv_io, t);
            if (t->gsrc == 0)
                fatal("GLIB I/O #%d: failed to add I/O watch", t->id);

            t->timer = g_timeout_add(msecs, glib_send_io, t);
            if (t->timer == 0)
                fatal("GLIB I/O #%d: could not create I/O timer", t->id);

            info("GLIB I/O #%d: interval=%d, target=%d", t->id, *iv,
                 t->target);

            cfg.nrunning++;
            iv++;
            if (!*iv)
                iv = intervals;
        }
    }
    else
        fatal("could not allocate %d glib I/O watches", cfg.ngio);
}


static void check_glib_io(void)
{
    glib_io_t *t;
    int        i;

    for (i = 0, t = gios; i < cfg.ngio; i++, t++) {
        if (t->target != 0 && t->sent != t->received)
            warning("GLIB I/O #%d: FAIL (only %d/%d)", t->id, t->received,
                    t->sent);
        else
            info("GLIB I/O #%d: OK (%d/%d)", t->id, t->received, t->sent);
    }
}


/*
 * DBUS tests
 */

#define DBUS_PATH   "/"
#define DBUS_IFACE  "org.murphy.test"
#define DBUS_METHOD "message"
#define DBUS_SIGNAL "signal"

typedef struct {
    int             pipe[2];
    pid_t           client;
    char            address[256];

    mrp_mainloop_t *ml;
    DBusConnection *conn;
    mrp_timer_t    *sigtimer;

    int             nmethod;
    int             nack;
    int             nsignal;
} dbus_test_t;


static void glib_pump_cleanup(void);

static dbus_test_t dbus_test = { pipe: { -1, -1 } };




int mrp_setup_dbus_connection(mrp_mainloop_t *ml, DBusConnection *conn);


static void open_dbus_pipe(void)
{
    if (pipe(dbus_test.pipe) < 0)
        fatal("failed to opend pipe for DBUS tests");
}


static void close_dbus_pipe(char *dir)
{
    while (*dir) {
        switch (*dir++) {
        case 'r':
            if (dbus_test.pipe[0] != -1) {
                close(dbus_test.pipe[0]);
                dbus_test.pipe[0] = -1;
            }
            break;

        case 'w':
            if (dbus_test.pipe[1] != -1) {
                close(dbus_test.pipe[1]);
                dbus_test.pipe[1] = -1;
            }
            break;
        }
    }
}


static void recv_dbus_reply(DBusPendingCall *pending, void *user_data)
{
    DBusMessage *msg;
    char        *reply;

    MRP_UNUSED(user_data);

    if ((msg = dbus_pending_call_steal_reply(pending)) != NULL) {
        if (dbus_message_get_args(msg, NULL, DBUS_TYPE_STRING,
                                  &reply, DBUS_TYPE_INVALID)) {
            info("DBUS test: got reply #%d '%s'", dbus_test.nack, reply);
            dbus_test.nack++;
        }

        dbus_message_unref(msg);
    }

    dbus_pending_call_unref(pending);

    if (dbus_test.nack >= cfg.ndbus_method) {
        char dummy[256];

        cfg.nrunning--;

        /* block until the client is done */
        if (read(dbus_test.pipe[0], dummy, sizeof(dummy)) < 0) {
            /* just ignore it... */
        }
    }
}


static int send_dbus_message(DBusConnection *conn, char *addr, char *buf)
{
    DBusMessage     *msg;
    DBusPendingCall *pending;

    msg = dbus_message_new_method_call(addr, DBUS_PATH,
                                       DBUS_IFACE, DBUS_METHOD);

    if (msg == NULL)
        fatal("failed to create DBUS message");

    if (!dbus_message_append_args(msg,
                                  DBUS_TYPE_STRING, &buf, DBUS_TYPE_INVALID))
        fatal("failed to add arguments to DBUS method call");

    if (!dbus_connection_send_with_reply(conn, msg, &pending, 5000))
        fatal("failed to send DBUS message");

    if (!dbus_pending_call_set_notify(pending, recv_dbus_reply, NULL, NULL))
        fatal("failed to set pending call notification callback");

    dbus_message_unref(msg);

    return TRUE;
}


static int send_dbus_reply(DBusConnection *conn, DBusMessage *msg, char *buf)
{
    DBusMessage *reply;

    if ((reply = dbus_message_new_method_return(msg)) != NULL) {
        if (!dbus_message_append_args(reply, DBUS_TYPE_STRING, &buf,
                                      DBUS_TYPE_INVALID))
            fatal("failed to add arguments to DBUS method reply");

        if (!dbus_connection_send(conn, reply, NULL))
            fatal("failed to send DBUS reply");

        dbus_message_unref(reply);
    }

    dbus_test.nmethod++;
    if (dbus_test.nmethod >= cfg.ndbus_method)
        cfg.nrunning--;

    return TRUE;
}


static DBusConnection *connect_to_dbus(char *name)
{
    DBusConnection *conn;
    DBusError       error;
    unsigned int    flags;
    int             status;

    dbus_error_init(&error);

    if ((conn = dbus_bus_get(DBUS_BUS_SESSION, NULL)) != NULL) {
        if (!name || !*name)
            return conn;

        flags  = DBUS_NAME_FLAG_REPLACE_EXISTING | DBUS_NAME_FLAG_DO_NOT_QUEUE;
        status = dbus_bus_request_name(conn, name, flags, &error);

        if (status == DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER)
            return conn;
        else
            error("failed to get name '%s' on DBUS (error: %s)", name,
                  error.message ? error.message : "unknown");
    }

    return NULL;
}


static void client_send_msg(mrp_mainloop_t *ml, mrp_timer_t *t,
                            void *user_data)
{
    char buf[1024];

    MRP_UNUSED(ml);
    MRP_UNUSED(user_data);


    snprintf(buf, sizeof(buf), "DBUS message #%d", dbus_test.nmethod);
    send_dbus_message(dbus_test.conn, dbus_test.address, buf);

    info("DBUS client: sent #%d message", dbus_test.nmethod);

    dbus_test.nmethod++;

    if (dbus_test.nmethod >= cfg.ndbus_method)
        mrp_del_timer(t);
    /* cfg.nrunning updated only once we've received the last reply */
}


static void setup_dbus_client(mrp_mainloop_t *ml)
{
    DBusConnection *conn;
    int             i, nmethod, nsignal;
    size_t          size;

    nmethod = cfg.ndbus_method;
    nsignal = cfg.ndbus_signal;
    mrp_clear(&cfg);
    cfg.ndbus_method = nmethod;
    cfg.ndbus_signal = nsignal;

    mrp_mainloop_quit(ml, 0);
    glib_pump_cleanup();
    mrp_mainloop_destroy(ml);

    for (i = 3; i < 1024; i++)
        if (i != dbus_test.pipe[0])
            close(i);

    size = sizeof(dbus_test.address);
    if (read(dbus_test.pipe[0], dbus_test.address, size) > 0) {
        info("DBUS test: got address '%s'", dbus_test.address);
    }

    /*sleep(5);*/

    if ((ml = dbus_test.ml = mrp_mainloop_create()) == NULL)
        fatal("failed to create mainloop");

    if ((conn = dbus_test.conn = connect_to_dbus(NULL)) == NULL)
        fatal("failed to connect to DBUS");

    if (!mrp_setup_dbus_connection(ml, conn))
        fatal("failed to setup DBUS connection with mainloop");

    if (mrp_add_timer(ml, 1000, client_send_msg, NULL) == NULL)
        fatal("failed to create DBUS message sending timer");

    if (mrp_add_timer(ml, 1000, check_quit, NULL) == NULL)
        fatal("failed to create quit-check timer");

    cfg.nrunning++;
}


static DBusHandlerResult dispatch_method(DBusConnection *c,
                                         DBusMessage *msg, void *data)
{
#define SAFESTR(str) (str ? str : "<none>")
    const char *path      = dbus_message_get_path(msg);
    const char *interface = dbus_message_get_interface(msg);
    const char *member    = dbus_message_get_member(msg);
    const char *message;
    char        reply[1024];

    MRP_UNUSED(data);

    if (dbus_message_get_type(msg) != DBUS_MESSAGE_TYPE_METHOD_CALL || !member)
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    if (strcmp(path, DBUS_PATH) ||
        strcmp(interface, DBUS_IFACE) ||
        strcmp(member, DBUS_METHOD))
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    /*info("DBUS server: got call: path='%s', interface='%s', member='%s')...",
      SAFESTR(path), SAFESTR(interface), SAFESTR(member));*/

    if (dbus_message_get_args(msg, NULL,
                              DBUS_TYPE_STRING, &message, DBUS_TYPE_INVALID)) {
        snprintf(reply, sizeof(reply), "ACK: got '%s'", message);
        if (!send_dbus_reply(c, msg, reply))
            fatal("failed to sent reply to DBUS message");
    }

    return DBUS_HANDLER_RESULT_HANDLED;
}



static void setup_dbus_server(mrp_mainloop_t *ml)
{
    static struct DBusObjectPathVTable vtable = {
        .message_function = dispatch_method
    };

    char *addr = "org.murphy.test";

    MRP_UNUSED(ml);

    if ((dbus_test.conn = connect_to_dbus(addr)) == NULL)
        fatal("failed to connect to DBUS");

    if (!mrp_setup_dbus_connection(ml, dbus_test.conn))
        fatal("failed to setup DBUS connection with mainloop");

    if (!dbus_connection_register_fallback(dbus_test.conn, "/", &vtable, NULL))
        fatal("failed to set up method dispatching");

    if (write(dbus_test.pipe[1], addr, strlen(addr) + 1) < 0) {
        /* just ignore it... */
    }

    cfg.nrunning++;
}



static void fork_dbus_client(mrp_mainloop_t *ml)
{
    dbus_test.client = fork();

    switch (dbus_test.client) {
    case -1:
        fatal("failed to fork DBUS test client");
        break;

    case 0:
        setup_dbus_client(ml);
        break;

    default:
        info("DBUS test: child pid %u", dbus_test.client);
        close(0);
        /*sleep(10);*/
        setup_dbus_server(ml);
    }
}


static void sigchild_handler(mrp_mainloop_t *ml, mrp_sighandler_t *h,
                             int signum, void *user_data)
{
    int status;

    MRP_UNUSED(ml);
    MRP_UNUSED(user_data);

    info("DBUS test: received signal %d (%s)", signum, signames[signum]);

    if (dbus_test.client != 0) {
        if (waitpid(dbus_test.client, &status, WNOHANG) == dbus_test.client) {
            info("DBUS test: client exited with status %d.", status);
            dbus_test.client = 0;
            close_dbus_pipe("w");
            mrp_del_sighandler(h);
            cfg.nrunning--;
        }
        else
            error("waitpid failed for pid %u", dbus_test.client);
    }
}


static void setup_dbus_tests(mrp_mainloop_t *ml)
{
    mrp_sighandler_t *h;

    if ((h = mrp_add_sighandler(ml, SIGCHLD, sigchild_handler, NULL)) != NULL) {
        open_dbus_pipe();
        fork_dbus_client(ml);
    }
    else
        fatal("failed create SIGCHLD handler");
}


static void check_dbus(void)
{
    if (dbus_test.client != 0) {
        if (dbus_test.nmethod == cfg.ndbus_method)
            info("DBUS test: method calls: OK (%d/%d)",
                 dbus_test.nmethod, cfg.ndbus_method);
        else
            error("DBUS test: method calls: FAILED (%d/%d)",
                  dbus_test.nmethod, cfg.ndbus_method);
    }
    else {
        if (dbus_test.nack == cfg.ndbus_method)
            info("DBUS test: method replies: OK (%d/%d)",
                 dbus_test.nack, cfg.ndbus_method);
        else
            error("DBUS test: method replies: FAILED (%d/%d)",
                  dbus_test.nack, cfg.ndbus_method);
    }
}



#include "glib-pump.c"
#include "dbus-pump.c"



static void config_set_defaults(test_config_t *cfg)
{
    mrp_clear(cfg);

    cfg->nio     = 5;
    cfg->ntimer  = 10;
    cfg->nsignal = 5;
    cfg->ngio    = 5;
    cfg->ngtimer = 10;

    cfg->ndbus_method = 10;
    cfg->ndbus_signal = 10;

    cfg->log_mask   = MRP_LOG_UPTO(MRP_LOG_DEBUG);
    cfg->log_target = MRP_LOG_TO_STDERR;

    cfg->runtime = DEFAULT_RUNTIME;
}


static void print_usage(const char *argv0, int exit_code, const char *fmt, ...)
{
    va_list ap;

    if (fmt && *fmt) {
        va_start(ap, fmt);
        vprintf(fmt, ap);
        va_end(ap);
    }

    printf("usage: %s [options]\n\n"
           "The possible options are:\n"
           "  -r, --runtime                  how many seconds to run tests\n"
           "  -i, --ios                      number of I/O watches\n"
           "  -t, --timers                   number of timers\n"
           "  -I, --glib-ios                 number of glib I/O watches\n"
           "  -T, --glib-timers              number of glib timers\n"
           "  -S, --dbus-signals             number of D-Bus signals\n"
           "  -M, --dbus-methods             number of D-Bus methods\n"
           "  -o, --log-target=TARGET        log target to use\n"
           "      TARGET is one of stderr,stdout,syslog, or a logfile path\n"
           "  -l, --log-level=LEVELS         logging level to use\n"
           "      LEVELS is a comma separated list of info, error and warning\n"
           "  -v, --verbose                  increase logging verbosity\n"
           "  -d, --debug                    enable debug messages\n"
#ifdef PULSE_ENABLED
           "  -p, --pulse                    use pulse mainloop\n"
#endif
           "  -h, --help                     show help on usage\n",
           argv0);

    if (exit_code < 0)
        return;
    else
        exit(exit_code);
}


int parse_cmdline(test_config_t *cfg, int argc, char **argv)
{
#ifdef PULSE_ENABLED
#   define PULSE_OPTION "p"
#else
#   define PULSE_OPTION ""
#endif
#   define OPTIONS "r:i:t:s:I:T:S:M:l:o:vdh"PULSE_OPTION
    struct option options[] = {
        { "runtime"     , required_argument, NULL, 'r' },
        { "ios"         , required_argument, NULL, 'i' },
        { "timers"      , required_argument, NULL, 't' },
        { "signals"     , required_argument, NULL, 's' },
        { "glib-ios"    , required_argument, NULL, 'I' },
        { "glib-timers" , required_argument, NULL, 'T' },
        { "dbus-signals", required_argument, NULL, 'S' },
        { "dbus-methods", required_argument, NULL, 'M' },
#ifdef PULSE_ENABLED
        { "pulse-main"  , no_argument      , NULL, 'p' },
#endif
        { "log-level"   , required_argument, NULL, 'l' },
        { "log-target"  , required_argument, NULL, 'o' },
        { "verbose"     , optional_argument, NULL, 'v' },
        { "debug"       , no_argument      , NULL, 'd' },
        { "help"        , no_argument      , NULL, 'h' },
        { NULL, 0, NULL, 0 }
    };
    char *end;
    int   opt, debug;

    debug = FALSE;
    config_set_defaults(cfg);

    while ((opt = getopt_long(argc, argv, OPTIONS, options, NULL)) != -1) {
        switch (opt) {
        case 'r':
            cfg->runtime = (int)strtoul(optarg, &end, 10);
            if (end && *end)
                print_usage(argv[0], EINVAL,
                            "invalid runtime length '%s'.", optarg);
            break;

        case 'i':
            cfg->nio = (int)strtoul(optarg, &end, 10);
            if (end && *end)
                print_usage(argv[0], EINVAL,
                            "invalid number of I/O watches '%s'.", optarg);
            break;

        case 't':
            cfg->ntimer = (int)strtoul(optarg, &end, 10);
            if (end && *end)
                print_usage(argv[0], EINVAL,
                            "invalid number of timers '%s'.", optarg);
            break;

        case 's':
            cfg->nsignal = (int)strtoul(optarg, &end, 10);
            if (end && *end)
                print_usage(argv[0], EINVAL,
                            "invalid number of signals '%s'.", optarg);
            break;

        case 'I':
            cfg->ngio = (int)strtoul(optarg, &end, 10);
            if (end && *end)
                print_usage(argv[0], EINVAL,
                            "invalid number of glib I/O watches '%s'.", optarg);
            break;

        case 'T':
            cfg->ngtimer = (int)strtoul(optarg, &end, 10);
            if (end && *end)
                print_usage(argv[0], EINVAL,
                            "invalid number of glib timers '%s'.", optarg);
            break;

        case 'S':
            cfg->ndbus_signal = (int)strtoul(optarg, &end, 10);
            if (end && *end)
                print_usage(argv[0], EINVAL,
                            "invalid number of DBUS signals '%s'.", optarg);
            break;

        case 'M':
            cfg->ndbus_method = (int)strtoul(optarg, &end, 10);
            if (end && *end)
                print_usage(argv[0], EINVAL,
                            "invalid number of DBUS methods '%s'.", optarg);
            break;

#ifdef PULSE_ENABLED
        case 'p':
            cfg->pa_main = pa_mainloop_new();
            if (cfg->pa_main == NULL) {
                mrp_log_error("Failed to create PulseAudio mainloop.");
                exit(1);
            }
            cfg->pa = pa_mainloop_get_api(cfg->pa_main);
            break;
#endif

        case 'v':
            cfg->log_mask <<= 1;
            cfg->log_mask  |= 1;
            break;

        case 'l':
            cfg->log_mask = mrp_log_parse_levels(optarg);
            if (cfg->log_mask < 0)
                print_usage(argv[0], EINVAL, "invalid log level '%s'", optarg);
            break;

        case 'o':
            cfg->log_target = mrp_log_parse_target(optarg);
            if (!cfg->log_target)
                print_usage(argv[0], EINVAL, "invalid log target '%s'", optarg);
            break;

        case 'd':
            debug = TRUE;
            break;

        case 'h':
            print_usage(argv[0], -1, "");
            exit(0);
            break;

        default:
            print_usage(argv[0], EINVAL, "invalid option '%c'", opt);
        }
    }

    if (debug)
        cfg->log_mask |= MRP_LOG_MASK_DEBUG;

    return TRUE;
}


int main(int argc, char *argv[])
{
    mrp_mainloop_t *ml;
    int             retval;

    mrp_clear(&cfg);
    parse_cmdline(&cfg, argc, argv);

    mrp_log_set_mask(cfg.log_mask);
    mrp_log_set_target(cfg.log_target);

    if ((ml = mrp_mainloop_create()) == NULL)
        fatal("failed to create main loop.");

    setup_timers(ml);
    setup_io(ml);
    setup_signals(ml);

    glib_pump_setup(ml);

    setup_glib_io();
    setup_glib_timers();

    dbus_test.ml = ml;
    setup_dbus_tests(ml);
    ml = dbus_test.ml;

    if (mrp_add_timer(ml, 1000, check_quit, NULL) == NULL)
        fatal("failed to create quit-check timer");

#ifdef PULSE_ENABLED
    if (cfg.pa != NULL) {
        mrp_log_info("Running with PulseAudio mainloop.");

        if (!mrp_mainloop_register_with_pulse(ml, cfg.pa)) {
            mrp_log_error("Failed to register with PulseAudio mainloop.");
            exit(1);
        }

        pa_mainloop_run(cfg.pa_main, &retval);

        mrp_log_info("PulseAudio mainloop exited with status %d.", retval);

        mrp_mainloop_unregister_from_pulse(ml, cfg.pa);

        pa_mainloop_free(cfg.pa_main);
    }
    else
#endif
        retval = mrp_mainloop_run(ml);

    check_io();
    check_timers();
    check_signals();

    check_glib_io();
    check_glib_timers();

    if (dbus_test.client != 0)
        close(dbus_test.pipe[1]);   /* let the client continue */

    check_dbus();

    glib_pump_cleanup();

    mrp_mainloop_destroy(ml);
}
