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
 * ds_pool: Pool Server Internal Declarations
 */

#ifndef __POOL_SRV_INTERNAL_H__
#define __POOL_SRV_INTERNAL_H__

#include <gurt/list.h>
#include <daos_srv/daos_server.h>
#include <daos_security.h>

/**
 * DSM server thread local storage structure
 */
struct pool_tls {
	/* in-memory structures TLS instance */
	struct d_list_head	dt_pool_list;
};

extern struct dss_module_key pool_module_key;

static inline struct pool_tls *
pool_tls_get()
{
	struct pool_tls			*tls;
	struct dss_thread_local_storage	*dtc;

	dtc = dss_tls_get();
	tls = (struct pool_tls *)dss_module_key_get(dtc, &pool_module_key);
	return tls;
}

struct pool_iv_map {
	struct pool_buf	piv_pool_buf;
};

struct pool_iv_prop {
	char		pip_label[DAOS_PROP_LABEL_MAX_LEN];
	char		pip_owner[DAOS_ACL_MAX_PRINCIPAL_BUF_LEN];
	char		pip_owner_grp[DAOS_ACL_MAX_PRINCIPAL_BUF_LEN];
	uint64_t	pip_space_rb;
	uint64_t	pip_self_heal;
	uint64_t	pip_reclaim;
	struct daos_acl	pip_acl;
};

struct pool_iv_entry {
	uuid_t				piv_pool_uuid;
	uint32_t			piv_master_rank;
	uint32_t			piv_pool_map_ver;
	union	{
		struct pool_iv_map	piv_map;
		struct pool_iv_prop	piv_prop;
	};
};

struct pool_map_refresh_ult_arg {
	uint32_t	iua_pool_version;
	uuid_t		iua_pool_uuid;
	ABT_eventual	iua_eventual;
};

/*
 * srv_pool.c
 */
void ds_pool_rsvc_class_register(void);
void ds_pool_rsvc_class_unregister(void);
int ds_pool_svc_start_all(void);
int ds_pool_svc_stop_all(void);
void ds_pool_create_handler(crt_rpc_t *rpc);
void ds_pool_connect_handler(crt_rpc_t *rpc);
void ds_pool_disconnect_handler(crt_rpc_t *rpc);
void ds_pool_query_handler(crt_rpc_t *rpc);
void ds_pool_update_handler(crt_rpc_t *rpc);
void ds_pool_evict_handler(crt_rpc_t *rpc);
void ds_pool_svc_stop_handler(crt_rpc_t *rpc);
void ds_pool_attr_list_handler(crt_rpc_t *rpc);
void ds_pool_attr_get_handler(crt_rpc_t *rpc);
void ds_pool_attr_set_handler(crt_rpc_t *rpc);

/*
 * srv_target.c
 */
int ds_pool_cache_init(void);
void ds_pool_cache_fini(void);
int ds_pool_hdl_hash_init(void);
void ds_pool_hdl_hash_fini(void);
void ds_pool_tgt_connect_handler(crt_rpc_t *rpc);
int ds_pool_tgt_connect_aggregator(crt_rpc_t *source, crt_rpc_t *result,
				   void *priv);
void ds_pool_tgt_disconnect_handler(crt_rpc_t *rpc);
int ds_pool_tgt_disconnect_aggregator(crt_rpc_t *source, crt_rpc_t *result,
				      void *priv);
void ds_pool_tgt_update_map_handler(crt_rpc_t *rpc);
int ds_pool_tgt_update_map_aggregator(crt_rpc_t *source, crt_rpc_t *result,
				      void *priv);
void ds_pool_tgt_query_handler(crt_rpc_t *rpc);
int ds_pool_tgt_query_aggregator(crt_rpc_t *source, crt_rpc_t *result,
				 void *priv);
void ds_pool_child_purge(struct pool_tls *tls);
void ds_pool_replicas_update_handler(crt_rpc_t *rpc);

/*
 * srv_util.c
 */
int ds_pool_group_create(const uuid_t pool_uuid, const struct pool_map *map,
			 crt_group_t **group);
int ds_pool_group_destroy(const uuid_t pool_uuid, crt_group_t *group);
int ds_pool_map_tgts_update(struct pool_map *map,
			    struct pool_target_id_list *tgts, int opc);
int ds_pool_check_failed_replicas(struct pool_map *map, d_rank_list_t *replicas,
				  d_rank_list_t *failed, d_rank_list_t *alt);

/*
 * srv_iv.c
 */
uint32_t pool_iv_map_ent_size(int nr);
int ds_pool_iv_init(void);
int ds_pool_iv_fini(void);
int pool_iv_prop_update(struct ds_pool *pool, daos_prop_t *prop);
int pool_iv_prop_fetch(struct ds_pool *pool, daos_prop_t *prop);
int pool_iv_map_update(struct ds_pool *pool, struct pool_buf *buf,
		       uint32_t map_ver);
int pool_iv_map_fetch(void *ns, struct pool_iv_entry *pool_iv);
void ds_pool_map_refresh_ult(void *arg);
#endif /* __POOL_SRV_INTERNAL_H__ */
