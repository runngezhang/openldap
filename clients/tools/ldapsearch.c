/* $OpenLDAP$ */
/*
 * Copyright 1998-2002 The OpenLDAP Foundation, All Rights Reserved.
 * COPYING RESTRICTIONS APPLY, see COPYRIGHT file
 */

#include "portable.h"

#include <stdio.h>

#include <ac/stdlib.h>

#include <ac/ctype.h>
#include <ac/signal.h>
#include <ac/string.h>
#include <ac/unistd.h>
#include <ac/errno.h>
#include <sys/stat.h>

#ifdef HAVE_FCNTL_H
#include <fcntl.h>
#endif
#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif
#ifdef HAVE_IO_H
#include <io.h>
#endif

#include <ldap.h>

#include "ldif.h"
#include "lutil.h"
#include "lutil_ldap.h"
#include "ldap_defaults.h"
#include "ldap_log.h"

static char *def_tmpdir;
static char *def_urlpre;

static void
usage( const char *s )
{
	fprintf( stderr,
"usage: %s [options] [filter [attributes...]]\nwhere:\n"
"  filter\tRFC-2254 compliant LDAP search filter\n"
"  attributes\twhitespace-separated list of attribute descriptions\n"
"    which may include:\n"
"      1.1   no attributes\n"
"      *     all user attributes\n"
"      +     all operational attributes\n"

"Search options:\n"
"  -a deref   one of never (default), always, search, or find\n"
"  -A         retrieve attribute names only (no values)\n"
"  -b basedn  base dn for search\n"
"  -E [!]<ctrl>[=<ctrlparam>] search controls (! indicates criticality)\n"
"             [!]mv=<filter>              (matched values filter)\n"
"             [!]pr=<size>                (paged results)\n"
#ifdef LDAP_CONTROL_SUBENTRIES
"             [!]subentries[=true|false]  (subentries)\n"
#endif
"  -F prefix  URL prefix for files (default: %s)\n"
"  -l limit   time limit (in seconds) for search\n"
"  -L         print responses in LDIFv1 format\n"
"  -LL        print responses in LDIF format without comments\n"
"  -LLL       print responses in LDIF format without comments\n"
"             and version\n"
"  -s scope   one of base, one, or sub (search scope)\n"
"  -S attr    sort the results by attribute `attr'\n"
"  -t         write binary values to files in temporary directory\n"
"  -tt        write all values to files in temporary directory\n"
"  -T path    write files to directory specified by path (default: %s)\n"
"  -u         include User Friendly entry names in the output\n"
"  -z limit   size limit (in entries) for search\n"

"Common options:\n"
"  -d level   set LDAP debugging level to `level'\n"
"  -D binddn  bind DN\n"
"  -e [!]<ctrl>[=<ctrlparam>] general controls (! indicates criticality)\n"
"             [!]authzid=<authzid> (\"dn:<dn>\" or \"u:<user>\")\n"
"             [!]manageDSAit       (alternate form, see -M)\n"
"             [!]noop\n"
"  -f file    read operations from `file'\n"
"  -h host    LDAP server\n"
"  -H URI     LDAP Uniform Resource Indentifier(s)\n"
"  -I         use SASL Interactive mode\n"
"  -k         use Kerberos authentication\n"
"  -K         like -k, but do only step 1 of the Kerberos bind\n"
"  -M         enable Manage DSA IT control (-MM to make critical)\n"
"  -n         show what would be done but don't actually search\n"
"  -O props   SASL security properties\n"
"  -p port    port on LDAP server\n"
"  -P version procotol version (default: 3)\n"
"  -Q         use SASL Quiet mode\n"
"  -R realm   SASL realm\n"
"  -U authcid SASL authentication identity\n"
"  -v         run in verbose mode (diagnostics to standard output)\n"
"  -w passwd  bind passwd (for simple authentication)\n"
"  -W         prompt for bind passwd\n"
"  -x         Simple authentication\n"
"  -X authzid SASL authorization identity (\"dn:<dn>\" or \"u:<user>\")\n"
"  -y file    Read passwd from file\n"
"  -Y mech    SASL mechanism\n"
"  -Z         Start TLS request (-ZZ to require successful response)\n"
, s, def_urlpre, def_tmpdir );

	exit( EXIT_FAILURE );
}

static void print_entry LDAP_P((
	LDAP	*ld,
	LDAPMessage	*entry,
	int		attrsonly));

static void print_reference(
	LDAP *ld,
	LDAPMessage *reference );

static void print_extended(
	LDAP *ld,
	LDAPMessage *extended );

static void print_partial(
	LDAP *ld,
	LDAPMessage *partial );

static int print_result(
	LDAP *ld,
	LDAPMessage *result,
	int search );

static void print_ctrls(
	LDAPControl **ctrls );

static int write_ldif LDAP_P((
	int type,
	char *name,
	char *value,
	ber_len_t vallen ));

static int dosearch LDAP_P((
	LDAP	*ld,
	char	*base,
	int		scope,
	char	*filtpatt,
	char	*value,
	char	**attrs,
	int		attrsonly,
	LDAPControl **sctrls,
	LDAPControl **cctrls,
	struct timeval *timeout,
	int	sizelimit ));

static char *tmpdir = NULL;
static char *urlpre = NULL;
static char *prog = NULL;
static char	*binddn = NULL;
static struct berval passwd = { 0, NULL };
static char	*base = NULL;
static char	*ldaphost = NULL;
static char *ldapuri = NULL;
static int	ldapport = 0;
#ifdef HAVE_CYRUS_SASL
static unsigned sasl_flags = LDAP_SASL_AUTOMATIC;
static char	*sasl_realm = NULL;
static char	*sasl_authc_id = NULL;
static char	*sasl_authz_id = NULL;
static char	*sasl_mech = NULL;
static char	*sasl_secprops = NULL;
#endif
static int	use_tls = 0;
static char	*sortattr = NULL;
static int	verbose, not, includeufn, vals2tmp, ldif;

static int pagedResults = 0;
static ber_int_t pageSize = 0;
static ber_int_t entriesLeft = 0;
static ber_int_t morePagedResults = 1;
static struct berval cookie = { 0, NULL };
static int npagedresponses;
static int npagedentries;
static int npagedreferences;
static int npagedextended;
static int npagedpartial;

static int parse_page_control(
	LDAP *ld,
	LDAPMessage *result,
	struct berval *cookie );

static void
urlize(char *url)
{
	char *p;

	if (*LDAP_DIRSEP != '/') {
		for (p = url; *p; p++) {
			if (*p == *LDAP_DIRSEP)
				*p = '/';
		}
	}
}

