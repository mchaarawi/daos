/*
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
/**
 * \file
 *
 * ds_pool: Pool Service
 *
 * This file contains the server API methods and the RPC handlers that are both
 * related pool metadata.
 */

#define D_LOGFAC DD_FAC(pool)

#include <daos_srv/pool.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <daos_api.h> /* for daos_prop_alloc/_free() */
#include <daos_security.h>
#include <daos/pool_map.h>
#include <daos/rpc.h>
#include <daos/rsvc.h>
#include <daos_srv/container.h>
#include <daos_srv/daos_mgmt_srv.h>
#include <daos_srv/daos_server.h>
#include <daos_srv/rdb.h>
#include <daos_srv/rebuild.h>
#include <daos_srv/security.h>
#include <cart/iv.h>
#include "rpc.h"
#include "srv_internal.h"
#include "srv_layout.h"

/* Pool service */
struct pool_svc {
	struct ds_rsvc		ps_rsvc;
	uuid_t			ps_uuid;	/* pool UUID */
	struct cont_svc	       *ps_cont_svc;	/* one combined svc for now */
	ABT_rwlock		ps_lock;	/* for DB data */
	rdb_path_t		ps_root;	/* root KVS */
	rdb_path_t		ps_handles;	/* pool handle KVS */
	rdb_path_t		ps_user;	/* pool user attributes KVS */
	struct ds_pool	       *ps_pool;
};

static struct pool_svc *
pool_svc_obj(struct ds_rsvc *rsvc)
{
	return container_of(rsvc, struct pool_svc, ps_rsvc);
}

static int
write_map_buf(struct rdb_tx *tx, const rdb_path_t *kvs, struct pool_buf *buf,
	      uint32_t version)
{
	d_iov_t	value;
	int		rc;

	D_DEBUG(DF_DSMS, "version=%u ntargets=%u ndomains=%u\n", version,
		buf->pb_target_nr, buf->pb_domain_nr);

	/* Write the version. */
	d_iov_set(&value, &version, sizeof(version));
	rc = rdb_tx_update(tx, kvs, &ds_pool_prop_map_version, &value);
	if (rc != 0)
		return rc;

	/* Write the buffer. */
	d_iov_set(&value, buf, pool_buf_size(buf->pb_nr));
	return rdb_tx_update(tx, kvs, &ds_pool_prop_map_buffer, &value);
}

/*
 * Retrieve the pool map buffer address in persistent memory and the pool map
 * version into "map_buf" and "map_version", respectively.
 */
static int
read_map_buf(struct rdb_tx *tx, const rdb_path_t *kvs, struct pool_buf **buf,
	     uint32_t *version)
{
	uint32_t	ver;
	d_iov_t	value;
	int		rc;

	/* Read the version. */
	d_iov_set(&value, &ver, sizeof(ver));
	rc = rdb_tx_lookup(tx, kvs, &ds_pool_prop_map_version, &value);
	if (rc != 0)
		return rc;

	/* Look up the buffer address. */
	d_iov_set(&value, NULL /* buf */, 0 /* size */);
	rc = rdb_tx_lookup(tx, kvs, &ds_pool_prop_map_buffer, &value);
	if (rc != 0)
		return rc;

	*buf = value.iov_buf;
	*version = ver;
	D_DEBUG(DF_DSMS, "version=%u ntargets=%u ndomains=%u\n", *version,
		(*buf)->pb_target_nr, (*buf)->pb_domain_nr);
	return 0;
}

/* Callers are responsible for destroying the object via pool_map_decref(). */
static int
read_map(struct rdb_tx *tx, const rdb_path_t *kvs, struct pool_map **map)
{
	struct pool_buf	       *buf;
	uint32_t		version;
	int			rc;

	rc = read_map_buf(tx, kvs, &buf, &version);
	if (rc != 0)
		return rc;

	return pool_map_create(buf, version, map);
}

/* Store uuid in file path. */
static int
uuid_store(const char *path, const uuid_t uuid)
{
	int	fd;
	int	rc;

	/* Create and open the UUID file. */
	fd = open(path, O_WRONLY | O_CREAT | O_EXCL, S_IRUSR | S_IWUSR);
	if (fd < 0) {
		D_ERROR(DF_UUID": failed to create uuid file %s: %d\n",
			DP_UUID(uuid), path, errno);
		rc = daos_errno2der(errno);
		goto out;
	}

	/* Write the UUID. */
	rc = write(fd, uuid, sizeof(uuid_t));
	if (rc != sizeof(uuid_t)) {
		if (rc != -1)
			errno = EIO;
		D_ERROR(DF_UUID": failed to write uuid into %s: %d %d\n",
			DP_UUID(uuid), path, rc, errno);
		rc = daos_errno2der(errno);
		goto out_fd;
	}

	/* Persist the UUID. */
	rc = fsync(fd);
	if (rc != 0) {
		D_ERROR(DF_UUID": failed to fsync %s: %d\n", DP_UUID(uuid),
			path, errno);
		rc = daos_errno2der(errno);
	}

	/* Free the resource and remove the file on errors. */
out_fd:
	close(fd);
	if (rc != 0)
		remove(path);
out:
	return rc;
}

/* Load uuid from file path. */
static int
uuid_load(const char *path, uuid_t uuid)
{
	int	fd;
	int	rc;

	/* Open the UUID file. */
	fd = open(path, O_RDONLY);
	if (fd < 0) {
		if (errno == ENOENT)
			D_DEBUG(DB_MD, "failed to open uuid file %s: %d\n",
				path, errno);
		else
			D_ERROR("failed to open uuid file %s: %d\n", path,
				errno);
		rc = daos_errno2der(errno);
		goto out;
	}

	/* Read the UUID. */
	rc = read(fd, uuid, sizeof(uuid_t));
	if (rc == sizeof(uuid_t)) {
		rc = 0;
	} else {
		if (rc != -1)
			errno = EIO;
		D_ERROR("failed to read %s: %d %d\n", path, rc, errno);
		rc = daos_errno2der(errno);
	}

	close(fd);
out:
	return rc;
}

static char *
pool_svc_rdb_path_common(const uuid_t pool_uuid, const char *suffix)
{
	char   *name;
	char   *path;
	int	rc;

	rc = asprintf(&name, RDB_FILE"pool%s", suffix);
	if (rc < 0)
		return NULL;
	rc = ds_mgmt_tgt_file(pool_uuid, name, NULL /* idx */, &path);
	D_FREE(name);
	if (rc != 0)
		return NULL;
	return path;
}

/* Return a pool service RDB path. */
static char *
pool_svc_rdb_path(const uuid_t pool_uuid)
{
	return pool_svc_rdb_path_common(pool_uuid, "");
}

/* Return a pool service RDB UUID file path. This file stores the RDB UUID. */
static char *
pool_svc_rdb_uuid_path(const uuid_t pool_uuid)
{
	return pool_svc_rdb_path_common(pool_uuid, "-uuid");
}

/*
 * Called by mgmt module on every storage node belonging to this pool.
 * "path" is the directory under which the VOS and metadata files shall be.
 * "target_uuid" returns the UUID generated for the target on this storage node.
 */
int
ds_pool_create(const uuid_t pool_uuid, const char *path, uuid_t target_uuid)
{
	char   *fpath;
	int	rc;

	uuid_generate(target_uuid);

	/* Store target_uuid in DSM_META_FILE. */
	rc = asprintf(&fpath, "%s/%s", path, DSM_META_FILE);
	if (rc < 0)
		return -DER_NOMEM;
	rc = uuid_store(fpath, target_uuid);
	D_FREE(fpath);

	return rc;
}

static int
uuid_compare_cb(const void *a, const void *b)
{
	uuid_t *ua = (uuid_t *)a;
	uuid_t *ub = (uuid_t *)b;

	return uuid_compare(*ua, *ub);
}

static void
pool_prop_copy_ptr(struct daos_prop_entry *entry_def,
		struct daos_prop_entry *entry, size_t len)
{
	D_ALLOC(entry_def->dpe_val_ptr, len);
	if (entry_def->dpe_val_ptr != NULL) {
		memcpy(entry_def->dpe_val_ptr, entry->dpe_val_ptr, len);
	}
}

static uint32_t
pool_prop_acl_get_length(struct daos_prop_entry *entry)
{
	if (entry->dpe_val_ptr == NULL) {
		D_WARN("ACL pool property was NULL\n");
		return 0;
	}

	return daos_acl_get_size((struct daos_acl *)entry->dpe_val_ptr);
}

/* copy \a prop to \a prop_def (duplicated default prop) */
static int
pool_prop_default_copy(daos_prop_t *prop_def, daos_prop_t *prop)
{
	struct daos_prop_entry	*entry;
	struct daos_prop_entry	*entry_def;
	int			 i;

	if (prop == NULL || prop->dpp_nr == 0 || prop->dpp_entries == NULL)
		return 0;

	for (i = 0; i < prop->dpp_nr; i++) {
		entry = &prop->dpp_entries[i];
		entry_def = daos_prop_entry_get(prop_def, entry->dpe_type);
		D_ASSERTF(entry_def != NULL, "type %d not found in "
			  "default prop.\n", entry->dpe_type);
		switch (entry->dpe_type) {
		case DAOS_PROP_PO_LABEL:
			D_FREE(entry_def->dpe_str);
			entry_def->dpe_str = strndup(entry->dpe_str,
						     DAOS_PROP_LABEL_MAX_LEN);
			if (entry_def->dpe_str == NULL)
				return -DER_NOMEM;
			break;
		case DAOS_PROP_PO_OWNER:
		case DAOS_PROP_PO_OWNER_GROUP:
			D_FREE(entry_def->dpe_str);
			entry_def->dpe_str =
					strndup(entry->dpe_str,
						DAOS_ACL_MAX_PRINCIPAL_LEN);
			if (entry_def->dpe_str == NULL)
				return -DER_NOMEM;
			break;
		case DAOS_PROP_PO_SPACE_RB:
		case DAOS_PROP_PO_SELF_HEAL:
		case DAOS_PROP_PO_RECLAIM:
			entry_def->dpe_val = entry->dpe_val;
			break;
		case DAOS_PROP_PO_ACL:
			if (entry->dpe_val_ptr != NULL) {
				pool_prop_copy_ptr(entry_def, entry,
					pool_prop_acl_get_length(entry));
				if (entry_def->dpe_val_ptr == NULL)
					return -DER_NOMEM;
			}
			break;
		default:
			D_ERROR("ignore bad dpt_type %d.\n", entry->dpe_type);
			break;
		}
	}

	return 0;
}

static int
pool_prop_write(struct rdb_tx *tx, const rdb_path_t *kvs, daos_prop_t *prop)
{
	struct daos_prop_entry	*entry;
	d_iov_t		 value;
	int			 i;
	int			 rc = 0;

	if (prop == NULL || prop->dpp_nr == 0 || prop->dpp_entries == NULL)
		return 0;

	for (i = 0; i < prop->dpp_nr; i++) {
		entry = &prop->dpp_entries[i];
		switch (entry->dpe_type) {
		case DAOS_PROP_PO_LABEL:
			d_iov_set(&value, entry->dpe_str,
				     strlen(entry->dpe_str));
			rc = rdb_tx_update(tx, kvs, &ds_pool_prop_label,
					   &value);
			if (rc)
				return rc;
			break;
		case DAOS_PROP_PO_OWNER:
			d_iov_set(&value, entry->dpe_str,
				     strlen(entry->dpe_str));
			rc = rdb_tx_update(tx, kvs, &ds_pool_prop_owner,
					   &value);
			if (rc)
				return rc;
			break;
		case DAOS_PROP_PO_OWNER_GROUP:
			d_iov_set(&value, entry->dpe_str,
				     strlen(entry->dpe_str));
			rc = rdb_tx_update(tx, kvs, &ds_pool_prop_owner_group,
					   &value);
			if (rc)
				return rc;
			break;
		case DAOS_PROP_PO_ACL:
			if (entry->dpe_val_ptr != NULL) {
				d_iov_set(&value, entry->dpe_val_ptr,
					     pool_prop_acl_get_length(entry));
				rc = rdb_tx_update(tx, kvs, &ds_pool_prop_acl,
						   &value);
				if (rc)
					return rc;
			}
			break;
		case DAOS_PROP_PO_SPACE_RB:
			d_iov_set(&value, &entry->dpe_val,
				     sizeof(entry->dpe_val));
			rc = rdb_tx_update(tx, kvs, &ds_pool_prop_space_rb,
					   &value);
			if (rc)
				return rc;
			break;
		case DAOS_PROP_PO_SELF_HEAL:
			d_iov_set(&value, &entry->dpe_val,
				     sizeof(entry->dpe_val));
			rc = rdb_tx_update(tx, kvs, &ds_pool_prop_self_heal,
					   &value);
			if (rc)
				return rc;
			break;
		case DAOS_PROP_PO_RECLAIM:
			d_iov_set(&value, &entry->dpe_val,
				     sizeof(entry->dpe_val));
			rc = rdb_tx_update(tx, kvs, &ds_pool_prop_reclaim,
					   &value);
			if (rc)
				return rc;
			break;
		default:
			D_ERROR("bad dpe_type %d.\n", entry->dpe_type);
			return -DER_INVAL;
		}
	}

	return rc;
}

