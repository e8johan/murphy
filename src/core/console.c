#define _GNU_SOURCE                      /* we want fopencookie */
#include <stdio.h>
#include <stdarg.h>
#include <unistd.h>
#include <errno.h>

#include <murphy/common/mm.h>
#include <murphy/common/list.h>
#include <murphy/common/log.h>
#include <murphy/common/msg.h>
#include <murphy/common/transport.h>

#include <murphy/core/console.h>

#define MAX_PROMPT 64                    /* ie. way too long */

#define COLOR  "\E"
#define YELLOW "33m"
#define WHITE  "37m"
#define RED    "31m"

#define CNORM  COLOR""WHITE
#define CWARN  COLOR""YELLOW
#define CERR   COLOR""RED

#define MRP_CFG_MAXLINE 4096             /* input line length limit */
#define MRP_CFG_MAXARGS   64             /* command argument limit */

typedef struct {
    char  buf[MRP_CFG_MAXLINE];          /* input buffer */
    char *token;                         /* current token */
    char *in;                            /* filling pointer */
    char *out;                           /* consuming pointer */
    char *next;                          /* next token buffer position */
    int   fd;                            /* input file */
    int   error;                         /* whether has encounted and error */
    char *file;                          /* file being processed */
    int   line;                          /* line number */
    int   next_newline;
    int   was_newline;
} input_t;

static int get_next_line(input_t *in, char **args, size_t size);



/*
 * an active console
 */

typedef struct {
    MRP_CONSOLE_PUBLIC_FIELDS;               /* publicly visible fields */
    mrp_console_group_t *grp;                /* active group if any */
    char                 prompt[MAX_PROMPT]; /* current prompt */
    input_t              in;                 /* input buffer */
    mrp_list_hook_t      hook;               /* to list of active consoles */
} console_t;


static int check_destroy(mrp_console_t *mc);
static int purge_destroyed(mrp_console_t *mc);
static FILE *console_fopen(mrp_console_t *mc);

static ssize_t input_evt(mrp_console_t *mc, void *buf, size_t size);
static void disconnected_evt(mrp_console_t *c, int error);
static ssize_t complete_evt(mrp_console_t *c, void *input, size_t isize,
			    char **completions, size_t csize);



static void register_commands(mrp_context_t *ctx);
static void unregister_commands(mrp_context_t *ctx);

void console_setup(mrp_context_t *ctx)
{
    mrp_list_init(&ctx->cmd_groups);
    mrp_list_init(&ctx->consoles);
    
    register_commands(ctx);
}


void console_cleanup(mrp_context_t *ctx)
{
    mrp_list_hook_t *p, *n;
    console_t       *c;

    mrp_list_foreach(&ctx->consoles, p, n) {
	c = mrp_list_entry(p, typeof(*c), hook);
	mrp_destroy_console((mrp_console_t *)c);
    }

    mrp_list_init(&ctx->cmd_groups);
    
    unregister_commands(ctx);
}


mrp_console_t *mrp_create_console(mrp_context_t *ctx, mrp_console_req_t *req,
				  void *backend_data)
{
    static mrp_console_evt_t evt = {
	.input        = input_evt,
	.disconnected = disconnected_evt,
	.complete     = complete_evt
    };
    
    console_t *c;

    if (req->write == NULL || req->close      == NULL ||
	req->free  == NULL || req->set_prompt == NULL)
	return NULL;

    if ((c = mrp_allocz(sizeof(*c))) != NULL) {
	mrp_list_init(&c->hook);
	c->ctx = ctx;
	c->req = *req;
	c->evt = evt;
	
	c->stdout = console_fopen((mrp_console_t *)c);
	c->stderr = console_fopen((mrp_console_t *)c);

	if (c->stdout == NULL || c->stderr == NULL)
	    goto fail;

	c->backend_data  = backend_data;
	c->check_destroy = check_destroy;

	c->in.file = "<console input>";
	c->in.line = 0;
	c->in.fd   = -1;

	mrp_list_append(&ctx->consoles, &c->hook);
	mrp_set_console_prompt((mrp_console_t *)c);
    }
    else {
    fail:
	if (c != NULL) {
	    if (c->stdout != NULL)
		fclose(c->stdout);
	    if (c->stderr != NULL)
		fclose(c->stderr);
	    mrp_free(c);
	    c = NULL;
	}
    }

    return (mrp_console_t *)c;
}


static int purge_destroyed(mrp_console_t *mc)
{
    console_t *c = (console_t *)mc;

    if (c->destroyed && !c->busy) {
	mrp_debug("Purging destroyed console %p...", c);

	mrp_list_delete(&c->hook);

	fclose(c->stdout);
	fclose(c->stderr);
    
	c->req.free(c->backend_data);
	mrp_free(c);

	return TRUE;
    }
    else
	return FALSE;
}


