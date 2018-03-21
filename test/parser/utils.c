
#include <assert.h>
#include <err.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <talloc/talloc.h>
#include <types/types.h>
#include <url/url.h>

#include "device-handler.h"
#include "parser.h"
#include "resource.h"
#include "event.h"
#include "platform.h"
#include "paths.h"
#include "parser-conf.h"

#include "parser-test.h"

struct p_item {
	struct list_item list;
	struct parser *parser;
};

struct test_file {
	struct discover_device	*dev;
	enum {
		TEST_FILE,
		TEST_DIR,
	}			type;
	const char		*name;
	void			*data;
	int			size;
	struct list_item	list;
};

STATIC_LIST(parsers);

void __register_parser(struct parser *parser)
{
	struct p_item* i = talloc(NULL, struct p_item);

	i->parser = parser;
	list_add(&parsers, &i->list);
}

static void __attribute__((destructor)) __cleanup_parsers(void)
{
	struct p_item *item, *tmp;

	list_for_each_entry_safe(&parsers, item, tmp, list)
		talloc_free(item);
}

static struct discover_device *test_create_device_simple(
		struct parser_test *test)
{
	static int dev_idx;
	char name[10];

	sprintf(name, "__test%d", dev_idx++);

	return test_create_device(test, name);
}

struct discover_device *test_create_device(struct parser_test *test,
		const char *name)
{
	struct discover_device *dev;

	dev = discover_device_create(test->handler, NULL, name);

	dev->device->id = talloc_strdup(dev, name);
	dev->device_path = talloc_asprintf(dev, "/dev/%s", name);
	dev->mount_path = talloc_asprintf(dev, "/test/mount/%s", name);
	dev->mounted = true;

	return dev;
}

static struct discover_context *test_create_context(struct parser_test *test)
{
	struct discover_context *ctx;

	ctx = talloc_zero(test, struct discover_context);
	assert(ctx);

	list_init(&ctx->boot_options);
	ctx->device = test_create_device_simple(test);
	ctx->test_data = test;
	ctx->handler = test->handler;
	device_handler_add_device(test->handler, ctx->device);

	return ctx;
}

/* define our own test platform */
static bool test_platform_probe(struct platform *p __attribute__((unused)),
		void *ctx __attribute__((unused)))
{
	return true;
}

struct platform test_platform = {
	.name = "test",
	.probe = test_platform_probe,
};

register_platform(test_platform);

struct parser_test *test_init(void)
{
	struct parser_test *test;

	test = talloc_zero(NULL, struct parser_test);
	platform_init(NULL);
	test->handler = device_handler_init(NULL, NULL, 0);
	test->ctx = test_create_context(test);
	list_init(&test->files);

	return test;
}

void test_fini(struct parser_test *test)
{
	device_handler_destroy(test->handler);
	talloc_free(test);
	platform_fini();
}

void __test_read_conf_data(struct parser_test *test,
		struct discover_device *dev, const char *conf_file,
		const char *buf, size_t len)
{
	test_add_file_data(test, dev, conf_file, buf, len);
}

void test_read_conf_file(struct parser_test *test, const char *filename,
		const char *conf_file)
{
	struct stat stat;
	size_t size;
	char *path;
	int fd, rc;
	char *buf;

	path = talloc_asprintf(test, "%s/%s", TEST_CONF_BASE, filename);

	fd = open(path, O_RDONLY);
	if (fd < 0)
		err(EXIT_FAILURE, "Can't open test conf file %s\n", path);

	rc = fstat(fd, &stat);
	assert(!rc);
	(void)rc;

	size = stat.st_size;
	buf = talloc_array(test, char, size + 1);

	rc = read(fd, buf, size);
	assert(rc == (ssize_t)size);

	*(buf + size) = '\0';

	close(fd);
	talloc_free(path);

	test_add_file_data(test, test->ctx->device, conf_file, buf, size);
}

void test_add_file_data(struct parser_test *test, struct discover_device *dev,
		const char *filename, const void *data, int size)
{
	struct test_file *file;

	file = talloc_zero(test, struct test_file);
	file->type = TEST_FILE;
	file->dev = dev;
	file->name = filename;
	file->data = talloc_memdup(test, data, size);
	file->size = size;
	list_add(&test->files, &file->list);
}

void test_add_dir(struct parser_test *test, struct discover_device *dev,
		const char *dirname)
{
	struct test_file *file;

	file = talloc_zero(test, struct test_file);
	file->type = TEST_DIR;
	file->dev = dev;
	file->name = dirname;
	/* Pick a non-zero size for directories so that "[ -s <dir
	 * path> ]" sees that the file has non-zero size. */
	file->size = 1;
	list_add(&test->files, &file->list);
}

