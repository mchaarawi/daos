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
 * This file is part of daos
 *
 * vos/vos_obj.c
 */
#define D_LOGFAC	DD_FAC(vos)

#include <daos/common.h>
#include <daos/btree.h>
#include <daos_types.h>
#include <daos_srv/vos.h>
#include <vos_internal.h>

/** Ensure the values of recx flags map to those exported by evtree */
D_CASSERT((uint32_t)VOS_RECX_FLAG_UNKNOWN == (uint32_t)EVT_UNKNOWN);
D_CASSERT((uint32_t)VOS_RECX_FLAG_COVERED == (uint32_t)EVT_COVERED);
D_CASSERT((uint32_t)VOS_RECX_FLAG_VISIBLE == (uint32_t)EVT_VISIBLE);
D_CASSERT((uint32_t)VOS_RECX_FLAG_PARTIAL == (uint32_t)EVT_PARTIAL);
D_CASSERT((uint32_t)VOS_RECX_FLAG_LAST == (uint32_t)EVT_LAST);

/**
 * @} vos_tree_helper
 */

static int
key_punch(struct vos_object *obj, daos_epoch_t epoch, uint32_t pm_ver,
	  daos_key_t *dkey, unsigned int akey_nr, daos_key_t *akeys,
	  uint32_t flags)
{
	struct vos_key_bundle	kbund;
	struct vos_rec_bundle	rbund;
	daos_csum_buf_t		csum;
	d_iov_t		kiov;
	d_iov_t		riov;
	int			rc;

	rc = obj_tree_init(obj);
	if (rc)
		D_GOTO(out, rc);

	tree_key_bundle2iov(&kbund, &kiov);
	kbund.kb_epoch = epoch;

	tree_rec_bundle2iov(&rbund, &riov);
	rbund.rb_off	= UMOFF_NULL;
	rbund.rb_ver	= pm_ver;
	rbund.rb_csum	= &csum;
	memset(&csum, 0, sizeof(csum));

	if (!akeys) {
		kbund.kb_key = dkey;
		rbund.rb_iov = dkey;
		rbund.rb_tclass	= VOS_BTR_DKEY;

		rc = key_tree_punch(obj, obj->obj_toh, &kiov, &riov, flags);
		if (rc != 0)
			D_GOTO(out, rc);

	} else {
		daos_handle_t	toh;
		int		i;

		rc = key_tree_prepare(obj, epoch, obj->obj_toh, VOS_BTR_DKEY,
				      dkey, SUBTR_CREATE, DAOS_INTENT_PUNCH,
				      NULL, &toh);
		if (rc) {
			/* If the key is punched, then do nothing */
			if (rc == -DER_NONEXIST)
				rc = 0;
			D_GOTO(out, rc);
		}
		rbund.rb_tclass	= VOS_BTR_AKEY;
		for (i = 0; i < akey_nr; i++) {
			kbund.kb_key = &akeys[i];
			rbund.rb_iov = &akeys[i];
			rc = key_tree_punch(obj, toh, &kiov, &riov, flags);
			if (rc != 0)
				break;
		}
		key_tree_release(toh, 0);
	}
 out:
	return rc;
}

static int
obj_punch(daos_handle_t coh, struct vos_object *obj, daos_epoch_t epoch,
	  uint32_t flags)
{
	struct vos_container	*cont;
	int			 rc;

	cont = vos_hdl2cont(coh);
	rc = vos_oi_punch(cont, obj->obj_id, epoch, flags, obj->obj_df);
	if (rc)
		D_GOTO(failed, rc);

	/* evict it from catch, because future fetch should only see empty
	 * object (without obj_df)
	 */
	vos_obj_evict(obj);
failed:
	return rc;
}

/**
 * Punch an object, or punch a dkey, or punch an array of akeys.
 */
int
vos_obj_punch(daos_handle_t coh, daos_unit_oid_t oid, daos_epoch_t epoch,
	      uint32_t pm_ver, uint32_t flags, daos_key_t *dkey,
	      unsigned int akey_nr, daos_key_t *akeys, struct dtx_handle *dth)
{
	struct vos_container	*cont;
	struct vos_object	*obj = NULL;
	int			 rc = 0;

	D_DEBUG(DB_IO, "Punch "DF_UOID", epoch "DF_U64"\n",
		DP_UOID(oid), epoch);

	vos_dth_set(dth);
	cont = vos_hdl2cont(coh);

	rc = vos_tx_begin(cont->vc_pool);
	if (rc != 0)
		goto reset;

	/* Commit the CoS DTXs via the PUNCH PMDK transaction. */
	if (dth != NULL && dth->dth_dti_cos_count > 0 &&
	    dth->dth_dti_cos_done == 0) {
		vos_dtx_commit_internal(cont, dth->dth_dti_cos,
					dth->dth_dti_cos_count);
		dth->dth_dti_cos_done = 1;
	}

	/* NB: punch always generate a new incarnation of the object */
	rc = vos_obj_hold(vos_obj_cache_current(), vos_hdl2cont(coh), oid,
			  epoch, false, DAOS_INTENT_PUNCH, &obj);
	if (rc == 0) {
		if (dkey) /* key punch */
			rc = key_punch(obj, epoch, pm_ver, dkey,
				       akey_nr, akeys, flags);
		else /* object punch */
			rc = obj_punch(coh, obj, epoch, flags);
	}

	if (dth != NULL && rc == 0)
		rc = vos_dtx_prepared(dth);

	rc = vos_tx_end(cont->vc_pool, rc);
	if (obj != NULL)
		vos_obj_release(vos_obj_cache_current(), obj);

reset:
	vos_dth_set(NULL);
	if (rc != 0)
		D_DEBUG(DB_IO, "Failed to punch object "DF_UOID": rc = %d\n",
			DP_UOID(oid), rc);

	return rc;
}

/**
 * @defgroup vos_obj_iters VOS object iterators
 * @{
 *
 * - iterate d-key
 * - iterate a-key (array)
 * - iterate recx
 */
