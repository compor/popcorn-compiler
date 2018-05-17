#include <stdio.h>
#include <unistd.h>
#include <assert.h>
#include <pthread.h>
#include <stack_transform.h>
#include "platform.h"
#include "migrate.h"
#include "config.h"
#include "arch.h"
#include "internal.h"
#include "mapping.h"
#include "debug.h"

#if _SIG_MIGRATION == 1
#include "trigger.h"
#endif

#if _TIME_REWRITE == 1
#include "timer.h"
#endif

/* Thread migration status information. */
struct popcorn_thread_status {
	int current_nid;
	int proposed_nid;
	int peer_nid;
	int peer_pid;
} status;

#if _ENV_SELECT_MIGRATE == 1

/*
 * The user can specify at which point a thread should migrate by specifying
 * program counter address ranges via environment variables.
 */

/* Environment variables specifying at which function to migrate */
static const char *env_start_aarch64 = "AARCH64_MIGRATE_START";
static const char *env_end_aarch64 = "AARCH64_MIGRATE_END";
static const char *env_start_powerpc64 = "POWERPC64_MIGRATE_START";
static const char *env_end_powerpc64 = "POWERPC64_MIGRATE_END";
static const char *env_start_x86_64 = "X86_64_MIGRATE_START";
static const char *env_end_x86_64 = "X86_64_MIGRATE_END";

/* Per-arch functions (specified via address range) at which to migrate */
static void *start_aarch64 = NULL;
static void *end_aarch64 = NULL;
static void *start_powerpc64 = NULL;
static void *end_powerpc64 = NULL;
static void *start_x86_64 = NULL;
static void *end_x86_64 = NULL;

/* TLS keys indicating if the thread has previously migrated */
static pthread_key_t num_migrated_aarch64 = 0;
static pthread_key_t num_migrated_powerpc64 = 0;
static pthread_key_t num_migrated_x86_64 = 0;

/* Read environment variables to setup migration points. */
static void __attribute__((constructor))
__init_migrate_testing(void)
{
  const char *start;
  const char *end;

#ifdef __aarch64__
  start = getenv(env_start_aarch64);
  end = getenv(env_end_aarch64);
  if(start && end)
  {
    start_aarch64 = (void *)strtoll(start, NULL, 16);
    end_aarch64 = (void *)strtoll(end, NULL, 16);
    if(start_aarch64 && end_aarch64)
      pthread_key_create(&num_migrated_aarch64, NULL);
  }
#elif defined(__powerpc64__)
  start = getenv(env_start_powerpc64);
  end = getenv(env_end_powerpc64);
  if(start && end)
  {
    start_powerpc64 = (void *)strtoll(start, NULL, 16);
    end_powerpc64 = (void *)strtoll(end, NULL, 16);
    if(start_powerpc64 && end_powerpc64)
      pthread_key_create(&num_migrated_powerpc64, NULL);
  }
#else
  start = getenv(env_start_x86_64);
  end = getenv(env_end_x86_64);
  if(start && end)
  {
    start_x86_64 = (void *)strtoll(start, NULL, 16);
    end_x86_64 = (void *)strtoll(end, NULL, 16);
    if(start_x86_64 && end_x86_64)
      pthread_key_create(&num_migrated_x86_64, NULL);
  }
#endif
}

/*
 * Check environment variables to see if this call site is the function at
 * which we should migrate.
 */
static inline int do_migrate(void *addr)
{
  int retval = -1;
#ifdef __aarch64__
  if(start_aarch64 && !pthread_getspecific(num_migrated_aarch64)) {
    if(start_aarch64 <= addr && addr < end_aarch64) {
      pthread_setspecific(num_migrated_aarch64, (void *)1);
      retval = 0;
    }
  }
#elif defined(__powerpc64__)
  if(start_powerpc64 && !pthread_getspecific(num_migrated_powerpc64)) {
    if(start_powerpc64 <= addr && addr < end_powerpc64) {
      pthread_setspecific(num_migrated_powerpc64, (void *)1);
      retval = 1;
    }
  }
#else
  if(start_x86_64 && !pthread_getspecific(num_migrated_x86_64)) {
    if(start_x86_64 <= addr && addr < end_x86_64) {
      pthread_setspecific(num_migrated_x86_64, (void *)1);
      retval = 2;
    }
  }
#endif
  return retval;
}

#else /* _ENV_SELECT_MIGRATE */

static inline int do_migrate(void __attribute__((unused)) *fn)
{
	struct popcorn_thread_status status;
	if (syscall(SYSCALL_GET_THREAD_STATUS, &status)) return -1;

	return status.proposed_nid;
}

#endif /* _ENV_SELECT_MIGRATE */

static struct node_info {
  unsigned int status;
  int arch;
  int distance;
} ni[MAX_POPCORN_NODES];

int node_available(int nid)
{
  if(nid < 0 || nid >= MAX_POPCORN_NODES) return 0;
  else return ni[nid].status;
}

enum arch current_arch(void)
{
	int nid = current_nid();
	if (nid < 0 || nid >= MAX_POPCORN_NODES) return ARCH_UNKNOWN;

	return ni[nid].arch;
}

int current_nid(void)
{
	struct popcorn_thread_status status;
	if (syscall(SYSCALL_GET_THREAD_STATUS, &status)) return -1;

	return status.current_nid;
}

