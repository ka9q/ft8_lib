// unknown origin; hacked by Phil Karn, KA9Q Oct 2023
// Written by KA9Q May/June 2025 to process a hierarchy of spool directories
// decode_ft8 [-v] [-4] [-f megahertz] file_or_directory
// If given a file, decodes just that file
// If given a directory, scans and processes every file in that directory
// Uses inotify() on linux, otherwise just polls
// INPUT FILES ARE DELETED AFTER SUCCESSFUL DECODING!

#define _GNU_SOURCE 1
#include <stdint.h>
#include <stdlib.h>
#include <libgen.h>
#include <signal.h>
#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include <locale.h>
#include <errno.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dirent.h>
#include <poll.h>
#include <assert.h>
#include <getopt.h>
#include <sys/xattr.h>
#include <limits.h>
#include <sys/file.h>
#ifdef __linux__
#include <sys/inotify.h>
#endif

#include "common/wave.h"
#include "common/debug.h"

#define LOG_LEVEL LOG_FATAL

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__) || defined(__DragonFly__)
  // The great thing about standards...
  #define st_mtim st_mtimespec
  #define st_atim st_atimespec
  #define st_ctim st_ctimespec
#endif


int Verbose = 0;
bool NoDelete; // Don't delete input file after decoding
bool Run_queue = false; // When true, exit after running queue (suitable for calling from cron)
#define SORT_SIZE (8192) // Max size of file name sort list

#define HSIZE 127
struct wd_hashtab {
  int wd; // inotify watch descriptor
  char *path; // Path name to this directory
} Wd_hashtab[HSIZE];

static int has_suffix(const char *filename, const char *suffix);
int process_file(char const *path,bool is_ft8,double base_freq); // Either file or directory (calls recursively)
void process_directory(char const *path, bool is_ft8, double base_freq); // Directory only; called recursively
int add_watches_recursive(int fd, const char *path);
int scompare(void const *a, void const *b);
void usage();

