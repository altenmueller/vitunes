/*
 * Copyright (c) 2010, 2011 Ryan Flannery <ryan.flannery@gmail.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "vitunes.h"
#include "config.h"     /* NOTE: must be after vitunes.h */
#include "socket.h"

/*****************************************************************************
 * GLOBALS, EXPORTED
 ****************************************************************************/

/* key playlists to keep track of */
playlist *viewing_playlist;
playlist *playing_playlist;

/* visual mode start position */
int visual_mode_start = -1;

/* signal flags */
volatile sig_atomic_t VSIG_QUIT = 0;            /* 1 = quit vitunes */
volatile sig_atomic_t VSIG_RESIZE = 0;          /* 1 = resize display */
volatile sig_atomic_t VSIG_SIGCHLD = 0;         /* 1 = got sigchld */
volatile sig_atomic_t VSIG_PLAYER_MONITOR = 0;  /* 1 = update player stats */

/*
 * enum used for QUIT_CAUSE values. Currently only one is used, but might add
 * more.  see the end of the main function and the signal_handler for how/why
 * this is used.
 */
enum { EXIT_NORMAL, BAD_PLAYER };
volatile sig_atomic_t QUIT_CAUSE = EXIT_NORMAL;

/* used with -DDEBUG */
FILE  *debug_log;


/*****************************************************************************
 * GLOBALS, LOCAL
 ****************************************************************************/

/* location patterns of various things (%s is user's home directory) */
const char *VITUNES_DIR_FMT  = "%s/.vitunes";
const char *CONF_FILE_FMT    = "%s/.vitunes/vitunes.conf";
const char *DB_FILE_FMT      = "%s/.vitunes/vitunes.db";
const char *PLAYLIST_DIR_FMT = "%s/.vitunes/playlists";
const char *INPUT_FILE_FMT   = "%s/.vitunes/input";

/* absolute paths of key stuff */
char *vitunes_dir;
char *conf_file;
char *db_file;
char *playlist_dir;
char *player_backend;
char *input_file;


/*****************************************************************************
 * local functions
 ****************************************************************************/

/* misc. functions */
int  handle_switches(int argc, char *argv[]);
void usage(const char *);
void signal_handler(int);
void setup_timer();


