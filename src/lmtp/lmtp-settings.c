/* Copyright (c) 2009-2010 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "buffer.h"
#include "settings-parser.h"
#include "service-settings.h"
#include "master-service.h"
#include "master-service-settings.h"
#include "lda-settings.h"
#include "lmtp-settings.h"
#include "mail-storage-settings.h"

#include <stddef.h>
#include <unistd.h>

/* <settings checks> */
static struct file_listener_settings lmtp_login_unix_listeners_array[] = {
	{ "lmtp", 0666, "", "" }
};
static struct file_listener_settings *lmtp_login_unix_listeners[] = {
	&lmtp_login_unix_listeners_array[0]
};
static buffer_t lmtp_login_unix_listeners_buf = {
	lmtp_login_unix_listeners, sizeof(lmtp_login_unix_listeners), { 0, }
};
/* </settings checks> */

struct service_settings lmtp_login_service_settings = {
	.name = "lmtp",
	.protocol = "lmtp",
	.type = "",
	.executable = "lmtp",
	.user = "",
	.group = "",
	.privileged_group = "",
	.extra_groups = "",
	.chroot = "",

	.drop_priv_before_exec = FALSE,

	.process_min_avail = 0,
	.process_limit = 0,
	.client_limit = 0,
	.service_count = 0,
	.idle_kill = 0,
	.vsz_limit = 0,

	.unix_listeners = { { &lmtp_login_unix_listeners_buf,
			      sizeof(lmtp_login_unix_listeners[0]) } },
	.fifo_listeners = ARRAY_INIT,
	.inet_listeners = ARRAY_INIT
};

#undef DEF
#define DEF(type, name) \
	{ type, #name, offsetof(struct lmtp_settings, name), NULL }

static const struct setting_define lmtp_setting_defines[] = {
	DEF(SET_BOOL, lmtp_proxy),

	SETTING_DEFINE_LIST_END
};

static const struct lmtp_settings lmtp_default_settings = {
	.lmtp_proxy = FALSE
};

static const struct setting_parser_info *lmtp_setting_dependencies[] = {
	&lda_setting_parser_info,
	NULL
};

const struct setting_parser_info lmtp_setting_parser_info = {
	.module_name = "lmtp",
	.defines = lmtp_setting_defines,
	.defaults = &lmtp_default_settings,

	.type_offset = (size_t)-1,
	.struct_size = sizeof(struct lmtp_settings),

	.parent_offset = (size_t)-1,

	.dependencies = lmtp_setting_dependencies
};

void lmtp_settings_dup(const struct setting_parser_context *set_parser,
		       pool_t pool,
		       const struct mail_user_settings **user_set_r,
		       const struct lmtp_settings **lmtp_set_r,
		       const struct lda_settings **lda_set_r)
{
	void **sets;

	sets = settings_parser_get_list(set_parser) + 1;
	*user_set_r = settings_dup(&mail_user_setting_parser_info, sets[0], pool);
	*lda_set_r = settings_dup(&lda_setting_parser_info, sets[1], pool);
	*lmtp_set_r = settings_dup(&lmtp_setting_parser_info, sets[2], pool);
}