void test_set_event_source(struct parser_test *test)
{
        test->ctx->event = talloc_zero(test->ctx, struct event);
}

void test_set_event_param(struct event *event, const char *name,
                const char *value)
{
        event_set_param(event, name, value);
}

void test_set_event_device(struct event *event, const char *dev)
{
	event->device = talloc_strdup(event, dev);
}

int parser_request_file(struct discover_context *ctx,
		struct discover_device *dev, const char *filename,
		char **buf, int *len)
{
	struct parser_test *test = ctx->test_data;
	struct test_file *file;
	char *tmp;

	list_for_each_entry(&test->files, file, list) {
		if (file->dev != dev)
			continue;
		if (strcmp(file->name, filename))
			continue;
		if (file->type != TEST_FILE)
			continue;

		/* the read_file() interface always adds a trailing null
		 * for string-safety; do the same here */
		tmp = talloc_array(test, char, file->size + 1);
		memcpy(tmp, file->data, file->size);
		tmp[file->size] = '\0';
		*buf = tmp;
		*len = file->size;
		return 0;
	}

	return -1;
}

int parser_stat_path(struct discover_context *ctx,
		struct discover_device *dev, const char *path,
		struct stat *statbuf)
{
	struct parser_test *test = ctx->test_data;
	struct test_file *file;

	list_for_each_entry(&test->files, file, list) {
		if (file->dev != dev)
			continue;
		if (strcmp(file->name, path))
			continue;

		statbuf->st_size = (off_t)file->size;
		switch (file->type) {
		case TEST_FILE:
			statbuf->st_mode = S_IFREG;
			break;
		case TEST_DIR:
			statbuf->st_mode = S_IFDIR;
			break;
		default:
			fprintf(stderr, "%s: bad test file mode %d!", __func__,
				file->type);
			exit(EXIT_FAILURE);
		}

		return 0;
	}

	return -1;
}

int parser_replace_file(struct discover_context *ctx,
		struct discover_device *dev, const char *filename,
		char *buf, int len)
{
	struct parser_test *test = ctx->test_data;
	struct test_file *f, *file;

	list_for_each_entry(&test->files, f, list) {
		if (f->dev != dev)
			continue;
		if (strcmp(f->name, filename))
			continue;

		file = f;
		break;
	}

	if (!file) {
		file = talloc_zero(test, struct test_file);
		file->dev = dev;
		file->name = filename;
		list_add(&test->files, &file->list);
	}

	file->data = talloc_memdup(test, buf, len);
	file->size = len;
	return 0;
}

int parser_scandir(struct discover_context *ctx, const char *dirname,
		   struct dirent ***files, int (*filter)(const struct dirent *)
		   __attribute__((unused)),
		   int (*comp)(const struct dirent **, const struct dirent **)
		   __attribute__((unused)))
{
	struct parser_test *test = ctx->test_data;
	struct test_file *f;
	char *filename;
	struct dirent **dirents = NULL, **new_dirents;
	int n = 0, namelen;

	list_for_each_entry(&test->files, f, list) {
		if (f->dev != ctx->device)
			continue;

		filename = strrchr(f->name, '/');
		if (!filename)
			continue;

		namelen = strlen(filename);

		if (strncmp(f->name, dirname, strlen(f->name) - namelen))
			continue;

		if (!dirents) {
			dirents = malloc(sizeof(struct dirent *));
		} else {
			new_dirents = realloc(dirents, sizeof(struct dirent *)
					      * (n + 1));
			if (!new_dirents)
				goto err_cleanup;

			dirents = new_dirents;
		}

		dirents[n] = malloc(sizeof(struct dirent) + namelen + 1);

		if (!dirents[n])
			goto err_cleanup;

		strcpy(dirents[n]->d_name, filename + 1);
		n++;
	}

	*files = dirents;

	return n;

err_cleanup:
	do {
		free(dirents[n]);
	} while (n-- > 0);

	free(dirents);

	return -1;
}

struct load_url_result *load_url_async(void *ctx, struct pb_url *url,
		load_url_complete async_cb, void *async_data,
		waiter_cb stdout_cb, void *stdout_data)
{
	struct conf_context *conf = async_data;
	struct parser_test *test = conf->dc->test_data;
	struct load_url_result *result;
	char tmp[] = "/tmp/pb-XXXXXX";
	ssize_t rc = -1, sz = 0;
	struct test_file *file;
	int fd;

	/* Ignore the stdout callback for tests */
	(void)stdout_cb;
	(void)stdout_data;

	fd = mkstemp(tmp);

	if (fd < 0)
		return NULL;

	/* Some parsers will expect to need to read a file, so write the
	 * specified file to a temporary file */
	list_for_each_entry(&test->files, file, list) {
		if (file->dev)
			continue;

		if (strcmp(file->name, url->full))
			continue;

		while (sz < file->size) {
			rc = write(fd, file->data, file->size);
			if (rc < 0) {
				fprintf(stderr,
					"Failed to write to tmpfile, %m\n");
				break;
			}
			sz += rc;
		}
		break;
	}

	close(fd);

	result = talloc_zero(ctx, struct load_url_result);
	if (!result)
		return NULL;

	result->local = talloc_strdup(result, tmp);
	result->url = url;
	if (rc < 0)
		result->status = LOAD_ERROR;
	else
		result->status = result->local ? LOAD_OK : LOAD_ERROR;
	result->cleanup_local = true;

	async_cb(result, conf);

	return result;
}