static int
init_pool_metadata(struct rdb_tx *tx, const rdb_path_t *kvs,
		   uint32_t nnodes, uuid_t target_uuids[], const char *group,
		   const d_rank_list_t *target_addrs, daos_prop_t *prop,
		   uint32_t ndomains, const int32_t *domains)
{
	struct pool_buf	       *map_buf;
	struct pool_component	map_comp;
	uint32_t		map_version = 1;
	uint32_t		nhandles = 0;
	uuid_t		       *uuids;
	d_iov_t		value;
	struct rdb_kvs_attr	attr;
	int			ntargets = nnodes * dss_tgt_nr;
	int			rc;
	int			i;

	/* Prepare the pool map attribute buffers. */
	map_buf = pool_buf_alloc(ndomains + nnodes + ntargets);
	if (map_buf == NULL)
		return -DER_NOMEM;
	/*
	 * Make a sorted target UUID array to determine target IDs. See the
	 * bsearch() call below.
	 */
	D_ALLOC_ARRAY(uuids, nnodes);
	if (uuids == NULL)
		D_GOTO(out_map_buf, rc = -DER_NOMEM);
	memcpy(uuids, target_uuids, sizeof(uuid_t) * nnodes);
	qsort(uuids, nnodes, sizeof(uuid_t), uuid_compare_cb);

	/* Fill the pool_buf out. */
	/* fill domains */
	for (i = 0; i < ndomains; i++) {
		map_comp.co_type = PO_COMP_TP_RACK;	/* TODO */
		map_comp.co_status = PO_COMP_ST_UPIN;
		map_comp.co_index = i;
		map_comp.co_id = i;
		map_comp.co_rank = 0;
		map_comp.co_ver = map_version;
		map_comp.co_fseq = 1;
		map_comp.co_nr = domains[i];

		rc = pool_buf_attach(map_buf, &map_comp, 1 /* comp_nr */);
		if (rc != 0)
			D_GOTO(out_uuids, rc);
	}

	/* fill nodes */
	for (i = 0; i < nnodes; i++) {
		uuid_t *p = bsearch(target_uuids[i], uuids, nnodes,
				    sizeof(uuid_t), uuid_compare_cb);

		map_comp.co_type = PO_COMP_TP_NODE;
		map_comp.co_status = PO_COMP_ST_UPIN;
		map_comp.co_index = i;
		map_comp.co_id = p - uuids;
		map_comp.co_rank = target_addrs->rl_ranks[i];
		map_comp.co_ver = map_version;
		map_comp.co_fseq = 1;
		map_comp.co_nr = dss_tgt_nr;

		rc = pool_buf_attach(map_buf, &map_comp, 1 /* comp_nr */);
		if (rc != 0)
			D_GOTO(out_uuids, rc);
	}

	/* fill targets */
	for (i = 0; i < nnodes; i++) {
		int j;

		for (j = 0; j < dss_tgt_nr; j++) {
			map_comp.co_type = PO_COMP_TP_TARGET;
			map_comp.co_status = PO_COMP_ST_UPIN;
			map_comp.co_index = j;
			map_comp.co_id = i * dss_tgt_nr + j;
			map_comp.co_rank = target_addrs->rl_ranks[i];
			map_comp.co_ver = map_version;
			map_comp.co_fseq = 1;
			map_comp.co_nr = 1;

			rc = pool_buf_attach(map_buf, &map_comp, 1);
			if (rc != 0)
				D_GOTO(out_uuids, rc);
		}
	}

	/* Initialize the pool map properties. */
	rc = write_map_buf(tx, kvs, map_buf, map_version);
	if (rc != 0)
		D_GOTO(out_uuids, rc);
	d_iov_set(&value, uuids, sizeof(uuid_t) * nnodes);
	rc = rdb_tx_update(tx, kvs, &ds_pool_prop_map_uuids, &value);
	if (rc != 0)
		D_GOTO(out_uuids, rc);

	/* Write the optional properties. */
	rc = pool_prop_write(tx, kvs, prop);
	if (rc != 0)
		D_GOTO(out_uuids, rc);

	/* Write the handle properties. */
	d_iov_set(&value, &nhandles, sizeof(nhandles));
	rc = rdb_tx_update(tx, kvs, &ds_pool_prop_nhandles, &value);
	if (rc != 0)
		D_GOTO(out_uuids, rc);
	attr.dsa_class = RDB_KVS_GENERIC;
	attr.dsa_order = 16;
	rc = rdb_tx_create_kvs(tx, kvs, &ds_pool_prop_handles, &attr);
	if (rc != 0)
		D_GOTO(out_uuids, rc);

	/* Create pool user attributes KVS */
	rc = rdb_tx_create_kvs(tx, kvs, &ds_pool_attr_user, &attr);
	if (rc != 0)
		D_GOTO(out_uuids, rc);

out_uuids:
	D_FREE(uuids);
out_map_buf:
	pool_buf_free(map_buf);
	return rc;
}

/*
 * nreplicas inputs how many replicas are wanted, while ranks->rl_nr
 * outputs how many replicas are actually selected, which may be less than
 * nreplicas. If successful, callers are responsible for calling
 * d_rank_list_free(*ranksp).
 */
static int
select_svc_ranks(int nreplicas, const d_rank_list_t *target_addrs,
		 int ndomains, const int *domains, d_rank_list_t **ranksp)
{
	int			i_rank_zero = -1;
	int			selectable;
	d_rank_list_t       *ranks;
	int			i;
	int			j;

	if (nreplicas <= 0)
		return -DER_INVAL;

	/* Determine the number of selectable targets. */
	selectable = target_addrs->rl_nr;
	if (daos_rank_list_find((d_rank_list_t *)target_addrs, 0 /* rank */,
				&i_rank_zero)) {
		/*
		 * Unless it is the only target available, we don't select rank
		 * 0 for now to avoid losing orterun stdout.
		 */
		if (selectable > 1)
			selectable -= 1 /* rank 0 */;
	}

	if (nreplicas > selectable)
		nreplicas = selectable;
	ranks = daos_rank_list_alloc(nreplicas);
	if (ranks == NULL)
		return -DER_NOMEM;

	/* TODO: Choose ranks according to failure domains. */
	j = 0;
	for (i = 0; i < target_addrs->rl_nr; i++) {
		if (j == ranks->rl_nr)
			break;
		if (i == i_rank_zero && selectable > 1)
			/* This is rank 0 and it's not the only rank. */
			continue;
		D_DEBUG(DB_MD, "ranks[%d]: %u\n", j, target_addrs->rl_ranks[i]);
		ranks->rl_ranks[j] = target_addrs->rl_ranks[i];
		j++;
	}
	D_ASSERTF(j == ranks->rl_nr, "%d == %u\n", j, ranks->rl_nr);

	*ranksp = ranks;
	return 0;
}

/**
 * Create a (combined) pool(/container) service. This method shall be called on
 * a single storage node in the pool. "target_uuids" shall be an array of the
 * target UUIDs returned by the ds_pool_create() calls.
 *
 * \param[in]		pool_uuid	pool UUID
 * \param[in]		ntargets	number of targets in the pool
 * \param[in]		target_uuids	array of \a ntargets target UUIDs
 * \param[in]		group		crt group ID (unused now)
 * \param[in]		target_addrs	list of \a ntargets target ranks
 * \param[in]		ndomains	number of domains the pool spans over
 * \param[in]		domains		serialized domain tree
 * \param[in]		prop		pool properties
 * \param[in,out]	svc_addrs	\a svc_addrs.rl_nr inputs how many
 *					replicas shall be created; returns the
 *					list of pool service replica ranks
 */
int
ds_pool_svc_create(const uuid_t pool_uuid, int ntargets, uuid_t target_uuids[],
		   const char *group, const d_rank_list_t *target_addrs,
		   int ndomains, const int *domains, daos_prop_t *prop,
		   d_rank_list_t *svc_addrs)
{
	d_rank_list_t	       *ranks;
	uuid_t			rdb_uuid;
	d_iov_t		psid;
	struct rsvc_client	client;
	struct dss_module_info *info = dss_get_module_info();
	crt_endpoint_t		ep;
	crt_rpc_t	       *rpc;
	struct pool_create_in  *in;
	struct pool_create_out *out;
	int			rc;

	D_ASSERTF(ntargets == target_addrs->rl_nr, "ntargets=%u num=%u\n",
		  ntargets, target_addrs->rl_nr);

	rc = select_svc_ranks(svc_addrs->rl_nr, target_addrs, ndomains,
			      domains, &ranks);
	if (rc != 0)
		D_GOTO(out, rc);

	uuid_generate(rdb_uuid);
	d_iov_set(&psid, (void *)pool_uuid, sizeof(uuid_t));
	rc = ds_rsvc_dist_start(DS_RSVC_CLASS_POOL, &psid, rdb_uuid, ranks,
				true /* create */, true /* bootstrap */,
				ds_rsvc_get_md_cap());
	if (rc != 0)
		D_GOTO(out_ranks, rc);

	rc = rsvc_client_init(&client, ranks);
	if (rc != 0)
		D_GOTO(out_creation, rc);

rechoose:
	/* Create a POOL_CREATE request. */
	ep.ep_grp = NULL;
	rsvc_client_choose(&client, &ep);
	rc = pool_req_create(info->dmi_ctx, &ep, POOL_CREATE, &rpc);
	if (rc != 0) {
		D_ERROR(DF_UUID": failed to create POOL_CREATE RPC: %d\n",
			DP_UUID(pool_uuid), rc);
		D_GOTO(out_client, rc);
	}
	in = crt_req_get(rpc);
	uuid_copy(in->pri_op.pi_uuid, pool_uuid);
	uuid_clear(in->pri_op.pi_hdl);
	in->pri_ntgts = ntargets;
	in->pri_tgt_uuids.ca_count = ntargets;
	in->pri_tgt_uuids.ca_arrays = target_uuids;
	in->pri_tgt_ranks = (d_rank_list_t *)target_addrs;
	in->pri_prop = prop;
	in->pri_ndomains = ndomains;
	in->pri_domains.ca_count = ndomains;
	in->pri_domains.ca_arrays = (int *)domains;

	/* Send the POOL_CREATE request. */
	rc = dss_rpc_send(rpc);
	out = crt_reply_get(rpc);
	D_ASSERT(out != NULL);
	rc = rsvc_client_complete_rpc(&client, &ep, rc,
				      rc == 0 ? out->pro_op.po_rc : -DER_IO,
				      rc == 0 ? &out->pro_op.po_hint : NULL);
	if (rc == RSVC_CLIENT_RECHOOSE) {
		crt_req_decref(rpc);
		dss_sleep(1000 /* ms */);
		D_GOTO(rechoose, rc);
	}
	rc = out->pro_op.po_rc;
	if (rc != 0) {
		D_ERROR(DF_UUID": failed to create pool: %d\n",
			DP_UUID(pool_uuid), rc);
		D_GOTO(out_rpc, rc);
	}

	rc = daos_rank_list_copy(svc_addrs, ranks);
	D_ASSERTF(rc == 0, "daos_rank_list_copy: %d\n", rc);
out_rpc:
	crt_req_decref(rpc);
out_client:
	rsvc_client_fini(&client);
out_creation:
	if (rc != 0)
		ds_rsvc_dist_stop(DS_RSVC_CLASS_POOL, &psid, ranks,
				  true /* destroy */);
out_ranks:
	d_rank_list_free(ranks);
out:
	return rc;
}

int
ds_pool_svc_destroy(const uuid_t pool_uuid)
{
	char		id[DAOS_UUID_STR_SIZE];
	d_iov_t	psid;
	crt_group_t    *group;
	int		rc;

	ds_rebuild_leader_stop(pool_uuid, -1);
	d_iov_set(&psid, (void *)pool_uuid, sizeof(uuid_t));
	rc = ds_rsvc_dist_stop(DS_RSVC_CLASS_POOL, &psid, NULL /* ranks */,
			       true /* destroy */);
	if (rc != 0) {
		D_ERROR(DF_UUID": failed to destroy pool service: %d\n",
			DP_UUID(pool_uuid), rc);
		return rc;
	}

	uuid_unparse_lower(pool_uuid, id);
	group = crt_group_lookup(id);
	if (group != NULL) {
		D_DEBUG(DB_MD, DF_UUID": destroying pool group\n",
			DP_UUID(pool_uuid));
		rc = dss_group_destroy(group);
		if (rc != 0) {
			D_ERROR(DF_UUID": failed to destroy pool group: %d\n",
				DP_UUID(pool_uuid), rc);
			return rc;
		}
	}

	return 0;
}

static int
pool_svc_create_group(struct pool_svc *svc, struct pool_map *map)
{
	char		id[DAOS_UUID_STR_SIZE];
	crt_group_t    *group;
	int		rc;

	/* Check if the pool group exists locally. */
	uuid_unparse_lower(svc->ps_uuid, id);
	group = crt_group_lookup(id);
	if (group != NULL)
		return 0;

	/* Attempt to create the pool group. */
	rc = ds_pool_group_create(svc->ps_uuid, map, &group);
	if (rc != 0) {
		D_ERROR(DF_UUID": failed to create pool group: %d\n",
			 DP_UUID(svc->ps_uuid), rc);
		return rc;
	}

	return 0;
}

static int
pool_svc_name_cb(d_iov_t *id, char **name)
{
	char *s;

	if (id->iov_len != sizeof(uuid_t))
		return -DER_INVAL;
	D_ALLOC(s, DAOS_UUID_STR_SIZE);
	if (s == NULL)
		return -DER_NOMEM;
	uuid_unparse_lower(id->iov_buf, s);
	s[8] = '\0'; /* strlen(DF_UUID) */
	*name = s;
	return 0;
}

static int
pool_svc_load_uuid_cb(d_iov_t *id, uuid_t db_uuid)
{
	char   *path;
	int	rc;

	if (id->iov_len != sizeof(uuid_t))
		return -DER_INVAL;
	path = pool_svc_rdb_uuid_path(id->iov_buf);
	if (path == NULL)
		return -DER_NOMEM;
	rc = uuid_load(path, db_uuid);
	D_FREE(path);
	return rc;
}

static int
pool_svc_store_uuid_cb(d_iov_t *id, uuid_t db_uuid)
{
	char   *path;
	int	rc;

	if (id->iov_len != sizeof(uuid_t))
		return -DER_INVAL;
	path = pool_svc_rdb_uuid_path(id->iov_buf);
	if (path == NULL)
		return -DER_NOMEM;
	rc = uuid_store(path, db_uuid);
	D_FREE(path);
	return rc;
}

static int
pool_svc_delete_uuid_cb(d_iov_t *id)
{
	char   *path;
	int	rc;

	if (id->iov_len != sizeof(uuid_t))
		return -DER_INVAL;
	path = pool_svc_rdb_uuid_path(id->iov_buf);
	if (path == NULL)
		return -DER_NOMEM;
	rc = remove(path);
	if (rc != 0) {
		D_ERROR(DF_UUID": failed to remove %s: %d\n",
			DP_UUID(id->iov_buf), path, errno);
		rc = daos_errno2der(errno);
	}
	D_FREE(path);
	return rc;
}

