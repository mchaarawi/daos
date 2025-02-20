/**
 * (C) Copyright 2016-2018 Intel Corporation.
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
 * This file is part of daos
 *
 * tests/suite/daos_obj_array.c
 */

#include "daos_test.h"

#define STACK_BUF_LEN	24

static void
byte_array_simple_stack(void **state)
{
	test_arg_t	*arg = *state;
	daos_obj_id_t	 oid;
	daos_handle_t	 oh;
	d_iov_t	 dkey;
	d_sg_list_t	 sgl;
	d_iov_t	 sg_iov;
	daos_iod_t	 iod;
	daos_recx_t	 recx;
	char		 buf_out[STACK_BUF_LEN];
	char		 buf[STACK_BUF_LEN];
	int		 rc;

	dts_buf_render(buf, STACK_BUF_LEN);

	/** open object */
	oid = dts_oid_gen(OC_SX, 0, arg->myrank);
	rc = daos_obj_open(arg->coh, oid, 0, &oh, NULL);
	assert_int_equal(rc, 0);

	/** init dkey */
	d_iov_set(&dkey, "dkey", strlen("dkey"));

	/** init scatter/gather */
	d_iov_set(&sg_iov, buf, sizeof(buf));
	sgl.sg_nr		= 1;
	sgl.sg_nr_out		= 0;
	sgl.sg_iovs		= &sg_iov;

	/** init I/O descriptor */
	d_iov_set(&iod.iod_name, "akey", strlen("akey"));
	daos_csum_set(&iod.iod_kcsum, NULL, 0);
	iod.iod_nr	= 1;
	iod.iod_size	= 1;
	recx.rx_idx	= 0;
	recx.rx_nr	= sizeof(buf);
	iod.iod_recxs	= &recx;
	iod.iod_eprs	= NULL;
	iod.iod_csums	= NULL;
	iod.iod_type	= DAOS_IOD_ARRAY;

	/** update record */
	print_message("writing %d bytes in a single recx\n", STACK_BUF_LEN);
	rc = daos_obj_update(oh, DAOS_TX_NONE, &dkey, 1, &iod, &sgl, NULL);
	assert_int_equal(rc, 0);

	/** fetch record size & verify */
	print_message("fetching record size\n");
	iod.iod_size	= DAOS_REC_ANY;
	rc = daos_obj_fetch(oh, DAOS_TX_NONE, &dkey, 1, &iod, NULL, NULL, NULL);
	assert_int_equal(rc, 0);
	assert_int_equal(iod.iod_size, 1);

	/** fetch */
	print_message("reading data back ...\n");
	memset(buf_out, 0, sizeof(buf_out));
	d_iov_set(&sg_iov, buf_out, sizeof(buf_out));
	rc = daos_obj_fetch(oh, DAOS_TX_NONE, &dkey, 1, &iod, &sgl, NULL, NULL);
	assert_int_equal(rc, 0);
	/** Verify data consistency */
	print_message("validating data ...\n");
	assert_memory_equal(buf, buf_out, sizeof(buf));

	/** close object */
	rc = daos_obj_close(oh, NULL);
	assert_int_equal(rc, 0);
	print_message("all good\n");
}

