/*
**  Sendmail
**  Copyright (c) 1983  Eric P. Allman
**  Berkeley, California
**
**  Copyright (c) 1983 Regents of the University of California.
**  All rights reserved.  The Berkeley software License Agreement
**  specifies the terms and conditions for redistribution.
*/

#ifndef lint
static char	SccsId[] = "@(#)recipient.c	5.7 (Berkeley) %G%";
#endif not lint

# include <pwd.h>
# include "sendmail.h"
# include <sys/stat.h>

/*
**  SENDTOLIST -- Designate a send list.
**
**	The parameter is a comma-separated list of people to send to.
**	This routine arranges to send to all of them.
**
**	The `ctladdr' is the address that expanded to be this one,
**	e.g., in an alias expansion.  This is used for a number of
**	purposed, most notably inheritance of uid/gid for protection
**	purposes.  It is also used to detect self-reference in group
**	expansions and the like.
**
**	Parameters:
**		list -- the send list.
**		ctladdr -- the address template for the person to
**			send to -- effective uid/gid are important.
**			This is typically the alias that caused this
**			expansion.
**		sendq -- a pointer to the head of a queue to put
**			these people into.
**		qflags -- special flags to set in the q_flags field.
**
**	Returns:
**		pointer to chain of addresses.
**
**	Side Effects:
**		none.
*/

# define MAXRCRSN	10

ADDRESS *
sendto(list, copyf, ctladdr, qflags)
	char *list;
	ADDRESS *ctladdr;
	ADDRESS **sendq;
	u_short qflags;
{
	register char *p;
	register ADDRESS *al;	/* list of addresses to send to */
	bool firstone;		/* set on first address sent */
	bool selfref;		/* set if this list includes ctladdr */
	char delimiter;		/* the address delimiter */
	ADDRESS *sibl;		/* sibling pointer in tree */
	ADDRESS *prev;		/* previous sibling */

# ifdef DEBUG
	if (tTd(25, 1))
	{
		printf("sendto: %s\n   ctladdr=", list);
		printaddr(ctladdr, FALSE);
	}
# endif DEBUG

	/* heuristic to determine old versus new style addresses */
	if (ctladdr == NULL &&
	    (index(list, ',') != NULL || index(list, ';') != NULL ||
	     index(list, '<') != NULL || index(list, '(') != NULL))
		CurEnv->e_flags &= ~EF_OLDSTYLE;
	delimiter = ' ';
	if (!bitset(EF_OLDSTYLE, CurEnv->e_flags) || ctladdr != NULL)
		delimiter = ',';

	firstone = TRUE;
	selfref = FALSE;
	al = NULL;

	for (p = list; *p != '\0'; )
	{
		register ADDRESS *a;
		extern char *DelimChar;		/* defined in prescan */

		/* parse the address */
		while (isspace(*p) || *p == ',')
			p++;
		a = parseaddr(p, (ADDRESS *) NULL, 1, delimiter);
		p = DelimChar;
		if (a == NULL)
			continue;
		a->q_next = al;
		a->q_alias = ctladdr;
		if (ctladdr != NULL)
			a->q_flags |= ctladdr->q_flags & ~QPRIMARY;
		a->q_flags |= qflags;

		/* see if this should be marked as a primary address */
		if (ctladdr == NULL ||
		    (firstone && *p == '\0' && bitset(QPRIMARY, ctladdr->q_flags)))
			a->q_flags |= QPRIMARY;

		/* put on send queue or suppress self-reference */
		if (ctladdr != NULL && sameaddr(ctladdr, a))
			selfref = TRUE;
		else
			al = a;
		firstone = FALSE;
	}

	/* if this alias doesn't include itself, delete ctladdr */
	if (!selfref && ctladdr != NULL)
		ctladdr->q_flags |= QDONTSEND;

	/* arrange to send to everyone on the local send list */
	prev = sibl = NULL;
	if (ctladdr != NULL)
		prev = ctladdr->q_child;
	while (al != NULL)
	{
		register ADDRESS *a = al;
		extern ADDRESS *recipient();
		extern ADDRESS *recipient();

		al = a->q_next;
		sibl = recipient(a);
		if (sibl != NULL)
		{
			extern ADDRESS *addrref();

			/* inherit full name */
			if (sibl->q_fullname == NULL && ctladdr != NULL)
				sibl->q_fullname = ctladdr->q_fullname;

			/* link tree together (but only if the node is new) */
			if (sibl == a)
			{
				sibl->q_sibling = prev;
				prev = sibl;
			}
		}
	}

	CurEnv->e_to = NULL;
	if (ctladdr != NULL)
		ctladdr->q_child = prev;
	return (prev);
}
/*
**  ADDRREF -- return pointer to address that references another address.
**
**	Parameters:
**		a -- address to check.
**		r -- reference to find.
**
**	Returns:
**		address of node in tree rooted at 'a' that references
**			'r'.
**		NULL if no such node exists.
**
**	Side Effects:
**		none.
*/

