/*
 * Copyright (c) 1999-2003  Albert Dorofeev <albert@tigr.net>
 * For the updates see http://www.tigr.net/
 *
 * This software is distributed under GPL. For details see LICENSE file.
 */

/* kvm/uvm use (BSD port) code:
 * Copyright (c) 2000  Scott Aaron Bamford <sab@zeekuschris.com>
 * BSD additions for for this code are licensed BSD style.
 * All other code and the project as a whole is under the GPL.
 * For details see LICENSE.
 * BSD systems dont have /proc/meminfo. it is still posible to get the disired
 * information from the uvm/kvm functions. Linux machines shouldn't have
 * <uvm/vum_extern.h> so should use the /proc/meminfo way. BSD machines (NetBSD
 * i use, but maybe others?) dont have /proc/meminfo so we instead get our info
 * using kvm/uvm.
 */

/*
 * The FreeBSD port is
 * Copyright (c) 2000 Andre Yelistratov <andre@express.ru>
 */

#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>

#include "state.h"

#include "config.h"

#ifdef HAVE_UVM_UVM_EXTERN_H
/* sab - 2000/01/21
 * this should only happen on *BSD and will use the BSD kvm/uvm interface
 * instead of /proc/meminfo
 */
#include <sys/types.h>
#include <sys/param.h>
#include <sys/sysctl.h>
  
#include <uvm/uvm_extern.h>
#endif /* HAVE_UVM_UVM_EXTERN_H */

extern struct asmem_state state;

#ifndef HAVE_UVM_UVM_EXTERN_H
#define BUFFER_LENGTH 400
int fd;
FILE *fp;
char buf[BUFFER_LENGTH];
#endif /* !HAVE_UVM_UVM_EXTERN */

// Machine dependent headers
// FreeBSD
#if defined(__FreeBSD__)
#include <sys/conf.h>
#include <osreldate.h>
#include <kvm.h>
#include <sys/vmmeter.h>

//4.0
#if __FreeBSD_version > 400000
#include <time.h>
#else
//2.0-3.4
#include <sys/rlist.h>
#endif /* __FreeBSD_version > 400000 */
#endif /* if defined(__FreeBSD__) */


// Machine dependent declarations
// FreeBSD
#if defined(__FreeBSD__)
#define pagetok(size) ((size) << pageshift)
#define SWAP_DEVICES 3  /* Seems to be enough */
struct vmmeter sum;
kvm_t   *kd;

static struct nlist nlst[] = {
#define X_CCPU		0
    { "_ccpu" },
#define X_CP_TIME	1
    { "_cp_time" },
#define X_AVENRUN	2
    { "_averunnable" },

/* Swap */
#define VM_SWAPLIST	3
	{ "_swaplist" },/* list of free swap areas */
#define VM_SWDEVT	4
	{ "_swdevt" },	/* list of swap devices and sizes */
#define VM_NSWAP	5
	{ "_nswap" },	/* size of largest swap device */
#define VM_NSWDEV	6
	{ "_nswdev" },	/* number of swap devices */
#define VM_DMMAX	7
	{ "_dmmax" },	/* maximum size of a swap block */
#define X_BUFSPACE	8
	{ "_bufspace" },	/* K in buffer cache */
#define X_CNT           9
    { "_cnt" },		        /* struct vmmeter cnt */

/* Last pid */
#define X_LASTPID	10
    { "_nextpid" },		
    { 0 }
};
unsigned long cnt_offset;

//4.0
#if __FreeBSD_version > 400000
#else
//2.0-3.4

#define	SVAR(var) __STRING(var)	/* to force expansion */
#define	KGET(idx, var)							\
	KGET1(idx, &var, sizeof(var), SVAR(var))
#define	KGET1(idx, p, s, msg)						\
	KGET2(nlst[idx].n_value, p, s, msg)
#define	KGET2(addr, p, s, msg)						\
	if (kvm_read(kd, (u_long)(addr), p, s) != s) {		        \
		warnx("cannot read %s: %s", msg, kvm_geterr(kd));       \
		return (0);                                             \
       }
#define	KGETRET(addr, p, s, msg)					\
	if (kvm_read(kd, (u_long)(addr), p, s) != s) {			\
		warnx("cannot read %s: %s", msg, kvm_geterr(kd));	\
		return (0);						\
	}
