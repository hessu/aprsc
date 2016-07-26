/*
 *	aprsc
 *
 *	(c) Tomi Manninen <tomi.manninen@hut.fi>
 *
 *     This program is licensed under the BSD license, which can be found
 *     in the file LICENSE.
 *	
 */

#ifndef CFGFILE_H
#define CFGFILE_H

#define CFGLINE_LEN	102400

struct cfgcmd {
	char	*name;
	int	(*function)	(void *dest, int argc, char **argv);
	void	*dest;
};

extern int parse_args(char *argv[], char *cmd);
extern char *argstr(int arg, int argc, char **argv);

extern int read_cfgfile(char *f, struct cfgcmd *cmds);

extern int do_string(char **dest, int argc, char **argv);
extern int do_string_array(char ***dest, int argc, char **argv);
extern void free_string_array(char **dest);
extern int do_char(char *dest, int argc, char **argv);
extern int do_int(int *dest, int argc, char **argv);
extern int do_boolean(int *dest, int argc, char **argv);

extern long long hatoll(char *s);
extern char *strlwr(char *s);

#endif