ADDRESS *
addrref(a, r)
	register ADDRESS *a;
	register ADDRESS *r;
{
	register ADDRESS *q;

	while (a != NULL)
	{
		if (a->q_child == r || a->q_sibling == r)
			return (a);
		q = addrref(a->q_child, r);
		if (q != NULL)
			return (q);
		a = a->q_sibling;
	}
	return (NULL);
}
/*
**  RECIPIENT -- Designate a message recipient
**
**	Saves the named person for future mailing.
**
**	Parameters:
**		a -- the (preparsed) address header for the recipient.
**		sendq -- a pointer to the head of a queue to put the
**			recipient in.  Duplicate supression is done
**			in this queue.
**
**	Returns:
**		pointer to address actually inserted in send list.
**
**	Side Effects:
**		none.
*/

ADDRESS *
ADDRESS *
recipient(a, sendq)
	register ADDRESS *a;
	register ADDRESS **sendq;
{
	register ADDRESS *q;
	ADDRESS **pq;
	register struct mailer *m;
	register char *p;
	bool quoted = FALSE;		/* set if the addr has a quote bit */
	char buf[MAXNAME];		/* unquoted image of the user name */
	extern ADDRESS *getctladdr();
	extern bool safefile();

	CurEnv->e_to = a->q_paddr;
	m = a->q_mailer;
	errno = 0;
# ifdef DEBUG
	if (tTd(26, 1))
	{
		printf("\nrecipient: ");
		printaddr(a, FALSE);
	}
# endif DEBUG

	/* break aliasing loops */
	if (AliasLevel > MAXRCRSN)
	{
		usrerr("aliasing/forwarding loop broken");
		return (NULL);
	}

	/*
	**  Finish setting up address structure.
	*/

	/* set the queue timeout */
	a->q_timeout = TimeOut;

	/* map user & host to lower case if requested on non-aliases */
	if (a->q_alias == NULL)
		loweraddr(a);

	/* get unquoted user for file, program or user.name check */
	(void) strcpy(buf, a->q_user);
	for (p = buf; *p != '\0' && !quoted; p++)
	{
		if (!isascii(*p) && (*p & 0377) != (SpaceSub & 0377))
			quoted = TRUE;
	}
	stripquotes(buf, TRUE);

	/* do sickly crude mapping for program mailing, etc. */
	if (m == LocalMailer && buf[0] == '|')
	{
		a->q_mailer = m = ProgMailer;
		a->q_user++;
		if (a->q_alias == NULL && !tTd(0, 1) && !QueueRun && !ForceMail)
		{
			usrerr("Cannot mail directly to programs");
			a->q_flags |= QDONTSEND;
		}
	}

	/*
	**  Look up this person in the recipient list.
	**	If they are there already, return, otherwise continue.
	**	If the list is empty, just add it.  Notice the cute
	**	hack to make from addresses suppress things correctly:
	**	the QDONTSEND bit will be set in the send list.
	**	[Please note: the emphasis is on "hack."]
	*/

	for (pq = sendq; (q = *pq) != NULL; pq = &q->q_next)
	{
		if (!ForceMail && sameaddr(q, a))
		{
# ifdef DEBUG
			if (tTd(26, 1))
			{
				printf("%s in sendq: ", a->q_paddr);
				printaddr(q, FALSE);
			}
# endif DEBUG
			if (Verbose && !bitset(QDONTSEND|QPSEUDO, a->q_flags))
				message(Arpa_Info, "duplicate suppressed");
			if (!bitset(QPRIMARY, q->q_flags))
				q->q_flags |= a->q_flags;
			if (!bitset(QPSEUDO, a->q_flags))
				q->q_flags &= ~QPSEUDO;
			return (q);
		}
	}

	/* add address on list */
	*pq = a;
	a->q_next = NULL;
	CurEnv->e_nrcpts++;

	/*
	**  Alias the name and handle :include: specs.
	*/

	if (m == LocalMailer && !bitset(QDONTSEND, a->q_flags))
	{
		if (strncmp(a->q_user, ":include:", 9) == 0)
		{
			a->q_flags |= QDONTSEND;
			if (a->q_alias == NULL && !tTd(0, 1) && !QueueRun && !ForceMail)
				usrerr("Cannot mail directly to :include:s");
			else
			{
				message(Arpa_Info, "including file %s", &a->q_user[9]);
				include(&a->q_user[9], " sending", a, sendq);
			}
		}
		else
			alias(a, sendq);
	}

	/*
	**  If the user is local and still being sent, verify that
	**  the address is good.  If it is, try to forward.
	**  If the address is already good, we have a forwarding
	**  loop.  This can be broken by just sending directly to
	**  the user (which is probably correct anyway).
	*/

	if (!bitset(QDONTSEND, a->q_flags) && m == LocalMailer)
	{
		struct stat stb;
		extern bool writable();

		/* see if this is to a file */
		if (buf[0] == '/')
		{
			p = rindex(buf, '/');
			/* check if writable or creatable */
			if (a->q_alias == NULL && !tTd(0, 1) && !QueueRun && !ForceMail)
			{
				usrerr("Cannot mail directly to files");
				a->q_flags |= QDONTSEND;
			}
			else if ((stat(buf, &stb) >= 0) ? (!writable(&stb)) :
			    (*p = '\0', !safefile(buf, getruid(), S_IWRITE|S_IEXEC)))
			{
				a->q_flags |= QBADADDR;
				giveresponse(EX_CANTCREAT, m, CurEnv);
			}
		}
		else
		{
			register struct passwd *pw;
			extern struct passwd *finduser();

			/* warning -- finduser may trash buf */
			pw = finduser(buf);
			if (pw == NULL)
			{
				a->q_flags |= QBADADDR;
				giveresponse(EX_NOUSER, m, CurEnv);
			}
			else
			{
				char nbuf[MAXNAME];

				char nbuf[MAXNAME];

				if (strcmp(a->q_user, pw->pw_name) != 0)
				{
					a->q_user = newstr(pw->pw_name);
					(void) strcpy(buf, pw->pw_name);
				}
				a->q_home = newstr(pw->pw_dir);
				a->q_uid = pw->pw_uid;
				a->q_gid = pw->pw_gid;
				a->q_flags |= QGOODUID;
				buildfname(pw->pw_gecos, pw->pw_name, nbuf);
				if (nbuf[0] != '\0')
					a->q_fullname = newstr(nbuf);
				fullname(pw, nbuf);
				if (nbuf[0] != '\0')
					a->q_fullname = newstr(nbuf);
				if (!quoted)
					forward(a, sendq);
			}
		}
	}
	return (a);

	return (a);
}
/*
**  FINDUSER -- find the password entry for a user.
**
**	This looks a lot like getpwnam, except that it may want to
**	do some fancier pattern matching in /etc/passwd.
**
**	This routine contains most of the time of many sendmail runs.
**	It deserves to be optimized.
**
**	Parameters:
**		name -- the name to match against.
**
**	Returns:
**		A pointer to a pw struct.
**		NULL if name is unknown or ambiguous.
**
**	Side Effects:
**		may modify name.
*/