int parser_request_url(struct discover_context *ctx, struct pb_url *url,
		char **buf, int *len)
{
	struct parser_test *test = ctx->test_data;
	struct test_file *file;
	char *tmp;

	list_for_each_entry(&test->files, file, list) {
		if (file->dev)
			continue;

		if (strcmp(file->name, url->full))
			continue;

		/* the read_file() interface always adds a trailing null
		 * for string-safety; do the same here */
		tmp = talloc_array(test, char, file->size + 1);
		memcpy(tmp, file->data, file->size);
		tmp[file->size] = '\0';
		*buf = tmp;
		*len = file->size;
		return 0;
	}

	return -1;
}

int test_run_parser(struct parser_test *test, const char *parser_name)
{
	struct p_item* i;

	list_for_each_entry(&parsers, i, list) {
		if (strcmp(i->parser->name, parser_name))
			continue;
		test->ctx->parser = i->parser;
		return i->parser->parse(test->ctx);
	}

	errx(EXIT_FAILURE, "%s: parser '%s' not found", __func__, parser_name);
}

bool resource_resolve(struct device_handler *handler, struct parser *parser,
		struct resource *resource)
{
	if (!resource)
		return true;
	if (resource->resolved)
		return true;

	assert(parser);
	assert(parser->resolve_resource);

	return parser->resolve_resource(handler, resource);
}

void boot_option_resolve(struct device_handler *handler,
		struct discover_boot_option *opt)
{
	resource_resolve(handler, opt->source, opt->boot_image);
	resource_resolve(handler, opt->source, opt->initrd);
	resource_resolve(handler, opt->source, opt->icon);
}

void test_hotplug_device(struct parser_test *test, struct discover_device *dev)
{
	struct discover_boot_option *opt;

	device_handler_add_device(test->handler, dev);

	list_for_each_entry(&test->ctx->boot_options, opt, list)
		boot_option_resolve(test->handler, opt);
}

void test_remove_device(struct parser_test *test, struct discover_device *dev)
{
	struct discover_boot_option *opt, *tmp;

	if (dev == test->ctx->device) {
		list_for_each_entry_safe(&test->ctx->boot_options,
				opt, tmp, list) {
			list_remove(&opt->list);
			talloc_free(opt);
		}
	}

	device_handler_remove(test->handler, dev);
}

struct discover_boot_option *get_boot_option(struct discover_context *ctx,
		int idx)
{
	struct discover_boot_option *opt;
	int i = 0;

	list_for_each_entry(&ctx->boot_options, opt, list) {
		if (i++ == idx)
			return opt;
	}

	assert(0);

	return NULL;
}

void __check_boot_option_count(struct discover_context *ctx, int count,
		const char *file, int line)
{
	struct discover_boot_option *opt;
	int defaults = 0, i = 0;

	list_for_each_entry(&ctx->boot_options, opt, list) {
		i++;
		if (opt->option->is_default)
			defaults++;
	}

	if (defaults > 1) {
		fprintf(stderr, "%s:%d: parser returned multiple default "
				"options\n", file, line);
		exit(EXIT_FAILURE);
	}

	if (i == count)
		return;

	fprintf(stderr, "%s:%d: boot option count check failed\n", file, line);
	fprintf(stderr, "expected %d options, got %d:\n", count, i);

	i = 1;
	list_for_each_entry(&ctx->boot_options, opt, list)
		fprintf(stderr, "  %2d: %s [%s]\n", i++, opt->option->name,
				opt->option->id);

	exit(EXIT_FAILURE);
}

void __check_args(struct discover_boot_option *opt, const char *args,
		const char *file, int line)
{
	int rc;

	if (!opt->option->boot_args && !args)
		return;

	if (!opt->option->boot_args) {
		fprintf(stderr, "%s:%d: arg check failed\n", file, line);
		fprintf(stderr, "  no arguments parsed\n");
		fprintf(stderr, "  expected '%s'\n", args);
		exit(EXIT_FAILURE);
	}

	rc = strcmp(opt->option->boot_args, args);
	if (rc) {
		fprintf(stderr, "%s:%d: arg check failed\n", file, line);
		fprintf(stderr, "  got      '%s'\n", opt->option->boot_args);
		fprintf(stderr, "  expected '%s'\n", args);
		exit(EXIT_FAILURE);
	}
}