static int
pool_svc_locate_cb(d_iov_t *id, char **path)
{
	char *s;

	if (id->iov_len != sizeof(uuid_t))
		return -DER_INVAL;
	s = pool_svc_rdb_path(id->iov_buf);
	if (s == NULL)
		return -DER_NOMEM;
	*path = s;
	return 0;
}

static int
pool_svc_alloc_cb(d_iov_t *id, struct ds_rsvc **rsvc)
{
	struct pool_svc	       *svc;
	int			rc;

	if (id->iov_len != sizeof(uuid_t)) {
		rc = -DER_INVAL;
		goto err;
	}

	D_ALLOC_PTR(svc);
	if (svc == NULL) {
		rc = -DER_NOMEM;
		goto err;
	}

	d_iov_set(&svc->ps_rsvc.s_id, svc->ps_uuid, sizeof(uuid_t));

	uuid_copy(svc->ps_uuid, id->iov_buf);

	rc = ABT_rwlock_create(&svc->ps_lock);
	if (rc != ABT_SUCCESS) {
		D_ERROR("failed to create ps_lock: %d\n", rc);
		rc = dss_abterr2der(rc);
		goto err_svc;
	}

	rc = rdb_path_init(&svc->ps_root);
	if (rc != 0)
		goto err_lock;
	rc = rdb_path_push(&svc->ps_root, &rdb_path_root_key);
	if (rc != 0)
		goto err_root;

	rc = rdb_path_clone(&svc->ps_root, &svc->ps_handles);
	if (rc != 0)
		goto err_root;
	rc = rdb_path_push(&svc->ps_handles, &ds_pool_prop_handles);
	if (rc != 0)
		goto err_handles;

	rc = rdb_path_clone(&svc->ps_root, &svc->ps_user);
	if (rc != 0)
		goto err_handles;
	rc = rdb_path_push(&svc->ps_user, &ds_pool_attr_user);
	if (rc != 0)
		goto err_user;

	rc = ds_cont_svc_init(&svc->ps_cont_svc, svc->ps_uuid, 0 /* id */,
			      &svc->ps_rsvc);
	if (rc != 0)
		goto err_user;

	*rsvc = &svc->ps_rsvc;
	return 0;

err_user:
	rdb_path_fini(&svc->ps_user);
err_handles:
	rdb_path_fini(&svc->ps_handles);
err_root:
	rdb_path_fini(&svc->ps_root);
err_lock:
	ABT_rwlock_free(&svc->ps_lock);
err_svc:
	D_FREE(svc);
err:
	return rc;
}

static void
pool_svc_free_cb(struct ds_rsvc *rsvc)
{
	struct pool_svc *svc = pool_svc_obj(rsvc);

	ds_cont_svc_fini(&svc->ps_cont_svc);
	rdb_path_fini(&svc->ps_user);
	rdb_path_fini(&svc->ps_handles);
	rdb_path_fini(&svc->ps_root);
	ABT_rwlock_free(&svc->ps_lock);
	D_FREE(svc);
}

static int
pool_svc_step_up_cb(struct ds_rsvc *rsvc)
{
	struct pool_svc		       *svc = pool_svc_obj(rsvc);
	struct rdb_tx			tx;
	struct ds_pool		       *pool;
	d_rank_list_t		       *replicas = NULL;
	struct pool_map		       *map = NULL;
	uint32_t			map_version;
	struct ds_pool_create_arg	arg;
	d_rank_t			rank;
	int				rc;

	/* Read the pool map into map and map_version. */
	rc = rdb_tx_begin(rsvc->s_db, rsvc->s_term, &tx);
	if (rc != 0)
		goto out;
	ABT_rwlock_rdlock(svc->ps_lock);
	rc = read_map(&tx, &svc->ps_root, &map);
	if (rc == 0)
		rc = rdb_get_ranks(rsvc->s_db, &replicas);
	ABT_rwlock_unlock(svc->ps_lock);
	rdb_tx_end(&tx);
	if (rc != 0) {
		if (rc == -DER_NONEXIST) {
			D_DEBUG(DF_DSMS, DF_UUID": new db\n",
				DP_UUID(svc->ps_uuid));
			rc = DER_UNINIT;
		} else {
			D_ERROR(DF_UUID": failed to get %s: %d\n",
				DP_UUID(svc->ps_uuid),
				map == NULL ? "pool map" : "replica ranks", rc);
		}
		goto out;
	}
	map_version = pool_map_get_version(map);

	/* Create the pool group. */
	rc = pool_svc_create_group(svc, map);
	if (rc != 0)
		goto out;

	/* Create or revalidate svc->ps_pool with map and map_version. */
	D_ASSERT(svc->ps_pool == NULL);
	arg.pca_map_version = map_version;
	arg.pca_need_group = 1;
	rc = ds_pool_lookup_create(svc->ps_uuid, &arg, &svc->ps_pool);
	if (rc != 0) {
		D_ERROR(DF_UUID": failed to get ds_pool: %d\n",
			DP_UUID(svc->ps_uuid), rc);
		goto out;
	}
	pool = svc->ps_pool;
	ABT_rwlock_wrlock(pool->sp_lock);
	if (pool->sp_map != map) {
		/* An existing ds_pool; map not used yet. */
		D_ASSERTF(pool->sp_map_version <= map_version, "%u <= %u\n",
			  pool->sp_map_version, map_version);
		D_ASSERTF(pool->sp_map == NULL ||
			  pool_map_get_version(pool->sp_map) <= map_version,
			  "%u <= %u\n", pool_map_get_version(pool->sp_map),
			  map_version);
		if (pool->sp_map == NULL ||
		    pool_map_get_version(pool->sp_map) < map_version) {
			struct pool_map *tmp;

			rc = pl_map_update(pool->sp_uuid, map,
					   pool->sp_map != NULL ? false : true);
			if (rc != 0) {
				svc->ps_pool = NULL;
				ABT_rwlock_unlock(pool->sp_lock);
				ds_pool_put(pool);
				goto out;
			}

			/* Need to update pool->sp_map. Swap with map. */
			tmp = pool->sp_map;
			pool->sp_map = map;
			map = tmp;
			pool->sp_map_version = map_version;
		}
	} else {
		map = NULL; /* taken over by pool */
	}
	ABT_rwlock_unlock(pool->sp_lock);

	ds_cont_svc_step_up(svc->ps_cont_svc);

	rc = ds_rebuild_regenerate_task(pool, replicas);
	if (rc != 0) {
		ds_cont_svc_step_down(svc->ps_cont_svc);
		ds_pool_put(svc->ps_pool);
		svc->ps_pool = NULL;
		goto out;
	}

	rc = crt_group_rank(NULL, &rank);
	D_ASSERTF(rc == 0, "%d\n", rc);
	D_PRINT(DF_UUID": rank %u became pool service leader "DF_U64"\n",
		DP_UUID(svc->ps_uuid), rank, svc->ps_rsvc.s_term);
out:
	if (map != NULL)
		pool_map_decref(map);
	if (replicas)
		d_rank_list_free(replicas);
	return rc;
}

static void
pool_svc_step_down_cb(struct ds_rsvc *rsvc)
{
	struct pool_svc	       *svc = pool_svc_obj(rsvc);
	d_rank_t		rank;
	int			rc;

	ds_cont_svc_step_down(svc->ps_cont_svc);
	D_ASSERT(svc->ps_pool != NULL);
	ds_pool_put(svc->ps_pool);
	svc->ps_pool = NULL;

	rc = crt_group_rank(NULL, &rank);
	D_ASSERTF(rc == 0, "%d\n", rc);
	D_PRINT(DF_UUID": rank %u no longer pool service leader "DF_U64"\n",
		DP_UUID(svc->ps_uuid), rank, svc->ps_rsvc.s_term);
}

static void
pool_svc_drain_cb(struct ds_rsvc *rsvc)
{
	struct pool_svc *svc = pool_svc_obj(rsvc);

	ds_rebuild_leader_stop(svc->ps_uuid, -1);
}

static struct ds_rsvc_class pool_svc_rsvc_class = {
	.sc_name	= pool_svc_name_cb,
	.sc_load_uuid	= pool_svc_load_uuid_cb,
	.sc_store_uuid	= pool_svc_store_uuid_cb,
	.sc_delete_uuid	= pool_svc_delete_uuid_cb,
	.sc_locate	= pool_svc_locate_cb,
	.sc_alloc	= pool_svc_alloc_cb,
	.sc_free	= pool_svc_free_cb,
	.sc_step_up	= pool_svc_step_up_cb,
	.sc_step_down	= pool_svc_step_down_cb,
	.sc_drain	= pool_svc_drain_cb
};

void
ds_pool_rsvc_class_register(void)
{
	ds_rsvc_class_register(DS_RSVC_CLASS_POOL, &pool_svc_rsvc_class);
}

void
ds_pool_rsvc_class_unregister(void)
{
	ds_rsvc_class_unregister(DS_RSVC_CLASS_POOL);
}

static int
pool_svc_lookup(uuid_t uuid, struct pool_svc **svcp)
{
	struct ds_rsvc *rsvc;
	d_iov_t	id;
	int		rc;

	d_iov_set(&id, uuid, sizeof(uuid_t));
	rc = ds_rsvc_lookup(DS_RSVC_CLASS_POOL, &id, &rsvc);
	if (rc != 0)
		return rc;
	*svcp = pool_svc_obj(rsvc);
	return 0;
}

static void
pool_svc_put(struct pool_svc *svc)
{
	ds_rsvc_put(&svc->ps_rsvc);
}

static int
pool_svc_lookup_leader(uuid_t uuid, struct pool_svc **svcp,
		       struct rsvc_hint *hint)
{
	struct ds_rsvc *rsvc;
	d_iov_t	id;
	int		rc;

	d_iov_set(&id, uuid, sizeof(uuid_t));
	rc = ds_rsvc_lookup_leader(DS_RSVC_CLASS_POOL, &id, &rsvc, hint);
	if (rc != 0)
		return rc;
	*svcp = pool_svc_obj(rsvc);
	return 0;
}

static void
pool_svc_put_leader(struct pool_svc *svc)
{
	ds_rsvc_put_leader(&svc->ps_rsvc);
}

/** Look up container service \a pool_uuid. */
int
ds_pool_cont_svc_lookup_leader(uuid_t pool_uuid, struct cont_svc **svcp,
			       struct rsvc_hint *hint)
{
	struct pool_svc	       *pool_svc;
	int			rc;

	rc = pool_svc_lookup_leader(pool_uuid, &pool_svc, hint);
	if (rc != 0)
		return rc;
	*svcp = pool_svc->ps_cont_svc;
	return 0;
}

/*
 * Try to start a pool's pool service if its RDB exists. Continue the iteration
 * upon errors as other pools may still be able to work.
 */
static int
start_one(uuid_t uuid, void *arg)
{
	char	       *path;
	d_iov_t	id;
	struct stat	st;
	int		rc;

	/*
	 * Check if an RDB file exists, to avoid unnecessary error messages
	 * from the ds_rsvc_start() call.
	 */
	path = pool_svc_rdb_path(uuid);
	if (path == NULL) {
		D_ERROR(DF_UUID": failed allocate rdb path\n", DP_UUID(uuid));
		return 0;
	}
	rc = stat(path, &st);
	D_FREE(path);
	if (rc != 0) {
		if (errno != ENOENT)
			D_ERROR(DF_UUID": failed to check rdb existence: %d\n",
				DP_UUID(uuid), errno);
		return 0;
	}

	d_iov_set(&id, uuid, sizeof(uuid_t));
	ds_rsvc_start(DS_RSVC_CLASS_POOL, &id, NULL /* db_uuid */,
		      false /* create */, 0 /* size */, NULL /* replicas */,
		      NULL /* arg */);
	return 0;
}

static void
pool_svc_start_all(void *arg)
{
	int rc;

	/* Scan the storage and start all pool services. */
	rc = ds_mgmt_tgt_pool_iterate(start_one, NULL /* arg */);
	if (rc != 0)
		D_ERROR("failed to scan all pool services: %d\n", rc);
}

/* Note that this function is currently called from the main xstream. */
int
ds_pool_svc_start_all(void)
{
	ABT_thread	thread;
	int		rc;

	/* Create a ULT to call ds_pool_svc_start() in xstream 0. */
	rc = dss_ult_create(pool_svc_start_all, NULL,
			    DSS_ULT_POOL_SRV, 0, 0, &thread);
	if (rc != 0) {
		D_ERROR("failed to create pool service start ULT: %d\n", rc);
		return rc;
	}
	ABT_thread_join(thread);
	ABT_thread_free(&thread);
	return 0;
}

/*
 * Note that this function is currently called from the main xstream to save
 * one ULT creation.
 */
int
ds_pool_svc_stop_all(void)
{
	return ds_rsvc_stop_all(DS_RSVC_CLASS_POOL);
}

static int
bcast_create(crt_context_t ctx, struct pool_svc *svc, crt_opcode_t opcode,
	     crt_bulk_t bulk_hdl, crt_rpc_t **rpc)
{
	return ds_pool_bcast_create(ctx, svc->ps_pool, DAOS_POOL_MODULE, opcode,
				    rpc, bulk_hdl, NULL);
}

/**
 * Retrieve the latest leader hint from \a db and fill it into \a hint.
 *
 * \param[in]	db	database
 * \param[out]	hint	rsvc hint
 */
void
ds_pool_set_hint(struct rdb *db, struct rsvc_hint *hint)
{
	int rc;

	rc = rdb_get_leader(db, &hint->sh_term, &hint->sh_rank);
	if (rc != 0)
		return;
	hint->sh_flags |= RSVC_HINT_VALID;
}

