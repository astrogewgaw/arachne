/*
  ▄▀▄ █▀▄ ▄▀▄ ▄▀▀ █▄█ █▄ █ ██▀
  █▀█ █▀▄ █▀█ ▀▄▄ █ █ █ ▀█ █▄▄

  Weave in fake FRBs into live GMRT data.
  Code: https://github.com/astrogewgaw/arachne.
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

/* Struct to store program configuration. */
typedef struct {
  int nf;         // Number of channels.
  double fl;      // Lowest frequency.
  double fh;      // Highest frequency.
  double dt;      // Sampling time.
  double df;      // Channel width.
  double bw;      // Bandwidth.
  double tsys;    // System temperature.
  double antgain; // Antenna gain.
  double sysgain; // System gain.
} Config;

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

/* Trim out whitespace and nulls from a string. */
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

/* Find the lesser of two numbers. */
double min(double x1, double x2) {
  if (x1 < x2) return x1;
  return x2;
}

/* Find the greater of two numbers. */
double max(double x1, double x2) {
  if (x1 > x2) return x1;
  return x2;
}

/* Clip a number b/w two values. */
double clip(double x, double x1, double x2) {
  if (x >= x1 && x < x2) return x;
  if (x < x1) return x1;
  return x2;
}

/* Find the probability of a Gaussian random variable. */
double prob(double x) { return 0.5 + 0.5 * erf(x / sqrt(2)); }

/* Set the seed for the RNG. */
long set_seed() { return -time(NULL); }

/* Get a random number from the RNG b/w 0 and 1. */
double random_deviate(long *seed) {
  if (*seed < 0) {
    init_genrand(-(*seed));
    *seed = 1;
  }
  return genrand_real1();
}