static void
array_simple(void **state)
{
	test_arg_t	*arg = *state;
	daos_obj_id_t	 oid;
	daos_handle_t	 oh;
	d_iov_t	 dkey;
	d_sg_list_t	 sgl;
	d_iov_t	 sg_iov;
	daos_iod_t	 iod;
	daos_recx_t	 recx;
	char		*buf;
	char		*buf_out;
	int		 rc;

	D_ALLOC(buf, arg->size * arg->nr);
	assert_non_null(buf);

	dts_buf_render(buf, arg->size * arg->nr);

	/** open object */
	oid = dts_oid_gen(OC_SX, 0, arg->myrank);
	rc = daos_obj_open(arg->coh, oid, 0, &oh, NULL);
	assert_int_equal(rc, 0);

	/** init dkey */
	d_iov_set(&dkey, "dkey", strlen("dkey"));

	/** init scatter/gather */
	d_iov_set(&sg_iov, buf, arg->size * arg->nr);
	sgl.sg_nr		= 1;
	sgl.sg_nr_out		= 0;
	sgl.sg_iovs		= &sg_iov;

	/** init I/O descriptor */
	d_iov_set(&iod.iod_name, "akey", strlen("akey"));
	daos_csum_set(&iod.iod_kcsum, NULL, 0);
	iod.iod_nr	= 1;
	iod.iod_size	= arg->size;
	srand(time(NULL) + arg->size);
	recx.rx_idx	= rand();
	recx.rx_nr	= arg->nr;
	iod.iod_recxs	= &recx;
	iod.iod_eprs	= NULL;
	iod.iod_csums	= NULL;
	iod.iod_type	= DAOS_IOD_ARRAY;

	/** update record */
	print_message("writing %lu records of %lu bytes each at offset %lu\n",
		      recx.rx_nr, iod.iod_size, recx.rx_idx);
	rc = daos_obj_update(oh, DAOS_TX_NONE, &dkey, 1, &iod, &sgl, NULL);
	assert_int_equal(rc, 0);

	/** fetch data back */
	print_message("reading data back ...\n");
	D_ALLOC(buf_out, arg->size * arg->nr);
	assert_non_null(buf_out);
	memset(buf_out, 0, arg->size * arg->nr);
	d_iov_set(&sg_iov, buf_out, arg->size * arg->nr);
	iod.iod_size	= DAOS_REC_ANY;
	rc = daos_obj_fetch(oh, DAOS_TX_NONE, &dkey, 1, &iod, &sgl, NULL, NULL);
	assert_int_equal(rc, 0);
	/** verify record size */
	print_message("validating record size ...\n");
	assert_int_equal(iod.iod_size, arg->size);
	/** Verify data consistency */
	print_message("validating data ...\n");
	assert_memory_equal(buf, buf_out, arg->size * arg->nr);

	/** close object */
	rc = daos_obj_close(oh, NULL);
	assert_int_equal(rc, 0);

	D_FREE(buf_out);
	D_FREE(buf);
	print_message("all good\n");
}

#define NUM_RECORDS 24

static void
array_partial(void **state)
{
	test_arg_t	*arg = *state;
	daos_obj_id_t	 oid;
	daos_handle_t	 oh;
	d_iov_t	 dkey;
	d_sg_list_t	 sgl;
	d_iov_t	 sg_iov;
	daos_iod_t	 iod;
	daos_recx_t	 recx;
	daos_recx_t	 recxs[4];
	char		*buf;
	char		*buf_out;
	int		 rc, i;

	arg->size = 4;

	D_ALLOC(buf, arg->size * NUM_RECORDS);
	assert_non_null(buf);

	dts_buf_render(buf, arg->size * NUM_RECORDS);

	/** open object */
	oid = dts_oid_gen(OC_SX, 0, arg->myrank);
	rc = daos_obj_open(arg->coh, oid, 0, &oh, NULL);
	assert_int_equal(rc, 0);

	/** init dkey */
	d_iov_set(&dkey, "dkey", strlen("dkey"));

	/** init scatter/gather */
	d_iov_set(&sg_iov, buf, arg->size * NUM_RECORDS);
	sgl.sg_nr		= 1;
	sgl.sg_nr_out		= 0;
	sgl.sg_iovs		= &sg_iov;

	/** init I/O descriptor */
	d_iov_set(&iod.iod_name, "akey", strlen("akey"));
	daos_csum_set(&iod.iod_kcsum, NULL, 0);
	iod.iod_nr	= 1;
	iod.iod_size	= arg->size;
	recx.rx_idx	= 0;
	recx.rx_nr	= NUM_RECORDS;
	iod.iod_recxs	= &recx;
	iod.iod_eprs	= NULL;
	iod.iod_csums	= NULL;
	iod.iod_type	= DAOS_IOD_ARRAY;

	/** update record */
	print_message("writing %lu records of %lu bytes each at offset %lu\n",
		      recx.rx_nr, iod.iod_size, recx.rx_idx);
	rc = daos_obj_update(oh, DAOS_TX_NONE, &dkey, 1, &iod, &sgl, NULL);
	assert_int_equal(rc, 0);

	/** fetch 1/2 of the records back */
	print_message("reading 1/2 of the records back ...\n");
	D_ALLOC(buf_out, arg->size * NUM_RECORDS/2);
	assert_non_null(buf_out);
	memset(buf_out, 0, arg->size * NUM_RECORDS/2);
	d_iov_set(&sg_iov, buf_out, arg->size * NUM_RECORDS/2);
	iod.iod_size	= arg->size;
	iod.iod_nr	= 4;
	for (i = 0; i < 4; i++) {
		recxs[i].rx_idx	= i*6;
		recxs[i].rx_nr	= 3;
	}

	iod.iod_recxs = recxs;
	rc = daos_obj_fetch(oh, DAOS_TX_NONE, &dkey, 1, &iod, &sgl, NULL, NULL);
	print_message("fetch returns %d\n", rc);
	assert_int_equal(rc, 0);
	/** verify record size */
	print_message("validating record size ...\n");
	assert_int_equal(iod.iod_size, arg->size);
	/** Verify data consistency */
	print_message("validating data ...\n");

	for (i = 0; i < 4; i++) {
		char *tmp1 = buf + i * 6 * arg->size;
		char *tmp2 = buf_out + i * 3 * arg->size;

		assert_memory_equal(tmp1, tmp2, arg->size * 3);
	}

	/** close object */
	rc = daos_obj_close(oh, NULL);
	assert_int_equal(rc, 0);

	D_FREE(buf_out);
	D_FREE(buf);
	print_message("all good\n");
}