static int
pool_prop_read(struct rdb_tx *tx, const struct pool_svc *svc, uint64_t bits,
	       daos_prop_t **prop_out)
{
	daos_prop_t	*prop;
	d_iov_t	 value;
	uint64_t	 val;
	uint32_t	 idx = 0, nr = 0;
	int		 rc;

	if (bits & DAOS_PO_QUERY_PROP_LABEL)
		nr++;
	if (bits & DAOS_PO_QUERY_PROP_SPACE_RB)
		nr++;
	if (bits & DAOS_PO_QUERY_PROP_SELF_HEAL)
		nr++;
	if (bits & DAOS_PO_QUERY_PROP_RECLAIM)
		nr++;
	if (bits & DAOS_PO_QUERY_PROP_ACL)
		nr++;
	if (bits & DAOS_PO_QUERY_PROP_OWNER)
		nr++;
	if (bits & DAOS_PO_QUERY_PROP_OWNER_GROUP)
		nr++;
	if (nr == 0)
		return 0;

	prop = daos_prop_alloc(nr);
	if (prop == NULL)
		return -DER_NOMEM;
	*prop_out = prop;
	if (bits & DAOS_PO_QUERY_PROP_LABEL) {
		d_iov_set(&value, NULL, 0);
		rc = rdb_tx_lookup(tx, &svc->ps_root, &ds_pool_prop_label,
				   &value);
		if (rc != 0)
			return rc;
		if (value.iov_len > DAOS_PROP_LABEL_MAX_LEN) {
			D_ERROR("bad label length %zu (> %d).\n", value.iov_len,
				DAOS_PROP_LABEL_MAX_LEN);
			return -DER_IO;
		}
		D_ASSERT(idx < nr);
		prop->dpp_entries[idx].dpe_type = DAOS_PROP_PO_LABEL;
		prop->dpp_entries[idx].dpe_str =
			strndup(value.iov_buf, value.iov_len);
		if (prop->dpp_entries[idx].dpe_str == NULL)
			return -DER_NOMEM;
		idx++;
	}
	if (bits & DAOS_PO_QUERY_PROP_SPACE_RB) {
		d_iov_set(&value, &val, sizeof(val));
		rc = rdb_tx_lookup(tx, &svc->ps_root, &ds_pool_prop_space_rb,
				   &value);
		if (rc != 0)
			return rc;
		D_ASSERT(idx < nr);
		prop->dpp_entries[idx].dpe_type = DAOS_PROP_PO_SPACE_RB;
		prop->dpp_entries[idx].dpe_val = val;
		idx++;
	}
	if (bits & DAOS_PO_QUERY_PROP_SELF_HEAL) {
		d_iov_set(&value, &val, sizeof(val));
		rc = rdb_tx_lookup(tx, &svc->ps_root, &ds_pool_prop_self_heal,
				   &value);
		if (rc != 0)
			return rc;
		D_ASSERT(idx < nr);
		prop->dpp_entries[idx].dpe_type = DAOS_PROP_PO_SELF_HEAL;
		prop->dpp_entries[idx].dpe_val = val;
		idx++;
	}
	if (bits & DAOS_PO_QUERY_PROP_RECLAIM) {
		d_iov_set(&value, &val, sizeof(val));
		rc = rdb_tx_lookup(tx, &svc->ps_root, &ds_pool_prop_reclaim,
				   &value);
		if (rc != 0)
			return rc;
		D_ASSERT(idx < nr);
		prop->dpp_entries[idx].dpe_type = DAOS_PROP_PO_RECLAIM;
		prop->dpp_entries[idx].dpe_val = val;
		idx++;
	}
	if (bits & DAOS_PO_QUERY_PROP_ACL) {
		d_iov_set(&value, NULL, 0);
		rc = rdb_tx_lookup(tx, &svc->ps_root, &ds_pool_prop_acl,
				   &value);
		if (rc != 0)
			return rc;
		D_ASSERT(idx < nr);
		prop->dpp_entries[idx].dpe_type = DAOS_PROP_PO_ACL;
		D_ALLOC(prop->dpp_entries[idx].dpe_val_ptr, value.iov_buf_len);
		if (prop->dpp_entries[idx].dpe_val_ptr == NULL)
			return -DER_NOMEM;
		memcpy(prop->dpp_entries[idx].dpe_val_ptr, value.iov_buf,
		       value.iov_buf_len);
		idx++;
	}
	if (bits & DAOS_PO_QUERY_PROP_OWNER) {
		d_iov_set(&value, NULL, 0);
		rc = rdb_tx_lookup(tx, &svc->ps_root, &ds_pool_prop_owner,
				   &value);
		if (rc != 0)
			return rc;
		if (value.iov_len > DAOS_ACL_MAX_PRINCIPAL_LEN) {
			D_ERROR("bad owner length %zu (> %d).\n", value.iov_len,
				DAOS_ACL_MAX_PRINCIPAL_LEN);
			return -DER_IO;
		}
		D_ASSERT(idx < nr);
		prop->dpp_entries[idx].dpe_type = DAOS_PROP_PO_OWNER;
		prop->dpp_entries[idx].dpe_str =
			strndup(value.iov_buf, value.iov_len);
		if (prop->dpp_entries[idx].dpe_str == NULL)
			return -DER_NOMEM;
		idx++;
	}
	if (bits & DAOS_PO_QUERY_PROP_OWNER_GROUP) {
		d_iov_set(&value, NULL, 0);
		rc = rdb_tx_lookup(tx, &svc->ps_root, &ds_pool_prop_owner_group,
				   &value);
		if (rc != 0)
			return rc;
		if (value.iov_len > DAOS_ACL_MAX_PRINCIPAL_LEN) {
			D_ERROR("bad owner group length %zu (> %d).\n",
				value.iov_len,
				DAOS_ACL_MAX_PRINCIPAL_LEN);
			return -DER_IO;
		}
		D_ASSERT(idx < nr);
		prop->dpp_entries[idx].dpe_type = DAOS_PROP_PO_OWNER_GROUP;
		prop->dpp_entries[idx].dpe_str =
			strndup(value.iov_buf, value.iov_len);
		if (prop->dpp_entries[idx].dpe_str == NULL)
			return -DER_NOMEM;
		idx++;
	}
	return 0;
}

static int
pool_map_update(crt_context_t ctx, struct pool_svc *svc,
		uint32_t map_version, struct pool_buf *buf)
{
	int			rc;

	/* If iv_ns is NULL, it means the pool is not connected,
	 * then it only update its own(leader's) pool map, instead
	 * of distributing pool map to all other servers. offline
	 * rebuild will redistribute the pool map by itself anyway.
	 */
	if (svc->ps_pool->sp_iv_ns == NULL) {
		rc = ds_pool_tgt_map_update(svc->ps_pool, buf, map_version);
		return rc;
	}

	D_DEBUG(DF_DSMS, DF_UUID": update ver %d pb_nr %d\n",
		 DP_UUID(svc->ps_uuid), map_version, buf->pb_nr);

	rc = pool_iv_map_update(svc->ps_pool, buf, map_version);

	return rc;
}

/*
 * We use this RPC to not only create the pool metadata but also initialize the
 * pool/container service DB.
 */
void
ds_pool_create_handler(crt_rpc_t *rpc)
{
	struct pool_create_in  *in = crt_req_get(rpc);
	struct pool_create_out *out = crt_reply_get(rpc);
	struct pool_svc	       *svc;
	struct rdb_tx		tx;
	d_iov_t		value;
	struct rdb_kvs_attr	attr;
	daos_prop_t	       *prop_dup = NULL;
	int			rc;

	D_DEBUG(DF_DSMS, DF_UUID": processing rpc %p\n",
		DP_UUID(in->pri_op.pi_uuid), rpc);

	if (in->pri_ntgts != in->pri_tgt_uuids.ca_count ||
	    in->pri_ntgts != in->pri_tgt_ranks->rl_nr)
		D_GOTO(out, rc = -DER_PROTO);
	if (in->pri_ndomains != in->pri_domains.ca_count)
		D_GOTO(out, rc = -DER_PROTO);

	/* This RPC doesn't care about whether the service is up. */
	rc = pool_svc_lookup(in->pri_op.pi_uuid, &svc);
	if (rc != 0)
		D_GOTO(out, rc);

	/*
	 * Simply serialize this whole RPC with rsvc_step_{up,down}_cb() and
	 * ds_rsvc_stop().
	 */
	ABT_mutex_lock(svc->ps_rsvc.s_mutex);

	if (svc->ps_rsvc.s_stop) {
		D_DEBUG(DB_MD, DF_UUID": pool service already stopping\n",
			DP_UUID(svc->ps_uuid));
		D_GOTO(out_mutex, rc = -DER_CANCELED);
	}

	rc = rdb_tx_begin(svc->ps_rsvc.s_db, RDB_NIL_TERM, &tx);
	if (rc != 0)
		D_GOTO(out_mutex, rc);
	ABT_rwlock_wrlock(svc->ps_lock);
	ds_cont_wrlock_metadata(svc->ps_cont_svc);

	/* See if the DB has already been initialized. */
	d_iov_set(&value, NULL /* buf */, 0 /* size */);
	rc = rdb_tx_lookup(&tx, &svc->ps_root, &ds_pool_prop_map_buffer,
			   &value);
	if (rc != -DER_NONEXIST) {
		if (rc == 0)
			D_DEBUG(DF_DSMS, DF_UUID": db already initialized\n",
				DP_UUID(svc->ps_uuid));
		else
			D_ERROR(DF_UUID": failed to look up pool map: %d\n",
				DP_UUID(svc->ps_uuid), rc);
		D_GOTO(out_tx, rc);
	}

	/* duplicate the default properties, overwrite it with pool create
	 * parameter and then write to pool meta data.
	 */
	prop_dup = daos_prop_dup(&pool_prop_default, true);
	if (prop_dup == NULL) {
		D_ERROR("daos_prop_dup failed.\n");
		D_GOTO(out_tx, rc = -DER_NOMEM);
	}
	rc = pool_prop_default_copy(prop_dup, in->pri_prop);
	if (rc) {
		D_ERROR("daos_prop_default_copy failed.\n");
		D_GOTO(out_tx, rc);
	}

	/* Initialize the DB and the metadata for this pool. */
	attr.dsa_class = RDB_KVS_GENERIC;
	attr.dsa_order = 8;
	rc = rdb_tx_create_root(&tx, &attr);
	if (rc != 0)
		D_GOTO(out_tx, rc);
	rc = init_pool_metadata(&tx, &svc->ps_root, in->pri_tgt_uuids.ca_count,
				in->pri_tgt_uuids.ca_arrays, NULL /* group */,
				in->pri_tgt_ranks, prop_dup,
				in->pri_ndomains, in->pri_domains.ca_arrays);
	if (rc != 0)
		D_GOTO(out_tx, rc);
	rc = ds_cont_init_metadata(&tx, &svc->ps_root, in->pri_op.pi_uuid);
	if (rc != 0)
		D_GOTO(out_tx, rc);

	rc = rdb_tx_commit(&tx);
	if (rc != 0)
		D_GOTO(out_tx, rc);

out_tx:
	daos_prop_free(prop_dup);
	ds_cont_unlock_metadata(svc->ps_cont_svc);
	ABT_rwlock_unlock(svc->ps_lock);
	rdb_tx_end(&tx);
	if (rc != 0)
		D_GOTO(out_svc, rc);

	if (svc->ps_rsvc.s_state == DS_RSVC_UP_EMPTY) {
		/*
		 * The DB is no longer empty. Since the previous
		 * pool_svc_step_up_cb() call didn't finish stepping up due to
		 * an empty DB, and there hasn't been a pool_svc_step_down_cb()
		 * call yet, we should call pool_svc_step_up() to finish
		 * stepping up.
		 */
		D_DEBUG(DF_DSMS, DF_UUID": trying to finish stepping up\n",
			DP_UUID(in->pri_op.pi_uuid));
		rc = pool_svc_step_up_cb(&svc->ps_rsvc);
		if (rc != 0) {
			D_ASSERT(rc != DER_UNINIT);
			/* TODO: Ask rdb to step down. */
			D_GOTO(out_svc, rc);
		}
		svc->ps_rsvc.s_state = DS_RSVC_UP;
		ABT_cond_broadcast(svc->ps_rsvc.s_state_cv);
	}

out_mutex:
	ABT_mutex_unlock(svc->ps_rsvc.s_mutex);
out_svc:
	ds_rsvc_set_hint(&svc->ps_rsvc, &out->pro_op.po_hint);
	pool_svc_put(svc);
out:
	out->pro_op.po_rc = rc;
	D_DEBUG(DF_DSMS, DF_UUID": replying rpc %p: %d\n",
		DP_UUID(in->pri_op.pi_uuid), rpc, rc);
	crt_reply_send(rpc);
}

static int
pool_connect_bcast(crt_context_t ctx, struct pool_svc *svc,
		   const uuid_t pool_hdl, uint64_t capas,
		   d_iov_t *global_ns, struct daos_pool_space *ps,
		   crt_bulk_t map_buf_bulk)
{
	struct pool_tgt_connect_in     *in;
	struct pool_tgt_connect_out    *out;
	d_rank_t		       rank;
	crt_rpc_t		       *rpc;
	int				rc;

	D_DEBUG(DF_DSMS, DF_UUID": bcasting\n", DP_UUID(svc->ps_uuid));

	rc = crt_group_rank(svc->ps_pool->sp_group, &rank);
	if (rc != 0)
		D_GOTO(out, rc);

	rc = bcast_create(ctx, svc, POOL_TGT_CONNECT, map_buf_bulk, &rpc);
	if (rc != 0)
		D_GOTO(out, rc);

	in = crt_req_get(rpc);
	uuid_copy(in->tci_uuid, svc->ps_uuid);
	uuid_copy(in->tci_hdl, pool_hdl);
	in->tci_capas = capas;
	in->tci_map_version = pool_map_get_version(svc->ps_pool->sp_map);
	in->tci_iv_ns_id = ds_iv_ns_id_get(svc->ps_pool->sp_iv_ns);
	in->tci_iv_ctxt.iov_buf = global_ns->iov_buf;
	in->tci_iv_ctxt.iov_buf_len = global_ns->iov_buf_len;
	in->tci_iv_ctxt.iov_len = global_ns->iov_len;
	in->tci_master_rank = rank;
	if (ps != NULL)
		in->tci_query_bits = DAOS_PO_QUERY_SPACE;

	rc = dss_rpc_send(rpc);
	if (rc != 0)
		D_GOTO(out_rpc, rc);

	out = crt_reply_get(rpc);
	rc = out->tco_rc;
	if (rc != 0) {
		D_ERROR(DF_UUID": failed to connect to %d targets\n",
			DP_UUID(svc->ps_uuid), rc);
		rc = -DER_IO;
	} else {
		if (ps != NULL)
			*ps = out->tco_space;
	}

out_rpc:
	crt_req_decref(rpc);
out:
	D_DEBUG(DF_DSMS, DF_UUID": bcasted: %d\n", DP_UUID(svc->ps_uuid), rc);
	return rc;
}

