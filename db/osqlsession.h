/*
   Copyright 2015 Bloomberg Finance L.P.

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.
 */

#ifndef _OSQLSESSION_H_
#define _OSQLSESSION_H_

#include "comdb2.h"
#include "errstat.h"
#include "comdb2uuid.h"
#include "sqloffload.h"

typedef struct osql_sess osql_sess_t;
typedef struct osql_req osql_req_t;
typedef struct blocksql_tran blocksql_tran_t;
typedef struct osql_uuid_req osql_uuid_req_t;

/* Magic rqid value that means "please use uuid instead" */
#define OSQL_RQID_USE_UUID 1

struct osql_sess {

    /* request part */
    unsigned long long rqid; /* identifies the client request session */
    uuid_t uuid;

    pthread_mutex_t mtx; /* mutex and cond for thread sync */
    pthread_cond_t cond;

    struct ireq *iq; /* iq owning this session -- set to NULL once dispatched */
    struct ireq *iqcopy; /* iq owning this session */

    char tzname[DB_MAX_TZNAMEDB]; /* tzname used for this request */

    int clients;                 /* number of clients;
                                    prevents freeing rq while reader_thread gets a new reply for
                                    it
                                  */
    int terminate; /* gets set if anything goes wrong w/ the session and we need
                      to abort */
    int dispatched; /* Set when session is dispatched to handle_buf */

    enum OSQL_REQ_TYPE type; /* session version */

    unsigned long long completed; /* set to rqid of the completed rqid */
    uuid_t completed_uuid;
    pthread_mutex_t completed_lock;

    struct errstat
        xerr;        /* error info(zeroed if ok), meaningful if completed=1 */
    time_t last_row; /* mark the last received row, used for poking */

    const char *sql; /* if set, pointer to sql string (part of req) */

    /* this are set for each session retry */
    uint64_t startus; /* when this started */
    uint64_t endus;   /* when this was complete */
    /* count how many ops where received, used for id the packet order;
       would be nice if this was generated by replicant, but this will do*/
    unsigned long long seq;
    unsigned int tran_rows; /* number of rows that are actual ADD/UPD/DEL */

    int queryid;
    int tableversion;
    char *tablename; // remember tablename in saveop for reordering
    hash_t *selectv_genids;
    unsigned long long last_genid; // remember updrec/insrec genid for qblobs
    unsigned long long ins_seq; // remember key seq for inserts into ins tmptbl
    uint16_t tbl_idx;
    bool last_is_ins : 1; // 1 if processing INSERT, 0 for any other oql type
    bool is_reorder_on : 1;
    bool selectv_writelock_on_update : 1;
};


enum { REQ_OPTION_QUERY_LIMITS = 1 };

/**
 * Terminates an in-use osql session (for which we could potentially
 * receive message from sql thread).
 * It calls osql_remove_session.
 * Returns 0 if success
 *
 * NOTE: it is possible to inline clean a request on master bounce,
 * which starts by unlinking the session first, and freeing bplog afterwards
 */
int osql_close_session(struct ireq *iq, osql_sess_t **sess, int is_linked, const char *func, const char *callfunc, int line);

/**
 * Get the request id, aka rqid
 *
 */
unsigned long long osql_sess_getrqid(osql_sess_t *sess);

/**
 * Register client
 * Prevent temporary the session destruction
 *
 */
int osql_sess_addclient(osql_sess_t *sess);

/**
 * Unregister client
 *
 */
int osql_sess_remclient(osql_sess_t *sess);

/**
 * Mark session duration and reported result.
 *
 */
int osql_sess_set_complete(unsigned long long rqid, uuid_t uuid,
                           osql_sess_t *sess, struct errstat *xerr);

/**
 * Log query to the reqlog
 */
void osql_sess_reqlogquery(osql_sess_t *sess, struct reqlogger *reqlog);

/**
 * Print summary session
 *
 */
int osql_sess_getcrtinfo(void *obj, void *arg);

/**
 * Returns associated blockproc transaction
 *
 */
void *osql_sess_getbptran(osql_sess_t *sess);

/* Lock the session */
int osql_sess_lock(osql_sess_t *sess);

/* Unlock the session */
int osql_sess_unlock(osql_sess_t *sess);

/* Return terminated flag */
int osql_sess_is_terminated(osql_sess_t *sess);

/* Set dispatched flag */
void osql_sess_set_dispatched(osql_sess_t *sess, int dispatched);

/* Get dispatched flag */
int osql_sess_dispatched(osql_sess_t *sess);

/* Lock complete lock */
int osql_sess_lock_complete(osql_sess_t *sess);

/* Unlock complete lock */
int osql_sess_unlock_complete(osql_sess_t *sess);

/**
 * Handles a new op received for session "rqid"
 * It saves the packet in the local bplog
 * Return 0 if success
 * Set found if the session is found or not
 *
 */
int osql_sess_rcvop(unsigned long long rqid, uuid_t uuid, int type, void *data,
                    int datalen, int *found);

/**
 * If the node "arg" machine the provided session
 * "obj", mark the session terminated
 * If "*arg: is 0, "obj" is marked terminated anyway
 *
 */
int osql_session_testterminate(void *obj, void *arg);

/**
 * Creates an sock osql session and add it to the repository
 * It makes possible reception of following log ops, but it
 * has no block processor associated yet.
 * Returns created object if success, NULL otherwise
 *
 */
osql_sess_t *osql_sess_create_sock(const char *sql, int sqlen, char *tzname,
                                   int type, unsigned long long rqid,
                                   uuid_t uuid, struct ireq *iq, int *replaced,
                                   bool is_reorder_on);

/**
 * Returns
 * - total time in ms (tottm)
 * - retries (rtrs)
 *
 */
void osql_sess_getsummary(osql_sess_t *sess, int *tottm, int *rtrs);

int osql_sess_type(osql_sess_t *sess);
int osql_sess_queryid(osql_sess_t *sess);
void osql_sess_getuuid(osql_sess_t *sess, uuid_t uuid);

/**
 * Needed for socksql and bro-s, which creates sessions before
 * iq->bplogs.
 * If we fail to dispatch to a blockprocession thread, we need this function
 * to clear the session from repository and free that leaked memory
 *
 */
void osql_sess_clear_on_error(struct ireq *iq, unsigned long long rqid,
                              uuid_t uuid);

int osql_session_is_sorese(osql_sess_t *sess);
int osql_session_set_ireq(osql_sess_t *sess, struct ireq *iq);
struct ireq *osql_session_get_ireq(osql_sess_t *sess);
int osql_cache_selectv(int type, osql_sess_t *sess, unsigned long long,
                       char *rpl);
int osql_process_selectv(osql_sess_t *sess,
                         int (*wr_sv)(void *arg, const char *tablename,
                                      int tableversion,
                                      unsigned long long genid),
                         void *wr_arg);

/**
 * Terminate a session if the session is not yet completed/dispatched
 * Return 0 if session is successfully terminated,
 *        -1 for errors,
 *        1 otherwise (if session was already processed)
 */
int osql_sess_try_terminate(osql_sess_t *sess);
#endif
