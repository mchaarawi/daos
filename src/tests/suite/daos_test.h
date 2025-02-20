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
/**
 * This file is part of DAOS
 * src/tests/suite/daos_test.h
 */
#ifndef __DAOS_TEST_H
#define __DAOS_TEST_H
#if !defined(__has_warning)  /* gcc */
	#pragma GCC diagnostic ignored "-Wframe-larger-than="
#else
	#if __has_warning("-Wframe-larger-than=") /* valid clang warning */
		#pragma GCC diagnostic ignored "-Wframe-larger-than="
	#endif
#endif

#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <time.h>
#include <linux/limits.h>
#include <sys/stat.h>
#include <dirent.h>

#include <cmocka.h>
/* redefine cmocka's skip() so it will no longer abort()
 * if CMOCKA_TEST_ABORT=1
 *
 * it can't be redefined as a function as it must return from current context
 */
#undef skip
#define skip() \
	do { \
		const char *abort_test = getenv("CMOCKA_TEST_ABORT"); \
		if (abort_test != NULL && abort_test[0] == '1') \
			print_message("Skipped !!!\n"); \
		else \
			_skip(__FILE__, __LINE__); \
		return; \
	} while  (0)

#include <mpi.h>
#include <daos/debug.h>
#include <daos/common.h>
#include <daos/mgmt.h>
#include <daos/tests_lib.h>
#include <daos.h>

/** Server crt group ID */
extern const char *server_group;

/** Pool service replicas */
extern unsigned int svc_nreplicas;

/* the temporary IO dir*/
extern char *test_io_dir;
/* the IO conf file*/
extern const char *test_io_conf;

extern int daos_event_priv_reset(void);
#define TEST_RANKS_MAX_NUM	(13)

/* the pool used for daos test suite */
struct test_pool {
	d_rank_t		ranks[TEST_RANKS_MAX_NUM];
	uuid_t			pool_uuid;
	daos_handle_t		poh;
	daos_pool_info_t	pool_info;
	daos_size_t		pool_size;
	d_rank_list_t		svc;
	/* flag of slave that share the pool of other test_arg_t */
	bool			slave;
	bool			destroyed;
};

struct epoch_io_args {
	d_list_t		 op_list;
	int			 op_lvl; /* enum test_level */
	daos_size_t		 op_iod_size;
	/* now using only one oid, can change later when needed */
	daos_obj_id_t		 op_oid;
	/* cached dkey/akey used last time, so need not specify it every time */
	char			*op_dkey;
	char			*op_akey;
	int			op_no_verify:1;
};

typedef struct {
	bool			multi_rank;
	int			myrank;
	int			rank_size;
	const char	       *group;
	struct test_pool	pool;
	uuid_t			co_uuid;
	unsigned int		mode;
	unsigned int		uid;
	unsigned int		gid;
	daos_handle_t		eq;
	daos_handle_t		coh;
	daos_cont_info_t	co_info;
	int			setup_state;
	bool			async;
	bool			hdl_share;
	uint64_t		fail_loc;
	uint64_t		fail_num;
	uint64_t		fail_value;
	bool			overlap;
	int			expect_result;
	daos_size_t		size;
	int			nr;
	int			srv_nnodes;
	int			srv_ntgts;
	int			srv_disabled_ntgts;
	int			index;
	daos_epoch_t		hce;

	/* The callback is called before pool rebuild. like disconnect
	 * pool etc.
	 */
	int			(*rebuild_pre_cb)(void *test_arg);
	void			*rebuild_pre_cb_arg;

	/* The callback is called during pool rebuild, used for concurrent IO,
	 * container destroy etc
	 */
	int			(*rebuild_cb)(void *test_arg);
	void			*rebuild_cb_arg;
	/* The callback is called after pool rebuild, used for validating IO
	 * after rebuild
	 */
	int			(*rebuild_post_cb)(void *test_arg);
	void			*rebuild_post_cb_arg;
	/* epoch IO OP queue */
	struct epoch_io_args	eio_args;
} test_arg_t;

enum {
	SETUP_EQ,
	SETUP_POOL_CREATE,
	SETUP_POOL_CONNECT,
	SETUP_CONT_CREATE,
	SETUP_CONT_CONNECT,
};

#define DEFAULT_POOL_SIZE	(4ULL << 30)

#define WAIT_ON_ASYNC_ERR(arg, ev, err)			\
	do {						\
		int _rc;					\
		daos_event_t *evp;			\
							\
		if (!arg->async)			\
			break;				\
							\
		_rc = daos_eq_poll(arg->eq, 1,		\
				  DAOS_EQ_WAIT,		\
				  1, &evp);		\
		assert_int_equal(_rc, 1);		\
		assert_ptr_equal(evp, &ev);		\
		assert_int_equal(ev.ev_error, err);	\
	} while (0)

