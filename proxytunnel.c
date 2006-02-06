/* Proxytunnel - (C) 2001-2006 Jos Visser / Mark Janssen    */
/* Contact:                  josv@osp.nl / maniac@maniac.nl */

/*
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
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <signal.h>
#include <syslog.h>
#include <stdarg.h>

#include "io.h"
#include "config.h"
#include "cmdline.h"
#include "basicauth.h"
#include "proxytunnel.h"
#include "ntlm.h"

/* Define DARWIN if compiling on MacOS-X (Darwin), to work around some
 * inconsistencies. (Darwin doesn't know socklen_t) */
#ifdef DARWIN
#define socklen_t int
#endif

/* Globals */
int read_fd=0;                  /* The file descriptor to read from */
int write_fd=1;                 /* The file destriptor to write to */


/*
 * Kill the program (signal handler)
 */
void signal_handler( int signal )
{
	close(0);
	close(1);

	if ( sd != 0 )
		close( sd );

	message( "Tunnel closed on signal %d\n", signal );
	exit(1);
}

/*
 * Create and connect the socket that connects to the proxy. After
 * this routine the sd socket is connected to the proxy.
 */
void tunnel_connect() {
	struct sockaddr_in 	sa;
	struct hostent		*he;

	/*
	 * Create the socket
	 */
	if( ( sd = socket( AF_INET, SOCK_STREAM, 0 ) ) < 0 )
	{
		my_perror("Can not create socket");
		exit(1);
	}

	/* 
	 * Lookup the IP address of the proxy
	 */
	if( ! ( he = gethostbyname( args_info.proxyhost_arg ) ) )
	{
		my_perror("Proxy host not found");
		exit(1);
	}
 
 	if( args_info.verbose_flag )
	{
 		message( "%s is %d.%d.%d.%d\n",
				args_info.proxyhost_arg,
				he->h_addr[0] & 255,
				he->h_addr[1] & 255,
				he->h_addr[2] & 255,
				he->h_addr[3] & 255 );
	}

	/*
	 * Set up the structure to connect to the proxy port of the proxy host
	 */
	memset( &sa, '\0', sizeof( sa ) );
  	sa.sin_family = AF_INET;
  	memcpy( &sa.sin_addr.s_addr, he->h_addr, 4);
  	sa.sin_port = htons( args_info.proxyport_arg );
  
  	/*
	 * Connect the socket
	 */
  	if( connect( sd, (struct sockaddr*) &sa, sizeof( sa ) ) < 0 )
	{
		my_perror("connect() failed");
		exit(1);
	}

	if( ! args_info.quiet_flag )
	{
		message( "Connected to %s:%d\n",
			args_info.proxyhost_arg,
			args_info.proxyport_arg );
	}

	/* Make sure we get warned when someone hangs up on us */
	signal(SIGHUP,signal_handler);
}

/*
 * Leave a goodbye message
 */
void einde() {
#ifdef CYGWIN
   	message( "Goodbye" );
#else
        syslog(LOG_NOTICE,"Goodbye...");
        closelog();
#endif
}

/*
 * Run as a standalone daemon
 */