void mrp_destroy_console(mrp_console_t *mc)
{
    if (mc != NULL && !mc->destroyed) {
	if (mc->stdout != NULL)
	    fflush(mc->stdout);
	if (mc->stderr != NULL)
	    fflush(mc->stderr);
	
	mc->destroyed = TRUE;

	if (mc->backend_data != NULL) {
	    MRP_CONSOLE_BUSY(mc, {
		    mc->req.close(mc);
		});
	}
	
	purge_destroyed(mc);
    }
}


static int check_destroy(mrp_console_t *c)
{
    return purge_destroyed(c);
}


void mrp_console_printf(mrp_console_t *mc, const char *fmt, ...)
{
    console_t *c = (console_t *)mc;
    va_list    ap;

    va_start(ap, fmt);
    vfprintf(c->stdout, fmt, ap);
    va_end(ap);
    
    fflush(c->stdout);
}


void mrp_set_console_prompt(mrp_console_t *mc)
{
    console_t *c = (console_t *)mc;
    char      *prompt, buf[MAX_PROMPT];

    if (c->destroyed)
	return;

    if (c->grp != NULL) {
	prompt = buf;
	
	snprintf(buf, sizeof(buf), "murphy %s", c->grp->name);
    }
    else
	prompt = "murphy";
    
    if (strcmp(prompt, c->prompt)) {
	strcpy(c->prompt, prompt);
	c->req.set_prompt(mc, prompt);
    }
}


static mrp_console_group_t *find_group(mrp_context_t *ctx, const char *name)
{
    mrp_list_hook_t     *p, *n;
    mrp_console_group_t *grp;

    mrp_list_foreach(&ctx->cmd_groups, p, n) {
	grp = mrp_list_entry(p, typeof(*grp), hook);
	if (!strcmp(grp->name, name))
	    return grp;
    }
    
    return NULL;
}


static mrp_console_cmd_t *find_command(mrp_console_group_t *group,
				       const char *command)
{
    mrp_console_cmd_t *cmd;
    int                i;

    for (i = 0, cmd = group->commands; i < group->ncommand; i++, cmd++) {
	if (!strcmp(cmd->name, command))
	    return cmd;
    }
    
    return NULL;
}


int mrp_add_console_group(mrp_context_t *ctx, mrp_console_group_t *group)
{
    if (group != NULL && find_group(ctx, group->name) == NULL) {
	mrp_list_append(&ctx->cmd_groups, &group->hook);
	return TRUE;
    }
    else
	return FALSE;
}


int mrp_del_console_group(mrp_context_t *ctx, mrp_console_group_t *group)
{
    if (group != NULL && find_group(ctx, group->name) == group) {
	mrp_list_delete(&group->hook);
	return TRUE;
    }
    else
	return FALSE;
}


