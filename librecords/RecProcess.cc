/** @file

  Record process definitions

  @section license License

  Licensed to the Apache Software Foundation (ASF) under one
  or more contributor license agreements.  See the NOTICE file
  distributed with this work for additional information
  regarding copyright ownership.  The ASF licenses this file
  to you under the Apache License, Version 2.0 (the
  "License"); you may not use this file except in compliance
  with the License.  You may obtain a copy of the License at

      http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.
 */

#include "inktomi++.h"

#include "P_EventSystem.h"
#include "P_RecCore.h"
#include "P_RecProcess.h"
#include "P_RecMessage.h"
#include "P_RecUtils.h"

static bool g_initialized = false;
static bool g_message_initialized = false;
static bool g_started = false;
static ink_cond g_force_req_cond;
static ink_mutex g_force_req_mutex;
static RecModeT g_mode_type = RECM_NULL;

#define REC_PROCESS
#include "P_RecCore.i"
#undef  REC_PROCESS

//-------------------------------------------------------------------------
// raw_stat_get_total
//-------------------------------------------------------------------------

static int
raw_stat_get_total(RecRawStatBlock * rsb, int id, RecRawStat * total)
{

  int i;
  RecRawStat *tlp;
  total->sum = 0;
  total->count = 0;

  // get global values
  total->sum = rsb->global[id]->sum;
  total->count = rsb->global[id]->count;

  // get thread local values
  for (i = 0; i < eventProcessor.n_ethreads; i++) {
    tlp = ((RecRawStat *) ((char *) (eventProcessor.all_ethreads[i]) + rsb->ethr_stat_offset)) + id;
    total->sum += tlp->sum;
    total->count += tlp->count;
  }

  return REC_ERR_OKAY;

}

//-------------------------------------------------------------------------
// raw_stat_sync_to_global
//-------------------------------------------------------------------------

static int
raw_stat_sync_to_global(RecRawStatBlock *rsb, int id)
{

  int i;
  RecRawStat *tlp;
  RecRawStat total;
  total.sum = 0;
  total.count = 0;

  // sum the thread local values
  for (i = 0; i < eventProcessor.n_ethreads; i++) {
    tlp = ((RecRawStat *) ((char *) (eventProcessor.all_ethreads[i]) + rsb->ethr_stat_offset)) + id;
    total.sum += tlp->sum;
    total.count += tlp->count;
  }

  // get the delta from the last sync
  RecRawStat delta;
  delta.sum = total.sum - rsb->global[id]->last_sum;
  delta.count = total.count - rsb->global[id]->last_count;

  Debug("stats", "raw_stat_sync_to_global(): rsb pointer:%016llX id:%d delta:%lld total:%lld last:%lld global:%lld\n", (long long)rsb, id, delta.sum, total.sum, rsb->global[id]->last_sum, rsb->global[id]->sum);

  // increment the global values by the delta
  ink_atomic_increment64(&(rsb->global[id]->sum), delta.sum);
  ink_atomic_increment64(&(rsb->global[id]->count), delta.count);

  // set the new totals as the last values seen
  ink_atomic_swap64(&(rsb->global[id]->last_sum), total.sum);
  ink_atomic_swap64(&(rsb->global[id]->last_count), total.count);

  return REC_ERR_OKAY;

}

//-------------------------------------------------------------------------
// raw_stat_clear_sum
//-------------------------------------------------------------------------

static int
raw_stat_clear_sum(RecRawStatBlock *rsb, int id)
{
  Debug("stats", "raw_stat_clear_sum(): rsb pointer:%llX id:%d\n", (long long)rsb, id);

  // the globals need to be reset too
  ink_atomic_swap64(&(rsb->global[id]->sum), 0);
  ink_atomic_swap64(&(rsb->global[id]->last_sum), 0);

  // reset the local stats
  RecRawStat *tlp;
  for (int i = 0; i < eventProcessor.n_ethreads; i++) {
    tlp = ((RecRawStat *) ((char *) (eventProcessor.all_ethreads[i]) + rsb->ethr_stat_offset)) + id;
    ink_atomic_swap64(&(tlp->sum), 0);
  }
  return REC_ERR_OKAY;
}

