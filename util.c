/*
   Copyright (C) Andrew Tridgell 2002

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

#include "ccache.h"

static FILE *logfile;

/* log a message to the CCACHE_LOGFILE location */
void cc_log(const char *format, ...)
{
	va_list ap;
	extern char *cache_logfile;

	if (!cache_logfile) return;

	if (!logfile) logfile = fopen(cache_logfile, "a");
	if (!logfile) return;

	va_start(ap, format);
	vfprintf(logfile, format, ap);
	va_end(ap);
	fflush(logfile);
}

/* something went badly wrong! */
void fatal(const char *msg)
{
	cc_log("FATAL: %s\n", msg);
	exit(1);
}

#ifndef ENABLE_ZLIB
/* copy all data from one file descriptor to another */
void copy_fd(int fd_in, int fd_out)
{
	char buf[10240];
	int n;

	while ((n = read(fd_in, buf, sizeof(buf))) > 0) {
		if (write(fd_out, buf, n) != n) {
			fatal("Failed to copy fd");
		}
	}
}

static int safe_rename(const char* oldpath, const char* newpath)
{
	/* safe_rename is for creating entries in the cache.

	   Works like rename(), but it never overwrites an existing
	   cache entry. This avoids corruption on NFS. */
	int status = link(oldpath, newpath);
	if (status == 0 || errno == EEXIST) {
		return unlink(oldpath);
	}
	else {
		return -1;
	}
}

/* move a file using rename */
int move_file(const char *src, const char *dest) {
	return safe_rename(src, dest);
}

/* copy a file - used when hard links don't work
   the copy is done via a temporary file and atomic rename
*/
int copy_file(const char *src, const char *dest)
{
	int fd1, fd2;
	char buf[10240];
	int n;
	char *tmp_name;
	mode_t mask;

	x_asprintf(&tmp_name, "%s.XXXXXX", dest);

	fd1 = open(src, O_RDONLY|O_BINARY);
	if (fd1 == -1) {
		free(tmp_name);
		return -1;
	}

	fd2 = mkstemp(tmp_name);
	if (fd2 == -1) {
		close(fd1);
		free(tmp_name);
		return -1;
	}

	while ((n = read(fd1, buf, sizeof(buf))) > 0) {
		if (write(fd2, buf, n) != n) {
			close(fd2);
			close(fd1);
			unlink(tmp_name);
			free(tmp_name);
			return -1;
		}
	}

	close(fd1);

	/* get perms right on the tmp file */
	mask = umask(0);
	fchmod(fd2, 0666 & ~mask);
	umask(mask);

	/* the close can fail on NFS if out of space */
	if (close(fd2) == -1) {
		unlink(tmp_name);
		free(tmp_name);
		return -1;
	}

	unlink(dest);

	if (rename(tmp_name, dest) == -1) {
		unlink(tmp_name);
		free(tmp_name);
		return -1;
	}

	free(tmp_name);

	return 0;
}

#else /* ENABLE_ZLIB */

/* copy all data from one file descriptor to another
   possibly decompressing it
*/
void copy_fd(int fd_in, int fd_out) {
	char buf[10240];
	int n;
	gzFile gz_in;

	gz_in = gzdopen(dup(fd_in), "rb");

	if (!gz_in) {
		fatal("Failed to copy fd");
	}

	while ((n = gzread(gz_in, buf, sizeof(buf))) > 0) {
		if (write(fd_out, buf, n) != n) {
			fatal("Failed to copy fd");
		}
	}
}

