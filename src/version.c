
#include "version.h"
#include "version_data.h"
#include "version_branch.h"
#include "config.h"

#define VERSION_BUILD VERSION "-" SVNVERSION VERSION_BRANCH
#define VERSTR PROGNAME " " VERSION_BUILD
#define VERSTR_APRSIS VERSTR

const char version_build[] = VERSION_BUILD;
const char verstr[] = VERSTR;
const char verstr_aprsis[] = VERSTR_APRSIS;

const char verstr_build_time[] = BUILD_TIME;
const char verstr_build_user[] = BUILD_USER;
