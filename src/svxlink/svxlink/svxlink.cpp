/**
@file	 svxlink.cpp
@brief   The main file for the SvxLink server
@author  Tobias Blomberg / SM0SVX
@date	 2004-03-28

\verbatim
SvxLink - A Multi Purpose Voice Services System for Ham Radio Use
Copyright (C) 2003-2008 Tobias Blomberg / SM0SVX

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
\endverbatim
*/



/****************************************************************************
 *
 * System Includes
 *
 ****************************************************************************/

#include <termios.h>
#include <unistd.h>
#include <stdio.h>
#include <ctype.h>
#include <stdlib.h>
#include <popt.h>
#include <locale.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dirent.h>
#include <pwd.h>
#include <grp.h>

#include <string>
#include <iostream>
#include <iomanip>
#include <algorithm>
#include <vector>
#include <cstring>
#include <set>


/****************************************************************************
 *
 * Project Includes
 *
 ****************************************************************************/

#include <AsyncCppApplication.h>
#include <AsyncConfig.h>
#include <AsyncTimer.h>
#include <AsyncFdWatch.h>
#include <AsyncAudioIO.h>
#include <LocationInfo.h>
#include <common.h>
#include <config.h>


/****************************************************************************
 *
 * Local Includes
 *
 ****************************************************************************/

#include "version/SVXLINK.h"
#include "MsgHandler.h"
#include "SimplexLogic.h"
#include "RepeaterLogic.h"
#include "LinkManager.h"



/****************************************************************************
 *
 * Namespaces to use
 *
 ****************************************************************************/

using namespace std;
using namespace Async;
using namespace sigc;



/****************************************************************************
 *
 * Defines & typedefs
 *
 ****************************************************************************/

#define PROGRAM_NAME "SvxLink"



/****************************************************************************
 *
 * Local class definitions
 *
 ****************************************************************************/



/****************************************************************************
 *
 * Prototypes
 *
 ****************************************************************************/

static void parse_arguments(int argc, const char **argv);
static void stdinHandler(FdWatch *w);
static void write_to_logfile(const char *buf);
static void stdout_handler(FdWatch *w);
static void initialize_logics(Config &cfg);
static void sighup_handler(int signal);
static void sigterm_handler(int signal);
static bool open_logfile(void);


/****************************************************************************
 *
 * Exported Global Variables
 *
 ****************************************************************************/



/****************************************************************************
 *
 * Local Global Variables
 *
 ****************************************************************************/

static char   	      	*pidfile_name = NULL;
static char   	      	*logfile_name = NULL;
static char   	      	*runasuser = NULL;
static char   	      	*config = NULL;
static int    	      	daemonize = 0;
static int    	      	logfd = -1;
static vector<Logic*>  	logic_vec;
static FdWatch	      	*stdin_watch = 0;
static FdWatch	      	*stdout_watch = 0;
static string         	tstamp_format;
static struct sigaction sighup_oldact, sigint_oldact, sigterm_oldact;
static volatile bool  	reopen_log = false;


/****************************************************************************
 *
 * MAIN
 *
 ****************************************************************************/


/*
 *----------------------------------------------------------------------------
 * Function:  main
 * Purpose:   Start everything...
 * Input:     argc  - The number of arguments passed to this program
 *    	      	      (including the program name).
 *    	      argv  - The arguments passed to this program. argv[0] is the
 *    	      	      program name.
 * Output:    Return 0 on success, else non-zero.
 * Author:    Tobias Blomberg, SM0SVX
 * Created:   2004-03-28
 * Remarks:   
 * Bugs:      
 *----------------------------------------------------------------------------
 */
