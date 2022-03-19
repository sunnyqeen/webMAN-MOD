#define NO_MSG							NULL

int64_t file_copy(const char *file1, char *file2, u64 maxbytes);

static bool copy_in_progress = false;
static bool dont_copy_same_size = true; // skip copy the file if it already exists in the destination folder with the same file size
static bool allow_sc36 = true; // used to skip decrypt dev_bdvd files in file_copy function if it's called from folder_copy

static u32 copied_count = 0;

#define COPY_WHOLE_FILE		0
#define SAVE_ALL			0
#define APPEND_TEXT			(-0xADD0ADD0ADD000ALL)
#define DONT_CLEAR_DATA		-1
#define RECURSIVE_DELETE	2 // don't check for ADMIN/USER

static void enable_dev_blind(const char *msg);
static sys_addr_t g_sysmem = NULL;

#include "file_ntfs.h"
#include "file_size.h"
#include "file_devs.h"
#include "file_copy.h"
#include "file_delete.h"

static bool isDir(const char *path)
{
#ifdef USE_NTFS
	if(is_ntfs_path2(path))
	{
		char tmp[STD_PATH_LEN];
		strcpy(tmp, ntfs_path(path)); tmp[5] = ':';
		struct stat bufn;
		return ((ps3ntfs_stat(tmp, &bufn) >= 0) && (bufn.st_mode & S_IFDIR));
	}
#endif

	struct CellFsStat s;
	if(cellFsStat(path, &s) == CELL_FS_SUCCEEDED)
		return ((s.st_mode & CELL_FS_S_IFDIR) != 0);
	else
		return false;
}

static bool file_exists(const char *path)
{
	return (file_ssize(path) >= 0);
}

static bool not_exists(const char *path)
{
	return !file_exists(path);
}

#ifdef COBRA_ONLY
static bool is_app_dir(const char *path, const char *app_name)
{
	char eboot[STD_PATH_LEN];
	sprintf(eboot, "%s/%s/USRDIR/EBOOT.BIN", path, app_name);
	return file_exists(eboot);
}

static bool is_iso_0(const char *filename)
{
	#ifdef MOUNT_PNG
	if(!extcasecmp(filename, ".0.PNG", 6)) return true;
	#endif
	return !extcasecmp(filename, ".iso.0", 6);
}
#endif

static char *get_ext(const char *path)
{
	int plen = strlen(path) - 4;
	if(plen < 0) plen = 0;
	else if(path[plen + 1] == '.') plen++;
	else if(path[plen + 2] == '.') plen+=2;
	return (char *)(path + plen);
}

static char *get_filename(const char *path)
{
	return strrchr(path, '/'); // return with slash
}

#ifndef LITE_EDITION
static char *remove_filename(const char *path)
{
	char *p = strrchr(path, '/'); if(p) *p = NULL; else p = (char*)path;
	return p;
}
#endif
#ifdef COBRA_ONLY
static bool change_ext(char *filename, int num_ext, const char *file_ext[])
{
	char *ext = get_ext(filename);
	for(u8 e = 0; e < num_ext; e++)
	{
		sprintf(ext, "%s", file_ext[e]);
		if(file_exists(filename)) return true;
	}
	return false;
}

static void change_cue2iso(char *cue_file)
{
	if(is_ext(cue_file, ".cue") || is_ext(cue_file, ".ccd"))
	{
		const char *iso_ext[8] = {".bin", ".iso", ".img", ".mdf", ".BIN", ".ISO", ".IMG", ".MDF"};
		change_ext(cue_file, 8, iso_ext);
	}
}

#define check_ps3_game(path)
#else
static void check_ps3_game(char *path)
{
	char *p = strstr(path, "/PS3_GAME");
	if(p)
	{
		p[6] = 'M', p[7] = '0', p[8] = '1';  // PS3_GM01
		if(file_exists(path)) return;
		p[6] = 'A', p[7] = 'M', p[8] = 'E';  // PS3_GAME
	}
}
#endif

