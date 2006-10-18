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
  as well as in libcurl. Options to consider are poll () and /dev/epoll.
*/
#define MAX_FD_IN_THREAD 1000


static int create_ip_addrs (batch_context* bctx, int bctx_num);
static int client_tracing_function (CURL *handle, curl_infotype type, 
                                    unsigned char *data, size_t size, void *userp);
static size_t do_nothing_write_func (void *ptr, size_t size, size_t nmemb, 
                                     void *stream);
static void* batch_function (void *batch_data);
static int initial_handles_init (struct client_context*const cdata);
static int alloc_init_client_post_buffers (struct client_context* cctx);
static int alloc_init_client_contexts (client_context** p_cctx, 
                                       batch_context* bctx, FILE* output_file);
static void free_batch_data_allocations (struct batch_context* bctx);

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
                       "main: error: failed parsing of the command line.\n");
      return -1;
    }

  if (geteuid())
    {
      fprintf (stderr, 
               "main: error: lacking root preveledges to run this program.\n");
      return -1;
    }

  memset(bc_arr, 0, sizeof(bc_arr));

  /* Parse the configuration file. */
  if ((batches_num = parse_config_file (config_file, bc_arr, 
                                        sizeof(bc_arr)/sizeof(*bc_arr))) <= 0)
    {
      fprintf (stderr, "main: parse_config_file () - error.\n");
      return -1;
    }
   
  /* Add ip-addresses to the loading network interfaces
     and keep the ip-addr of each loading client in batch-contexts. */
  if (create_ip_addrs (bc_arr, batches_num) == -1)
    {
      fprintf (stderr, "main: create_ip_addrs () - error.\n");
      return -1;
    }

  fprintf (stderr, "%s - completed adding IP addresses.\n", 
           __func__);
  
  if (! threads_run)
    {
      fprintf (stderr, "\nRUNNING WITHOUT THREADS\n\n");
      sleep (1) ;
      batch_function (&bc_arr[0]);
    }
  else
    {
      fprintf (stderr, "\nSTARTING THREADS\n\n");
      sleep (1);
      
      /* Init openssl mutexes and pass two callbacks to openssl. */
        if (thread_openssl_setup () == -1)
        {
          fprintf (stderr, "thread_setup () - failed.\n");
           return -1;
        }
      
      /* Opening threads for the batches of clients */
      for (i = 0 ; i < batches_num ; i++) 
        {
          error = pthread_create (&tid[i], NULL, batch_function, &bc_arr[i]);

          if (0 != error)
            fprintf(stderr, "Couldn't run thread number %d, errno %d\n", i, error);
          else 
            fprintf(stderr, "Thread %d, starts normally\n", i);
        }

      /* Waiting for all the threads to terminate */
      for (i = 0 ; i < batches_num ; i++) 
        {
          error = pthread_join (tid[i], NULL) ;
          fprintf(stderr, "Thread %d terminated normally\n", i) ;
        }

      thread_openssl_cleanup ();
    }
   
  return 0;
}

/*
  Runs the batch test. Prepared to be used as a thread function.
*/
static void* batch_function (void * batch_data)
{
  batch_context* bctx = (batch_context *) batch_data;
  client_context* cctx = NULL;
  FILE* output_file = 0;
  char output_file_name[BATCH_NAME_SIZE+4];
  int  i = 0, rval = -1;

  if (! stderr_print_client_msg)
    {
      /* Init batch logfile for the batch clients output */
      sprintf (output_file_name, "%s.log", bctx->batch_name);

      if (!(output_file = fopen(output_file_name, "w")))
        {
          fprintf (stderr, 
                   "%s - \"%s\" - failed to open file \"%s\" with errno %d.\n", 
                     __func__, bctx->batch_name, output_file_name, errno);
          return NULL;
        }
    }
  
  if (alloc_init_client_contexts (&cctx, bctx, output_file) == -1)
    {
      fprintf (stderr, "%s - \"%s\" - failed to allocate or init cctx.\n", 
               __func__, bctx->batch_name);
      goto cleanup;
    }
 
  /* Init CURL handles taking care about each individual connection. 
   */
  if (initial_handles_init (cctx) == -1)
    {
      fprintf (stderr, "%s - \"%s\" initial_handles_init () failed.\n", 
               __func__, bctx->batch_name);
      goto cleanup;
    }

  /* 
     Now run the long cycles of the user-defined actions, like 
     fetching various urls and and sleeping in between.
  */
  if (loading_mode == LOAD_MODE_STORMING)
    {
       /* MODE_LOAD_STORM */
      rval = user_activity_storm (cctx);
    }
  else /* LOAD_MODE_SMOOTH */
    { 
      rval = user_activity_smooth (cctx);
    }

  if (rval == -1)
    {
      fprintf (stderr, "%s - \"%s\" -user_activity_storm() failed.\n", 
               __func__, bctx->batch_name);
      goto cleanup;
    }

 cleanup:

  if (bctx->multiple_handle)
      curl_multi_cleanup(bctx->multiple_handle);

  for (i = 0 ; i < bctx->client_num ; i++)
    {
      if (bctx->client_handles_array[i])
        curl_easy_cleanup(bctx->client_handles_array[i]);

      /* Free POST-buffers */
      if (cctx[i].post_data_login)
          free (cctx[i].post_data_login);
      if (cctx[i].post_data_logoff)
          free (cctx[i].post_data_logoff);
    }

  free(cctx);
  if (output_file)
      fclose (output_file);
  free_batch_data_allocations (bctx);

  return NULL;
}

