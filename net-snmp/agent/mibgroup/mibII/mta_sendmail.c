/*
 *  MTA-MIB implementation for sendmail - mibII/mta_sendmail.c
 *  Christoph Mammitzsch <Christoph.Mammitzsch@tu-clausthal.de>
 *
 *  05.04.2000:
 *
 *    - supports sendmail 8.10.0 statistics files now
 *    - function read_option has been removed
 *
 *  12.04.2000:
 *
 *    - renamed configuration tokens:
 *        sendmail config        -> sendmail_config
 *        sendmail stats         -> sendmail_stats
 *        sendmail queue         -> sendmail_queue
 *        sendmail index         -> sendmail_index
 *        sendmail statcachetime -> sendmail_stats_t
 *        sendmail dircacetime   -> sendmail_queue_t
 *
 *    - now using snmpd_register_config_handler instead of config_parse_dot_conf
 *
 *  15.04.2000:
 *
 *    - introduced new function print_error
 *    - changed open_sendmailst and read_sendmailcf to use the new function
 *    - changed calls to open_sendmailst and read_sendmailcf
 *    - added some error handling to calls to chdir(), close() and closedir()
 *
 */


/** "include files" */
#ifdef __lint
# define SNMP_NO_DEBUGGING 1 /* keeps lint from complaining about the DEBUGMSG* macros */
#endif

#include <config.h>

#include "mibincl.h"
#include "mta_sendmail.h"

#include <sys/types.h>

#include <stdio.h>

#include <ctype.h>

#ifdef HAVE_STRING_H
# include <string.h>
#else
# include <strings.h>
#endif

#if HAVE_STDLIB_H
#include <stdlib.h>
#endif

#ifdef HAVE_UNISTD_H
# include <unistd.h>
#endif

#ifdef HAVE_FCNTL_H
# include <fcntl.h>
#endif

#if HAVE_DIRENT_H
#include <dirent.h>
#else
# define dirent direct
# if HAVE_SYS_NDIR_H
#  include <sys/ndir.h>
# endif
# if HAVE_SYS_DIR_H
#  include <sys/dir.h>
# endif
# if HAVE_NDIR_H
#  include <ndir.h>
# endif
#endif

#ifdef HAVE_SYS_STAT_H
# include <sys/stat.h>
#endif

#if TIME_WITH_SYS_TIME
# include <sys/time.h>
# include <time.h>
#else
# if HAVE_SYS_TIME_H
#  include <sys/time.h>
# else
#  include <time.h>
# endif
#endif

#if HAVE_STDARG_H
#include <stdarg.h>
#else
#include <varargs.h>
#endif

#include <errno.h>

/**/

/** "macros and variables for registering the OID tree" */

/* prefix for all OIDs */
static oid mta_variables_oid[] = { 1,3,6,1,2,1,28 };

/* bits that indicate what's needed to compute the value */
#define   NEEDS_STATS   (1 << 6)
#define   NEEDS_DIR     (1 << 7)
#define   NEEDS         (NEEDS_STATS | NEEDS_DIR)

/* symbolic names for the magic values */
#define   MTARECEIVEDMESSAGES           3 | NEEDS_STATS
#define   MTASTOREDMESSAGES             4 | NEEDS_DIR
#define   MTATRANSMITTEDMESSAGES        5 | NEEDS_STATS
#define   MTARECEIVEDVOLUME             6 | NEEDS_STATS
#define   MTASTOREDVOLUME               7 | NEEDS_DIR
#define   MTATRANSMITTEDVOLUME          8 | NEEDS_STATS

#define   MTAGROUPRECEIVEDMESSAGES     19 | NEEDS_STATS
#define   MTAGROUPREJECTEDMESSAGES     20 | NEEDS_STATS
#define   MTAGROUPTRANSMITTEDMESSAGES  22 | NEEDS_STATS
#define   MTAGROUPRECEIVEDVOLUME       23 | NEEDS_STATS
#define   MTAGROUPTRANSMITTEDVOLUME    25 | NEEDS_STATS
#define   MTAGROUPNAME                 43
#define   MTAGROUPHIERARCHY            49

/* structure that tells the agent, which function returns what values */
static struct variable4 mta_variables[] = {
  { MTARECEIVEDMESSAGES        , ASN_COUNTER  , RONLY, var_mtaEntry     , 3, { 1, 1,  1 } },
  { MTASTOREDMESSAGES          , ASN_GAUGE    , RONLY, var_mtaEntry     , 3, { 1, 1,  2 } },
  { MTATRANSMITTEDMESSAGES     , ASN_COUNTER  , RONLY, var_mtaEntry     , 3, { 1, 1,  3 } },
  { MTARECEIVEDVOLUME          , ASN_COUNTER  , RONLY, var_mtaEntry     , 3, { 1, 1,  4 } },
  { MTASTOREDVOLUME            , ASN_GAUGE    , RONLY, var_mtaEntry     , 3, { 1, 1,  5 } },
  { MTATRANSMITTEDVOLUME       , ASN_COUNTER  , RONLY, var_mtaEntry     , 3, { 1, 1,  6 } },