// Note: not static, so if other libraries depend on querying node information
// in constructors (e.g., libopenpop) they can *secretly* declare & call this
// themselves.  We won't expose the function declaration though.
void __attribute__((constructor)) __init_nodes_info(void)
{
	int ret;
	int origin_nid = -1;

	ret = syscall(SYSCALL_GET_NODE_INFO, &origin_nid, ni);
	if (ret)
	{
		perror("Cannot retrieve Popcorn node information");
		for (int i = 0; i < MAX_POPCORN_NODES; i++)
		{
			ni[i].status = 0;
			ni[i].arch = ARCH_UNKNOWN;
			ni[i].distance = -1;
		}
		set_default_node(-1);
	}
	else set_default_node(origin_nid);
}

/* Data needed post-migration. */
struct shim_data {
  void (*callback)(void *);
  void *callback_data;
  void *regset;
  void *post_syscall;
};

#if _DEBUG == 1
/*
 * Flag indicating we should spin post-migration in order to wait until a
 * debugger can attach.
 */
static volatile int __hold = 1;
#endif

/* Generate a call site to get rewriting metadata for outermost frame. */
static void* __attribute__((noinline))
get_call_site() { return __builtin_return_address(0); };

/* Check & invoke migration if requested. */
// Note: a pointer to data necessary to bootstrap execution after migration is
// saved by the pthread library.
void
__migrate_shim_internal(int nid, void (*callback)(void *), void *callback_data)
{
  int err;
  struct shim_data data;
  struct shim_data *data_ptr;

  if(!node_available(nid))
  {
    fprintf(stderr, "Destination node is not available!\n");
    return;
  }

  data_ptr = *pthread_migrate_args();
  if(!data_ptr) // Invoke migration
  {
    unsigned long sp = 0, bp = 0;
    const enum arch dst_arch = ni[nid].arch;
    union {
       struct regset_aarch64 aarch;
       struct regset_powerpc64 powerpc;
       struct regset_x86_64 x86;
    } regs_src, regs_dst;
#if _TIME_REWRITE == 1
    unsigned long long start, end;
#endif
    GET_LOCAL_REGSET(regs_src);

#if _TIME_REWRITE == 1
    TIMESTAMP(start);
#endif
    if(REWRITE_STACK(regs_src, regs_dst, dst_arch))
    {
#if _TIME_REWRITE == 1
      TIMESTAMP(end);
      printf("Stack transformation time: %lluns\n", TIMESTAMP_DIFF(start, end));
#endif
      data.callback = callback;
      data.callback_data = callback_data;
      data.regset = &regs_dst;
      *pthread_migrate_args() = &data;
#if _SIG_MIGRATION == 1
      clear_migrate_flag();
#endif

      switch(dst_arch) {
      case ARCH_AARCH64:
        regs_dst.aarch.pc = __migrate_fixup_aarch64;
        sp = (unsigned long)regs_dst.aarch.sp;
        bp = (unsigned long)regs_dst.aarch.x[29];
#if _LOG == 1
        dump_regs_aarch64(&regs_dst.aarch, LOG_FILE);
#endif
        break;
      case ARCH_POWERPC64:
        regs_dst.powerpc.pc = __migrate_fixup_powerpc64;
        sp = (unsigned long)regs_dst.powerpc.r[1];
        bp = (unsigned long)regs_dst.powerpc.r[31];
#if _LOG == 1
        dump_regs_powerpc64(&regs_dst.powerpc, LOG_FILE);
#endif
        break;
      case ARCH_X86_64:
        regs_dst.x86.rip = __migrate_fixup_x86_64;
        sp = (unsigned long)regs_dst.x86.rsp;
        bp = (unsigned long)regs_dst.x86.rbp;
#if _LOG == 1
        dump_regs_x86_64(&regs_dst.x86, LOG_FILE);
#endif
        break;
      default: assert(0 && "Unsupported architecture!");
      }

      // This code has different behavior depending on the type of migration:
      //
      // - Heterogeneous: we transformed the stack assuming we're re-entering
      //   __migrate_shim_internal, so we'll resume at the beginning
      //
      // - Homogeneous: we copied the existing register set.  Rather than
      //   re-entering at the beginning (which would push another frame onto
      //   the stack), resume after the migration syscall.
      //
      // Note that when migration fails, we resume after the syscall and
      // err is set to 1.
      MIGRATE(err);
      if(err)
      {
        perror("Could not migrate to node");
        *pthread_migrate_args() = NULL;
        return;
      }
      data_ptr = *pthread_migrate_args();
    }
    else
    {
      fprintf(stderr, "Could not rewrite stack!\n");
      return;
    }
  }

  // Post-migration
#if _DEBUG == 1
  // Hold until we can attach post-migration
  while(__hold);
#endif
  if(data_ptr->callback) data_ptr->callback(data_ptr->callback_data);

  *pthread_migrate_args() = NULL;
}

/* Check if we should migrate, and invoke migration. */
void check_migrate(void (*callback)(void *), void *callback_data)
{
  int nid = do_migrate(__builtin_return_address(0));
  if (nid >= 0 && nid != current_nid())
    __migrate_shim_internal(nid, callback, callback_data);
}

/* Invoke migration to a particular node if we're not already there. */
void migrate(int nid, void (*callback)(void *), void *callback_data)
{
  if (nid != current_nid())
    __migrate_shim_internal(nid, callback, callback_data);
}

/* Invoke migration to a particular node according to a thread schedule. */
void migrate_schedule(size_t region,
                      int popcorn_tid,
                      void (*callback)(void *),
                      void *callback_data)
{
  int nid = get_node_mapping(region, popcorn_tid);
  if (nid != current_nid())
    __migrate_shim_internal(nid, callback, callback_data);
}