//-------------------------------------------------------------------------
// raw_stat_clear_count
//-------------------------------------------------------------------------

static int
raw_stat_clear_count(RecRawStatBlock * rsb, int id)
{
  Debug("stats", "raw_stat_clear_count(): rsb pointer:%llX id:%d\n", (long long)rsb, id);

  // the globals need to be reset too
  ink_atomic_swap64(&(rsb->global[id]->count), 0);
  ink_atomic_swap64(&(rsb->global[id]->last_count), 0);

  // reset the local stats
  RecRawStat *tlp;
  for (int i = 0; i < eventProcessor.n_ethreads; i++) {
    tlp = ((RecRawStat *) ((char *) (eventProcessor.all_ethreads[i]) + rsb->ethr_stat_offset)) + id;
    ink_atomic_swap64(&(tlp->count), 0);
  }
  return REC_ERR_OKAY;
}

//-------------------------------------------------------------------------
// recv_message_cb__process
//-------------------------------------------------------------------------

static int
recv_message_cb__process(RecMessage * msg, RecMessageT msg_type, void *cookie)
{
  int err;
  if ((err = recv_message_cb(msg, msg_type, cookie)) == REC_ERR_OKAY) {
    if (msg_type == RECG_PULL_ACK) {
      ink_mutex_acquire(&g_force_req_mutex);
      ink_cond_signal(&g_force_req_cond);
      ink_mutex_release(&g_force_req_mutex);
    }
  }
  return err;
}

//-------------------------------------------------------------------------
// raw_stat_sync_cont
//-------------------------------------------------------------------------

struct raw_stat_sync_cont:public Continuation
{
  raw_stat_sync_cont(ProxyMutex * m):Continuation(m)
  {
    SET_HANDLER(&raw_stat_sync_cont::exec_callbacks);
  }
  int exec_callbacks(int event, Event * e)
  {
    REC_NOWARN_UNUSED(event);
    REC_NOWARN_UNUSED(e);
    while (true) {
      RecExecRawStatSyncCbs();
      ink_sleep(REC_RAW_STAT_SYNC_INTERVAL_SEC);
    }
    return EVENT_DONE;
  }
};

//-------------------------------------------------------------------------
// config_update_cont
//-------------------------------------------------------------------------

struct config_update_cont:public Continuation
{
  config_update_cont(ProxyMutex * m):Continuation(m)
  {
    SET_HANDLER(&config_update_cont::exec_callbacks);
  }
  int exec_callbacks(int event, Event * e)
  {
    REC_NOWARN_UNUSED(event);
    REC_NOWARN_UNUSED(e);
    while (true) {
      RecExecConfigUpdateCbs();
      ink_sleep(REC_CONFIG_UPDATE_INTERVAL_SEC);
    }
    return EVENT_DONE;
  }
};

//-------------------------------------------------------------------------
// sync_cont
//-------------------------------------------------------------------------

struct sync_cont:public Continuation
{
  textBuffer *m_tb;
    sync_cont(ProxyMutex * m):Continuation(m)
  {
    SET_HANDLER(&sync_cont::sync);
    m_tb = NEW(new textBuffer(65536));
  }
   ~sync_cont()
  {
    if (m_tb != NULL) {
      delete m_tb;
      m_tb = NULL;
    }
  }
  int sync(int event, Event * e)
  {
    REC_NOWARN_UNUSED(event);
    REC_NOWARN_UNUSED(e);
    while (true) {
      send_push_message();
      RecSyncStatsFile();
      if (RecSyncConfigToTB(m_tb) == REC_ERR_OKAY) {
        int nbytes;
        RecDebug(DL_Note, "Writing '%s'", g_rec_config_fpath);
        RecHandle h_file = RecFileOpenW(g_rec_config_fpath);
        RecFileWrite(h_file, m_tb->bufPtr(), m_tb->spaceUsed(), &nbytes);
        RecFileClose(h_file);
      }
      ink_sleep(REC_REMOTE_SYNC_INTERVAL_SEC);
    }
    return EVENT_DONE;
  }
};