static int
key_iter_fetch_helper(struct vos_obj_iter *oiter, struct vos_key_bundle *kbund,
		      struct vos_rec_bundle *rbund, d_iov_t *keybuf,
		      daos_anchor_t *anchor)
{
	d_iov_t		kiov;
	d_iov_t		kbund_kiov;
	d_iov_t		riov;
	daos_csum_buf_t		csum;

	tree_key_bundle2iov(kbund, &kiov);
	tree_rec_bundle2iov(rbund, &riov);
	kbund->kb_key = &kbund_kiov;

	rbund->rb_iov	= keybuf;
	rbund->rb_csum	= &csum;

	d_iov_set(rbund->rb_iov, NULL, 0); /* no copy */
	daos_csum_set(rbund->rb_csum, NULL, 0);

	return dbtree_iter_fetch(oiter->it_hdl, &kiov, &riov, anchor);
}

static int
key_iter_fetch(struct vos_obj_iter *oiter, vos_iter_entry_t *ent,
	       daos_anchor_t *anchor)
{
	struct vos_key_bundle	kbund;
	struct vos_rec_bundle	rbund;
	int			rc;

	rc = key_iter_fetch_helper(oiter, &kbund, &rbund, &ent->ie_key, anchor);

	if (rc == 0) {
		D_ASSERT(rbund.rb_krec);
		if (rbund.rb_krec->kr_bmap & KREC_BF_PUNCHED)
			ent->ie_epoch = rbund.rb_krec->kr_latest;
		else
			ent->ie_epoch = DAOS_EPOCH_MAX;
		ent->ie_earliest = rbund.rb_krec->kr_earliest;
		if (oiter->it_iter.it_type == VOS_ITER_AKEY) {
			if (rbund.rb_krec->kr_bmap & KREC_BF_EVT) {
				ent->ie_child_type = VOS_ITER_RECX;
			} else {
				D_ASSERT(rbund.rb_krec->kr_bmap & KREC_BF_BTR);
				ent->ie_child_type = VOS_ITER_SINGLE;
			}
		} else {
			ent->ie_child_type = VOS_ITER_AKEY;
		}
	}
	return rc;
}

static int
key_iter_fetch_root(struct vos_obj_iter *oiter, vos_iter_type_t type,
		    struct vos_iter_info *info)
{
	struct vos_object	*obj = oiter->it_obj;
	struct vos_krec_df	*krec;
	struct vos_key_bundle	 kbund;
	struct vos_rec_bundle	 rbund;
	d_iov_t		 keybuf;
	int			 rc;

	rc = key_iter_fetch_helper(oiter, &kbund, &rbund, &keybuf, NULL);

	if (rc != 0) {
		D_DEBUG(DB_TRACE, "Could not fetch key: rc = %d\n", rc);
		return rc;
	}

	krec = rbund.rb_krec;
	info->ii_vea_info = obj->obj_cont->vc_pool->vp_vea_info;
	info->ii_uma = vos_obj2uma(obj);

	/* Limit the nested iterator to the bounds of this key */
	info->ii_epr.epr_lo = MAX(oiter->it_epr.epr_lo, krec->kr_earliest);
	info->ii_epr.epr_hi = MIN(oiter->it_epr.epr_hi, krec->kr_latest);

	if (type == VOS_ITER_RECX) {
		if ((krec->kr_bmap & KREC_BF_EVT) == 0)
			return -DER_NONEXIST;
		info->ii_evt = &krec->kr_evt;
	} else {
		if ((krec->kr_bmap & KREC_BF_BTR) == 0)
			return -DER_NONEXIST;
		info->ii_btr = &krec->kr_btr;
	}

	return 0;
}

static int
key_iter_copy(struct vos_obj_iter *oiter, vos_iter_entry_t *ent,
	      d_iov_t *iov_out)
{
	if (ent->ie_key.iov_len > iov_out->iov_buf_len)
		return -DER_OVERFLOW;

	D_ASSERT(ent->ie_key.iov_buf != NULL);
	D_ASSERT(iov_out->iov_buf != NULL);

	memcpy(iov_out->iov_buf, ent->ie_key.iov_buf, ent->ie_key.iov_len);
	iov_out->iov_len = ent->ie_key.iov_len;
	return 0;
}

/**
 * Check if the current entry can match the iterator condition, this function
 * retuns IT_OPC_NOOP for true, returns IT_OPC_NEXT or IT_OPC_PROBE if further
 * operation is required. If IT_OPC_PROBE is returned, then the key to be
 * probed and its epoch range are also returned to @ent.
 */
static int
key_iter_match(struct vos_obj_iter *oiter, vos_iter_entry_t *ent, int *probe_p)
{
	struct vos_object	*obj = oiter->it_obj;
	daos_epoch_range_t	*epr = &oiter->it_epr;
	struct vos_key_bundle	 kbund;
	struct vos_rec_bundle	 rbund;
	daos_handle_t		 toh;
	d_iov_t		 kiov;
	d_iov_t		 riov;
	int			 probe;
	int			 rc;

	rc = key_iter_fetch(oiter, ent, NULL);
	if (rc) {
		if (rc == -DER_INPROGRESS)
			D_DEBUG(DB_TRACE, "Cannot fetch the entry because of "
				"conflict modification: %d\n", rc);
		else
			D_ERROR("Failed to fetch the entry: %d\n", rc);
		return rc;
	}

	probe = 0;
	if (ent->ie_earliest > epr->epr_hi) {
		/* The key was created after our range.
		 * GT + EPOCH_MAX will probe next key.
		 */
		ent->ie_epoch = DAOS_EPOCH_MAX;
		probe = BTR_PROBE_GT;
	} else if (ent->ie_epoch <= epr->epr_lo) {
		/* Key is punched.   Probe to next match */
		probe = BTR_PROBE_GT;
		ent->ie_epoch = epr->epr_lo;
	}

	if (probe != 0) {
		/* not in the range, need further operation */
		*probe_p = probe;
		return IT_OPC_PROBE;
	}

	if ((oiter->it_iter.it_type == VOS_ITER_AKEY) ||
	    (oiter->it_akey.iov_buf == NULL)) /* dkey w/o akey as condition */
		return IT_OPC_NOOP;

	/* else: has akey as condition */
	if (epr->epr_lo != epr->epr_hi) {
		D_ERROR("Cannot support epoch range for conditional iteration "
			"because it is not clearly defined.\n");
		return -DER_INVAL; /* XXX simplify it for now */
	}

	rc = key_tree_prepare(obj, ent->ie_epoch, obj->obj_toh, VOS_BTR_DKEY,
			      &ent->ie_key, 0, vos_iter_intent(&oiter->it_iter),
			      NULL, &toh);
	if (rc != 0) {
		D_DEBUG(DB_IO, "can't load the akey tree: %d\n", rc);
		return rc;
	}

	/* check if the akey exists */
	tree_rec_bundle2iov(&rbund, &riov);
	tree_key_bundle2iov(&kbund, &kiov);
	kbund.kb_key = &oiter->it_akey;
	kbund.kb_epoch = epr->epr_lo;

	rc = dbtree_fetch(toh, BTR_PROBE_GT | BTR_PROBE_MATCHED,
			  vos_iter_intent(&oiter->it_iter), &kiov, NULL, &riov);
	key_tree_release(toh, false);
	if (rc == 0)
		return IT_OPC_NOOP; /* match the condition (akey), done */

	if (rc == -DER_NONEXIST)
		return IT_OPC_NEXT; /* no matched akey */

	return rc;
}

