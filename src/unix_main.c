/* unix_misc.c -- Miscellaneous functions for Unix
   Copyright (C) 1993, 1994 John Harper <john@dcs.warwick.ac.uk>
   $Id$

   This file is part of Jade.

   Jade is free software; you can redistribute it and/or modify it
   under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2, or (at your option)
   any later version.

   Jade is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with Jade; see the file COPYING.	If not, write to
   the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.  */

#include "jade.h"
#include "jade_protos.h"

#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <pwd.h>
#include <netdb.h>

#ifdef HAVE_STRERROR
# include <errno.h>
#else
  extern int sys_nerr, errno;
  extern char *sys_errlist[];
#endif

#ifdef ENVIRON_UNDECLARED
  extern char **environ;
#endif

_PR bool file_exists(u_char *);
_PR u_long file_mod_time(u_char *);
_PR void sys_misc_init(void);

_PR bool same_files(u_char *, u_char *);
_PR u_char * file_part(u_char *);
_PR VALUE lookup_errno(void);
_PR void doconmsg(u_char *);
_PR VALUE read_file(u_char *);
_PR u_long sys_time(void);
_PR int add_file_part(u_char *, const u_char *, int);
_PR VALUE sys_expand_file_name(VALUE);
_PR VALUE sys_fully_qualify_file_name(VALUE);

bool
same_files(u_char *file1, u_char *file2)
{
    bool rc = FALSE;
    struct stat stat1, stat2;
    if(!stat(file1, &stat1))
    {
	if(!stat(file2, &stat2))
	{
	    if((stat1.st_dev == stat2.st_dev)
	       && (stat1.st_ino == stat2.st_ino))
	    {
		rc = TRUE;
	    }
	}
    }
    else
	rc = !strcmp(file1, file2);
    return(rc);
}

u_char *
file_part(u_char *fname)
{
    u_char *tmp = strrchr(fname, '/');
    if(tmp)
	return(tmp + 1);
    return(fname);
}

VALUE
lookup_errno(void)
{
#ifdef HAVE_STRERROR
    return(string_dup(strerror(errno)));
#else
    if(errno >= sys_nerr)
        return(string_dup(sys_errlist[errno]));
    else
        return(MKSTR("<error>"));
#endif
}

void
doconmsg(u_char *msg)
{
    fputs(msg, stderr);
}

VALUE
read_file(u_char *fileName)
{
    FILE *fh = fopen(fileName, "r");
    if(fh)
    {
	struct stat stat;
	if(!fstat(fileno(fh), &stat))
	{
	    VALUE mem = make_string(stat.st_size + 1);
	    if(mem)
	    {
		fread(VSTR(mem), 1, stat.st_size, fh);
		VSTR(mem)[stat.st_size] = 0;
		fclose(fh);
		return(mem);
	    }
	    else
		mem_error();
	}
	fclose(fh);
    }
    return(cmd_signal(sym_file_error,
		      list_2(lookup_errno(), string_dup(fileName))));
}

u_long
sys_time(void)
{
    return(time(NULL));
}

int
add_file_part(u_char *buf, const u_char *part, int bufLen)
{
    int bufend = strlen(buf);
    int partlen = strlen(part);
    if((bufend > 0) && (buf[bufend-1] != '/') && (*part != '/'))
    {
	if(++bufend >= bufLen)
	    return(FALSE);
	buf[bufend-1] = '/';
	buf[bufend] = 0;
    }
    if((bufend + partlen) >= bufLen)
	return(FALSE);
    strcpy(buf + bufend, part);
    return(TRUE);
}

_PR VALUE cmd_delete_file(VALUE file);
DEFUN_INT("delete-file", cmd_delete_file, subr_delete_file, (VALUE file), V_Subr1, DOC_delete_file, "fDelete file:") /*
::doc:delete_file::
delete-file FILE-NAME

Attempts to delete the file called FILE-NAME.
::end:: */
{
    DECLARE1(file, STRINGP);
    if(!unlink(VSTR(file)))
	return(sym_t);
    return(signal_file_error(file));
}

_PR VALUE cmd_rename_file(VALUE src, VALUE dst);
DEFUN_INT("rename-file", cmd_rename_file, subr_rename_file, (VALUE src, VALUE dst), V_Subr2, DOC_rename_file, "fRename file:\nFRename file `%s' as:") /*
::doc:rename_file::
rename-file SRC DEST

Tries to rename the file SRC as DEST, this doesn't work across filesystems, or
if a file DEST already exists.
::end:: */
{
    DECLARE1(src, STRINGP);
    DECLARE2(dst, STRINGP);
    if(!rename(VSTR(src), VSTR(dst)))
	return(sym_t);
    return(signal_file_error(list_2(src, dst)));
}

