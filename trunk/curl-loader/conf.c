/*
*     conf.c
*
* 2006 Copyright (c) 
* Robert Iakobashvili, <coroberti@gmail.com>
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

// must be first include
#include "fdsetsize.h"

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

#include "conf.h"

/*
  Command line configuration options. Setting defaults here.
*/
 /* Configurable time to cut connect () in prog */  
int connect_timeout = 5;

/* Flag, whether to perform verbose logging */
int verbose_logging = 0;

/* Flag, whether to run batches as batch per thread. */
int threads_run = 0;

/* 
   Time in seconds between snapshot statistics printouts to
   screen as well as to the statistics file
*/
long snapshot_statistics_timeout = 3; /* Seconds */
/*  
    Rewind logfile, if above the size above MB 
*/
long logfile_rewind_size = 1024;

 /* Non-zero - dont reuse connections */
int reuse_connection_forbidden = 0;

/* Whether to stdout the downloaded file body */
int output_to_stdout = 0;

/* If to output client messages to stderr, otherwise to logfile */
int stderr_print_client_msg = 0;

/* Storming or smooth loading */
int loading_mode = LOAD_MODE_DEFAULT;

 /* Whether to include url to all log outputs. */
int url_logging = 0;

/* Name of the configuration file */
char config_file[PATH_MAX + 1];

/* 
   On errors, whether to continue loading for this client 
   from the next cycle, or to give it up.
 */
unsigned long error_recovery_client = 1; /* Default: error recovery and continue */



int parse_command_line (int argc, char *argv [])
{
  int rget_opt = 0;

    while ((rget_opt = getopt (argc, argv, "c:ehf:i:l:m:op:rstvu")) != EOF) 
    {
      switch (rget_opt) 
        {
        case 'c': /* Connection establishment timeout */
          if (!optarg || (connect_timeout = atoi (optarg)) <= 0)
            {
              fprintf (stderr, 
                       "%s error: -c option should be a positive number in seconds.\n",
                  __func__);
              return -1;
            }
          break;
          
        case 'e':
          error_recovery_client = 0;
          break;

        case 'h':
          print_help ();
          exit (0);

        case 'f': /* Configuration file */
          if (optarg)
            strcpy(config_file, optarg);
          else
            {
              fprintf (stderr, 
                       "%s error: -f option should be followed by a filename.\n", 
                       __func__);
              return -1;
            }
          break;

          case 'i': /* Statistics snapshot timeout */
          if (!optarg ||
              (snapshot_statistics_timeout = atoi (optarg)) < 1)
            {
              fprintf (stderr, 
                       "%s error: -i option should be followed by a number >= 1.\n", 
                       __func__);
              return -1;
            }
          break;
            
        case 'l': /* Number of cycles before a logfile rewinds. */
          if (!optarg || 
              (logfile_rewind_size = atol (optarg)) < 2)
            {
              fprintf (stderr, 
                       "%s: error: -l option should be followed by a number >= 2.\n",
                  __func__);
              return -1;
            }
          break;

        case 'm': /* Modes of loading: SMOOTH and STORMING */

            if (!optarg || 
                ((loading_mode = atoi (optarg)) != LOAD_MODE_STORMING &&
                 loading_mode != LOAD_MODE_SMOOTH && loading_mode != LOAD_MODE_HYPER ))
            {
              fprintf (stderr, 
                       "%s error: -m to be followed by a number either %d or %d or %d.\n",
                       __func__, LOAD_MODE_STORMING, LOAD_MODE_SMOOTH, LOAD_MODE_HYPER);
              return -1;
            }

            if (loading_mode == LOAD_MODE_HYPER)
              {
                fprintf (stderr, 
                       "%s error: LOAD_MODE_HYPER is \"Under Construction\".\n",
                       __func__);
              return -1;
              }
          
          break;

        case 'o': /* Print body of the file to stdout. Default - just skip it. */
          output_to_stdout = 1;
          break;

        case 'r': /* Means - reuse connections, dont close and re-open. */
          reuse_connection_forbidden = 1;
          break;

        case 's': /* Stderr printout of client messages (instead of to a batch logfile). */
          stderr_print_client_msg = 1;
          break;

        case 't': /* Run each batch of clients in a dedicated thread. */
            threads_run = 1;
            break;

        case 'v':
          verbose_logging = 1; 
          break;

        case 'u':
          url_logging = 1; 
          break;

        default: 
            fprintf (stderr, "%s error: not supported option\n", __func__);
          print_help ();
          return -1;
        }
    }

  if (optind < argc) 
    {
        fprintf (stderr, "%s error: non-option argv-elements: ", __func__);
        while (optind < argc)
            fprintf (stderr, "%s ", argv[optind++]);
        fprintf (stderr, "\n");
        print_help ();
      return -1;
    }

  return 0;
}

void print_help ()
{
  fprintf (stderr, "Note, to run your load, create your batch configuration file.\n\n");
  fprintf (stderr, "usage: run as a root:\n");
  fprintf (stderr, "#./curl-loader -f <configuration file name> with [other options below]:\n");
  fprintf (stderr, " -c[onnection establishment timeout, seconds]\n");
  fprintf (stderr, " -e[rror drop client (smooth mode). Client on error doesn't attempt next cycle]\n");
  fprintf (stderr, " -i[ntermediate (snapshot) statistics time interval (default 3 sec)]\n");
  fprintf (stderr, " -l[ogfile max size in MB (default 1024). On the size reached, file pointer rewinded]\n");
  fprintf (stderr, " -m[ode of loading, 1 - storming, 2 - smooth (default), 3 - hyper (under construction)]\n");
  fprintf (stderr, " -o[utput to stdout bodies of downloaded files - attn!- bulky]\n");
  fprintf (stderr, " -r[euse onnections disabled. Close connections and re-open them. Try with and without]\n");
  fprintf (stderr, " -s[tderr printout of client messages instead of to logfile - attn!- bulky]\n");
  fprintf (stderr, " -t[hreads enable - enables threads, each runs a batch of clients]\n");
  fprintf (stderr, " -v[erbose output to the logfiles; includes info about headers sent/received]\n");
  fprintf (stderr, " -u[rl logging - logs url names to logfile, when -v verbose option is used]\n");
  fprintf (stderr, "\n");

  fprintf (stderr, "Use \"make menuconfig\" after reading HOWTOS-CONFIG-FILE\n" 
           "to create your loading batch configuration file\n\n");

  fprintf (stderr, "For more examples of configuration files please, look at \"conf-examples\" directory.\n");
  fprintf (stderr, "\n");
  fprintf (stderr, "Note, that there is no any more limit of 1000 sockets per batch of clients.\n");
  fprintf (stderr, "Running thousands and more clients, please do not forget to consider the options:\n");
  fprintf (stderr, "- to increase limit of open descriptors in shell by running e.g. #ulimit -n 20000:\n");
  fprintf (stderr, "- to increase total limit of  open descriptors in systeme somewhere in /proc\n");
  fprintf (stderr, "- to consider reusing sockets in time-wait state: by #echo 1 > \n");
  fprintf (stderr, " /proc/sys/net/ipv4/tcp_tw_recycle\n");
  fprintf (stderr, "- and/or  #echo 1 > /proc/sys/net/ipv4/tcp_tw_reuse\n");
  fprintf (stderr, "\n");
  fprintf (stderr, "\n");
}
