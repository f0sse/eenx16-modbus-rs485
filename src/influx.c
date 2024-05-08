
/*
 * influx.c
 * lucas@pamorana.net (2024)
 *
 * Functions to post metrics to InfluxDB.
 */

#ifdef __LINTER__
#define _DEFAULT_SOURCE
#endif


#if defined(DEBUG) && !defined(NDEBUG)
#define zDEBUG 1
#else
#define zDEBUG 0
#endif


/*---------------------------------------------------------------------------*\
|*                                  HEADERS                                  *|
\*---------------------------------------------------------------------------*/

#include <stddef.h>
#include <stdarg.h>
#include <stdint.h>
#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <assert.h>
#include <unistd.h>

/* for data transmission */
#include <curl/curl.h>
#include <curl/curlver.h>

#include "influx.h"

/*---------------------------------------------------------------------------*\
|*                               HELPER MACROS                               *|
\*---------------------------------------------------------------------------*/

typedef union /* const pointers to non-const... <.> */
{
	const
	void *pc; // constant
	void *pm; // mutable
}
pedantic;

#ifndef DISCARD_QUALIFIER
#define DISCARD_QUALIFIER(PTR) (pedantic){.pc=(PTR)}.pm
#endif

#ifdef NULL
/*
 * assert NULL length, to catch
 * pointer <=> integer bugs due
 * to NULL definition variations.
 *
 * if you explode here, check the
 * NULL definition on your system.
 */
enum { z_assert_null_definition = 1/!!(sizeof(NULL) == sizeof(void *)) };
#else
#error "what the heck?"
#endif


/*---------------------------------------------------------------------------*\
|*                                INFLUX  API                                *|
\*---------------------------------------------------------------------------*/

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
char *fstringa (char *str, const char fmt[static 1], ...)
{
	va_list  vap;
	size_t   slen;
	size_t   flen = 1;
	void    *tmp;
	int      ret;
	int      olderrno = errno;

	if (str == NULL)
		slen = 0;
	else
		slen = strlen(str);

	tmp = realloc(str, slen + flen);

	if (tmp == NULL)
	{
		free(str);
		errno = ENOMEM;
		return NULL;
	}
	else
		str = tmp;

	for (;;)
	{
		va_start(vap, fmt);
		ret = vsnprintf(&str[slen], flen, fmt, vap);
		va_end(vap);

		if (ret < 0)
		{
			free(str);
			/* vsnprintf has set errno */
			return NULL;
		}

		if ((size_t) ret < flen)
		{
			/* success */
			errno = olderrno;
			return str;
		}

		flen = (size_t) ret + 1;
		tmp = realloc(str, slen + flen);

		if (tmp == NULL)
		{
			free(str);
			errno = ENOMEM;
			return NULL;
		}
		else
			str = tmp;
	}
}


/*
 * fstring:
 *   allocate just enough heap memory, and write the fully formatted
 *   user data to it. remember to free it with "free".
 */
#define fstring(...) fstringa(NULL, __VA_ARGS__)


/*
 * influx_writer_create:
 *   creates a new writer instance.
 *   "host"   should be a URL, of the form "protocol://host:port".
 *   "org"    should be the InfluxDB organisation.
 *   "bucket" should be the InfluxDB bucket.
 *   "prec"   is the desired precision for the timestamps.
 */
