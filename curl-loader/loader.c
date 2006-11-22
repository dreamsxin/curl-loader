/* 
 *     loader.c
 *
 * 2006 Copyright (c) 
 * Robert Iakobashvili, <coroberti@gmail.com>
 * Michael Moser, <moser.michael@gmail.com>
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
 *
 * Cooked from the CURL-project examples with thanks to the 
 * great CURL-project authors and contributors.
 */

#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <stdlib.h>

#include <sys/time.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <curl/curl.h>

#include "batch.h"
#include "client.h"
#include "loader.h"
#include "conf.h"
#include "ssl_thr_lock.h"



/*
  The limitation is due to using select() in our mget_url ()
  as well as in libcurl. Options are to consider are poll () and /dev/epoll.
*/
#define MAX_FD_IN_THREAD 1000
#define EXPLORER_USERAGENT_STR "Mozilla/4.0 (compatible; MSIE 6.0; Windows NT 5.0)" 


static int create_ip_addrs (batch_context* bctx, int bctx_num);
static int client_tracing_function (CURL *handle, 
                                    curl_infotype type, 
                                    unsigned char *data, 
                                    size_t size, 
                                    void *userp);
static size_t do_nothing_write_func (void *ptr, 
                                     size_t size, 
                                     size_t nmemb, 
                                     void *stream);
static void* batch_function (void *batch_data);
static int initial_handles_init (struct client_context*const cdata);
static int setup_curl_handle_appl (
                                   struct client_context*const cctx,  
                                   url_context* url_ctx,
                                   int post_method);
static int alloc_init_client_post_buffers (struct client_context* cctx);
static int alloc_init_client_contexts (
                                       client_context** p_cctx, 
                                       batch_context* bctx, 
                                       FILE* output_file);
static void free_batch_data_allocations (struct batch_context* bctx);

int stop_loading = 0;

static void sigint_handler (int signum)
{
  (void) signum;

  stop_loading = 1;

  fprintf (stderr, "\n\n======= SIGINT Received ============.\n");
}

typedef int (*pf_user_activity) (struct client_context*const);

static pf_user_activity ua_array[3] = 
{ 
  user_activity_hyper,
  user_activity_storm,
  user_activity_smooth
};

int 
main (int argc, char *argv [])
{
  batch_context bc_arr[BATCHES_MAX_NUM];
  pthread_t tid[BATCHES_MAX_NUM];
  int batches_num = 0; 
  int i = 0, error = 0;

  signal (SIGPIPE, SIG_IGN);

  if (parse_command_line (argc, argv) == -1)
    {
      fprintf (stderr, 
               "%s - error: failed parsing of the command line.\n", __func__);
      return -1;
    }

  if (geteuid())
    {
      fprintf (stderr, 
               "%s - error: lacking root preveledges to run this program.\n", __func__);
      return -1;
    }

  memset(bc_arr, 0, sizeof(bc_arr));

  /* 
     Parse the configuration file. 
  */
  if ((batches_num = parse_config_file (config_file, bc_arr, 
                                        sizeof(bc_arr)/sizeof(*bc_arr))) <= 0)
    {
      fprintf (stderr, "%s - error: parse_config_file () - error.\n", __func__);
      return -1;
    }
   
  /* 
     Add ip-addresses to the loading network interfaces
     and keep the ip-addr of each loading client in batch-contexts. 
  */
  if (create_ip_addrs (bc_arr, batches_num) == -1)
    {
      fprintf (stderr, "%s - error: create_ip_addrs () failed. \n", __func__);
      return -1;
    }

  fprintf (stderr, "%s - accomplished setting IP-addresses to the loading interfaces.\n", 
           __func__);

  signal (SIGINT, sigint_handler);
  
  if (! threads_run)
    {
      fprintf (stderr, "\nRUNNING WITHOUT THREADS\n\n");
      sleep (1) ;
      batch_function (&bc_arr[0]);
    }
  else
    {
      fprintf (stderr, "\n%s - STARTING THREADS\n\n", __func__);
      sleep (1);
      
      /* Init openssl mutexes and pass two callbacks to openssl. */
      if (thread_openssl_setup () == -1)
        {
          fprintf (stderr, "%s - error: thread_setup () - failed.\n", __func__);
          return -1;
        }
      
      /* Opening threads for the batches of clients */
      for (i = 0 ; i < batches_num ; i++) 
        {
          error = pthread_create (&tid[i], NULL, batch_function, &bc_arr[i]);

          if (0 != error)
            fprintf(stderr, "%s - error: Couldn't run thread number %d, errno %d\n", 
                    __func__, i, error);
          else 
            fprintf(stderr, "%s - note: Thread %d, started normally\n", __func__, i);
        }

      /* Waiting for all the threads to terminate */
      for (i = 0 ; i < batches_num ; i++) 
        {
          error = pthread_join (tid[i], NULL) ;
          fprintf(stderr, "%s - note: Thread %d terminated normally\n", __func__, i) ;
        }

      thread_openssl_cleanup ();
    }
   
  return 0;
}