static int
set_size_uint8(void **state)
{
	test_arg_t	*arg = *state;

	arg->size = sizeof(uint8_t);
	arg->nr   = 131071;

	return 0;
}

static int
set_size_uint16(void **state)
{
	test_arg_t	*arg = *state;

	arg->size = sizeof(uint16_t);
	arg->nr   = 1 << 9;

	return 0;
}

static int
set_size_uint32(void **state)
{
	test_arg_t	*arg = *state;

	arg->size = sizeof(uint32_t);
	arg->nr   = 1 << 8;

	return 0;
}

static int
set_size_uint64(void **state)
{
	test_arg_t	*arg = *state;

	arg->size = sizeof(uint64_t);
	arg->nr   = 1 << 7;

	return 0;
}

static int
set_size_131071(void **state)
{
	test_arg_t	*arg = *state;

	arg->size = 131071;
	arg->nr   = 1 << 3;

	return 0;
}

static int
set_size_1mb(void **state)
{
	test_arg_t	*arg = *state;

	arg->size = 1 << 20;
	arg->nr   = 10;

	return 0;
}

static void
replicator(void **state)
{
	test_arg_t	*arg = *state;
	daos_obj_id_t	 oid;
	daos_handle_t	 oh;
	d_iov_t	 dkey;
	d_sg_list_t	 sgl;
	d_iov_t	 sg_iov;
	daos_iod_t	 iod;
	daos_recx_t	 recx;
	char		 buf_out[4608];
	char		 buf[192];
	int		 rc;

	dts_buf_render(buf, 192);

	/** open object */
	oid = dts_oid_gen(OC_SX, 0, arg->myrank);
	rc = daos_obj_open(arg->coh, oid, 0, &oh, NULL);
	assert_int_equal(rc, 0);

	/** init dkey */
	d_iov_set(&dkey, "dkey", strlen("dkey"));

	/** init scatter/gather */
	d_iov_set(&sg_iov, buf, sizeof(buf));
	sgl.sg_nr		= 1;
	sgl.sg_nr_out		= 0;
	sgl.sg_iovs		= &sg_iov;

	/** init I/O descriptor */
	d_iov_set(&iod.iod_name, "akey", strlen("akey"));
	daos_csum_set(&iod.iod_kcsum, NULL, 0);
	iod.iod_nr	= 1;
	iod.iod_size	= 1;
	recx.rx_idx	= 27136;
	recx.rx_nr	= sizeof(buf);
	iod.iod_recxs	= &recx;
	iod.iod_eprs	= NULL;
	iod.iod_csums	= NULL;
	iod.iod_type	= DAOS_IOD_ARRAY;

	/** update record */
	print_message("writing %d bytes in a single recx\n", 192);
	rc = daos_obj_update(oh, DAOS_TX_NONE, &dkey, 1, &iod, &sgl, NULL);
	assert_int_equal(rc, 0);

	recx.rx_idx     = 30208;
	iod.iod_recxs	= &recx;
	print_message("writing %d bytes in a single recx\n", 192);
	rc = daos_obj_update(oh, DAOS_TX_NONE, &dkey, 1, &iod, &sgl, NULL);
	assert_int_equal(rc, 0);

	recx.rx_idx     = 28672;
	iod.iod_recxs	= &recx;
	print_message("writing %d bytes in a single recx\n", 192);
	rc = daos_obj_update(oh, DAOS_TX_NONE, &dkey, 1, &iod, &sgl, NULL);
	assert_int_equal(rc, 0);

	/** fetch */
	print_message("reading data back ...\n");
	memset(buf_out, 0, sizeof(buf_out));
	d_iov_set(&sg_iov, buf_out, sizeof(buf_out));
	recx.rx_idx     = 27136;
	recx.rx_nr      = sizeof(buf_out);
	iod.iod_recxs	= &recx;
	rc = daos_obj_fetch(oh, DAOS_TX_NONE, &dkey, 1, &iod, &sgl, NULL, NULL);
	assert_int_equal(rc, 0);

	/** close object */
	rc = daos_obj_close(oh, NULL);
	assert_int_equal(rc, 0);
	print_message("all good\n");
}