int
main( int argc, char **argv )
{
	char		*infile, *filtpattern, **attrs = NULL, line[BUFSIZ];
	FILE		*fp = NULL;
	int			rc, i, first, scope, deref, attrsonly, manageDSAit, noop, crit;
	int			referrals, timelimit, sizelimit, debug;
	int		authmethod, version, want_bindpw;
	LDAP		*ld = NULL;
	BerElement	*ber = NULL;
	char	*control = NULL, *cvalue;
	int		subentries, valuesReturnFilter;
	struct berval 	*sebvalp = NULL, *vrbvalp = NULL;
	char	*vrFilter  = NULL;
	char	*pw_file = NULL;
	char	*authzid = NULL;
	struct berval	*prbvalp = NULL;

	infile = NULL;
	debug = verbose = not = vals2tmp = referrals =
		subentries = valuesReturnFilter =
		attrsonly = manageDSAit = noop = ldif = want_bindpw = 0;

	npagedresponses = npagedentries = npagedreferences =
		npagedextended = npagedpartial = 0;

	prog = lutil_progname( "ldapsearch", argc, argv );

	lutil_log_initialize(argc, argv);

	deref = sizelimit = timelimit = version = -1;

	scope = LDAP_SCOPE_SUBTREE;
	authmethod = -1;

	if((def_tmpdir = getenv("TMPDIR")) == NULL &&
	   (def_tmpdir = getenv("TMP")) == NULL &&
	   (def_tmpdir = getenv("TEMP")) == NULL )
	{
		def_tmpdir = LDAP_TMPDIR;
	}

	if ( !*def_tmpdir )
		def_tmpdir = LDAP_TMPDIR;

	def_urlpre = malloc( sizeof("file:////") + strlen(def_tmpdir) );

	if( def_urlpre == NULL ) {
		perror( "malloc" );
		return EXIT_FAILURE;
	}

	sprintf( def_urlpre, "file:///%s/",
		def_tmpdir[0] == *LDAP_DIRSEP ? &def_tmpdir[1] : def_tmpdir );

	urlize( def_urlpre );

	while (( i = getopt( argc, argv, "Aa:b:E:F:f:Ll:S:s:T:tuz:"
		"Cd:e:D:h:H:IkKMnO:p:P:QR:U:vw:WxX:y:Y:Z")) != EOF )
	{
	switch( i ) {
	/* Search Options */
	case 'a':	/* set alias deref option */
		if ( strcasecmp( optarg, "never" ) == 0 ) {
		deref = LDAP_DEREF_NEVER;
		} else if ( strncasecmp( optarg, "search", sizeof("search")-1 ) == 0 ) {
		deref = LDAP_DEREF_SEARCHING;
		} else if ( strncasecmp( optarg, "find", sizeof("find")-1 ) == 0 ) {
		deref = LDAP_DEREF_FINDING;
		} else if ( strcasecmp( optarg, "always" ) == 0 ) {
		deref = LDAP_DEREF_ALWAYS;
		} else {
		fprintf( stderr, "alias deref should be never, search, find, or always\n" );
		usage(prog);
		}
		break;
	case 'A':	/* retrieve attribute names only -- no values */
		++attrsonly;
		break;
	case 'b': /* search base */
		base = strdup( optarg );
		break;
	case 'E': /* search controls */
		if( version == LDAP_VERSION2 ) {
			fprintf( stderr, "%s: -E incompatible with LDAPv%d\n",
				prog, version );
			return EXIT_FAILURE;
		}

		/* should be extended to support comma separated list of
		 *	[!]key[=value] parameters, e.g.  -E !foo,bar=567
		 */

		crit = 0;
		cvalue = NULL;
		if( optarg[0] == '!' ) {
			crit = 1;
			optarg++;
		}

		control = strdup( optarg );
		if ( (cvalue = strchr( control, '=' )) != NULL ) {
			*cvalue++ = '\0';
		}

		if ( strcasecmp( control, "mv" ) == 0 ) {
			/* ValuesReturnFilter control */
			if( valuesReturnFilter ) {
				fprintf( stderr, "ValuesReturnFilter previously specified");
				return EXIT_FAILURE;
			}
			valuesReturnFilter= 1 + crit;

			if ( cvalue == NULL ) {
				fprintf( stderr,
					"missing filter in ValuesReturnFilter control\n");
				return EXIT_FAILURE;
			}

			vrFilter = cvalue;
			version = LDAP_VERSION3;
			break;

		} else if ( strcasecmp( control, "pr" ) == 0 ) {
			int num, tmp;
			/* PagedResults control */
			if ( pagedResults != 0 ) {
				fprintf( stderr, "PagedResultsControl previously specified" );
				return EXIT_FAILURE;
			}
			
			num = sscanf( cvalue, "%d", &tmp );
			if ( num != 1 ) {
				fprintf( stderr, "Invalid value for PagedResultsControl, %s.\n", cvalue);
				return EXIT_FAILURE;

			}
			pageSize = (ber_int_t) tmp;
			pagedResults = 1 + crit;
			break;

#ifdef LDAP_CONTROL_SUBENTRIES
		} else if ( strcasecmp( control, "subentries" ) == 0 ) {
			if( subentries ) {
				fprintf( stderr, "subentries control previously specified");
				return EXIT_FAILURE;
			}
			if( cvalue == NULL || strcasecmp( cvalue, "true") == 0 ) {
				subentries = 2;
			} else if ( strcasecmp( cvalue, "false") == 0 ) {
				subentries = 1;
			} else {
				fprintf( stderr,
					"subentries control value \"%s\" invalid\n");
				return EXIT_FAILURE;
			}
			if( crit ) subentries *= -1;
#endif

		} else {
			fprintf( stderr, "Invalid control name: %s\n", control );
			usage(prog);
			return EXIT_FAILURE;
		}
	case 'f':	/* input file */
		if( infile != NULL ) {
			fprintf( stderr, "%s: -f previously specified\n", prog );
			return EXIT_FAILURE;
		}
		infile = strdup( optarg );
		break;
	case 'F':	/* uri prefix */
		if( urlpre ) free( urlpre );
		urlpre = strdup( optarg );
		break;
	case 'l':	/* time limit */
		timelimit = atoi( optarg );
		if( timelimit < 0 ) {
			fprintf( stderr, "%s: invalid timelimit (%d) specified\n",
				prog, timelimit );
			return EXIT_FAILURE;
		}
		break;
	case 'L':	/* print entries in LDIF format */
		++ldif;
		break;
	case 's':	/* search scope */
		if ( strcasecmp( optarg, "base" ) == 0 ) {
		scope = LDAP_SCOPE_BASE;
		} else if ( strncasecmp( optarg, "one", sizeof("one")-1 ) == 0 ) {
		scope = LDAP_SCOPE_ONELEVEL;
		} else if ( strncasecmp( optarg, "sub", sizeof("sub")-1 ) == 0 ) {
		scope = LDAP_SCOPE_SUBTREE;
		} else {
		fprintf( stderr, "scope should be base, one, or sub\n" );
		usage(prog);
		}
		break;
	case 'S':	/* sort attribute */
		sortattr = strdup( optarg );
		break;
	case 'u':	/* include UFN */
		++includeufn;
		break;
	case 't':	/* write attribute values to TMPDIR files */
		++vals2tmp;
		break;
	case 'T':	/* tmpdir */
		if( tmpdir ) free( tmpdir );
		tmpdir = strdup( optarg );
		break;
	case 'z':	/* size limit */
		sizelimit = atoi( optarg );
		break;

	/* Common Options */
	case 'C':
		referrals++;
		break;
	case 'd':
	    debug |= atoi( optarg );
	    break;
	case 'D':	/* bind DN */
		if( binddn != NULL ) {
			fprintf( stderr, "%s: -D previously specified\n", prog );
			return EXIT_FAILURE;
		}
	    binddn = strdup( optarg );
	    break;
	case 'e': /* general controls */
		if( version == LDAP_VERSION2 ) {
			fprintf( stderr, "%s: -e incompatible with LDAPv%d\n",
				prog, version );
			return EXIT_FAILURE;
		}

		/* should be extended to support comma separated list of
		 *	[!]key[=value] parameters, e.g.  -e !foo,bar=567
		 */

		crit = 0;
		cvalue = NULL;
		if( optarg[0] == '!' ) {
			crit = 1;
			optarg++;
		}

		control = strdup( optarg );
		if ( (cvalue = strchr( control, '=' )) != NULL ) {
			*cvalue++ = '\0';
		}

		if ( strcasecmp( control, "authzid" ) == 0 ) {
			if( authzid != NULL ) {
				fprintf( stderr, "authzid control previously specified");
				return EXIT_FAILURE;
			}
			if( cvalue == NULL ) {
				fprintf( stderr, "authzid: control value expected" );
				usage(prog);
				return EXIT_FAILURE;
			}
			if( !crit ) {
				fprintf( stderr, "authzid: must be marked critical" );
				usage(prog);
				return EXIT_FAILURE;
			}

			assert( authzid == NULL );
			authzid = control;
			break;
			
		} else if ( strcasecmp( control, "manageDSAit" ) == 0 ) {
			if( manageDSAit ) {
				fprintf( stderr, "manageDSAit control previously specified");
				return EXIT_FAILURE;
			}
			if( cvalue != NULL ) {
				fprintf( stderr, "manageDSAit: no control value expected" );
				usage(prog);
				return EXIT_FAILURE;
			}

			manageDSAit = 1 + crit;
			break;
			
		} else if ( strcasecmp( control, "noop" ) == 0 ) {
			if( noop ) {
				fprintf( stderr, "noop control previously specified");
				return EXIT_FAILURE;
			}
			if( cvalue != NULL ) {
				fprintf( stderr, "noop: no control value expected" );
				usage(prog);
				return EXIT_FAILURE;
			}

			noop = 1 + crit;
			break;

		} else {
			fprintf( stderr, "Invalid general control name: %s\n", control );
			usage(prog);
			return EXIT_FAILURE;
		}
	case 'h':	/* ldap host */
		if( ldapuri != NULL ) {
			fprintf( stderr, "%s: -h incompatible with -H\n", prog );
			return EXIT_FAILURE;
		}
		if( ldaphost != NULL ) {
			fprintf( stderr, "%s: -h previously specified\n", prog );
			return EXIT_FAILURE;
		}
	    ldaphost = strdup( optarg );
	    break;
	case 'H':	/* ldap URI */
		if( ldaphost != NULL ) {
			fprintf( stderr, "%s: -H incompatible with -h\n", prog );
			return EXIT_FAILURE;
		}
		if( ldapport ) {
			fprintf( stderr, "%s: -H incompatible with -p\n", prog );
			return EXIT_FAILURE;
		}
		if( ldapuri != NULL ) {
			fprintf( stderr, "%s: -H previously specified\n", prog );
			return EXIT_FAILURE;
		}
	    ldapuri = strdup( optarg );
	    break;
	case 'I':
#ifdef HAVE_CYRUS_SASL
		if( version == LDAP_VERSION2 ) {
			fprintf( stderr, "%s: -I incompatible with version %d\n",
				prog, version );
			return EXIT_FAILURE;
		}
		if( authmethod != -1 && authmethod != LDAP_AUTH_SASL ) {
			fprintf( stderr, "%s: incompatible previous "
				"authentication choice\n",
				prog );
			return EXIT_FAILURE;
		}
		authmethod = LDAP_AUTH_SASL;
		version = LDAP_VERSION3;
		sasl_flags = LDAP_SASL_INTERACTIVE;
		break;
#else
		fprintf( stderr, "%s: was not compiled with SASL support\n",
			prog );
		return( EXIT_FAILURE );
#endif
	case 'k':	/* kerberos bind */
#ifdef LDAP_API_FEATURE_X_OPENLDAP_V2_KBIND
		if( version > LDAP_VERSION2 ) {
			fprintf( stderr, "%s: -k incompatible with LDAPv%d\n",
				prog, version );
			return EXIT_FAILURE;
		}

		if( authmethod != -1 ) {
			fprintf( stderr, "%s: -k incompatible with previous "
				"authentication choice\n", prog );
			return EXIT_FAILURE;
		}
			
		authmethod = LDAP_AUTH_KRBV4;
#else
		fprintf( stderr, "%s: not compiled with Kerberos support\n", prog );
		return EXIT_FAILURE;
#endif
	    break;
	case 'K':	/* kerberos bind, part one only */
#ifdef LDAP_API_FEATURE_X_OPENLDAP_V2_KBIND
		if( version > LDAP_VERSION2 ) {
			fprintf( stderr, "%s: -k incompatible with LDAPv%d\n",
				prog, version );
			return EXIT_FAILURE;
		}
		if( authmethod != -1 ) {
			fprintf( stderr, "%s: incompatible with previous "
				"authentication choice\n", prog );
			return EXIT_FAILURE;
		}

		authmethod = LDAP_AUTH_KRBV41;
#else
		fprintf( stderr, "%s: not compiled with Kerberos support\n", prog );
		return( EXIT_FAILURE );
#endif
	    break;
	case 'M':
		/* enable Manage DSA IT */
		if( version == LDAP_VERSION2 ) {
			fprintf( stderr, "%s: -M incompatible with LDAPv%d\n",
				prog, version );
			return EXIT_FAILURE;
		}
		manageDSAit++;
		version = LDAP_VERSION3;
		break;
	case 'n':	/* print deletes, don't actually do them */
	    ++not;
	    break;
	case 'O':
#ifdef HAVE_CYRUS_SASL
		if( sasl_secprops != NULL ) {
			fprintf( stderr, "%s: -O previously specified\n", prog );
			return EXIT_FAILURE;
		}
		if( version == LDAP_VERSION2 ) {
			fprintf( stderr, "%s: -O incompatible with LDAPv%d\n",
				prog, version );
			return EXIT_FAILURE;
		}
		if( authmethod != -1 && authmethod != LDAP_AUTH_SASL ) {
			fprintf( stderr, "%s: incompatible previous "
				"authentication choice\n", prog );
			return EXIT_FAILURE;
		}
		authmethod = LDAP_AUTH_SASL;
		version = LDAP_VERSION3;
		sasl_secprops = strdup( optarg );
#else
		fprintf( stderr, "%s: not compiled with SASL support\n",
			prog );
		return( EXIT_FAILURE );
#endif
		break;
	case 'p':
		if( ldapport ) {
			fprintf( stderr, "%s: -p previously specified\n", prog );
			return EXIT_FAILURE;
		}
	    ldapport = atoi( optarg );
	    break;
	case 'P':
		switch( atoi(optarg) ) {
		case 2:
			if( version == LDAP_VERSION3 ) {
				fprintf( stderr, "%s: -P 2 incompatible with version %d\n",
					prog, version );
				return EXIT_FAILURE;
			}
			version = LDAP_VERSION2;
			break;
		case 3:
			if( version == LDAP_VERSION2 ) {
				fprintf( stderr, "%s: -P 2 incompatible with version %d\n",
					prog, version );
				return EXIT_FAILURE;
			}
			version = LDAP_VERSION3;
			break;
		default:
			fprintf( stderr, "%s: protocol version should be 2 or 3\n",
				prog );
			usage( prog );
			return( EXIT_FAILURE );
		} break;
	case 'Q':
#ifdef HAVE_CYRUS_SASL
		if( version == LDAP_VERSION2 ) {
			fprintf( stderr, "%s: -Q incompatible with version %d\n",
				prog, version );
			return EXIT_FAILURE;
		}
		if( authmethod != -1 && authmethod != LDAP_AUTH_SASL ) {
			fprintf( stderr, "%s: incompatible previous "
				"authentication choice\n",
				prog );
			return EXIT_FAILURE;
		}
		authmethod = LDAP_AUTH_SASL;
		version = LDAP_VERSION3;
		sasl_flags = LDAP_SASL_QUIET;
		break;
#else
		fprintf( stderr, "%s: not compiled with SASL support\n",
			prog );
		return( EXIT_FAILURE );
#endif
	case 'R':
#ifdef HAVE_CYRUS_SASL
		if( sasl_realm != NULL ) {
			fprintf( stderr, "%s: -R previously specified\n", prog );
			return EXIT_FAILURE;
		}
		if( version == LDAP_VERSION2 ) {
			fprintf( stderr, "%s: -R incompatible with version %d\n",
				prog, version );
			return EXIT_FAILURE;
		}
		if( authmethod != -1 && authmethod != LDAP_AUTH_SASL ) {
			fprintf( stderr, "%s: incompatible previous "
				"authentication choice\n",
				prog );
			return EXIT_FAILURE;
		}
		authmethod = LDAP_AUTH_SASL;
		version = LDAP_VERSION3;
		sasl_realm = strdup( optarg );
#else
		fprintf( stderr, "%s: not compiled with SASL support\n",
			prog );
		return( EXIT_FAILURE );
#endif
		break;
	case 'U':
#ifdef HAVE_CYRUS_SASL
		if( sasl_authc_id != NULL ) {
			fprintf( stderr, "%s: -U previously specified\n", prog );
			return EXIT_FAILURE;
		}
		if( version == LDAP_VERSION2 ) {
			fprintf( stderr, "%s: -U incompatible with version %d\n",
				prog, version );
			return EXIT_FAILURE;
		}
		if( authmethod != -1 && authmethod != LDAP_AUTH_SASL ) {
			fprintf( stderr, "%s: incompatible previous "
				"authentication choice\n",
				prog );
			return EXIT_FAILURE;
		}
		authmethod = LDAP_AUTH_SASL;
		version = LDAP_VERSION3;
		sasl_authc_id = strdup( optarg );
#else
		fprintf( stderr, "%s: not compiled with SASL support\n",
			prog );
		return( EXIT_FAILURE );
#endif
		break;
	case 'v':	/* verbose mode */
	    verbose++;
	    break;
	case 'w':	/* password */
	    passwd.bv_val = strdup( optarg );
		{
			char* p;

			for( p = optarg; *p != '\0'; p++ ) {
				*p = '\0';
			}
		}
		passwd.bv_len = strlen( passwd.bv_val );
	    break;
	case 'W':
		want_bindpw++;
		break;
	case 'y':
		pw_file = optarg;
		break;
	case 'Y':
#ifdef HAVE_CYRUS_SASL
		if( sasl_mech != NULL ) {
			fprintf( stderr, "%s: -Y previously specified\n", prog );
			return EXIT_FAILURE;
		}
		if( version == LDAP_VERSION2 ) {
			fprintf( stderr, "%s: -Y incompatible with version %d\n",
				prog, version );
			return EXIT_FAILURE;
		}
		if( authmethod != -1 && authmethod != LDAP_AUTH_SASL ) {
			fprintf( stderr, "%s: incompatible with authentication choice\n", prog );
			return EXIT_FAILURE;
		}
		authmethod = LDAP_AUTH_SASL;
		version = LDAP_VERSION3;
		sasl_mech = strdup( optarg );
#else
		fprintf( stderr, "%s: not compiled with SASL support\n",
			prog );
		return( EXIT_FAILURE );
#endif
		break;
	case 'x':
		if( authmethod != -1 && authmethod != LDAP_AUTH_SIMPLE ) {
			fprintf( stderr, "%s: incompatible with previous "
				"authentication choice\n", prog );
			return EXIT_FAILURE;
		}
		authmethod = LDAP_AUTH_SIMPLE;
		break;
	case 'X':
#ifdef HAVE_CYRUS_SASL
		if( sasl_authz_id != NULL ) {
			fprintf( stderr, "%s: -X previously specified\n", prog );
			return EXIT_FAILURE;
		}
		if( version == LDAP_VERSION2 ) {
			fprintf( stderr, "%s: -X incompatible with LDAPv%d\n",
				prog, version );
			return EXIT_FAILURE;
		}
		if( authmethod != -1 && authmethod != LDAP_AUTH_SASL ) {
			fprintf( stderr, "%s: -X incompatible with "
				"authentication choice\n", prog );
			return EXIT_FAILURE;
		}
		authmethod = LDAP_AUTH_SASL;
		version = LDAP_VERSION3;
		sasl_authz_id = strdup( optarg );
#else
		fprintf( stderr, "%s: not compiled with SASL support\n",
			prog );
		return( EXIT_FAILURE );
#endif
		break;
	case 'Z':
#ifdef HAVE_TLS
		if( version == LDAP_VERSION2 ) {
			fprintf( stderr, "%s: -Z incompatible with version %d\n",
				prog, version );
			return EXIT_FAILURE;
		}
		version = LDAP_VERSION3;
		use_tls++;
#else
		fprintf( stderr, "%s: not compiled with TLS support\n",
			prog );
		return( EXIT_FAILURE );
#endif
		break;
	default:
		fprintf( stderr, "%s: unrecognized option -%c\n",
			prog, optopt );
		usage(prog);
	}
	}

	if (version == -1) {
		version = LDAP_VERSION3;
	}
	if (authmethod == -1 && version > LDAP_VERSION2) {
#ifdef HAVE_CYRUS_SASL
		authmethod = LDAP_AUTH_SASL;
#else
		authmethod = LDAP_AUTH_SIMPLE;
#endif
	}

	if (( argc - optind < 1 ) ||
		( *argv[optind] != '(' /*')'*/ &&
		( strchr( argv[optind], '=' ) == NULL ) ) )
	{
		filtpattern = "(objectclass=*)";
	} else {
		filtpattern = strdup( argv[optind++] );
	}

	if ( argv[optind] != NULL ) {
		attrs = &argv[optind];
	}

	if ( infile != NULL ) {
		if ( infile[0] == '-' && infile[1] == '\0' ) {
			fp = stdin;
		} else if (( fp = fopen( infile, "r" )) == NULL ) {
			perror( infile );
			return EXIT_FAILURE;
		}
	}

	if ( tmpdir == NULL ) {
		tmpdir = def_tmpdir;

		if ( urlpre == NULL )
			urlpre = def_urlpre;
	}

	if( urlpre == NULL ) {
		urlpre = malloc( sizeof("file:////") + strlen(tmpdir) );

		if( urlpre == NULL ) {
			perror( "malloc" );
			return EXIT_FAILURE;
		}

		sprintf( urlpre, "file:///%s/",
			tmpdir[0] == *LDAP_DIRSEP ? &tmpdir[1] : tmpdir );

		urlize( urlpre );
	}

	if ( debug ) {
		if( ber_set_option( NULL, LBER_OPT_DEBUG_LEVEL, &debug ) != LBER_OPT_SUCCESS ) {
			fprintf( stderr, "Could not set LBER_OPT_DEBUG_LEVEL %d\n", debug );
		}
		if( ldap_set_option( NULL, LDAP_OPT_DEBUG_LEVEL, &debug ) != LDAP_OPT_SUCCESS ) {
			fprintf( stderr, "Could not set LDAP_OPT_DEBUG_LEVEL %d\n", debug );
		}
		ldif_debug = debug;
	}

#ifdef SIGPIPE
	(void) SIGNAL( SIGPIPE, SIG_IGN );
#endif

	if( ( ldaphost != NULL || ldapport ) && ( ldapuri == NULL ) ) {
		if ( verbose ) {
			fprintf( stderr, "ldap_init( %s, %d )\n",
				ldaphost != NULL ? ldaphost : "<DEFAULT>",
				ldapport );
		}

		ld = ldap_init( ldaphost, ldapport );
		if( ld == NULL ) {
			perror("ldapsearch: ldap_init");
			return EXIT_FAILURE;
		}

	} else {
		if ( verbose ) {
			fprintf( stderr, "ldap_initialize( %s )\n",
				ldapuri != NULL ? ldapuri : "<DEFAULT>" );
		}

		rc = ldap_initialize( &ld, ldapuri );
		if( rc != LDAP_SUCCESS ) {
			fprintf( stderr, "Could not create LDAP session handle (%d): %s\n",
				rc, ldap_err2string(rc) );
			return EXIT_FAILURE;
		}
	}

	if (deref != -1 &&
		ldap_set_option( ld, LDAP_OPT_DEREF, (void *) &deref ) != LDAP_OPT_SUCCESS )
	{
		fprintf( stderr, "Could not set LDAP_OPT_DEREF %d\n", deref );
		return EXIT_FAILURE;
	}
	if (timelimit != -1 &&
		ldap_set_option( ld, LDAP_OPT_TIMELIMIT, (void *) &timelimit ) != LDAP_OPT_SUCCESS )
	{
		fprintf( stderr, "Could not set LDAP_OPT_TIMELIMIT %d\n", timelimit );
		return EXIT_FAILURE;
	}
	if (sizelimit != -1 &&
		ldap_set_option( ld, LDAP_OPT_SIZELIMIT, (void *) &sizelimit ) != LDAP_OPT_SUCCESS )
	{
		fprintf( stderr, "Could not set LDAP_OPT_SIZELIMIT %d\n", sizelimit );
		return EXIT_FAILURE;
	}

	/* referrals */
	if (ldap_set_option( ld, LDAP_OPT_REFERRALS,
		referrals ? LDAP_OPT_ON : LDAP_OPT_OFF ) != LDAP_OPT_SUCCESS )
	{
		fprintf( stderr, "Could not set LDAP_OPT_REFERRALS %s\n",
			referrals ? "on" : "off" );
		return EXIT_FAILURE;
	}

	if (version == -1 ) {
		version = LDAP_VERSION3;
	}

	if( ldap_set_option( ld, LDAP_OPT_PROTOCOL_VERSION, &version )
		!= LDAP_OPT_SUCCESS )
	{
		fprintf( stderr, "Could not set LDAP_OPT_PROTOCOL_VERSION %d\n",
			version );
		return EXIT_FAILURE;
	}

	if ( use_tls && ( ldap_start_tls_s( ld, NULL, NULL ) != LDAP_SUCCESS )) {
		ldap_perror( ld, "ldap_start_tls" );
		if ( use_tls > 1 ) {
			return EXIT_FAILURE;
		}
	}

	if ( pw_file || want_bindpw ) {
		if ( pw_file ) {
			rc = lutil_get_filed_password( pw_file, &passwd );
			if( rc ) return EXIT_FAILURE;
		} else {
			passwd.bv_val = getpassphrase( "Enter LDAP Password: " );
			passwd.bv_len = passwd.bv_val ? strlen( passwd.bv_val ) : 0;
		}
	}

	if ( authmethod == LDAP_AUTH_SASL ) {
#ifdef HAVE_CYRUS_SASL
		void *defaults;

		if( sasl_secprops != NULL ) {
			rc = ldap_set_option( ld, LDAP_OPT_X_SASL_SECPROPS,
				(void *) sasl_secprops );
			
			if( rc != LDAP_OPT_SUCCESS ) {
				fprintf( stderr,
					"Could not set LDAP_OPT_X_SASL_SECPROPS: %s\n",
					sasl_secprops );
				return( EXIT_FAILURE );
			}
		}
		
		defaults = lutil_sasl_defaults( ld,
			sasl_mech,
			sasl_realm,
			sasl_authc_id,
			passwd.bv_val,
			sasl_authz_id );

		rc = ldap_sasl_interactive_bind_s( ld, binddn,
			sasl_mech, NULL, NULL,
			sasl_flags, lutil_sasl_interact, defaults );

		if( rc != LDAP_SUCCESS ) {
			ldap_perror( ld, "ldap_sasl_interactive_bind_s" );
			return( EXIT_FAILURE );
		}
#else
		fprintf( stderr, "%s: not compiled with SASL support\n",
			prog);
		return( EXIT_FAILURE );
#endif
	} else {
		if ( ldap_bind_s( ld, binddn, passwd.bv_val, authmethod )
				!= LDAP_SUCCESS ) {
			ldap_perror( ld, "ldap_bind" );
			return( EXIT_FAILURE );
		}
	}

getNextPage:
	if ( manageDSAit || noop || subentries
		|| valuesReturnFilter || pageSize )
	{
		int err;
		int i=0;
		int crit = 0;
		LDAPControl c[6];
		LDAPControl *ctrls[7];
		
		if ( authzid ) {
			c[i].ldctl_oid = LDAP_CONTROL_PROXY_AUTHZ;
			c[i].ldctl_value.bv_val = authzid;
			c[i].ldctl_value.bv_len = strlen( authzid );
			c[i].ldctl_iscritical = 1;

			if( c[i].ldctl_iscritical ) crit++;
			ctrls[i] = &c[i];
			ctrls[++i] = NULL;
		}

		if ( manageDSAit ) {
			c[i].ldctl_oid = LDAP_CONTROL_MANAGEDSAIT;
			c[i].ldctl_value.bv_val = NULL;
			c[i].ldctl_value.bv_len = 0;
			c[i].ldctl_iscritical = manageDSAit > 1;

			if( c[i].ldctl_iscritical ) crit++;
			ctrls[i] = &c[i];
			ctrls[++i] = NULL;
		}

		if ( noop ) {
			c[i].ldctl_oid = LDAP_CONTROL_NOOP;
			c[i].ldctl_value.bv_val = NULL;
			c[i].ldctl_value.bv_len = 0;
			c[i].ldctl_iscritical = noop > 1;

			if( c[i].ldctl_iscritical ) crit++;
			ctrls[i] = &c[i];
			ctrls[++i] = NULL;
		}

#ifdef LDAP_CONTROL_SUBENTRIES
		if ( subentries ) {
	        if (( ber = ber_alloc_t(LBER_USE_DER)) == NULL ) {
				return EXIT_FAILURE;
			}

			err = ber_printf( ber, "{b}", abs(subentries) == 1 ? 0 : 1 );
	    	if ( err == LBER_ERROR ) {
				ber_free( ber, 1 );
				fprintf( stderr, "Subentries control encoding error!\n" );
				return EXIT_FAILURE;
			}

			if ( ber_flatten( ber, &sebvalp ) == LBER_ERROR ) {
				return EXIT_FAILURE;
			}

			c[i].ldctl_oid = LDAP_CONTROL_SUBENTRIES;
			c[i].ldctl_value=(*sebvalp);
			c[i].ldctl_iscritical = subentries < 1;

			if( c[i].ldctl_iscritical ) crit++;
			ctrls[i] = &c[i];
			ctrls[++i] = NULL;
		}
#endif

		if ( valuesReturnFilter ) {
	        if (( ber = ber_alloc_t(LBER_USE_DER)) == NULL ) {
				return EXIT_FAILURE;
			}

	    	if ( ( err = ldap_put_vrFilter( ber, vrFilter ) ) == -1 ) {
				ber_free( ber, 1 );
				fprintf( stderr, "Bad ValuesReturnFilter: %s\n", vrFilter );
				return EXIT_FAILURE;
			}

			if ( ber_flatten( ber, &vrbvalp ) == LBER_ERROR ) {
				return EXIT_FAILURE;
			}

			ber_free( ber, 1 );

			c[i].ldctl_oid = LDAP_CONTROL_VALUESRETURNFILTER;
			c[i].ldctl_value=(*vrbvalp);
			c[i].ldctl_iscritical = valuesReturnFilter > 1;

			if( c[i].ldctl_iscritical ) crit++;
			ctrls[i] = &c[i];
			ctrls[++i] = NULL;
		}

		if ( pagedResults ) {
			if (( ber = ber_alloc_t(LBER_USE_DER)) == NULL ) {
				return EXIT_FAILURE;
			}

			ber_printf( ber, "{iO}", pageSize, &cookie );
			if ( ber_flatten( ber, &prbvalp ) == LBER_ERROR ) {
				return EXIT_FAILURE;
			}
			
			ber_free( ber, 1 );

			c[i].ldctl_oid = LDAP_CONTROL_PAGEDRESULTS;
			c[i].ldctl_value=(*prbvalp);
			c[i].ldctl_iscritical = pagedResults > 1;

			if( c[i].ldctl_iscritical ) crit++;
			ctrls[i] = &c[i];
			ctrls[++i] = NULL;
		}

		err = ldap_set_option( ld, LDAP_OPT_SERVER_CONTROLS, ctrls );

		if( err != LDAP_OPT_SUCCESS ) {
			fprintf( stderr, "Could not set %scontrols\n",
				crit ? "critical " : "" );
			if( crit ) {
				return EXIT_FAILURE;
			}
		}

		ber_bvfree( sebvalp );
		ber_bvfree( vrbvalp );
		ber_bvfree( prbvalp );
	}
	
	if ( verbose ) {
		fprintf( stderr, "filter%s: %s\nrequesting: ",
			infile != NULL ? " pattern" : "",
			filtpattern );

		if ( attrs == NULL ) {
			fprintf( stderr, "ALL" );
		} else {
			for ( i = 0; attrs[ i ] != NULL; ++i ) {
				fprintf( stderr, "%s ", attrs[ i ] );
			}
		}
		fprintf( stderr, "\n" );
	}

	if ( ldif == 0 ) {
		printf( "# extended LDIF\n" );
	} else if ( ldif < 3 ) {
		printf( "version: %d\n\n", 1 );
	}

	if (ldif < 2 ) {
		printf( "#\n"
			"# LDAPv%d\n"
			"# base <%s> with scope %s\n"
			"# filter%s: %s\n"
			"# requesting: ",
			version,
			base, (scope == LDAP_SCOPE_BASE) ? "base"
				: ((scope == LDAP_SCOPE_ONELEVEL) ? "one" : "sub"),
			infile != NULL ? " pattern" : "",
			filtpattern );

		if ( attrs == NULL ) {
			printf( "ALL" );
		} else {
			for ( i = 0; attrs[ i ] != NULL; ++i ) {
				printf( "%s ", attrs[ i ] );
			}
		}

		if ( manageDSAit ) {
			printf("\n# with manageDSAit %scontrol",
				manageDSAit > 1 ? "critical " : "" );
		}
		if ( noop ) {
			printf("\n# with noop %scontrol",
				noop > 1 ? "critical " : "" );
		}
		if ( subentries ) {
			printf("\n# with subentries %scontrol: %s",
				subentries < 0 ? "critical " : "",
				abs(subentries) == 1 ? "false" : "true" );
		}
		if ( valuesReturnFilter ) {
			printf("\n# with valuesReturnFilter %scontrol: %s",
				valuesReturnFilter > 1 ? "critical " : "", vrFilter );
		}
		if ( pageSize ) {
			printf("\n# with pagedResults %scontrol: size=%d",
				(pagedResults > 1) ? "critical " : "", 
				pageSize );
		}

		printf( "\n#\n\n" );
	}

	if ( infile == NULL ) {
		rc = dosearch( ld, base, scope, NULL, filtpattern,
			attrs, attrsonly, NULL, NULL, NULL, -1 );

	} else {
		rc = 0;
		first = 1;
		while ( rc == 0 && fgets( line, sizeof( line ), fp ) != NULL ) { 
			line[ strlen( line ) - 1 ] = '\0';
			if ( !first ) {
				putchar( '\n' );
			} else {
				first = 0;
			}
			rc = dosearch( ld, base, scope, filtpattern, line,
				attrs, attrsonly, NULL, NULL, NULL, -1 );
		}
		if ( fp != stdin ) {
			fclose( fp );
		}
	}

	if ( ( pageSize != 0 ) && ( morePagedResults != 0 ) ) { 
		char	buf[6];
		int	i, moreEntries, tmpSize;

		/* Loop to get the next pages when 
		 * enter is pressed on the terminal.
		 */
		if ( entriesLeft > 0 ) {
			printf( "Estimate entries: %d\n", entriesLeft );
		}
		printf( "Press [size] Enter for the next {%d|size} entries.\n",
			(int)pageSize ); 
		i = 0;
		moreEntries = getchar();
		while ( moreEntries != EOF && moreEntries != '\n' ) { 
			if ( i < sizeof(buf) - 1 ) {
				buf[i] = moreEntries;
				i++;
			}
			moreEntries = getchar();
		}
		buf[i] = '\0';

		if ( i > 0 && isdigit( buf[0] ) ) {
			int num = sscanf( buf, "%d", &tmpSize );
			if ( num != 1 ) {
				fprintf( stderr, "Invalid value for PagedResultsControl, %s.\n", buf);
				return EXIT_FAILURE;

			}
			pageSize = (ber_int_t)tmpSize;
		}

		goto getNextPage;	
	}

	ldap_unbind( ld );
	return( rc );
}