int
swapmode(unsigned long *retavail,unsigned long *retfree)
{
	char *header;
	int hlen, nswap, nswdev, dmmax;
	int i, div, avail, nfree, npfree, used;
	struct swdevt *sw;
	long blocksize, *perdev;
	u_long ptr;
	struct rlist head;
#if __FreeBSD_version >= 220000
	struct rlisthdr swaplist;
#else 
	struct rlist *swaplist;
#endif
	struct rlist *swapptr;

	/*
	 * Counter for error messages. If we reach the limit,
	 * stop reading information from swap devices and
	 * return zero. This prevent endless 'bad address'
	 * messages.
	 */
	static warning = 10;

	if (warning <= 0) {
	    /* a single warning */
	    if (!warning) {
		warning--;
		printf( 
			"Too many errors, stopped reading swap devices ...\n");
		(void)sleep(3);
	    }
	    return(0);
	}
	warning--; /* decrease counter, see end of function */

	KGET(VM_NSWAP, nswap);
	if (!nswap) {
		printf("No swap space available\n");
		return(0);
	}

	KGET(VM_NSWDEV, nswdev);
	KGET(VM_DMMAX, dmmax);
	KGET1(VM_SWAPLIST, &swaplist, sizeof(swaplist), "swaplist");
	if ((sw = (struct swdevt *)malloc(nswdev * sizeof(*sw))) == NULL ||
	    (perdev = (long *)malloc(nswdev * sizeof(*perdev))) == NULL)
		err(1, "malloc");
	KGET1(VM_SWDEVT, &ptr, sizeof ptr, "swdevt");
	KGET2(ptr, sw, nswdev * sizeof(*sw), "*swdevt");

	/* Count up swap space. */
	nfree = 0;
	memset(perdev, 0, nswdev * sizeof(*perdev));
#if  __FreeBSD_version >= 220000
	swapptr = swaplist.rlh_list;
	while (swapptr) {
#else
	while (swaplist) {
#endif
		int	top, bottom, next_block;
#if  __FreeBSD_version >= 220000
		KGET2(swapptr, &head, sizeof(struct rlist), "swapptr");
#else
		KGET2(swaplist, &head, sizeof(struct rlist), "swaplist");
#endif

		top = head.rl_end;
		bottom = head.rl_start;

		nfree += top - bottom + 1;

		/*
		 * Swap space is split up among the configured disks.
		 *
		 * For interleaved swap devices, the first dmmax blocks
		 * of swap space some from the first disk, the next dmmax
		 * blocks from the next, and so on up to nswap blocks.
		 *
		 * The list of free space joins adjacent free blocks,
		 * ignoring device boundries.  If we want to keep track
		 * of this information per device, we'll just have to
		 * extract it ourselves.
		 */
		while (top / dmmax != bottom / dmmax) {
			next_block = ((bottom + dmmax) / dmmax);
			perdev[(bottom / dmmax) % nswdev] +=
				next_block * dmmax - bottom;
			bottom = next_block * dmmax;
		}
		perdev[(bottom / dmmax) % nswdev] +=
			top - bottom + 1;

#if  __FreeBSD_version >= 220000
		swapptr = head.rl_next;
#else
		swaplist = head.rl_next;
#endif
	}

	header = getbsize(&hlen, &blocksize);
	div = blocksize / 512;
	avail = npfree = 0;
	for (i = 0; i < nswdev; i++) {
		int xsize, xfree;

		/*
		 * Don't report statistics for partitions which have not
		 * yet been activated via swapon(8).
		 */
		if (!(sw[i].sw_flags & SW_FREED))
			continue;

		/* The first dmmax is never allocated to avoid trashing of
		 * disklabels
		 */
		xsize = sw[i].sw_nblks - dmmax;
		xfree = perdev[i];
		used = xsize - xfree;
		npfree++;
		avail += xsize;
	}

	/* 
	 * If only one partition has been set up via swapon(8), we don't
	 * need to bother with totals.
	 */
	*retavail = avail / 2;
	*retfree = nfree / 2;
	used = avail - nfree;
	free(sw); free(perdev);

	/* increase counter, no errors occurs */
	warning++; 

	return  (0);
}


#endif /* __FreeBSD_version > 400000 */
#endif /* if defined(__FreeBSD__) */


void error_handle( int place, const char * message )
{
	int error_num;
	error_num = errno;
	/* if that was an interrupt - quit quietly */
	if (error_num == EINTR) {
		printf("asmem: Interrupted.\n");
		return;
	}
	switch ( place )
	{
	case 1: /* opening the /proc/meminfo file */
		switch (error_num)
		{
		case ENOENT :
			printf("asmem: The file %s does not exist. "
			"Weird system it is.\n", state.proc_mem_filename);
			break;
		case EACCES :
			printf("asmem: You do not have permissions "
			"to read %s\n", state.proc_mem_filename);
			break;
		default :
			printf("asmem: cannot open %s. Error %d: %s\n",
				state.proc_mem_filename, errno,
				strerror(errno));
			break;
		}
		break;
	default: /* catchall for the rest */
		printf("asmem: %s: Error %d: %s\n",
			message, errno, strerror(errno));
	}
}

#ifdef DEBUG
/* sab - 2000/01/21
 * Moved there here so it can be used in both BSD style and /proc/meminfo style
 * without repeating code and alowing us to keep the two main functions seperate
 */
#define verb_debug() { \
       printf("+- Total : %ld, used : %ld, free : %ld \n", \
                       state.fresh.total, \
                       state.fresh.used,\
                       state.fresh.free);\
       printf("|  Shared : %ld, buffers : %ld, cached : %ld \n",\
                       state.fresh.shared,\
                       state.fresh.buffers,\
                       state.fresh.cached);\
       printf("+- Swap total : %ld, used : %ld, free : %ld \n",\
                       state.fresh.swap_total,\
                       state.fresh.swap_used,\
                       state.fresh.swap_free);\
       }
