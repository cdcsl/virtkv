/*
 * Demand-based FTL Utility Implementation
 */

#include "utility.h"
#include "cache.h"

struct algo_req *make_algo_req_default(uint8_t type, value_set *value) {
	struct algo_req *a_req = (struct algo_req *)kzalloc(sizeof(struct algo_req), GFP_KERNEL);
	a_req->parents = NULL;
	a_req->type = type;
	a_req->type_lower = 0;
	a_req->rapid = false;
	a_req->end_req = demand_end_req;
    a_req->need_retry = false;

	struct demand_params *d_params = (struct demand_params *)kzalloc(sizeof(struct demand_params), GFP_KERNEL);
	d_params->value = value;
	d_params->wb_entry = NULL;
	//d_params->cmt = NULL;
	d_params->sync_mutex = NULL;
	d_params->offset = 0;

	a_req->params = (void *)d_params;

	return a_req;
}

struct algo_req *make_algo_req_rw(uint8_t type, value_set *value, request *req, snode *wb_entry) {
	struct algo_req *a_req = make_algo_req_default(type, value);
	a_req->parents = req;
	a_req->rapid = true;

	struct demand_params *d_params = (struct demand_params *)a_req->params;
	d_params->wb_entry = wb_entry;

	return a_req;
}

struct algo_req *make_algo_req_sync(uint8_t type, value_set *value) {
	struct algo_req *a_req = make_algo_req_default(type, value);
	a_req->rapid = true;
    a_req->sqid = U64_MAX;

	struct demand_params *d_params = (struct demand_params *)a_req->params;
	d_params->sync_mutex = (dl_sync *)kzalloc(sizeof(dl_sync), GFP_KERNEL);
	dl_sync_init(d_params->sync_mutex, 1);

	return a_req;
}

void free_algo_req(struct algo_req *a_req) {
	kfree(a_req->params);
	kfree(a_req);
}


#ifdef HASH_KVSSD
void copy_key_from_key(KEYT *dst, KEYT *src) {
	dst->len = src->len;
	dst->key = (char *)kzalloc(src->len, GFP_KERNEL);
	memcpy(dst->key, src->key, src->len);
}
/*void copy_key_from_value(KEYT *dst, value_set *src) {
	dst->len = *(uint8_t *)src->value;
	dst->key = (char *)kzalloc(dst->len, GFP_KERNEL);
	memcpy(dst->key, src->value+1, dst->len);
} */
void copy_key_from_value(KEYT *dst, value_set *src, int offset) {
#ifdef DVALUE
	PTR ptr = src->value + offset*GRAINED_UNIT;
#else
	PTR ptr = src->value;
#endif
	dst->len = *(uint8_t*) ptr;
	dst->key = (char *)kzalloc(dst->len, GFP_KERNEL);

	memcpy(dst->key, ptr + sizeof(uint8_t), dst->len);
}

void copy_value(value_set *dst, value_set *src, int size) {
	memcpy(dst->value, src->value, size);
}
void copy_value_onlykey(value_set *dst, value_set *src) {
	uint8_t len = *(uint8_t *)src->value;
	memcpy(dst->value, src->value, sizeof(uint8_t));
	memcpy(dst->value+1, src->value+1, len);
}
#ifdef DVALUE
void copy_key_from_grain(KEYT *dst, value_set *src, int offset) {
	PTR ptr = src->value + offset*GRAINED_UNIT;
	dst->len = *(uint8_t *)ptr;
	dst->key = (char *)kzalloc(dst->len, GFP_KERNEL);
	memcpy(dst->key, ptr+1, dst->len);
}
#endif
#endif

lpa_t get_lpa(KEYT key, void *_h_params) {
#ifdef HASH_KVSSD
	struct hash_params *h_params = (struct hash_params *)_h_params;
	h_params->lpa = PROBING_FUNC(h_params->hash, h_params->cnt) % 
                    (d_cache->env.nr_valid_tentries-1) + 1;
    BUG_ON(h_params->lpa == 2 || h_params->lpa == 0);
    NVMEV_DEBUG("Got LPA %llu key %s\n", h_params->lpa, key.key);
	return h_params->lpa;
#else
	return key;
#endif
}

lpa_t *get_oob(blockmanager *bm, ppa_t ppa) {
	return (lpa_t *)bm->get_oob(bm, ppa);
}

void set_oob(blockmanager *bm, lpa_t lpa, ppa_t ppa, page_t type) {
	int offset = 0;

#ifdef DVALUE
	switch (type) {
	case DATA:
		offset = G_OFFSET(ppa);
		ppa = G_IDX(ppa);
		break;
	case MAP:
		break;
	default:
		printk("[ERROR] Invalid type\n");
		printk("Should have aborted!!!! %s:%d\n", __FILE__, __LINE__);;
	}
#endif

	lpa_t *lpa_list = get_oob(bm, ppa);
	lpa_list[offset] = lpa;
}

void set_oob_bulk(blockmanager *bm, lpa_t *lpa_list, ppa_t ppa) {
	lpa_t *oob = get_oob(bm, ppa);
	memcpy(oob, lpa_list, 64);
}

struct inflight_params *get_iparams(request *const req, snode *wb_entry) {
	struct inflight_params *i_params;
	if (req) {
		if (!req->params) req->params = kzalloc(sizeof(struct inflight_params), GFP_KERNEL);
		i_params = (struct inflight_params *)req->params;
	} else if (wb_entry) {
		if(!wb_entry->params) wb_entry->params = kzalloc(sizeof(struct inflight_params), GFP_KERNEL);
		i_params = (struct inflight_params *)wb_entry->params;
	} else {
		printk("Should have aborted!!!! %s:%d\n", __FILE__, __LINE__);;
	}
	return i_params;
}

void free_iparams(request *const req, snode *wb_entry) {
	if (req && req->params) {
		kfree(req->params);
		req->params = NULL;
	} else if (wb_entry && wb_entry->params) {
		kfree(wb_entry->params);
		wb_entry->params = NULL;
	}
}

int hash_collision_logging(int cnt, rw_t type) {
	if (cnt > MAX_HASH_COLLISION) {
		return 1;
	}

	switch (type) {
	case DREAD:
		d_stat.r_hash_collision_cnt[cnt]++;
		break;
	case DWRITE:
		d_stat.w_hash_collision_cnt[cnt]++;
		break;
	default:
		printk("[ERROR] No R/W type found, at %s:%d\n", __FILE__, __LINE__);
		printk("Should have aborted!!!! %s:%d\n", __FILE__, __LINE__);
	}
	return 0;
}

void warn_notfound(char *f, int l) {
#ifdef WARNING_NOTFOUND
	printk("[WARNING] Read Target Data Not Found, at %s:%d\n", f, l);
#endif
}

int wb_lpa_compare(const void *a, const void *b) {
	lpa_t lpa_a = ((struct hash_params *)(*(snode **)a)->hash_params)->lpa;
	lpa_t lpa_b = ((struct hash_params *)(*(snode **)b)->hash_params)->lpa;

	if (lpa_a < lpa_b) return -1;
	if (lpa_a == lpa_b) return 0;
	if (lpa_a > lpa_b) return 1;

	return 0;
}

void insert_retry_read(request *const req) {
	if (req->parents) {
		q_enqueue((void *)req, d_member.range_q);
	} else {
        printk("insert_retry_read FIXME %pS %pS %pS\n", 
                __builtin_return_address(0), __builtin_return_address(1), 
                __builtin_return_address(2));
		//inf_assign_try(req);
	}
}
