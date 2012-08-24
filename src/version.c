
#include "version.h"
#include "version_data.h"
#include "version_branch.h"
#include "config.h"

#define VERSTR PROGNAME " " VERSION "-" SVNVERSION VERSION_BRANCH
#define VERSTR_APRSIS VERSTR

const char verstr[] = VERSTR;
const char verstr_aprsis[] = VERSTR_APRSIS;

const char verstr_build_time[] = BUILD_TIME;
const char verstr_build_by[] = BUILD_USER;
