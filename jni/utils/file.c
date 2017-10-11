/* file.c - Contains all files related utilities
 */

#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/sendfile.h>
#include <selinux/selinux.h>

#include "utils.h"

char **excl_list = (char *[]) { NULL };

static int is_excl(const char *name) {
	for (int i = 0; excl_list[i]; ++i) {
		if (strcmp(name, excl_list[i]) == 0)
			return 1;
	}
	return 0;
}

int mkdir_p(const char *pathname, mode_t mode) {
	char *path = strdup(pathname), *p;
	errno = 0;
	for (p = path + 1; *p; ++p) {
		if (*p == '/') {
			*p = '\0';
			if (mkdir(path, mode) == -1) {
				if (errno != EEXIST)
					return -1;
			}
			*p = '/';
		}
	}
	if (mkdir(path, mode) == -1) {
		if (errno != EEXIST)
			return -1;
	}
	free(path);
	return 0;
}

void rm_rf(const char *path) {
	int fd = xopen(path, O_RDONLY | O_CLOEXEC);
	frm_rf(fd);
	close(fd);
	rmdir(path);
}

void frm_rf(int dirfd) {
	struct dirent *entry;
	int newfd;
	DIR *dir = fdopendir(dirfd);

	while ((entry = readdir(dir))) {
		if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
			continue;
		if (is_excl(entry->d_name))
			continue;
		switch (entry->d_type) {
		case DT_DIR:
			newfd = openat(dirfd, entry->d_name, O_RDONLY | O_CLOEXEC);
			frm_rf(newfd);
			close(newfd);
			unlinkat(dirfd, entry->d_name, AT_REMOVEDIR);
			break;
		default:
			unlinkat(dirfd, entry->d_name, 0);
			break;
		}
	}
}

/* This will only on the same file system */
void mv_f(const char *source, const char *destination) {
	struct stat st;
	xlstat(source, &st);
	int src, dest;
	struct file_attr a;

	if (S_ISDIR(st.st_mode)) {
		xmkdir_p(destination, st.st_mode & 0777);
		src = xopen(source, O_RDONLY | O_CLOEXEC);
		dest = xopen(destination, O_RDONLY | O_CLOEXEC);
		fclone_attr(src, dest);
		mv_dir(src, dest);
		close(src);
		close(dest);
	} else{
		getattr(source, &a);
		rename(source, destination);
		setattr(destination, &a);
	}
	rmdir(source);
}

/* This will only on the same file system */
void mv_dir(int src, int dest) {
	struct dirent *entry;
	DIR *dir;
	int newsrc, newdest;
	struct file_attr a;

	dir = fdopendir(src);
	while ((entry = readdir(dir))) {
		if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
			continue;
		if (is_excl(entry->d_name))
			continue;
		getattrat(src, entry->d_name, &a);
		switch (entry->d_type) {
		case DT_DIR:
			mkdirat(dest, entry->d_name, a.st.st_mode & 0777);
			newsrc = openat(src, entry->d_name, O_RDONLY | O_CLOEXEC);
			newdest = openat(dest, entry->d_name, O_RDONLY | O_CLOEXEC);
			fsetattr(newdest, &a);
			mv_dir(newsrc, newdest);
			close(newsrc);
			close(newdest);
			unlinkat(src, entry->d_name, AT_REMOVEDIR);
			break;
		case DT_LNK:
		case DT_REG:
			renameat(src, entry->d_name, dest, entry->d_name);
			setattrat(dest, entry->d_name, &a);
			break;
		}
	}
}

void cp_afc(const char *source, const char *destination) {
	int src, dest;
	struct file_attr a;
	getattr(source, &a);

	if (S_ISDIR(a.st.st_mode)) {
		xmkdir_p(destination, a.st.st_mode & 0777);
		src = xopen(source, O_RDONLY | O_CLOEXEC);
		dest = xopen(destination, O_RDONLY | O_CLOEXEC);
		fsetattr(dest, &a);
		clone_dir(src, dest);
		close(src);
		close(dest);
	} else{
		unlink(destination);
		if (S_ISREG(a.st.st_mode)) {
			src = xopen(source, O_RDONLY);
			dest = xopen(destination, O_WRONLY | O_CREAT | O_TRUNC);
			xsendfile(dest, src, NULL, a.st.st_size);
			fsetattr(src, &a);
			close(src);
			close(dest);
		} else if (S_ISLNK(a.st.st_mode)) {
			char buf[PATH_MAX];
			xreadlink(source, buf, sizeof(buf));
			xsymlink(buf, destination);
			setattr(destination, &a);
		}
	}
}

