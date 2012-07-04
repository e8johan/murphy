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
#include <stddef.h>
#include <string.h>
#include <stdarg.h>

#include <murphy/common/log.h>

static int log_mask = MRP_LOG_MASK_ERROR;
static const char *log_target = MRP_LOG_TO_STDERR;
static FILE *log_fp;


mrp_log_mask_t mrp_log_parse_levels(const char *levels)
{
    const char *p;
    mrp_log_mask_t mask;

    mask = 0;

    if (levels == NULL) {
        if (mask == 0)
            mask = 1;
        else {
            mask <<= 1;
            mask  |= 1;
        }
    }
    else {
        p = levels;
        while (p && *p) {
#           define MATCHES(s, l) (!strcmp(s, l) ||                      \
                                  !strncmp(s, l",", sizeof(l",") - 1))

            if (MATCHES(p, "info"))
                mask |= MRP_LOG_MASK_INFO;
            else if (MATCHES(p, "error"))
                mask |= MRP_LOG_MASK_ERROR;
            else if (MATCHES(p, "warning"))
                mask |= MRP_LOG_MASK_WARNING;
            else
                return -1;

            if ((p = strchr(p, ',')) != NULL)
                p += 1;

#           undef MATCHES
        }
    }

    return mask;
}


const char *mrp_log_parse_target(const char *target)
{
    if (!strcmp(target, "stderr"))
        return MRP_LOG_TO_STDERR;
    if (!strcmp(target, "stdout"))
        return MRP_LOG_TO_STDOUT;
    if (!strcmp(target, "syslog"))
        return MRP_LOG_TO_SYSLOG;
    else
        return target;
}


mrp_log_mask_t mrp_log_enable(mrp_log_mask_t enabled)
{
    mrp_log_mask_t old_mask = log_mask;

    log_mask |= enabled;

    return old_mask;
}


mrp_log_mask_t mrp_log_disable(mrp_log_mask_t disabled)
{
    mrp_log_mask_t old_mask = log_mask;

    log_mask &= ~disabled;

    return old_mask;
}


mrp_log_mask_t mrp_log_set_mask(mrp_log_mask_t enabled)
{
    mrp_log_mask_t old_mask = log_mask;

    log_mask = enabled;

    return old_mask;
}


int mrp_log_set_target(const char *target)
{
    const char *old_target = log_target;
    FILE       *old_fp     = log_fp;

    if (!target || log_target == target)
        return FALSE;

    if (target == MRP_LOG_TO_SYSLOG) {
        log_target = target;
        log_fp     = NULL;
        openlog(NULL, 0, LOG_DAEMON);
    }
    else if (target == MRP_LOG_TO_STDERR) {
        log_target = target;
        log_fp     = stdout;
    }
    else if (target == MRP_LOG_TO_STDOUT) {
        log_target = target;
        log_fp     = stdout;
    }
    else {
        log_target = target;
        log_fp     = fopen(log_target, "a");

        if (log_fp == NULL) {
            log_target = old_target;
            log_fp     = old_fp;

            return FALSE;
        }
    }

    if (old_target == MRP_LOG_TO_SYSLOG)
        closelog();
    else if (old_target != MRP_LOG_TO_STDOUT && old_target != MRP_LOG_TO_STDERR)
        fclose(old_fp);

    return TRUE;
}


void mrp_log_msgv(mrp_log_level_t level, const char *file,
                  int line, const char *func, const char *format, va_list ap)
{
    int         lvl;
    const char *prefix;
    char        prfx[2*1024];

    (void)file;
    (void)line;

    if (!(log_mask & (1 << level)))
        return;

    switch (level) {
    case MRP_LOG_ERROR:   lvl = LOG_ERR;     prefix = "E: "; break;
    case MRP_LOG_WARNING: lvl = LOG_WARNING; prefix = "W: "; break;
    case MRP_LOG_INFO:    lvl = LOG_INFO;    prefix = "I: "; break;
    case MRP_LOG_DEBUG:   lvl = LOG_INFO;
        snprintf(prfx, sizeof(prfx) - 1, "D: [%s] ", func);
        prfx[sizeof(prfx)-1] = '\0';
        prefix = prfx;
        break;
    default:
        return;
    }

    if (log_fp == NULL)
        vsyslog(lvl, format, ap);
    else {
        fputs(prefix, log_fp);
        vfprintf(log_fp, format, ap); fputs("\n", log_fp);
        fflush(log_fp);
    }
}


void mrp_log_msg(mrp_log_level_t level, const char *file,
                 int line, const char *func, const char *format, ...)
{
    va_list     ap;

    (void)file;
    (void)line;

    if (!(log_mask & (1 << level)))
        return;

    va_start(ap, format);
    mrp_log_msgv(level, file, line, func, format, ap);
    va_end(ap);
}


/*
 * workaround for not being able to initialize log_fp to stderr
 */

static __attribute__((constructor)) void set_default_logging(void)
{
    log_target = MRP_LOG_TO_STDERR;
    log_fp     = stderr;
}
