/* Copyright (C) 2006 Timo Sirainen */

#include "common.h"
#include "ioloop.h"
#include "array.h"
#include "str.h"
#include "seq-range-array.h"
#include "maildir-storage.h"
#include "client.h"
#include "namespace.h"
#include "lazy-expunge-plugin.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <dirent.h>
#include <time.h>

#define LAZY_EXPUNGE_CONTEXT(obj) \
	*((void **)array_idx_modifyable(&(obj)->module_contexts, \
					lazy_expunge_storage_module_id))

enum lazy_namespace {
	LAZY_NAMESPACE_EXPUNGE,
	LAZY_NAMESPACE_DELETE,
	LAZY_NAMESPACE_DELETE_EXPUNGE,

	LAZY_NAMESPACE_COUNT
};

struct lazy_expunge_mail_storage {
	struct mail_storage_vfuncs super;
	bool internal_namespace;
};

struct lazy_expunge_mailbox {
	struct mailbox_vfuncs super;
};

struct lazy_expunge_transaction {
	array_t ARRAY_DEFINE(expunge_seqs, struct seq_range);
	struct mailbox *expunge_box;
};

struct lazy_expunge_mail {
	struct mail_vfuncs super;
};

static void (*lazy_expunge_next_hook_mail_storage_created)
	(struct mail_storage *storage);
static void (*lazy_expunge_next_hook_client_created)(struct client **client);

static unsigned int lazy_expunge_storage_module_id = 0;
static bool lazy_expunge_storage_module_id_set = FALSE;

static struct namespace *lazy_namespaces[LAZY_NAMESPACE_COUNT];

static struct mailbox *
mailbox_open_or_create(struct mail_storage *storage, const char *name)
{
	struct mailbox *box;
	bool syntax, temp;

	box = mailbox_open(storage, name, NULL, MAILBOX_OPEN_FAST |
			   MAILBOX_OPEN_KEEP_RECENT |
			   MAILBOX_OPEN_NO_INDEX_FILES);
	if (box != NULL)
		return box;

	(void)mail_storage_get_last_error(storage, &syntax, &temp);
	if (syntax || temp)
		return NULL;

	/* probably the mailbox just doesn't exist. try creating it. */
	if (mail_storage_mailbox_create(storage, name, FALSE) < 0)
		return NULL;

	/* and try opening again */
	box = mailbox_open(storage, name, NULL, MAILBOX_OPEN_FAST |
			   MAILBOX_OPEN_KEEP_RECENT);
	return box;
}

static int lazy_expunge_mail_expunge(struct mail *_mail)
{
	struct lazy_expunge_transaction *lt =
		LAZY_EXPUNGE_CONTEXT(_mail->transaction);
	struct mail_storage *deststorage;

	if (lt->expunge_box == NULL) {
		deststorage = lazy_namespaces[LAZY_NAMESPACE_EXPUNGE]->storage;
		lt->expunge_box = mailbox_open_or_create(deststorage,
							 _mail->box->name);
		if (lt->expunge_box == NULL) {
			mail_storage_set_critical(_mail->box->storage,
				"lazy_expunge: Couldn't open expunge mailbox");
			return -1;
		}
	}

	seq_range_array_add(&lt->expunge_seqs, 32, _mail->uid);
	return 0;
}

static struct mailbox_transaction_context *
lazy_expunge_transaction_begin(struct mailbox *box,
			       enum mailbox_transaction_flags flags)
{
	struct lazy_expunge_mailbox *qbox = LAZY_EXPUNGE_CONTEXT(box);
	struct mailbox_transaction_context *t;
	struct lazy_expunge_transaction *lt;

	t = qbox->super.transaction_begin(box, flags);
	lt = i_new(struct lazy_expunge_transaction, 1);

	array_idx_set(&t->module_contexts, lazy_expunge_storage_module_id, &lt);
	return t;
}

struct lazy_expunge_move_context {
	string_t *path;
	unsigned int dir_len;
};

static int lazy_expunge_move(struct maildir_mailbox *mbox __attr_unused__,
			     const char *path, void *context)
{
	struct lazy_expunge_move_context *ctx = context;
	const char *p;

	str_truncate(ctx->path, ctx->dir_len);
	p = strrchr(path, '/');
	str_append(ctx->path, p == NULL ? path : p + 1);

	if (rename(path, str_c(ctx->path)) == 0)
		return 1;
	return errno == ENOENT ? 0 : -1;
}

static int lazy_expunge_move_expunges(struct mailbox *srcbox,
				      struct lazy_expunge_transaction *lt)
{
	struct maildir_mailbox *msrcbox = (struct maildir_mailbox *)srcbox;
	struct mailbox_transaction_context *trans;
	struct index_transaction_context *itrans;
	struct lazy_expunge_move_context ctx;
	const struct seq_range *range;
	unsigned int i, count;
	const char *dir;
	uint32_t seq, uid, seq1, seq2;
	bool is_file;
	int ret = 0;