static int dosearch(
	LDAP	*ld,
	char	*base,
	int		scope,
	char	*filtpatt,
	char	*value,
	char	**attrs,
	int		attrsonly,
	LDAPControl **sctrls,
	LDAPControl **cctrls,
	struct timeval *timeout,
	int sizelimit )
{
	char			*filter;
	int			rc;
	int			nresponses;
	int			nentries;
	int			nreferences;
	int			nextended;
	int			npartial;
	LDAPMessage		*res, *msg;
	ber_int_t	msgid;

	if( filtpatt != NULL ) {
		filter = malloc( strlen( filtpatt ) + strlen( value ) );
		if( filter == NULL ) {
			perror( "malloc" );
			return EXIT_FAILURE;
		}

		sprintf( filter, filtpatt, value );

		if ( verbose ) {
			fprintf( stderr, "filter: %s\n", filter );
		}

		if( ldif < 2 ) {
			printf( "#\n# filter: %s\n#\n", filter );
		}

	} else {
		filter = value;
	}

	if ( not ) {
		return LDAP_SUCCESS;
	}

	rc = ldap_search_ext( ld, base, scope, filter, attrs, attrsonly,
		sctrls, cctrls, timeout, sizelimit, &msgid );

	if ( filtpatt != NULL ) {
		free( filter );
	}

	if( rc != LDAP_SUCCESS ) {
		fprintf( stderr, "%s: ldap_search_ext: %s (%d)\n",
			prog, ldap_err2string( rc ), rc );
		return( rc );
	}

	nresponses = nentries = nreferences = nextended = npartial = 0;

	res = NULL;

	while ((rc = ldap_result( ld, LDAP_RES_ANY,
		sortattr ? LDAP_MSG_ALL : LDAP_MSG_ONE,
		NULL, &res )) > 0 )
	{
		if( sortattr ) {
			(void) ldap_sort_entries( ld, &res,
				( *sortattr == '\0' ) ? NULL : sortattr, strcasecmp );
		}

		for ( msg = ldap_first_message( ld, res );
			msg != NULL;
			msg = ldap_next_message( ld, msg ) )
		{
			if( nresponses++ ) putchar('\n');

			switch( ldap_msgtype( msg ) ) {
			case LDAP_RES_SEARCH_ENTRY:
				nentries++;
				print_entry( ld, msg, attrsonly );
				break;

			case LDAP_RES_SEARCH_REFERENCE:
				nreferences++;
				print_reference( ld, msg );
				break;

			case LDAP_RES_EXTENDED:
				nextended++;
				print_extended( ld, msg );

				if( ldap_msgid( msg ) == 0 ) {
					/* unsolicited extended operation */
					goto done;
				}
				break;

			case LDAP_RES_EXTENDED_PARTIAL:
				npartial++;
				print_partial( ld, msg );
				break;

			case LDAP_RES_SEARCH_RESULT:
				rc = print_result( ld, msg, 1 );
				if ( pageSize != 0 ) { 
					rc = parse_page_control( ld, msg, &cookie );
				}
				goto done;
			}
		}

		ldap_msgfree( res );
	}

	if ( rc == -1 ) {
		ldap_perror( ld, "ldap_result" );
		return( rc );
	}

done:
	if ( pageSize != 0 ) { 
		npagedresponses = npagedresponses + nresponses;
		npagedentries = npagedentries + nentries;
		npagedreferences = npagedreferences + nreferences;
		npagedextended = npagedextended + nextended;
		npagedpartial = npagedpartial + npartial;
		if ( ( morePagedResults == 0 ) && ( ldif < 2 ) ) {
			printf( "\n# numResponses: %d\n", npagedresponses );
			if( nentries ) printf( "# numEntries: %d\n", npagedentries );
			if( nextended ) printf( "# numExtended: %d\n", npagedextended );
			if( npartial ) printf( "# numPartial: %d\n", npagedpartial );
			if( nreferences ) printf( "# numReferences: %d\n", npagedreferences );
		}
	} else if ( ldif < 2 ) {
		printf( "\n# numResponses: %d\n", nresponses );
		if( nentries ) printf( "# numEntries: %d\n", nentries );
		if( nextended ) printf( "# numExtended: %d\n", nextended );
		if( npartial ) printf( "# numPartial: %d\n", npartial );
		if( nreferences ) printf( "# numReferences: %d\n", nreferences );
	}

	return( rc );
}