int
main(int argc, char *argv[])
{
   char   input_buffer[256];
   char  *home;
   int    previous_command;
   int    input;
   int    sock = -1;
   fd_set fds;

#ifdef DEBUG
   if ((debug_log = fopen("vitunes-debug.log", "w")) == NULL)
      err(1, "failed to open debug log");
#endif

   /*------------------------------------------------------------------------
    * build paths names needed by vitunes & handle switches
    *----------------------------------------------------------------------*/

   /* get home dir */
   if ((home = getenv("HOME")) == NULL)
      errx(1, "$HOME not set. Can't find my config files.");

   /* build paths & other needed strings */
   asprintf(&vitunes_dir,    VITUNES_DIR_FMT,  home);
   asprintf(&conf_file,      CONF_FILE_FMT,    home);
   asprintf(&db_file,        DB_FILE_FMT,      home);
   asprintf(&playlist_dir,   PLAYLIST_DIR_FMT, home);
   asprintf(&input_file,     INPUT_FILE_FMT,   home);
   asprintf(&player_backend, "%s", DEFAULT_PLAYER_BACKEND);
   if (vitunes_dir == NULL || conf_file == NULL
   ||  db_file == NULL     || playlist_dir == NULL
   ||  player_backend == NULL)
      err(1, "failed to create needed file names");

   /* handle command-line switches & e-commands */
   handle_switches(argc, argv);

   if(sock_send_msg(VITUNES_RUNNING) != -1) {
      printf("Vitunes appears to be running already. Won't open socket.");
   } else {
      if((sock = sock_listen()) == -1)
         errx(1, "failed to open socket.");
   }


   /*
    * IF we've reached here, then there were no e-commands.
    * start vitunes normally...
    */


   /*------------------------------------------------------------------------
    * initialize stuff
    *--------------------------------------------------------------------- */

   /* setup signal handlers (XXX must be before player_init) */
   signal(SIGPIPE,  SIG_IGN);          /* broken pipe with child (ignore) */
   signal(SIGCHLD,  signal_handler);   /* child died */
   signal(SIGHUP,   signal_handler);   /* quit */
   signal(SIGINT,   signal_handler);   /* quit */
   signal(SIGQUIT,  signal_handler);   /* quit */
   signal(SIGTERM,  signal_handler);   /* quit */
   signal(SIGWINCH, signal_handler);   /* resize */
   setup_timer();                      /* periodic timer to update player */

   /* init small stuff (XXX some must be done before medialib_load) */
   mi_query_init();        /* global query description */
   mi_sort_init();         /* global sort description */
   mi_display_init();      /* global display description */
   ybuffer_init();         /* global yank/copy buffer */
   toggleset_init();       /* global toggleset (list of toggle-lists) */

   /* load media library (database and all playlists) & sort */
   medialib_load(db_file, playlist_dir);
   if (mdb.library->nfiles == 0) {
      printf("The vitunes database is currently empty.\n");
      printf("See 'vitunes -e help add' for how to add files.");
      return 0;
   }

   /* apply default sort to library */
   qsort(mdb.library->files, mdb.library->nfiles, sizeof(meta_info*), mi_compare);

   /* setup user interface and default colors */
   kb_init();
   ui_init(DEFAULT_LIBRARY_WINDOW_WIDTH);
   paint_setup_colors();

   /* basic ui setup to get ui started */
   setup_viewing_playlist(mdb.library);
   ui.library->nrows  = mdb.nplaylists;
   playing_playlist = NULL;
   player_info.mode = DEFAULT_PLAYER_MODE;

   /* load config file and run commands in it now */
   load_config();

   /* start media player child */
   player_init(player_backend);
   atexit(player_destroy);

   /* initial painting of the display */
   paint_all();

   /* -----------------------------------------------------------------------
    * begin input loop
    * -------------------------------------------------------------------- */

   previous_command = -1;
   while (!VSIG_QUIT) {
      struct timeval  tv;

      /* handle any signal flags */
      process_signals();

      tv.tv_sec = 1;
      tv.tv_usec = 0;

      /* handle input file */
      if ( gnum_get() == 0 ) {
        FILE* input_file_ptr = fopen( input_file, "r+" );
        if ( input_file_ptr ) {
          if ( fgets( input_buffer, 255, input_file_ptr ) ) {
            kb_execute( input_buffer[0] );
            fclose( input_file_ptr );
            remove( input_file );
          }
        }
      }

      FD_ZERO(&fds);
      FD_SET(0, &fds);
      if(sock > 0)
         FD_SET(sock, &fds);
      errno = 0;
      if(select((sock > 0 ? sock : 0) + 1, &fds, NULL, NULL, &tv) == -1) {
         if(errno == 0 || errno == EINTR)
            continue;
         break;
      }

      if(sock > 0) {
         if(FD_ISSET(sock, &fds))
            sock_recv_and_exec(sock);
      }

      if(FD_ISSET(0, &fds)) {
         /* handle any available input */
         if ((input = getch()) && input != ERR) {
            if (isdigit(input) &&  (input != '0' || gnum_get() > 0))
               gnum_add(input - '0');
            else if (input == '\n' && gnum_get() > 0 && previous_command >= 0)
               kb_execute(previous_command);
            else
               kb_execute(input);
         }
      }

   }

   /* -----------------------------------------------------------------------
    * cleanup
    * -------------------------------------------------------------------- */

   ui_destroy();
   player_destroy();
   medialib_destroy();

   mi_query_clear();
   ybuffer_free();
   toggleset_free();

   sock_remove(sock);

   free(player_backend);
   free(input_file);
   free(playlist_dir);
   free(db_file);
   free(conf_file);
   free(vitunes_dir);

   /* do we have any odd cause for quitting? */
   if (QUIT_CAUSE != EXIT_NORMAL) {
      switch (QUIT_CAUSE) {
         case BAD_PLAYER:
            warnx("It appears the media player is misbehaving.  Apologies.");
            break;
      }
   }

   return 0;
}

/* print proper usage */
void
usage(const char *pname)
{
   fprintf(stderr,"\
usage: %s [-f config-file] [-d database-file] [-p playlist-dir] [-m player-path] [-e COMMAND ...]\n\
See \"%s -e help\" for information about what e-commands are available.\n\
",
   pname, pname);
   exit(1);
}

/* actual signal handler */
void
signal_handler(int sig)
{
   switch (sig) {
      case SIGHUP:
      case SIGINT:
      case SIGQUIT:
      case SIGTERM:
         VSIG_QUIT = 1;
         break;
      case SIGALRM:
         VSIG_PLAYER_MONITOR = 1;
         break;
      case SIGWINCH:
         VSIG_RESIZE = 1;
         break;
      case SIGCHLD:
         VSIG_SIGCHLD = 1;
         break;
   }
}

/* handle any signal flags */
void
process_signals()
{
   static playlist *prev_queue = NULL;
   static int       prev_qidx = -1;
   static bool      prev_is_playing = false;
   static float     prev_volume = -1;

   /* handle resize event */
   if (VSIG_RESIZE) {
      ui_resize();
      ui_clear();
      paint_all();
      VSIG_RESIZE = 0;
   }

   /* monitor player */
   if (VSIG_PLAYER_MONITOR) {
      player_monitor();

      if (prev_is_playing || player.playing())
         paint_player();

      /* need to repaint anything else? */
      if (prev_is_playing != player.playing()) {
         paint_library();
         paint_playlist();
      } else if (prev_queue != player_info.queue) {
         paint_library();
         if (prev_queue == viewing_playlist) {
            paint_playlist();
         }
      }
      if (player_info.queue == viewing_playlist
      &&  prev_qidx != player_info.qidx) {
         paint_playlist();
      }
      if (prev_volume != player.volume()) {
         paint_message("volume: %3.0f%%", player.volume());
         prev_volume = player.volume();
      }

      prev_queue = player_info.queue;
      prev_qidx = player_info.qidx;
      prev_is_playing = player.playing();
      VSIG_PLAYER_MONITOR = 0;
   }

   /* restart player if needed */
   if (VSIG_SIGCHLD) {
      if (player.sigchld != NULL) player.sigchld();
      VSIG_SIGCHLD = 0;
   }
}