void do_daemon()
{
	int			listen_sd;
	struct sockaddr_in	sa_serv;
	struct sockaddr_in	sa_cli;
	socklen_t		client_len;
	int			pid = 0;
	int			sd_client;
	char 			buf[80];
	unsigned char		addr[4];

	if ( ( listen_sd = socket( AF_INET, SOCK_STREAM, 0 ) ) < 0 )
	{
		my_perror( "Server socket creation failed" );
		exit(1);
	}

	memset( &sa_serv, '\0', sizeof( sa_serv ) );
	sa_serv.sin_family = AF_INET;
	sa_serv.sin_addr.s_addr = INADDR_ANY;
	sa_serv.sin_port = htons( args_info.standalone_arg );

	if ( bind( listen_sd, (struct sockaddr * )&sa_serv, sizeof( sa_serv ) ) < 0)
	{
		my_perror("Server socket bind failed");
		exit(1);
	}

	signal(SIGHUP,SIG_IGN);
	signal(SIGCHLD,SIG_IGN);

/* For the moment, turn of forking into background on the cygwin platform
 * so users can run it in a command window and ctrl-c it to cancel.
 * Also so we can put logging there, since there's nog syslog on cygwin (AFAIK)
 * 	-- Maniac
 */
#ifndef CYGWIN
	if ( ( pid = fork( ) ) < 0 )
	{
		my_perror( "Cannot fork into the background" );
		exit( 1 );
	}
	else if ( pid > 0 )
	{
       		message( "Forked into the background with pid %d\n", pid );
       		exit(0);
	}

	openlog( program_name, LOG_CONS|LOG_PID,LOG_DAEMON );
	i_am_daemon = 1;
#endif /* CYGWIN */
	atexit( einde );
	listen( listen_sd, 5 );

	while (1==1)
	{
		/* 2002/04/21
		 *
		 * Workaround a CYGWIN bug, see:
		 * http://www.gnu.org/software/serveez/manual/BUGS
		 * for bug #B0007
		 *
		 * 2004/06/23: Apparently Solaris needs this too, so 
		 * we'll do it by default, can't hurt
		 *
		 * -- Maniac
		 */

		client_len = sizeof( sa_cli );

		sd_client = accept( listen_sd,
			(struct sockaddr *)&sa_cli, &client_len );

		if ( sd_client < 0 )
		{
        		my_perror( "accept() failed. Bailing out..." );
        		exit(1);
		}

		if ( ( pid = fork() ) < 0 )
		{
        		my_perror( "Cannot fork worker" );
		}
		else if ( pid == 0 )
		{
        		read_fd = write_fd = sd_client;
			tunnel_connect();
			proxy_protocol();
			if (args_info.ntlm_flag) {
				proxy_protocol();
				proxy_protocol();
			}
			cpio();
			exit( 0 );
		}

		memcpy( &addr, &sa_cli.sin_addr.s_addr, 4 );
		sprintf( (char *) buf, "%u.%u.%u.%u", addr[0], addr[1], addr[2], addr[3] );
#ifdef CYGWIN
		message( "Started tunnel pid=%d for connection from %s",
		      pid, buf );
#else
		syslog( LOG_NOTICE,
		"Started tunnel pid=%d for connection from %s", pid, buf );

#endif
		close( sd_client );
	}
}


/*
 * We begin at the beginning
 */
int main( int argc, char *argv[] )
{
	program_name = argv[0];

	/*
	 * New and improved option handling, using GNU getopts  -- Maniac
	 */

	cmdline_parser( argc, argv, &args_info );

	/*
	 * This is what we do:
	 * - Check if we need to run as a daemon. If so, a completely
	 *   different mainline is needed...
	 * - Set a signal for the hangup (HUP) signal
	 * - Optionally create the proxy basic authenticcation cookie
	 * - Connect the sd socket to the proxy
	 * - Execute the proxy protocol to connect it to the origin server
	 * - Enter copy in-out mode to channel data hence and forth
	 */

	signal( SIGHUP, signal_handler );

	/* If the usename is given, but password is not, prompt for it */
	if( args_info.user_given && !args_info.pass_given )
	{
		char *cp;
		cp = getpass ("Enter proxy password:");
		if (cp != NULL && strlen (cp) > 0)
		{
			args_info.pass_arg = strdup (cp);
			args_info.pass_given = 1;
			memset (cp, 0, strlen(cp));
		}
	}

	if( args_info.user_given && args_info.pass_given )
	{
		if (args_info.ntlm_flag) {
			build_type1();
			message("Build Type 1 NTLM Message : %s\n", ntlm_type1_buf);
		} else
			make_basicauth();
	}

	/* Do we need to run as a standalone daemon? */
	if ( args_info.standalone_arg > 0 )
	{
		/* Do processing in the other mainline... */
		do_daemon();
	}
	else
	{
		/* Inetd trick */
		if( args_info.inetd_flag )
		{
			write_fd=0;
		}

		/* Main processing */
		tunnel_connect();
		proxy_protocol();
		cpio();
	}

	exit( 0 );
}