struct influx_writer *influx_writer_create
(
	const char            *host,
	const char            *org,
	const char            *bucket,
	enum influx_precision  prec
)
{
	struct influx_writer *write;

	write = calloc(1, sizeof(struct influx_writer));

	if (write)
	{
		CURLUcode rc;

		char *query = NULL;

		if ((write->curl    = curl_easy_init()) == NULL
		||  (write->curlurl = curl_url())       == NULL
		){
			influx_writer_destroy(write);
			errno = ENOMEM;
			return NULL;
		}

		/* construct the endpoint url object using CURL's URL API: */
		do
		{
			if ((rc = curl_url_set(write->curlurl, CURLUPART_URL, host, CURLU_DEFAULT_SCHEME | CURLU_DISALLOW_USER)) != CURLE_OK)
				break;

			if ((rc = curl_url_set(write->curlurl, CURLUPART_PATH, INFLUX_API_WRITE_PATH, 0)) != CURLE_OK)
				break;

			query = fstring("org=%s", org); /* if null, just removes query params */
			if ((rc = curl_url_set(write->curlurl, CURLUPART_QUERY, query, CURLU_APPENDQUERY | CURLU_URLENCODE)) != CURLE_OK)
				break;
			free(query);

			query = fstring("bucket=%s", bucket);
			if ((rc = curl_url_set(write->curlurl, CURLUPART_QUERY, query, CURLU_APPENDQUERY | CURLU_URLENCODE)) != CURLE_OK)
				break;
			free(query);

			query = fstring("precision=%s", influx_precision_str[prec]);
			if ((rc = curl_url_set(write->curlurl, CURLUPART_QUERY, query, CURLU_APPENDQUERY | CURLU_URLENCODE)) != CURLE_OK)
				break;
			free(query);
		}
		while (0);

		if (rc != CURLE_OK)
		{
			fprintf(stderr, "curl_url_set: %s\n", curl_url_strerror(rc));
			free(query);
			influx_writer_destroy(write);
			errno = EINVAL;
			return NULL;
		}

		/* done! */
	} /* <-- if calloc writer */
	else
		errno = ENOMEM;

	return write;
}


/*
 * influx_writer_destroy:
 *   deallocate an influxdb writer. does nothing if ctx is NULL.
 */
void influx_writer_destroy (struct influx_writer *ctx)
{
	if (ctx)
	{
		curl_url_cleanup  (ctx->curlurl);
		curl_easy_cleanup (ctx->curl);
		free(ctx);
	}
}


/*
 * influx_timestamp_precision:
 *   get a timestamp as string, based on the current time.
 *   precision will decide how many digits will be included
 *   in the sub-second resolution.
 */
static char *influx_timestamp_precision (enum influx_precision prec)
{
	struct timespec ts;

	char *stamp;

	const double divisors[INFLUX_PRECISION_END] = \
	{
		1.e9,
		1.e6,
		1.e3,
		1.e0
	};

	if (clock_gettime(CLOCK_REALTIME, &ts))
	{
		perror("clock_gettime");
		return NULL;
	}

	if (prec)
		stamp = fstring("%ld%.0f", ts.tv_sec, (double) ts.tv_nsec / divisors[prec]);
	else
		stamp = fstring("%ld", ts.tv_sec);

	return stamp;
}


/*
 * influx_writer_line:
 *   constructs a line protocol line from a set of tags and fields.
 *     "measurement" is a name for this measurement.
 *     "tags"        is a NULL-terminated list of "struct tags".
 *     "fields"      is a NULL-terminated list of "struct fields".
 *     "prec"        is the precision for the automatically generated timestamp.
 */
char *influx_writer_line
(
	const char *measurement,
	const struct tag   *tags[],
	const struct field *fields[],
	enum influx_precision prec
)
{
	char
		*timestamp,
		*fieldstr,
		*tagstr,
		*retval;

	if (!tags || !fields || !measurement)
	{
		errno = EINVAL;
		return NULL;
	}

	timestamp = influx_timestamp_precision(prec);
	fieldstr  = fstring("");
	tagstr    = fstring("");

	if (!timestamp || !fieldstr || !tagstr)
	{
		free(timestamp);
		free(fieldstr);
		free(tagstr);
		errno = ENOMEM;
		return NULL;
	}

	for (int i=0; tags[i]; i++)
	{
		char *sep = (*tagstr == '\0') ? "" : ",";

		if (!*(tags[i]->name) || !*(tags[i]->value))
			continue;

		tagstr = fstringa(tagstr, "%s%s=%s", sep, tags[i]->name, tags[i]->value);

		if (tagstr == NULL)
			break;
	}

	for (int i=0; fields[i]; i++)
	{
		char *sep = (*fieldstr == '\0') ? "" : ",";

		if (!*(fields[i]->name))
			continue;

		fieldstr = fstringa(fieldstr, "%s%s=%f", sep, fields[i]->name, fields[i]->value);

		if (fieldstr == NULL)
			break;
	}

	if (tagstr && fieldstr)
		retval = fstring("%s,%s %s %s", measurement, tagstr, fieldstr, timestamp);
	else
	{
		errno = ENOMEM;
		retval = NULL;
	}

	free(tagstr);
	free(fieldstr);
	free(timestamp);

	return retval;
}