  { MTAGROUPRECEIVEDMESSAGES   , ASN_COUNTER  , RONLY, var_mtaGroupEntry, 3, { 2, 1,  2 } },
  { MTAGROUPREJECTEDMESSAGES   , ASN_COUNTER  , RONLY, var_mtaGroupEntry, 3, { 2, 1,  3 } },
  { MTAGROUPTRANSMITTEDMESSAGES, ASN_COUNTER  , RONLY, var_mtaGroupEntry, 3, { 2, 1,  5 } },
  { MTAGROUPRECEIVEDVOLUME     , ASN_COUNTER  , RONLY, var_mtaGroupEntry, 3, { 2, 1,  6 } },
  { MTAGROUPTRANSMITTEDVOLUME  , ASN_COUNTER  , RONLY, var_mtaGroupEntry, 3, { 2, 1,  8 } },
  { MTAGROUPNAME               , ASN_OCTET_STR, RONLY, var_mtaGroupEntry, 3, { 2, 1, 25 } },
  { MTAGROUPHIERARCHY          , ASN_INTEGER  , RONLY, var_mtaGroupEntry, 3, { 2, 1, 31 } },
};
/**/

/** "other macros and structures" */

/* for boolean values */
#ifndef FALSE
#define FALSE 0
#endif
#ifndef TRUE
#define TRUE  1
#endif
#ifndef BOOL
#define BOOL  short
#endif

/* important constants */
#define FILENAMELEN     200   /* maximum length for filenames */
#define MAXMAILERS       25   /* maximum number of mailers (copied from the sendmail sources) */
#define MNAMELEN         20   /* maximum length of mailernames (copied from the sendmail sources) */
#define STAT_VERSION_8_9  2   /* version of sendmail V8.9.x statistics files (copied from the sendmail sources) */
#define STAT_VERSION_8_10 3   /* version of sendmail V8.10.x statistics files (copied from the sendmail sources) */
#define STAT_MAGIC  0x1B1DE   /* magic value to identify statistics files from sendmail V8.9.x or higher (copied from the sendmail sources) */

/* structure of sendmail.st file from sendmail V8.10.x (copied from the sendmail sources) */
struct statisticsV8_10
{
        int     stat_magic;             /* magic number */
        int     stat_version;           /* stat file version */
        time_t  stat_itime;             /* file initialization time */
        short   stat_size;              /* size of this structure */
        long    stat_cf;                /* # from connections */
        long    stat_ct;                /* # to connections */
        long    stat_cr;                /* # rejected connections */
        long    stat_nf[MAXMAILERS];    /* # msgs from each mailer */
        long    stat_bf[MAXMAILERS];    /* kbytes from each mailer */
        long    stat_nt[MAXMAILERS];    /* # msgs to each mailer */
        long    stat_bt[MAXMAILERS];    /* kbytes to each mailer */
        long    stat_nr[MAXMAILERS];    /* # rejects by each mailer */
        long    stat_nd[MAXMAILERS];    /* # discards by each mailer */
};

/* structure of sendmail.st file from sendmail V8.9.x (copied from the sendmail sources) */
struct statisticsV8_9
{
        int     stat_magic;             /* magic number */
        int     stat_version;           /* stat file version */
        time_t  stat_itime;             /* file initialization time */
        short   stat_size;              /* size of this structure */
        long    stat_nf[MAXMAILERS];    /* # msgs from each mailer */
        long    stat_bf[MAXMAILERS];    /* kbytes from each mailer */
        long    stat_nt[MAXMAILERS];    /* # msgs to each mailer */
        long    stat_bt[MAXMAILERS];    /* kbytes to each mailer */
        long    stat_nr[MAXMAILERS];    /* # rejects by each mailer */
        long    stat_nd[MAXMAILERS];    /* # discards by each mailer */
};

/* structure of sendmail.st file from sendmail V8.8.x (copied from the sendmail sources) */
struct statisticsV8_8
{
        time_t  stat_itime;             /* file initialization time */
        short   stat_size;              /* size of this structure */
        long    stat_nf[MAXMAILERS];    /* # msgs from each mailer */
        long    stat_bf[MAXMAILERS];    /* kbytes from each mailer */
        long    stat_nt[MAXMAILERS];    /* # msgs to each mailer */
        long    stat_bt[MAXMAILERS];    /* kbytes to each mailer */
};
/**/

/** "static variables" */

static char sendmailst_fn[FILENAMELEN+1];         /* name of statistics file */
static int  sendmailst_fh = -1;                   /* filehandle for statistics file */
static char sendmailcf_fn[FILENAMELEN+1];         /* name of sendmails config file */
static char mqueue_dn[FILENAMELEN+1];             /* name of the queue directory */
static DIR *mqueue_dp = NULL;                     /* directoryhandle for the queue directory */
static char mailernames[MAXMAILERS][MNAMELEN+1];  /* array of mailer names */
static int  mailers = MAXMAILERS;                 /* number of mailer names in array */

static long   *stat_nf;   /* pointer to stat_nf array within the statistics structure */
static long   *stat_bf;   /* pointer to stat_bf array within the statistics structure */
static long   *stat_nt;   /* pointer to stat_nt array within the statistics structure */
static long   *stat_bt;   /* pointer to stat_bt array within the statistics structure */
static long   *stat_nr;   /* pointer to stat_nr array within the statistics structure,
                             only valid for statistics files from sendmail >=V8.9.0    */
