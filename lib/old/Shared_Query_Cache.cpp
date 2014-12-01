#include "proxysql.h"
#include "cpp.h"
#include "proxysql_atomic.h"

static void hash_value_destroy_func(void * hash_entry) {
	fdb_hash_entry *entry= (fdb_hash_entry *) hash_entry;
	entry->expire=EXPIRE_DROPIT;
}


Shared_Query_Cache::Shared_Query_Cache(uint64_t _max_memory_size) {
	QCnow=time(NULL);
	size=SHARED_QUERY_CACHE_HASH_TABLES;
	shutdown=0;
	purge_loop_time=DEFAULT_purge_loop_time;
	purge_total_time=DEFAULT_purge_total_time;
	purge_threshold_pct_min=DEFAULT_purge_threshold_pct_min;
	purge_threshold_pct_max=DEFAULT_purge_threshold_pct_max;
	max_memory_size=_max_memory_size;
	cntDel=0;
	cntGet=0;
	cntGetOK=0;
	cntSet=0;
	cntSetERR=0;
	cntPurge=0;
	size_keys=0;
	size_values=0;
	size_metas=0;
	dataIN=0;
	dataOUT=0;
	int i;
	for (i=0; i<size; i++) {
		spinlock_rwlock_init(&fdb_hashes[i].lock);
		fdb_hashes[i].hash = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, hash_value_destroy_func);
		fdb_hashes[i].ptrArray=new PtrArray();
		fdb_hashes[i].dataSize=0;
		fdb_hashes[i].purgeChunkSize=0;
		fdb_hashes[i].purgeIdx=0;
	}
};

Shared_Query_Cache::~Shared_Query_Cache() {
	// this function assumes that the QC is not in use . No locks acquired.

	unsigned int i;
//	shutdown=1; //causes the purge thread to exit
//	pthread_join(purge_thread_id,NULL);

	for (i=0; i<SHARED_QUERY_CACHE_HASH_TABLES; i++) {
		g_hash_table_remove_all(fdb_hashes[i].hash);
		while (fdb_hashes[i].ptrArray->len) {
			fdb_hashes[i].ptrArray->remove_index_fast(0);
		}
		delete fdb_hashes[i].ptrArray;
		g_hash_table_destroy(fdb_hashes[i].hash);
	}
	
}


unsigned char * Shared_Query_Cache::get(const unsigned char *kp) {
	unsigned char *result=NULL;
	uint32_t kl=strlen((const char *)kp);
	if (kl < 3) return result;
	unsigned char i;
	i=(*((unsigned char *)kp))+(*((unsigned char *)kp+1))+(*((unsigned char *)kp+2));
	i=i%SHARED_QUERY_CACHE_HASH_TABLES;
	
	spin_rdlock(&fdb_hashes[i].lock);
	fdb_hash_entry *entry=(fdb_hash_entry *)g_hash_table_lookup(fdb_hashes[i].hash, kp);
	if (entry!=NULL) { __sync_fetch_and_add(&entry->ref_count,1); }
	spin_rdunlock(&fdb_hashes[i].lock);

	if (entry!=NULL) {
		time_t t=QCnow;
		if (entry->expire > t) {
			result=(unsigned char *)malloc(entry->length);
			memcpy(result,entry->value,entry->length);
			__sync_fetch_and_add(&cntGetOK,1);
			__sync_fetch_and_add(&dataOUT,entry->length);
			if (t > entry->access) entry->access=t;
		}
		__sync_fetch_and_sub(&entry->ref_count,1);
	}
	__sync_fetch_and_add(&cntGet,1);
	return result;
}

