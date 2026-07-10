/** matlock - Matrix Lock
 * See LICENCE file for copyright and licence details. */

#ifndef CONFIG_HPP__
#define CONFIG_HPP__


/* user and group to drop privileges to (compile-time only; everything else
 * lives in /etc/matlock.yaml and $XDG_CONFIG_HOME/matlock/matlock.yaml) */
static const char* user  = "nobody";
static const char* group = "nobody";

#endif /* CONFIG_HPP__ */
