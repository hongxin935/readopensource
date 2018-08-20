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
extern int always_checksum;
extern time_t starttime;

extern char *backup_suffix;

extern int block_size;
extern int update_only;
extern int make_backups;
extern int preserve_links;
extern int preserve_perms;
extern int preserve_devices;
extern int preserve_uid;
extern int preserve_gid;
extern int preserve_times;

/*
  free a sums struct
  */
static void free_sums(struct sum_struct *s)
{
  if (s->sums) free(s->sums);
  free(s);
}



/*
  send a sums struct down a fd
  */
static void send_sums(struct sum_struct *s,int f_out)
{
  int i;

  /* tell the other guy how many we are going to be doing and how many
     bytes there are in the last chunk */
  write_int(f_out,s?s->count:0);
  write_int(f_out,s?s->n:block_size);
  write_int(f_out,s?s->remainder:0);
  if (s)
    for (i=0;i<s->count;i++) {
      write_int(f_out,s->sums[i].sum1);
      write_buf(f_out,s->sums[i].sum2,SUM_LENGTH);
    }
  write_flush(f_out);
}


/*
  generate a stream of signatures/checksums that describe a buffer

  generate approximately one checksum every n bytes
  */
static struct sum_struct *generate_sums(char *buf,off_t len,int n)
{
  int i;
  struct sum_struct *s;
  int count;
  int block_len = n;
  int remainder = (len%block_len);
  off_t offset = 0;

  count = (len+(block_len-1))/block_len;

  s = (struct sum_struct *)malloc(sizeof(*s));
  if (!s) out_of_memory("generate_sums");

  s->count = count;
  s->remainder = remainder;
  s->n = n;
  s->flength = len;

  if (count==0) {
    s->sums = NULL;
    return s;
  }

  if (verbose > 3)
    fprintf(stderr,"count=%d rem=%d n=%d flength=%d\n",
	    s->count,s->remainder,s->n,(int)s->flength);

  s->sums = (struct sum_buf *)malloc(sizeof(s->sums[0])*s->count);
  if (!s->sums) out_of_memory("generate_sums");
  
  for (i=0;i<count;i++) {
	  // 考虑 len < n
	  // 不足 n 即一个数据快的情况下有
    int n1 = MIN(len,n);

    s->sums[i].sum1 = get_checksum1(buf,n1);
    get_checksum2(buf,n1,s->sums[i].sum2);

    s->sums[i].offset = offset;
    s->sums[i].len = n1;
    s->sums[i].i = i;

    if (verbose > 3)
      fprintf(stderr,"chunk[%d] offset=%d len=%d sum1=%08x\n",
	      i,(int)s->sums[i].offset,s->sums[i].len,s->sums[i].sum1);

    len -= n1;
    buf += n1;
    offset += n1;
  }

  return s;
}


/*
  receive the checksums for a buffer
  */
static struct sum_struct *receive_sums(int f)
{
  struct sum_struct *s;
  int i;
  off_t offset = 0;
  int block_len;

  s = (struct sum_struct *)malloc(sizeof(*s));
  if (!s) out_of_memory("receive_sums");

  s->count = read_int(f);
  s->n = read_int(f);
  s->remainder = read_int(f);  
  s->sums = NULL;

  if (verbose > 3)
    fprintf(stderr,"count=%d n=%d rem=%d\n",
	    s->count,s->n,s->remainder);

  block_len = s->n;

  if (s->count == 0) 
    return(s);

  s->sums = (struct sum_buf *)malloc(sizeof(s->sums[0])*s->count);
  if (!s->sums) out_of_memory("receive_sums");

  for (i=0;i<s->count;i++) {
    s->sums[i].sum1 = read_int(f);
    read_buf(f,s->sums[i].sum2,SUM_LENGTH);

    s->sums[i].offset = offset;
    s->sums[i].i = i;

    if (i == s->count-1 && s->remainder != 0) {
      s->sums[i].len = s->remainder;
    } else {
      s->sums[i].len = s->n;
    }
    offset += s->sums[i].len;

    if (verbose > 3)
      fprintf(stderr,"chunk[%d] len=%d offset=%d sum1=%08x\n",
	      i,s->sums[i].len,(int)s->sums[i].offset,s->sums[i].sum1);
  }

  s->flength = offset;

  return s;
}


static void set_perms(char *fname,struct file_struct *file)
{
#ifdef HAVE_UTIME
  if (preserve_times) {
    struct utimbuf tbuf;  
    tbuf.actime = time(NULL);
    tbuf.modtime = file->modtime;
    if (utime(fname,&tbuf) != 0)
      fprintf(stderr,"failed to set times on %s : %s\n",
	      fname,strerror(errno));
  }
#endif

#ifdef HAVE_CHMOD
  if (preserve_perms) {
    if (chmod(fname,file->mode) != 0)
      fprintf(stderr,"failed to set permissions on %s : %s\n",
	      fname,strerror(errno));
  }
#endif

#ifdef HAVE_CHOWN
  if (preserve_uid || preserve_gid) {
    if (chown(fname,
	      preserve_uid?file->uid:-1,
	      preserve_gid?file->gid:-1) != 0) {
      fprintf(stderr,"chown %s : %s\n",fname,strerror(errno));
    }
  }
#endif
}


