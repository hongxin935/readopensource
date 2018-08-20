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

int verbose = 0;
int always_checksum = 0;
time_t starttime;
off_t total_size = 0;
int block_size=BLOCK_SIZE;

char *backup_suffix = BACKUP_SUFFIX;

int make_backups = 0;
int preserve_links = 0;
int preserve_perms = 0;
int preserve_devices = 0;
int preserve_uid = 0;
int preserve_gid = 0;
int preserve_times = 0;
int update_only = 0;

static int server = 0;
static int sender = 0;
static int recurse = 0;


static void report(int f)
{
  int in,out,tsize;
  time_t t = time(NULL);
  
  if (!verbose) return;

  if (server && sender) {
    write_int(f,read_total());
    write_int(f,write_total());
    write_int(f,total_size);
    write_flush(f);
    return;
  }
    
  if (sender) {
    in = read_total();
    out = write_total();
    tsize = (int)total_size;
  } else {
    in = read_int(f);
    out = read_int(f);
    tsize = read_int(f);
  }

  printf("wrote %d bytes  read %d bytes  %g bytes/sec\n",
	 out,in,(in+out)/(0.5 + (t-starttime)));        
  printf("total size is %d  speedup is %g\n",
	 tsize,(1.0*tsize)/(in+out));
}


int do_cmd(char *cmd,char *machine,char *user,char *path,int *f_in,int *f_out)
{
  char *args[100];
  int i,x,argc=0;
  char *tok,*p;
  char argstr[30]="-s";
  char bsize[30];

  if (!cmd)
    cmd = getenv(RSYNC_RSH_ENV);
  if (!cmd)
    cmd = RSYNC_RSH;
  cmd = strdup(cmd);
  if (!cmd) 
    goto oom;

  for (tok=strtok(cmd," ");tok;tok=strtok(NULL," ")) {
    args[argc++] = tok;
  }

  if (user) {
    args[argc++] = "-l";
    args[argc++] = user;
  }
  args[argc++] = machine;

  args[argc++] = RSYNC_NAME;

  x = 2;
  for (i=0;i<verbose;i++)
    argstr[x++] = 'v';
  if (!sender)
    argstr[x++] = 'S';
  if (make_backups)
    argstr[x++] = 'b';
  if (update_only)
    argstr[x++] = 'u';
  if (preserve_links)
    argstr[x++] = 'l';
  if (preserve_uid)
    argstr[x++] = 'o';
  if (preserve_gid)
    argstr[x++] = 'g';
  if (preserve_devices)
    argstr[x++] = 'D';
  if (preserve_times)
    argstr[x++] = 't';
  if (preserve_perms)
    argstr[x++] = 'p';
  if (recurse)
    argstr[x++] = 'r';
  if (always_checksum)
    argstr[x++] = 'c';
  argstr[x] = 0;

  args[argc++] = argstr;

  if (block_size != BLOCK_SIZE) {
    sprintf(bsize,"-B%d",block_size);
    args[argc++] = bsize;
  }    
  
  if (path && *path) {
    char *dir = strdup(path);
    p = strrchr(dir,'/');
    if (p) {
      *p = 0;
      args[argc++] = dir;
      p++;
    } else {
      args[argc++] = ".";
      p = dir;
    }
    if (p[0])
      args[argc++] = path;
  }

  args[argc] = NULL;

  if (verbose > 3) {
    fprintf(stderr,"cmd=");
    for (i=0;i<argc;i++)
      fprintf(stderr,"%s ",args[i]);
    fprintf(stderr,"\n");
  }

  return piped_child(args,f_in,f_out);

oom:
  out_of_memory("do_cmd");
  return 0; /* not reached */
}


void do_server_sender(int argc,char *argv[])
{
  int i;
  char *dir = argv[0];
  struct file_list *flist;

  if (verbose > 2)
    fprintf(stderr,"server_sender starting pid=%d\n",(int)getpid());
  
  if (chdir(dir) != 0) {
    fprintf(stderr,"chdir %s: %s\n",dir,strerror(errno));
    exit(1);
  }
  argc--;
  argv++;
  
  if (strcmp(dir,".")) {
    int l = strlen(dir);
    for (i=0;i<argc;i++)
      argv[i] += l+1;
  }

  if (argc == 0 && recurse) {
    argc=1;
    argv--;
    argv[0] = ".";
  }
    

  flist = send_file_list(STDOUT_FILENO,recurse,argc,argv);
  send_files(flist,STDOUT_FILENO,STDIN_FILENO);
  report(STDOUT_FILENO);
  exit(0);
}