/*
  Initial initialization of curl multi-handle and 
  the curl handles (clients), used in the batch.
*/
static int initial_handles_init (client_context*const cdata)
{
  batch_context* bctx = cdata->bctx;
  int k = 0;
  int auth_login = 0;

  if (! (bctx->multiple_handle = curl_multi_init()) )
    {
      fprintf (stderr, 
               "\"%s\" -%s () - failed to initialize bctx->multiple_handle .\n", 
               bctx->batch_name, __func__) ;
      return -1;
    }

    if (bctx->do_auth && post_login_format[0])
    {
      if (alloc_init_client_post_buffers (cdata) == -1)
        {
          fprintf (stderr, "\"%s\" - alloc_client_post_buffers ().\n", __func__);
          return -1;
        }
      else
        {
          auth_login = 1;
        }
    }

  for (k = 0 ; k < bctx->client_num ; k++)
    {
      /* Initialize all the CURL handlers */
      if (!(bctx->client_handles_array[k] = curl_easy_init ()))
        {
          fprintf (stderr,
                   "%s - failed to init bctx->client_handles_array for k = %d.\n",
                   __func__, k);
          return -1;
        }
      else
        {
          bctx->curl_handlers_count++;
        }

      single_handle_setup (
                           &cdata[k], /* pointer to client context */
                           0, /* start from the first (zero index) url */
                           0, /* zero cycle */
                           NULL /* no post buffers*/  
                           );
    }

  return 0;
}