#if 1
/* This is the original version, the old way of doing things. */
static void
print_entry(
	LDAP	*ld,
	LDAPMessage	*entry,
	int		attrsonly)
{
	char		*a, *dn, *ufn;
	char	tmpfname[ 256 ];
	char	url[ 256 ];
	int			i, rc;
	BerElement		*ber = NULL;
	struct berval	**bvals;
	LDAPControl **ctrls = NULL;
	FILE		*tmpfp;

	dn = ldap_get_dn( ld, entry );
	ufn = NULL;

	if ( ldif < 2 ) {
		ufn = ldap_dn2ufn( dn );
		write_ldif( LDIF_PUT_COMMENT, NULL, ufn, ufn ? strlen( ufn ) : 0 );
	}
	write_ldif( LDIF_PUT_VALUE, "dn", dn, dn ? strlen( dn ) : 0);

	rc = ldap_get_entry_controls( ld, entry, &ctrls );

	if( rc != LDAP_SUCCESS ) {
		fprintf(stderr, "print_entry: %d\n", rc );
		ldap_perror( ld, "ldap_get_entry_controls" );
		exit( EXIT_FAILURE );
	}

	if( ctrls ) {
		print_ctrls( ctrls );
		ldap_controls_free( ctrls );
	}

	if ( includeufn ) {
		if( ufn == NULL ) {
			ufn = ldap_dn2ufn( dn );
		}
		write_ldif( LDIF_PUT_VALUE, "ufn", ufn, ufn ? strlen( ufn ) : 0 );
	}

	if( ufn != NULL ) ldap_memfree( ufn );
	ldap_memfree( dn );

	for ( a = ldap_first_attribute( ld, entry, &ber ); a != NULL;
		a = ldap_next_attribute( ld, entry, ber ) )
	{
		if ( attrsonly ) {
			write_ldif( LDIF_PUT_NOVALUE, a, NULL, 0 );

		} else if (( bvals = ldap_get_values_len( ld, entry, a )) != NULL ) {
			for ( i = 0; bvals[i] != NULL; i++ ) {
				if ( vals2tmp > 1 || ( vals2tmp
					&& ldif_is_not_printable( bvals[i]->bv_val, bvals[i]->bv_len ) ))
				{
					int tmpfd;
					/* write value to file */
					snprintf( tmpfname, sizeof tmpfname,
						"%s" LDAP_DIRSEP "ldapsearch-%s-XXXXXX",
						tmpdir, a );
					tmpfp = NULL;

					tmpfd = mkstemp( tmpfname );

					if ( tmpfd < 0  ) {
						perror( tmpfname );
						continue;
					}

					if (( tmpfp = fdopen( tmpfd, "w")) == NULL ) {
						perror( tmpfname );
						continue;
					}

					if ( fwrite( bvals[ i ]->bv_val,
						bvals[ i ]->bv_len, 1, tmpfp ) == 0 )
					{
						perror( tmpfname );
						fclose( tmpfp );
						continue;
					}

					fclose( tmpfp );

					snprintf( url, sizeof url, "%s%s", urlpre,
						&tmpfname[strlen(tmpdir) + sizeof(LDAP_DIRSEP) - 1] );

					urlize( url );
					write_ldif( LDIF_PUT_URL, a, url, strlen( url ));

				} else {
					write_ldif( LDIF_PUT_VALUE, a,
						bvals[ i ]->bv_val, bvals[ i ]->bv_len );
				}
			}
			ber_bvecfree( bvals );
		}
	}

	if( ber != NULL ) {
		ber_free( ber, 0 );
	}
}
#else
/* This is the proposed new way of doing things.
 * It is more efficient, but the API is non-standard.
 */
