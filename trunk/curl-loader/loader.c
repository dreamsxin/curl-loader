/* 
 *     loader.c
 *
 * 2006-2007 Copyright (c) 
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

// must be the first include
#include "fdsetsize.h"

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
#include <curl/multi.h>

// getrlimit
#include <sys/time.h>
#include <sys/resource.h>

#include "batch.h"
#include "client.h"
#include "loader.h"
#include "conf.h"
#include "ssl_thr_lock.h"


#define OPEN_FDS_SUGGESTION 10000


static int create_ip_addrs (batch_context* bctx, int bctx_num);
int client_tracing_function (CURL *handle, 
                                    curl_infotype type, 
                                    unsigned char *data, 
                                    size_t size, 
                                    void *userp);
size_t do_nothing_write_func (void *ptr, 
                                     size_t size, 
                                     size_t nmemb, 
                                     void *stream);
static void* batch_function (void *batch_data);
static int initial_handles_init (struct client_context*const cdata);
int setup_curl_handle_appl (struct client_context*const cctx,  
                                   url_context* url_ctx,
                                   int post_method);
static int alloc_init_client_post_buffers (struct client_context* cctx);
static int alloc_init_client_contexts (batch_context* bctx, FILE* output_file);
static void free_batch_data_allocations (struct batch_context* bctx);
static int ipv6_increment(const struct in6_addr *const src, 
                          struct in6_addr *const dest);

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
  struct rlimit file_limit;
  int ret;

  fprintf(stderr, " __FD_SETSIZE %d  FD_SETSIZE %d __NFDBITS %d  \n",
          __FD_SETSIZE,  FD_SETSIZE, __NFDBITS );

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

  
  ret = getrlimit(RLIMIT_NOFILE, &file_limit);

  if (!ret && file_limit.rlim_cur < OPEN_FDS_SUGGESTION)
    {
      fprintf(stderr, 
              " %s - WARNING: the current limit of open descriptors for a process is below %d."
              "Consider, increase of the limit in your shell, e.g. using ulimit -n %d command\n",
              __func__, OPEN_FDS_SUGGESTION, OPEN_FDS_SUGGESTION);
      sleep (3);
    }

  if (!ret && file_limit.rlim_cur > CURL_LOADER_FD_SETSIZE)
    {
      fprintf(stderr, 
              " %s - ERROR: The current open descriptors limit in your shell is larger then this program allows for.\n"
              "This program allows for maximum of %d file descriptors, whereas the current shell limit is %d\n"
              "If you will get notifications, like \"fd (socket) <num> is less than FD_SETSIZE\" increase\n" 
              "CURL_LOADER_FD_SETSIZE in Makefile and recompile.\n"
              "Alternatively, decrease the shell limit by e.g. #ulimit -n %d .\n",
              __func__ , CURL_LOADER_FD_SETSIZE, (int) file_limit.rlim_cur,
              CURL_LOADER_FD_SETSIZE - 1);   
    exit (-1);
  }

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
     and keep them in batch-contexts. 
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
	 fprintf (stderr, "\nExited batch_function. Look in the files:\n"
              "- %s.log for errors and traces;\n"
              "- %s.txt for loading statistics;\n"
              "- %s.ctx for the virtual client based statistics.\n"
              "You may add -v and -u options to the command line for more verbouse output to %s.log file.\n",
              bc_arr[0].batch_name, bc_arr[0].batch_name, bc_arr[0].batch_name, bc_arr[0].batch_name);
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
  FILE* log_file = 0;
  FILE* statistics_file = 0;
  
  int  rval = -1;

  if (!bctx)
    {
      fprintf (stderr, 
               "%s - error: batch_data input is zero.\n", __func__);
      return NULL;
    }
 

  if (! stderr_print_client_msg)
    {
      /*
        Init batch logfile for the batch client output 
      */
      sprintf (bctx-> batch_logfile, "./%s.log", bctx->batch_name);

      if (!(log_file = fopen(bctx-> batch_logfile, "w")))
        {
          fprintf (stderr, 
                   "%s - \"%s\" - failed to open file \"%s\" with errno %d.\n", 
                   __func__, bctx->batch_name, bctx-> batch_logfile, errno);
          return NULL;
        }
    }

  /*
    Init batch statistics file
  */
  sprintf (bctx->batch_statistics, "./%s.txt", bctx->batch_name);
      
  if (!(statistics_file = fopen(bctx->batch_statistics, "w")))
  {
      fprintf (stderr, 
               "%s - \"%s\" - failed to open file \"%s\" with errno %d.\n", 
               __func__, bctx->batch_name, bctx->batch_statistics, errno);
      return NULL;
  }
  else
  {
      bctx->statistics_file = statistics_file;
      print_statistics_header (statistics_file);
  }
  
  /* 
     Allocate and init objects, containing client-context information.
  */
  if (alloc_init_client_contexts (bctx, log_file) == -1)
    {
      fprintf (stderr, "%s - \"%s\" - failed to allocate or init client_contexts.\n", 
               __func__, bctx->batch_name);
      goto cleanup;
    }
 
  /* 
     Init libcurl MCURL and CURL handles. Setup of the handles is delayed to
     the later step, depending on whether login, UAS or logoff URL is required.
  */
  if (initial_handles_init (bctx->cctx_array) == -1)
    {
      fprintf (stderr, "%s - \"%s\" initial_handles_init () failed.\n", 
               __func__, bctx->batch_name);
      goto cleanup;
    }

  /* 
     Now run login, user-defined actions, like fetching various urls and and 
     sleeping in between, and logoff.
     Calls user activity loading function corresponding to the loading mode
     used (user_activity_smooth () or user_activity_storm () or user_activity_hyper ()).
  */ 
  rval = ua_array[loading_mode] (bctx->cctx_array);

  if (rval == -1)
    {
      fprintf (stderr, "%s - \"%s\" -user activity failed.\n", 
               __func__, bctx->batch_name);
      goto cleanup;
    }

 cleanup:
  if (bctx->multiple_handle)
    curl_multi_cleanup(bctx->multiple_handle);

  if (log_file)
      fclose (log_file);
  if (statistics_file)
      fclose (statistics_file);

  free_batch_data_allocations (bctx);

  return NULL;
}

