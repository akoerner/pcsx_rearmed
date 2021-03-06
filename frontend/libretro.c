/*
 * (C) notaz, 2012,2014,2015
 *
 * This work is licensed under the terms of the GNU GPLv2 or later.
 * See the COPYING file in the top-level directory.
 */

#define _GNU_SOURCE 1 // strcasestr
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#ifdef __MACH__
#include <unistd.h>
#include <sys/syscall.h>
#endif

#include "../libpcsxcore/misc.h"
#include "../libpcsxcore/psxcounters.h"
#include "../libpcsxcore/psxmem_map.h"
#include "../libpcsxcore/new_dynarec/new_dynarec.h"
#include "../libpcsxcore/cdrom.h"
#include "../libpcsxcore/cdriso.h"
#include "../libpcsxcore/cheat.h"
#include "../plugins/dfsound/out.h"
#include "../plugins/dfsound/spu_config.h"
#include "../plugins/dfinput/externals.h"
#include "cspace.h"
#include "main.h"
#include "plugin.h"
#include "plugin_lib.h"
#include "arm_features.h"
#include "revision.h"
#include "libretro.h"

static retro_video_refresh_t video_cb;
static retro_input_poll_t input_poll_cb;
static retro_input_state_t input_state_cb;
static retro_environment_t environ_cb;
static retro_audio_sample_batch_t audio_batch_cb;
static struct retro_rumble_interface rumble;

static void *vout_buf;
static int vout_width, vout_height;
static int vout_doffs_old, vout_fb_dirty;
static bool vout_can_dupe;
static bool duping_enable;

static int plugins_opened;
static int is_pal_mode;

/* memory card data */
extern char Mcd1Data[MCD_SIZE];
extern char McdDisable[2];

/* PCSX ReARMed core calls and stuff */
int in_type1, in_type2;
int in_a1[2] = { 127, 127 }, in_a2[2] = { 127, 127 };
int in_keystate;
int in_enable_vibration = 1;

/* PSX max resolution is 640x512, but with enhancement it's 1024x512 */
#define VOUT_MAX_WIDTH 1024
#define VOUT_MAX_HEIGHT 512

static void init_memcard(char *mcd_data)
{
	unsigned off = 0;
	unsigned i;

	memset(mcd_data, 0, MCD_SIZE);

	mcd_data[off++] = 'M';
	mcd_data[off++] = 'C';
	off += 0x7d;
	mcd_data[off++] = 0x0e;

	for (i = 0; i < 15; i++) {
		mcd_data[off++] = 0xa0;
		off += 0x07;
		mcd_data[off++] = 0xff;
		mcd_data[off++] = 0xff;
		off += 0x75;
		mcd_data[off++] = 0xa0;
	}

	for (i = 0; i < 20; i++) {
		mcd_data[off++] = 0xff;
		mcd_data[off++] = 0xff;
		mcd_data[off++] = 0xff;
		mcd_data[off++] = 0xff;
		off += 0x04;
		mcd_data[off++] = 0xff;
		mcd_data[off++] = 0xff;
		off += 0x76;
	}
}

static int vout_open(void)
{
	return 0;
}

static void vout_set_mode(int w, int h, int raw_w, int raw_h, int bpp)
{
	vout_width = w;
	vout_height = h;
}

#ifndef FRONTEND_SUPPORTS_RGB565
static void convert(void *buf, size_t bytes)
{
	unsigned int i, v, *p = buf;

	for (i = 0; i < bytes / 4; i++) {
		v = p[i];
		p[i] = (v & 0x001f001f) | ((v >> 1) & 0x7fe07fe0);
	}
}
#endif

static void vout_flip(const void *vram, int stride, int bgr24, int w, int h)
{
	unsigned short *dest = vout_buf;
	const unsigned short *src = vram;
	int dstride = vout_width, h1 = h;
	int doffs;

	if (vram == NULL) {
		// blanking
		memset(vout_buf, 0, dstride * h * 2);
		goto out;
	}

	doffs = (vout_height - h) * dstride;
	doffs += (dstride - w) / 2 & ~1;
	if (doffs != vout_doffs_old) {
		// clear borders
		memset(vout_buf, 0, dstride * h * 2);
		vout_doffs_old = doffs;
	}
	dest += doffs;

	if (bgr24)
	{
		// XXX: could we switch to RETRO_PIXEL_FORMAT_XRGB8888 here?
		for (; h1-- > 0; dest += dstride, src += stride)
		{
			bgr888_to_rgb565(dest, src, w * 3);
		}
	}
	else
	{
		for (; h1-- > 0; dest += dstride, src += stride)
		{
			bgr555_to_rgb565(dest, src, w * 2);
		}
	}

out:
#ifndef FRONTEND_SUPPORTS_RGB565
	convert(vout_buf, vout_width * vout_height * 2);
#endif
	vout_fb_dirty = 1;
	pl_rearmed_cbs.flip_cnt++;
}