/**
 * Check if the current item can match the provided condition (with the
 * giving a-key). If the item can't match the condition, this function
 * traverses the tree until a matched item is found.
 */
static int
key_iter_match_probe(struct vos_obj_iter *oiter)
{
	int	rc;

	while (1) {
		vos_iter_entry_t	entry;
		struct vos_key_bundle	kbund;
		d_iov_t		kiov;
		int			opc = 0;

		rc = key_iter_match(oiter, &entry, &opc);
		switch (rc) {
		default:
			D_ASSERT(rc < 0);
			D_ERROR("match failed, rc=%d\n", rc);
			goto out;

		case IT_OPC_NOOP:
			/* already match the condition, no further operation */
			rc = 0;
			goto out;

		case IT_OPC_PROBE:
			/* probe the returned key and epoch range */
			tree_key_bundle2iov(&kbund, &kiov);
			kbund.kb_key	= &entry.ie_key;
			kbund.kb_epoch	= entry.ie_epoch;
			rc = dbtree_iter_probe(oiter->it_hdl, opc,
					       vos_iter_intent(&oiter->it_iter),
					       &kiov, NULL);
			if (rc)
				goto out;
			break;

		case IT_OPC_NEXT:
			/* move to the next tree record */
			rc = dbtree_iter_next_with_intent(oiter->it_hdl,
					vos_iter_intent(&oiter->it_iter));
			if (rc)
				goto out;
			break;
		}
	}
 out:
	return rc;
}

static int
key_iter_probe(struct vos_obj_iter *oiter, daos_anchor_t *anchor)
{
	int	rc;

	rc = dbtree_iter_probe(oiter->it_hdl,
			       anchor ? BTR_PROBE_GE : BTR_PROBE_FIRST,
			       vos_iter_intent(&oiter->it_iter),
			       NULL, anchor);
	if (rc)
		D_GOTO(out, rc);

	rc = key_iter_match_probe(oiter);
	if (rc)
		D_GOTO(out, rc);
 out:
	return rc;
}

static int
key_iter_next(struct vos_obj_iter *oiter)
{
	int	rc;

	rc = dbtree_iter_next_with_intent(oiter->it_hdl,
					  vos_iter_intent(&oiter->it_iter));
	if (rc)
		D_GOTO(out, rc);

	rc = key_iter_match_probe(oiter);
	if (rc)
		D_GOTO(out, rc);
out:
	return rc;
}

/**
 * Iterator for the d-key tree.
 */
static int
dkey_iter_prepare(struct vos_obj_iter *oiter, daos_key_t *akey)
{
	/* optional condition, d-keys with the provided attribute (a-key) */
	oiter->it_akey = *akey;

	return dbtree_iter_prepare(oiter->it_obj->obj_toh, 0, &oiter->it_hdl);
}

/**
 * Iterator for the akey tree.
 */
static int
akey_iter_prepare(struct vos_obj_iter *oiter, daos_key_t *dkey)
{
	struct vos_object	*obj = oiter->it_obj;
	daos_handle_t		 toh;
	int			 rc;

	rc = key_tree_prepare(obj, oiter->it_epr.epr_lo, obj->obj_toh,
			      VOS_BTR_DKEY, dkey, 0,
			      vos_iter_intent(&oiter->it_iter), NULL, &toh);
	if (rc != 0) {
		if (rc == -DER_INPROGRESS)
			D_DEBUG(DB_TRACE, "Cannot load the akey tree because "
				"of some conflict modification: %d\n", rc);
		else
			D_ERROR("Cannot load the akey tree: %d\n", rc);
		return rc;
	}

	/* see BTR_ITER_EMBEDDED for the details */
	rc = dbtree_iter_prepare(toh, BTR_ITER_EMBEDDED, &oiter->it_hdl);
	if (rc)
		D_GOTO(out, rc);

	key_tree_release(toh, false);
out:
	return rc;
}

/**
 * Record extent (recx) iterator
 */

/**
 * Record extent (recx) iterator
 */
static int singv_iter_fetch(struct vos_obj_iter *oiter,
			   vos_iter_entry_t *it_entry,
			   daos_anchor_t *anchor);
/**
 * Prepare the iterator for the recx tree.
 */
static int
singv_iter_prepare(struct vos_obj_iter *oiter, daos_key_t *dkey,
		   daos_key_t *akey)
{
	struct vos_object	*obj = oiter->it_obj;
	daos_handle_t		 dk_toh;
	daos_handle_t		 ak_toh;
	int			 rc;

	rc = key_tree_prepare(obj, oiter->it_epr.epr_hi, obj->obj_toh,
			      VOS_BTR_DKEY, dkey, 0,
			      vos_iter_intent(&oiter->it_iter), NULL, &dk_toh);
	if (rc != 0)
		D_GOTO(failed_0, rc);

	rc = key_tree_prepare(obj, oiter->it_epr.epr_hi, dk_toh,
			      VOS_BTR_AKEY, akey, 0,
			      vos_iter_intent(&oiter->it_iter), NULL, &ak_toh);
	if (rc != 0)
		D_GOTO(failed_1, rc);

	/* see BTR_ITER_EMBEDDED for the details */
	rc = dbtree_iter_prepare(ak_toh, BTR_ITER_EMBEDDED, &oiter->it_hdl);
	if (rc != 0) {
		D_DEBUG(DB_IO, "Cannot prepare singv iterator: %d\n", rc);
		D_GOTO(failed_2, rc);
	}
 failed_2:
	key_tree_release(ak_toh, false);
 failed_1:
	key_tree_release(dk_toh, false);
 failed_0:
	return rc;
}