/****************************************************************************************
* Function name - batch_function
* Description -   Runs the batch test either within the main-thread or in a separate thread.
*
* Input -         *batch_data contains loading configuration and active entities for a 
*                  particular batch of clients.
*
* Return Code/Output - NULL in all cases
****************************************************************************************/
static void* batch_function (void * batch_data)
{
  batch_context* bctx = (batch_context *) batch_data;
  client_context* cctx = NULL;
  FILE* output_file = 0;
  FILE* statistics_file = 0;
  char output_filename[BATCH_NAME_SIZE+4];
  char statistics_filename[BATCH_NAME_SIZE+4];
  
  int  i = 0, rval = -1;

  if (! stderr_print_client_msg)
    {
      /*
        Init batch logfile for the batch clients output 
      */
      sprintf (output_filename, "%s.log", bctx->batch_name);

      if (!(output_file = fopen(output_filename, "w")))
        {
          fprintf (stderr, 
                   "%s - \"%s\" - failed to open file \"%s\" with errno %d.\n", 
                   __func__, bctx->batch_name, output_filename, errno);
          return NULL;
        }

      /*
        Init batch statistics file for loading statistics.
      */
      sprintf (statistics_filename, "%s.txt", bctx->batch_name);
      
      if (!(statistics_file = fopen(statistics_filename, "w")))
        {
          fprintf (stderr, 
                   "%s - \"%s\" - failed to open file \"%s\" with errno %d.\n", 
                   __func__, bctx->batch_name, statistics_filename, errno);
          return NULL;
        }
      else
        {
          bctx->statistics_file = statistics_file;
          print_statistics_header (statistics_file);
        }
    }
  
  /* 
     Allocates and inits objects, containing client-context information.
  */
  if (alloc_init_client_contexts (&cctx, bctx, output_file) == -1)
    {
      fprintf (stderr, "%s - \"%s\" - failed to allocate or init cctx.\n", 
               __func__, bctx->batch_name);
      goto cleanup;
    }
 
  /* 
     Init MCURL and CURL handles. Setup of the handles is delayed to
     the later step, depending on whether login, UAS or logoff is required.
  */
  if (initial_handles_init (cctx) == -1)
    {
      fprintf (stderr, "%s - \"%s\" initial_handles_init () failed.\n", 
               __func__, bctx->batch_name);
      goto cleanup;
    }

  /* 
     Now run login, user-defined actions, like fetching various urls and and 
     sleeping in between, and logoff.
  */ 
  rval = ua_array[loading_mode] (cctx);

  if (rval == -1)
    {
      fprintf (stderr, "%s - \"%s\" -user activity failed.\n", 
               __func__, bctx->batch_name);
      goto cleanup;
    }

 cleanup:
  if (bctx->multiple_handle)
    curl_multi_cleanup(bctx->multiple_handle);

  for (i = 0 ; i < bctx->client_num ; i++)
    {
      if (bctx->cctx_array[i].handle)
        curl_easy_cleanup(bctx->cctx_array[i].handle);

      /* Free POST-buffers */
      if (cctx[i].post_data_login)
        free (cctx[i].post_data_login);
      if (cctx[i].post_data_logoff)
        free (cctx[i].post_data_logoff);
    }

  free(cctx);

  if (output_file)
      fclose (output_file);
  if (statistics_file)
      fclose (statistics_file);

  free_batch_data_allocations (bctx);

  return NULL;
}