//-------------------------------------------------------------------------
// stat_sync_cont
//-------------------------------------------------------------------------

struct stat_sync_cont:public Continuation
{
  stat_sync_cont(ProxyMutex * m):Continuation(m)
  {
    SET_HANDLER(&stat_sync_cont::sync);
  }
  int sync(int event, Event * e)
  {
    REC_NOWARN_UNUSED(event);
    REC_NOWARN_UNUSED(e);
    while (true) {
      RecExecRawStatUpdateFuncs();
      ink_sleep(REC_STAT_UPDATE_INTERVAL_SEC);
    }
    return EVENT_DONE;
  }
};

//-------------------------------------------------------------------------
// RecProcessInit
//-------------------------------------------------------------------------

int
RecProcessInit(RecModeT mode_type, Diags * _diags)
{

  if (g_initialized) {
    return REC_ERR_OKAY;
  }

  g_records_tree = new RecTree(NULL);

  g_mode_type = mode_type;

  if (RecCoreInit(mode_type, _diags) == REC_ERR_FAIL) {
    return REC_ERR_FAIL;
  }

  /* -- defer RecMessageInit() until ProcessManager is initialized and
   *    started
   if (RecMessageInit(mode_type) == REC_ERR_FAIL) {
   return REC_ERR_FAIL;
   }

   if (RecMessageRegisterRecvCb(recv_message_cb__process, NULL)) {
   return REC_ERR_FAIL;
   }

   ink_cond_init(&g_force_req_cond);
   ink_mutex_init(&g_force_req_mutex, NULL);
   if (mode_type == RECM_CLIENT) {
   send_pull_message(RECG_PULL_REQ);
   ink_cond_wait(&g_force_req_cond, &g_force_req_mutex); 
   ink_mutex_release(&g_force_req_mutex);
   }
   */

  g_initialized = true;

  return REC_ERR_OKAY;

}

//-------------------------------------------------------------------------
// RecProcessInitMessage
//-------------------------------------------------------------------------
int
RecProcessInitMessage(RecModeT mode_type)
{

  if (g_message_initialized) {
    return REC_ERR_OKAY;
  }

  if (RecMessageInit(mode_type) == REC_ERR_FAIL) {
    return REC_ERR_FAIL;
  }

  if (RecMessageRegisterRecvCb(recv_message_cb__process, NULL)) {
    return REC_ERR_FAIL;
  }

  ink_cond_init(&g_force_req_cond);
  ink_mutex_init(&g_force_req_mutex, NULL);
  if (mode_type == RECM_CLIENT) {
    send_pull_message(RECG_PULL_REQ);
    ink_mutex_acquire(&g_force_req_mutex);
    ink_cond_wait(&g_force_req_cond, &g_force_req_mutex);
    ink_mutex_release(&g_force_req_mutex);
  }

  g_message_initialized = true;

  return REC_ERR_OKAY;

}

//-------------------------------------------------------------------------
// RecProcessStart
//-------------------------------------------------------------------------

int
RecProcessStart()
{

  if (g_started) {
    return REC_ERR_OKAY;
  }

  raw_stat_sync_cont *rssc = NEW(new raw_stat_sync_cont(new_ProxyMutex()));
  eventProcessor.spawn_thread(rssc);

  config_update_cont *cuc = NEW(new config_update_cont(new_ProxyMutex()));
  eventProcessor.spawn_thread(cuc);

  sync_cont *sc = NEW(new sync_cont(new_ProxyMutex()));
  eventProcessor.spawn_thread(sc);

  stat_sync_cont *ssc = NEW(new stat_sync_cont(new_ProxyMutex()));
  eventProcessor.spawn_thread(ssc);

  g_started = true;

  return REC_ERR_OKAY;

}

//-------------------------------------------------------------------------
// RecAllocateRawStatBlock
//-------------------------------------------------------------------------