/**
 * Probe the single value based on @opc and conditions in @entry (epoch),
 * return the matched one to @entry.
 */
static int
singv_iter_probe_fetch(struct vos_obj_iter *oiter, dbtree_probe_opc_t opc,
		       vos_iter_entry_t *entry)
{
	struct vos_key_bundle	kbund;
	d_iov_t		kiov;
	int			rc;

	tree_key_bundle2iov(&kbund, &kiov);
	kbund.kb_epoch = entry->ie_epoch;

	rc = dbtree_iter_probe(oiter->it_hdl, opc,
			       vos_iter_intent(&oiter->it_iter), &kiov, NULL);
	if (rc != 0)
		return rc;

	memset(entry, 0, sizeof(*entry));
	rc = singv_iter_fetch(oiter, entry, NULL);
	return rc;
}

/**
 * Find the data that was written before/in the specified epoch of @oiter
 * for the recx in @entry. If this recx has no data for this epoch, then
 * this function will move on to the next recx and repeat this process.
 */
static int
singv_iter_probe_epr(struct vos_obj_iter *oiter, vos_iter_entry_t *entry)
{
	daos_epoch_range_t *epr = &oiter->it_epr;

	while (1) {
		int	opc;
		int	rc;

		switch (oiter->it_epc_expr) {
		default:
			return -DER_INVAL;

		case VOS_IT_EPC_EQ:
			if (entry->ie_epoch > epr->epr_hi)
				return -DER_NONEXIST;

			if (entry->ie_epoch < epr->epr_lo) {
				entry->ie_epoch = epr->epr_lo;
				opc = BTR_PROBE_EQ;
				break;
			}
			return 0;

		case VOS_IT_EPC_RE:
			if (entry->ie_epoch > epr->epr_hi)
				return -DER_NONEXIST;

			if (entry->ie_epoch < epr->epr_lo) {
				entry->ie_epoch = epr->epr_lo;
				opc = BTR_PROBE_GE;
				break;
			}
			return 0;

		case VOS_IT_EPC_RR:
			if (entry->ie_epoch < epr->epr_lo)
				return -DER_NONEXIST; /* end of story */

			if (entry->ie_epoch > epr->epr_hi) {
				entry->ie_epoch = epr->epr_hi;
				opc = BTR_PROBE_LE;
				break;
			}
			return 0;

		case VOS_IT_EPC_GE:
			if (entry->ie_epoch < epr->epr_lo) {
				entry->ie_epoch = epr->epr_lo;
				opc = BTR_PROBE_GE;
				break;
			}
			return 0;

		case VOS_IT_EPC_LE:
			if (entry->ie_epoch > epr->epr_lo) {
				entry->ie_epoch = epr->epr_lo;
				opc = BTR_PROBE_LE;
				break;
			}
			return 0;
		}
		rc = singv_iter_probe_fetch(oiter, opc, entry);
		if (rc != 0)
			return rc;
	}
}

static int
singv_iter_probe(struct vos_obj_iter *oiter, daos_anchor_t *anchor)
{
	vos_iter_entry_t	entry;
	daos_anchor_t		tmp;
	int			opc;
	int			rc;

	if (oiter->it_epc_expr == VOS_IT_EPC_RR)
		opc = anchor == NULL ? BTR_PROBE_LAST : BTR_PROBE_LE;
	else
		opc = anchor == NULL ? BTR_PROBE_FIRST : BTR_PROBE_GE;

	rc = dbtree_iter_probe(oiter->it_hdl, opc,
			       vos_iter_intent(&oiter->it_iter), NULL, anchor);
	if (rc != 0)
		return rc;

	memset(&entry, 0, sizeof(entry));
	rc = singv_iter_fetch(oiter, &entry, &tmp);
	if (rc != 0)
		return rc;

	if (anchor != NULL) {
		if (memcmp(anchor, &tmp, sizeof(tmp)) == 0)
			return 0;

		D_DEBUG(DB_IO, "Can't find the provided anchor\n");
		/**
		 * the original recx has been merged/discarded, so we need to
		 * call singv_iter_probe_epr() and check if the current record
		 * can match the condition.
		 */
	}
	rc = singv_iter_probe_epr(oiter, &entry);
	return rc;
}

static int
singv_iter_fetch(struct vos_obj_iter *oiter, vos_iter_entry_t *it_entry,
		 daos_anchor_t *anchor)
{
	struct vos_key_bundle	kbund;
	struct vos_rec_bundle	rbund;
	d_iov_t		kiov;
	d_iov_t		riov;
	int			rc;

	tree_key_bundle2iov(&kbund, &kiov);
	kbund.kb_epoch	= it_entry->ie_epoch;

	tree_rec_bundle2iov(&rbund, &riov);
	rbund.rb_biov	= &it_entry->ie_biov;
	rbund.rb_csum	= &it_entry->ie_csum;

	memset(&it_entry->ie_biov, 0, sizeof(it_entry->ie_biov));
	daos_csum_set(rbund.rb_csum, NULL, 0);

	rc = dbtree_iter_fetch(oiter->it_hdl, &kiov, &riov, anchor);
	if (rc)
		D_GOTO(out, rc);

	it_entry->ie_epoch	 = kbund.kb_epoch;
	it_entry->ie_rsize	 = rbund.rb_rsize;
	it_entry->ie_ver	 = rbund.rb_ver;
	it_entry->ie_recx.rx_idx = 0;
	it_entry->ie_recx.rx_nr  = 1;
 out:
	return rc;
}