/****************************************************************************************
* Function name - initial_handles_init
*
* Description - Initial initialization of curl multi-handle and the curl handles (clients), 
*               used in the batch
* Input -       *ctx_array - array of clients for a particular batch of clients
*
* Return Code/Output - On Success - 0, on Error -1
****************************************************************************************/
static int initial_handles_init (client_context*const ctx_array)
{
  batch_context* bctx = ctx_array->bctx;
  int k = 0;

  /* Init CURL multi-handle. */
  if (! (bctx->multiple_handle = curl_multi_init()) )
    {
      fprintf (stderr, 
               "%s - error: curl_multi_init() failed for batch \"%s\" .\n", 
               __func__, bctx->batch_name) ;
      return -1;
    }

  /* Allocate and fill login/logoff POST strings for each client. */ 
  if (bctx->do_login)
    {
      if (alloc_init_client_post_buffers (ctx_array) == -1)
        {
          fprintf (stderr, "%s - error: alloc_client_post_buffers () .\n", __func__);
          return -1;
        }
    }

  /* Initialize all the CURL handlers */
  for (k = 0 ; k < bctx->client_num ; k++)
    {
      if (!(bctx->cctx_array[k].handle = curl_easy_init ()))
        {
          fprintf (stderr,"%s - error: curl_easy_init () failed for k=%d.\n",
                   __func__, k);
          return -1;
        }
    }
        
  bctx->active_clients_count = bctx->client_num;

  return 0;
}



/****************************************************************************************
* Function name - setup_curl_handle
*
* Description - Setup for a single curl handle (client): removes a handle from multi-handle, 
*               resets the handle, inits it, and, finally, adds the handle back to the
*               multi-handle.
* Input -       *cctx - pointer to client context, which contains CURL handle pointer;
*               *url_ctx - pointer to url-context, containing all url-related information;
*               cycle_number - current number of loading cycle, passing here for storming mode;
*               post_method - when 'true', POST method is used instead of the default GET
* Return Code/Output - On Success - 0, on Error -1
****************************************************************************************/
int setup_curl_handle (client_context*const cctx,
                         url_context* url_ctx,
                         long cycle_number,
                         int post_method)
{
  batch_context* bctx = cctx->bctx;
  CURL* handle = cctx->handle; //bctx->client_handles_array[cctx->client_index];

  if (!cctx || !url_ctx)
    {
      return -1;
    }

  /*
    Remove the handle from the multiple handle and reset it. 
    Still the handle remembers DNS, cookies, etc. 
  */
  curl_multi_remove_handle (bctx->multiple_handle, handle);
  curl_easy_reset (handle);

  /* Bind the handle to a certain IP-address */
  curl_easy_setopt (handle, CURLOPT_INTERFACE, 
                    bctx->ip_addr_array [cctx->client_index]);

  curl_easy_setopt (handle, CURLOPT_NOSIGNAL, 1);
    
  /* Set the url */
  curl_easy_setopt (handle, CURLOPT_URL, url_ctx->url_str);
  
  /* Set the index to client for smooth-mode */
  if (url_ctx->url_uas_num >= 0)
    cctx->uas_url_curr_index = url_ctx->url_uas_num;
  
  bctx->url_index = url_ctx->url_uas_num;

  curl_easy_setopt (handle, CURLOPT_DNS_CACHE_TIMEOUT, -1);

  /* Set the connection timeout */
  curl_easy_setopt (handle, CURLOPT_CONNECTTIMEOUT, connect_timeout);

  /* Define the connection re-use policy. When passed 1, dont re-use */
  curl_easy_setopt (handle, CURLOPT_FRESH_CONNECT, reuse_connection_forbidden);

  /* 
     If DNS resolving is necesary, global DNS cache is enough,
     otherwise compile libcurl with ares (cares) library support.
     Attention: DNS global cache is not thread-safe, therefore use
     cares for asynchronous DNS lookups.

     curl_easy_setopt (handle, CURLOPT_DNS_USE_GLOBAL_CACHE, 1); 
  */
     
  curl_easy_setopt (handle, CURLOPT_VERBOSE, 1);
  curl_easy_setopt (handle, CURLOPT_DEBUGFUNCTION, 
                    client_tracing_function);

  if (!output_to_stdout)
    {
      curl_easy_setopt (handle, CURLOPT_WRITEFUNCTION,
                        do_nothing_write_func);
    }

  curl_easy_setopt (handle, CURLOPT_SSL_VERIFYPEER, 0);
  curl_easy_setopt (handle, CURLOPT_SSL_VERIFYHOST, 0);
    
  /* Set current cycle_number in buffer. */
  if (loading_mode == LOAD_MODE_STORMING)
    {
      cctx->cycle_num = cycle_number;
    }

  /* 
     This is to return cctx pointer as the last void* userp to the 
     tracing function. 
  */
  curl_easy_setopt (handle, CURLOPT_DEBUGDATA, cctx);

  /* Set the private pointer to be used by the smooth-mode. */
  curl_easy_setopt (handle, CURLOPT_PRIVATE, cctx);

  /* Without the buffer set, we do not get any errors in tracing function. */
  curl_easy_setopt (handle, CURLOPT_ERRORBUFFER, bctx->error_buffer);    

  /* 
     Application (url) specific setups, like HTTP-specific, FTP-specific, etc. 
  */
  if (setup_curl_handle_appl (cctx, url_ctx, post_method) == -1)
    {
      fprintf (stderr,
               "%s - error: setup_curl_handle_appl () failed .\n",
               __func__);
      return -1;
    }
     
  /* The handle is supposed to be removed before. */
  curl_multi_add_handle(bctx->multiple_handle, handle);

  return 0;
}
  
