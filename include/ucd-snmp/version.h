#ifdef UCD_COMPATIBLE

#include <net-snmp/library/version.h>

static const char *VersionInfo=NetSnmpVersionInfo;

#else

#error "Please update your headers or configure using --enable-ucd-compatibility"

#endif
