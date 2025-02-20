/**
 * (C) Copyright 2018-2019 Intel Corporation.
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
 * provided in Contract No. B620873.
 * Any reproduction of computer software, computer software documentation, or
 * portions thereof marked with this legend must also reproduce the markings.
 */

/*
 * Blob I/O library provides functionality of blob I/O over SG list consists
 * of SCM or NVMe IOVs, PMDK & SPDK are used for SCM and NVMe I/O respectively.
 */

#ifndef __BIO_API_H__
#define __BIO_API_H__

#include <daos/mem.h>
#include <daos/common.h>
#include <abt.h>

typedef struct {
	/*
	 * Byte offset within PMDK pmemobj pool for SCM;
	 * Byte offset within SPDK blob for NVMe.
	 */
	uint64_t	ba_off;
	/* DAOS_MEDIA_SCM or DAOS_MEDIA_NVME */
	uint16_t	ba_type;
	/* Is the address a hole ? */
	uint16_t	ba_hole;
	uint32_t	ba_padding;
} bio_addr_t;

/** Ensure this remains compatible */
D_CASSERT(sizeof(((bio_addr_t *)0)->ba_off) == sizeof(umem_off_t));

struct bio_iov {
	/*
	 * For SCM, it's direct memory address of 'ba_off';
	 * For NVMe, it's a DMA buffer allocated by SPDK malloc API.
	 */
	void		*bi_buf;
	/* Data length in bytes */
	size_t		 bi_data_len;
	bio_addr_t	 bi_addr;
};

struct bio_sglist {
	struct bio_iov	*bs_iovs;
	unsigned int	 bs_nr;
	unsigned int	 bs_nr_out;
};

/* Opaque I/O descriptor */
struct bio_desc;
/* Opaque I/O context */
struct bio_io_context;
/* Opaque per-xstream context */
struct bio_xs_context;

/**
 * Header for SPDK blob per VOS pool
 */
struct bio_blob_hdr {
	uint32_t	bbh_magic;
	uint32_t	bbh_blk_sz;
	uint32_t	bbh_hdr_sz; /* blocks reserved for blob header */
	uint32_t	bbh_vos_id; /* service xstream id */
	uint64_t	bbh_blob_id;
	uuid_t		bbh_blobstore;
	uuid_t		bbh_pool;
};

/*
 * Current device health state (health statistics). Periodically updated in
 * bio_bs_monitor(). Used to determine faulty device status.
 */
struct bio_dev_state {
	uint64_t	 bds_timestamp;
	uint64_t	*bds_media_errors; /* supports 128-bit values */
	uint64_t	 bds_error_count; /* error log page */
	uint32_t	 bds_bio_err;
	uint16_t	 bds_temperature; /* in Kelvin */
	/* Critical warnings */
	uint8_t		 bds_temp_warning	: 1;
	uint8_t		 bds_avail_spare_warning	: 1;
	uint8_t		 bds_dev_reliabilty_warning : 1;
	uint8_t		 bds_read_only_warning	: 1;
	uint8_t		 bds_volatile_mem_warning: 1; /*volatile memory backup*/
};

static inline void
bio_addr_set(bio_addr_t *addr, uint16_t type, uint64_t off)
{
	addr->ba_type = type;
	addr->ba_off = umem_off2offset(off);
}

static inline bool
bio_addr_is_hole(bio_addr_t *addr)
{
	return addr->ba_hole != 0;
}

static inline void
bio_addr_set_hole(bio_addr_t *addr, uint16_t hole)
{
	addr->ba_hole = hole;
}

static inline uint64_t
bio_iov2off(struct bio_iov *biov)
{
	return biov->bi_addr.ba_off;
}

static inline int
bio_sgl_init(struct bio_sglist *sgl, unsigned int nr)
{
	memset(sgl, 0, sizeof(*sgl));

	sgl->bs_nr = nr;
	D_ALLOC_ARRAY(sgl->bs_iovs, nr);
	return sgl->bs_iovs == NULL ? -DER_NOMEM : 0;
}

static inline void
bio_sgl_fini(struct bio_sglist *sgl)
{
	if (sgl->bs_iovs == NULL)
		return;

	D_FREE(sgl->bs_iovs);
	memset(sgl, 0, sizeof(*sgl));
}

/*
 * Convert bio_sglist into d_sg_list_t, caller is responsible to
 * call daos_sgl_fini(sgl, false) to free iovs.
 */