/****************************************************************************************
* Function name - setup_curl_handle_appl
*
* Description - Application/url-type specific setup for a single curl handle (client)
* Input -       *cctx - pointer to client context, which contains CURL handle pointer;
*               *url_ctx - pointer to url-context, containing all url-related information;
*               post_method - when 'true', POST method is used instead of the default GET
* Return Code/Output - On Success - 0, on Error -1
****************************************************************************************/
static int setup_curl_handle_appl (
                                   client_context*const cctx,  
                                   url_context* url_ctx,
                                   int post_method)
{
  //  batch_context* bctx = cctx->bctx;
  CURL* handle = cctx->handle; // bctx->client_handles_array[cctx->client_index];

  cctx->is_https = (url_ctx->url_appl_type == URL_APPL_HTTPS);

  if (url_ctx->url_appl_type == URL_APPL_HTTPS ||
      url_ctx->url_appl_type == URL_APPL_HTTP)
    {
      /*************** HTTP-specific *************************************/
      
      /* 
         Follow possible HTTP-redirection from header Location of the 
         3xx HTTP responses, like 301, 302, 307, etc. It also updates the url 
         in the options, so you do not need to parse headers and extract the 
         value of header Location. Great job done by the libcurl people.
      */
      curl_easy_setopt (handle, CURLOPT_FOLLOWLOCATION, 1);
      
      /* Enable infinitive (-1) redirection number. */
      curl_easy_setopt (handle, CURLOPT_MAXREDIRS, -1);     
      /* 
         TODO: Lets be Explorer-6, but actually User-Agent header 
         should be configurable. 
      */
      curl_easy_setopt (handle, CURLOPT_USERAGENT, EXPLORER_USERAGENT_STR);
      
      /* Enable cookies. This is important for verious authentication schemes. */
      curl_easy_setopt (handle, CURLOPT_COOKIEFILE, "");
      
      /* Make POST, using post buffer, if requested. */
      if (post_method)
        {
          char* post_buff = NULL;
          
          if (url_ctx->url_lstep == URL_LOAD_LOGIN)
            post_buff = cctx->post_data_login;
          else if (url_ctx->url_lstep == URL_LOAD_LOGOFF)
            post_buff = cctx->post_data_logoff;
          else
            {            
              fprintf (stderr,
                       "%s - error: post_method to be used only for login or logoff url.\n",
                       __func__);
              return -1;
            }
          curl_easy_setopt(handle, CURLOPT_POSTFIELDS, post_buff);
        }
    }
  else
    {
      /* FTP-specific setup */
    }

  return 0;
}

