#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/shm.h>
#include <sys/time.h>
#include <unistd.h>

#include "extern/argtable3.h" // For argument parsing.
#include "extern/log.h"       // For logging.
#include "extern/toml.h"      // For parsing TOML files.

#define WEAVE_VERSION "0.1.0"

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
  unsigned char data[(long)(BLKSIZE) * (long)(MAXBLKS)];
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
 * to intrrupt or terminate the program sent by the user.
 * We choose to terminate the program when this happens.
 */
static volatile sig_atomic_t keep = 1;
static void handler(int _) {
  (void)_;
  keep = 0;
  exit(_);
}

int main(int argc, char *argv[]) {
  /* Attach the handler to SIGINT. */
  signal(SIGINT, handler);

  /* Code to handle argument parsing. */
  struct arg_lit *help;
  struct arg_lit *version;
  struct arg_file *cfgfile;
  struct arg_file *burstfile;
  struct arg_end *end;

  void *argtable[] = {
      help = arg_litn("h", "help", 0, 1, "Display help."),
      version = arg_litn("V", "version", 0, 1, "Display version."),
      cfgfile = arg_file0("c", "config", "<file>", "Configuration file."),
      end = arg_end(20),
  };

  int exitcode = 0;
  char progname[] = "weave";
  int nerrors = arg_parse(argc, argv, argtable);

  if (help->count > 0) {
    printf("Usage: %s", progname);
    arg_print_syntax(stdout, argtable, "\n");
    printf("Weave in fake FRBs into telescope data in real-time.\n\n");
    arg_print_glossary(stdout, argtable, "  %-25s %s\n");
    exitcode = 0;
    goto exit;
  }

  if (version->count > 0) {
    printf("Version: %s\n", WEAVE_VERSION);
    exitcode = 0;
    goto exit;
  }

  if (nerrors > 0) {
    arg_print_errors(stdout, end, progname);
    printf("Try '%s --help' for more information.\n", progname);
    exitcode = 1;
    goto exit;
  }

  if (cfgfile->count == 0) {
    log_error("No configuration file specified.");
    exit(1);
  }

  /* Code to read in the configuration file. */
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
  log_info("Sampling time = %.2f s.", cfg.dt);
  log_info("System temperature = %.2f K.", cfg.tsys);
  log_info("System gain = %.2f Jy.", cfg.gain);

  /* Code to setup reading in the raw data from the ring buffer,
   * and writing it out to another one. Here we check whether the
   * required shared memory exists, and whether we can create the
   * other one.
   */
  unsigned char *raw = (unsigned char *)malloc((long)MAXBLKS * (long)BLKSIZE);

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

  /* The infinite loop that keeps reading in the data and writing it
   * out to another shared memory continuously, injecting the burst(s)
   * at the right time.
   */
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

    log_debug("Block being read = %d", currentReadBlock);
    log_debug("Record being read = %d", recNumRead);
    log_debug("Block being written = %d", BufRead->curr_blk);
    log_debug("Record being written = %d", BufRead->curr_rec);

    if (BufRead->curr_blk - currentReadBlock >= MAXBLKS - 1) {
      log_debug("Realigning...");
      recNumRead = (BufRead->curr_rec - 1 + MAXBLKS) % MAXBLKS;
      currentReadBlock = BufRead->curr_blk - 1;
    }

    memcpy(raw, BufRead->data + ((long)BLKSIZE * (long)recNumRead), BLKSIZE);

    recNumRead = (recNumRead + 1) % MAXBLKS;
    currentReadBlock++;

    /* Code for burst injection. */

    memcpy(BufWrite->data + (long)BLKSIZE * (long)recNumWrite, raw, BLKSIZE);
    BufWrite->curr_rec = (recNumWrite + 1) % MAXBLKS;
    BufWrite->curr_blk += 1;
    recNumWrite = (recNumWrite + 1) % MAXBLKS;
  }
  /* Free the memory we allocated to store the data. */
  free(raw);

/* Free up memory if and when the argument parsing exits. */
exit:
  arg_freetable(argtable, sizeof(argtable) / sizeof(argtable[0]));
  return exitcode;
}