int main(int argc, char *argv[]){
  const char* path = NULL;
  bool is_ft8 = true;

  // Base freq; if not specified with -f in megahertz, extracted from input filename of form
  // yyyymmddThhmmssZ_ffffffff_usb.wav
  // where yyyymmdd is year, month, day
  // hhmmss is hour, minute, second UTC
  // ffffffffff is frequency in *hertz*
  double base_freq = 0;
  int c;
  while((c = getopt(argc,argv,"48f:vnr")) != -1){
    switch(c){
    case 'r':
      Run_queue = true;
      break;
    case 'n':
      NoDelete = true;
      break;
    case 'v':
      Verbose++;
      break;
    case '8':
      is_ft8 = true; // In case it's not the default
      break;
    case '4': // Decode FT4; default is FT8
      is_ft8 = false;
      break;
    case 'f': // Base frequency in Megahertz, otherwise extracted from file name
      base_freq = strtod(optarg,NULL);
      break;
    }
  }
  {
    char *loc = getenv("LANG");
    setlocale(LC_ALL,loc); // To get commas in long numerical strings
  }
  if(argc <= optind){
    usage();
    exit(1);
  }
  path = argv[optind];
  {
    struct stat statbuf;
    if(lstat(path,&statbuf) == -1){
      fprintf(stderr,"Can't stat %s: %s\n",path,strerror(errno));
      exit(1);
    }
    if((statbuf.st_mode & S_IFMT) == S_IFREG){
      // Regular file specified; process just it and quit
      int r = process_file(path, is_ft8, base_freq);
      exit(r);
    }
    if((statbuf.st_mode & S_IFMT) != S_IFDIR){
      // Must be a directory at this point
      fprintf(stderr,"Only regular files and directories supported\n");
      exit(1);
    }
  }
#ifdef __linux__ // inotify is linux-only; non-linux will run simple timer-based directory scan below
  extern int Watches;

  int fd = inotify_init();
  if(fd == -1){
    perror("inotify_init");
    exit(1);
  }
  {
    // Set inotify fd to nonblocking so we can use poll() on it
    int flags = fcntl(fd,F_GETFL, 0);
    if(flags == -1)
      perror("get inotify fd flags");
    int r = fcntl(fd,F_SETFL,flags | O_NONBLOCK);
    if(r == -1)
      perror("set inotify fd nonblock");
  }
  // Only examine the file when it's closed for write or moved into place
  // Watch for deletion of current directory (causes us to exit)
  // Be careful with IN_CREATE; we don't want to read ordinary files until they're closed,
  // but we do want to wach for newly created directorie
  add_watches_recursive(fd, path);

  struct timespec last_poll = {0};
  int poll_interval = 31; // Initial value doesn't really matter

  while(true){
    // Re-scan the directory every 0-31 seconds
    // Will happen on the first loop since last_poll{0} is now in the distant past
    struct timespec now;
    clock_gettime(CLOCK_REALTIME,&now);
    if(now.tv_sec >= last_poll.tv_sec + poll_interval){
      poll_interval = 1 + (random() & 31); // 1-32 seconds inclusive
      process_directory(path, is_ft8, base_freq);
      last_poll = now;
      if(Run_queue)
	exit(0);
    }
    // Don't block on the inotify read indefinitely
    struct pollfd pd = {
      .fd = fd,
      .events = POLLIN,
    };
    int timeout = 1000; // wait max 1 sec

    int r = poll(&pd,1,timeout);
    if(r < 0) {
      fprintf(stderr,"Poll error: %s\n",strerror(errno));
      break;
    }
    if(!(pd.events & POLLIN))
      continue;

    char buffer[8192];
    int len;
    struct inotify_event *event = NULL;
    while((len = read(fd,buffer,sizeof(buffer))) > 0){
      char *file_list[SORT_SIZE] = {0}; // List of files to sort and process from each event
      int filecount = 0;
      int event_num = 0;
      for(int i = 0; i < len; i += sizeof(struct inotify_event) + event->len,event_num++){
	event = (struct inotify_event *)&buffer[i];
	// Find the directory it happened in
	int hp = event->wd % HSIZE;
	int i;
	for(i=0; i < HSIZE; i++){
	  if(Wd_hashtab[hp].wd == event->wd)
	    break; // found
	  hp = (hp + 1) % HSIZE; // keep scanning
	}
	// Entry is present if we found 'wd' AND the path name is non-null
	// We could find 'wd' with a null path if it had been created and deleted
	char const * const dirname = (i != HSIZE) ? Wd_hashtab[hp].path : NULL;

	if(Verbose > 1){
	  fprintf(stderr,"wd %d, event %d, directory %s, name %s, length %d, event 0x%x ",event->wd,
		  event_num,dirname, event->name, event->len,event->mask);
	  void print_inotify_mask(uint32_t mask);
	  print_inotify_mask(event->mask);
	  fprintf(stderr,"\n");
	}
	if(dirname == NULL){
	  fprintf(stderr,"unknown directory for watch event %d mask %x\n",event->wd,event->mask);
	  continue;
	}
	if(strcmp(event->name,".") == 0 || strcmp(event->name, "..") == 0)
	  continue; // igore anything regarding the current directory or its parent

	if(event->mask & IN_IGNORED){ // both flags set when directory is deleted
	  // Kernel dropped a watch, a directory was removed
	  // Would like to remove Wd_hashtable[h] entry but cannot
	  if(Verbose)
	    fprintf(stderr,"Dropping watch %d on %s\n",event->wd,dirname);
	  // Remove from hash table BUT leave wd unchanged as a "tombstone" for the deleted entry
	  // so searches for other nearby values won't erroneously fail
	  free(Wd_hashtab[hp].path);
	  Wd_hashtab[hp].path = NULL; // Leave .wd in case the entry is reused
	  Watches--;
	  if(Watches == 0){
	    fprintf(stderr,"No directory watches left, exiting\n");
	    exit(0);
	  }
	  continue;
	}
	// Construct full pathname and process
	char *fullname = NULL;
	int r = asprintf(&fullname,"%s/%s",dirname, event->name); // mallocs memory for fullname
	if(r <= 0){
	  fprintf(stderr,"asprintf %s/%s failed\n",dirname, event->name);
	  free(fullname);
	  continue;
	}
	// Use lstat so we'll ignore symbolic links
	struct stat statbuf = {0};
	if(lstat(fullname,&statbuf) != 0){
	  free(fullname);
	  continue;
	}
	switch(statbuf.st_mode & S_IFMT){
	case S_IFREG:
	  if((event->mask & (IN_CLOSE_WRITE | IN_MOVED_TO)) && has_suffix(fullname,".wav")){
	    // The usual sequence is for pcmrecord to write a .wav.tmp file and atomically rename it
	    // This causes a IN_CLOSE_WRITE event on the tmp followed by IN_MOVED_TO the wav filename
	    // We also monitor IN_CLOSE_WRITE in case a .wav file is written directly, without renaming
	    // Add to list for sorting
	    if(filecount < SORT_SIZE)
	      file_list[filecount++] = strdup(fullname); // mallocs memory, freed after sort and process
	    else
	      process_file(fullname, is_ft8, base_freq); // sort table is full (unlikely) so just process it
	  }
	  break;
	case S_IFDIR:
	  if((event->mask & IN_CREATE) && (event->mask & IN_ISDIR)){
	    // New directory appeared; add it to the list
	    add_watches_recursive(fd, fullname);
	  }
	  break;
	default: // Ignore symbolic links and everything else
	  break;
	}
	free(fullname);
      }
      // Sort and process list of files in each event
      // Doesn't really work well because the events are often split across several reads
      // and I'd rather not delay artificially
      qsort(file_list,filecount,sizeof file_list[0],scompare);
      for(int i=0; i < filecount; i++){
	process_file(file_list[i], is_ft8, base_freq);
	free(file_list[i]);
      }
    }
    if(len < 0 && errno != EAGAIN){
      fprintf(stderr,"inotify read returns error: %s\n",strerror(errno));
      break;
    }
    // Otherwise go back and poll
  }
  close(fd);
#else
  // Simple timed directory scan without inotify
  while(true){
    // Re-scan the directory every 1-8 seconds
    // Will happen on the first loop since last_poll is in the distant past
    process_directory(path, is_ft8, base_freq);
    if(Run_queue)
      break;
    sleep(1 + (random() & 7)); // Random sleep between 1 and 8 sec; prevent synchronizing of multiple workers
  }
#endif
  exit(0);
}
#ifdef __linux__
int scompare(void const *a, void const *b){
  char const *ap = *(char const **)a;
  char const *bp = *(char const **)b;
  // Shorter strings (lower frequencies) always compare less
  if(strlen(ap) < strlen(bp))
    return -1;
  if(strlen(ap) > strlen(bp))
    return +1;
  return strcmp(ap,bp);
}