/****************************************************************************************
* Function name - client_tracing_function
* 
* Description - Used to log activities of each client to $batch_name.log file
* Input -       *handle - pointer to CURL handle;
*               type - type of libcurl information passed, like headers, data, info, etc
*               *data- pointer to data, like headers, etc
*               size - number of bytes passed with data-pointer
*               *userp - pointer to user-specific data, which in our case is the 
*                        client_context structure
* Return Code/Output - On Success - 0, on Error -1
****************************************************************************************/
static int client_tracing_function (CURL *handle, curl_infotype type, 
                                    unsigned char *data, size_t size, void *userp)
{
  client_context* cctx = (client_context*) userp;
  char*url_target = NULL, *url_effective = NULL;

  if (url_logging)
    {
      switch (cctx->client_state)
        {
        case CSTATE_LOGIN:
          url_target = cctx->bctx->login_url.url_str;
          break;
        case CSTATE_UAS_CYCLING:
          url_target = cctx->bctx->uas_url_ctx_array[cctx->uas_url_curr_index].url_str;
          break;
        case CSTATE_LOGOFF:
          url_target = cctx->bctx->logoff_url.url_str;
          break;

        default:
          url_target = NULL;
        }
      /* Clients are being redirected back and forth by 3xx redirects. */
      curl_easy_getinfo (handle, CURLINFO_EFFECTIVE_URL, &url_effective);
    }

  const char*const url = url_effective ? url_effective : url_target;
  const int url_print = (url_logging && url) ? 1 : 0;
  const int url_diff = (url_print && url_effective && url_target) ? 
    strcmp(url_effective, url_target) : 0;

  switch (type)
    {
    case CURLINFO_TEXT:
      if (verbose_logging)
          fprintf(cctx->file_output, "%ld %s :== Info: %s: eff-url: %s, url: %s\n",
                  cctx->cycle_num, cctx->client_name, data,
                  url_print ? url : "", url_diff ? url_target : "");
      break;

    case CURLINFO_ERROR:
      fprintf(cctx->file_output, "%ld %s !! ERROR: %s: eff-url: %s, url: %s\n", 
              cctx->cycle_num, cctx->client_name, data, 
              url_print ? url : "", url_diff ? url_target : "");

      cctx->client_state = CSTATE_ERROR;

      stat_err_inc (cctx);
      hdrs_clear_all (cctx);
      break;

    case CURLINFO_HEADER_OUT:
      if (verbose_logging)
          fprintf(cctx->file_output, "%ld %s => Send header: eff-url: %s, url: %s\n", 
                  cctx->cycle_num, cctx->client_name, url_print ? url : "", url_diff ? url_target : "");

      stat_data_out_add (cctx, (unsigned long) size);

      if (! hdrs_req (cctx))
        {
          /* First header of the HTTP-request. */
          hdrs_req_inc (cctx);
          stat_req_inc (cctx); /* Increment number of requests */
          cctx->req_timestamp = get_tick_count ();
        }
      hdrs_clear_non_req (cctx);
      break;

    case CURLINFO_DATA_OUT:
      if (verbose_logging)
          fprintf(cctx->file_output, "%ld %s => Send data: eff-url: %s, url: %s\n", 
                  cctx->cycle_num, cctx->client_name, url_print ? url : "",
                  url_diff ? url_target : "");

      stat_data_out_add (cctx, (unsigned long) size);
      hdrs_clear_all (cctx);
      break;

    case CURLINFO_SSL_DATA_OUT:
      if (verbose_logging) 
          fprintf(cctx->file_output, "%ld %s => Send ssl data: eff-url: %s, url: %s\n", 
                  cctx->cycle_num, cctx->client_name, url_print ? url : "",
                  url_diff ? url_target : "");

      stat_data_out_add (cctx, (unsigned long) size);
      hdrs_clear_all (cctx);
      break;
      
    case CURLINFO_HEADER_IN:
      /* 
         CURL library assists us by passing to the full HTTP-headers, 
         not just parts. 
      */
      stat_data_in_add (cctx, (unsigned long) size);

      {
        long response_status = 0, response_module = 0;
        
        if (verbose_logging)
          fprintf(cctx->file_output, "%ld %s <= Recv header: eff-url: %s, url: %s\n", 
                  cctx->cycle_num, cctx->client_name, url_print ? url : "",
                  url_diff ? url_target : "");
        
        curl_easy_getinfo (handle, CURLINFO_RESPONSE_CODE, &response_status);

        response_module = response_status / (long)100;
        
        switch (response_module)
          {
          case 1: /* 100-Continue and 101 responses */
            if (verbose_logging)
              fprintf(cctx->file_output, "%ld %s:!! %ld CONTINUE: eff-url: %s, url: %s\n", 
                      cctx->cycle_num, cctx->client_name, response_status,
                      url_print ? url : "", url_diff ? url_target : "");
            hdrs_clear_all (cctx);
            break;

          case 2: /* 200 OK */
            if (verbose_logging)
              fprintf(cctx->file_output, "%ld %s:!! %ld OK: eff-url: %s, url: %s\n",
                      cctx->cycle_num, cctx->client_name, response_status,
                      url_print ? url : "", url_diff ? url_target : "");

            if (! hdrs_2xx (cctx))
              {
                /* First header of 2xx response */
                hdrs_2xx_inc (cctx);
                stat_2xx_inc (cctx); /* Increment number of 2xx responses */

                /* Count into the averages HTTP/S server response delay */
                const unsigned long time_2xx_resp = get_tick_count ();
                stat_appl_delay_2xx_add (cctx, time_2xx_resp);
                stat_appl_delay_add (cctx, time_2xx_resp);
              }
            hdrs_clear_non_2xx (cctx);
            break;
       
          case 3: /* 3xx REDIRECTIONS */
            fprintf(cctx->file_output, "%ld %s:!! %ld REDIRECTION: %s: eff-url: %s, url: %s\n", 
                    cctx->cycle_num, cctx->client_name, response_status, data,
                    url_print ? url : "", url_diff ? url_target : "");

            if (! hdrs_3xx (cctx))
              {
                /* First header of 3xx response */
                hdrs_3xx_inc (cctx);
                stat_3xx_inc (cctx); /* Increment number of 3xx responses */
                const unsigned long time_3xx_resp = get_tick_count ();
                stat_appl_delay_add (cctx, time_3xx_resp);
              }
            hdrs_clear_non_3xx (cctx);
            break;

          case 4: /* 4xx Client Error. Being a client side, can we ever get here?*/
             break;

          case 5: /* 5xx Server Error */
            fprintf(cctx->file_output, "%ld %s :!! %ld SERVER_ERROR : %s: eff-url: %s, url: %s\n", 
                    cctx->cycle_num, cctx->client_name, response_status, data,
                    url_print ? url : "", url_diff ? url_target : "");

            if (! hdrs_5xx (cctx))
              {
                /* First header of 5xx response */
                hdrs_5xx_inc (cctx);
                stat_5xx_inc (cctx);  /* Increment number of 5xx responses */

                const unsigned long time_5xx_resp = get_tick_count ();
                stat_appl_delay_add (cctx, time_5xx_resp);
              }
            hdrs_clear_non_5xx (cctx);
            break;

          default :
            fprintf(cctx->file_output, 
                    "%ld %s:<= WARNING: parsing error: wrong status code \"%s\".\n", 
                    cctx->cycle_num, cctx->client_name, (char*) data);
            /* FTP breaks it: - cctx->client_state = CSTATE_ERROR; */
            break;
          }
      }
      break;

    case CURLINFO_DATA_IN:
      if (verbose_logging) 
          fprintf(cctx->file_output, "%ld %s <= Recv data: eff-url: %s, url: %s\n", 
                  cctx->cycle_num, cctx->client_name, 
                  url_print ? url : "", url_diff ? url_target : "");

      stat_data_in_add (cctx,  (unsigned long) size);
      hdrs_clear_all (cctx);
      break;

    case CURLINFO_SSL_DATA_IN:
      if (verbose_logging) 
          fprintf(cctx->file_output, "%ld %s <= Recv ssl data: eff-url: %s, url: %s\n", 
                  cctx->cycle_num, cctx->client_name, 
                  url_print && url ? url : "", url_diff ? url_target : "");

      stat_data_in_add (cctx,  (unsigned long) size);
      hdrs_clear_all (cctx);
      break;

    default:
      fprintf (stderr, "default OUT - \n");
    }

  // fflush (cctx->file_output); // Don't do it
  return 0;
}