static void
print_entry(
	LDAP	*ld,
	LDAPMessage	*entry,
	int		attrsonly)
{
	char		*ufn = NULL;
	char	tmpfname[ 256 ];
	char	url[ 256 ];
	int			i, rc;
	BerElement		*ber = NULL;
	struct berval		bv, *bvals, **bvp = &bvals;
	LDAPControl **ctrls = NULL;
	FILE		*tmpfp;

	rc = ldap_get_dn_ber( ld, entry, &ber, &bv );

	if ( ldif < 2 ) {
		ufn = ldap_dn2ufn( bv.bv_val );
		write_ldif( LDIF_PUT_COMMENT, NULL, ufn, ufn ? strlen( ufn ) : 0 );
	}
	write_ldif( LDIF_PUT_VALUE, "dn", bv.bv_val, bv.bv_len );

	rc = ldap_int_get_controls( ber, &ctrls );

	if( rc != LDAP_SUCCESS ) {
		fprintf(stderr, "print_entry: %d\n", rc );
		ldap_perror( ld, "ldap_get_entry_controls" );
		exit( EXIT_FAILURE );
	}

	if( ctrls ) {
		print_ctrls( ctrls );
		ldap_controls_free( ctrls );
	}

	if ( includeufn ) {
		if( ufn == NULL ) {
			ufn = ldap_dn2ufn( bv.bv_val );
		}
		write_ldif( LDIF_PUT_VALUE, "ufn", ufn, ufn ? strlen( ufn ) : 0 );
	}

	if( ufn != NULL ) ldap_memfree( ufn );

	if ( attrsonly ) bvp = NULL;

	for ( rc = ldap_get_attribute_ber( ld, entry, ber, &bv, bvp );
		rc == LDAP_SUCCESS;
		rc = ldap_get_attribute_ber( ld, entry, ber, &bv, bvp ) )
	{
		if (bv.bv_val == NULL) break;

		if ( attrsonly ) {
			write_ldif( LDIF_PUT_NOVALUE, bv.bv_val, NULL, 0 );

		} else {
			for ( i = 0; bvals[i].bv_val != NULL; i++ ) {
				if ( vals2tmp > 1 || ( vals2tmp
					&& ldif_is_not_printable( bvals[i].bv_val, bvals[i].bv_len ) ))
				{
					int tmpfd;
					/* write value to file */
					snprintf( tmpfname, sizeof tmpfname,
						"%s" LDAP_DIRSEP "ldapsearch-%s-XXXXXX",
						tmpdir, bv.bv_val );
					tmpfp = NULL;

					tmpfd = mkstemp( tmpfname );

					if ( tmpfd < 0  ) {
						perror( tmpfname );
						continue;
					}

					if (( tmpfp = fdopen( tmpfd, "w")) == NULL ) {
						perror( tmpfname );
						continue;
					}

					if ( fwrite( bvals[ i ].bv_val,
						bvals[ i ].bv_len, 1, tmpfp ) == 0 )
					{
						perror( tmpfname );
						fclose( tmpfp );
						continue;
					}

					fclose( tmpfp );

					snprintf( url, sizeof url, "%s%s", urlpre,
						&tmpfname[strlen(tmpdir) + sizeof(LDAP_DIRSEP) - 1] );

					urlize( url );
					write_ldif( LDIF_PUT_URL, bv.bv_val, url, strlen( url ));

				} else {
					write_ldif( LDIF_PUT_VALUE, bv.bv_val,
						bvals[ i ].bv_val, bvals[ i ].bv_len );
				}
			}
			ber_memfree( bvals );
		}
	}

	if( ber != NULL ) {
		ber_free( ber, 0 );
	}
}
#endif

