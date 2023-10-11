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
#define WV_HDRKEY 5031
#define WV_BUFKEY 5032
#define BLKSIZE (32 * 512 * 4096)

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

static volatile sig_atomic_t keep = 1;
static void handler(int _) {
  (void)_;
  keep = 0;
  exit(_);
}

int main() {
  int recNumRead = 0;
  int recNumWrite = 0;
  int currentReadBlock = 0;

  signal(SIGINT, handler);

  FILE *dump = fopen("test.raw", "w");
  if (dump == NULL) {
    printf("Could not open file.\n");
    exit(1);
  }

  unsigned char *raw = (unsigned char *)malloc((long)MAXBLKS * (long)BLKSIZE);

  int idHdrRead = shmget(HDRKEY, sizeof(Header), SHM_RDONLY);
  int idBufRead = shmget(BUFKEY, sizeof(Buffer), SHM_RDONLY);
  if (idHdrRead < 0 || idBufRead < 0) {
    printf("\nShared memory does not exist.\n");
    exit(1);
  }

  Header *HdrRead = (Header *)shmat(idHdrRead, 0, 0);
  Buffer *BufRead = (Buffer *)shmat(idBufRead, 0, 0);
  if ((BufRead) == (Buffer *)-1) {
    printf("\nCould not attach to shared memory.\n");
    exit(1);
  }

  int idHdrWrite = shmget(WV_HDRKEY, sizeof(Header), IPC_CREAT | 0666);
  int idBufWrite = shmget(WV_BUFKEY, sizeof(Buffer), IPC_CREAT | 0666);
  if (idHdrRead < 0 || idBufRead < 0) {
    printf("\nCould not create shared memory.\n");
    exit(1);
  }

  Header *HdrWrite = (Header *)shmat(idHdrWrite, 0, 0);
  Buffer *BufWrite = (Buffer *)shmat(idBufWrite, 0, 0);
  if ((BufWrite) == (Buffer *)-1) {
    printf("\nCould not attach to shared memory.\n");
    exit(1);
  }

  BufWrite->curr_rec = 0;
  BufWrite->curr_blk = 0;
  recNumWrite = (BufWrite->curr_rec) % MAXBLKS;

  HdrWrite->active = 1;

  while (keep) {
    int flag = 0;
    while (currentReadBlock == BufRead->curr_blk) {
      usleep(2000);
      if (flag == 0) {
        printf("\nWaiting...\n");
        flag = 1;
      }
    }
    if (flag == 1)
      printf("\nReady!\n");

    printf("Block being read = %d\n", currentReadBlock);
    printf("Record being read = %d\n", recNumRead);
    printf("Block being written = %d\n", BufRead->curr_blk);
    printf("Record being written = %d\n", BufRead->curr_rec);

    if (BufRead->curr_blk - currentReadBlock >= MAXBLKS - 1) {
      printf("\nRealigning...\n");
      recNumRead = (BufRead->curr_rec - 1 + MAXBLKS) % MAXBLKS;
      currentReadBlock = BufRead->curr_blk - 1;
    }

    memcpy(raw, BufRead->data + ((long)BLKSIZE * (long)recNumRead), BLKSIZE);

    recNumRead = (recNumRead + 1) % MAXBLKS;
    currentReadBlock++;

    memcpy(BufWrite->data + (long)BLKSIZE * (long)recNumWrite, raw, BLKSIZE);
    BufWrite->curr_rec = (recNumWrite + 1) % MAXBLKS;
    BufWrite->curr_blk += 1;
    recNumWrite = (recNumWrite + 1) % MAXBLKS;

    fwrite(raw, 1, BLKSIZE, dump);
  }
  free(raw);
  return 0;
}
