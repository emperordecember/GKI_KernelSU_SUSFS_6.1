// SPDX-License-Identifier: GPL-2.0
/*
 * Standalone SUSFS supercall bridge for kernels built without KernelSU.
 *
 * The regular SUSFS integration expects KernelSU to provide the reboot
 * supercall dispatcher (ksu_handle_sys_reboot) and a handful of KSU hooks.
 * This file supplies a minimal in-kernel implementation of those symbols so
 * that an otherwise clean "no root manager" kernel can still host SUSFS.
 * KernelSU-specific behaviour (manager fd installation, seccomp tweaks,
 * SELinux hiding) is deliberately left disabled.
 */

#include <linux/cred.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/jump_label.h>
#include <linux/kernel.h>
#include <linux/mm_types.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/security.h>
#include <linux/types.h>
#include <linux/uidgid.h>
#include <linux/workqueue.h>
#include <linux/susfs.h>

#define KSU_INSTALL_MAGIC1 0xDEADBEEF

/* Referenced by fs/susfs.c and by the hooks added by the SUSFS patch. */
struct static_key_true ksu_is_input_hook_enabled = { .key = STATIC_KEY_INIT_FALSE };
struct static_key_true ksu_su_compat_enabled = { .key = STATIC_KEY_INIT_FALSE };
struct static_key_true ksu_is_init_rc_hook_enabled = { .key = STATIC_KEY_INIT_FALSE };
bool ksu_selinux_hide_enabled __read_mostly = false;
bool ksu_selinux_hide_running __read_mostly = false;

u32 susfs_ksu_sid = 0;
u32 susfs_priv_app_sid = 0;

struct cred *ksu_cred = NULL;
struct page *fake_status = NULL;
DEFINE_STATIC_KEY_FALSE(fake_status_initialize_key);

extern struct work_struct susfs_extra_works;

/* ------------------------------------------------------------------ */
/* KernelSU hook stubs                                                */
/* ------------------------------------------------------------------ */

__attribute__((cold))
int ksu_handle_input_handle_event(unsigned int *type, unsigned int *code, int *value)
{
	return 0;
}

__attribute__((cold))
void ksu_handle_sys_read(unsigned int fd)
{
}

bool __ksu_is_allow_uid_for_current(uid_t uid)
{
	return false;
}

int ksu_handle_execveat(int *fd, struct filename **filename_ptr, void *argv,
			void *envp, int *flags)
{
	return 0;
}

int ksu_handle_execveat_sucompat(int *fd, struct filename **filename_ptr,
				 void *argv, void *envp, int *flags)
{
	return 0;
}

int ksu_handle_post_execveat_sucompat(int *fd, struct filename **filename_ptr,
				      void *argv, void *envp, int *flags,
				      int *retval)
{
	return 0;
}

int ksu_handle_faccessat(int *dfd, struct filename **filename, int *mode,
			 int *__unused_flags)
{
	return 0;
}

int ksu_handle_stat(int *dfd, struct filename **filename, int *flags)
{
	return 0;
}

void ksu_handle_vfs_fstat(int fd, loff_t *kstat_size_ptr)
{
}

int my_setprocattr(const char *name, void *value, size_t size)
{
	return -EINVAL;
}

ssize_t my_write_access(struct file *file, char *buf, size_t size)
{
	return -EINVAL;
}

ssize_t my_write_context(struct file *file, char *buf, size_t size)
{
	return -EINVAL;
}

void initialize_fake_status(void)
{
}

void setup_selinux(const char *domain, struct cred *cred)
{
}

/*
 * Called from __sys_setresuid() before the transition.  KSU uses this to
 * re-apply sus_path loop entries when zygote spawns a new app process; for
 * standalone we only care about a root process transitioning, which is rare
 * enough for a plain schedule_work().
 */
int ksu_handle_setresuid(uid_t ruid, uid_t euid, uid_t suid)
{
	if (current_uid().val != 0)
		return 0;

	if (!work_pending(&susfs_extra_works))
		schedule_work(&susfs_extra_works);

	return 0;
}

/* ------------------------------------------------------------------ */
/* Domain detection and reboot supercall                              */
/* ------------------------------------------------------------------ */

static bool susfs_current_sid(u32 *sid)
{
	*sid = 0;
	security_cred_getsecid(current_cred(), sid);
	return *sid != 0;
}