static void check_path_alias(char *param)
{
	if(islike(param, "/dev_blind")) enable_dev_blind(NULL);

	if(not_exists(param))
	{
		check_path_tags(param);

		if(!islike(param, "/dev_") && !islike(param, "/net"))
		{
			if(strstr(param, ".ps3")) return;

			char path[STD_PATH_LEN];
			int len = snprintf(path, STD_PATH_LEN - 1, "%s", (*param == '/') ? param + 1 : param);
			char *wildcard = strchr(path, '*'); if(wildcard) *wildcard = 0;
			if((len == 4) && path[3] == '/') path[3] = 0; // normalize path
			if(IS(path, "pkg")) {sprintf(param, DEFAULT_PKG_PATH);} else
			if(IS(path, "xml")) {*path = 0;} else
			if(IS(path, "xmb")) {enable_dev_blind(NULL); sprintf(param, "/dev_blind/vsh/resource/explore/xmb");} else
			if(IS(path, "res")) {enable_dev_blind(NULL); sprintf(param, "/dev_blind/vsh/resource");} else
			if(IS(path, "mod")) {enable_dev_blind(NULL); sprintf(param, "/dev_blind/vsh/module");} else
			if(IS(path, "cov")) {sprintf(param, "%s/covers", MM_ROOT_STD);} else
			if(IS(path, "cvr")) {sprintf(param, "%s/covers_retro/psx", MM_ROOT_STD);} else
			if(islike(path, "res/"))  {sprintf(param, "/dev_blind/vsh/resource/%s", path + 4);} else
			if(isDir(html_base_path)) {snprintf(param, HTML_RECV_LAST, "%s/%s", html_base_path, path);} // use html path (if path is omitted)

			if(not_exists(param)) {snprintf(param, HTML_RECV_LAST, "%s/%s", HTML_BASE_PATH, path);} // try HTML_BASE_PATH
			if(not_exists(param)) {snprintf(param, HTML_RECV_LAST, "%s/%s", webman_config->home_url, path);} // try webman_config->home_url
			if(not_exists(param)) {snprintf(param, HTML_RECV_LAST, "%s%s",  HDD0_GAME_DIR, path);} // try /dev_hdd0/game
			if(not_exists(param)) {snprintf(param, HTML_RECV_LAST, "%s%s", _HDD0_GAME_DIR, path);} // try /dev_hdd0//game
			if(not_exists(param))
			{
				for(u8 i = 0; i < (MAX_DRIVES + 1); i++)
				{
					if(i == NET) i = NTFS + 1;
					snprintf(param, HTML_RECV_LAST, "%s/%s", drives[i], path);
					if(file_exists(param)) break;
				}
			} // try hdd0, usb0, usb1, etc.
			if(not_exists(param)) {snprintf(param, HTML_RECV_LAST, "%s/%s", "/dev_hdd0/tmp", path);} // try hdd0
			if(not_exists(param)) {snprintf(param, HTML_RECV_LAST, "%s/%s", HDD0_HOME_DIR, path);} // try /dev_hdd0/home
			if(wildcard) {*wildcard = '*'; strcat(param, wildcard);}
		}
	}
}

#if defined(COPY_PS3) || defined(PKG_HANDLER) || defined(MOUNT_GAMEI)
static void mkdir_tree(char *path)
{
	size_t path_len = strlen(path);
#ifdef USE_NTFS
	if(is_ntfs_path2(path))
	{
		char *npath = (char*)ntfs_path(path);
		npath[5] = ':';
		for(u16 p = 7; p < path_len; p++)
			if(path[p] == '/') {path[p] = '\0'; ps3ntfs_mkdir(ntfs_path(path), DMODE); path[p] = '/';}
	}
	else
#endif
	{
		for(u16 p = 12; p < path_len; p++)
			if(path[p] == '/') {path[p] = '\0'; cellFsMkdir(path, DMODE); path[p] = '/';}
	}
}
#endif

