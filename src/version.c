
#include "version.h"
#include "version_data.h"
#include "version_branch.h"
#include "config.h"

const char version_build[] = VERSION "-" SRCVERSION VERSION_BRANCH;
const char verstr[]        = PROGNAME " " VERSION "-" SRCVERSION VERSION_BRANCH;
const char verstr_aprsis[] = PROGNAME " " VERSION "-" SRCVERSION VERSION_BRANCH;

const char verstr_build_time[] = BUILD_TIME;
const char verstr_build_user[] = BUILD_USER;
