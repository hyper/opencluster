//-----------------------------------------------------------------------------
// ocd - Open Cluster Daemon
//	Enhanced hash-map storage cluster.
//-----------------------------------------------------------------------------

#define OCD_MAIN


// So that this service can be used with libevent 1.x as well as 2.x, we have a compatibility 
// wrapper.   It is only applied at COMPILE TIME.
#include "event-compat.h"

// includes
#include "bucket.h"
#include "constants.h"
#include "item.h"
#include "logging.h"
#include "params.h"
#include "payload.h"
#include "seconds.h"
#include "server.h"
#include "settle.h"
#include "shutdown.h"
#include "stats.h"
#include "timeout.h"

#include <assert.h>
#include <fcntl.h>
#include <pwd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>



//-----------------------------------------------------------------------------
// Globals... for other objects in this project to access them, include "globals.h" which has all 
// the externs defined.

// event base for listening on the sockets, and timeout events.
struct event_base *_evbase = NULL;




// signal catchers that are used to clean up, and store final data before 
// shutting down.
struct event *_sigint_event = NULL;



//--------------------------------------------------------------------------------------------------
// Since this is the interrupt handler, we need to do as little as possible here, and just start up 
// an event to take care of it.
static void sigint_handler(evutil_socket_t fd, short what, void *arg)
{
	assert(arg == NULL);

#ifndef NDEBUG
 	if (_verbose > 0 ) printf("\nSIGINT received.\n\n");
#endif
	
	// delete the signal events.
	assert(_sigint_event);
	event_free(_sigint_event);
	_sigint_event = NULL;

	log_prepareshutdown();

	shutdown_start();
	
// 	printf("SIGINT complete\n");
}




//-----------------------------------------------------------------------------
// print some info to the user, so that they can know what the parameters do.
static void usage(void) {
	printf(
		PACKAGE " " VERSION "\n"
		"-l <file>          config file for listener details\n"
		"-c <num>           max simultaneous connections, default is 1024\n"
		"-n <file>          Other cluster node to connect to. Can be specified more than once.\n"
		"\n"
		"-d                 run as a daemon\n"
		"-P <file>          save PID in <file>, only used with -d option\n"
		"-u <username>      assume identity of <username> (only when run as root)\n"
		"\n"
		"-g <filemask>      Logfile location and prefix.  The actual logfile name will be\n"
		"                   appended with a datestamp.\n"
		"-m <mb>            mb limit before a new logfile is created.\n"
		"-v                 verbose (print errors/warnings while in event loop)\n"
		"                     eg, -vvv will set it to level 3 (WARN)\n"
		"                       1 - FATAL\n"
		"                       2 - ERROR\n"
		"                       3 - WARN\n"
		"                       4 - STATS\n"
		"                       5 - INFO\n"
		"                       6 - DEBUG\n"
		"                       7 - EXTRA\n"
		"\n"
		"-h                 print this help and exit\n"
	);
	return;
}



