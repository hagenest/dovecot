/* Copyright (c) 2006-2015 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "mail-storage-hooks.h"
#include "fts-filter.h"
#include "fts-tokenizer.h"
#include "fts-parser.h"
#include "fts-storage.h"
#include "fts-user.h"
#include "fts-plugin.h"


const char *fts_plugin_version = DOVECOT_ABI_VERSION;

static struct mail_storage_hooks fts_mail_storage_hooks = {
	.mail_namespaces_added = fts_mail_namespaces_added,
	.mailbox_allocated = fts_mailbox_allocated,
	.mail_allocated = fts_mail_allocated
};

void fts_plugin_init(struct module *module)
{
	fts_filters_init();
	fts_tokenizers_init();
	mail_storage_hooks_add(module, &fts_mail_storage_hooks);
}

void fts_plugin_deinit(void)
{
	fts_filters_deinit();
	fts_tokenizers_deinit();
	fts_parsers_unload();
	mail_storage_hooks_remove(&fts_mail_storage_hooks);
}
