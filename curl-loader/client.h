/*
*     client.h
*
* 2006 Copyright (c) 
* Robert Iakobashvili, <coroberti@gmail.com>
* Michael Moser,  <moser.michael@gmail.com>                 
* All rights reserved.
*
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation; either version 2 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program; if not, write to the Free Software
* Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

#ifndef CLIENT_H
#define CLIENT_H

#include <curl/curl.h>

#include "statistics.h"
#include "timer_node.h"

#define CLIENT_NAME_LEN 32

/*
  cstates - states of a virtual client.

  Completion states:
  CSTATE_ERROR and CSTATE_FINISHED_OK

  Initial state:
  CSTATE_INIT

  Operational states:
  CSTATE_LOGIN, CSTATE_LOGOFF are optionally cycling states, 
  if cycling is enabled
  CSTATE_UAS_CYCLING - is always cycling state.
*/
typedef enum cstate
  {
    /* Completion state */
    CSTATE_ERROR = -1,

    /* State 0, thus, calloc set the state automatically */
    CSTATE_INIT,
  
    /* Operational states */
    CSTATE_LOGIN,
    CSTATE_UAS_CYCLING,
    CSTATE_LOGOFF,

    /* Completion state */
    CSTATE_FINISHED_OK,
  } cstate;

/* Forward declarations */
struct batch_context;

/*
  client_context -the structure is the placeholder of  a virtual client 
  stateful information.
  
  Client context is passed to curl handle as the private data to be returned 
  back to the tracing function for output to logfile.

  Client context is also heavily used for login/logoff operations, keeping
  client-specific POST-buffers, is a placeholder for loading statistics and counters,
  assisting to collect such statistics.
  
  Client context keeps client specific context for loading, e.g. pointer to libcurl handle
  CURL* handle, number of cycles done, etc.

  client_context inherits from timer_node (the first field) to enable usage of client_context
  in timer-queue
*/
typedef struct client_context
{
  /*
    Inheritance from timer_node to enable its usage in the timer-queue.
    We can use it as a timer_node without disclosing all other
    structure of client_context.
  */
  timer_node tn;

   /*
      <Current Cycle Num> space <Client Sequence Num> space <Client IP-address> 
   */
  char client_name[CLIENT_NAME_LEN];

  /* 
     Library handle, representing all knowledge about the client from the
     side of libcurl library. We set to it url, timeouts, etc, using libcurl API.
  */
  CURL* handle;
 
  /* Current cycle number. Useful for multiple runs. */
  long cycle_num;

  /* 
     The file to be used as an output channel for this particular 
     client (normally used a file-per-batch logging strategy) 
  */
  FILE* file_output;

/* 
   Batch context, which is running the client. Used for getting configs, like 
   urls and writing collected client statistics to the batch summary statistics. 
*/
  struct batch_context* bctx;

  /* Index of the client within its batch. */
  size_t client_index;

  /* Index of the currently used UAS url. */
  size_t uas_url_curr_index;

  /* Remember here preload UAS url. */
  size_t preload_uas_url_curr_index;

   /* Current state of the client. */
  cstate client_state;

  /* Remember here pre-load state of a client. */
  cstate preload_state;

   /* Number of errors */ 
  int errors_num;

  /* 
     Flag controlling sequence of GET-POST logins and logoffs.
     When 0 - do GET, when 1 do POST and set back to 0
  */
  int get_post_count;

  /* Whether to update statistics of https or http. What about ftp: TODO */
  int is_https;
 
  /* 
     The buffers for the POST method login and logoff are allocated only, 
     when a batch is configured to run login or logoff, respectively.
  */
  char* post_data_login;
  char* post_data_logoff;

  /* 
     Counter of the headers going in or out.  For the first header in request
     or response, the respective counter is zero, whereas next headers of the same
     request/response are positive numbers.

     Indication of the first header is used to collect statistics. Statistics is
     updated only once on the first header of req/resp.
  */
  int first_hdr_req;
  int first_hdr_2xx;
  int first_hdr_3xx;
  int first_hdr_4xx;
  int first_hdr_5xx;

  /* 
     Timestamp of a request sent. Used to calculate server 
     application response delay. 
  */
  unsigned long req_sent_timestamp;

  /*
    Client-based statistics. Parallel to updating batch statistics, 
    client-based statistics is also updated.
  */
  stat_point st;

} client_context;

int first_hdr_req (client_context* cctx);
void first_hdr_req_inc (client_context* cctx);

int first_hdr_2xx (client_context* cctx);
void first_hdr_2xx_inc (client_context* cctx);

int first_hdr_3xx (client_context* cctx);
void first_hdr_3xx_inc (client_context* cctx);

int first_hdr_4xx (client_context* cctx);
void first_hdr_4xx_inc (client_context* cctx);

int first_hdr_5xx (client_context* cctx);
void first_hdr_5xx_inc (client_context* cctx);

void first_hdrs_clear_all (client_context* cctx);
void first_hdrs_clear_non_req (client_context* cctx);
void first_hdrs_clear_non_2xx (client_context* cctx);
void first_hdrs_clear_non_3xx (client_context* cctx);
void first_hdrs_clear_non_4xx (client_context* cctx);
void first_hdrs_clear_non_5xx (client_context* cctx);

void stat_data_out_add (client_context* cctx, unsigned long bytes);
void stat_data_in_add (client_context* cctx, unsigned long bytes);
void stat_err_inc (client_context* cctx);
void stat_req_inc (client_context* cctx);
void stat_2xx_inc (client_context* cctx);
void stat_3xx_inc (client_context* cctx);
void stat_4xx_inc (client_context* cctx);
void stat_5xx_inc (client_context* cctx);

void stat_appl_delay_add (client_context* cctx, unsigned long resp_timestamp);
void stat_appl_delay_2xx_add (client_context* cctx, unsigned long resp_timestamp);

void dump_client (FILE* file, client_context* cctx);

/* Currently, in smooth mode */
int pending_active_and_waiting_clients_num (struct batch_context* bctx);

/*
  Flag used to indicate, that no more loading is necessary.
  Time to dump the final statistics, clients table and exit.
  Set by SIGINT.
*/
extern int stop_loading;

#define min(a,b) \
       ({ typeof (a) _a = (a); \
           typeof (b) _b = (b); \
         _a < _b ? _a : _b; })

void set_timer_handling_func (client_context* cctx, handle_timer);

int handle_cctx_timer (struct timer_node*, void*, unsigned long);

#endif /*   CLIENT_H */