	dir = mail_storage_get_mailbox_path(lt->expunge_box->storage,
					    lt->expunge_box->name, &is_file);
	dir = t_strconcat(dir, "/cur/", NULL);

	ctx.path = str_new(default_pool, 256);
	str_append(ctx.path, dir);
	ctx.dir_len = str_len(ctx.path);

	trans = mailbox_transaction_begin(srcbox,
					  MAILBOX_TRANSACTION_FLAG_EXTERNAL);
	itrans = (struct index_transaction_context *)trans;

	range = array_get(&lt->expunge_seqs, &count);
	for (i = 0; i < count && ret == 0; i++) {
		if (mailbox_get_uids(srcbox, range[i].seq1, range[i].seq2,
				     &seq1, &seq2) < 0) {
			ret = -1;
			break;
		}

		for (uid = range[i].seq1; uid <= range[i].seq2; uid++) {
			if (maildir_file_do(msrcbox, uid, lazy_expunge_move,
					    &ctx) < 0) {
				ret = -1;
				break;
			}
		}
		for (seq = seq1; seq <= seq2; seq++)
			mail_index_expunge(itrans->trans, seq);
	}

	if (mailbox_transaction_commit(&trans, 0) < 0)
		ret = -1;

	str_free(&ctx.path);
	return ret;
}

static void lazy_expunge_transaction_free(struct lazy_expunge_transaction *lt)
{
	if (lt->expunge_box != NULL)
		mailbox_close(&lt->expunge_box);
	if (array_is_created(&lt->expunge_seqs))
		array_free(&lt->expunge_seqs);
	i_free(lt);
}

static int
lazy_expunge_transaction_commit(struct mailbox_transaction_context *ctx,
				enum mailbox_sync_flags flags)
{
	struct lazy_expunge_mailbox *qbox = LAZY_EXPUNGE_CONTEXT(ctx->box);
	struct lazy_expunge_transaction *lt = LAZY_EXPUNGE_CONTEXT(ctx);
	struct mailbox *srcbox = ctx->box;
	int ret;

	ret = qbox->super.transaction_commit(ctx, flags);

	if (ret == 0 && array_is_created(&lt->expunge_seqs))
		ret = lazy_expunge_move_expunges(srcbox, lt);

	lazy_expunge_transaction_free(lt);
	return ret;
}

static void
lazy_expunge_transaction_rollback(struct mailbox_transaction_context *ctx)
{
	struct lazy_expunge_mailbox *qbox = LAZY_EXPUNGE_CONTEXT(ctx->box);
	struct lazy_expunge_transaction *lt = LAZY_EXPUNGE_CONTEXT(ctx);

	qbox->super.transaction_rollback(ctx);
	lazy_expunge_transaction_free(lt);
}

static struct mail *
lazy_expunge_mail_alloc(struct mailbox_transaction_context *t,
			enum mail_fetch_field wanted_fields,
			struct mailbox_header_lookup_ctx *wanted_headers)
{
	struct lazy_expunge_mailbox *qbox = LAZY_EXPUNGE_CONTEXT(t->box);
	struct lazy_expunge_mail *lmail;
	struct mail *_mail;
	struct mail_private *mail;

	_mail = qbox->super.mail_alloc(t, wanted_fields, wanted_headers);
	mail = (struct mail_private *)_mail;

	lmail = p_new(mail->pool, struct lazy_expunge_mail, 1);
	lmail->super = mail->v;

	mail->v.expunge = lazy_expunge_mail_expunge;
	array_idx_set(&mail->module_contexts,
		      lazy_expunge_storage_module_id, &lmail);
	return _mail;
}

static struct mailbox *
lazy_expunge_mailbox_open(struct mail_storage *storage, const char *name,
			  struct istream *input, enum mailbox_open_flags flags)
{
	struct lazy_expunge_mail_storage *lstorage =
		LAZY_EXPUNGE_CONTEXT(storage);
	struct mailbox *box;
	struct lazy_expunge_mailbox *qbox;

	box = lstorage->super.mailbox_open(storage, name, input, flags);
	if (box == NULL || lstorage->internal_namespace)
		return box;

	qbox = p_new(box->pool, struct lazy_expunge_mailbox, 1);
	qbox->super = box->v;

	box->v.transaction_begin = lazy_expunge_transaction_begin;
	box->v.transaction_commit = lazy_expunge_transaction_commit;
	box->v.transaction_rollback = lazy_expunge_transaction_rollback;
	box->v.mail_alloc = lazy_expunge_mail_alloc;
	array_idx_set(&box->module_contexts,
		      lazy_expunge_storage_module_id, &qbox);
	return box;
}

