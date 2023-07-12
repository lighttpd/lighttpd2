#include <lighttpd/settings.h>


#if defined(LIGHTY_OS_LINUX)
#include <fcntl.h>

gsize li_memory_usage(void) {
	/* parse /proc/self/stat */
	int d;
	gchar s[PATH_MAX];
	gchar c;
	unsigned int u;
	long unsigned int lu;
	long int ld;
	long long unsigned int llu;
	gsize rss;
	FILE *f;

	f = fopen("/proc/self/stat", "r");
	if (!f)
		return 0;

	/* fields according to proc(5) */
	d = fscanf(
		f,
		"%d %s %c %d %d %d %d %d %u %lu %lu %lu %lu %lu %lu %ld %ld %ld %ld %ld %ld %lu %lu"
		" %ld %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %d %d %u %u %llu %lu %ld",
		&d, s, &c, &d, &d, &d, &d, &d, &u, &lu, &lu, &lu, &lu, &lu, &lu, &ld, &ld, &ld, &ld, &ld, &ld, &lu, &lu,
		(long int*)&rss, &lu, &lu, &lu, &lu, &lu, &lu, &lu, &lu, &lu, &lu, &lu, &lu, &lu, &d, &d, &u, &u, &llu, &lu, &ld
	);

	fclose(f);

	/* rss is the 24th field */
	if (d < 24)
		return 0;

	return rss * getpagesize();
}

#elif defined(LIGHTY_OS_FREEBSD)
#include <paths.h>
#include <fcntl.h>
#include <kvm.h>
#include <sys/param.h>
#include <sys/sysctl.h>
#include <sys/user.h>

gsize li_memory_usage(void) {
	kvm_t *kvm;
	struct kinfo_proc *ki_proc;
	gint cnt;
	gsize rss;

	kvm = kvm_open(NULL, _PATH_DEVNULL, NULL, O_RDONLY, "kvm_open");
	if (!kvm)
		return 0;

	ki_proc = kvm_getprocs(kvm, KERN_PROC_PID, getpid(), &cnt);
	if (!ki_proc) {
		kvm_close(kvm);
		return 0;
	}

	rss = ki_proc->ki_rssize * getpagesize();

	kvm_close(kvm);

	return rss;
}
#elif defined(LIGHTY_OS_MACOSX)
#include <mach/task.h>
#include <mach/mach_init.h>

gsize li_memory_usage(void) {
	/* info from https://miknight.blogspot.com/2005/11/resident-set-size-in-mac-os-x.html */
	struct task_basic_info tbinfo;
	mach_msg_type_number_t cnt = TASK_BASIC_INFO_COUNT;

	if (KERN_SUCCESS != task_info(mach_task_self(), TASK_BASIC_INFO, (task_info_t) &tbinfo, &cnt))
		return 0;

	return tbinfo.resident_size;
}

#elif defined(LIGHTY_OS_SOLARIS)

gsize li_memory_usage(void) {
	/* /proc/$pid/psinfo: http://docs.sun.com/app/docs/doc/816-5174/proc-4?l=en&a=view */
	gchar path[64];
	psinfo_t psinfo;
	FILE *f;

	sprintf(path, "/proc/%d/psinfo", getpid());

	f = fopen(path, "r");
	if (!f)
		return 0;

	if (fread(&psinfo, sizeof(psinfo_t), 1, f) != 1) {
		fclose(f);
		return 0;
	}

	return psinfo.pr_rssize * 1024;
}

#else

gsize li_memory_usage(void) {
	/* unsupported OS */
	return 0;
}

#endif