int main(int argc, char **argv)
{
  setlocale(LC_ALL, "");

  CppApplication app;

  parse_arguments(argc, const_cast<const char **>(argv));

  struct sigaction act;
  act.sa_handler = sighup_handler;
  sigemptyset(&act.sa_mask);
  act.sa_flags = 0;
  if (sigaction(SIGHUP, &act, &sighup_oldact) == -1)
  {
    perror("sigaction");
    exit(1);
  }

  act.sa_handler = sigterm_handler;
  if (sigaction(SIGTERM, &act, &sigterm_oldact) == -1)
  {
    perror("sigaction");
    exit(1);
  }
  
  act.sa_handler = sigterm_handler;
  if (sigaction(SIGINT, &act, &sigint_oldact) == -1)
  {
    perror("sigaction");
    exit(1);
  }
  
  int pipefd[2] = {-1, -1};
  int noclose = 0;
  if (logfile_name != 0)
  {
      /* Open the logfile */
    if (!open_logfile())
    {
      exit(1);
    }

      /* Create a pipe to route stdout through */
    if (pipe(pipefd) == -1)
    {
      perror("pipe");
      exit(1);
    }
    stdout_watch = new FdWatch(pipefd[0], FdWatch::FD_WATCH_RD);
    // must explicitly specify name space for ptr_fun() to avoid conflict
    // with ptr_fun() in /usr/include/c++/4.5/bits/stl_function.h
    stdout_watch->activity.connect(sigc::ptr_fun(&stdout_handler));

      /* Redirect stdout to the logpipe */
    if (close(STDOUT_FILENO) == -1)
    {
      perror("close(stdout)");
      exit(1);
    }
    if (dup2(pipefd[1], STDOUT_FILENO) == -1)
    {
      perror("dup2(stdout)");
      exit(1);
    }

      /* Redirect stderr to the logpipe */
    if (close(STDERR_FILENO) == -1)
    {
      perror("close(stderr)");
      exit(1);
    }
    if (dup2(pipefd[1], STDERR_FILENO) == -1)
    {
      perror("dup2(stderr)");
      exit(1);
    }

      /* Close stdin */
    close(STDIN_FILENO);
    
      /* Force stdout to line buffered mode */
    if (setvbuf(stdout, NULL, _IOLBF, 0) != 0)
    {
      perror("setlinebuf");
      exit(1);
    }    
    
      /* Tell the daemon function call not to close the file descriptors */
    noclose = 1;
  }

  if (daemonize)
  {
    if (daemon(0, noclose) == -1)
    {
      perror("daemon");
      exit(1);
    }
  }

  if (pidfile_name != NULL)
  {
    FILE *pidfile = fopen(pidfile_name, "w");
    if (pidfile == 0)
    {
      char err[256];
      sprintf(err, "fopen(\"%s\")", pidfile_name);
      perror(err);
      fflush(stderr);
      exit(1);
    }
    fprintf(pidfile, "%d\n", getpid());
    fclose(pidfile);
  }

  if (runasuser != NULL)
  {
      // Setup supplementary group IDs
    if (initgroups(runasuser, getgid()))
    {
      perror("initgroups");
      exit(1);
    }

    struct passwd *passwd = getpwnam(runasuser);
    if (passwd == NULL)
    {
      perror("getpwnam");
      exit(1);
    }
    if (setgid(passwd->pw_gid) == -1)
    {
      perror("setgid");
      exit(1);
    }
    if (setuid(passwd->pw_uid) == -1)
    {
      perror("setuid");
      exit(1);
    }
  }

  const char *home_dir = getenv("HOME");
  if (home_dir == NULL)
  {
    home_dir = ".";
  }
  
  tstamp_format = "%c";

  Config cfg;
  string cfg_filename;
  if (config != NULL)
  {
    cfg_filename = string(config);
    if (!cfg.open(cfg_filename))
    {
      cerr << "*** ERROR: Could not open configuration file: "
      	   << config << endl;
      exit(1);
    }
  }
  else
  {
    cfg_filename = string(home_dir);
    cfg_filename += "/.svxlink/svxlink.conf";
    if (!cfg.open(cfg_filename))
    {
      cfg_filename = SVX_SYSCONF_INSTALL_DIR "/svxlink.conf";
      if (!cfg.open(cfg_filename))
      {
	cfg_filename = SYSCONF_INSTALL_DIR "/svxlink.conf";
	if (!cfg.open(cfg_filename))
	{
	  cerr << "*** ERROR: Could not open configuration file. Tried:\n"
      	       << "\t" << home_dir << "/.svxlink/svxlink.conf\n"
      	       << "\t" SVX_SYSCONF_INSTALL_DIR "/svxlink.conf\n"
	       << "\t" SYSCONF_INSTALL_DIR "/svxlink.conf\n"
	       << "Possible reasons for failure are: None of the files exist,\n"
	       << "you do not have permission to read the file or there was a\n"
	       << "syntax error in the file\n";
	  exit(1);
	}
      }
    }
  }
  string main_cfg_filename(cfg_filename);
  
  string cfg_dir;
  if (cfg.getValue("GLOBAL", "CFG_DIR", cfg_dir))
  {
    if (cfg_dir[0] != '/')
    {
      int slash_pos = main_cfg_filename.rfind('/');
      if (slash_pos != -1)
      {
      	cfg_dir = main_cfg_filename.substr(0, slash_pos+1) + cfg_dir;
      }
      else
      {
      	cfg_dir = string("./") + cfg_dir;
      }
    }
    
    DIR *dir = opendir(cfg_dir.c_str());
    if (dir == NULL)
    {
      cerr << "*** ERROR: Could not read from directory spcified by "
      	   << "configuration variable GLOBAL/CFG_DIR=" << cfg_dir << endl;
      exit(1);
    }
    
    struct dirent *dirent;
    while ((dirent = readdir(dir)) != NULL)
    {
      char *dot = strrchr(dirent->d_name, '.');
      if ((dot == NULL) || (strcmp(dot, ".conf") != 0))
      {
      	continue;
      }
      cfg_filename = cfg_dir + "/" + dirent->d_name;
      if (!cfg.open(cfg_filename))
       {
	 cerr << "*** ERROR: Could not open configuration file: "
	      << cfg_filename << endl;
	 exit(1);
       }
    }
    
    if (closedir(dir) == -1)
    {
      cerr << "*** ERROR: Error closing directory specified by"
      	   << "configuration variable GLOBAL/CFG_DIR=" << cfg_dir << endl;
      exit(1);
    }
  }
  
  cfg.getValue("GLOBAL", "TIMESTAMP_FORMAT", tstamp_format);
  
  cout << PROGRAM_NAME " v" SVXLINK_VERSION " (" __DATE__
          ") Copyright (C) 2003-2014 Tobias Blomberg / SM0SVX\n\n";
  cout << PROGRAM_NAME " comes with ABSOLUTELY NO WARRANTY. "
          "This is free software, and you are\n";
  cout << "welcome to redistribute it in accordance with the terms "
          "and conditions in the\n";
  cout << "GNU GPL (General Public License) version 2 or later.\n";

  cout << "\nUsing configuration file: " << main_cfg_filename << endl;
  
  string value;
  if (cfg.getValue("GLOBAL", "CARD_SAMPLE_RATE", value))
  {
    int rate = atoi(value.c_str());
    if (rate == 48000)
    {
      AudioIO::setBlocksize(1024);
      AudioIO::setBlockCount(4);
    }
    else if (rate == 16000)
    {
      AudioIO::setBlocksize(512);
      AudioIO::setBlockCount(2);
    }
    #if INTERNAL_SAMPLE_RATE <= 8000
    else if (rate == 8000)
    {
      AudioIO::setBlocksize(256);
      AudioIO::setBlockCount(2);
    }
    #endif
    else
    {
      cerr << "*** ERROR: Illegal sound card sample rate specified for "
      	      "config variable GLOBAL/CARD_SAMPLE_RATE. Valid rates are "
	      #if INTERNAL_SAMPLE_RATE <= 8000
	      "8000, "
	      #endif
	      "16000 and 48000\n";
      exit(1);
    }
    AudioIO::setSampleRate(rate);
    cout << "--- Using sample rate " << rate << "Hz\n";
  }
  
    // Init locationinfo
  if (cfg.getValue("GLOBAL", "LOCATION_INFO", value))
  {
    if (!LocationInfo::initialize(cfg, value))
    {
      cerr << "*** ERROR: Could not init LocationInfo, "
           << "check configuration section LOCATION_INFO=" << value << "\n";
      exit(1);
    }
  }

    // Init Logiclinking
  if (cfg.getValue("GLOBAL", "LINKS", value))
  {
    if (!LinkManager::initialize(cfg, value))
    {
      cerr << "*** ERROR: Could not initialize link manager. "
           << "GLOBAL/LINKS=" << value << ".\n";
      exit(1);
    }
  }

  initialize_logics(cfg);

  if (LinkManager::hasInstance())
  {
    LinkManager::instance()->allLogicsStarted();
  }

  struct termios org_termios;
  if (logfile_name == 0)
  {
    struct termios termios;
    tcgetattr(STDIN_FILENO, &org_termios);
    termios = org_termios;
    termios.c_lflag &= ~(ICANON | ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &termios);

    stdin_watch = new FdWatch(STDIN_FILENO, FdWatch::FD_WATCH_RD);
    // must explicitly specify name space for ptr_fun() to avoid conflict
    // with ptr_fun() in /usr/include/c++/4.5/bits/stl_function.h
    stdin_watch->activity.connect(sigc::ptr_fun(&stdinHandler));
  }

  app.exec();

  if (stdin_watch != 0)
  {
    delete stdin_watch;
    tcsetattr(STDIN_FILENO, TCSANOW, &org_termios);
  }

  if (stdout_watch != 0)
  {
    delete stdout_watch;
    close(pipefd[0]);
    close(pipefd[1]);
  }

  vector<Logic*>::iterator lit;
  for (lit=logic_vec.begin(); lit!=logic_vec.end(); lit++)
  {
    delete *lit;
  }
  logic_vec.clear();
  
  LinkManager::deleteInstance();

  if (logfd != -1)
  {
    close(logfd);
  }
  
  if (sigaction(SIGHUP, &sighup_oldact, NULL) == -1)
  {
    perror("sigaction");
  }
  
  if (sigaction(SIGTERM, &sigterm_oldact, NULL) == -1)
  {
    perror("sigaction");
  }
  
  if (sigaction(SIGINT, &sigint_oldact, NULL) == -1)
  {
    perror("sigaction");
  }
  
  return 0;
  
} /* main */