static long   *stat_nd;   /* pointer to stat_nd array within the statistics structure,
                             only valid for statistics files from sendmail >=V8.9.0    */
static int    stats_size; /* size of statistics structure */
static long   stats[sizeof (struct statisticsV8_10) / sizeof (long) + 1]; /* buffer for statistics structure */
static time_t lastreadstats;        /* time stats file has been read */
static long   mqueue_count;         /* number of messages in queue */
static long   mqueue_size;          /* total size of messages in queue */
static time_t lastreaddir;          /* time queue directory has been read */
static long   applindex = 1;        /* ApplIndex value for OIDs */
static long   stat_cache_time = 5;  /* time (in seconds) to wait before reading stats file again */
static long   dir_cache_time = 10;  /* time (in seconds) to wait before scanning queue directoy again */

/**/

/** static void print_error(int priority, BOOL config, BOOL config_only, char *function, char *format, ...)
 *
 *  Description:
 *
 *    Called to print errors. It uses the config_perror or the snmp_log function
 *    depending on whether the config parameter is TRUE or FALSE.
 *
 *  Parameters:
 *
 *    priority:    priority to be used when calling the snmp_log function
 *
 *    config:      indicates whether this function has been called during the
 *                 configuration process or not. If set to TRUE, the function
 *                 config_perror will be used to report the error.
 *
 *    config_only: if set to TRUE, the error will only be printed when function
 *                 has been called during the configuration process.
 *
 *    function:    name of the calling function. Used when printing via snmp_log.
 *
 *    format:      format string for the error message
 *
 *    ...:         additional parameters to insert into the error message string
 *
 */

#if HAVE_STDARG_H
static void print_error(int priority, BOOL config, BOOL config_only, const char *function, const char *format, ...)
#else
static void print_error(va_alist)
  va_dcl
#endif
{
  va_list ap;
  char buffer[2*FILENAMELEN+200]; /* I know, that's not perfectly safe, but since I don't use more
                                     than two filenames in one error message, that should be enough */

#if HAVE_STDARG_H
  va_start(ap, format);
#else
  int priority;
  BOOL config;
  BOOL config_only;
  const char *function;
  const char *format;

  va_start(ap);
  priority    = va_arg(ap, int);
  config      = va_arg(ap, BOOL);
  config_only = va_arg(ap, BOOL);
  function    = va_arg(ap, char *);
  format      = va_arg(ap, char *);
#endif

  vsprintf(buffer, format, ap);

  if (config)
  {
    config_perror(buffer);
  }
  else if (!config_only)
  {
    snmp_log(priority, "%s: %s\n", function, buffer);
  }
  va_end(ap);
}

/**/

/** static void open_sendmailst(BOOL config)
 *
 *  Description:
 *
 *    Closes old sendmail.st file, then tries to open the new sendmail.st file
 *    and guess it's version. If it succeeds, it initializes the stat_*
 *    pointers and the stats_size variable.
 *
 *  Parameters:
 *
 *    config: TRUE if function has been called during the configuration process
 *
 *  Returns:
 *
 *    nothing
 *
 */