bool Shared_Query_Cache::set(unsigned char *kp, uint32_t kl, unsigned char *vp, uint32_t vl, time_t expire) {
	if (kl < 3) return false;
	fdb_hash_entry *entry = (fdb_hash_entry *)malloc(sizeof(fdb_hash_entry));
	entry->klen=kl;
	entry->length=vl;
	entry->ref_count=0;
	//entry->key=(unsigned char *)calloc(1,kl);
	entry->key=(unsigned char *)malloc(kl);
	memcpy(entry->key,kp,kl);
	entry->value=(unsigned char *)malloc(vl);
	memcpy(entry->value,vp,vl);
	entry->self=entry;
	entry->access=QCnow;
	if (expire > HASH_EXPIRE_MAX) {
		entry->expire=expire; // expire is a unix timestamp
	} else {
		entry->expire=QCnow+expire; // expire is seconds
	}
	unsigned char i;
	i=(*((unsigned char *)kp))+(*((unsigned char *)kp+1))+(*((unsigned char *)kp+2));
	i=i%SHARED_QUERY_CACHE_HASH_TABLES;
	spin_wrlock(&fdb_hashes[i].lock);
	fdb_hashes[i].ptrArray->add(entry);
	g_hash_table_replace(fdb_hashes[i].hash, (void *)entry->key, (void *)entry);
	spin_wrunlock(&fdb_hashes[i].lock);

	__sync_fetch_and_add(&cntSet,1);
	__sync_fetch_and_add(&size_keys,kl);
	__sync_fetch_and_add(&size_values,vl);
	__sync_fetch_and_add(&dataIN,vl);
	__sync_fetch_and_add(&size_metas,sizeof(fdb_hash_entry)+sizeof(fdb_hash_entry *));

	return true;
}

uint64_t Shared_Query_Cache::flush() {
	int i;
	uint64_t total_size=0;
	for (i=0; i<SHARED_QUERY_CACHE_HASH_TABLES; i++) {
		spin_wrlock(&fdb_hashes[i].lock);
	}
	for (i=0; i<SHARED_QUERY_CACHE_HASH_TABLES; i++) {
		total_size+=g_hash_table_size(fdb_hashes[i].hash);
		g_hash_table_remove_all(fdb_hashes[i].hash);
	}
	for (i=0; i<SHARED_QUERY_CACHE_HASH_TABLES; i++) {
		spin_wrunlock(&fdb_hashes[i].lock);
	}
	return total_size;
};

uint64_t Shared_Query_Cache::current_free_memory() {
	uint64_t cur_size=size_keys+size_values+size_metas;
	return (cur_size > max_memory_size ? 0 : max_memory_size-cur_size);
}


unsigned int Shared_Query_Cache::current_used_memory_pct() {
	uint64_t cur_size=size_keys+size_values+size_metas;
	float pctf = (float) cur_size*100/max_memory_size;
	if (pctf > 100) return 100;
	int pct=pctf;
	return pct;
}


void * Shared_Query_Cache::purgeHash_thread(void *) {
	uint64_t min_idx=0;
	unsigned int i;
	while (shutdown==0) {
		usleep(purge_loop_time);
		time_t t=time(NULL);
		QCnow=t;
		if (current_used_memory_pct() < purge_threshold_pct_min ) continue;
		for (i=0; i<SHARED_QUERY_CACHE_HASH_TABLES; i++) {
			spin_wrlock(&fdb_hashes[i].lock);
			if (fdb_hashes[i].purgeIdx==0) {
				if (fdb_hashes[i].ptrArray->len) {
					fdb_hashes[i].purgeIdx=fdb_hashes[i].ptrArray->len;
					fdb_hashes[i].purgeChunkSize=fdb_hashes[i].ptrArray->len*purge_loop_time/purge_total_time;
					if (fdb_hashes[i].purgeChunkSize < 10) { fdb_hashes[i].purgeChunkSize=fdb_hashes[i].ptrArray->len; } // this should prevent a bug with few entries left in the cache
				}
			}
			if (min_idx < fdb_hashes[i].purgeChunkSize ) min_idx=0;
			if (fdb_hashes[i].purgeIdx) while( --fdb_hashes[i].purgeIdx > min_idx) {
				fdb_hash_entry *entry=(fdb_hash_entry *)fdb_hashes[i].ptrArray->index(fdb_hashes[i].purgeIdx);
				if (( entry->expire!=EXPIRE_DROPIT) && entry->expire <= QCnow) {
					g_hash_table_remove(fdb_hashes[i].hash,entry->key);
				}
				if ( (entry->expire==EXPIRE_DROPIT)
					&& (__sync_fetch_and_add(&entry->ref_count,0)==0)
				) {
					__sync_fetch_and_sub(&size_keys,entry->klen);
					__sync_fetch_and_sub(&size_values,entry->length);
					__sync_fetch_and_sub(&size_metas,sizeof(fdb_hash_entry)+sizeof(fdb_hash_entry *));
					free(entry->key);
					free(entry->value);
					entry->self=NULL;
					free(entry);

					__sync_fetch_and_add(&cntPurge,1);
					fdb_hashes[i].ptrArray->remove_index_fast(fdb_hashes[i].purgeIdx);
				}
			}
			spin_wrunlock(&fdb_hashes[i].lock);
		}
	}
	return NULL;
}