static int
singv_iter_next(struct vos_obj_iter *oiter)
{
	vos_iter_entry_t entry;
	int		 rc;
	int		 opc;

	memset(&entry, 0, sizeof(entry));
	rc = singv_iter_fetch(oiter, &entry, NULL);
	if (rc != 0)
		return rc;

	if (oiter->it_epc_expr == VOS_IT_EPC_RE)
		entry.ie_epoch += 1;
	else if (oiter->it_epc_expr == VOS_IT_EPC_RR)
		entry.ie_epoch -= 1;
	else
		entry.ie_epoch = DAOS_EPOCH_MAX;

	opc = (oiter->it_epc_expr == VOS_IT_EPC_RR) ?
		BTR_PROBE_LE : BTR_PROBE_GE;

	rc = singv_iter_probe_fetch(oiter, opc, &entry);
	if (rc != 0)
		return rc;

	rc = singv_iter_probe_epr(oiter, &entry);
	return rc;
}

#define recx_flags_set(flags, setting)	\
	(((flags) & (setting)) == (setting))

static uint32_t
recx_get_flags(struct vos_obj_iter *oiter)
{
	uint32_t options = EVT_ITER_EMBEDDED;

	if (recx_flags_set(oiter->it_flags,
			   VOS_IT_RECX_VISIBLE | VOS_IT_RECX_SKIP_HOLES)) {
		options |= EVT_ITER_VISIBLE | EVT_ITER_SKIP_HOLES;
		D_ASSERT(!recx_flags_set(oiter->it_flags, VOS_IT_RECX_COVERED));
		goto done;
	}
	D_ASSERT(!recx_flags_set(oiter->it_flags, VOS_IT_RECX_SKIP_HOLES));
	if (oiter->it_flags & VOS_IT_RECX_VISIBLE)
		options |= EVT_ITER_VISIBLE;
	if (oiter->it_flags & VOS_IT_RECX_COVERED)
		options |= EVT_ITER_COVERED;

done:
	if (oiter->it_flags & VOS_IT_RECX_REVERSE)
		options |= EVT_ITER_REVERSE;
	if (oiter->it_flags & VOS_IT_FOR_PURGE)
		options |= EVT_ITER_FOR_PURGE;
	if (oiter->it_flags & VOS_IT_FOR_REBUILD)
		options |= EVT_ITER_FOR_REBUILD;
	return options;
}

/**
 * Prepare the iterator for the recx tree.
 */
static int
recx_iter_prepare(struct vos_obj_iter *oiter, daos_key_t *dkey,
		  daos_key_t *akey)
{
	struct vos_object	*obj = oiter->it_obj;
	struct evt_filter	 filter;
	daos_handle_t		 dk_toh;
	daos_handle_t		 ak_toh;
	int			 rc;
	uint32_t		 options;

	rc = key_tree_prepare(obj, oiter->it_epr.epr_hi, obj->obj_toh,
			      VOS_BTR_DKEY, dkey, 0,
			      vos_iter_intent(&oiter->it_iter), NULL, &dk_toh);
	if (rc != 0)
		D_GOTO(failed_0, rc);

	rc = key_tree_prepare(obj, oiter->it_epr.epr_hi, dk_toh,
			      VOS_BTR_AKEY, akey, SUBTR_EVT,
			      vos_iter_intent(&oiter->it_iter), NULL, &ak_toh);
	if (rc != 0)
		D_GOTO(failed_1, rc);

	filter.fr_ex.ex_lo = 0;
	filter.fr_ex.ex_hi = ~(0ULL);
	filter.fr_epr = oiter->it_epr;
	options = recx_get_flags(oiter);
	rc = evt_iter_prepare(ak_toh, options, &filter,
			      &oiter->it_hdl);
	if (rc != 0) {
		D_DEBUG(DB_IO, "Cannot prepare recx iterator : %d\n", rc);
		D_GOTO(failed_2, rc);
	}
 failed_2:
	key_tree_release(ak_toh, true);
 failed_1:
	key_tree_release(dk_toh, false);
 failed_0:
	return rc;
}
static int
recx_iter_probe(struct vos_obj_iter *oiter, daos_anchor_t *anchor)
{
	int	opc;
	int	rc;

	opc = anchor ? EVT_ITER_FIND : EVT_ITER_FIRST;
	rc = evt_iter_probe(oiter->it_hdl, opc, NULL, anchor);
	return rc;
}

static int
recx_iter_fetch(struct vos_obj_iter *oiter, vos_iter_entry_t *it_entry,
		daos_anchor_t *anchor)
{
	struct evt_extent	*ext;
	struct evt_entry	 entry;
	int			 rc;
	unsigned int		 inob;

	rc = evt_iter_fetch(oiter->it_hdl, &inob, &entry, anchor);
	if (rc != 0)
		D_GOTO(out, rc);

	memset(it_entry, 0, sizeof(*it_entry));

	ext = &entry.en_sel_ext;
	it_entry->ie_epoch	 = entry.en_epoch;
	it_entry->ie_recx.rx_idx = ext->ex_lo;
	it_entry->ie_recx.rx_nr	 = evt_extent_width(ext);
	ext = &entry.en_ext;
	/* Also export the original extent and the visibility flags */
	it_entry->ie_orig_recx.rx_idx = ext->ex_lo;
	it_entry->ie_orig_recx.rx_nr	 = evt_extent_width(ext);
	it_entry->ie_recx_flags = entry.en_visibility;
	it_entry->ie_rsize	= inob;
	it_entry->ie_ver	= entry.en_ver;
	it_entry->ie_biov.bi_buf = NULL;
	it_entry->ie_biov.bi_data_len = it_entry->ie_recx.rx_nr *
					it_entry->ie_rsize;
	it_entry->ie_biov.bi_addr = entry.en_addr;
 out:
	return rc;
}

static int
recx_iter_copy(struct vos_obj_iter *oiter, vos_iter_entry_t *it_entry,
	       d_iov_t *iov_out)
{
	struct bio_io_context	*bioc;
	struct bio_iov		*biov = &it_entry->ie_biov;

	D_ASSERT(biov->bi_buf == NULL);
	D_ASSERT(iov_out->iov_buf != NULL);

	/* Skip copy and return success for a punched record */
	if (bio_addr_is_hole(&biov->bi_addr))
		return 0;
	else if (iov_out->iov_buf_len < biov->bi_data_len)
		return -DER_OVERFLOW;

	/*
	 * Set 'iov_len' beforehand, cause it will be used as copy
	 * size in bio_read().
	 */
	iov_out->iov_len = biov->bi_data_len;
	bioc = oiter->it_obj->obj_cont->vc_pool->vp_io_ctxt;
	D_ASSERT(bioc != NULL);

	return bio_read(bioc, biov->bi_addr, iov_out);
}