/****************************************************************************************
* Function name - initial_handles_init
*
* Description - Libcurl initialization of curl multi-handle and the curl handles (clients), 
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
        
  return 0;
}



/****************************************************************************************
* Function name - setup_curl_handle
*
* Description - Setup for a single curl handle (client): removes a handle from multi-handle, 
*               inits it, using setup_curl_handle_init () function, and adds the
*               handle back to the multi-handle.
*
* Input -       *cctx - pointer to client context, containing CURL handle pointer;
*               *url_ctx - pointer to url-context, containing all url-related information;
*               cycle_number - current number of loading cycle, passed here for storming mode;
*               post_method - when 'true', POST method is used instead of the default GET
*
* Return Code/Output - On Success - 0, on Error -1
****************************************************************************************/
int setup_curl_handle (client_context*const cctx,
                         url_context* url_ctx,
                         long cycle_number,
                         int post_method)
{
  batch_context* bctx = cctx->bctx;
  CURL* handle = cctx->handle;
  int m_error = -1;

  if (!cctx || !url_ctx)
    {
      return -1;
    }

  /*
    Remove the handle from the multiple handle and reset it. 
    Still the handle remembers DNS, cookies, etc. 
  */

  /* TODO: check the issue */
#if 1
  if ((m_error = curl_multi_remove_handle (bctx->multiple_handle, 
                                           handle)) != CURLM_OK)
    {
      fprintf (stderr,"%s - error: curl_multi_remove_handle () failed with error %d.\n",
               __func__, m_error);
      return -1;
    }
#endif
  
  if (setup_curl_handle_init (cctx, url_ctx, cycle_number, post_method) == -1)
  {
      fprintf (stderr,"%s - error: failed.\n",__func__);
      return -1;
  }
     
  /* The handle is supposed to be removed before. */
#if 1
  if ((m_error = curl_multi_add_handle(bctx->multiple_handle, handle)) != CURLM_OK)
    {
      fprintf (stderr,"%s - error: curl_multi_add_handle () failed with error %d.\n",
               __func__, m_error);
          return -1;
    }
#endif

  return 0;
}