static void open_sendmailst(BOOL config)
{
  int filelen;

  if (sendmailst_fh != -1)
  {
      while (close(sendmailst_fh) == -1 && errno == EINTR)
      {
        /* do nothing */
      }
  }

  sendmailst_fh = open(sendmailst_fn, O_RDONLY);

  if (sendmailst_fh == -1)
  {
    print_error(LOG_ERR, config, TRUE, "mibII/mta_sendmail.c:open_sendmailst","could not open file \"%s\"\n", sendmailst_fn);
    return;
  }

  filelen = read(sendmailst_fh, (void *)&stats, sizeof stats);

  if (((struct statisticsV8_10 *)stats)->stat_magic == STAT_MAGIC)
  {
    if (((struct statisticsV8_10 *)stats)->stat_version == STAT_VERSION_8_10 &&
        ((struct statisticsV8_10 *)stats)->stat_size    == sizeof (struct statisticsV8_10) &&
          filelen == sizeof (struct  statisticsV8_10))
    {
      DEBUGMSGTL(("mibII/mta_sendmail.c:open_sendmailst", "looks like file \"%s\" has been created by sendmail V8.10.0 or newer\n", sendmailst_fn));
      stat_nf = (((struct statisticsV8_10 *)stats)->stat_nf);
      stat_bf = (((struct statisticsV8_10 *)stats)->stat_bf);
      stat_nt = (((struct statisticsV8_10 *)stats)->stat_nt);
      stat_bt = (((struct statisticsV8_10 *)stats)->stat_bt);
      stat_nr = (((struct statisticsV8_10 *)stats)->stat_nr);
      stat_nd = (((struct statisticsV8_10 *)stats)->stat_nd);
      stats_size = sizeof (struct statisticsV8_10);
    }
    else if (((struct statisticsV8_9 *)stats)->stat_version == STAT_VERSION_8_9 &&
             ((struct statisticsV8_9 *)stats)->stat_size    == sizeof (struct statisticsV8_9) &&
             filelen == sizeof (struct  statisticsV8_9))
    {
      DEBUGMSGTL(("mibII/mta_sendmail.c:open_sendmailst", "looks like file \"%s\" has been created by sendmail V8.9.x\n", sendmailst_fn));
      stat_nf = (((struct statisticsV8_9 *)stats)->stat_nf);
      stat_bf = (((struct statisticsV8_9 *)stats)->stat_bf);
      stat_nt = (((struct statisticsV8_9 *)stats)->stat_nt);
      stat_bt = (((struct statisticsV8_9 *)stats)->stat_bt);
      stat_nr = (((struct statisticsV8_9 *)stats)->stat_nr);
      stat_nd = (((struct statisticsV8_9 *)stats)->stat_nd);
      stats_size = sizeof (struct statisticsV8_9);
    } else {
      print_error(LOG_WARNING, config, FALSE, "mibII/mta_sendmail.c:open_sendmailst", "could not guess version of statistics file \"%s\"\n", sendmailst_fn);
      while (close(sendmailst_fh) == -1 && errno == EINTR)
      {
        /* do nothing */
      }
      sendmailst_fh = -1;
    }
  } else {
    if (((struct statisticsV8_8 *)stats)->stat_size == sizeof (struct statisticsV8_8) &&
        filelen == sizeof (struct statisticsV8_8))
    {
      DEBUGMSGTL(("mibII/mta_sendmail.c:open_sendmailst", "looks like file \"%s\" has been created by sendmail V8.8.x\n", sendmailst_fn));
      stat_nf = (((struct statisticsV8_8 *)stats)->stat_nf);
      stat_bf = (((struct statisticsV8_8 *)stats)->stat_bf);
      stat_nt = (((struct statisticsV8_8 *)stats)->stat_nt);
      stat_bt = (((struct statisticsV8_8 *)stats)->stat_bt);
      stat_nr = (long *) NULL;
      stat_nd = (long *) NULL;
      stats_size = sizeof (struct statisticsV8_8);
    } else {
      print_error(LOG_WARNING, config, FALSE, "mibII/mta_sendmail.c:open_sendmailst", "could not guess version of statistics file \"%s\"\n", sendmailst_fn);
      while (close(sendmailst_fh) == -1 && errno == EINTR)
      {
        /* do nothing */
      }
      sendmailst_fh = -1;
    }
  }
}

/**/

/** static BOOL read_sendmailcf(BOOL config)
 *
 *  Description:
 *
 *    Tries to open the file named in sendmailcf_fn and to get the names of
 *    the mailers, the status file and the mailqueue directory.
 *
 *  Parameters:
 *
 *    config: TRUE if function has been called during the configuration process
 *
 *  Returns:
 *
 *    TRUE  : config file has been successfully opened
 *
 *    FALSE : could not open config file
 *
 */

