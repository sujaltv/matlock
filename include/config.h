/** matlock - Matrix Lock
 * See LICENCE file for copyright and licence details. */

/* user and group to drop privileges to */
static const char *user  = "nobody";
static const char *group = "nobody";

static const char *colorname[NUMCOLS] = {
	[INIT] =   "#000000",   /* after initialization */
	[INPUT] =  "#181C1F",   /* during input */
	[FAILED] = "#2B1211",   /* wrong password */
};

/* treat a cleared input like a wrong password (color) */
static const int failonclear = 1;