static int _copy_file(const char *src, const char *dest, int mode) {
	int fd_in, fd_out;
	gzFile gz_in, gz_out = NULL;
	char buf[10240];
	int n, ret;
	char *tmp_name;
	mode_t mask;
	struct stat st;

	x_asprintf(&tmp_name, "%s.XXXXXX", dest);

	if (getenv("CCACHE_NOCOMPRESS")) {
		mode = COPY_UNCOMPRESSED;
	}

	/* open source file */
	fd_in = open(src, O_RDONLY);
	if (fd_in == -1) {
		return -1;
	}

	gz_in = gzdopen(fd_in, "rb");
	if (!gz_in) {
		close(fd_in);
		return -1;
	}

	/* open destination file */
	fd_out = mkstemp(tmp_name);
	if (fd_out == -1) {
		gzclose(gz_in);
		free(tmp_name);
		return -1;
	}

	if (mode == COPY_TO_CACHE) {
		/* The gzip file format occupies at least 20 bytes. So
		   it will always occupy an entire filesystem block,
		   even for empty files.
		   Since most stderr files will be empty, we turn off
		   compression in this case to save space.
		*/
		if (fstat(fd_in, &st) != 0) {
			gzclose(gz_in);
			close(fd_out);
			free(tmp_name);
			return -1;
		}
		if (file_size(&st) == 0) {
			mode = COPY_UNCOMPRESSED;
		}
	}

	if (mode == COPY_TO_CACHE) {
		gz_out = gzdopen(dup(fd_out), "wb");
		if (!gz_out) {
			gzclose(gz_in);
			close(fd_out);
			free(tmp_name);
			return -1;
		}
	}

	while ((n = gzread(gz_in, buf, sizeof(buf))) > 0) {
		if (mode == COPY_TO_CACHE) {
			ret = gzwrite(gz_out, buf, n);
		} else {
			ret = write(fd_out, buf, n);
		}
		if (ret != n) {
			gzclose(gz_in);
			if (gz_out) {
				gzclose(gz_out);
			}
			close(fd_out);
			unlink(tmp_name);
			free(tmp_name);
			return -1;
		}
	}

	gzclose(gz_in);
	if (gz_out) {
		gzclose(gz_out);
	}

	/* get perms right on the tmp file */
	mask = umask(0);
	fchmod(fd_out, 0666 & ~mask);
	umask(mask);

	/* the close can fail on NFS if out of space */
	if (close(fd_out) == -1) {
		unlink(tmp_name);
		free(tmp_name);
		return -1;
	}

	unlink(dest);

	if (rename(tmp_name, dest) == -1) {
		unlink(tmp_name);
		free(tmp_name);
		return -1;
	}

	free(tmp_name);

	return 0;
}

/* move a file to the cache, compressing it */
int move_file(const char *src, const char *dest) {
	int ret;

	ret = _copy_file(src, dest, COPY_TO_CACHE);
	if (ret != -1) unlink(src);
	return ret;
}

/* copy a file from the cache, decompressing it */
int copy_file(const char *src, const char *dest) {
	return _copy_file(src, dest, COPY_FROM_CACHE);
}
#endif /* ENABLE_ZLIB */

/* test if a file is zlib compressed */
int test_if_compressed(const char *filename) {
	FILE *f;

	f = fopen(filename, "rb");
	if (!f) {
		return 0;
	}

	/* test if file starts with 1F8B, which is zlib's
	 * magic number */
	if ((fgetc(f) != 0x1f) || (fgetc(f) != 0x8b)) {
		fclose(f);
		return 0;
	}

	fclose(f);
	return 1;
}

/* make sure a directory exists */
int create_dir(const char *dir)
{
	struct stat st;
	if (stat(dir, &st) == 0) {
		if (S_ISDIR(st.st_mode)) {
			return 0;
		}
		errno = ENOTDIR;
		return 1;
	}
	if (mkdir(dir, 0777) != 0 && errno != EEXIST) {
		return 1;
	}
	return 0;
}

char const CACHEDIR_TAG[] =
	"Signature: 8a477f597d28d172789f06886806bc55\n"
	"# This file is a cache directory tag created by ccache.\n"
	"# For information about cache directory tags, see:\n"
	"#	http://www.brynosaurus.com/cachedir/\n";

