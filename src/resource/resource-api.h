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

#ifndef __MURPHY_RESOURCE_API_H__
#define __MURPHY_RESOURCE_API_H__

#include <resource/data-types.h>

mrp_resource_client_t *mrp_resource_client_create(const char *name,
                                                  void *user_data);
void mrp_resource_client_destroy(mrp_resource_client_t *client);

int mrp_zone_definition_create(mrp_attr_def_t *attrdefs);
uint32_t mrp_zone_create(const char *name, mrp_attr_def_t *attrs);

mrp_resource_class_t *mrp_resource_class_create(const char *name,
                                                uint32_t priority);
void mrp_resource_class_add_resource_set(mrp_resource_class_t *class,
                                         uint32_t zone_id,
                                         mrp_resource_set_t *resource_set);
int mrp_resource_class_print(char *buf, int len);

uint32_t mrp_resource_definition_create(const char *name,
                                        bool shareable,
                                        mrp_attr_def_t *attrdefs);
mrp_resource_t *mrp_resource_create(const char *name, bool shared,
                                    mrp_attr_def_t *attrs);

mrp_resource_set_t *mrp_resource_set_create(uint32_t client_id,
                                            void *client_data,
                                            uint32_t priority);
int mrp_resource_set_add_resource(mrp_resource_set_t *resource_set,
                                  const char *resource_name,
                                  bool shared,
                                  mrp_attr_def_t *attrs,
                                  bool mandatory);
void mrp_resource_set_acquire(mrp_resource_set_t *resource_set);
void mrp_resource_set_release(mrp_resource_set_t *resource_set);


int mrp_resource_owner_print(char *buf, int len);



#endif  /* __MURPHY_RESOURCE_API_H__ */

/*
 * Local Variables:
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 *
 */