static int
bulk_cb(const struct crt_bulk_cb_info *cb_info)
{
	ABT_eventual *eventual = cb_info->bci_arg;

	ABT_eventual_set(*eventual, (void *)&cb_info->bci_rc,
			 sizeof(cb_info->bci_rc));
	return 0;
}

/*
 * Transfer the pool map to "remote_bulk". If the remote bulk buffer is too
 * small, then return -DER_TRUNC and set "required_buf_size" to the local pool
 * map buffer size.
 * If the map_buf_bulk is non-NULL, then the created local bulk handle for
 * pool_buf will be returned and caller needs to do crt_bulk_free later.
 * If the map_buf_bulk is NULL then the internally created local bulk handle
 * will be freed within this function.
 */
static int
transfer_map_buf(struct pool_buf *map_buf, uint32_t map_version,
		 struct pool_svc *svc, crt_rpc_t *rpc,
		 crt_bulk_t remote_bulk, uint32_t *required_buf_size)
{
	size_t			map_buf_size;
	daos_size_t		remote_bulk_size;
	d_iov_t		map_iov;
	d_sg_list_t		map_sgl;
	crt_bulk_t		bulk = CRT_BULK_NULL;
	struct crt_bulk_desc	map_desc;
	crt_bulk_opid_t		map_opid;
	ABT_eventual		eventual;
	int		       *status;
	int			rc;

	if (map_version != pool_map_get_version(svc->ps_pool->sp_map)) {
		D_ERROR(DF_UUID": found different cached and persistent pool "
			"map versions: cached=%u persistent=%u\n",
			DP_UUID(svc->ps_uuid),
			pool_map_get_version(svc->ps_pool->sp_map),
			map_version);
		D_GOTO(out, rc = -DER_IO);
	}

	map_buf_size = pool_buf_size(map_buf->pb_nr);

	/* Check if the client bulk buffer is large enough. */
	rc = crt_bulk_get_len(remote_bulk, &remote_bulk_size);
	if (rc != 0)
		D_GOTO(out, rc);
	if (remote_bulk_size < map_buf_size) {
		D_ERROR(DF_UUID": remote pool map buffer ("DF_U64") < required "
			"(%lu)\n", DP_UUID(svc->ps_uuid), remote_bulk_size,
			map_buf_size);
		*required_buf_size = map_buf_size;
		D_GOTO(out, rc = -DER_TRUNC);
	}

	d_iov_set(&map_iov, map_buf, map_buf_size);
	map_sgl.sg_nr = 1;
	map_sgl.sg_nr_out = 0;
	map_sgl.sg_iovs = &map_iov;

	rc = crt_bulk_create(rpc->cr_ctx, &map_sgl, CRT_BULK_RO, &bulk);
	if (rc != 0)
		D_GOTO(out, rc);

	/* Prepare "map_desc" for crt_bulk_transfer(). */
	map_desc.bd_rpc = rpc;
	map_desc.bd_bulk_op = CRT_BULK_PUT;
	map_desc.bd_remote_hdl = remote_bulk;
	map_desc.bd_remote_off = 0;
	map_desc.bd_local_hdl = bulk;
	map_desc.bd_local_off = 0;
	map_desc.bd_len = map_iov.iov_len;

	rc = ABT_eventual_create(sizeof(*status), &eventual);
	if (rc != ABT_SUCCESS)
		D_GOTO(out_bulk, rc = dss_abterr2der(rc));

	rc = crt_bulk_transfer(&map_desc, bulk_cb, &eventual, &map_opid);
	if (rc != 0)
		D_GOTO(out_eventual, rc);

	rc = ABT_eventual_wait(eventual, (void **)&status);
	if (rc != ABT_SUCCESS)
		D_GOTO(out_eventual, rc = dss_abterr2der(rc));

	if (*status != 0)
		D_GOTO(out_eventual, rc = *status);

out_eventual:
	ABT_eventual_free(&eventual);
out_bulk:
	if (bulk != CRT_BULK_NULL)
		crt_bulk_free(bulk);
out:
	return rc;
}

void
ds_pool_connect_handler(crt_rpc_t *rpc)
{
	struct pool_connect_in	       *in = crt_req_get(rpc);
	struct pool_connect_out	       *out = crt_reply_get(rpc);
	struct pool_svc		       *svc;
	struct pool_buf			*map_buf;
	uint32_t			map_version;
	struct rdb_tx			tx;
	d_iov_t				key;
	d_iov_t				value;
	struct pool_hdl			hdl;
	d_iov_t				iv_iov;
	unsigned int			iv_ns_id;
	uint32_t			nhandles;
	int				skip_update = 0;
	int				rc;
	daos_prop_t		       *prop;
	uint64_t			prop_bits;
	struct daos_prop_entry	       *acl_entry;
	struct pool_owner		owner;
	struct daos_prop_entry	       *owner_entry;
	struct daos_prop_entry	       *owner_grp_entry;

	D_DEBUG(DF_DSMS, DF_UUID": processing rpc %p: hdl="DF_UUID"\n",
		DP_UUID(in->pci_op.pi_uuid), rpc, DP_UUID(in->pci_op.pi_hdl));

	rc = pool_svc_lookup_leader(in->pci_op.pi_uuid, &svc,
				    &out->pco_op.po_hint);
	if (rc != 0)
		D_GOTO(out, rc);

	/* sp_iv_ns will be destroyed when pool is destroyed,
	 * see pool_free_ref()
	 */
	D_ASSERT(svc->ps_pool != NULL);
	if (svc->ps_pool->sp_iv_ns == NULL) {
		rc = ds_iv_ns_create(rpc->cr_ctx, svc->ps_pool->sp_uuid,
				     NULL, &iv_ns_id, &iv_iov,
				     &svc->ps_pool->sp_iv_ns);
		if (rc)
			D_GOTO(out_svc, rc);
	} else {
		rc = ds_iv_global_ns_get(svc->ps_pool->sp_iv_ns, &iv_iov);
		if (rc)
			D_GOTO(out_svc, rc);
	}

	if (in->pci_query_bits & DAOS_PO_QUERY_REBUILD_STATUS) {
		rc = ds_rebuild_query(in->pci_op.pi_uuid, &out->pco_rebuild_st);
		if (rc != 0)
			D_GOTO(out_svc, rc);
	}

	rc = rdb_tx_begin(svc->ps_rsvc.s_db, svc->ps_rsvc.s_term, &tx);
	if (rc != 0)
		D_GOTO(out_svc, rc);

	ABT_rwlock_wrlock(svc->ps_lock);

	/* Check existing pool handles. */
	d_iov_set(&key, in->pci_op.pi_hdl, sizeof(uuid_t));
	d_iov_set(&value, &hdl, sizeof(hdl));
	rc = rdb_tx_lookup(&tx, &svc->ps_handles, &key, &value);
	if (rc == 0) {
		if (hdl.ph_capas == in->pci_capas) {
			/*
			 * The handle already exists; only do the pool map
			 * transfer.
			 */
			skip_update = 1;
		} else {
			/* The existing one does not match the new one. */
			D_ERROR(DF_UUID": found conflicting pool handle\n",
				DP_UUID(in->pci_op.pi_uuid));
			D_GOTO(out_lock, rc = -DER_EXIST);
		}
	} else if (rc != -DER_NONEXIST) {
		D_GOTO(out_lock, rc);
	}

	/* Fetch properties, the  ACL and ownership info for access check,
	 * all properties will update to IV.
	 */
	prop_bits = DAOS_PO_QUERY_PROP_ALL;
	rc = pool_prop_read(&tx, svc, prop_bits, &prop);
	if (rc != 0) {
		D_ERROR(DF_UUID": cannot get access data for pool, rc=%d\n",
			DP_UUID(in->pci_op.pi_uuid), rc);
		D_GOTO(out_map_version, rc);
	}
	D_ASSERT(prop != NULL);

	acl_entry = daos_prop_entry_get(prop, DAOS_PROP_PO_ACL);
	D_ASSERT(acl_entry != NULL);
	D_ASSERT(acl_entry->dpe_val_ptr != NULL);

	owner_entry = daos_prop_entry_get(prop, DAOS_PROP_PO_OWNER);
	D_ASSERT(owner_entry != NULL);
	D_ASSERT(owner_entry->dpe_str != NULL);

	owner_grp_entry = daos_prop_entry_get(prop, DAOS_PROP_PO_OWNER_GROUP);
	D_ASSERT(owner_grp_entry != NULL);
	D_ASSERT(owner_grp_entry->dpe_str != NULL);

	owner.user = owner_entry->dpe_str;
	owner.group = owner_grp_entry->dpe_str;

	rc = ds_sec_check_pool_access(acl_entry->dpe_val_ptr, &owner,
			&in->pci_cred, in->pci_capas);
	if (rc != 0) {
		D_ERROR(DF_UUID": refusing connect attempt for "
			DF_X64" error: %d\n", DP_UUID(in->pci_op.pi_uuid),
			in->pci_capas, rc);
		D_GOTO(out_pool_prop, rc = -DER_NO_PERM);
	}

	rc = read_map_buf(&tx, &svc->ps_root, &map_buf, &map_version);
	if (rc != 0) {
		D_ERROR(DF_UUID": failed to read pool map: %d\n",
			DP_UUID(svc->ps_uuid), rc);
		D_GOTO(out, rc);
	}

	rc = transfer_map_buf(map_buf, map_version, svc, rpc, in->pci_map_bulk,
			      &out->pco_map_buf_size);
	if (rc != 0)
		D_GOTO(out_map_version, rc);

	/*
	 * Transfer the pool map to the client before adding the pool handle,
	 * so that we don't need to worry about rolling back the transaction
	 * when the tranfer fails. The client has already been authenticated
	 * and authorized at this point. If an error occurs after the transfer
	 * completes, then we simply return the error and the client will throw
	 * its pool_buf away.
	 */
	if (skip_update)
		D_GOTO(out_pool_prop, rc = 0);

	d_iov_set(&value, &nhandles, sizeof(nhandles));
	rc = rdb_tx_lookup(&tx, &svc->ps_root, &ds_pool_prop_nhandles, &value);
	if (rc != 0)
		D_GOTO(out_pool_prop, rc);

	/* Take care of exclusive handles. */
	if (nhandles != 0) {
		if (in->pci_capas & DAOS_PC_EX) {
			D_DEBUG(DF_DSMS, DF_UUID": others already connected\n",
				DP_UUID(in->pci_op.pi_uuid));
			D_GOTO(out_pool_prop, rc = -DER_BUSY);
		} else {
			/*
			 * If there is a non-exclusive handle, then all handles
			 * are non-exclusive.
			 */
			d_iov_set(&value, &hdl, sizeof(hdl));
			rc = rdb_tx_fetch(&tx, &svc->ps_handles,
					  RDB_PROBE_FIRST, NULL /* key_in */,
					  NULL /* key_out */, &value);
			if (rc != 0)
				D_GOTO(out_pool_prop, rc);
			if (hdl.ph_capas & DAOS_PC_EX)
				D_GOTO(out_pool_prop, rc = -DER_BUSY);
		}
	}

	rc = pool_connect_bcast(rpc->cr_ctx, svc, in->pci_op.pi_hdl,
		in->pci_capas, &iv_iov,
		(in->pci_query_bits & DAOS_PO_QUERY_SPACE) ?
		&out->pco_space : NULL,
		CRT_BULK_NULL);
	if (rc != 0) {
		D_ERROR(DF_UUID": failed to connect to targets: %d\n",
			DP_UUID(in->pci_op.pi_uuid), rc);
		D_GOTO(out_pool_prop, rc);
	}

	hdl.ph_capas = in->pci_capas;
	nhandles++;

	d_iov_set(&value, &nhandles, sizeof(nhandles));
	rc = rdb_tx_update(&tx, &svc->ps_root, &ds_pool_prop_nhandles, &value);
	if (rc != 0)
		D_GOTO(out_pool_prop, rc);

	d_iov_set(&key, in->pci_op.pi_hdl, sizeof(uuid_t));
	d_iov_set(&value, &hdl, sizeof(hdl));
	rc = rdb_tx_update(&tx, &svc->ps_handles, &key, &value);
	if (rc != 0)
		D_GOTO(out_pool_prop, rc);

	rc = rdb_tx_commit(&tx);
	if (rc)
		D_GOTO(out_pool_prop, rc);

	/* Update pool map by IV */
	rc = pool_map_update(rpc->cr_ctx, svc, map_version, map_buf);
	if (rc) {
		D_ERROR("pool_map_update failed %d.\n", rc);
		D_GOTO(out_pool_prop, rc);
	}

	/* Update pool properties by IV */
	rc = pool_iv_prop_update(svc->ps_pool, prop);
	if (rc)
		D_ERROR("pool_iv_prop_update failed %d.\n", rc);

out_pool_prop:
	daos_prop_free(prop);
out_map_version:
	out->pco_op.po_map_version = pool_map_get_version(svc->ps_pool->sp_map);
out_lock:
	ABT_rwlock_unlock(svc->ps_lock);
	rdb_tx_end(&tx);
out_svc:
	ds_rsvc_set_hint(&svc->ps_rsvc, &out->pco_op.po_hint);
	pool_svc_put_leader(svc);
out:
	out->pco_op.po_rc = rc;
	D_DEBUG(DF_DSMS, DF_UUID": replying rpc %p: %d\n",
		DP_UUID(in->pci_op.pi_uuid), rpc, rc);
	crt_reply_send(rpc);
}

static int
pool_disconnect_bcast(crt_context_t ctx, struct pool_svc *svc,
		      uuid_t *pool_hdls, int n_pool_hdls)
{
	struct pool_tgt_disconnect_in  *in;
	struct pool_tgt_disconnect_out *out;
	crt_rpc_t		       *rpc;
	int				rc;

	D_DEBUG(DF_DSMS, DF_UUID": bcasting\n", DP_UUID(svc->ps_uuid));

	rc = bcast_create(ctx, svc, POOL_TGT_DISCONNECT, NULL, &rpc);
	if (rc != 0)
		D_GOTO(out, rc);

	in = crt_req_get(rpc);
	uuid_copy(in->tdi_uuid, svc->ps_uuid);
	in->tdi_hdls.ca_arrays = pool_hdls;
	in->tdi_hdls.ca_count = n_pool_hdls;
	rc = dss_rpc_send(rpc);
	if (rc != 0)
		D_GOTO(out_rpc, rc);

	out = crt_reply_get(rpc);
	rc = out->tdo_rc;
	if (rc != 0) {
		D_ERROR(DF_UUID": failed to disconnect from %d targets\n",
			DP_UUID(svc->ps_uuid), rc);
		rc = -DER_IO;
	}

out_rpc:
	crt_req_decref(rpc);
out:
	D_DEBUG(DF_DSMS, DF_UUID": bcasted: %d\n", DP_UUID(svc->ps_uuid), rc);
	return rc;
}