static BOOL read_sendmailcf(BOOL config)
{
  FILE *sendmailcf_fp;
  char line[500];
  char *filename;
  int  linenr;
  int  linelen;
  int  found_sendmailst = FALSE;
  int  found_mqueue     = FALSE;
  int  i;


  sendmailcf_fp = fopen(sendmailcf_fn, "r");
  if (sendmailcf_fp == NULL)
  {
    print_error(LOG_ERR, config, TRUE, "mibII/mta_sendmail.c:read_sendmailcf", "could not open file \"%s\"\n", sendmailcf_fn);
    return FALSE;
  }

  /* initializes the standard mailers, which aren't necessarily mentioned in the sendmail.cf file */
  strcpy(mailernames[0],"prog");
  strcpy(mailernames[1],"*file*");
  strcpy(mailernames[2],"*include*");
  mailers=3;

  linenr = 1;
  while (fgets(line, sizeof line, sendmailcf_fp) != NULL)
  {
    linelen = strlen(line);

    if (line[linelen - 1] != '\n')
    {
      print_error(LOG_WARNING, config, FALSE, "mibII/mta_sendmail.c:read_sendmailcf", "line %d in config file \"%s\" is too long\n", linenr, sendmailcf_fn);
      while (fgets(line, sizeof line, sendmailcf_fp) != NULL && line[strlen(line) - 1] != '\n') /* skip rest of the line */
      {
        /* nothing to do */
      }
      linenr++;
      continue;
    }

    line[--linelen] = '\0';

    switch (line[0])
    {

      case 'M':

        if (mailers < MAXMAILERS)
        {
          for (i=1; line[i] != ',' && ! isspace(line[i]) && line[i] != '\0' && i <= MNAMELEN; i++)
          {
            mailernames[mailers][i-1] = line[i];
          }
          mailernames[mailers][i-1] = '\0';

          DEBUGMSGTL(("mibII/mta_sendmail.c:read_sendmailcf","found mailer \"%s\"\n",mailernames[mailers]));

          for (i=0; i < mailers && strcmp(mailernames[mailers], mailernames[i]) != 0; i++)
          {
            /* nothing to do */
          }

          if (i == mailers)
          {
            mailers++;
          } else {
            if (i < 3)
            {
              DEBUGMSGTL(("mibII/mta_sendmail.c:read_sendmailcf","mailer \"%s\" already existed, but since it's one of the predefined mailers, that's probably nothing to worry about\n", mailernames[mailers]));
            } else {
              DEBUGMSGTL(("mibII/mta_sendmail.c:read_sendmailcf","mailer \"%s\" already existed\n",mailernames[mailers]));
            }
            mailernames[mailers][0]='\0';
          }
        } else {
          print_error(LOG_WARNING, config, FALSE, "mibII/mta_sendmail.c:read_sendmailcf", "found too many mailers in config file \"%s\"\n", sendmailcf_fn);
        }


       break;

      case 'O':

        switch (line[1])
        {

          case ' ':

            if (strncasecmp(line + 2, "StatusFile", 10) == 0)
            {
              filename = line + 12;
            }
            else if (strncasecmp(line + 2, "QueueDirectory", 14) == 0)
            {
              filename = line + 16;
            }
            else
            {
              break;
            }

            if (*filename != ' ' && *filename != '=')
            {
              break;
            }

            while (*filename == ' ')
            {
              filename++;
            }

            if (*filename != '=')
            {
              print_error(LOG_WARNING, config, FALSE, "mibII/mta_sendmail.c:read_sendmailcf", "line %d in config file \"%s\" ist missing an '='\n", linenr, sendmailcf_fn);
              break;
            }

            filename++;
            while (*filename == ' ')
            {
              filename++;
            }

            if (strlen(filename) > FILENAMELEN)
            {
              print_error(LOG_WARNING, config, FALSE, "mibII/mta_sendmail.c:read_sendmailcf", "line %d config file \"%s\" contains a filename that's too long\n", linenr, sendmailcf_fn);
              break;
            }

            if (strncasecmp(line + 2, "StatusFile", 10) == 0)
            {

              strcpy(sendmailst_fn, filename);
              found_sendmailst = TRUE;
              DEBUGMSGTL(("mibII/mta_sendmail.c:read_sendmailcf","found statatistics file \"%s\"\n", sendmailst_fn));
            }
            else if (strncasecmp(line + 2, "QueueDirectory", 14) == 0)
            {
              strcpy(mqueue_dn, filename);
              found_mqueue = TRUE;
              DEBUGMSGTL(("mibII/mta_sendmail.c:read_sendmailcf","found mailqueue directory \"%s\"\n", mqueue_dn));
            } else {
              print_error(LOG_CRIT, config, FALSE, "mibII/mta_sendmail.c:read_sendmailcf", "This shouldn't happen.\n");
            }

           break;

          case 'S':

            if (strlen(line+2) > FILENAMELEN)
            {
              print_error(LOG_WARNING, config, FALSE, "mibII/mta_sendmail.c:read_sendmailcf", "line %d config file \"%s\" contains a filename that's too long\n", linenr, sendmailcf_fn);
              break;
            }
            strcpy(sendmailst_fn, line+2);
            found_sendmailst = TRUE;
            DEBUGMSGTL(("mibII/mta_sendmail.c:read_sendmailcf","found statatistics file \"%s\"\n", sendmailst_fn));
           break;

          case 'Q':

            if (strlen(line+2) > FILENAMELEN)
            {
              print_error(LOG_WARNING, config, FALSE, "mibII/mta_sendmail.c:read_sendmailcf", "line %d config file \"%s\" contains a filename that's too long\n", linenr, sendmailcf_fn);
              break;
            }
            strcpy(mqueue_dn, line+2);
            found_mqueue = TRUE;
            DEBUGMSGTL(("mibII/mta_sendmail.c:read_sendmailcf","found mailqueue directory \"%s\"\n", mqueue_dn));
           break;
        }

      break;
    }

    linenr++;
  }

  for (i = 0; i < 10 && fclose(sendmailcf_fp) != 0; i++)
  {
    /* nothing to do */
  }

  for (i = mailers; i < MAXMAILERS; i++)
  {
    mailernames[i][0] = '\0';
  }

  if (found_sendmailst)
  {
    open_sendmailst(config);
  }

  if (found_mqueue)
  {
    if (mqueue_dp)
    {
      while (closedir(mqueue_dp) == -1 && errno == EINTR)
      {
        /* do nothing */
      }

    }
    mqueue_dp = opendir(mqueue_dn);
    if (mqueue_dp == NULL)
    {
      print_error(LOG_ERR, config, FALSE, "mibII/mta_sendmail.c:read_sendmailcf", "could not open mailqueue directory \"%s\" mentioned in config file \"%s\"\n", mqueue_dn, sendmailcf_fn);
    }
  }

  return TRUE;
}
/**/

/** static void mta_sendmail_parse_config(const char* token, char *line)
 *
 *  Description:
 *
 *    Called by the agent for each configuration line that belongs to this module.
 *    The possible tokens are:
 *
 *    sendmail_config  - filename of the sendmail configutarion file
 *    sendmail_stats   - filename of the sendmail statistics file
 *    sendmail_queue   - name of the sendmail mailqueue directory
 *    sendmail_index   - the ApplIndex to use for the table
 *    sendmail_stats_t - the time (in seconds) to cache statistics
 *    sendmail_queue_t - the time (in seconds) to cache the directory scanning results
 *
 *    For "sendmail_config", "sendmail_stats" and "sendmail_queue", the copy_word
 *    function is used to copy the filename.
 *
 *  Parameters:
 *
 *    token: first word of the line
 *
 *    line:  rest of the line
 *
 *  Returns:
 *
 *    nothing
 *
 */