static void vout_close(void)
{
}

static void *pl_mmap(unsigned int size)
{
	return psxMap(0, size, 0, MAP_TAG_VRAM);
}

static void pl_munmap(void *ptr, unsigned int size)
{
	psxUnmap(ptr, size, MAP_TAG_VRAM);
}

struct rearmed_cbs pl_rearmed_cbs = {
	.pl_vout_open = vout_open,
	.pl_vout_set_mode = vout_set_mode,
	.pl_vout_flip = vout_flip,
	.pl_vout_close = vout_close,
	.mmap = pl_mmap,
	.munmap = pl_munmap,
	/* from psxcounters */
	.gpu_hcnt = &hSyncCount,
	.gpu_frame_count = &frame_counter,
};

void pl_frame_limit(void)
{
	/* called once per frame, make psxCpu->Execute() above return */
	stop = 1;
}

void pl_timing_prepare(int is_pal)
{
	is_pal_mode = is_pal;
}

void plat_trigger_vibrate(int pad, int low, int high)
{
    rumble.set_rumble_state(pad, RETRO_RUMBLE_STRONG, high << 8);
    rumble.set_rumble_state(pad, RETRO_RUMBLE_WEAK, low ? 0xffff : 0x0);
}

void pl_update_gun(int *xn, int *yn, int *xres, int *yres, int *in)
{
}

/* sound calls */
static int snd_init(void)
{
	return 0;
}

static void snd_finish(void)
{
}

static int snd_busy(void)
{
	return 0;
}

static void snd_feed(void *buf, int bytes)
{
	if (audio_batch_cb != NULL)
		audio_batch_cb(buf, bytes / 4);
}

void out_register_libretro(struct out_driver *drv)
{
	drv->name = "libretro";
	drv->init = snd_init;
	drv->finish = snd_finish;
	drv->busy = snd_busy;
	drv->feed = snd_feed;
}

/* libretro */
void retro_set_environment(retro_environment_t cb)
{
   static const struct retro_variable vars[] = {
      { "pcsx_rearmed_frameskip", "Frameskip; 0|1|2|3" },
      { "pcsx_rearmed_region", "Region; Auto|NTSC|PAL" },
      { "pcsx_rearmed_pad1type", "Pad 1 Type; standard|analog" },
      { "pcsx_rearmed_pad2type", "Pad 2 Type; standard|analog" },
#ifndef DRC_DISABLE
      { "pcsx_rearmed_drc", "Dynamic recompiler; enabled|disabled" },
#endif
#ifdef __ARM_NEON__
      { "pcsx_rearmed_neon_interlace_enable", "Enable interlacing mode(s); disabled|enabled" },
      { "pcsx_rearmed_neon_enhancement_enable", "Enhanced resolution (slow); disabled|enabled" },
      { "pcsx_rearmed_neon_enhancement_no_main", "Enhanced resolution speed hack; disabled|enabled" },
#endif
      { "pcsx_rearmed_duping_enable", "Frame duping; on|off" },
      { "pcsx_rearmed_spu_reverb", "Sound: Reverb; on|off" },
      { "pcsx_rearmed_spu_interpolation", "Sound: Interpolation; simple|gaussian|cubic|off" },
      { NULL, NULL },
   };

   environ_cb = cb;

   cb(RETRO_ENVIRONMENT_SET_VARIABLES, (void*)vars);
}

void retro_set_video_refresh(retro_video_refresh_t cb) { video_cb = cb; }
void retro_set_audio_sample(retro_audio_sample_t cb) { (void)cb; }
void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio_batch_cb = cb; }
void retro_set_input_poll(retro_input_poll_t cb) { input_poll_cb = cb; }
void retro_set_input_state(retro_input_state_t cb) { input_state_cb = cb; }

unsigned retro_api_version(void)
{
	return RETRO_API_VERSION;
}

void retro_set_controller_port_device(unsigned port, unsigned device)
{
}

void retro_get_system_info(struct retro_system_info *info)
{
	memset(info, 0, sizeof(*info));
	info->library_name = "PCSX-ReARMed";
	info->library_version = "r22";
	info->valid_extensions = "bin|cue|img|mdf|pbp|toc|cbn|m3u";
	info->need_fullpath = true;
}

void retro_get_system_av_info(struct retro_system_av_info *info)
{
	memset(info, 0, sizeof(*info));
	info->timing.fps            = is_pal_mode ? 50 : 60;
	info->timing.sample_rate    = 44100;
	info->geometry.base_width   = 320;
	info->geometry.base_height  = 240;
	info->geometry.max_width    = VOUT_MAX_WIDTH;
	info->geometry.max_height   = VOUT_MAX_HEIGHT;
	info->geometry.aspect_ratio = 4.0 / 3.0;
}

