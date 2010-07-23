/*	$OpenBSD: grep.c,v 1.42 2010/07/02 22:18:03 tedu Exp $	*/

/*-
 * Copyright (c) 1999 James Howard and Dag-Erling Co�dan Sm�rgrav
 * Copyright (C) 2008-2009 Gabor Kovesdan <gabor@FreeBSD.org>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include <sys/stat.h>
#include <sys/types.h>

#include <ctype.h>
#include <err.h>
#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <libgen.h>
#include <locale.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "grep.h"

#ifndef WITHOUT_NLS
#include <nl_types.h>
nl_catd	 catalog;
#endif

/*
 * Default messags to use when NLS is disabled or no catalogue
 * is found.
 */
const char	*errstr[] = {
	"",
/* 1*/	"(standard input)",
/* 2*/	"cannot read bzip2 compressed file",
/* 3*/	"unknown --color option",
/* 4*/	"usage: %s [-abcDEFGHhIiJLlmnOoPqRSsUVvwxZ] [-A num] [-B num] [-C[num]]\n",
/* 5*/	"\t[-e pattern] [-f file] [--binary-files=value] [--color=when]\n",
/* 6*/	"\t[--context[=num]] [--directories=action] [--label] [--line-buffered]\n",
/* 7*/	"\t[--null] [pattern] [file ...]\n",
/* 8*/	"unknown --binary-files option",
/* 9*/	"Binary file %s matches\n",
/*10*/	"%s (BSD grep) %s\n",
};

/* Flags passed to regcomp() and regexec() */
int		 cflags = 0;
int		 eflags = REG_STARTEND;

/* Shortcut for matching all cases like empty regex */
bool		 matchall;

/* Searching patterns */
unsigned int	 patterns, pattern_sz;
char		**pattern;
regex_t		*r_pattern;
fastgrep_t	*fg_pattern;

/* Filename exclusion/inclusion patterns */
unsigned int	 epatterns, epattern_sz;
struct epat	*epattern;

/* For regex errors  */
char	 re_error[RE_ERROR_BUF + 1];

/* Command-line flags */
unsigned long long Aflag;	/* -A x: print x lines trailing each match */
unsigned long long Bflag;	/* -B x: print x lines leading each match */
bool	 Hflag;		/* -H: always print file name */
bool	 Lflag;		/* -L: only show names of files with no matches */
bool	 bflag;		/* -b: show block numbers for each match */
bool	 cflag;		/* -c: only show a count of matching lines */
bool	 hflag;		/* -h: don't print filename headers */
bool	 iflag;		/* -i: ignore case */
bool	 lflag;		/* -l: only show names of files with matches */
bool	 mflag;		/* -m x: stop reading the files after x matches */
unsigned long long mcount;	/* count for -m */
bool	 nflag;		/* -n: show line numbers in front of matching lines */
bool	 oflag;		/* -o: print only matching part */
bool	 qflag;		/* -q: quiet mode (don't output anything) */
bool	 sflag;		/* -s: silent mode (ignore errors) */
bool	 vflag;		/* -v: only show non-matching lines */
bool	 wflag;		/* -w: pattern must start and end on word boundaries */
bool	 xflag;		/* -x: pattern must match entire line */
bool	 lbflag;	/* --line-buffered */
bool	 nullflag;	/* --null */
bool	 exclflag;	/* --exclude */
char	*label;		/* --label */
char	*color;		/* --color */
int	 grepbehave = GREP_BASIC;	/* -EFGP: type of the regex */
int	 binbehave = BINFILE_BIN;	/* -aIU: handling of binary files */
int	 filebehave = FILE_STDIO;	/* -JZ: normal, gzip or bzip2 file */
int	 devbehave = DEV_GREP;		/* -D: handling of devices */
int	 dirbehave = DIR_GREP;		/* -dRr: handling of directories */
int	 linkbehave = LINK_GREP;	/* -OpS: handling of symlinks */

enum {
	BIN_OPT = CHAR_MAX + 1,
	COLOR_OPT,
	HELP_OPT,
	MMAP_OPT,
	LINEBUF_OPT,
	LABEL_OPT,
	NULL_OPT,
	R_EXCLUDE_OPT,
	R_INCLUDE_OPT,
	R_DEXCLUDE_OPT,
	R_DINCLUDE_OPT
};