/****************************************************************************************
* Function name - alloc_init_client_post_buffers
*
* Description - Allocate and initialize buffers to be used for POST-ing
* Input -       *ctx - pointer to client context
*
* Return Code/Output - On Success - 0, on Error -1
****************************************************************************************/
static int alloc_init_client_post_buffers (client_context* cctx)
{
  int i;
  batch_context* bctx = cctx->bctx;
  const char percent_symbol = '%';
  int counter_percent_sym = 0;

  if (! bctx->login_post_str[0])
    {
      return -1;
    }

  char* pos = bctx->login_post_str;

  /* Calculate number of '%' symbols in post_login_format */
  while ((pos = strchr (pos, percent_symbol)))
    {
      counter_percent_sym++;
      pos = pos + 1;
    }

  for (i = 0;  i < bctx->client_num; i++)
    {
      /*
        Allocate client buffers for POSTing login and logoff credentials.
      */
      if (! (cctx[i].post_data_login = 
             (char *) calloc(POST_LOGIN_BUF_SIZE, sizeof (char))))
        {
          fprintf (stderr,
                   "\"%s\" - %s - failed to allocate post login buffer.\n",
                   bctx->batch_name, __func__) ;
          return -1;
        }
      
      if (! (cctx[i].post_data_logoff = 
             (char *) calloc(POST_LOGOFF_BUF_SIZE, sizeof (char))))
        {
          fprintf (stderr,
                   "%s - error: %s - failed to allocate post login buffer.\n",
                   __func__, bctx->batch_name);
          return -1;
        }

      if (counter_percent_sym == 4)
        {
          /* For each client init post buffer, containing username and
             password with uniqueness added via added to the base 
             username and password client index. */
          snprintf (cctx[i].post_data_login, 
                    POST_LOGIN_BUF_SIZE, 
                    bctx->login_post_str,
                    bctx->login_username, i + 1,
                    bctx->login_password, i + 1);
        }
      else if (counter_percent_sym == 2)
        {
          /* All clients have the same login_username and password.*/
          snprintf (cctx[i].post_data_login, 
                    POST_LOGIN_BUF_SIZE, 
                    bctx->login_post_str,
                    bctx->login_username, 
                    bctx->login_password);
        }
      else
        {
          return -1;
        }
      
      if (bctx->logoff_post_str[0])
        {
          snprintf (cctx[i].post_data_logoff,
                    POST_LOGOFF_BUF_SIZE,
                    "%s",
                    bctx->logoff_post_str);
        }
    }
  return 0;
}