/* savestates */
size_t retro_serialize_size(void) 
{ 
	// it's currently 4380651-4397047 bytes,
	// but have some reserved for future
	return 0x440000;
}

struct save_fp {
	char *buf;
	size_t pos;
	int is_write;
};

static void *save_open(const char *name, const char *mode)
{
	struct save_fp *fp;

	if (name == NULL || mode == NULL)
		return NULL;

	fp = malloc(sizeof(*fp));
	if (fp == NULL)
		return NULL;

	fp->buf = (char *)name;
	fp->pos = 0;
	fp->is_write = (mode[0] == 'w' || mode[1] == 'w');

	return fp;
}

static int save_read(void *file, void *buf, u32 len)
{
	struct save_fp *fp = file;
	if (fp == NULL || buf == NULL)
		return -1;

	memcpy(buf, fp->buf + fp->pos, len);
	fp->pos += len;
	return len;
}

static int save_write(void *file, const void *buf, u32 len)
{
	struct save_fp *fp = file;
	if (fp == NULL || buf == NULL)
		return -1;

	memcpy(fp->buf + fp->pos, buf, len);
	fp->pos += len;
	return len;
}

static long save_seek(void *file, long offs, int whence)
{
	struct save_fp *fp = file;
	if (fp == NULL)
		return -1;

	switch (whence) {
	case SEEK_CUR:
		fp->pos += offs;
		return fp->pos;
	case SEEK_SET:
		fp->pos = offs;
		return fp->pos;
	default:
		return -1;
	}
}

static void save_close(void *file)
{
	struct save_fp *fp = file;
	size_t r_size = retro_serialize_size();
	if (fp == NULL)
		return;

	if (fp->pos > r_size)
		SysPrintf("ERROR: save buffer overflow detected\n");
	else if (fp->is_write && fp->pos < r_size)
		// make sure we don't save trash in leftover space
		memset(fp->buf + fp->pos, 0, r_size - fp->pos);
	free(fp);
}

bool retro_serialize(void *data, size_t size)
{ 
	int ret = SaveState(data);
	return ret == 0 ? true : false;
}

bool retro_unserialize(const void *data, size_t size)
{
	int ret = LoadState(data);
	return ret == 0 ? true : false;
}

/* cheats */
void retro_cheat_reset(void)
{
	ClearAllCheats();
}

void retro_cheat_set(unsigned index, bool enabled, const char *code)
{
	char buf[256];
	int ret;

	// cheat funcs are destructive, need a copy..
	strncpy(buf, code, sizeof(buf));
	buf[sizeof(buf) - 1] = 0;

	if (index < NumCheats)
		ret = EditCheat(index, "", buf);
	else
		ret = AddCheat("", buf);

	if (ret != 0)
		SysPrintf("Failed to set cheat %#u\n", index);
	else if (index < NumCheats)
		Cheats[index].Enabled = enabled;
}

/* multidisk support */
static bool disk_ejected;
static unsigned int disk_current_index;
static unsigned int disk_count;
static struct disks_state {
	char *fname;
	int internal_index; // for multidisk eboots
} disks[8];

static bool disk_set_eject_state(bool ejected)
{
	// weird PCSX API..
	SetCdOpenCaseTime(ejected ? -1 : (time(NULL) + 2));
	LidInterrupt();

	disk_ejected = ejected;
	return true;
}

static bool disk_get_eject_state(void)
{
	/* can't be controlled by emulated software */
	return disk_ejected;
}

static unsigned int disk_get_image_index(void)
{
	return disk_current_index;
}

static bool disk_set_image_index(unsigned int index)
{
	if (index >= sizeof(disks) / sizeof(disks[0]))
		return false;

	CdromId[0] = '\0';
	CdromLabel[0] = '\0';

	if (disks[index].fname == NULL) {
		SysPrintf("missing disk #%u\n", index);
		CDR_shutdown();

		// RetroArch specifies "no disk" with index == count,
		// so don't fail here..
		disk_current_index = index;
		return true;
	}

	SysPrintf("switching to disk %u: \"%s\" #%d\n", index,
		disks[index].fname, disks[index].internal_index);

	cdrIsoMultidiskSelect = disks[index].internal_index;
	set_cd_image(disks[index].fname);
	if (ReloadCdromPlugin() < 0) {
		SysPrintf("failed to load cdr plugin\n");
		return false;
	}
	if (CDR_open() < 0) {
		SysPrintf("failed to open cdr plugin\n");
		return false;
	}

	if (!disk_ejected) {
		SetCdOpenCaseTime(time(NULL) + 2);
		LidInterrupt();
	}

	disk_current_index = index;
	return true;
}