/*
  Setup for a single curl handle (client).
*/
int single_handle_setup (
                         client_context*const cctx,
                         size_t url_num, 
                         long cycle_number,
                         char* post_buff)
{
  batch_context* bctx = cctx->bctx;
  CURL* handle = bctx->client_handles_array[cctx->client_index];
  char* url = bctx->url_ctx_arr[url_num].url_str;

#define  HTTPS_S "http://" 
  cctx->is_https = (strncmp (url, HTTPS_S, sizeof(HTTPS_S)) == 0);

  curl_easy_reset (handle);

  curl_easy_setopt (handle, CURLOPT_INTERFACE, 
                    bctx->ip_addr_array [cctx->client_index]);

  curl_easy_setopt (handle, CURLOPT_NOSIGNAL, 1);
    
  /* set the url */
  curl_easy_setopt (handle, CURLOPT_URL, url);
  cctx->url_curr_index = url_num; /* set the index to client for smooth loading */
  

  curl_easy_setopt (handle, CURLOPT_DNS_CACHE_TIMEOUT, -1);

  /* set the connection timeout */
  curl_easy_setopt (handle, CURLOPT_CONNECTTIMEOUT, connect_timeout);

  /* define the connection re-use policy. When passed 1, dont re-use */
  curl_easy_setopt (handle, CURLOPT_FRESH_CONNECT, reuse_connection_forbidden);

  /* If DNS resolving is necesary, global DNS cache is enough,
     otherwise compile libcurl with ares (cares) library support.

     Attention: DNS global cache is not thread-safe, therefore use
     cares for asynchronous DNS lookups.
  */
  /* curl_easy_setopt (handle, CURLOPT_DNS_USE_GLOBAL_CACHE, 1); */
    
  /* 
     Follow possible HTTP-redirection from header Location of the 
     3xx HTTP responses, like 301, 302, 307, etc. It also updates the url 
     in the options, so you do not need to parse headers and extract the 
     value of header Location. Great job done by the libcurl people.
  */
  curl_easy_setopt (handle, CURLOPT_FOLLOWLOCATION, 1);

  /* Enable infinitive redirections number. */
  curl_easy_setopt (handle, CURLOPT_MAXREDIRS, -1);

#define EXPLORER_USERAGENT_STR "Mozilla/4.0 (compatible; MSIE 6.0; Windows NT 5.0)" 
  /* Lets be Explorer-6, but actually User-Agent header should be configurable */
  curl_easy_setopt (handle, CURLOPT_USERAGENT, EXPLORER_USERAGENT_STR);

  /* Enable cookies. This is important for verious authentication schemes. */
   curl_easy_setopt (handle, CURLOPT_COOKIEFILE, "");


  curl_easy_setopt (handle, CURLOPT_VERBOSE, 1);
  curl_easy_setopt (handle, CURLOPT_DEBUGFUNCTION, 
                    client_tracing_function);

  if (!output_to_stdout)
    curl_easy_setopt (handle, CURLOPT_WRITEFUNCTION,
                      do_nothing_write_func);

  curl_easy_setopt (handle, CURLOPT_SSL_VERIFYPEER, 0);
  curl_easy_setopt (handle, CURLOPT_SSL_VERIFYHOST, 0);
    
  /* Set current cycle_number in buffer. */
  if (loading_mode == LOAD_MODE_STORMING)
    cctx->cycle_num = cycle_number;

  curl_easy_setopt (handle, CURLOPT_DEBUGDATA, cctx);

  /* Without the buffer set, we do not get any errors in tracing function. */
  curl_easy_setopt (handle, CURLOPT_ERRORBUFFER, bctx->error_buffer);

  if (bctx->do_auth && post_buff)
    curl_easy_setopt(handle, CURLOPT_POSTFIELDS, post_buff);

  bctx->url_index = url_num;
     
  curl_multi_add_handle(bctx->multiple_handle, handle);

  return 0;
}