/* Housekeeping */
bool	 first = true;	/* flag whether we are processing the first match */
bool	 prev;		/* flag whether or not the previous line matched */
int	 tail;		/* lines left to print */
bool	 notfound;	/* file not found */

extern char	*__progname;

/*
 * Prints usage information and returns 2.
 */
static void
usage(void)
{
	fprintf(stderr, getstr(4), __progname);
	fprintf(stderr, "%s", getstr(5));
	fprintf(stderr, "%s", getstr(5));
	fprintf(stderr, "%s", getstr(6));
	fprintf(stderr, "%s", getstr(7));
	exit(2);
}

static const char	*optstr = "0123456789A:B:C:D:EFGHIJLOPSRUVZabcd:e:f:hilm:nopqrsuvwxy";

struct option long_options[] =
{
	{"binary-files",	required_argument,	NULL, BIN_OPT},
	{"help",		no_argument,		NULL, HELP_OPT},
	{"mmap",		no_argument,		NULL, MMAP_OPT},
	{"line-buffered",	no_argument,		NULL, LINEBUF_OPT},
	{"label",		required_argument,	NULL, LABEL_OPT},
	{"null",		no_argument,		NULL, NULL_OPT},
	{"color",		optional_argument,	NULL, COLOR_OPT},
	{"colour",		optional_argument,	NULL, COLOR_OPT},
	{"exclude",		required_argument,	NULL, R_EXCLUDE_OPT},
	{"include",		required_argument,	NULL, R_INCLUDE_OPT},
	{"exclude-dir",		required_argument,	NULL, R_DEXCLUDE_OPT},
	{"include-dir",		required_argument,	NULL, R_DINCLUDE_OPT},
	{"after-context",	required_argument,	NULL, 'A'},
	{"text",		no_argument,		NULL, 'a'},
	{"before-context",	required_argument,	NULL, 'B'},
	{"byte-offset",		no_argument,		NULL, 'b'},
	{"context",		optional_argument,	NULL, 'C'},
	{"count",		no_argument,		NULL, 'c'},
	{"devices",		required_argument,	NULL, 'D'},
        {"directories",		required_argument,	NULL, 'd'},
	{"extended-regexp",	no_argument,		NULL, 'E'},
	{"regexp",		required_argument,	NULL, 'e'},
	{"fixed-strings",	no_argument,		NULL, 'F'},
	{"file",		required_argument,	NULL, 'f'},
	{"basic-regexp",	no_argument,		NULL, 'G'},
	{"no-filename",		no_argument,		NULL, 'h'},
	{"with-filename",	no_argument,		NULL, 'H'},
	{"ignore-case",		no_argument,		NULL, 'i'},
	{"bz2decompress",	no_argument,		NULL, 'J'},
	{"files-with-matches",	no_argument,		NULL, 'l'},
	{"files-without-match", no_argument,            NULL, 'L'},
	{"max-count",		required_argument,	NULL, 'm'},
	{"line-number",		no_argument,		NULL, 'n'},
	{"only-matching",	no_argument,		NULL, 'o'},
	{"quiet",		no_argument,		NULL, 'q'},
	{"silent",		no_argument,		NULL, 'q'},
	{"recursive",		no_argument,		NULL, 'r'},
	{"no-messages",		no_argument,		NULL, 's'},
	{"binary",		no_argument,		NULL, 'U'},
	{"unix-byte-offsets",	no_argument,		NULL, 'u'},
	{"invert-match",	no_argument,		NULL, 'v'},
	{"version",		no_argument,		NULL, 'V'},
	{"word-regexp",		no_argument,		NULL, 'w'},
	{"line-regexp",		no_argument,		NULL, 'x'},
	{"decompress",          no_argument,            NULL, 'Z'},
	{NULL,			no_argument,		NULL, 0}
};

/*
 * Adds a searching pattern to the internal array.
 */