_PR VALUE cmd_copy_file(VALUE src, VALUE dst);
DEFUN_INT("copy-file", cmd_copy_file, subr_copy_file, (VALUE src, VALUE dst), V_Subr2, DOC_copy_file, "fCopy file:\nFCopy file `%s' to:") /*
::doc:copy_file::
copy-file SRC DEST

Copies the file called SRC to the file DEST.
::end:: */
{
    VALUE res = sym_t;
    int srcf;
    DECLARE1(src, STRINGP);
    DECLARE2(dst, STRINGP);
    srcf = open(VSTR(src), O_RDONLY);
    if(srcf != -1)
    {
	int dstf = open(VSTR(dst), O_WRONLY | O_CREAT | O_TRUNC, 0666);
	if(dstf != -1)
	{
	    struct stat statb;
	    int rd;
	    if(fstat(srcf, &statb) == 0)
		chmod(VSTR(dst), statb.st_mode);
	    do {
		u_char buf[BUFSIZ];
		int wr;
		rd = read(srcf, buf, BUFSIZ);
		if(rd < 0)
		{
		    res = signal_file_error(src);
		    break;
		}
		wr = write(dstf, buf, rd);
		if(wr != rd)
		{
		    res = signal_file_error(dst);
		    break;
		}
	    } while(rd != 0);
	    close(dstf);
	}
	else
	    res = signal_file_error(dst);
	close(srcf);
    }
    else
	res = signal_file_error(src);
    return(res);
}

_PR VALUE cmd_file_readable_p(VALUE file);
DEFUN("file-readable-p", cmd_file_readable_p, subr_file_readable_p, (VALUE file), V_Subr1, DOC_file_readable_p) /*
::doc:file_readable_p::
file-readable-p FILE

Returns t if FILE available for reading from.
::end:: */
{
    DECLARE1(file, STRINGP);
    if(!access(VSTR(file), R_OK))
	return(sym_t);
    return(sym_nil);
}

_PR VALUE cmd_file_writable_p(VALUE file);
DEFUN("file-writable-p", cmd_file_writable_p, subr_file_writable_p, (VALUE file), V_Subr1, DOC_file_writeable_p) /*
::doc:file_writeable_p::
file-writable-p FILE

Returns t if FILE available for writing to.
::end:: */
{
    DECLARE1(file, STRINGP);
    if(!access(VSTR(file), W_OK))
	return(sym_t);
    return(sym_nil);
}

_PR VALUE cmd_file_exists_p(VALUE file);
DEFUN("file-exists-p", cmd_file_exists_p, subr_file_exists_p, (VALUE file), V_Subr1, DOC_file_exists_p) /*
::doc:file_exists_p::
file-exists-p FILE

Returns t if FILE exists.
::end:: */
{
    DECLARE1(file, STRINGP);
    if(!access(VSTR(file), F_OK))
	return(sym_t);
    return(sym_nil);
}
bool
file_exists(u_char *fileName)
{
    if(!access(fileName, F_OK))
    {
	struct stat statb;
	if(!stat(fileName, &statb) && !S_ISDIR(statb.st_mode))
	    return(TRUE);
    }
    return(FALSE);
}

_PR VALUE cmd_file_regular_p(VALUE file);
DEFUN("file-regular-p", cmd_file_regular_p, subr_file_regular_p, (VALUE file), V_Subr1, DOC_file_regular_p) /*
::doc:file_regular_p::
file-regular-p FILE

Returns t if FILE is a ``normal'' file, ie, not a directory, device, symbolic
link, etc...
::end:: */
{
    struct stat statb;
    DECLARE1(file, STRINGP);
    if(!stat(VSTR(file), &statb))
    {
	if(S_ISREG(statb.st_mode))
	    return(sym_t);
    }
    return(sym_nil);
}

_PR VALUE cmd_file_directory_p(VALUE file);
DEFUN("file-directory-p", cmd_file_directory_p, subr_file_directory_p, (VALUE file), V_Subr1, DOC_file_directory_p) /*
::doc:file_directory_p::
file-directory-p FILE

Returns t if FILE is a directory.
::end:: */
{
    struct stat statb;
    DECLARE1(file, STRINGP);
    if(!stat(VSTR(file), &statb))
    {
	if(S_ISDIR(statb.st_mode))
	    return(sym_t);
    }
    return(sym_nil);
}

