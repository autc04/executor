#if !defined(_RSYS_PARSE_H_)
#define _RSYS_PARSE_H_

/*
 * Copyright 1995 by Abacus Research and Development, Inc.
 * All rights reserved.
 *
 */

extern FILE *configfile;    // hidden parameter for options parser
extern int yyparse(void); /* ick -- that's what yacc produces */

#endif