struct passwd *
finduser(name)
	char *name;
{
	register struct passwd *pw;
	register char *p;
	extern struct passwd *getpwent();
	extern struct passwd *getpwnam();

	/* map upper => lower case */
	for (p = name; *p != '\0'; p++)
	{
		if (isascii(*p) && isupper(*p))
			*p = tolower(*p);
	}

	/* look up this login name using fast path */
	if ((pw = getpwnam(name)) != NULL)
		return (pw);

	/* search for a matching full name instead */
	for (p = name; *p != '\0'; p++)
	{
		if (*p == (SpaceSub & 0177) || *p == '_')
			*p = ' ';
	}
	(void) setpwent();
	while ((pw = getpwent()) != NULL)
	{
		extern bool sameword();
		char buf[MAXNAME];

		fullname(pw, buf);
		if (index(buf, ' ') != NULL && sameword(buf, name))
		{
				message(Arpa_Info, "sending to %s <%s>",
				    buf, pw->pw_name);
			return (pw);
		}
	}
	return (NULL);
}
/*
**  WRITABLE -- predicate returning if the file is writable.
**
**	This routine must duplicate the algorithm in sys/fio.c.
**	Unfortunately, we cannot use the access call since we
**	won't necessarily be the real uid when we try to
**	actually open the file.
**
**	Notice that ANY file with ANY execute bit is automatically
**	not writable.  This is also enforced by mailfile.
**
**	Parameters:
**		s -- pointer to a stat struct for the file.
**
**	Returns:
**		TRUE -- if we will be able to write this file.
**		FALSE -- if we cannot write this file.
**
**	Side Effects:
**		none.
*/

