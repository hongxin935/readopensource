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

/* generate and receive file lists */

#include "rsync.h"

extern int verbose;
extern int always_checksum;
extern off_t total_size;

extern int make_backups;
extern int preserve_links;
extern int preserve_perms;
extern int preserve_devices;
extern int preserve_uid;
extern int preserve_gid;
extern int preserve_times;


/*
  This function is used to check if a file should be included/excluded
  from the list of files based on its name and type etc

  This will be expanded soon to include globbing functions, .ignore
  files and anything else Paul can dream up :-)
 */
static int match_file_name(char *fname,struct stat *st)
{

  return 1;
}



static void send_directory(int f,struct file_list *flist,char *dir);

static char *flist_dir = NULL;

static void send_file_entry(struct file_struct *file,int f)
{
  write_int(f,strlen(file->name));
  write_buf(f,file->name,strlen(file->name));
  write_int(f,(int)file->modtime);
  write_int(f,(int)file->length);
  write_int(f,(int)file->mode);
  if (preserve_uid)
    write_int(f,(int)file->uid);
  if (preserve_gid)
    write_int(f,(int)file->gid);
  if (preserve_devices) {
    if (verbose > 2)
      fprintf(stderr,"dev=0x%x\n",(int)file->dev);
    write_int(f,(int)file->dev);
  }

#if SUPPORT_LINKS
  if (preserve_links && S_ISLNK(file->mode)) {
    write_int(f,strlen(file->link));
    write_buf(f,file->link,strlen(file->link));
  }
#endif

  if (always_checksum) {
    write_buf(f,file->sum,SUM_LENGTH);
  }       
}


static struct file_struct *make_file(int recurse,char *fname)
{
  static struct file_struct file;
  struct stat st;
  char sum[SUM_LENGTH];

  bzero(sum,SUM_LENGTH);

  if (lstat(fname,&st) != 0) {
    fprintf(stderr,"%s: %s\n",
	    fname,strerror(errno));
    return NULL;
  }

  if (S_ISDIR(st.st_mode) && !recurse) {
    fprintf(stderr,"skipping directory %s\n",fname);
    return NULL;
  }

  if (!match_file_name(fname,&st))
    return NULL;

  if (verbose > 2)
    fprintf(stderr,"make_file(%s)\n",fname);

  file.name = strdup(fname);
  file.modtime = st.st_mtime;
  file.length = st.st_size;
  file.mode = st.st_mode;
  file.uid = st.st_uid;
  file.gid = st.st_gid;
#ifdef HAVE_ST_RDEV
  file.dev = st.st_rdev;
#endif

#if SUPPORT_LINKS
  if (S_ISLNK(st.st_mode)) {
    int l;
    char lnk[MAXPATHLEN];
    if ((l=readlink(fname,lnk,MAXPATHLEN-1)) == -1) {
      fprintf(stderr,"readlink %s : %s\n",fname,strerror(errno));
      return NULL;
    }
    lnk[l] = 0;
    file.link = strdup(lnk);
  }
#endif

  if (always_checksum && S_ISREG(st.st_mode)) {
    file_checksum(fname,file.sum,st.st_size);
  }       

  if (flist_dir)
    file.dir = strdup(flist_dir);
  else
    file.dir = NULL;

  if (!S_ISDIR(st.st_mode))
    total_size += st.st_size;

  return &file;
}


static int flist_malloced;

static void send_file_name(int f,struct file_list *flist,
			   int recurse,char *fname)
{
  struct file_struct *file;

  file = make_file(recurse,fname);

  if (!file) return;
  
  if (flist->count >= flist_malloced) {
    flist_malloced += 100;
    flist->files = (struct file_struct *)realloc(flist->files,
						 sizeof(flist->files[0])*
						 flist_malloced);
    if (!flist->files)
      out_of_memory("send_file_name");
  }

  flist->files[flist->count++] = *file;    

  send_file_entry(file,f);

  if (S_ISDIR(file->mode) && recurse) {      
    send_directory(f,flist,file->name);
    return;
  }
}



static void send_directory(int f,struct file_list *flist,char *dir)
{
  DIR *d;
  struct dirent *di;
  char fname[MAXPATHLEN];
  int l;
  char *p;

  d = opendir(dir);
  if (!d) {
    fprintf(stderr,"%s: %s\n",
	    dir,strerror(errno));
    return;
  }

  strcpy(fname,dir);
  l = strlen(fname);
  if (fname[l-1] != '/')
    strcat(fname,"/");
  p = fname + strlen(fname);
  
  for (di=readdir(d); di; di=readdir(d)) {
    if (strcmp(di->d_name,".")==0 ||
	strcmp(di->d_name,"..")==0)
      continue;
    strcpy(p,di->d_name);
    send_file_name(f,flist,1,fname);
  }

  closedir(d);
}