static void mta_sendmail_parse_config(const char *token, char *line)
{
  if (strlen(line) > FILENAMELEN) /* Might give some false alarm, but better to be safe than sorry */
  {
    config_perror("line too long");
    return;
  }

  if (strcasecmp(token,"sendmail_stats") == 0)
  {
    while (isspace(*line))
    {
      line++;
    }
    copy_word(line, sendmailst_fn);

    open_sendmailst(TRUE);

    if (sendmailst_fh == -1)
    {
      char str[FILENAMELEN+50];
      sprintf (str, "couldn't open file \"%s\"", sendmailst_fn);
      config_perror(str);
      return;
    }

    DEBUGMSGTL(("mibII/mta_sendmail.c:mta_sendmail_parse_config", "opened statistics file \"%s\"\n", sendmailst_fn));
    return;
  }
  else if (strcasecmp(token,"sendmail_config") == 0)
  {
    while (isspace(*line))
    {
      line++;
    }
    copy_word(line, sendmailcf_fn);

    read_sendmailcf(TRUE);

    DEBUGMSGTL(("mibII/mta_sendmail.c:mta_sendmail_parse_config", "read config file \"%s\"\n", sendmailcf_fn));
    return;
  }
  else if (strcasecmp(token,"sendmail_queue") == 0)
  {
    while (isspace(*line))
    {
      line++;
    }
    copy_word(line, mqueue_dn);

    if (mqueue_dp != NULL)
    {
      while (closedir(mqueue_dp) == -1 && errno == EINTR)
      {
        /* do nothing */
      }
    }

    mqueue_dp = opendir(mqueue_dn);

    if (mqueue_dp == NULL)
    {
      char str[FILENAMELEN+50];
      sprintf (str, "could not open mailqueue directory \"%s\"", mqueue_dn);
      config_perror(str);
      return;
    }

    DEBUGMSGTL(("mibII/mta_sendmail.c:mta_sendmail_parse_config", "opened mailqueue directory \"%s\"\n", mqueue_dn));
    return;
  }
  else if (strcasecmp(token,"sendmail_index") == 0)
  {
    while (isspace(*line))
    {
      line++;
    }
    applindex = atol(line);
    if (applindex < 1)
    {
      config_perror("invalid index number");
      applindex = 1;
    }
  }
  else if (strcasecmp(token,"sendmail_stats_t") == 0)
  {
    while (isspace(*line))
    {
      line++;
    }
    stat_cache_time = atol(line);
    if (stat_cache_time < 1)
    {
      config_perror("invalid cache time");
      applindex = 5;
    }
  }
  else if (strcasecmp(token,"sendmail_queue_t") == 0)
  {
    while (isspace(*line))
    {
      line++;
    }
    dir_cache_time = atol(line);
    if (dir_cache_time < 1)
    {
      config_perror("invalid cache time");
      applindex = 10;
    }
  }
  else
  {
    config_perror("mibII/mta_sendmail.c says: What should I do with that token? Did you ./configure the agent properly?");
  }

  return;
}
/**/

/** void init_mta_sendmail(void)
 *
 *  Description:
 *
 *    Called by the agent to initialize the module. The function will register
 *    the OID tree and the config handler and try some default values for the
 *    sendmail.cf and sendmail.st files and for the mailqueue directory.
 *
 *  Parameters:
 *
 *    none
 *
 *  Returns:
 *
 *    nothing
 *
 */

void init_mta_sendmail(void)
{
  REGISTER_MIB("mibII/mta_sendmail", mta_variables, variable4, mta_variables_oid);

  snmpd_register_config_handler("sendmail_config" , mta_sendmail_parse_config, NULL, "file");
  snmpd_register_config_handler("sendmail_stats"  , mta_sendmail_parse_config, NULL, "file");
  snmpd_register_config_handler("sendmail_queue"  , mta_sendmail_parse_config, NULL, "directory");
  snmpd_register_config_handler("sendmail_index"  , mta_sendmail_parse_config, NULL, "integer");
  snmpd_register_config_handler("sendmail_stats_t", mta_sendmail_parse_config, NULL, "cachetime/sec");
  snmpd_register_config_handler("sendmail_queue_t", mta_sendmail_parse_config, NULL, "cachetime/sec");

  strcpy(sendmailcf_fn, "/etc/mail/sendmail.cf");
  if (read_sendmailcf(FALSE) == FALSE)
  {
    strcpy(sendmailcf_fn, "/etc/sendmail.cf");
    read_sendmailcf(FALSE);
  }

  if (sendmailst_fh == -1)
  {
    strcpy(sendmailst_fn, "/etc/mail/statistics");
    open_sendmailst(FALSE);
    if (sendmailst_fh == -1)
    {
      strcpy(sendmailst_fn, "/etc/mail/sendmail.st");
      open_sendmailst(FALSE);
    }
  }

  if (mqueue_dp == NULL)
  {
    strcpy(mqueue_dn, "/var/spool/mqueue");
    mqueue_dp = opendir(mqueue_dn);
  }
}
/**/

/** unsigned char *var_mtaEntry(struct variable *vp, oid *name, size_t *length, int exact, size_t *var_len, WriteMethod **write_method)
 *
 *  Description:
 *
 *    Called by the agent in order to get the values for the mtaTable.
 *
 *  Parameters:
 *
 *    see agent documentation
 *
 *  Returns:
 *
 *    see agent documentation
 *
 */

