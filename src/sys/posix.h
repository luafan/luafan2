/*
 * sys/posix.h — LuaFan v2 POSIX/system-services binding (M6).
 *
 * Registered as fan.posix on POSIX-like platforms with:
 *   fan.posix.getpid()                  -> integer
 *   fan.posix.fork()                    -> pid (0 in child; -1/nil,err on fail)
 *   fan.posix.waitpid(pid?=-1, opts?=0) -> pid, status  | nil, err
 *   fan.posix.kill(pid, sig?=SIGTERM)   -> true | nil, err
 *      *  kill refuses dangerous defaults (pid==0/-1 or PID 1) unless the
 *         caller passes force=true as a 3rd argument.
 *   fan.posix.setpgid(pid?, pgid?)      -> 0 | nil, err
 *   fan.posix.getpgid(pid?)             -> pgid | nil, err
 *   fan.posix.setsid()                  -> sid | nil, err
 *   fan.posix.setaffinity(mask)         -> true | nil, err   (Linux/Android)
 *   fan.posix.getaffinity()             -> mask | nil, err   (Linux/Android)
 *   fan.posix.getcpucount()             -> integer
 *   fan.posix.getinterfaces()           -> array of
 *                                          {name,type,host,netmask,dst?,broadcast?}
 *      * IPv6 netmask is emitted with the correct sockaddr_in6 length.
 *   fan.posix.setprogname(name)         -> nil     (Linux/glibc only, safe)
 *   fan.posix.readdir(path)             -> {names...} | nil, err, errno
 *      * Returns raw entries including "." and ".."; caller filters.
 *   fan.posix.stat(path[, opts])        -> {mode,size,mtime,...} | nil, err
 *      * mode is one of "file", "directory", "link", "socket", "fifo",
 *        "char device", "block device", "other". size is bytes (regular
 *        files); mtime is a POSIX epoch integer. Uses stat(2) (follows
 *        symlinks); pass opts={ link=true } for lstat(2). Extra fields:
 *        ino, dev, nlink, uid, gid, atime, ctime, blksize, blocks.
 *        Covers what v1 LuaFileSystem consumers (route/service/mapping/
 *        webfile) read.
 *
 * Signal name -> number is exposed as fan.posix.signals =
 *   { SIGTERM=15, SIGKILL=9, SIGINT=2, SIGHUP=1, SIGUSR1=..., SIGUSR2=... }
 * so callers can write fan.posix.kill(pid, fan.posix.signals.SIGKILL, {force=true}).
 */
#ifndef FAN2_SYS_POSIX_H
#define FAN2_SYS_POSIX_H
#include <lua.h>
void fan_posix_register(lua_State *L);
#endif
