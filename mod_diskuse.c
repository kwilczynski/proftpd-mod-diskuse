/*
 * ProFTPD: mod_diskuse -- a module for refusing uploads based on disk usage
 *
 * Copyright (c) 2002-2011 TJ Saunders
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307, USA.
 *
 * As a special exemption, TJ Saunders gives permission to link this program
 * with OpenSSL, and distribute the resulting executable, without including
 * the source code for OpenSSL in the source distribution.
 *
 * This is mod_diskuse, contrib software for proftpd 1.2 and above.
 * For more information contact TJ Saunders <tj@castaglia.org>.
 *
 * $Id: mod_diskuse.c,v 1.7 2011/01/18 19:13:09 tj Exp tj $
 */

#include "conf.h"
#include "mod_diskuse.h"

/* Make sure the version of proftpd is as necessary. */
#if PROFTPD_VERSION_NUMBER < 0x0001030201
# error "ProFTPD 1.3.2rc1 or later required"
#endif

#if defined(HAVE_SYS_PARAM_H)
# include <sys/param.h>
#endif

#if defined(HAVE_SYS_MOUNT_H)
# include <sys/mount.h>
#endif

#if defined(HAVE_SYS_VFS_H)
# include <sys/vfs.h>
#endif

#if defined(HAVE_SYS_STATVFS_H)
# include <sys/statvfs.h>
#endif

#define MOD_DISKUSE_VERSION		"mod_diskuse/0.9"

static unsigned char have_max_diskuse = FALSE;
static double min_diskfree = 0.0;
static double current_diskfree = 0.0;

/* Support routines
 */