static unsigned int disk_get_num_images(void)
{
	return disk_count;
}

static bool disk_replace_image_index(unsigned index,
	const struct retro_game_info *info)
{
	char *old_fname;
	bool ret = true;

	if (index >= sizeof(disks) / sizeof(disks[0]))
		return false;

	old_fname = disks[index].fname;
	disks[index].fname = NULL;
	disks[index].internal_index = 0;

	if (info != NULL) {
		disks[index].fname = strdup(info->path);
		if (index == disk_current_index)
			ret = disk_set_image_index(index);
	}

	if (old_fname != NULL)
		free(old_fname);

	return ret;
}

static bool disk_add_image_index(void)
{
	if (disk_count >= 8)
		return false;

	disk_count++;
	return true;
}

static struct retro_disk_control_callback disk_control = {
	.set_eject_state = disk_set_eject_state,
	.get_eject_state = disk_get_eject_state,
	.get_image_index = disk_get_image_index,
	.set_image_index = disk_set_image_index,
	.get_num_images = disk_get_num_images,
	.replace_image_index = disk_replace_image_index,
	.add_image_index = disk_add_image_index,
};

// just in case, maybe a win-rt port in the future?
#ifdef _WIN32
#define SLASH '\\'
#else
#define SLASH '/'
#endif

static char base_dir[PATH_MAX];

static bool read_m3u(const char *file)
{
	char line[PATH_MAX];
	char name[PATH_MAX];
	FILE *f = fopen(file, "r");
	if (!f)
		return false;

	while (fgets(line, sizeof(line), f) && disk_count < sizeof(disks) / sizeof(disks[0])) {
		if (line[0] == '#')
			continue;
		char *carrige_return = strchr(line, '\r');
		if (carrige_return)
			*carrige_return = '\0';
		char *newline = strchr(line, '\n');
		if (newline)
			*newline = '\0';

		if (line[0] != '\0')
		{
			snprintf(name, sizeof(name), "%s%c%s", base_dir, SLASH, line);
			disks[disk_count++].fname = strdup(name);
		}
	}

	fclose(f);
	return (disk_count != 0);
}

static void extract_directory(char *buf, const char *path, size_t size)
{
   char *base;
   strncpy(buf, path, size - 1);
   buf[size - 1] = '\0';

   base = strrchr(buf, '/');
   if (!base)
      base = strrchr(buf, '\\');

   if (base)
      *base = '\0';
   else
   {
      buf[0] = '.';
      buf[1] = '\0';
   }
}

#if defined(__QNX__) || defined(_WIN32)
/* Blackberry QNX doesn't have strcasestr */

/*
 * Find the first occurrence of find in s, ignore case.
 */
char *
strcasestr(const char *s, const char*find)
{
	char c, sc;
	size_t len;

	if ((c = *find++) != 0) {
		c = tolower((unsigned char)c);
		len = strlen(find);
		do {
			do {
				if ((sc = *s++) == 0)
					return (NULL);
			} while ((char)tolower((unsigned char)sc) != c);
		} while (strncasecmp(s, find, len) != 0);
		s--;
	}
	return ((char *)s);
}
#endif