/****************************************************************************
 *
 * Functions
 *
 ****************************************************************************/

/*
 *----------------------------------------------------------------------------
 * Function:  parse_arguments
 * Purpose:   Parse the command line arguments.
 * Input:     argc  - Number of arguments in the command line
 *    	      argv  - Array of strings with the arguments
 * Output:    Returns 0 if all is ok, otherwise -1.
 * Author:    Tobias Blomberg, SM0SVX
 * Created:   2000-06-13
 * Remarks:   
 * Bugs:      
 *----------------------------------------------------------------------------
 */
static void parse_arguments(int argc, const char **argv)
{
  poptContext optCon;
  const struct poptOption optionsTable[] =
  {
    POPT_AUTOHELP
    {"pidfile", 0, POPT_ARG_STRING, &pidfile_name, 0,
	    "Specify the name of the pidfile to use", "<filename>"},
    {"logfile", 0, POPT_ARG_STRING, &logfile_name, 0,
	    "Specify the logfile to use (stdout and stderr)", "<filename>"},
    {"runasuser", 0, POPT_ARG_STRING, &runasuser, 0,
	    "Specify the user to run SvxLink as", "<username>"},
    {"config", 0, POPT_ARG_STRING, &config, 0,
	    "Specify the configuration file to use", "<filename>"},
    /*
    {"int_arg", 'i', POPT_ARG_INT, &int_arg, 0,
	    "Description of int argument", "<an int>"},
    */
    {"daemon", 0, POPT_ARG_NONE, &daemonize, 0,
	    "Start SvxLink as a daemon", NULL},
    {NULL, 0, 0, NULL, 0}
  };
  int err;
  //const char *arg = NULL;
  //int argcnt = 0;
  
  optCon = poptGetContext(PROGRAM_NAME, argc, argv, optionsTable, 0);
  poptReadDefaultConfig(optCon, 0);
  
  err = poptGetNextOpt(optCon);
  if (err != -1)
  {
    fprintf(stderr, "\t%s: %s\n",
	    poptBadOption(optCon, POPT_BADOPTION_NOALIAS),
	    poptStrerror(err));
    exit(1);
  }

  /*
  printf("string_arg  = %s\n", string_arg);
  printf("int_arg     = %d\n", int_arg);
  printf("bool_arg    = %d\n", bool_arg);
  */
  
    /* Parse arguments that do not begin with '-' (leftovers) */
  /*
  arg = poptGetArg(optCon);
  while (arg != NULL)
  {
    printf("arg %2d      = %s\n", ++argcnt, arg);
    arg = poptGetArg(optCon);
  }
  */

  poptFreeContext(optCon);

} /* parse_arguments */