#define WAIT_ON_ASYNC(arg, ev) WAIT_ON_ASYNC_ERR(arg, ev, 0)

int
test_teardown(void **state);
int
test_setup(void **state, unsigned int step, bool multi_rank,
	   daos_size_t pool_size, struct test_pool *pool);
int
test_setup_next_step(void **state, struct test_pool *pool, daos_prop_t *po_prop,
		     daos_prop_t *co_prop);

static inline int
async_enable(void **state)
{
	test_arg_t	*arg = *state;

	arg->overlap = false;
	arg->async   = true;
	return 0;
}

static inline int
async_disable(void **state)
{
	test_arg_t	*arg = *state;

	arg->overlap = false;
	arg->async   = false;
	return 0;
}

static inline int
async_overlap(void **state)
{
	test_arg_t	*arg = *state;

	arg->overlap = true;
	arg->async   = true;
	return 0;
}

static inline int
test_case_teardown(void **state)
{
	assert_int_equal(daos_event_priv_reset(), 0);
	return 0;
}

static inline int
hdl_share_enable(void **state)
{
	test_arg_t	*arg = *state;

	arg->hdl_share = true;
	return 0;
}

enum {
	HANDLE_POOL,
	HANDLE_CO
};

int run_daos_mgmt_test(int rank, int size);
int run_daos_pool_test(int rank, int size);
int run_daos_cont_test(int rank, int size);
int run_daos_capa_test(int rank, int size);
int run_daos_io_test(int rank, int size, int *tests, int test_size);
int run_daos_epoch_io_test(int rank, int size, int *tests, int test_size);
int run_daos_obj_array_test(int rank, int size);
int run_daos_array_test(int rank, int size);
int run_daos_kv_test(int rank, int size);
int run_daos_epoch_test(int rank, int size);
int run_daos_epoch_recovery_test(int rank, int size);
int run_daos_md_replication_test(int rank, int size);
int run_daos_oid_alloc_test(int rank, int size);
int run_daos_degraded_test(int rank, int size);
int run_daos_rebuild_test(int rank, int size, int *tests, int test_size);

void daos_kill_server(test_arg_t *arg, const uuid_t pool_uuid, const char *grp,
		      d_rank_list_t *svc, d_rank_t rank);
void daos_kill_exclude_server(test_arg_t *arg, const uuid_t pool_uuid,
			      const char *grp, d_rank_list_t *svc);
typedef int (*test_setup_cb_t)(void **state);
typedef int (*test_teardown_cb_t)(void **state);

bool test_runable(test_arg_t *arg, unsigned int required_tgts);
int test_pool_get_info(test_arg_t *arg, daos_pool_info_t *pinfo);
int test_get_leader(test_arg_t *arg, d_rank_t *rank);
bool test_rebuild_query(test_arg_t **args, int args_cnt);
void test_rebuild_wait(test_arg_t **args, int args_cnt);
void daos_exclude_target(const uuid_t pool_uuid, const char *grp,
			 const d_rank_list_t *svc, d_rank_t rank, int tgt);
void daos_add_target(const uuid_t pool_uuid, const char *grp,
		     const d_rank_list_t *svc, d_rank_t rank, int tgt);

void daos_exclude_server(const uuid_t pool_uuid, const char *grp,
			 const d_rank_list_t *svc, d_rank_t rank);
void daos_add_server(const uuid_t pool_uuid, const char *grp,
		     const d_rank_list_t *svc, d_rank_t rank);

int run_daos_sub_tests(const struct CMUnitTest *tests, int tests_size,
		       daos_size_t pool_size, int *sub_tests,
		       int sub_tests_size, test_setup_cb_t setup_cb,
		       test_teardown_cb_t teardown_cb);

static inline void
daos_test_print(int rank, char *message)
{
	if (!rank)
		print_message("%s\n", message);
}

