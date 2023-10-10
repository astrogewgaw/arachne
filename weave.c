#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/shm.h>
#include <sys/time.h>
#include <unistd.h>

#define MAXBLKS 16
#define HDRKEY 2031
#define BUFKEY 2032
#define BLKSIZE (32 * 512 * 4096)

typedef struct {
  unsigned int flag;
  unsigned int curr_blk, curr_rec, blk_size;
  int overflow;
  double comptime[MAXBLKS];
  double datatime[MAXBLKS];
  unsigned char data[(long)(BLKSIZE) * (long)(MAXBLKS)];
} Buffer;

typedef struct {
  unsigned int active, status;
  double comptime, datatime, reftime;
  struct timeval timestamp[MAXBLKS];
  struct timeval timestamp_gps[MAXBLKS];
  double blk_nano[MAXBLKS];
} Header;

static volatile sig_atomic_t keep = 1;
static void handler(int _) {
  (void)_;
  keep = 0;
  exit(_);
}

int main() {
  int idhdr;
  int idbuf;
  int numrec = 0;
  int numblk = 0;
  Header *HdrRead;
  Buffer *BufRead;

  signal(SIGINT, handler);

  FILE *dump = fopen("test.raw", "w");
  if (dump == NULL) {
    printf("Could not open file.\n");
    exit(1);
  }

  unsigned char *raw = (unsigned char *)malloc((long)MAXBLKS * (long)BLKSIZE);

  idhdr = shmget(HDRKEY, sizeof(Header), SHM_RDONLY);
  idbuf = shmget(BUFKEY, sizeof(Buffer), SHM_RDONLY);
  if (idhdr < 0 || idbuf < 0) {
    printf("\nShared memory does not exist.\n");
    exit(1);
  }

  HdrRead = (Header *)shmat(idhdr, 0, SHM_RDONLY);
  BufRead = (Buffer *)shmat(idbuf, 0, SHM_RDONLY);
  if ((BufRead) == (Buffer *)-1) {
    printf("\nCould not attach to shared memory.\n");
    exit(1);
  }

  while (keep) {
    int flag = 0;
    while (numblk == BufRead->curr_blk) {
      usleep(2000);
      if (flag == 0) {
        printf("\nWaiting...\n");
        flag = 1;
      }
    }
    if (flag == 1)
      printf("\nReady!\n");

    printf("Block being read = %d\n", numblk);
    printf("Record being read = %d\n", numrec);
    printf("Block being written = %d\n", BufRead->curr_blk);
    printf("Record being written = %d\n", BufRead->curr_rec);

    if (BufRead->curr_blk - numblk >= MAXBLKS - 1) {
      printf("\nRealigning...\n");
      numrec = (BufRead->curr_rec - 1 + MAXBLKS) % MAXBLKS;
      numblk = BufRead->curr_blk - 1;
    }

    memcpy(raw, BufRead->data + ((long)BLKSIZE * (long)numrec), BLKSIZE);

    numrec = (numrec + 1) % MAXBLKS;
    numblk++;

    fwrite(raw, 1, BLKSIZE, dump);
  }
  free(raw);
  return 0;
}