static ssize_t input_evt(mrp_console_t *mc, void *buf, size_t size)
{
    console_t           *c = (console_t *)mc;
    mrp_console_group_t *grp;
    mrp_console_cmd_t   *cmd;
    char                *args[MRP_CFG_MAXARGS];
    int                  argc;
    char               **argv;
    int                  len;

    len = size;
    strncpy(c->in.buf, buf, len);
    c->in.buf[len++] = '\n';
    c->in.buf[len]   = '\0';
	    
    c->in.token = c->in.buf;
    c->in.out   = c->in.buf;
    c->in.next  = c->in.buf;
    c->in.in    = c->in.buf + len;
    c->in.line  = 1;
    *c->in.in   = '\0';

    argv = args + 1;
    argc = get_next_line(&c->in, argv, MRP_ARRAY_SIZE(args) - 1);
    grp  = c->grp;
    cmd  = NULL;
    
    /*
     * Notes: Uhmmkay... so this will need t get replaced eventually with
     *        decent input processing login.
     */

    if (argc < 0)
	fprintf(c->stderr, "failed to parse command: '%.*s'\n",
		(int)size, (char *)buf);
    else if (argc == 0)
	goto prompt;
    
    if (argc == 1) {
	if (!strcmp(argv[0], "exit")) {
	    if (grp != NULL) {
		c->grp = NULL;
		goto prompt;
	    }
	    else {
		grp = find_group(c->ctx, "");
		cmd = find_command(grp, "exit");
		goto execute;
	    }
	}
	else {
	    if (grp == NULL || *argv[0] == '/') {
		grp = find_group(c->ctx, argv[0]);
		if (c->grp == NULL && grp != NULL) {
		    c->grp = grp;
		    goto prompt;
		}
		else {
		    grp = find_group(c->ctx, "");
		    cmd = find_command(grp, argv[0]);
		    
		    if (cmd == NULL && *argv[0] == '/')
			cmd = find_command(grp, argv[0] + 1);
		    
		    goto execute;
		}
	    }
	    else {
		if (grp != NULL) {
		    cmd = find_command(grp, argv[0]);
		
		    if (cmd == NULL) {
			grp = find_group(c->ctx, "");
			cmd = find_command(grp, argv[0] + 1);
		    }
		}
		else {
		    grp = find_group(c->ctx, "");
		    cmd = find_command(grp, argv[0]);
		    
		    if (cmd == NULL && *argv[0] == '/')
			cmd = find_command(grp, argv[0] + 1);
		}

		goto execute;
	    }
	}
    }
    else {
	if (*argv[0] == '/') {
	    grp = find_group(c->ctx, argv[0] + 1);
	    
	    if (grp != NULL)
		cmd = find_command(grp, argv[1]);
	    else {
		grp = find_group(c->ctx, "");
		cmd = find_command(grp, argv[0] + 1);
	    }

	    goto execute;
	}
	else {
	    if (c->grp != NULL) {
		grp = c->grp;
		cmd = find_command(grp, argv[0]);

		if (cmd == NULL) {
		    grp = find_group(c->ctx, "");
		    cmd = find_command(grp, argv[0]);
		}
	    }
	    else {
		grp = find_group(c->ctx, argv[0]);
		cmd = grp ? find_command(grp, argv[1]) : NULL;
	    }

	    goto execute;
	}
    }

 execute:
    if (cmd != NULL) {
	MRP_CONSOLE_BUSY(mc, {
		cmd->tok(mc, grp->user_data, argc, argv);
	    });
    }
    else
	fprintf(mc->stderr, "invalid command '%.*s'\n", (int)size, (char *)buf);
    
 prompt:
    if (mc->check_destroy(mc))
	return size;
    
    fflush(mc->stdout);
    fflush(mc->stderr);

    mrp_set_console_prompt(mc);
    
    return size;
}


static void disconnected_evt(mrp_console_t *c, int error)
{
    mrp_log_info("Console %p has been disconnected (error: %d).", c, error);
}


static ssize_t complete_evt(mrp_console_t *c, void *input, size_t isize,
			     char **completions, size_t csize)
{
    MRP_UNUSED(c);
    MRP_UNUSED(input);
    MRP_UNUSED(isize);
    MRP_UNUSED(completions);
    MRP_UNUSED(csize);
    
    return 0;
}


/*
 * stream-based console I/O
 */

static ssize_t cookie_write(void *cptr, const char *buf, size_t size)
{
    console_t *c = (console_t *)cptr;
    ssize_t    ssize;

    if (c->destroyed)
	return size;

    MRP_CONSOLE_BUSY(c, {
	    ssize = c->req.write((mrp_console_t *)c, (char *)buf, size);
	});

    return ssize;
}


static int cookie_close(void *cptr)
{
    MRP_UNUSED(cptr);
    
    return 0;
}


static FILE *console_fopen(mrp_console_t *mc)
{
    static cookie_io_functions_t io_func = {
	.read  = NULL,
	.write = cookie_write,
	.seek  = NULL,
	.close = cookie_close
    };

    return fopencookie((void *)mc, "w", io_func);
}


/*
 * builtin console commands
 */

#include "console-command.c"

static void register_commands(mrp_context_t *ctx)
{
    mrp_add_console_group(ctx, &builtin_cmd_group);
}


static void unregister_commands(mrp_context_t *ctx)
{
    mrp_del_console_group(ctx, &builtin_cmd_group);
}


/*
 * XXX TODO Verbatim copy of config.c tokenizer. Separate this out
 *          to common (maybe common/text-utils.c), generalize and
 *          clean it up.
 */

#define MRP_START_COMMENT   '#'

static char *get_next_token(input_t *in);

static int get_next_line(input_t *in, char **args, size_t size)
{
    char *token;
    int   narg;

    narg = 0;
    while ((token = get_next_token(in)) != NULL && narg < (int)size) {
	if (in->error)
	    return -1;
	
	if (token[0] != '\n')
	    args[narg++] = token;
	else {
	    if (*args[0] != MRP_START_COMMENT && narg && *args[0] != '\n')
		return narg;
	    else
		narg = 0;
	}
    }

    if (in->error)
	return -1;

    if (narg >= (int)size) {
	mrp_log_error("Too many tokens on line %d of %s.",
		      in->line - 1, in->file);
	return -1;
    }
    else {
	if (*args[0] != MRP_START_COMMENT && *args[0] != '\n')
	    return narg;
	else
	    return 0;
    }
}