/****************************************************************************************
* Function name - alloc_init_client_contexts
*
* Description - Allocate and initialize client contexts
* Input -       *bctx - back-pointer to batch context to be set to all clients  of the batch
*               *output_file - output file to be used by all clients of the batch
* Output -      **p_cctx - pointer to client contexts array to be allocated and initialized
*
* Return Code -  On Success - 0, on Error -1
****************************************************************************************/
static int alloc_init_client_contexts (
                                       client_context** p_cctx, 
                                       batch_context* bctx,
                                       FILE* output_file)
{
  int i;
  client_context* cctx = 0;

  /* Allocate client contexts */
  if (!(cctx  = (client_context *) calloc(bctx->client_num, 
                                          sizeof (client_context))))
    {
      fprintf (stderr, "\"%s\" - %s - failed to allocate cctx.\n", 
               bctx->batch_name, __func__);
      return -1;
    }

  *p_cctx = cctx;

  /* 
     Iterate through client contexts and initialize them. 
  */
  for (i = 0 ; i < bctx->client_num ; i++)
    {
      /* 
         Build client name for logging, based on sequence number and 
         ip-address for each simulated client. 
      */
      cctx[i].cycle_num = 0;

      snprintf(cctx[i].client_name, 
               sizeof(cctx[i].client_name) - 1, 
               "%d (%s) ", i + 1, bctx->ip_addr_array[i]);

       /* 
         Set index of the client within the batch.
         Useful to get the client's CURL handle from bctx. 
      */
      cctx[i].client_index = i;
      cctx[i].uas_url_curr_index = 0; /* Actually zeroed by calloc. */

      /* Set output stream for each client to be either batch logfile or stderr. */
      cctx[i].file_output = stderr_print_client_msg ? stderr : output_file;

      /* 
         Set pointer in client to its batch object. The pointer will be used to get 
         configuration and set back statistics to batch.
      */
      cctx[i].bctx = bctx;
    }

  bctx->cctx_array = cctx;

  return 0;
}

