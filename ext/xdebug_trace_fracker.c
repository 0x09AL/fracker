#include "xdebug_trace_fracker.h"
#include "xdebug_var.h"

#include "ext/json/php_json.h"
#include "zend_smart_str.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <json.h>

#define LOG_PREFIX "[!] fracker: "

#define CTXT(x) (((xdebug_trace_fracker_context*) ctxt)->x)

extern ZEND_DECLARE_MODULE_GLOBALS(xdebug);

/* TODO add return values? in that case the order changes and we need to export
   an additional value (function_nr from function_stack_entry) */

static int get_server_address(struct sockaddr_in *address)
{
    /* prepare the address structure */
    memset(address, 0, sizeof(*address));
    address->sin_family = AF_INET;
    address->sin_port = htons(XG(trace_fracker_port));
    if (inet_aton(XG(trace_fracker_host), &address->sin_addr)) {
        return 1;
    } else {
        printf(LOG_PREFIX "invalid server address %s\n", XG(trace_fracker_host));
        return 0;
    }
}

static int connect_to_server(const struct sockaddr_in *address)
{
    int socket_fd;

    /* allocate a socket file descriptor */
    socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd == -1) {
        printf(LOG_PREFIX "cannot create the TCP socket\n");
        return -1;
    }

    /* connect to the server */
    if (connect(socket_fd, (const struct sockaddr *)address, sizeof(*address))) {
        printf(LOG_PREFIX "cannot connect to %s:%ld\n", XG(trace_fracker_host), XG(trace_fracker_port));
        return -1;
    }

    return socket_fd;
}

static void write_json_object(int fd, struct json_object *object)
{
    const char *string;

    /* write the object followed by a newline then cleanup */
    string = json_object_to_json_string(object);
    write(fd, string, strlen(string));
    write(fd, "\n", 1);
    json_object_put(object);
}

static struct json_object *zval_to_json(zval *value)
{
    struct json_object *object;
    smart_str buf = { 0 };

    /* XXX zval -> JSON using PHP api, then parse it back */
    php_json_encode(&buf, value, 0);
    smart_str_0(&buf);
    object = json_tokener_parse(buf.s->val);
    smart_str_free(&buf);
    return object;
}

void *xdebug_trace_fracker_init(char *fname, char *script_filename, long options TSRMLS_DC)
{
    xdebug_trace_fracker_context *ctxt;
    struct sockaddr_in server_addr;
    int socket_fd;

    /* fetch server information from ini file */
    if (!get_server_address(&server_addr)) {
        return NULL;
    }

    /* establish a connection to the server */
    socket_fd = connect_to_server(&server_addr);
    if (socket_fd == -1) {
        return NULL;
    }

    /* allocate and populate the context */
    ctxt = xdmalloc(sizeof(xdebug_trace_fracker_context));
    CTXT(socket_fd) = socket_fd;
    return ctxt;
}

void xdebug_trace_fracker_deinit(void *ctxt TSRMLS_DC)
{
    /* release resources */
    close(CTXT(socket_fd));
    xdfree(ctxt);
}

void xdebug_trace_fracker_write_header(void *ctxt TSRMLS_DC)
{
    struct json_object *info;

    /* fill request info */
    info = json_object_new_object();
    json_object_object_add(info, "server", zval_to_json(&PG(http_globals)[TRACK_VARS_SERVER]));
    json_object_object_add(info, "get", zval_to_json(&PG(http_globals)[TRACK_VARS_GET]));
    json_object_object_add(info, "post", zval_to_json(&PG(http_globals)[TRACK_VARS_POST]));

    /* serialize and send */
    write_json_object(CTXT(socket_fd), info);
}

void xdebug_trace_fracker_write_footer(void *ctxt TSRMLS_DC) {}

char *xdebug_trace_fracker_get_filename(void *ctxt TSRMLS_DC)
{
    return (char *)"{TCP}";
}

void xdebug_trace_fracker_function_entry(void *ctxt, function_stack_entry *fse, int function_nr TSRMLS_DC)
{
    struct json_object *info, *arguments;
    char *function;

    /* fill call info */
    function = xdebug_show_fname(fse->function, 0, 0 TSRMLS_CC);
    info = json_object_new_object();
    json_object_object_add(info, "function", json_object_new_string(function));
    json_object_object_add(info, "file", json_object_new_string(fse->filename));
    json_object_object_add(info, "line", json_object_new_int(fse->lineno));
    json_object_object_add(info, "level", json_object_new_int(fse->level));
    xdfree(function);

    /* process arguments */
    arguments = json_object_new_array();
    if (fse->include_filename) {
        struct json_object *argument;

        /* XXX require and include are handled differently (unfortunately this
           is not the actual variable value but a computed one) */

        /* fill and add argument info */
        argument = json_object_new_object();
        json_object_object_add(argument, "name", NULL);
        json_object_object_add(argument, "value", json_object_new_string(fse->include_filename));
        json_object_array_add(arguments, argument);
    } else {
        int i;

        for (i = 0; i < fse->varc; i++) {
            const char *name;
            struct json_object *argument;

            /* fill and add argument info */
            name = fse->var[i].name;
            argument = json_object_new_object();
            json_object_object_add(argument, "name", name ? json_object_new_string(name) : NULL);
            json_object_object_add(argument, "value", zval_to_json(&fse->var[i].data));
            json_object_array_add(arguments, argument);
        }
    }
    json_object_object_add(info, "arguments", arguments);

    /* serialize and send */
    write_json_object(CTXT(socket_fd), info);
}

void xdebug_trace_fracker_function_exit(void *ctxt, function_stack_entry *fse, int function_nr TSRMLS_DC) {}

void xdebug_trace_fracker_function_return_value(void *ctxt, function_stack_entry *fse, int function_nr, zval *return_value TSRMLS_DC) {}

void xdebug_trace_fracker_generator_return_value(void *ctxt, function_stack_entry *fse, int function_nr, zend_generator *generator TSRMLS_DC) {}

void xdebug_trace_fracker_assignment(void *ctxt, function_stack_entry *fse, char *full_varname, zval *value, char *right_full_varname, const char *op, char *file, int lineno TSRMLS_DC) {}

xdebug_trace_handler_t xdebug_trace_handler_fracker =
{
    xdebug_trace_fracker_init,
    xdebug_trace_fracker_deinit,
    xdebug_trace_fracker_write_header,
    xdebug_trace_fracker_write_footer,
    xdebug_trace_fracker_get_filename,
    xdebug_trace_fracker_function_entry,
    xdebug_trace_fracker_function_exit,
    xdebug_trace_fracker_function_return_value,
    xdebug_trace_fracker_generator_return_value,
    xdebug_trace_fracker_assignment
};