/*
 * the curl library will use a callback function
 * to let this application handle the response data.
 *
 * this is the structure used by the callback
 * implementation below.
 */
struct mem
{
	char   *mem;
	size_t  len;
};


/*
 * [POV: libcurl]
 *
 * a curl "write" callback definition, for ingress data.
 * the task: copy retrieved data from "data" to "user".
 *
 * assumption:
 *   'user' is a "struct mem" pointer.
 */
size_t response_callback
(
	void   *data,
	size_t  size,
	size_t  leng,
	void   *user
)
{
	char   *ptr;
	size_t  add_len = (size * leng);
	struct  mem *restrict mem = user;

	/*
	 * adjust the size of struct mem. according to the curl documentation:
	 *
	 *   "This callback function gets called by libcurl as soon as there is
	 *    data received that needs to be saved.
	 *   "
	 * => always resize the "user" buffer.
	 */

	/* if mem->mem is NULL, it behaves like malloc */
	ptr = realloc(mem->mem, mem->len + add_len + 1U); /* extra null-byte */

	if (ptr)
	{
		/* re-allocation success; copy data */
		mem->mem = ptr;
		memcpy(&(mem->mem[mem->len]), data, add_len);
		mem->len += add_len;
		mem->mem[mem->len] = '\0'; /* null-terminate */

		return add_len;
	}
	else
	{
		/*
		 * re-allocation failure; leave mem->mem intact,
		 * for the higher-level user to free
		 */
		errno = ENOMEM;
		return 0U;
	}
}


/*
 * [POV: libcurl]
 *
 * a curl "read" callback definition, for egress data.
 * the task: copy user data from "user" to "dest".
 *
 * assumption:
 *   "user" is a "struct mem" pointer, that has been
 *  set up with the data to write.
 */
size_t request_callback
(
	void   *dest,
	size_t  size, /* of chunk    */
	size_t  leng, /* # of chunks */
	void   *user
)
{
	struct mem *restrict upload = user;
	size_t max = (size * leng);

	if (max < 1)
		return 0;

	if (upload->len)
	{
		/* there's data left */
		size_t copylen = max;

		if (copylen > upload->len)
			copylen = upload->len;

		memcpy(dest, upload->mem, copylen);

		upload->mem += copylen;
		upload->len -= copylen;

		return copylen;
	}

	return 0;
}


/* catch errors and clean up if append fails. not much better than that. */
static struct curl_slist *slist_append(struct curl_slist *s, const char *h)
{
	struct curl_slist *tmp;

	tmp = curl_slist_append(s, h);

	if (!tmp)
	{
		curl_slist_free_all(s);
		return NULL;
	}

	/*
	 * yes, if one call fails the list will be rebuilt from that point
	 * on in the chained calls. is stupid? yes. unlikely to matter, ever,
	 * unless you do embedded stuff. then you should do something smarter.
	 *
	 * see usage below to clearly see the issue.
	 */
	return tmp;
}


/*
 * influx_http_post:
 *   send a POST request with "lines" to the InfluxDB API.
 *   "headers" should be a CURL struct slist with additional HTTP headers.
 *   if no errors occur and no HTTP errors are returned, the "response" pointer
 *   will be set to the response body (heap allocated).
 *   local system errors will be returned as -1.
 *   HTTP API errors (4xx and 5xx) will be returned as positive integers.
 */
