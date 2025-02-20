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

enum cont_op {
	CONT_CREATE,
	CONT_DESTROY,
	CONT_LIST_OBJS,
	CONT_QUERY,
	CONT_STAT,
	CONT_GET_PROP,
	CONT_SET_PROP,
	CONT_LIST_ATTRS,
	CONT_DEL_ATTR,
	CONT_GET_ATTR,
	CONT_SET_ATTR,
	CONT_CREATE_SNAP,
	CONT_LIST_SNAPS,
	CONT_DESTROY_SNAP,
	CONT_ROLLBACK
};

enum pool_op {
	POOL_LIST_CONTAINERS,
	POOL_QUERY,
	POOL_STAT,
	POOL_GET_PROP,
	POOL_GET_ATTR,
	POOL_LIST_ATTRS
};

enum obj_op {
	OBJ_QUERY,
	OBJ_LIST_KEYS,
	OBJ_DUMP
};

/* cmd_args_s: consolidated result of parsing command-line arguments
 * for pool, cont, obj commands, much of which is common.
 */

struct cmd_args_s {
	enum pool_op		p_op;		/* pool sub-command */
	enum cont_op		c_op;		/* cont sub-command */
	enum obj_op		o_op;		/* obj sub-command */
	char			*sysname;	/* --sys-name or --sys */
	uuid_t			p_uuid;		/* --pool */
	daos_handle_t		pool;
	uuid_t			c_uuid;		/* --cont */
	daos_handle_t		cont;
	char			*mdsrv_str;	/* --svc */
	d_rank_list_t		*mdsrv;
	int			force_destroy;	/* --force (cont destroy) */
	char			*attrname_str;	/* --attr attribute name */
	char			*value_str;	/* --value attribute value */

	/* Container unified namespace (path) related */
	char			*path;		/* --path cont namespace */
	daos_cont_layout_t	type;		/* --type cont type */
	daos_oclass_id_t	oclass;		/* --oclass object class */
	daos_size_t		chunk_size;	/* --chunk_size of cont objs */

	/* Container snapshot/rollback related */
	char			*snapname_str;	/* --snap cont snapshot name */
	daos_epoch_t		epc;		/* --epc cont epoch */
	char			*epcrange_str;	/* --epcrange cont epochs */
	daos_epoch_t		epcrange_begin;
	daos_epoch_t		epcrange_end;
	daos_obj_id_t		oid;

	FILE			*ostream;	/* help_hdlr() stream */
};

#define ARGS_VERIFY_PUUID(ap, label, rcexpr)			\
	do {							\
		if (uuid_is_null((ap)->p_uuid)) {		\
			fprintf(stderr, "pool UUID required\n");\
			D_GOTO(label, (rcexpr));		\
		}						\
	} while (0)

#define ARGS_VERIFY_MDSRV(ap, label, rcexpr)				\
	do {								\
		if ((ap)->mdsrv_str == NULL) {				\
			fprintf(stderr, "--svc must be specified\n");	\
			D_GOTO(label, (rcexpr));			\
		}							\
		if ((ap)->mdsrv == NULL) {				\
			fprintf(stderr, "failed to parse--svc=%s\n",	\
					(ap)->mdsrv_str);		\
			D_GOTO(label, (rcexpr));			\
		}							\
		if ((ap)->mdsrv->rl_nr == 0) {				\
			fprintf(stderr, "--svc must not be empty\n");	\
			D_GOTO(label, (rcexpr));			\
		}							\
	} while (0)

#define ARGS_VERIFY_CUUID(ap, label, rcexpr)				\
	do {								\
		if (uuid_is_null((ap)->c_uuid)) {			\
			fprintf(stderr, "container UUID required\n");	\
			D_GOTO(label, (rcexpr));			\
		}							\
	} while (0)

#define ARGS_VERIFY_OID(ap, label, rcexpr)				\
	do {								\
		if (((ap)->oid.hi == 0) && ((ap)->oid.lo == 0)) {	\
			fprintf(stderr, "object ID required\n");	\
			D_GOTO(label, (rcexpr));			\
		}							\
	} while (0)

#define ARGS_VERIFY_PATH_CREATE(ap, label, rcexpr)			\
	do {								\
		if (((ap)->type == DAOS_PROP_CO_LAYOUT_UNKOWN) ||	\
		    ((ap)->oclass == OC_UNKNOWN)	||		\
		    ((ap)->chunk_size == 0)) {				\
			fprintf(stderr, "create by --path : must also "	\
					"specify --type, --oclass, "	\
					"and --chunk_size\n");		\
			D_GOTO(label, (rcexpr));			\
		}							\
	} while (0)

#define ARGS_VERIFY_PATH_NON_CREATE(ap, label, rcexpr)			\
	do {								\
		if (((ap)->type != DAOS_PROP_CO_LAYOUT_UNKOWN) ||	\
		    ((ap)->oclass != OC_UNKNOWN)	||		\
		    ((ap)->chunk_size != 0)) {				\
			fprintf(stderr, "query by --path : do not "	\
					"specify --type, --oclass, "	\
					"or --chunk_size\n");		\
			D_GOTO(label, (rcexpr));			\
		}							\
		if (!uuid_is_null((ap)->p_uuid)) {			\
			fprintf(stderr, "query by --path : do not "	\
					"specify --pool\n");		\
			D_GOTO(label, (rcexpr));			\
		}							\
		if (!uuid_is_null((ap)->c_uuid)) {			\
			fprintf(stderr, "query by --path : do not "	\
					"specify --cont\n");		\
			D_GOTO(label, (rcexpr));			\
		}							\
	} while (0)

typedef int (*command_hdlr_t)(struct cmd_args_s *ap);

/* Pool operations */
int pool_query_hdlr(struct cmd_args_s *ap);

/* TODO: implement these pool op functions
 * int pool_list_cont_hdlr(struct cmd_args_s *ap);
 * int pool_stat_hdlr(struct cmd_args_s *ap);
 * int pool_get_prop_hdlr(struct cmd_args_s *ap);
 * int pool_get_attr_hdlr(struct cmd_args_s *ap);
 * int pool_list_attrs_hdlr(struct cmd_args_s *ap);
 */

/* Container operations */
int cont_create_hdlr(struct cmd_args_s *ap);
int cont_create_uns_hdlr(struct cmd_args_s *ap);
int cont_query_hdlr(struct cmd_args_s *ap);
int cont_destroy_hdlr(struct cmd_args_s *ap);

/* TODO implement the following container op functions
 * all with signatures similar to this:
 * int cont_FN_hdlr(struct cmd_args_s *ap)
 *
 * cont_list_objs_hdlr()
 * int cont_stat_hdlr()
 * int cont_get_prop_hdlr()
 * int cont_set_prop_hdlr()
 * int cont_list_attrs_hdlr()
 * int cont_del_attr_hdlr()
 * int cont_get_attr_hdlr()
 * int cont_set_attr_hdlr()
 * int cont_create_snap_hdlr()
 * int cont_list_snaps_hdlr()
 * int cont_destroy_snap_hdlr()
 * int cont_rollback_hdlr()
 */

int obj_query_hdlr(struct cmd_args_s *ap);
