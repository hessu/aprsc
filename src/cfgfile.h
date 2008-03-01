/*
 *	aprsc
 *
 *	(c) Tomi Manninen <tomi.manninen@hut.fi>
 *
 *    This program is free software; you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation; either version 2 of the License, or
 *    (at your option) any later version.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *
 *    You should have received a copy of the GNU General Public License
 *    along with this program; if not, write to the Free Software
 *    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
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
extern int do_int(int *dest, int argc, char **argv);

extern long long hatoll(char *s);
extern char *strlwr(char *s);

#endif