static int influx_http_post
(
	struct influx_writer     *ctx,
	const char               *lines,
	const struct curl_slist  *headers,
	char                    **response
)
{
	int retval = -1;

	struct mem resp = \
	{
		.len = 0,
		.mem = NULL
	};

	sigset_t old_sigset;
	sigset_t all_sigset;

	struct mem req = \
	{
		.len = strlen(lines),
		.mem = DISCARD_QUALIFIER(lines)
	};

	if (!ctx || !lines)
	{
		errno = EINVAL;
		return retval;
	}

	/*
	 * save the current sigmask, to be restored later,
	 * and temporarily block all signals
	 */
	sigfillset(&all_sigset);
	sigprocmask(SIG_BLOCK, &all_sigset, &old_sigset);

	if (ctx->curl)
	{
		CURLcode rc;

		/*
		 * curl options are sticky, but since the same set
		 * of values are installed every time, there's no
		 * real danger in not calling curl_easy_reset() in
		 * this context.
		 */

		#define SETOPT_TRY(SETOPT)  rc=(SETOPT);if(rc!=CURLE_OK) break


		/*
		 * Sure, we could use non-local jumps to handle this in a more appealing
		 * fashion, but the overhead of the setjmp API would just slow this down
		 * more than what can be justified.
		 */
		do
		{
			SETOPT_TRY( curl_easy_setopt(ctx->curl,CURLOPT_CURLU,ctx->curlurl)                    );
			SETOPT_TRY( curl_easy_setopt(ctx->curl,CURLOPT_USERAGENT,"libcurl/" LIBCURL_VERSION)  );
			SETOPT_TRY( curl_easy_setopt(ctx->curl,CURLOPT_NOPROGRESS,1L)                         );
			SETOPT_TRY( curl_easy_setopt(ctx->curl,CURLOPT_NOSIGNAL,1L)                           );
			SETOPT_TRY( curl_easy_setopt(ctx->curl,CURLOPT_SSL_VERIFYPEER,1L)                     );
			SETOPT_TRY( curl_easy_setopt(ctx->curl,CURLOPT_SSL_VERIFYHOST,1L)                     );
			SETOPT_TRY( curl_easy_setopt(ctx->curl,CURLOPT_IPRESOLVE,CURL_IPRESOLVE_WHATEVER)     );
			SETOPT_TRY( curl_easy_setopt(ctx->curl,CURLOPT_WRITEFUNCTION,response_callback)       );
			SETOPT_TRY( curl_easy_setopt(ctx->curl,CURLOPT_WRITEDATA,&resp)                       );
			SETOPT_TRY( curl_easy_setopt(ctx->curl,CURLOPT_VERBOSE,(long)zDEBUG)                  );
			SETOPT_TRY( curl_easy_setopt(ctx->curl,CURLOPT_READFUNCTION,request_callback)         );
			SETOPT_TRY( curl_easy_setopt(ctx->curl,CURLOPT_READDATA,&req)                         );
			SETOPT_TRY( curl_easy_setopt(ctx->curl,CURLOPT_POST,1L)                               );
			SETOPT_TRY( curl_easy_setopt(ctx->curl,CURLOPT_ACCEPT_ENCODING,"gzip")                );
			SETOPT_TRY( curl_easy_setopt(ctx->curl,CURLOPT_FAILONERROR,1L)                        );
			if (headers)
			{
			SETOPT_TRY( curl_easy_setopt(ctx->curl,CURLOPT_HTTPHEADER,DISCARD_QUALIFIER(headers)) );
			}
		}
		while(0);

		if (rc != CURLE_OK)
		{
			/* setopt error code "rc" */
			fprintf(stderr, "curl_easy_setopt(): %s\n", curl_easy_strerror(rc));
			retval = -1;
		}
		else
		{
			/* setopt no error */
			rc = curl_easy_perform(ctx->curl);

			switch (rc)
			{
			case CURLE_OK:
				if (resp.mem)
					if (response)
					{
						*response = resp.mem;
						resp.mem  = NULL;
					}
				retval = 0;
				break;

			case CURLE_HTTP_RETURNED_ERROR:
				{
					long http_status = -1;
					curl_easy_getinfo(ctx->curl, CURLINFO_RESPONSE_CODE, &http_status);
					retval = (int) http_status;
				}
				break;

			default:
				fprintf(stderr, "curl_easy_perform(): %s\n", curl_easy_strerror(rc));
				break;
			}

			free(resp.mem);
		}

		#undef SETOPT_TRY

	} /* <- if curl_easy handle */
	else
		fprintf(stderr, "%s(): %s\n", __func__, "libcurl not initialized");

	/* restore the signal mask */
	sigprocmask(SIG_SETMASK, &old_sigset, NULL);

	return retval;
}


/*
 * influx_lines_post:
 *   performs an HTTP POST request to the InfluxDB API.
 *   the authorization token is fetched from the environment variable
 *   "INFLUXDB_TOKEN".
 *   "lines" should be line protocol lines, newline-separated.
 */
