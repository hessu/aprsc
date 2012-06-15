/*
 *	Configuration file parsing
 *
 *	(c) Tomi Manninen <tomi.manninen@hut.fi>
 *
 *	This program is licensed under the BSD license, which can be found
 *	in the file LICENSE.
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <errno.h>

#include "cfgfile.h"
#include "hmalloc.h"

 /* ***************************************************************** */

/*
 *	Convert string to long long
 */
 
long long hatoll(char *s)
{
	long long res = 0LL;
	
	while (isdigit((int)*s))
		res = res * 10 + (*s++ - '0');
	
	return res;
}

 /* ***************************************************************** */

/*
 *	String to lower case
 */

extern char *strlwr(char *s)
{
	char *c;
	for (c = s; (*c); c++) {
		*c = tolower(*c);
	}
	return s;
}

 /* ***************************************************************** */

int do_string(char **dest, int argc, char **argv)
{
	if (argc < 2)
		return -1;
	if (*dest)
		hfree(*dest);
	*dest = hstrdup(argv[1]);
	return 0;
}

int do_int(int *dest, int argc, char **argv)
{
	if (argc < 2)
		return -1;
	*dest = atoi(argv[1]);
	return 0;
}

int do_boolean(int *dest, int argc, char **argv)
{
	if (argc < 2)
		return -1;
	
	if (strcasecmp(argv[1], "yes") == 0
		|| strcasecmp(argv[1], "true") == 0
		|| strcasecmp(argv[1], "on") == 0
		|| strcasecmp(argv[1], "enable") == 0
		|| strcasecmp(argv[1], "enabled") == 0) {
			*dest = 1;
			return 0;
	}
	
	if (strcasecmp(argv[1], "no") == 0
		|| strcasecmp(argv[1], "false") == 0
		|| strcasecmp(argv[1], "off") == 0
		|| strcasecmp(argv[1], "disable") == 0
		|| strcasecmp(argv[1], "disabled") == 0) {
			*dest = 0;
			return 0;
	}
	
	return -1;
}

 /* ***************************************************************** */

/*
 *	Parse c-style escapes (neat to have!)
 */
 
static char *parse_string(char *str)
{
	char *cp = str;
	unsigned long num;

	while (*str != '\0' && *str != '\"') {
		if (*str == '\\') {
			str++;
			switch (*str++) {
			case 'n':
				*cp++ = '\n';
				break;
			case 't':
				*cp++ = '\t';
				break;
			case 'v':
				*cp++ = '\v';
				break;
			case 'b':
				*cp++ = '\b';
				break;
			case 'r':
				*cp++ = '\r';
				break;
			case 'f':
				*cp++ = '\f';
				break;
			case 'a':
				*cp++ = '\007';
				break;
			case '\\':
				*cp++ = '\\';
				break;
			case '\"':
				*cp++ = '\"';
				break;
			case 'x':
				str--;
				num = strtoul(str, &str, 16);
				*cp++ = (char) num;
				break;
			case '0':
			case '1':
			case '2':
			case '3':
			case '4':
			case '5':
			case '6':
			case '7':
				str--;
				num = strtoul(str, &str, 8);
				*cp++ = (char) num;
				break;
			case '\0':
				return NULL;
			default:
				*cp++ = *(str - 1);
				break;
			};
		} else {
			*cp++ = *str++;
		}
	}
	if (*str == '\"')
		str++;		/* skip final quote */
	*cp = '\0';		/* terminate string */
	return str;
}

/*
 *	Parse command line to argv, honoring quotes and such
 */
 
int parse_args(char *argv[],char *cmd)
{
	int ct = 0;
	int quoted;
	
	while (ct < 255)
	{
		quoted = 0;
		while (*cmd && isspace((int)*cmd))
			cmd++;
		if (*cmd == 0)
			break;
		if (*cmd == '"') {
			quoted++;
			cmd++;
		}
		argv[ct++] = cmd;
		if (quoted) {
			if ((cmd = parse_string(cmd)) == NULL)
				return 0;
		} else {
			while (*cmd && !isspace((int)*cmd))
				cmd++;
		}
		if (*cmd)
			*cmd++ = 0;
	}
	argv[ct] = NULL;
	return ct;
}

/*
 *	Combine arguments back to a string
 */
 
char *argstr(int arg, int argc, char **argv)
{
	static char s[CFGLINE_LEN+1];
	int i;
	
	s[0] = '\0';
	
	for (i = arg; i < argc; i++) {
		strncat(s, argv[i], CFGLINE_LEN - strlen(s));
		strncat(s, " ", CFGLINE_LEN - strlen(s));
	}
	
	if ((i = strlen(s)) > 0)
		s[i-1] = '\0';
	
	return s;
}

/*
 *	Find the command from the command table and execute it.
 */
 
int cmdparse(struct cfgcmd *cmds, char *cmdline)
{
	struct cfgcmd *cmdp;
	int argc;
	char *argv[256];

	if ((argc = parse_args(argv, cmdline)) == 0 || *argv[0] == '#')
		return 0;
	strlwr(argv[0]);
	for (cmdp = cmds; cmdp->function != NULL; cmdp++)
		if (strncasecmp(cmdp->name, argv[0], strlen(argv[0])) == 0)
			break;
	if (cmdp->function == NULL) {
		fprintf(stderr, "No such configuration file directive: %s\n", argv[0]);
		return -1;
	}
	return (*cmdp->function)(cmdp->dest, argc, argv);
}

 /* ***************************************************************** */

/*
 *	Read configuration
 */
 
int read_cfgfile(char *f, struct cfgcmd *cmds)
{
	FILE *fp;
	char line[CFGLINE_LEN];
	int ret, n = 0;
	
	if ((fp = fopen(f, "r")) == NULL) {
		fprintf(stderr, "Cannot open %s: %s\n", f, strerror(errno));
		return 1;
	}
	
	while (fgets(line, CFGLINE_LEN, fp) != NULL) {
		n++;
		ret = cmdparse(cmds, line);
		if (ret < 0) {
			fprintf(stderr, "Problem in %s at line %d: %s\n", f, n, line);
			fclose(fp);
			return 2;
		}
	}
	fclose(fp);
	
	return 0;
}