// this is used to fork the process and run in the background as a different user.  
void daemonize(const char *username, const char *pidfile, const int noclose)
{
	struct passwd *pw;
	struct sigaction sa;
	int fd;
	FILE *fp;
	
	if (getuid() == 0 || geteuid() == 0) {
		if (username == 0 || *username == '\0') {
			fprintf(stderr, "can't run as root without the -u switch\n");
			exit(EXIT_FAILURE);
		}
		assert(username);
		pw = getpwnam((const char *)username);
		if (pw == NULL) {
			fprintf(stderr, "can't find the user %s to switch to\n", username);
			exit(EXIT_FAILURE);
		}
		if (setgid(pw->pw_gid) < 0 || setuid(pw->pw_uid) < 0) {
			fprintf(stderr, "failed to assume identity of user %s\n", username);
			exit(EXIT_FAILURE);
		}
	}
	
	sa.sa_handler = SIG_IGN;
	sa.sa_flags = 0;
	if (sigemptyset(&sa.sa_mask) == -1 || sigaction(SIGPIPE, &sa, 0) == -1) {
		perror("failed to ignore SIGPIPE; sigaction");
		exit(EXIT_FAILURE);
	}
	
	switch (fork()) {
		case -1:
			exit(EXIT_FAILURE);
		case 0:
			break;
		default:
			_exit(EXIT_SUCCESS);
	}
	
	if (setsid() == -1)
		exit(EXIT_FAILURE);
	
	(void)chdir("/");
	
	if (noclose == 0 && (fd = open("/dev/null", O_RDWR, 0)) != -1) {
		(void)dup2(fd, STDIN_FILENO);
		(void)dup2(fd, STDOUT_FILENO);
		(void)dup2(fd, STDERR_FILENO);
		if (fd > STDERR_FILENO)
			(void)close(fd);
	}
	
	// save the PID in if we're a daemon, do this after thread_init due to a
	// file descriptor handling bug somewhere in libevent
	if (pidfile != NULL) {
		if ((fp = fopen(pidfile, "w")) == NULL) {
			fprintf(stderr, "Could not open the pid file %s for writing\n", pidfile);
			exit(EXIT_FAILURE);
		}
		
		fprintf(fp,"%ld\n", (long)getpid());
		if (fclose(fp) == -1) {
			fprintf(stderr, "Could not close the pid file %s.\n", pidfile);
			exit(EXIT_FAILURE);
		}
	}
}













	


//-----------------------------------------------------------------------------
// Main... process command line parameters, and then setup our listening 
// sockets and event loop.
int main(int argc, char **argv) 
{

	assert(sizeof(char) == 1);
	assert(sizeof(short) == 2);
	assert(sizeof(int) == 4); 
	assert(sizeof(long long) == 8);
	
///============================================================================
/// Initialization.
///============================================================================

	parse_params(argc, argv);

	// daemonize
	if (_daemonize) {
		daemonize(_username, _pid_file, _verbose);
	}


	
	// create our event base which will be the pivot point for pretty much everything.
#if ( _EVENT_NUMERIC_VERSION >= 0x02000000 )
	assert(event_get_version_number() == LIBEVENT_VERSION_NUMBER);
#endif
	
	_evbase = event_base_new();
	assert(_evbase);

	// initialise signal handlers.
	assert(_evbase);
	_sigint_event = evsignal_new(_evbase, SIGINT, sigint_handler, NULL);
	assert(_sigint_event);
	event_add(_sigint_event, NULL);

	log_init(_logfile, _verbose, _logfile_max);
	
	seconds_init();
	
	payload_init();
	
	// we need to set a timer to fire in 5 seconds to setup the cluster if no connections were made.
	settle_init();

	// statistics are generated every second, setup a timer that can fire and handle the stats.
	stats_init();

	client_init_commands(4096);
	
	// initialise the servers that we listen on.
	server_listen();

	// attempt to connect to the other known nodes in the cluster.
	node_connect_all();
	

///============================================================================
/// Main Event Loop.
///============================================================================

	// enter the processing loop.  This function will not return until there is
	// nothing more to do and the service has shutdown.  Therefore everything
	// needs to be setup and running before this point.  
	logger(LOG_INFO, "Starting main loop.");
	assert(_evbase);
	event_base_dispatch(_evbase);

///============================================================================
/// Shutdown
///============================================================================

	// need to shut the log down now, because we have lost the event loop and cant actually use it 
	// any more.
	log_shutdown();
	
	assert(_evbase);
	event_base_free(_evbase);
	_evbase = NULL;

	
	
	// server interface should be shutdown.
// 	assert(_server == NULL);
// 	assert(_node_count == 0);
// 	assert(_client_count == 0);


	client_cleanup(void);	
	
	// make sure signal handlers have been cleared.
	assert(_sigint_event == NULL);

	
	payload_free();
	
	// we are done, cleanup what is left in the control structure.
	_interface = NULL;
	
	if (_username) { free((void*)_username); _username = NULL; }
	if (_pid_file) { free((void*)_pid_file); _pid_file = NULL; }
	
// 	assert(_conncount == 0);
	assert(_sigint_event == NULL);
	assert(_evbase == NULL);
	
	return 0;
}


