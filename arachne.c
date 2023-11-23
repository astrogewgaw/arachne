/*
 *                             __
 *      ____ __________ ______/ /_  ____  ___
 *     / __ `/ ___/ __ `/ ___/ __ \/ __ \/ _ \
 *    / /_/ / /  / /_/ / /__/ / / / / / /  __/
 *    \__,_/_/   \__,_/\___/_/ /_/_/ /_/\___/
 *
 */

#include <ctype.h>
#include <math.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/shm.h>
#include <sys/time.h>
#include <unistd.h>

/* External libraries. */
#include "extern/argtable3.h" // For argument parsing.
#include "extern/log.h"       // For logging.
#include "extern/mt19937.h"   // For random number generation.
#include "extern/toml.h"      // For parsing TOML files.

/* Arachne's version number. */
#define ARACHNE_VERSION "0.1.0"

/* SHARED MEMORY SHENANIGANS!
 * ==========================
 *
 * For a sampling time of 1.31072 ms, the shared memory at the
 * telescope is structured as 32 blocks, with each block being
 * 512 samples, or 0.67108864 s, long. The entire shared memory
 * at the telescope is 21.47483648 s long, with a size of 64 MB.
 * We then form another shared memory when we wish to search for
 * FRBs, where each block is 21.47483648 s long, and there are
 * 16 blocks. This makes this shared memory 343.59738368 s long,
 * with a size of 1 GB.
 */
#define MAXBLKS 16
#define IN_HDRKEY 2031
#define IN_BUFKEY 2032
#define OUT_HDRKEY 5031
#define OUT_BUFKEY 5032
#define BLKSIZE (32 * 512 * 4096)
#define TOTALSIZE (long)(BLKSIZE) * (long)(MAXBLKS)

/* Struct to store program configuration. */
typedef struct {
  int nf;      // Number of channels.
  double t1;   // Starting time.
  double t2;   // End time.
  double f1;   // Lowest frequency.
  double f2;   // Highest frequency.
  double dt;   // Sampling time.
  double df;   // Channel width.
  double bw;   // Bandwidth.
  double tsys; // System temperature.
  double gain; // System gain.
} Config;

/* Struct for storing data from the ring buffer. */
typedef struct {
  unsigned int flag;
  unsigned int curr_blk;
  unsigned int curr_rec;
  unsigned int blk_size;
  int overflow;
  double comptime[MAXBLKS];
  double datatime[MAXBLKS];
  unsigned char data[TOTALSIZE];
} Buffer;

/* Struct for storing the ring buffer's header. */
typedef struct {
  unsigned int active;
  unsigned int status;
  double comptime;
  double datatime;
  double reftime;
  struct timeval timestamp[MAXBLKS];
  struct timeval timestamp_gps[MAXBLKS];
  double blk_nano[MAXBLKS];
} Header;

/* Code to handle SIGINT. SIGINT is the signal sent when
 * we press Ctrl+C. One can think of SIGINT as a request
 * to interrupt or terminate the program sent by the user.
 * We choose to terminate the program when this happens.
 */
static volatile sig_atomic_t keep = 1;
static void handler(int _) {
  (void)_;
  keep = 0;
  exit(_);
}

/* Trim padding and spaces from a string. */
char *trim(char *str) {
  size_t len = 0;
  char *begp = str;
  char *endp = NULL;
  if (str == NULL) {
    return NULL;
  }
  if (str[0] == '\0') {
    return str;
  }
  len = strlen(str);
  endp = str + len;
  while (isspace((unsigned char)*begp)) {
    ++begp;
  }
  if (endp != begp) {
    while (isspace((unsigned char)*(--endp)) && endp != begp) {
    }
  }
  if (begp != str && endp == begp)
    *str = '\0';
  else if (str + len - 1 != endp)
    *(endp + 1) = '\0';
  endp = str;
  if (begp != str) {
    while (*begp) {
      *endp++ = *begp++;
    }
    *endp = '\0';
  }
  return str;
}

/* Basic functions for:
 *  1. finding the least of two numbers.
 *  2. finding the greater of two numbers.
 *  3. clipping a number between two values.
 *  4. calculating the probability of a Gaussian random variable.
 */