static void stdinHandler(FdWatch *w)
{
  char buf[1];
  int cnt = ::read(STDIN_FILENO, buf, 1);
  if (cnt == -1)
  {
    fprintf(stderr, "*** error reading from stdin\n");
    Application::app().quit();
    return;
  }
  else if (cnt == 0)
  {
      /* Stdin file descriptor closed */
    delete stdin_watch;
    stdin_watch = 0;
    return;
  }
  
  switch (toupper(buf[0]))
  {
    case 'Q':
      Application::app().quit();
      break;
    
    case '\n':
      putchar('\n');
      break;
    
    case '0': case '1': case '2': case '3':
    case '4': case '5': case '6': case '7':
    case '8': case '9': case 'A': case 'B':
    case 'C': case 'D': case '*': case '#':
    case 'H':
      logic_vec[0]->injectDtmfDigit(buf[0], 100);
      break;
    
    default:
      break;
  }
}


static void write_to_logfile(const char *buf)
{
  if (logfd == -1)
  {
    cout << buf;
    return;
  }
  
  const char *ptr = buf;
  while (*ptr != 0)
  {
    static bool print_timestamp = true;
    ssize_t ret;
    
    if (print_timestamp)
    {
      if (!tstamp_format.empty())
      {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        string fmt(tstamp_format);
        const string frac_code("%f");
        size_t pos = fmt.find(frac_code);
        if (pos != string::npos)
        {
          stringstream ss;
          ss << setfill('0') << setw(3) << (tv.tv_usec / 1000);
          fmt.replace(pos, frac_code.length(), ss.str());
        }
	struct tm *tm = localtime(&tv.tv_sec);
	char tstr[256];
	size_t tlen = strftime(tstr, sizeof(tstr), fmt.c_str(), tm);
	ret = write(logfd, tstr, tlen);
        assert(ret == static_cast<ssize_t>(tlen));
	ret = write(logfd, ": ", 2);
        assert(ret == 2);
	print_timestamp = false;
      }

      if (reopen_log)
      {
	const char *reopen_txt = "SIGHUP received. Reopening logfile\n";
        const size_t reopen_txt_len = strlen(reopen_txt);
	ret = write(logfd, reopen_txt, reopen_txt_len);
        assert(ret == static_cast<ssize_t>(reopen_txt_len));
	open_logfile();
	reopen_log = false;
	print_timestamp = true;
	continue;
      }
    }

    size_t write_len = 0;
    const char *nl = strchr(ptr, '\n');
    if (nl != 0)
    {
      write_len = nl-ptr+1;
      print_timestamp = true;
    }
    else
    {
      write_len = strlen(ptr);
    }
    ret = write(logfd, ptr, write_len);
    assert(ret == static_cast<ssize_t>(write_len));
    ptr += write_len;
  }
} /* write_to_logfile */