static void
add_pattern(char *pat, size_t len)
{

	/* Check if we can do a shortcut */
	if (len == 0 || matchall) {
		matchall = true;
		return;
	}
	/* Increase size if necessary */
	if (patterns == pattern_sz) {
		pattern_sz *= 2;
		pattern = grep_realloc(pattern, ++pattern_sz *
		    sizeof(*pattern));
	}
	if (len > 0 && pat[len - 1] == '\n')
		--len;
	/* pat may not be NUL-terminated */
	pattern[patterns] = grep_malloc(len + 1);
	memcpy(pattern[patterns], pat, len);
	pattern[patterns][len] = '\0';
	++patterns;
}

/*
 * Adds an include/exclude pattern to the internal array.
 */
static void
add_epattern(char *pat, size_t len, int type, int mode)
{

	/* Increase size if necessary */
	if (epatterns == epattern_sz) {
		epattern_sz *= 2;
		epattern = grep_realloc(epattern, ++epattern_sz *
		    sizeof(struct epat));
	}
	if (len > 0 && pat[len - 1] == '\n')
		 --len;
	epattern[epatterns].pat = grep_malloc(len + 1);
	memcpy(epattern[epatterns].pat, pat, len);
	epattern[epatterns].pat[len] = '\0';
	epattern[epatterns].type = type;
	epattern[epatterns].mode = mode;
	++epatterns;
}

/*
 * Reads searching patterns from a file and adds them with add_pattern().
 */
static void
read_patterns(const char *fn)
{
	FILE *f;
	char *line;
	size_t len;

	if ((f = fopen(fn, "r")) == NULL)
		err(2, "%s", fn);
	while ((line = fgetln(f, &len)) != NULL)
		add_pattern(line, *line == '\n' ? 0 : len);
	if (ferror(f))
		err(2, "%s", fn);
	fclose(f);
}

