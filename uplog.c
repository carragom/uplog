/*
    Copyright (c) 2003 by Mats Engstrom, Nerdlabs Consulting (mats@nerdlabs.org)

    Uplog is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    Uplog is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with uplog; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <syslog.h>
#include <netinet/in.h>
#include <netdb.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <strings.h>
#include <time.h>


// Nr of mS to sleep while polling for a new second
#define POLL_INTERVAL	20

// Max time in mS to wait for reply before timing out
#define WAIT_TIMEOUT	900

// Use the echo-port as default
#define DEFAULT_PORT	7

// Define the characters to be displayed
#define TIMEOUT_CHAR	'.'
#define REPLY_CHAR	'X'
#define INVALID_CHAR	':'

// Use LOCAL7 as the syslog facility
#define SYSLOG_FACILITY	LOG_LOCAL7


#define TRUE	1
#define FALSE	0


// Used as a flag to terminate the program by a SIGHUP
static volatile int abort_flag;


//
//  Set the abort-flag on receiveing a SIGHUP or SIGINT
//

void  SigHandler(int sig) {
  abort_flag=TRUE;
}



//
// Copy the current localtime in YYYY-MM-DD HH:NN-format
// to arg.
//

void GetTime(char *arg, int maxlen) {
  struct tm 	*tm;
  time_t	now;

  now=time(NULL);
  tm=localtime(&now);
  strftime(arg,maxlen,"%Y-%m-%d %H:%M",tm);
}



//
// Convert a hostname (www.nerdlabs.org) or an ip-address in dotted-quad 
// format (213.242.131.25) to an in_addr_t that can be used by the 
// socket-functions.
//

in_addr_t mygethostbyname(char *host) {
  struct hostent *he;
  in_addr_t ip;
        
  ip=0;
  he=gethostbyname(host);
  if (he!=NULL) {
    ip=*(in_addr_t *)he->h_addr;
  }
  return ip;
}
                                                 




//
// Daemonize the process
//

int daemonize(void) {
  pid_t	pid;

 
  pid=fork();
  if (pid==-1) {
    return errno;
  }
  if (pid!=0) {
    exit(0);
  } 
  
  setsid();
  
  signal(SIGHUP,SIG_IGN);
  
  pid=fork();
  if (pid==-1) {
    return errno;
  }
  if (pid!=0) {
    exit(0);
  }
  
  close(STDIN_FILENO);
  close(STDOUT_FILENO);
  close(STDERR_FILENO);
  
  return 0;
}





//
// Show the usage-text to the user
//

void usage(void) {
  printf("%s: An UDP based logging uptime checker\n\n",PACKAGE);
  printf("Usage: %s [options] host\n",PACKAGE);
  printf("  -h          Show this help\n");
  printf("  -V          Show program version\n");
  printf("  -f          Run program in foreground, print to stdout\n");
  printf("  -p PORT     Send packets to PORT, default is 7\n");
  printf("  -l LOGDIR   Write logs in LOGDIR, default it current dir\n");
  printf("\n Please report bugs to mats@nerdlabs.org\n");
}
                                                 

//
// Show the version-text to the user
//

void version(void) {
  printf("%s version %s\n",PACKAGE,VERSION);
}


//
// This is the main program. The program flow is:
//
// 	Initialize variables
// 	Parse command line options
// 	Connect the UDP socket
// 	Deamonize if not running as a foreground program
// 	Loop until aborted
// 	  Wait for new second
// 	  Display/Log results if new minute
// 	  Send packet
// 	  Wait for packet to arrive or timeout 
// 	  Select a character to display depending of outcome
// 	EndLoop
//

int main(int argc, char *argv[]) {

  struct sockaddr_in servaddr;	// The socket structure
  int		sck;		// The connected socket
  char		host[256];	// The host to send packets to
  int		port;		// The port to send packets to
  int		result;		// Generic function-result variable
  unsigned int	request;	// The data to send()
  unsigned int	reply;		// To read() the reply into
  char		datastring[100]; // Collects the result for one minute
  FILE		*f;		// Used for the log file
  struct timeval tv;		// Convert time to ascii	
  time_t 	now;		// To get current time into
  fd_set	set;  		// Used for select()
  char		resultchar;	// The result (X:.) of the current ping
  int		daemon_flag;	// False is run in foreground
  char		logpath[256];	// Path of the logfiles
  char		filename[256];	// Concats the path+host for fopen()
  char		tmpstring[256]; // Temp string for building error messages in
  int		c;		// Used when parsing command line options


  // Initialize variables that can be modified by the command line options
  
  abort_flag=FALSE;
  daemon_flag=TRUE;
  strcpy(logpath,"./");
  port=DEFAULT_PORT;

  
  //  Parse the command line options
  
  while ((c = getopt(argc, argv, "hVfl:p:")) != -1) {
    switch (c) {

      case 'h':
        usage();
        exit(EXIT_FAILURE);
        break;

      case 'V':
        version();
        exit(EXIT_FAILURE);
        break;

      case 'f':
        daemon_flag=FALSE;
        break;
        
      case 'l':
        strncpy(logpath,optarg,sizeof(logpath));
        break;

      case 'p':
        port=strtol(optarg,NULL,10);
        break;
        
      default:
        usage();
        exit(EXIT_FAILURE);
        break;
    }
  }
  
  // Now, take care of the host argument. This version only handles one host
  
  if (argc-optind<1) {
    printf("Host to check is missing.\nType %s -h for help\n",PACKAGE); 
    exit(EXIT_FAILURE);
  }
  if (argc-optind>1) {
    printf("Multiple hosts not supported.\nType %s -h for help\n",PACKAGE); 
    exit(EXIT_FAILURE);
  }
  strncpy(host,argv[optind],sizeof(host));

  // Create and (pseudo-) connect a UDP-socket
  bzero(&servaddr,sizeof(servaddr));
  servaddr.sin_family=AF_INET;
  servaddr.sin_port=htons(port);
  servaddr.sin_addr.s_addr=mygethostbyname(host);

  if (servaddr.sin_addr.s_addr==0) {
    printf("Can't resolve host %s\n",host);
    exit(EXIT_FAILURE);
  }

  sck=socket(AF_INET,SOCK_DGRAM,0);
  if (sck<0) {
    perror("Error creating socket");
    exit(EXIT_FAILURE);
  } 

  result=connect(sck,(struct sockaddr *)&servaddr,sizeof(servaddr));
  if (result!=0) {
    perror("Error in connect()");
    exit(EXIT_FAILURE);
  } 

  // Set up a signal handler for SIGHUP's or SIGINT's

  if (daemon_flag) {
    signal(SIGHUP, SigHandler);
  } else {
    signal(SIGINT, SigHandler);
  }
  
  // If the program is to be run as a daemon then
  // open the syslog and become a daemon

  if (daemon_flag) {
    openlog(PACKAGE,LOG_PID,SYSLOG_FACILITY);
    syslog(LOG_NOTICE,"Started");
    result=daemonize();
    if (result==-1) {
      perror("Error at deaemon()");
      exit(EXIT_FAILURE);
    }
  }

  // Copy the date and time to the data collection string and 
  // initialize the request value to be sent

  request=0;
  GetTime(datastring,sizeof(datastring));
  strcat(datastring," ");
  if (!daemon_flag) {
    printf("%s",datastring);
    fflush(NULL);
  }

  // Loop in the main loop until aborted 
  
  while (abort_flag==0) {
  
    // Wait for a new second.  This is done by polling the current time
    // every POLL_INTERVAL milli seconds. If this is set to 20 mS the 
    // cpu-load is very low, about 0.2 cpu seconds per day on an old P90.
  
    now=time(NULL);
    while (time(NULL)==now) {
      tv.tv_sec=0;
      tv.tv_usec=POLL_INTERVAL*1000;
      result=select(0,NULL,NULL,NULL,&tv);
    }

    // If this is a new minute then output the collected data
    // and begin on a new collection-string
  
    if (now%60==0) {
      if (daemon_flag) {
        snprintf(filename,sizeof(filename),"%s/%s",logpath,host);
        f=fopen(filename,"a");
        if (f==NULL) {
	  snprintf(tmpstring,sizeof(tmpstring),"Error opening %s",host);
	  syslog(LOG_ERR,tmpstring);
          syslog(LOG_NOTICE,"Exiting");
          closelog();
          exit(EXIT_FAILURE);
        }
        fprintf(f,"%s\n",datastring);
        fclose(f);
      }
      GetTime(datastring,sizeof(datastring));
      strcat(datastring," ");
      if (!daemon_flag) {
        printf("\n%s",datastring);
        fflush(NULL);
      }
    }

    // Increment the request number and send the UDP-packet 
  
    request++;
    result=write(sck,&request,sizeof(request));

    // Wait for the reply to come back from the destination
  
    FD_ZERO(&set);
    FD_SET(sck,&set);
    tv.tv_sec=0;
    tv.tv_usec=WAIT_TIMEOUT*1000;
    result=select(sck+1,&set,NULL,NULL,&tv);

    // Check if the read timed out or got error back
  
    if (result<1) {
      resultchar=TIMEOUT_CHAR;
    } else {
      // Check if we got back the value we sent
      result=read(sck,&reply,sizeof(reply));
      if (reply==request) {
        resultchar=REPLY_CHAR;
      } else {
        resultchar=INVALID_CHAR;
      }
    }

    // If running in foreground then output the result right away,
    // otherwise collect the result for an entire minute into the
    // datastring
    
    if (!daemon_flag) {
      printf("%c",resultchar);
      fflush(NULL);
     } else {
      datastring[strlen(datastring)+1]=0;
      datastring[strlen(datastring)]=resultchar;
    }              
		                             
 }

  // The program is exiting, close the socket and quit
  
  close(sck);

  if (daemon_flag) {
    syslog(LOG_NOTICE,"Exiting");
    closelog();
  } else {
    printf("\nStopped by user\n");
  }

  return EXIT_SUCCESS;	
}