static void lookup_current_diskfree(const char *path) {
#if defined(HAVE_SYS_STATVFS_H) || defined(HAVE_SYS_VFS_H) || defined(HAVE_STATFS)
  char *tmp = NULL;

# if defined(HAVE_SYS_STATVFS_H)
  struct statvfs fsbuf;
# elif defined(HAVE_SYS_VFS_H)
  struct statfs fsbuf;
# elif defined(HAVE_STATFS)
  struct statfs fsbuf;
# endif

  /* backtrack up the path name to find the parent directory */
  tmp = strrchr(path, '/');
  if (tmp == NULL) {
    pr_log_debug(DEBUG0, MOD_DISKUSE_VERSION ": no slash in path '%s'", path);

  } else {
    *++tmp = '\0';
  }

# if defined(HAVE_SYS_STATVFS_H)
  if (statvfs(path, &fsbuf) < 0) {
# elif defined(HAVE_SYS_VFS_H)
  if (statfs(path, &fsbuf) < 0) {
# elif defined(HAVE_STATFS)
  if (statfs(path, &fsbuf) < 0) {
# endif
    pr_log_debug(DEBUG2, MOD_DISKUSE_VERSION ": unable to stat fs '%s': %s",
      path, strerror(errno));
    current_diskfree = -1.0;

  } else {
    current_diskfree = ((double) fsbuf.f_bavail / (double) fsbuf.f_blocks);
  }
#else 
  current_diskuse = -1.0;
#endif /* !HAVE_SYS_STATVFS_H || !HAVE_SYS_VFS_H || !HAVE_STATFS */
}

static void lookup_min_diskfree(void) {
  config_rec *c = NULL;

  /* No need to lookup the limit again */
  if (have_max_diskuse)
    return;

  c = find_config(TOPLEVEL_CONF, CONF_PARAM, "MaxDiskUsage", FALSE);
  while (c) {

    if (c->argc == 4) {
      if (strcmp(c->argv[2], "user") == 0) {
        if (pr_user_or_expression((char **) &c->argv[3])) {
          min_diskfree = *((double *) c->argv[1]);
          have_max_diskuse = TRUE;

          if (min_diskfree >= 0.0) {
            pr_log_debug(DEBUG5, MOD_DISKUSE_VERSION
              ": MaxDiskUsage (%s%%) in effect for current user",
              (char *) c->argv[0]);
          }

          return;
        }

      } else if (strcmp(c->argv[2], "group") == 0) {
        if (pr_group_and_expression((char **) &c->argv[3])) {
          min_diskfree = *((double *) c->argv[1]);
          have_max_diskuse = TRUE;

          if (min_diskfree >= 0.0) {
            pr_log_debug(DEBUG5, MOD_DISKUSE_VERSION
              ": MaxDiskUsage (%s%%) in effect for current group",
              (char *) c->argv[0]);
          }

          return;
        }

      } else if (strcmp(c->argv[2], "class") == 0) {
        if (pr_class_or_expression((char **) &c->argv[3])) {
          min_diskfree = *((double *) c->argv[1]);
          have_max_diskuse = TRUE;

          if (min_diskfree >= 0.0) {
            pr_log_debug(DEBUG5, MOD_DISKUSE_VERSION
              ":  MaxDiskUsage (%s%%) in effect for current class",
              (char *) c->argv[0]);
          }

          return;
        }
      }

      c = find_config_next(c, c->next, CONF_PARAM, "MaxDiskUsage", FALSE);

    } else {
      min_diskfree = *((double *) c->argv[1]);
      have_max_diskuse = TRUE;

      if (min_diskfree >= 0.0) {
        pr_log_debug(DEBUG5, MOD_DISKUSE_VERSION
          ": MaxDiskUsage (%s%%) in effect", (char *) c->argv[0]);
      }

      return;
    }
  }
}

/* Configuration handlers
 */

MODRET set_maxdiskusage(cmd_rec *cmd) {
#if defined(HAVE_STATFS) || defined(HAVE_STATVFS)
  config_rec *c = NULL;
  double diskuse = 0.0, diskfree = 0.0;
  char *endp = NULL;

  if (cmd->argc-1 != 1 &&
      cmd->argc-1 != 3) {
    CONF_ERROR(cmd, "incorrect number of parameters");
  }

  CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL|CONF_ANON);

  /* Verify the additional classifiers, if present */
  if (cmd->argc-1 == 3) {
    if (strcmp(cmd->argv[2], "user") == 0 ||
        strcmp(cmd->argv[2], "group") == 0 ||
        strcmp(cmd->argv[2], "class") == 0) {

      /* no-op */

    } else {
      CONF_ERROR(cmd, pstrcat(cmd->tmp_pool, "unknown classifier used: '",
        cmd->argv[2], "'", NULL)); 
    }
  }

  /* Parse the percentage parameter.  Check for a "none" argument, which is
   * used to nullify inherited limits.
   */
  if (strcasecmp(cmd->argv[1], "none") != 0) {
    diskuse = strtod(cmd->argv[1], &endp);

    if (endp && *endp) {
      CONF_ERROR(cmd, pstrcat(cmd->tmp_pool, "'", cmd->argv[1],
        "' is not a valid percentage", NULL));
    }

    diskfree = 1.0 - (diskuse / 100.0);

    if (diskfree < 0.0 ||
        diskfree >= 1.0) {
      CONF_ERROR(cmd, "percent must be greater than 0, less than 100");
    }

  } else {
    diskfree = -1.0;
  }

  if (cmd->argc-1 == 1) {
    c = add_config_param(cmd->argv[0], 2, NULL, NULL);
    c->argv[0] = pstrdup(c->pool, cmd->argv[1]);
    c->argv[1] = pcalloc(c->pool, sizeof(double));
    *((double *) c->argv[1]) = diskfree;

  } else {
    array_header *acl = NULL;
    int argc = cmd->argc - 3;
    char **argv = cmd->argv + 2;

    acl = pr_parse_expression(cmd->tmp_pool, &argc, argv);

    c = add_config_param(cmd->argv[0], 0);
    c->argc = argc + 3;

    c->argv = pcalloc(c->pool, ((argc + 4) * sizeof(char *)));
    argv = (char **) c->argv;

    *argv++ = pstrdup(c->pool, cmd->argv[1]);

    *argv = pcalloc(c->pool, sizeof(double));
    *((double *) *argv++) = diskfree;

    *argv++ = pstrdup(c->pool, cmd->argv[2]);

    if (argc && acl) {
      while (argc--) {
        *argv++ = pstrdup(c->pool, *((char **) acl->elts));
        acl->elts = ((char **) acl->elts) + 1;
      }
    }

    /* don't forget the terminating NULL */
    *argv = NULL;
  }

  c->flags |= CF_MERGEDOWN_MULTI;
  return PR_HANDLED(cmd);

#else /* no HAVE_STATFS or HAVE_STATVFS */
  CONF_ERROR(cmd, pstrcat(cmd->tmp_pool, "The ", cmd->argv[0],
    " directive cannot be used on this system, as it does not have statfs(2) "
    "or statvfs(2)"));
#endif
}

/* Command handlers
 */

MODRET diskuse_pre_stor(cmd_rec *cmd) {

  lookup_min_diskfree();
  lookup_current_diskfree(dir_canonical_vpath(cmd->tmp_pool, cmd->arg));

  if (current_diskfree >= 0.0 &&
      min_diskfree >= 0.0 &&
      current_diskfree < min_diskfree) {
    pr_log_pri(PR_LOG_NOTICE, MOD_DISKUSE_VERSION
      ": %s denied to user %s (max usage %f, currently %f)",
      cmd->argv[0], session.user, (1.0 - min_diskfree),
      (1.0 - current_diskfree));
    pr_response_add_err(R_552, _("Insufficient disk space"));
    return PR_ERROR(cmd);
  }

  return PR_DECLINED(cmd);
}

/* Event handlers
 */

#if defined(PR_SHARED_MODULE)
static void diskuse_mod_unload_ev(const void *event_data, void *user_data) {
  if (strcmp("mod_diskuse.c", (const char *) event_data) == 0) {
    pr_event_unregister(&diskuse_module, NULL, NULL);
  }
}
#endif

/* Module initialization
 */

static int diskuse_init(void) {

#if defined(PR_SHARED_MODULE)
  pr_event_register(&diskuse_module, "core.module-unload",
    diskuse_mod_unload_ev, NULL);
#endif

  return 0;
}

/* Module API tables
 */

static conftable diskuse_conftab[] = {
  { "MaxDiskUsage",	set_maxdiskusage,	NULL },
  { NULL }
};

static cmdtable diskuse_cmdtab[] = {
  { PRE_CMD, C_APPE, G_NONE, diskuse_pre_stor, FALSE, FALSE },
  { PRE_CMD, C_STOR, G_NONE, diskuse_pre_stor, FALSE, FALSE },
  { PRE_CMD, C_STOU, G_NONE, diskuse_pre_stor, FALSE, FALSE },
  { 0, NULL }
};

module diskuse_module = {
  NULL, NULL,

  /* Module API version 2.0 */
  0x20,

  /* the module name */
  "diskuse",

  /* module configuration handler table */
  diskuse_conftab,

  /* module command handler table */
  diskuse_cmdtab,

  /* Module authentication handler table */
  NULL,

  /* Module initialization */
  diskuse_init,

  /* Module session initialization */
  NULL,

  /* Module version */
  MOD_DISKUSE_VERSION
};