double min(double x, double y) { return (x < y) ? x : y; }
double max(double x, double y) { return (x > y) ? x : y; }
double clip(double x, double x1, double x2) {
  return (x >= x1 && x < x2) ? x : ((x < x1) ? x1 : x2);
}
double prob(double x) { return 0.5 + 0.5 * erf(x / sqrt(2)); }

/* Functions for random number generation. */
long set_seed() { return -time(NULL); }
double random_deviate(long *seed) {
  if (*seed < 0) {
    init_genrand(-(*seed));
    *seed = 1;
  }
  return genrand_real1();
}

/* Print Arachne's logo. */
void print_logo() {
  char *logo = "\n"
               "                         __             \n"
               "  ____ __________ ______/ /_  ____  ___ \n"
               " / __ `/ ___/ __ `/ ___/ __ \\/ __ \\/ _ \\\n"
               "/ /_/ / /  / /_/ / /__/ / / / / / /  __/\n"
               "\\__,_/_/   \\__,_/\\___/_/ /_/_/ /_/\\___/ \n"
               "                                        ";

  printf("\e[1m%s\e[m\n\n", logo);
  printf("\e[1mWeave in fake FRBs into live GMRT data.\e[m\n");
  printf("\e[1mCode: \e[4mhttps://github.com/astrogewgaw/arachne.\e[m\n\n");
}