static inline int
bio_sgl_convert(struct bio_sglist *bsgl, d_sg_list_t *sgl)
{
	int i, rc;

	D_ASSERT(sgl != NULL);
	D_ASSERT(bsgl != NULL);

	rc = daos_sgl_init(sgl, bsgl->bs_nr_out);
	if (rc != 0)
		return -DER_NOMEM;

	sgl->sg_nr_out = bsgl->bs_nr_out;

	for (i = 0; i < sgl->sg_nr_out; i++) {
		struct bio_iov	*biov = &bsgl->bs_iovs[i];
		d_iov_t	*iov = &sgl->sg_iovs[i];

		iov->iov_buf = biov->bi_buf;
		iov->iov_len = biov->bi_data_len;
		iov->iov_buf_len = biov->bi_data_len;
	}

	return 0;
}

/**
 * Callbacks called on NVMe device state transition
 *
 * \param tgt_ids[IN]	Affected target IDs
 * \param tgt_cnt[IN]	Target count
 *
 * \return		0: Reaction finished;
 *			1: Reaction is in progress;
 *			-ve: Error happened;
 */
struct bio_reaction_ops {
	int (*faulty_reaction)(int *tgt_ids, int tgt_cnt);
	int (*reint_reaction)(int *tgt_ids, int tgt_cnt);
};

/**
 * Global NVMe initialization.
 *
 * \param[IN] storage_path	daos storage directory path
 * \param[IN] nvme_conf		NVMe config file
 * \param[IN] shm_id		shm id to enable multiprocess mode in SPDK
 * \param[IN] ops		Reaction callback functions
 *
 * \return		Zero on success, negative value on error
 */
int bio_nvme_init(const char *storage_path, const char *nvme_conf, int shm_id,
		  struct bio_reaction_ops *ops);

/**
 * Global NVMe finilization.
 *
 * \return		N/A
 */
void bio_nvme_fini(void);

/*
 * Initialize SPDK env and per-xstream NVMe context.
 *
 * \param[OUT] pctxt	Per-xstream NVMe context to be returned
 * \param[IN] xs_id	xstream ID
 *
 * \returns		Zero on success, negative value on error
 */
int bio_xsctxt_alloc(struct bio_xs_context **pctxt, int xs_id);

/*
 * Finalize per-xstream NVMe context and SPDK env.
 *
 * \param[IN] ctxt	Per-xstream NVMe context
 *
 * \returns		N/A
 */
void bio_xsctxt_free(struct bio_xs_context *ctxt);

/**
 * NVMe poller to poll NVMe I/O completions.
 *
 * \param[IN] ctxt	Per-xstream NVMe context
 *
 * \return		Executed message count
 */
size_t bio_nvme_poll(struct bio_xs_context *ctxt);

/*
 * Create per VOS instance blob.
 *
 * \param[IN] uuid	Pool UUID
 * \param[IN] xs_ctxt	Per-xstream NVMe context
 * \param[IN] blob_sz	Size of the blob to be created
 *
 * \returns		Zero on success, negative value on error
 */
int bio_blob_create(uuid_t uuid, struct bio_xs_context *xs_ctxt,
		    uint64_t blob_sz);

/*
 * Delete per VOS instance blob.
 *
 * \param[IN] uuid	Pool UUID
 * \param[IN] xs_ctxt	Per-xstream NVMe context
 *
 * \returns		Zero on success, negative value on error
 */
int bio_blob_delete(uuid_t uuid, struct bio_xs_context *xs_ctxt);

/*
 * Open per VOS instance I/O context.
 *
 * \param[OUT] pctxt	I/O context to be returned
 * \param[IN] xs_ctxt	Per-xstream NVMe context
 * \param[IN] umem	umem instance
 * \param[IN] uuid	Pool UUID
 *
 * \returns		Zero on success, negative value on error
 */
int bio_ioctxt_open(struct bio_io_context **pctxt,
		    struct bio_xs_context *xs_ctxt,
		    struct umem_instance *umem, uuid_t uuid);

/*
 * Finalize per VOS instance I/O context.
 *
 * \param[IN] ctxt	I/O context
 *
 * \returns		Zero on success, negative value on error
 */
int bio_ioctxt_close(struct bio_io_context *ctxt);

/*
 * Unmap (TRIM) the extent being freed.
 *
 * \param[IN] ctxt	I/O context
 * \param[IN] off	Offset in bytes
 * \param[IN] len	Length in bytes
 *
 * \returns		Zero on success, negative value on error
 */
int bio_blob_unmap(struct bio_io_context *ctxt, uint64_t off, uint64_t len);