int
main(int argc, char *argv[])
{
	char **aargv, **eargv, *eopts;
	char *ep;
	unsigned long long l;
	unsigned int aargc, eargc, i;
	int c, lastc, needpattern, newarg, prevoptind;

	setlocale(LC_ALL, "");

#ifndef WITHOUT_NLS
	catalog = catopen("grep", NL_CAT_LOCALE);
#endif

	/* Check what is the program name of the binary.  In this
	   way we can have all the funcionalities in one binary
	   without the need of scripting and using ugly hacks. */
	switch (__progname[0]) {
	case 'e':
		grepbehave = GREP_EXTENDED;
		break;
	case 'f':
		grepbehave = GREP_FIXED;
		break;
	case 'g':
		grepbehave = GREP_BASIC;
		break;
	case 'z':
		filebehave = FILE_GZIP;
		switch(__progname[1]) {
		case 'e':
			grepbehave = GREP_EXTENDED;
			break;
		case 'f':
			grepbehave = GREP_FIXED;
			break;
		case 'g':
			grepbehave = GREP_BASIC;
			break;
		}
		break;
	}

	lastc = '\0';
	newarg = 1;
	prevoptind = 1;
	needpattern = 1;

	eopts = getenv("GREP_OPTIONS");

	eargc = 1;
	if (eopts != NULL) {
		char *str;

		for(i = 0; i < strlen(eopts); i++)
			if (eopts[i] == ' ')
				eargc++;

		eargv = (char **)grep_malloc(sizeof(char *) * (eargc + 1));

		str = strtok(eopts, " ");
		eargc = 0;

		while(str != NULL) {
			eargv[++eargc] = (char *)grep_malloc(sizeof(char) *
			    (strlen(str) + 1));
			strlcpy(eargv[eargc], str, strlen(str) + 1);
			str = strtok(NULL, " ");
		}
		eargv[++eargc] = NULL;

		aargv = (char **)grep_calloc(eargc + argc + 1,
		    sizeof(char *));
		aargv[0] = argv[0];

		for(i = 1; i < eargc; i++)
			aargv[i] = eargv[i];
		for(int j = 1; j < argc; j++)
			aargv[i++] = argv[j];

		aargc = eargc + argc - 1;

	} else {
		aargv = argv;
		aargc = argc;
	}

	while (((c = getopt_long(aargc, aargv, optstr, long_options, NULL)) !=
	    -1)) {
		switch (c) {
		case '0': case '1': case '2': case '3': case '4':
		case '5': case '6': case '7': case '8': case '9':
			if (newarg || !isdigit(lastc))
				Aflag = 0;
			else if (Aflag > LLONG_MAX / 10) {
				errno = ERANGE;
				err(2, NULL);
			}
			Aflag = Bflag = (Aflag * 10) + (c - '0');
			break;
		case 'C':
			if (optarg == NULL) {
				Aflag = Bflag = 2;
				break;
			}
			/* FALLTHROUGH */
		case 'A':
			/* FALLTHROUGH */
		case 'B':
			errno = 0;
			l = strtoull(optarg, &ep, 10);
			if (((errno == ERANGE) && (l == ULLONG_MAX)) ||
			    ((errno == EINVAL) && (l == 0)))
				err(2, NULL);
			else if (ep[0] != '\0') {
				errno = EINVAL;
				err(2, NULL);
			}
			if (c == 'A')
				Aflag = l;
			else if (c == 'B')
				Bflag = l;
			else
				Aflag = Bflag = l;
			break;
		case 'a':
			binbehave = BINFILE_TEXT;
			break;
		case 'b':
			bflag = true;
			break;
		case 'c':
			cflag = true;
			break;
		case 'D':
			if (strcmp(optarg, "skip") == 0)
				devbehave = DEV_SKIP;
			break;
		case 'd':
			if (strcmp("recurse", optarg) == 0) {
				Hflag = true;
				dirbehave = DIR_RECURSE;
			} else if (strcmp("skip", optarg) == 0)
				dirbehave = DIR_SKIP;
			else if (strcmp("read", optarg) != 0) {
				errno = EINVAL;
				err(2, NULL);
			}
			break;
		case 'E':
			grepbehave = GREP_EXTENDED;
			break;
		case 'e':
			add_pattern(optarg, strlen(optarg));
			needpattern = 0;
			break;
		case 'F':
			grepbehave = GREP_FIXED;
			break;
		case 'f':
			read_patterns(optarg);
			needpattern = 0;
			break;
		case 'G':
			grepbehave = GREP_BASIC;
			break;
		case 'H':
			Hflag = true;
			break;
		case 'h':
			Hflag = false;
			hflag = true;
			break;
		case 'I':
			binbehave = BINFILE_SKIP;
			break;
		case 'i':
		case 'y':
			iflag =  true;
			cflags |= REG_ICASE;
			break;
		case 'J':
			filebehave = FILE_BZIP;
			break;
		case 'L':
			lflag = false;
			Lflag = qflag = true;
			break;
		case 'l':
			Lflag = false;
			lflag = qflag = true;
			break;
		case 'm':
			mflag = true;
			errno = 0;
			mcount = strtoull(optarg, &ep, 10);
			if (((errno == ERANGE) && (mcount == ULLONG_MAX)) ||
			    ((errno == EINVAL) && (mcount == 0)))
				err(2, NULL);
			else if (ep[0] != '\0') {
				errno = EINVAL;
				err(2, NULL);
			}
			break;
		case 'n':
			nflag = true;
			break;
		case 'O':
			linkbehave = LINK_EXPLICIT;
			break;
		case 'o':
			oflag = true;
			break;
		case 'p':
			linkbehave = LINK_SKIP;
			break;
		case 'q':
			qflag = true;
			break;
		case 'S':
			linkbehave = LINK_GREP;
			break;
		case 'R':
		case 'r':
			dirbehave = DIR_RECURSE;
			Hflag = true;
			break;
		case 's':
			sflag = true;
			break;
		case 'U':
			binbehave = BINFILE_BIN;
			break;
		case 'u':
		case MMAP_OPT:
			/* noop, compatibility */
			break;
		case 'V':
			printf(getstr(10), __progname, VERSION);
			exit(0);
		case 'v':
			vflag = true;
			break;
		case 'w':
			wflag = true;
			break;
		case 'x':
			xflag = true;
			break;
		case 'Z':
			filebehave = FILE_GZIP;
			break;
		case BIN_OPT:
			if (strcmp("binary", optarg) == 0)
				binbehave = BINFILE_BIN;
			else if (strcmp("without-match", optarg) == 0)
				binbehave = BINFILE_SKIP;
			else if (strcmp("text", optarg) == 0)
				binbehave = BINFILE_TEXT;
			else
				errx(2, "%s", getstr(8));
			break;
		case COLOR_OPT:
			if (optarg == NULL || strcmp("auto", optarg) == 0 ||
			    strcmp("always", optarg) == 0 ) {
				color = getenv("GREP_COLOR");
				if (color == NULL) {
					color = grep_malloc(sizeof(char) * 6);
					strcpy(color, "01;31");
				}
			} else if (strcmp("never", optarg) == 0)
				color = NULL;
			else
				errx(2, "%s", getstr(3));
			break;
		case LABEL_OPT:
			label = optarg;
			break;
		case LINEBUF_OPT:
			lbflag = true;
			break;
		case NULL_OPT:
			nullflag = true;
			break;
		case R_INCLUDE_OPT:
			exclflag = true;
			add_epattern(basename(optarg), strlen(basename(optarg)),
			    FILE_PAT, INCL_PAT);
			break;
		case R_EXCLUDE_OPT:
			exclflag = true;
			add_epattern(basename(optarg), strlen(basename(optarg)),
			    FILE_PAT, EXCL_PAT);
			break;
		case R_DINCLUDE_OPT:
			exclflag = true;
			add_epattern(basename(optarg), strlen(basename(optarg)),
			    DIR_PAT, INCL_PAT);
			break;
		case R_DEXCLUDE_OPT:
			exclflag = true;
			add_epattern(basename(optarg), strlen(basename(optarg)),
			    DIR_PAT, EXCL_PAT);
			break;
		case HELP_OPT:
		default:
			usage();
		}
		lastc = c;
		newarg = optind != prevoptind;
		prevoptind = optind;
	}
	aargc -= optind;
	aargv += optind;

	/* Fail if we don't have any pattern */
	if (aargc == 0 && needpattern)
		usage();

	/* Process patterns from command line */
	if (aargc != 0 && needpattern) {
		add_pattern(*aargv, strlen(*aargv));
		--aargc;
		++aargv;
	}

	switch (grepbehave) {
	case GREP_FIXED:
	case GREP_BASIC:
		break;
	case GREP_EXTENDED:
		cflags |= REG_EXTENDED;
		break;
	default:
		/* NOTREACHED */
		usage();
	}

	fg_pattern = grep_calloc(patterns, sizeof(*fg_pattern));
	r_pattern = grep_calloc(patterns, sizeof(*r_pattern));
/*
 * XXX: fgrepcomp() and fastcomp() are workarounds for regexec() performance.
 * Optimizations should be done there.
 */
		/* Check if cheating is allowed (always is for fgrep). */
	if (grepbehave == GREP_FIXED) {
		for (i = 0; i < patterns; ++i)
			fgrepcomp(&fg_pattern[i], pattern[i]);
	} else {
		for (i = 0; i < patterns; ++i) {
			if (fastcomp(&fg_pattern[i], pattern[i])) {
				/* Fall back to full regex library */
				c = regcomp(&r_pattern[i], pattern[i], cflags);
				if (c != 0) {
					regerror(c, &r_pattern[i], re_error,
					    RE_ERROR_BUF);
					errx(2, "%s", re_error);
				}
			}
		}
	}

	if (lbflag)
		setlinebuf(stdout);

	if ((aargc == 0 || aargc == 1) && !Hflag)
		hflag = true;

	if (aargc == 0)
		exit(!procfile("-"));

	if (dirbehave == DIR_RECURSE)
		c = grep_tree(aargv);
	else 
		for (c = 0; aargc--; ++aargv)
			c+= procfile(*aargv);

#ifndef WITHOUT_NLS
	catclose(catalog);
#endif

	/* Find out the correct return value according to the
	   results and the command line option. */
	exit(c ? (notfound ? (qflag ? 0 : 2) : 0) : (notfound ? 2 : 1));
}