RecRawStatBlock *
RecAllocateRawStatBlock(int num_stats)
{

  ink_off_t ethr_stat_offset;
  RecRawStatBlock *rsb;

  // allocate thread-local raw-stat memory
  if ((ethr_stat_offset = eventProcessor.allocate(num_stats * sizeof(RecRawStat))) == -1) {
    return NULL;
  }
  // create the raw-stat-block structure
  rsb = (RecRawStatBlock *) xmalloc(sizeof(RecRawStatBlock));
  memset(rsb, 0, sizeof(RecRawStatBlock));
  rsb->ethr_stat_offset = ethr_stat_offset;
  rsb->global = (RecRawStat **) xmalloc(num_stats * sizeof(RecRawStat *));
  memset(rsb->global, 0, num_stats * sizeof(RecRawStat *));
  rsb->num_stats = 0;
  rsb->max_stats = num_stats;
  return rsb;

}

//-------------------------------------------------------------------------
// RecRegisterRawStat
//-------------------------------------------------------------------------

int
RecRegisterRawStat(RecRawStatBlock * rsb,
                   RecT rec_type,
                   char *name, RecDataT data_type, RecPersistT persist_type, int id, RecRawStatSyncCb sync_cb)
{

  Debug("stats", "RecRawStatSyncCb(): rsb pointer:%llX id:%d\n", (long long)rsb, id);

  // check to see if we're good to proceed
  ink_debug_assert(id < rsb->max_stats);

  int err = REC_ERR_OKAY;

  RecRecord *r;
  RecData data_default;
  memset(&data_default, 0, sizeof(RecData));

  // register the record
  if ((r = RecRegisterStat(rec_type, name, data_type, data_default, persist_type)) == NULL) {
    err = REC_ERR_FAIL;
    goto Ldone;
  }
  if (i_am_the_record_owner(r->rec_type)) {
    r->sync_required = r->sync_required | REC_PEER_SYNC_REQUIRED;
  } else {
    send_register_message(r);
  }

  // store a pointer to our record->stat_meta.data_raw in our rsb
  rsb->global[id] = &(r->stat_meta.data_raw);
  rsb->global[id]->last_sum = 0;
  rsb->global[id]->last_count = 0;

  // setup the periodic sync callback
  RecRegisterRawStatSyncCb(name, sync_cb, rsb, id);

Ldone:
  return err;

}

//-------------------------------------------------------------------------
// RecRawStatSync...
//-------------------------------------------------------------------------

// Note: On these RecRawStatSync callbacks, our 'data' is protected
// under its lock by the caller, so no need to worry!

int
RecRawStatSyncSum(const char *name, RecDataT data_type, RecData * data, RecRawStatBlock * rsb, int id)
{
  REC_NOWARN_UNUSED(name);
  RecRawStat total;
  raw_stat_sync_to_global(rsb, id);
  total.sum = rsb->global[id]->sum;
  total.count = rsb->global[id]->count;
  RecDataSetFromInk64(data_type, data, total.sum);
  return REC_ERR_OKAY;
}

int
RecRawStatSyncCount(const char *name, RecDataT data_type, RecData * data, RecRawStatBlock * rsb, int id)
{
  REC_NOWARN_UNUSED(name);
  RecRawStat total;
  raw_stat_sync_to_global(rsb, id);
  total.sum = rsb->global[id]->sum;
  total.count = rsb->global[id]->count;
  RecDataSetFromInk64(data_type, data, total.count);
  return REC_ERR_OKAY;
}

int
RecRawStatSyncAvg(const char *name, RecDataT data_type, RecData * data, RecRawStatBlock * rsb, int id)
{
  REC_NOWARN_UNUSED(name);
  RecRawStat total;
  RecFloat avg = 0.0f;
  raw_stat_sync_to_global(rsb, id);
  total.sum = rsb->global[id]->sum;
  total.count = rsb->global[id]->count;
  if (total.count != 0)
    avg = (float) ((double) total.sum / (double) total.count);
  RecDataSetFromFloat(data_type, data, avg);
  return REC_ERR_OKAY;
}