static void
read_empty(void **state)
{
	test_arg_t	*arg = *state;
	daos_obj_id_t	 oid;
	daos_handle_t	 oh;
	d_iov_t	 dkey;
	d_sg_list_t	 sgl;
	d_iov_t	 sg_iov;
	daos_iod_t	 iod;
	daos_recx_t	 recx;
	char		 *buf;
	daos_size_t	 buf_len;
	int		 rc;

	buf_len = 4194304;
	D_ALLOC(buf, buf_len);
	D_ASSERT(buf != NULL);

	/** open object */
	oid = dts_oid_gen(OC_SX, 0, arg->myrank);
	rc = daos_obj_open(arg->coh, oid, 0, &oh, NULL);
	assert_int_equal(rc, 0);

	/** init dkey */
	d_iov_set(&dkey, "dkey", strlen("dkey"));

	/** init scatter/gather */
	d_iov_set(&sg_iov, buf, buf_len);
	sgl.sg_nr		= 1;
	sgl.sg_nr_out		= 0;
	sgl.sg_iovs		= &sg_iov;

	/** init I/O descriptor */
	d_iov_set(&iod.iod_name, "akey", strlen("akey"));
	daos_csum_set(&iod.iod_kcsum, NULL, 0);
	iod.iod_nr	= 1;
	iod.iod_size	= 1;
	recx.rx_idx	= 0;
	recx.rx_nr	= buf_len;
	iod.iod_recxs	= &recx;
	iod.iod_eprs	= NULL;
	iod.iod_csums	= NULL;
	iod.iod_type	= DAOS_IOD_ARRAY;

	/** fetch */
	print_message("reading empty object ...\n");
	rc = daos_obj_fetch(oh, DAOS_TX_NONE, &dkey, 1, &iod, &sgl, NULL, NULL);
	assert_int_equal(rc, 0);

	/** close object */
	rc = daos_obj_close(oh, NULL);
	assert_int_equal(rc, 0);
	print_message("all good\n");
	D_FREE(buf);
}

#define ENUM_DESC_BUF	512
#define ENUM_DESC_NR	5

enum {
	OBJ_DKEY,
	OBJ_AKEY
};

static void
enumerate_key(daos_handle_t oh, int *total_nr, daos_key_t *dkey, int key_type)
{
	char		*buf;
	daos_key_desc_t  kds[ENUM_DESC_NR];
	daos_anchor_t	 anchor = {0};
	d_sg_list_t	 sgl;
	d_iov_t		 sg_iov;
	int		 key_nr = 0;
	int		 rc;

	buf = malloc(ENUM_DESC_BUF);
	d_iov_set(&sg_iov, buf, ENUM_DESC_BUF);
	sgl.sg_nr		= 1;
	sgl.sg_nr_out		= 0;
	sgl.sg_iovs		= &sg_iov;

	while (!daos_anchor_is_eof(&anchor)) {
		uint32_t nr = ENUM_DESC_NR;

		memset(buf, 0, ENUM_DESC_BUF);
		if (key_type == OBJ_DKEY)
			rc = daos_obj_list_dkey(oh, DAOS_TX_NONE, &nr, kds,
						&sgl, &anchor, NULL);
		else
			rc = daos_obj_list_akey(oh, DAOS_TX_NONE, dkey, &nr,
						kds, &sgl, &anchor, NULL);
		assert_int_equal(rc, 0);
		if (nr == 0)
			continue;
		key_nr += nr;
	}

	*total_nr = key_nr;
}

#define SM_BUF_LEN 10