// Add a directory to the watch list
int Watches = 0;
int add_watches_recursive(int const fd, const char *path) {
  if(fd < 0)
    return 0;
  DIR *dir = opendir(path);
  if (dir == NULL)
    return 0; // Fails if it's not a directory (eg. an ordinary file)

  int const wd = inotify_add_watch(fd, path, IN_CLOSE_WRITE | IN_MOVED_TO | IN_DELETE_SELF | IN_CREATE | IN_ISDIR);
  if(wd == -1){
    fprintf(stderr,"inotify_add_watch(%s) failed: %s\n",path,strerror(errno));
    return 0;
  }
  if(Verbose)
    fprintf(stderr,"wd %d watching directory %s\n",wd, path);

  Watches++;
  // Add to hash table
  int hp = wd % HSIZE;
  int i;
  for(i=0; i < HSIZE; i++){
    if(Wd_hashtab[hp].wd == wd || Wd_hashtab[hp].wd == 0) // Will be wd if previously used
      break;
    hp = (hp + 1) % HSIZE; // keep scanning
  }
  if(i == HSIZE){
    fprintf(stderr,"add_watches_recursive(%s) failed: Hash table full\n",path);
    return -1;
  }
  free(Wd_hashtab[hp].path); // Just in case it's already present, but how can this happen?
  Wd_hashtab[hp].path = strdup(path);
  Wd_hashtab[hp].wd = wd;

  // Now look recursively for subdirectories
  struct dirent *ent = NULL;
  while ((ent = readdir(dir)) != NULL) {
    if (ent->d_type == DT_DIR &&
	strcmp(ent->d_name, ".") != 0 &&
	strcmp(ent->d_name, "..") != 0) {
      char *subpath = NULL;
      int r = asprintf(&subpath, "%s/%s", path, ent->d_name);
      if(r <= 0){
	free(subpath);
	fprintf(stderr,"add_watches_recursive(%s) failed when adding %s, no memory\n",path,ent->d_name);
	abort(); // out of memory should be very rare!
      }
      add_watches_recursive(fd, subpath);
    }
  }
  closedir(dir);
  return 0;
}
void print_inotify_mask(uint32_t mask) {
    if (mask & IN_ACCESS)        fprintf(stderr,"IN_ACCESS ");
    if (mask & IN_ATTRIB)        fprintf(stderr,"IN_ATTRIB ");
    if (mask & IN_CLOSE_WRITE)   fprintf(stderr,"IN_CLOSE_WRITE ");
    if (mask & IN_CLOSE_NOWRITE) fprintf(stderr,"IN_CLOSE_NOWRITE ");
    if (mask & IN_CREATE)        fprintf(stderr,"IN_CREATE ");
    if (mask & IN_DELETE)        fprintf(stderr,"IN_DELETE ");
    if (mask & IN_DELETE_SELF)   fprintf(stderr,"IN_DELETE_SELF ");
    if (mask & IN_MODIFY)        fprintf(stderr,"IN_MODIFY ");
    if (mask & IN_MOVE_SELF)     fprintf(stderr,"IN_MOVE_SELF ");
    if (mask & IN_MOVED_FROM)    fprintf(stderr,"IN_MOVED_FROM ");
    if (mask & IN_MOVED_TO)      fprintf(stderr,"IN_MOVED_TO ");
    if (mask & IN_OPEN)          fprintf(stderr,"IN_OPEN ");

    if (mask & IN_ISDIR)         fprintf(stderr,"IN_ISDIR ");

    if (mask & IN_UNMOUNT)       fprintf(stderr,"IN_UNMOUNT ");
    if (mask & IN_Q_OVERFLOW)    fprintf(stderr,"IN_Q_OVERFLOW ");
    if (mask & IN_IGNORED)       fprintf(stderr,"IN_IGNORED ");

    // Special flags (not really events)
    if (mask & IN_ONLYDIR)       fprintf(stderr,"IN_ONLYDIR ");
    if (mask & IN_DONT_FOLLOW)   fprintf(stderr,"IN_DONT_FOLLOW ");
    if (mask & IN_EXCL_UNLINK)   fprintf(stderr,"IN_EXCL_UNLINK ");
    if (mask & IN_MASK_ADD)      fprintf(stderr,"IN_MASK_ADD ");
    if (mask & IN_ONESHOT)       fprintf(stderr,"IN_ONESHOT ");
}
#endif