static inline void
handle_share(daos_handle_t *hdl, int type, int rank, daos_handle_t poh,
	     int verbose)
{
	d_iov_t	ghdl = { NULL, 0, 0 };
	int		rc;

	if (rank == 0) {
		/** fetch size of global handle */
		if (type == HANDLE_POOL)
			rc = daos_pool_local2global(*hdl, &ghdl);
		else
			rc = daos_cont_local2global(*hdl, &ghdl);
		assert_int_equal(rc, 0);
	}

	/** broadcast size of global handle to all peers */
	rc = MPI_Bcast(&ghdl.iov_buf_len, 1, MPI_UINT64_T, 0, MPI_COMM_WORLD);
	assert_int_equal(rc, MPI_SUCCESS);

	/** allocate buffer for global pool handle */
	D_ALLOC(ghdl.iov_buf, ghdl.iov_buf_len);
	ghdl.iov_len = ghdl.iov_buf_len;

	if (rank == 0) {
		/** generate actual global handle to share with peer tasks */
		if (verbose)
			print_message("rank 0 call local2global on %s handle",
				      (type == HANDLE_POOL) ?
				      "pool" : "container");
		if (type == HANDLE_POOL)
			rc = daos_pool_local2global(*hdl, &ghdl);
		else
			rc = daos_cont_local2global(*hdl, &ghdl);
		assert_int_equal(rc, 0);
		if (verbose)
			print_message("success\n");
	}

	/** broadcast global handle to all peers */
	if (rank == 0 && verbose == 1)
		print_message("rank 0 broadcast global %s handle ...",
			      (type == HANDLE_POOL) ? "pool" : "container");
	rc = MPI_Bcast(ghdl.iov_buf, ghdl.iov_len, MPI_BYTE, 0,
		       MPI_COMM_WORLD);
	assert_int_equal(rc, MPI_SUCCESS);
	if (rank == 0 && verbose == 1)
		print_message("success\n");

	if (rank != 0) {
		/** unpack global handle */
		if (verbose)
			print_message("rank %d call global2local on %s handle",
				      rank, type == HANDLE_POOL ?
				      "pool" : "container");
		if (type == HANDLE_POOL) {
			/* NB: Only pool_global2local are different */
			rc = daos_pool_global2local(ghdl, hdl);
		} else {
			rc = daos_cont_global2local(poh, ghdl, hdl);
		}

		assert_int_equal(rc, 0);
		if (verbose)
			print_message("rank %d global2local success\n", rank);
	}

	D_FREE(ghdl.iov_buf);

	MPI_Barrier(MPI_COMM_WORLD);
}

#define MAX_KILLS	3
extern d_rank_t ranks_to_kill[MAX_KILLS];
d_rank_t test_get_last_svr_rank(test_arg_t *arg);

/* make dir including its parent dir */
static inline int
test_mkdir(char *dir, mode_t mode)
{
	char	*p;
	mode_t	 stored_mode;
	char	 parent_dir[PATH_MAX] = { 0 };

	if (dir == NULL || *dir == '\0')
		return daos_errno2der(errno);

	stored_mode = umask(0);
	p = strrchr(dir, '/');
	if (p != NULL) {
		strncpy(parent_dir, dir, p - dir);
		if (access(parent_dir, F_OK) != 0)
			test_mkdir(parent_dir, mode);

		if (access(dir, F_OK) != 0) {
			if (mkdir(dir, mode) != 0) {
				print_message("mkdir %s failed %d.\n",
					      dir, errno);
				return daos_errno2der(errno);
			}
		}
	}
	umask(stored_mode);

	return 0;
}

/* force == 1 to remove non-empty directory */
static inline int
test_rmdir(const char *path, bool force)
{
	DIR    *dir;
	struct dirent *ent;
	char   *fullpath = NULL;
	int    rc = 0, len;

	D_ASSERT(path != NULL);
	len = strlen(path);
	if (len == 0 || len > PATH_MAX)
		D_GOTO(out, rc = -DER_INVAL);

	if (!force) {
		rc = rmdir(path);
		if (rc != 0)
			rc = errno;
		D_GOTO(out, rc = daos_errno2der(rc));
	}

	dir = opendir(path);
	if (dir == NULL) {
		if (errno == ENOENT)
			D_GOTO(out, rc);
		D_ERROR("can't open directory %s, %d (%s)",
			path, errno, strerror(errno));
		D_GOTO(out, rc = daos_errno2der(errno));
	}

	while ((ent = readdir(dir)) != NULL) {
		if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, ".."))
			continue;   /* skips the dots */

		D_ASPRINTF(fullpath, "%s/%s", path, ent->d_name);
		if (fullpath == NULL)
			D_GOTO(out, -DER_NOMEM);

		switch (ent->d_type) {
		case DT_DIR:
			rc = test_rmdir(fullpath, force);
			if (rc != 0)
				D_ERROR("test_rmdir %s failed, rc %d",
						fullpath, rc);
			break;
		case DT_REG:
			rc = unlink(fullpath);
			if (rc != 0)
				D_ERROR("unlink %s failed, rc %d",
						fullpath, rc);
			break;
		default:
			D_WARN("find unexpected type %d", ent->d_type);
		}

		D_FREE(fullpath);
	}

	rc = closedir(dir);
	if (rc == 0) {
		rc = rmdir(path);
		if (rc != 0)
			rc = errno;
	}

out:
	D_FREE(fullpath);
	return rc;
}

#endif