static int
recx_iter_next(struct vos_obj_iter *oiter)
{
	return evt_iter_next(oiter->it_hdl);
}

static int
recx_iter_fini(struct vos_obj_iter *oiter)
{
	return evt_iter_finish(oiter->it_hdl);
}

/**
 * common functions for iterator.
 */
static int vos_obj_iter_fini(struct vos_iterator *vitr);

/** prepare an object content iterator */
int
vos_obj_iter_prep(vos_iter_type_t type, vos_iter_param_t *param,
		  struct vos_iterator **iter_pp)
{
	struct vos_obj_iter *oiter;
	int		     rc;

	D_ALLOC_PTR(oiter);
	if (oiter == NULL)
		return -DER_NOMEM;

	oiter->it_iter.it_type = type;
	oiter->it_epr = param->ip_epr;
	oiter->it_epc_expr = param->ip_epc_expr;
	oiter->it_flags = param->ip_flags;
	if (param->ip_flags & VOS_IT_FOR_PURGE)
		oiter->it_iter.it_for_purge = 1;
	if (param->ip_flags & VOS_IT_FOR_REBUILD)
		oiter->it_iter.it_for_rebuild = 1;

	/* XXX the condition epoch ranges could cover multiple versions of
	 * the object/key if it's punched more than once. However, rebuild
	 * system should guarantee this will never happen.
	 */
	rc = vos_obj_hold(vos_obj_cache_current(), vos_hdl2cont(param->ip_hdl),
			  param->ip_oid, param->ip_epr.epr_hi, true,
			  vos_iter_intent(&oiter->it_iter), &oiter->it_obj);
	if (rc != 0)
		D_GOTO(failed, rc);

	if (vos_obj_is_empty(oiter->it_obj)) {
		D_DEBUG(DB_IO, "Empty object, nothing to iterate\n");
		D_GOTO(failed, rc = -DER_NONEXIST);
	}

	rc = obj_tree_init(oiter->it_obj);
	if (rc != 0)
		goto failed;

	switch (type) {
	default:
		D_ERROR("unknown iterator type %d.\n", type);
		rc = -DER_INVAL;
		break;

	case VOS_ITER_DKEY:
		rc = dkey_iter_prepare(oiter, &param->ip_akey);
		break;

	case VOS_ITER_AKEY:
		rc = akey_iter_prepare(oiter, &param->ip_dkey);
		break;

	case VOS_ITER_SINGLE:
		rc = singv_iter_prepare(oiter, &param->ip_dkey,
					&param->ip_akey);
		break;

	case VOS_ITER_RECX:
		rc = recx_iter_prepare(oiter, &param->ip_dkey, &param->ip_akey);
		break;
	}

	if (rc != 0)
		D_GOTO(failed, rc);

	*iter_pp = &oiter->it_iter;
	return 0;
 failed:
	vos_obj_iter_fini(&oiter->it_iter);
	return rc;
}

int
vos_obj_iter_nested_tree_fetch(struct vos_iterator *iter, vos_iter_type_t type,
			       struct vos_iter_info *info)
{
	struct vos_obj_iter	*oiter = vos_iter2oiter(iter);
	int			 rc = 0;

	switch (iter->it_type) {
	default:
		D_ASSERT(0);
	case VOS_ITER_RECX:
	case VOS_ITER_SINGLE:
		D_ERROR("Iterator type has no subtree\n");
		return -DER_INVAL;
	case VOS_ITER_DKEY:
		if (type != VOS_ITER_AKEY) {
			D_ERROR("Invalid nested iterator type for "
				"VOS_ITER_DKEY: %d\n", type);
			return -DER_INVAL;
		}
		break;
	case VOS_ITER_AKEY:
		if (type != VOS_ITER_RECX &&
		    type != VOS_ITER_SINGLE) {
			D_ERROR("Invalid nested iterator type for "
				"VOS_ITER_AKEY: %d\n", type);
			return -DER_INVAL;
		}
	};

	rc = key_iter_fetch_root(oiter, type, info);

	if (rc != 0) {
		D_DEBUG(DB_TRACE, "Failed to fetch and initialize cursor "
			"subtree: rc=%d\n", rc);
		return rc;
	}

	info->ii_obj = oiter->it_obj;

	return 0;
}

static int
nested_dkey_iter_init(struct vos_obj_iter *oiter, struct vos_iter_info *info)
{
	int	rc;

	/* XXX the condition epoch ranges could cover multiple versions of
	 * the object/key if it's punched more than once. However, rebuild
	 * system should guarantee this will never happen.
	 */
	rc = vos_obj_hold(vos_obj_cache_current(), vos_hdl2cont(info->ii_hdl),
			  info->ii_oid, info->ii_epr.epr_hi, true,
			  vos_iter_intent(&oiter->it_iter), &oiter->it_obj);
	if (rc != 0)
		return rc;

	if (vos_obj_is_empty(oiter->it_obj)) {
		D_DEBUG(DB_IO, "Empty object, nothing to iterate\n");
		D_GOTO(failed, rc = -DER_NONEXIST);
	}

	rc = obj_tree_init(oiter->it_obj);

	if (rc != 0)
		goto failed;

	rc = dkey_iter_prepare(oiter, info->ii_akey);

	if (rc != 0)
		goto failed;

	return 0;
failed:
	vos_obj_release(vos_obj_cache_current(), oiter->it_obj);

	return rc;
}

int
vos_obj_iter_nested_prep(vos_iter_type_t type, struct vos_iter_info *info,
			 struct vos_iterator **iter_pp)
{
	struct vos_obj_iter	*oiter;
	struct evt_filter	 filter;
	daos_handle_t		 toh;
	int			 rc = 0;
	uint32_t		 options;

	D_ALLOC_PTR(oiter);
	if (oiter == NULL)
		return -DER_NOMEM;