static void
array_dkey_punch_enumerate(void **state)
{
	test_arg_t	*arg = *state;
	daos_obj_id_t	 oid;
	daos_handle_t	 oh;
	d_iov_t	 dkey;
	d_sg_list_t	 sgl;
	d_iov_t	 sg_iov;
	daos_iod_t	 iod;
	daos_recx_t	 recx;
	char		 buf[SM_BUF_LEN];
	int		 total_nr;
	int		 i;
	int		 rc;

	dts_buf_render(buf, SM_BUF_LEN);

	/** open object */
	oid = dts_oid_gen(OC_SX, 0, arg->myrank);
	rc = daos_obj_open(arg->coh, oid, 0, &oh, NULL);
	assert_int_equal(rc, 0);

	/** init scatter/gather */
	d_iov_set(&sg_iov, buf, sizeof(buf));
	sgl.sg_nr		= 1;
	sgl.sg_nr_out		= 0;
	sgl.sg_iovs		= &sg_iov;

	/** init I/O descriptor */
	d_iov_set(&iod.iod_name, "akey", strlen("akey"));
	daos_csum_set(&iod.iod_kcsum, NULL, 0);
	iod.iod_nr	= 1;
	iod.iod_size	= 1;
	recx.rx_nr	= SM_BUF_LEN;
	recx.rx_idx	= 0;
	iod.iod_recxs	= &recx;
	iod.iod_eprs	= NULL;
	iod.iod_csums	= NULL;
	iod.iod_type	= DAOS_IOD_ARRAY;

	print_message("Inserting 100 dkeys...\n");
	for (i = 0; i < 100; i++) {
		char dkey_str[10];

		/** init dkey */
		sprintf(dkey_str, "dkey_%d", i);
		d_iov_set(&dkey, dkey_str, strlen(dkey_str));
		rc = daos_obj_update(oh, DAOS_TX_NONE, &dkey, 1, &iod, &sgl,
				     NULL);
		assert_int_equal(rc, 0);
	}

	total_nr = 0;
	print_message("Enumerating dkeys before punch...\n");
	enumerate_key(oh, &total_nr, NULL, OBJ_DKEY);
	print_message("DONE DKEY Enumeration (%d extents) -------\n", total_nr);
	assert_int_equal(total_nr, 100);

	/** punch first 10 records */
	print_message("Punching 10 dkeys...\n");
	for (i = 0; i < 10; i++) {
		char dkey_str[10];

		/** init dkey */
		sprintf(dkey_str, "dkey_%d", i);
		d_iov_set(&dkey, dkey_str, strlen(dkey_str));
		rc = daos_obj_punch_dkeys(oh, DAOS_TX_NONE, 1, &dkey, NULL);
		assert_int_equal(rc, 0);
	}

	total_nr = 0;
	print_message("Enumerating dkeys after punch...\n");
	enumerate_key(oh, &total_nr, NULL, OBJ_DKEY);
	print_message("DONE DKEY Enumeration (%d extents) -------\n", total_nr);
	assert_int_equal(total_nr, 90);

	/** close object */
	rc = daos_obj_close(oh, NULL);
	assert_int_equal(rc, 0);
	print_message("all good\n");
}