static void mkdirs(char *param)
{
	cellFsMkdir(TMP_DIR, DMODE);
	cellFsMkdir(WMTMP, DMODE);
	cellFsMkdir("/dev_hdd0/packages", DMODE);
	//cellFsMkdir("/dev_hdd0/GAMES",  DMODE);
	//cellFsMkdir("/dev_hdd0/PS3ISO", DMODE);
	//cellFsMkdir("/dev_hdd0/DVDISO", DMODE);
	//cellFsMkdir("/dev_hdd0/BDISO",  DMODE);
	//cellFsMkdir("/dev_hdd0/PS2ISO", DMODE);
	//cellFsMkdir("/dev_hdd0/PSXISO", DMODE);
	//cellFsMkdir("/dev_hdd0/PSPISO", DMODE);
	#ifdef MOUNT_ROMS
	cellFsMkdir("/dev_hdd0/ROMS", DMODE);
	#endif

	sprintf(param, "/dev_hdd0");
	for(u8 i = 0; i < 9; i++)
	{
		if(i == 1 || i == 7) continue; // skip /GAMEZ & /PSXGAMES
		sprintf(param + 9 , "/%s", paths[i]);
		cellFsMkdir(param, DMODE);
	}

	param[9] = '\0'; // <- return /dev_hdd0
}

size_t read_file(const char *file, char *data, const size_t size, s32 offset)
{
	if(!data) return 0;

	int fd = 0; u64 read_e = 0;

	if(offset < 0) offset = 0; else memset(data, 0, size);

#ifdef USE_NTFS
	if(is_ntfs_path2(file))
	{
		fd = ps3ntfs_open(ntfs_path(file), O_RDONLY, 0);
		if(fd >= 0)
		{
			ps3ntfs_seek64(fd, offset, SEEK_SET);
			read_e = ps3ntfs_read(fd, (void *)data, size);
			ps3ntfs_close(fd);
		}
		return read_e;
	}
#endif

	if(cellFsOpen(file, CELL_FS_O_RDONLY, &fd, NULL, 0) == CELL_FS_SUCCEEDED)
	{
		if(cellFsReadWithOffset(fd, offset, (void *)data, size, &read_e) != CELL_FS_SUCCEEDED) read_e = 0;
		cellFsClose(fd);
	}

	return read_e;
}

static u16 read_sfo(const char *file, char *data)
{
	return (u16)read_file(file, data, _4KB_, 0);
}

static int write_file(const char *file, int flags, const char *data, u64 offset, int size, bool crlf)
{
	int fd = 0;

#ifdef USE_NTFS
	if(is_ntfs_path2(file))
	{
		int nflags = O_CREAT | O_WRONLY;
		if(flags & CELL_FS_O_APPEND) nflags |= O_APPEND;
		if(flags & CELL_FS_O_TRUNC)  nflags |= O_TRUNC;

		fd = ps3ntfs_open(ntfs_path(file), nflags, MODE);
		if(fd >= 0)
		{
			if(offset) ps3ntfs_seek64(fd, offset, SEEK_SET);
			if((size <= SAVE_ALL) && data) size = strlen(data);
			if( size ) ps3ntfs_write(fd, data, size);
			if( crlf ) ps3ntfs_write(fd, (void *)"\r\n", 2);
			ps3ntfs_close(fd);
			return CELL_FS_SUCCEEDED;
		}
		return FAILED;
	}
#endif

	cellFsChmod(file, MODE); // set permissions for overwrite

	if(cellFsOpen(file, flags, &fd, NULL, 0) == CELL_FS_SUCCEEDED)
	{
		if(offset) cellFsLseek(fd, offset, CELL_FS_SEEK_SET, &offset);
		if((size <= SAVE_ALL) && data) size = strlen(data);
		if( size ) cellFsWrite(fd, (void *)data, size, NULL);
		if( crlf ) cellFsWrite(fd, (void *)"\r\n", 2, NULL);
		cellFsClose(fd);

		cellFsChmod(file, MODE); // set permissions if created

		return CELL_FS_SUCCEEDED;
	}

	return FAILED;
}

