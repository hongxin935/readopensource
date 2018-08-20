/* 
   Copyright (C) Andrew Tridgell 1996
   Copyright (C) Paul Mackerras 1996
   
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
   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*/

#include "rsync.h"

extern int verbose;

typedef unsigned short tag;

#define TABLESIZE (1<<16)
#define NULL_TAG ((tag)-1)

static int false_alarms;
static int tag_hits;
static int matches;

struct target {
  tag t;
  int i;
};

static struct target *targets=NULL;

static tag tag_table[TABLESIZE];

#define gettag(sum) (((sum)>>16) + ((sum)&0xFFFF))

static int compare_targets(struct target *t1,struct target *t2)
{
  return(t1->t - t2->t);
}

static void build_hash_table(struct sum_struct *s)
{
  int i;

  targets = (struct target *)malloc(sizeof(targets[0])*s->count);
  if (!targets) out_of_memory("build_hash_table");

  for (i=0;i<s->count;i++) {
    targets[i].i = i;
    targets[i].t = gettag(s->sums[i].sum1);
  }

  qsort(targets,s->count,sizeof(targets[0]),(int (*)())compare_targets);

  for (i=0;i<TABLESIZE;i++)
    tag_table[i] = NULL_TAG;

  for (i=s->count-1;i>=0;i--) {    
    tag_table[targets[i].t] = i;
  }
}



static off_t last_match;


static void matched(int f,struct sum_struct *s,char *buf,off_t len,int offset,int i)
{
  int n = offset - last_match;
  
  if (verbose > 2)
    if (i != -1)
      fprintf(stderr,"match at %d last_match=%d j=%d len=%d n=%d\n",
	      (int)offset,(int)last_match,i,(int)s->sums[i].len,n);

  if (n > 0) {
    write_int(f,n);
    write_buf(f,buf+last_match,n);
  }
  write_int(f,-(i+1));
  if (i != -1)
    last_match = offset + s->sums[i].len;
  if (n > 0)
    write_flush(f);
}


static void hash_search(int f,struct sum_struct *s,char *buf,off_t len)
{
  int offset,j,k;
  int end;
  char sum2[SUM_LENGTH];
  uint32 s1, s2, sum;

  if (verbose > 2)
    fprintf(stderr,"hash search b=%d len=%d\n",s->n,(int)len);

  k = MIN(len, s->n);
  sum = get_checksum1(buf, k);
  s1 = sum;
  s2 = sum >> 16;
  if (verbose > 3)
    fprintf(stderr, "sum=%.8x k=%d\n", sum, k);

  offset = 0;

  end = len + 1 - s->sums[s->count-1].len;

  if (verbose > 3)
    fprintf(stderr,"hash search s->n=%d len=%d count=%d\n",
	    s->n,(int)len,s->count);

  do {
    tag t = (s1 + s2) & 0xffff;		/* gettag(sum) */
    j = tag_table[t];
    if (verbose > 4)
      fprintf(stderr,"offset=%d sum=%08x\n",
	      offset,sum);

    if (j != NULL_TAG) {
      int done_csum2 = 0;

      sum = (s1 & 0xffff) + (s2 << 16);
      tag_hits++;
      do {
	int i = targets[j].i;

	if (sum == s->sums[i].sum1) {
	  if (verbose > 3)
	    fprintf(stderr,"potential match at %d target=%d %d sum=%08x\n",
		    offset,j,i,sum);

	  if (!done_csum2) {
	    get_checksum2(buf+offset,MIN(s->n,len-offset),sum2);
	    done_csum2 = 1;
	  }
	  if (memcmp(sum2,s->sums[i].sum2,SUM_LENGTH) == 0) {
	    matched(f,s,buf,len,offset,i);
	    offset += s->sums[i].len - 1;
	    k = MIN((len-offset), s->n);
	    sum = get_checksum1(buf+offset, k);
	    s1 = sum;
	    s2 = sum >> 16;
	    ++matches;
	    break;
	  } else {
	    false_alarms++;
	  }
	}
	j++;
      } while (j<s->count && targets[j].t == t);
    }

    /* Trim off the first byte from the checksum */
    s1 -= buf[offset];
    s2 -= k * buf[offset];

    /* Add on the next byte (if there is one) to the checksum */
    if (k < (len-offset)) {
      s1 += buf[offset+k];
      s2 += s1;
    } else {
      --k;
    }

    if (verbose > 3) 
      fprintf(stderr,"s2:s1 = %.4x%.4x sum=%.8x k=%d offset=%d took %x added %x\n",
	      s2&0xffff, s1&0xffff, get_checksum1(buf+offset+1,k),
	      k, (int)offset, buf[offset], buf[offset+k]);
  } while (++offset < end);

  matched(f,s,buf,len,len,-1);
}


void match_sums(int f,struct sum_struct *s,char *buf,off_t len)
{
  last_match = 0;
  false_alarms = 0;
  tag_hits = 0;

  if (len > 0 && s->count>0) {
    build_hash_table(s);

    if (verbose > 2) 
      fprintf(stderr,"built hash table\n");

    hash_search(f,s,buf,len);

    if (verbose > 2) 
      fprintf(stderr,"done hash search\n");
  } else {
    matched(f,s,buf,len,len,-1);
  }

  if (targets) {
    free(targets);
    targets=NULL;
  }

  if (verbose > 2)
    fprintf(stderr, "false_alarms=%d tag_hits=%d matches=%d\n",
	    false_alarms, tag_hits, matches);
}