/* Calculate probability of shifting the bit: 8 bit injection */
int cal_bit_shift_prob(int in, double pval, double lvl, double signal) {
  double plvl;
  int out;
  if (in == 255){
    out = in;
  } else {
    for (int m = 255 - in; m >= 0; m--) {
      plvl = ((prob(min((in - 127)*lvl, (in + m - 127)*lvl - signal)) - prob(max((in - 128)*lvl, (in + m - 128)*lvl - signal)))/
                (prob((in - 127)*lvl) - prob((in - 128)*lvl))); // prob n--> n+m
      if (pval < plvl){
        out = in + m;
        break;
      }
    }
  }
  return out;
}
/* Print Arachne's logo. */
void print_logo() {
  char *logo = "\n"
               " ▄▀▄ █▀▄ ▄▀▄ ▄▀▀ █▄█ █▄ █ ██▀\n"
               " █▀█ █▀▄ █▀█ ▀▄▄ █ █ █ ▀█ █▄▄\n";

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
      cfgfile = arg_file0("c", NULL, "<FILE>", "Specify config file."),
      frbs = arg_filen(NULL, NULL, "<FRB>", 0, argc + 2, "FRBs to inject."),
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

  if (cfgfile->count == 0) {
    printf("No configuration file specified.\n");
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
  toml_table_t *fields = toml_parse_file(cf, errbuf, sizeof(errbuf));
  if (!fields) {
    log_error("Cannot parse configuration file.");
    exit(1);
  }
  fclose(cf);

  toml_table_t *opts = toml_table_in(fields, "opts");
  toml_table_t *sys = toml_table_in(fields, "system");

  toml_datum_t dumpmode = toml_bool_in(opts, "dump");
  toml_datum_t debugmode = toml_bool_in(opts, "debug");
  toml_datum_t verbmode = toml_bool_in(opts, "verbose");
  toml_datum_t debugfile = toml_string_in(opts, "debugfile");

  toml_datum_t nf = toml_int_in(sys, "nchan");
  toml_datum_t band = toml_int_in(sys, "band");
  toml_datum_t dt = toml_double_in(sys, "tsamp");
  toml_datum_t nantennas = toml_int_in(sys, "nantennas");
  toml_datum_t arraytype = toml_string_in(sys, "arraytype");

  /*==========================================================================*/
  /*============================= LOGGING SETUP ==============================*/
  /*==========================================================================*/

  FILE *logfile = fopen("arachne.log", "w");
  if (logfile == NULL) {
    printf("Could not open file for logging.\n");
    exit(1);
  }

  int loglvl = LOG_INFO;
  if ((debug->count > 0) || (debugmode.u.b)) loglvl = LOG_DEBUG;

  log_set_level(loglvl);
  log_add_fp(logfile, loglvl);
  if (!((verbose->count > 0) || verbmode.u.b)) log_set_quiet(true);

  /*==========================================================================*/
  /*========================== CONFIGURATION SETUP ===========================*/
  /*==========================================================================*/

  Config cfg;
  cfg.nf = (nf.ok) ? nf.u.i : 4096;
  cfg.dt = (dt.ok) ? dt.u.d : 1.31072e-3;
  switch (band.u.i) {
  case 2:
    log_error("Band 2 not yet supported.");
    exit(1);
  case 3:
    cfg.fl = 300.0;
    cfg.fh = 500.0;
    cfg.tsys = 165.0;
    cfg.antgain = 0.38;
    break;
  case 4:
    cfg.fl = 550.0;
    cfg.fh = 750.0;
    cfg.tsys = 100.0;
    cfg.antgain = 0.32;
    break;
  case 5:
    cfg.fl = 1000.0;
    cfg.fh = 1400.0;
    cfg.tsys = 75.0;
    cfg.antgain = 0.22;
    break;
  default:
    log_error("This band does not exist at the GMRT.");
    exit(1);
  }
  cfg.bw = cfg.fh - cfg.fl;
  cfg.df = cfg.bw / (double)cfg.nf;
  cfg.sysgain = cfg.antgain * nantennas.u.i;

  log_info("Lowest frequency = %.2f MHz.", cfg.fl);
  log_info("Highest frequency = %.2f MHz.", cfg.fh);
  log_info("Bandwidth = %.2f MHz.", cfg.bw);
  log_info("Channel width = %.2f kHz.", cfg.df * 1e3);
  log_info("Number of channels = %d.", cfg.nf);
  log_info("Sampling time = %e s.", cfg.dt);
  log_info("System temperature = %.2f K.", cfg.tsys);
  log_info("Antenna gain = %.2f Jy / K", cfg.antgain);
  log_info("System gain = %.2f Jy / K.", cfg.sysgain);

  /* If debugging, dump data from ring buffer to file. */
  FILE *dump;
  if (dumpmode.u.b) {
    dump = fopen(debugfile.u.s, "w");
    if (dump == NULL) {
      log_error("Could not open file.");
      exit(1);
    }
  }

  /* Check if we injecting something. */
  if (frbs->count == 0)
    log_warn("No FRBs will be injected since none specified.");

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

    int blknt = BLKSIZE / 4096;
    long blkbeg = (long)currentReadBlock * (long)BLKSIZE;
    long blkend = (long)(currentReadBlock + 1) * (long)BLKSIZE;
    double blktime = blknt * cfg.dt * (double)currentReadBlock;
    log_debug("Reading block no. %d, t = %.2lf s.", currentReadBlock, blktime);

    if (BufRead->curr_blk - currentReadBlock >= MAXBLKS - 1) {
      log_debug("Realigning...");
      recNumRead = (BufRead->curr_rec - 1 + MAXBLKS) % MAXBLKS;
      currentReadBlock = BufRead->curr_blk - 1;
    }

    memcpy(raw, BufRead->data + (long)BLKSIZE * (long)recNumRead, BLKSIZE);

    /*==================================================================*/
    /*======================== FRB INJECTION ===========================*/
    /*==================================================================*/

    if (frbs->count > 0) {
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

        if (nnz == 0) {
          log_warn("Cannot inject since no burst in the file.");
          continue;
        }

        int *rows = (int *)malloc(nnz * sizeof(int));
        int *cols = (int *)malloc(nnz * sizeof(int));
        float *fluxes = (float *)malloc(nnz * sizeof(float));
        for (int i = 0; i < nnz; ++i) fread(&rows[i], sizeof(int), 1, bf);
        for (int i = 0; i < nnz; ++i) fread(&cols[i], sizeof(int), 1, bf);
        for (int i = 0; i < nnz; ++i) fread(&fluxes[i], sizeof(float), 1, bf);

        long seed = set_seed();                /* Set the seed for injection. */
        long offset = (long)(tburst / cfg.dt); /* Burst offset. */
        double sigma =
            cfg.tsys / cfg.sysgain /
            sqrt(2 * cfg.dt * (cfg.df * 1e6)); /* Ideal RMS calculation. */

        /* Begin injection. */
        for (int i = 0; i < nnz; ++i) {
          long I = (offset + (long)rows[i]) * (long)cfg.nf;

          /* Flip the band if it is Band 4 at the GMRT, otherwise do nothing. */
          if (band.u.i == 4)
            I += (cfg.nf - 1 - (long)cols[i]);
          else
            I += (long)cols[i];

          if ((I <= blkbeg) || (I >= blkend)) break;
          I = I % (long)BLKSIZE;
          int in = raw[I];
          int out;

          double lvl = 0.030765;
          double plvl1, plvl2, plvl3;
          double signal = fluxes[i] / sigma;
          double pval = random_deviate(&seed);

         /*======================== 8 bit FRB Injection ===========================*/
          out = cal_bit_shift_prob(in, pval, lvl, signal);
          raw[I] = out;
        }
          free(rows);
          free(cols);
          free(fluxes);
          fclose(bf);
      }
    }
    
    if (dumpmode.u.b) fwrite(raw, 1, BLKSIZE, dump);
    memcpy(BufWrite->data + (long)BLKSIZE * (long)recNumWrite, raw, BLKSIZE);

    recNumRead = (recNumRead + 1) % MAXBLKS;
    currentReadBlock++;

    BufWrite->curr_rec = (recNumWrite + 1) % MAXBLKS;
    BufWrite->curr_blk += 1;
    recNumWrite = (recNumWrite + 1) % MAXBLKS;
  }
  free(raw);                      /* Free the memory allocated for data. */
  if (dumpmode.u.b) fclose(dump); /* Close the file opened for debugging. */

/* Free up memory if and when the argument parsing exits. */
exit:
  arg_freetable(argtable, sizeof(argtable) / sizeof(argtable[0]));
  return exitcode;
}