unsigned char *
var_mtaEntry(struct variable *vp,
                oid     *name, 
                size_t  *length, 
                int     exact, 
                size_t  *var_len, 
                WriteMethod **write_method)
{


  static long   long_ret;
  auto   int    i;
  auto   int    result;
  auto   time_t current_time;


  if (exact)
  {
    if (*length != vp->namelen + 1)
    {
      return NULL;
    }
    result = snmp_oid_compare(name, *length - 1, vp->name, vp->namelen);
    if (result != 0 || name[*length - 1] != applindex)
    {
      return NULL;
    }
  } else {
    if (*length <= vp->namelen)
    {
      result = -1;
    } else {
      result = snmp_oid_compare(name, *length - 1, vp->name, vp->namelen);
    }
    if (result > 0)
    {
      return NULL;
    }
    if (result == 0 && name[*length - 1] >= applindex)
    {
      return NULL;
    }
    if (result < 0)
    {
      memcpy(name, vp->name, (int) vp->namelen * (int) sizeof *name);
      *length = vp->namelen + 1;
    }
    name[vp->namelen] = applindex;
  }

  *write_method = (WriteMethod *) NULL;
  *var_len = sizeof(long);    /* default to 'long' results */

  if (vp->magic & NEEDS_STATS)
  {
    if (sendmailst_fh == -1) return NULL;
    current_time = time(NULL);
    if (current_time == (time_t) -1 || current_time > lastreadstats + stat_cache_time)
    {
      if (lseek(sendmailst_fh, 0, SEEK_SET) == -1)
      {
        snmp_log(LOG_ERR, "mibII/mta_sendmail.c:var_mtaEntry: could not rewind to the beginning of file \"%s\"\n", sendmailst_fn);
        return NULL;
      }
      if (read(sendmailst_fh, (void *)&stats, stats_size) != stats_size)
      {
        snmp_log(LOG_ERR, "mibII/mta_sendmail.c:var_mtaEntry: could not read from statistics file \"%s\"\n", sendmailst_fn);
        return NULL;
      }
      if (current_time != (time_t) -1)
      {
        lastreadstats = current_time;
      }
    }
  }

  if (vp->magic & NEEDS_DIR)
  {
    if (mqueue_dp == NULL) return NULL;
    current_time = time(NULL);
    if (current_time == (time_t) -1 || current_time > lastreaddir + dir_cache_time)
    {
      struct dirent *dirptr;
      struct stat    filestat;
      char   cwd[200];
      if(getcwd(cwd, sizeof cwd) == NULL)
      {
        snmp_log(LOG_ERR, "mibII/mta_sendmail.c:var_mtaEntry: could not get current working directory\n");
        return NULL;
      }
      if(chdir(mqueue_dn) != 0)
      {
        snmp_log(LOG_ERR, "mibII/mta_sendmail.c:var_mtaEntry: could not enter mailqueue directory \"%s\"\n", mqueue_dn);
        return NULL;
      }
      rewinddir(mqueue_dp);
      mqueue_count = 0;
      mqueue_size = 0;
      while ((dirptr = readdir(mqueue_dp)) != NULL)
      {
        if(dirptr->d_name[0] == 'd' && dirptr->d_name[1] == 'f')
        {
          if(stat(dirptr->d_name, &filestat) == 0)
          {
            mqueue_size += (filestat.st_size + 999) / 1000; /* That's how sendmail calculates it's statistics too */
            mqueue_count++;
          } else {
            snmp_log(LOG_ERR, "mibII/mta_sendmail.c:var_mtaEntry: could not get size of file \"%s\" in directory \"%s\"\n", dirptr->d_name, mqueue_dn);
            if (chdir(cwd) != 0)
            {
              snmp_log(LOG_ERR, "mibII/mta_sendmail.c:var_mtaEntry: could not go back to directory \"%s\"  where I just came from\n", mqueue_dn);
            }
            return NULL;
          }
        }
      }
      if (chdir(cwd) != 0)
      {
        snmp_log(LOG_ERR, "mibII/mta_sendmail.c:var_mtaEntry: could not go back to directory \"%s\"  where I just came from\n", mqueue_dn);
      }
      if (current_time != (time_t) -1)
      {
        lastreaddir = current_time;
      }
    }
  }

  switch(vp->magic) {

    case MTARECEIVEDMESSAGES:

        long_ret = 0;
        for ( i=0; i<MAXMAILERS; i++)
        {
          long_ret += stat_nf[i];
        }
        return (unsigned char *) &long_ret;

    case MTASTOREDMESSAGES:

        long_ret = mqueue_count;
        return (unsigned char *) &long_ret;

    case MTATRANSMITTEDMESSAGES:

        long_ret = 0;
        for ( i=0; i<MAXMAILERS; i++)
        {
          long_ret += stat_nt[i];
        }
        return (unsigned char *) &long_ret;

    case MTARECEIVEDVOLUME:

        long_ret = 0;
        for ( i=0; i<MAXMAILERS; i++)
        {
          long_ret += stat_bf[i];
        }
        return (unsigned char *) &long_ret;

    case MTASTOREDVOLUME:

        long_ret = mqueue_size;
        return (unsigned char *) &long_ret;

    case MTATRANSMITTEDVOLUME:

        long_ret = 0;
        for ( i=0; i<MAXMAILERS; i++)
        {
          long_ret += stat_bt[i];
        }
        return (unsigned char *) &long_ret;

    default:
        snmp_log(LOG_ERR, "mibII/mta_sendmail.c:mtaEntry: unknown magic value\n");
  }
  return NULL;
}
/**/