void do_server_recv(int argc,char *argv[])
{
  int pid,i,status;
  char *dir = NULL;
  struct file_list *flist;
  char *fname=NULL;
  struct stat st;
  
  if (verbose > 2)
    fprintf(stderr,"server_recv(%d) starting pid=%d\n",argc,(int)getpid());

  if (argc > 0) {
    dir = argv[0];
    argc--;
    argv++;
  }

  if (argc > 0) {
    fname = argv[0];
    dir = NULL;

    if (stat(fname,&st) != 0) {
      if (!recurse || mkdir(fname,0777) != 0) {
	fprintf(stderr,"stat %s : %s\n",fname,strerror(errno));
	exit(1);
      }
      stat(fname,&st);
    }

    if (S_ISDIR(st.st_mode)) {
      dir = fname;
      fname = NULL;
    }
  }

  if (dir) {
    if (chdir(dir) != 0) {
      fprintf(stderr,"chdir %s : %s\n",dir,strerror(errno));
      exit(1);
    } else {
      if (verbose > 2) {
	fprintf(stderr,"server changed to directory %s\n",dir);
      }
    }
  }

  flist = recv_file_list(STDIN_FILENO);
  if (!flist) {
    fprintf(stderr,"nothing to do\n");
    exit(1);
  }

  if ((pid=fork()) == 0) {
    if (verbose > 2)
      fprintf(stderr,"generator starting pid=%d count=%d\n",
	      (int)getpid(),flist->count);

    for (i = 0; i < flist->count; i++) {
      if (S_ISDIR(flist->files[i].mode)) {
	if (mkdir(flist->files[i].name,flist->files[i].mode) != 0 && 
	    errno != EEXIST) {	 
	  fprintf(stderr,"mkdir %s: %s\n",
		  flist->files[i].name,strerror(errno));
	  exit(1);
	}
	continue;
      }
      fname = flist->files[i].name;
      if (flist->count == 1 &&
	  argc > 0)
	fname = argv[0];
      recv_generator(fname,flist,i,STDOUT_FILENO);
    }
    write_int(STDOUT_FILENO,-1);
    write_flush(STDOUT_FILENO);
    if (verbose > 1)
      fprintf(stderr,"generator wrote %d\n",write_total());
    exit(0);
  }

  recv_files(STDIN_FILENO,flist,fname);
  if (verbose > 1)
    fprintf(stderr,"receiver read %d\n",read_total());
  waitpid(pid, &status, 0);
  exit(status);
}


void usage(void)
{
  fprintf(stderr,"rsync version %s Copyright Andrew Tridgell and Paul Mackerras\n\n",VERSION);
  fprintf(stderr,"Usage:\t%s [options] src user@host:dest\nOR",RSYNC_NAME);
  fprintf(stderr,"\t%s [options] user@host:src dest\n\n",RSYNC_NAME);
  fprintf(stderr,"Options:\n");
  fprintf(stderr,"-v       : increase verbosity\n");
  fprintf(stderr,"-c       : always checksum\n");
  fprintf(stderr,"-a       : archive mode (same as -rlptDog)\n");
  fprintf(stderr,"-r       : recurse into directories\n");
  fprintf(stderr,"-b       : make backups (default ~ extension)\n");
  fprintf(stderr,"-u       : update only (don't overwrite newer files)\n");
  fprintf(stderr,"-l       : preserve soft links\n");
  fprintf(stderr,"-p       : preserve permissions\n");
  fprintf(stderr,"-o       : preserve owner (root only)\n");
  fprintf(stderr,"-g       : preserve group\n");
  fprintf(stderr,"-D       : preserve devices (root only)\n");
  fprintf(stderr,"-t       : preserve times\n");  
  fprintf(stderr,"-e cmd   : specify rsh replacement\n");
}


