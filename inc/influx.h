
/*
 * influx.h
 * lucas@pamorana.net (2024)
 */

#ifndef _INFLUX_H
#define _INFLUX_H

#include <stddef.h>
#include <stdint.h>

struct field
{
	char   *name;
	double  value;
};

struct tag
{
	char *name;
	char *value;
};

enum influx_precision
{
	INFLUX_PRECISION_S = 0,
	INFLUX_PRECISION_MS,
	INFLUX_PRECISION_US,
	INFLUX_PRECISION_NS,
	INFLUX_PRECISION_END
};

static const char *const influx_precision_str[INFLUX_PRECISION_END] = \
{
	"s",
	"ms",
	"us",
	"ns"
};

struct influx_field_elem
{
	struct field f;
	struct influx_field_elem *next;
};

struct influx_field_list
{
	size_t num;
	struct influx_field_elem *next;
	struct influx_field_elem *last;
};

struct influx_writer
{
	/*
	 * in the curl_easy documentation, they advice the user
	 * to re-use an initialized curl_easy handle for as long
	 * as possible (my guess: keep-alive, sessions, etc.).
	 */
	void *curl;
	void *curlurl;

	/*
	 * https://docs.influxdata.com/influxdb/cloud/api/#operation/PostWrite
	 *
	 * specifies four different timestamp precisions.
	 */
	enum influx_precision precision;

	/*
	 * Authorization token is fetched from the environment variable
	 * "INFLUXDB_TOKEN".
	 */
};

#define INFLUX_API_WRITE_PATH "/api/v2/write"


/*
 * influx_writer_create:
 *   creates a new writer instance.
 *   "host"   should be a URL, of the form "protocol://host:port".
 *   "org"    should be the InfluxDB organisation.
 *   "bucket" should be the InfluxDB bucket.
 *   "prec"   is the desired precision for the timestamps.
 */
struct influx_writer *influx_writer_create (const char *hosturl, const char *org, const char *bucket, enum influx_precision prec);


/*
 * influx_writer_destroy:
 *   deallocate an influxdb writer. does nothing if ctx is NULL.
 */
void influx_writer_destroy (struct influx_writer *ctx);


/*
 * influx_writer_write:
 *   write a list of line protocol lines to InfluxDB.
 *   if API returns HTTP (2|3)xx, this function returns 0 and "response"
 *   is set to point to a response body ONLY when the server sent one.
 *
 *   HTTP API error codes (4|5)xx will be returned as positive integers.
 *   internal library and system errors will be returned as -1.
 *
 *   the response body is formatted as json, if one is received.
 */
int influx_writer_write (struct influx_writer *ctx, const char *lines[], char **response);


/*
 * influx_writer_line:
 *   constructs a line protocol line from a set of tags and fields.
 *     "measurement" is a name for this measurement.
 *     "tags"        is a NULL-terminated list of "struct tags".
 *     "fields"      is a NULL-terminated list of "struct fields".
 *     "prec"        is the precision for the automatically generated timestamp.
 */
char *influx_writer_line (const char *measurement, const struct tag *t[], const struct field *f[], enum influx_precision prec);


/*
 * influx_field_list_create:
 *   initializes a linked list of fields. heap allocated.
 */
struct influx_field_list *influx_field_list_create (void);


/*
 * influx_field_list_append:
 *   append a new measurement to a list of fields.
 *   returns -1 if heap allocation errors, or if lst is NULL or malformed.
 */
int influx_field_list_append (struct influx_field_list *lst, const char *field, const double value);


/*
 * influx_field_list_destroy:
 *   free all resources and elements belonging to "lst",
 *   including the list object.
 */
void influx_field_list_destroy (struct influx_field_list *lst);


/*
 * influx_field_compact_free:
 *   free the resources occupied by a compact field list,
 *   including the list itself.
 */
void influx_field_compact_free (struct field *fields[]);


/*
 * influx_field_list_compact:
 *   reduce the flexible linked list structure into a
 *   compact element array. heap allocated.
 *   returns NULL on memory allocation errors.
 */
struct field **influx_field_list_compact (const struct influx_field_list *lst);


/*
 * fstring:
 *   allocate just enough heap memory, and write the fully formatted
 *   user data to it. remember to free it with "free".
 */
#define fstring(...) fstringa(NULL, __VA_ARGS__)


/*
 * fstringa:
 *   same as fstring, but append the new format string to an existing buffer.
 *   if a reallocation error occurs, the original str will be free'd, and NULL
 *   is returned.
 */
#if (defined(__GNUC__) && __GNUC__ >= 4)
/* interpret fmt as a format string */
__attribute__ ((format (printf, 2, 3)))
#endif
char *fstringa (char *str, const char fmt[static 1], ...);


#endif /* _INFLUX_H */