static void stdout_handler(FdWatch *w)
{
  char buf[256];
  ssize_t len = read(w->fd(), buf, sizeof(buf)-1);
  if (len == -1)
  {
    return;
  }
  buf[len] = 0;
  
  write_to_logfile(buf);
  
} /* stdout_handler  */


static void initialize_logics(Config &cfg)
{
  string logics;
  if (!cfg.getValue("GLOBAL", "LOGICS", logics) || logics.empty())
  {
    cerr << "*** ERROR: Config variable GLOBAL/LOGICS is not set\n";
    exit(1);
  }

  string::iterator comma;
  string::iterator begin = logics.begin();
  do
  {
    comma = find(begin, logics.end(), ',');
    string logic_name;
    if (comma == logics.end())
    {
      logic_name = string(begin, logics.end());
    }
    else
    {
      logic_name = string(begin, comma);
      begin = comma + 1;
    }
    
    cout << "\nStarting logic: " << logic_name << endl;
    
    string logic_type;
    if (!cfg.getValue(logic_name, "TYPE", logic_type) || logic_type.empty())
    {
      cerr << "*** ERROR: Logic TYPE not specified for logic \""
      	   << logic_name << "\". Skipping...\n";
      continue;
    }
    Logic *logic = 0;
    if (logic_type == "Simplex")
    {
      logic = new SimplexLogic(cfg, logic_name);
    }
    else if (logic_type == "Repeater")
    {
      logic = new RepeaterLogic(cfg, logic_name);
    }
    else
    {
      cerr << "*** ERROR: Unknown logic type \"" << logic_type
      	   << "\"specified for logic " << logic_name << ".\n";
      continue;
    }
    if ((logic == 0) || !logic->initialize())
    {
      cerr << "*** ERROR: Could not initialize Logic object \""
      	   << logic_name << "\". Skipping...\n";
      delete logic;
      continue;
    }
    
    logic_vec.push_back(logic);
  } while (comma != logics.end());
  
  if (logic_vec.size() == 0)
  {
    cerr << "*** ERROR: No logics available. Bailing out...\n";
    exit(1);
  }
} /* initialize_logics */


