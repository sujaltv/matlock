/** matlock - Matrix Lock
 * See LICENCE file for copyright and licence details. */

#ifndef CONFIG_HPP__
#define CONFIG_HPP__


/* user and group to drop privileges to */
static const char* user  = "nobody";
static const char* group = "nobody";

/* background colour */
static const char *background = "#141D1A";

/* change the characters in the raining columns */
static const bool mutate_chars = true;

/* font colours */
static const char *fontcolour[Matlock::States::NUMSTATES] = {
	[Matlock::States::INIT] =   "#1cff7b",   /* after initialization */
	[Matlock::States::INPUT] =  "#28a99e",   /* during input */
	[Matlock::States::FAILED] = "#E28743",   /* wrong password */
};

/* treat a cleared input like a wrong password (color) */
static const int failonclear = 0;

#endif /* CONFIG_HPP__ */