static int
pool_disconnect_hdls(struct rdb_tx *tx, struct pool_svc *svc, uuid_t *hdl_uuids,
		     int n_hdl_uuids, crt_context_t ctx)
{
	d_iov_t	value;
	uint32_t	nhandles;
	int		i;
	int		rc;

	D_ASSERTF(n_hdl_uuids > 0, "%d\n", n_hdl_uuids);

	D_DEBUG(DF_DSMS, DF_UUID": disconnecting %d hdls: hdl_uuids[0]="DF_UUID
		"\n", DP_UUID(svc->ps_uuid), n_hdl_uuids,
		DP_UUID(hdl_uuids[0]));

	/*
	 * TODO: Send POOL_TGT_CLOSE_CONTS and somehow retry until every
	 * container service has responded (through ds_pool).
	 */
	rc = ds_cont_close_by_pool_hdls(svc->ps_uuid, hdl_uuids, n_hdl_uuids,
					ctx);
	if (rc != 0)
		D_GOTO(out, rc);

	rc = pool_disconnect_bcast(ctx, svc, hdl_uuids, n_hdl_uuids);
	if (rc != 0)
		D_GOTO(out, rc);

	d_iov_set(&value, &nhandles, sizeof(nhandles));
	rc = rdb_tx_lookup(tx, &svc->ps_root, &ds_pool_prop_nhandles, &value);
	if (rc != 0)
		D_GOTO(out, rc);

	nhandles -= n_hdl_uuids;

	for (i = 0; i < n_hdl_uuids; i++) {
		d_iov_t key;

		d_iov_set(&key, hdl_uuids[i], sizeof(uuid_t));
		rc = rdb_tx_delete(tx, &svc->ps_handles, &key);
		if (rc != 0)
			D_GOTO(out, rc);
	}

	d_iov_set(&value, &nhandles, sizeof(nhandles));
	rc = rdb_tx_update(tx, &svc->ps_root, &ds_pool_prop_nhandles, &value);
	if (rc != 0)
		D_GOTO(out, rc);

out:
	D_DEBUG(DF_DSMS, DF_UUID": leaving: %d\n", DP_UUID(svc->ps_uuid), rc);
	return rc;
}

void
ds_pool_disconnect_handler(crt_rpc_t *rpc)
{
	struct pool_disconnect_in      *pdi = crt_req_get(rpc);
	struct pool_disconnect_out     *pdo = crt_reply_get(rpc);
	struct pool_svc		       *svc;
	struct rdb_tx			tx;
	d_iov_t			key;
	d_iov_t			value;
	struct pool_hdl			hdl;
	int				rc;

	D_DEBUG(DF_DSMS, DF_UUID": processing rpc %p: hdl="DF_UUID"\n",
		DP_UUID(pdi->pdi_op.pi_uuid), rpc, DP_UUID(pdi->pdi_op.pi_hdl));

	rc = pool_svc_lookup_leader(pdi->pdi_op.pi_uuid, &svc,
				    &pdo->pdo_op.po_hint);
	if (rc != 0)
		D_GOTO(out, rc);

	rc = rdb_tx_begin(svc->ps_rsvc.s_db, svc->ps_rsvc.s_term, &tx);
	if (rc != 0)
		D_GOTO(out_svc, rc);

	ABT_rwlock_wrlock(svc->ps_lock);

	d_iov_set(&key, pdi->pdi_op.pi_hdl, sizeof(uuid_t));
	d_iov_set(&value, &hdl, sizeof(hdl));
	rc = rdb_tx_lookup(&tx, &svc->ps_handles, &key, &value);
	if (rc != 0) {
		if (rc == -DER_NONEXIST)
			rc = 0;
		D_GOTO(out_lock, rc);
	}

	rc = pool_disconnect_hdls(&tx, svc, &pdi->pdi_op.pi_hdl,
				  1 /* n_hdl_uuids */, rpc->cr_ctx);
	if (rc != 0)
		D_GOTO(out_lock, rc);

	rc = rdb_tx_commit(&tx);
	/* No need to set pdo->pdo_op.po_map_version. */
out_lock:
	ABT_rwlock_unlock(svc->ps_lock);
	rdb_tx_end(&tx);
out_svc:
	ds_rsvc_set_hint(&svc->ps_rsvc, &pdo->pdo_op.po_hint);
	pool_svc_put_leader(svc);
out:
	pdo->pdo_op.po_rc = rc;
	D_DEBUG(DF_DSMS, DF_UUID": replying rpc %p: %d\n",
		DP_UUID(pdi->pdi_op.pi_uuid), rpc, rc);
	crt_reply_send(rpc);
}

static int
pool_space_query_bcast(crt_context_t ctx, struct pool_svc *svc, uuid_t pool_hdl,
		       struct daos_pool_space *ps)
{
	struct pool_tgt_query_in	*in;
	struct pool_tgt_query_out	*out;
	crt_rpc_t			*rpc;
	int				 rc;

	D_DEBUG(DB_MD, DF_UUID": bcasting\n", DP_UUID(svc->ps_uuid));

	rc = bcast_create(ctx, svc, POOL_TGT_QUERY, NULL, &rpc);
	if (rc != 0)
		goto out;

	in = crt_req_get(rpc);
	uuid_copy(in->tqi_op.pi_uuid, svc->ps_uuid);
	uuid_copy(in->tqi_op.pi_hdl, pool_hdl);
	rc = dss_rpc_send(rpc);
	if (rc != 0)
		goto out_rpc;

	out = crt_reply_get(rpc);
	rc = out->tqo_rc;
	if (rc != 0) {
		D_ERROR(DF_UUID": failed to query from %d targets\n",
			DP_UUID(svc->ps_uuid), rc);
		rc = -DER_IO;
	} else {
		D_ASSERT(ps != NULL);
		*ps = out->tqo_space;
	}

out_rpc:
	crt_req_decref(rpc);
out:
	D_DEBUG(DB_MD, DF_UUID": bcasted: %d\n", DP_UUID(svc->ps_uuid), rc);
	return rc;
}

void
ds_pool_query_handler(crt_rpc_t *rpc)
{
	struct pool_query_in   *in = crt_req_get(rpc);
	struct pool_query_out  *out = crt_reply_get(rpc);
	daos_prop_t	       *prop = NULL;
	struct pool_buf		*map_buf;
	uint32_t		map_version;
	struct pool_svc	       *svc;
	struct rdb_tx		tx;
	d_iov_t		key;
	d_iov_t		value;
	struct pool_hdl		hdl;
	int			rc;

	D_DEBUG(DF_DSMS, DF_UUID": processing rpc %p: hdl="DF_UUID"\n",
		DP_UUID(in->pqi_op.pi_uuid), rpc, DP_UUID(in->pqi_op.pi_hdl));

	rc = pool_svc_lookup_leader(in->pqi_op.pi_uuid, &svc,
				    &out->pqo_op.po_hint);
	if (rc != 0)
		D_GOTO(out, rc);

	if (in->pqi_query_bits & DAOS_PO_QUERY_REBUILD_STATUS) {
		rc = ds_rebuild_query(in->pqi_op.pi_uuid, &out->pqo_rebuild_st);
		if (rc != 0)
			D_GOTO(out_svc, rc);
	}

	rc = rdb_tx_begin(svc->ps_rsvc.s_db, svc->ps_rsvc.s_term, &tx);
	if (rc != 0)
		D_GOTO(out_svc, rc);

	ABT_rwlock_rdlock(svc->ps_lock);

	/* Verify the pool handle. Note: since rebuild will not
	 * connect the pool, so we only verify the non-rebuild
	 * pool.
	 */
	if (!is_rebuild_pool(in->pqi_op.pi_uuid, in->pqi_op.pi_hdl)) {
		d_iov_set(&key, in->pqi_op.pi_hdl, sizeof(uuid_t));
		d_iov_set(&value, &hdl, sizeof(hdl));
		rc = rdb_tx_lookup(&tx, &svc->ps_handles, &key, &value);
		if (rc != 0) {
			if (rc == -DER_NONEXIST)
				rc = -DER_NO_HDL;
			D_GOTO(out_lock, rc);
		}
	}

	/* read optional properties */
	rc = pool_prop_read(&tx, svc, in->pqi_query_bits, &prop);
	if (rc != 0)
		D_GOTO(out_map_version, rc);
	out->pqo_prop = prop;

	if (DAOS_FAIL_CHECK(DAOS_FORCE_PROP_VERIFY)) {
		daos_prop_t		*iv_prop = NULL;
		struct daos_prop_entry	*entry, *iv_entry;
		int			i;

		D_ALLOC_PTR(iv_prop);
		if (iv_prop == NULL)
			D_GOTO(out_map_version, rc = -DER_NOMEM);

		rc = pool_iv_prop_fetch(svc->ps_pool, iv_prop);
		if (rc) {
			D_ERROR("pool_iv_prop_fetch failed %d.\n", rc);
			daos_prop_free(iv_prop);
			D_GOTO(out_map_version, rc);
		}

		for (i = 0; i < prop->dpp_nr; i++) {
			entry = &prop->dpp_entries[i];
			iv_entry = daos_prop_entry_get(iv_prop,
						       entry->dpe_type);
			D_ASSERT(iv_entry != NULL);
			switch (entry->dpe_type) {
			case DAOS_PROP_PO_LABEL:
			case DAOS_PROP_PO_OWNER:
			case DAOS_PROP_PO_OWNER_GROUP:
				D_ASSERT(strlen(entry->dpe_str) <=
					 DAOS_PROP_LABEL_MAX_LEN);
				if (strncmp(entry->dpe_str, iv_entry->dpe_str,
					    DAOS_PROP_LABEL_MAX_LEN) != 0) {
					D_ERROR("mismatch %s - %s.\n",
						entry->dpe_str,
						iv_entry->dpe_str);
					rc = -DER_IO;
				}
				break;
			case DAOS_PROP_PO_SPACE_RB:
			case DAOS_PROP_PO_SELF_HEAL:
			case DAOS_PROP_PO_RECLAIM:
				if (entry->dpe_val != iv_entry->dpe_val) {
					D_ERROR("type %d mismatch "DF_U64" - "
						DF_U64".\n", entry->dpe_type,
						entry->dpe_val,
						iv_entry->dpe_val);
					rc = -DER_IO;
				}
				break;
			case DAOS_PROP_PO_ACL:
				break;
			default:
				D_ASSERTF(0, "bad dpe_type %d\n",
					  entry->dpe_type);
				break;
			};
		}
		daos_prop_free(iv_prop);
		if (rc) {
			D_ERROR("iv_prop verify failed %d.\n", rc);
			D_GOTO(out_map_version, rc);
		}
	}

	rc = read_map_buf(&tx, &svc->ps_root, &map_buf, &map_version);
	if (rc != 0) {
		D_ERROR(DF_UUID": failed to read pool map: %d\n",
			DP_UUID(svc->ps_uuid), rc);
		D_GOTO(out_map_version, rc);
	}

	rc = transfer_map_buf(map_buf, map_version, svc, rpc, in->pqi_map_bulk,
			      &out->pqo_map_buf_size);
	if (rc != 0)
		D_GOTO(out_map_version, rc);

out_map_version:
	out->pqo_op.po_map_version = pool_map_get_version(svc->ps_pool->sp_map);
out_lock:
	ABT_rwlock_unlock(svc->ps_lock);
	rdb_tx_end(&tx);
out_svc:
	ds_rsvc_set_hint(&svc->ps_rsvc, &out->pqo_op.po_hint);
	/* See comment above, rebuild doesn't connect the pool */
	if (rc == 0 && (in->pqi_query_bits & DAOS_PO_QUERY_SPACE) &&
	    !is_rebuild_pool(in->pqi_op.pi_uuid, in->pqi_op.pi_hdl))
		rc = pool_space_query_bcast(rpc->cr_ctx, svc, in->pqi_op.pi_hdl,
					    &out->pqo_space);
	pool_svc_put_leader(svc);
out:
	out->pqo_op.po_rc = rc;
	D_DEBUG(DF_DSMS, DF_UUID": replying rpc %p: %d\n",
		DP_UUID(in->pqi_op.pi_uuid), rpc, rc);
	crt_reply_send(rpc);
	daos_prop_free(prop);
}


static int
replace_failed_replicas(struct pool_svc *svc, struct pool_map *map)
{
	d_rank_list_t	*replicas;
	d_rank_list_t	*tmp_replicas;
	d_rank_list_t	 failed_ranks;
	d_rank_list_t	 replace_ranks;
	int		 rc;

	rc = rdb_get_ranks(svc->ps_rsvc.s_db, &replicas);
	if (rc != 0)
		D_GOTO(out, rc);
	rc = ds_pool_check_failed_replicas(map, replicas, &failed_ranks,
					   &replace_ranks);
	if (rc != 0) {
		D_DEBUG(DB_MD, DF_UUID": cannot replace failed replicas: %d\n",
			DP_UUID(svc->ps_uuid), rc);
		D_GOTO(out, rc);
	}
	if (replace_ranks.rl_nr > 0)
		ds_rsvc_add_replicas_s(&svc->ps_rsvc, &replace_ranks,
				       ds_rsvc_get_md_cap());
	if (failed_ranks.rl_nr > 0)
		ds_rsvc_remove_replicas_s(&svc->ps_rsvc, &failed_ranks);
	/** `replace_ranks.rl_ranks` is not allocated and shouldn't be freed **/
	D_FREE(failed_ranks.rl_ranks);

	if (rdb_get_ranks(svc->ps_rsvc.s_db, &tmp_replicas) == 0) {
		daos_rank_list_sort(replicas);
		daos_rank_list_sort(tmp_replicas);
		if (!daos_rank_list_identical(replicas, tmp_replicas))
			D_DEBUG(DB_MD, DF_UUID": failed to update replicas\n",
				DP_UUID(svc->ps_uuid));
		d_rank_list_free(tmp_replicas);
	}
	d_rank_list_free(replicas);
out:
	return rc;
}