_PR VALUE cmd_file_symlink_p(VALUE file);
DEFUN("file-symlink-p", cmd_file_symlink_p, subr_file_symlink_p, (VALUE file), V_Subr1, DOC_file_symlink_p) /*
::doc:file_symlink_p::
file-symlink-p FILE

Returns t if FILE is a symbolic link to another file.
::end:: */
{
    struct stat statb;
    DECLARE1(file, STRINGP);
    if(!stat(VSTR(file), &statb))
    {
	if(S_ISLNK(statb.st_mode))
	    return(sym_t);
    }
    return(sym_nil);
}

_PR VALUE cmd_file_owner_p(VALUE file);
DEFUN("file-owner-p", cmd_file_owner_p, subr_file_owner_p, (VALUE file), V_Subr1, DOC_file_owner_p) /*
::doc:file_owner_p::
file-owner-p FILE

Returns t if the ownership (uid & gid) of file FILE (a string) is the same
as that of any files written by the editor.
::end:: */
{
    struct stat statb;
    DECLARE1(file, STRINGP);
    if(!stat(VSTR(file), &statb))
    {
	if((statb.st_uid == geteuid()) && (statb.st_gid == getegid()))
	    return(sym_t);
    }
    return(sym_nil);
}

_PR VALUE cmd_file_nlinks(VALUE file);
DEFUN("file-nlinks", cmd_file_nlinks, subr_file_nlinks, (VALUE file), V_Subr1, DOC_file_nlinks) /*
::doc:file_nlinks::
file-nlinks FILE

Returns the number of links pointing to the file called FILE. This will be
one if FILE has only one name. Doesn't count symbolic links.
::end:: */
{
    struct stat statb;
    DECLARE1(file, STRINGP);
    if(!stat(VSTR(file), &statb))
	return(make_number(statb.st_nlink));
    return(sym_nil);
}

_PR VALUE cmd_file_modes(VALUE file);
DEFUN("file-modes", cmd_file_modes, subr_file_modes, (VALUE file), V_Subr1, DOC_file_modes) /*
::doc:file_modes::
file-modes FILE

Return the access permissions of the file called FILE, an integer. Note that
the format of this integer is not defined, it differs from system to system.
::end:: */
{
    struct stat statb;
    DECLARE1(file, STRINGP);
    if(!stat(VSTR(file), &statb))
	return(make_number(statb.st_mode));
    return(sym_nil);
}

_PR VALUE cmd_set_file_modes(VALUE file, VALUE modes);
DEFUN("set-file-modes", cmd_set_file_modes, subr_set_file_modes, (VALUE file, VALUE modes), V_Subr2, DOC_set_file_modes) /*
::doc:set_file_modes::
set-file-modes FILE MODES

Sets the access permissions of FILE to MODES, an integer. The only real way
you can get this integer is from `file-modes' since it changes from system
to system.
::end:: */
{
    DECLARE1(file, STRINGP);
    DECLARE2(modes, NUMBERP);
    if(chmod(VSTR(file), VNUM(modes)) == 0)
	return(modes);
    return(signal_file_error(file));
}

u_long
file_mod_time(u_char *file)
{
    struct stat statb;
    if(!stat(file, &statb))
	return(statb.st_mtime);
    return(0);
}
_PR VALUE cmd_file_modtime(VALUE file);
DEFUN("file-modtime", cmd_file_modtime, subr_file_modtime, (VALUE file), V_Subr1, DOC_file_modtime) /*
::doc:file_modtime::
file-modtime FILE

Return the time (an integer) that FILE was last modified.
::end:: */
{
    DECLARE1(file, STRINGP);
    return(make_number(file_mod_time(VSTR(file))));
}

_PR VALUE cmd_directory_files(VALUE dirname);
DEFUN("directory-files", cmd_directory_files, subr_directory_files, (VALUE dirname), V_Subr1, DOC_directory_files) /*
::doc:directory_files::
directory-files DIRECTORY

Returns a list of the names of all files in directory DIRECTORY, directories
in DIRECTORY have a `/' character appended to their name.
::end:: */
{
    DIR *dir;
    u_char *dname;
    DECLARE1(dirname, STRINGP);
    dname = VSTR(dirname);
    if(*dname == 0)
	dname = ".";
    dir = opendir(dname);
    if(dir)
    {
	VALUE list = sym_nil;
	struct dirent *de;
	while((de = readdir(dir)))
	{
	    VALUE name;
	    if(!((name = string_dup(de->d_name)) && (list = cmd_cons(name, list))))
	    {
		mem_error();
		closedir(dir);
		return(NULL);
	    }
	}
	closedir(dir);
	return(list);
    }
    return(cmd_signal(sym_file_error, list_2(lookup_errno(), dirname)));
}

