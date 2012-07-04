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

#ifndef __MURPHY_DEBUG_H__
#define __MURPHY_DEBUG_H__

#include <stdio.h>

#include <murphy/common/macros.h>
#include <murphy/common/debug-info.h>

MRP_CDECL_BEGIN

/** Macro to generate a debug site string. */
#define MRP_DEBUG_SITE(file, line, func)                                  \
    "__DEBUG_SITE_"file":"MRP_STRINGIFY(line)

/** Log a debug message if the invoking debug site is enabled. */
#define mrp_debug(fmt, args...)        do {                               \
        static const char *__site =                                       \
            MRP_DEBUG_SITE(__FILE__, __LINE__, __FUNCTION__);             \
        static int __site_stamp = -1;                                     \
        static int __site_enabled;                                        \
                                                                          \
        if (MRP_UNLIKELY(__site_stamp != mrp_debug_stamp)) {              \
            __site_enabled = mrp_debug_check(__FUNCTION__,                \
                                             __FILE__, __LINE__);         \
            __site_stamp   = mrp_debug_stamp;                             \
        }                                                                 \
                                                                          \
        if (MRP_UNLIKELY(__site_enabled))                                 \
            mrp_debug_msg(__site, __LOC__, fmt, ## args);                 \
    } while (0)

/** Global debug configuration stamp, exported for minimum-overhead checking. */
extern int mrp_debug_stamp;

/** Enable/disable debug messages globally. */
int mrp_debug_enable(int enabled);

/** Reset all debug configuration to the defaults. */
void mrp_debug_reset(void);

/** Apply the debug configuration settings given in cmd. */
int mrp_debug_set_config(const char *cmd);

/** Dump the active debug configuration. */
int mrp_debug_dump_config(FILE *fp);

/** Low-level log wrapper for debug messages. */
void mrp_debug_msg(const char *site, const char *file, int line,
                   const char *func, const char *format, ...);

/** Check if the given debug site is enabled. */
int mrp_debug_check(const char *func, const char *file, int line);

/** Register line->funcion mapping for file. */
int mrp_debug_register_file(mrp_debug_file_t *df);

/** Unregister line->funcion mapping for file. */
int mrp_debug_unregister_file(mrp_debug_file_t *df);

/** Return the name of the function that corresponds to file:line. */
const char *mrp_debug_site_function(const char *file, int line);

MRP_CDECL_END

#endif /* __MURPHY_DEBUG_H__ */
