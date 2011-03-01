#include <tools/ccanlint/ccanlint.h>
#include <tools/tools.h>
#include <ccan/htable/htable_type.h>
#include <ccan/foreach/foreach.h>
#include <ccan/talloc/talloc.h>
#include <ccan/grab_file/grab_file.h>
#include <ccan/str/str.h>
#include <ccan/str_talloc/str_talloc.h>
#include <ccan/hash/hash.h>
#include <ccan/read_write_all/read_write_all.h>
#include <errno.h>
#include <err.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include "reduce_features.h"

bool features_were_reduced;

static const char *can_run(struct manifest *m)
{
	if (!config_header)
		return talloc_strdup(m, "Could not read config.h");
	return NULL;
}

static size_t option_hash(const char *name)
{
	return hash(name, strlen(name), 0);
}

static const char *option_name(const char *name)
{
	return name;
}

static bool option_cmp(const char *name1, const char *name2)
{
	return streq(name1, name2);
}

HTABLE_DEFINE_TYPE(char, option_name, option_hash, option_cmp, option);

static unsigned int add_options(struct htable_option *opts,
				struct pp_conditions *cond)
{
	unsigned int num = 0;
	if (cond->parent)
		num += add_options(opts, cond->parent);
	if (cond->type == PP_COND_IF || cond->type == PP_COND_IFDEF) {
		if (strstarts(cond->symbol, "HAVE_")) {
			if (!htable_option_get(opts, cond->symbol)) {
				htable_option_add(opts, cond->symbol);
				num++;
			}
		}
	}
	return num;
}

static struct htable_option *get_used_options(struct manifest *m)
{
	struct list_head *list;
	struct ccan_file *f;
	unsigned int i, num;
	struct htable_option *opts = htable_option_new();
	struct line_info *info;

	num = 0;
	foreach_ptr(list, &m->c_files, &m->h_files) {
		list_for_each(list, f, list) {
			info = get_ccan_line_info(f);
			struct pp_conditions *prev = NULL;

			for (i = 0; i < f->num_lines; i++) {
				if (info[i].cond && info[i].cond != prev) {
					num += add_options(opts, info[i].cond);
					prev = info[i].cond;
				}
			}
		}
	}

	if (!num) {
		htable_option_free(opts);
		opts = NULL;
	}
	return opts;
}

static struct htable_option *get_config_options(struct manifest *m)
{
	const char **lines = (const char **)strsplit(m, config_header, "\n");
	unsigned int i;
	struct htable_option *opts = htable_option_new();

	for (i = 0; i < talloc_array_length(lines) - 1; i++) {
		char *sym;

		if (!get_token(&lines[i], "#"))
			continue;
		if (!get_token(&lines[i], "define"))
			continue;
		sym = get_symbol_token(lines, &lines[i]);
		if (!strstarts(sym, "HAVE_"))
			continue;
		/* Don't override endian... */
		if (strends(sym, "_ENDIAN"))
			continue;
		if (!get_token(&lines[i], "1"))
			continue;
		htable_option_add(opts, sym);
	}
	return opts;
}

static void do_reduce_features(struct manifest *m,
			       bool keep,
			       unsigned int *timeleft, struct score *score)
{
	struct htable_option *options_used, *options_avail, *options;
	struct htable_option_iter i;
	int fd;
	const char *sym;
	char *hdr;

	/* This isn't really a test, as such. */
	score->total = 0;
	score->pass = true;

	options_used = get_used_options(m);
	if (!options_used) {
		return;
	}
	options_avail = get_config_options(m);

	options = NULL;
	for (sym = htable_option_first(options_used, &i);
	     sym;
	     sym = htable_option_next(options_used, &i)) {
		if (htable_option_get(options_avail, sym)) {
			if (!options)
				options = htable_option_new();
			htable_option_add(options, sym);
		}
	}
	htable_option_free(options_avail);
	htable_option_free(options_used);

	if (!options)
		return;

	/* Now make our own config.h variant, with our own options. */
	hdr = talloc_strdup(m, "/* Modified by reduce_features */\n");
	hdr = talloc_append_string(hdr, config_header);
	for (sym = htable_option_first(options, &i);
	     sym;
	     sym = htable_option_next(options, &i)) {
		hdr = talloc_asprintf_append
			(hdr, "#undef %s\n#define %s 0\n", sym, sym);
	}
	fd = open("config.h", O_EXCL|O_CREAT|O_RDWR, 0600);
	if (fd < 0)
		err(1, "Creating config.h");
	if (!write_all(fd, hdr, strlen(hdr)))
		err(1, "Writing config.h");
	close(fd);
	features_were_reduced = true;
}

struct ccanlint reduce_features = {
	.key = "reduce_features",
	.name = "Produce config.h with reduced features",
	.can_run = can_run,
	.check = do_reduce_features,
	.needs = "tests_compile"
};
REGISTER_TEST(reduce_features);
