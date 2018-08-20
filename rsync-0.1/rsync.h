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

#define BLOCK_SIZE 700
#define RSYNC_RSH_ENV "RSYNC_RSH"
#define RSYNC_RSH "rsh"
#define RSYNC_NAME "rsync"
#define BACKUP_SUFFIX "~"

/* update this if you make incompatible changes */
#define PROTOCOL_VERSION 6

#include "config.h"

#include <sys/types.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <stdio.h>

#ifdef HAVE_SYS_PARAM_H
#include <sys/param.h>
#endif

#ifdef STDC_HEADERS
# include <stdlib.h>
# include <string.h>
#endif

#ifdef HAVE_COMPAT_H
#include <compat.h>
#endif

#ifdef HAVE_MALLOC_H
#include <malloc.h>
#endif

#ifdef TIME_WITH_SYS_TIME
#include <sys/time.h>
#include <time.h>
#else
#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#else
#include <time.h>
#endif
#endif

#ifdef HAVE_FCNTL_H
#include <fcntl.h>
#else
#ifdef HAVE_SYS_FCNTL_H
#include <sys/fcntl.h>
#endif
#endif

#include <sys/stat.h>

#include <signal.h>
#ifdef HAVE_SYS_WAIT_H
#include <sys/wait.h>
#endif
#ifdef HAVE_CTYPE_H
#include <ctype.h>
#endif
#ifdef HAVE_GRP_H
#include <grp.h>
#endif
#include <errno.h>

#include <sys/mman.h>
#include <utime.h>

#ifndef S_ISLNK
#define S_ISLNK(mode) (((mode) & S_IFLNK) == S_IFLNK)
#endif

#ifndef uchar
#define uchar unsigned char
#endif

#ifndef int32
#if (SIZEOF_INT == 4)
#define int32 int
#elif (SIZEOF_LONG == 4)
#define int32 long
#elif (SIZEOF_SHORT == 4)
#define int32 short
#endif
#endif

#ifndef uint32
#define uint32 unsigned int32
#endif


#ifndef MIN
#define MIN(a,b) ((a)<(b)?(a):(b))
#endif

#ifndef MAX
#define MAX(a,b) ((a)>(b)?(a):(b))
#endif

/* the length of the md4 checksum */
#define SUM_LENGTH 16

#ifndef MAXPATHLEN
#define MAXPATHLEN 1024
#endif

struct file_struct {
  time_t modtime;
  off_t length;
  mode_t mode;
  dev_t dev;
  uid_t uid;
  gid_t gid;
  char *name;
  char *dir;
  char *link;
  char sum[SUM_LENGTH];
};

struct file_list {
  int count;
  struct file_struct *files;
};

struct sum_buf {
  off_t offset;			/* offset in file of this chunk */  // 数据块偏移
  int len;			/* length of chunk of file */ // 数据块大小
  int i;			/* index of this chunk */ // 第i个数据块
  uint32 sum1;	                /* simple checksum */ 
  char sum2[SUM_LENGTH];	/* md4 checksum  */
};

struct sum_struct {
  off_t flength;		/* total file length */ // buf总字节数
  int count;			/* how many chunks */ // 有多少个n字节块
  int remainder;		/* flength % block_length */ // 不足n字节的那个数据块有多少字节
  int n;			/* block_length */ // 按多少字节分数据块
  struct sum_buf *sums;		/* points to info for each chunk */
};


#include "byteorder.h"
#include "version.h"
#include "proto.h"
#include "md4.h"

#if !HAVE_STRERROR
extern char *sys_errlist[];
#define strerror(i) sys_errlist[i]
#endif

#ifndef HAVE_STRCHR
# define strchr                 index
# define strrchr                rindex
#endif

#if HAVE_DIRENT_H
# include <dirent.h>
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

#ifndef HAVE_ERRNO_DECL
extern int errno;
#endif

#ifndef HAVE_BCOPY
#define bcopy(src,dest,n) memcpy(dest,src,n)
#endif

#ifndef HAVE_BZERO
#define bzero(buf,n) memset(buf,0,n)
#endif

#define SUPPORT_LINKS (HAVE_READLINK && defined(S_ISLNK))

#if !SUPPORT_LINKS
#define lstat stat
#endif