/* The main function. */
int main(int argc, char *argv[]) {
  /* Attach the handler to SIGINT. */
  signal(SIGINT, handler);

  /*==========================================================================*/
  /*========================= ARGUMENT PARSING ===============================*/
  /*==========================================================================*/

  struct arg_lit *help;
  struct arg_lit *debug;
  struct arg_file *frbs;
  struct arg_lit *version;
  struct arg_lit *verbose;
  struct arg_file *cfgfile;
  struct arg_end *end;

  void *argtable[] = {
      help = arg_litn("h", NULL, 0, 1, "Display help."),
      version = arg_litn("V", NULL, 0, 1, "Display version."),
      debug = arg_litn("d", NULL, 0, 1, "Activate debugging mode."),
      verbose = arg_litn("v", NULL, 0, 1, "Enable verbose output."),
      cfgfile = arg_file0("c", NULL, "<file>", "Specify a configuration file."),
      frbs = arg_filen("f", NULL, "<file>", 0, argc + 2,
                       "Specify FRB file(s) to inject."),
      end = arg_end(20),
  };

  int exitcode = 0;
  char progname[] = "arachne";
  int nerrors = arg_parse(argc, argv, argtable);

  if (help->count > 0) {
    print_logo();
    printf("Usage: %s", progname);
    arg_print_syntax(stdout, argtable, "\n");
    arg_print_glossary(stdout, argtable, "  %-25s %s\n");
    exitcode = 0;
    goto exit;
  }

  if (version->count > 0) {
    printf("Version: %s\n", ARACHNE_VERSION);
    exitcode = 0;
    goto exit;
  }

  if (nerrors > 0) {
    arg_print_errors(stdout, end, progname);
    printf("Try '%s --help' for more information.\n", progname);
    exitcode = 1;
    goto exit;
  }

  print_logo();

  FILE *logfile = fopen("arachne.log", "w");
  if (logfile == NULL) {
    log_error("Could not open file for logging.");
    exit(1);
  }

  int loglvl = LOG_INFO;
  if (debug->count > 0) loglvl = LOG_DEBUG;

  log_set_level(loglvl);
  log_add_fp(logfile, loglvl);
  if (verbose->count == 0) log_set_quiet(true);

  if (cfgfile->count == 0) {
    log_error("No configuration file specified.");
    exit(1);
  }

  /*==========================================================================*/
  /*========================= CONFIGURATION PARSING ==========================*/
  /*==========================================================================*/

  FILE *cf = fopen(*cfgfile->filename, "r");
  char errbuf[200];
  if (!cf) {
    log_error("Cannot open configuration file.");
    exit(1);
  }
  toml_table_t *tab = toml_parse_file(cf, errbuf, sizeof(errbuf));
  if (!tab) {
    log_error("Cannot parse configuration file.");
    exit(1);
  }
  fclose(cf);

  toml_table_t *system = toml_table_in(tab, "system");
  if (!system) {
    log_error("Missing [system] in configuration.");
    exit(1);
  }

  toml_table_t *bursts = toml_table_in(tab, "bursts");
  if (!bursts) {
    log_error("Missing [bursts] in configuration.");
    exit(1);
  }

  toml_datum_t nf = toml_int_in(system, "nf");
  toml_datum_t t1 = toml_double_in(system, "t1");
  toml_datum_t t2 = toml_double_in(system, "t2");
  toml_datum_t f1 = toml_double_in(system, "f1");
  toml_datum_t f2 = toml_double_in(system, "f2");
  toml_datum_t dt = toml_double_in(system, "dt");
  toml_datum_t tsys = toml_double_in(system, "tsys");
  toml_datum_t gain = toml_double_in(system, "gain");

  if (!nf.ok) {
    log_error("Need to specify the number of frequency channels.");
    exit(1);
  }

  if (!t1.ok) {
    log_error("Need to specify a starting time.");
    exit(1);
  }

  if (!t2.ok) {
    log_error("Need to specify a end time.");
    exit(1);
  }

  if (!f1.ok) {
    log_error("Need to specify the lowest frequency of the band.");
    exit(1);
  }

  if (!f2.ok) {
    log_error("Need to specify the highest frequency of the band.");
    exit(1);
  }

  if (!dt.ok) {
    log_error("Need to specify the sampling time.");
    exit(1);
  }

  if (!tsys.ok) {
    log_error("Need to specify the system temperature.");
    exit(1);
  }

  if (!gain.ok) {
    log_error("Need to specify the system gain.");
    exit(1);
  }

  Config cfg;
  cfg.nf = nf.u.i;
  cfg.t1 = t1.u.d;
  cfg.t2 = t2.u.d;
  cfg.f1 = f1.u.d;
  cfg.f2 = f2.u.d;
  cfg.dt = dt.u.d;
  cfg.tsys = tsys.u.d;
  cfg.gain = gain.u.d;
  cfg.bw = cfg.f2 - cfg.f1;
  cfg.df = cfg.bw / (double)cfg.nf;

  log_info("Start time = %.2f s.", cfg.t1);
  log_info("End time = %.2f s.", cfg.t2);
  log_info("Lowest frequency = %.2f MHz.", cfg.f1);
  log_info("Highest frequency = %.2f MHz.", cfg.f2);
  log_info("Bandwidth = %.2f MHz.", cfg.bw);
  log_info("Channel width = %.2f kHz.", cfg.df * 1e3);
  log_info("Number of channels = %d.", cfg.nf);
  log_info("Sampling time = %e s.", cfg.dt);
  log_info("System temperature = %.2f K.", cfg.tsys);
  log_info("System gain = %.2f Jy / K.", cfg.gain);

  /* If debugging, dump data from ring buffer to file. */
  FILE *dump;
  if (debug->count > 0) {
    dump = fopen("temp.raw", "w");
    if (dump == NULL) {
      log_error("Could not open file.");
      exit(1);
    }
  }

  /*==========================================================================*/
  /*======================= SHARED MEMORY SHENANIGANS ========================*/
  /*==========================================================================*/

  unsigned char *raw = (unsigned char *)malloc(BLKSIZE);

  int recNumRead = 0;
  int recNumWrite = 0;
  int currentReadBlock = 0;

  int idHdrRead = shmget(IN_HDRKEY, sizeof(Header), SHM_RDONLY);
  int idBufRead = shmget(IN_BUFKEY, sizeof(Buffer), SHM_RDONLY);
  if (idHdrRead < 0 || idBufRead < 0) {
    log_error("Shared memory does not exist.");
    exit(1);
  }

  Header *HdrRead = (Header *)shmat(idHdrRead, 0, 0);
  Buffer *BufRead = (Buffer *)shmat(idBufRead, 0, 0);
  if ((BufRead) == (Buffer *)-1) {
    log_error("Could not attach to shared memory.");
    exit(1);
  } else {
    log_info("Attached to shared memory with id = %d.", idBufRead);
  }

  int idHdrWrite = shmget(OUT_HDRKEY, sizeof(Header), IPC_CREAT | 0666);
  int idBufWrite = shmget(OUT_BUFKEY, sizeof(Buffer), IPC_CREAT | 0666);
  if (idHdrWrite < 0 || idBufWrite < 0) {
    log_error("Could not create shared memory.");
    exit(1);
  }

  Header *HdrWrite = (Header *)shmat(idHdrWrite, 0, 0);
  Buffer *BufWrite = (Buffer *)shmat(idBufWrite, 0, 0);
  if ((BufWrite) == (Buffer *)-1) {
    log_error("Could not attach to shared memory.");
    exit(1);
  } else {
    log_info("Created another shared memory with id = %d.", idBufWrite);
  }

  BufWrite->curr_rec = 0;
  BufWrite->curr_blk = 0;
  recNumWrite = (BufWrite->curr_rec) % MAXBLKS;
  HdrWrite->active = 1;

  /*==========================================================================*/
  /*======================== MAIN EXECUTION LOOP =============================*/
  /*==========================================================================*/

  while (keep) {
    int flag = 0;
    while (currentReadBlock == BufRead->curr_blk) {
      usleep(2000);
      if (flag == 0) {
        log_debug("Waiting...");
        flag = 1;
      }
    }
    if (flag == 1) log_debug("Ready!");
    double blktime = ((double)BLKSIZE / 4096) * cfg.dt;
    log_debug("Reading block no. %d", currentReadBlock);
    log_debug("At t = %f s in the data.", (double)currentReadBlock * blktime);

    if (BufRead->curr_blk - currentReadBlock >= MAXBLKS - 1) {
      log_debug("Realigning...");
      recNumRead = (BufRead->curr_rec - 1 + MAXBLKS) % MAXBLKS;
      currentReadBlock = BufRead->curr_blk - 1;
    }

    memcpy(raw, BufRead->data + (long)BLKSIZE * (long)recNumRead, BLKSIZE);

    /*==================================================================*/
    /*======================== REQUANTIZATION ==========================*/
    /*==================================================================*/

    unsigned char temp = 0;
    for (int i = 0; i < BLKSIZE; i = i + 4) {
      temp = ((((raw[i + 0] << 2) & 0xc0) >> 6) & 0x03) |
             ((((raw[i + 1] << 2) & 0xc0) >> 4) & 0x0c) |
             ((((raw[i + 2] << 2) & 0xc0) >> 2) & 0x30) |
             ((((raw[i + 3] << 2) & 0xc0)));

      raw[i + 3] = (temp & 0x03);
      raw[i + 2] = (temp & 0x0c) >> 2;
      raw[i + 1] = (temp & 0x30) >> 4;
      raw[i + 0] = (temp & 0xc0) >> 6;
    }

    /*==================================================================*/
    /*======================== FRB INJECTION ===========================*/
    /*==================================================================*/

    if (frbs->count > 0) {
      log_info("Weaving in %d FRB(s) into shared memory.", frbs->count);
      for (int idx = 0; idx < frbs->count; ++idx) {
        /* Get the burst's data and metadata. */
        FILE *bf = fopen(frbs->filename[idx], "r");

        long M, N, nnz = 0;
        double dm, flux, width, tburst;

        fread(&M, sizeof(long), 1, bf);
        fread(&N, sizeof(long), 1, bf);
        fread(&nnz, sizeof(long), 1, bf);
        fread(&dm, sizeof(double), 1, bf);
        fread(&flux, sizeof(double), 1, bf);
        fread(&width, sizeof(double), 1, bf);
        fread(&tburst, sizeof(double), 1, bf);

        int *rows = (int *)malloc(nnz * sizeof(int));
        int *cols = (int *)malloc(nnz * sizeof(int));
        float *fluxes = (float *)malloc(nnz * sizeof(float));
        if (nnz > 0) {
          for (int i = 0; i < nnz; ++i) fread(&rows[i], sizeof(int), 1, bf);
          for (int i = 0; i < nnz; ++i) fread(&cols[i], sizeof(int), 1, bf);
          for (int i = 0; i < nnz; ++i) fread(&fluxes[i], sizeof(float), 1, bf);
        } else {
          log_warn("Cannot inject since no burst in the file.");
          continue;
        }

        /* Set the seed for injection. */
        long seed = set_seed();

        /* Begin injection. */
        for (int i = 0; i < nnz; ++i) {
          float flux = fluxes[i];
          double sigma =
              cfg.tsys / cfg.gain / sqrt(2 * cfg.dt * (cfg.df * 1e6));

          long offset = (long)(tburst / cfg.dt);
          long blkbeg = (long)currentReadBlock * (long)BLKSIZE;
          long blkend = (long)(currentReadBlock + 1) * (long)BLKSIZE;
          long I = (offset + (long)rows[i]) * (long)cfg.nf + (long)cols[i];

          if ((I <= blkbeg) || (I >= blkend))
            break;
          else {
            if (i == 0) {
              log_info(
                  "Injecting FRB %d with DM = %lf pc per cm^-3, flux = %lf Jy, "
                  "width = %lf s, at %lf s in the stream.",
                  idx + 1, dm, flux, width, tburst);
            }
          }
          I = I % (long)BLKSIZE;

          int out;
          int in = raw[I];

          double lvl = 1;
          double plvl1, plvl2, plvl3;
          double signal = flux / sigma;
          double pval = random_deviate(&seed);

          if (in == 3)
            out = 3;
          else if (in == 2) {
            plvl1 = (prob(max(0, lvl - signal)) - 0.5) / (prob(lvl) - 0.5);

            if (pval < plvl1)
              out = 2;
            else
              out = 3;
          } else if (in == 1) {
            plvl1 =
                (0.5 - prob(clip(-lvl, 0, lvl - signal))) / (0.5 - prob(-lvl));
            plvl2 = plvl1 + (prob(clip(-lvl, 0, lvl - signal)) -
                             prob(max(-signal, -lvl))) /
                                (0.5 - prob(-lvl));

            if (pval < plvl1)
              out = 3;
            else if (pval < plvl2)
              out = 2;
            else
              out = 1;
          } else if (in == 0) {
            plvl1 = (prob(-lvl) - prob(min(-signal + lvl, -lvl))) / prob(-lvl);
            plvl2 = plvl1 + (prob(min(-signal + lvl, -lvl)) -
                             prob(min(-signal, -lvl))) /
                                prob(-lvl);
            plvl3 = plvl2 + (prob(min(-signal, -lvl)) - prob(-lvl - signal)) /
                                prob(-lvl);

            if (pval < plvl1)
              out = 3;
            else if (pval < plvl2)
              out = 2;
            else if (pval < plvl3)
              out = 1;
            else
              out = 0;
          }
          raw[I] = out;
          if (i == nnz - 1)
            log_info("Injection successful for FRB %d.", idx + 1);
        }
        free(rows);
        free(cols);
        free(fluxes);
        fclose(bf);
      }
    } else {
      log_warn("No file(s) specified for the simulated FRB(s).");
      log_warn("Arachne won't weave in anything into shared memory.");
    }

    if (debug->count > 0) fwrite(raw, 1, BLKSIZE, dump);

    memcpy(BufWrite->data + (long)BLKSIZE * (long)recNumWrite, raw, BLKSIZE);

    recNumRead = (recNumRead + 1) % MAXBLKS;
    currentReadBlock++;

    BufWrite->curr_rec = (recNumWrite + 1) % MAXBLKS;
    BufWrite->curr_blk += 1;
    recNumWrite = (recNumWrite + 1) % MAXBLKS;
  }
  free(raw);    /* Free the memory allocated for data. */
  fclose(dump); /* Close the file opened for debugging. */

/* Free up memory if and when the argument parsing exits. */
exit:
  arg_freetable(argtable, sizeof(argtable) / sizeof(argtable[0]));
  return exitcode;
}