void clone_dir(int src, int dest) {
	struct dirent *entry;
	DIR *dir;
	int srcfd, destfd, newsrc, newdest;
	char buf[PATH_MAX];
	ssize_t size;
	struct file_attr a;

	dir = fdopendir(src);
	while ((entry = readdir(dir))) {
		if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
			continue;
		if (is_excl(entry->d_name))
			continue;
		getattrat(src, entry->d_name, &a);
		switch (entry->d_type) {
		case DT_DIR:
			mkdirat(dest, entry->d_name, a.st.st_mode & 0777);
			setattrat(dest, entry->d_name, &a);
			newsrc = openat(src, entry->d_name, O_RDONLY | O_CLOEXEC);
			newdest = openat(dest, entry->d_name, O_RDONLY | O_CLOEXEC);
			clone_dir(newsrc, newdest);
			close(newsrc);
			close(newdest);
			break;
		case DT_REG:
			destfd = openat(dest, entry->d_name, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, a.st.st_mode & 0777);
			srcfd = openat(src, entry->d_name, O_RDONLY | O_CLOEXEC);
			sendfile(destfd, srcfd, 0, a.st.st_size);
			fsetattr(destfd, &a);
			close(destfd);
			close(srcfd);
			break;
		case DT_LNK:
			size = readlinkat(src, entry->d_name, buf, sizeof(buf));
			buf[size] = '\0';
			symlinkat(buf, dest, entry->d_name);
			setattrat(dest, entry->d_name, &a);
			break;
		}
	}
}

int getattr(const char *path, struct file_attr *a) {
	int fd = open(path, O_PATH | O_NOFOLLOW | O_CLOEXEC);
	if (fd < 0)
		return -1;
	int ret = fgetattr(fd, a);
	close(fd);
	return ret;
}

int getattrat(int dirfd, const char *pathname, struct file_attr *a) {
	int fd = openat(dirfd, pathname, O_PATH | O_NOFOLLOW | O_CLOEXEC);
	if (fd < 0)
		return -1;
	int ret = fgetattr(fd, a);
	close(fd);
	return ret;
}

int fgetattr(int fd, struct file_attr *a) {
	if (fstat(fd, &a->st) < 0)
		return -1;
	char *con = "";
	if (fgetfilecon(fd, &con) < 0)
		return -1;
	strcpy(a->con, con);
	freecon(con);
	return 0;
}

int setattr(const char *path, struct file_attr *a) {
	int fd = open(path, O_PATH | O_NOFOLLOW | O_CLOEXEC);
	if (fd < 0)
		return -1;
	int ret = fsetattr(fd, a);
	close(fd);
	return ret;
}

int setattrat(int dirfd, const char *pathname, struct file_attr *a) {
	int fd = openat(dirfd, pathname, O_PATH | O_NOFOLLOW | O_CLOEXEC);
	if (fd < 0)
		return -1;
	int ret = fsetattr(fd, a);
	close(fd);
	return ret;
}

int fsetattr(int fd, struct file_attr *a) {
	if (fchmod(fd, a->st.st_mode & 0777) < 0)
		return -1;
	if (fchown(fd, a->st.st_uid, a->st.st_gid) < 0)
		return -1;
	if (strlen(a->con) && fsetfilecon(fd, a->con) < 0)
		return -1;
	return 0;
}

void clone_attr(const char *source, const char *target) {
	struct file_attr a;
	getattr(source, &a);
	setattr(target, &a);
}

void fclone_attr(const int sourcefd, const int targetfd) {
	struct file_attr a;
	fgetattr(sourcefd, &a);
	fsetattr(targetfd, &a);
}