bool
writable(s)
	register struct stat *s;
{
	int euid, egid;
	int bits;

	if (bitset(0111, s->st_mode))
		return (FALSE);
	euid = getruid();
	egid = getrgid();
	if (geteuid() == 0)
	{
		if (bitset(S_ISUID, s->st_mode))
			euid = s->st_uid;
		if (bitset(S_ISGID, s->st_mode))
			egid = s->st_gid;
	}

	if (euid == 0)
		return (TRUE);
	bits = S_IWRITE;
	if (euid != s->st_uid)
	{
		bits >>= 3;
		if (egid != s->st_gid)
			bits >>= 3;
	}
	return ((s->st_mode & bits) != 0);
}
/*
**  INCLUDE -- handle :include: specification.
**
**	Parameters:
**		fname -- filename to include.
**		msg -- message to print in verbose mode.
**		ctladdr -- address template to use to fill in these
**			addresses -- effective user/group id are
**			the important things.
**		sendq -- a pointer to the head of the send queue
**			to put these addresses in.
**
**	Returns:
**		none.
**
**	Side Effects:
**		reads the :include: file and sends to everyone
**		listed in that file.
*/

include(fname, msg, ctladdr, sendq)
	char *fname;
	char *msg;
	ADDRESS *ctladdr;
	ADDRESS **sendq;
{
	char buf[MAXLINE];
	register FILE *fp;
	char *oldto = CurEnv->e_to;
	char *oldfilename = FileName;
	int oldlinenumber = LineNumber;

	fp = fopen(fname, "r");
	if (fp == NULL)
	{
		usrerr("Cannot open %s", fname);
		return;
	}
	if (getctladdr(ctladdr) == NULL)
	{
		struct stat st;

		if (fstat(fileno(fp), &st) < 0)
			syserr("Cannot fstat %s!", fname);
		ctladdr->q_uid = st.st_uid;
		ctladdr->q_gid = st.st_gid;
		ctladdr->q_flags |= QGOODUID;
	}

	/* read the file -- each line is a comma-separated list. */
	FileName = fname;
	LineNumber = 0;
	while (fgets(buf, sizeof buf, fp) != NULL)
	{
		register char *p = index(buf, '\n');

		if (p != NULL)
			*p = '\0';
		if (buf[0] == '\0')
			continue;
		CurEnv->e_to = oldto;
		message(Arpa_Info, "%s to %s", msg, buf);
		AliasLevel++;
		sendto(buf, 1, ctladdr, 0);
		AliasLevel--;
	}

	(void) fclose(fp);
	FileName = oldfilename;
	LineNumber = oldlinenumber;
}
/*
**  SENDTOARGV -- send to an argument vector.
**
**	Parameters:
**		argv -- argument vector to send to.
**
**	Returns:
**		none.
**
**	Side Effects:
**		puts all addresses on the argument vector onto the
**			send queue.
*/

sendtoargv(argv)
	register char **argv;
{
	register char *p;
	extern bool sameword();

	while ((p = *argv++) != NULL)
	{
		if (argv[0] != NULL && argv[1] != NULL && sameword(argv[0], "at"))
		{
			char nbuf[MAXNAME];

			if (strlen(p) + strlen(argv[1]) + 2 > sizeof nbuf)
				usrerr("address overflow");
			else
			{
				(void) strcpy(nbuf, p);
				(void) strcat(nbuf, "@");
				(void) strcat(nbuf, argv[1]);
				p = newstr(nbuf);
				argv += 2;
			}
		}
		sendto(p, 0, (ADDRESS *) NULL, 0);
	}
}
/*
**  GETCTLADDR -- get controlling address from an address header.
**
**	If none, get one corresponding to the effective userid.
**
**	Parameters:
**		a -- the address to find the controller of.
**
**	Returns:
**		the controlling address.
**
**	Side Effects:
**		none.
*/

ADDRESS *
getctladdr(a)
	register ADDRESS *a;
{
	while (a != NULL && !bitset(QGOODUID, a->q_flags))
		a = a->q_alias;
	return (a);
}