static void print_reference(
	LDAP *ld,
	LDAPMessage *reference )
{
	int rc;
	char **refs = NULL;
	LDAPControl **ctrls;

	if( ldif < 2 ) {
		printf("# search reference\n");
	}

	rc = ldap_parse_reference( ld, reference, &refs, &ctrls, 0 );

	if( rc != LDAP_SUCCESS ) {
		ldap_perror(ld, "ldap_parse_reference");
		exit( EXIT_FAILURE );
	}

	if( refs ) {
		int i;
		for( i=0; refs[i] != NULL; i++ ) {
			write_ldif( ldif ? LDIF_PUT_COMMENT : LDIF_PUT_VALUE,
				"ref", refs[i], strlen(refs[i]) );
		}
		ber_memvfree( (void **) refs );
	}

	if( ctrls ) {
		print_ctrls( ctrls );
		ldap_controls_free( ctrls );
	}
}

static void print_extended(
	LDAP *ld,
	LDAPMessage *extended )
{
	int rc;
	char *retoid = NULL;
	struct berval *retdata = NULL;

	if( ldif < 2 ) {
		printf("# extended result response\n");
	}

	rc = ldap_parse_extended_result( ld, extended,
		&retoid, &retdata, 0 );

	if( rc != LDAP_SUCCESS ) {
		ldap_perror(ld, "ldap_parse_extended_result");
		exit( EXIT_FAILURE );
	}

	if ( ldif < 2 ) {
		write_ldif( ldif ? LDIF_PUT_COMMENT : LDIF_PUT_VALUE,
			"extended", retoid, retoid ? strlen(retoid) : 0 );
	}
	ber_memfree( retoid );

	if(retdata) {
		if ( ldif < 2 ) {
			write_ldif( ldif ? LDIF_PUT_COMMENT : LDIF_PUT_BINARY,
				"data", retdata->bv_val, retdata->bv_len );
		}
		ber_bvfree( retdata );
	}

	print_result( ld, extended, 0 );
}