bool retro_load_game(const struct retro_game_info *info)
{
	size_t i;
	bool is_m3u = (strcasestr(info->path, ".m3u") != NULL);

   struct retro_input_descriptor desc[] = {
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,  "D-Pad Left" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,    "D-Pad Up" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,  "D-Pad Down" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,     "Cross" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,     "Circle" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,     "Triangle" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,     "Square" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,     "L1" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2,    "L2" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L3,    "L3" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,     "R1" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2,    "R2" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R3,    "R3" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT,    "Select" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,    "Start" },
      { 0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X, "Left Analog X" },
      { 0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y, "Left Analog Y" },
      { 0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X, "Right Analog X" },
      { 0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y, "Right Analog Y" },


      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,  "D-Pad Left" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,    "D-Pad Up" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,  "D-Pad Down" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,     "Cross" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,     "Circle" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,     "Triangle" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,     "Square" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,     "L1" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2,    "L2" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L3,    "L3" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,     "R1" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2,    "R2" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R3,    "R3" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT,    "Select" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,    "Start" },
      { 1, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X, "Left Analog X" },
      { 1, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y, "Left Analog Y" },
      { 1, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X, "Right Analog X" },
      { 1, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y, "Right Analog Y" },

      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,  "D-Pad Left" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,    "D-Pad Up" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,  "D-Pad Down" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,     "Cross" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,     "Circle" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,     "Triangle" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,     "Square" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,     "L1" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2,    "L2" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L3,    "L3" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,     "R1" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2,    "R2" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R3,    "R3" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT,    "Select" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,    "Start" },
      { 2, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X, "Left Analog X" },
      { 2, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y, "Left Analog Y" },
      { 2, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X, "Right Analog X" },
      { 2, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y, "Right Analog Y" },

      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,  "D-Pad Left" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,    "D-Pad Up" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,  "D-Pad Down" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,     "Cross" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,     "Circle" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,     "Triangle" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,     "Square" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,     "L1" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2,    "L2" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L3,    "L3" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,     "R1" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2,    "R2" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R3,    "R3" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT,    "Select" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,    "Start" },
      { 3, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X, "Left Analog X" },
      { 3, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y, "Left Analog Y" },
      { 3, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X, "Right Analog X" },
      { 3, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y, "Right Analog Y" },

      { 4, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,  "D-Pad Left" },
      { 4, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,    "D-Pad Up" },
      { 4, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,  "D-Pad Down" },
      { 4, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right" },
      { 4, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,     "Cross" },
      { 4, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,     "Circle" },
      { 4, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,     "Triangle" },
      { 4, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,     "Square" },
      { 4, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,     "L1" },
      { 4, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2,    "L2" },
      { 4, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L3,    "L3" },
      { 4, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,     "R1" },
      { 4, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2,    "R2" },
      { 4, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R3,    "R3" },
      { 4, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT,    "Select" },
      { 4, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,    "Start" },
      { 4, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X, "Left Analog X" },
      { 4, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y, "Left Analog Y" },
      { 4, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X, "Right Analog X" },
      { 4, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y, "Right Analog Y" },

      { 5, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,  "D-Pad Left" },
      { 5, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,    "D-Pad Up" },
      { 5, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,  "D-Pad Down" },
      { 5, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right" },
      { 5, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,     "Cross" },
      { 5, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,     "Circle" },
      { 5, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,     "Triangle" },
      { 5, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,     "Square" },
      { 5, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,     "L1" },
      { 5, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2,    "L2" },
      { 5, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L3,    "L3" },
      { 5, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,     "R1" },
      { 5, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2,    "R2" },
      { 5, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R3,    "R3" },
      { 5, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT,    "Select" },
      { 5, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,    "Start" }, 
      { 5, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X, "Left Analog X" },
      { 5, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y, "Left Analog Y" },
      { 5, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X, "Right Analog X" },
      { 5, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y, "Right Analog Y" },

      { 6, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,  "D-Pad Left" },
      { 6, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,    "D-Pad Up" },
      { 6, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,  "D-Pad Down" },
      { 6, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right" },
      { 6, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,     "Cross" },
      { 6, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,     "Circle" },
      { 6, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,     "Triangle" },
      { 6, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,     "Square" },
      { 6, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,     "L1" },
      { 6, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2,    "L2" },
      { 6, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L3,    "L3" },
      { 6, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,     "R1" },
      { 6, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2,    "R2" },
      { 6, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R3,    "R3" },
      { 6, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT,    "Select" },
      { 6, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,    "Start" }, 
      { 6, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X, "Left Analog X" },
      { 6, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y, "Left Analog Y" },
      { 6, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X, "Right Analog X" },
      { 6, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y, "Right Analog Y" },

      { 7, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,  "D-Pad Left" },
      { 7, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,    "D-Pad Up" },
      { 7, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,  "D-Pad Down" },
      { 7, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right" },
      { 7, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,     "Cross" },
      { 7, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,     "Circle" },
      { 7, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,     "Triangle" },
      { 7, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,     "Square" },
      { 7, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,     "L1" },
      { 7, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2,    "L2" },
      { 7, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L3,    "L3" },
      { 7, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,     "R1" },
      { 7, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2,    "R2" },
      { 7, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R3,    "R3" },
      { 7, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT,    "Select" },
      { 7, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,    "Start" }, 
      { 7, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X, "Left Analog X" },
      { 7, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y, "Left Analog Y" },
      { 7, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X, "Right Analog X" },
      { 7, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y, "Right Analog Y" },

      { 0 },
   };

   environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, desc);

#ifdef FRONTEND_SUPPORTS_RGB565
	enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_RGB565;
	if (environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt)) {
		SysPrintf("RGB565 supported, using it\n");
	}
#endif

	if (info == NULL || info->path == NULL) {
		SysPrintf("info->path required\n");
		return false;
	}

	if (plugins_opened) {
		ClosePlugins();
		plugins_opened = 0;
	}

	for (i = 0; i < sizeof(disks) / sizeof(disks[0]); i++) {
		if (disks[i].fname != NULL) {
			free(disks[i].fname);
			disks[i].fname = NULL;
		}
		disks[i].internal_index = 0;
	}

	disk_current_index = 0;
	extract_directory(base_dir, info->path, sizeof(base_dir));

	if (is_m3u) {
		if (!read_m3u(info->path)) {
			SysPrintf("failed to read m3u file\n");
			return false;
		}
	} else {
		disk_count = 1;
		disks[0].fname = strdup(info->path);
	}

	set_cd_image(disks[0].fname);

	/* have to reload after set_cd_image for correct cdr plugin */
	if (LoadPlugins() == -1) {
		SysPrintf("failed to load plugins\n");
		return false;
	}

	plugins_opened = 1;
	NetOpened = 0;

	if (OpenPlugins() == -1) {
		SysPrintf("failed to open plugins\n");
		return false;
	}

	plugin_call_rearmed_cbs();
	dfinput_activate();

	Config.PsxAuto = 1;
	if (CheckCdrom() == -1) {
		SysPrintf("unsupported/invalid CD image: %s\n", info->path);
		return false;
	}

	SysReset();

	if (LoadCdrom() == -1) {
		SysPrintf("could not load CD-ROM!\n");
		return false;
	}
	emu_on_new_cd(0);

	// multidisk images
	if (!is_m3u) {
		disk_count = cdrIsoMultidiskCount < 8 ? cdrIsoMultidiskCount : 8;
		for (i = 1; i < sizeof(disks) / sizeof(disks[0]) && i < cdrIsoMultidiskCount; i++) {
			disks[i].fname = strdup(info->path);
			disks[i].internal_index = i;
		}
	}

	return true;
}

