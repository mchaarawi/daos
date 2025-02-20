/**
 * (C) Copyright 2016 Intel Corporation.
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
 * common/fail_loc.c to inject failure scenario
 */
#define D_LOGFAC	DD_FAC(common)

#include <daos/common.h>
#include <gurt/fault_inject.h>

uint64_t daos_fail_loc;
uint64_t daos_fail_value;
uint64_t daos_fail_num;

void
daos_fail_loc_reset()
{
	daos_fail_loc_set(0);
	D_DEBUG(DB_ANY, "*** fail_loc="DF_X64"\n", daos_fail_loc);
}

int
daos_fail_check(uint64_t fail_loc)
{
	struct d_fault_attr_t	*attr;
	int			grp;

	if (daos_fail_loc == 0)
		return 0;

	if ((daos_fail_loc & DAOS_FAIL_MASK_LOC) !=
		(fail_loc & DAOS_FAIL_MASK_LOC))
		return 0;

	/**
	 * TODO reset fail_loc to save some cycles once the
	 * current fail_loc finish the job.
	 */
	grp = DAOS_FAIL_GROUP_GET(fail_loc);
	attr = d_fault_attr_lookup(grp);
	if (attr == NULL) {
		D_DEBUG(DB_ANY, "No attr fail_loc="DF_X64" value="DF_U64
			", input_loc ="DF_X64" idx %d\n", daos_fail_loc,
			daos_fail_value, fail_loc, grp);
		return 0;
	}

	if (d_should_fail(attr)) {
		D_DEBUG(DB_ANY, "*** fail_loc="DF_X64" value="DF_U64
			", input_loc ="DF_X64" idx %d\n", daos_fail_loc,
			daos_fail_value, fail_loc, grp);
		return 1;
	}

	return 0;
}

void
daos_fail_loc_set(uint64_t fail_loc)
{
	struct d_fault_attr_t attr_in = { 0 };

	/* If fail_loc is 0, let's assume it will reset unit test fail loc */
	if (fail_loc == 0)
		attr_in.fa_id = DAOS_FAIL_UNIT_TEST_GROUP;
	else
		attr_in.fa_id = DAOS_FAIL_GROUP_GET(fail_loc);

	D_ASSERT(attr_in.fa_id > 0);
	D_ASSERT(attr_in.fa_id < DAOS_FAIL_MAX_GROUP);

	attr_in.fa_probability_x = 1;
	attr_in.fa_probability_y = 1;
	if (fail_loc & DAOS_FAIL_ONCE) {
		attr_in.fa_max_faults = 1;
	} else if (fail_loc & DAOS_FAIL_SOME) {
		D_ASSERT(daos_fail_num > 0);
		attr_in.fa_max_faults = daos_fail_num;
	} else if (fail_loc & DAOS_FAIL_ALWAYS) {
		attr_in.fa_max_faults = 0;
	}

	d_fault_attr_set(attr_in.fa_id, attr_in);
	daos_fail_loc = fail_loc;
	D_DEBUG(DB_ANY, "*** fail_loc="DF_X64"\n", daos_fail_loc);
}

void
daos_fail_num_set(uint64_t value)
{
	daos_fail_num = value;
}

void
daos_fail_value_set(uint64_t value)
{
	daos_fail_value = value;
}

uint64_t
daos_fail_value_get(void)
{
	return daos_fail_value;
}

int
daos_fail_init(void)
{
	struct d_fault_attr_t	attr = { 0 };
	int rc;

	rc = d_fault_inject_init();
	if (rc)
		return rc;

	rc = d_fault_attr_set(DAOS_FAIL_UNIT_TEST_GROUP, attr);
	if (rc)
		d_fault_inject_fini();

	return rc;
}

void
daos_fail_fini()
{
	d_fault_inject_fini();
}