static void print_partial(
	LDAP *ld,
	LDAPMessage *partial )
{
	int rc;
	char *retoid = NULL;
	struct berval *retdata = NULL;
	LDAPControl **ctrls = NULL;

	if( ldif < 2 ) {
		printf("# extended partial response\n");
	}

	rc = ldap_parse_extended_partial( ld, partial,
		&retoid, &retdata, &ctrls, 0 );

	if( rc != LDAP_SUCCESS ) {
		ldap_perror(ld, "ldap_parse_extended_partial");
		exit( EXIT_FAILURE );
	}

	if ( ldif < 2 ) {
		write_ldif( ldif ? LDIF_PUT_COMMENT : LDIF_PUT_VALUE,
			"partial", retoid, retoid ? strlen(retoid) : 0 );
	}

	ber_memfree( retoid );

	if( retdata ) {
		if ( ldif < 2 ) {
			write_ldif( ldif ? LDIF_PUT_COMMENT : LDIF_PUT_BINARY,
				"data", retdata->bv_val, retdata->bv_len );
		}

		ber_bvfree( retdata );
	}

	if( ctrls ) {
		print_ctrls( ctrls );
		ldap_controls_free( ctrls );
	}
}

static int print_result(
	LDAP *ld,
	LDAPMessage *result, int search )
{
	int rc;
	int err;
	char *matcheddn = NULL;
	char *text = NULL;
	char **refs = NULL;
	LDAPControl **ctrls = NULL;

	if( search ) {
		if ( ldif < 2 ) {
			printf("# search result\n");
		}
		if ( ldif < 1 ) {
			printf("%s: %d\n", "search", ldap_msgid(result) );
		}
	}

	rc = ldap_parse_result( ld, result,
		&err, &matcheddn, &text, &refs, &ctrls, 0 );

	if( rc != LDAP_SUCCESS ) {
		ldap_perror(ld, "ldap_parse_result");
		exit( EXIT_FAILURE );
	}


	if( !ldif ) {
		printf( "result: %d %s\n", err, ldap_err2string(err) );

	} else if ( err != LDAP_SUCCESS ) {
		fprintf( stderr, "%s (%d)\n", ldap_err2string(err), err );
	}

	if( matcheddn && *matcheddn ) {
		if( !ldif ) {
			write_ldif( LDIF_PUT_VALUE,
				"matchedDN", matcheddn, strlen(matcheddn) );
		} else {
			fprintf( stderr, "Matched DN: %s\n", matcheddn );
		}

		ber_memfree( matcheddn );
	}

	if( text && *text ) {
		if( !ldif ) {
			write_ldif( LDIF_PUT_TEXT, "text",
				text, strlen(text) );
		} else {
			fprintf( stderr, "Additional information: %s\n", text );
		}

		ber_memfree( text );
	}

	if( refs ) {
		int i;
		for( i=0; refs[i] != NULL; i++ ) {
			if( !ldif ) {
				write_ldif( LDIF_PUT_VALUE, "ref", refs[i], strlen(refs[i]) );
			} else {
				fprintf( stderr, "Referral: %s\n", refs[i] );
			}
		}

		ber_memvfree( (void **) refs );
	}

	if( ctrls ) {
		print_ctrls( ctrls );
		ldap_controls_free( ctrls );
	}

	return err;
}