int
RecRawStatSyncHrTimeAvg(const char *name, RecDataT data_type, RecData * data, RecRawStatBlock * rsb, int id)
{
  REC_NOWARN_UNUSED(name);
  RecRawStat total;
  RecFloat r;
  raw_stat_sync_to_global(rsb, id);
  total.sum = rsb->global[id]->sum;
  total.count = rsb->global[id]->count;
  if (total.count == 0) {
    r = 0.0f;
  } else {
    r = (float) ((double) total.sum / (double) total.count);
    r = r / (float) (HRTIME_SECOND);
  }
  RecDataSetFromFloat(data_type, data, r);
  return REC_ERR_OKAY;
}

int
RecRawStatSyncIntMsecsToFloatSeconds(const char *name, RecDataT data_type, RecData * data,
                                     RecRawStatBlock * rsb, int id)
{
  REC_NOWARN_UNUSED(name);
  RecRawStat total;
  RecFloat r;
  raw_stat_sync_to_global(rsb, id);
  total.sum = rsb->global[id]->sum;
  total.count = rsb->global[id]->count;
  if (total.count == 0) {
    r = 0.0f;
  } else {
    r = (float) ((double) total.sum / 1000);
  }
  RecDataSetFromFloat(data_type, data, r);
  return REC_ERR_OKAY;
}

int
RecRawStatSyncMHrTimeAvg(const char *name, RecDataT data_type, RecData * data, RecRawStatBlock * rsb, int id)
{
  REC_NOWARN_UNUSED(name);
  RecRawStat total;
  RecFloat r;
  raw_stat_sync_to_global(rsb, id);
  total.sum = rsb->global[id]->sum;
  total.count = rsb->global[id]->count;
  if (total.count == 0) {
    r = 0.0f;
  } else {
    r = (float) ((double) total.sum / (double) total.count);
    r = r / (float) (HRTIME_MSECOND);
  }
  RecDataSetFromFloat(data_type, data, r);
  return REC_ERR_OKAY;
}

//-------------------------------------------------------------------------
// RecIncrRawStatXXX
//-------------------------------------------------------------------------

int
RecIncrRawStatBlock(RecRawStatBlock * rsb, EThread * ethread, RecRawStat * stat_array)
{
  REC_NOWARN_UNUSED(rsb);
  REC_NOWARN_UNUSED(ethread);
  REC_NOWARN_UNUSED(stat_array);
  return REC_ERR_FAIL;
}

//-------------------------------------------------------------------------
// RecSetRawStatXXX
//-------------------------------------------------------------------------

int
RecSetRawStatSum(RecRawStatBlock * rsb, int id, ink64 data)
{
  raw_stat_clear_sum(rsb, id);
  ink_atomic_swap64(&(rsb->global[id]->sum), data);
  return REC_ERR_OKAY;
}

int
RecSetRawStatCount(RecRawStatBlock * rsb, int id, ink64 data)
{
  raw_stat_clear_count(rsb, id);
  ink_atomic_swap64(&(rsb->global[id]->count), data);
  return REC_ERR_OKAY;
}

int
RecSetRawStatBlock(RecRawStatBlock * rsb, RecRawStat * stat_array)
{
  REC_NOWARN_UNUSED(rsb);
  REC_NOWARN_UNUSED(stat_array);
  return REC_ERR_FAIL;
}

//-------------------------------------------------------------------------
// RecGetRawStatXXX
//-------------------------------------------------------------------------

int
RecGetRawStatSum(RecRawStatBlock * rsb, int id, ink64 * data)
{
  RecRawStat total;
  raw_stat_get_total(rsb, id, &total);
  *data = total.sum;
  return REC_ERR_OKAY;
}

int
RecGetRawStatCount(RecRawStatBlock * rsb, int id, ink64 * data)
{
  RecRawStat total;
  raw_stat_get_total(rsb, id, &total);
  *data = total.count;
  return REC_ERR_OKAY;
}

//-------------------------------------------------------------------------
// RecIncrGlobalRawStatXXX
//-------------------------------------------------------------------------

int
RecIncrGlobalRawStat(RecRawStatBlock * rsb, int id, ink64 incr)
{
  ink_atomic_increment64(&(rsb->global[id]->sum), incr);
  ink_atomic_increment64(&(rsb->global[id]->count), 1);
  return REC_ERR_OKAY;
}