bool susfs_is_current_ksu_domain(void)
{
	u32 sid;

	if (!susfs_ksu_sid)
		return false;
	if (!susfs_current_sid(&sid))
		return false;

	return sid == susfs_ksu_sid;
}

int ksu_handle_sys_reboot(int magic1, int magic2, unsigned int cmd,
			  void __user **arg)
{
	u32 sid;

	if (magic1 != KSU_INSTALL_MAGIC1)
		return -EINVAL;
	if (magic2 != SUSFS_MAGIC || current_uid().val != 0)
		return -EINVAL;

	/*
	 * The first SUSFS request comes from the root manager/su process.
	 * Remember its SELinux domain so mount hiding and open-redirect can
	 * distinguish it from other uid 0 tasks (notably init).
	 */
	if (!susfs_ksu_sid && susfs_current_sid(&sid))
		susfs_ksu_sid = sid;

	switch (cmd) {
#ifdef CONFIG_KSU_SUSFS_SUS_PATH
	case CMD_SUSFS_ADD_SUS_PATH:
		susfs_add_sus_path(arg);
		return 0;
	case CMD_SUSFS_ADD_SUS_PATH_LOOP:
		susfs_add_sus_path_loop(arg);
		return 0;
#endif
#ifdef CONFIG_KSU_SUSFS_SUS_MOUNT
	case CMD_SUSFS_HIDE_SUS_MNTS_FOR_NON_SU_PROCS:
		susfs_set_hide_sus_mnts_for_non_su_procs(arg);
		return 0;
#endif
#ifdef CONFIG_KSU_SUSFS_SUS_KSTAT
	case CMD_SUSFS_ADD_SUS_KSTAT:
		susfs_add_sus_kstat(arg);
		return 0;
	case CMD_SUSFS_UPDATE_SUS_KSTAT:
		susfs_update_sus_kstat(arg);
		return 0;
	case CMD_SUSFS_ADD_SUS_KSTAT_STATICALLY:
		susfs_add_sus_kstat(arg);
		return 0;
#endif
#ifdef CONFIG_KSU_SUSFS_SPOOF_UNAME
	case CMD_SUSFS_SET_UNAME:
		susfs_set_uname(arg);
		return 0;
#endif
#ifdef CONFIG_KSU_SUSFS_ENABLE_LOG
	case CMD_SUSFS_ENABLE_LOG:
		susfs_enable_log(arg);
		return 0;
#endif
#ifdef CONFIG_KSU_SUSFS_SPOOF_CMDLINE_OR_BOOTCONFIG
	case CMD_SUSFS_SET_CMDLINE_OR_BOOTCONFIG:
		susfs_set_cmdline_or_bootconfig(arg);
		return 0;
#endif
#ifdef CONFIG_KSU_SUSFS_OPEN_REDIRECT
	case CMD_SUSFS_ADD_OPEN_REDIRECT:
		susfs_add_open_redirect(arg);
		return 0;
#endif
#ifdef CONFIG_KSU_SUSFS_SUS_MAP
	case CMD_SUSFS_ADD_SUS_MAP:
		susfs_add_sus_map(arg);
		return 0;
#endif
#ifdef CONFIG_KSU_SUSFS_PRIORITY_UID_FILTER
	case CMD_SUSFS_SET_PRIORITY_UID_FILTER:
		susfs_set_priority_uid_filter(arg);
		return 0;
#endif
	case CMD_SUSFS_ENABLE_AVC_LOG_SPOOFING:
		susfs_set_avc_log_spoofing(arg);
		return 0;
	case CMD_SUSFS_SHOW_ENABLED_FEATURES:
		susfs_get_enabled_features(arg);
		return 0;
	case CMD_SUSFS_SHOW_VARIANT:
		susfs_show_variant(arg);
		return 0;
	case CMD_SUSFS_SHOW_VERSION:
		susfs_show_version(arg);
		return 0;
	default:
		return -EINVAL;
	}
}

static int __init susfs_standalone_init(void)
{
	/*
	 * sus_path loop work runs with these creds; KernelSU normally provides
	 * a ksu domain cred.  For standalone the kernel creds are sufficient
	 * because the work only re-flags inode mappings.
	 */
	ksu_cred = prepare_kernel_cred(NULL);
	if (!ksu_cred)
		pr_warn("SUSFS standalone: failed to prepare ksu_cred\n");

	susfs_init();

	pr_info("SUSFS standalone: supercall bridge ready\n");
	return 0;
}
late_initcall(susfs_standalone_init);
