/*
*     batch.h
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


#ifndef STATISTICS_H
#define STATISTICS_H

#include <stdio.h>

#include "timer_tick.h"

/*
  stat_point -
  The structure is used to collect loading statistics.
  The two instances are kept by each batch context - 
  one is for the latest snapshot interval and another 
  for the total summary values.
*/
typedef struct stat_point
{
   /* Inbound bytes number */
  unsigned long long data_in;
   /* Outbound bytes number */
  unsigned long long data_out;

   /* Number of requests received */
  unsigned long requests;

  /* Number of 2xx responses */
  unsigned long resp_oks;

  /* Number of 3xx redirections */
  unsigned long resp_redirs;

  /* Number of 4xx responses */
  unsigned long resp_cl_errs;

  /* Number of 5xx responses */
  unsigned long resp_serv_errs;

  /* Errors of resolving, connecting, internal errors, etc. */
  unsigned long other_errs;

   /* Num of data points used to calculate average application delay */
  int appl_delay_points;
  /* Average delay in msec between request and response */
  unsigned long  appl_delay;

  /* 
     Num of data points used to calculate average application delay 
     for 2xx-OK responses.
  */
  int appl_delay_2xx_points;
   /* Average delay in msec between request and 2xx-OK response */
  unsigned long  appl_delay_2xx;

} stat_point;

/*
  op_stat_point
  Operation statistics point: two instances are residing in each batch
  context - one for the latest snapshot interval and another for the total
  summary values.
*/
typedef struct op_stat_point
{
  /* Number of successful logins */
  unsigned long* login_ok;
  /* Number of failed logins */
  unsigned long* login_failed;

  /* Number of url-counters in the below arrays */
  unsigned long uas_url_num;
  /* Array of UAS-url counters for successful fetches */
  unsigned long* uas_url_ok;
  /* Array of UAS-url counters for failed fetches */
  unsigned long* uas_url_failed;

  /* Number of successful logoffs */
  unsigned long* logoff_ok;
  /* Number of failed logoffs */
  unsigned long* logoff_failed;

  /* Used for CAPS calculation */
  unsigned long call_init_count;

} op_stat_point;

/****************************************************************************************
* Function name - stat_point_add
*
* Description - Adds counters of one stat_point structure to anther
* Input -       *left -  pointer to the stat_point, where counter will be added
*               *right -  pointer to the stat_point, which counter will be added to the <left>
* Return Code/Output - None
****************************************************************************************/
void stat_point_add (stat_point* left, stat_point* right);

/****************************************************************************************
* Function name - stat_point_reset
*
* Description - Nulls counters of a stat_point structure
* Input -       *point -  pointer to the stat_point
* 
* Return Code/Output - None
****************************************************************************************/
void stat_point_reset (stat_point* point);




/****************************************************************************************
* Function name - op_stat_point_add
*
* Description - Adds counters of one op_stat_point structure to anther
* Input -       *left -  pointer to the op_stat_point, where counter will be added
*               *right -  pointer to the op_stat_point, which counter will be added to the <left>
* Return Code/Output - None
****************************************************************************************/
void op_stat_point_add (op_stat_point* left, op_stat_point* right);


/****************************************************************************************
* Function name - op_stat_point_reset
*
* Description - Nulls counters of an op_stat_point structure
* Input -       *point -  pointer to the op_stat_point
* 
* Return Code/Output - None
****************************************************************************************/
void op_stat_point_reset (op_stat_point* point);


/****************************************************************************************
* Function name - op_stat_point_init
*
* Description - Initializes an allocated op_stat_point by allocating relevant fields for counters
* Input -       *point -  pointer to the op_stat_point, where counter will be added
*               login -  boolean flag, whether login is relevant (1) or not (0)
*               uas_url_num -  number of UAS urls, which can be 0, if UAS is not relevant
*               logoff -  boolean flag, whether login is relevant (1) or not (0)
* Return Code/Output - None
****************************************************************************************/
int op_stat_point_init (
                        op_stat_point* point, 
                        size_t login, 
                        size_t uas_url_num, 
                        size_t logoff);


/****************************************************************************************
* Function name -  op_stat_point_release
*
* Description - Releases memory allocated by op_stat_point_init ()
* Input -       *point -  pointer to the op_stat_point, where counter will be added
*
* Return Code/Output - None
****************************************************************************************/
void op_stat_point_release (op_stat_point* point);

/****************************************************************************************
* Function name -  op_stat_update
*
* Description - Updates operation statistics using information from client context
* Input -       *point -  pointer to the op_stat_point, where counters to be updated
*               current_state - current state of a client
*               prev_state -  previous state of a client
*               current_uas_url_index -  current uas url index of a the client
*               prev_uas_url_index -  previous uas url index of a the client
*
* Return Code/Output - None
****************************************************************************************/
void op_stat_update (op_stat_point* op_stat, 
                     int current_state, 
                     int prev_state,
                     size_t current_uas_url_index,
                     size_t prev_uas_url_index);

void op_stat_call_init_count_inc (op_stat_point* op_stat);

struct client_context;
struct batch_context;

/****************************************************************************************
* Function name - dump_final_statistics
*
* Description - Dumps final statistics counters to stderr and statistics file using 
*                     print_snapshot_interval_statistics and print_statistics_* functions as well as calls
*                     dump_clients () to dump the clients table.
* Input -       *cctx - pointer to client context, where the decision to complete loading 
*                     (and dump) has been made. 
* Return Code/Output - None
****************************************************************************************/
void dump_final_statistics (struct client_context* cctx);

/****************************************************************************************
* Function name - dump_snapshot_interval and up to the interval time summary statistics
*
* Description - Dumps summary statistics since the start of load
* Input -       *bctx - pointer to batch structure
*               now -  current time in msec since epoch
*
* Return Code/Output - None
****************************************************************************************/
void dump_snapshot_interval (struct batch_context* bctx, unsigned long now);


/****************************************************************************************
* Function name - print_snapshot_interval_statistics
*
* Description - Dumps final statistics counters to stderr and statistics file using 
*                     print_snapshot_interval_statistics and print_statistics_* functions 
*                     as well as calls dump_clients () to dump the clients table.
*                     
* Input -       clients - number of active clients
*               period - latest time period in milliseconds
*               *http - pointer to the HTTP collected statistics to output
*               *https - pointer to the HTTPS collected statistics to output
* Return Code/Output - None
****************************************************************************************/
void print_snapshot_interval_statistics (unsigned long period,  stat_point *http,
                                   stat_point *https);

/****************************************************************************************
* Function name - print_statistics_header
*
* Description - Prints to a file header for statistics numbers, describing counters
* Input -       *file - open file pointer
* Return Code/Output - None
****************************************************************************************/
void print_statistics_header (FILE* file);

#endif /* STATISTICS_H */