#else
#define verb_debug()
#endif /* DEBUG */

#if defined(__FreeBSD__)
int read_meminfo() {
      int pagesize, pageshift;
#if __FreeBSD_version > 400000

      struct kvm_swap kswap[SWAP_DEVICES];
      int i, swaps;
      int swap_total = 0;
      int swap_free = 0;
      int swap_used = 0;
      static int old_swap_total, old_swap_used;
      static time_t saved_time = 0;
      time_t current_time;
      #define GETSWAP_DELAY 60 /* 1 min */

      /* get the info */
      if (kvm_read(kd, cnt_offset, (int *)(&sum), sizeof(sum)) != sizeof(sum))
            return (-1); 
            
      /* we obtain swap info every GETSWAP_DELAY seconds because of
       * kvm_getswapinfo CPU load 
       */
      current_time = time(NULL);
      if ((current_time-saved_time) > GETSWAP_DELAY) {
        saved_time = current_time;
        if (swaps = kvm_getswapinfo(kd, kswap, SWAP_DEVICES, 0) < 0)
                return (-1);
      
        /* process swap info */
        for (i=0; i<=swaps; i++) {
            swap_total += kswap[i].ksw_total;
            swap_used += kswap[i].ksw_used;
        }

        /* setup pageshift */
        pagesize = getpagesize();
        pageshift = 0;
        while (pagesize > 1) {
              pageshift++;
              pagesize >>= 1;
        }
        /* store obtained results */
        old_swap_total = swap_total;
        old_swap_used = swap_used;
        
      } else {
        swap_total = old_swap_total;
        swap_used = old_swap_used;
      }
      
      state.fresh.swap_total = pagetok(swap_total);
      state.fresh.swap_free = pagetok(swap_total-swap_used);
      state.fresh.swap_used = pagetok(swap_used);
      
#else
      /* get the info */
      if (kvm_read(kd, cnt_offset, (int *)(&sum), sizeof(sum)) != sizeof(sum))
            return (-1); 
            /* setup pageshift */
      pagesize = getpagesize();
      pageshift = 0;
      while (pagesize > 1) {
              pageshift++;
              pagesize >>= 1;
      }
      swapmode (&state.fresh.swap_total,&state.fresh.swap_free);
      state.fresh.swap_used = state.fresh.swap_total-state.fresh.swap_free;

      state.fresh.swap_total = state.fresh.swap_total << 10;
      state.fresh.swap_free = state.fresh.swap_free << 10;
      state.fresh.swap_used = state.fresh.swap_used << 10;

#endif /* if __FreeBSD_version > 400000  */

      state.fresh.total =  pagetok(sum.v_page_count);
      state.fresh.used = pagetok(sum.v_page_count-sum.v_free_count);
      state.fresh.free = pagetok(sum.v_free_count);
                        
      state.fresh.shared = 0;  /* dont know how to get these */
      state.fresh.buffers = 0;
      state.fresh.cached = 0;

      verb_debug();
      return 0;
}