int
RecIncrGlobalRawStatSum(RecRawStatBlock * rsb, int id, ink64 incr)
{
  ink_atomic_increment64(&(rsb->global[id]->sum), incr);
  return REC_ERR_OKAY;
}

int
RecIncrGlobalRawStatCount(RecRawStatBlock * rsb, int id, ink64 incr)
{
  ink_atomic_increment64(&(rsb->global[id]->count), incr);
  return REC_ERR_OKAY;
}

//-------------------------------------------------------------------------
// RecSetGlobalRawStatXXX
//-------------------------------------------------------------------------

int
RecSetGlobalRawStatSum(RecRawStatBlock * rsb, int id, ink64 data)
{
  ink_atomic_swap64(&(rsb->global[id]->sum), data);
  return REC_ERR_OKAY;
}

int
RecSetGlobalRawStatCount(RecRawStatBlock * rsb, int id, ink64 data)
{
  ink_atomic_swap64(&(rsb->global[id]->count), data);
  return REC_ERR_OKAY;
}

//-------------------------------------------------------------------------
// RecGetGlobalRawStatXXX
//-------------------------------------------------------------------------

int
RecGetGlobalRawStatSum(RecRawStatBlock * rsb, int id, ink64 * data)
{
  *data = rsb->global[id]->sum;
  return REC_ERR_OKAY;
}

int
RecGetGlobalRawStatCount(RecRawStatBlock * rsb, int id, ink64 * data)
{
  *data = rsb->global[id]->count;
  return REC_ERR_OKAY;
}

//-------------------------------------------------------------------------
// RegGetGlobalRawStatXXXPtr
//-------------------------------------------------------------------------

RecRawStat *
RecGetGlobalRawStatPtr(RecRawStatBlock * rsb, int id)
{
  return rsb->global[id];
}

ink64 *
RecGetGlobalRawStatSumPtr(RecRawStatBlock * rsb, int id)
{
  return &(rsb->global[id]->sum);
}

ink64 *
RecGetGlobalRawStatCountPtr(RecRawStatBlock * rsb, int id)
{
  return &(rsb->global[id]->count);
}

//-------------------------------------------------------------------------
// RecRegisterRawStatSyncCb
//-------------------------------------------------------------------------

int
RecRegisterRawStatSyncCb(char *name, RecRawStatSyncCb sync_cb, RecRawStatBlock * rsb, int id)
{
  int err = REC_ERR_FAIL;
  RecRecord *r;

  ink_rwlock_rdlock(&g_records_rwlock);
  if (ink_hash_table_lookup(g_records_ht, name, (void **) &r)) {
    rec_mutex_acquire(&(r->lock));
    if (REC_TYPE_IS_STAT(r->rec_type)) {
      if (!(r->stat_meta.sync_cb)) {
        r->stat_meta.sync_rsb = rsb;
        r->stat_meta.sync_id = id;
        r->stat_meta.sync_cb = sync_cb;
        err = REC_ERR_OKAY;
      }
    }
    rec_mutex_release(&(r->lock));
  }
  ink_rwlock_unlock(&g_records_rwlock);

  return err;

}

//-------------------------------------------------------------------------
// RecExecRawStatSyncCbs
//-------------------------------------------------------------------------

int
RecExecRawStatSyncCbs()
{

  RecRecord *r;
  int i, num_records;

  num_records = g_num_records;
  for (i = 0; i < num_records; i++) {
    r = &(g_records[i]);
    rec_mutex_acquire(&(r->lock));
    if (REC_TYPE_IS_STAT(r->rec_type)) {
      if (r->stat_meta.sync_cb) {
        (*(r->stat_meta.sync_cb)) (r->name, r->data_type, &(r->data), r->stat_meta.sync_rsb, r->stat_meta.sync_id);
        r->sync_required = REC_SYNC_REQUIRED;
      }
    }
    rec_mutex_release(&(r->lock));
  }

  return REC_ERR_OKAY;

}