void recv_generator(char *fname,struct file_list *flist,int i,int f_out)
{  
  int fd;
  struct stat st;
  char *buf;
  struct sum_struct *s;
  char sum[SUM_LENGTH];
  int statret;

  if (verbose > 2)
    fprintf(stderr,"recv_generator(%s)\n",fname);

  statret = stat(fname,&st);

#if SUPPORT_LINKS
  if (preserve_links && S_ISLNK(flist->files[i].mode)) {
    char lnk[MAXPATHLEN];
    int l;
    if (statret == 0) {
      l = readlink(fname,lnk,MAXPATHLEN-1);
      if (l > 0) {
	lnk[l] = 0;
	if (strcmp(lnk,flist->files[i].link) == 0)
	  return;
      }
    }
    unlink(fname);
    if (symlink(flist->files[i].link,fname) != 0) {
      fprintf(stderr,"link %s -> %s : %s\n",
	      fname,flist->files[i].link,strerror(errno));
    } else {
      if (verbose) 
	fprintf(stderr,"%s -> %s\n",fname,flist->files[i].link);
    }
    return;
  }
#endif

#ifdef HAVE_MKNOD
  if (preserve_devices && 
      (S_ISCHR(flist->files[i].mode) || S_ISBLK(flist->files[i].mode))) {
    if (statret != 0 || 
	st.st_mode != flist->files[i].mode ||
	st.st_rdev != flist->files[i].dev) {	
      unlink(fname);
      if (verbose > 2)
	fprintf(stderr,"mknod(%s,0%o,0x%x)\n",
		fname,(int)flist->files[i].mode,(int)flist->files[i].dev);
      if (mknod(fname,flist->files[i].mode,flist->files[i].dev) != 0) {
	fprintf(stderr,"mknod %s : %s\n",fname,strerror(errno));
      } else {
	if (verbose)
	  fprintf(stderr,"%s\n",fname);
	set_perms(fname,&flist->files[i]);
      }
    }
    return;
  }
#endif

  if (!S_ISREG(flist->files[i].mode)) {
    fprintf(stderr,"skipping non-regular file %s\n",fname);
    return;
  }

  if (statret == -1) {
    if (errno == ENOENT) {
      write_int(f_out,i);
      send_sums(NULL,f_out);
    } else {
      if (verbose > 1)
	fprintf(stderr,"recv_generator failed to open %s\n",fname);
    }
    return;
  }

  if (!S_ISREG(st.st_mode)) {
    fprintf(stderr,"%s : not a regular file\n",fname);
    return;
  }

  if (update_only && st.st_mtime >= flist->files[i].modtime) {
    if (verbose > 1)
      fprintf(stderr,"%s is newer\n",fname);
    return;
  }

  if (always_checksum && S_ISREG(st.st_mode)) {
    file_checksum(fname,sum,st.st_size);
  }

  // 接收端对比是否有修改
  if ((st.st_size == flist->files[i].length &&
       ((!preserve_perms || st.st_mtime == flist->files[i].modtime) ||
	(S_ISREG(st.st_mode) && 
	 always_checksum && memcmp(sum,flist->files[i].sum,SUM_LENGTH) == 0)))) {
    if (verbose > 1)
      fprintf(stderr,"%s is uptodate\n",fname);
    return;
  }

  /* open the file */  
  fd = open(fname,O_RDONLY);

  if (fd == -1) {
    fprintf(stderr,"failed to open %s : %s\n",fname,strerror(errno));
    return;
  }

  if (st.st_size > 0) {
    buf = map_file(fd,st.st_size);
    if (!buf) {
      fprintf(stderr,"mmap : %s\n",strerror(errno));
      close(fd);
      return;
    }
  } else {
    buf = NULL;
  }

  if (verbose > 3)
    fprintf(stderr,"mapped %s of size %d\n",fname,(int)st.st_size);

  s = generate_sums(buf,st.st_size,BLOCK_SIZE);

  write_int(f_out,i);
  send_sums(s,f_out);
  write_flush(f_out);

  close(fd);
  unmap_file(buf,st.st_size);

  free_sums(s);
}



static void receive_data(int f_in,char *buf,int fd)
{
  int i,n,remainder,len,count;
  int size = 0;
  char *buf2=NULL;
  off_t offset = 0;
  off_t offset2;

  count = read_int(f_in);
  n = read_int(f_in);
  remainder = read_int(f_in);

  for (i=read_int(f_in); i != 0; i=read_int(f_in)) {
    if (i > 0) {
      if (i > size) {
	if (buf2) free(buf2);
	buf2 = (char *)malloc(i);
	size = i;
	if (!buf2) out_of_memory("receive_data");
      }

      if (verbose > 3)
	fprintf(stderr,"data recv %d at %d\n",i,(int)offset);

      read_buf(f_in,buf2,i);
      write(fd,buf2,i);
      offset += i;
    } else {
      i = -(i+1);
      offset2 = i*n;
      len = n;
      if (i == count-1 && remainder != 0)
	len = remainder;

      if (verbose > 3)
	fprintf(stderr,"chunk[%d] of size %d at %d offset=%d\n",
		i,len,(int)offset2,(int)offset);

      write(fd,buf+offset2,len);
      offset += len;
    }
  }
  if (buf2) free(buf2);
}