/* setup timer signal handler above */
void
setup_timer()
{
   struct sigaction sig_act;
   struct itimerval timer;

   /* create timer signal handler */
   if (sigemptyset(&sig_act.sa_mask) < 0)
      err(1, "setup_timer: sigemptyset failed");

   sig_act.sa_flags = 0;
   sig_act.sa_handler = signal_handler;
   if (sigaction(SIGALRM, &sig_act, NULL) < 0)
      err(1, "setup_timer: sigaction failed");

   /* setup timer details */
   timer.it_value.tv_sec = 0;
   timer.it_value.tv_usec = 500000;    /* 1 half-second */
   timer.it_interval.tv_sec = 0;
   timer.it_interval.tv_usec = 500000; /* 1 half-second */

   /* register timer */
   if (setitimer(ITIMER_REAL, &timer, NULL) < 0)
      err(1, "setup_timer: setitimer failed");
}

/*
 * load config file and execute all command-mode commands within.
 * XXX note that this requires mdb, ui, and player to all be loaded/setup
 */
void
load_config()
{
   const char *errmsg = NULL;
   size_t  length, linenum;
   FILE   *fin;
   char   *line;
   char   *copy;
   char  **argv;
   int     argc;
   bool    found;
   int     found_idx = 0;
   int     num_matches;
   int     i, ret;

   if ((fin = fopen(conf_file, "r")) == NULL)
      return;

   linenum = 0;
   while (!feof(fin)) {

      /* get next line */
      if ((line = fparseln(fin, &length, &linenum, NULL, 0)) == NULL) {
         if (ferror(fin))
            err(1, "error reading config file '%s'", conf_file);
         else
            break;
      }

      /* skip whitespace */
      copy = line;
      copy += strspn(copy, " \t\n");
      if (copy[0] == '\0') {
         free(line);
         continue;
      }

      /* parse line into argc/argv */
      if (str2argv(copy, &argc, &argv, &errmsg) != 0) {
         endwin();
         errx(1, "%s line %zd: parse error: %s", conf_file, linenum, errmsg);
      }

      /* run command */
      found = false;
      num_matches = 0;
      for (i = 0; i < CommandPathSize; i++) {
         if (match_command_name(argv[0], CommandPath[i].name)) {
            found = true;
            found_idx = i;
            num_matches++;
         }
      }

      if (found && num_matches == 1) {
         if ((ret = (CommandPath[found_idx].func)(argc, argv)) != 0) {
            endwin();
            errx(1, "%s line %zd: error with command '%s' [%i]",
               conf_file, linenum, argv[0], ret);
         }
      } else if (num_matches > 1) {
         endwin();
         errx(1, "%s line %zd: ambiguous abbreviation '%s'",
            conf_file, linenum, argv[0]);
      } else {
         endwin();
         errx(1, "%s line %zd: unknown command'%s'",
            conf_file, linenum, argv[0]);
      }

      argv_free(&argc, &argv);
      free(line);
   }

   fclose(fin);
}

/*
 * parse the command line and handle all switches.
 * this also handles all of the e-commands.
 */
int
handle_switches(int argc, char *argv[])
{
   int ch;
   int i;
   int had_c_commands = 0;

   while ((ch = getopt(argc, argv, "he:f:d:p:m:c:")) != -1) {
      switch (ch) {
         case 'c':
            if(sock_send_msg(optarg) == -1)
               errx(1, "Failed to send message. Vitunes not running?");
            had_c_commands = 1;
            break;

         case 'd':
            if ((db_file = strdup(optarg)) == NULL)
               err(1, "handle_switches: strdup db_file failed");
            break;

         case 'e':
            /* an e-command */
            argc -= optind - 1;
            argv += optind - 1;

            for (i = 0; i < ECMD_PATH_SIZE; i++) {
               if (strcmp(optarg, ECMD_PATH[i].name) == 0)
                  exit((ECMD_PATH[i].func)(argc, argv));
            }

            errx(1, "Unknown e-command '%s'.  See 'vitunes -e help' for list.",
               optarg);
            break;

         case 'f':
            if ((conf_file = strdup(optarg)) == NULL)
               err(1, "handle_switches: strdup conf_file failed");
            break;

         case 'm':
            if ((player_backend = strdup(optarg)) == NULL)
               err(1, "handle_switches: strdup player_backend failed");
            break;

         case 'p':
            if ((playlist_dir = strdup(optarg)) == NULL)
               err(1, "handle_switches: strdup playlist_dir failed");
            break;

         case 'h':
         case '?':
         default:
            usage(argv[0]);
            /* NOT REACHED */
      }
   }

   if(had_c_commands)
      exit(0);

   return 0;
}