_PR VALUE cmd_user_login_name(void);
DEFUN("user-login-name", cmd_user_login_name, subr_user_login_name, (void), V_Subr0, DOC_user_login_name) /*
::doc:user_login_name::
user-login-name

Returns the login name of the user (a string).
On the Amiga this is taken from the environment variable `USERNAME'.
::end:: */
{
    /* Just look this up once, then use the saved copy.	 */
    static VALUE user_login_name;
    char *tmp;
    if(user_login_name)
	return(user_login_name);
    if(!(tmp = getlogin()))
    {
	struct passwd *pwd;
	if(!(pwd = getpwuid(geteuid())))
	    return(NULL);
	tmp = pwd->pw_name;
    }
    user_login_name = string_dup(tmp);
    mark_static(&user_login_name);
    return(user_login_name);
}

_PR VALUE cmd_user_full_name(void);
DEFUN("user-full-name", cmd_user_full_name, subr_user_full_name, (void), V_Subr0, DOC_user_full_name) /*
::doc:user_full_name::
user-full-name

Returns the real name of the user (a string).
On the Amiga this is taken from the environment variable `REALNAME'.
::end:: */
{
    struct passwd *pwd;
    static VALUE user_full_name;
    if(user_full_name)
	return(user_full_name);
    if(!(pwd = getpwuid(geteuid())))
	return(NULL);
#ifndef FULL_NAME_TERMINATOR
    user_full_name = string_dup(pwd->pw_gecos);
#else
    {
	char *end = strchr(pwd->pw_gecos, FULL_NAME_TERMINATOR);
	if(end)
	    user_full_name = string_dupn(pwd->pw_gecos, end - pwd->pw_gecos);
	else
	    user_full_name = string_dup(pwd->pw_gecos);
    }
#endif
    mark_static(&user_full_name);
    return(user_full_name);
}

_PR VALUE cmd_user_home_directory(void);
DEFUN("user-home-directory", cmd_user_home_directory, subr_user_home_directory, (void), V_Subr0, DOC_user_home_directory) /*
::doc:user_home_directory::
user-home-directory

Returns the user's home directory (a string). It will be terminated by a slash
(or whatever is appropriate) so that it can be glued together with a file name
making a valid path name.

The first place we look for the directory name is the `HOME' environment
variable. If this doesn't exist we either use the `SYS:' assignment when
on AmigaDOS or look in the /etc/passwd file if on Unix.
::end:: */
{
    static VALUE user_home_directory;
    if(user_home_directory)
	return(user_home_directory);
    else
    {
	char *src;
	int len;
	src = getenv("HOME");
	if(!src)
	{
	    struct passwd *pwd;
	    pwd = getpwuid(geteuid());
	    if(!pwd || !pwd->pw_dir)
	    {
		return(cmd_signal(sym_error, LIST_1(MKSTR("Can't find your home directory"))));
	    }
	    src = pwd->pw_dir;
	}
	len = strlen(src);
	if(src[len] != '/')
	{
	    user_home_directory = string_dupn(src, len + 1);
	    VSTR(user_home_directory)[len] = '/';
	    VSTR(user_home_directory)[len+1] = 0;
	}
	else
	    user_home_directory = string_dup(src);
	mark_static(&user_home_directory);
	return(user_home_directory);
    }
}

_PR VALUE cmd_system_name(void);
DEFUN("system-name", cmd_system_name, subr_system_name, (void), V_Subr0,  DOC_system_name) /*
::doc:system_name::
system-name

Returns the name of the host which the editor is running on.
On the Amiga this is taken from the environment variable `HOSTNAME'.
::end:: */
{
    u_char buf[128];
    struct hostent *h;
    static VALUE system_name;
    if(system_name)
	return(system_name);
    if(gethostname(buf, 128))
	return(NULL);
    h = gethostbyname(buf);
    if(h)
	system_name = string_dup((u_char *)h->h_name);
    else
	system_name = string_dup(buf);
    mark_static(&system_name);
    return(system_name);
}