int create_cachedirtag(const char *dir)
{
	char *filename;
	struct stat st;
	FILE *f;
	x_asprintf(&filename, "%s/CACHEDIR.TAG", dir);
	if (stat(filename, &st) == 0) {
		if (S_ISREG(st.st_mode)) {
			goto success;
		}
		errno = EEXIST;
		goto error;
	}
	f = fopen(filename, "w");
	if (!f) goto error;
	if (fwrite(CACHEDIR_TAG, sizeof(CACHEDIR_TAG)-1, 1, f) != 1) {
		goto error;
	}
	if (fclose(f)) goto error;
success:
	free(filename);
	return 0;
error:
	free(filename);
	return 1;
}

/*
  this is like asprintf() but dies if the malloc fails
  note that we use vsnprintf in a rather poor way to make this more portable
*/
void x_asprintf(char **ptr, const char *format, ...)
{
	va_list ap;

	*ptr = NULL;
	va_start(ap, format);
	if (vasprintf(ptr, format, ap) == -1) {
		fatal("out of memory in x_asprintf");
	}
	va_end(ap);

	if (!*ptr) fatal("out of memory in x_asprintf");
}

/*
  this is like strdup() but dies if the malloc fails
*/
char *x_strdup(const char *s)
{
	char *ret;
	ret = strdup(s);
	if (!ret) {
		fatal("out of memory in strdup\n");
	}
	return ret;
}

/*
  this is like malloc() but dies if the malloc fails
*/
void *x_malloc(size_t size)
{
	void *ret;
	ret = malloc(size);
	if (!ret) {
		fatal("out of memory in malloc\n");
	}
	return ret;
}

/*
  this is like realloc() but dies if the malloc fails
*/
void *x_realloc(void *ptr, size_t size)
{
	void *p2;
	if (!ptr) return x_malloc(size);
	p2 = realloc(ptr, size);
	if (!p2) {
		fatal("out of memory in x_realloc");
	}
	return p2;
}


/*
   revsusive directory traversal - used for cleanup
   fn() is called on all files/dirs in the tree
 */
void traverse(const char *dir, void (*fn)(const char *, struct stat *))
{
	DIR *d;
	struct dirent *de;

	d = opendir(dir);
	if (!d) return;

	while ((de = readdir(d))) {
		char *fname;
		struct stat st;

		if (strcmp(de->d_name,".") == 0) continue;
		if (strcmp(de->d_name,"..") == 0) continue;

		if (strlen(de->d_name) == 0) continue;

		x_asprintf(&fname, "%s/%s", dir, de->d_name);
		if (lstat(fname, &st)) {
			if (errno != ENOENT) {
				perror(fname);
			}
			free(fname);
			continue;
		}

		if (S_ISDIR(st.st_mode)) {
			traverse(fname, fn);
		}

		fn(fname, &st);
		free(fname);
	}

	closedir(d);
}


/* return the base name of a file - caller frees */
char *str_basename(const char *s)
{
	char *p = strrchr(s, '/');
	if (p) {
		return x_strdup(p+1);
	}

	return x_strdup(s);
}

/* return the dir name of a file - caller frees */
char *dirname(char *s)
{
	char *p;
	s = x_strdup(s);
	p = strrchr(s, '/');
	if (p) {
		*p = 0;
	}
	return s;
}

int lock_fd(int fd)
{
	struct flock fl;
	int ret;

	fl.l_type = F_WRLCK;
	fl.l_whence = SEEK_SET;
	fl.l_start = 0;
	fl.l_len = 1;
	fl.l_pid = 0;

	/* not sure why we would be getting a signal here,
	   but one user claimed it is possible */
	do {
		ret = fcntl(fd, F_SETLKW, &fl);
	} while (ret == -1 && errno == EINTR);
	return ret;
}