int main(int argc,char *argv[])
{
    int i, pid, status, pid2, status2;
    int opt;
    extern char *optarg;
    extern int optind;
    char *shell_cmd = NULL;
    char *shell_machine = NULL;
    char *shell_path = NULL;
    char *shell_user = NULL;
    char *p;
    int f_in,f_out;
    struct file_list *flist;
    char *local_name = NULL;

    starttime = time(NULL);

    while ((opt=getopt(argc, argv, "oblpguDtcahvSsre:B:")) != EOF)
      switch (opt) 
	{
	case 'h':
	  usage();
	  exit(0);

	case 'b':
	  make_backups=1;
	  break;

	case 'u':
	  update_only=1;
	  break;

#if SUPPORT_LINKS
	case 'l':
	  preserve_links=1;
	  break;
#endif

	case 'p':
	  preserve_perms=1;
	  break;

	case 'o':
	  if (getuid() == 0) {
	    preserve_uid=1;
	  } else {
	    fprintf(stderr,"-o only allowed for root\n");
	    exit(1);
	  }
	  break;

	case 'g':
	  preserve_gid=1;
	  break;

	case 'D':
	  if (getuid() == 0) {
	    preserve_devices=1;
	  } else {
	    fprintf(stderr,"-D only allowed for root\n");
	    exit(1);
	  }
	  break;

	case 't':
	  preserve_times=1;
	  break;

	case 'c':
	  always_checksum=1;
	  break;

	case 'v':
	  verbose++;
	  break;

	case 'a':
	  recurse=1;
#if SUPPORT_LINKS
	  preserve_links=1;
#endif
	  preserve_perms=1;
	  preserve_times=1;
	  preserve_gid=1;
	  if (getuid() == 0) {
	    preserve_devices=1;
	    preserve_uid=1;
	  }	    
	  break;

	case 's':
	  server = 1;
	  break;

	case 'S':
	  if (!server) {
	    usage();
	    exit(1);
	  }
	  sender = 1;
	  break;

	case 'r':
	  recurse = 1;
	  break;

	case 'e':
	  shell_cmd = optarg;
	  break;

	case 'B':
	  block_size = atoi(optarg);
	  break;

	default:
	  fprintf(stderr,"bad option -%c\n",opt);
	  exit(1);
	}

    while (optind--) {
      argc--;
      argv++;
    }

    if (server) {
      int version = read_int(STDIN_FILENO);
      if (version != PROTOCOL_VERSION) {
	fprintf(stderr,"protocol version mismatch %d %d\n",
		version,PROTOCOL_VERSION);
	exit(1);
      }
      write_int(STDOUT_FILENO,PROTOCOL_VERSION);
      write_flush(STDOUT_FILENO);
	
      if (sender)
	do_server_sender(argc,argv);
      else
	do_server_recv(argc,argv);
      exit(0);
    }

    if (argc < 2) {
      usage();
      exit(1);
    }

    p = strchr(argv[0],':');

    if (p) {
      sender = 0;
      *p = 0;
      shell_machine = argv[0];
      shell_path = p+1;
      argc--;
      argv++;
    } else {
      sender = 1;

      p = strchr(argv[argc-1],':');
      if (!p) {
	usage();
	exit(1);
      }

      *p = 0;
      shell_machine = argv[argc-1];
      shell_path = p+1;
      argc--;
    }

    p = strchr(shell_machine,'@');
    if (p) {
      *p = 0;
      shell_user = shell_machine;
      shell_machine = p+1;
    }

    if (verbose > 3) {
      fprintf(stderr,"cmd=%s machine=%s user=%s path=%s\n",
	      shell_cmd?shell_cmd:"",
	      shell_machine?shell_machine:"",
	      shell_user?shell_user:"",
	      shell_path?shell_path:"");
    }
    
    signal(SIGCHLD,SIG_IGN);

    if (!sender && argc != 1) {
      usage();
      exit(1);
    }

    pid = do_cmd(shell_cmd,shell_machine,shell_user,shell_path,&f_in,&f_out);

    write_int(f_out,PROTOCOL_VERSION);
    write_flush(f_out);
    {
      int version = read_int(f_in);
      if (version != PROTOCOL_VERSION) {
	fprintf(stderr,"protocol version mismatch\n");
	exit(1);
      }	
    }

    if (verbose > 3) 
      fprintf(stderr,"parent=%d child=%d sender=%d recurse=%d\n",
	      (int)getpid(),pid,sender,recurse);

    if (sender) {
      flist = send_file_list(f_out,recurse,argc,argv);
      if (verbose > 3) 
	fprintf(stderr,"file list sent\n");
      send_files(flist,f_out,f_in);
      if (verbose > 3)
	fprintf(stderr,"waiting on %d\n",pid);
      waitpid(pid, &status, 0);
      report(-1);
      exit(status);
    }

    flist = recv_file_list(f_in);
    if (flist->count == 0) {
      exit(0);
    }

    {
      struct stat st;
      if (stat(argv[0],&st) != 0) {
	if (mkdir(argv[0],0777) != 0) {
	  fprintf(stderr,"mkdir %s : %s\n",argv[0],strerror(errno));
	  exit(1);
	}
	stat(argv[0],&st);
      }
      
      if (S_ISDIR(st.st_mode)) {
	if (chdir(argv[0]) != 0) {
	  fprintf(stderr,"chdir %s : %s\n",argv[0],strerror(errno));
	  exit(1);
	}
      } else {
	local_name = argv[0];
      }
    }

    if ((pid2=fork()) == 0) {
      for (i = 0; i < flist->count; i++) {
	if (S_ISDIR(flist->files[i].mode)) {
	  if (mkdir(flist->files[i].name,flist->files[i].mode) != 0 &&
	      errno != EEXIST) {
	    fprintf(stderr,"mkdir %s : %s\n",
		    flist->files[i].name,strerror(errno));
	  }
	  continue;
	}
	recv_generator(local_name?local_name:flist->files[i].name,
		       flist,i,f_out);
      }
      write_int(f_out,-1);
      write_flush(f_out);
      if (verbose > 1)
	fprintf(stderr,"generator wrote %d\n",write_total());
      exit(0);
    }

    recv_files(f_in,flist,local_name);
    report(f_in);
    if (verbose > 1)
      fprintf(stderr,"receiver read %d\n",read_total());
    waitpid(pid, &status, 0);
    waitpid(pid2, &status2, 0);

    return status | status2;
}