#else
#ifdef HAVE_UVM_UVM_EXTERN_H
/* using kvm/uvm (BSD systems) ... */

#define pagetok(size) ((size) << pageshift)

int read_meminfo()
{
      int pagesize, pageshift;
      int mib[2];
      size_t usize;
      struct uvmexp uvm_exp;

      /* get the info */
      mib[0] = CTL_VM;
      mib[1] = VM_UVMEXP;
      usize = sizeof(uvm_exp);
      if (sysctl(mib, 2, &uvm_exp, &usize, NULL, 0) < 0) {
        fprintf(stderr, "asmem: sysctl uvm_exp failed: %s\n",
            strerror(errno));
          return -1;
      }

      /* setup pageshift */
      pagesize = uvm_exp.pagesize;
      pageshift = 0;
      while (pagesize > 1)
      {
              pageshift++;
              pagesize >>= 1;
      }

      /* update state */
      state.fresh.total =  pagetok(uvm_exp.npages);
      state.fresh.used = pagetok(uvm_exp.active);
      state.fresh.free = pagetok(uvm_exp.free);
      state.fresh.shared = 0;  /* dont know how to get these */
      state.fresh.buffers = 0;
      state.fresh.cached = 0;
      state.fresh.swap_total =  pagetok(uvm_exp.swpages);
      state.fresh.swap_used = pagetok(uvm_exp.swpginuse);
      state.fresh.swap_free = pagetok(uvm_exp.swpages-uvm_exp.swpginuse);
      verb_debug();
      return 0;
}

#else
/* default /proc/meminfo (Linux) method ... */

int getnum(FILE *fp, char *marker)
{
	char thebuf[255];
	int done = 0;
	int theval;

	do {
		if (fgets(thebuf, sizeof(thebuf), fp) == NULL) {
			printf("file error\n");
			return (-1);
		
		} else
			if (strstr(thebuf, marker)) {
				sscanf(thebuf, "%*s %d %*s\n",
					&theval);
				return (theval);
			}
	}while (!done);
}

int read_meminfo()
{
	int result;

	fflush(fp);

	result = fseek(fp, 0L, SEEK_SET);

	if ( result < 0 ) {
		error_handle(2, "seek");
		return -1;
	}

	state.fresh.total = getnum(fp, "MemTotal") * 1024;
	state.fresh.free = getnum(fp, "MemFree") * 1024;
	/* state.fresh.shared = getnum(fp, "MemShared") * 1024; */
	state.fresh.shared = 0;   /* this is always 0 */
	state.fresh.buffers = getnum(fp, "Buffers") * 1024;
	state.fresh.cached = getnum(fp, "Cached") * 1024;
	state.fresh.swap_total = getnum(fp, "SwapTotal") * 1024;
	state.fresh.swap_free = getnum(fp, "SwapFree") * 1024;
	state.fresh.swap_used = state.fresh.swap_total - state.fresh.swap_free;
	state.fresh.used = state.fresh.total - state.fresh.free;

	return 0;
}

#endif /* (else) HAVE_UVM_UVM_EXTERN_H */
#endif /* FreeBSD */
int open_meminfo()
{
#if defined(__FreeBSD__)
        if ((kd = kvm_open(NULL, NULL, NULL, O_RDONLY, "kvm_open")) == NULL) {
            return -1;
        }
                            
        if (kvm_nlist(kd,nlst) < 0) {
            return -1;
        }
        cnt_offset = nlst[X_CNT].n_value;

#else
#ifndef HAVE_UVM_UVM_EXTERN_H
	int result;
	if ((fp = fopen(state.proc_mem_filename, "r")) == NULL) {
		error_handle(1, "");
		return -1;
	}
#endif /* !HAVE_UVM_UVM_EXTERN_H */
#endif /* defined(__FreeBSD__)  */
	return 0;
}

int close_meminfo()
{
#if defined(__FreeBSD__)
        kvm_close(kd);
#else
#ifndef HAVE_UVM_UVM_EXTERN_H
	fclose(fp);
#endif /* !HAVE_UVM_UVM_EXTERN_H */
#endif /* defined(__FreeBSD__) */
	return 0;
}