static int influx_lines_post
(
	struct influx_writer  *ctx,
	const char            *lines,
	char                 **response
)
{
	int ret;

	char
		*authorization;

	struct curl_slist *headers = NULL;

	headers = slist_append(headers, "Accept: application/json");
	headers = slist_append(headers, "Content-Type: text/plain; charset=utf-8");

	if ((authorization = getenv("INFLUXDB_TOKEN")))
	{
		authorization = fstring("Authorization: Token %s", authorization);

		if (authorization == NULL)
		{
			curl_slist_free_all(headers);
			return -1;
		}

		headers = slist_append(headers, authorization);
		free(authorization);
	}

	ret = influx_http_post(ctx, lines, headers, response);

	curl_slist_free_all(headers);

	return ret;
}


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
int influx_writer_write (struct influx_writer *ctx, const char *lines[], char **response)
{
	int rc;

	char *data = NULL;

	for (int i=0; lines[i]; i++)
	{
		data = fstringa(data, "%s\n", lines[i]);

		if (data == NULL)
		{
			errno = ENOMEM;
			return -1;
		}
	}

	rc = influx_lines_post(ctx, data, response);

	free(data);

	return rc;
}


/*
 * influx_field_list_create:
 *   initializes a linked list of fields. heap allocated.
 */
struct influx_field_list *influx_field_list_create (void)
{
	return (struct influx_field_list *) \
		calloc(1, sizeof(struct influx_field_list));
}


/*
 * influx_field_list_append:
 *   append a new measurement to a list of fields.
 *   returns -1 if heap allocation errors, or if lst is NULL or malformed.
 */
int influx_field_list_append (struct influx_field_list *lst, const char *field, const double value)
{
	struct field f = \
	{
		.name  = fstring("%s", field),
		.value = value
	};

	if (f.name == NULL)
		return -1;

	if (lst)
	{
		if (lst->next == NULL)
		{
			lst->next = calloc(1, sizeof(struct influx_field_elem));

			if (lst->next)
			{
				lst->num++;
				lst->next->f = f;
				lst->last = lst->next;
				return 0;
			}
			else
			{
				free(f.name);
				return -1;
			}
		}
		else
		if (lst->last)
		{
			lst->last->next = calloc(1, sizeof(struct influx_field_elem));

			if (lst->last->next)
			{
				lst->num++;
				lst->last->next->f = f;
				lst->last = lst->last->next;
				return 0;
			}
			else
			{
				free(f.name);
				return -1;
			}
		}
		else
		{
			free(f.name);
			return -1;
		}
	}
	else
		return -1;
}


/*
 * influx_field_list_destroy:
 *   free all resources and elements belonging to "lst",
 *   including the list object.
 */
void influx_field_list_destroy (struct influx_field_list *lst)
{
	struct influx_field_elem
		*head,
		*tail;

	if (lst)
	{
		head = tail = lst->next;

		while (head)
		{
			free(head->f.name);
			head = head->next;
			free(tail);
			tail = head;
		}
	}

	free(lst);
}

/*
 * influx_field_compact_free:
 *   free the resources occupied by a compact field list,
 *   including the list itself.
 */
void influx_field_compact_free (struct field *fields[])
{
	if (fields)
	{
		for (int i=0; fields[i]; i++)
		{
			free(fields[i]->name);
			free(fields[i]);
		}

		free(fields);
	}
}

/*
 * influx_field_list_compact:
 *   reduce the flexible linked list structure into a
 *   compact element array. heap allocated.
 *   returns NULL on memory allocation errors.
 */
struct field **influx_field_list_compact (const struct influx_field_list *lst)
{
	if (lst)
	{
		size_t i=0;

		struct influx_field_elem *head;

		struct field **fields = calloc(lst->num + 1, sizeof(struct influx_field_list *));

		if (fields == NULL)
			return fields;

		head = lst->next;

		while (head && i < lst->num)
		{
			struct field *f = fields[i++] = calloc(1, sizeof(struct field));

			if (f == NULL || (f->name = fstring("%s", head->f.name)) == NULL)
			{
				influx_field_compact_free(fields);
				return NULL;
			}
			f->value = head->f.value;

			/* next element */
			head = head->next;
		}

		return fields;
	}
	else
		return NULL;
}
