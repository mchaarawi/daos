/**
 * (C) Copyright 2016-2019 Intel Corporation.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * GOVERNMENT LICENSE RIGHTS-OPEN SOURCE SOFTWARE
 * The Government's rights to use, modify, reproduce, release, perform, display,
 * or disclose this software are subject to the terms of the Apache License as
 * provided in Contract No. B609815.
 * Any reproduction of computer software, computer software documentation, or
 * portions thereof marked with this legend must also reproduce the markings.
 */

#include "dfuse_common.h"
#include "dfuse.h"
#include "daos_fs.h"
#include "daos_api.h"

/* Lookup a container within a pool */
static bool
dfuse_cont_open(fuse_req_t req, struct dfuse_inode_entry *parent,
		const char *name, bool create)
{
	struct dfuse_projection_info	*fs_handle = fuse_req_userdata(req);
	struct dfuse_inode_entry	*ie = NULL;
	struct dfuse_dfs		*dfs;
	uuid_t				co_uuid;
	dfs_t				*ddfs;
	int				rc;

	/* This code is only supposed to support one level of directory descent
	 * so check that the lookup is relative to the root of the sub-tree,
	 * and abort if not.
	 */
	D_ASSERT(parent->ie_stat.st_ino == parent->ie_dfs->dfs_root);

	/* Dentry names where are not valid uuids cannot possibly be added so in
	 * this case return the negative dentry with a timeout to prevent future
	 * lookups.
	 */
	if (uuid_parse(name, co_uuid) < 0) {
		struct fuse_entry_param entry = {.entry_timeout = 60};

		DFUSE_LOG_ERROR("Invalid container uuid");
		DFUSE_REPLY_ENTRY(req, entry);
		return false;
	}

	D_ALLOC_PTR(dfs);
	if (!dfs) {
		D_GOTO(err, rc = ENOMEM);
	}
	strncpy(dfs->dfs_cont, name, NAME_MAX);
	dfs->dfs_cont[NAME_MAX] = '\0';
	strncpy(dfs->dfs_pool, parent->ie_dfs->dfs_pool, NAME_MAX - 1);
	dfs->dfs_pool[NAME_MAX] = '\0';

	if (create) {
		rc = daos_cont_create(parent->ie_dfs->dfs_poh, co_uuid,
				      NULL, NULL);
		if (rc != -DER_SUCCESS) {
			DFUSE_LOG_ERROR("daos_cont_create() failed: (%d)", rc);
			D_GOTO(err, rc = daos_der2errno(rc));
		}
	} else {
		rc = dfuse_check_for_inode(fs_handle, dfs, &ie);
		if (rc == -DER_SUCCESS) {
			struct fuse_entry_param	entry = {0};

			DFUSE_TRA_INFO(ie, "Reusing existing container entry "
				       "without reconnect");

			D_FREE(dfs);

			/* Update the stat information, but copy in the
			 * inode value afterwards.
			 */
			rc = dfs_ostat(ie->ie_dfs->dfs_ns,
				       ie->ie_obj, &entry.attr);
			if (rc) {
				DFUSE_TRA_ERROR(ie, "dfs_ostat() failed: (%s)",
						strerror(-rc));
				D_GOTO(err, rc = -rc);
			}

			entry.attr.st_ino = ie->ie_stat.st_ino;
			entry.generation = 1;
			entry.ino = entry.attr.st_ino;
			DFUSE_REPLY_ENTRY(req, entry);
			return true;
		}
	}

	rc = daos_cont_open(parent->ie_dfs->dfs_poh, co_uuid,
			    DAOS_COO_RW, &dfs->dfs_coh, &dfs->dfs_co_info,
			    NULL);
	if (rc != -DER_SUCCESS) {
		DFUSE_LOG_ERROR("daos_cont_open() failed: (%d)", rc);
		D_GOTO(err, rc = daos_der2errno(rc));
	}

	D_ALLOC_PTR(ie);
	if (!ie) {
		D_GOTO(close, rc = ENOMEM);
	}

	rc = dfs_mount(parent->ie_dfs->dfs_poh, dfs->dfs_coh, O_RDWR, &ddfs);
	if (rc) {
		DFUSE_LOG_ERROR("dfs_mount() failed: (%s)", strerror(-rc));
		D_GOTO(close, rc = -rc);
	}

	dfs->dfs_ns = ddfs;

	rc = dfs_lookup(dfs->dfs_ns, "/", O_RDONLY, &ie->ie_obj, NULL,
			&ie->ie_stat);
	if (rc) {
		DFUSE_TRA_ERROR(ie, "dfs_lookup() failed: (%s)", strerror(-rc));
		D_GOTO(close, rc = -rc);
	}

	ie->ie_parent = parent->ie_stat.st_ino;
	strncpy(ie->ie_name, name, NAME_MAX);
	ie->ie_name[NAME_MAX] = '\0';

	atomic_fetch_add(&ie->ie_ref, 1);
	ie->ie_dfs = dfs;

	rc = dfuse_lookup_inode(fs_handle, ie->ie_dfs, NULL,
				&ie->ie_stat.st_ino);
	if (rc) {
		DFUSE_TRA_ERROR(ie, "dfuse_lookup_inode() failed: (%d)", rc);
		D_GOTO(release, rc = -rc);
	}

	dfs->dfs_root = ie->ie_stat.st_ino;
	dfs->dfs_ops = &dfuse_dfs_ops;

	dfuse_reply_entry(fs_handle, ie, NULL, req);
	return true;

release:
	dfs_release(ie->ie_obj);
close:
	daos_cont_close(dfs->dfs_coh, NULL);
	D_FREE(ie);
err:
	DFUSE_REPLY_ERR_RAW(fs_handle, req, rc);
	D_FREE(dfs);
	return false;
}

bool
dfuse_cont_lookup(fuse_req_t req, struct dfuse_inode_entry *parent,
		  const char *name)
{
	return dfuse_cont_open(req, parent, name, false);
}

bool
dfuse_cont_mkdir(fuse_req_t req, struct dfuse_inode_entry *parent,
		 const char *name, mode_t mode)
{
	return dfuse_cont_open(req, parent, name, true);
}