bool retro_load_game_special(unsigned game_type, const struct retro_game_info *info, size_t num_info)
{
	return false;
}

void retro_unload_game(void) 
{
}

unsigned retro_get_region(void)
{
	return is_pal_mode ? RETRO_REGION_PAL : RETRO_REGION_NTSC;
}

void *retro_get_memory_data(unsigned id)
{
	if (id == RETRO_MEMORY_SAVE_RAM)
		return Mcd1Data;
	else
		return NULL;
}

size_t retro_get_memory_size(unsigned id)
{
	if (id == RETRO_MEMORY_SAVE_RAM)
		return MCD_SIZE;
	else
		return 0;
}

void retro_reset(void)
{
	SysReset();
}

static const unsigned short retro_psx_map[] = {
	[RETRO_DEVICE_ID_JOYPAD_B]	= 1 << DKEY_CROSS,
	[RETRO_DEVICE_ID_JOYPAD_Y]	= 1 << DKEY_SQUARE,
	[RETRO_DEVICE_ID_JOYPAD_SELECT]	= 1 << DKEY_SELECT,
	[RETRO_DEVICE_ID_JOYPAD_START]	= 1 << DKEY_START,
	[RETRO_DEVICE_ID_JOYPAD_UP]	= 1 << DKEY_UP,
	[RETRO_DEVICE_ID_JOYPAD_DOWN]	= 1 << DKEY_DOWN,
	[RETRO_DEVICE_ID_JOYPAD_LEFT]	= 1 << DKEY_LEFT,
	[RETRO_DEVICE_ID_JOYPAD_RIGHT]	= 1 << DKEY_RIGHT,
	[RETRO_DEVICE_ID_JOYPAD_A]	= 1 << DKEY_CIRCLE,
	[RETRO_DEVICE_ID_JOYPAD_X]	= 1 << DKEY_TRIANGLE,
	[RETRO_DEVICE_ID_JOYPAD_L]	= 1 << DKEY_L1,
	[RETRO_DEVICE_ID_JOYPAD_R]	= 1 << DKEY_R1,
	[RETRO_DEVICE_ID_JOYPAD_L2]	= 1 << DKEY_L2,
	[RETRO_DEVICE_ID_JOYPAD_R2]	= 1 << DKEY_R2,
	[RETRO_DEVICE_ID_JOYPAD_L3]	= 1 << DKEY_L3,
	[RETRO_DEVICE_ID_JOYPAD_R3]	= 1 << DKEY_R3,
};
#define RETRO_PSX_MAP_LEN (sizeof(retro_psx_map) / sizeof(retro_psx_map[0]))

static void update_variables(bool in_flight)
{
   struct retro_variable var;
   
   var.value = NULL;
   var.key = "pcsx_rearmed_frameskip";

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) || var.value)
      pl_rearmed_cbs.frameskip = atoi(var.value);

   var.value = NULL;
   var.key = "pcsx_rearmed_region";

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) || var.value)
   {
      Config.PsxAuto = 0;
      if (strcmp(var.value, "Automatic") == 0)
         Config.PsxAuto = 1;
      else if (strcmp(var.value, "NTSC") == 0)
         Config.PsxType = 0;
      else if (strcmp(var.value, "PAL") == 0)
         Config.PsxType = 1;
   }

   var.value = NULL;
   var.key = "pcsx_rearmed_pad1type";

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) || var.value)
   {
      in_type1 = PSE_PAD_TYPE_STANDARD;
      if (strcmp(var.value, "analog") == 0)
         in_type1 = PSE_PAD_TYPE_ANALOGPAD;
   }

   var.value = NULL;
   var.key = "pcsx_rearmed_pad2type";

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) || var.value)
   {
      in_type2 = PSE_PAD_TYPE_STANDARD;
      if (strcmp(var.value, "analog") == 0)
         in_type2 = PSE_PAD_TYPE_ANALOGPAD;
   }