static int dir_move_or_merge(struct mail_storage *storage,
			     const char *srcdir, const char *destdir)
{
	DIR *dir;
	struct dirent *dp;
	string_t *src_path, *dest_path;
	unsigned int src_dirlen, dest_dirlen;
	int ret = 0;

	if (rename(srcdir, destdir) == 0 || errno == ENOENT)
		return 0;

	if (!EDESTDIREXISTS(errno)) {
		mail_storage_set_critical(storage,
			"rename(%s, %s) failed: %m", srcdir, destdir);
	}

	/* rename all the files separately */
	dir = opendir(srcdir);
	if (dir == NULL) {
		mail_storage_set_critical(storage,
			"opendir(%s) failed: %m", srcdir);
		return -1;
	}

	src_path = t_str_new(512);
	dest_path = t_str_new(512);

	str_append(src_path, srcdir);
	str_append(dest_path, destdir);
	str_append_c(src_path, '/');
	str_append_c(dest_path, '/');
	src_dirlen = str_len(src_path);
	dest_dirlen = str_len(dest_path);

	while ((dp = readdir(dir)) != NULL) {
		if (dp->d_name[0] == '.' &&
		    (dp->d_name[1] == '\0' ||
		     (dp->d_name[1] == '.' && dp->d_name[2] == '\0')))
			continue;

		str_truncate(src_path, src_dirlen);
		str_append(src_path, dp->d_name);
		str_truncate(dest_path, dest_dirlen);
		str_append(dest_path, dp->d_name);

		if (rename(str_c(src_path), str_c(dest_path)) < 0 &&
		    errno != ENOENT) {
			mail_storage_set_critical(storage,
				"rename(%s, %s) failed: %m",
				str_c(src_path), str_c(dest_path));
			ret = -1;
		}
	}
	if (closedir(dir) < 0) {
		mail_storage_set_critical(storage,
			"closedir(%s) failed: %m", srcdir);
		ret = -1;
	}
	if (ret == 0) {
		if (rmdir(srcdir) < 0) {
			mail_storage_set_critical(storage,
				"rmdir(%s) failed: %m", srcdir);
			ret = -1;
		}
	}
	return ret;
}

static int
mailbox_move(struct mail_storage *src_storage, const char *src_name,
	     struct mail_storage *dest_storage, const char **_dest_name)
{
	struct index_storage *src_istorage =
		(struct index_storage *)src_storage;
	struct index_storage *dest_istorage =
		(struct index_storage *)dest_storage;
	const char *dest_name = *_dest_name;
	const char *srcdir, *src2dir, *src3dir, *destdir;
	bool is_file;

	srcdir = mail_storage_get_mailbox_path(src_storage, src_name, &is_file);
	destdir = mail_storage_get_mailbox_path(dest_storage, dest_name,
						&is_file);
	while (rename(srcdir, destdir) < 0) {
		if (errno == ENOENT)
			return 0;

		if (!EDESTDIREXISTS(errno)) {
			mail_storage_set_critical(src_storage,
				"rename(%s, %s) failed: %m", srcdir, destdir);
			return -1;
		}

		/* mailbox is being deleted multiple times per second.
		   update the filename. */
		dest_name = t_strdup_printf("%s-%04u", *_dest_name,
					    (uint32_t)random());
		destdir = mail_storage_get_mailbox_path(dest_storage, dest_name,
							&is_file);
	}

	t_push();
	src2dir = mail_storage_get_mailbox_control_dir(src_storage, src_name);
	if (strcmp(src2dir, srcdir) != 0) {
		destdir = mail_storage_get_mailbox_control_dir(dest_storage,
							       dest_name);
		(void)dir_move_or_merge(src_storage, src2dir, destdir);
	}
	src3dir = t_strconcat(src_istorage->index_dir, "/"MAILDIR_FS_SEP_S,
			      src_name, NULL);
	if (strcmp(src3dir, srcdir) != 0 && strcmp(src3dir, src2dir) != 0) {
		destdir = t_strconcat(dest_istorage->index_dir,
				      "/"MAILDIR_FS_SEP_S,
				      dest_name, NULL);
		(void)dir_move_or_merge(src_storage, src3dir, destdir);
	}
	t_pop();

	*_dest_name = dest_name;
	return 1;
}

