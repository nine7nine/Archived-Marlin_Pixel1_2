#include "trace.h"
#include <linux/compiler.h>

noinline __noclone int DYN_FTRACE_TEST_NAME(void)
{
	/* used to call mcount */
	return 0;
}

noinline __noclone int DYN_FTRACE_TEST_NAME2(void)
{
	/* used to call mcount */
	return 0;
}