void sighup_handler(int signal)
{
  if (logfile_name == 0)
  {
    ssize_t ret = write(STDOUT_FILENO, "Ignoring SIGHUP\n", 16);
    assert(ret == 16);
    return;
  }
  
  ssize_t ret = write(STDOUT_FILENO, "SIGHUP received. Logfile reopened\n", 34);
  assert(ret == 34);
  reopen_log = true;
    
} /* sighup_handler */


void sigterm_handler(int signal)
{
  const char *signame = 0;
  switch (signal)
  {
    case SIGTERM:
      signame = "SIGTERM";
      break;
    case SIGINT:
      signame = "SIGINT";
      break;
    default:
      signame = "???";
      break;
  }
  string msg("\n");
  msg += signame;
  msg += " received. Shutting down application...\n";
  write_to_logfile(msg.c_str());
  Application::app().quit();
} /* sigterm_handler */


bool open_logfile(void)
{
  if (logfd != -1)
  {
    close(logfd);
  }
  
  logfd = open(logfile_name, O_WRONLY | O_APPEND | O_CREAT, 00644);
  if (logfd == -1)
  {
    char str[256] = "open(\"";
    strcat(str, logfile_name);
    strcat(str, "\")");
    perror(str);
    return false;
  }

  return true;
  
} /* open_logfile */


/*
 * This file has not been truncated
 */

