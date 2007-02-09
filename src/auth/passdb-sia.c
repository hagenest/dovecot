/* Copyright (C) 2006 Simon L Jackson */

/* Tru64 SIA support */

#include "common.h"

#ifdef PASSDB_SIA

#include "safe-memset.h"
#include "passdb.h"

#include <sia.h>
#include <siad.h>
#include <sys/security.h>

static int checkpw_collect(int timeout __attr_unused__, int rendition,
			   uchar_t *title __attr_unused__,
			   int nprompts __attr_unused__,
			   prompt_t *prompts __attr_unused__)
{
	switch (rendition) {
	case SIAONELINER:
	case SIAINFO:
	case SIAWARNING:
		return SIACOLSUCCESS;
	}

	/* everything else is bogus */
	return SIACOLABORT;
}

static void
local_sia_verify_plain(struct auth_request *request, const char *password,
		       verify_plain_callback_t *callback)
{
	char *argutility = "dovecot";

	/* check if the password is valid */
	if (sia_validate_user(checkpw_collect, 1, &argutility, NULL,
			      (char *)request->user, NULL, NULL, NULL,
			      (char *)password) != SIASUCCESS) {
		auth_request_log_info(request, "sia", "password mismatch");
                callback(PASSDB_RESULT_PASSWORD_MISMATCH, request);
	} else {
		callback(PASSDB_RESULT_OK, request);
	}
}

struct passdb_module_interface passdb_sia = {
        "sia",

        NULL,
        NULL,
        NULL,

        local_sia_verify_plain,
        NULL
};

#endif