static void print_ctrls(
	LDAPControl **ctrls )
{
	int i;
	for(i=0; ctrls[i] != NULL; i++ ) {
		/* control: OID criticality base64value */
		struct berval *b64 = NULL;
		ber_len_t len;
		char *str;

		len = ldif ? 2 : 0;
		len += strlen( ctrls[i]->ldctl_oid );

		/* add enough for space after OID and the critical value itself */
		len += ctrls[i]->ldctl_iscritical
			? sizeof("true") : sizeof("false");

		/* convert to base64 */
		if( ctrls[i]->ldctl_value.bv_len ) {
			b64 = ber_memalloc( sizeof(struct berval) );
			
			b64->bv_len = LUTIL_BASE64_ENCODE_LEN(
				ctrls[i]->ldctl_value.bv_len ) + 1;
			b64->bv_val = ber_memalloc( b64->bv_len + 1 );

			b64->bv_len = lutil_b64_ntop(
				ctrls[i]->ldctl_value.bv_val, ctrls[i]->ldctl_value.bv_len,
				b64->bv_val, b64->bv_len );
		}

		if( b64 ) {
			len += 1 + b64->bv_len;
		}

		str = malloc( len + 1 );
		if ( ldif ) {
			strcpy( str, ": " );
		} else {
			str[0] = '\0';
		}
		strcat( str, ctrls[i]->ldctl_oid );
		strcat( str, ctrls[i]->ldctl_iscritical
			? " true" : " false" );

		if( b64 ) {
			strcat(str, " ");
			strcat(str, b64->bv_val );
		}

		if ( ldif < 2 ) {
			write_ldif( ldif ? LDIF_PUT_COMMENT : LDIF_PUT_VALUE,
				"control", str, len );
		}

		free( str );
		ber_bvfree( b64 );
	}
}

static int
write_ldif( int type, char *name, char *value, ber_len_t vallen )
{
	char	*ldif;

	if (( ldif = ldif_put( type, name, value, vallen )) == NULL ) {
		return( -1 );
	}

	fputs( ldif, stdout );
	ber_memfree( ldif );

	return( 0 );
}


static int 
parse_page_control(
	LDAP *ld,
	LDAPMessage *result,
	struct berval *cookie )
{
	int rc;
	int err;
	LDAPControl **ctrl = NULL;
	LDAPControl *ctrlp = NULL;
	BerElement *ber;
	ber_tag_t tag;
	struct berval servercookie = { 0, NULL };


	rc = ldap_parse_result( ld, result,
		&err, NULL, NULL, NULL, &ctrl, 0 );

	if( rc != LDAP_SUCCESS ) {
		ldap_perror(ld, "ldap_parse_result");
		exit( EXIT_FAILURE );
	}

	if ( err != LDAP_SUCCESS ) {
		fprintf( stderr, "%s (%d)\n", ldap_err2string(err), err );
	}

	if( ctrl ) {
		/* Parse the control value
		 * searchResult ::= SEQUENCE {
		 *		size	INTEGER (0..maxInt),
		 *				-- result set size estimate from server - unused
		 *		cookie	OCTET STRING
		 */
		ctrlp = *ctrl;
		ber = ber_init( &ctrlp->ldctl_value );
		if ( ber == NULL ) {
			fprintf( stderr, "Internal error.\n" );
			return EXIT_FAILURE;
		}

		tag = ber_scanf( ber, "{im}", &entriesLeft, &servercookie );
		ber_dupbv( cookie, &servercookie );
		(void) ber_free( ber, 1 );

		if( tag == LBER_ERROR ) {
			fprintf( stderr, "Paged results response control could not be decoded.\n" );
			return EXIT_FAILURE;
		}

		if( entriesLeft < 0 ) {
			fprintf( stderr, "Invalid entries estimate in paged results response.\n" );
			return EXIT_FAILURE;
		}


		ldap_controls_free( ctrl );
	} else {
		morePagedResults = 0;
	}

	return err;
}