#ifdef __ARM_NEON__
   var.value = "NULL";
   var.key = "pcsx_rearmed_neon_interlace_enable";

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) || var.value)
   {
      if (strcmp(var.value, "disabled") == 0)
         pl_rearmed_cbs.gpu_neon.allow_interlace = 0;
      else if (strcmp(var.value, "enabled") == 0)
         pl_rearmed_cbs.gpu_neon.allow_interlace = 1;
   }

   var.value = NULL;
   var.key = "pcsx_rearmed_neon_enhancement_enable";

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) || var.value)
   {
      if (strcmp(var.value, "disabled") == 0)
         pl_rearmed_cbs.gpu_neon.enhancement_enable = 0;
      else if (strcmp(var.value, "enabled") == 0)
         pl_rearmed_cbs.gpu_neon.enhancement_enable = 1;
   }

   var.value = NULL;
   var.key = "pcsx_rearmed_neon_enhancement_no_main";

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) || var.value)
   {
      if (strcmp(var.value, "disabled") == 0)
         pl_rearmed_cbs.gpu_neon.enhancement_no_main = 0;
      else if (strcmp(var.value, "enabled") == 0)
         pl_rearmed_cbs.gpu_neon.enhancement_no_main = 1;
   }
#endif

   var.value = "NULL";
   var.key = "pcsx_rearmed_duping_enable";

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) || var.value)
   {
      if (strcmp(var.value, "off") == 0)
         duping_enable = false;
      else if (strcmp(var.value, "on") == 0)
         duping_enable = true;
   }

#ifndef DRC_DISABLE
   var.value = NULL;
   var.key = "pcsx_rearmed_drc";

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) || var.value)
   {
      R3000Acpu *prev_cpu = psxCpu;

      if (strcmp(var.value, "disabled") == 0)
         Config.Cpu = CPU_INTERPRETER;
      else if (strcmp(var.value, "enabled") == 0)
         Config.Cpu = CPU_DYNAREC;

      psxCpu = (Config.Cpu == CPU_INTERPRETER) ? &psxInt : &psxRec;
      if (psxCpu != prev_cpu) {
         prev_cpu->Shutdown();
         psxCpu->Init();
         psxCpu->Reset(); // not really a reset..
      }
   }
#endif
   psxCpu->ApplyConfig();

   var.value = "NULL";
   var.key = "pcsx_rearmed_spu_reverb";

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) || var.value)
   {
      if (strcmp(var.value, "off") == 0)
         spu_config.iUseReverb = false;
      else if (strcmp(var.value, "on") == 0)
         spu_config.iUseReverb = true;
   }

   var.value = "NULL";
   var.key = "pcsx_rearmed_spu_interpolation";

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) || var.value)
   {
      if (strcmp(var.value, "simple") == 0)
         spu_config.iUseInterpolation = 1;
      else if (strcmp(var.value, "gaussian") == 0)
         spu_config.iUseInterpolation = 2;
      else if (strcmp(var.value, "cubic") == 0)
         spu_config.iUseInterpolation = 3;
      else if (strcmp(var.value, "off") == 0)
         spu_config.iUseInterpolation = 0;
   }

   if (in_flight) {
      // inform core things about possible config changes
      plugin_call_rearmed_cbs();

      if (GPU_open != NULL && GPU_close != NULL) {
         GPU_close();
         GPU_open(&gpuDisp, "PCSX", NULL);
      }

      dfinput_activate();
   }
}

void retro_run(void) 
{
	int i;

	input_poll_cb();

	bool updated = false;
	if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated)
		update_variables(true);

	in_keystate = 0;
	for (i = 0; i < RETRO_PSX_MAP_LEN; i++)
		if (input_state_cb(1, RETRO_DEVICE_JOYPAD, 0, i))
			in_keystate |= retro_psx_map[i];
	in_keystate <<= 16;
	for (i = 0; i < RETRO_PSX_MAP_LEN; i++)
		if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, i))
			in_keystate |= retro_psx_map[i];

	if (in_type1 == PSE_PAD_TYPE_ANALOGPAD)
	{
		in_a1[0] = (input_state_cb(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X) / 256) + 128;
		in_a1[1] = (input_state_cb(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y) / 256) + 128;
		in_a2[0] = (input_state_cb(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X) / 256) + 128;
		in_a2[1] = (input_state_cb(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y) / 256) + 128;
	}

	stop = 0;
	psxCpu->Execute();

	video_cb((vout_fb_dirty || !vout_can_dupe || !duping_enable) ? vout_buf : NULL,
		vout_width, vout_height, vout_width * 2);
	vout_fb_dirty = 0;
}