/* return size on disk of a file */
size_t file_size(struct stat *st)
{
	size_t size = st->st_blocks * 512;
	if ((size_t)st->st_size > size) {
		/* probably a broken stat() call ... */
		size = (st->st_size + 1023) & ~1023;
	}
	return size;
}


/* a safe open/create for read-write */
int safe_open(const char *fname)
{
	int fd = open(fname, O_RDWR|O_BINARY);
	if (fd == -1 && errno == ENOENT) {
		fd = open(fname, O_RDWR|O_CREAT|O_EXCL|O_BINARY, 0666);
		if (fd == -1 && errno == EEXIST) {
			fd = open(fname, O_RDWR|O_BINARY);
		}
	}
	return fd;
}

/* display a kilobyte unsigned value in M, k or G */
void display_size(unsigned v)
{
	if (v > 1024*1024) {
		printf("%8.1f Gbytes", v/((double)(1024*1024)));
	} else if (v > 1024) {
		printf("%8.1f Mbytes", v/((double)(1024)));
	} else {
		printf("%8u Kbytes", v);
	}
}

/* return a value in multiples of 1024 give a string that can end
   in K, M or G
*/
size_t value_units(const char *s)
{
	char m;
	double v = atof(s);
	m = s[strlen(s)-1];
	switch (m) {
	case 'G':
	case 'g':
	default:
		v *= 1024*1024;
		break;
	case 'M':
	case 'm':
		v *= 1024;
		break;
	case 'K':
	case 'k':
		v *= 1;
		break;
	}
	return (size_t)v;
}


/*
  a sane realpath() function, trying to cope with stupid path limits and
  a broken API
*/
char *x_realpath(const char *path)
{
	int maxlen;
	char *ret, *p;
#ifdef PATH_MAX
	maxlen = PATH_MAX;
#elif defined(MAXPATHLEN)
	maxlen = MAXPATHLEN;
#elif defined(_PC_PATH_MAX)
	maxlen = pathconf(path, _PC_PATH_MAX);
#endif
	if (maxlen < 4096) maxlen = 4096;

	ret = x_malloc(maxlen);

#if HAVE_REALPATH
	p = realpath(path, ret);
#else
	/* yes, there are such systems. This replacement relies on
	   the fact that when we call x_realpath we only care about symlinks */
	{
		int len = readlink(path, ret, maxlen-1);
		if (len == -1) {
			free(ret);
			return NULL;
		}
		ret[len] = 0;
		p = ret;
	}
#endif
	if (p) {
		p = x_strdup(p);
		free(ret);
		return p;
	}
	free(ret);
	return NULL;
}

/* a getcwd that will returns an allocated buffer */
char *gnu_getcwd(void)
{
	unsigned size = 128;

	while (1) {
		char *buffer = (char *)x_malloc(size);
		if (getcwd(buffer, size) == buffer) {
			return buffer;
		}
		free(buffer);
		if (errno != ERANGE) {
			return 0;
		}
		size *= 2;
	}
}

#ifndef HAVE_MKSTEMP
/* cheap and nasty mkstemp replacement */
int mkstemp(char *template)
{
	mktemp(template);
	return open(template, O_RDWR | O_CREAT | O_EXCL | O_BINARY, 0600);
}
#endif


/* create an empty file */
int create_empty_file(const char *fname)
{
	int fd;

	fd = open(fname, O_WRONLY|O_CREAT|O_TRUNC|O_EXCL|O_BINARY, 0666);
	if (fd == -1) {
		return -1;
	}
	close(fd);
	return 0;
}

/*
  return current users home directory or die
*/
const char *get_home_directory(void)
{
	const char *p = getenv("HOME");
	if (p) {
		return p;
	}
#ifdef HAVE_GETPWUID
	{
		struct passwd *pwd = getpwuid(getuid());
		if (pwd) {
			return pwd->pw_dir;
		}
	}
#endif
	cc_log("Unable to determine home directory");
	return NULL;
}