/** unsigned char *var_mtaGroupEntry(struct variable *vp, oid *name, size_t *length, int exact, size_t *var_len, WriteMethod **write_method)
 *
 *  Description:
 *
 *    Called by the agent in order to get the values for the mtaGroupTable.
 *
 *  Parameters:
 *
 *    see agent documentation
 *
 *  Returns:
 *
 *    see agent documentation
 *
 */

unsigned char *
var_mtaGroupEntry(struct variable *vp,
            oid     *name,
            size_t  *length,
            int     exact,
            size_t  *var_len,
            WriteMethod **write_method)
{


  static long   long_ret;
  auto   long   row;
  auto   int    result;
  auto   time_t current_time;


  if (exact)
  {
    if (*length != vp->namelen + 2)
    {
      return NULL;
    }
    result = snmp_oid_compare(name, *length - 2, vp->name, vp->namelen);
    if (result != 0 || name[*length - 2] != applindex ||
        name[*length - 1] <= 0 || name[*length - 1] > mailers)
    {
      return NULL;
    }
  } else {
    if (*length < vp->namelen)
    {
      result = -1;
    } else {
      result = snmp_oid_compare(name, vp->namelen, vp->name, vp->namelen);
    }
    if (result > 0) /* OID prefix too large */
    {
      return NULL;
    }
    if (result == 0) /* OID prefix matches exactly,... */
    {
      if (*length > vp->namelen && name[vp->namelen] > applindex) /* ... but ApplIndex too large */
      {
        return NULL;
      }
      if (*length > vp->namelen && name[vp->namelen] == applindex) /* ... ApplIndex ok,... */
      {
        if (*length > vp->namelen + 1 && name[vp->namelen + 1] >= 1)
        {
          if (name[vp->namelen + 1] >= mailers) /* ... but mailernr too large */
          {
            return NULL;
          } else {
            name[vp->namelen + 1] ++ ;
          }
        } else {
          name[vp->namelen + 1] = 1;
        }
      } else {
        name[vp->namelen] = applindex;
        name[vp->namelen + 1] = 1;
      }
    } else { /* OID prefix too small */
      memcpy(name, vp->name, (int) vp->namelen * (int) sizeof *name);
      name[vp->namelen] = applindex;
      name[vp->namelen + 1] = 1;
    }
    *length = vp->namelen + 2;
  }

  *write_method = 0;
  *var_len = sizeof(long);    /* default to 'long' results */

  if (vp->magic & NEEDS_STATS)
  {
    if (sendmailst_fh == -1) return NULL;
     current_time = time(NULL);
    if (current_time == (time_t) -1 || current_time > lastreadstats + stat_cache_time)
    {
      if (lseek(sendmailst_fh, 0, SEEK_SET) == -1)
      {
        snmp_log(LOG_ERR, "mibII/mta_sendmail.c:var_mtaGroupEntry: could not rewind to beginning of file \"%s\"\n", sendmailst_fn);
        return NULL;
      }
      if (read(sendmailst_fh, (void *)&stats, stats_size) != stats_size)
      {
        snmp_log(LOG_ERR, "mibII/mta_sendmail.c:var_mtaGroupEntry: could not read statistics file \"%s\"\n", sendmailst_fn);
        return NULL;
      }
      if (current_time != (time_t) -1)
      {
        lastreadstats = current_time;
      }
    }
  }

  row = name[*length - 1] - 1;

  switch(vp->magic) {

    case MTAGROUPRECEIVEDMESSAGES:
        
        long_ret = (long) stat_nf[row];
        return (unsigned char *) &long_ret;

    case MTAGROUPREJECTEDMESSAGES:

        if (stat_nr != NULL && stat_nd != NULL)
        {
          long_ret = (long) (stat_nr[row] + stat_nd[row]); /* Number of rejected plus number of discarded messages */
          return (unsigned char *) &long_ret;
        } else {
          return NULL;
        }

    case MTAGROUPTRANSMITTEDMESSAGES:
        
        long_ret = (long) stat_nt[row];
        return (unsigned char *) &long_ret;

    case MTAGROUPRECEIVEDVOLUME:
        
        long_ret = (long) stat_bf[row];
        return (unsigned char *) &long_ret;

    case MTAGROUPTRANSMITTEDVOLUME:
        
        long_ret = (long) stat_bt[row];
        return (unsigned char *) &long_ret;

    case MTAGROUPNAME:

        *var_len=strlen(mailernames[row]);
        return (unsigned char *) (*var_len > 0 ? mailernames[row] : NULL);

    case MTAGROUPHIERARCHY:

        long_ret = (long) -1;
        return (unsigned char *) &long_ret;

    default:
        snmp_log(LOG_ERR, "mibII/mta_sendmail.c:mtaGroupEntry: unknown magic value\n");
  }
  return NULL;
}
/**/

