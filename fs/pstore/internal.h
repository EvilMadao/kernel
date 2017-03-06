#ifndef __PSTORE_INTERNAL_H__
#define __PSTORE_INTERNAL_H__

#include <linux/types.h>
#include <linux/time.h>
#include <linux/pstore.h>

#ifdef CONFIG_PSTORE_FTRACE
extern void pstore_register_ftrace(void);
extern void pstore_unregister_ftrace(void);
#else
static inline void pstore_register_ftrace(void) {}
static inline void pstore_unregister_ftrace(void) {}
#endif

#ifdef CONFIG_PSTORE_PMSG
extern void pstore_register_pmsg(void);
extern void pstore_unregister_pmsg(void);
#else
static inline void pstore_register_pmsg(void) {}
static inline void pstore_unregister_pmsg(void) {}
#endif

extern struct pstore_info *psinfo;

extern void	pstore_set_kmsg_bytes(int);
extern void	pstore_get_records(unsigned);
/* Flags for the pstore iterator pstore_get_records() */
#define PGR_QUIET	0
#define PGR_VERBOSE	1
#define PGR_POPULATE	2
#define PGR_SYSLOG	4
#define PGR_CLEAR	8

extern int	pstore_mkfile(enum pstore_type_id, char *psname, u64 id,
			      int count, char *data, bool compressed,
			      size_t size, struct timespec time,
			      struct pstore_info *psi);
extern bool	pstore_is_mounted(void);

#endif