/*
  Used to log activity for each individual session. 
*/
static int client_tracing_function (CURL *handle, curl_infotype type, 
                         unsigned char *data, size_t size, void *userp)
{
  client_context* cd = (client_context*) userp;

  switch (type)
    {
    case CURLINFO_TEXT:
      if (verbose_logging)
        {
          fprintf(cd->file_output, "%ld %s %s :== Info: %s", 
                  cd->cycle_num, cd->client_name, 
                  url_logging ? 
                  cd->bctx->url_ctx_arr[cd->url_curr_index].url_str : "", data);
        }
      break; 

    case CURLINFO_ERROR:
       fprintf(cd->file_output, "%ld %s %s   !! ERROR: %s", 
               cd->cycle_num, cd->client_name, 
               url_logging ?
               cd->bctx->url_ctx_arr[cd->url_curr_index].url_str : "", data);

       cd->client_state = STATE_ERROR;
      break;

    case CURLINFO_HEADER_OUT:
      if (verbose_logging)
        {
          fprintf(cd->file_output, "%ld %s %s => Send header\n", 
                  cd->cycle_num, cd->client_name,
                  url_logging ?
                  cd->bctx->url_ctx_arr[cd->url_curr_index].url_str : "");
        }

      if (cd->is_https)
	  cd->bctx->https_delta.data_out += (unsigned long) size; 
	else 
	  cd->bctx->http_delta.data_out += (unsigned long) size;
      break;

    case CURLINFO_DATA_OUT:
      if (verbose_logging)
        {
          fprintf(cd->file_output, "%ld %s %s => Send data\n", 
                  cd->cycle_num, cd->client_name,
                  url_logging ?
                  cd->bctx->url_ctx_arr[cd->url_curr_index].url_str : "");
        }
      cd->bctx->http_delta.data_out += (unsigned long) size;
      break;

    case CURLINFO_SSL_DATA_OUT:
      if (verbose_logging) 
        {
          fprintf(cd->file_output, "%ld %s %s => Send ssl data\n", 
                  cd->cycle_num, cd->client_name,
                  url_logging ?
                  cd->bctx->url_ctx_arr[cd->url_curr_index].url_str : "");
        }
      cd->bctx->https_delta.data_out += (unsigned long) size;
      break;
      
    case CURLINFO_HEADER_IN:
      /* 
         CURL library assists us by passing whole HTTP-headers, not just parts. 
      */
      if (cd->is_https)
        cd->bctx->https_delta.data_in += (unsigned long) size; 
      else 
        cd->bctx->http_delta.data_in += (unsigned long) size;

      {
        long response_status = 0, response_module = 0;
        
        if (verbose_logging)
          fprintf(cd->file_output, "%ld %s %s <= Recv header\n", 
                  cd->cycle_num, cd->client_name, 
                  url_logging ? 
                  cd->bctx->url_ctx_arr[cd->url_curr_index].url_str : "");
        
        curl_easy_getinfo (handle, CURLINFO_RESPONSE_CODE, &response_status);
        response_module = response_status / (long)100;
        
        switch (response_module)
          {
          case 1: /* 100-Continue and 101 responses */
            if (verbose_logging)
              fprintf(cd->file_output, "%ld %s:!! 100CONTINUE\n", 
                      cd->cycle_num, cd->client_name);
            break;  
          case 2: /* 200 OK */
            if (verbose_logging)
              fprintf(cd->file_output, "%ld %s:!! OK\n",
                      cd->cycle_num, cd->client_name);
            break;       
          case 3: /* 3xx REDIRECTIONS */
            fprintf(cd->file_output, "%ld %s:!! REDIRECTION: %s\n", 
                    cd->cycle_num, cd->client_name, data);
            break;
          case 4: /* 4xx Client Error */
            fprintf(cd->file_output, "%ld %s %s :!! CLIENT_ERROR(%ld): %s\n", 
                    cd->cycle_num, cd->client_name, 
                    url_logging ?
                    cd->bctx->url_ctx_arr[cd->url_curr_index].url_str : "",
                    response_status, data);
            break;
          case 5: /* 5xx Server Error */
            fprintf(cd->file_output, "%ld %s %s :!! SERVER_ERROR(%ld): %s\n", 
                    cd->cycle_num, cd->client_name,
                    url_logging ?
                    cd->bctx->url_ctx_arr[cd->url_curr_index].url_str : "",
                    response_status, data);
            break;
          default :
            fprintf(cd->file_output, 
                    "%ld %s:<= parsing error: wrong status code \"%s\".\n", 
                    cd->cycle_num, cd->client_name, (char*) data);
            break;
          }
      }
      break;

    case CURLINFO_DATA_IN:
      if (verbose_logging) 
        {
          fprintf(cd->file_output, "%ld %s %s <= Recv data\n", 
                  cd->cycle_num, cd->client_name, 
                  url_logging ?
                  cd->bctx->url_ctx_arr[cd->url_curr_index].url_str : "");
        }
      cd->bctx->http_delta.data_in += (unsigned long) size;         
      break;

    case CURLINFO_SSL_DATA_IN:
      if (verbose_logging) 
        {
          fprintf(cd->file_output, "%ld %s %s <= Recv ssl data\n", 
                  cd->cycle_num, cd->client_name,
                  url_logging ?
                  cd->bctx->url_ctx_arr[cd->url_curr_index].url_str : "");
        }
      cd->bctx->https_delta.data_in += (unsigned long) size;
      break;

    default:
      return 0;
    }

  return 0;
}