/****************************************************************************************
* Function name - free_batch_data_allocations
*
* Description - Deallocates all batch allocations
* Input -       *bctx - pointer to batch context to release its allocations
* Return Code/Output - None
****************************************************************************************/
static void free_batch_data_allocations (batch_context* bctx)
{
  int i;

  /* Free the login and logoff urls */
  free (bctx->login_url.url_str);
  bctx->login_url.url_str = NULL;

  free (bctx->logoff_url.url_str);
  bctx->logoff_url.url_str = NULL;

  /* Free the allocated UAS url contexts*/
  if (bctx->uas_url_ctx_array && bctx->uas_urls_num)
    {
      /* Free all URL-strings */
      for (i = 0 ; i < bctx->uas_urls_num; i++)
        {
          if (bctx->uas_url_ctx_array[i].url_str)
            {
              free (bctx->uas_url_ctx_array[i].url_str);
              bctx->uas_url_ctx_array[i].url_str = NULL ;
            }
        }

      /* Free URL context array */
      free (bctx->uas_url_ctx_array);
      bctx->uas_url_ctx_array = NULL;
    }
}

/****************************************************************************************
* Function name - create_ip_addrs
*
* Description - Adds ip-addresses of batches of loading clients to network adapter/s
* Input -       *bctx_array - pointer to the array of batch contexts
*               bctx_num - number of batch contexts in <bctx_array>
* Return Code/Output - None
****************************************************************************************/
static int create_ip_addrs (batch_context* bctx_array, int bctx_num)
{
  int bi, cli; /* Batch and client indexes */
  struct in_addr in_address;
  char*** ip_addresses =0;
  char* dotted_ip_addr = 0;

  /* Add secondary IP-addresses to the "loading" network interface. */
  if (!(ip_addresses = (char***)calloc (bctx_num, sizeof (char**))))
    {
      fprintf (stderr, "%s - error: failed to allocate ip_addresses.\n", __func__);
      return -1;
    }
  
  for (bi = 0 ; bi < bctx_num ; bi++) 
    {
      /* Allocate array of IP-addresses */
      if (!(ip_addresses[bi] = (char**)calloc (bctx_array[bi].client_num, 
                                               sizeof (char *))))
        {
          fprintf (stderr, 
                   "%s - error: failed to allocate array of ip-addresses for batch %d.\n", 
                   __func__, bi);
          return -1;
        }

      /* Set them to the batch contexts to remember them. */
      bctx_array[bi].ip_addr_array = ip_addresses[bi]; 

      /* Allocate for each client a buffer and snprintf to it the IP-address string.
       */
      for (cli = 0; cli < bctx_array[bi].client_num; cli++)
        {
          if (!(ip_addresses[bi][cli] = (char*)calloc (IPADDR_STR_SIZE, sizeof (char))))
            {
              fprintf (stderr, "%s - allocation of ip_addresses[%d][%d] failed\n", 
                       __func__, bi, cli) ;
              return -1;
            }
 
          /* Advance the ip-address, using client index as the offset. 
           */
          in_address.s_addr = htonl (bctx_array[bi].ip_addr_min + cli);

          if (! (dotted_ip_addr = inet_ntoa (in_address)))
            {
              fprintf (stderr, "%s - inet_ntoa() failed for ip_addresses[%d][%d]\n", 
                       __func__, bi, cli) ;
              return -1;
            }

          snprintf (ip_addresses[bi][cli], IPADDR_STR_SIZE - 1, "%s", dotted_ip_addr);
        }

      /* 
         Add all the addresses to the network interface as secondary ip-addresses.
         using netlink userland-kernel interface.
      */
      if (add_secondary_ip_addrs (bctx_array[bi].net_interface, bctx_array[bi].client_num, 
                                  (const char** const) ip_addresses[bi], bctx_array[bi].cidr_netmask) == -1)
        {
          fprintf (stderr, 
                   "%s - error: add_secondary_ip_addrs() - failed for batch = %d\n", 
                   __func__, bi);
          return -1;
        }
    }
  return 0;
}

/*
  The callback to libcurl to skip all body bites of the fetched urls.
*/
static size_t 
do_nothing_write_func (void *ptr, size_t size, size_t nmemb, void *stream)
{
  (void)ptr;
  (void)stream;

  /* 
     Overwriting the default behavior to write body bytes to stdout and 
     just skipping the body bytes without any output. 
  */
  return (size*nmemb);
}