void __check_name(struct discover_boot_option *opt, const char *name,
		const char *file, int line)
{
	int rc;

	rc = strcmp(opt->option->name, name);
	if (rc) {
		fprintf(stderr, "%s:%d: name check failed\n", file, line);
		fprintf(stderr, "  got      '%s'\n", opt->option->name);
		fprintf(stderr, "  expected '%s'\n", name);
		exit(EXIT_FAILURE);
	}
}

void __check_is_default(struct discover_boot_option *opt,
		const char *file, int line)
{
	if (opt->option->is_default)
		return;

	fprintf(stderr, "%s:%d: default check failed\n", file, line);
	exit(EXIT_FAILURE);
}

void __check_resolved_local_resource(struct resource *res,
		struct discover_device *dev, const char *local_path,
		const char *file, int line)
{
	const char *exp_url, *got_url;

	if (!res)
		errx(EXIT_FAILURE, "%s:%d: No resource", file, line);

	if (!res->resolved)
		errx(EXIT_FAILURE, "%s:%d: Resource is not resolved",
				file, line);

	exp_url = talloc_asprintf(res, "file://%s%s",
			dev->mount_path, local_path);
	got_url = pb_url_to_string(res->url);

	if (strcmp(got_url, exp_url)) {
		fprintf(stderr, "%s:%d: Resource mismatch\n", file, line);
		fprintf(stderr, "  got      '%s'\n", got_url);
		fprintf(stderr, "  expected '%s'\n", exp_url);
		exit(EXIT_FAILURE);
	}
}

void __check_resolved_url_resource(struct resource *res,
		const char *url, const char *file, int line)
{
	char *res_url;

	if (!res)
		errx(EXIT_FAILURE, "%s:%d: No resource", file, line);

	if (!res->resolved)
		errx(EXIT_FAILURE, "%s:%d: Resource is not resolved",
				file, line);

	res_url = pb_url_to_string(res->url);
	if (strcmp(url, res_url)) {
		fprintf(stderr, "%s:%d: Resource mismatch\n", file, line);
		fprintf(stderr, "  got      '%s'\n", res_url);
		fprintf(stderr, "  expected '%s'\n", url);
		exit(EXIT_FAILURE);
	}
}
void __check_unresolved_resource(struct resource *res,
		const char *file, int line)
{
	if (!res)
		errx(EXIT_FAILURE, "%s:%d: No resource", file, line);

	if (res->resolved)
		errx(EXIT_FAILURE, "%s:%d: Resource is resolved", file, line);
}

void __check_not_present_resource(struct resource *res,
		const char *file, int line)
{
	if (res)
		errx(EXIT_FAILURE, "%s:%d: Resource present", file, line);
}

static void dump_file_data(const void *buf, int len)
{
	int i, j, hex_len = strlen("00 ");
	const int row_len = 16;

	for (i = 0; i < len; i += row_len) {
		char hbuf[row_len * hex_len + 1];
		char cbuf[row_len + strlen("|") + 1];

		for (j = 0; (j < row_len) && ((i+j) < len); j++) {
			char c = ((const char *)buf)[i + j];

			snprintf(hbuf + j * hex_len, hex_len + 1, "%02x ", c);

			if (!isprint(c))
				c = '.';

			snprintf(cbuf + j, hex_len + 1, "%c", c);
		}

		strcat(cbuf, "|");

		fprintf(stderr, "%08x  %*s |%s\n", i,
				0 - (int)sizeof(hbuf) + 1, hbuf, cbuf);
	}
}

void __check_file_contents(struct parser_test *test,
		struct discover_device *dev, const char *filename,
		const char *buf, int len,
		const char *srcfile, int srcline)
{
	struct test_file *f, *file = NULL;

	list_for_each_entry(&test->files, f, list) {
		if (f->dev != dev)
			continue;
		if (strcmp(f->name, filename))
			continue;

		file = f;
		break;
	}

	if (!file)
		errx(EXIT_FAILURE, "%s:%d: File '%s' not found",
				srcfile, srcline, filename);

	if (file->size != len || memcmp(file->data, buf, len)) {
		fprintf(stderr, "%s:%d: File '%s' data/size mismatch\n",
				srcfile, srcline, filename);
		fprintf(stderr, "Expected:\n");
		dump_file_data(buf, len);
		fprintf(stderr, "Got:\n");
		dump_file_data(file->data, file->size);
		exit(EXIT_FAILURE);
	}
}
