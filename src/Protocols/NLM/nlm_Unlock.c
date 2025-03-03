// SPDX-License-Identifier: LGPL-3.0-or-later
/*
 * Copyright IBM Corporation, 2010
 *  Contributor: Aneesh Kumar K.v  <aneesh.kumar@linux.vnet.ibm.com>
 *
 * --------------------------
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 3 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301 USA
 *
 */

#include "config.h"
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include "log.h"
#include "fsal.h"
#include "nfs_proto_functions.h"
#include "sal_functions.h"
#include "nlm_util.h"
#include "nlm_async.h"

/**
 * @brief Free a range lock
 *
 * @param[in]  args
 * @param[in]  req
 * @param[out] res
 */

int nlm4_Unlock(nfs_arg_t *args, struct svc_req *req, nfs_res_t *res)
{
	nlm4_unlockargs *arg = &args->arg_nlm4_unlock;
	struct fsal_obj_handle *obj;
	state_status_t state_status = STATE_SUCCESS;
	char buffer[MAXNETOBJ_SZ * 2] = "\0";
	state_nsm_client_t *nsm_client;
	state_nlm_client_t *nlm_client;
	state_owner_t *nlm_owner;
	fsal_lock_param_t lock;
	int rc;
	state_t *state;

	/* NLM doesn't have a BADHANDLE error, nor can rpc_execute deal with
	 * responding to an NLM_*_MSG call, so we check here if the export is
	 * NULL and if so, handle the response.
	 */
	if (op_ctx->ctx_export == NULL) {
		res->res_nlm4.stat.stat = NLM4_STALE_FH;
		LogInfo(COMPONENT_NLM, "INVALID HANDLE: NLM4_UNLOCK");
		return NFS_REQ_OK;
	}

	netobj_to_string(&arg->cookie, buffer, sizeof(buffer));

	LogDebug(
		COMPONENT_NLM,
		"REQUEST PROCESSING: Calling NLM4_UNLOCK svid=%d off=%llx len=%llx cookie=%s",
		(int)arg->alock.svid, (unsigned long long)arg->alock.l_offset,
		(unsigned long long)arg->alock.l_len, buffer);

	copy_netobj(&res->res_nlm4test.cookie, &arg->cookie);

	/* unlock doesn't care if owner is found */
	rc = nlm_process_parameters(req, false, &arg->alock, &lock, &obj,
				    CARE_NOT, &nsm_client, &nlm_client,
				    &nlm_owner, NULL, 0, &state);

	if (rc >= 0) {
		/* resent the error back to the client */
		res->res_nlm4.stat.stat = (nlm4_stats)rc;
		LogDebug(COMPONENT_NLM, "REQUEST RESULT: NLM4_UNLOCK %s",
			 lock_result_str(res->res_nlm4.stat.stat));
		return NFS_REQ_OK;
	}

	if (state != NULL)
		state_status =
			state_unlock(obj, state, nlm_owner, false, 0, &lock);

	if (state_status != STATE_SUCCESS) {
		/* Unlock could fail in the FSAL and make a bit of a mess,
		 * especially if we are in out of memory situation. Such an
		 * error is already logged.
		 */
		res->res_nlm4test.test_stat.stat =
			nlm_convert_state_error(state_status);
	} else {
		res->res_nlm4.stat.stat = NLM4_GRANTED;
	}

	/* Release the NLM Client and NLM Owner references we have */
	if (state != NULL)
		dec_state_t_ref(state);
	dec_nsm_client_ref(nsm_client);
	dec_nlm_client_ref(nlm_client);
	dec_state_owner_ref(nlm_owner);
	obj->obj_ops->put_ref(obj);

	LogDebug(COMPONENT_NLM, "REQUEST RESULT: NLM4_UNLOCK %s",
		 lock_result_str(res->res_nlm4.stat.stat));
	return NFS_REQ_OK;
}

static void nlm4_unlock_message_resp(state_async_queue_t *arg)
{
	state_nlm_async_data_t *nlm_arg =
		&arg->state_async_data.state_nlm_async_data;
	nfs_res_t *res = &nlm_arg->nlm_async_args.nlm_async_res;

	if (isFullDebug(COMPONENT_NLM)) {
		char buffer[1024] = "\0";

		netobj_to_string(&res->res_nlm4test.cookie, buffer, 1024);

		LogFullDebug(COMPONENT_NLM,
			     "Calling nlm_send_async cookie=%s status=%s",
			     buffer, lock_result_str(res->res_nlm4.stat.stat));
	}

	nlm_send_async(NLMPROC4_UNLOCK_RES, nlm_arg->nlm_async_host, res, NULL);

	nlm4_Unlock_Free(res);
	dec_nsm_client_ref(nlm_arg->nlm_async_host->slc_nsm_client);
	dec_nlm_client_ref(nlm_arg->nlm_async_host);
	gsh_free(arg);
}

/**
 * @brief Unlock Message
 *
 * @param[in]  args
 * @param[in]  req
 * @param[out] res
 *
 */
int nlm4_Unlock_Message(nfs_arg_t *args, struct svc_req *req, nfs_res_t *res)
{
	state_nlm_client_t *nlm_client = NULL;
	state_nsm_client_t *nsm_client;
	nlm4_unlockargs *arg = &args->arg_nlm4_unlock;
	int rc = NFS_REQ_OK;

	LogDebug(COMPONENT_NLM,
		 "REQUEST PROCESSING: Calling nlm_Unlock_Message");

	nsm_client = get_nsm_client(CARE_NO_MONITOR, arg->alock.caller_name);

	if (nsm_client != NULL)
		nlm_client = get_nlm_client(CARE_NO_MONITOR, req->rq_xprt,
					    nsm_client, arg->alock.caller_name);

	if (nlm_client == NULL)
		rc = NFS_REQ_DROP;
	else
		rc = nlm4_Unlock(args, req, res);

	if (rc == NFS_REQ_OK)
		rc = nlm_send_async_res_nlm4(nlm_client,
					     nlm4_unlock_message_resp, res);

	if (rc == NFS_REQ_DROP) {
		if (nsm_client != NULL)
			dec_nsm_client_ref(nsm_client);

		if (nlm_client != NULL)
			dec_nlm_client_ref(nlm_client);

		LogCrit(COMPONENT_NLM,
			"Could not send async response for nlm_Unlock_Message");
	}

	return NFS_REQ_DROP;
}

/**
 * nlm4_Unlock_Free: Frees the result structure allocated for nlm4_Unlock
 *
 * Frees the result structure allocated for nlm4_Lock. Does Nothing in fact.
 *
 * @param res        [INOUT]   Pointer to the result structure.
 *
 */
void nlm4_Unlock_Free(nfs_res_t *res)
{
	netobj_free(&res->res_nlm4test.cookie);
}