/* Callers are responsible for d_rank_list_free(*replicasp). */
static int
ds_pool_update_internal(uuid_t pool_uuid, struct pool_target_id_list *tgts,
			unsigned int opc, uint32_t *map_version_p,
			struct rsvc_hint *hint, bool *p_updated,
			d_rank_list_t **replicasp)
{
	struct pool_svc	       *svc;
	struct rdb_tx		tx;
	struct pool_map	       *map = NULL;
	uint32_t		map_version_before;
	uint32_t		map_version = 0;
	struct pool_buf	       *map_buf = NULL;
	bool			updated = false;
	struct dss_module_info *info = dss_get_module_info();
	int			rc;

	rc = pool_svc_lookup_leader(pool_uuid, &svc, hint);
	if (rc != 0)
		D_GOTO(out, rc);

	rc = rdb_tx_begin(svc->ps_rsvc.s_db, svc->ps_rsvc.s_term, &tx);
	if (rc != 0)
		D_GOTO(out_svc, rc);
	ABT_rwlock_wrlock(svc->ps_lock);

	/* Create a temporary pool map based on the last committed version. */
	rc = read_map(&tx, &svc->ps_root, &map);
	if (rc != 0)
		D_GOTO(out_replicas, rc);

	/*
	 * Attempt to modify the temporary pool map and save its versions
	 * before and after. If the version hasn't changed, we are done.
	 */
	map_version_before = pool_map_get_version(map);
	rc = ds_pool_map_tgts_update(map, tgts, opc);
	if (rc != 0)
		D_GOTO(out_replicas, rc);
	map_version = pool_map_get_version(map);

	D_DEBUG(DF_DSMS, DF_UUID": version=%u->%u\n",
		DP_UUID(svc->ps_uuid), map_version_before, map_version);
	if (map_version == map_version_before)
		D_GOTO(out_replicas, rc = 0);

	/* Write the new pool map. */
	rc = pool_buf_extract(map, &map_buf);
	if (rc != 0)
		D_GOTO(out_replicas, rc);
	rc = write_map_buf(&tx, &svc->ps_root, map_buf, map_version);
	if (rc != 0)
		D_GOTO(out_replicas, rc);

	rc = rdb_tx_commit(&tx);
	if (rc != 0) {
		D_DEBUG(DB_MD, DF_UUID": failed to commit: %d\n",
			DP_UUID(svc->ps_uuid), rc);
		D_GOTO(out_replicas, rc);
	}

	updated = true;

	/** TODO: Call disabled; enable along with `rsvc_client` changes **/
	(void) replace_failed_replicas;

out_replicas:
	if (replicasp != NULL) {
		if (rc == 0)
			rc = rdb_get_ranks(svc->ps_rsvc.s_db, replicasp);
		else
			*replicasp = NULL;
	}

	if (map_version_p != NULL)
		*map_version_p = pool_map_get_version((map == NULL || rc != 0) ?
						      svc->ps_pool->sp_map :
						      map);
	ABT_rwlock_unlock(svc->ps_lock);
	rdb_tx_end(&tx);
	if (map)
		pool_map_decref(map);

	/*
	 * Distribute pool map to other targets, and ignore the return code
	 * as we are more about committing a pool map change than its
	 * dissemination.
	 */
	if (updated)
		rc = pool_map_update(info->dmi_ctx, svc, map_version, map_buf);

	if (map_buf != NULL)
		pool_buf_free(map_buf);
out_svc:
	if (hint != NULL)
		ds_rsvc_set_hint(&svc->ps_rsvc, hint);
	pool_svc_put_leader(svc);
out:
	if (p_updated)
		*p_updated = updated;
	return rc;
}

static int
pool_find_all_targets_by_addr(uuid_t pool_uuid,
			      struct pool_target_addr_list *list,
			      struct pool_target_id_list *tgt_list,
			      struct pool_target_addr_list *out_list,
			      struct rsvc_hint *hint)
{
	struct pool_svc	*svc;
	struct rdb_tx	tx;
	struct pool_map *map = NULL;
	int		i;
	int		rc;

	rc = pool_svc_lookup_leader(pool_uuid, &svc, hint);
	if (rc != 0)
		D_GOTO(out, rc);

	rc = rdb_tx_begin(svc->ps_rsvc.s_db, svc->ps_rsvc.s_term, &tx);
	if (rc != 0)
		D_GOTO(out_svc, rc);
	ABT_rwlock_rdlock(svc->ps_lock);

	/* Create a temporary pool map based on the last committed version. */
	rc = read_map(&tx, &svc->ps_root, &map);

	ABT_rwlock_unlock(svc->ps_lock);
	rdb_tx_end(&tx);
	if (rc != 0)
		D_GOTO(out_svc, rc);

	for (i = 0; i < list->pta_number; i++) {
		struct pool_target *tgt;
		int tgt_nr;
		int j;
		int ret;

		tgt_nr = pool_map_find_target_by_rank_idx(map,
				list->pta_addrs[i].pta_rank,
				list->pta_addrs[i].pta_target, &tgt);
		if (tgt_nr <= 0) {
			/* Can not locate the target in pool map, let's
			 * add it to the output list
			 */
			D_WARN("Can not find %u/%d , add to out_list\n",
				list->pta_addrs[i].pta_rank,
				(int)list->pta_addrs[i].pta_target);
			ret = pool_target_addr_list_append(out_list,
						&list->pta_addrs[i]);
			if (ret) {
				rc = ret;
				break;
			}
		}

		for (j = 0; j < tgt_nr; j++) {
			struct pool_target_id tid;

			tid.pti_id = tgt[j].ta_comp.co_id;
			ret = pool_target_id_list_append(tgt_list, &tid);
			if (ret) {
				rc = ret;
				break;
			}
		}
	}
out_svc:
	pool_svc_put_leader(svc);
out:
	if (map != NULL)
		pool_map_decref(map);
	return rc;
}

int
ds_pool_tgt_exclude_out(uuid_t pool_uuid, struct pool_target_id_list *list)
{
	return ds_pool_update_internal(pool_uuid, list, POOL_EXCLUDE_OUT,
				       NULL, NULL, NULL, NULL);
}

int
ds_pool_tgt_exclude(uuid_t pool_uuid, struct pool_target_id_list *list)
{
	return ds_pool_update_internal(pool_uuid, list, POOL_EXCLUDE,
				       NULL, NULL, NULL, NULL);
}

/*
 * Perform a pool map update indicated by opc. If successful, the new pool map
 * version is reported via map_version. Upon -DER_NOTLEADER, a pool service
 * leader hint, if available, is reported via hint (if not NULL).
 */
static int
ds_pool_update(uuid_t pool_uuid, crt_opcode_t opc,
	       struct pool_target_addr_list *list,
	       struct pool_target_addr_list *out_list,
	       uint32_t *map_version, struct rsvc_hint *hint)
{
	struct pool_target_id_list	target_list = { 0 };
	d_rank_list_t			*replicas = NULL;
	bool				updated;
	int				rc;

	/* Convert target address list to target id list */
	rc = pool_find_all_targets_by_addr(pool_uuid, list, &target_list,
					   out_list, hint);
	if (rc)
		D_GOTO(out, rc);

	/* Update target by target id */
	rc = ds_pool_update_internal(pool_uuid, &target_list, opc, map_version,
				     hint, &updated, &replicas);
	if (rc)
		D_GOTO(out, rc);

	if (updated && opc == POOL_EXCLUDE) {
		char	*env;
		int	 ret;

		env = getenv(REBUILD_ENV);
		if ((env && !strcasecmp(env, REBUILD_ENV_DISABLED)) ||
		    daos_fail_check(DAOS_REBUILD_DISABLE)) {
			D_DEBUG(DB_TRACE, "Rebuild is disabled\n");
		} else { /* enabled by default */
			D_ASSERT(replicas != NULL);
			ret = ds_rebuild_schedule(pool_uuid, *map_version,
						  &target_list, replicas);
			if (ret != 0) {
				D_ERROR("rebuild fails rc %d\n", ret);
				if (rc == 0)
					rc = ret;
			}
		}
	}

out:
	pool_target_id_list_free(&target_list);
	if (replicas != NULL)
		d_rank_list_free(replicas);
	return rc;
}

void
ds_pool_update_handler(crt_rpc_t *rpc)
{
	struct pool_tgt_update_in	*in = crt_req_get(rpc);
	struct pool_tgt_update_out	*out = crt_reply_get(rpc);
	struct pool_target_addr_list	list = { 0 };
	struct pool_target_addr_list	out_list = { 0 };
	int				rc;

	if (in->pti_addr_list.ca_arrays == NULL ||
	    in->pti_addr_list.ca_count == 0)
		D_GOTO(out, rc = -DER_INVAL);

	D_DEBUG(DF_DSMS, DF_UUID": processing rpc %p: ntargets=%zu\n",
		DP_UUID(in->pti_op.pi_uuid), rpc, in->pti_addr_list.ca_count);

	list.pta_number = in->pti_addr_list.ca_count;
	list.pta_addrs = in->pti_addr_list.ca_arrays;
	rc = ds_pool_update(in->pti_op.pi_uuid, opc_get(rpc->cr_opc), &list,
			    &out_list, &out->pto_op.po_map_version,
			    &out->pto_op.po_hint);
	if (rc)
		D_GOTO(out, rc);

	out->pto_addr_list.ca_arrays = out_list.pta_addrs;
	out->pto_addr_list.ca_count = out_list.pta_number;

out:
	out->pto_op.po_rc = rc;
	D_DEBUG(DF_DSMS, DF_UUID": replying rpc %p: %d\n",
		DP_UUID(in->pti_op.pi_uuid), rpc, rc);
	crt_reply_send(rpc);
	pool_target_addr_list_free(&out_list);
}

struct evict_iter_arg {
	uuid_t *eia_hdl_uuids;
	size_t	eia_hdl_uuids_size;
	int	eia_n_hdl_uuids;
};

static int
evict_iter_cb(daos_handle_t ih, d_iov_t *key, d_iov_t *val, void *varg)
{
	struct evict_iter_arg  *arg = varg;

	D_ASSERT(arg->eia_hdl_uuids != NULL);
	D_ASSERT(arg->eia_hdl_uuids_size > sizeof(uuid_t));

	if (key->iov_len != sizeof(uuid_t) ||
	    val->iov_len != sizeof(struct pool_hdl)) {
		D_ERROR("invalid key/value size: key="DF_U64" value="DF_U64"\n",
			key->iov_len, val->iov_len);
		return -DER_IO;
	}

	/*
	 * Make sure arg->eia_hdl_uuids[arg->eia_hdl_uuids_size] have enough
	 * space for this handle.
	 */
	if (sizeof(uuid_t) * (arg->eia_n_hdl_uuids + 1) >
	    arg->eia_hdl_uuids_size) {
		uuid_t *hdl_uuids_tmp;
		size_t	hdl_uuids_size_tmp;

		hdl_uuids_size_tmp = arg->eia_hdl_uuids_size * 2;
		D_ALLOC(hdl_uuids_tmp, hdl_uuids_size_tmp);
		if (hdl_uuids_tmp == NULL)
			return -DER_NOMEM;
		memcpy(hdl_uuids_tmp, arg->eia_hdl_uuids,
		       arg->eia_hdl_uuids_size);
		D_FREE(arg->eia_hdl_uuids);
		arg->eia_hdl_uuids = hdl_uuids_tmp;
		arg->eia_hdl_uuids_size = hdl_uuids_size_tmp;
	}

	uuid_copy(arg->eia_hdl_uuids[arg->eia_n_hdl_uuids], key->iov_buf);
	arg->eia_n_hdl_uuids++;
	return 0;
}

/*
 * Callers are responsible for freeing *hdl_uuids if this function returns zero.
 */
static int
find_hdls_to_evict(struct rdb_tx *tx, struct pool_svc *svc, uuid_t **hdl_uuids,
		   size_t *hdl_uuids_size, int *n_hdl_uuids)
{
	struct evict_iter_arg	arg;
	int			rc;

	arg.eia_hdl_uuids_size = sizeof(uuid_t) * 4;
	D_ALLOC(arg.eia_hdl_uuids, arg.eia_hdl_uuids_size);
	if (arg.eia_hdl_uuids == NULL)
		return -DER_NOMEM;
	arg.eia_n_hdl_uuids = 0;

	rc = rdb_tx_iterate(tx, &svc->ps_handles, false /* backward */,
			    evict_iter_cb, &arg);
	if (rc != 0) {
		D_FREE(arg.eia_hdl_uuids);
		return rc;
	}

	*hdl_uuids = arg.eia_hdl_uuids;
	*hdl_uuids_size = arg.eia_hdl_uuids_size;
	*n_hdl_uuids = arg.eia_n_hdl_uuids;
	return 0;
}

void
ds_pool_evict_handler(crt_rpc_t *rpc)
{
	struct pool_evict_in   *in = crt_req_get(rpc);
	struct pool_evict_out  *out = crt_reply_get(rpc);
	struct pool_svc	       *svc;
	struct rdb_tx		tx;
	uuid_t		       *hdl_uuids;
	size_t			hdl_uuids_size;
	int			n_hdl_uuids;
	int			rc;

	D_DEBUG(DF_DSMS, DF_UUID": processing rpc %p\n",
		DP_UUID(in->pvi_op.pi_uuid), rpc);

	rc = pool_svc_lookup_leader(in->pvi_op.pi_uuid, &svc,
				    &out->pvo_op.po_hint);
	if (rc != 0)
		D_GOTO(out, rc);

	rc = rdb_tx_begin(svc->ps_rsvc.s_db, svc->ps_rsvc.s_term, &tx);
	if (rc != 0)
		D_GOTO(out_svc, rc);

	ABT_rwlock_wrlock(svc->ps_lock);

	rc = find_hdls_to_evict(&tx, svc, &hdl_uuids, &hdl_uuids_size,
				&n_hdl_uuids);
	if (rc != 0)
		D_GOTO(out_lock, rc);

	if (n_hdl_uuids > 0)
		rc = pool_disconnect_hdls(&tx, svc, hdl_uuids, n_hdl_uuids,
					  rpc->cr_ctx);

	rc = rdb_tx_commit(&tx);
	/* No need to set out->pvo_op.po_map_version. */
	D_FREE(hdl_uuids);
out_lock:
	ABT_rwlock_unlock(svc->ps_lock);
	rdb_tx_end(&tx);
out_svc:
	ds_rsvc_set_hint(&svc->ps_rsvc, &out->pvo_op.po_hint);
	pool_svc_put_leader(svc);
out:
	out->pvo_op.po_rc = rc;
	D_DEBUG(DF_DSMS, DF_UUID": replying rpc %p: %d\n",
		DP_UUID(in->pvi_op.pi_uuid), rpc, rc);
	crt_reply_send(rpc);
}

