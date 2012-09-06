#include <stdarg.h>
#include <errno.h>

#include <murphy/common/mm.h>
#include <murphy/common/debug.h>
#include <murphy/common/log.h>

#include "scanner.h"
#include "resolver-types.h"
#include "target.h"
#include "target-sorter.h"
#include "fact.h"
#include "resolver.h"

mrp_resolver_t *mrp_resolver_parse(const char *path)
{
    yy_res_parser_t  parser;
    mrp_resolver_t  *r;

    mrp_clear(&parser);
    r = mrp_allocz(sizeof(*r));

    if (r != NULL) {
        if (parser_parse_file(&parser, path)) {
            if (create_targets(r, &parser) == 0 &&
                sort_targets(r)            == 0 &&
                compile_target_scripts(r)  == 0 &&
                (r->ctbl = mrp_create_context_table()) != NULL) {
                parser_cleanup(&parser);
                return r;
            }
        }
        else
            mrp_log_error("Failed to parse resolver input.");
    }

    mrp_resolver_destroy(r);
    parser_cleanup(&parser);

    return NULL;
}


void mrp_resolver_destroy(mrp_resolver_t *r)
{
    if (r != NULL) {
        mrp_destroy_context_table(r->ctbl);
        destroy_targets(r);
        destroy_facts(r);

        mrp_free(r);
    }
}


int mrp_resolver_update_targetl(mrp_resolver_t *r, const char *target, ...)
{
    const char         *name;
    mrp_script_value_t  value;
    va_list             ap;
    int                 id, status;

    if (mrp_push_context_frame(r->ctbl) == 0) {
        va_start(ap, target);
        while ((name = va_arg(ap, char *)) != NULL) {
            id = mrp_get_context_id(r->ctbl, name);

            if (id > 0) {
                value.type = va_arg(ap, int);

#define         HANDLE_TYPE(_type, _member, _va_type)                   \
                case MRP_SCRIPT_TYPE_##_type:                           \
                    value._member =                                     \
                        (typeof(value._member))va_arg(ap, _va_type);    \
                    break

                switch (value.type) {
                    HANDLE_TYPE(STRING, str, char *  );
                    HANDLE_TYPE(BOOL  , bln, int     );
                    HANDLE_TYPE(UINT8 ,  u8, uint32_t);
                    HANDLE_TYPE(SINT8 ,  s8, int32_t );
                    HANDLE_TYPE(UINT16, u16, uint32_t);
                    HANDLE_TYPE(SINT16, s16, int32_t );
                    HANDLE_TYPE(UINT32, u32, uint32_t);
                    HANDLE_TYPE(SINT32, s32, int32_t );
                    HANDLE_TYPE(UINT64, u64, uint64_t);
                    HANDLE_TYPE(SINT64, u64, uint64_t);
                    HANDLE_TYPE(DOUBLE, dbl, double  );
                default:
                    errno  = EINVAL;
                    status = -1;
                    goto pop_frame;
                }
#undef          HANDLE_TYPE

                if (mrp_set_context_value(r->ctbl, id, &value) < 0) {
                    status = -1;
                    goto pop_frame;
                }
            }
            else {
                errno  = ESRCH;
                status = -1;
                goto pop_frame;
            }
        }

        status = update_target_by_name(r, target);

    pop_frame:
        mrp_pop_context_frame(r->ctbl);
        va_end(ap);
    }
    else
        status = -1;

    return status;
}


int mrp_resolver_update_targetv(mrp_resolver_t *r, const char *target,
                                const char **variables,
                                mrp_script_value_t *values,
                                int nvariable)
{
    const char         *name;
    mrp_script_value_t *value;
    int                 id, i, status;

    if (mrp_push_context_frame(r->ctbl) == 0) {
        for (i = 0; i < nvariable; i++) {
            name  = variables[i];
            value = values + i;
            id    = mrp_get_context_id(r->ctbl, name);

            if (id > 0) {
                if (mrp_set_context_value(r->ctbl, id, value) < 0) {
                    status = -1;
                    goto pop_frame;
                }
            }
            else {
                errno  = ESRCH;
                status = -1;
                goto pop_frame;
            }
        }

        status = update_target_by_name(r, target);

    pop_frame:
        mrp_pop_context_frame(r->ctbl);
    }
    else
        status = -1;

    return status;
}


void mrp_resolver_dump_targets(mrp_resolver_t *r, FILE *fp)
{
    dump_targets(r, fp);
}


void mrp_resolver_dump_facts(mrp_resolver_t *r, FILE *fp)
{
    int     i;
    fact_t *f;

    fprintf(fp, "%d facts\n", r->nfact);
    for (i = 0; i < r->nfact; i++) {
        f = r->facts + i;
        fprintf(fp, "  #%d: %s\n", i, f->name);
    }
}


int mrp_resolver_register_interpreter(mrp_interpreter_t *i)
{
    return mrp_register_interpreter(i);
}


int mrp_resolver_unregister_interpreter(const char *name)
{
    return mrp_unregister_interpreter(name);
}


int mrp_resolver_declare_variable(mrp_resolver_t *r, const char *name,
                                  mrp_script_type_t type)
{
    return mrp_declare_context_variable(r->ctbl, name, type);
}


int mrp_resolver_get_value(mrp_resolver_t *r, int id, mrp_script_value_t *v)
{
    return mrp_get_context_value(r->ctbl, id, v);
}


int mrp_resolver_get_value_by_name(mrp_resolver_t *r, const char *name,
                                   mrp_script_value_t *v)
{
    return mrp_get_context_value(r->ctbl, mrp_get_context_id(r->ctbl, name), v);
}