static int
lazy_expunge_mailbox_delete(struct mail_storage *storage, const char *name)
{
	struct lazy_expunge_mail_storage *lstorage =
		LAZY_EXPUNGE_CONTEXT(storage);
	struct mail_storage *dest_storage;
	enum mailbox_name_status status;
	const char *destname;
	struct tm *tm;
	char timestamp[256];
	int ret;

	if (lstorage->internal_namespace)
		return lstorage->super.mailbox_delete(storage, name);

	mail_storage_clear_error(storage);

	/* first do the normal sanity checks */
	if (strcmp(name, "INBOX") == 0) {
		mail_storage_set_error(storage, "INBOX can't be deleted.");
		return -1;
	}

	if (mail_storage_get_mailbox_name_status(storage, name, &status) < 0)
		return -1;
	if (status == MAILBOX_NAME_INVALID) {
		mail_storage_set_error(storage, "Invalid mailbox name");
		return -1;
	}

	/* destination mailbox name needs to contain a timestamp */
	tm = localtime(&ioloop_time);
	if (strftime(timestamp, sizeof(timestamp), "%Y%m%d-%H%M%S", tm) == 0)
		strocpy(timestamp, dec2str(ioloop_time), sizeof(timestamp));
	destname = t_strconcat(name, "-", timestamp, NULL);

	/* first move the actual mailbox */
	dest_storage = lazy_namespaces[LAZY_NAMESPACE_DELETE]->storage;
	if ((ret = mailbox_move(storage, name, dest_storage, &destname)) < 0)
		return -1;
	if (ret == 0) {
		mail_storage_set_error(storage,
			MAIL_STORAGE_ERR_MAILBOX_NOT_FOUND, name);
		return -1;
	}

	/* next move the expunged messages mailbox, if it exists */
	storage = lazy_namespaces[LAZY_NAMESPACE_EXPUNGE]->storage;
	dest_storage = lazy_namespaces[LAZY_NAMESPACE_DELETE_EXPUNGE]->storage;
	(void)mailbox_move(storage, name, dest_storage, &destname);
	return 0;
}

static void lazy_expunge_mail_storage_created(struct mail_storage *storage)
{
	struct lazy_expunge_mail_storage *lstorage;

	if (lazy_expunge_next_hook_mail_storage_created != NULL)
		lazy_expunge_next_hook_mail_storage_created(storage);

	/* only maildir supported for now */
	if (strcmp(storage->name, "maildir") != 0)
		return;

	lstorage = p_new(storage->pool, struct lazy_expunge_mail_storage, 1);
	lstorage->super = storage->v;
	storage->v.mailbox_open = lazy_expunge_mailbox_open;
	storage->v.mailbox_delete = lazy_expunge_mailbox_delete;

	if (!lazy_expunge_storage_module_id_set) {
		lazy_expunge_storage_module_id = mail_storage_module_id++;
		lazy_expunge_storage_module_id_set = TRUE;
	}

	array_idx_set(&storage->module_contexts,
		      lazy_expunge_storage_module_id, &lstorage);
}

static void lazy_expunge_hook_client_created(struct client **client)
{
	struct lazy_expunge_mail_storage *lstorage;
	const char *const *p;
	int i;

	if (lazy_expunge_next_hook_client_created != NULL)
		lazy_expunge_next_hook_client_created(client);

	/* FIXME: this works only as long as there's only one client. */
	t_push();
	p = t_strsplit(getenv("LAZY_EXPUNGE"), " ");
	for (i = 0; i < LAZY_NAMESPACE_COUNT; i++, p++) {
		const char *name = *p;

		if (name == NULL)
			i_fatal("lazy_expunge: Missing namespace #%d", i + 1);

		lazy_namespaces[i] =
			namespace_find_prefix((*client)->namespaces, name);
		if (lazy_namespaces[i] == NULL)
			i_fatal("lazy_expunge: Unknown namespace: '%s'", name);
		if (strcmp(lazy_namespaces[i]->storage->name, "maildir") != 0) {
			i_fatal("lazy_expunge: Namespace must be in maildir "
				"format: %s", name);
		}

		/* we don't want to override these namespaces' expunge/delete
		   operations. */
		lstorage = LAZY_EXPUNGE_CONTEXT(lazy_namespaces[i]->storage);
		lstorage->internal_namespace = TRUE;
	}
	t_pop();
}

void lazy_expunge_plugin_init(void)
{
	if (getenv("LAZY_EXPUNGE") == NULL)
		return;

	lazy_expunge_next_hook_client_created = hook_client_created;
	hook_client_created = lazy_expunge_hook_client_created;

	lazy_expunge_next_hook_mail_storage_created =
		hook_mail_storage_created;
	hook_mail_storage_created = lazy_expunge_mail_storage_created;
}

void lazy_expunge_plugin_deinit(void)
{
	if (lazy_expunge_storage_module_id_set) {
		hook_client_created = lazy_expunge_hook_client_created;

		hook_mail_storage_created =
			lazy_expunge_next_hook_mail_storage_created;
	}
}