static void
array_akey_punch_enumerate(void **state)
{
	test_arg_t	*arg = *state;
	daos_obj_id_t	 oid;
	daos_handle_t	 oh;
	d_iov_t	 dkey;
	d_sg_list_t	 sgl;
	d_iov_t	 sg_iov;
	daos_iod_t	 iod;
	daos_recx_t	 recx;
	char		 buf[SM_BUF_LEN];
	int		 total_nr;
	int		 i;
	int		 rc;

	dts_buf_render(buf, SM_BUF_LEN);

	/** open object */
	oid = dts_oid_gen(OC_SX, 0, arg->myrank);
	rc = daos_obj_open(arg->coh, oid, 0, &oh, NULL);
	assert_int_equal(rc, 0);

	/** init scatter/gather */
	d_iov_set(&sg_iov, buf, sizeof(buf));
	sgl.sg_nr		= 1;
	sgl.sg_nr_out		= 0;
	sgl.sg_iovs		= &sg_iov;

	/** init dkey */
	d_iov_set(&dkey, "dkey", strlen("dkey"));

	/** init I/O descriptor */
	daos_csum_set(&iod.iod_kcsum, NULL, 0);
	iod.iod_nr	= 1;
	iod.iod_size	= 1;
	recx.rx_nr	= SM_BUF_LEN;
	recx.rx_idx	= 0;
	iod.iod_recxs	= &recx;
	iod.iod_eprs	= NULL;
	iod.iod_csums	= NULL;
	iod.iod_type	= DAOS_IOD_ARRAY;

	print_message("Inserting 100 akeys...\n");
	for (i = 0; i < 100; i++) {
		char akey_str[10];

		sprintf(akey_str, "akey_%d", i);
		d_iov_set(&iod.iod_name, akey_str, strlen(akey_str));
		rc = daos_obj_update(oh, DAOS_TX_NONE, &dkey, 1, &iod, &sgl,
				     NULL);
		assert_int_equal(rc, 0);
	}

	total_nr = 0;
	print_message("Enumerating akeys before punch...\n");
	enumerate_key(oh, &total_nr, &dkey, OBJ_AKEY);
	print_message("DONE AKEY Enumeration (%d extents) -------\n", total_nr);
	assert_int_equal(total_nr, 100);

	/** punch first 10 akeys */
	print_message("Punching 10 akeys...\n");
	for (i = 0; i < 10; i++) {
		char akey_str[10];
		daos_key_t akey;

		sprintf(akey_str, "akey_%d", i);
		d_iov_set(&akey, akey_str, strlen(akey_str));
		rc = daos_obj_punch_akeys(oh, DAOS_TX_NONE, &dkey, 1, &akey,
					  NULL);
		assert_int_equal(rc, 0);
	}

	total_nr = 0;
	print_message("Enumerating akeys after punch...\n");
	enumerate_key(oh, &total_nr, &dkey, OBJ_AKEY);
	print_message("DONE AKEY Enumeration (%d extents) -------\n", total_nr);
	assert_int_equal(total_nr, 90);

	print_message("Fetch akeys after punch and verify size...\n");
	for (i = 0; i < 100; i++) {
		char akey_str[10];

		sprintf(akey_str, "akey_%d", i);
		d_iov_set(&iod.iod_name, akey_str, strlen(akey_str));

		iod.iod_size = DAOS_REC_ANY;
		rc = daos_obj_fetch(oh, DAOS_TX_NONE, &dkey, 1, &iod,
				    NULL, NULL, NULL);
		assert_int_equal(rc, 0);
		if (i < 10)
			assert_int_equal(iod.iod_size, 0);
		else
			assert_int_equal(iod.iod_size, 1);
	}

	/** close object */
	rc = daos_obj_close(oh, NULL);
	assert_int_equal(rc, 0);
	print_message("all good\n");
}