static bool try_use_bios(const char *path)
{
	FILE *f;
	long size;
	const char *name;

	f = fopen(path, "rb");
	if (f == NULL)
		return false;

	fseek(f, 0, SEEK_END);
	size = ftell(f);
	fclose(f);

	if (size != 512 * 1024)
		return false;

	name = strrchr(path, SLASH);
	if (name++ == NULL)
		name = path;
	snprintf(Config.Bios, sizeof(Config.Bios), "%s", name);
	return true;
}

#if 1
#include <sys/types.h>
#include <dirent.h>

static bool find_any_bios(const char *dirpath, char *path, size_t path_size)
{
	DIR *dir;
	struct dirent *ent;
	bool ret = false;

	dir = opendir(dirpath);
	if (dir == NULL)
		return false;

	while ((ent = readdir(dir))) {
		if (strncasecmp(ent->d_name, "scph", 4) != 0)
			continue;

		snprintf(path, path_size, "%s/%s", dirpath, ent->d_name);
		ret = try_use_bios(path);
		if (ret)
			break;
	}
	closedir(dir);
	return ret;
}
#else
#define find_any_bios(...) false
#endif

static void check_system_specs(void)
{
   unsigned level = 6;
   environ_cb(RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL, &level);
}

void retro_init(void)
{
	const char *bios[] = { "scph1001", "scph5501", "scph7001" };
	const char *dir;
	char path[256];
	int i, ret;
	bool found_bios = false;

#ifdef __MACH__
	// magic sauce to make the dynarec work on iOS
	syscall(SYS_ptrace, 0 /*PTRACE_TRACEME*/, 0, 0, 0);
#endif

	ret = emu_core_preinit();
	ret |= emu_core_init();
	if (ret != 0) {
		SysPrintf("PCSX init failed.\n");
		exit(1);
	}

#if defined(_POSIX_C_SOURCE) && (_POSIX_C_SOURCE >= 200112L)
	posix_memalign(&vout_buf, 16, VOUT_MAX_WIDTH * VOUT_MAX_HEIGHT * 2);
#else
	vout_buf = malloc(VOUT_MAX_WIDTH * VOUT_MAX_HEIGHT * 2);
#endif

	if (environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &dir) && dir)
	{
		snprintf(Config.BiosDir, sizeof(Config.BiosDir), "%s/", dir);

		for (i = 0; i < sizeof(bios) / sizeof(bios[0]); i++) {
			snprintf(path, sizeof(path), "%s/%s.bin", dir, bios[i]);
			found_bios = try_use_bios(path);
			if (found_bios)
				break;
		}

		if (!found_bios)
			found_bios = find_any_bios(dir, path, sizeof(path));
	}
	if (found_bios) {
		SysPrintf("found BIOS file: %s\n", Config.Bios);
	}
	else
	{
		SysPrintf("no BIOS files found.\n");
		struct retro_message msg = 
		{
			"no BIOS found, expect bugs!",
			180
		};
		environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE, (void*)&msg);
	}

	environ_cb(RETRO_ENVIRONMENT_GET_CAN_DUPE, &vout_can_dupe);
	environ_cb(RETRO_ENVIRONMENT_SET_DISK_CONTROL_INTERFACE, &disk_control);
	environ_cb(RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE, &rumble);

	/* Set how much slower PSX CPU runs * 100 (so that 200 is 2 times)
	 * we have to do this because cache misses and some IO penalties
	 * are not emulated. Warning: changing this may break compatibility. */
	cycle_multiplier = 175;
#ifdef HAVE_PRE_ARMV7
	cycle_multiplier = 200;
#endif
	pl_rearmed_cbs.gpu_peops.iUseDither = 1;
	spu_config.iUseFixedUpdates = 1;

	McdDisable[0] = 0;
	McdDisable[1] = 1;
	init_memcard(Mcd1Data);

	SaveFuncs.open = save_open;
	SaveFuncs.read = save_read;
	SaveFuncs.write = save_write;
	SaveFuncs.seek = save_seek;
	SaveFuncs.close = save_close;

	update_variables(false);
	check_system_specs();
}

void retro_deinit(void)
{
	SysClose();
	free(vout_buf);
	vout_buf = NULL;
}
