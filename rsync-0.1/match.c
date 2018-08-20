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
    fprintf(stderr, " %d -> %u -> %d\n", i, targets[i].t, targets[i].i);
  }

  // 从大到小排序
  qsort(targets,s->count,sizeof(targets[0]),(int (*)())compare_targets);

  for (i=0;i<s->count;i++) {
    fprintf(stderr, " %d -> %u -> %d\n", i, targets[i].t, targets[i].i);
  }
  for (i=0;i<TABLESIZE;i++)
    tag_table[i] = NULL_TAG;

  // vector< pair< tag, i > > targets
  // tag_table 类似bitmap， 第 x 个，存tag 为x 的下标i
  // 此函数如果用复杂一点的数据结构，就是 map<tag, pair<tag, i> > , 更简洁就是 map<tag, i>
  // 如果存在相同的 targets[i].t 那么这里记录下来的是最小的 i 
  // 所以后面在查的时候，命中了，还得从i开始循环遍历整个 targets 才能把所有可能的 targets[i].t 找到
  for (i=s->count-1;i>=0;i--) {    
    tag_table[targets[i].t] = i;
  }
}



static off_t last_match;


// 直到找到一个match
// 把上一次match的位置 到这一次match的位置中间的所有buf发过去（也就是不match的部分发过去）
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
  write_int(f,-(i+1)); // 可能是0(有一方为空，剩余数据发送)， -1 第一块数据就相同, -2 依次类推
  if (i != -1)
    last_match = offset + s->sums[i].len;
  if (n > 0)
    write_flush(f);
}


static void hash_search(int f,struct sum_struct *s,char *buf,off_t len)
{
    // 对比本地文件 buf 和 对端传递过来的 checksums
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

  // 对于本地的buf，每次移动一个bytes，对比当前chunk是否在对端的checksums之中，
  // 在对端的checksums中，说明对端该数据块没有发生变化，此时发送直到找到match时的所有不match的数据过去
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
      // get_checksum1 中
      // s1 += buf[i];
      // s2 += s1;
      // 1
      // 1 2 3
      // 1 2 3 4
      // 第一个byte在s2中将会是k次
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

  // 最后有剩余部分的话，就发送过去
  matched(f,s,buf,len,len,-1);
}


void match_sums(int f,struct sum_struct *s,char *buf,off_t len)
{
    // 对比本地文件 buf 和 对端传递过来的 checksums
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
      // 任意一边文件为空
      // 发送端为空 len = 0 
      // 对端为空
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