static void
array_recx_punch_enumerate(void **state)
{
	test_arg_t	*arg = *state;
	daos_obj_id_t	 oid;
	daos_handle_t	 oh;
	d_iov_t	 dkey;
	d_sg_list_t	 sgl;
	d_iov_t	 sg_iov;
	daos_iod_t	 iod;
	daos_recx_t	 recx;
	char		 buf[SM_BUF_LEN];
	daos_anchor_t	 anchor;
	int		 total_nr = 0;
	int		 i;
	int		 rc;

	dts_buf_render(buf, SM_BUF_LEN);

	/** open object */
	oid = dts_oid_gen(OC_SX, 0, arg->myrank);
	rc = daos_obj_open(arg->coh, oid, 0, &oh, NULL);
	assert_int_equal(rc, 0);

	/** init dkey */
	d_iov_set(&dkey, "dkey", strlen("dkey"));

	/** init scatter/gather */
	d_iov_set(&sg_iov, buf, sizeof(buf));
	sgl.sg_nr		= 1;
	sgl.sg_nr_out		= 0;
	sgl.sg_iovs		= &sg_iov;

	/** init I/O descriptor */
	d_iov_set(&iod.iod_name, "akey", strlen("akey"));
	daos_csum_set(&iod.iod_kcsum, NULL, 0);
	iod.iod_nr	= 1;
	iod.iod_size	= 1;
	recx.rx_nr	= SM_BUF_LEN;
	iod.iod_recxs	= &recx;
	iod.iod_eprs	= NULL;
	iod.iod_csums	= NULL;
	iod.iod_type	= DAOS_IOD_ARRAY;

	/** insert 100 extents */
	for (i = 0; i < 100; i++) {
		recx.rx_idx = i * SM_BUF_LEN;
		rc = daos_obj_update(oh, DAOS_TX_NONE, &dkey, 1, &iod, &sgl,
				     NULL);
		assert_int_equal(rc, 0);
	}

	/** enumerate extents before punch */
	print_message("Enumerating extents before punch...\n");
	memset(&anchor, 0, sizeof(anchor));
	while (!daos_anchor_is_eof(&anchor)) {
		daos_size_t size = 0;
		uint32_t nr = 5;
		daos_recx_t recxs[5];
		daos_epoch_range_t eprs[5];

		rc = daos_obj_list_recx(oh, DAOS_TX_NONE, &dkey, &iod.iod_name,
					&size, &nr, recxs, eprs, &anchor, true,
					NULL);
		assert_int_equal(rc, 0);
		total_nr += nr;
	}
	print_message("DONE recx Enumeration (%d extents) -------\n", total_nr);
	assert_int_equal(total_nr, 100);

	/** punch first 10 records */
	iod.iod_size	= 0;
	recx.rx_nr	= SM_BUF_LEN;
	iod.iod_recxs	= &recx;
	for (i = 0; i < 10; i++) {
		recx.rx_idx = i * SM_BUF_LEN;
		print_message("punching idx: %"PRIu64" len %"PRIu64"\n",
			      recx.rx_idx, recx.rx_nr);
		rc = daos_obj_update(oh, DAOS_TX_NONE, &dkey, 1, &iod, NULL,
				     NULL);
		assert_int_equal(rc, 0);
	}

	/** enumerate records again */
	print_message("Enumerating extents after punch...\n");
	memset(&anchor, 0, sizeof(anchor));
	total_nr = 0;
	while (!daos_anchor_is_eof(&anchor)) {
		daos_size_t size = 0;
		uint32_t nr = 5;
		daos_recx_t recxs[5];
		daos_epoch_range_t eprs[5];

		rc = daos_obj_list_recx(oh, DAOS_TX_NONE, &dkey, &iod.iod_name,
					&size, &nr, recxs, eprs, &anchor, true,
					NULL);
		assert_int_equal(rc, 0);
		total_nr += nr;
	}
	print_message("DONE recx Enumeration (%d extents) -------\n", total_nr);
	assert_int_equal(total_nr, 90);

	/** close object */
	rc = daos_obj_close(oh, NULL);
	assert_int_equal(rc, 0);
	print_message("all good\n");
}

static const struct CMUnitTest array_tests[] = {
	{ "ARRAY1: byte array with buffer on stack",
	  byte_array_simple_stack, NULL, test_case_teardown},
	{ "ARRAY2: array of uint8_t",
	  array_simple, set_size_uint8, test_case_teardown},
	{ "ARRAY3: array of uint16_t",
	  array_simple, set_size_uint16, test_case_teardown},
	{ "ARRAY4: array of uint32_t",
	  array_simple, set_size_uint32, test_case_teardown},
	{ "ARRAY5: array of uint64_t",
	  array_simple, set_size_uint64, test_case_teardown},
	{ "ARRAY6: array of 131071-byte records",
	  array_simple, set_size_131071, test_case_teardown},
	{ "ARRAY7: array of 1MB records",
	  array_simple, set_size_1mb, test_case_teardown},
	{ "ARRAY8: partial I/O on array",
	  array_partial, NULL, test_case_teardown},
	{ "ARRAY9: segfault replicator",
	  replicator, NULL, test_case_teardown},
	{ "ARRAY10: read from empty object",
	  read_empty, NULL, test_case_teardown},
	{ "ARRAY11: Array DKEY punch/enumerate",
	  array_dkey_punch_enumerate, NULL, test_case_teardown},
	{ "ARRAY12: Array AKEY punch/enumerate",
	  array_akey_punch_enumerate, NULL, test_case_teardown},
	{ "ARRAY13: Array RECX punch/enumerate",
	  array_recx_punch_enumerate, NULL, test_case_teardown},
};

static int
obj_array_setup(void **state)
{
	return test_setup(state, SETUP_CONT_CONNECT, false, DEFAULT_POOL_SIZE,
			  NULL);
}

int
run_daos_obj_array_test(int rank, int size)
{
	int rc = 0;

	if (rank == 0)
		rc = cmocka_run_group_tests_name("DAOS OBJ Array tests",
						 array_tests, obj_array_setup,
						 test_teardown);
	MPI_Barrier(MPI_COMM_WORLD);
	return rc;
}