// Process a single audio file, delete if successful
// Return -1 on decoding error, 0 on success, 1 if the file couldn't be found or locked
int process_file(char const * const path, bool is_ft8, double base_freq){
  if(path == NULL || strlen(path) == 0)
    return -1;

  // Try to lock it
  char lockfile[PATH_MAX+5]; // If too long, open will fail with ENAMETOOLONG
  int lock_fd = -1;
  snprintf(lockfile,sizeof lockfile,"%s.lock",path);
  int tries = 5;
  for(; tries >= 0; tries--){
    lock_fd = open(lockfile,O_WRONLY|O_EXCL|O_CREAT,0644);
    if(lock_fd != -1)
      break; // Successful lock creation

    // Try to read an existing lock file
    lock_fd = open(lockfile,O_RDONLY);
    if(lock_fd == -1){
      fprintf(stderr,"Attempt to open existing lockfile %s failed: %s\n",lockfile,strerror(errno));
      return 1;
    }
    int pid;
    int rcount = read(lock_fd,&pid,sizeof pid);
    if(rcount != sizeof pid){
      // rcount == 0 might happen if another task has created the lockfile but not yet written to it
      // however, if the empty lock file is stale, we'll continue to treat the file as locked
      // no truly race-free way to handle this, except by looking at its age
      if(rcount < 0)
	fprintf(stderr,"Attempt to read existing lockfile %s failed: %s\n",lockfile,strerror(errno));

      close(lock_fd);
      return 1;
    }
    close(lock_fd);
    lock_fd = -1;
    // Send it a no-op kill -0 to see if it still exists
    // If it does, kill will return 0 if we own it, or -1 and errno == EPERM if it exists and owned by someone else
    if(kill(pid,0) == 0 || errno == EPERM)
      return 1;

    if(errno != ESRCH){ // Only ESRCH indicates the process doesn't exist
      fprintf(stderr,"error sending 'kill -0' to process %d: %s\n",pid,strerror(errno));
      return 1;
    }
    // Locking process no longer exists, remove the lock and try again
    // If two processes race to do this, the unlink will succeed only for one
    if(unlink(lockfile) != 0){
      fprintf(stderr,"Attempt to unlink stale lockfile %s failed: %s\n",lockfile,strerror(errno));
      return 1;
    } // else try again to create lock
    sleep(1);
  }
  if(tries < 0){
    fprintf(stderr,"Can't lock %s\n",path);
    return 1;
  }
  int const pid = getpid();
  if(write(lock_fd,&pid,sizeof pid) != sizeof pid){
    fprintf(stderr,"Can't write pid %d to lock file %s: %s\n",pid,lockfile,strerror(errno));
    close(lock_fd);
    unlink(lockfile);
    return 1;
  }
  close(lock_fd);

  // Lock successfully created, we now look at the file
  {
    struct stat statbuf;
    if(lstat(path,&statbuf) == -1){
      unlink(lockfile);
      return -1;
    }
    if((statbuf.st_mode & S_IFMT) != S_IFREG){
      unlink(lockfile);
      return -1;
    }
  }
  // Try to open it
  int const fd = open(path,O_RDONLY);
  if(fd == -1){
    unlink(lockfile);
    return 1; // Somebody else aleady got to it (unlikely with locking?)
  }
  if(flock(fd,LOCK_EX|LOCK_NB) == -1){
    // Could happen if file is still being written
    close(fd);
    unlink(lockfile);
    return 1;
  }

  int sample_rate = 0; // These get overwritten by load_wav
  int num_samples = 0;
  float *signal = NULL; // now allocated by load_wav, must free if we ever loop

  // load_wav now allocates signal, we must free (unless we exit right away, as we currently do)
  assert(path != NULL);
  int const rc = load_wav(&signal, &num_samples, &sample_rate, path, fd);
  flock(fd,LOCK_UN);
  close(fd); // remove the lock file later, after possible file removal
  if(Verbose)
    fprintf(stderr,"decode %s: %d samples, sample rate %d Hz\n", path, num_samples, sample_rate);

  if (rc < 0 || num_samples < (is_ft8 ? 12.64 : 4.48 ) * sample_rate){
    struct stat statbuf = {0};
    if(lstat(path,&statbuf) == 0){
      struct timespec ts = {0};
      timespec_get(&ts,TIME_UTC);
      int const age = ts.tv_sec - statbuf.st_mtime;
      if(age > 60){
	// ignore very young files in case they're still being written
	// Could actually do this after 15 sec or so, but the directory scan will get it eventually
	if(NoDelete){
	  fprintf(stderr,"%s: short/bad file, %'lld bytes, %'d seconds old\n",
		  path,
		  (long long)statbuf.st_size,
		  age);
	} else {
	  fprintf(stderr,"%s: short/bad file, %'lld bytes, %'d seconds old, deleting\n",
		  path,
		  (long long)statbuf.st_size,
		  age);
	  int r = unlink(path);
	  if(r != 0)
	    fprintf(stderr,"can't delete %s: %s\n",path,strerror(errno));
	}
      }
    }
    {
      int const r = unlink(lockfile);
      if(r != 0){
	fprintf(stderr,"can't delete %s: %s\n",lockfile,strerror(errno));
      }
    }
    return -1;
  }
  if(base_freq == 0){
    assert(path != NULL);
    // Look first for extended file attribute "user.frequency" (linux) or "frequency" (macos)
    char att_buffer[1024] = {0}; // Shouldn't be anywhere near this long
#ifdef __linux__
    ssize_t s = getxattr(path,"user.frequency",att_buffer,sizeof(att_buffer) - 1);
#else
    ssize_t s = getxattr(path,"frequency",att_buffer,sizeof(att_buffer) - 1,0,0);
#endif
    if(s > 0){
      // Extract from attribute
      base_freq = strtod(att_buffer,NULL);
      base_freq /= 1e6; // Hz -> MHz
      if(Verbose > 1)
	fprintf(stderr,"Extracted base frequency %lf MHz from attribute\n",base_freq);
    } else {
      // Extract from file name
      char *cp,*cp1;
      // Should use basename in case directory element has _
      if((cp = strchr(path,'_')) != NULL && (cp1 = strrchr(path,'_')) != NULL){
	base_freq = strtod(cp+1,NULL) / 1e6;
	if(Verbose > 1)
	  fprintf(stderr,"Extracted base frequency %lf MHz from file name\n",base_freq);
      }
    }
  }
  if(base_freq == 0)
    fprintf(stderr,"Unknown base frequency for %s\n",path);

  struct tm tmp = {0};
  double fsec = 0; // Fractional second
  bool tmp_set = false;
  {
    // Look first for extended file attribute "user.unixstarttime" or "unixstarttime"
    char att_buffer[1024] = {0};
#ifdef __linux__
    ssize_t s = getxattr(path,"user.unixstarttime",att_buffer,sizeof(att_buffer) - 1);
#else
    ssize_t s = getxattr(path,"unixstarttime",att_buffer,sizeof(att_buffer) - 1,0,0);
#endif
    if(s > 0){
      // Extract from attribute
      double t = strtod(att_buffer,NULL);
      time_t tt = t;
      fsec = fmod(t,1.0);
      if(gmtime_r(&tt,&tmp) != NULL){
	tmp_set = true;
	if(Verbose > 1)
	  fprintf(stderr,"Time extracted from attribute\n");
      }
    }
  }
  if(!tmp_set){
    // that didn't work, try extracting date-time from file name
      char *npath = strdup(path);
      char const *bn = basename(npath);
      int year,mon,day,hr,minute,sec;
      char junk;
      int r = sscanf(bn,"%04d%02d%02d%c%02d%02d%02d",&year,&mon,&day,&junk,&hr,&minute,&sec);
      free(npath);
      if(r == 7){
	// Convert to Unix-style struct tm (using its conventions)
	tmp.tm_year = year - 1900;
	tmp.tm_mon = mon - 1;
	tmp.tm_mday = day;
	tmp.tm_hour = hr;
	tmp.tm_min = minute;
	tmp.tm_sec = sec;
	// fsec remains at zero
	tmp_set = true;
	if(Verbose > 1)
	  fprintf(stderr,"Time extracted from filename\n");
      }
  }
  if(!tmp_set){
    // That didn't work either, so subtract 7.5 or 15 sec from the modification time
    // not really tested, but seems simple enough
    struct stat statbuf = {0};
    if(lstat(path,&statbuf) == 0){
      struct timespec ts = {0};
      ts.tv_sec = statbuf.st_mtim.tv_sec;
      ts.tv_nsec = statbuf.st_mtim.tv_nsec;
      if(is_ft8){
	// 15 sec for FT8
	ts.tv_sec -= 15;
      } else {
	// 7.5 sec for FT4
	ts.tv_sec -= 7;
	ts.tv_nsec -= 500000000; // 1/2 sec
	if(ts.tv_nsec < 0){
	  ts.tv_nsec += 1000000000; // 1 sec
	  ts.tv_sec--;
	}
      }
      time_t tt = ts.tv_sec;
      fsec = ts.tv_nsec * 1.0e-9; // nanosec to fractional second
      if(gmtime_r(&tt,&tmp) != NULL){
	tmp_set = true;
	fprintf(stderr,"Time inferred from file mod time\n");
      }
    }
  }

  if(!tmp_set)
    fprintf(stderr,"%s: recording time unknown\n",path);

  // Do the actual decoding.
  // Should pass sub-second timing to help the decoder produce a more accurate timing offset
  process_buffer(signal, sample_rate, num_samples, is_ft8, base_freq, &tmp,fsec);
  free(signal); // allocated by load_wav
  signal = NULL;
  fflush(stdout);

  // Done with the file (could have been deleted earlier, but just in case we crash)
  if(!NoDelete){
    int const r = unlink(path); // Done with it; still need the name later
    if(r != 0)
      fprintf(stderr,"can't unlink %s: %s\n",path,strerror(errno));
  }
  {
    int const r = unlink(lockfile); // And the lock file (delete after the file it locks)
    if(r != 0)
      fprintf(stderr,"can't unlink %s: %s\n",lockfile,strerror(errno));
  }
  return 0;
}
// Returns 1 if filename ends with suffix (e.g., ".job"), else 0
static int has_suffix(const char *filename, const char *suffix) {
  if(filename == NULL || suffix == NULL)
    return 0;
  size_t len_filename = strlen(filename);
  size_t len_suffix = strlen(suffix);

  if (len_filename < len_suffix)
    return 0; // too short to match

  return strcmp(filename + len_filename - len_suffix, suffix) == 0;
}
// strcmp wrapper for qsort()
int cmp_dirent(const void *a, const void *b) {
  const struct dirent *sa = (const struct dirent *)a;
  const struct dirent *sb = (const struct dirent *)b;
  return strcmp(sa->d_name, sb->d_name);
}
// Recursively scan a directory and process the files inside
// If there are "too many" entries in a directory, ignore them and
// we'll get them when we rescan the same directory on a timer
void process_directory(char const *path, bool is_ft8, double base_freq){
  if(path == NULL)
    path = "."; // Default to current directory

  int cwd_fd = -1;
  int dir_fd = -1;
  DIR *dirp = NULL;

  if(Verbose > 1)
    fprintf(stderr,"processing directory %s\n",path);

  cwd_fd = open(".", O_RDONLY|O_DIRECTORY);
  if(cwd_fd == -1)
    fprintf(stderr,"Can't read current directory: %s\n",strerror(errno));

  // chdir into the specified directory and work from there as
  // dirent entries will be interpreted relative to the current directory
  // But we must pop back before returning
  dir_fd = open(path,O_RDONLY|O_DIRECTORY);
  if(dir_fd == -1){
    fprintf(stderr,"Can't open directory %s: %s\n",path,strerror(errno));
    goto done;
  }
  if(fchdir(dir_fd) != 0){
    fprintf(stderr,"Can't change into directory %s: %s\n",path,strerror(errno));
    goto done;
  }
  dirp = fdopendir(dir_fd);
  if(dirp == NULL){
    fprintf(stderr,"Can't scan directory %s: %s\n",path,strerror(errno));
    goto done;
  }
  // Sort entries from oldest to newest
  // If there are more than SORT_SIZE files, we'll get them next time
  char *file_list[SORT_SIZE] = {0};
  int filecount = 0;
  struct dirent *d = NULL;
  while((d = readdir(dirp)) != NULL){
    if(strlen(d->d_name) == 0)
      continue; // can this happen?
    // ignore directories "." and ".." or we'd recurse forever
    switch(d->d_type){
    case DT_DIR:
      if(strcmp(d->d_name,".") != 0 && strcmp(d->d_name,"..") != 0)
	process_directory(d->d_name, is_ft8, base_freq); // Recursive call
      break;
    case DT_REG:
      if(has_suffix(d->d_name,".wav") && filecount < SORT_SIZE)
	file_list[filecount++] = strdup(d->d_name); // d_name changes when readdir is called again
      break;
    default:
      break;
    }
  }
  closedir(dirp);
  dir_fd = -1; // closedir does this
  dirp = NULL;
  qsort(file_list,filecount,sizeof file_list[0],scompare);
  for(int i=0; i < filecount; i++){
    process_file(file_list[i], is_ft8, base_freq);
    free(file_list[i]);
  }
  // Return to our originally scheduled program
done:;
  if(cwd_fd != -1){
    if(fchdir(cwd_fd) == -1) // Change back if necessary
      fprintf(stderr,"Can't change into current directory: %s\n",strerror(errno));
    close(cwd_fd);
  }
  if(dirp != NULL){
    // Probably already closed
    closedir(dirp); // also closes dir_fd
    dir_fd = -1;
  }
  if(dir_fd != -1)
    close(dir_fd); // otherwise close it separately
}


void usage()
{
  fprintf(stderr, "decode_ft8 [-v] [-8|-4] [-d] [-f basefreq] file_or_directory\n");
}
