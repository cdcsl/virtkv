#define _LARGEFILE64_SOURCE

#include "../../nvmev.h"
#include "../include/settings.h"
#include "virt_lower.h"

void schedule_internal_operation_cb(int sqid, unsigned long long nsecs_target,
                                    void* mem, uint64_t ppa, uint64_t len,
                                    bool (*cb)(void*), void *args, bool);

lower_info virt_info = {
	.create=virt_create,
	.destroy=virt_destroy,
	.write=virt_push_data,
	.read=virt_pull_data,
	.device_badblock_checker=NULL,
	.trim_block=virt_trim_block,
	.trim_a_block=virt_trim_block,
	.refresh=virt_refresh,
	.stop=virt_stop,
	.lower_alloc=NULL,
	.lower_free=NULL,
	.lower_flying_req_wait=virt_flying_req_wait
};

uint32_t virt_create(lower_info *li,blockmanager *bm){
	li->SOK=sizeof(uint32_t);
	li->write_op=li->read_op=li->trim_op=0;

	return 1;
}

void *virt_refresh(lower_info *li){
	li->write_op = li->read_op = li->trim_op = 0;
	return NULL;
}
void *virt_destroy(lower_info *li){
	return NULL;
}

static struct ppa ppa_to_struct(const struct ssdparams *spp, uint64_t ppa_)
{
    struct ppa ppa;

    ppa.ppa = 0;
    ppa.g.ch = (ppa_ / spp->pgs_per_ch) % spp->pgs_per_ch;
    ppa.g.lun = (ppa_ % spp->pgs_per_ch) / spp->pgs_per_lun;
    ppa.g.pl = 0 ; //ppa_ % spp->tt_pls; // (ppa_ / spp->pgs_per_pl) % spp->pls_per_lun;
    ppa.g.blk = (ppa_ % spp->pgs_per_lun) / spp->pgs_per_blk;
    ppa.g.pg = ppa_ % spp->pgs_per_blk;

    //printk("%s: For PPA %llu we got ch:%d, lun:%d, pl:%d, blk:%d, pg:%d\n", 
    //        __func__, ppa_, ppa.g.ch, ppa.g.lun, ppa.g.pl, ppa.g.blk, ppa.g.pg);

	NVMEV_ASSERT(ppa_ < spp->tt_pgs);

	return ppa;
}

uint64_t virt_push_data(uint32_t PPA, uint32_t size, 
                        value_set* value, bool async,
                        algo_req *const req){
    uint64_t nsecs_completed, nsecs_latest;
    struct ppa ppa;
    struct nand_cmd swr = {
        .type = USER_IO,
        .cmd = NAND_WRITE,
        .interleave_pci_dma = false,
        .xfer_size = size,
        .stime = 0,
    };

    BUG_ON(async);
    BUG_ON(!value->ssd);
    BUG_ON(!req);
    BUG_ON(!value);
    BUG_ON(req->sqid == UINT_MAX);

    char tmp[128];
    memcpy(tmp, nvmev_vdev->ns[0].mapped + (PPA * value->ssd->sp.pgsz), 16);
    tmp[17] = '\0';

    char tmp2[128];
    memcpy(tmp2, nvmev_vdev->ns[0].mapped + (PPA * value->ssd->sp.pgsz) + 1024, 16);
    tmp2[17] = '\0';

    NVMEV_DEBUG("Writing PPA %u (%u) size %u pagesize %u in virt_push_data %s %s\n", 
                PPA, PPA * value->ssd->sp.pgsz, size, value->ssd->sp.pgsz, tmp, tmp2);

    memcpy(nvmev_vdev->ns[0].mapped + (PPA * value->ssd->sp.pgsz), value->value, size);

    ppa = ppa_to_struct(&value->ssd->sp, PPA);
    swr.ppa = &ppa;
    nsecs_completed = ssd_advance_nand((struct ssd*) value->ssd, &swr);

    //schedule_internal_operation_cb(nvmev_vdev->sqes[1]->qid, nsecs_completed, 
    //                               value->value, PPA, size, 
    //                               (void*) req->end_req, req, false);

    req->end_req(req);

	return nsecs_completed;
}

uint64_t virt_pull_data(uint32_t PPA, uint32_t size, 
                     value_set* value, bool async,
                     algo_req *const req) {	
    uint64_t nsecs_completed, nsecs_latest;
    struct ppa ppa;
    struct nand_cmd swr = {
        .type = USER_IO,
        .cmd = NAND_READ,
        .interleave_pci_dma = true,
        .xfer_size = size,
        .stime = 0,
    };

    BUG_ON(async);
    BUG_ON(!value->ssd);
    BUG_ON(!req);
    BUG_ON(!value);

    //printk("Reading PPA %u (%u) size %u sqid %u %s in virt_dev. req ppa %u\n", 
    //        PPA, PPA * value->ssd->sp.pgsz, size, nvmev_vdev->sqes[1]->qid, 
    //        async ? "ASYNCHRONOUSLY" : "SYNCHRONOUSLY", req->ppa);

    ppa = ppa_to_struct(&value->ssd->sp, PPA);
    swr.ppa = &ppa;
    nsecs_completed = ssd_advance_nand((struct ssd*) value->ssd, &swr);

    //printk("Advanced nand PPA %u\n", PPA);
    if(!async) {
        //BUG_ON(!req->parents);
        //BUG_ON(!req->parents->value);
        //BUG_ON(!req->parents->value->value);

        BUG_ON(!value);
        BUG_ON(!value->value);

        //printk("Value %p\n", value);
        //printk("Value->value %p\n", value->value);

        memcpy(value->value, 
               nvmev_vdev->ns[0].mapped + (PPA * value->ssd->sp.pgsz), size);
        //printk("Got %s (%s)\n", 
        //        (char*) value->value, 
        //        (char*) (nvmev_vdev->ns[0].mapped + (PPA * PAGESIZE)));
        //printk("Ending a synchronous read req %p ppa %u\n", req, PPA);
        req->end_req(req);
    } else {
        //printk("Scheduling with req %p\n", req);
        schedule_internal_operation_cb(nvmev_vdev->sqes[1]->qid, nsecs_completed, 
                value->value, PPA, size, 
                (void*) req->end_req, (void*) req, true);
    }

    if(req && req->need_retry) {
        //printk("Need a retry.\n");
        kfree(req);
        return UINT_MAX - 1;
    } else {
        kfree(req->params);
        kfree(req);
        return nsecs_completed;
    }
}

void *virt_trim_block(uint32_t PPA, bool async){
	virt_info.req_type_cnt[TRIM]++;
	uint64_t range[2];
	//range[0]=PPA*virt_info.SOP;
	//range[0]=offset_hooker((uint64_t)PPA*virt_info.SOP,TRIM);
	//range[1]=_PPB*virt_info.SOP;
	return NULL;
}

void virt_stop(void){}

void virt_flying_req_wait(void){
	return ;
}