int save_file(const char *file, const char *mem, s64 size)
{
	bool crlf = (size == APPEND_TEXT); // auto add new line

	int flags = CELL_FS_O_CREAT | CELL_FS_O_TRUNC | CELL_FS_O_WRONLY;
	if( size < 0 )  {flags = CELL_FS_O_APPEND | CELL_FS_O_CREAT | CELL_FS_O_WRONLY; size = crlf ? SAVE_ALL : -size;} else
	if(!extcmp(file, "/PARAM.SFO", 10)) flags = CELL_FS_O_CREAT | CELL_FS_O_WRONLY;
/*
	cellFsChmod(file, MODE); // set permissions for overwrite

	int fd = 0;
	if(cellFsOpen(file, flags, &fd, NULL, 0) == CELL_FS_SUCCEEDED)
	{
		if(size) cellFsWrite(fd, (void *)mem, size, NULL);
		if(crlf) cellFsWrite(fd, (void *)"\r\n", 2, NULL);
		cellFsClose(fd);

		cellFsChmod(file, MODE); // set permissions if created

		return CELL_FS_SUCCEEDED;
	}
	return FAILED;
*/
	return write_file(file, flags, mem, 0, (int)size, crlf);
}
/*
static void addlog(const char *msg1, const char *msg2, u64 i)
{
	char msg[200];
	snprintf(msg, 199, "%llx %s %s", i, msg1, msg2);
	save_file("/dev_hdd0/wmm.log", msg, APPEND_TEXT);
}
*/

//////////////////////////////////////////////////////////////

static int wait_path(const char *path, u8 timeout, bool found)
{
	if(*path!='/') return FAILED;

	for(u8 n = 0; n < (timeout * 20); n++)
	{
		if(file_exists(path) == found) return CELL_FS_SUCCEEDED;
		if(!working) break;
		sys_ppu_thread_usleep(50000);
	}
	return FAILED;
}

int wait_for(const char *path, u8 timeout)
{
	return wait_path(path, timeout, true);
}

#define MAX_WAIT	30

static u8 wait_for_xmb(void)
{
	u8 t = 0;
	while(View_Find("explore_plugin") == 0) {if(++t > MAX_WAIT) break; sys_ppu_thread_sleep(1);}
	return (t > MAX_WAIT); // true = timeout
}

#ifdef MOUNT_ROMS
static void copy_rom_media(const char *src_path)
{
	// get rom name & file extension
	char *name = get_filename(src_path);
	if(!name) return;

	char dst_path[64];
	char path[MAX_LINE_LEN];
	const char *PS3_GAME[2] = { "/PS3_GAME", ""};

	char *ext  = strrchr(++name, '.');
	if(ext)
	{
		for(u8 p = 0; p < 2; p++)
		{
			// copy rom icon to ICON0.PNG
			sprintf(dst_path, "%s%s/%s", PKGLAUNCH_DIR, PS3_GAME[p], "ICON0.PNG");
			{strcpy(ext, ".png"); if(file_exists(src_path)) {file_copy(src_path, dst_path, COPY_WHOLE_FILE);} else
			{strcpy(ext, ".PNG"); if(file_exists(src_path)) {file_copy(src_path, dst_path, COPY_WHOLE_FILE);} else
			{
				sprintf(path, "%s/%s", WMTMP, name);
				char *ext2 = strrchr(path, '.');
				{strcpy(ext2, ".png"); if(file_exists(path)) {file_copy(path, dst_path, COPY_WHOLE_FILE);} else
				{strcpy(ext2, ".PNG"); if(file_exists(path)) {file_copy(path, dst_path, COPY_WHOLE_FILE);} else
															 {file_copy(PKGLAUNCH_ICON, dst_path, COPY_WHOLE_FILE);}}}
			}}}

			strcpy(path, src_path); char *path_ = get_filename(path) + 1;

			const char *media[5] = {"PIC0.PNG", "PIC1.PNG", "PIC2.PNG", "SND0.AT3", "ICON1.PAM"};
			for(u8 i = 0; i < 5; i++)
			{
				sprintf(dst_path, "%s%s/%s", PKGLAUNCH_DIR, PS3_GAME[p], media[i]); cellFsUnlink(dst_path);
				strcpy(ext + 1, media[i]);
				if(file_exists(src_path))
					file_copy(src_path, dst_path, COPY_WHOLE_FILE);
				else
				{
					strcpy(path_, media[i]);
					if(file_exists(path))
						file_copy(path, dst_path, COPY_WHOLE_FILE);
				}
			}
		}
		*ext = NULL;
	}

	// patch title name in PARAM.SFO of PKGLAUNCH
	sprintf(dst_path, "%s/PS3_GAME/%s", PKGLAUNCH_DIR, "PARAM.SFO");
	write_file(dst_path, CELL_FS_O_CREAT | CELL_FS_O_WRONLY, name, 0x378, 0x80, false);
}
#endif // #ifdef MOUNT_ROMS