_PR VALUE cmd_setenv(VALUE name, VALUE val);
DEFUN("setenv", cmd_setenv, subr_setenv, (VALUE name, VALUE val), V_Subr2, DOC_setenv) /*
::doc:setenv::
setenv VARIABLE [VALUE]

Sets the value of the environment variable called VARIABLE (a string) to
the string VALUE. If VALUE is undefined or nil the variable is removed from
the environment.
::end:: */
{
    /* This function was adapted from the GNU libc putenv() function,
       hopefully it's portable... 
         It's main problem is that it doesn't know whether or not to
       unallocate the strings making up the environment when they're
       finished with.  */
    char **ep;
    int name_len;
    DECLARE1(name, STRINGP);
    name_len = STRING_LEN(name);
    if(!STRINGP(val))
    {
	/* remove the variable NAME */
	for(ep = environ; *ep != NULL; ep++)
	{
	    if((strncmp(VSTR(name), *ep, name_len) == 0)
	       && ((*ep)[name_len] == '='))
	    {
		while(ep[1] != NULL)
		{
		    ep[0] = ep[1];
		    ep++;
		}
		*ep = NULL;
		return(sym_t);
	    }
	}
	return(sym_nil);
    }
    else
    {
	/* setting NAME to VAL */
	int size;
	char *env_string = str_alloc(name_len + 1 + STRING_LEN(val));
	if(!env_string)
	    return(mem_error());
	memcpy(env_string, VSTR(name), name_len);
	env_string[name_len] = '=';
	strcpy(env_string + name_len + 1, VSTR(val));
	for(ep = environ, size = 0; *ep != NULL; ep++)
	{
	    if((strncmp(VSTR(name), *ep, name_len) == 0)
	       && ((*ep)[name_len] == '='))
		break;
	    size++;
	}
	if(*ep == NULL)
	{
	    /* NAME not in list, create new entry */
	    static char **last_environ = NULL;
	    char **new_environ = str_alloc((size + 2) * sizeof(char *));
	    if(!new_environ)
		return(mem_error());
	    memcpy(new_environ, environ, size * sizeof(char *));
	    new_environ[size] = env_string;
	    new_environ[size + 1] = NULL;
	    if(last_environ != NULL)
		str_free(last_environ);
	    last_environ = environ = new_environ;
	}
	else
	{
	    /* adjust existing entry */
	    *ep = env_string;
	}
	return(val);
    }
}

VALUE
sys_expand_file_name(VALUE namev)
{
    u_char *name = VSTR(namev);
    if(*name == '~')
    {
	VALUE home = cmd_user_home_directory();
	if(!home || !STRINGP(home))
	    return(NULL);
	switch(name[1])
	{
	case 0:
	    return(home);
	case '/':
	    {
		u_char buf[512];
		strcpy(buf, VSTR(home));
		if(!add_file_part(buf, name + 2, 512))
		    return(NULL);
		return(string_dup(buf));
	    }
	default:
	    return(cmd_signal(sym_file_error,
			      list_2(MKSTR("Can't expand"), namev)));
	}
    }
    return(namev);
}

VALUE
sys_fully_qualify_file_name(VALUE name)
{
    u_char buf[512];
    if(*VSTR(name) == '/')
	/* starting from root. */
	return(name);
    if(getcwd(buf, 512) != NULL)
    {
	if(add_file_part(buf, VSTR(name), 512))
	    return(string_dup(buf));
    }
    return(NULL);
}

void
sys_misc_init(void)
{
    ADD_SUBR(subr_delete_file);
    ADD_SUBR(subr_rename_file);
    ADD_SUBR(subr_copy_file);
    ADD_SUBR(subr_file_readable_p);
    ADD_SUBR(subr_file_writable_p);
    ADD_SUBR(subr_file_exists_p);
    ADD_SUBR(subr_file_regular_p);
    ADD_SUBR(subr_file_directory_p);
    ADD_SUBR(subr_file_symlink_p);
    ADD_SUBR(subr_file_owner_p);
    ADD_SUBR(subr_file_nlinks);
    ADD_SUBR(subr_file_modes);
    ADD_SUBR(subr_set_file_modes);
    ADD_SUBR(subr_file_modtime);
    ADD_SUBR(subr_directory_files);
    ADD_SUBR(subr_user_login_name);
    ADD_SUBR(subr_user_full_name);
    ADD_SUBR(subr_user_home_directory);
    ADD_SUBR(subr_system_name);
    ADD_SUBR(subr_setenv);
}
