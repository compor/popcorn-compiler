static inline struct pthread *__pthread_self()
{
	char *self;
	__asm__ __volatile__ ("mrs %0,tpidr_el0" : "=r"(self));
	return (void*)(self);
//	return (void*)(self + 16 - sizeof(struct pthread));
}

//chris
//#define TLS_ABOVE_TP
//#define TP_ADJ(p) ((char *)(p) + sizeof(struct pthread) - 16)
#define TP_ADJ(p) (p)


#define CANCEL_REG_IP 33
