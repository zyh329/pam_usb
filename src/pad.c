/*
 * Copyright (c) 2003-2006 Andrea Luzzardi <scox@sig11.org>
 *
 * This file is part of the pam_usb project. pam_usb is free software;
 * you can redistribute it and/or modify it under the terms of the GNU General
 * Public License version 2, as published by the Free Software Foundation.
 *
 * pam_usb is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc., 59 Temple
 * Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <pwd.h>
#include <time.h>
#include <libhal-storage.h>
#include "conf.h"
#include "log.h"
#include "volume.h"
#include "pad.h"

static FILE	*pusb_pad_open_device(t_pusb_options *opts,
				      LibHalVolume *volume,
				      const char *user,
				      const char *mode)
{
  FILE		*f;
  char		*path;
  size_t	path_size;
  const char	*mnt_point;

  mnt_point = (char *)libhal_volume_get_mount_point(volume);
  if (!mnt_point)
    return (NULL);
  path_size = strlen(mnt_point) + 1 + strlen(opts->device_pad_directory) +
    1 + strlen(user) + 1 + strlen(opts->hostname) + strlen(".pad") + 1;
  if (!(path = malloc(path_size)))
    {
      log_error("malloc error!\n");
      return (NULL);
    }
  memset(path, 0x00, path_size);
  snprintf(path, path_size, "%s/%s/%s.%s.pad", mnt_point,
	   opts->device_pad_directory, user, opts->hostname);
  f = fopen(path, mode);
  free(path);
  if (!f)
    {
      log_debug("Cannot open device file: %s\n", strerror(errno));
      return (NULL);
    }
  return (f);
}

static int	pusb_pad_protect(const char *user, int fd)
{
  struct passwd	*user_ent = NULL;

  log_debug("Protecting pad file...\n");
  if (!(user_ent = getpwnam(user)))
    {
      log_error("Unable to retrieve informations for user \"%s\": %s\n",
		strerror(errno));
      return (0);
    }
  if (fchown(fd, user_ent->pw_uid, user_ent->pw_gid) == -1)
    {
      log_error("Unable to change owner of the pad: %s\n",
		strerror(errno));
      return (0);
    }
  if (fchmod(fd, S_IRUSR | S_IWUSR) == -1)
    {
      log_error("Unable to change mode of the pad: %s\n",
		strerror(errno));
      return (0);
    }
  return (1);
}

static FILE	*pusb_pad_open_system(t_pusb_options *opts,
				      const char *user,
				      const char *mode)
{
  FILE		*f;
  char		*path;
  size_t	path_size;

  path_size = strlen(opts->system_pad_directory) + 1 +
    strlen(user) + 1 + strlen(opts->device.name) + strlen(".pad") + 1;
  if (!(path = malloc(path_size)))
    {
      log_error("malloc error\n");
      return (NULL);
    }
  memset(path, 0x00, path_size);
  snprintf(path, path_size, "%s/%s.%s.pad", opts->system_pad_directory,
	   user, opts->device.name);
  f = fopen(path, mode);
  free(path);
  if (!f)
    {
      log_debug("Cannot open system file: %s\n", strerror(errno));
      return (NULL);
    }
  return (f);
}

static void	pusb_pad_update(t_pusb_options *opts,
				LibHalVolume *volume,
				const char *user)
{
  FILE		*f_device = NULL;
  FILE		*f_system = NULL;
  char		magic[1024];
  int		i;

  if (!(f_device = pusb_pad_open_device(opts, volume, user, "w+")))
    {
      log_error("Unable to update pads.\n");
      return ;
    }
  pusb_pad_protect(user, fileno(f_device));
  if (!(f_system = pusb_pad_open_system(opts, user, "w+")))
    {
      log_error("Unable to update pads.\n");
      fclose(f_device);
      return ;
    }
  pusb_pad_protect(user, fileno(f_system));
  log_debug("Generating %d bytes unique pad...\n", sizeof(magic));
  srand(getpid() * time(NULL));
  for (i = 0; i < sizeof(magic); ++i)
    magic[i] = (char)rand();
  log_debug("Writing pad to the device...\n");
  fwrite(magic, sizeof(char), sizeof(magic), f_system);
  log_debug("Writing pad to the system...\n");
  fwrite(magic, sizeof(char), sizeof(magic), f_device);
  log_debug("Synchronizing filesystems...\n");
  fclose(f_system);
  fclose(f_device);
  sync();
  log_debug("One time pads updated.\n");
}

static int	pusb_pad_compare(t_pusb_options *opts, LibHalVolume *volume,
				 const char *user)
{
  FILE		*f_device = NULL;
  FILE		*f_system = NULL;
  char		magic_device[1024];
  char		magic_system[1024];
  int		retval;

  if (!(f_system = pusb_pad_open_system(opts, user, "r")))
    return (0);
  if (!(f_device = pusb_pad_open_device(opts, volume, user, "r")))
    {
      fclose(f_system);
      return (0);
    }
  log_debug("Loading device pad...\n");
  fread(magic_device, sizeof(char), sizeof(magic_device), f_device);
  log_debug("Loading system pad...\n");
  fread(magic_system, sizeof(char), sizeof(magic_system), f_system);
  retval = memcmp(magic_system, magic_device, sizeof(magic_system));
  fclose(f_system);
  fclose(f_device);
  return (retval == 0);
}

int		pusb_pad_check(t_pusb_options *opts, LibHalContext *ctx,
			       const char *user)
{
  LibHalVolume	*volume = NULL;
  int		retval;

  volume = pusb_volume_get(opts, ctx);
  if (!volume)
    return (0);
  retval = pusb_pad_compare(opts, volume, user);
  if (retval)
    {
      log_info("Verification match, updating one time pads...\n");
      pusb_pad_update(opts, volume, user);
    }
  else
    log_error("Pad checking failed !\n");
  pusb_volume_destroy(volume);
  return (retval);
}