static int alloc_init_client_post_buffers (client_context* cctx)
{
  int i;
  batch_context* bctx = cctx->bctx;
  const char percent_symbol = '%';
  char* pos = post_login_format;
  int count_percent = 0;

  if (! post_login_format[0])
    {
      return -1;
    }

  /* Calculate number of % symbols in post_login_format */
  while ((pos = strchr (pos, percent_symbol)))
    {
      count_percent++;
      pos = pos + 1;
    }

  for (i = 0;  i < bctx->client_num; i++)
    {
      /* Allocate client buffers for POSTing login and logoff credentials.
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
                   "\"%s\" - %s - failed to allocate post login buffer.\n",
                   bctx->batch_name, __func__) ;
          return -1;
        }

      if (count_percent == 4)
        {
          /* For each client init post buffer, containing username and
             password with uniqueness added via added to the base 
             username and password client index. */
          snprintf (cctx[i].post_data_login, 
                    POST_LOGIN_BUF_SIZE, 
                    post_login_format,
                    bctx->username, i + 1,
                    bctx->password, i + 1);
        }
      else if (count_percent == 2)
        {
          /* All clients have the same username and password.*/
          snprintf (cctx[i].post_data_login, 
                    POST_LOGIN_BUF_SIZE, 
                    post_login_format,
                    bctx->username, 
                    bctx->password);
        }
      else
        {
          return -1;
        }
      
      if (post_logoff_format[0])
        {
          snprintf (cctx[i].post_data_logoff,
                    POST_LOGOFF_BUF_SIZE,
                    "%s",
                    post_logoff_format);
        }
    }
  return 0;
}

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

  /* Pass through client contexts and initialize */
  for (i = 0 ; i < bctx->client_num ; i++)
    {
      /* Build client name for logging, based on sequence number and 
          ip-address for each simulated client. 
      */
      cctx[i].cycle_num = 0;

      snprintf(cctx[i].client_name, 
               sizeof(cctx[i].client_name) - 1, 
               "%d (%s) ", i + 1, bctx->ip_addr_array[i]);

      /* Set index of the client within the batch.
         Useful to get the client's CURL handle from bctx. */
      cctx[i].client_index = i;

      cctx[i].url_curr_index = 0; /* Actually zeroed by calloc. */

      /* Set output stream for each client to be either batch logfile or stderr. */
      cctx[i].file_output = stderr_print_client_msg ? stderr : output_file;

      /* Set pointer to batch for each client to be user to get configuration 
         and set back statistics to batch */
      cctx[i].bctx = bctx;
    }
  return 0;
}

static void free_batch_data_allocations (batch_context* bctx)
{
  int i;

  /* Free the CURL handles. */
  if (bctx->client_num > 0 && bctx->client_handles_array)
    {
      free (bctx->client_handles_array);
      bctx->client_handles_array = NULL;
    }

  /* Free the allocated url contexts*/
  if (bctx->url_ctx_arr && bctx->urls_num)
    {
      /* Free all URL-strings */
      for (i = 0 ; i < bctx->urls_num; i++)
        {
          if (bctx->url_ctx_arr[i].url_str)
            {
              free (bctx->url_ctx_arr[i].url_str);
              bctx->url_ctx_arr[i].url_str = NULL ;
            }
        }

      /* Free URL context array */
      free (bctx->url_ctx_arr);
      bctx->url_ctx_arr = NULL;
    }
}

static int create_ip_addrs (batch_context* bctx, int bctx_num)
{
  int bi, cli; /* Batch and client indexes */
  struct in_addr in_address;
  char*** ip_addresses = NULL;
  char* dotted_ip_addr = NULL;

  /* Add secondary IP-addresses to the "loading" network interface. */
  if (!(ip_addresses = (char***)calloc (bctx_num, sizeof (char**))))
    {
      fprintf (stderr, "%s - failed to allocate ip_addresses.\n", __func__);
      return -1;
    }
  
  for (bi = 0 ; bi < bctx_num ; bi++) 
    {
      /* Allocate array of IP-addresses */
      if (!(ip_addresses[bi] = (char**)calloc (bctx[bi].client_num, 
                                              sizeof (char *))))
        {
          fprintf (stderr, 
                   "%s - failed to allocate array of ip-addresses for batch %d.\n", 
                   __func__, bi);
          return -1;
        }

      /* Set them to the batch contexts to remember them. */
      bctx[bi].ip_addr_array = ip_addresses[bi]; 

      /* Allocate for each client a buffer and snprintf to it the IP-address string.
       */
      for (cli = 0; cli < bctx[bi].client_num; cli++)
        {
          if (!(ip_addresses[bi][cli] = (char*)calloc (IPADDR_STR_SIZE, sizeof (char))))
            {
              fprintf (stderr, "%s - allocation of ip_addresses[%d][%d] failed\n", 
                       __func__, bi, cli) ;
              return -1;
            }
 
          /* Advance the ip-address, using client index as the offset. 
           */
          in_address.s_addr = htonl (bctx[bi].ip_addr_min + cli);

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
      if (add_secondary_ip_addrs (bctx[bi].net_interface, bctx[bi].client_num, 
                                  (const char** const) ip_addresses[bi], bctx[bi].cidr_netmask) == -1)
        {
          fprintf (stderr, 
                   "%s - add_secondary_ip_addrs() - failed for batch = %d\n", 
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

  /* Overwriting the default behavior to write body bytes to stdout and 
     just skipping the body bytes without any output. */
  return (size*nmemb);
}