/****************************************************************************************
* Function name - setup_curl_handle_init
*
* Description - Resets client context kept CURL handle and inits it locally, using 
*               setup_curl_handle_appl () function for the application-specific 
*               (HTTP/FTP) initialization.
*
* Input -       *cctx - pointer to client context, containing CURL handle pointer;
*               *url_ctx - pointer to url-context, containing all url-related information;
*               cycle_number - current number of loading cycle, passed here for storming mode;
*               post_method - when 'true', POST method is used instead of the default GET
*
* Return Code/Output - On Success - 0, on Error -1
****************************************************************************************/
int setup_curl_handle_init (client_context*const cctx,
                         url_context* url_ctx,
                         long cycle_number,
                         int post_method)
{
  batch_context* bctx = cctx->bctx;
  CURL* handle = cctx->handle;

  if (!cctx || !url_ctx)
    {
      return -1;
    }

  curl_easy_reset (handle);

  if (bctx->ipv6)
    curl_easy_setopt (handle, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V6);
      
  /* Bind the handle to a certain IP-address */
  curl_easy_setopt (handle, CURLOPT_INTERFACE, 
                    bctx->ip_addr_array [cctx->client_index]);

  curl_easy_setopt (handle, CURLOPT_NOSIGNAL, 1);
    
  /* Set the url */
  if (url_ctx->url_str && url_ctx->url_str[0])
    {
      curl_easy_setopt (handle, CURLOPT_URL, url_ctx->url_str);
    }
  else
    {
      fprintf (stderr,"%s - error: empty url provided.\n",
                   __func__);
      exit (-1);
    }
  
  /* Set the index to client for the smooth-mode */
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
     This is to return cctx pointer as the void* userp to the 
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
     
  return 0;
}
  
/****************************************************************************************
* Function name - setup_curl_handle_appl
*
* Description - Application/url-type specific setup for a single curl handle (client)
*
* Input -       *cctx - pointer to client context, containing CURL handle pointer;
*               *url_ctx - pointer to url-context, containing all url-related information;
*               post_method - when 'true', POST method is used instead of the default GET
*
* Return Code/Output - On Success - 0, on Error -1
****************************************************************************************/
int setup_curl_handle_appl (client_context*const cctx,  
                                   url_context* url_ctx,
                                   int post_method)
{
    batch_context* bctx = cctx->bctx;
    CURL* handle = cctx->handle;
    
    cctx->is_https = (url_ctx->url_appl_type == URL_APPL_HTTPS);
    
    if (url_ctx->url_appl_type == URL_APPL_HTTPS ||
        url_ctx->url_appl_type == URL_APPL_HTTP)
    {
        
         /* HTTP-specific initialization */
        
      /* 
         Follow possible HTTP-redirection from header Location of the 
         3xx HTTP responses, like 301, 302, 307, etc. It also updates the url, 
         thus no need to parse header Location. Great job done by the libcurl 
         people.
      */
      curl_easy_setopt (handle, CURLOPT_FOLLOWLOCATION, 1);
      curl_easy_setopt (handle, CURLOPT_UNRESTRICTED_AUTH, 1);
      
      /* Enable infinitive (-1) redirection number. */
      curl_easy_setopt (handle, CURLOPT_MAXREDIRS, -1);

      /* 
         Setup the User-Agent header, configured by user. The default is MSIE-6 header.
       */
      curl_easy_setopt (handle, CURLOPT_USERAGENT, bctx->user_agent);

      /*
        Setup the custom HTTP headers, if appropriate.
      */
      if (bctx->custom_http_hdrs && bctx->custom_http_hdrs_num)
        {
          curl_easy_setopt (handle, CURLOPT_HTTPHEADER, 
                            bctx->custom_http_hdrs);
        }
      
      /* Enable cookies. This is important for various authentication schemes. */
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

      if (url_ctx->username[0])
        {
          char userpwd[256];
          sprintf (userpwd, "%s:%s", url_ctx->username, url_ctx->password);

          curl_easy_setopt(handle, CURLOPT_USERPWD, userpwd);
          curl_easy_setopt(handle, CURLOPT_HTTPAUTH, CURLAUTH_ANY);
          
          curl_easy_setopt(handle, CURLOPT_PROXYUSERPWD, userpwd);
          curl_easy_setopt(handle, CURLOPT_PROXYAUTH, CURLAUTH_ANY);
        }
    }
  else
    {
      /* FTP-specific setup. Place here */
    }

  return 0;
}