/**
 * Write to per VOS instance blob.
 *
 * \param[IN] ctxt	VOS instance I/O context
 * \param[IN] addr	SPDK blob addr info including byte offset
 * \param[IN] iov	IO vector containing buffer to be written
 *
 * \returns		Zero on success, negative value on error
 */
int bio_write(struct bio_io_context *ctxt, bio_addr_t addr, d_iov_t *iov);

/**
 * Read from per VOS instance blob.
 *
 * \param[IN] ctxt	VOS instance I/O context
 * \param[IN] addr	SPDK blob addr info including byte offset
 * \param[IN] iov	IO vector containing buffer from read
 *
 * \returns		Zero on success, negative value on error
 */
int bio_read(struct bio_io_context *ctxt, bio_addr_t addr, d_iov_t *iov);

/**
 * Write SGL to per VOS instance blob.
 *
 * \param[IN] ctxt	VOS instance I/O context
 * \param[IN] bsgl	SPDK blob addr SGL
 * \param[IN] sgl	Buffer SGL to be written
 *
 * \returns		Zero on success, negative value on error
 */
int bio_writev(struct bio_io_context *ioctxt, struct bio_sglist *bsgl,
	       d_sg_list_t *sgl);

/**
 * Read SGL from per VOS instance blob.
 *
 * \param[IN] ctxt	VOS instance I/O context
 * \param[IN] bsgl	SPDK blob addr SGL
 * \param[IN] sgl	Buffer SGL for read
 *
 * \returns		Zero on success, negative value on error
 */
int bio_readv(struct bio_io_context *ioctxt, struct bio_sglist *bsgl,
	      d_sg_list_t *sgl);

/*
 * Finish setting up blob header and write info to blob offset 0.
 *
 * \param[IN] ctxt	I/O context
 * \param[IN] hdr	VOS blob header struct
 *
 * \returns		Zero on success, negative value on error
 */
int bio_write_blob_hdr(struct bio_io_context *ctxt, struct bio_blob_hdr *hdr);

/**
 * Allocate & initialize an io descriptor
 *
 * \param ctxt       [IN]	I/O context
 * \param sgl_cnt    [IN]	SG list count
 * \param update     [IN]	update or fetch operation?
 *
 * \return			Opaque io descriptor or NULL on error
 */
struct bio_desc *bio_iod_alloc(struct bio_io_context *ctxt,
			       unsigned int sgl_cnt, bool update);
/**
 * Free an io descriptor
 *
 * \param biod       [IN]	io descriptor to be freed
 *
 * \return			N/A
 */
void bio_iod_free(struct bio_desc *biod);

/**
 * Prepare all the SG lists of an io descriptor.
 *
 * For SCM IOV, it needs only to convert the PMDK pmemobj offset into direct
 * memory address; For NVMe IOV, it maps the SPDK blob page offset to an
 * internally maintained DMA buffer, it also needs fill the buffer for fetch
 * operation.
 *
 * \param biod       [IN]	io descriptor
 *
 * \return			Zero on success, negative value on error
 */
int bio_iod_prep(struct bio_desc *biod);

/*
 * Post operation after the RDMA transfer or local copy done for the io
 * descriptor.
 *
 * For SCM IOV, it's a noop operation; For NVMe IOV, it releases the DMA buffer
 * held in bio_iod_prep(), it also needs to write back the data from DMA buffer
 * to the NVMe device for update operation.
 *
 * \param biod       [IN]	io descriptor
 *
 * \return			Zero on success, negative value on error
 */
int bio_iod_post(struct bio_desc *biod);

/*
 * Helper function to copy data between SG lists of io descriptor and user
 * specified DRAM SG lists.
 *
 * \param biod       [IN]	io descriptor
 * \param sgls       [IN]	DRAM SG lists
 * \param nr_sgl     [IN]	Number of SG lists
 *
 * \return			Zero on success, negative value on error
 */
int bio_iod_copy(struct bio_desc *biod, d_sg_list_t *sgls, unsigned int nr_sgl);

/*
 * Helper function to get the specified SG list of an io descriptor
 *
 * \param biod       [IN]	io descriptor
 * \param idx        [IN]	Index of the SG list
 *
 * \return			SG list, or NULL on error
 */
struct bio_sglist *bio_iod_sgl(struct bio_desc *biod, unsigned int idx);

/*
 * Wrapper of ABT_thread_yield()
 */
static inline void
bio_yield(void)
{
	D_ASSERT(pmemobj_tx_stage() == TX_STAGE_NONE);
	ABT_thread_yield();
}

#endif /* __BIO_API_H__ */