	oiter->it_epr = info->ii_epr;
	oiter->it_epc_expr = info->ii_epc_expr;
	oiter->it_flags = info->ii_flags;
	if (type != VOS_ITER_DKEY)
		oiter->it_obj = info->ii_obj;
	if (info->ii_flags & VOS_IT_FOR_PURGE)
		oiter->it_iter.it_for_purge = 1;
	if (info->ii_flags & VOS_IT_FOR_REBUILD)
		oiter->it_iter.it_for_rebuild = 1;

	switch (type) {
	default:
		D_ERROR("unknown iterator type %d.\n", type);
		rc = -DER_INVAL;
		goto failed;

	case VOS_ITER_DKEY:
		rc = nested_dkey_iter_init(oiter, info);
		if (rc != 0)
			goto failed;
		goto success;
	case VOS_ITER_SINGLE:
	case VOS_ITER_AKEY:
		rc = dbtree_open_inplace_ex(info->ii_btr, info->ii_uma,
					vos_cont2hdl(info->ii_obj->obj_cont),
					info->ii_vea_info, &toh);
		if (rc) {
			D_DEBUG(DB_TRACE, "Failed to open tree for iterator:"
				" rc = %d\n", rc);
			goto failed;
		}
		rc = dbtree_iter_prepare(toh, BTR_ITER_EMBEDDED,
					 &oiter->it_hdl);
		break;

	case VOS_ITER_RECX:
		rc = evt_open(info->ii_evt, info->ii_uma,
			      vos_cont2hdl(info->ii_obj->obj_cont),
			      info->ii_vea_info, &toh);
		if (rc) {
			D_DEBUG(DB_TRACE, "Failed to open tree for iterator:"
				" rc = %d\n", rc);
			goto failed;
		}
		filter.fr_ex.ex_lo = 0;
		filter.fr_ex.ex_hi = ~(0ULL);
		filter.fr_epr = oiter->it_epr;
		options = recx_get_flags(oiter);
		rc = evt_iter_prepare(toh, options, &filter, &oiter->it_hdl);
		break;
	}
	key_tree_release(toh, type == VOS_ITER_RECX);

	if (rc != 0) {
		D_DEBUG(DB_TRACE, "Failed to prepare iterator: rc = %d\n", rc);
		goto failed;
	}

success:
	*iter_pp = &oiter->it_iter;
	return 0;
failed:
	D_FREE(oiter);
	return rc;
}

/** release the object iterator */
static int
vos_obj_iter_fini(struct vos_iterator *iter)
{
	struct vos_obj_iter	*oiter = vos_iter2oiter(iter);
	int			 rc;

	if (daos_handle_is_inval(oiter->it_hdl))
		D_GOTO(out, rc = -DER_NO_HDL);

	switch (iter->it_type) {
	default:
		D_ASSERT(0);
		break;

	case VOS_ITER_DKEY:
	case VOS_ITER_AKEY:
	case VOS_ITER_SINGLE:
		rc = dbtree_iter_finish(oiter->it_hdl);
		break;

	case VOS_ITER_RECX:
		rc = recx_iter_fini(oiter);
		break;
	}
 out:
	/* Release the object only if we didn't borrow it from the parent
	 * iterator.   The generic code reference counts the iterators
	 * to ensure that a parent never gets removed before all nested
	 * iterators are finalized
	 */
	if (oiter->it_obj != NULL &&
	    (iter->it_type == VOS_ITER_DKEY || !iter->it_from_parent))
		vos_obj_release(vos_obj_cache_current(), oiter->it_obj);

	D_FREE(oiter);
	return 0;
}

int
vos_obj_iter_probe(struct vos_iterator *iter, daos_anchor_t *anchor)
{
	struct vos_obj_iter *oiter = vos_iter2oiter(iter);

	switch (iter->it_type) {
	default:
		D_ASSERT(0);
		return -DER_INVAL;

	case VOS_ITER_DKEY:
	case VOS_ITER_AKEY:
		return key_iter_probe(oiter, anchor);

	case VOS_ITER_SINGLE:
		return singv_iter_probe(oiter, anchor);

	case VOS_ITER_RECX:
		return recx_iter_probe(oiter, anchor);
	}
}

static int
vos_obj_iter_next(struct vos_iterator *iter)
{
	struct vos_obj_iter *oiter = vos_iter2oiter(iter);

	switch (iter->it_type) {
	default:
		D_ASSERT(0);
		return -DER_INVAL;

	case VOS_ITER_DKEY:
	case VOS_ITER_AKEY:
		return key_iter_next(oiter);

	case VOS_ITER_SINGLE:
		return singv_iter_next(oiter);

	case VOS_ITER_RECX:
		return recx_iter_next(oiter);
	}
}

static int
vos_obj_iter_fetch(struct vos_iterator *iter, vos_iter_entry_t *it_entry,
		   daos_anchor_t *anchor)
{
	struct vos_obj_iter *oiter = vos_iter2oiter(iter);

	switch (iter->it_type) {
	default:
		D_ASSERT(0);
		return -DER_INVAL;

	case VOS_ITER_DKEY:
	case VOS_ITER_AKEY:
		return key_iter_fetch(oiter, it_entry, anchor);

	case VOS_ITER_SINGLE:
		return singv_iter_fetch(oiter, it_entry, anchor);

	case VOS_ITER_RECX:
		return recx_iter_fetch(oiter, it_entry, anchor);
	}
}

static int
vos_obj_iter_copy(struct vos_iterator *iter, vos_iter_entry_t *it_entry,
		  d_iov_t *iov_out)
{
	struct vos_obj_iter *oiter = vos_iter2oiter(iter);

	switch (iter->it_type) {
	case VOS_ITER_DKEY:
	case VOS_ITER_AKEY:
		return key_iter_copy(oiter, it_entry, iov_out);
	case VOS_ITER_SINGLE:
	case VOS_ITER_RECX:
		return recx_iter_copy(oiter, it_entry, iov_out);
	default:
		D_ASSERT(0);
		return -DER_INVAL;
	}
}

static int
obj_iter_delete(struct vos_obj_iter *oiter, void *args)
{
	int		rc = 0;
	struct vos_pool	*vpool;

	D_DEBUG(DB_TRACE, "BTR delete called of obj\n");
	vpool = vos_obj2pool(oiter->it_obj);

	rc = vos_tx_begin(vpool);
	if (rc != 0)
		goto exit;

	rc = dbtree_iter_delete(oiter->it_hdl, args);

	rc = vos_tx_end(vpool, rc);
exit:
	if (rc != 0)
		D_ERROR("Failed to delete iter entry: %d\n", rc);
	return rc;
}