struct file_list *send_file_list(int f,int recurse,int argc,char *argv[])
{
  int i,l;
  struct stat st;
  char *p,*dir;
  char dbuf[MAXPATHLEN];
  struct file_list *flist;

  flist = (struct file_list *)malloc(sizeof(flist[0]));
  if (!flist) out_of_memory("send_file_list");

  flist->count=0;
  flist_malloced = 100;
  flist->files = (struct file_struct *)malloc(sizeof(flist->files[0])*
					      flist_malloced);
  if (!flist->files) out_of_memory("send_file_list");

  for (i=0;i<argc;i++) {
    char *fname = argv[i];

    l = strlen(fname);
    if (l != 1 && fname[l-1] == '/')
      fname[l-1] = 0;

    if (lstat(fname,&st) != 0) {
      fprintf(stderr,"%s : %s\n",fname,strerror(errno));
      continue;
    }

    if (S_ISDIR(st.st_mode) && !recurse) {
      fprintf(stderr,"skipping directory %s\n",fname);
      continue;
    }

    if (S_ISDIR(st.st_mode) && argc == 1) {
      if (chdir(fname) != 0) {
	fprintf(stderr,"chdir %s : %s\n",fname,strerror(errno));
	continue;
      }
      send_file_name(f,flist,recurse,".");
      continue;
    } 

    dir = NULL;
    p = strrchr(fname,'/');
    if (p && !p[1]) {
      *p = 0;
      p = strrchr(fname,'/');
    }
    if (p) {
      *p = 0;
      dir = fname;
      fname = p+1;      
    }

    if (dir && *dir) {
      if (getcwd(dbuf,MAXPATHLEN-1) == NULL) {
	fprintf(stderr,"getwd : %s\n",strerror(errno));
	exit(1);
      }
      if (chdir(dir) != 0) {
	fprintf(stderr,"chdir %s : %s\n",dir,strerror(errno));
	continue;
      }
      flist_dir = dir;
      send_file_name(f,flist,recurse,fname);
      flist_dir = NULL;
      if (chdir(dbuf) != 0) {
	fprintf(stderr,"chdir %s : %s\n",dbuf,strerror(errno));
	exit(1);
      }
      continue;
    }

    send_file_name(f,flist,recurse,fname);
  }

  write_int(f,0);
  write_flush(f);
  return flist;
}



struct file_list *recv_file_list(int f)
{
  int l;
  struct file_list *flist;
  int malloc_count=0;

  if (verbose > 2)
    fprintf(stderr,"recv_file_list starting\n");

  flist = (struct file_list *)malloc(sizeof(flist[0]));
  if (!flist)
    goto oom;

  malloc_count=100;
  flist->files = (struct file_struct *)malloc(sizeof(flist->files[0])*
					      malloc_count);
  if (!flist->files)
    goto oom;

  flist->count=0;

  for (l=read_int(f); l; l=read_int(f)) {
    int i = flist->count;

    if (i >= malloc_count) {
      malloc_count += 100;
      flist->files =(struct file_struct *)realloc(flist->files,
						  sizeof(flist->files[0])*
						  malloc_count);
      if (!flist->files)
	goto oom;
    }

    flist->files[i].name = (char *)malloc(l+1);
    if (!flist->files[i].name) 
      goto oom;

    read_buf(f,flist->files[i].name,l);
    flist->files[i].name[l] = 0;
    flist->files[i].modtime = (time_t)read_int(f);
    flist->files[i].length = (off_t)read_int(f);
    flist->files[i].mode = (mode_t)read_int(f);
    if (preserve_uid)
      flist->files[i].uid = (uid_t)read_int(f);
    if (preserve_gid)
      flist->files[i].gid = (gid_t)read_int(f);
    if (preserve_devices)
      flist->files[i].dev = (dev_t)read_int(f);

#if SUPPORT_LINKS
    if (preserve_links && S_ISLNK(flist->files[i].mode)) {
      int l = read_int(f);
      flist->files[i].link = (char *)malloc(l+1);
      read_buf(f,flist->files[i].link,l);
      flist->files[i].link[l] = 0;
    }
#endif

    if (always_checksum)
      read_buf(f,flist->files[i].sum,SUM_LENGTH);

    if (S_ISREG(flist->files[i].mode))
      total_size += flist->files[i].length;

    flist->count++;

    if (verbose > 2)
      fprintf(stderr,"recv_file_name(%s)\n",flist->files[i].name);
  }


  if (verbose > 2)
    fprintf(stderr,"received %d names\n",flist->count);

  return flist;

oom:
    out_of_memory("recv_file_list");
    return NULL; /* not reached */
}