static inline void skip_whitespace(input_t *in)
{
    while ((*in->out == ' ' || *in->out == '\t') && in->out < in->in)
	in->out++;
}


static char *get_next_token(input_t *in)
{
    ssize_t len;
    int     diff, size;
    int     quote, quote_line;
    char   *p, *q;

    /*
     * Newline:
     *
     *     If the previous token was terminated by a newline,
     *     take care of properly returning and administering
     *     the newline token here.
     */

    if (in->next_newline) {
	in->next_newline = FALSE;
	in->was_newline  = TRUE;
	in->line++;

	return "\n";
    }
    
    
    /*
     * if we just finished a line, discard all old data/tokens
     */

    if (*in->token == '\n' || in->was_newline) {
	diff = in->out - in->buf;
	size = in->in - in->out;
	memmove(in->buf, in->out, size);
	in->out  -= diff;
	in->in   -= diff;
	in->next  = in->buf;
	*in->in   = '\0';
    }

    /*
     * refill the buffer if we're empty or just flushed all tokens
     */

    if (in->token == in->buf && in->fd != -1) {
	size = sizeof(in->buf) - 1 - (in->in - in->buf);
	len  = read(in->fd, in->in, size);
	
	if (len < size) {
	    close(in->fd);
	    in->fd = -1;
	}
	
	if (len < 0) {
	    mrp_log_error("Failed to read from config file (%d: %s).",
			  errno, strerror(errno));
	    in->error = TRUE;
	    close(in->fd);
	    in->fd = -1;

	    return NULL;
	}
	
	in->in += len;
	*in->in = '\0';
    }

    if (in->out >= in->in)
	return NULL;

    skip_whitespace(in);
    
    quote = FALSE;
    quote_line = 0;

    p = in->out;
    q = in->next;
    in->token = q;

    while (p < in->in) {
	/*printf("[%c]\n", *p == '\n' ? '.' : *p);*/
	switch (*p) {
	    /*
	     * Quoting:
	     *
	     *     If we're not within a quote, mark a quote started.
	     *     Otherwise if quote matches, close quoting. Otherwise
	     *     copy the quoted quote verbatim.
	     */
	case '\'':
	case '\"':
	    if (!quote) {
		quote      = *p++;
		quote_line = in->line;
	    }
	    else {
		if (*p == quote) {
		    quote      = FALSE;
		    quote_line = 0;
		    p++;
		}
		else {
		    *q++ = *p++;
		}
	    }
	    in->was_newline = FALSE;
	    break;
	    
	    /*
	     * Whitespace:
	     *
	     *     If we're quoting, copy verbatim. Otherwise mark the end
	     *     of the token.
	     */
	case ' ':
	case '\t':
	    if (quote)
		*q++ = *p++;
	    else {
		p++;
		*q++ = '\0';
		
		in->out  = p;
		in->next = q;

		return in->token;
	    }
	    in->was_newline = FALSE;
	    break;

	    /*
	     * Escaping:
	     *
	     *     If the last character in the input, copy verbatim.
	     *     Otherwise if it escapes a '\n', skip both. Otherwise
	     *     copy the escaped character verbatim.
	     */
	case '\\':
	    if (p < in->in - 1) {
		p++;
		if (*p != '\n')
		    *q++ = *p++;
		else {
		    p++;
		    in->line++;
		    in->out = p;
		    skip_whitespace(in);
		    p = in->out;
		}
	    }
	    else
		*q++ = *p++;
	    in->was_newline = FALSE;
	    break;
	    
	    /*
	     * Newline:
	     *
	     *     We don't allow newlines to be quoted. Otherwise
	     *     if the token is not the newline itself, we mark
	     *     the next token to be newline and return the token
	     *     it terminated.
	     */
	case '\n':
	    if (quote) {
		mrp_log_error("%s:%d: Unterminated quote (%c) started "
			      "on line %d.", in->file, in->line, quote,
			      quote_line);
		in->error = TRUE;

		return NULL;
	    }
	    else {
		*q = '\0';
		p++;
		
		in->out  = p;
		in->next = q;
		
		if (in->token == q) {
		    in->line++;
		    in->was_newline = TRUE;
		    return "\n";
		}
		else {
		    in->next_newline = TRUE;
		    return in->token;
		}
	    }
	    break;

	    /*
	     * CR: just ignore it
	     */
	case '\r':
	    p++;
	    break;

	default:
	    *q++ = *p++;
	    in->was_newline = FALSE;
	}
    }
    
    if (in->fd == -1) {
	*q = '\0';
	in->out = p;
	in->in = q;
	
	return in->token;
    }
    else {
	mrp_log_error("Input line %d of file %s exceeds allowed length.",
		      in->line, in->file);
	return NULL;
    }
}