static int
vos_obj_iter_delete(struct vos_iterator *iter, void *args)
{
	struct vos_obj_iter *oiter = vos_iter2oiter(iter);

	switch (iter->it_type) {
	default:
		D_ASSERT(0);
		return -DER_INVAL;

	case VOS_ITER_DKEY:
	case VOS_ITER_AKEY:
	case VOS_ITER_SINGLE:
		return obj_iter_delete(oiter, args);

	case VOS_ITER_RECX:
		return evt_iter_delete(oiter->it_hdl, args);
	}
}

static int
vos_obj_iter_empty(struct vos_iterator *iter)
{
	struct vos_obj_iter *oiter = vos_iter2oiter(iter);

	if (daos_handle_is_inval(oiter->it_hdl))
		return -DER_NO_HDL;

	switch (iter->it_type) {
	default:
		D_ASSERT(0);
		return -DER_INVAL;
	case VOS_ITER_DKEY:
	case VOS_ITER_AKEY:
	case VOS_ITER_SINGLE:
		return dbtree_iter_empty(oiter->it_hdl);
	case VOS_ITER_RECX:
		return evt_iter_empty(oiter->it_hdl);
	}
}

struct vos_iter_ops	vos_obj_iter_ops = {
	.iop_prepare		= vos_obj_iter_prep,
	.iop_nested_tree_fetch	= vos_obj_iter_nested_tree_fetch,
	.iop_nested_prepare	= vos_obj_iter_nested_prep,
	.iop_finish		= vos_obj_iter_fini,
	.iop_probe		= vos_obj_iter_probe,
	.iop_next		= vos_obj_iter_next,
	.iop_fetch		= vos_obj_iter_fetch,
	.iop_copy		= vos_obj_iter_copy,
	.iop_delete		= vos_obj_iter_delete,
	.iop_empty		= vos_obj_iter_empty,
};
/**
 * @} vos_obj_iters
 */

static int
vos_oi_set_attr_helper(daos_handle_t coh, daos_unit_oid_t oid,
		       daos_epoch_t epoch, uint64_t attr, bool set)
{
	struct vos_pool		*vpool;
	struct vos_object	*obj = NULL;
	struct vos_container	*cont;
	daos_epoch_range_t	 epr = {epoch, epoch};
	int			 rc;

	cont = vos_hdl2cont(coh);
	vpool = vos_cont2pool(cont);
	rc = vos_tx_begin(vpool);
	if (rc != 0)
		goto exit;

	rc = vos_obj_hold(vos_obj_cache_current(), vos_hdl2cont(coh), oid,
			  epoch, false, DAOS_INTENT_UPDATE, &obj);
	if (rc != 0)
		goto end;

	rc = umem_tx_add_ptr(vos_obj2umm(obj), &obj->obj_df->vo_oi_attr,
			     sizeof(obj->obj_df->vo_oi_attr));
	if (rc != 0)
		goto end;

	if (set) {
		obj->obj_df->vo_oi_attr |= attr;
	} else {
		/* Only clear bits that are set */
		uint64_t to_clear = attr & obj->obj_df->vo_oi_attr;

		obj->obj_df->vo_oi_attr ^= to_clear;
	}

	rc = vos_df_ts_update(obj, &obj->obj_df->vo_latest, &epr);
end:
	rc = vos_tx_end(vpool, rc);
exit:
	if (rc != 0)
		D_DEBUG(DB_IO, "Failed to set attributes on object: %d\n", rc);

	vos_obj_release(vos_obj_cache_current(), obj);
	return rc;
}

int
vos_oi_set_attr(daos_handle_t coh, daos_unit_oid_t oid, daos_epoch_t epoch,
		uint64_t attr)
{
	if (attr & VOS_OI_PUNCHED) {
		D_ERROR("Setting punched flag not allowed\n");
		return -DER_INVAL;
	}

	if (attr & VOS_OI_REMOVED) {
		D_ERROR("Setting removed flag not allowed\n");
		return -DER_INVAL;
	}

	D_DEBUG(DB_IO, "Set attributes "DF_UOID", epoch "DF_U64", attributes "
		 DF_X64"\n", DP_UOID(oid), epoch, attr);

	return vos_oi_set_attr_helper(coh, oid, epoch, attr, true);
}

int
vos_oi_clear_attr(daos_handle_t coh, daos_unit_oid_t oid, daos_epoch_t epoch,
		uint64_t attr)
{
	if (attr & VOS_OI_PUNCHED) {
		D_ERROR("Reset of punched flag not allowed\n");
		return -DER_INVAL;
	}

	if (attr & VOS_OI_REMOVED) {
		D_ERROR("Reset of removed flag not allowed\n");
		return -DER_INVAL;
	}

	D_DEBUG(DB_IO, "Clear attributes "DF_UOID", epoch "DF_U64
		 ", attributes "DF_X64"\n", DP_UOID(oid), epoch, attr);

	return vos_oi_set_attr_helper(coh, oid, epoch, attr, false);
}

int
vos_oi_get_attr(daos_handle_t coh, daos_unit_oid_t oid, daos_epoch_t epoch,
		struct dtx_handle *dth, uint64_t *attr)
{
	struct vos_object *obj;
	int		   rc = 0;

	D_DEBUG(DB_IO, "Get attributes "DF_UOID", epoch "DF_U64"\n",
		 DP_UOID(oid), epoch);

	if (attr == NULL) {
		D_ERROR("Invalid attribute argument\n");
		return -DER_INVAL;
	}

	vos_dth_set(dth);
	rc = vos_obj_hold(vos_obj_cache_current(), vos_hdl2cont(coh), oid,
			  epoch, true, DAOS_INTENT_DEFAULT, &obj);
	vos_dth_set(NULL);
	if (rc != 0)
		return rc;

	*attr = 0;

	if (obj->obj_df == NULL) /* nothing to do */
		D_GOTO(out, rc = 0);

	*attr = obj->obj_df->vo_oi_attr;

out:
	vos_obj_release(vos_obj_cache_current(), obj);
	return rc;
}