/****************************************************************************************
* Function name - client_tracing_function
* 
* Description - Used to log activities of each client to the <batch_name>.log file
*
* Input -       *handle - pointer to CURL handle;
*               type - type of libcurl information passed, like headers, data, info, etc
*               *data- pointer to data, like headers, etc
*               size - number of bytes passed with <data> pointer
*               *userp - pointer to user-specific data, which in our case is the 
*                        client_context structure
*
* Return Code/Output - On Success - 0, on Error -1
****************************************************************************************/
int client_tracing_function (CURL *handle, 
                                    curl_infotype type, 
                                    unsigned char *data, 
                                    size_t size, 
                                    void *userp)
{
  client_context* cctx = (client_context*) userp;
  char*url_target = NULL, *url_effective = NULL;
#if 0
  char buf[300];
  int n;

  n = snprintf(buf,sizeof(buf)-1,"->client_tracing_function cctx=%p size=%d | %s",cctx,size,data);
  buf[n] = '\0';
  printf("%s\n",buf);
#endif  

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
      first_hdrs_clear_all (cctx);
      break;

    case CURLINFO_HEADER_OUT:
      if (verbose_logging)
          fprintf(cctx->file_output, "%ld %s => Send header: eff-url: %s, url: %s\n", 
                  cctx->cycle_num, cctx->client_name, url_print ? url : "", url_diff ? url_target : "");

      stat_data_out_add (cctx, (unsigned long) size);

      if (! first_hdr_req (cctx))
        {
          /* First header of the HTTP-request. */
          first_hdr_req_inc (cctx);
          stat_req_inc (cctx); /* Increment number of requests */
          cctx->req_sent_timestamp = get_tick_count ();
        }
      first_hdrs_clear_non_req (cctx);
      break;

    case CURLINFO_DATA_OUT:
      if (verbose_logging)
          fprintf(cctx->file_output, "%ld %s => Send data: eff-url: %s, url: %s\n", 
                  cctx->cycle_num, cctx->client_name, url_print ? url : "",
                  url_diff ? url_target : "");

      stat_data_out_add (cctx, (unsigned long) size);
      first_hdrs_clear_all (cctx);
      break;

    case CURLINFO_SSL_DATA_OUT:
      if (verbose_logging) 
          fprintf(cctx->file_output, "%ld %s => Send ssl data: eff-url: %s, url: %s\n", 
                  cctx->cycle_num, cctx->client_name, url_print ? url : "",
                  url_diff ? url_target : "");

      stat_data_out_add (cctx, (unsigned long) size);
      first_hdrs_clear_all (cctx);
      break;
      
    case CURLINFO_HEADER_IN:
      /* 
         CURL library assists us by passing here the full HTTP-header, 
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
            first_hdrs_clear_all (cctx);
            break;

          case 2: /* 200 OK */

            if (! first_hdr_2xx (cctx))
              {
                if (verbose_logging)
                    fprintf(cctx->file_output, "%ld %s:!! %ld OK: eff-url: %s, url: %s\n",
                            cctx->cycle_num, cctx->client_name, response_status,
                            url_print ? url : "", url_diff ? url_target : "");

                /* First header of 2xx response */
                first_hdr_2xx_inc (cctx);
                stat_2xx_inc (cctx); /* Increment number of 2xx responses */

                /* Count into the averages HTTP/S server response delay */
                const unsigned long time_2xx_resp = get_tick_count ();
                stat_appl_delay_2xx_add (cctx, time_2xx_resp);
                stat_appl_delay_add (cctx, time_2xx_resp);
              }
            first_hdrs_clear_non_2xx (cctx);
            break;
       
          case 3: /* 3xx REDIRECTIONS */

            if (! first_hdr_3xx (cctx))
              {
                fprintf(cctx->file_output, "%ld %s:!! %ld REDIRECTION: %s: eff-url: %s, url: %s\n", 
                    cctx->cycle_num, cctx->client_name, response_status, data,
                    url_print ? url : "", url_diff ? url_target : "");

                /* First header of 3xx response */
                first_hdr_3xx_inc (cctx);
                stat_3xx_inc (cctx); /* Increment number of 3xx responses */
                const unsigned long time_3xx_resp = get_tick_count ();
                stat_appl_delay_add (cctx, time_3xx_resp);
              }
            first_hdrs_clear_non_3xx (cctx);
            break;

          case 4: /* 4xx Client Error */

              if (! first_hdr_4xx (cctx))
              {
                fprintf(cctx->file_output, "%ld %s :!! %ld CLIENT_ERROR : %s: eff-url: %s, url: %s\n", 
                      cctx->cycle_num, cctx->client_name, response_status, data,
                      url_print ? url : "", url_diff ? url_target : "");

                /*
                  We are not marking client on 401 and 407 as coming to the
                  error state, because the responses are just authentication 
                  challenges, that virtual client may overcome.
                */
                if (response_status != 401 || response_status != 407)
                  {
                    cctx->client_state = CSTATE_ERROR;
                  }

                /* First header of 4xx response */
                first_hdr_4xx_inc (cctx);
                stat_4xx_inc (cctx);  /* Increment number of 4xx responses */

                const unsigned long time_4xx_resp = get_tick_count ();
                stat_appl_delay_add (cctx, time_4xx_resp);
              }
             first_hdrs_clear_non_4xx (cctx);
             break;

          case 5: /* 5xx Server Error */

            if (! first_hdr_5xx (cctx))
              {
                fprintf(cctx->file_output, "%ld %s :!! %ld SERVER_ERROR : %s: eff-url: %s, url: %s\n", 
                    cctx->cycle_num, cctx->client_name, response_status, data,
                    url_print ? url : "", url_diff ? url_target : "");

                cctx->client_state = CSTATE_ERROR;

                /* First header of 5xx response */
                first_hdr_5xx_inc (cctx);
                stat_5xx_inc (cctx);  /* Increment number of 5xx responses */

                const unsigned long time_5xx_resp = get_tick_count ();
                stat_appl_delay_add (cctx, time_5xx_resp);
              }
            first_hdrs_clear_non_5xx (cctx);
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
      first_hdrs_clear_all (cctx);
      break;

    case CURLINFO_SSL_DATA_IN:
      if (verbose_logging) 
          fprintf(cctx->file_output, "%ld %s <= Recv ssl data: eff-url: %s, url: %s\n", 
                  cctx->cycle_num, cctx->client_name, 
                  url_print && url ? url : "", url_diff ? url_target : "");

      stat_data_in_add (cctx,  (unsigned long) size);
      first_hdrs_clear_all (cctx);
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
* Description - Allocate and initialize post form buffers to be used for POST-ing
* Input -       *cctx - pointer to client context
*
* Return Code/Output - On Success - 0, on Error -1
****************************************************************************************/
static int alloc_init_client_post_buffers (client_context* cctx)
{
  int i;
  batch_context* bctx = cctx->bctx;

  if (! bctx->login_post_str[0])
    {
      if (bctx->login_req_type == LOGOFF_REQ_TYPE_GET)
        {
          return 0;
        }
      else
        {
          fprintf (stderr,
                   "%s - error: %s - LOGIN_POST_STR not defined.\n",
                   __func__, bctx->batch_name);
          return -1;
        }
    }

  if (bctx->login_credentials_file)
    {
      /* 
         When we are loading users with passwords credentials from a file
         (tag LOGIN_CREDENTIALS_FILE, this is done in post_validation () of 
         parce_conf.c.
      */
      return 0;
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

      if (bctx->login_post_str_usertype ==  
          POST_STR_USERTYPE_UNIQUE_USERS_AND_PASSWORDS)
        {
          /* 
             For each client init post buffer, containing username and password 
             with uniqueness added via added to the base username and password
             client index.
          */
          snprintf (cctx[i].post_data_login, 
                    POST_LOGIN_BUF_SIZE, 
                    bctx->login_post_str,
                    bctx->login_url.username, 
                    i + 1,
                    bctx->login_url.password[0] ? bctx->login_url.password : "",
                    i + 1);
        }
      else if (bctx->login_post_str_usertype ==  
          POST_STR_USERTYPE_UNIQUE_USERS_SAME_PASSWORD)
        {
          /* 
             For each client init post buffer, containing username with uniqueness 
             added via added to the base username client index. Password is kept
             the same for all users.
          */
          snprintf (cctx[i].post_data_login, 
                    POST_LOGIN_BUF_SIZE, 
                    bctx->login_post_str,
                    bctx->login_url.username, 
                    i + 1,
                    bctx->login_url.password[0] ? bctx->login_url.password : "");
        }
      else if ((bctx->login_post_str_usertype ==  
                POST_STR_USERTYPE_SINGLE_USER) ||
               (bctx->login_post_str_usertype ==  
                POST_STR_USERTYPE_LOAD_USERS_FROM_FILE))
        {
          /* All clients have the same login_username and password.*/
          snprintf (cctx[i].post_data_login, 
                    POST_LOGIN_BUF_SIZE, 
                    bctx->login_post_str,
                    bctx->login_url.username, 
                    bctx->login_url.password[0] ? bctx->login_url.password : "");
        }
      else
        {
          fprintf (stderr,
                   "\"%s\" error: none valid bctx->post_str_usertype.\n", __func__) ;
          return -1;
        }
      
      if (bctx->logoff_post_str[0])
        {
            if (! (cctx[i].post_data_logoff = 
                   (char *) calloc(POST_LOGOFF_BUF_SIZE, sizeof (char))))
            {
                fprintf (stderr,
                         "%s - error: %s - failed to allocate post login buffer.\n",
                         __func__, bctx->batch_name);
                return -1;
            }

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
* Input -       *bctx - pointer to batch context to be set to all clients  of the batch
*               *log_file - output file to be used by all clients of the batch
*
* Return Code -  On Success - 0, on Error -1
****************************************************************************************/
static int alloc_init_client_contexts (
                                       batch_context* bctx,
                                       FILE* log_file)
{
  int i;

  /*
    Allocate client contexts, if not allocated before. When users and passwords 
    are loaded from a credentials files (tag LOGIN_CREDENTIALS_FILE), the array 
    is allocated already in post_validate () of parse_conf.c.
  */
  if (!bctx->cctx_array)
    {
      if (!(bctx->cctx_array  = (client_context *) calloc(bctx->client_num, 
                                              sizeof (client_context))))
        {
          fprintf (stderr, "\"%s\" - %s - failed to allocate cctx.\n", 
                   bctx->batch_name, __func__);
          return -1;
        }
    }

  /* 
     Iterate through client contexts and initialize them. 
  */
  for (i = 0 ; i < bctx->client_num ; i++)
    {
      /* 
         Set the timer handling function, which is used by the smooth 
         loading mode. 
      */
      set_timer_handling_func (&bctx->cctx_array[i], handle_cctx_timer);

      /* 
         Build client name for logging, based on sequence number and 
         ip-address for each simulated client. 
      */
      bctx->cctx_array[i].cycle_num = 0;

      snprintf(bctx->cctx_array[i].client_name, 
               sizeof(bctx->cctx_array[i].client_name) - 1, 
               "%d (%s) ", 
               i + 1, 
               bctx->ip_addr_array[i]);

       /* 
         Set index of the client within the batch.
         Useful to get the client's CURL handle from bctx. 
      */
      bctx->cctx_array[i].client_index = i;
      bctx->cctx_array[i].uas_url_curr_index = 0; /* Actually zeroed by calloc. */
      
      /* Set output stream for each client to be either batch logfile or stderr. */
      bctx->cctx_array[i].file_output = stderr_print_client_msg ? stderr : log_file;

      /* 
         Set pointer in client to its batch object. The pointer will be used to get 
         configuration and set back statistics to batch.
      */
      bctx->cctx_array[i].bctx = bctx;
    }

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

  /* 
     Free client contexts 
  */
  if (bctx->cctx_array)
    {
      for (i = 0 ; i < bctx->client_num ; i++)
        {
          if (bctx->cctx_array[i].handle)
            curl_easy_cleanup(bctx->cctx_array[i].handle);
          
          /* Free POST-buffers */
          
          if (bctx->cctx_array[i].post_data_login)
            free (bctx->cctx_array[i].post_data_login);
          if (bctx->cctx_array[i].post_data_logoff)
            free (bctx->cctx_array[i].post_data_logoff);
        }

      free(bctx->cctx_array);
      bctx->cctx_array = NULL;
    }
  
  /* Free custom HTTP headers. */
  if (bctx->custom_http_hdrs)
    {
      curl_slist_free_all(bctx->custom_http_hdrs);
      bctx->custom_http_hdrs = NULL;
    }


  /* Free login credentials. */
  free (bctx->login_credentials_file);
  bctx->login_credentials_file = NULL;

  /* Free the login and logoff urls */
  free (bctx->login_url.url_str);
  bctx->login_url.url_str = NULL;

  free (bctx->logoff_url.url_str);
  bctx->logoff_url.url_str = NULL;

  free (bctx->login_credentials_file);

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
  int batch_index, client_index; /* Batch and client indexes */
  struct in_addr in_address;
  struct in6_addr in6_prev, in6_new;
  char*** ip_addresses =0;
  char* ipv4_string = 0;
  char ipv6_string[INET6_ADDRSTRLEN+1];

  /* Add secondary IP-addresses to the "loading" network interface. */
  if (!(ip_addresses = (char***)calloc (bctx_num, sizeof (char**))))
    {
      fprintf (stderr, "%s - error: failed to allocate ip_addresses.\n", __func__);
      return -1;
    }
  
  for (batch_index = 0 ; batch_index < bctx_num ; batch_index++) 
    {
      /* Allocate array of IP-addresses */
      if (!(ip_addresses[batch_index] = (char**)calloc (bctx_array[batch_index].client_num, 
                                               sizeof (char *))))
        {
          fprintf (stderr, 
                   "%s - error: failed to allocate array of ip-addresses for batch %d.\n", 
                   __func__, batch_index);
          return -1;
        }

      /* Set them to the batch contexts to remember them. */
      bctx_array[batch_index].ip_addr_array = ip_addresses[batch_index]; 

      /* Allocate for each client a buffer and snprintf to it the IP-address string.
       */
      for (client_index = 0; client_index < bctx_array[batch_index].client_num; client_index++)
        {
          if (!(ip_addresses[batch_index][client_index] = 
                (char*)calloc (bctx_array[batch_index].ipv6 ? 
                               INET6_ADDRSTRLEN + 1 : INET_ADDRSTRLEN + 1, sizeof (char))))
            {
              fprintf (stderr, "%s - allocation of ip_addresses[%d][%d] failed\n", 
                       __func__, batch_index, client_index) ;
              return -1;
            }

          if (bctx_array[batch_index].ipv6 == 0)
            {
              /* 
                 Advance the IPv4-address, using client index as the offset. 
              */
              in_address.s_addr = htonl (bctx_array[batch_index].ip_addr_min + client_index);
              if (! (ipv4_string = inet_ntoa (in_address)))
                {
                  fprintf (stderr, "%s - inet_ntoa() failed for ip_addresses[%d][%d]\n", 
                           __func__, batch_index, client_index) ;
                  return -1;
                }
            }
          else
            {
              /* 
                 Advance the IPv6-address by incrementing previous address. 
              */
              if (client_index == 0)
                {
                  memcpy (&in6_prev, &bctx_array[batch_index].ipv6_addr_min, sizeof (in6_prev));
                  memcpy (&in6_new, &bctx_array[batch_index].ipv6_addr_min, sizeof (in6_new));
                }
              else
                {
                  if (ipv6_increment (&in6_prev, &in6_new) == -1)
                    {
                      fprintf (stderr, "%s - ipv6_increment() failed for ip_addresses[%d][%d]\n", 
                               __func__, batch_index, client_index) ;
                      return -1;
                    }
                }

              if (!inet_ntop (AF_INET6, &in6_new, ipv6_string, sizeof (ipv6_string)))
                {
                  fprintf (stderr, "%s - inet_ntoa() failed for ip_addresses[%d][%d]\n", 
                           __func__, batch_index, client_index) ;
                  return -1;
                }
              else
                {
                  /* Remember in6_new address in in6_prev */
                  memcpy (&in6_prev, &in6_new, sizeof (in6_prev));
                }
            }

          snprintf (ip_addresses[batch_index][client_index], 
                    bctx_array[batch_index].ipv6 ? INET6_ADDRSTRLEN : INET_ADDRSTRLEN, 
                    "%s", 
                    bctx_array[batch_index].ipv6 ? ipv6_string : ipv4_string);
        }

      /* 
         Add all the addresses to the network interface as the secondary 
         ip-addresses, using netlink userland-kernel interface.
      */
      if (add_secondary_ip_addrs (bctx_array[batch_index].net_interface,
                                  bctx_array[batch_index].client_num, 
                                  (const char** const) ip_addresses[batch_index], 
                                  bctx_array[batch_index].cidr_netmask,
                                  bctx_array[batch_index].scope) == -1)
        {
          fprintf (stderr, 
                   "%s - error: add_secondary_ip_addrs() - failed for batch = %d\n", 
                   __func__, batch_index);
          return -1;
        }
    }
  return 0;
}

/*
  The callback to libcurl to skip all body bytes of the fetched urls.
*/
size_t 
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

/****************************************************************************************
* Function name - rewind_logfile_above_maxsize
*
* Description - Rewinds the file pointer, when reaching configurable max-size
* Input -       *filepointer - file pointer to control and rewind, when necessary
*
* Return Code/Output - On Success - 0, on Error -1
****************************************************************************************/
int rewind_logfile_above_maxsize (FILE* filepointer)
{
  long position = -1;

  if (!filepointer)
    return -1;

  if (filepointer == stderr)
    return 0;

  if ((position = ftell (filepointer)) == -1)
    {
      fprintf (stderr, 
               "%s - error: ftell () failed with errno = %d\n", __func__, errno);
      return 0;
    }

  if (position > (logfile_rewind_size* 1024*1024))
    {
      rewind (filepointer);
      fprintf (stderr, "%s - logfile with size %ld rewinded.\n", __func__, position);
    }

  return 0;
}

/****************************************************************************************
* Function name - ipv6_increment
*
* Description - Increments the source IPv6 address to provide the next
* Input -       *src - pointer to the IPv6 address to be used as the source
* Input/Output    *dest - pointer to the resulted incremented address
*
* Return Code/Output - On Success - 0, on Error -1
****************************************************************************************/
/* store 'src + 1' in dest, and check that dest remains in the same scope as src */
static int ipv6_increment(const struct in6_addr *const src, 
                          struct in6_addr *const dest)
{
  uint32_t temp = 1;
  int i;

  for (i = 15; i > 1; i--) 
    {
      temp += src->s6_addr[i];
      dest->s6_addr[i] = temp & 0xff;
      temp >>= 8;
    }

  if (temp != 0)
    {
      fprintf (stderr, "%s - error: passing the scope.\n "
               "Check you IPv6 range.\n", __func__);
      return -1;
    }

  dest->s6_addr[1] = src->s6_addr[1];
  dest->s6_addr[0] = src->s6_addr[0];

  return 0;
}