int recv_files(int f_in,struct file_list *flist,char *local_name)
{  
  int fd1,fd2;
  struct stat st;
  char *fname;
  char fnametmp[MAXPATHLEN];
  char *buf;
  int i;

  if (verbose > 2)
    fprintf(stderr,"recv_files(%d) starting\n",flist->count);

  while (1) 
    {
      i = read_int(f_in);
      if (i == -1) break;

      fname = flist->files[i].name;

      if (local_name)
	fname = local_name;

      if (verbose > 2)
	fprintf(stderr,"recv_files(%s)\n",fname);

      /* open the file */  
      fd1 = open(fname,O_RDONLY|O_CREAT,flist->files[i].mode);

      if (fd1 == -1) {
	fprintf(stderr,"recv_files failed to open %s\n",fname);
	return -1;
      }

      if (fstat(fd1,&st) != 0) {
	fprintf(stderr,"fstat %s : %s\n",fname,strerror(errno));
	close(fd1);
	return -1;
      }

      if (!S_ISREG(st.st_mode)) {
	fprintf(stderr,"%s : not a regular file\n",fname);
	close(fd1);
	return -1;
      }

      if (st.st_size > 0) {
	buf = map_file(fd1,st.st_size);
	if (!buf) return -1;
      } else {
	buf = NULL;
      }

      if (verbose > 2)
	fprintf(stderr,"mapped %s of size %d\n",fname,(int)st.st_size);

      /* open tmp file */
      sprintf(fnametmp,"%s.XXXXXX",fname);
      if (NULL == mktemp(fnametmp)) 
	return -1;
      fd2 = open(fnametmp,O_WRONLY|O_CREAT,st.st_mode);
      if (fd2 == -1) return -1;

      if (verbose)
	fprintf(stderr,"%s\n",fname);

      /* recv file data */
      receive_data(f_in,buf,fd2);

      close(fd1);
      close(fd2);

      if (verbose > 2)
	fprintf(stderr,"renaming %s to %s\n",fnametmp,fname);

      if (make_backups) {
	char fnamebak[MAXPATHLEN];
	sprintf(fnamebak,"%s%s",fname,backup_suffix);
	if (rename(fname,fnamebak) != 0) {
	  fprintf(stderr,"rename %s %s : %s\n",fname,fnamebak,strerror(errno));
	  exit(1);
	}
      }

      /* move tmp file over real file */
      if (rename(fnametmp,fname) != 0) {
	fprintf(stderr,"rename %s -> %s : %s\n",fnametmp,fname,strerror(errno));
      }

      unmap_file(buf,st.st_size);

      set_perms(fname,&flist->files[i]);
    }

  if (verbose > 2)
    fprintf(stderr,"recv_files finished\n");
  
  return 0;
}



off_t send_files(struct file_list *flist,int f_out,int f_in)
{ 
  int fd;
  struct sum_struct *s;
  char *buf;
  struct stat st;
  char fname[MAXPATHLEN];  
  off_t total=0;
  int i;

  if (verbose > 2)
    fprintf(stderr,"send_files starting\n");

  while (1) 
    {
      i = read_int(f_in);
      if (i == -1) break;

      fname[0] = 0;
      if (flist->files[i].dir) {
	strcpy(fname,flist->files[i].dir);
	strcat(fname,"/");
      }
      strcat(fname,flist->files[i].name);

      if (verbose > 2) 
	fprintf(stderr,"send_files(%d,%s)\n",i,fname);

      fd = open(fname,O_RDONLY);
      if (fd == -1) {
	fprintf(stderr,"send_files failed to open %s: %s\n",
		fname,strerror(errno));
	return -1;
      }
  
      s = receive_sums(f_in);
      if (!s) 
	return -1;

      /* map the local file */
      if (fstat(fd,&st) != 0) 
	return -1;
      
      if (st.st_size > 0) {
	buf = map_file(fd,st.st_size);
	if (!buf) return -1;
      } else {
	buf = NULL;
      }

      if (verbose > 2)
	fprintf(stderr,"send_files mapped %s of size %d\n",
		fname,(int)st.st_size);

      write_int(f_out,i);

      write_int(f_out,s->count);
      write_int(f_out,s->n);
      write_int(f_out,s->remainder);

      if (verbose > 2)
	fprintf(stderr,"calling match_sums %s\n",fname);
      
      match_sums(f_out,s,buf,st.st_size);
      write_flush(f_out);
      
      unmap_file(buf,st.st_size);
      close(fd);

      free_sums(s);

      if (verbose > 2)
	fprintf(stderr,"sender finished %s\n",fname);

      total += st.st_size;
    }

  write_int(f_out,-1);
  write_flush(f_out);

  return total;
}