/* This RPC could be implemented by ds_rsvc. */
void
ds_pool_svc_stop_handler(crt_rpc_t *rpc)
{
	struct pool_svc_stop_in	       *in = crt_req_get(rpc);
	struct pool_svc_stop_out       *out = crt_reply_get(rpc);
	d_iov_t			id;
	int				rc;

	D_DEBUG(DF_DSMS, DF_UUID": processing rpc %p\n",
		DP_UUID(in->psi_op.pi_uuid), rpc);

	d_iov_set(&id, in->psi_op.pi_uuid, sizeof(uuid_t));
	rc = ds_rsvc_stop_leader(DS_RSVC_CLASS_POOL, &id, &out->pso_op.po_hint);

	out->pso_op.po_rc = rc;
	D_DEBUG(DF_DSMS, DF_UUID": replying rpc %p: %d\n",
		DP_UUID(in->psi_op.pi_uuid), rpc, rc);
	crt_reply_send(rpc);
}

/**
 * update pool map to all servers.
 **/
int
ds_pool_map_buf_get(uuid_t uuid, d_iov_t *iov, uint32_t *map_version)
{
	struct pool_svc	*svc;
	struct rdb_tx	tx;
	struct pool_buf	*map_buf;
	int		rc;

	rc = pool_svc_lookup_leader(uuid, &svc, NULL /* hint */);
	if (rc != 0)
		D_GOTO(out, rc);

	rc = rdb_tx_begin(svc->ps_rsvc.s_db, svc->ps_rsvc.s_term, &tx);
	if (rc != 0)
		D_GOTO(out_svc, rc);

	ABT_rwlock_rdlock(svc->ps_lock);
	rc = read_map_buf(&tx, &svc->ps_root, &map_buf, map_version);
	if (rc != 0) {
		D_ERROR(DF_UUID": failed to read pool map: %d\n",
			DP_UUID(svc->ps_uuid), rc);
		D_GOTO(out_lock, rc);
	}
	D_ASSERT(map_buf != NULL);
	iov->iov_buf = map_buf;
	iov->iov_len = pool_buf_size(map_buf->pb_nr);
	iov->iov_buf_len = pool_buf_size(map_buf->pb_nr);
out_lock:
	ABT_rwlock_unlock(svc->ps_lock);
	rdb_tx_end(&tx);
out_svc:
	pool_svc_put_leader(svc);
out:
	return rc;
}

/* Try to create iv namespace for the pool */
int
ds_pool_iv_ns_update(struct ds_pool *pool, unsigned int master_rank,
		     d_iov_t *iv_iov, unsigned int iv_ns_id)
{
	return ds_iv_ns_update(pool->sp_uuid, master_rank, pool->sp_group,
			       iv_iov, iv_ns_id, &pool->sp_iv_ns);
}

int
ds_pool_svc_term_get(uuid_t uuid, uint64_t *term)
{
	struct pool_svc	*svc;
	int		rc;

	rc = pool_svc_lookup_leader(uuid, &svc, NULL /* hint */);
	if (rc != 0)
		return rc;

	*term = svc->ps_rsvc.s_term;

	pool_svc_put_leader(svc);
	return 0;
}

void
ds_pool_attr_set_handler(crt_rpc_t *rpc)
{
	struct pool_attr_set_in  *in = crt_req_get(rpc);
	struct pool_op_out	 *out = crt_reply_get(rpc);
	struct pool_svc		 *svc;
	struct rdb_tx		  tx;
	int			  rc;

	D_DEBUG(DF_DSMS, DF_UUID": processing rpc %p: hdl="DF_UUID"\n",
		DP_UUID(in->pasi_op.pi_uuid), rpc, DP_UUID(in->pasi_op.pi_hdl));

	rc = pool_svc_lookup_leader(in->pasi_op.pi_uuid, &svc, &out->po_hint);
	if (rc != 0)
		goto out;

	rc = rdb_tx_begin(svc->ps_rsvc.s_db, svc->ps_rsvc.s_term, &tx);
	if (rc != 0)
		goto out_svc;

	ABT_rwlock_wrlock(svc->ps_lock);
	rc = ds_rsvc_set_attr(&svc->ps_rsvc, &tx, &svc->ps_user,
			      in->pasi_bulk, rpc, in->pasi_count);
	if (rc != 0)
		goto out_lock;

	rc = rdb_tx_commit(&tx);

out_lock:
	ABT_rwlock_unlock(svc->ps_lock);
	rdb_tx_end(&tx);
out_svc:
	ds_rsvc_set_hint(&svc->ps_rsvc, &out->po_hint);
	pool_svc_put_leader(svc);
out:
	out->po_rc = rc;
	D_DEBUG(DF_DSMS, DF_UUID": replying rpc %p: %d\n",
		DP_UUID(in->pasi_op.pi_uuid), rpc, rc);
	crt_reply_send(rpc);
}

void
ds_pool_attr_get_handler(crt_rpc_t *rpc)
{
	struct pool_attr_get_in  *in = crt_req_get(rpc);
	struct pool_op_out	 *out = crt_reply_get(rpc);
	struct pool_svc		 *svc;
	struct rdb_tx		  tx;
	int			  rc;

	D_DEBUG(DF_DSMS, DF_UUID": processing rpc %p: hdl="DF_UUID"\n",
		DP_UUID(in->pagi_op.pi_uuid), rpc, DP_UUID(in->pagi_op.pi_hdl));

	rc = pool_svc_lookup_leader(in->pagi_op.pi_uuid, &svc, &out->po_hint);
	if (rc != 0)
		goto out;

	rc = rdb_tx_begin(svc->ps_rsvc.s_db, svc->ps_rsvc.s_term, &tx);
	if (rc != 0)
		goto out_svc;

	ABT_rwlock_rdlock(svc->ps_lock);
	rc = ds_rsvc_get_attr(&svc->ps_rsvc, &tx, &svc->ps_user, in->pagi_bulk,
			      rpc, in->pagi_count, in->pagi_key_length);
	ABT_rwlock_unlock(svc->ps_lock);
	rdb_tx_end(&tx);
out_svc:
	ds_rsvc_set_hint(&svc->ps_rsvc, &out->po_hint);
	pool_svc_put_leader(svc);
out:
	out->po_rc = rc;
	D_DEBUG(DF_DSMS, DF_UUID": replying rpc %p: %d\n",
		DP_UUID(in->pagi_op.pi_uuid), rpc, rc);
	crt_reply_send(rpc);

}

void
ds_pool_attr_list_handler(crt_rpc_t *rpc)
{
	struct pool_attr_list_in	*in	    = crt_req_get(rpc);
	struct pool_attr_list_out	*out	    = crt_reply_get(rpc);
	struct pool_svc			*svc;
	struct rdb_tx			 tx;
	int				 rc;

	D_DEBUG(DF_DSMS, DF_UUID": processing rpc %p: hdl="DF_UUID"\n",
		DP_UUID(in->pali_op.pi_uuid), rpc, DP_UUID(in->pali_op.pi_hdl));

	rc = pool_svc_lookup_leader(in->pali_op.pi_uuid, &svc,
				    &out->palo_op.po_hint);
	if (rc != 0)
		goto out;

	rc = rdb_tx_begin(svc->ps_rsvc.s_db, svc->ps_rsvc.s_term, &tx);
	if (rc != 0)
		goto out_svc;

	ABT_rwlock_rdlock(svc->ps_lock);
	rc = ds_rsvc_list_attr(&svc->ps_rsvc, &tx, &svc->ps_user,
			       in->pali_bulk, rpc, &out->palo_size);
	ABT_rwlock_unlock(svc->ps_lock);
	rdb_tx_end(&tx);
out_svc:
	ds_rsvc_set_hint(&svc->ps_rsvc, &out->palo_op.po_hint);
	pool_svc_put_leader(svc);
out:
	out->palo_op.po_rc = rc;
	D_DEBUG(DF_DSMS, DF_UUID": replying rpc %p: %d\n",
		DP_UUID(in->pali_op.pi_uuid), rpc, rc);
	crt_reply_send(rpc);
}

void
ds_pool_replicas_update_handler(crt_rpc_t *rpc)
{
	struct pool_membership_in	*in = crt_req_get(rpc);
	struct pool_membership_out	*out = crt_reply_get(rpc);
	d_rank_list_t			*ranks;
	d_iov_t				 id;
	int				 rc;

	rc = daos_rank_list_dup(&ranks, in->pmi_targets);
	if (rc != 0)
		goto out;
	d_iov_set(&id, in->pmi_uuid, sizeof(uuid_t));

	switch (opc_get(rpc->cr_opc)) {
	case POOL_REPLICAS_ADD:
		rc = ds_rsvc_add_replicas(DS_RSVC_CLASS_POOL, &id, ranks,
					  ds_rsvc_get_md_cap(), &out->pmo_hint);
		break;

	case POOL_REPLICAS_REMOVE:
		rc = ds_rsvc_remove_replicas(DS_RSVC_CLASS_POOL, &id, ranks,
					     &out->pmo_hint);
		break;

	default:
		D_ASSERT(0);
	}

	out->pmo_failed = ranks;
out:
	out->pmo_rc = rc;
	crt_reply_send(rpc);
}

/**
 * Check whether the leader replica of the given object resides
 * on current server or not.
 *
 * \param [IN]	pool_uuid	The pool UUID
 * \param [IN]	oid		The OID of the object to be checked
 * \param [IN]	version		The pool map version
 * \param [OUT]	plo		The pointer to the pl_obj_layout of the object
 *
 * \return			+1 if leader is on current server.
 * \return			Zero if the leader resides on another server.
 * \return			Negative value if error.
 */
int
ds_pool_check_leader(uuid_t pool_uuid, daos_unit_oid_t *oid,
		     uint32_t version, struct pl_obj_layout **plo)
{
	struct ds_pool		*pool;
	struct pl_map		*map = NULL;
	struct pl_obj_layout	*layout = NULL;
	struct pool_target	*target;
	struct daos_obj_md	 md = { 0 };
	int			 leader;
	d_rank_t		 myrank;
	int			 rc = 0;

	pool = ds_pool_lookup(pool_uuid);
	if (pool == NULL)
		return -DER_INVAL;

	map = pl_map_find(pool_uuid, oid->id_pub);
	if (map == NULL) {
		D_WARN("Failed to find pool map tp select leader for "
		       DF_UOID" version = %d\n", DP_UOID(*oid), version);
		rc = -DER_INVAL;
		goto out;
	}

	md.omd_id = oid->id_pub;
	md.omd_ver = version;
	rc = pl_obj_place(map, &md, NULL, &layout);
	if (rc != 0)
		goto out;

	leader = pl_select_leader(oid->id_pub, oid->id_shard,
				  layout->ol_grp_size, true,
				  pl_obj_get_shard, layout);
	if (leader < 0) {
		D_WARN("Failed to select leader for "DF_UOID
		       "version = %d: rc = %d\n",
		       DP_UOID(*oid), version, leader);
		D_GOTO(out, rc = leader);
	}

	D_DEBUG(DB_TRACE, "get new leader tgt id %d\n", leader);
	rc = pool_map_find_target(pool->sp_map, leader, &target);
	if (rc < 0)
		goto out;

	if (rc != 1)
		D_GOTO(out, rc = -DER_INVAL);

	crt_group_rank(pool->sp_group, &myrank);
	if (myrank != target->ta_comp.co_rank) {
		rc = 0;
	} else {
		if (plo != NULL)
			*plo = layout;
		rc = 1;
	}

out:
	if (rc <= 0 && layout != NULL)
		pl_obj_layout_free(layout);
	if (map != NULL)
		pl_map_decref(map);
	ds_pool_put(pool);
	return rc;
}

/* Update pool map version for current xstream. */
int
ds_pool_child_map_refresh_sync(struct ds_pool_child *dpc)
{
	struct pool_map_refresh_ult_arg	arg;
	ABT_eventual			eventual;
	int				*status;
	int				rc;

	rc = ABT_eventual_create(sizeof(*status), &eventual);
	if (rc != ABT_SUCCESS)
		return dss_abterr2der(rc);

	arg.iua_pool_version = dpc->spc_map_version;
	uuid_copy(arg.iua_pool_uuid, dpc->spc_uuid);
	arg.iua_eventual = eventual;

	rc = dss_ult_create(ds_pool_map_refresh_ult, &arg, DSS_ULT_POOL_SRV,
			    0, 0, NULL);
	if (rc)
		D_GOTO(out_eventual, rc);

	rc = ABT_eventual_wait(eventual, (void **)&status);
	if (rc != ABT_SUCCESS)
		D_GOTO(out_eventual, rc = dss_abterr2der(rc));
	if (*status != 0)
		D_GOTO(out_eventual, rc = *status);

out_eventual:
	ABT_eventual_free(&eventual);
	return rc;
}

int
ds_pool_child_map_refresh_async(struct ds_pool_child *dpc)
{
	struct pool_map_refresh_ult_arg	*arg;
	int				rc;

	D_ALLOC_PTR(arg);
	if (arg == NULL)
		return -DER_NOMEM;
	arg->iua_pool_version = dpc->spc_map_version;
	uuid_copy(arg->iua_pool_uuid, dpc->spc_uuid);

	rc = dss_ult_create(ds_pool_map_refresh_ult, arg, DSS_ULT_POOL_SRV,
			    0, 0, NULL);
	return rc;
}
